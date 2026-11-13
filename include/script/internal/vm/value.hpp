// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The new core's value: eight bytes, NaN-boxed with JavaScriptCore's 64-bit encoding, which stores a pointer to a cell as
// itself so that counting never unmasks one.
//
//   pointer to a cell   0000 pppp pppp pppp        top 16 bits zero, low 4 bits zero (cells are 16-byte aligned)
//   int32               FFFE 0000 iiii iiii
//   double              the double's bits + 0002 0000 0000 0000   (so 0002.. to FFF2..)
//   null                0x02     false / true  0x06 / 0x07     undefined  0x0A
//   hole (array)        0x12     never visible to a program
//   uninitialised (TDZ) 0x22     never visible to a program
//   exception sentinel  0        only as a return value: "an exception is pending in the VM"
//
// A `JSValue` has no destructor and no copy semantics: counting is explicit (`retain`, `release`), and `Handle` is the
// RAII form for C++ code that keeps a value across a call that can run script.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

#include "script/value.hpp"

namespace script {
namespace internal {
namespace vm {

inline constexpr std::uint64_t kNumberTag = 0xFFFE000000000000ull;  // all set: int32; any set: number
inline constexpr std::uint64_t kDoubleOffset = 0x0002000000000000ull;
inline constexpr std::uint64_t kOtherTag = 0x2;
inline constexpr std::uint64_t kNotCellMask = kNumberTag | kOtherTag;
inline constexpr std::uint64_t kNull = 0x02, kFalse = 0x06, kTrue = 0x07, kUndefined = 0x0A, kHole = 0x12, kUninit = 0x22;
inline constexpr std::uint64_t kCanonicalNaN = 0x7FF8000000000000ull;
/// Cells live below this address; the heap is made to keep to it (a pointer is a value as it stands).
inline constexpr std::uint64_t kAddressLimit = 1ull << 47;

using CellPtr = detail::GCObject*;

struct JSValue
{
    std::uint64_t bits;

    constexpr bool operator==(JSValue o) const noexcept { return bits == o.bits; }
    constexpr bool operator!=(JSValue o) const noexcept { return bits != o.bits; }

    // ---- predicates ----
    constexpr bool isCell() const noexcept { return (bits & kNotCellMask) == 0 && bits != 0; }
    constexpr bool isInt32() const noexcept { return (bits & kNumberTag) == kNumberTag; }
    constexpr bool isNumber() const noexcept { return (bits & kNumberTag) != 0; }
    constexpr bool isDouble() const noexcept { return isNumber() && !isInt32(); }
    constexpr bool isBool() const noexcept { return (bits & ~1ull) == kFalse; }
    constexpr bool isNull() const noexcept { return bits == kNull; }
    constexpr bool isUndefined() const noexcept { return bits == kUndefined; }
    constexpr bool isNullish() const noexcept { return (bits & ~8ull) == kNull; }
    constexpr bool isHole() const noexcept { return bits == kHole; }
    constexpr bool isUninit() const noexcept { return bits == kUninit; }
    constexpr bool isException() const noexcept { return bits == 0; }

    // ---- reading ----
    CellPtr asCell() const noexcept { return reinterpret_cast<CellPtr>(static_cast<std::uintptr_t>(bits)); }
    constexpr std::int32_t asInt32() const noexcept { return static_cast<std::int32_t>(static_cast<std::uint32_t>(bits)); }
    double asDouble() const noexcept
    {
        const std::uint64_t raw = bits - kDoubleOffset;
        double d;
        std::memcpy(&d, &raw, sizeof d);
        return d;
    }
    /// Any number as a double.
    double asNumber() const noexcept { return isInt32() ? double(asInt32()) : asDouble(); }
    constexpr bool asBool() const noexcept { return bits == kTrue; }

    // ---- making ----
    static constexpr JSValue raw(std::uint64_t bits) noexcept { return JSValue{bits}; }
    static constexpr JSValue null() noexcept { return JSValue{kNull}; }
    static constexpr JSValue undefined() noexcept { return JSValue{kUndefined}; }
    static constexpr JSValue hole() noexcept { return JSValue{kHole}; }
    static constexpr JSValue uninit() noexcept { return JSValue{kUninit}; }
    static constexpr JSValue exception() noexcept { return JSValue{0}; }
    static constexpr JSValue boolean(bool b) noexcept { return JSValue{b ? kTrue : kFalse}; }
    static constexpr JSValue int32(std::int32_t i) noexcept { return JSValue{kNumberTag | static_cast<std::uint32_t>(i)}; }
    /// A double as it is: no narrowing to int32. Every NaN becomes the canonical one, so no NaN can look like another tag.
    static JSValue boxDouble(double d) noexcept
    {
        std::uint64_t b;
        std::memcpy(&b, &d, sizeof b);
        if (d != d)
            b = kCanonicalNaN;
        return JSValue{b + kDoubleOffset};
    }
    /// A number made by arithmetic: an int32 when it is an integer in range and not -0, else a double. The only way C++
    /// code makes a number.
    static JSValue boxNumber(double d) noexcept
    {
        if (d >= -2147483648.0 && d <= 2147483647.0) {
            const std::int32_t i = static_cast<std::int32_t>(d);
            if (double(i) == d && !(i == 0 && std::signbit(d)))
                return int32(i);
        }
        return boxDouble(d);
    }
    /// A cell as it is, uncounted.
    static JSValue cell(CellPtr c) noexcept { return JSValue{static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(c))}; }
};
static_assert(sizeof(JSValue) == 8, "a value is eight bytes");

/// One more count for the cell a value holds, if it holds one.
[[gnu::always_inline]] inline void retain(JSValue v) noexcept
{
    if (v.isCell())
        detail::incRef(v.asCell());
}
/// One count fewer; the cell is destroyed at zero.
[[gnu::always_inline]] inline void release(JSValue v) noexcept
{
    if (v.isCell())
        detail::decRef(v.asCell());
}

/// Owns one count of what it holds.
class Handle
{
public:
    Handle() noexcept : v_(JSValue::undefined()) {}
    /// Takes the count `v` carries.
    static Handle adopt(JSValue v) noexcept
    {
        Handle h;
        h.v_ = v;
        return h;
    }
    /// Takes a count of its own.
    static Handle retained(JSValue v) noexcept
    {
        retain(v);
        return adopt(v);
    }
    Handle(const Handle& o) noexcept : v_(o.v_) { retain(v_); }
    Handle(Handle&& o) noexcept : v_(o.v_) { o.v_ = JSValue::undefined(); }
    Handle& operator=(const Handle& o) noexcept
    {
        Handle tmp(o);
        swap(tmp);
        return *this;
    }
    Handle& operator=(Handle&& o) noexcept
    {
        swap(o);
        return *this;
    }
    ~Handle() { release(v_); }

    JSValue get() const noexcept { return v_; }
    /// The value, and the count with it: the handle is empty afterwards.
    JSValue take() noexcept
    {
        const JSValue v = v_;
        v_ = JSValue::undefined();
        return v;
    }
    void swap(Handle& o) noexcept
    {
        const JSValue t = v_;
        v_ = o.v_;
        o.v_ = t;
    }

private:
    JSValue v_;
};

}  // namespace vm
}  // namespace internal
}  // namespace script
