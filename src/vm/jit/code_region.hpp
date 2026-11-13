// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Executable memory for the vm core's compiled code: one reservation with two views of the same pages, one read-write
// for the compiler and one read-execute for running, so no page is ever writable and executable through the same mapping
// and a compile never changes the protection of code that is running. Process-wide; thread-safe.
#pragma once

#include <cstddef>
#include <cstdint>

namespace script {
namespace internal {
namespace vm {
namespace jit {

struct CodeSpan
{
    std::uint8_t* rw = nullptr;
    std::uint8_t* rx = nullptr;
    std::size_t bytes = 0;
    explicit operator bool() const noexcept { return rx != nullptr; }
};

/// Space for `bytes` of code, or an empty span when the region is full or this host cannot make one.
CodeSpan allocateCode(std::size_t bytes) noexcept;
void releaseCode(const CodeSpan& span) noexcept;
/// Bytes of code space in use.
std::size_t codeInUse() noexcept;

}  // namespace jit
}  // namespace vm
}  // namespace internal
}  // namespace script
