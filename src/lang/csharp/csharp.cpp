// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// C#: the front end's registration. Source goes through the lexer, the
// parser, the checker and the emitter into a typed assembly the typed engine
// runs. Everything language-specific about C# lives in this directory; the
// engine it targets lives in src/typed.
#include "script/internal/language.hpp"
#include "script/internal/typed/checker.hpp"
#include "script/internal/typed/emitter.hpp"
#include "script/internal/typed/parser.hpp"
#include "script/internal/typed/runtime.hpp"
#include "script/internal/typed/vm.hpp"

namespace script {
namespace internal {
namespace typed {

namespace {

std::shared_ptr<CompiledUnit> compileCSharp(ContextImpl& ctx, std::string_view source,
                                            std::string_view name,
                                            std::vector<Diagnostic>& diagnostics)
{
    Runtime& rt = ctx.typed();
    rt.syncHostClasses();
    rt.beforeCompile();
    Arena arena;
    Parser parser(source, std::string(name), arena);
    CompilationUnit* unit = parser.parseUnit();

    auto assembly = std::make_shared<AssemblyImpl>();
    assembly->language = Language::CSharp;
    assembly->name = std::string(name);
    assembly->rt = &rt;
    assembly->declaresNames = !unit->types.empty();

    Checker checker(rt, arena, ctx.options.compile, *assembly);
    if (!checker.check(unit, diagnostics))
        return nullptr;

    Emitter emitter(rt, *assembly, ctx.options.compile);
    emitter.emitUnit(unit);

    rt.assemblies.push_back(assembly);
    if (rt.jit &&
        (ctx.options.compile.native || ctx.options.jit.mode == JitOptions::Mode::Eager))
        rt.jit->compileAll(rt, *assembly);
    return assembly;
}

Value runCSharp(ContextImpl& ctx, CompiledUnit& unit, const std::vector<Value>& args)
{
    Runtime& rt = ctx.typed();
    auto& image = static_cast<AssemblyImpl&>(unit);
    rt.ensureInitialized();
    if (!image.entry)
        throw Error(ErrorCode::InvalidOperation, "the assembly has no entry point");

    std::vector<Value> call = args;
    call.resize(image.entry->params.size());
    return invoke(rt, image.entry, call.data(), std::uint32_t(call.size()));
}

}  // namespace

const LanguageInfo& csharpLanguage() noexcept
{
    static const char* const extensions[] = {".cs", nullptr};
    static const LanguageInfo info = [] {
        LanguageInfo l;
        l.id = Language::CSharp;
        l.name = "csharp";
        l.extensions = extensions;
        l.engine = EngineKind::Typed;
        l.compile = &compileCSharp;
        l.run = &runCSharp;
        return l;
    }();
    return info;
}

}  // namespace typed
}  // namespace internal
}  // namespace script
