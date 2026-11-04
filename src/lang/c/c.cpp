// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// C: the front end's registration. The work is in frontend.cpp, shared with C++.
#include "script/internal/c/frontend.hpp"

namespace script {
namespace internal {
namespace typed {

namespace {

std::shared_ptr<CompiledUnit> compileC(ContextImpl& ctx, std::string_view source,
                                       std::string_view name, std::vector<Diagnostic>& diagnostics)
{
    return compileCFamily(ctx, source, name, diagnostics, Language::C);
}

}  // namespace

const LanguageInfo& cLanguage() noexcept
{
    static const char* const extensions[] = {".c", ".h", nullptr};
    static const LanguageInfo info = [] {
        LanguageInfo l;
        l.id = Language::C;
        l.name = "c";
        l.extensions = extensions;
        l.engine = EngineKind::Typed;
        l.compile = &compileC;
        l.run = &runCFamily;
        return l;
    }();
    return info;
}

}  // namespace typed
}  // namespace internal
}  // namespace script
