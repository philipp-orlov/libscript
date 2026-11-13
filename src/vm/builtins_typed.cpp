// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// ArrayBuffer, the typed arrays and DataView. The buffers do not resize or detach.
#include <algorithm>
#include <cmath>
#include <cstring>

#include "script/internal/vm/typed.hpp"
#include "builtins_util.hpp"

namespace script {
namespace internal {
namespace vm {
namespace bi {

namespace {

const char* const kNames[] = {"Int8Array",  "Uint8Array",   "Uint8ClampedArray", "Int16Array",    "Uint16Array",   "Int32Array",
                              "Uint32Array", "Float32Array", "Float64Array",      "BigInt64Array", "BigUint64Array"};

BufferState* bufferOf(JSValue v)
{
    if (isObject(v) && asObject(v)->cls() == ObjClass::ArrayBuffer)
        if (Rare* r = asObject(v)->rare())
            return static_cast<BufferState*>(r->ext);
    return nullptr;
}

JSTypedArray* asTyped(JSValue v)
{
    return isObject(v) && asObject(v)->cls() == ObjClass::TypedArray ? static_cast<JSTypedArray*>(asObject(v)) : nullptr;
}

JSTypedArray* thisTyped(VM& vm, JSValue self)
{
    if (JSTypedArray* t = asTyped(self))
        return t;
    typeError(vm, "this is not a typed array.");
    return nullptr;
}

/// ToIndex: false with a RangeError pending.
bool toIndex(VM& vm, JSValue v, const char* what, double* out)
{
    if (v.isUndefined()) {
        *out = 0;
        return true;
    }
    double d;
    if (!toNumber(vm, v, &d))
        return false;
    d = d != d ? 0 : std::trunc(d);
    if (d < 0 || d > 9007199254740991.0) {
        vm.throwError(ErrorCode::RangeError, std::string("Invalid ") + what);
        return false;
    }
    *out = d;
    return true;
}

double relative(double v, double length, double fallback, bool undefinedGivesFallback, bool isUndefined)
{
    if (isUndefined && undefinedGivesFallback)
        return fallback;
    v = v != v ? 0 : std::trunc(v);
    if (v < 0)
        return std::max(0.0, length + v);
    return std::min(v, length);
}

bool relativeArg(VM& vm, JSValue v, double length, double fallback, double* out)
{
    if (v.isUndefined()) {
        *out = fallback;
        return true;
    }
    double d;
    if (!toNumber(vm, v, &d))
        return false;
    *out = relative(d, length, fallback, false, false);
    return true;
}

/// A new typed array of `kind` with its own buffer.
JSTypedArray* makeTyped(VM& vm, ElemKind kind, JSObject* proto, std::uint32_t length)
{
    const std::size_t bytes = std::size_t(length) * kElemSize[std::size_t(kind)];
    JSObject* buffer = vm.newArrayBuffer(bytes);
    if (!buffer)
        return nullptr;
    Handle keep = Handle::adopt(toJSValue(buffer));
    return vm.newTypedArray(proto ? proto : vm.protos.typedArrays[std::size_t(kind)], kind, buffer, 0, length);
}

NATIVE(arrayBufferCtor)
{
    UNUSED();
    if (vm.newTarget.isUndefined() || !isObject(self))
        return typeError(vm, "Constructor ArrayBuffer requires 'new'");
    double length;
    if (!toIndex(vm, arg(args, argc, 0), "array buffer length", &length))
        return JSValue::exception();
    JSObject* o = vm.newArrayBuffer(std::size_t(length));
    if (!o)
        return JSValue::exception();
    o->setProto(vm, asObject(self)->proto(vm));
    return toJSValue(o);
}

NATIVE(arrayBufferGetter)
{
    UNUSED();
    BufferState* b = bufferOf(self);
    if (!b)
        return typeError(vm, "Method ArrayBuffer.prototype getter called on incompatible receiver");
    switch (reinterpret_cast<std::intptr_t>(data)) {
        case 0: return JSValue::boxNumber(double(b->size));
        case 1: return JSValue::boolean(false);
        default: return JSValue::boxNumber(double(b->size));
    }
}

NATIVE(arrayBufferSlice)
{
    UNUSED();
    BufferState* b = bufferOf(self);
    if (!b)
        return typeError(vm, "Method ArrayBuffer.prototype.slice called on incompatible receiver");
    double from, to;
    if (!relativeArg(vm, arg(args, argc, 0), double(b->size), 0, &from) ||
        !relativeArg(vm, arg(args, argc, 1), double(b->size), double(b->size), &to))
        return JSValue::exception();
    const std::size_t n = to > from ? std::size_t(to - from) : 0;
    JSObject* o = vm.newArrayBuffer(n);
    if (!o)
        return JSValue::exception();
    if (n)
        std::memcpy(static_cast<BufferState*>(o->rare()->ext)->data, b->data + std::size_t(from), n);
    return toJSValue(o);
}

NATIVE(arrayBufferIsView)
{
    UNUSED();
    const JSValue v = arg(args, argc, 0);
    return JSValue::boolean(asTyped(v) || (isObject(v) && asObject(v)->cls() == ObjClass::DataView));
}

// ------------------------------------------------------------- typed arrays ---

ElemKind kindOfConstructor(VM& vm, JSValue ctor)
{
    for (JSValue c = ctor; isFunction(c);) {
        auto* f = static_cast<JSFunction*>(asObject(c));
        if (f->flags & JSFunction::kNative)
            if (Rare* r = f->rare(); r && r->nativeData)
                return ElemKind(reinterpret_cast<std::intptr_t>(r->nativeData) - 1);
        JSObject* p = f->proto(vm);
        c = p ? toJSValue(p) : JSValue::undefined();
    }
    return ElemKind::Count;
}

NATIVE(typedCtor)
{
    UNUSED();
    const ElemKind kind = ElemKind(reinterpret_cast<std::intptr_t>(data) - 1);
    const std::uint32_t size = kElemSize[std::size_t(kind)];
    if (vm.newTarget.isUndefined() || !isObject(self))
        return typeError(vm, std::string("Constructor ") + kNames[std::size_t(kind)] + " requires 'new'");
    JSObject* proto = asObject(self)->proto(vm);
    const JSValue a0 = arg(args, argc, 0);
    if (!isObject(a0)) {
        double length;
        if (!toIndex(vm, a0, "typed array length", &length))
            return JSValue::exception();
        if (length * size > double(std::size_t(1) << 31))
            return vm.throwError(ErrorCode::RangeError, "Invalid typed array length: " + std::to_string(std::uint64_t(length)));
        JSTypedArray* t = makeTyped(vm, kind, proto, std::uint32_t(length));
        return t ? toJSValue(t) : JSValue::exception();
    }
    if (BufferState* b = bufferOf(a0)) {
        double offset, length = -1;
        if (!toIndex(vm, arg(args, argc, 1), "offset", &offset))
            return JSValue::exception();
        if (std::fmod(offset, size) != 0)
            return vm.throwError(ErrorCode::RangeError, std::string("start offset of ") + kNames[std::size_t(kind)] +
                                                              " should be a multiple of " + std::to_string(size));
        if (!arg(args, argc, 2).isUndefined() && !toIndex(vm, args[2], "typed array length", &length))
            return JSValue::exception();
        double count;
        if (length < 0) {
            if (std::fmod(double(b->size), size) != 0)
                return vm.throwError(ErrorCode::RangeError, std::string("byte length of ") + kNames[std::size_t(kind)] +
                                                                  " should be a multiple of " + std::to_string(size));
            if (offset > double(b->size))
                return vm.throwError(ErrorCode::RangeError, "Start offset " + std::to_string(std::uint64_t(offset)) + " is outside the bounds of the buffer");
            count = (double(b->size) - offset) / size;
        } else {
            if (offset + length * size > double(b->size))
                return vm.throwError(ErrorCode::RangeError, "Invalid typed array length: " + std::to_string(std::uint64_t(length)));
            count = length;
        }
        return toJSValue(vm.newTypedArray(proto, kind, asObject(a0), std::uint32_t(offset), std::uint32_t(count)));
    }
    std::vector<Handle> items;
    if (JSTypedArray* src = asTyped(a0)) {
        if (isBigKind(src->kind) != isBigKind(kind))
            return typeError(vm, "Cannot mix BigInt and other types, use explicit conversions");
        JSTypedArray* t = makeTyped(vm, kind, proto, src->length);
        if (!t)
            return JSValue::exception();
        Handle keep = Handle::adopt(toJSValue(t));
        for (std::uint32_t i = 0; i < src->length; ++i) {
            JSValue v = typedGet(vm, src, i);
            const bool ok = typedSet(vm, t, double(i), v);
            release(v);
            if (!ok)
                return JSValue::exception();
        }
        return keep.take();
    }
    JSValue method = getProperty(vm, a0, vm.atoms.symIterator);
    if (method.isException())
        return method;
    const bool iterable = isFunction(method);
    release(method);
    if (iterable) {
        if (!forEachIterated(vm, a0, [&](JSValue v) {
                items.push_back(Handle::retained(v));
                return Step::Continue;
            }))
            return JSValue::exception();
    } else {
        JSValue len = getProperty(vm, a0, vm.atoms.length);
        if (len.isException())
            return len;
        double n = 0;
        const bool ok = toNumber(vm, len, &n);
        release(len);
        if (!ok)
            return JSValue::exception();
        n = n != n ? 0 : std::max(0.0, std::trunc(n));
        if (n > double(std::size_t(1) << 31))
            return vm.throwError(ErrorCode::RangeError, "Invalid typed array length");
        for (std::uint32_t i = 0; i < std::uint32_t(n); ++i) {
            JSValue v = getIndex(vm, a0, i);
            if (v.isException())
                return v;
            items.push_back(Handle::adopt(v));
        }
    }
    JSTypedArray* t = makeTyped(vm, kind, proto, std::uint32_t(items.size()));
    if (!t)
        return JSValue::exception();
    Handle keep = Handle::adopt(toJSValue(t));
    for (std::size_t i = 0; i < items.size(); ++i)
        if (!typedSet(vm, t, double(i), items[i].get()))
            return JSValue::exception();
    return keep.take();
}

NATIVE(typedGetter)
{
    UNUSED();
    JSTypedArray* t = asTyped(self);
    const int which = int(reinterpret_cast<std::intptr_t>(data));  // 0 length, 1 byteLength, 2 byteOffset, 3 buffer, 4 tag
    if (which == 4) {
        if (!t)
            return JSValue::undefined();
        return makeString(vm, std::string_view(kNames[std::size_t(t->kind)]));
    }
    if (!t)
        return typeError(vm, "Receiver is not a typed array");
    switch (which) {
        case 0: return JSValue::boxNumber(double(t->length));
        case 1: return JSValue::boxNumber(double(t->length) * kElemSize[std::size_t(t->kind)]);
        case 2: return JSValue::boxNumber(double(t->byteOffset));
        default: retain(t->buffer); return t->buffer;
    }
}

/// every, some, forEach, find, findIndex, findLast, findLastIndex, filter, map: one loop.
NATIVE(typedIterate)
{
    UNUSED();
    JSTypedArray* t = thisTyped(vm, self);
    if (!t)
        return JSValue::exception();
    const int kind = int(reinterpret_cast<std::intptr_t>(data));
    // 0 forEach, 1 map, 2 filter, 3 some, 4 every, 5 find, 6 findIndex, 7 findLast, 8 findLastIndex
    const JSValue callback = arg(args, argc, 0), thisArg = arg(args, argc, 1);
    if (!isFunction(callback))
        return typeError(vm, describe(vm, callback) + " is not a function");
    Handle keep = Handle::retained(self);
    const std::uint32_t n = t->length;
    JSTypedArray* mapped = nullptr;
    Handle mappedKeep;
    std::vector<Handle> kept;
    if (kind == 1) {
        mapped = makeTyped(vm, t->kind, nullptr, n);
        if (!mapped)
            return JSValue::exception();
        mappedKeep = Handle::adopt(toJSValue(mapped));
    }
    const bool backwards = kind >= 7;
    for (std::uint32_t step = 0; step < n; ++step) {
        const std::uint32_t i = backwards ? n - 1 - step : step;
        if (i >= t->length)
            continue;
        Handle e = Handle::adopt(typedGet(vm, t, i));
        JSValue callArgs[3] = {e.get(), JSValue::int32(std::int32_t(i)), self};
        JSValue r = vm.call(callback, thisArg, callArgs, 3);
        if (r.isException())
            return r;
        Handle rk = Handle::adopt(r);
        switch (kind) {
            case 1:
                if (!typedSet(vm, mapped, double(i), r))
                    return JSValue::exception();
                break;
            case 2:
                if (toBoolean(r))
                    kept.push_back(e);
                break;
            case 3:
                if (toBoolean(r))
                    return JSValue::boolean(true);
                break;
            case 4:
                if (!toBoolean(r))
                    return JSValue::boolean(false);
                break;
            case 5:
            case 7:
                if (toBoolean(r))
                    return Handle(e).take();
                break;
            case 6:
            case 8:
                if (toBoolean(r))
                    return JSValue::int32(std::int32_t(i));
                break;
        }
    }
    switch (kind) {
        case 1: return mappedKeep.take();
        case 2: {
            JSTypedArray* out = makeTyped(vm, t->kind, nullptr, std::uint32_t(kept.size()));
            if (!out)
                return JSValue::exception();
            Handle outKeep = Handle::adopt(toJSValue(out));
            for (std::size_t i = 0; i < kept.size(); ++i)
                if (!typedSet(vm, out, double(i), kept[i].get()))
                    return JSValue::exception();
            return outKeep.take();
        }
        case 3: return JSValue::boolean(false);
        case 4: return JSValue::boolean(true);
        case 6:
        case 8: return JSValue::int32(-1);
        default: return JSValue::undefined();
    }
}

NATIVE(typedReduce)
{
    UNUSED();
    JSTypedArray* t = thisTyped(vm, self);
    if (!t)
        return JSValue::exception();
    const bool right = data != nullptr;
    const JSValue callback = arg(args, argc, 0);
    if (!isFunction(callback))
        return typeError(vm, describe(vm, callback) + " is not a function");
    Handle keep = Handle::retained(self);
    std::uint32_t n = t->length, step = 0;
    Handle acc;
    if (argc > 1) {
        acc = Handle::retained(args[1]);
    } else {
        if (n == 0)
            return typeError(vm, "Reduce of empty array with no initial value");
        acc = Handle::adopt(typedGet(vm, t, right ? n - 1 : 0));
        step = 1;
    }
    for (; step < n; ++step) {
        const std::uint32_t i = right ? n - 1 - step : step;
        if (i >= t->length)
            continue;
        Handle e = Handle::adopt(typedGet(vm, t, i));
        JSValue callArgs[4] = {acc.get(), e.get(), JSValue::int32(std::int32_t(i)), self};
        JSValue r = vm.call(callback, JSValue::undefined(), callArgs, 4);
        if (r.isException())
            return r;
        acc = Handle::adopt(r);
    }
    return acc.take();
}

NATIVE(typedFill)
{
    UNUSED();
    JSTypedArray* t = thisTyped(vm, self);
    if (!t)
        return JSValue::exception();
    double from, to;
    if (!relativeArg(vm, arg(args, argc, 1), t->length, 0, &from) || !relativeArg(vm, arg(args, argc, 2), t->length, t->length, &to))
        return JSValue::exception();
    const std::uint32_t size = kElemSize[std::size_t(t->kind)];
    if (to <= from) {
        // Still converts the value, which may throw.
        double d;
        if (!isBigKind(t->kind) && !toNumber(vm, arg(args, argc, 0), &d))
            return JSValue::exception();
        retain(self);
        return self;
    }
    if (!typedSet(vm, t, from, arg(args, argc, 0)))
        return JSValue::exception();
    const std::uint8_t* first = t->data + std::size_t(from) * size;
    for (std::size_t i = std::size_t(from) + 1; i < std::size_t(to); ++i)
        std::memcpy(t->data + i * size, first, size);
    retain(self);
    return self;
}

NATIVE(typedSet_)
{
    UNUSED();
    JSTypedArray* t = thisTyped(vm, self);
    if (!t)
        return JSValue::exception();
    double offset;
    if (!toIndex(vm, arg(args, argc, 1), "offset", &offset))
        return JSValue::exception();
    const JSValue src = arg(args, argc, 0);
    if (JSTypedArray* s = asTyped(src)) {
        if (offset + s->length > t->length)
            return vm.throwError(ErrorCode::RangeError, "offset is out of bounds");
        if (isBigKind(s->kind) != isBigKind(t->kind))
            return typeError(vm, "Cannot mix BigInt and other types, use explicit conversions");
        if (s->kind == t->kind) {
            std::memmove(t->data + std::size_t(offset) * kElemSize[std::size_t(t->kind)], s->data,
                         std::size_t(s->length) * kElemSize[std::size_t(s->kind)]);
        } else {
            std::vector<Handle> copy;
            for (std::uint32_t i = 0; i < s->length; ++i)
                copy.push_back(Handle::adopt(typedGet(vm, s, i)));
            for (std::uint32_t i = 0; i < s->length; ++i)
                if (!typedSet(vm, t, offset + i, copy[i].get()))
                    return JSValue::exception();
        }
        return JSValue::undefined();
    }
    if (src.isNullish())
        return typeError(vm, "Cannot convert undefined or null to object");
    std::vector<Handle> items;
    if (isString(src)) {
        for (std::uint32_t i = 0; i < asString(src)->length; ++i)
            items.push_back(Handle::adopt(makeString(vm, asString(src)->text().substr(i, 1))));
    } else if (isObject(src)) {
        JSValue len = getProperty(vm, src, vm.atoms.length);
        if (len.isException())
            return len;
        double n = 0;
        const bool ok = toNumber(vm, len, &n);
        release(len);
        if (!ok)
            return JSValue::exception();
        n = n != n ? 0 : std::max(0.0, std::trunc(n));
        if (offset + n > t->length)
            return vm.throwError(ErrorCode::RangeError, "offset is out of bounds");
        for (std::uint32_t i = 0; i < std::uint32_t(n); ++i) {
            JSValue v = getIndex(vm, src, i);
            if (v.isException())
                return v;
            items.push_back(Handle::adopt(v));
        }
    }
    if (offset + items.size() > t->length)
        return vm.throwError(ErrorCode::RangeError, "offset is out of bounds");
    for (std::size_t i = 0; i < items.size(); ++i)
        if (!typedSet(vm, t, offset + double(i), items[i].get()))
            return JSValue::exception();
    return JSValue::undefined();
}

NATIVE(typedSlice)
{
    UNUSED();
    JSTypedArray* t = thisTyped(vm, self);
    if (!t)
        return JSValue::exception();
    const bool sub = data != nullptr;  // subarray: a view on the same buffer
    double from, to;
    if (!relativeArg(vm, arg(args, argc, 0), t->length, 0, &from) || !relativeArg(vm, arg(args, argc, 1), t->length, t->length, &to))
        return JSValue::exception();
    const std::uint32_t n = to > from ? std::uint32_t(to - from) : 0;
    const std::uint32_t size = kElemSize[std::size_t(t->kind)];
    Handle keep = Handle::retained(self);
    JSObject* proto = vm.protos.typedArrays[std::size_t(t->kind)];
    if (sub) {
        JSTypedArray* v = vm.newTypedArray(proto, t->kind, asObject(t->buffer), t->byteOffset + std::uint32_t(from) * size, n);
        return toJSValue(v);
    }
    JSTypedArray* out = makeTyped(vm, t->kind, nullptr, n);
    if (!out)
        return JSValue::exception();
    if (n)
        std::memcpy(out->data, t->data + std::size_t(from) * size, std::size_t(n) * size);
    return toJSValue(out);
}

NATIVE(typedSearch)
{
    UNUSED();
    JSTypedArray* t = thisTyped(vm, self);
    if (!t)
        return JSValue::exception();
    const int mode = int(reinterpret_cast<std::intptr_t>(data));  // 0 indexOf, 1 lastIndexOf, 2 includes
    const double n = t->length;
    const JSValue target = arg(args, argc, 0);
    double start;
    if (mode == 1) {
        bool ok = true;
        if (argc > 1) {
            double d;
            if (!toNumber(vm, args[1], &d))
                return JSValue::exception();
            d = d != d ? 0 : std::trunc(d);
            start = d < 0 ? n + d : std::min(d, n - 1);
        } else {
            start = n - 1;
        }
        (void)ok;
        for (double i = start; i >= 0; --i)
            if (i < t->length) {
                Handle e = Handle::adopt(typedGet(vm, t, std::uint32_t(i)));
                if (strictEquals(e.get(), target))
                    return JSValue::boxNumber(i);
            }
        return JSValue::int32(-1);
    }
    bool ok;
    start = 0;
    if (argc > 1) {
        double d;
        if (!toNumber(vm, args[1], &d))
            return JSValue::exception();
        d = d != d ? 0 : std::trunc(d);
        start = d < 0 ? std::max(0.0, n + d) : d;
    }
    (void)ok;
    for (double i = start; i < n && i < t->length; ++i) {
        Handle e = Handle::adopt(typedGet(vm, t, std::uint32_t(i)));
        if (mode == 2 ? sameValueZero(e.get(), target) : strictEquals(e.get(), target))
            return mode == 2 ? JSValue::boolean(true) : JSValue::boxNumber(i);
    }
    return mode == 2 ? JSValue::boolean(false) : JSValue::int32(-1);
}

NATIVE(typedJoin)
{
    UNUSED();
    JSTypedArray* t = thisTyped(vm, self);
    if (!t)
        return JSValue::exception();
    String sep = u",";
    if (!arg(args, argc, 0).isUndefined()) {
        JSValue s = toStringValue(vm, args[0]);
        if (s.isException())
            return s;
        sep = String(asString(s)->text());
        release(s);
    }
    String out;
    for (std::uint32_t i = 0; i < t->length; ++i) {
        if (i)
            out += sep;
        Handle e = Handle::adopt(typedGet(vm, t, i));
        JSValue s = toStringValue(vm, e.get());
        if (s.isException())
            return s;
        out += asString(s)->text();
        release(s);
    }
    return makeString(vm, StringView(out));
}

NATIVE(typedReverse)
{
    UNUSED();
    JSTypedArray* t = thisTyped(vm, self);
    if (!t)
        return JSValue::exception();
    const std::uint32_t size = kElemSize[std::size_t(t->kind)];
    std::uint8_t tmp[8];
    for (std::uint32_t i = 0, j = t->length; i + 1 < j; ++i, --j) {
        std::uint8_t* a = t->data + std::size_t(i) * size;
        std::uint8_t* b = t->data + std::size_t(j - 1) * size;
        std::memcpy(tmp, a, size);
        std::memcpy(a, b, size);
        std::memcpy(b, tmp, size);
    }
    retain(self);
    return self;
}

NATIVE(typedAt)
{
    UNUSED();
    JSTypedArray* t = thisTyped(vm, self);
    if (!t)
        return JSValue::exception();
    double i;
    if (!toNumber(vm, arg(args, argc, 0), &i))
        return JSValue::exception();
    i = i != i ? 0 : std::trunc(i);
    if (i < 0)
        i += t->length;
    if (i < 0 || i >= t->length)
        return JSValue::undefined();
    return typedGet(vm, t, std::uint32_t(i));
}

NATIVE(typedSort)
{
    UNUSED();
    JSTypedArray* t = thisTyped(vm, self);
    if (!t)
        return JSValue::exception();
    const JSValue compare = arg(args, argc, 0);
    if (!compare.isUndefined() && !isFunction(compare))
        return typeError(vm, "The comparison function must be either a function or undefined");
    Handle keep = Handle::retained(self);
    const std::uint32_t n = t->length;
    if (compare.isUndefined() && !isBigKind(t->kind)) {
        std::vector<double> values(n);
        for (std::uint32_t i = 0; i < n; ++i)
            values[i] = typedGet(vm, t, i).asNumber();
        std::stable_sort(values.begin(), values.end(), [](double a, double b) {
            if (a != a)
                return false;
            if (b != b)
                return true;
            if (a == 0 && b == 0)
                return std::signbit(a) && !std::signbit(b);
            return a < b;
        });
        for (std::uint32_t i = 0; i < n; ++i)
            storeNumber(t->kind, t->data + std::size_t(i) * kElemSize[std::size_t(t->kind)], values[i]);
        retain(self);
        return self;
    }
    std::vector<Handle> items;
    for (std::uint32_t i = 0; i < n; ++i)
        items.push_back(Handle::adopt(typedGet(vm, t, i)));
    bool failed = false;
    std::stable_sort(items.begin(), items.end(), [&](const Handle& x, const Handle& y) {
        if (failed)
            return false;
        if (compare.isUndefined()) {
            bool ok;
            const int c = isBigInt(x.get()) ? bigintCompareWith(vm, x.get(), y.get(), &ok) : 0;
            return c < 0;
        }
        JSValue pair[2] = {x.get(), y.get()};
        JSValue r = vm.call(compare, JSValue::undefined(), pair, 2);
        if (r.isException()) {
            failed = true;
            return false;
        }
        double d;
        const bool ok = toNumber(vm, r, &d);
        release(r);
        if (!ok) {
            failed = true;
            return false;
        }
        return d < 0;
    });
    if (failed)
        return JSValue::exception();
    for (std::uint32_t i = 0; i < n && i < t->length; ++i)
        if (!typedSet(vm, t, double(i), items[i].get()))
            return JSValue::exception();
    retain(self);
    return self;
}

NATIVE(typedFrom)
{
    UNUSED();
    const ElemKind kind = kindOfConstructor(vm, self);
    if (kind == ElemKind::Count)
        return typeError(vm, "TypedArray.from requires a typed array constructor");
    const JSValue mapFn = arg(args, argc, 1);
    if (!mapFn.isUndefined() && !isFunction(mapFn))
        return typeError(vm, describe(vm, mapFn) + " is not a function");
    std::vector<Handle> items;
    const JSValue src = arg(args, argc, 0);
    if (src.isNullish())
        return typeError(vm, describe(vm, src) + " is not iterable");
    JSValue method = getProperty(vm, src, vm.atoms.symIterator);
    if (method.isException())
        return method;
    const bool iterable = isFunction(method);
    release(method);
    if (iterable) {
        if (!forEachIterated(vm, src, [&](JSValue v) {
                items.push_back(Handle::retained(v));
                return Step::Continue;
            }))
            return JSValue::exception();
    } else if (isObject(src) || isString(src)) {
        double n = 0;
        if (isString(src)) {
            n = asString(src)->length;
        } else {
            JSValue len = getProperty(vm, src, vm.atoms.length);
            if (len.isException())
                return len;
            const bool ok = toNumber(vm, len, &n);
            release(len);
            if (!ok)
                return JSValue::exception();
        }
        n = n != n ? 0 : std::max(0.0, std::trunc(n));
        for (std::uint32_t i = 0; i < std::uint32_t(n); ++i) {
            JSValue v = getIndex(vm, src, i);
            if (v.isException())
                return v;
            items.push_back(Handle::adopt(v));
        }
    }
    JSTypedArray* t = makeTyped(vm, kind, nullptr, std::uint32_t(items.size()));
    if (!t)
        return JSValue::exception();
    Handle keep = Handle::adopt(toJSValue(t));
    for (std::size_t i = 0; i < items.size(); ++i) {
        JSValue v = items[i].get();
        Handle mapped;
        if (!mapFn.isUndefined()) {
            JSValue callArgs[2] = {v, JSValue::int32(std::int32_t(i))};
            JSValue r = vm.call(mapFn, arg(args, argc, 2), callArgs, 2);
            if (r.isException())
                return r;
            mapped = Handle::adopt(r);
            v = r;
        }
        if (!typedSet(vm, t, double(i), v))
            return JSValue::exception();
    }
    return keep.take();
}

NATIVE(typedOf)
{
    UNUSED();
    const ElemKind kind = kindOfConstructor(vm, self);
    if (kind == ElemKind::Count)
        return typeError(vm, "TypedArray.of requires a typed array constructor");
    JSTypedArray* t = makeTyped(vm, kind, nullptr, argc);
    if (!t)
        return JSValue::exception();
    Handle keep = Handle::adopt(toJSValue(t));
    for (std::uint32_t i = 0; i < argc; ++i)
        if (!typedSet(vm, t, double(i), args[i]))
            return JSValue::exception();
    return keep.take();
}

// ----------------------------------------------------------------- DataView ---

struct ViewState : NativeState
{
    JSValue buffer = JSValue::undefined();
    std::uint32_t offset = 0, length = 0;
    ~ViewState() override { release(buffer); }
    void clear(VM&) noexcept override
    {
        release(buffer);
        buffer = JSValue::undefined();
        length = 0;
    }
    void values(std::vector<JSValue>& out) const override { out.push_back(buffer); }
};

ViewState* viewOf(JSValue v)
{
    if (isObject(v) && asObject(v)->cls() == ObjClass::DataView)
        if (Rare* r = asObject(v)->rare())
            return static_cast<ViewState*>(r->ext);
    return nullptr;
}

NATIVE(dataViewCtor)
{
    UNUSED();
    if (vm.newTarget.isUndefined() || !isObject(self))
        return typeError(vm, "Constructor DataView requires 'new'");
    const JSValue buf = arg(args, argc, 0);
    BufferState* b = bufferOf(buf);
    if (!b)
        return typeError(vm, "First argument to DataView constructor must be an ArrayBuffer");
    double offset, length = -1;
    if (!toIndex(vm, arg(args, argc, 1), "DataView offset", &offset))
        return JSValue::exception();
    if (offset > double(b->size))
        return vm.throwError(ErrorCode::RangeError, "Start offset " + std::to_string(std::uint64_t(offset)) + " is outside the bounds of the buffer");
    if (!arg(args, argc, 2).isUndefined() && !toIndex(vm, args[2], "DataView length", &length))
        return JSValue::exception();
    if (length < 0)
        length = double(b->size) - offset;
    else if (offset + length > double(b->size))
        return vm.throwError(ErrorCode::RangeError, "Invalid DataView length " + std::to_string(std::uint64_t(length)));
    auto* state = new ViewState();
    retain(buf);
    state->buffer = buf;
    state->offset = std::uint32_t(offset);
    state->length = std::uint32_t(length);
    JSObject* o = asObject(self);
    o->h.cls = std::uint8_t(ObjClass::DataView);
    delete o->rareOf(vm).ext;
    o->rareOf(vm).ext = state;
    retain(self);
    return self;
}

NATIVE(dataViewGetter)
{
    UNUSED();
    ViewState* v = viewOf(self);
    if (!v)
        return typeError(vm, "Receiver is not a DataView");
    switch (reinterpret_cast<std::intptr_t>(data)) {
        case 0: retain(v->buffer); return v->buffer;
        case 1: return JSValue::boxNumber(double(v->length));
        default: return JSValue::boxNumber(double(v->offset));
    }
}

NATIVE(dataViewAccess)
{
    UNUSED();
    ViewState* v = viewOf(self);
    if (!v)
        return typeError(vm, "Receiver is not a DataView");
    const auto bits = reinterpret_cast<std::intptr_t>(data);
    const ElemKind kind = ElemKind(bits & 255);
    const bool isSet = (bits >> 8) & 1;
    const std::uint32_t size = kElemSize[std::size_t(kind)];
    double index;
    if (!toIndex(vm, arg(args, argc, 0), "DataView offset", &index))
        return JSValue::exception();
    JSValue value = JSValue::undefined();
    double number = 0;
    std::uint64_t big = 0;
    if (isSet) {
        if (isBigKind(kind)) {
            JSValue prim = toPrimitive(vm, arg(args, argc, 1), false);
            if (prim.isException())
                return prim;
            if (!isBigInt(prim)) {
                release(prim);
                return typeError(vm, "Cannot convert " + describe(vm, arg(args, argc, 1)) + " to a BigInt");
            }
            big = bigintLow64(prim);
            release(prim);
        } else if (!toNumber(vm, arg(args, argc, 1), &number)) {
            return JSValue::exception();
        }
    }
    const bool little = toBoolean(arg(args, argc, isSet ? 2 : 1));
    if (index + size > double(v->length))
        return vm.throwError(ErrorCode::RangeError, "Offset is outside the bounds of the DataView");
    BufferState* b = bufferOf(v->buffer);
    std::uint8_t* p = b->data + v->offset + std::size_t(index);
    std::uint8_t tmp[8];
    if (isSet) {
        if (isBigKind(kind))
            std::memcpy(tmp, &big, 8);
        else
            storeNumber(kind, tmp, number);
        if (!little)
            std::reverse(tmp, tmp + size);
        std::memcpy(p, tmp, size);
        return JSValue::undefined();
    }
    std::memcpy(tmp, p, size);
    if (!little)
        std::reverse(tmp, tmp + size);
    if (isBigKind(kind)) {
        std::int64_t raw;
        std::memcpy(&raw, tmp, 8);
        return bigintFromInt64(vm, raw, kind == ElemKind::BigUint64);
    }
    value = loadNumber(kind, tmp);
    return value;
}

}  // namespace

void installTypedArrays(VM& vm)
{
    JSObject* objectProto = vm.protos.object;

    // ArrayBuffer.
    JSObject* abProto = vm.newObject(objectProto);
    vm.setIntrinsic(&vm.protos.arrayBuffer, abProto);
    release(toJSValue(abProto));
    JSFunction* abCtor = constructor(vm, "ArrayBuffer", arrayBufferCtor, 1, abProto);
    method(vm, abCtor, "isView", arrayBufferIsView, 1);
    auto getter = [&](JSObject* target, const char* name, FastNative fn, void* data) {
        Key key = vm.internKey(std::string_view(name));
        Key fnName = vm.internKey(std::string_view(std::string("get ") + name));
        JSFunction* g = vm.newNativeFunction(vm.protos.function, fn, data, fnName, 0);
        Handle keep = Handle::adopt(toJSValue(g));
        defineAccessor(vm, target, key, toJSValue(g), JSValue::undefined(), attr::kConfigurable);
        detail::decRef(key);
        detail::decRef(fnName);
    };
    getter(abProto, "byteLength", arrayBufferGetter, reinterpret_cast<void*>(0));
    getter(abProto, "resizable", arrayBufferGetter, reinterpret_cast<void*>(1));
    getter(abProto, "maxByteLength", arrayBufferGetter, reinterpret_cast<void*>(2));
    method(vm, abProto, "slice", arrayBufferSlice, 2);
    abProto->putOwn(vm, vm.atoms.symToStringTag, makeString(vm, std::string_view("ArrayBuffer")), attr::kConfigurable);

    // %TypedArray%.
    JSObject* taProto = vm.newObject(objectProto);
    JSFunction* taCtor = constructor(vm, "TypedArray", [](VM& vm2, JSValue, const JSValue*, std::uint32_t, void*) -> JSValue {
        return typeError(vm2, "Abstract class TypedArray not directly constructable");
    }, 0, taProto);
    {
        // Not a global.
        Key key = vm.internKey(std::string_view("TypedArray"));
        vm.global->deleteOwn(vm, key);
        detail::decRef(key);
    }
    method(vm, taCtor, "from", typedFrom, 1);
    method(vm, taCtor, "of", typedOf, 0);
    getter(taProto, "length", typedGetter, reinterpret_cast<void*>(0));
    getter(taProto, "byteLength", typedGetter, reinterpret_cast<void*>(1));
    getter(taProto, "byteOffset", typedGetter, reinterpret_cast<void*>(2));
    getter(taProto, "buffer", typedGetter, reinterpret_cast<void*>(3));
    {
        Key fnName = vm.internKey(std::string_view("get [Symbol.toStringTag]"));
        JSFunction* g = vm.newNativeFunction(vm.protos.function, typedGetter, reinterpret_cast<void*>(4), fnName, 0);
        Handle keep = Handle::adopt(toJSValue(g));
        defineAccessor(vm, taProto, vm.atoms.symToStringTag, toJSValue(g), JSValue::undefined(), attr::kConfigurable);
        detail::decRef(fnName);
    }
    static const char* const iterNames[] = {"forEach", "map", "filter", "some", "every", "find", "findIndex", "findLast", "findLastIndex"};
    for (int i = 0; i < 9; ++i)
        method(vm, taProto, iterNames[i], typedIterate, 1, reinterpret_cast<void*>(std::intptr_t(i)));
    method(vm, taProto, "reduce", typedReduce, 1, nullptr);
    method(vm, taProto, "reduceRight", typedReduce, 1, reinterpret_cast<void*>(1));
    method(vm, taProto, "fill", typedFill, 1);
    method(vm, taProto, "set", typedSet_, 1);
    method(vm, taProto, "slice", typedSlice, 2, nullptr);
    method(vm, taProto, "subarray", typedSlice, 2, reinterpret_cast<void*>(1));
    method(vm, taProto, "indexOf", typedSearch, 1, reinterpret_cast<void*>(0));
    method(vm, taProto, "lastIndexOf", typedSearch, 1, reinterpret_cast<void*>(1));
    method(vm, taProto, "includes", typedSearch, 1, reinterpret_cast<void*>(2));
    method(vm, taProto, "join", typedJoin, 1);
    method(vm, taProto, "reverse", typedReverse, 0);
    method(vm, taProto, "sort", typedSort, 1);
    method(vm, taProto, "at", typedAt, 1);
    // The iterators and `toString` are the arrays'.
    for (const char* name : {"values", "keys", "entries", "toString"}) {
        Key k = vm.internKey(std::string_view(name));
        if (JSValue* f = vm.protos.array->findOwn(vm, k)) {
            retain(*f);
            taProto->putOwn(vm, k, *f, attr::kWritable | attr::kConfigurable);
        }
        detail::decRef(k);
    }
    {
        Key values = vm.internKey(std::string_view("values"));
        if (JSValue* f = taProto->findOwn(vm, values)) {
            retain(*f);
            taProto->putOwn(vm, vm.atoms.symIterator, *f, attr::kWritable | attr::kConfigurable);
        }
        detail::decRef(values);
    }
    for (int k = 0; k < int(ElemKind::Count); ++k) {
        JSObject* proto = vm.newObject(taProto);
        vm.protos.typedArrays[k] = proto;  // keeps the creation count
        retain(toJSValue(proto));
        JSFunction* ctor = constructor(vm, kNames[k], typedCtor, 3, proto);
        ctor->rareOf(vm).nativeData = reinterpret_cast<void*>(std::intptr_t(k + 1));
        ctor->setProto(vm, taCtor);
        value(vm, ctor, "BYTES_PER_ELEMENT", JSValue::int32(std::int32_t(kElemSize[k])), 0);
        value(vm, proto, "BYTES_PER_ELEMENT", JSValue::int32(std::int32_t(kElemSize[k])), 0);
        release(toJSValue(proto));
    }
    release(toJSValue(taProto));

    // DataView.
    JSObject* dvProto = vm.newObject(objectProto);
    JSFunction* dvCtor = constructor(vm, "DataView", dataViewCtor, 1, dvProto);
    (void)dvCtor;
    getter(dvProto, "buffer", dataViewGetter, reinterpret_cast<void*>(0));
    getter(dvProto, "byteLength", dataViewGetter, reinterpret_cast<void*>(1));
    getter(dvProto, "byteOffset", dataViewGetter, reinterpret_cast<void*>(2));
    static const char* const viewNames[] = {"Int8", "Uint8", nullptr, "Int16", "Uint16", "Int32", "Uint32", "Float32", "Float64", "BigInt64", "BigUint64"};
    for (int k = 0; k < int(ElemKind::Count); ++k) {
        if (!viewNames[k])
            continue;
        method(vm, dvProto, (std::string("get") + viewNames[k]).c_str(), dataViewAccess, 1, reinterpret_cast<void*>(std::intptr_t(k)));
        method(vm, dvProto, (std::string("set") + viewNames[k]).c_str(), dataViewAccess, 2, reinterpret_cast<void*>(std::intptr_t(k | 256)));
    }
    dvProto->putOwn(vm, vm.atoms.symToStringTag, makeString(vm, std::string_view("DataView")), attr::kConfigurable);
    release(toJSValue(dvProto));
}

}  // namespace bi
}  // namespace vm
}  // namespace internal
}  // namespace script
