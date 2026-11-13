// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The bytecode compiler: the syntax tree of the shared ECMAScript parser in, a `CodeBlock` out.
#pragma once

#include "script/internal/dyn/ast.hpp"
#include "script/internal/vm/bytecode.hpp"

namespace script {
namespace internal {
namespace vm {

/// Compile a whole program. The block has one count, which the caller owns. Throws `Error(SyntaxError)` for what the
/// language forbids and `Error(NotSupported)` for what this core does not compile yet.
CodeBlock* compileProgram(VM& vm, const dyn::Node& program, std::string_view file,
                          std::shared_ptr<const std::string> source = nullptr);

}  // namespace vm
}  // namespace internal
}  // namespace script
