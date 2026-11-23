// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "script/internal/vm/convert.hpp"

#include "script/internal/heap.hpp"

#include <cstring>

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
        s = text->interned ? text : vm.ctx.atomize(text);
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
    StringCell* x = asString(a);
    StringCell* y = asString(b);
    const std::size_t total = std::size_t(x->length) + y->length;
    if (total <= ContextImpl::kInternedLength && x->length && y->length)
        return fromValue(vm, vm.ctx.fresh(x->text(), y->text()));  // not interned: what is made is rarely asked for again
    return fromValue(vm, vm.ctx.concat(x, y));
}

int compareStrings(JSValue a, JSValue b) noexcept
{
    const StringView x = asString(a)->text();
    const StringView y = asString(b)->text();
    // One pass to the first unit that differs, four at a time while they agree.
    const std::size_t n = x.size() < y.size() ? x.size() : y.size();
    std::size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        std::uint64_t p, q;
        std::memcpy(&p, x.data() + i, 8);
        std::memcpy(&q, y.data() + i, 8);
        if (p != q)
            break;
    }
    for (; i < n; ++i)
        if (x[i] != y[i])
            return x[i] < y[i] ? -1 : 1;
    return x.size() < y.size() ? -1 : x.size() > y.size() ? 1 : 0;
}

bool stringsEqual(JSValue a, JSValue b) noexcept
{
    if (a == b)
        return true;
    const StringCell* x = asString(a);
    const StringCell* y = asString(b);
    if (x->length != y->length)
        return false;
    if (x->interned && y->interned)
        return false;  // two atoms of one text are one cell
    if (x->hash_ && y->hash_ && x->hash_ != y->hash_)
        return false;
    return x->text() == y->text();
}

}  // namespace vm
}  // namespace internal
}  // namespace script
