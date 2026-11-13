// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "script/internal/vm/typed.hpp"
#include "script/internal/vm/convert.hpp"

namespace script {
namespace internal {
namespace vm {

JSValue typedGet(VM& vm, const JSTypedArray* t, std::uint32_t i)
{
    const std::uint8_t* p = t->data + std::size_t(i) * kElemSize[std::size_t(t->kind)];
    if (!isBigKind(t->kind))
        return loadNumber(t->kind, p);
    std::int64_t v;
    std::memcpy(&v, p, 8);
    return bigintFromInt64(vm, v, t->kind == ElemKind::BigUint64);
}

bool typedConvert(VM& vm, ElemKind kind, JSValue v, JSValue* out)
{
    if (isBigKind(kind)) {
        // ToBigInt: a BigInt, or a boolean or string that makes one.
        JSValue prim = toPrimitive(vm, v, false);
        if (prim.isException())
            return false;
        if (isBigInt(prim)) {
            *out = prim;
            return true;
        }
        if (prim.isBool()) {
            *out = bigintFromInt64(vm, prim == JSValue::boolean(true) ? 1 : 0, false);
            release(prim);
            return true;
        }
        if (isString(prim)) {
            bool ok;
            JSValue big = bigintFromText(vm, asString(prim)->text(), &ok);
            release(prim);
            if (!ok) {
                if (big.isException())
                    return false;
                vm.throwError(ErrorCode::SyntaxError, "Cannot convert a string to a BigInt");
                return false;
            }
            *out = big;
            return true;
        }
        release(prim);
        vm.throwError(ErrorCode::TypeError, "Cannot convert " + describe(vm, v) + " to a BigInt");
        return false;
    }
    double d;
    if (!toNumber(vm, v, &d))
        return false;
    *out = JSValue::boxNumber(d);
    return true;
}

bool typedSet(VM& vm, JSTypedArray* t, double index, JSValue v)
{
    JSValue prim;
    if (!typedConvert(vm, t->kind, v, &prim))
        return false;
    if (validTypedIndex(t, index)) {
        if (isBigKind(t->kind)) {
            const std::uint64_t bits = bigintLow64(prim);
            std::memcpy(t->data + std::size_t(index) * 8, &bits, 8);
        } else {
            storeNumber(t->kind, t->data + std::size_t(index) * kElemSize[std::size_t(t->kind)], prim.asNumber());
        }
    }
    release(prim);
    return true;
}

}  // namespace vm
}  // namespace internal
}  // namespace script
