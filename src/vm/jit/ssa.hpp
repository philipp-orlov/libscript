// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The graph of the SSA tier: one value per (instruction, register it writes), per register at the function's entry and
// per phi, and for each basic block the value each register holds where the block starts. Built in one pass over the
// blocks in bytecode order (a loop header gets a phi for each register the loop writes); phis that turn out to join one
// value only are then replaced by it. No code is made here: see ssa_emit.inc.
#pragma once

#include <cstdint>
#include <vector>

#include "script/internal/vm/bytecode.hpp"
#include "opt_analysis.hpp"

namespace script {
namespace internal {
namespace vm {
namespace ssa {

inline constexpr std::uint32_t kNone = ~0u;

using Bits = std::vector<std::uint64_t>;
inline bool bitTest(const Bits& b, std::size_t i) { return (b[i >> 6] >> (i & 63)) & 1; }
inline void bitSet(Bits& b, std::size_t i) { b[i >> 6] |= std::uint64_t(1) << (i & 63); }
inline void bitClear(Bits& b, std::size_t i) { b[i >> 6] &= ~(std::uint64_t(1) << (i & 63)); }

struct Value
{
    enum Kind : std::uint8_t { Entry, Def, Phi } kind = Def;
    std::uint16_t reg = 0;      ///< the register it was made for
    std::uint32_t at = 0;       ///< Def: the instruction; Phi: the block; Entry: 0
    std::vector<std::uint32_t> in;  ///< Phi: the value from each predecessor, in `Block::preds` order
};

struct Block
{
    std::uint32_t start = 0, end = 0;   ///< instruction indices [start, end)
    std::uint32_t last = 0;             ///< the index of its last instruction
    std::vector<std::uint32_t> preds, succs;  ///< block indices; succs[0] is the fall-through, when there is one
    bool loopHeader = false;
    bool reached = false;
    bool noEntry = false;  ///< a loop a generator yields in: the baseline code does not enter there (it resumes there)
    std::uint32_t loopEnd = 0;          ///< a header: the end of the last block that jumps back to it
    std::vector<std::uint32_t> phis;    ///< values
    std::vector<std::uint32_t> nameIn;  ///< per register: the value it holds at the start (kNone when not live)
    std::vector<std::uint32_t> nameOut; ///< per register: the value it holds at the end
    Bits liveIn, liveOut;
};

struct Graph
{
    bool ok = false;
    const char* why = "";
    std::uint32_t n = 0, nregs = 0;
    std::vector<Value> values;
    std::vector<Block> blocks;
    std::vector<std::uint32_t> blockOf;  ///< per instruction start: its block (kNone elsewhere)
    std::vector<std::uint32_t> defBase;  ///< per instruction: the value of its first register written (kNone: none)
    std::vector<std::uint8_t> isIns;
    /// Per loop header block: registers some instruction of the loop writes, or whose frame slot a call there covers.
    std::vector<Bits> loopWrites;
    /// What a handler that covers an instruction finds live there (one that throws goes on in the baseline code, at the
    /// handler): for each instruction, where in `heldSets` its registers are (0: the empty first). Nothing here compiles the
    /// handlers themselves.
    std::vector<std::uint32_t> heldAt;
    std::vector<std::vector<std::uint16_t>> heldSets;
    const std::vector<std::uint16_t>& held(std::uint32_t i) const
    {
        return heldSets[heldAt.empty() ? 0 : heldAt[i]];
    }

    std::uint32_t entryValue(unsigned reg) const { return reg; }  ///< values 0 .. nregs-1 are the entry values
    /// For each instruction of block `b`, in order: the registers live after it.
    std::vector<Bits> liveAfter(const CodeBlock& code, std::uint32_t b) const;
};

/// The registers instruction `i` reads and writes, as `opt::effectsOf` says, and also those it reads that it does not
/// name (`JNotCode` looks at the `this` after the callee).
opt::Effects effects(const CodeBlock& code, std::uint32_t i);

/// Whether `JNotCode` at `i` leaves the code when the function is another: the call it guards is the plain call that follows
/// the callee's code run in place, and the guard has not failed before. That call is then not in the graph.
bool guardExits(const CodeBlock& code, std::uint32_t i);

/// Whether frames that `new` made run the function often: its returns look at `this` then.
bool constructs(const CodeBlock& code);

/// Whether the SSA tier can take the instruction at all (its registers are modelled, it has a way to be compiled).
bool supported(const CodeBlock& code, std::uint32_t i);

Graph build(const CodeBlock& code);

/// Whether tier 2 should compile `code` instead: it uses there what the SSA tier does not have (a self-inlined recursion
/// it runs with borrowed frames, a property a loop reads and writes on an object the loop does not change, which it keeps
/// in a register, a tiny callee in a loop, which it runs in place). `why` says which.
bool preferTier2(const CodeBlock& code, const VM& vm, const char** why);

}  // namespace ssa
}  // namespace vm
}  // namespace internal
}  // namespace script
