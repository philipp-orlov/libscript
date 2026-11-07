// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Arithmetic on integers of any size, in sign and magnitude.
#pragma once

#include "script/internal/dyn/runtime.hpp"

namespace script {
namespace internal {
namespace dyn {
namespace bigint {

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

Value fromInt64(Runtime& rt, std::int64_t value);
Value fromUint64(Runtime& rt, std::uint64_t value);
/// Of a number with no fraction, which the caller has checked.
Value fromDouble(Runtime& rt, double value);
/// Digits in the radix, with nothing else in them. False, leaving `out` alone, when they are not.
bool parseDigits(Runtime& rt, StringView digits, int radix, bool negative, Value& out);
/// StringToBigInt: a string as `BigInt(s)` and comparisons read it. False when it is no integer.
bool parseString(Runtime& rt, StringView text, Value& out);
String toString(const BigIntCell* a, int radix = 10);
/// The digits of a literal in decimal, as a property name spelled that way is.
String literalToDecimal(StringView text);
double toDouble(const BigIntCell* a) noexcept;
/// The low 64 bits, in two's complement.
std::uint64_t toUint64(const BigIntCell* a) noexcept;

Value binary(Runtime& rt, BinaryOp op, const BigIntCell* a, const BigIntCell* b);
Value negate(Runtime& rt, const BigIntCell* a);
Value bitNot(Runtime& rt, const BigIntCell* a);
Value increment(Runtime& rt, const BigIntCell* a, int by);
Value asIntN(Runtime& rt, std::uint64_t bits, const BigIntCell* a, bool isSigned);

/// -1, 0 or 1.
int compare(const BigIntCell* a, const BigIntCell* b) noexcept;
/// -1, 0 or 1, or 2 when `d` is NaN.
int compareDouble(const BigIntCell* a, double d) noexcept;
bool equal(const BigIntCell* a, const BigIntCell* b) noexcept;

}  // namespace bigint
}  // namespace dyn
}  // namespace internal
}  // namespace script
