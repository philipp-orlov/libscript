// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The C family's compile and run hooks, shared by the C and C++
// registrations: source goes through the C lexer and parser (with the
// dialect the language asks for) into the typed engine's syntax tree, then
// through the C# checker and emitter exactly as a C# program does.
#pragma once

#include <memory>
#include <string_view>
#include <vector>

#include "script/internal/language.hpp"

namespace script {
namespace internal {
namespace typed {

std::shared_ptr<CompiledUnit> compileCFamily(ContextImpl& ctx, std::string_view source,
                                             std::string_view name,
                                             std::vector<Diagnostic>& diagnostics, Language language);
Value runCFamily(ContextImpl& ctx, CompiledUnit& unit, const std::vector<Value>& args);

}  // namespace typed
}  // namespace internal
}  // namespace script
