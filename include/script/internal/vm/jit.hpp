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
#include <utility>
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
    void* entry = nullptr;                    ///< the code for the first instruction
    std::size_t bytes = 0;
    std::vector<std::uint32_t> labels;        ///< per instruction index: offset from `rx` of its code, or ~0u
    std::vector<ValidityCell*> validities;    ///< cells the code reads, each held with a count of its own
    void* addressFor(std::uint32_t index) const noexcept { return rx + labels[index]; }
};

/// The native behind `Math.floor`, `Math.sqrt` and the other one-argument functions of `Math`: a function whose `data` is
/// a `double (*)(double)`, which compiled code calls directly.
FastNative mathUnaryNative() noexcept;
/// `String.prototype.charAt`, and with data 1 `charCodeAt`: compiled code reads a character itself.
FastNative stringCharNative() noexcept;
/// `Array.prototype.push` and `pop`: compiled code does the usual case itself.
FastNative arrayPushNative() noexcept;
FastNative arrayPopNative() noexcept;
/// `String.fromCharCode`: compiled code gives a one-argument call of a character below 256 its kept string.
FastNative stringFromCharCodeNative() noexcept;
/// Whether a `Math` native's data is `Math.sqrt`'s, which compiled code does with one instruction.
bool mathUnaryIsSqrt(const void* data) noexcept;

/// Optimized code of a function that changes no object it did not make and no global, and calls only itself, holds some
/// registers without a count (`regs`), and passes them to its calls of itself. Whatever could run other code there -- a
/// failed guard, a slow path -- counts first what every such frame from there up holds (`scriptVmBorrowGate`).
struct BorrowInfo
{
    std::vector<std::uint16_t> regs;
    std::uint8_t retired = 0;  ///< the code is no longer the function's entry: its calls of itself go the long way
};

/// What the optimizing compiler makes. Owned by its code block.
struct Tier2Code
{
    BorrowInfo* borrow = nullptr;                                 ///< owned
    std::uint8_t* rx = nullptr;
    std::size_t bytes = 0;
    void* entry = nullptr;                                        ///< the function's entry, as the baseline code's
    std::vector<std::pair<std::uint32_t, std::uint32_t>> osr;     ///< (loop header, offset from `rx` of the way in)
    std::vector<ValidityCell*> validities;
    std::vector<JSValue> held;                                    ///< values the code compares against, each counted
    std::vector<std::uint8_t> siteExits;                          ///< exits so far at each instruction
    Tier2Code* older = nullptr;                                   ///< a version it replaced, kept while frames may be in it
    void* osrAddress(std::uint32_t header) const noexcept
    {
        for (const auto& e : osr)
            if (e.first == header)
                return rx + e.second;
        return nullptr;
    }
};
void releaseTier2(Tier2Code* code, VM* vm) noexcept;

/// Whether this build and host can run compiled code at all.
bool tier1Available() noexcept;
/// Translate `code`; sets `code->tier1` or, when the function is not suitable, `code->tier1Failed`.
void compileTier1(VM& vm, CodeBlock* code);
void releaseTier1(Tier1Code* code, VM* vm) noexcept;
/// A copy of `code` that runs the function's calls of itself in place one level deep, made once and kept while a function
/// holds it (null when the function is not one for it); it comes with no count of its own. See self_inline.cpp.
CodeBlock* selfInline(VM& vm, CodeBlock* code);
/// The block `f` (whose block is `code`, turning hot) runs from now on: `selfInline`'s copy when there is one, which `f` then
/// holds in place of `code`. The copy starts cold, in the interpreter, which learns what its instructions make.
CodeBlock* tierUpBlock(VM& vm, struct JSFunction* f, CodeBlock* code);
/// Run the top frame, which belongs to a compiled function, from the instruction at `index`. Returns what `run` would.
JSValue enterTier1(VM& vm, Tier1Code* code, std::uint32_t index);

/// The interpreter, restricted to one instruction of the top frame: used by compiled code for every instruction it does
/// not translate itself. Returns the raw bits of `kStepFall` (went on to the next instruction), `kStepJump` (the frame's
/// pc is where to go) or the result of the function (a value, or the exception sentinel), the frame having been left.
inline constexpr std::uint64_t kStepFall = kHole;
inline constexpr std::uint64_t kStepJump = kUninit;
std::uint64_t stepInstruction(VM& vm, std::uint32_t index);
/// Records in the element profile of instruction `index` (elemprof, and the typed kind) what `base` is.
void noteElemBase(CodeBlock* code, std::uint32_t index, JSValue base);
/// What the interpreter does when the call that the instruction at `index` made returned the exception sentinel: look for
/// a handler in the top frame; `kStepJump` if there is one, else the frame is left and the exception sentinel returned.
std::uint64_t stepThrow(VM& vm, std::uint32_t index);

/// One instruction's work without the interpreter's dispatch (interp.cpp): 1 when done, 0 for a pending exception, with
/// the frame's pc set after the instruction. For compiled code that has no template of its own for the instruction, or
/// whose template gave up.
int jitGetGlobal(VM& vm, std::uint32_t index);
int jitGetProp(VM& vm, std::uint32_t index);
int jitPutProp(VM& vm, std::uint32_t index);
int jitInitProp(VM& vm, std::uint32_t index);
int jitDefPrivate(VM& vm, std::uint32_t index);
int jitNewObject(VM& vm, std::uint32_t index);
int jitNewArray(VM& vm, std::uint32_t index);
int jitArrayPush(VM& vm, std::uint32_t index);
int jitBinary(VM& vm, std::uint32_t index);
int jitBinaryImm(VM& vm, std::uint32_t index);
int jitInstanceOf(VM& vm, std::uint32_t index);
int jitIn(VM& vm, std::uint32_t index);
int jitToString(VM& vm, std::uint32_t index);
int jitToNumber(VM& vm, std::uint32_t index);
int jitUnary(VM& vm, std::uint32_t index);
int jitCopyBox(VM& vm, std::uint32_t index);
int jitRestSlice(VM& vm, std::uint32_t index);
int jitSpreadObj(VM& vm, std::uint32_t index);
int jitKeysOf(VM& vm, std::uint32_t index);
int jitLoadCallee(VM& vm, std::uint32_t index);
int jitNewTarget(VM& vm, std::uint32_t index);
int jitIterInit(VM& vm, std::uint32_t index);
int jitIterNext(VM& vm, std::uint32_t index);
int jitIterClose(VM& vm, std::uint32_t index);
int jitGetElem(VM& vm, std::uint32_t index);
int jitGetSuperProp(VM& vm, std::uint32_t index);
int jitPutElem(VM& vm, std::uint32_t index);
int jitClosure(VM& vm, std::uint32_t index);
int jitNewBox(VM& vm, std::uint32_t index);

}  // namespace vm
}  // namespace internal
}  // namespace script
