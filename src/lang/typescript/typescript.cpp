// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// TypeScript: the front end's registration. Source goes through the shared
// ECMAScript lexer and parser with the TypeScript syntax switched on, then
// the structural checker, then the eraser, and what is left is JavaScript for
// the dynamic engine's compiler.
#include "script/internal/dyn/checker.hpp"
#include "script/internal/dyn/compiler.hpp"
#include "script/internal/dyn/erase.hpp"
#include "script/internal/dyn/parser.hpp"
#include "script/internal/dyn/runtime.hpp"
#include "script/internal/dyn/vm.hpp"
#include "script/internal/language.hpp"

namespace script {
namespace internal {
namespace dyn {

Value runProgramUnit(ContextImpl& ctx, CompiledUnit& unit, const std::vector<Value>& args);

namespace {

/// Whatever the embedder installed on the global object is visible to a
/// script, so the checker has to know it exists. Names with a declaration --
/// from `Class<T>`, `hostClass` or `Context::declare` -- get their types from
/// it; the rest are `any`.
std::vector<String> hostGlobals(Runtime& rt)
{
    std::vector<String> names;
    for (const Value& key : ownKeys(rt, rt.globalObject.get(), true, false))
        names.emplace_back(static_cast<StringCell*>(detail::Access::ptr(key))->text());
    return names;
}

std::shared_ptr<CompiledUnit> compileTypeScript(ContextImpl& ctx, std::string_view source,
                                                std::string_view name,
                                                std::vector<Diagnostic>& diagnostics)
{
    Runtime& rt = ctx.dyn();
    rt.syncHostClasses();
    Parser parser(source, std::string(name), /*typescript=*/true);
    NodePtr program = parser.parseProgram();
    if (ctx.options.check.enabled) {
        diagnostics = checkProgram(*program, std::string(name), ctx.options.check,
                                   hostGlobals(rt), rt.hostDeclarations() + rt.ambient);
        if (!diagnostics.empty())
            return nullptr;
    }
    erase(*program);
    auto unit = std::make_shared<Program>();
    unit->language = Language::TypeScript;
    unit->name = std::string(name);
    unit->code = compileProgram(rt, *program, std::string(name));
    rt.adoptProgram(unit);
    return unit;
}

void declareTypeScript(ContextImpl& ctx, std::string_view declarations)
{
    // Parsed here so a malformed declaration is reported at the point it was
    // added rather than at the next evaluation.
    Parser parser(declarations, "<declare>", /*typescript=*/true);
    parser.parseProgram();
    Runtime& rt = ctx.dyn();
    rt.ambient += std::string(declarations);
    rt.ambient += '\n';
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
        l.engine = EngineKind::Dynamic;
        l.compile = &compileTypeScript;
        l.run = &runProgramUnit;
        l.declare = &declareTypeScript;
        return l;
    }();
    return info;
}

}  // namespace dyn
}  // namespace internal
}  // namespace script
