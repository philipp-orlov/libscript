// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// C++: the front end's registration. C++ is the C front end with its dialect
// flag on -- classes, references, `std::string`, `std::vector`, `std::map`,
// streams and exceptions lowered onto the same C#-shaped tree -- so the
// work is in src/lang/c/, shared with C.
#include "script/internal/c/frontend.hpp"

namespace script {
namespace internal {
namespace typed {

namespace {

std::shared_ptr<CompiledUnit> compileCpp(ContextImpl& ctx, std::string_view source,
                                         std::string_view name, std::vector<Diagnostic>& diagnostics)
{
    return compileCFamily(ctx, source, name, diagnostics, Language::Cpp);
}

}  // namespace

const LanguageInfo& cppLanguage() noexcept
{
    static const char* const extensions[] = {".cpp", ".cc", ".cxx", ".hpp", ".hh", ".hxx", nullptr};
    static const LanguageInfo info = [] {
        LanguageInfo l;
        l.id = Language::Cpp;
        l.name = "cpp";
        l.extensions = extensions;
        l.engine = EngineKind::Typed;
        l.compile = &compileCpp;
        l.run = &runCFamily;
        return l;
    }();
    return info;
}

}  // namespace typed
}  // namespace internal
}  // namespace script
