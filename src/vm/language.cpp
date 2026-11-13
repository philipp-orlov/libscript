// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// JavaScript's registration on the new core: the shared ECMAScript parser, the bytecode compiler, and the VM that runs the
// result in the global scope.
#include "script/internal/dyn/parser.hpp"
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
    unit->code = compileProgram(vm, *tree, name);
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
