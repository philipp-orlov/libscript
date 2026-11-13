// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Arithmetic on integers of any size, in sign and magnitude.
#pragma once

#include "script/internal/dyn/runtime.hpp"
#include "script/internal/context.hpp"

namespace script {
namespace internal {
namespace dyn {
namespace bigint {

/// What the arithmetic needs of its host: where cells come from, and how a failure is raised (it does not return).
struct Env
{
    ContextImpl& ctx;
    void (*failure)(void* owner, ErrorCode code, const char* message);
    void* owner;
    void (*loopCheck)(void* owner, std::size_t tempBytes) = nullptr;  ///< every 1024th pass of a long loop
    void checkLoop(std::size_t iteration, std::size_t tempBytes)
    {
        if ((iteration & 1023) == 0 && loopCheck)
            loopCheck(owner, tempBytes);
    }
    [[noreturn]] void fail(ErrorCode code, const char* message) { failure(owner, code, message); __builtin_unreachable(); }
};

enum class BinaryOp : std::uint8_t
{
    Add,
    Sub,
    Mul,
    Div,
    Mod,
    Pow,
    And,
    Or,
    Xor,
    Shl,
    Shr,
};

inline BigIntCell* cellOf(const Value& v) noexcept { return static_cast<BigIntCell*>(detail::Access::ptr(v)); }

Value fromInt64(Env& rt, std::int64_t value);
Value fromUint64(Env& rt, std::uint64_t value);
/// Of a number with no fraction, which the caller has checked.
Value fromDouble(Env& rt, double value);
/// Digits in the radix, with nothing else in them. False, leaving `out` alone, when they are not.
bool parseDigits(Env& rt, StringView digits, int radix, bool negative, Value& out);
/// StringToBigInt: a string as `BigInt(s)` and comparisons read it. False when it is no integer.
bool parseString(Env& rt, StringView text, Value& out);
String toString(const BigIntCell* a, int radix = 10);
/// The digits of a literal in decimal, as a property name spelled that way is.
String literalToDecimal(StringView text);
double toDouble(const BigIntCell* a) noexcept;
/// The low 64 bits, in two's complement.
std::uint64_t toUint64(const BigIntCell* a) noexcept;

Value binary(Env& rt, BinaryOp op, const BigIntCell* a, const BigIntCell* b);
Value negate(Env& rt, const BigIntCell* a);
Value bitNot(Env& rt, const BigIntCell* a);
Value increment(Env& rt, const BigIntCell* a, int by);
Value asIntN(Env& rt, std::uint64_t bits, const BigIntCell* a, bool isSigned);

/// -1, 0 or 1.
int compare(const BigIntCell* a, const BigIntCell* b) noexcept;
/// -1, 0 or 1, or 2 when `d` is NaN.
int compareDouble(const BigIntCell* a, double d) noexcept;
bool equal(const BigIntCell* a, const BigIntCell* b) noexcept;

/// The dynamic engine's way in: failures are its errors.
Env envFor(Runtime& rt);
inline Value fromInt64(Runtime& rt, std::int64_t v) { Env e = envFor(rt); return fromInt64(e, v); }
inline Value fromUint64(Runtime& rt, std::uint64_t v) { Env e = envFor(rt); return fromUint64(e, v); }
inline Value fromDouble(Runtime& rt, double v) { Env e = envFor(rt); return fromDouble(e, v); }
inline bool parseDigits(Runtime& rt, StringView d, int radix, bool neg, Value& out) { Env e = envFor(rt); return parseDigits(e, d, radix, neg, out); }
inline bool parseString(Runtime& rt, StringView t, Value& out) { Env e = envFor(rt); return parseString(e, t, out); }
inline Value binary(Runtime& rt, BinaryOp op, const BigIntCell* a, const BigIntCell* b) { Env e = envFor(rt); return binary(e, op, a, b); }
inline Value negate(Runtime& rt, const BigIntCell* a) { Env e = envFor(rt); return negate(e, a); }
inline Value bitNot(Runtime& rt, const BigIntCell* a) { Env e = envFor(rt); return bitNot(e, a); }
inline Value increment(Runtime& rt, const BigIntCell* a, int by) { Env e = envFor(rt); return increment(e, a, by); }
inline Value asIntN(Runtime& rt, std::uint64_t bits, const BigIntCell* a, bool isSigned) { Env e = envFor(rt); return asIntN(e, bits, a, isSigned); }

}  // namespace bigint
}  // namespace dyn
}  // namespace internal
}  // namespace script
