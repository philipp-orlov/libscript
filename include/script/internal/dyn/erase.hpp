// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Type erasure: a checked TypeScript tree in, a plain JavaScript tree out.
//
// Keeping this separate is what leaves the compiler and the interpreter exactly
// as they were. Everything with no run-time meaning is removed here, and the
// three constructs that do have one -- enums, namespaces and constructor
// parameter properties -- are lowered into ordinary syntax.
#pragma once

#include "script/internal/dyn/ast.hpp"

namespace script {
namespace internal {
namespace dyn {

/// Rewrites `program` in place. Afterwards no node of a TypeScript-only kind
/// remains and no node carries a type annotation.
void erase(Node& program);

}  // namespace dyn
}  // namespace internal
}  // namespace script
