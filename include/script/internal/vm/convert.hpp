// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Between the API's `Value` and the new core's `JSValue`, and the string glue the core shares with the other engines
// (strings, symbols and bigints stay the old family's cells).
#pragma once

#include "script/internal/vm/object.hpp"

namespace script {
namespace internal {
namespace vm {

/// An owned `JSValue` for a `Value`. `Int`, `Char` and `Float` become numbers; `Long` the nearest double (exact up to
/// 2^53); a cell of the old family is shared as it is. A typed-engine cell is kept as a cell and keeps its kind.
JSValue fromValue(VM& vm, const Value& v);
/// A counted `Value` for a `JSValue` (which keeps its own count): numbers are `Int` when they are int32 and `Double`
/// otherwise; a cell takes its tag from its kind.
Value toValue(VM& vm, JSValue v);
/// The API type a cell is seen as.
Type typeOfCell(const detail::GCObject* cell) noexcept;

inline bool isString(JSValue v) noexcept
{
    return v.isCell() && !isVmCell(v.asCell()) && v.asCell()->kind == detail::CellKind::String;
}
inline StringCell* asString(JSValue v) noexcept
{
    return static_cast<StringCell*>(v.asCell());
}
inline bool isSymbol(JSValue v) noexcept
{
    return v.isCell() && !isVmCell(v.asCell()) && v.asCell()->kind == detail::CellKind::Symbol;
}
inline bool isBigInt(JSValue v) noexcept
{
    return v.isCell() && !isVmCell(v.asCell()) && v.asCell()->kind == detail::CellKind::BigInt;
}

/// The property key a value names: an interned string or a symbol. Int32 values go through a table of the first
/// thousand index names. The result is owned. Null, with `vm.exception` set, for what cannot be a key (an object: the
/// caller converts it to a primitive first).
Key toPropertyKey(VM& vm, JSValue v);
/// `a + b` on strings, owned; a rope when long.
JSValue concatStrings(VM& vm, JSValue a, JSValue b);
/// `((v[0] + v[1]) + v[2]) ...` for `n` values, which are strings (`Op::StrCat`): one string of the whole length when
/// that is short, the sums one after another when a piece is long (a long sum is a rope, not a copy). Owned, or the
/// exception sentinel.
JSValue concatAll(VM& vm, const JSValue* v, std::uint32_t n);
/// Code-unit order, as `<` on strings.
int compareStrings(JSValue a, JSValue b) noexcept;
bool stringsEqual(JSValue a, JSValue b) noexcept;

}  // namespace vm
}  // namespace internal
}  // namespace script
