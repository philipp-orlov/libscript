// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// How much native stack the calling thread has left. The recursive parts of the library -- the parsers, the checkers,
// the code generators and the builtins that walk nested data -- ask before they go one level deeper, so source or data
// nested too deeply for the thread's stack is refused with an error instead of killing the process. The answer comes
// from the thread's real stack bounds, so it holds for a 256 KiB worker thread as well as for the main thread.
#pragma once

#include <cstddef>
#include <cstdint>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace script::internal {

/// Native stack kept free for what runs between two checks: the frames of one level of recursion, the formatting of the
/// error, and the unwinding that follows it.
inline constexpr std::size_t kStackReserve = 96 * 1024;

/// The lowest address of the calling thread's stack that may be used, or 0 when this platform cannot say. Cached per
/// thread; zero means "not looked up yet", and `nativeStackFloorLookup` stores 1 for "cannot be determined".
extern thread_local std::uintptr_t nativeStackFloorCache;
std::uintptr_t nativeStackFloorLookup() noexcept;

/// True when fewer than `reserve` bytes of the calling thread's stack remain. Always false where the stack's bounds
/// are unknown, so such a platform keeps only the depth counters it already had.
inline bool nativeStackLow(std::size_t reserve = kStackReserve) noexcept
{
    std::uintptr_t floor = nativeStackFloorCache;
    if (floor == 0)
        floor = nativeStackFloorLookup();
    if (floor == 1)
        return false;
    // The frame address, not the address of a local: with AddressSanitizer's use-after-return detection a local can
    // live on a heap-allocated "fake stack" far from the real one.
#if defined(_MSC_VER)
    const std::uintptr_t here = reinterpret_cast<std::uintptr_t>(_AddressOfReturnAddress());
#else
    const std::uintptr_t here = reinterpret_cast<std::uintptr_t>(__builtin_frame_address(0));
#endif
    return here < floor + reserve;
}

/// Raise the error for source or data nested too deeply: `Error(CompileError)` carrying "`what` is nested too deeply".
[[noreturn]] void throwTooDeep(const char* what);
/// For the passes that walk a syntax tree recursively -- the checkers, the code generators, the cloner. The parsers
/// limit the nesting they build, but a tree they accept can still be deeper than a small thread stack allows.
inline void checkNativeStack(const char* what = "expression")
{
    if (nativeStackLow())
        throwTooDeep(what);
}

/// Deepest nesting of expressions, in levels of parentheses, brackets, calls and prefix operators, that a front end
/// accepts. Real code stays far below it; a generator that goes past it has a bug.
inline constexpr std::size_t kMaxExpressionNesting = 500;
/// Longest run of `a + b + c + ...`, `a.b.c...` or `f()()()...` a front end accepts. The parser builds such a run
/// iteratively, but the checker, the code generator and the destructor of the tree all walk it recursively.
inline constexpr std::size_t kMaxChainLength = 2000;

/// Counts one level of recursion for as long as the object lives, so every way out of the function -- including the
/// exception that reports the overflow -- gives the level back.
class NestingScope
{
public:
    explicit NestingScope(std::size_t& counter) noexcept : counter_(counter) { ++counter_; }
    ~NestingScope() { --counter_; }
    NestingScope(const NestingScope&) = delete;
    NestingScope& operator=(const NestingScope&) = delete;

    /// True when this level is past `limit`, or the thread's stack is nearly used up.
    bool tooDeep(std::size_t limit = kMaxExpressionNesting) const noexcept
    {
        return counter_ > limit || nativeStackLow();
    }

private:
    std::size_t& counter_;
};

}  // namespace script::internal
