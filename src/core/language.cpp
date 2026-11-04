// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The language table: every front end this build contains, and the names and
// extensions that select one. To add a language, add its option to
// CMakeLists.txt and one block below. See docs/adding-a-language.md.
#include "script/internal/language.hpp"

#include <cstring>

#include "script/config.hpp"

namespace script {
namespace internal {

#if SCRIPT_LANG_CSHARP
namespace typed {
const LanguageInfo& csharpLanguage() noexcept;
}
#endif
#if SCRIPT_LANG_TYPESCRIPT
namespace dyn {
const LanguageInfo& typescriptLanguage() noexcept;
}
#endif
#if SCRIPT_LANG_JAVASCRIPT
namespace dyn {
const LanguageInfo& javascriptLanguage() noexcept;
}
#endif
#if SCRIPT_LANG_PYTHON
namespace dyn {
const LanguageInfo& pythonLanguage() noexcept;
}
#endif
#if SCRIPT_LANG_C
namespace typed {
const LanguageInfo& cLanguage() noexcept;
}
#endif
#if SCRIPT_LANG_CPP
namespace typed {
const LanguageInfo& cppLanguage() noexcept;
}
#endif

const std::vector<const LanguageInfo*>& languageTable() noexcept
{
    static const std::vector<const LanguageInfo*> table = {
#if SCRIPT_LANG_CSHARP
        &typed::csharpLanguage(),
#endif
#if SCRIPT_LANG_TYPESCRIPT
        &dyn::typescriptLanguage(),
#endif
#if SCRIPT_LANG_JAVASCRIPT
        &dyn::javascriptLanguage(),
#endif
#if SCRIPT_LANG_PYTHON
        &dyn::pythonLanguage(),
#endif
#if SCRIPT_LANG_C
        &typed::cLanguage(),
#endif
#if SCRIPT_LANG_CPP
        &typed::cppLanguage(),
#endif
    };
    return table;
}

const LanguageInfo* languageInfo(Language id) noexcept
{
    for (const LanguageInfo* info : languageTable()) {
        if (info->id == id)
            return info;
    }
    return nullptr;
}

}  // namespace internal

// ------------------------------------------------------------- public ---

namespace {

struct KnownExtension
{
    const char* extension;
    Language language;
};

/// Every extension the library defines, whether or not this build has the
/// language, so a missing one is reported as missing rather than mistaken
/// for the default language.
constexpr KnownExtension kExtensions[] = {
    {".cs", Language::CSharp},
    {".ts", Language::TypeScript},
    {".mts", Language::TypeScript},
    {".js", Language::JavaScript},
    {".mjs", Language::JavaScript},
    {".cjs", Language::JavaScript},
    {".py", Language::Python},
    {".c", Language::C},
    {".h", Language::C},
    {".cpp", Language::Cpp},
    {".cc", Language::Cpp},
    {".cxx", Language::Cpp},
    {".hpp", Language::Cpp},
    {".hh", Language::Cpp},
    {".hxx", Language::Cpp},
};

bool equalsIgnoringCase(std::string_view a, std::string_view b) noexcept
{
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const char x = a[i] >= 'A' && a[i] <= 'Z' ? char(a[i] + 32) : a[i];
        const char y = b[i] >= 'A' && b[i] <= 'Z' ? char(b[i] + 32) : b[i];
        if (x != y)
            return false;
    }
    return true;
}

}  // namespace

const char* toString(Language language) noexcept
{
    switch (language) {
        case Language::Auto: return "auto";
        case Language::CSharp: return "csharp";
        case Language::TypeScript: return "typescript";
        case Language::JavaScript: return "javascript";
        case Language::Python: return "python";
        case Language::C: return "c";
        case Language::Cpp: return "cpp";
    }
    return "?";
}

Language languageNamed(std::string_view name) noexcept
{
    if (!name.empty() && name.front() == '.')
        name.remove_prefix(1);
    if (equalsIgnoringCase(name, "csharp") || equalsIgnoringCase(name, "cs") ||
        equalsIgnoringCase(name, "c#"))
        return Language::CSharp;
    if (equalsIgnoringCase(name, "typescript") || equalsIgnoringCase(name, "ts"))
        return Language::TypeScript;
    if (equalsIgnoringCase(name, "javascript") || equalsIgnoringCase(name, "js"))
        return Language::JavaScript;
    if (equalsIgnoringCase(name, "python") || equalsIgnoringCase(name, "py"))
        return Language::Python;
    if (equalsIgnoringCase(name, "c") || equalsIgnoringCase(name, "h"))
        return Language::C;
    if (equalsIgnoringCase(name, "cpp") || equalsIgnoringCase(name, "c++") ||
        equalsIgnoringCase(name, "cxx") || equalsIgnoringCase(name, "cc") ||
        equalsIgnoringCase(name, "hpp") || equalsIgnoringCase(name, "hh"))
        return Language::Cpp;
    return Language::Auto;
}

Language languageOfFile(std::string_view path) noexcept
{
    const std::size_t dot = path.rfind('.');
    if (dot == std::string_view::npos)
        return Language::Auto;
    const std::string_view extension = path.substr(dot);
    for (const KnownExtension& known : kExtensions) {
        if (equalsIgnoringCase(extension, known.extension))
            return known.language;
    }
    return Language::Auto;
}

bool supported(Language language) noexcept
{
    return internal::languageInfo(language) != nullptr;
}

const std::vector<Language>& supportedLanguages() noexcept
{
    static const std::vector<Language> languages = [] {
        std::vector<Language> out;
        for (const internal::LanguageInfo* info : internal::languageTable())
            out.push_back(info->id);
        return out;
    }();
    return languages;
}

}  // namespace script
