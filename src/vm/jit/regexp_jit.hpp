// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Native matchers for regular expressions: the instructions of a program (dyn/regexp_program.hpp) as machine code, with
// the same stack of choices the matcher keeps.
#pragma once

#include <string_view>

#include "script/internal/dyn/regexp.hpp"

namespace script {
namespace internal {
namespace vm {
namespace jit {

/// `regex::exec`, by the program's native matcher once it has run hot and where this build can make one.
bool regexExec(const dyn::regex::Program& program, std::u16string_view input, std::size_t start, dyn::regex::Matcher& matcher);

}  // namespace jit
}  // namespace vm
}  // namespace internal
}  // namespace script
