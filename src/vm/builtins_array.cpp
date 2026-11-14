// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The Array.prototype methods that call back into the program, over any array-like: each element is asked for as the
// language asks, so what a callback or a getter changes along the way is seen. Dense arrays are read directly.
#include <algorithm>

#include "script/internal/stack_guard.hpp"
#include "builtins_util.hpp"

namespace script {
namespace internal {
namespace vm {
namespace bi {

namespace {

/// A name for a property, interned for as long as this holds it.
struct Name
{
    Key key;
    Name(VM& vm, const char* text) : key(vm.internKey(std::string_view(text))) {}
    Name(const Name&) = delete;
    Name& operator=(const Name&) = delete;
    ~Name() { detail::decRef(key); }
};

/// `ToLength(O.length)`; false with an exception pending.
bool lengthOf(VM& vm, JSValue o, double* out)
{
    if (hasElems(o)) {
        *out = static_cast<JSArray*>(asObject(o))->length;
        return true;
    }
    Handle v = Handle::adopt(getProperty(vm, o, vm.atoms.length));
    if (v.get().isException())
        return false;
    double d = 0;
    if (!toNumber(vm, v.get(), &d))
        return false;
    d = d != d ? 0 : std::trunc(d);
    *out = d <= 0 ? 0 : std::min(d, 9007199254740991.0);
    return true;
}

/// `O[i]` when `O` has it (`mustHave`) or always: 1 with `*out` owned, 0 for absent, -1 on an exception.
int readIndex(VM& vm, JSValue o, double i, Handle* out, bool mustHave)
{
    if (hasElems(o)) {
        auto* a = static_cast<JSArray*>(asObject(o));
        if (i < double(a->length) && !a->at(std::uint32_t(i)).isHole()) {
            *out = Handle::retained(a->at(std::uint32_t(i)));
            return 1;
        }
    }
    Key k = toPropertyKey(vm, JSValue::boxNumber(i));
    if (!k)
        return -1;
    if (mustHave && !hasProperty(vm, o, k)) {
        detail::decRef(k);
        return vm.exception.isUndefined() ? 0 : -1;
    }
    JSValue v = getProperty(vm, o, k);
    detail::decRef(k);
    if (v.isException())
        return -1;
    *out = Handle::adopt(v);
    return 1;
}

/// `CreateDataPropertyOrThrow(A, i, v)`; the caller keeps its count of `v`.
bool createData(VM& vm, JSValue a, double i, JSValue v)
{
    if (isArray(a) && !(asObject(a)->h.flags & (cellflags::kFrozen | cellflags::kNotExtensible))) {
        auto* arr = static_cast<JSArray*>(asObject(a));
        // Appending, or replacing an element that is there: nothing kept by name can be in the way.
        const bool plain = arr->proto(vm) == vm.protos.array && !arr->isDictionary(vm) && arr->shapeOf(vm)->count == 0 &&
                           vm.protos.array->proto(vm) == vm.protos.object;
        if (i < double(arr->length) ? (plain || !arr->at(std::uint32_t(i)).isHole()) : i == double(arr->length)) {
            retain(v);
            if (arr->setAt(vm, std::uint32_t(i), v))
                return true;
            return vm.throwError(ErrorCode::TypeError, "Cannot add property to array"), false;
        }
    }
    Key k = toPropertyKey(vm, JSValue::boxNumber(i));
    if (!k)
        return false;
    retain(v);
    const bool ok = isObject(a) && defineOwn(vm, asObject(a), k, v, attr::kDefault);
    detail::decRef(k);
    if (!ok && vm.exception.isUndefined())
        vm.throwError(ErrorCode::TypeError, "Cannot define property " + std::to_string(std::uint64_t(i)) + ", object is not extensible");
    return ok;
}

/// ArrayCreate(length) with the RangeErrors the language has.
JSValue arrayCreate(VM& vm, double length)
{
    if (length > 4294967295.0)
        return vm.throwError(ErrorCode::RangeError, "Invalid array length");
    if (length > double(JSArray::kMaxDenseGap))
        throw Error(ErrorCode::OutOfMemory, "arrays are dense; this length is too large");
    JSArray* a = vm.newArray(vm.protos.array);
    Handle keep = Handle::adopt(toJSValue(a));
    if (length > 0)
        a->resize(vm, std::uint32_t(length));
    return keep.take();
}

}  // namespace

JSValue arraySpeciesCreate(VM& vm, JSValue original, double length)
{
    JSValue probe = original;
    while (isObject(probe) && proxyOf(asObject(probe))) {
        ProxyState* ps = proxyOf(asObject(probe));
        if (!isObject(ps->handler))
            return vm.throwError(ErrorCode::TypeError, "Cannot perform 'IsArray' on a proxy that has been revoked");
        probe = ps->target;
    }
    if (!isArray(probe))
        return arrayCreate(vm, length);
    Handle ctor = Handle::adopt(getProperty(vm, original, vm.atoms.constructor));
    if (ctor.get().isException())
        return ctor.get();
    if (isObject(ctor.get())) {
        ctor = Handle::adopt(getProperty(vm, ctor.get(), vm.atoms.symSpecies));
        if (ctor.get().isException())
            return ctor.get();
        if (ctor.get().isNull())
            ctor = Handle();
    }
    if (ctor.get().isUndefined())
        return arrayCreate(vm, length);
    if (!isConstructor(ctor.get()))
        return vm.throwError(ErrorCode::TypeError, "object.constructor[Symbol.species] is not a constructor");
    const JSValue lengthArg = JSValue::boxNumber(length);
    return vm.construct(ctor.get(), &lengthArg, 1);
}

namespace {

/// forEach, map, filter, some, every, find, findIndex, findLast, findLastIndex: the kind is in `data`.
NATIVE(arrayIterate)
{
    UNUSED();
    if (self.isNullish())
        return typeError(vm, "Array.prototype method called on null or undefined");
    Handle o = Handle::adopt(toObject(vm, self));
    if (o.get().isException())
        return o.get();
    const int kind = int(reinterpret_cast<std::intptr_t>(data));
    double len = 0;
    if (!lengthOf(vm, o.get(), &len))
        return JSValue::exception();
    const JSValue callback = arg(args, argc, 0), thisArg = arg(args, argc, 1);
    if (!isFunction(callback))
        return typeError(vm, describe(vm, callback) + " is not a function");
    Handle out;
    if (kind == 1 || kind == 2) {
        out = Handle::adopt(arraySpeciesCreate(vm, o.get(), kind == 1 ? len : 0));
        if (out.get().isException())
            return out.get();
    }
    const bool finder = kind >= 5;
    const bool backwards = kind >= 7;
    double kept = 0;
    for (double n = 0; n < len; ++n) {
        const double i = backwards ? len - 1 - n : n;
        Handle e;
        const int found = readIndex(vm, o.get(), i, &e, !finder);
        if (found < 0)
            return JSValue::exception();
        if (!found)
            continue;
        JSValue call[3] = {e.get(), JSValue::boxNumber(i), o.get()};
        JSValue r = vm.call(callback, thisArg, call, 3);
        if (r.isException())
            return r;
        Handle result = Handle::adopt(r);
        switch (kind) {
            case 0: break;
            case 1:
                if (!createData(vm, out.get(), i, r))
                    return JSValue::exception();
                break;
            case 2:
                if (toBoolean(r) && !createData(vm, out.get(), kept++, e.get()))
                    return JSValue::exception();
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
                    return e.take();
                break;
            default:
                if (toBoolean(r))
                    return JSValue::boxNumber(i);
                break;
        }
    }
    switch (kind) {
        case 1:
        case 2: return out.take();
        case 3: return JSValue::boolean(false);
        case 4: return JSValue::boolean(true);
        case 6:
        case 8: return JSValue::int32(-1);
        default: return JSValue::undefined();
    }
}

NATIVE(arrayReduce)
{
    UNUSED();
    if (self.isNullish())
        return typeError(vm, "Array.prototype method called on null or undefined");
    Handle o = Handle::adopt(toObject(vm, self));
    if (o.get().isException())
        return o.get();
    const bool right = data != nullptr;
    double len = 0;
    if (!lengthOf(vm, o.get(), &len))
        return JSValue::exception();
    const JSValue callback = arg(args, argc, 0);
    if (!isFunction(callback))
        return typeError(vm, describe(vm, callback) + " is not a function");
    double n = 0;
    Handle acc;
    if (argc >= 2) {
        acc = Handle::retained(args[1]);
    } else {
        bool found = false;
        for (; n < len && !found; ++n) {
            const int r = readIndex(vm, o.get(), right ? len - 1 - n : n, &acc, true);
            if (r < 0)
                return JSValue::exception();
            found = r > 0;
        }
        if (!found)
            return typeError(vm, "Reduce of empty array with no initial value");
    }
    for (; n < len; ++n) {
        const double i = right ? len - 1 - n : n;
        Handle e;
        const int found = readIndex(vm, o.get(), i, &e, true);
        if (found < 0)
            return JSValue::exception();
        if (!found)
            continue;
        JSValue call[4] = {acc.get(), e.get(), JSValue::boxNumber(i), o.get()};
        JSValue r = vm.call(callback, JSValue::undefined(), call, 4);
        if (r.isException())
            return r;
        acc = Handle::adopt(r);
    }
    return acc.take();
}


/// ToIntegerOrInfinity.
bool integerOf(VM& vm, JSValue v, double* out)
{
    double d = 0;
    if (!toNumber(vm, v, &d))
        return false;
    *out = d != d ? 0 : std::trunc(d);
    return true;
}

/// A relative index (`start`, `end`) as the methods clamp it into [0, len].
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

bool setIndex(VM& vm, JSValue o, double i, JSValue v)
{
    Key k = toPropertyKey(vm, JSValue::boxNumber(i));
    if (!k)
        return false;
    const bool ok = setProperty(vm, o, k, v, true);
    detail::decRef(k);
    return ok;
}

bool deleteIndex(VM& vm, JSValue o, double i)
{
    Key k = toPropertyKey(vm, JSValue::boxNumber(i));
    if (!k)
        return false;
    const bool ok = deleteProperty(vm, o, k, true);
    detail::decRef(k);
    return ok;
}

/// `ToObject(this)` and its length.
bool thisAndLength(VM& vm, JSValue self, Handle* o, double* len)
{
    if (self.isNullish()) {
        vm.throwError(ErrorCode::TypeError, "Array.prototype method called on null or undefined");
        return false;
    }
    *o = Handle::adopt(toObject(vm, self));
    if (o->get().isException())
        return false;
    return lengthOf(vm, o->get(), len);
}

/// `Array.from` and `Array.of`: the result is made by `this` when that is a constructor other than Array.
bool generic(VM& vm, JSValue self)
{
    return isConstructor(self) && self.bits != vm.arrayConstructor.bits;
}

/// Set `A.length` after the elements went in, as the generic path must.
bool finishLength(VM& vm, JSValue a, double n)
{
    return setProperty(vm, a, vm.atoms.length, JSValue::boxNumber(n), true);
}

NATIVE(arrayFrom)
{
    UNUSED();
    const JSValue items = arg(args, argc, 0), mapFn = arg(args, argc, 1), thisArg = arg(args, argc, 2);
    if (!mapFn.isUndefined() && !isFunction(mapFn))
        return typeError(vm, describe(vm, mapFn) + " is not a function");
    const bool viaCtor = generic(vm, self);
    Handle out;
    double index = 0;
    auto put = [&](JSValue v) -> Step {
        if ((std::uint64_t(index) & 1023) == 1023 && vm.ctx.interruptAsked()) {
            vm.throwError(ErrorCode::Interrupted, "execution was interrupted");
            return Step::Fail;
        }
        Handle mapped;
        if (!mapFn.isUndefined()) {
            JSValue callArgs[2] = {v, JSValue::boxNumber(index)};
            mapped = Handle::adopt(vm.call(mapFn, thisArg, callArgs, 2));
            if (mapped.get().isException())
                return Step::Fail;
            v = mapped.get();
        }
        if (!createData(vm, out.get(), index, v))
            return Step::Fail;
        ++index;
        return Step::Continue;
    };
    if (items.isNullish())
        return typeError(vm, describe(vm, items) + " is not iterable");
    Handle method = Handle::adopt(getProperty(vm, items, vm.atoms.symIterator));
    if (method.get().isException())
        return method.get();
    if (!method.get().isNullish()) {
        if (!isFunction(method.get()))
            return typeError(vm, "Symbol.iterator is not a function");
        out = Handle::adopt(viaCtor ? vm.construct(self, nullptr, 0) : toJSValue(vm.newArray(vm.protos.array)));
        if (out.get().isException())
            return out.get();
        if (!forEachIterated(vm, items, put))
            return JSValue::exception();
        if (!finishLength(vm, out.get(), index))
            return JSValue::exception();
        return out.take();
    }
    Handle arrayLike = Handle::adopt(toObject(vm, items));
    if (arrayLike.get().isException())
        return arrayLike.get();
    double len = 0;
    if (!lengthOf(vm, arrayLike.get(), &len))
        return JSValue::exception();
    if (viaCtor) {
        JSValue n = JSValue::boxNumber(len);
        out = Handle::adopt(vm.construct(self, &n, 1));
    } else {
        // Grown element by element, so the memory limit and the interrupt handler see a huge length coming.
        out = Handle::adopt(len > 4294967295.0 ? arrayCreate(vm, len) : toJSValue(vm.newArray(vm.protos.array)));
    }
    if (out.get().isException())
        return out.get();
    for (double k = 0; k < len; ++k) {
        Handle value;
        if (readIndex(vm, arrayLike.get(), k, &value, false) < 0)
            return JSValue::exception();
        if (put(value.get()) == Step::Fail)
            return JSValue::exception();
    }
    if (!finishLength(vm, out.get(), len))
        return JSValue::exception();
    return out.take();
}

NATIVE(arrayOf)
{
    UNUSED();
    Handle out;
    if (generic(vm, self)) {
        JSValue n = JSValue::int32(std::int32_t(argc));
        out = Handle::adopt(vm.construct(self, &n, 1));
    } else {
        out = Handle::adopt(arrayCreate(vm, double(argc)));
    }
    if (out.get().isException())
        return out.get();
    for (std::uint32_t i = 0; i < argc; ++i)
        if (!createData(vm, out.get(), double(i), args[i]))
            return JSValue::exception();
    if (!finishLength(vm, out.get(), double(argc)))
        return JSValue::exception();
    return out.take();
}

NATIVE(arrayAtGeneric)
{
    UNUSED();
    Handle o;
    double len = 0, rel = 0;
    if (!thisAndLength(vm, self, &o, &len) || !integerOf(vm, arg(args, argc, 0), &rel))
        return JSValue::exception();
    const double k = rel >= 0 ? rel : len + rel;
    if (k < 0 || k >= len)
        return JSValue::undefined();
    Handle e;
    if (readIndex(vm, o.get(), k, &e, false) < 0)
        return JSValue::exception();
    return e.take();
}

NATIVE(arrayToReversedGeneric)
{
    UNUSED();
    Handle o;
    double len = 0;
    if (!thisAndLength(vm, self, &o, &len))
        return JSValue::exception();
    Handle out = Handle::adopt(arrayCreate(vm, len));
    if (out.get().isException())
        return out.get();
    for (double k = 0; k < len; ++k) {
        Handle e;
        if (readIndex(vm, o.get(), len - k - 1, &e, false) < 0 || !createData(vm, out.get(), k, e.get()))
            return JSValue::exception();
    }
    return out.take();
}

NATIVE(arrayWithGeneric)
{
    UNUSED();
    Handle o;
    double len = 0, rel = 0;
    if (!thisAndLength(vm, self, &o, &len) || !integerOf(vm, arg(args, argc, 0), &rel))
        return JSValue::exception();
    const double actual = rel >= 0 ? rel : len + rel;
    if (actual >= len || actual < 0)
        return vm.throwError(ErrorCode::RangeError, "Invalid index : " + describe(vm, arg(args, argc, 0)));
    Handle out = Handle::adopt(arrayCreate(vm, len));
    if (out.get().isException())
        return out.get();
    for (double k = 0; k < len; ++k) {
        Handle e;
        if (k == actual)
            e = Handle::retained(arg(args, argc, 1));
        else if (readIndex(vm, o.get(), k, &e, false) < 0)
            return JSValue::exception();
        if (!createData(vm, out.get(), k, e.get()))
            return JSValue::exception();
    }
    return out.take();
}

NATIVE(arrayToSplicedGeneric)
{
    UNUSED();
    Handle o;
    double len = 0, start = 0;
    if (!thisAndLength(vm, self, &o, &len) || !relativeTo(vm, arg(args, argc, 0), len, 0, &start))
        return JSValue::exception();
    const std::uint32_t inserts = argc > 2 ? argc - 2 : 0;
    double skip = 0;
    if (argc == 1) {
        skip = len - start;
    } else if (argc > 1) {
        double d = 0;
        if (!integerOf(vm, args[1], &d))
            return JSValue::exception();
        skip = std::max(0.0, std::min(d, len - start));
    }
    const double newLen = len + inserts - skip;
    if (newLen > 9007199254740991.0)
        return typeError(vm, "Invalid array length");
    Handle out = Handle::adopt(arrayCreate(vm, newLen));
    if (out.get().isException())
        return out.get();
    double i = 0;
    for (; i < start; ++i) {
        Handle e;
        if (readIndex(vm, o.get(), i, &e, false) < 0 || !createData(vm, out.get(), i, e.get()))
            return JSValue::exception();
    }
    for (std::uint32_t n = 0; n < inserts; ++n, ++i)
        if (!createData(vm, out.get(), i, args[2 + n]))
            return JSValue::exception();
    for (double r = start + skip; i < newLen; ++i, ++r) {
        Handle e;
        if (readIndex(vm, o.get(), r, &e, false) < 0 || !createData(vm, out.get(), i, e.get()))
            return JSValue::exception();
    }
    return out.take();
}

NATIVE(arrayToSortedGeneric)
{
    UNUSED();
    const JSValue compare = arg(args, argc, 0);
    if (!compare.isUndefined() && !isFunction(compare))
        return typeError(vm, "The comparison function must be either a function or undefined");
    Handle o;
    double len = 0;
    if (!thisAndLength(vm, self, &o, &len))
        return JSValue::exception();
    Handle out = Handle::adopt(arrayCreate(vm, len));
    if (out.get().isException())
        return out.get();
    auto* a = static_cast<JSArray*>(asObject(out.get()));
    for (double k = 0; k < len; ++k) {
        Handle e;
        if (readIndex(vm, o.get(), k, &e, false) < 0)
            return JSValue::exception();
        retain(e.get());
        a->setAt(vm, std::uint32_t(k), e.get());
    }
    JSValue r = sortDenseArray(vm, out.get(), compare);
    if (r.isException())
        return r;
    release(r);
    return out.take();
}

/// FlattenIntoArray; false with an exception pending.
bool flatten(VM& vm, JSValue target, JSValue source, double sourceLen, double* targetIndex, double depth, JSValue mapper, JSValue thisArg)
{
    if (nativeStackLow()) {
        vm.throwError(ErrorCode::StackOverflow, "Maximum call stack size exceeded");
        return false;
    }
    for (double i = 0; i < sourceLen; ++i) {
        Handle element;
        const int found = readIndex(vm, source, i, &element, true);
        if (found < 0)
            return false;
        if (!found)
            continue;
        if (!mapper.isUndefined()) {
            JSValue call[3] = {element.get(), JSValue::boxNumber(i), source};
            JSValue r = vm.call(mapper, thisArg, call, 3);
            if (r.isException())
                return false;
            element = Handle::adopt(r);
        }
        bool flat = false;
        if (depth > 0) {
            JSValue probe = element.get();
            while (isObject(probe) && proxyOf(asObject(probe))) {
                ProxyState* ps = proxyOf(asObject(probe));
                if (!isObject(ps->handler)) {
                    vm.throwError(ErrorCode::TypeError, "Cannot perform 'IsArray' on a proxy that has been revoked");
                    return false;
                }
                probe = ps->target;
            }
            flat = isArray(probe);
        }
        if (flat) {
            double inner = 0;
            if (!lengthOf(vm, element.get(), &inner) || !flatten(vm, target, element.get(), inner, targetIndex, depth - 1, JSValue::undefined(), JSValue::undefined()))
                return false;
        } else {
            if (*targetIndex >= 9007199254740991.0) {
                vm.throwError(ErrorCode::TypeError, "Invalid array length");
                return false;
            }
            if (!createData(vm, target, *targetIndex, element.get()))
                return false;
            ++*targetIndex;
        }
    }
    return true;
}

/// flat (data null) and flatMap.
NATIVE(arrayFlatGeneric)
{
    UNUSED();
    Handle o;
    double len = 0;
    if (!thisAndLength(vm, self, &o, &len))
        return JSValue::exception();
    const bool map = data != nullptr;
    JSValue mapper = JSValue::undefined();
    double depth = 1;
    if (map) {
        mapper = arg(args, argc, 0);
        if (!isFunction(mapper))
            return typeError(vm, describe(vm, mapper) + " is not a function");
    } else if (!arg(args, argc, 0).isUndefined()) {
        if (!integerOf(vm, args[0], &depth))
            return JSValue::exception();
        depth = std::max(depth, 0.0);
    }
    Handle out = Handle::adopt(arraySpeciesCreate(vm, o.get(), 0));
    if (out.get().isException())
        return out.get();
    double at = 0;
    if (!flatten(vm, out.get(), o.get(), len, &at, depth, mapper, arg(args, argc, 1)))
        return JSValue::exception();
    return out.take();
}

/// Array.prototype.toLocaleString, and the typed arrays' (data non-null): each element's own `toLocaleString`.
NATIVE(arrayToLocaleString)
{
    UNUSED();
    Handle o;
    double len = 0;
    if (!thisAndLength(vm, self, &o, &len))
        return JSValue::exception();
    Name toLocale(vm, "toLocaleString");
    String out;
    for (double k = 0; k < len; ++k) {
        if (k > 0)
            out.push_back(u',');
        Handle e;
        if (readIndex(vm, o.get(), k, &e, false) < 0)
            return JSValue::exception();
        if (e.get().isNullish())
            continue;
        Handle f = Handle::adopt(getProperty(vm, e.get(), toLocale.key));
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

/// includes, indexOf and lastIndexOf (data 0, 1, 2) over anything with a length.
NATIVE(arrayIndexOfGeneric)
{
    UNUSED();
    const int mode = int(reinterpret_cast<std::intptr_t>(data));
    Handle o;
    double len = 0;
    if (!thisAndLength(vm, self, &o, &len))
        return JSValue::exception();
    if (len == 0)
        return mode == 0 ? JSValue::boolean(false) : JSValue::int32(-1);
    const JSValue needle = arg(args, argc, 0);
    double n = 0;
    if (mode == 2) {
        if (argc > 1) {
            if (!integerOf(vm, args[1], &n))
                return JSValue::exception();
        } else {
            n = len - 1;
        }
        double k = (n >= 0 ? std::min(n, len - 1) : len + n) + 0.0;
        for (; k >= 0; --k) {
            Handle e;
            const int found = readIndex(vm, o.get(), k, &e, true);
            if (found < 0)
                return JSValue::exception();
            if (found && strictEquals(e.get(), needle))
                return JSValue::boxNumber(k);
        }
        return JSValue::int32(-1);
    }
    if (!integerOf(vm, arg(args, argc, 1), &n))
        return JSValue::exception();
    double k = (n >= 0 ? n : std::max(len + n, 0.0)) + 0.0;
    JSArray* dense = hasElems(o.get()) ? static_cast<JSArray*>(asObject(o.get())) : nullptr;
    for (; k < len; ++k) {
        if (dense && k < double(dense->length) && !dense->at(std::uint32_t(k)).isHole()) {
            const JSValue e = dense->at(std::uint32_t(k));
            if (mode == 0 ? sameValueZero(e, needle) : strictEquals(e, needle))
                return mode == 0 ? JSValue::boolean(true) : JSValue::boxNumber(k);
            continue;
        }
        Handle e;
        const int found = readIndex(vm, o.get(), k, &e, mode == 1);
        if (found < 0)
            return JSValue::exception();
        if (found && (mode == 0 ? sameValueZero(e.get(), needle) : strictEquals(e.get(), needle)))
            return mode == 0 ? JSValue::boolean(true) : JSValue::boxNumber(k);
    }
    return mode == 0 ? JSValue::boolean(false) : JSValue::int32(-1);
}

}  // namespace


JSValue arrayFillGeneric(VM& vm, JSValue self, const JSValue* args, std::uint32_t argc)
{
    Handle o;
    double len = 0, start = 0, end = 0;
    if (!thisAndLength(vm, self, &o, &len) || !relativeTo(vm, arg(args, argc, 1), len, 0, &start) ||
        !relativeTo(vm, arg(args, argc, 2), len, len, &end))
        return JSValue::exception();
    for (double k = start; k < end; ++k)
        if (!setIndex(vm, o.get(), k, arg(args, argc, 0)))
            return JSValue::exception();
    return o.take();
}

JSValue arrayCopyWithinGeneric(VM& vm, JSValue self, const JSValue* args, std::uint32_t argc)
{
    Handle o;
    double len = 0, to = 0, from = 0, final = 0;
    if (!thisAndLength(vm, self, &o, &len) || !relativeTo(vm, arg(args, argc, 0), len, 0, &to) ||
        !relativeTo(vm, arg(args, argc, 1), len, 0, &from) || !relativeTo(vm, arg(args, argc, 2), len, len, &final))
        return JSValue::exception();
    double count = std::min(final - from, len - to);
    double direction = 1;
    if (from < to && to < from + count) {
        direction = -1;
        from += count - 1;
        to += count - 1;
    }
    for (; count > 0; --count, from += direction, to += direction) {
        Handle e;
        const int found = readIndex(vm, o.get(), from, &e, true);
        if (found < 0)
            return JSValue::exception();
        if (found ? !setIndex(vm, o.get(), to, e.get()) : !deleteIndex(vm, o.get(), to))
            return JSValue::exception();
    }
    return o.take();
}

JSValue arrayReverseGeneric(VM& vm, JSValue self)
{
    Handle o;
    double len = 0;
    if (!thisAndLength(vm, self, &o, &len))
        return JSValue::exception();
    const double middle = std::floor(len / 2);
    for (double lower = 0; lower != middle; ++lower) {
        const double upper = len - lower - 1;
        Handle lowerValue, upperValue;
        const int hasLower = readIndex(vm, o.get(), lower, &lowerValue, true);
        if (hasLower < 0)
            return JSValue::exception();
        const int hasUpper = readIndex(vm, o.get(), upper, &upperValue, true);
        if (hasUpper < 0)
            return JSValue::exception();
        bool ok = true;
        if (hasLower && hasUpper)
            ok = setIndex(vm, o.get(), lower, upperValue.get()) && setIndex(vm, o.get(), upper, lowerValue.get());
        else if (hasUpper)
            ok = setIndex(vm, o.get(), lower, upperValue.get()) && deleteIndex(vm, o.get(), upper);
        else if (hasLower)
            ok = deleteIndex(vm, o.get(), lower) && setIndex(vm, o.get(), upper, lowerValue.get());
        if (!ok)
            return JSValue::exception();
    }
    return o.take();
}

JSValue arraySortGeneric(VM& vm, JSValue self, const JSValue* args, std::uint32_t argc)
{
    const JSValue compare = arg(args, argc, 0);
    if (!compare.isUndefined() && !isFunction(compare))
        return typeError(vm, "The comparison function must be either a function or undefined");
    Handle o;
    double len = 0;
    if (!thisAndLength(vm, self, &o, &len))
        return JSValue::exception();
    JSArray* temp = vm.newArray(vm.protos.array);
    Handle tempKeep = Handle::adopt(toJSValue(temp));
    for (double k = 0; k < len; ++k) {
        Handle e;
        const int found = readIndex(vm, o.get(), k, &e, true);
        if (found < 0)
            return JSValue::exception();
        if (found)
            temp->append(vm, e.take());
    }
    const std::uint32_t count = temp->length;
    JSValue r = sortDenseArray(vm, tempKeep.get(), compare);
    if (r.isException())
        return r;
    release(r);
    for (std::uint32_t j = 0; j < count; ++j) {
        if (!setIndex(vm, o.get(), double(j), temp->at(j)))
            return JSValue::exception();
    }
    for (double j = count; j < len; ++j)
        if (!deleteIndex(vm, o.get(), j))
            return JSValue::exception();
    return o.take();
}

void installArrayCallbacks(VM& vm)
{
    JSObject* ap = vm.protos.array;
    struct Entry
    {
        const char* name;
        std::intptr_t kind;
    };
    for (const Entry& e : {Entry{"forEach", 0}, Entry{"map", 1}, Entry{"filter", 2}, Entry{"some", 3}, Entry{"every", 4},
                           Entry{"find", 5}, Entry{"findIndex", 6}, Entry{"findLast", 7}, Entry{"findLastIndex", 8}})
        method(vm, ap, e.name, arrayIterate, 1, reinterpret_cast<void*>(e.kind));
    if (JSObject* ctor = isObject(vm.arrayConstructor) ? asObject(vm.arrayConstructor) : nullptr) {
        method(vm, static_cast<JSFunction*>(ctor), "from", arrayFrom, 1);
        method(vm, static_cast<JSFunction*>(ctor), "of", arrayOf, 0);
    }
    method(vm, ap, "at", arrayAtGeneric, 1);
    method(vm, ap, "toReversed", arrayToReversedGeneric, 0);
    method(vm, ap, "with", arrayWithGeneric, 2);
    method(vm, ap, "toSpliced", arrayToSplicedGeneric, 2);
    method(vm, ap, "toSorted", arrayToSortedGeneric, 1);
    method(vm, ap, "flat", arrayFlatGeneric, 0, nullptr);
    method(vm, ap, "flatMap", arrayFlatGeneric, 1, reinterpret_cast<void*>(1));
    method(vm, ap, "includes", arrayIndexOfGeneric, 1, reinterpret_cast<void*>(0));
    method(vm, ap, "indexOf", arrayIndexOfGeneric, 1, reinterpret_cast<void*>(1));
    method(vm, ap, "lastIndexOf", arrayIndexOfGeneric, 1, reinterpret_cast<void*>(2));
    method(vm, ap, "toLocaleString", arrayToLocaleString, 0);
    method(vm, ap, "reduce", arrayReduce, 1, nullptr);
    method(vm, ap, "reduceRight", arrayReduce, 1, reinterpret_cast<void*>(1));
}

}  // namespace bi
}  // namespace vm
}  // namespace internal
}  // namespace script
