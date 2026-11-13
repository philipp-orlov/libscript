// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// JavaScript's registration on the new core: the shared ECMAScript parser, the bytecode compiler, and the VM that runs the
// result in the global scope.
#include <cstdio>
#include <cstdlib>

#include "script/internal/dyn/parser.hpp"
#include "script/config.hpp"
#if SCRIPT_VM_LANG_TYPESCRIPT
#include "script/internal/dyn/checker.hpp"
#include "script/internal/dyn/erase.hpp"
#include "script/internal/dyn/runtime.hpp"
#endif
#include "../vm/builtins_util.hpp"
#include "script/internal/vm/compiler.hpp"
#include "script/internal/vm/convert.hpp"
#include "script/internal/vm/host.hpp"
#include "script/internal/vm/operators.hpp"
#include "script/internal/vm/vm.hpp"

namespace script {
namespace internal {
namespace vm {

namespace {

struct Program : CompiledUnit
{
    CodeBlock* code = nullptr;
    VM* vm = nullptr;

    ~Program() override
    {
        if (!vm || alive.expired())
            return;
        vm->removeCodeRoot(&code);
        if (code)
            vm->releaseCode(code);
    }
    std::size_t imageSize() const noexcept override { return code ? code->code.size() * sizeof(Ins) : 0; }
};

std::shared_ptr<CompiledUnit> compileJavaScript(ContextImpl& ctx, std::string_view source, std::string_view name,
                                                std::vector<Diagnostic>&)
{
    VM& vm = ctx.vm();
    dyn::Parser parser(source, std::string(name), /*typescript=*/false);
    dyn::NodePtr tree = parser.parseProgram();
    auto unit = std::make_shared<Program>();
    unit->language = Language::JavaScript;
    unit->name = std::string(name);
    unit->context = &ctx;
    unit->alive = ctx.alive;
    unit->vm = &vm;
    unit->code = compileProgram(vm, *tree, name, std::make_shared<const std::string>(source));
#if defined(SCRIPT_JIT_ENV_OVERRIDE)
    if (std::getenv("SCRIPT_VM_DUMP"))
        std::fputs(dump(*unit->code).c_str(), stderr);
#endif
    vm.addCodeRoot(&unit->code);
    return unit;
}

Value runProgram(ContextImpl& ctx, CompiledUnit& unit, const std::vector<Value>&)
{
    VM& vm = ctx.vm();
    auto& program = static_cast<Program&>(unit);
    if (!program.code)
        throw Error(ErrorCode::InvalidOperation, "the program's context has ended");
    ContextImpl::ActiveEngine active(ctx, EngineKind::Vm);
    host::syncHostClasses(ctx);
    JSFunction* main = vm.newFunction(vm.protos.function, program.code, nullptr, 0);
    Handle keep = Handle::adopt(toJSValue(main));
    JSValue result = vm.call(keep.get(), toJSValue(vm.global), nullptr, 0);
    if (result.isException())
        host::raisePending(vm);
    Value out = toValue(vm, result);
    release(result);
    return out;
}

}  // namespace

JSValue VM::evalSource(std::string_view source, std::string_view name)
{
    CodeBlock* code = nullptr;
    try {
        dyn::Parser parser(source, std::string(name), /*typescript=*/false);
        dyn::NodePtr tree = parser.parseProgram();
        code = compileProgram(*this, *tree, name, std::make_shared<const std::string>(source));
    } catch (const Error& e) {
        return throwError(e.code() == ErrorCode::CompileError ? ErrorCode::SyntaxError : e.code(), e.what());
    }
    JSFunction* main = newFunction(protos.function, code, nullptr, 0);
    releaseCode(code);  // the function has its own count
    Handle keep = Handle::adopt(toJSValue(main));
    return call(keep.get(), toJSValue(global), nullptr, 0);
}

JSValue VM::evalDirect(JSValue sourceValue)
{
    Frame* fr = frameBase + frameCount - 1;
    const CodeBlock* caller = fr->code;
    const Vec<EvalVar>& vars = caller->evalScopes[evalIndex];
    std::vector<String> names;
    for (const EvalVar& v : vars)
        names.push_back(String(static_cast<StringCell*>(caller->atoms[v.atom])->text()));
    const std::string text = toUtf8(asString(sourceValue)->text());
    CodeBlock* code = nullptr;
    try {
        dyn::Parser parser(text, "<eval>", /*typescript=*/false);
        dyn::NodePtr tree = parser.parseProgram();
        code = compileEval(*this, *tree, "<eval>", std::make_shared<const std::string>(text), names,
                           (caller->flags & CodeBlock::kStrict) != 0, evalIndex < caller->evalHome.size() && caller->evalHome[evalIndex]);
    } catch (const Error& e) {
        return throwError(e.code() == ErrorCode::CompileError ? ErrorCode::SyntaxError : e.code(), e.what());
    }
    const std::uint32_t n = std::uint32_t(code->upvals.size());
    std::vector<Box*> boxes(n);
    for (std::uint32_t i = 0; i < n; ++i) {
        const EvalVar& v = vars[code->upvals[i].index];
        boxes[i] = v.local ? reinterpret_cast<Box*>(fr->R[v.index].asCell()) : fr->fn->upvals()[v.index];
    }
    JSFunction* f = newFunction(protos.function, code, boxes.data(), n);
    releaseCode(code);  // the function has its own count
    Handle keep = Handle::adopt(toJSValue(f));
    return call(keep.get(), JSValue::undefined(), nullptr, 0);
}

#if SCRIPT_VM_LANG_TYPESCRIPT
namespace {

/// What the embedder installed on the global object is visible to a script, so the checker has to know it is there.
std::vector<String> hostGlobals(VM& vm)
{
    Vec<Handle> keys;
    bi::ownKeys(vm, vm.global, true, false, false, keys);
    std::vector<String> names;
    for (const Handle& k : keys)
        if (isString(k.get()))
            names.emplace_back(asString(k.get())->text());
    return names;
}

std::shared_ptr<CompiledUnit> compileTypeScript(ContextImpl& ctx, std::string_view source, std::string_view name,
                                                std::vector<Diagnostic>& diagnostics)
{
    VM& vm = ctx.vm();
    host::syncHostClasses(ctx);
    dyn::Parser parser(source, std::string(name), /*typescript=*/true);
    dyn::NodePtr tree = parser.parseProgram();
    if (ctx.options.check.enabled) {
        diagnostics = dyn::checkProgram(*tree, std::string(name), ctx.options.check, hostGlobals(vm),
                                        dyn::hostDeclarationsOf(ctx) + vm.ambient);
        if (!diagnostics.empty())
            return nullptr;
    }
    dyn::erase(*tree);
    auto unit = std::make_shared<Program>();
    unit->language = Language::TypeScript;
    unit->name = std::string(name);
    unit->context = &ctx;
    unit->alive = ctx.alive;
    unit->vm = &vm;
    unit->code = compileProgram(vm, *tree, name, std::make_shared<const std::string>(source));
    vm.addCodeRoot(&unit->code);
    return unit;
}

void declareTypeScript(ContextImpl& ctx, std::string_view declarations)
{
    // Parsed here so a malformed declaration is reported where it was added.
    dyn::Parser parser(declarations, "<declare>", /*typescript=*/true);
    parser.parseProgram();
    ctx.vm().ambient += std::string(declarations);
    ctx.vm().ambient += '\n';
}

}  // namespace

const LanguageInfo& typescriptLanguage() noexcept
{
    static const char* const extensions[] = {".ts", ".mts", nullptr};
    static const LanguageInfo info = [] {
        LanguageInfo l;
        l.id = Language::TypeScript;
        l.name = "typescript";
        l.extensions = extensions;
        l.engine = EngineKind::Vm;
        l.compile = &compileTypeScript;
        l.run = &runProgram;
        l.declare = &declareTypeScript;
        return l;
    }();
    return info;
}
#endif

const LanguageInfo& javascriptLanguage() noexcept
{
    static const char* const extensions[] = {".js", ".mjs", ".cjs", nullptr};
    static const LanguageInfo info = [] {
        LanguageInfo l;
        l.id = Language::JavaScript;
        l.name = "javascript";
        l.extensions = extensions;
        l.engine = EngineKind::Vm;
        l.compile = &compileJavaScript;
        l.run = &runProgram;
        return l;
    }();
    return info;
}

}  // namespace vm
}  // namespace internal
}  // namespace script
