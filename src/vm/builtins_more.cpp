// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// More of the standard library: the rest of Object, Reflect, and further Array, String, Number and Math functions.
#include <algorithm>
#include <cmath>
#include <limits>

#include "script/internal/dyn/numconv.hpp"
#include "script/internal/dyn/object.hpp"
#include "builtins_util.hpp"

namespace script {
namespace internal {
namespace vm {
namespace bi {

void ownKeys(VM& vm, JSObject* o, bool strings, bool symbols, bool onlyEnumerable, std::vector<Handle>& out)
{
    o->materialize(vm);
    std::vector<std::pair<std::uint32_t, Key>> indexed;
    std::vector<Key> named, syms;
    if (strings && o->cls() == ObjClass::Array) {
        auto* a = static_cast<JSArray*>(o);
        for (std::uint32_t i = 0; i < a->length; ++i)
            if (!a->elems[i].isHole())
                indexed.push_back({i, nullptr});
    }
    o->forEachOwn(vm, [&](Key k, std::uint8_t attrs, JSValue) {
        if (onlyEnumerable && !(attrs & attr::kEnumerable))
            return;
        if (k->kind == detail::CellKind::Symbol) {
            if (symbols && !static_cast<dyn::SymbolCell*>(k)->isPrivate)
                syms.push_back(k);
            return;
        }
        if (!strings)
            return;
        std::uint32_t index;
        if (arrayIndexOf(k, &index))
            indexed.push_back({index, k});
        else
            named.push_back(k);
    });
    std::stable_sort(indexed.begin(), indexed.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
    for (auto& [index, key] : indexed) {
        if (key) {
            out.push_back(Handle::retained(JSValue::cell(key)));
        } else {
            Key made = toPropertyKey(vm, JSValue::boxNumber(double(index)));
            out.push_back(Handle::adopt(JSValue::cell(made)));
        }
    }
    if (strings && !onlyEnumerable && o->cls() == ObjClass::Array)
        out.push_back(Handle::retained(JSValue::cell(vm.atoms.length)));
    for (Key k : named)
        out.push_back(Handle::retained(JSValue::cell(k)));
    for (Key k : syms)
        out.push_back(Handle::retained(JSValue::cell(k)));
}

JSValue getIndex(VM& vm, JSValue base, std::uint32_t i)
{
    if (isArray(base)) {
        const JSValue e = static_cast<JSArray*>(asObject(base))->at(i);
        if (!e.isHole()) {
            retain(e);
            return e;
        }
    }
    Key k = toPropertyKey(vm, JSValue::boxNumber(double(i)));
    if (!k)
        return JSValue::exception();
    JSValue v = getProperty(vm, base, k);
    detail::decRef(k);
    return v;
}

namespace {

/// The own property `key` of `o`, if there is one. Value, getter and setter are owned.
struct OwnDesc
{
    std::uint8_t attrs = 0;
    Handle value, getter, setter;
    bool accessor = false;
};

bool ownDescriptor(VM& vm, JSObject* o, Key key, OwnDesc& out)
{
    o->materialize(vm);
    const bool frozen = (o->h.flags & cellflags::kFrozen) != 0;
    if (o->cls() == ObjClass::Array) {
        auto* a = static_cast<JSArray*>(o);
        if (key == vm.atoms.length) {
            out.attrs = frozen ? 0 : attr::kWritable;
            out.value = Handle::adopt(JSValue::boxNumber(double(a->length)));
            return true;
        }
        std::uint32_t index;
        if (arrayIndexOf(key, &index)) {
            const JSValue e = a->at(index);
            if (e.isHole())
                return false;
            out.attrs = frozen ? attr::kEnumerable : attr::kDefault;
            out.value = Handle::retained(e);
            return true;
        }
    }
    std::uint8_t attrs = 0;
    JSValue* slot = o->findOwn(vm, key, &attrs);
    if (!slot)
        return false;
    out.attrs = attrs;
    if (attrs & attr::kAccessor) {
        auto* gs = reinterpret_cast<GetterSetter*>(slot->asCell());
        out.accessor = true;
        out.getter = Handle::retained(gs->getter);
        out.setter = Handle::retained(gs->setter);
    } else {
        out.value = Handle::retained(*slot);
    }
    return true;
}

JSValue descriptorObject(VM& vm, JSObject* o, Key key)
{
    OwnDesc d;
    if (!ownDescriptor(vm, o, key, d))
        return JSValue::undefined();
    JSObject* r = vm.newObject(vm.protos.object);
    Handle keep = Handle::adopt(toJSValue(r));
    auto put = [&](const char* name, JSValue v) {
        Key k = vm.internKey(std::string_view(name));
        retain(v);
        r->putOwn(vm, k, v, attr::kDefault);
        detail::decRef(k);
    };
    if (d.accessor) {
        put("get", d.getter.get());
        put("set", d.setter.get());
    } else {
        put("value", d.value.get());
        put("writable", JSValue::boolean((d.attrs & attr::kWritable) != 0));
    }
    put("enumerable", JSValue::boolean((d.attrs & attr::kEnumerable) != 0));
    put("configurable", JSValue::boolean((d.attrs & attr::kConfigurable) != 0));
    return keep.take();
}

JSObject* objectArg(VM& vm, JSValue v, const char* who)
{
    if (isObject(v))
        return asObject(v);
    typeError(vm, std::string(who) + " called on non-object");
    return nullptr;
}

/// A key from any value (an owned count), converting objects first.
Key keyArg(VM& vm, JSValue v)
{
    JSValue prim = toPrimitive(vm, v, true);
    if (prim.isException())
        return nullptr;
    Key k = toPropertyKey(vm, prim);
    release(prim);
    return k;
}

JSValue keysArray(VM& vm, std::vector<Handle>& keys)
{
    JSArray* out = vm.newArray(vm.protos.array, 0, std::uint32_t(keys.size()));
    Handle keep = Handle::adopt(toJSValue(out));
    for (Handle& k : keys)
        out->append(vm, k.get()), retain(k.get());
    return keep.take();
}

bool sameValue(JSValue a, JSValue b)
{
    if (a.isNumber() && b.isNumber()) {
        const double x = a.asNumber(), y = b.asNumber();
        if (x != x && y != y)
            return true;
        return x == y && std::signbit(x) == std::signbit(y);
    }
    return strictEquals(a, b);
}

NATIVE(objectGetOwnPropertyNames)
{
    UNUSED();
    const JSValue v = arg(args, argc, 0);
    if (v.isNullish())
        return typeError(vm, "Cannot convert undefined or null to object");
    std::vector<Handle> keys;
    const int mode = int(reinterpret_cast<std::intptr_t>(data));  // 0 names, 1 symbols
    if (isObject(v)) {
        ownKeys(vm, asObject(v), mode == 0, mode == 1, false, keys);
    } else if (isString(v) && mode == 0) {
        for (std::uint32_t i = 0; i < asString(v)->length; ++i)
            keys.push_back(Handle::adopt(toStringValue(vm, JSValue::boxNumber(double(i)))));
        keys.push_back(Handle::retained(JSValue::cell(vm.atoms.length)));
    }
    return keysArray(vm, keys);
}

NATIVE(objectGetOwnPropertyDescriptor)
{
    UNUSED();
    JSObject* o = objectArg(vm, arg(args, argc, 0), "Object.getOwnPropertyDescriptor");
    if (!o)
        return JSValue::exception();
    Key k = keyArg(vm, arg(args, argc, 1));
    if (!k)
        return JSValue::exception();
    JSValue d = descriptorObject(vm, o, k);
    detail::decRef(k);
    return d;
}

NATIVE(objectGetOwnPropertyDescriptors)
{
    UNUSED();
    JSObject* o = objectArg(vm, arg(args, argc, 0), "Object.getOwnPropertyDescriptors");
    if (!o)
        return JSValue::exception();
    JSObject* out = vm.newObject(vm.protos.object);
    Handle keep = Handle::adopt(toJSValue(out));
    std::vector<Handle> keys;
    ownKeys(vm, o, true, true, false, keys);
    for (Handle& k : keys) {
        Key key = static_cast<Key>(k.get().asCell());
        JSValue d = descriptorObject(vm, o, key);
        if (!d.isUndefined()) {
            retain(JSValue::cell(key));
            out->putOwn(vm, key, d, attr::kDefault);
            detail::decRef(key);
        }
    }
    return keep.take();
}

NATIVE(objectDefineProperties)
{
    UNUSED();
    JSObject* o = objectArg(vm, arg(args, argc, 0), "Object.defineProperties");
    if (!o)
        return JSValue::exception();
    const JSValue props = arg(args, argc, 1);
    if (!isObject(props))
        return typeError(vm, "Property description must be an object");
    std::vector<Handle> keys;
    ownKeys(vm, asObject(props), true, true, true, keys);
    for (Handle& k : keys) {
        Key key = static_cast<Key>(k.get().asCell());
        JSValue d = getProperty(vm, props, key);
        if (d.isException())
            return d;
        Handle dk = Handle::adopt(d);
        if (!isObject(d))
            return typeError(vm, "Property description must be an object");
        JSValue r = definePropertyFromDescriptor(vm, o, key, asObject(d));
        if (r.isException())
            return r;
    }
    retain(arg(args, argc, 0));
    return arg(args, argc, 0);
}

NATIVE(objectFromEntries)
{
    UNUSED();
    JSObject* out = vm.newObject(vm.protos.object);
    Handle keep = Handle::adopt(toJSValue(out));
    const bool ok = forEachIterated(vm, arg(args, argc, 0), [&](JSValue entry) {
        if (!isObject(entry)) {
            typeError(vm, "Iterator value " + describe(vm, entry) + " is not an entry object");
            return Step::Fail;
        }
        JSValue k = getIndex(vm, entry, 0);
        if (k.isException())
            return Step::Fail;
        Handle kk = Handle::adopt(k);
        JSValue v = getIndex(vm, entry, 1);
        if (v.isException())
            return Step::Fail;
        Key key = keyArg(vm, k);
        if (!key) {
            release(v);
            return Step::Fail;
        }
        out->putOwn(vm, key, v, attr::kDefault);
        detail::decRef(key);
        return Step::Continue;
    });
    if (!ok)
        return JSValue::exception();
    return keep.take();
}

NATIVE(objectIs)
{
    UNUSED();
    return JSValue::boolean(sameValue(arg(args, argc, 0), arg(args, argc, 1)));
}

NATIVE(objectExtensibility)
{
    UNUSED();
    const int mode = int(reinterpret_cast<std::intptr_t>(data));  // 0 isExtensible, 1 preventExtensions, 2 seal, 3 isSealed
    const JSValue v = arg(args, argc, 0);
    if (!isObject(v))
        return mode == 0 ? JSValue::boolean(false) : mode == 3 ? JSValue::boolean(true) : (retain(v), v);
    JSObject* o = asObject(v);
    if (mode == 0)
        return JSValue::boolean(!(o->h.flags & cellflags::kNotExtensible));
    if (mode == 1) {
        o->h.flags |= cellflags::kNotExtensible;
        retain(v);
        return v;
    }
    o->materialize(vm);
    std::vector<Key> keys;
    o->forEachOwn(vm, [&](Key k, std::uint8_t, JSValue) { keys.push_back(k); });
    if (mode == 2) {
        o->h.flags |= cellflags::kNotExtensible;
        for (Key k : keys) {
            std::uint8_t attrs = 0;
            o->findOwn(vm, k, &attrs);
            o->setAttrs(vm, k, std::uint8_t(attrs & ~attr::kConfigurable));
        }
        retain(v);
        return v;
    }
    bool sealed = (o->h.flags & cellflags::kNotExtensible) != 0;
    for (Key k : keys) {
        std::uint8_t attrs = 0;
        o->findOwn(vm, k, &attrs);
        sealed = sealed && !(attrs & attr::kConfigurable);
    }
    if (o->cls() == ObjClass::Array && static_cast<JSArray*>(o)->length && !(o->h.flags & cellflags::kFrozen))
        sealed = false;
    return JSValue::boolean(sealed);
}

NATIVE(objectHasOwn)
{
    UNUSED();
    const JSValue v = arg(args, argc, 0);
    if (v.isNullish())
        return typeError(vm, "Cannot convert undefined or null to object");
    Key k = keyArg(vm, arg(args, argc, 1));
    if (!k)
        return JSValue::exception();
    bool has = false;
    if (isObject(v)) {
        OwnDesc d;
        has = ownDescriptor(vm, asObject(v), k, d);
    } else if (isString(v)) {
        std::uint32_t index;
        has = k == vm.atoms.length || (arrayIndexOf(k, &index) && index < asString(v)->length);
    }
    detail::decRef(k);
    return JSValue::boolean(has);
}

NATIVE(objectPropertyIsEnumerable)
{
    UNUSED();
    Key k = keyArg(vm, arg(args, argc, 0));
    if (!k)
        return JSValue::exception();
    bool result = false;
    if (isObject(self)) {
        OwnDesc d;
        result = ownDescriptor(vm, asObject(self), k, d) && (d.attrs & attr::kEnumerable);
    }
    detail::decRef(k);
    return JSValue::boolean(result);
}

NATIVE(objectToLocaleString)
{
    UNUSED();
    Key k = vm.internKey(std::string_view("toString"));
    JSValue f = getProperty(vm, self, k);
    detail::decRef(k);
    if (f.isException())
        return f;
    Handle keep = Handle::adopt(f);
    return vm.call(f, self, nullptr, 0);
}

NATIVE(protoGetter)
{
    UNUSED();
    if (self.isNullish())
        return typeError(vm, "Cannot convert undefined or null to object");
    JSObject* p = nullptr;
    if (isObject(self))
        p = asObject(self)->proto(vm);
    else if (self.isNumber())
        p = vm.protos.number;
    else if (isString(self))
        p = vm.protos.string;
    else if (self.isBool())
        p = vm.protos.boolean;
    if (!p)
        return JSValue::null();
    retain(toJSValue(p));
    return toJSValue(p);
}

NATIVE(protoSetter)
{
    UNUSED();
    const JSValue p = arg(args, argc, 0);
    if (!isObject(self) || !(isObject(p) || p.isNull()))
        return JSValue::undefined();
    if (p.isNull()) {
        asObject(self)->setProto(vm, nullptr);
    } else {
        if (asObject(self)->wouldCycle(vm, asObject(p)))
            return typeError(vm, "Cyclic __proto__ value");
        asObject(self)->setProto(vm, asObject(p));
    }
    return JSValue::undefined();
}

// ------------------------------------------------------------------ Reflect ---

bool listFromArrayLike(VM& vm, JSValue list, std::vector<Handle>& out)
{
    if (!isObject(list)) {
        typeError(vm, "CreateListFromArrayLike called on non-object");
        return false;
    }
    if (isArray(list)) {
        auto* a = static_cast<JSArray*>(asObject(list));
        for (std::uint32_t i = 0; i < a->length; ++i)
            out.push_back(Handle::retained(a->at(i).isHole() ? JSValue::undefined() : a->at(i)));
        return true;
    }
    JSValue len = getProperty(vm, list, vm.atoms.length);
    if (len.isException())
        return false;
    double d = 0;
    const bool ok = toNumber(vm, len, &d);
    release(len);
    if (!ok)
        return false;
    for (std::uint32_t i = 0; i < std::uint32_t(std::max(0.0, d)); ++i) {
        JSValue e = getIndex(vm, list, i);
        if (e.isException())
            return false;
        out.push_back(Handle::adopt(e));
    }
    return true;
}

std::vector<JSValue> raw(const std::vector<Handle>& hs)
{
    std::vector<JSValue> v;
    for (const Handle& h : hs)
        v.push_back(h.get());
    return v;
}

NATIVE(reflectApply)
{
    UNUSED();
    if (!isFunction(arg(args, argc, 0)))
        return typeError(vm, describe(vm, arg(args, argc, 0)) + " is not a function");
    std::vector<Handle> list;
    if (!listFromArrayLike(vm, arg(args, argc, 2), list))
        return JSValue::exception();
    std::vector<JSValue> values = raw(list);
    return vm.call(arg(args, argc, 0), arg(args, argc, 1), values.data(), std::uint32_t(values.size()));
}

NATIVE(reflectConstruct)
{
    UNUSED();
    std::vector<Handle> list;
    if (!listFromArrayLike(vm, arg(args, argc, 1), list))
        return JSValue::exception();
    std::vector<JSValue> values = raw(list);
    return vm.construct(arg(args, argc, 0), values.data(), std::uint32_t(values.size()));
}

NATIVE(reflectOp)
{
    UNUSED();
    const int mode = int(reinterpret_cast<std::intptr_t>(data));
    // 0 get, 1 set, 2 has, 3 deleteProperty, 4 defineProperty, 5 getOwnPropertyDescriptor
    JSObject* o = objectArg(vm, arg(args, argc, 0), "Reflect operation");
    if (!o)
        return JSValue::exception();
    Key k = keyArg(vm, arg(args, argc, 1));
    if (!k)
        return JSValue::exception();
    JSValue result;
    switch (mode) {
        case 0: {
            const JSValue receiver = argc > 2 ? args[2] : args[0];
            result = getPropertyFrom(vm, o, receiver, k);
            break;
        }
        case 1: result = JSValue::boolean(setProperty(vm, args[0], k, arg(args, argc, 2), false)); break;
        case 2: result = JSValue::boolean(hasProperty(vm, args[0], k)); break;
        case 3: result = JSValue::boolean(deleteProperty(vm, args[0], k, false)); break;
        case 4: {
            if (!isObject(arg(args, argc, 2))) {
                result = typeError(vm, "Property description must be an object");
                break;
            }
            JSValue r = definePropertyFromDescriptor(vm, o, k, asObject(args[2]));
            if (r.isException()) {
                vm.clearException();
                result = JSValue::boolean(false);
            } else {
                result = JSValue::boolean(true);
            }
            break;
        }
        default: result = descriptorObject(vm, o, k); break;
    }
    detail::decRef(k);
    return result;
}

NATIVE(reflectOwnKeys)
{
    UNUSED();
    JSObject* o = objectArg(vm, arg(args, argc, 0), "Reflect.ownKeys");
    if (!o)
        return JSValue::exception();
    std::vector<Handle> keys;
    ownKeys(vm, o, true, true, false, keys);
    return keysArray(vm, keys);
}

NATIVE(reflectProto)
{
    UNUSED();
    JSObject* o = objectArg(vm, arg(args, argc, 0), "Reflect.getPrototypeOf");
    if (!o)
        return JSValue::exception();
    if (!data) {
        JSObject* p = o->proto(vm);
        if (!p)
            return JSValue::null();
        retain(toJSValue(p));
        return toJSValue(p);
    }
    const JSValue p = arg(args, argc, 1);
    if (!(isObject(p) || p.isNull()))
        return typeError(vm, "Object prototype may only be an Object or null");
    if (p.isNull()) {
        o->setProto(vm, nullptr);
        return JSValue::boolean(true);
    }
    if (o->wouldCycle(vm, asObject(p)) || (o->h.flags & cellflags::kNotExtensible))
        return JSValue::boolean(false);
    o->setProto(vm, asObject(p));
    return JSValue::boolean(true);
}

// -------------------------------------------------------------------- Array ---

double toIntegerOr(VM& vm, JSValue v, double fallback, bool* ok)
{
    *ok = true;
    if (v.isUndefined())
        return fallback;
    double d;
    if (!toNumber(vm, v, &d)) {
        *ok = false;
        return 0;
    }
    return d != d ? 0 : std::trunc(d);
}

NATIVE(arrayFrom)
{
    UNUSED();
    const JSValue items = arg(args, argc, 0), mapFn = arg(args, argc, 1), thisArg = arg(args, argc, 2);
    if (!mapFn.isUndefined() && !isFunction(mapFn))
        return typeError(vm, describe(vm, mapFn) + " is not a function");
    if (items.isNullish())
        return typeError(vm, describe(vm, items) + " is not iterable");
    JSArray* out = vm.newArray(vm.protos.array);
    Handle keep = Handle::adopt(toJSValue(out));
    std::uint32_t index = 0;
    auto add = [&](JSValue v) -> Step {
        if (!mapFn.isUndefined()) {
            JSValue callArgs[2] = {v, JSValue::int32(std::int32_t(index))};
            JSValue r = vm.call(mapFn, thisArg, callArgs, 2);
            if (r.isException())
                return Step::Fail;
            out->append(vm, r);
        } else {
            retain(v);
            out->append(vm, v);
        }
        ++index;
        return Step::Continue;
    };
    JSValue method = getProperty(vm, items, vm.atoms.symIterator);
    if (method.isException())
        return method;
    const bool iterable = isFunction(method);
    release(method);
    if (iterable) {
        if (!forEachIterated(vm, items, add))
            return JSValue::exception();
        return keep.take();
    }
    std::vector<Handle> list;
    if (isString(items)) {
        for (std::uint32_t i = 0; i < asString(items)->length; ++i)
            list.push_back(Handle::adopt(makeString(vm, asString(items)->text().substr(i, 1))));
    } else if (isObject(items)) {
        if (!listFromArrayLike(vm, items, list))
            return JSValue::exception();
    }
    for (Handle& h : list)
        if (add(h.get()) == Step::Fail)
            return JSValue::exception();
    return keep.take();
}

NATIVE(arrayOf)
{
    UNUSED();
    JSArray* out = vm.newArray(vm.protos.array, 0, argc);
    Handle keep = Handle::adopt(toJSValue(out));
    for (std::uint32_t i = 0; i < argc; ++i) {
        retain(args[i]);
        out->append(vm, args[i]);
    }
    return keep.take();
}

NATIVE(arrayAt)
{
    UNUSED();
    if (!isArray(self))
        return typeError(vm, "this is not an array");
    auto* a = static_cast<JSArray*>(asObject(self));
    bool ok;
    double i = toIntegerOr(vm, arg(args, argc, 0), 0, &ok);
    if (!ok)
        return JSValue::exception();
    if (i < 0)
        i += a->length;
    if (i < 0 || i >= a->length || a->at(std::uint32_t(i)).isHole())
        return JSValue::undefined();
    retain(a->at(std::uint32_t(i)));
    return a->at(std::uint32_t(i));
}

bool flattenInto(VM& vm, JSArray* out, JSValue source, double depth, JSValue mapFn, JSValue thisArg, bool mapTop)
{
    Handle keep = Handle::retained(source);
    auto* a = static_cast<JSArray*>(asObject(source));
    for (std::uint32_t i = 0; i < a->length; ++i) {
        if (a->at(i).isHole())
            continue;
        Handle e = Handle::retained(a->at(i));
        if (mapTop) {
            JSValue callArgs[3] = {e.get(), JSValue::int32(std::int32_t(i)), source};
            JSValue r = vm.call(mapFn, thisArg, callArgs, 3);
            if (r.isException())
                return false;
            e = Handle::adopt(r);
        }
        if (isArray(e.get()) && (mapTop ? depth >= 1 : depth > 0)) {
            if (!flattenInto(vm, out, e.get(), mapTop ? depth - 1 : depth - 1, JSValue::undefined(), JSValue::undefined(), false))
                return false;
        } else {
            retain(e.get());
            out->append(vm, e.get());
        }
    }
    return true;
}

NATIVE(arrayFlat)
{
    UNUSED();
    if (!isArray(self))
        return typeError(vm, "this is not an array");
    const bool map = data != nullptr;
    JSValue mapFn = JSValue::undefined();
    double depth = 1;
    if (map) {
        mapFn = arg(args, argc, 0);
        if (!isFunction(mapFn))
            return typeError(vm, describe(vm, mapFn) + " is not a function");
    } else {
        bool ok;
        depth = toIntegerOr(vm, arg(args, argc, 0), 1, &ok);
        if (!ok)
            return JSValue::exception();
    }
    JSArray* out = vm.newArray(vm.protos.array);
    Handle keep = Handle::adopt(toJSValue(out));
    if (!flattenInto(vm, out, self, depth, mapFn, arg(args, argc, 1), map))
        return JSValue::exception();
    return keep.take();
}

NATIVE(arrayFindLast)
{
    UNUSED();
    if (!isArray(self))
        return typeError(vm, "this is not an array");
    const JSValue callback = arg(args, argc, 0);
    if (!isFunction(callback))
        return typeError(vm, describe(vm, callback) + " is not a function");
    Handle keep = Handle::retained(self);
    auto* a = static_cast<JSArray*>(asObject(self));
    for (std::uint32_t n = a->length; n-- > 0;) {
        if (n >= a->length)
            continue;
        Handle e = Handle::retained(a->at(n).isHole() ? JSValue::undefined() : a->at(n));
        JSValue callArgs[3] = {e.get(), JSValue::int32(std::int32_t(n)), self};
        JSValue r = vm.call(callback, arg(args, argc, 1), callArgs, 3);
        if (r.isException())
            return r;
        const bool hit = toBoolean(r);
        release(r);
        if (hit)
            return data ? JSValue::int32(std::int32_t(n)) : e.take();
    }
    return data ? JSValue::int32(-1) : JSValue::undefined();
}

// ------------------------------------------------------------------- String ---

NATIVE(stringAt)
{
    UNUSED();
    if (self.isNullish())
        return typeError(vm, "String.prototype.at called on null or undefined");
    Handle text = Handle::adopt(toStringValue(vm, self));
    if (text.get().isException())
        return JSValue::exception();
    bool ok;
    double i = toIntegerOr(vm, arg(args, argc, 0), 0, &ok);
    if (!ok)
        return JSValue::exception();
    const StringView v = asString(text.get())->text();
    if (i < 0)
        i += double(v.size());
    if (i < 0 || i >= double(v.size()))
        return JSValue::undefined();
    return makeString(vm, v.substr(std::size_t(i), 1));
}

NATIVE(stringCodePointAt)
{
    UNUSED();
    if (self.isNullish())
        return typeError(vm, "String.prototype.codePointAt called on null or undefined");
    Handle text = Handle::adopt(toStringValue(vm, self));
    if (text.get().isException())
        return JSValue::exception();
    bool ok;
    const double i = toIntegerOr(vm, arg(args, argc, 0), 0, &ok);
    if (!ok)
        return JSValue::exception();
    const StringView v = asString(text.get())->text();
    if (i < 0 || i >= double(v.size()))
        return JSValue::undefined();
    const std::size_t at = std::size_t(i);
    std::uint32_t c = v[at];
    if (c >= 0xd800 && c < 0xdc00 && at + 1 < v.size() && v[at + 1] >= 0xdc00 && v[at + 1] < 0xe000)
        c = 0x10000 + ((c - 0xd800) << 10) + (v[at + 1] - 0xdc00);
    return JSValue::int32(std::int32_t(c));
}

NATIVE(stringFromCodePoint)
{
    UNUSED();
    String out;
    for (std::uint32_t i = 0; i < argc; ++i) {
        double d;
        if (!toNumber(vm, args[i], &d))
            return JSValue::exception();
        if (d != std::trunc(d) || d < 0 || d > 0x10ffff)
            return vm.throwError(ErrorCode::RangeError, "Invalid code point " + toUtf8(StringView(script::internal::numberToString(d))));
        const std::uint32_t c = std::uint32_t(d);
        if (c >= 0x10000) {
            out.push_back(char16_t(0xd800 + ((c - 0x10000) >> 10)));
            out.push_back(char16_t(0xdc00 + ((c - 0x10000) & 0x3ff)));
        } else {
            out.push_back(char16_t(c));
        }
    }
    return makeString(vm, StringView(out));
}

NATIVE(stringIdentity)
{
    UNUSED();
    if (self.isNullish())
        return typeError(vm, "String.prototype method called on null or undefined");
    return toStringValue(vm, self);
}

// ------------------------------------------------------------------- Number ---

NATIVE(numberFormat)
{
    UNUSED();
    if (!self.isNumber())
        return typeError(vm, "Number.prototype method requires that 'this' be a Number");
    const double d = self.asNumber();
    const int mode = int(reinterpret_cast<std::intptr_t>(data));  // 0 toPrecision, 1 toExponential
    const JSValue a = arg(args, argc, 0);
    if (mode == 0 && a.isUndefined())
        return makeString(vm, StringView(script::internal::numberToString(d)));
    bool ok;
    const double digits = toIntegerOr(vm, a, 0, &ok);
    if (!ok)
        return JSValue::exception();
    if (!std::isfinite(d))
        return makeString(vm, StringView(script::internal::numberToString(d)));
    if (mode == 0) {
        if (digits < 1 || digits > 100)
            return vm.throwError(ErrorCode::RangeError, "toPrecision() argument must be between 1 and 100");
        return makeString(vm, StringView(dyn::numberToPrecision(d, int(digits))));
    }
    if (digits < 0 || digits > 100)
        return vm.throwError(ErrorCode::RangeError, "toExponential() argument must be between 0 and 100");
    if (a.isUndefined()) {
        // As many digits as it takes for the text to read back as the same number.
        for (int n = 0; n <= 20; ++n) {
            const String text = dyn::numberToExponential(d, n);
            if (dyn::stringToNumber(StringView(text)) == d || n == 20)
                return makeString(vm, StringView(text));
        }
    }
    return makeString(vm, StringView(dyn::numberToExponential(d, int(digits))));
}

// --------------------------------------------------------------------- Math ---

NATIVE(mathFn)
{
    UNUSED();
    double x;
    if (!toNumber(vm, arg(args, argc, 0), &x))
        return JSValue::exception();
    auto fn = reinterpret_cast<double (*)(double)>(data);
    return JSValue::boxNumber(fn(x));
}

double fround(double x) { return double(float(x)); }
double clz32(double x)
{
    const std::uint32_t v = toUint32(x);
    return v ? double(__builtin_clz(v)) : 32.0;
}

// ------------------------------------------------------------------- BigInt ---

NATIVE(bigintCtor)
{
    UNUSED();
    if (!vm.newTarget.isUndefined())
        return typeError(vm, "BigInt is not a constructor");
    JSValue prim = toPrimitive(vm, arg(args, argc, 0), false);
    if (prim.isException())
        return prim;
    Handle keep = Handle::adopt(prim);
    if (isBigInt(prim)) {
        retain(prim);
        return prim;
    }
    if (prim.isNumber()) {
        const double d = prim.asNumber();
        if (!std::isfinite(d) || d != std::trunc(d))
            return vm.throwError(ErrorCode::RangeError, "The number " + toUtf8(StringView(script::internal::numberToString(d))) +
                                                              " cannot be converted to a BigInt because it is not an integer");
        return bigintFromDouble(vm, d);
    }
    if (prim.isBool())
        return bigintFromDouble(vm, prim == JSValue::boolean(true) ? 1 : 0);
    if (isString(prim)) {
        bool ok;
        JSValue r = bigintFromText(vm, asString(prim)->text(), &ok);
        if (!ok) {
            if (r.isException())
                return r;
            return vm.throwError(ErrorCode::SyntaxError, "Cannot convert " + toUtf8(asString(prim)->text()) + " to a BigInt");
        }
        return r;
    }
    return typeError(vm, "Cannot convert " + describe(vm, prim) + " to a BigInt");
}

NATIVE(bigintAsNFn)
{
    UNUSED();
    const bool isSigned = data == nullptr;
    bool ok;
    const double bits = toIntegerOr(vm, arg(args, argc, 0), 0, &ok);
    if (!ok)
        return JSValue::exception();
    if (bits < 0 || bits > 9007199254740991.0)
        return vm.throwError(ErrorCode::RangeError, "Invalid value: not (convertible to) a safe integer");
    const JSValue v = arg(args, argc, 1);
    if (!isBigInt(v))
        return typeError(vm, "Cannot convert " + describe(vm, v) + " to a BigInt");
    return bigintAsN(vm, std::uint64_t(bits), v, isSigned);
}

NATIVE(bigintToStringFn)
{
    UNUSED();
    if (!isBigInt(self))
        return typeError(vm, "BigInt.prototype.toString requires that 'this' be a BigInt");
    bool ok;
    const double radix = toIntegerOr(vm, arg(args, argc, 0), 10, &ok);
    if (!ok)
        return JSValue::exception();
    if (radix < 2 || radix > 36)
        return vm.throwError(ErrorCode::RangeError, "toString() radix must be between 2 and 36");
    return bigintToString(vm, self, int(radix));
}

NATIVE(bigintValueOfFn)
{
    UNUSED();
    if (!isBigInt(self))
        return typeError(vm, "BigInt.prototype.valueOf requires that 'this' be a BigInt");
    retain(self);
    return self;
}

}  // namespace

void installMore(VM& vm)
{
    JSObject* objectProto = vm.protos.object;
    Key objectKey = vm.internKey(std::string_view("Object"));
    JSValue* objectSlot = vm.global->findOwn(vm, objectKey);
    detail::decRef(objectKey);
    auto* objectCtorFn = static_cast<JSFunction*>(asObject(*objectSlot));

    method(vm, objectCtorFn, "getOwnPropertyNames", objectGetOwnPropertyNames, 1, reinterpret_cast<void*>(0));
    method(vm, objectCtorFn, "getOwnPropertySymbols", objectGetOwnPropertyNames, 1, reinterpret_cast<void*>(1));
    method(vm, objectCtorFn, "getOwnPropertyDescriptor", objectGetOwnPropertyDescriptor, 2);
    method(vm, objectCtorFn, "getOwnPropertyDescriptors", objectGetOwnPropertyDescriptors, 1);
    method(vm, objectCtorFn, "defineProperties", objectDefineProperties, 2);
    method(vm, objectCtorFn, "fromEntries", objectFromEntries, 1);
    method(vm, objectCtorFn, "is", objectIs, 2);
    method(vm, objectCtorFn, "hasOwn", objectHasOwn, 2);
    method(vm, objectCtorFn, "isExtensible", objectExtensibility, 1, reinterpret_cast<void*>(0));
    method(vm, objectCtorFn, "preventExtensions", objectExtensibility, 1, reinterpret_cast<void*>(1));
    method(vm, objectCtorFn, "seal", objectExtensibility, 1, reinterpret_cast<void*>(2));
    method(vm, objectCtorFn, "isSealed", objectExtensibility, 1, reinterpret_cast<void*>(3));
    method(vm, objectProto, "propertyIsEnumerable", objectPropertyIsEnumerable, 1);
    method(vm, objectProto, "toLocaleString", objectToLocaleString, 0);
    {
        Key key = vm.internKey(std::string_view("__proto__"));
        Key gname = vm.internKey(std::string_view("get __proto__"));
        Key sname = vm.internKey(std::string_view("set __proto__"));
        JSFunction* g = vm.newNativeFunction(vm.protos.function, protoGetter, nullptr, gname, 0);
        Handle gk = Handle::adopt(toJSValue(g));
        JSFunction* st = vm.newNativeFunction(vm.protos.function, protoSetter, nullptr, sname, 1);
        Handle sk = Handle::adopt(toJSValue(st));
        defineAccessor(vm, objectProto, key, toJSValue(g), toJSValue(st), attr::kConfigurable);
        detail::decRef(key);
        detail::decRef(gname);
        detail::decRef(sname);
    }

    // Reflect.
    JSObject* reflect = vm.newObject(objectProto);
    defineGlobal(vm, "Reflect", toJSValue(reflect));
    method(vm, reflect, "apply", reflectApply, 3);
    method(vm, reflect, "construct", reflectConstruct, 2);
    method(vm, reflect, "get", reflectOp, 2, reinterpret_cast<void*>(0));
    method(vm, reflect, "set", reflectOp, 3, reinterpret_cast<void*>(1));
    method(vm, reflect, "has", reflectOp, 2, reinterpret_cast<void*>(2));
    method(vm, reflect, "deleteProperty", reflectOp, 2, reinterpret_cast<void*>(3));
    method(vm, reflect, "defineProperty", reflectOp, 3, reinterpret_cast<void*>(4));
    method(vm, reflect, "getOwnPropertyDescriptor", reflectOp, 2, reinterpret_cast<void*>(5));
    method(vm, reflect, "ownKeys", reflectOwnKeys, 1);
    method(vm, reflect, "getPrototypeOf", reflectProto, 1, nullptr);
    method(vm, reflect, "setPrototypeOf", reflectProto, 2, reinterpret_cast<void*>(1));
    method(vm, reflect, "isExtensible", objectExtensibility, 1, reinterpret_cast<void*>(0));
    method(vm, reflect, "preventExtensions", objectExtensibility, 1, reinterpret_cast<void*>(1));

    // Array.
    Key arrayKey = vm.internKey(std::string_view("Array"));
    auto* arrayCtorFn = static_cast<JSFunction*>(asObject(*vm.global->findOwn(vm, arrayKey)));
    detail::decRef(arrayKey);
    method(vm, arrayCtorFn, "from", arrayFrom, 1);
    method(vm, arrayCtorFn, "of", arrayOf, 0);
    JSObject* ap = vm.protos.array;
    method(vm, ap, "at", arrayAt, 1);
    method(vm, ap, "flat", arrayFlat, 0, nullptr);
    method(vm, ap, "flatMap", arrayFlat, 1, reinterpret_cast<void*>(1));
    method(vm, ap, "findLast", arrayFindLast, 1, nullptr);
    method(vm, ap, "findLastIndex", arrayFindLast, 1, reinterpret_cast<void*>(1));

    // String.
    Key stringKey = vm.internKey(std::string_view("String"));
    auto* stringCtorFn = static_cast<JSFunction*>(asObject(*vm.global->findOwn(vm, stringKey)));
    detail::decRef(stringKey);
    method(vm, stringCtorFn, "fromCodePoint", stringFromCodePoint, 1);
    JSObject* sp = vm.protos.string;
    method(vm, sp, "at", stringAt, 1);
    method(vm, sp, "codePointAt", stringCodePointAt, 1);
    method(vm, sp, "normalize", stringIdentity, 0);
    method(vm, sp, "toLocaleLowerCase", [](VM& vm2, JSValue self, const JSValue*, std::uint32_t, void*) -> JSValue {
        JSValue t = toStringValue(vm2, self);
        if (t.isException())
            return t;
        Handle keep = Handle::adopt(t);
        Key k = vm2.internKey(std::string_view("toLowerCase"));
        JSValue f = getProperty(vm2, t, k);
        detail::decRef(k);
        Handle fk = Handle::adopt(f);
        return vm2.call(f, t, nullptr, 0);
    }, 0);
    method(vm, sp, "toLocaleUpperCase", [](VM& vm2, JSValue self, const JSValue*, std::uint32_t, void*) -> JSValue {
        JSValue t = toStringValue(vm2, self);
        if (t.isException())
            return t;
        Handle keep = Handle::adopt(t);
        Key k = vm2.internKey(std::string_view("toUpperCase"));
        JSValue f = getProperty(vm2, t, k);
        detail::decRef(k);
        Handle fk = Handle::adopt(f);
        return vm2.call(f, t, nullptr, 0);
    }, 0);
    for (const auto& alias : {std::pair<const char*, const char*>{"trimLeft", "trimStart"}, {"trimRight", "trimEnd"}}) {
        Key from = vm.internKey(std::string_view(alias.second));
        Key to = vm.internKey(std::string_view(alias.first));
        if (JSValue* f = sp->findOwn(vm, from)) {
            retain(*f);
            sp->putOwn(vm, to, *f, attr::kWritable | attr::kConfigurable);
        }
        detail::decRef(from);
        detail::decRef(to);
    }

    // Number.
    JSObject* np = vm.protos.number;
    method(vm, np, "toPrecision", numberFormat, 1, reinterpret_cast<void*>(0));
    method(vm, np, "toExponential", numberFormat, 1, reinterpret_cast<void*>(1));
    {
        Key from = vm.internKey(std::string_view("toString"));
        Key to = vm.internKey(std::string_view("toLocaleString"));
        if (JSValue* f = np->findOwn(vm, from)) {
            retain(*f);
            np->putOwn(vm, to, *f, attr::kWritable | attr::kConfigurable);
        }
        detail::decRef(from);
        detail::decRef(to);
    }

    // BigInt.
    {
        JSObject* bp = vm.newObject(objectProto);
        vm.setIntrinsic(&vm.protos.bigint, bp);
        release(toJSValue(bp));
        JSFunction* bigCtor = constructor(vm, "BigInt", bigintCtor, 1, bp);
        bigCtor->flags &= ~JSFunction::kConstructor;
        method(vm, bigCtor, "asIntN", bigintAsNFn, 2, nullptr);
        method(vm, bigCtor, "asUintN", bigintAsNFn, 2, reinterpret_cast<void*>(1));
        method(vm, bp, "toString", bigintToStringFn, 0);
        method(vm, bp, "toLocaleString", bigintToStringFn, 0);
        method(vm, bp, "valueOf", bigintValueOfFn, 0);
        bp->putOwn(vm, vm.atoms.symToStringTag, makeString(vm, std::string_view("BigInt")), attr::kConfigurable);
    }
    // Math.
    Key mathKey = vm.internKey(std::string_view("Math"));
    JSObject* math = asObject(*vm.global->findOwn(vm, mathKey));
    detail::decRef(mathKey);
    struct Unary
    {
        const char* name;
        double (*fn)(double);
    };
    for (Unary u : {Unary{"asin", [](double x) { return std::asin(x); }}, Unary{"acos", [](double x) { return std::acos(x); }},
                    Unary{"sinh", [](double x) { return std::sinh(x); }}, Unary{"cosh", [](double x) { return std::cosh(x); }},
                    Unary{"tanh", [](double x) { return std::tanh(x); }}, Unary{"asinh", [](double x) { return std::asinh(x); }},
                    Unary{"acosh", [](double x) { return std::acosh(x); }}, Unary{"atanh", [](double x) { return std::atanh(x); }},
                    Unary{"expm1", [](double x) { return std::expm1(x); }}, Unary{"log1p", [](double x) { return std::log1p(x); }},
                    Unary{"fround", fround}, Unary{"clz32", clz32}})
        method(vm, math, u.name, mathFn, 1, reinterpret_cast<void*>(u.fn));
    value(vm, math, "LOG2E", JSValue::boxDouble(1.4426950408889634), 0);
    value(vm, math, "LOG10E", JSValue::boxDouble(0.4342944819032518), 0);
    value(vm, math, "SQRT1_2", JSValue::boxDouble(0.7071067811865476), 0);
    math->putOwn(vm, vm.atoms.symToStringTag, makeString(vm, std::string_view("Math")), attr::kConfigurable);
}

}  // namespace bi
}  // namespace vm
}  // namespace internal
}  // namespace script
