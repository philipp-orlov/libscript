// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The type checker: syntax tree in, diagnostics out.
//
// The checker never rewrites the tree and never stops the pipeline by itself;
// it reports, and the caller decides. Everything it allocates -- every type,
// every scope -- belongs to one `checkProgram` call and goes away with it.
#pragma once

#include <string>
#include <vector>

#include "script/diagnostic.hpp"
#include "script/internal/dyn/ast.hpp"
#include "script/internal/dyn/types.hpp"

namespace script {
namespace internal {
namespace dyn {

/// The declarations every program starts with: the globals the engine itself
/// provides, written in the language they describe. Parsed once per check,
/// which costs a fraction of a millisecond and saves a second hand-built
/// description of the runtime that could drift from this one.
const char* globalDeclarations() noexcept;

/// Type-check `program`. An empty result means it type-checks. `ambient` holds
/// the embedder's own declarations, and `hostGlobals` the names it installed on
/// the global object without declaring; those resolve as `any`.
std::vector<Diagnostic> checkProgram(const Node& program, const std::string& sourceName,
                                     const CheckOptions& options,
                                     const std::vector<String>& hostGlobals,
                                     const std::string& ambient);

}  // namespace dyn
}  // namespace internal
}  // namespace script
