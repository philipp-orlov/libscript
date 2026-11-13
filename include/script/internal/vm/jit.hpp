// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The baseline compiler's interface to the rest of the vm core. A function that has run hot is translated, one bytecode
// instruction at a time, into machine code that works on the same frame the interpreter uses: every register of the frame
// is in memory at every instruction boundary, so the interpreter and the compiled code can hand a frame to one another at
// any instruction (a call, a back edge, an exception handler).
//
// A compiled function is entered with its frame already pushed (`Frame::entry` set); it runs to the frame's return,
// pops the frame and returns the owned result (or the exception sentinel, with the frame popped and the exception
// pending), exactly as `run` does for an entry frame.
#pragma once

#include <cstdint>
#include <vector>

#include "script/internal/vm/vm.hpp"

namespace script {
namespace internal {
namespace vm {

struct CodeBlock;
struct Ins;

namespace jit {
struct CodeSpan;
}

/// What `compileTier1` makes. Owned by its code block.
struct Tier1Code
{
    std::uint8_t* rx = nullptr;               ///< where the code runs
    std::size_t bytes = 0;
    std::vector<std::uint32_t> labels;        ///< per instruction index: offset from `rx` of its code, or ~0u
    std::uint32_t exits = 0;
    void* addressFor(std::uint32_t index) const noexcept { return rx + labels[index]; }
};

/// Whether this build and host can run compiled code at all.
bool tier1Available() noexcept;
/// Translate `code`; sets `code->tier1` or, when the function is not suitable, `code->tier1Failed`.
void compileTier1(VM& vm, CodeBlock* code);
void releaseTier1(Tier1Code* code) noexcept;
/// Run the top frame, which belongs to a compiled function, from the instruction at `index`. Returns what `run` would.
JSValue enterTier1(VM& vm, Tier1Code* code, std::uint32_t index);

/// The interpreter, restricted to one instruction of the top frame: used by compiled code for every instruction it does
/// not translate itself. Returns the raw bits of `kStepFall` (went on to the next instruction), `kStepJump` (the frame's
/// pc is where to go) or the result of the function (a value, or the exception sentinel), the frame having been left.
inline constexpr std::uint64_t kStepFall = kHole;
inline constexpr std::uint64_t kStepJump = kUninit;
std::uint64_t stepInstruction(VM& vm, std::uint32_t index);

}  // namespace vm
}  // namespace internal
}  // namespace script
