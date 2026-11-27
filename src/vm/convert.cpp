// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "script/internal/vm/operators.hpp"
#include "script/internal/vm/convert.hpp"

#include "script/internal/heap.hpp"

#include <algorithm>
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
        if (i >= 0 && i < std::int32_t(VM::kIndexKeys)) {
            Key* chunk = vm.smallIndexKeys;
            if (i >= std::int32_t(VM::kSmallIndexKeys)) {
                Key*& more = vm.indexKeyChunks[std::size_t(i) / VM::kSmallIndexKeys];
                if (!more) {
                    more = static_cast<Key*>(vm.allocPayload(VM::kSmallIndexKeys * sizeof(Key)));
                    std::fill(more, more + VM::kSmallIndexKeys, nullptr);
                }
                chunk = more;
            }
            Key& slot = chunk[std::size_t(i) % VM::kSmallIndexKeys];
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
        s = v.asBool() ? vm.atoms.true_ : vm.atoms.false_;
    } else if (v.isNull()) {
        s = vm.atoms.null_;
    } else if (v.isUndefined()) {
        s = vm.atoms.undefined_;
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
    if (total <= ContextImpl::kInternedLength && x->length && y->length) {
        if (x->interned && y->interned && !vm.tearingDown) {
            // Two atoms: most often a name made the same way again and again. The join is kept as an atom too.
            VM::JoinSeen& seen =
                vm.joinsSeen[((reinterpret_cast<std::uintptr_t>(x) >> 4) * 31 + (reinterpret_cast<std::uintptr_t>(y) >> 4)) %
                             VM::kJoinsSeen];
            if (seen.head == x && seen.tail == y && seen.joined) {
                const JSValue joined = JSValue::cell(seen.joined);
                retain(joined);
                return joined;
            }
            if (seen.head != x || seen.tail != y) {
                // Met once: noted, and made as any other join (which may yet be appended to in place).
                if (seen.joined) {
                    detail::decRef(seen.head);
                    detail::decRef(seen.tail);
                    detail::decRef(seen.joined);
                }
                seen = VM::JoinSeen{x, y, nullptr};
                return fromValue(vm, vm.ctx.fresh(x->text(), y->text()));
            }
            const JSValue joined = fromValue(vm, vm.ctx.concat(x->text(), y->text()));  // interned, being short
            seen.joined = asString(joined);
            detail::incRef(x);
            detail::incRef(y);
            detail::incRef(seen.joined);
            return joined;
        }
        return fromValue(vm, vm.ctx.fresh(x->text(), y->text()));  // not interned: what is made is rarely asked for again
    }
    return fromValue(vm, vm.ctx.concat(x, y));
}

JSValue concatAll(VM& vm, const JSValue* v, std::uint32_t n)
{
    // Up to this many units in one new string; more, and the pieces are joined in pairs as `+` joins them.
    static constexpr std::size_t kFlat = 256;
    static constexpr std::uint32_t kPieces = 16;
    StringView pieces[kPieces];
    std::size_t total = 0;
    bool flat = n >= 2 && n <= kPieces;
    for (std::uint32_t i = 0; i < n && flat; ++i) {
        flat = isString(v[i]) && (total += asString(v[i])->length) < kFlat;
    }
    if (flat && total > 0) {
        for (std::uint32_t i = 0; i < n; ++i)
            pieces[i] = asString(v[i])->text();  // (a rope is made flat here: it may ask for memory, before the cell below)
        StringCell* cell = vm.ctx.newStringRoom(pieces[0], pieces[1], total);
        Char* out = cell->mutableChars() + cell->length;
        for (std::uint32_t i = 2; i < n; ++i) {
            if (!pieces[i].empty())
                std::memcpy(out, pieces[i].data(), pieces[i].size() * sizeof(Char));
            out += pieces[i].size();
        }
        cell->length = std::uint32_t(total);
        return JSValue::cell(cell);
    }
    JSValue sum = n ? v[0] : makeString(vm, StringView());
    if (n)
        retain(sum);
    for (std::uint32_t i = 1; i < n; ++i) {
        const JSValue next = add(vm, sum, v[i]);
        release(sum);
        if (next.isException())
            return next;
        sum = next;
    }
    return sum;
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
