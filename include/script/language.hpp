// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// libscript -- the languages a build can speak.
//
// Each language is compiled in or out with a CMake option (SCRIPT_LANG_*).
// The enum names every language the library knows how to build; whether a
// given one is present in this binary is a run-time question `supported`
// answers, so an embedder can degrade gracefully instead of failing to link.
#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace script {

enum class Language : std::uint8_t
{
    /// Decide from the source name's extension, else `Context::Options::language`,
    /// else the first language this build contains.
    Auto = 0,
    CSharp,
    TypeScript,
    JavaScript,
    Python,
    C,
    Cpp,
};

/// "csharp", "typescript", "javascript", "python", "c", "cpp"; "auto" for `Auto`.
const char* toString(Language) noexcept;
/// The language a name such as "csharp", "cs", "ts", ".js", "py", "c" or "c++" stands for, or `Auto`.
Language languageNamed(std::string_view name) noexcept;
/// The language a source file's extension implies, or `Auto` when none does.
/// Knows every extension the library defines, present in this build or not.
Language languageOfFile(std::string_view path) noexcept;

/// True when this build contains the language.
bool supported(Language) noexcept;
/// Every language this build contains, in the order `Auto` prefers them.
const std::vector<Language>& supportedLanguages() noexcept;

}  // namespace script
