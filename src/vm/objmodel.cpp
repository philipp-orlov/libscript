// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The internal methods of an object -- [[GetOwnProperty]], [[DefineOwnProperty]], [[Set]], [[GetPrototypeOf]] and the
// rest -- for ordinary objects and for proxies. A proxy's trap may answer however it likes, and each answer is checked
// against what the target says, as the language requires: a trap that contradicts a fixed property of its target is a
// TypeError. Object, Reflect and the proxy itself are built on these.
#include <algorithm>
#include <set>

#include "builtins_util.hpp"

namespace script {
namespace internal {
namespace vm {
namespace bi {

namespace {

bool sameValueOfPair(JSValue a, JSValue b)
{
    if (a.isNumber() && b.isNumber()) {
        const double x = a.asNumber(), y = b.asNumber();
        if (x != x && y != y)
            return true;
        return x == y && std::signbit(x) == std::signbit(y);
    }
    return strictEquals(a, b);
}

JSValue keyValueOf(Key k) { return JSValue::cell(k); }

JSValue fail(VM& vm, const std::string& message) { return vm.throwError(ErrorCode::TypeError, message); }

/// The trap `name` of the proxy's handler: a function, undefined for none, or the exception sentinel. A revoked proxy is a
/// TypeError.
JSValue trapOf(VM& vm, ProxyState* ps, const char* name)
{
    if (!isObject(ps->handler))
        return fail(vm, std::string("Cannot perform '") + name + "' on a proxy that has been revoked");
    Key k = vm.internKey(std::string_view(name));
    JSValue trap = getProperty(vm, ps->handler, k);
    detail::decRef(k);
    if (trap.isException())
        return trap;
    if (trap.isNullish()) {
        release(trap);
        return JSValue::undefined();
    }
    if (!isFunction(trap)) {
        release(trap);
        return fail(vm, std::string("proxy trap '") + name + "' is not a function");
    }
    return trap;
}

JSObject* targetOf(ProxyState* ps) { return asObject(ps->target); }

/// CompletePropertyDescriptor.
void complete(PropDesc& d)
{
    if (!d.isAccessor()) {
        if (!d.hasValue) {
            d.hasValue = true;
            d.value = Handle();
        }
        d.hasWritable = true;
    } else {
        d.hasGet = d.hasSet = true;
    }
    d.hasEnumerable = d.hasConfigurable = true;
}

/// IsCompatiblePropertyDescriptor.
bool compatible(bool extensible, const PropDesc& d, bool hasCurrent, const PropDesc& cur)
{
    if (!hasCurrent)
        return extensible;
    if (!d.hasValue && !d.hasWritable && !d.hasGet && !d.hasSet && !d.hasEnumerable && !d.hasConfigurable)
        return true;
    if (!cur.configurable) {
        if (d.hasConfigurable && d.configurable)
            return false;
        if (d.hasEnumerable && d.enumerable != cur.enumerable)
            return false;
        const bool generic = !d.isAccessor() && !d.isData();
        if (!generic) {
            if (d.isAccessor() != cur.isAccessor())
                return false;
            if (cur.isAccessor()) {
                if (d.hasGet && !sameValueOfPair(d.get.get(), cur.get.get()))
                    return false;
                if (d.hasSet && !sameValueOfPair(d.set.get(), cur.set.get()))
                    return false;
            } else if (!cur.writable) {
                if (d.hasWritable && d.writable)
                    return false;
                if (d.hasValue && !sameValueOfPair(d.value.get(), cur.value.get()))
                    return false;
            }
        }
    }
    return true;
}

bool isImmutablePrototype(VM& vm, JSObject* o) { return o == vm.protos.object; }

// ------------------------------------------------------------------ proxies ---

int proxyGetPrototypeOf(VM& vm, JSObject* proxy, Handle* out)
{
    ProxyState* ps = proxyOf(proxy);
    JSValue trap = trapOf(vm, ps, "getPrototypeOf");
    if (trap.isException())
        return -1;
    Handle trapKeep = Handle::adopt(trap);
    if (trap.isUndefined())
        return objGetPrototypeOf(vm, targetOf(ps), out);
    JSValue r = vm.call(trap, ps->handler, &ps->target, 1);
    if (r.isException())
        return -1;
    Handle result = Handle::adopt(r);
    if (!isObject(r) && !r.isNull())
        return fail(vm, "'getPrototypeOf' on proxy: trap returned neither object nor null"), -1;
    const int extensible = objIsExtensible(vm, targetOf(ps));
    if (extensible < 0)
        return -1;
    if (!extensible) {
        Handle targetProto;
        if (objGetPrototypeOf(vm, targetOf(ps), &targetProto) < 0)
            return -1;
        if (!sameValueOfPair(r, targetProto.get()))
            return fail(vm, "'getPrototypeOf' on proxy: proxy target is non-extensible but the trap did not return its actual prototype"), -1;
    }
    *out = std::move(result);
    return 1;
}

int proxySetPrototypeOf(VM& vm, JSObject* proxy, JSValue proto)
{
    ProxyState* ps = proxyOf(proxy);
    JSValue trap = trapOf(vm, ps, "setPrototypeOf");
    if (trap.isException())
        return -1;
    Handle trapKeep = Handle::adopt(trap);
    if (trap.isUndefined())
        return objSetPrototypeOf(vm, targetOf(ps), proto);
    JSValue callArgs[2] = {ps->target, proto};
    JSValue r = vm.call(trap, ps->handler, callArgs, 2);
    if (r.isException())
        return -1;
    const bool ok = toBoolean(r);
    release(r);
    if (!ok)
        return 0;
    const int extensible = objIsExtensible(vm, targetOf(ps));
    if (extensible < 0)
        return -1;
    if (extensible)
        return 1;
    Handle targetProto;
    if (objGetPrototypeOf(vm, targetOf(ps), &targetProto) < 0)
        return -1;
    if (!sameValueOfPair(proto, targetProto.get()))
        return fail(vm, "'setPrototypeOf' on proxy: trap returned truish for setting a new prototype on the non-extensible proxy target"), -1;
    return 1;
}

int proxyIsExtensible(VM& vm, JSObject* proxy)
{
    ProxyState* ps = proxyOf(proxy);
    JSValue trap = trapOf(vm, ps, "isExtensible");
    if (trap.isException())
        return -1;
    Handle trapKeep = Handle::adopt(trap);
    if (trap.isUndefined())
        return objIsExtensible(vm, targetOf(ps));
    JSValue r = vm.call(trap, ps->handler, &ps->target, 1);
    if (r.isException())
        return -1;
    const bool result = toBoolean(r);
    release(r);
    const int actual = objIsExtensible(vm, targetOf(ps));
    if (actual < 0)
        return -1;
    if (result != (actual != 0))
        return fail(vm, "'isExtensible' on proxy: trap result does not reflect extensibility of proxy target"), -1;
    return result;
}

int proxyPreventExtensions(VM& vm, JSObject* proxy)
{
    ProxyState* ps = proxyOf(proxy);
    JSValue trap = trapOf(vm, ps, "preventExtensions");
    if (trap.isException())
        return -1;
    Handle trapKeep = Handle::adopt(trap);
    if (trap.isUndefined())
        return objPreventExtensions(vm, targetOf(ps));
    JSValue r = vm.call(trap, ps->handler, &ps->target, 1);
    if (r.isException())
        return -1;
    const bool result = toBoolean(r);
    release(r);
    if (result) {
        const int extensible = objIsExtensible(vm, targetOf(ps));
        if (extensible < 0)
            return -1;
        if (extensible)
            return fail(vm, "'preventExtensions' on proxy: trap returned truish but the proxy target is extensible"), -1;
    }
    return result;
}

int proxyGetOwnProperty(VM& vm, JSObject* proxy, Key key, PropDesc* out)
{
    ProxyState* ps = proxyOf(proxy);
    JSValue trap = trapOf(vm, ps, "getOwnPropertyDescriptor");
    if (trap.isException())
        return -1;
    Handle trapKeep = Handle::adopt(trap);
    if (trap.isUndefined())
        return objGetOwnProperty(vm, targetOf(ps), key, out);
    JSValue callArgs[2] = {ps->target, keyValueOf(key)};
    JSValue r = vm.call(trap, ps->handler, callArgs, 2);
    if (r.isException())
        return -1;
    Handle result = Handle::adopt(r);
    if (!isObject(r) && !r.isUndefined())
        return fail(vm, "'getOwnPropertyDescriptor' on proxy: trap returned neither object nor undefined for property"), -1;
    PropDesc targetDesc;
    const int hasTarget = objGetOwnProperty(vm, targetOf(ps), key, &targetDesc);
    if (hasTarget < 0)
        return -1;
    if (r.isUndefined()) {
        if (!hasTarget)
            return 0;
        if (!targetDesc.configurable)
            return fail(vm, "'getOwnPropertyDescriptor' on proxy: trap returned undefined for a non-configurable property"), -1;
        const int extensible = objIsExtensible(vm, targetOf(ps));
        if (extensible < 0)
            return -1;
        if (!extensible)
            return fail(vm, "'getOwnPropertyDescriptor' on proxy: trap returned undefined for a property of a non-extensible target"), -1;
        return 0;
    }
    const int extensible = objIsExtensible(vm, targetOf(ps));
    if (extensible < 0)
        return -1;
    PropDesc resultDesc;
    if (!readPropertyDescriptor(vm, asObject(r), &resultDesc))
        return -1;
    complete(resultDesc);
    if (!compatible(extensible != 0, resultDesc, hasTarget != 0, targetDesc))
        return fail(vm, "'getOwnPropertyDescriptor' on proxy: trap returned a descriptor incompatible with the target's"), -1;
    if (!resultDesc.configurable) {
        if (!hasTarget || targetDesc.configurable)
            return fail(vm, "'getOwnPropertyDescriptor' on proxy: trap reported non-configurability for a configurable or missing property"), -1;
        if (resultDesc.hasWritable && !resultDesc.writable && targetDesc.writable)
            return fail(vm, "'getOwnPropertyDescriptor' on proxy: trap reported non-writability for a writable property"), -1;
    }
    *out = std::move(resultDesc);
    return 1;
}

int proxyDefineOwnProperty(VM& vm, JSObject* proxy, Key key, const PropDesc& desc)
{
    ProxyState* ps = proxyOf(proxy);
    JSValue trap = trapOf(vm, ps, "defineProperty");
    if (trap.isException())
        return -1;
    Handle trapKeep = Handle::adopt(trap);
    if (trap.isUndefined())
        return objDefineOwnProperty(vm, targetOf(ps), key, desc);
    Handle descObject = Handle::adopt(descriptorToObject(vm, desc));
    if (descObject.get().isException())
        return -1;
    JSValue callArgs[3] = {ps->target, keyValueOf(key), descObject.get()};
    JSValue r = vm.call(trap, ps->handler, callArgs, 3);
    if (r.isException())
        return -1;
    const bool ok = toBoolean(r);
    release(r);
    if (!ok)
        return 0;
    PropDesc targetDesc;
    const int hasTarget = objGetOwnProperty(vm, targetOf(ps), key, &targetDesc);
    if (hasTarget < 0)
        return -1;
    const int extensible = objIsExtensible(vm, targetOf(ps));
    if (extensible < 0)
        return -1;
    const bool settingNonConfigurable = desc.hasConfigurable && !desc.configurable;
    if (!hasTarget) {
        if (!extensible)
            return fail(vm, "'defineProperty' on proxy: trap returned truish for adding a property to the non-extensible target"), -1;
        if (settingNonConfigurable)
            return fail(vm, "'defineProperty' on proxy: trap returned truish for defining a non-configurable property that does not exist on the target"), -1;
        return 1;
    }
    if (!compatible(extensible != 0, desc, true, targetDesc))
        return fail(vm, "'defineProperty' on proxy: trap returned truish for adding a property that is incompatible with the existing property of the target"), -1;
    if (settingNonConfigurable && targetDesc.configurable)
        return fail(vm, "'defineProperty' on proxy: trap returned truish for defining a non-configurable property that is configurable on the target"), -1;
    if (!targetDesc.isAccessor() && !targetDesc.configurable && targetDesc.writable && desc.hasWritable && !desc.writable)
        return fail(vm, "'defineProperty' on proxy: trap returned truish for defining a non-writable property that is writable on the target"), -1;
    return 1;
}

int proxyHasProperty(VM& vm, JSObject* proxy, Key key)
{
    ProxyState* ps = proxyOf(proxy);
    JSValue trap = trapOf(vm, ps, "has");
    if (trap.isException())
        return -1;
    Handle trapKeep = Handle::adopt(trap);
    if (trap.isUndefined())
        return objHasProperty(vm, targetOf(ps), key);
    JSValue callArgs[2] = {ps->target, keyValueOf(key)};
    JSValue r = vm.call(trap, ps->handler, callArgs, 2);
    if (r.isException())
        return -1;
    const bool result = toBoolean(r);
    release(r);
    if (!result) {
        PropDesc targetDesc;
        const int hasTarget = objGetOwnProperty(vm, targetOf(ps), key, &targetDesc);
        if (hasTarget < 0)
            return -1;
        if (hasTarget) {
            if (!targetDesc.configurable)
                return fail(vm, "'has' on proxy: trap returned falsish for a non-configurable property of the target"), -1;
            const int extensible = objIsExtensible(vm, targetOf(ps));
            if (extensible < 0)
                return -1;
            if (!extensible)
                return fail(vm, "'has' on proxy: trap returned falsish for a property of a non-extensible target"), -1;
        }
    }
    return result;
}

int proxyDeleteProperty(VM& vm, JSObject* proxy, Key key)
{
    ProxyState* ps = proxyOf(proxy);
    JSValue trap = trapOf(vm, ps, "deleteProperty");
    if (trap.isException())
        return -1;
    Handle trapKeep = Handle::adopt(trap);
    if (trap.isUndefined())
        return objDelete(vm, targetOf(ps), key);
    JSValue callArgs[2] = {ps->target, keyValueOf(key)};
    JSValue r = vm.call(trap, ps->handler, callArgs, 2);
    if (r.isException())
        return -1;
    const bool ok = toBoolean(r);
    release(r);
    if (!ok)
        return 0;
    PropDesc targetDesc;
    const int hasTarget = objGetOwnProperty(vm, targetOf(ps), key, &targetDesc);
    if (hasTarget < 0)
        return -1;
    if (!hasTarget)
        return 1;
    if (!targetDesc.configurable)
        return fail(vm, "'deleteProperty' on proxy: trap returned truish for a non-configurable property of the target"), -1;
    const int extensible = objIsExtensible(vm, targetOf(ps));
    if (extensible < 0)
        return -1;
    if (!extensible)
        return fail(vm, "'deleteProperty' on proxy: trap returned truish for a property of a non-extensible target"), -1;
    return 1;
}

int proxySetProperty(VM& vm, JSObject* proxy, Key key, JSValue value, JSValue receiver)
{
    ProxyState* ps = proxyOf(proxy);
    JSValue trap = trapOf(vm, ps, "set");
    if (trap.isException())
        return -1;
    Handle trapKeep = Handle::adopt(trap);
    if (trap.isUndefined())
        return objSet(vm, targetOf(ps), key, value, receiver);
    JSValue callArgs[4] = {ps->target, keyValueOf(key), value, receiver};
    JSValue r = vm.call(trap, ps->handler, callArgs, 4);
    if (r.isException())
        return -1;
    const bool ok = toBoolean(r);
    release(r);
    if (!ok)
        return 0;
    PropDesc targetDesc;
    const int hasTarget = objGetOwnProperty(vm, targetOf(ps), key, &targetDesc);
    if (hasTarget < 0)
        return -1;
    if (hasTarget && !targetDesc.configurable) {
        if (!targetDesc.isAccessor() && !targetDesc.writable && !sameValueOfPair(value, targetDesc.value.get()))
            return fail(vm, "'set' on proxy: trap returned truish for a non-writable, non-configurable property with a different value"), -1;
        if (targetDesc.isAccessor() && targetDesc.set.get().isUndefined())
            return fail(vm, "'set' on proxy: trap returned truish for a non-configurable accessor property without a setter"), -1;
    }
    return 1;
}

bool proxyOwnKeysChecked(VM& vm, JSObject* proxy, Vec<Handle>& out)
{
    ProxyState* ps = proxyOf(proxy);
    JSValue trap = trapOf(vm, ps, "ownKeys");
    if (trap.isException())
        return false;
    Handle trapKeep = Handle::adopt(trap);
    if (trap.isUndefined())
        return objOwnKeys(vm, targetOf(ps), out);
    JSValue r = vm.call(trap, ps->handler, &ps->target, 1);
    if (r.isException())
        return false;
    Handle rKeep = Handle::adopt(r);
    if (!isObject(r))
        return fail(vm, "CreateListFromArrayLike called on non-object"), false;
    Handle len = Handle::adopt(getProperty(vm, r, vm.atoms.length));
    if (len.get().isException())
        return false;
    double n = 0;
    if (!toNumber(vm, len.get(), &n))
        return false;
    n = n != n ? 0 : std::trunc(n);
    n = n <= 0 ? 0 : std::min(n, 9007199254740991.0);
    Vec<Handle> trapResult;
    std::set<Key> seen;
    for (double i = 0; i < n; ++i) {
        Key ik = toPropertyKey(vm, JSValue::boxNumber(i));
        if (!ik)
            return false;
        JSValue k = getProperty(vm, r, ik);
        detail::decRef(ik);
        if (k.isException())
            return false;
        Handle kk = Handle::adopt(k);
        if (!isString(k) && !isSymbol(k))
            return fail(vm, "proxy [[OwnPropertyKeys]] must return an array with only string and symbol elements"), false;
        Key key = toPropertyKey(vm, k);
        if (!key)
            return false;
        Handle keyKeep = Handle::adopt(JSValue::cell(key));
        if (!seen.insert(key).second)
            return fail(vm, "'ownKeys' on proxy: trap returned duplicate entries"), false;
        trapResult.push_back(std::move(keyKeep));
    }
    const int extensible = objIsExtensible(vm, targetOf(ps));
    if (extensible < 0)
        return false;
    Vec<Handle> targetKeys;
    if (!objOwnKeys(vm, targetOf(ps), targetKeys))
        return false;
    Vec<Key> configurable, fixed;
    for (Handle& tk : targetKeys) {
        Key key = static_cast<Key>(tk.get().asCell());
        PropDesc d;
        const int has = objGetOwnProperty(vm, targetOf(ps), key, &d);
        if (has < 0)
            return false;
        (has && !d.configurable ? fixed : configurable).push_back(key);
    }
    if (extensible && fixed.empty()) {
        out = std::move(trapResult);
        return true;
    }
    std::set<Key> unchecked = seen;
    for (Key key : fixed) {
        if (!unchecked.erase(key))
            return fail(vm, "'ownKeys' on proxy: trap result did not include a non-configurable key of the target"), false;
    }
    if (extensible) {
        out = std::move(trapResult);
        return true;
    }
    for (Key key : configurable) {
        if (!unchecked.erase(key))
            return fail(vm, "'ownKeys' on proxy: trap result did not include all keys of the non-extensible target"), false;
    }
    if (!unchecked.empty())
        return fail(vm, "'ownKeys' on proxy: trap returned extra keys but the proxy target is non-extensible"), false;
    out = std::move(trapResult);
    return true;
}

// ----------------------------------------------------------------- ordinary ---

int ordinarySetWithReceiver(VM& vm, JSObject* o, Key key, JSValue value, JSValue receiver)
{
    PropDesc own;
    int has = objGetOwnProperty(vm, o, key, &own);
    if (has < 0)
        return -1;
    if (!has) {
        Handle parent;
        if (objGetPrototypeOf(vm, o, &parent) < 0)
            return -1;
        if (isObject(parent.get()))
            return objSet(vm, asObject(parent.get()), key, value, receiver);
        own.hasValue = own.hasWritable = true;
        own.writable = own.enumerable = own.configurable = true;
        own.hasEnumerable = own.hasConfigurable = true;
    }
    if (own.isAccessor()) {
        if (own.set.get().isUndefined())
            return 0;
        JSValue r = vm.call(own.set.get(), receiver, &value, 1);
        if (r.isException())
            return -1;
        release(r);
        return 1;
    }
    if (!own.writable)
        return 0;
    if (!isObject(receiver))
        return 0;
    JSObject* target = asObject(receiver);
    PropDesc existing;
    const int found = objGetOwnProperty(vm, target, key, &existing);
    if (found < 0)
        return -1;
    PropDesc update;
    update.hasValue = true;
    update.value = Handle::retained(value);
    if (found) {
        if (existing.isAccessor() || !existing.writable)
            return 0;
        return objDefineOwnProperty(vm, target, key, update);
    }
    update.hasWritable = update.hasEnumerable = update.hasConfigurable = true;
    update.writable = update.enumerable = update.configurable = true;
    return objDefineOwnProperty(vm, target, key, update);
}

}  // namespace

JSValue proxyTrap(VM& vm, ProxyState* ps, const char* name)
{
    return trapOf(vm, ps, name);
}

// --------------------------------------------------------------- the methods ---

int ordinaryOwnProperty(VM& vm, JSObject* o, Key key, PropDesc* out);

int objGetOwnProperty(VM& vm, JSObject* o, Key key, PropDesc* out)
{
    if (proxyOf(o))
        return proxyGetOwnProperty(vm, o, key, out);
    return ordinaryOwnProperty(vm, o, key, out);
}

int objDefineOwnProperty(VM& vm, JSObject* o, Key key, const PropDesc& desc)
{
    if (proxyOf(o))
        return proxyDefineOwnProperty(vm, o, key, desc);
    bool rejected = false;
    JSValue r = defineOrdinary(vm, o, key, desc, &rejected);
    if (!r.isException())
        return 1;
    if (!rejected)
        return -1;
    vm.clearException();
    return 0;
}

int objHasProperty(VM& vm, JSObject* o, Key key)
{
    if (proxyOf(o))
        return proxyHasProperty(vm, o, key);
    if (hasProperty(vm, toJSValue(o), key))
        return 1;
    return vm.exception.isUndefined() ? 0 : -1;
}

int objSet(VM& vm, JSObject* o, Key key, JSValue value, JSValue receiver)
{
    if (proxyOf(o))
        return proxySetProperty(vm, o, key, value, receiver);
    if (isObject(receiver) && asObject(receiver) == o) {
        if (setProperty(vm, receiver, key, value, false))
            return 1;
        return vm.exception.isUndefined() ? 0 : -1;
    }
    return ordinarySetWithReceiver(vm, o, key, value, receiver);
}

int objDelete(VM& vm, JSObject* o, Key key)
{
    if (proxyOf(o))
        return proxyDeleteProperty(vm, o, key);
    if (deleteProperty(vm, toJSValue(o), key, false))
        return 1;
    return vm.exception.isUndefined() ? 0 : -1;
}

int objGetPrototypeOf(VM& vm, JSObject* o, Handle* out)
{
    if (proxyOf(o))
        return proxyGetPrototypeOf(vm, o, out);
    JSObject* p = o->proto(vm);
    *out = p ? Handle::retained(toJSValue(p)) : Handle::adopt(JSValue::null());
    return 1;
}

int objSetPrototypeOf(VM& vm, JSObject* o, JSValue proto)
{
    if (proxyOf(o))
        return proxySetPrototypeOf(vm, o, proto);
    JSObject* next = isObject(proto) ? asObject(proto) : nullptr;
    if (o->proto(vm) == next)
        return 1;
    if (o->h.flags & cellflags::kNotExtensible)
        return 0;
    if (isImmutablePrototype(vm, o))
        return 0;
    // The chain is followed up to a proxy, whose own prototype the language does not look at.
    for (JSObject* p = next; p; p = p->proto(vm)) {
        if (p == o)
            return 0;
        if (proxyOf(p))
            break;
    }
    o->setProto(vm, next);
    return 1;
}

int isArrayValue(VM& vm, JSValue v)
{
    while (isObject(v) && proxyOf(asObject(v))) {
        ProxyState* ps = proxyOf(asObject(v));
        if (!isObject(ps->handler))
            return fail(vm, "Cannot perform 'IsArray' on a proxy that has been revoked"), -1;
        v = ps->target;
    }
    return isArray(v) ? 1 : 0;
}

std::string prototypeRefusal(VM& vm, JSObject* o)
{
    if (proxyOf(o))
        return "'setPrototypeOf' on proxy: trap returned falsish";
    if (o->h.flags & cellflags::kNotExtensible)
        return "#<Object> is not extensible";
    if (isImmutablePrototype(vm, o))
        return "Immutable prototype object cannot have its prototype set";
    return "cyclic prototype chain";
}

int objIsExtensible(VM& vm, JSObject* o)
{
    if (proxyOf(o))
        return proxyIsExtensible(vm, o);
    return (o->h.flags & cellflags::kNotExtensible) ? 0 : 1;
}

int objPreventExtensions(VM& vm, JSObject* o)
{
    if (proxyOf(o))
        return proxyPreventExtensions(vm, o);
    if (o->cls() == ObjClass::TypedArray) {
        // A typed array whose length can change with its buffer cannot be made non-extensible.
        if (static_cast<JSTypedArray*>(o)->tracking)
            return 0;
    }
    if (hasElems(o))
        static_cast<JSArray*>(o)->vals();
    o->h.flags |= cellflags::kNotExtensible;
    return 1;
}

bool objOwnKeys(VM& vm, JSObject* o, Vec<Handle>& out)
{
    if (proxyOf(o))
        return proxyOwnKeysChecked(vm, o, out);
    return ownKeys(vm, o, true, true, false, out);
}

bool objOwnKeysFiltered(VM& vm, JSObject* o, bool strings, bool symbols, bool onlyEnumerable, Vec<Handle>& out)
{
    if (!proxyOf(o))
        return ownKeys(vm, o, strings, symbols, onlyEnumerable, out);
    Vec<Handle> all;
    if (!proxyOwnKeysChecked(vm, o, all))
        return false;
    for (Handle& k : all) {
        if (isSymbol(k.get()) ? !symbols : !strings)
            continue;
        if (onlyEnumerable) {
            PropDesc d;
            const int has = objGetOwnProperty(vm, o, static_cast<Key>(k.get().asCell()), &d);
            if (has < 0)
                return false;
            if (!has || !d.enumerable)
                continue;
        }
        out.push_back(std::move(k));
    }
    return true;
}

JSValue descriptorToObject(VM& vm, const PropDesc& d)
{
    JSObject* r = vm.newObject(vm.protos.object);
    Handle keep = Handle::adopt(toJSValue(r));
    auto put = [&](const char* name, JSValue v) {
        Key k = vm.internKey(std::string_view(name));
        retain(v);
        r->putOwn(vm, k, v, attr::kDefault);
        detail::decRef(k);
    };
    if (d.hasValue)
        put("value", d.value.get());
    if (d.hasWritable)
        put("writable", JSValue::boolean(d.writable));
    if (d.hasGet)
        put("get", d.get.get());
    if (d.hasSet)
        put("set", d.set.get());
    if (d.hasEnumerable)
        put("enumerable", JSValue::boolean(d.enumerable));
    if (d.hasConfigurable)
        put("configurable", JSValue::boolean(d.configurable));
    return keep.take();
}

JSValue definePropertyFromDescriptor(VM& vm, JSObject* o, Key key, JSObject* descriptor)
{
    PropDesc desc;
    if (!readPropertyDescriptor(vm, descriptor, &desc))
        return JSValue::exception();
    const int r = objDefineOwnProperty(vm, o, key, desc);
    if (r < 0)
        return JSValue::exception();
    if (!r) {
        std::string name = key->kind == detail::CellKind::Symbol ? std::string("symbol") : toUtf8(static_cast<StringCell*>(key)->text());
        return fail(vm, "Cannot redefine property: " + name);
    }
    return JSValue::undefined();
}

JSValue objOwnDescriptorObject(VM& vm, JSObject* o, Key key)
{
    PropDesc d;
    const int has = objGetOwnProperty(vm, o, key, &d);
    if (has < 0)
        return JSValue::exception();
    if (!has)
        return JSValue::undefined();
    return descriptorToObject(vm, d);
}

int setIntegrity(VM& vm, JSObject* o, bool frozen)
{
    const int prevented = objPreventExtensions(vm, o);
    if (prevented <= 0)
        return prevented;
    Vec<Handle> keys;
    if (!objOwnKeys(vm, o, keys))
        return -1;
    for (Handle& k : keys) {
        Key key = static_cast<Key>(k.get().asCell());
        PropDesc d;
        if (frozen) {
            const int has = objGetOwnProperty(vm, o, key, &d);
            if (has < 0)
                return -1;
            if (!has)
                continue;
            PropDesc change;
            change.hasConfigurable = true;
            if (!d.isAccessor())
                change.hasWritable = true;
            const int ok = objDefineOwnProperty(vm, o, key, change);
            if (ok < 0)
                return -1;
            if (!ok)
                return fail(vm, "Cannot redefine property: " + toUtf8(key->kind == detail::CellKind::Symbol ? StringView(u"symbol") : static_cast<StringCell*>(key)->text())), -1;
        } else {
            d.hasConfigurable = true;
            const int ok = objDefineOwnProperty(vm, o, key, d);
            if (ok < 0)
                return -1;
            if (!ok)
                return fail(vm, "Cannot redefine property"), -1;
        }
    }
    return 1;
}

int testIntegrity(VM& vm, JSObject* o, bool frozen)
{
    const int extensible = objIsExtensible(vm, o);
    if (extensible < 0)
        return -1;
    if (extensible)
        return 0;
    Vec<Handle> keys;
    if (!objOwnKeys(vm, o, keys))
        return -1;
    for (Handle& k : keys) {
        PropDesc d;
        const int has = objGetOwnProperty(vm, o, static_cast<Key>(k.get().asCell()), &d);
        if (has < 0)
            return -1;
        if (!has)
            continue;
        if (d.configurable)
            return 0;
        if (frozen && !d.isAccessor() && d.writable)
            return 0;
    }
    return 1;
}

}  // namespace bi

using namespace bi;

JSValue proxyGet(VM& vm, JSObject* proxy, Key key, JSValue receiver)
{
    ProxyState* ps = proxyOf(proxy);
    JSValue trap = trapOf(vm, ps, "get");
    if (trap.isException())
        return trap;
    Handle trapKeep = Handle::adopt(trap);
    if (trap.isUndefined())
        return getPropertyFrom(vm, targetOf(ps), receiver, key);
    JSValue callArgs[3] = {ps->target, keyValueOf(key), receiver};
    JSValue r = vm.call(trap, ps->handler, callArgs, 3);
    if (r.isException())
        return r;
    Handle result = Handle::adopt(r);
    PropDesc targetDesc;
    const int has = objGetOwnProperty(vm, targetOf(ps), key, &targetDesc);
    if (has < 0)
        return JSValue::exception();
    if (has && !targetDesc.configurable) {
        if (!targetDesc.isAccessor() && !targetDesc.writable && !sameValueOfPair(r, targetDesc.value.get()))
            return fail(vm, "'get' on proxy: property is a read-only and non-configurable data property on the proxy target but the proxy did not return its actual value");
        if (targetDesc.isAccessor() && targetDesc.get.get().isUndefined() && !r.isUndefined())
            return fail(vm, "'get' on proxy: property is a non-configurable accessor property on the proxy target and does not have a getter function, but the trap did not return 'undefined'");
    }
    return result.take();
}

bool proxySet(VM& vm, JSObject* proxy, Key key, JSValue value, JSValue receiver, bool strict)
{
    const int r = proxySetProperty(vm, proxy, key, value, receiver);
    if (r < 0)
        return false;
    if (r == 0 && strict) {
        vm.throwError(ErrorCode::TypeError, "'set' on proxy: trap returned falsish");
        return false;
    }
    return r == 1;
}

int proxyHas(VM& vm, JSObject* proxy, Key key)
{
    return proxyHasProperty(vm, proxy, key);
}

int proxyDelete(VM& vm, JSObject* proxy, Key key, bool strict)
{
    const int r = proxyDeleteProperty(vm, proxy, key);
    if (r == 0 && strict) {
        vm.throwError(ErrorCode::TypeError, "'deleteProperty' on proxy: trap returned falsish");
        return -1;
    }
    return r;
}

}  // namespace vm
}  // namespace internal
}  // namespace script
