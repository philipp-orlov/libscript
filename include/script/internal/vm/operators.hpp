// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Conversions and operators of the language on `JSValue`, with the integer fast paths first. Results that are values
// are owned. A function that can run script (a conversion that calls `valueOf`) can fail: it says so by returning the
// exception sentinel or `false`, with `vm.exception` set.
#pragma once

#include "script/internal/vm/bytecode.hpp"
#include "script/internal/vm/object.hpp"

namespace script {
namespace internal {
namespace vm {

bool toBoolean(JSValue v) noexcept;
/// Whether `new v(...)` can work: a function made to be a constructor.
bool isConstructor(JSValue v) noexcept;
/// `ToPrimitive`; `preferString` is the hint. An owned value.
JSValue toPrimitive(VM& vm, JSValue v, bool preferString);
/// `hint` 0 default, 1 number, 2 string.
JSValue toPrimitiveHint(VM& vm, JSValue v, int hint);
/// `ToNumber` into `out`; false with an exception pending.
bool toNumber(VM& vm, JSValue v, double* out);
/// `ToNumeric`: a number, owned (no BigInt in this core yet).
JSValue toNumeric(VM& vm, JSValue v);
/// `ToString`, an owned string cell.
JSValue toStringValue(VM& vm, JSValue v);
/// `ToObject` for the values that have a wrapper this core can make; an owned value or the exception sentinel.
JSValue toObject(VM& vm, JSValue v);
std::int32_t toInt32(double d) noexcept;
inline std::uint32_t toUint32(double d) noexcept
{
    return std::uint32_t(toInt32(d));
}
/// A string made from text; owned.
JSValue makeString(VM& vm, StringView text);
JSValue makeString(VM& vm, std::string_view utf8);
/// `count` characters of the string `whole` from `start`: a long piece that is a good part of it points into its characters
/// (`ContextImpl::substring`), so cutting the front off a text over and over copies nothing.
JSValue makeSubstring(VM& vm, JSValue whole, std::size_t start, std::size_t count);
JSValue typeOf(VM& vm, JSValue v);

bool strictEquals(JSValue a, JSValue b) noexcept;
bool sameValueZero(JSValue a, JSValue b) noexcept;
/// `a == b`; `*ok` is false with an exception pending.
bool looseEquals(VM& vm, JSValue a, JSValue b, bool* ok);

/// `a + b`.
JSValue add(VM& vm, JSValue a, JSValue b);
/// `slot += y` where `slot` holds a string that nothing else refers to: the characters go on the end of the same cell
/// when it has room, and a longer cell with room to spare replaces it otherwise, so that a loop that appends takes linear
/// time. False when it does not apply and the general `add` must do it.
/// `x ** y` of two numbers.
double power(double x, double y) noexcept;
bool appendInPlace(VM& vm, JSValue& slot, JSValue y, bool owned);
/// The arithmetic and bitwise operators: `op` is one of Sub..UShr (and Exp).
JSValue arithmetic(VM& vm, Op op, JSValue a, JSValue b);
/// `a < b` and friends: `op` is Lt, Le, Gt or Ge. 0 or 1, or -1 with an exception pending.
int relational(VM& vm, Op op, JSValue a, JSValue b);
/// `a instanceof b`: 0 or 1, or -1 with an exception pending.
int instanceOf(VM& vm, JSValue a, JSValue b);
/// `a in b`.
int inOperator(VM& vm, JSValue a, JSValue b);
/// The name of a value for an error message: `undefined`, `null`, `5`, `[object Object]`.
std::string describe(VM& vm, JSValue v);
/// Whether iterating this array may skip the iterator protocol: it is a plain array and nothing changed how arrays iterate.
bool arrayIterationIntact(VM& vm, JSValue v);
/// Make `o` the wrapper object of the primitive `v` (which it counts), of class `cls`.
void wrapPrimitive(VM& vm, JSObject* o, ObjClass cls, JSValue v);
/// BigInt arithmetic; both operands are BigInts. Owned results, or the exception sentinel.
JSValue bigintBinary(VM& vm, Op op, JSValue a, JSValue b);
/// 0 negation, 1 `~`, 2 `++`, 3 `--`.
JSValue bigintUnary(VM& vm, int kind, JSValue a);
JSValue bigintFromText(VM& vm, StringView text, bool* ok);
JSValue bigintFromDouble(VM& vm, double d);
JSValue bigintToString(VM& vm, JSValue v, int radix);
JSValue bigintFromInt64(VM& vm, std::int64_t v, bool isUnsigned);
std::uint64_t bigintLow64(JSValue v);
JSValue bigintAsN(VM& vm, std::uint64_t bits, JSValue a, bool isSigned);
double bigintToDouble(JSValue v);
bool bigintIsZero(JSValue v) noexcept;
int bigintCompareWith(VM& vm, JSValue a, JSValue other, bool* ok);

}  // namespace vm
}  // namespace internal
}  // namespace script
