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
                          std::shared_ptr<const std::string> source = nullptr, bool lazy = false);

/// Compile the body of a stub (`CodeBlock::kLazy`) in place: the text is parsed again from `source`, which `lazy` allows
/// only when the tree came from it as JavaScript. Throws what `compileProgram` throws.
void compileLazy(VM& vm, CodeBlock* stub);

/// Compile the text of a direct `eval` as a function that reads the caller's variables `names` through upvalues: upvalue `i`
/// is the box of `names[i]`, and it keeps the caller's `this`. Its result is the program's completion value.
CodeBlock* compileEval(VM& vm, const dyn::Node& program, std::string_view file, std::shared_ptr<const std::string> source,
                       const std::vector<String>& names, bool strict, bool home = false);

}  // namespace vm
}  // namespace internal
}  // namespace script
