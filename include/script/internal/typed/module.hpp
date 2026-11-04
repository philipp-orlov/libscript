// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The on-disk form of a compiled assembly.
//
// `Assembly::serialize` writes this and `Context::load` reads it. The point is
// that loading does no work beyond fixing up references: no lexing, no
// parsing, no type checking, no layout. A process that starts from an image is
// running the first instruction of `Main` in the time it takes to read the
// bytes and walk two tables.
//
// The format is deliberately flat and little-endian throughout, so a future
// loader can map it instead of reading it, and so the phase-2 code generators
// can be handed a body without a parse step in between.
//
//   magic   "CSIL"
//   version u32                  -- bumped whenever anything below changes
//   strings count, then each as (u32 length, UTF-8 bytes)
//   types   count, then each type definition
//   methods count, then each method definition, bodies included
//   refs    the type and method reference tables the instructions index
//   literals the string constants `LdStr` indexes
//
// A reference to something this assembly defines is an index; a reference to
// anything else -- the core library, a host class, a `List<int>` -- is a name
// resolved when the image is loaded. That is what makes an image portable
// between processes without pinning it to one build of the engine.
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "script/internal/common.hpp"
#include "script/internal/typed/metadata.hpp"

namespace script {
namespace internal {
namespace typed {

struct Runtime;

constexpr std::uint32_t kImageMagic = 0x4C495343u;  // "CSIL"
constexpr std::uint32_t kImageVersion = 1;

std::vector<std::uint8_t> writeImage(const AssemblyImpl& assembly);
std::shared_ptr<AssemblyImpl> readImage(Runtime& rt, const void* data, std::size_t bytes);

/// Resolve a type by its printed name, constructing `T[]`, `List<T>` and the
/// other built-in generics if this context has not seen them yet.
const TypeInfo* resolveTypeName(Runtime& rt, StringView fullName);

}  // namespace typed
}  // namespace internal
}  // namespace script
