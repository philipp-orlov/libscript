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
#include <initializer_list>
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
/// Set when a value SSA code took for an int32 or a double from what the instruction had made (a load) was something
/// else: the next compile keeps that value boxed (the instruction itself has not failed).
inline constexpr std::uint8_t kProfNoSpec = 32;
/// Set on a load taken for an int32 that was a double there: it is taken for a number from then on.
inline constexpr std::uint8_t kProfWidened = 64;

/// The registers an instruction reads and writes. `ok` is false for an instruction the optimizing compiler does not model:
/// a function with one is left to the baseline compiler.
/// A short list of registers: the first `kInline` in place (an instruction reads one to three, a call its window),
/// the rest on the heap. `effectsOf` is asked for every instruction many times in one compile; a vector for each
/// answer was two calls of the general allocator.
class RegList
{
public:
    using value_type = std::uint16_t;
    using iterator = std::uint16_t*;
    using const_iterator = const std::uint16_t*;
    static constexpr std::uint32_t kInline = 12;

    RegList() noexcept = default;
    RegList(std::initializer_list<std::uint16_t> l) { assign(l.begin(), l.end()); }
    RegList& operator=(std::initializer_list<std::uint16_t> l)
    {
        assign(l.begin(), l.end());
        return *this;
    }
    RegList(const RegList& o) { assign(o.begin(), o.end()); }
    RegList(RegList&& o) noexcept { take(o); }
    RegList& operator=(const RegList& o)
    {
        if (this != &o)
            assign(o.begin(), o.end());
        return *this;
    }
    RegList& operator=(RegList&& o) noexcept
    {
        if (this != &o) {
            release();
            take(o);
        }
        return *this;
    }
    ~RegList() { release(); }

    void push_back(std::uint16_t r)
    {
        if (size_ == cap_)
            grow(cap_ * 2);
        data_[size_++] = r;
    }
    void clear() noexcept { size_ = 0; }
    std::size_t size() const noexcept { return size_; }
    bool empty() const noexcept { return size_ == 0; }
    iterator begin() noexcept { return data_; }
    iterator end() noexcept { return data_ + size_; }
    const_iterator begin() const noexcept { return data_; }
    const_iterator end() const noexcept { return data_ + size_; }
    std::uint16_t& operator[](std::size_t i) noexcept { return data_[i]; }
    const std::uint16_t& operator[](std::size_t i) const noexcept { return data_[i]; }
    std::uint16_t& back() noexcept { return data_[size_ - 1]; }
    const std::uint16_t& back() const noexcept { return data_[size_ - 1]; }
    const std::uint16_t* data() const noexcept { return data_; }
    template <class It>
    void assign(It first, It last)
    {
        size_ = 0;
        for (; first != last; ++first)
            push_back(std::uint16_t(*first));
    }

private:
    void grow(std::uint32_t cap)
    {
        auto* next = new std::uint16_t[cap];
        for (std::uint32_t i = 0; i < size_; ++i)
            next[i] = data_[i];
        release();
        data_ = next;
        cap_ = cap;
    }
    void release() noexcept
    {
        if (data_ != inline_)
            delete[] data_;
        data_ = inline_;
        cap_ = kInline;
    }
    void take(RegList& o) noexcept
    {
        size_ = o.size_;
        if (o.data_ == o.inline_) {
            data_ = inline_;
            cap_ = kInline;
            for (std::uint32_t i = 0; i < size_; ++i)
                inline_[i] = o.inline_[i];
        } else {
            data_ = o.data_;
            cap_ = o.cap_;
            o.data_ = o.inline_;
            o.cap_ = kInline;
        }
        o.size_ = 0;
    }
    std::uint16_t* data_ = inline_;
    std::uint32_t size_ = 0, cap_ = kInline;
    std::uint16_t inline_[kInline];
};

struct Effects
{
    RegList rd;
    RegList wr;
    bool ok = true;
};

Effects effectsOf(const CodeBlock& code, std::uint32_t i);
/// The registers live before each instruction of any function (`words` words to an instruction), what its handlers find
/// counted; false when `effects` does not know an instruction.
bool liveBefore(const CodeBlock& code, Effects (*effects)(const CodeBlock&, std::uint32_t), std::vector<std::uint64_t>& live,
                std::size_t& words);
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
