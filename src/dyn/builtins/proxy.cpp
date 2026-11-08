// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "script/internal/dyn/proxy.hpp"

#include <algorithm>

#include "script/internal/dyn/builtins.hpp"
#include "script/internal/dyn/numconv.hpp"
#include "script/internal/dyn/vm.hpp"

namespace script {
namespace internal {
namespace dyn {

bool ordinaryGetOwn(Runtime& rt, DynObject* o, GCObject* key, Descriptor& out);
bool ordinaryDefineOwn(Runtime& rt, DynObject* o, GCObject* key, const Descriptor& d);

namespace {

Value objectValue(DynObject* o) { return detail::Access::make(Type::Object, o); }
Value keyValue(GCObject* key) { return detail::Access::make(key->kind == CellKind::Symbol ? Type::Symbol : Type::String, key); }

[[noreturn]] void invariant(Runtime& rt, const char* what) { throwError(rt, ErrorCode::TypeError, std::string("a proxy's handler broke an invariant: ") + what); }

/// The handler and target, or a TypeError for a revoked proxy; and the trap of that name, undefined when the handler has
/// none.
struct Trap
{
    Value handler;
    DynObject* target = nullptr;
    Value method;
};

Trap trapOf(Runtime& rt, DynProxy* p, const char* name)
{
    if (p->handler.isNull() || p->handler.isUndefined())
        throwError(rt, ErrorCode::TypeError, std::string("a revoked proxy cannot be used (") + name + ")");
    Trap t;
    t.handler = p->handler;
    t.target = asObject(p->target);
    t.method = getProperty(rt, t.handler, rt.intern(name));
    if (t.method.isNullish())
        t.method = Value::undefined();
    else if (!asFunction(t.method))
        throwError(rt, ErrorCode::TypeError, std::string("the proxy's ") + name + " trap is not a function");
    return t;
}

bool isCompatible(bool extensible, const Descriptor& wanted, bool hasCurrent, const Descriptor& current)
{
    if (!hasCurrent)
        return extensible;
    if (current.configurable)
        return true;
    if (wanted.hasConfigurable && wanted.configurable)
        return false;
    if (wanted.hasEnumerable && wanted.enumerable != current.enumerable)
        return false;
    if (wanted.isAccessor() != current.isAccessor() && (wanted.isAccessor() || wanted.isData()))
        return false;
    if (current.isAccessor()) {
        if (wanted.hasGet && !sameValue(wanted.getter, current.getter))
            return false;
        if (wanted.hasSet && !sameValue(wanted.setter, current.setter))
            return false;
        return true;
    }
    if (!current.writable) {
        if (wanted.hasWritable && wanted.writable)
            return false;
        if (wanted.hasValue && !sameValue(wanted.value, current.value))
            return false;
    }
    return true;
}

void completeDescriptor(Descriptor& d)
{
    if (!d.isAccessor()) {
        d.hasValue = d.hasWritable = true;
    } else {
        d.hasGet = d.hasSet = true;
    }
    d.hasEnumerable = d.hasConfigurable = true;
}

}  // namespace

void DynProxy::children(GcList& out) const
{
    DynFunction::children(out);
    if (GCObject* p = detail::Access::ptr(target))
        out.push_back(p);
    if (GCObject* p = detail::Access::ptr(handler))
        out.push_back(p);
}

void DynProxy::clearReferences()
{
    DynFunction::clearReferences();
    target = Value::undefined();
    handler = Value::undefined();
}

DynProxy* asProxy(const DynObject* o) noexcept
{
    if (!o)
        return nullptr;
    if (o->cls == ObjClass::Proxy || (o->cls == ObjClass::Function && static_cast<const DynFunction*>(o)->fnKind == FnKind::Proxy))
        return static_cast<DynProxy*>(const_cast<DynObject*>(o));
    return nullptr;
}

Value descriptorToObject(Runtime& rt, const Descriptor& d)
{
    DynObject* out = rt.newObject(rt.objectProto.get());
    Value result = detail::Access::adopt(Type::Object, out);
    if (d.hasValue)
        out->putOwn(rt.atom.value.get(), d.value);
    if (d.hasWritable)
        out->putOwn(rt.intern("writable"), Value(d.writable));
    if (d.hasGet)
        out->putOwn(rt.intern("get"), d.getter);
    if (d.hasSet)
        out->putOwn(rt.intern("set"), d.setter);
    if (d.hasEnumerable)
        out->putOwn(rt.intern("enumerable"), Value(d.enumerable));
    if (d.hasConfigurable)
        out->putOwn(rt.intern("configurable"), Value(d.configurable));
    return result;
}

// ---- the operations

bool getOwnProperty(Runtime& rt, DynObject* o, GCObject* key, Descriptor& out)
{
    DynProxy* p = asProxy(o);
    if (!p)
        return ordinaryGetOwn(rt, o, key, out);
    const Trap t = trapOf(rt, p, "getOwnPropertyDescriptor");
    if (t.method.isUndefined())
        return getOwnProperty(rt, t.target, key, out);
    const Value result = callValue(rt, t.method, t.handler, {p->target, keyValue(key)});
    if (!result.isObject() && !result.isUndefined())
        throwError(rt, ErrorCode::TypeError, "getOwnPropertyDescriptor must return an object or undefined");
    Descriptor current;
    const bool hasCurrent = getOwnProperty(rt, t.target, key, current);
    if (result.isUndefined()) {
        if (!hasCurrent)
            return false;
        if (!current.configurable)
            invariant(rt, "a property it cannot remove was reported missing");
        if (!isExtensible(rt, t.target))
            invariant(rt, "a property of an object that cannot grow was reported missing");
        return false;
    }
    const bool extensible = isExtensible(rt, t.target);
    Descriptor d = toDescriptor(rt, result);
    completeDescriptor(d);
    if (!isCompatible(extensible, d, hasCurrent, current))
        invariant(rt, "the descriptor reported does not fit the target's");
    if (!d.configurable) {
        if (!hasCurrent || current.configurable)
            invariant(rt, "a property reported fixed is not fixed on the target");
        if (d.hasWritable && !d.writable && current.writable)
            invariant(rt, "a property reported read-only is writable on the target");
    }
    out = d;
    return true;
}

bool defineOwnProperty(Runtime& rt, DynObject* o, GCObject* key, const Descriptor& d)
{
    DynProxy* p = asProxy(o);
    if (!p)
        return ordinaryDefineOwn(rt, o, key, d);
    const Trap t = trapOf(rt, p, "defineProperty");
    if (t.method.isUndefined())
        return defineOwnProperty(rt, t.target, key, d);
    if (!toBoolean(callValue(rt, t.method, t.handler, {p->target, keyValue(key), descriptorToObject(rt, d)})))
        return false;
    Descriptor current;
    const bool hasCurrent = getOwnProperty(rt, t.target, key, current);
    const bool extensible = isExtensible(rt, t.target);
    const bool settingFixed = d.hasConfigurable && !d.configurable;
    if (!hasCurrent) {
        if (!extensible)
            invariant(rt, "a property was added to an object that cannot grow");
        if (settingFixed)
            invariant(rt, "a fixed property was defined that the target lacks");
        return true;
    }
    if (!isCompatible(extensible, d, true, current))
        invariant(rt, "the property defined does not fit the target's");
    if (settingFixed && current.configurable)
        invariant(rt, "a property defined fixed is not fixed on the target");
    if (!current.isAccessor() && !current.configurable && current.writable && d.hasWritable && !d.writable)
        invariant(rt, "a property defined read-only is writable on the target");
    return true;
}

Value getPrototypeOf(Runtime& rt, DynObject* o)
{
    DynProxy* p = asProxy(o);
    if (!p)
        return o->proto ? objectValue(o->proto.get()) : Value::null();
    const Trap t = trapOf(rt, p, "getPrototypeOf");
    if (t.method.isUndefined())
        return getPrototypeOf(rt, t.target);
    const Value result = callValue(rt, t.method, t.handler, {p->target});
    if (!result.isObject() && !result.isNull())
        throwError(rt, ErrorCode::TypeError, "getPrototypeOf must return an object or null");
    if (!isExtensible(rt, t.target) && !sameValue(result, getPrototypeOf(rt, t.target)))
        invariant(rt, "the prototype reported is not the fixed target's");
    return result;
}

bool setPrototypeOf(Runtime& rt, DynObject* o, DynObject* proto)
{
    DynProxy* p = asProxy(o);
    if (!p) {
        if (o->proto.get() == proto)
            return true;
        // Object.prototype's prototype is null for good: an immutable prototype exotic object.
        if (!o->extensible || o == rt.objectProto.get())
            return false;
        for (DynObject* up = proto; up; up = up->proto.get()) {
            if (up == o)
                return false;
            if (asProxy(up))
                break;
        }
        o->setProto(proto);
        return true;
    }
    const Trap t = trapOf(rt, p, "setPrototypeOf");
    if (t.method.isUndefined())
        return setPrototypeOf(rt, t.target, proto);
    if (!toBoolean(callValue(rt, t.method, t.handler, {p->target, proto ? objectValue(proto) : Value::null()})))
        return false;
    if (!isExtensible(rt, t.target) && asObject(getPrototypeOf(rt, t.target)) != proto)
        invariant(rt, "the prototype set is not the fixed target's");
    return true;
}

bool isExtensible(Runtime& rt, DynObject* o)
{
    DynProxy* p = asProxy(o);
    if (!p)
        return o->extensible;
    const Trap t = trapOf(rt, p, "isExtensible");
    if (t.method.isUndefined())
        return isExtensible(rt, t.target);
    const bool answer = toBoolean(callValue(rt, t.method, t.handler, {p->target}));
    if (answer != isExtensible(rt, t.target))
        invariant(rt, "isExtensible must answer as the target does");
    return answer;
}

bool preventExtensions(Runtime& rt, DynObject* o)
{
    DynProxy* p = asProxy(o);
    if (!p) {
        o->extensible = false;
        o->dropShape();
        return true;
    }
    const Trap t = trapOf(rt, p, "preventExtensions");
    if (t.method.isUndefined())
        return preventExtensions(rt, t.target);
    const bool answer = toBoolean(callValue(rt, t.method, t.handler, {p->target}));
    if (answer && isExtensible(rt, t.target))
        invariant(rt, "preventExtensions succeeded on a target that can still grow");
    return answer;
}

Vec<Value> ownPropertyKeys(Runtime& rt, DynObject* o)
{
    DynProxy* p = asProxy(o);
    if (!p)
        return ownKeys(rt, o, false, true);
    const Trap t = trapOf(rt, p, "ownKeys");
    if (t.method.isUndefined())
        return ownPropertyKeys(rt, t.target);
    const Value result = callValue(rt, t.method, t.handler, {p->target});
    if (!result.isObject())
        throwError(rt, ErrorCode::TypeError, "ownKeys must return an object");
    Vec<Value> keys;
    const double length = lengthOfLike(rt, result);
    for (double i = 0; i < length; ++i) {
        const Value k = getProperty(rt, result, rt.intern(numberToString(i)));
        if (!k.isString() && !k.isSymbol())
            throwError(rt, ErrorCode::TypeError, "ownKeys may only return strings and symbols");
        for (const Value& seen : keys) {
            if (strictEquals(seen, k))
                throwError(rt, ErrorCode::TypeError, "ownKeys returned a key twice");
        }
        keys.push_back(k);
    }
    const bool extensible = isExtensible(rt, t.target);
    const Vec<Value> targetKeys = ownPropertyKeys(rt, t.target);
    Vec<Value> remaining = keys;
    const auto take = [&](const Value& k) {
        for (auto it = remaining.begin(); it != remaining.end(); ++it) {
            if (strictEquals(*it, k)) {
                remaining.erase(it);
                return true;
            }
        }
        return false;
    };
    for (const Value& k : targetKeys) {
        Ref<GCObject> key = toPropertyKey(rt, k);
        Descriptor d;
        const bool fixed = getOwnProperty(rt, t.target, key.get(), d) && !d.configurable;
        if (!take(k) && (fixed || !extensible))
            invariant(rt, "ownKeys left out a key it must report");
    }
    if (!extensible && !remaining.empty())
        invariant(rt, "ownKeys reported a key a fixed target does not have");
    return keys;
}

bool isArray(Runtime& rt, const Value& v)
{
    if (asArray(v))
        return true;
    if (DynProxy* p = asProxy(asObject(v))) {
        if (p->handler.isNull())
            throwError(rt, ErrorCode::TypeError, "a revoked proxy cannot be used");
        return isArray(rt, p->target);
    }
    return false;
}

// ---- the VM's paths

bool setWithReceiver(Runtime& rt, DynObject* o, GCObject* key, const Value& v, const Value& receiver)
{
    if (DynProxy* p = asProxy(o))
        return proxySet(rt, p, key, v, receiver);
    // Nothing unusual about it: the quick way, which is the same.
    if (receiver.isObject() && asObject(receiver) == o && !asProxy(o)) {
        Descriptor own;
        if (getOwnProperty(rt, o, key, own) && own.isData() && !own.writable)
            return false;
        if (!own.isAccessor() && !getOwnProperty(rt, o, key, own) && !o->extensible) {
            bool inherited = false;
            for (Value up = getPrototypeOf(rt, o); up.isObject() && !inherited; up = getPrototypeOf(rt, asObject(up))) {
                Descriptor d;
                inherited = getOwnProperty(rt, asObject(up), key, d);
            }
            if (!inherited)
                return false;
        }
        setProperty(rt, receiver, key, v);
        return true;
    }
    // OrdinarySet: find the property up the chain, then write to the receiver.
    Descriptor d;
    bool found = getOwnProperty(rt, o, key, d);
    if (!found) {
        const Value parent = getPrototypeOf(rt, o);
        if (parent.isObject())
            return setWithReceiver(rt, asObject(parent), key, v, receiver);
        d = Descriptor();
        d.hasValue = d.hasWritable = d.hasEnumerable = d.hasConfigurable = true;
        d.writable = d.enumerable = d.configurable = true;
    }
    if (d.isAccessor()) {
        if (d.setter.isUndefined())
            return false;
        callValue(rt, d.setter, receiver, {v});
        return true;
    }
    if (!d.writable || !receiver.isObject())
        return false;
    DynObject* target = asObject(receiver);
    Descriptor existing;
    if (getOwnProperty(rt, target, key, existing)) {
        if (existing.isAccessor() || !existing.writable)
            return false;
        Descriptor change;
        change.hasValue = true;
        change.value = v;
        return defineOwnProperty(rt, target, key, change);
    }
    Descriptor fresh;
    fresh.hasValue = fresh.hasWritable = fresh.hasEnumerable = fresh.hasConfigurable = true;
    fresh.value = v;
    fresh.writable = fresh.enumerable = fresh.configurable = true;
    return defineOwnProperty(rt, target, key, fresh);
}

Value proxyGet(Runtime& rt, DynProxy* p, GCObject* key, const Value& receiver)
{
    const Trap t = trapOf(rt, p, "get");
    if (t.method.isUndefined())
        return getPropertyOfObject(rt, t.target, key, receiver);
    const Value result = callValue(rt, t.method, t.handler, {p->target, keyValue(key), receiver});
    Descriptor d;
    if (getOwnProperty(rt, t.target, key, d) && !d.configurable) {
        if (d.isData() && !d.writable && !sameValue(result, d.value))
            invariant(rt, "get reported another value for a fixed read-only property");
        if (d.isAccessor() && d.getter.isUndefined() && !result.isUndefined())
            invariant(rt, "get reported a value for a fixed property with no getter");
    }
    return result;
}

bool proxySet(Runtime& rt, DynProxy* p, GCObject* key, const Value& v, const Value& receiver)
{
    const Trap t = trapOf(rt, p, "set");
    if (t.method.isUndefined())
        return setWithReceiver(rt, t.target, key, v, receiver);
    if (!toBoolean(callValue(rt, t.method, t.handler, {p->target, keyValue(key), v, receiver})))
        return false;
    Descriptor d;
    if (getOwnProperty(rt, t.target, key, d) && !d.configurable) {
        if (d.isData() && !d.writable && !sameValue(v, d.value))
            invariant(rt, "set changed a fixed read-only property");
        if (d.isAccessor() && d.setter.isUndefined())
            invariant(rt, "set wrote a fixed property with no setter");
    }
    return true;
}

bool proxyHas(Runtime& rt, DynProxy* p, GCObject* key)
{
    const Trap t = trapOf(rt, p, "has");
    if (t.method.isUndefined())
        return hasProperty(rt, objectValue(t.target), key);
    const bool answer = toBoolean(callValue(rt, t.method, t.handler, {p->target, keyValue(key)}));
    if (!answer) {
        Descriptor d;
        if (getOwnProperty(rt, t.target, key, d)) {
            if (!d.configurable)
                invariant(rt, "has hid a fixed property");
            if (!isExtensible(rt, t.target))
                invariant(rt, "has hid a property of an object that cannot grow");
        }
    }
    return answer;
}

bool proxyDelete(Runtime& rt, DynProxy* p, GCObject* key)
{
    const Trap t = trapOf(rt, p, "deleteProperty");
    if (t.method.isUndefined())
        return deleteProperty(rt, objectValue(t.target), key);
    if (!toBoolean(callValue(rt, t.method, t.handler, {p->target, keyValue(key)})))
        return false;
    Descriptor d;
    if (getOwnProperty(rt, t.target, key, d)) {
        if (!d.configurable)
            invariant(rt, "deleteProperty removed a fixed property");
        if (!isExtensible(rt, t.target))
            invariant(rt, "deleteProperty removed a property of an object that cannot grow");
    }
    return true;
}

Value proxyCall(Runtime& rt, DynProxy* p, const Value& self, const std::vector<Value>& args)
{
    const Trap t = trapOf(rt, p, "apply");
    if (t.method.isUndefined())
        return callValue(rt, p->target, self, args);
    return callValue(rt, t.method, t.handler, {p->target, self, rt.makeArray(args)});
}

Value proxyConstruct(Runtime& rt, DynProxy* p, const std::vector<Value>& args, DynFunction* newTarget)
{
    const Trap t = trapOf(rt, p, "construct");
    DynFunction* nt = newTarget ? newTarget : p;
    if (t.method.isUndefined())
        return constructValue(rt, p->target, args, nt == p ? asFunction(p->target) : nt);
    const Value result = callValue(rt, t.method, t.handler, {p->target, rt.makeArray(args), objectValue(nt)});
    if (!result.isObject())
        throwError(rt, ErrorCode::TypeError, "construct must return an object");
    return result;
}

// ---- Proxy itself

namespace {

Value makeProxy(Runtime& rt, const Value& target, const Value& handler)
{
    if (!target.isObject() || !handler.isObject())
        throwError(rt, ErrorCode::TypeError, "a proxy needs an object for its target and its handler");
    auto* p = rt.newCell<DynProxy>();
    Value out = detail::Access::adopt(Type::Object, p);
    p->fnKind = FnKind::Proxy;
    DynFunction* callable = asFunction(target);
    if (!callable)
        p->cls = ObjClass::Proxy;
    p->constructible = callable && isConstructor(rt, callable);
    p->isNativeConstructor = p->constructible;
    p->target = target;
    p->handler = handler;
    return out;
}

}  // namespace

void installProxy(Runtime& rt)
{
    // Proxy has no prototype property: proxies have whatever prototype their handler says.
    Value ctorValue = rt.makeFunction(ascii("Proxy"), [](CallInfo& ci) -> Value {
        if (!ci.isConstructorCall())
            ci.fail(ErrorCode::TypeError, "Proxy must be called with 'new'");
        return makeProxy(rtOf(ci), ci.arg(0), ci.arg(1));
    }, 2);
    DynFunction* ctor = asFunction(ctorValue);
    ctor->isNativeConstructor = true;
    rt.globalObject->putOwn(rt.intern("Proxy"), ctorValue, PropWritable | PropConfigurable);
    rt.method(ctor, "revocable", [](CallInfo& ci) -> Value {
        Runtime& rt2 = rtOf(ci);
        const Value proxy = makeProxy(rt2, ci.arg(0), ci.arg(1));
        DynObject* out = rt2.newObject(rt2.objectProto.get());
        Value result = detail::Access::adopt(Type::Object, out);
        out->putOwn(rt2.intern("proxy"), proxy);
        auto revoked = std::make_shared<Value>(proxy);
        out->putOwn(rt2.intern("revoke"), rt2.makeFunction(String(), [revoked](CallInfo&) -> Value {
            if (DynProxy* p = asProxy(asObject(*revoked))) {
                p->handler = Value::null();
                p->target = Value::null();
            }
            *revoked = Value::undefined();
            return Value::undefined();
        }, 0));
        return result;
    }, 2);
}

}  // namespace dyn
}  // namespace internal
}  // namespace script
