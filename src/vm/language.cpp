// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// JavaScript's registration on the new core. Until the bytecode compiler exists (phase P2) it refuses every program.
#include "script/internal/vm/vm.hpp"

namespace script {
namespace internal {
namespace vm {

namespace {

std::shared_ptr<CompiledUnit> compileJavaScript(ContextImpl& ctx, std::string_view, std::string_view,
                                                std::vector<Diagnostic>&)
{
    ctx.vm();
    throw Error(ErrorCode::NotSupported, "vm core: not implemented");
}

Value runUnit(ContextImpl&, CompiledUnit&, const std::vector<Value>&)
{
    throw Error(ErrorCode::NotSupported, "vm core: not implemented");
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
        l.run = &runUnit;
        return l;
    }();
    return info;
}

}  // namespace vm
}  // namespace internal
}  // namespace script
