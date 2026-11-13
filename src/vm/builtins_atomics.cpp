// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Atomics. One thread runs a context, so each operation is a read and a write; wait finds the value it waits for either not
// there or never to be changed, and notify wakes nobody.

#include <cmath>
#include <cstring>

#include "builtins_util.hpp"
#include "script/internal/vm/convert.hpp"
#include "script/internal/vm/operators.hpp"
#include "script/internal/vm/typed.hpp"

namespace script {
namespace internal {
namespace vm {
namespace bi {

namespace {

BufferState* bufferStateOf(const JSTypedArray* t)
{
    if (isObject(t->buffer))
        if (Rare* r = asObject(t->buffer)->rare())
            return static_cast<BufferState*>(r->ext);
    return nullptr;
}

/// ValidateIntegerTypedArray, then ToIndex of the index: the array and the element, or null with an exception pending.
JSTypedArray* validate(VM& vm, JSValue ta, JSValue indexValue, bool waitable, std::uint32_t* index)
{
    if (!isObject(ta) || asObject(ta)->cls() != ObjClass::TypedArray) {
        typeError(vm, "Argument is not a typed array");
        return nullptr;
    }
    auto* t = static_cast<JSTypedArray*>(asObject(ta));
    const BufferState* b = bufferStateOf(t);
    if (waitable ? !(t->kind == ElemKind::Int32 || t->kind == ElemKind::BigInt64)
                 : (t->kind == ElemKind::Float32 || t->kind == ElemKind::Float64 || t->kind == ElemKind::Float16 ||
                    t->kind == ElemKind::Uint8Clamped || t->kind >= ElemKind::Count)) {
        typeError(vm, "Argument is not an integer typed array");
        return nullptr;
    }
    if (b && b->detached) {
        typeError(vm, "Cannot perform Atomics operation on a detached ArrayBuffer");
        return nullptr;
    }
    double d = 0;
    if (!indexValue.isUndefined()) {
        if (!toNumber(vm, indexValue, &d))
            return nullptr;
        d = d != d ? 0 : std::trunc(d);
        if (d < 0) {
            vm.throwError(ErrorCode::RangeError, "Invalid atomic access index");
            return nullptr;
        }
    }
    if (d >= double(t->length)) {
        vm.throwError(ErrorCode::RangeError, "Invalid atomic access index");
        return nullptr;
    }
    *index = std::uint32_t(d);
    return t;
}

/// The value as the raw bits an element of `t` holds: ToBigInt for the 64-bit kinds, ToIntegerOrInfinity otherwise. `*out`
/// is the value an operation answers with where that is the converted value (store).
bool toBits(VM& vm, const JSTypedArray* t, JSValue v, std::uint64_t* bits, Handle* converted)
{
    if (isBigKind(t->kind)) {
        JSValue prim = toPrimitive(vm, v, false);
        if (prim.isException())
            return false;
        JSValue big;
        if (isBigInt(prim)) {
            big = prim;
        } else if (prim.isBool()) {
            big = bigintFromInt64(vm, prim == JSValue::boolean(true) ? 1 : 0, false);
            release(prim);
        } else if (isString(prim)) {
            bool ok;
            big = bigintFromText(vm, asString(prim)->text(), &ok);
            release(prim);
            if (!ok) {
                if (!big.isException())
                    vm.throwError(ErrorCode::SyntaxError, "Cannot convert a string to a BigInt");
                return false;
            }
        } else {
            release(prim);
            vm.throwError(ErrorCode::TypeError, "Cannot convert " + describe(vm, v) + " to a BigInt");
            return false;
        }
        *bits = bigintLow64(big);
        *converted = Handle::adopt(big);
        return true;
    }
    double d;
    if (!toNumber(vm, v, &d))
        return false;
    d = (d != d || std::isinf(d)) ? 0 : std::trunc(d);
    if (d == 0)
        d = 0;  // not -0
    double m = std::fmod(d, 18446744073709551616.0);
    if (m < 0)
        m += 18446744073709551616.0;
    *bits = std::uint64_t(m);
    *converted = Handle::adopt(JSValue::boxNumber(d));
    return true;
}

std::uint64_t loadBits(const JSTypedArray* t, std::uint32_t i)
{
    const std::size_t size = kElemSize[std::size_t(t->kind)];
    std::uint64_t v = 0;
    std::memcpy(&v, t->data + std::size_t(i) * size, size);
    return v;
}

void storeBits(JSTypedArray* t, std::uint32_t i, std::uint64_t v)
{
    const std::size_t size = kElemSize[std::size_t(t->kind)];
    std::memcpy(t->data + std::size_t(i) * size, &v, size);
}

std::uint64_t maskOf(const JSTypedArray* t)
{
    const std::size_t size = kElemSize[std::size_t(t->kind)];
    return size == 8 ? ~std::uint64_t(0) : ((std::uint64_t(1) << (size * 8)) - 1);
}

NATIVE(atomicsRead)
{
    UNUSED();
    std::uint32_t index;
    JSTypedArray* t = validate(vm, arg(args, argc, 0), arg(args, argc, 1), false, &index);
    if (!t)
        return JSValue::exception();
    return typedGet(vm, t, index);
}

NATIVE(atomicsStore)
{
    UNUSED();
    std::uint32_t index;
    JSTypedArray* t = validate(vm, arg(args, argc, 0), arg(args, argc, 1), false, &index);
    if (!t)
        return JSValue::exception();
    std::uint64_t bits;
    Handle converted;
    if (!toBits(vm, t, arg(args, argc, 2), &bits, &converted))
        return JSValue::exception();
    if (index < t->length)
        storeBits(t, index, bits);
    retain(converted.get());
    return converted.get();
}

/// add (0), sub (1), and (2), or (3), xor (4), exchange (5).
NATIVE(atomicsReadModifyWrite)
{
    UNUSED();
    const int op = int(reinterpret_cast<std::intptr_t>(data));
    std::uint32_t index;
    JSTypedArray* t = validate(vm, arg(args, argc, 0), arg(args, argc, 1), false, &index);
    if (!t)
        return JSValue::exception();
    std::uint64_t operand;
    Handle converted;
    if (!toBits(vm, t, arg(args, argc, 2), &operand, &converted))
        return JSValue::exception();
    if (index >= t->length)
        return typeError(vm, "Cannot perform Atomics operation on a detached or shrunk ArrayBuffer");
    JSValue old = typedGet(vm, t, index);
    if (old.isException())
        return old;
    const std::uint64_t was = loadBits(t, index);
    std::uint64_t now;
    switch (op) {
        case 0: now = was + operand; break;
        case 1: now = was - operand; break;
        case 2: now = was & operand; break;
        case 3: now = was | operand; break;
        case 4: now = was ^ operand; break;
        default: now = operand; break;
    }
    storeBits(t, index, now);
    return old;
}

NATIVE(atomicsCompareExchange)
{
    UNUSED();
    std::uint32_t index;
    JSTypedArray* t = validate(vm, arg(args, argc, 0), arg(args, argc, 1), false, &index);
    if (!t)
        return JSValue::exception();
    std::uint64_t expected, replacement;
    Handle c1, c2;
    if (!toBits(vm, t, arg(args, argc, 2), &expected, &c1) || !toBits(vm, t, arg(args, argc, 3), &replacement, &c2))
        return JSValue::exception();
    if (index >= t->length)
        return typeError(vm, "Cannot perform Atomics operation on a detached or shrunk ArrayBuffer");
    JSValue old = typedGet(vm, t, index);
    if (old.isException())
        return old;
    if ((loadBits(t, index) & maskOf(t)) == (expected & maskOf(t)))
        storeBits(t, index, replacement);
    return old;
}

NATIVE(atomicsIsLockFree)
{
    UNUSED();
    double n;
    if (!toNumber(vm, arg(args, argc, 0), &n))
        return JSValue::exception();
    n = n != n ? 0 : std::trunc(n);
    return JSValue::boolean(n == 1 || n == 2 || n == 4 || n == 8);
}

NATIVE(atomicsWait)
{
    UNUSED();
    std::uint32_t index;
    JSTypedArray* t = validate(vm, arg(args, argc, 0), arg(args, argc, 1), true, &index);
    if (!t)
        return JSValue::exception();
    const BufferState* b = bufferStateOf(t);
    if (!b || !b->shared)
        return typeError(vm, "Atomics.wait cannot be called on a non-shared buffer");
    std::uint64_t expected;
    Handle converted;
    if (!toBits(vm, t, arg(args, argc, 2), &expected, &converted))
        return JSValue::exception();
    double timeout = std::numeric_limits<double>::infinity();
    if (!arg(args, argc, 3).isUndefined()) {
        if (!toNumber(vm, args[3], &timeout))
            return JSValue::exception();
        if (timeout != timeout)
            timeout = std::numeric_limits<double>::infinity();
    }
    if ((loadBits(t, index) & maskOf(t)) != (expected & maskOf(t)))
        return makeString(vm, std::string_view("not-equal"));
    // Nothing could wake it, so all it can do is run out of time.
    return makeString(vm, std::string_view("timed-out"));
}

NATIVE(atomicsNotify)
{
    UNUSED();
    std::uint32_t index;
    JSTypedArray* t = validate(vm, arg(args, argc, 0), arg(args, argc, 1), true, &index);
    if (!t)
        return JSValue::exception();
    if (!arg(args, argc, 2).isUndefined()) {
        double count;
        if (!toNumber(vm, args[2], &count))
            return JSValue::exception();
    }
    return JSValue::int32(0);
}

NATIVE(atomicsPause)
{
    UNUSED();
    const JSValue n = arg(args, argc, 0);
    if (!n.isUndefined() && !(n.isNumber() && n.asNumber() == std::trunc(n.asNumber()) && std::isfinite(n.asNumber())))
        return typeError(vm, "Atomics.pause takes an integer");
    return JSValue::undefined();
}

}  // namespace

void installAtomics(VM& vm)
{
    JSObject* atomics = vm.newObject(vm.protos.object);
    Handle keep = Handle::adopt(toJSValue(atomics));
    method(vm, atomics, "load", atomicsRead, 2);
    method(vm, atomics, "store", atomicsStore, 3);
    method(vm, atomics, "add", atomicsReadModifyWrite, 3, reinterpret_cast<void*>(0));
    method(vm, atomics, "sub", atomicsReadModifyWrite, 3, reinterpret_cast<void*>(1));
    method(vm, atomics, "and", atomicsReadModifyWrite, 3, reinterpret_cast<void*>(2));
    method(vm, atomics, "or", atomicsReadModifyWrite, 3, reinterpret_cast<void*>(3));
    method(vm, atomics, "xor", atomicsReadModifyWrite, 3, reinterpret_cast<void*>(4));
    method(vm, atomics, "exchange", atomicsReadModifyWrite, 3, reinterpret_cast<void*>(5));
    method(vm, atomics, "compareExchange", atomicsCompareExchange, 4);
    method(vm, atomics, "isLockFree", atomicsIsLockFree, 1);
    method(vm, atomics, "wait", atomicsWait, 4);
    method(vm, atomics, "notify", atomicsNotify, 3);
    method(vm, atomics, "pause", atomicsPause, 0);
    atomics->putOwn(vm, vm.atoms.symToStringTag, makeString(vm, std::string_view("Atomics")), attr::kConfigurable);
    retain(keep.get());
    defineGlobal(vm, "Atomics", keep.get());
}

}  // namespace bi
}  // namespace vm
}  // namespace internal
}  // namespace script
