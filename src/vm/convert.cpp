// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "script/internal/vm/convert.hpp"

#include "script/internal/heap.hpp"

namespace script {
namespace internal {
namespace vm {

Type typeOfCell(const detail::GCObject* cell) noexcept
{
    if (isVmCell(cell))
        return Type::Object;
    switch (cell->kind) {
        case detail::CellKind::String: return Type::String;
        case detail::CellKind::Symbol: return Type::Symbol;
        case detail::CellKind::BigInt: return Type::BigInt;
        case detail::CellKind::Array: return Type::Array;
        case detail::CellKind::Delegate: return Type::Delegate;
        default: return Type::Object;
    }
}

JSValue fromValue(VM&, const Value& v)
{
    switch (v.type()) {
        case Type::Undefined: return JSValue::undefined();
        case Type::Null: return JSValue::null();
        case Type::Bool: return JSValue::boolean(detail::Access::boolean(v));
        case Type::Char:
        case Type::Int: return JSValue::int32(detail::Access::i4(v));
        case Type::Long: return JSValue::boxNumber(double(detail::Access::i8(v)));
        case Type::Float: return JSValue::boxNumber(double(detail::Access::r4(v)));
        case Type::Double: return JSValue::boxNumber(detail::Access::r8(v));
        default: {
            detail::GCObject* cell = detail::Access::ptr(v);
            if (!cell)
                return JSValue::null();
            const JSValue out = JSValue::cell(cell);
            retain(out);
            return out;
        }
    }
}

Value toValue(VM&, JSValue v)
{
    // A script's numbers are doubles to the host, as they are on the other core.
    if (v.isInt32())
        return Value(double(v.asInt32()));
    if (v.isDouble())
        return Value(v.asDouble());
    if (v.isCell())
        return detail::Access::make(typeOfCell(v.asCell()), v.asCell());
    if (v.isBool())
        return Value(v.asBool());
    if (v.isNull())
        return Value::null();
    return Value::undefined();
}

// ------------------------------------------------------------------- strings ---

Key toPropertyKey(VM& vm, JSValue v)
{
    if (v.isInt32()) {
        const std::int32_t i = v.asInt32();
        if (i >= 0 && i < std::int32_t(VM::kSmallIndexKeys)) {
            Key& slot = vm.smallIndexKeys[i];
            if (!slot) {
                slot = vm.ctx.intern(numberToString(double(i)));
                detail::incRef(slot);  // the table's own count: a sweep of unused names must leave it be
            }
            detail::incRef(slot);
            return slot;
        }
    }
    Key s = nullptr;
    if (isString(v)) {
        StringCell* text = asString(v);
        s = text->interned ? text : vm.ctx.intern(text);
    } else if (isSymbol(v)) {
        s = v.asCell();
    } else if (v.isNumber()) {
        s = vm.ctx.intern(numberToString(v.asNumber()));
    } else if (v.isBool()) {
        s = vm.ctx.intern(v.asBool() ? "true" : "false");
    } else if (v.isNull()) {
        s = vm.ctx.intern("null");
    } else if (v.isUndefined()) {
        s = vm.ctx.intern("undefined");
    } else {
        vm.throwError(ErrorCode::TypeError, "Cannot convert an object to a property key without converting it first");
        return nullptr;
    }
    detail::incRef(s);
    return s;
}

JSValue concatStrings(VM& vm, JSValue a, JSValue b)
{
    return fromValue(vm, vm.ctx.concat(asString(a), asString(b)));
}

int compareStrings(JSValue a, JSValue b) noexcept
{
    const StringView x = asString(a)->text();
    const StringView y = asString(b)->text();
    return x.compare(y) < 0 ? -1 : x == y ? 0 : 1;
}

bool stringsEqual(JSValue a, JSValue b) noexcept
{
    return a == b || asString(a)->text() == asString(b)->text();
}

}  // namespace vm
}  // namespace internal
}  // namespace script
