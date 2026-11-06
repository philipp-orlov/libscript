// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// JavaScript: the front end's registration. Source goes through the shared
// ECMAScript lexer and parser in their JavaScript dialect, then straight into
// the dynamic engine's compiler. Nothing is checked or erased, because
// nothing needs to be.
#include "script/internal/dyn/compiler.hpp"
#include "script/internal/dyn/parser.hpp"
#include "script/internal/dyn/runtime.hpp"
#include "script/internal/dyn/vm.hpp"
#include "script/internal/language.hpp"

namespace script {
namespace internal {
namespace dyn {

Value runProgramUnit(ContextImpl& ctx, CompiledUnit& unit, const std::vector<Value>& args);

namespace {

std::shared_ptr<CompiledUnit> compileJavaScript(ContextImpl& ctx, std::string_view source,
                                                std::string_view name, std::vector<Diagnostic>&)
{
    Runtime& rt = ctx.dyn();
    rt.syncHostClasses();
    Parser parser(source, std::string(name), /*typescript=*/false);
    NodePtr program = parser.parseProgram();
    auto unit = std::make_shared<Program>();
    unit->language = Language::JavaScript;
    unit->name = std::string(name);
    unit->code = compileProgram(rt, *program, std::string(name), /*sloppyThis=*/true);
    rt.adoptProgram(unit);
    return unit;
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
        l.engine = EngineKind::Dynamic;
        l.compile = &compileJavaScript;
        l.run = &runProgramUnit;
        return l;
    }();
    return info;
}

}  // namespace dyn
}  // namespace internal
}  // namespace script
