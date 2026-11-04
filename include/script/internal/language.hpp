// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The hook a language plugs into.
//
// A language is a front end: it turns source into something one of the two
// engines runs. It registers itself with one `LanguageInfo`, and the public
// `Context` never names a language directly -- it looks the info up by id or
// by file extension and calls through the function pointers. Adding a
// language means writing a front end against one engine's input form, filling
// in one of these and adding it to the table in src/core/language.cpp. See
// docs/adding-a-language.md.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "script/diagnostic.hpp"
#include "script/language.hpp"
#include "script/value.hpp"

namespace script {
namespace internal {

struct ContextImpl;

/// Which machine runs a language's programs.
enum class EngineKind : std::uint8_t
{
    None = 0,
    Typed,    ///< resolved, typed AST in -> typed bytecode, optionally native code
    Dynamic,  ///< ECMAScript AST in -> dynamic bytecode
};

/// A compiled program of any language: what `Assembly` wraps. Each engine
/// derives its own.
struct CompiledUnit
{
    virtual ~CompiledUnit() = default;

    Language language = Language::Auto;
    std::string name;
    /// The context that compiled it, and a token that expires with that
    /// context, so a handle dropped on another thread can take the lock and
    /// one dropped after the context is gone knows not to try.
    ContextImpl* context = nullptr;
    std::weak_ptr<const bool> alive;

    virtual std::vector<std::string> methods() const { return {}; }
    virtual std::string entryPoint() const { return {}; }
    virtual std::size_t imageSize() const noexcept { return 0; }
};

struct LanguageInfo
{
    Language id = Language::Auto;
    const char* name = "";              ///< "csharp"
    const char* const* extensions = nullptr;  ///< {".cs", nullptr}
    EngineKind engine = EngineKind::None;

    /// Parse, check and compile. Returns null and fills `diagnostics` when the
    /// program does not compile; throws `Error(SyntaxError)` when it does not
    /// parse at all.
    std::shared_ptr<CompiledUnit> (*compile)(ContextImpl& rt, std::string_view source,
                                             std::string_view name,
                                             std::vector<Diagnostic>& diagnostics) = nullptr;
    /// Run a unit this language compiled.
    Value (*run)(ContextImpl& rt, CompiledUnit& unit, const std::vector<Value>& args) = nullptr;
    /// Accept ambient declarations (`Context::declare`); null when the
    /// language has no such notion.
    void (*declare)(ContextImpl& rt, std::string_view declarations) = nullptr;
};

/// The languages compiled into this build, in `Auto` preference order.
const std::vector<const LanguageInfo*>& languageTable() noexcept;
/// The info for a language, or null when the build lacks it.
const LanguageInfo* languageInfo(Language id) noexcept;

}  // namespace internal
}  // namespace script
