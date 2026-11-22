// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// What the optimizing compiler decides before it emits anything: where the basic blocks are, which registers are live where,
// which definitions and uses of a register belong together (a "web": one value's life, whatever the bytecode compiler called
// the register), and what each web holds, taken from what the instructions have made so far (`CodeBlock::prof`).
//
// A web that only ever holds int32s becomes an int32 variable kept in a machine register or a stack slot, one that holds
// numbers becomes a double, and everything else stays in the frame as the baseline compiler keeps it. The frame is still
// the truth at every point where something else could look at it: the compiler writes a variable back (boxed) before an
// instruction that reads its register through the frame, and in the stub of a guard that fails.
#pragma once

#include <cstdint>
#include <vector>

#include "script/internal/vm/bytecode.hpp"

namespace script {
namespace internal {
namespace vm {
namespace opt {

/// What a web holds. The order is the order of the lattice: a web takes the greatest of its definitions.
enum class Rep : std::uint8_t { None, I32, F64, Mem };

/// Bits of `CodeBlock::prof` besides `profBit`'s: set when a guard of the optimizing compiler failed at the instruction, so
/// that the next compile does not speculate there.
inline constexpr std::uint8_t kProfFailed = 16;

/// The registers an instruction reads and writes. `ok` is false for an instruction the optimizing compiler does not model:
/// a function with one is left to the baseline compiler.
struct Effects
{
    std::vector<std::uint16_t> rd;
    std::vector<std::uint16_t> wr;
    bool ok = true;
};

Effects effectsOf(const CodeBlock& code, std::uint32_t i);
/// Fills in `CodeBlock::suspendAt` and the rest of a generator's block: which registers each suspension has to keep.
void planSuspends(CodeBlock& code);
/// Instruction index after `i` (skips the cache word).
std::uint32_t nextOf(const CodeBlock& code, std::uint32_t i);
/// Where the jump instruction at `i` goes, or -1 when `i` is not a jump.
std::int64_t jumpTarget(const CodeBlock& code, std::uint32_t i);
bool isConditionalJump(Op op);

struct Loc
{
    enum Kind : std::uint8_t { Reg, Stack } kind = Stack;
    std::uint8_t reg = 0;    ///< a Reg (rax-r15) for I32, an xmm number for F64
};

struct Web
{
    std::uint16_t reg = 0;
    Rep rep = Rep::None;
    Loc loc;
    std::int32_t slot = -1;       ///< index of the web's own stack slot (every typed web has one)
    std::uint32_t first = ~0u, last = 0;
    std::uint32_t weight = 0;
    bool hasEntryDef = false;
};

/// A typed web live at an instruction, with the register it is the value of.
struct LiveWeb
{
    std::uint16_t reg;
    std::uint32_t web;
};

struct Plan
{
    bool ok = false;
    std::uint32_t n = 0;
    std::vector<Web> webs;
    /// Per instruction: the web of the register it writes (-1 when none or Mem); one entry per instruction start.
    std::vector<std::int32_t> defWeb;
    /// Per instruction: the web of each register it reads, in `Effects::rd` order (-1 for Mem).
    std::vector<std::vector<std::int32_t>> useWebs;
    /// Per instruction: the typed webs live just before it runs.
    std::vector<std::vector<LiveWeb>> liveBefore;
    /// Whether each instruction starts a basic block, and the ones that are loop headers (a backward jump goes there).
    std::vector<std::uint8_t> isLeader, isLoopHeader;
    /// Per instruction: an int32 add or subtract whose result only ever reaches ToInt32 (`(a + b) | 0`), so it may wrap.
    std::vector<std::uint8_t> wraps;
    std::vector<std::uint32_t> loopHeaders;
    /// Typed webs that a function entry loads from the frame (parameters) with a guard.
    std::vector<std::uint32_t> entryWebs;
    std::uint32_t stackSlots = 0;
    std::uint32_t typedWebs = 0;
};

/// Analyse `code` for the optimizing compiler. `plan.ok` is false when it is not suitable.
Plan analyse(const CodeBlock& code);

}  // namespace opt
}  // namespace vm
}  // namespace internal
}  // namespace script
