// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// More of the standard library: the rest of Object, Reflect, and further Array, String, Number and Math functions.
#include <algorithm>
#include <cmath>
#include <limits>

#include "script/internal/dyn/numconv.hpp"
#include "script/internal/dyn/object.hpp"
#include "script/internal/vm/typed.hpp"
#include "builtins_util.hpp"

namespace script {
namespace internal {
namespace vm {
namespace bi {

bool ownKeys(VM& vm, JSObject* o, bool strings, bool symbols, bool onlyEnumerable, Vec<Handle>& out)
{
    if (proxyOf(o))
        return objOwnKeysFiltered(vm, o, strings, symbols, onlyEnumerable, out);
    o->materialize(vm);
    Vec<std::pair<std::uint32_t, Key>> indexed;
    Vec<Key> named, syms;
    if (strings && o->cls() == ObjClass::TypedArray)
        for (std::uint32_t i = 0; i < static_cast<JSTypedArray*>(o)->length; ++i)
            indexed.push_back({i, nullptr});
    if (strings && hasElems(o)) {
        auto* a = static_cast<JSArray*>(o);
        for (std::uint32_t i = 0; i < a->length; ++i)
            if (!a->elems[i].isHole())
                indexed.push_back({i, nullptr});
    }
    if (strings && o->cls() == ObjClass::String) {
        JSValue text = unwrapped(toJSValue(o), ObjClass::String);
        if (isString(text))
            for (std::uint32_t i = 0; i < asString(text)->length; ++i)
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
    mergeSort(indexed.data(), indexed.data() + indexed.size(), [](const auto& x, const auto& y) { return x.first < y.first; });
    for (auto& [index, key] : indexed) {
        if (key) {
            out.push_back(Handle::retained(JSValue::cell(key)));
        } else {
            Key made = toPropertyKey(vm, JSValue::boxNumber(double(index)));
            out.push_back(Handle::adopt(JSValue::cell(made)));
        }
    }
    if (strings && !onlyEnumerable && (hasElems(o) || (o->cls() == ObjClass::String && isString(unwrapped(toJSValue(o), ObjClass::String)))))
        out.push_back(Handle::retained(JSValue::cell(vm.atoms.length)));
    for (Key k : named)
        out.push_back(Handle::retained(JSValue::cell(k)));
    for (Key k : syms)
        out.push_back(Handle::retained(JSValue::cell(k)));
    return true;
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
    if (o->cls() == ObjClass::TypedArray) {
        auto* t = static_cast<JSTypedArray*>(o);
        std::uint32_t index;
        if (arrayIndexOf(key, &index)) {
            if (index >= t->length)
                return false;
            out.attrs = attr::kDefault;
            out.value = Handle::adopt(typedGet(vm, t, index));
            return true;
        }
    }
    if (o->cls() == ObjClass::String) {
        JSValue text = unwrapped(toJSValue(o), ObjClass::String);
        std::uint32_t index;
        if (isString(text)) {
            if (key == vm.atoms.length) {
                out.attrs = 0;
                out.value = Handle::adopt(JSValue::boxNumber(double(asString(text)->length)));
                return true;
            }
            if (arrayIndexOf(key, &index) && index < asString(text)->length) {
                out.attrs = attr::kEnumerable;
                out.value = Handle::adopt(makeString(vm, asString(text)->text().substr(index, 1)));
                return true;
            }
        }
    }
    if (hasElems(o)) {
        auto* a = static_cast<JSArray*>(o);
        if (key == vm.atoms.length) {
            out.attrs = frozen ? 0 : attr::kWritable;
            out.value = Handle::adopt(JSValue::boxNumber(double(a->length)));
            return true;
        }
        std::uint32_t index;
        if (arrayIndexOf(key, &index)) {
            const JSValue e = a->at(index);
            if (!e.isHole()) {
                out.attrs = frozen ? attr::kEnumerable : attr::kDefault;
                out.value = Handle::retained(e);
                return true;
            }
            // A hole may stand for an index property with attributes of its own, kept by name.
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

JSValue descriptorObjectOf(VM& vm, JSObject* o, Key key)
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

JSValue descriptorObject(VM& vm, JSObject* o, Key key)
{
    return objOwnDescriptorObject(vm, o, key);
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

JSValue keysArray(VM& vm, Vec<Handle>& keys)
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
    Handle obj = Handle::adopt(toObject(vm, v));
    if (obj.get().isException())
        return obj.get();
    Vec<Handle> keys;
    const int mode = int(reinterpret_cast<std::intptr_t>(data));  // 0 names, 1 symbols
    if (!objOwnKeysFiltered(vm, asObject(obj.get()), mode == 0, mode == 1, false, keys))
        return JSValue::exception();
    return keysArray(vm, keys);
}

NATIVE(objectGetOwnPropertyDescriptor)
{
    UNUSED();
    Handle obj = Handle::adopt(toObject(vm, arg(args, argc, 0)));
    if (obj.get().isException())
        return obj.get();
    Key k = keyArg(vm, arg(args, argc, 1));
    if (!k)
        return JSValue::exception();
    JSValue d = descriptorObject(vm, asObject(obj.get()), k);
    detail::decRef(k);
    return d;
}

NATIVE(objectGetOwnPropertyDescriptors)
{
    UNUSED();
    Handle obj = Handle::adopt(toObject(vm, arg(args, argc, 0)));
    if (obj.get().isException())
        return obj.get();
    JSObject* o = asObject(obj.get());
    Vec<Handle> keys;
    if (!objOwnKeys(vm, o, keys))
        return JSValue::exception();
    JSObject* out = vm.newObject(vm.protos.object);
    Handle keep = Handle::adopt(toJSValue(out));
    for (Handle& k : keys) {
        Key key = static_cast<Key>(k.get().asCell());
        JSValue d = descriptorObject(vm, o, key);
        if (d.isException())
            return d;
        if (!d.isUndefined()) {
            PropDesc fresh;
            fresh.hasValue = fresh.hasWritable = fresh.hasEnumerable = fresh.hasConfigurable = true;
            fresh.writable = fresh.enumerable = fresh.configurable = true;
            fresh.value = Handle::adopt(d);
            if (objDefineOwnProperty(vm, out, key, fresh) < 0)
                return JSValue::exception();
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
    Handle props = Handle::adopt(toObject(vm, arg(args, argc, 1)));
    if (props.get().isException())
        return props.get();
    JSObject* source = asObject(props.get());
    Vec<Handle> keys;
    if (!objOwnKeys(vm, source, keys))
        return JSValue::exception();
    struct Pending
    {
        Key key;
        PropDesc desc;
    };
    std::vector<Pending> pending;
    for (Handle& k : keys) {
        Key key = static_cast<Key>(k.get().asCell());
        PropDesc own;
        const int has = objGetOwnProperty(vm, source, key, &own);
        if (has < 0)
            return JSValue::exception();
        if (!has || !own.enumerable)
            continue;
        Handle d = Handle::adopt(getProperty(vm, props.get(), key));
        if (d.get().isException())
            return d.get();
        if (!isObject(d.get()))
            return typeError(vm, "Property description must be an object");
        Pending item;
        item.key = key;
        if (!readPropertyDescriptor(vm, asObject(d.get()), &item.desc))
            return JSValue::exception();
        pending.push_back(std::move(item));
    }
    for (Pending& item : pending) {
        const int ok = objDefineOwnProperty(vm, o, item.key, item.desc);
        if (ok < 0)
            return JSValue::exception();
        if (!ok)
            return typeError(vm, "Cannot redefine property");
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
    // 0 isExtensible, 1 preventExtensions, 2 seal, 3 isSealed, 4 Reflect.isExtensible, 5 Reflect.preventExtensions
    const int mode = int(reinterpret_cast<std::intptr_t>(data));
    const JSValue v = arg(args, argc, 0);
    if (mode >= 4 && !isObject(v))
        return typeError(vm, std::string("Reflect.") + (mode == 4 ? "isExtensible" : "preventExtensions") + " called on non-object");
    if (!isObject(v))
        return mode == 0 ? JSValue::boolean(false) : mode == 3 ? JSValue::boolean(true) : (retain(v), v);
    JSObject* o = asObject(v);
    if (mode == 0 || mode == 4) {
        const int r = objIsExtensible(vm, o);
        return r < 0 ? JSValue::exception() : JSValue::boolean(r == 1);
    }
    if (mode == 1 || mode == 5) {
        const int r = objPreventExtensions(vm, o);
        if (r < 0)
            return JSValue::exception();
        if (mode == 5)
            return JSValue::boolean(r == 1);
        if (!r)
            return typeError(vm, "Cannot prevent extensions");
        retain(v);
        return v;
    }
    if (mode == 3) {
        const int r = testIntegrity(vm, o, false);
        return r < 0 ? JSValue::exception() : JSValue::boolean(r == 1);
    }
    if (proxyOf(o) || o->cls() == ObjClass::TypedArray) {
        const int r = setIntegrity(vm, o, false);
        if (r < 0)
            return JSValue::exception();
        if (!r)
            return typeError(vm, "Cannot seal");
        retain(v);
        return v;
    }
    o->materialize(vm);
    std::vector<Key> keys;
    o->forEachOwn(vm, [&](Key k, std::uint8_t, JSValue) { keys.push_back(k); });
    o->h.flags |= cellflags::kNotExtensible;
    for (Key k : keys) {
        std::uint8_t attrs = 0;
        o->findOwn(vm, k, &attrs);
        o->setAttrs(vm, k, std::uint8_t(attrs & ~attr::kConfigurable));
    }
    retain(v);
    return v;
}

NATIVE(objectHasOwn)
{
    UNUSED();
    Handle obj = Handle::adopt(toObject(vm, arg(args, argc, 0)));
    if (obj.get().isException())
        return obj.get();
    Key k = keyArg(vm, arg(args, argc, 1));
    if (!k)
        return JSValue::exception();
    PropDesc d;
    const int has = objGetOwnProperty(vm, asObject(obj.get()), k, &d);
    detail::decRef(k);
    return has < 0 ? JSValue::exception() : JSValue::boolean(has == 1);
}

NATIVE(objectPropertyIsEnumerable)
{
    UNUSED();
    Key k = keyArg(vm, arg(args, argc, 0));
    if (!k)
        return JSValue::exception();
    Handle obj = Handle::adopt(toObject(vm, self));
    if (obj.get().isException()) {
        detail::decRef(k);
        return obj.get();
    }
    PropDesc d;
    const int has = objGetOwnProperty(vm, asObject(obj.get()), k, &d);
    detail::decRef(k);
    return has < 0 ? JSValue::exception() : JSValue::boolean(has == 1 && d.enumerable);
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

/// `__defineGetter__` and `__defineSetter__` (data 0 or 1).
NATIVE(objectDefineAccessorLegacy)
{
    UNUSED();
    const bool setter = data != nullptr;
    Handle obj = Handle::adopt(toObject(vm, self));
    if (obj.get().isException())
        return obj.get();
    const JSValue fn = arg(args, argc, 1);
    if (!isFunction(fn))
        return typeError(vm, "Object.prototype.__define" + std::string(setter ? "Setter" : "Getter") + "__: Expecting function");
    Key k = keyArg(vm, arg(args, argc, 0));
    if (!k)
        return JSValue::exception();
    PropDesc desc;
    desc.hasEnumerable = desc.hasConfigurable = true;
    desc.enumerable = desc.configurable = true;
    (setter ? desc.hasSet : desc.hasGet) = true;
    (setter ? desc.set : desc.get) = Handle::retained(fn);
    const int r = objDefineOwnProperty(vm, asObject(obj.get()), k, desc);
    detail::decRef(k);
    if (r < 0)
        return JSValue::exception();
    if (!r)
        return typeError(vm, "Cannot redefine property");
    return JSValue::undefined();
}

/// `__lookupGetter__` and `__lookupSetter__` (data 0 or 1).
NATIVE(objectLookupAccessorLegacy)
{
    UNUSED();
    const bool setter = data != nullptr;
    Handle cur = Handle::adopt(toObject(vm, self));
    if (cur.get().isException())
        return cur.get();
    Key k = keyArg(vm, arg(args, argc, 0));
    if (!k)
        return JSValue::exception();
    JSValue result = JSValue::undefined();
    for (;;) {
        PropDesc d;
        const int has = objGetOwnProperty(vm, asObject(cur.get()), k, &d);
        if (has < 0) {
            result = JSValue::exception();
            break;
        }
        if (has) {
            if (d.isAccessor()) {
                result = (setter ? d.set : d.get).get();
                retain(result);
            }
            break;
        }
        Handle next;
        if (objGetPrototypeOf(vm, asObject(cur.get()), &next) < 0) {
            result = JSValue::exception();
            break;
        }
        if (!isObject(next.get()))
            break;
        cur = std::move(next);
    }
    detail::decRef(k);
    return result;
}

NATIVE(protoGetter)
{
    UNUSED();
    Handle obj = Handle::adopt(toObject(vm, self));
    if (obj.get().isException())
        return obj.get();
    Handle p;
    if (objGetPrototypeOf(vm, asObject(obj.get()), &p) < 0)
        return JSValue::exception();
    return p.take();
}

NATIVE(protoSetter)
{
    UNUSED();
    if (self.isNullish())
        return typeError(vm, "Object.prototype.__proto__ called on null or undefined");
    const JSValue p = arg(args, argc, 0);
    if (!isObject(self) || !(isObject(p) || p.isNull()))
        return JSValue::undefined();
    const int r = objSetPrototypeOf(vm, asObject(self), p);
    if (r < 0)
        return JSValue::exception();
    if (!r)
        return typeError(vm, prototypeRefusal(vm, asObject(self)));
    return JSValue::undefined();
}

// ------------------------------------------------------------------ Reflect ---

}  // namespace

int ordinaryOwnProperty(VM& vm, JSObject* o, Key key, PropDesc* out)
{
    OwnDesc d;
    if (!ownDescriptor(vm, o, key, d))
        return 0;
    out->hasEnumerable = out->hasConfigurable = true;
    out->enumerable = (d.attrs & attr::kEnumerable) != 0;
    out->configurable = (d.attrs & attr::kConfigurable) != 0;
    if (d.accessor) {
        out->hasGet = out->hasSet = true;
        out->get = d.getter;
        out->set = d.setter;
    } else {
        out->hasValue = out->hasWritable = true;
        out->writable = (d.attrs & attr::kWritable) != 0;
        out->value = d.value;
    }
    return 1;
}

bool listFromArrayLike(VM& vm, JSValue list, Vec<Handle>& out)
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

namespace {

std::vector<JSValue> raw(const Vec<Handle>& hs)
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
    Vec<Handle> list;
    if (!listFromArrayLike(vm, arg(args, argc, 2), list))
        return JSValue::exception();
    std::vector<JSValue> values = raw(list);
    return vm.call(arg(args, argc, 0), arg(args, argc, 1), values.data(), std::uint32_t(values.size()));
}

NATIVE(reflectConstruct)
{
    UNUSED();
    const JSValue target = argc > 2 ? args[2] : JSValue::undefined();
    auto constructible = [&](JSValue f) {
        if (!isFunction(f))
            return false;
        auto* fn = static_cast<JSFunction*>(asObject(f));
        if (fn->flags & JSFunction::kNative)
            return (fn->flags & JSFunction::kConstructor) != 0;
        return fn->code->constructor;
    };
    if (!constructible(arg(args, argc, 0)))
        return typeError(vm, describe(vm, arg(args, argc, 0)) + " is not a constructor");
    if (argc > 2 && !constructible(target))
        return typeError(vm, describe(vm, target) + " is not a constructor");
    Vec<Handle> list;
    if (!listFromArrayLike(vm, arg(args, argc, 1), list))
        return JSValue::exception();
    std::vector<JSValue> values = raw(list);
    return vm.construct(arg(args, argc, 0), values.data(), std::uint32_t(values.size()), target);
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
    int status = 0;
    switch (mode) {
        case 0: result = getPropertyFrom(vm, o, argc > 2 ? args[2] : args[0], k); break;
        case 1:
            status = objSet(vm, o, k, arg(args, argc, 2), argc > 3 ? args[3] : args[0]);
            result = status < 0 ? JSValue::exception() : JSValue::boolean(status == 1);
            break;
        case 2:
            status = objHasProperty(vm, o, k);
            result = status < 0 ? JSValue::exception() : JSValue::boolean(status == 1);
            break;
        case 3:
            status = objDelete(vm, o, k);
            result = status < 0 ? JSValue::exception() : JSValue::boolean(status == 1);
            break;
        case 4: {
            PropDesc desc;
            if (!isObject(arg(args, argc, 2))) {
                result = typeError(vm, "Property description must be an object");
                break;
            }
            if (!readPropertyDescriptor(vm, asObject(args[2]), &desc)) {
                result = JSValue::exception();
                break;
            }
            status = objDefineOwnProperty(vm, o, k, desc);
            result = status < 0 ? JSValue::exception() : JSValue::boolean(status == 1);
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
    Vec<Handle> keys;
    if (!objOwnKeys(vm, o, keys))
        return JSValue::exception();
    return keysArray(vm, keys);
}

NATIVE(reflectProto)
{
    UNUSED();
    JSObject* o = objectArg(vm, arg(args, argc, 0), data ? "Reflect.setPrototypeOf" : "Reflect.getPrototypeOf");
    if (!o)
        return JSValue::exception();
    if (!data) {
        Handle p;
        if (objGetPrototypeOf(vm, o, &p) < 0)
            return JSValue::exception();
        return p.take();
    }
    const JSValue p = arg(args, argc, 1);
    if (!(isObject(p) || p.isNull()))
        return typeError(vm, "Object prototype may only be an Object or null");
    const int r = objSetPrototypeOf(vm, o, p);
    return r < 0 ? JSValue::exception() : JSValue::boolean(r == 1);
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
    self = unwrapped(self, ObjClass::Number);
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
    // ToBigInt: a BigInt, or a boolean or string that makes one, after ToPrimitive.
    Handle big;
    {
        JSValue prim = toPrimitive(vm, arg(args, argc, 1), false);
        if (prim.isException())
            return prim;
        Handle keep = Handle::adopt(prim);
        if (isBigInt(prim)) {
            big = Handle::retained(prim);
        } else if (prim.isBool()) {
            big = Handle::adopt(bigintFromDouble(vm, prim == JSValue::boolean(true) ? 1 : 0));
        } else if (isString(prim)) {
            bool ok;
            JSValue r = bigintFromText(vm, asString(prim)->text(), &ok);
            if (!ok) {
                if (r.isException())
                    return r;
                return vm.throwError(ErrorCode::SyntaxError, "Cannot convert " + toUtf8(asString(prim)->text()) + " to a BigInt");
            }
            big = Handle::adopt(r);
        } else {
            return typeError(vm, "Cannot convert " + describe(vm, prim) + " to a BigInt");
        }
    }
    return bigintAsN(vm, std::uint64_t(bits), big.get(), isSigned);
}

NATIVE(bigintToStringFn)
{
    UNUSED();
    self = unwrapped(self, ObjClass::BigInt);
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
    self = unwrapped(self, ObjClass::BigInt);
    if (!isBigInt(self))
        return typeError(vm, "BigInt.prototype.valueOf requires that 'this' be a BigInt");
    retain(self);
    return self;
}

}  // namespace

JSValue ownDescriptorObject(VM& vm, JSObject* o, Key key) { return descriptorObjectOf(vm, o, key); }

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
    method(vm, objectProto, "__defineGetter__", objectDefineAccessorLegacy, 2, nullptr);
    method(vm, objectProto, "__defineSetter__", objectDefineAccessorLegacy, 2, reinterpret_cast<void*>(1));
    method(vm, objectProto, "__lookupGetter__", objectLookupAccessorLegacy, 1, nullptr);
    method(vm, objectProto, "__lookupSetter__", objectLookupAccessorLegacy, 1, reinterpret_cast<void*>(1));
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
    method(vm, reflect, "isExtensible", objectExtensibility, 1, reinterpret_cast<void*>(4));
    method(vm, reflect, "preventExtensions", objectExtensibility, 1, reinterpret_cast<void*>(5));

    // Array.
    Key arrayKey = vm.internKey(std::string_view("Array"));
    auto* arrayCtorFn = static_cast<JSFunction*>(asObject(*vm.global->findOwn(vm, arrayKey)));
    detail::decRef(arrayKey);
    JSObject* ap = vm.protos.array;

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
