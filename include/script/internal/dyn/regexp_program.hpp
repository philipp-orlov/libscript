// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// What a pattern is compiled to: for the matcher (dyn/regexp.cpp) and the compiler of native matchers (vm/jit/regexp_jit.cpp).
#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "script/internal/dyn/regexp.hpp"

namespace script {
namespace internal {
namespace dyn {
namespace regex {

using Ranges = std::vector<std::pair<char32_t, char32_t>>;

/// `max` of a loop without one.
inline constexpr std::uint32_t kUnbounded = 0xffffffffu;

/// One code unit a word, operands after it.
enum Op : std::uint32_t
{
    OpMatch,
    OpLit,        // unit
    OpLitBack,    // unit
    OpLit2,       // unit unit
    OpLit2Back,   // unit unit
    OpStr,        // offset length
    OpSet,        // set
    OpSetBack,    // set
    OpSetU,       // set
    OpSetUBack,   // set
    OpGreedy,     // set min max: the whole loop of a set, one unit at a time
    OpGreedyU,    // set min max: the same, a code point at a time
    OpSplit,      // target: try on, and that if it fails
    OpSplitLazy,  // target: try that, and on if it fails
    OpJump,       // target
    OpSave,       // register
    OpClear,      // first end: registers
    OpMark,       // register: the position, to know a loop's pass took nothing
    OpCheckProgress,  // register
    OpCheckCounted,   // register counter min: the same, once the minimum is done
    OpLoopInit,   // counter
    OpLoopEnter,  // counter min max greedy exit
    OpLoopNext,   // counter
    OpLineStart,
    OpLineStartM,
    OpLineEnd,
    OpLineEndM,
    OpWord,       // negated folds
    OpBackRef,    // group
    OpBackRefBack,
    OpBackRefFold,  // group: ignoring case
    OpBackRefFoldBack,
    OpLookBegin,  // register
    OpLookEnd,    // register
    OpNegBegin,   // register target
    OpNegEnd,     // register
};

struct CodeSet
{
    std::uint32_t low[8] = {};  ///< below 256
    Ranges high;                ///< the rest, as bare pairs to search
    char32_t highLo = 0, highHi = 0;  ///< the span of `high` (empty: 0, 0)
    bool has(std::uint32_t c) const noexcept
    {
        if (c < 256)
            return low[c >> 5] >> (c & 31) & 1;
        if (c < highLo || c > highHi)
            return false;
        if (high.size() <= 8) {
            // A few ranges (a class of some characters): looked at in turn.
            for (const auto& [lo, hi] : high)
                if (c >= lo && c <= hi)
                    return true;
            return false;
        }
        auto it = std::upper_bound(high.begin(), high.end(), c, [](char32_t v, const std::pair<char32_t, char32_t>& x) { return v < x.first; });
        return it != high.begin() && c <= std::prev(it)->second;
    }
    explicit CodeSet(const Ranges& r)
    {
        for (const auto& [lo, hi] : r) {
            for (char32_t c = lo; c <= hi && c < 256; ++c)
                low[c >> 5] |= 1u << (c & 31);
            if (hi >= 256)
                high.emplace_back(std::max<char32_t>(lo, 256), hi);
        }
        if (!high.empty()) {
            highLo = high.front().first;
            highHi = high.front().second;
            for (const auto& [lo, hi] : high) {
                highLo = std::min(highLo, lo);
                highHi = std::max(highHi, hi);
            }
        }
    }
};

/// Code for one program. `release` gives it back.
struct NativeMatcher
{
    virtual ~NativeMatcher() = default;
};

struct Program
{
    std::vector<std::uint32_t> code;
    std::vector<CodeSet> sets;
    std::u16string literals;
    std::uint32_t groups = 1;
    std::uint32_t registers = 2;
    std::vector<NamedGroup> names;
    unsigned flags = 0;
    bool anchored = false;       ///< only the start of the input can begin a match
    std::int32_t firstSet = -1;  ///< every match begins with one of these units
    std::int32_t firstUnit = -1; ///< when that is one unit, the unit

    /// The native matcher, once the program has run hot (vm/jit/regexp_jit.cpp): made once, by whichever thread gets there.
    mutable std::atomic<NativeMatcher*> native{nullptr};
    mutable std::atomic<std::uint32_t> heat{0};  ///< matches tried; past `kNoNative` there is none to be had

    Program() = default;
    ~Program();
};

/// The state of a match that a native matcher has the matcher do one instruction for.
struct StepState
{
    const Program* program;
    Matcher* matcher;
    const char16_t* s;
    std::int32_t length;
    std::int32_t pos;
    std::int32_t* regs;
    std::uint32_t* stack;
    std::size_t sp;
};
/// Runs the instruction at `pc`, one of those that only test or move (no jump): false when it fails.
bool step(StepState& state, std::uint32_t pc);
/// Where `unit` is first found in [at, length), or `length`.
std::int32_t findUnit(const char16_t* s, std::int32_t at, std::int32_t length, char16_t unit) noexcept;

}  // namespace regex
}  // namespace dyn
}  // namespace internal
}  // namespace script
