// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// %TypedArray%.prototype and %TypedArray%.from and .of, as the language has them: a typed array over a resizable buffer may
// shrink or grow while a callback runs, so each element is read as it is then (undefined past the end).
#include <algorithm>

#include "script/internal/vm/typed.hpp"
#include "builtins_util.hpp"

namespace script {
namespace internal {
namespace vm {
namespace bi {

namespace {

/// ValidateTypedArray: a typed array that is not out of bounds.
JSTypedArray* validate(VM& vm, JSValue v)
{
    JSTypedArray* t = typedArrayOf(v);
    if (!t) {
        typeError(vm, "this is not a typed array");
        return nullptr;
    }
    if (typedArrayOutOfBounds(t)) {
        typeError(vm, "the typed array is out of bounds");
        return nullptr;
    }
    return t;
}

bool integerOf(VM& vm, JSValue v, double* out)
{
    double d = 0;
    if (!toNumber(vm, v, &d))
        return false;
    *out = d != d ? 0 : std::trunc(d);
    return true;
}

/// A relative index clamped into [0, len]; `undefined` gives `fallback`.
bool relativeTo(VM& vm, JSValue v, double len, double fallback, double* out)
{
    if (v.isUndefined()) {
        *out = fallback;
        return true;
    }
    double d = 0;
    if (!integerOf(vm, v, &d))
        return false;
    *out = d < 0 ? std::max(len + d, 0.0) : std::min(d, len);
    return true;
}

/// `O[i]` as it is now: undefined when the array no longer reaches there. Owned.
JSValue elementAt(VM& vm, JSTypedArray* t, double i)
{
    if (i >= 0 && i < double(t->length) && !typedArrayOutOfBounds(t))
        return typedGet(vm, t, std::uint32_t(i));
    return JSValue::undefined();
}

/// `O.constructor[Symbol.species]` of a typed array, or its own kind's constructor, called with `args`, and checked to be a
/// typed array of a suitable length and content type.
JSValue speciesCreate(VM& vm, JSValue exemplar, const JSValue* args, std::uint32_t argc)
{
    JSTypedArray* ex = typedArrayOf(exemplar);
    Handle ctor = Handle::adopt(speciesConstructor(vm, exemplar, vm.typedCtors[std::size_t(ex->kind)]));
    if (ctor.get().isException())
        return ctor.get();
    JSValue made = vm.construct(ctor.get(), args, argc);
    if (made.isException())
        return made;
    Handle keep = Handle::adopt(made);
    JSTypedArray* t = validate(vm, made);
    if (!t)
        return JSValue::exception();
    if (isBigKind(t->kind) != isBigKind(ex->kind))
        return typeError(vm, "Cannot mix BigInt and other types, use explicit conversions");
    if (argc == 1 && args[0].isNumber() && double(t->length) < args[0].asNumber())
        return typeError(vm, "the typed array made is too short");
    return keep.take();
}

/// What a callback-taking method does with the result of a call, kinds as in `typedIterate`.
NATIVE(typedIterate)
{
    UNUSED();
    JSTypedArray* t = validate(vm, self);
    if (!t)
        return JSValue::exception();
    const int kind = int(reinterpret_cast<std::intptr_t>(data));
    // 0 forEach, 1 map, 2 filter, 3 some, 4 every, 5 find, 6 findIndex, 7 findLast, 8 findLastIndex
    const double len = t->length;
    const JSValue callback = arg(args, argc, 0), thisArg = arg(args, argc, 1);
    if (!isFunction(callback))
        return typeError(vm, describe(vm, callback) + " is not a function");
    Handle keep = Handle::retained(self);
    Handle mapped;
    if (kind == 1) {
        const JSValue lengthArg = JSValue::boxNumber(len);
        mapped = Handle::adopt(speciesCreate(vm, self, &lengthArg, 1));
        if (mapped.get().isException())
            return mapped.get();
    }
    std::vector<Handle> kept;
    const bool backwards = kind >= 7;
    for (double step = 0; step < len; ++step) {
        const double i = backwards ? len - 1 - step : step;
        Handle e = Handle::adopt(elementAt(vm, t, i));
        JSValue callArgs[3] = {e.get(), JSValue::boxNumber(i), self};
        JSValue r = vm.call(callback, thisArg, callArgs, 3);
        if (r.isException())
            return r;
        Handle rk = Handle::adopt(r);
        switch (kind) {
            case 1:
                if (!typedSet(vm, static_cast<JSTypedArray*>(asObject(mapped.get())), i, r))
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
                    return JSValue::boxNumber(i);
                break;
            default: break;
        }
    }
    switch (kind) {
        case 1: return mapped.take();
        case 2: {
            const JSValue countArg = JSValue::boxNumber(double(kept.size()));
            Handle out = Handle::adopt(speciesCreate(vm, self, &countArg, 1));
            if (out.get().isException())
                return out.get();
            auto* o = static_cast<JSTypedArray*>(asObject(out.get()));
            for (std::size_t i = 0; i < kept.size(); ++i)
                if (!typedSet(vm, o, double(i), kept[i].get()))
                    return JSValue::exception();
            return out.take();
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
    JSTypedArray* t = validate(vm, self);
    if (!t)
        return JSValue::exception();
    const bool right = data != nullptr;
    const double len = t->length;
    const JSValue callback = arg(args, argc, 0);
    if (!isFunction(callback))
        return typeError(vm, describe(vm, callback) + " is not a function");
    Handle keep = Handle::retained(self);
    double step = 0;
    Handle acc;
    if (argc > 1) {
        acc = Handle::retained(args[1]);
    } else {
        if (len == 0)
            return typeError(vm, "Reduce of empty array with no initial value");
        acc = Handle::adopt(elementAt(vm, t, right ? len - 1 : 0));
        step = 1;
    }
    for (; step < len; ++step) {
        const double i = right ? len - 1 - step : step;
        Handle e = Handle::adopt(elementAt(vm, t, i));
        JSValue callArgs[4] = {acc.get(), e.get(), JSValue::boxNumber(i), self};
        JSValue r = vm.call(callback, JSValue::undefined(), callArgs, 4);
        if (r.isException())
            return r;
        acc = Handle::adopt(r);
    }
    return acc.take();
}

NATIVE(typedAt)
{
    UNUSED();
    JSTypedArray* t = validate(vm, self);
    if (!t)
        return JSValue::exception();
    const double len = t->length;
    double rel = 0;
    if (!integerOf(vm, arg(args, argc, 0), &rel))
        return JSValue::exception();
    const double k = rel >= 0 ? rel : len + rel;
    if (k < 0 || k >= len)
        return JSValue::undefined();
    return elementAt(vm, t, k);
}

NATIVE(typedFill)
{
    UNUSED();
    JSTypedArray* t = validate(vm, self);
    if (!t)
        return JSValue::exception();
    const double len = t->length;
    Handle value;
    {
        JSValue converted;
        if (!typedConvert(vm, t->kind, arg(args, argc, 0), &converted))
            return JSValue::exception();
        value = Handle::adopt(converted);
    }
    double from = 0, to = 0;
    if (!relativeTo(vm, arg(args, argc, 1), len, 0, &from) || !relativeTo(vm, arg(args, argc, 2), len, len, &to))
        return JSValue::exception();
    if (typedArrayOutOfBounds(t))
        return typeError(vm, "the typed array is out of bounds");
    to = std::min(to, double(t->length));
    for (double k = from; k < to; ++k)
        if (!typedSet(vm, t, k, value.get()))
            return JSValue::exception();
    retain(self);
    return self;
}

NATIVE(typedCopyWithin)
{
    UNUSED();
    JSTypedArray* t = validate(vm, self);
    if (!t)
        return JSValue::exception();
    const double len = t->length;
    double to = 0, from = 0, last = 0;
    if (!relativeTo(vm, arg(args, argc, 0), len, 0, &to) || !relativeTo(vm, arg(args, argc, 1), len, 0, &from) ||
        !relativeTo(vm, arg(args, argc, 2), len, len, &last))
        return JSValue::exception();
    double count = std::min(last - from, len - to);
    if (count > 0) {
        if (typedArrayOutOfBounds(t))
            return typeError(vm, "the typed array is out of bounds");
        const double now = t->length;
        count = std::min(count, std::min(now - to, now - from));
        if (count > 0) {
            const std::size_t size = kElemSize[std::size_t(t->kind)];
            std::memmove(t->data + std::size_t(to) * size, t->data + std::size_t(from) * size, std::size_t(count) * size);
        }
    }
    retain(self);
    return self;
}

NATIVE(typedReverse)
{
    UNUSED();
    JSTypedArray* t = validate(vm, self);
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

NATIVE(typedSlice)
{
    UNUSED();
    JSTypedArray* t = validate(vm, self);
    if (!t)
        return JSValue::exception();
    const double len = t->length;
    double from = 0, to = 0;
    if (!relativeTo(vm, arg(args, argc, 0), len, 0, &from) || !relativeTo(vm, arg(args, argc, 1), len, len, &to))
        return JSValue::exception();
    const double count = std::max(to - from, 0.0);
    Handle keep = Handle::retained(self);
    const JSValue countArg = JSValue::boxNumber(count);
    Handle out = Handle::adopt(speciesCreate(vm, self, &countArg, 1));
    if (out.get().isException())
        return out.get();
    if (count > 0) {
        if (typedArrayOutOfBounds(t))
            return typeError(vm, "the typed array is out of bounds");
        auto* o = static_cast<JSTypedArray*>(asObject(out.get()));
        const double now = t->length;
        const double end = std::min(to, now);
        const std::size_t size = kElemSize[std::size_t(t->kind)];
        if (o->kind == t->kind) {
            // Bytes, one by one in the order the language copies them, so overlapping views behave.
            double n = std::max(end - from, 0.0);
            n = std::min(n, double(o->length));
            const std::uint8_t* src = t->data + std::size_t(from) * size;
            std::uint8_t* dst = o->data;
            for (std::size_t b = 0; b < std::size_t(n) * size; ++b)
                dst[b] = src[b];
        } else {
            double n = 0;
            for (double k = from; k < end; ++k, ++n) {
                Handle e = Handle::adopt(elementAt(vm, t, k));
                if (!typedSet(vm, o, n, e.get()))
                    return JSValue::exception();
            }
        }
    }
    return out.take();
}

NATIVE(typedSubarray)
{
    UNUSED();
    JSTypedArray* t = typedArrayOf(self);
    if (!t)
        return typeError(vm, "this is not a typed array");
    Handle keep = Handle::retained(self);
    const double srcLength = typedArrayOutOfBounds(t) ? 0.0 : double(t->length);
    double begin = 0;
    if (!relativeTo(vm, arg(args, argc, 0), srcLength, 0, &begin))
        return JSValue::exception();
    const std::size_t size = kElemSize[std::size_t(t->kind)];
    const double beginByteOffset = double(t->byteOffset) + begin * double(size);
    JSValue list[3] = {t->buffer, JSValue::boxNumber(beginByteOffset), JSValue::undefined()};
    std::uint32_t count = 2;
    if (!(t->tracking && arg(args, argc, 1).isUndefined())) {
        double end = 0;
        if (!relativeTo(vm, arg(args, argc, 1), srcLength, srcLength, &end))
            return JSValue::exception();
        list[2] = JSValue::boxNumber(std::max(end - begin, 0.0));
        count = 3;
    }
    return speciesCreate(vm, self, list, count);
}

NATIVE(typedSearch)
{
    UNUSED();
    JSTypedArray* t = validate(vm, self);
    if (!t)
        return JSValue::exception();
    const int mode = int(reinterpret_cast<std::intptr_t>(data));  // 0 indexOf, 1 lastIndexOf, 2 includes
    const double len = t->length;
    if (len == 0)
        return mode == 2 ? JSValue::boolean(false) : JSValue::int32(-1);
    const JSValue target = arg(args, argc, 0);
    double n = 0;
    if (mode == 1) {
        if (argc > 1) {
            if (!integerOf(vm, args[1], &n))
                return JSValue::exception();
        } else {
            n = len - 1;
        }
        double k = (n >= 0 ? std::min(n, len - 1) : len + n) + 0.0;
        for (; k >= 0; --k) {
            if (k < double(t->length) && !typedArrayOutOfBounds(t)) {
                Handle e = Handle::adopt(typedGet(vm, t, std::uint32_t(k)));
                if (strictEquals(e.get(), target))
                    return JSValue::boxNumber(k);
            }
        }
        return JSValue::int32(-1);
    }
    if (!integerOf(vm, arg(args, argc, 1), &n))
        return JSValue::exception();
    double k = (n >= 0 ? n : std::max(len + n, 0.0)) + 0.0;
    for (; k < len; ++k) {
        const bool present = k < double(t->length) && !typedArrayOutOfBounds(t);
        if (mode == 0 && !present)
            continue;
        Handle e = Handle::adopt(present ? typedGet(vm, t, std::uint32_t(k)) : JSValue::undefined());
        if (mode == 2 ? sameValueZero(e.get(), target) : strictEquals(e.get(), target))
            return mode == 2 ? JSValue::boolean(true) : JSValue::boxNumber(k);
    }
    return mode == 2 ? JSValue::boolean(false) : JSValue::int32(-1);
}

NATIVE(typedJoin)
{
    UNUSED();
    JSTypedArray* t = validate(vm, self);
    if (!t)
        return JSValue::exception();
    const double len = t->length;
    String sep = u",";
    if (!arg(args, argc, 0).isUndefined()) {
        JSValue s = toStringValue(vm, args[0]);
        if (s.isException())
            return s;
        sep = String(asString(s)->text());
        release(s);
    }
    String out;
    for (double i = 0; i < len; ++i) {
        if (i > 0)
            out += sep;
        Handle e = Handle::adopt(elementAt(vm, t, i));
        if (e.get().isUndefined())
            continue;
        JSValue s = toStringValue(vm, e.get());
        if (s.isException())
            return s;
        out += asString(s)->text();
        release(s);
    }
    return makeString(vm, StringView(out));
}

NATIVE(typedToLocaleString)
{
    UNUSED();
    JSTypedArray* t = validate(vm, self);
    if (!t)
        return JSValue::exception();
    const double len = t->length;
    Key toLocale = vm.internKey(std::string_view("toLocaleString"));
    struct Release
    {
        Key k;
        ~Release() { detail::decRef(k); }
    } guard{toLocale};
    String out;
    for (double k = 0; k < len; ++k) {
        if (k > 0)
            out.push_back(u',');
        Handle e = Handle::adopt(elementAt(vm, t, k));
        if (e.get().isNullish())
            continue;
        Handle f = Handle::adopt(getProperty(vm, e.get(), toLocale));
        if (f.get().isException())
            return f.get();
        if (!isFunction(f.get()))
            return typeError(vm, describe(vm, f.get()) + " is not a function");
        JSValue r = vm.call(f.get(), e.get(), nullptr, 0);
        if (r.isException())
            return r;
        Handle text = Handle::adopt(toStringValue(vm, r));
        release(r);
        if (text.get().isException())
            return text.get();
        out += asString(text.get())->text();
    }
    return makeString(vm, StringView(out));
}

/// The elements of `t` sorted: numerically unless `compare` says otherwise, -0 before +0 and NaN last. False with an
/// exception pending.
bool sortElements(VM& vm, std::vector<Handle>& items, JSValue compare)
{
    bool failed = false;
    auto less = [&](const Handle& a, const Handle& b) -> bool {
        if (failed)
            return false;
        if (!compare.isUndefined()) {
            JSValue pair[2] = {a.get(), b.get()};
            JSValue r = vm.call(compare, JSValue::undefined(), pair, 2);
            if (r.isException()) {
                failed = true;
                return false;
            }
            double d = 0;
            const bool ok = toNumber(vm, r, &d);
            release(r);
            if (!ok) {
                failed = true;
                return false;
            }
            return d < 0;
        }
        const JSValue x = a.get(), y = b.get();
        if (isBigInt(x) && isBigInt(y)) {
            bool ok = true;
            return bigintCompareWith(vm, x, y, &ok) < 0;
        }
        const double dx = x.asNumber(), dy = y.asNumber();
        if (dx != dx)
            return false;
        if (dy != dy)
            return true;
        if (dx < dy)
            return true;
        if (dx > dy)
            return false;
        return dx == 0 && dy == 0 && std::signbit(dx) && !std::signbit(dy);
    };
    std::stable_sort(items.begin(), items.end(), less);
    return !failed;
}

NATIVE(typedSort)
{
    UNUSED();
    const JSValue compare = arg(args, argc, 0);
    if (!compare.isUndefined() && !isFunction(compare))
        return typeError(vm, "The comparison function must be either a function or undefined");
    JSTypedArray* t = validate(vm, self);
    if (!t)
        return JSValue::exception();
    const double len = t->length;
    Handle keep = Handle::retained(self);
    std::vector<Handle> items;
    items.reserve(std::size_t(len));
    for (double k = 0; k < len; ++k)
        items.push_back(Handle::adopt(typedGet(vm, t, std::uint32_t(k))));
    if (!sortElements(vm, items, compare))
        return JSValue::exception();
    for (std::size_t k = 0; k < items.size(); ++k)
        if (!typedSet(vm, t, double(k), items[k].get()))
            return JSValue::exception();
    retain(self);
    return self;
}

NATIVE(typedToReversed)
{
    UNUSED();
    JSTypedArray* t = validate(vm, self);
    if (!t)
        return JSValue::exception();
    const std::uint32_t len = t->length;
    JSTypedArray* out = newTypedArrayOf(vm, t->kind, len);
    if (!out)
        return JSValue::exception();
    Handle keep = Handle::adopt(toJSValue(out));
    const std::size_t size = kElemSize[std::size_t(t->kind)];
    for (std::uint32_t k = 0; k < len; ++k)
        std::memcpy(out->data + std::size_t(k) * size, t->data + std::size_t(len - k - 1) * size, size);
    return keep.take();
}

NATIVE(typedToSorted)
{
    UNUSED();
    const JSValue compare = arg(args, argc, 0);
    if (!compare.isUndefined() && !isFunction(compare))
        return typeError(vm, "The comparison function must be either a function or undefined");
    JSTypedArray* t = validate(vm, self);
    if (!t)
        return JSValue::exception();
    const std::uint32_t len = t->length;
    JSTypedArray* out = newTypedArrayOf(vm, t->kind, len);
    if (!out)
        return JSValue::exception();
    Handle keep = Handle::adopt(toJSValue(out));
    Handle selfKeep = Handle::retained(self);
    std::vector<Handle> items;
    for (std::uint32_t k = 0; k < len; ++k)
        items.push_back(Handle::adopt(typedGet(vm, t, k)));
    if (!sortElements(vm, items, compare))
        return JSValue::exception();
    for (std::uint32_t k = 0; k < len; ++k)
        if (!typedSet(vm, out, double(k), items[k].get()))
            return JSValue::exception();
    return keep.take();
}

NATIVE(typedWith)
{
    UNUSED();
    JSTypedArray* t = validate(vm, self);
    if (!t)
        return JSValue::exception();
    const double len = t->length;
    double rel = 0;
    if (!integerOf(vm, arg(args, argc, 0), &rel))
        return JSValue::exception();
    const double actual = rel >= 0 ? rel : len + rel;
    JSValue converted;
    if (!typedConvert(vm, t->kind, arg(args, argc, 1), &converted))
        return JSValue::exception();
    Handle value = Handle::adopt(converted);
    if (!(actual >= 0 && actual < double(t->length)) || typedArrayOutOfBounds(t))
        return vm.throwError(ErrorCode::RangeError, "Invalid typed array index");
    JSTypedArray* out = newTypedArrayOf(vm, t->kind, std::uint32_t(len));
    if (!out)
        return JSValue::exception();
    Handle keep = Handle::adopt(toJSValue(out));
    for (double k = 0; k < len; ++k) {
        Handle e = k == actual ? Handle::retained(value.get()) : Handle::adopt(elementAt(vm, t, k));
        if (!typedSet(vm, out, k, e.get()))
            return JSValue::exception();
    }
    return keep.take();
}

NATIVE(typedSet_)
{
    UNUSED();
    JSTypedArray* target = typedArrayOf(self);
    if (!target)
        return typeError(vm, "this is not a typed array");
    Handle keep = Handle::retained(self);
    double offset = 0;
    if (!integerOf(vm, arg(args, argc, 1), &offset))
        return JSValue::exception();
    if (offset < 0)
        return vm.throwError(ErrorCode::RangeError, "offset is out of bounds");
    const JSValue source = arg(args, argc, 0);
    if (typedArrayOutOfBounds(target))
        return typeError(vm, "the typed array is out of bounds");
    const double targetLength = target->length;
    if (JSTypedArray* src = typedArrayOf(source)) {
        if (typedArrayOutOfBounds(src))
            return typeError(vm, "the source typed array is out of bounds");
        if (isBigKind(src->kind) != isBigKind(target->kind))
            return typeError(vm, "Cannot mix BigInt and other types, use explicit conversions");
        const double srcLength = src->length;
        if (offset == HUGE_VAL || srcLength + offset > targetLength)
            return vm.throwError(ErrorCode::RangeError, "offset is out of bounds");
        const std::size_t srcSize = kElemSize[std::size_t(src->kind)], dstSize = kElemSize[std::size_t(target->kind)];
        if (src->kind == target->kind) {
            std::memmove(target->data + std::size_t(offset) * dstSize, src->data, std::size_t(srcLength) * srcSize);
            return JSValue::undefined();
        }
        // Different kinds on one buffer: read the source first, as it was.
        std::vector<Handle> values;
        for (double k = 0; k < srcLength; ++k)
            values.push_back(Handle::adopt(typedGet(vm, src, std::uint32_t(k))));
        for (double k = 0; k < srcLength; ++k)
            if (!typedSet(vm, target, offset + k, values[std::size_t(k)].get()))
                return JSValue::exception();
        return JSValue::undefined();
    }
    Handle object = Handle::adopt(toObject(vm, source));
    if (object.get().isException())
        return object.get();
    Handle lengthValue = Handle::adopt(getProperty(vm, object.get(), vm.atoms.length));
    if (lengthValue.get().isException())
        return lengthValue.get();
    double srcLength = 0;
    if (!integerOf(vm, lengthValue.get(), &srcLength))
        return JSValue::exception();
    srcLength = std::max(0.0, std::min(srcLength, 9007199254740991.0));
    if (offset == HUGE_VAL || srcLength + offset > targetLength)
        return vm.throwError(ErrorCode::RangeError, "offset is out of bounds");
    for (double k = 0; k < srcLength; ++k) {
        Handle v = Handle::adopt(getIndex(vm, object.get(), std::uint32_t(k)));
        if (v.get().isException())
            return v.get();
        if (!typedSet(vm, target, offset + k, v.get()))
            return JSValue::exception();
    }
    return JSValue::undefined();
}

NATIVE(typedIterator)
{
    UNUSED();
    if (!validate(vm, self))
        return JSValue::exception();
    return makeArrayIterator(vm, self, int(reinterpret_cast<std::intptr_t>(data)));
}

/// `target[i] = v` as the language does it: a typed array converts and ignores what is out of range.
bool setIndexOn(VM& vm, JSValue target, double i, JSValue v)
{
    if (JSTypedArray* t = typedArrayOf(target))
        return typedSet(vm, t, i, v);
    Key k = toPropertyKey(vm, JSValue::boxNumber(i));
    if (!k)
        return false;
    const bool ok = setProperty(vm, target, k, v, true);
    detail::decRef(k);
    return ok;
}

/// `new C(length)` or the like for `TypedArray.from` and `.of`, checked to be a typed array that long.
JSValue createFrom(VM& vm, JSValue c, const JSValue* args, std::uint32_t argc, double minLength)
{
    JSValue made = vm.construct(c, args, argc);
    if (made.isException())
        return made;
    Handle keep = Handle::adopt(made);
    JSTypedArray* t = validate(vm, made);
    if (!t)
        return JSValue::exception();
    if (double(t->length) < minLength)
        return typeError(vm, "the typed array made is too short");
    return keep.take();
}

NATIVE(typedFrom)
{
    UNUSED();
    if (!isConstructor(self))
        return typeError(vm, describe(vm, self) + " is not a constructor");
    const JSValue source = arg(args, argc, 0), mapFn = arg(args, argc, 1), thisArg = arg(args, argc, 2);
    if (!mapFn.isUndefined() && !isFunction(mapFn))
        return typeError(vm, describe(vm, mapFn) + " is not a function");
    Handle usingIterator = Handle::adopt(getProperty(vm, source, vm.atoms.symIterator));
    if (usingIterator.get().isException())
        return usingIterator.get();
    if (!usingIterator.get().isNullish() && !isFunction(usingIterator.get()))
        return typeError(vm, "object[Symbol.iterator] is not a function");
    std::vector<Handle> values;
    if (!usingIterator.get().isNullish()) {
        Handle iterator = Handle::adopt(vm.call(usingIterator.get(), source, nullptr, 0));
        if (iterator.get().isException())
            return iterator.get();
        if (!isObject(iterator.get()))
            return typeError(vm, "Result of the Symbol.iterator method is not an object");
        Handle next = Handle::adopt(getProperty(vm, iterator.get(), vm.atoms.next));
        if (next.get().isException())
            return next.get();
        for (;;) {
            JSValue v;
            const int step = iteratorStep(vm, iterator.get(), next.get(), &v);
            if (step < 0)
                return JSValue::exception();
            if (step == 0)
                break;
            values.push_back(Handle::adopt(v));
        }
        const JSValue countArg = JSValue::boxNumber(double(values.size()));
        Handle target = Handle::adopt(createFrom(vm, self, &countArg, 1, double(values.size())));
        if (target.get().isException())
            return target.get();
        for (std::size_t k = 0; k < values.size(); ++k) {
            Handle mapped = Handle::retained(values[k].get());
            if (!mapFn.isUndefined()) {
                JSValue callArgs[2] = {values[k].get(), JSValue::boxNumber(double(k))};
                JSValue r = vm.call(mapFn, thisArg, callArgs, 2);
                if (r.isException())
                    return r;
                mapped = Handle::adopt(r);
            }
            if (!setIndexOn(vm, target.get(), double(k), mapped.get()))
                return JSValue::exception();
        }
        return target.take();
    }
    Handle arrayLike = Handle::adopt(toObject(vm, source));
    if (arrayLike.get().isException())
        return arrayLike.get();
    Handle lengthValue = Handle::adopt(getProperty(vm, arrayLike.get(), vm.atoms.length));
    if (lengthValue.get().isException())
        return lengthValue.get();
    double len = 0;
    if (!integerOf(vm, lengthValue.get(), &len))
        return JSValue::exception();
    len = std::max(0.0, std::min(len, 9007199254740991.0));
    const JSValue countArg = JSValue::boxNumber(len);
    Handle target = Handle::adopt(createFrom(vm, self, &countArg, 1, len));
    if (target.get().isException())
        return target.get();
    for (double k = 0; k < len; ++k) {
        Handle v = Handle::adopt(getIndex(vm, arrayLike.get(), std::uint32_t(k)));
        if (v.get().isException())
            return v.get();
        Handle mapped = Handle::retained(v.get());
        if (!mapFn.isUndefined()) {
            JSValue callArgs[2] = {v.get(), JSValue::boxNumber(k)};
            JSValue r = vm.call(mapFn, thisArg, callArgs, 2);
            if (r.isException())
                return r;
            mapped = Handle::adopt(r);
        }
        if (!setIndexOn(vm, target.get(), k, mapped.get()))
            return JSValue::exception();
    }
    return target.take();
}

NATIVE(typedOf)
{
    UNUSED();
    if (!isConstructor(self))
        return typeError(vm, describe(vm, self) + " is not a constructor");
    const JSValue countArg = JSValue::boxNumber(double(argc));
    Handle target = Handle::adopt(createFrom(vm, self, &countArg, 1, double(argc)));
    if (target.get().isException())
        return target.get();
    for (std::uint32_t k = 0; k < argc; ++k)
        if (!setIndexOn(vm, target.get(), double(k), args[k]))
            return JSValue::exception();
    return target.take();
}

}  // namespace

void installTypedArrayMethods(VM& vm, JSObject* taProto, JSObject* taCtor)
{
    {
        Key name = vm.internKey(std::string_view("get [Symbol.species]"));
        JSFunction* getter = vm.newNativeFunction(vm.protos.function,
                                                  [](VM&, JSValue self, const JSValue*, std::uint32_t, void*) -> JSValue {
                                                      retain(self);
                                                      return self;
                                                  },
                                                  nullptr, name, 0);
        Handle keep = Handle::adopt(toJSValue(getter));
        defineAccessor(vm, taCtor, vm.atoms.symSpecies, toJSValue(getter), JSValue::undefined(), attr::kConfigurable);
        detail::decRef(name);
    }
    method(vm, taCtor, "from", typedFrom, 1);
    method(vm, taCtor, "of", typedOf, 0);
    static const char* const iterNames[] = {"forEach", "map", "filter", "some", "every", "find", "findIndex", "findLast", "findLastIndex"};
    for (int i = 0; i < 9; ++i)
        method(vm, taProto, iterNames[i], typedIterate, 1, reinterpret_cast<void*>(std::intptr_t(i)));
    method(vm, taProto, "reduce", typedReduce, 1, nullptr);
    method(vm, taProto, "reduceRight", typedReduce, 1, reinterpret_cast<void*>(1));
    method(vm, taProto, "fill", typedFill, 1);
    method(vm, taProto, "copyWithin", typedCopyWithin, 2);
    method(vm, taProto, "set", typedSet_, 1);
    method(vm, taProto, "slice", typedSlice, 2);
    method(vm, taProto, "subarray", typedSubarray, 2);
    method(vm, taProto, "indexOf", typedSearch, 1, reinterpret_cast<void*>(0));
    method(vm, taProto, "lastIndexOf", typedSearch, 1, reinterpret_cast<void*>(1));
    method(vm, taProto, "includes", typedSearch, 1, reinterpret_cast<void*>(2));
    method(vm, taProto, "join", typedJoin, 1);
    method(vm, taProto, "reverse", typedReverse, 0);
    method(vm, taProto, "sort", typedSort, 1);
    method(vm, taProto, "at", typedAt, 1);
    method(vm, taProto, "toLocaleString", typedToLocaleString, 0);
    method(vm, taProto, "toReversed", typedToReversed, 0);
    method(vm, taProto, "toSorted", typedToSorted, 1);
    method(vm, taProto, "with", typedWith, 2);
    method(vm, taProto, "keys", typedIterator, 0, reinterpret_cast<void*>(0));
    method(vm, taProto, "entries", typedIterator, 0, reinterpret_cast<void*>(2));
    JSFunction* values = method(vm, taProto, "values", typedIterator, 0, reinterpret_cast<void*>(1));
    retain(toJSValue(values));
    taProto->putOwn(vm, vm.atoms.symIterator, toJSValue(values), attr::kWritable | attr::kConfigurable);
    // `toString` is the arrays'.
    Key k = vm.internKey(std::string_view("toString"));
    if (JSValue* f = vm.protos.array->findOwn(vm, k)) {
        retain(*f);
        taProto->putOwn(vm, k, *f, attr::kWritable | attr::kConfigurable);
    }
    detail::decRef(k);
}

}  // namespace bi
}  // namespace vm
}  // namespace internal
}  // namespace script
