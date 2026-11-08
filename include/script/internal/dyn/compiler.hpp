// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Compiler entry point: syntax tree in, bytecode out.
#pragma once

#include <memory>
#include <string>

#include "script/internal/dyn/ast.hpp"
#include "script/internal/dyn/bytecode.hpp"

namespace script {
namespace internal {
namespace dyn {

struct Runtime;

/// Compile a whole program. The result is a zero-argument function whose
/// `var` and function declarations target the global object and whose value is
/// the completion value of the last expression statement. `sloppyThis` is JavaScript's non-strict rule for `this` in a
/// plain call, for code that no "use strict" directive or class body covers.
std::shared_ptr<FunctionCode> compileProgram(Runtime& rt, const Node& program,
                                             const std::string& sourceName, bool sloppyThis = false,
                                             std::shared_ptr<const std::string> sourceText = nullptr);

/// The code of a direct `eval`: a function nested in the one that called it, seeing what `scope` says it sees.
std::shared_ptr<FunctionCode> compileEval(Runtime& rt, const Node& program, const std::string& sourceName,
                                          const EvalScope& scope, std::shared_ptr<const std::string> sourceText = nullptr);

}  // namespace dyn
}  // namespace internal
}  // namespace script
