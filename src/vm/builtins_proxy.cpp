// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Proxy: an object whose property operations go to its handler's traps, and to its target where there is no trap.
#include "builtins_util.hpp"
#include "script/internal/dyn/object.hpp"

namespace script {
namespace internal {
namespace vm {
namespace bi {

namespace {

JSValue keyValue(Key k) { return JSValue::cell(k); }

/// The trap `name` of the proxy's handler: a function, undefined for none (the operation goes to the target), or the
/// exception sentinel. A revoked proxy is a TypeError.
JSValue trapOf(VM& vm, ProxyState* ps, const char* name)
{
    if (!isObject(ps->handler))
        return vm.throwError(ErrorCode::TypeError, std::string("Cannot perform '") + name + "' on a proxy that has been revoked");
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
        return vm.throwError(ErrorCode::TypeError, std::string("proxy trap '") + name + "' is not a function");
    }
    return trap;
}

}  // namespace

}  // namespace bi

using namespace bi;

ProxyState* proxyOf(JSObject* o) noexcept
{
    if (o->cls() == ObjClass::Proxy || (o->cls() == ObjClass::Function && (o->h.flags & cellflags::kHasRare))) {
        Rare* r = o->rare();
        if (r && r->ext)
            return dynamic_cast<ProxyState*>(r->ext);
    }
    return nullptr;
}

JSValue proxyGet(VM& vm, JSObject* proxy, Key key, JSValue receiver)
{
    ProxyState* ps = proxyOf(proxy);
    JSValue trap = trapOf(vm, ps, "get");
    if (trap.isException())
        return trap;
    Handle trapKeep = Handle::adopt(trap);
    if (trap.isUndefined()) {
        return getPropertyFrom(vm, asObject(ps->target), receiver, key);
    }
    JSValue callArgs[3] = {ps->target, keyValue(key), receiver};
    JSValue r = vm.call(trap, ps->handler, callArgs, 3);
    if (r.isException() || !isObject(ps->target))
        return r;
    // The one invariant that matters in practice: a fixed data property of the target reads as it is.
    std::uint8_t attrs = 0;
    if (JSValue* slot = asObject(ps->target)->findOwn(vm, key, &attrs)) {
        if (!(attrs & attr::kConfigurable) && !(attrs & attr::kAccessor) && !(attrs & attr::kWritable) && !sameValueZero(*slot, r)) {
            release(r);
            return vm.throwError(ErrorCode::TypeError, "proxy get trap returned a different value for a non-configurable, non-writable property");
        }
    }
    return r;
}

bool proxySet(VM& vm, JSObject* proxy, Key key, JSValue value, JSValue receiver, bool strict)
{
    ProxyState* ps = proxyOf(proxy);
    JSValue trap = trapOf(vm, ps, "set");
    if (trap.isException())
        return false;
    Handle trapKeep = Handle::adopt(trap);
    if (trap.isUndefined())
        return setProperty(vm, ps->target, key, value, strict);
    JSValue callArgs[4] = {ps->target, keyValue(key), value, receiver};
    JSValue r = vm.call(trap, ps->handler, callArgs, 4);
    if (r.isException())
        return false;
    const bool ok = toBoolean(r);
    release(r);
    if (!ok && strict) {
        vm.throwError(ErrorCode::TypeError, "'set' on proxy: trap returned falsish");
        return false;
    }
    return ok;
}

int proxyHas(VM& vm, JSObject* proxy, Key key)
{
    ProxyState* ps = proxyOf(proxy);
    JSValue trap = trapOf(vm, ps, "has");
    if (trap.isException())
        return -1;
    Handle trapKeep = Handle::adopt(trap);
    if (trap.isUndefined())
        return hasProperty(vm, ps->target, key) ? 1 : (vm.exception.isUndefined() ? 0 : -1);
    JSValue callArgs[2] = {ps->target, keyValue(key)};
    JSValue r = vm.call(trap, ps->handler, callArgs, 2);
    if (r.isException())
        return -1;
    const bool ok = toBoolean(r);
    release(r);
    return ok;
}

int proxyDelete(VM& vm, JSObject* proxy, Key key, bool strict)
{
    ProxyState* ps = proxyOf(proxy);
    JSValue trap = trapOf(vm, ps, "deleteProperty");
    if (trap.isException())
        return -1;
    Handle trapKeep = Handle::adopt(trap);
    if (trap.isUndefined()) {
        const bool ok = deleteProperty(vm, ps->target, key, strict);
        return ok ? 1 : (vm.exception.isUndefined() ? 0 : -1);
    }
    JSValue callArgs[2] = {ps->target, keyValue(key)};
    JSValue r = vm.call(trap, ps->handler, callArgs, 2);
    if (r.isException())
        return -1;
    const bool ok = toBoolean(r);
    release(r);
    if (!ok && strict) {
        vm.throwError(ErrorCode::TypeError, "'deleteProperty' on proxy: trap returned falsish");
        return -1;
    }
    return ok;
}

namespace bi {

bool proxyOwnKeys(VM& vm, JSObject* proxy, std::vector<Handle>& out)
{
    ProxyState* ps = proxyOf(proxy);
    JSValue trap = trapOf(vm, ps, "ownKeys");
    if (trap.isException())
        return false;
    Handle trapKeep = Handle::adopt(trap);
    if (trap.isUndefined()) {
        if (!isObject(ps->target))
            return true;
        ownKeys(vm, asObject(ps->target), true, true, false, out);
        return true;
    }
    JSValue r = vm.call(trap, ps->handler, &ps->target, 1);
    if (r.isException())
        return false;
    Handle rKeep = Handle::adopt(r);
    if (!isObject(r)) {
        vm.throwError(ErrorCode::TypeError, "CreateListFromArrayLike called on non-object");
        return false;
    }
    JSValue len = getProperty(vm, r, vm.atoms.length);
    if (len.isException())
        return false;
    double n = 0;
    const bool ok = toNumber(vm, len, &n);
    release(len);
    if (!ok)
        return false;
    for (std::uint32_t i = 0; i < std::uint32_t(n); ++i) {
        JSValue k = getIndex(vm, r, i);
        if (k.isException())
            return false;
        if (!isString(k) && !isSymbol(k)) {
            release(k);
            vm.throwError(ErrorCode::TypeError, "proxy [[OwnPropertyKeys]] must return an array with only string and symbol elements");
            return false;
        }
        out.push_back(Handle::adopt(k));
    }
    return true;
}

JSValue proxyOwnDescriptor(VM& vm, JSObject* proxy, Key key)
{
    ProxyState* ps = proxyOf(proxy);
    JSValue trap = trapOf(vm, ps, "getOwnPropertyDescriptor");
    if (trap.isException())
        return trap;
    Handle trapKeep = Handle::adopt(trap);
    if (trap.isUndefined()) {
        if (!isObject(ps->target))
            return JSValue::undefined();
        return ownDescriptorObject(vm, asObject(ps->target), key);
    }
    JSValue callArgs[2] = {ps->target, keyValue(key)};
    return vm.call(trap, ps->handler, callArgs, 2);
}

JSValue proxyDefine(VM& vm, JSObject* proxy, Key key, JSObject* descriptor)
{
    ProxyState* ps = proxyOf(proxy);
    JSValue trap = trapOf(vm, ps, "defineProperty");
    if (trap.isException())
        return trap;
    Handle trapKeep = Handle::adopt(trap);
    if (trap.isUndefined())
        return definePropertyFromDescriptor(vm, asObject(ps->target), key, descriptor);
    JSValue callArgs[3] = {ps->target, keyValue(key), toJSValue(descriptor)};
    JSValue r = vm.call(trap, ps->handler, callArgs, 3);
    if (r.isException())
        return r;
    const bool ok = toBoolean(r);
    release(r);
    if (!ok)
        return vm.throwError(ErrorCode::TypeError, "'defineProperty' on proxy: trap returned falsish");
    return JSValue::undefined();
}

namespace {

/// What a callable proxy runs when called or constructed.
NATIVE(proxyCall)
{
    UNUSED();
    auto* me = static_cast<JSFunction*>(data);
    ProxyState* ps = proxyOf(me);
    const bool construct = !vm.newTarget.isUndefined();
    JSValue trap = trapOf(vm, ps, construct ? "construct" : "apply");
    if (trap.isException())
        return trap;
    Handle trapKeep = Handle::adopt(trap);
    if (trap.isUndefined()) {
        if (construct) {
            JSValue r = vm.construct(ps->target, args, argc);
            return r;
        }
        return vm.call(ps->target, self, args, argc);
    }
    JSArray* list = vm.newArray(vm.protos.array, 0, argc);
    Handle listKeep = Handle::adopt(toJSValue(list));
    for (std::uint32_t i = 0; i < argc; ++i) {
        retain(args[i]);
        list->append(vm, args[i]);
    }
    if (construct) {
        JSValue callArgs[3] = {ps->target, listKeep.get(), toJSValue(me)};
        JSValue r = vm.call(trap, ps->handler, callArgs, 3);
        if (r.isException())
            return r;
        if (!isObject(r)) {
            release(r);
            return typeError(vm, "proxy [[Construct]] must return an object");
        }
        return r;
    }
    JSValue callArgs[3] = {ps->target, self, listKeep.get()};
    return vm.call(trap, ps->handler, callArgs, 3);
}

JSValue makeProxy(VM& vm, JSValue target, JSValue handler)
{
    if (!isObject(target) || !isObject(handler))
        return typeError(vm, "Cannot create proxy with a non-object as target or handler");
    auto* state = new ProxyState();
    retain(target);
    retain(handler);
    state->target = target;
    state->handler = handler;
    JSObject* proto = asObject(target)->proto(vm);
    JSObject* o;
    if (isFunction(target)) {
        auto* f = vm.newNativeFunction(vm.protos.function, proxyCall, nullptr, nullptr, 0);
        f->rareOf(vm).nativeData = f;
        if (static_cast<JSFunction*>(asObject(target))->flags & JSFunction::kNative
                ? (static_cast<JSFunction*>(asObject(target))->flags & JSFunction::kConstructor) != 0
                : static_cast<JSFunction*>(asObject(target))->code->constructor)
            f->flags |= JSFunction::kConstructor;
        f->flags &= ~JSFunction::kLazyProps;
        o = f;
    } else {
        o = vm.newObject(static_cast<JSObject*>(nullptr));
        o->h.cls = std::uint8_t(ObjClass::Proxy);
    }
    Handle keep = Handle::adopt(toJSValue(o));
    delete o->rareOf(vm).ext;
    o->rareOf(vm).ext = state;
    if (proto)
        o->setProto(vm, proto);
    return keep.take();
}

NATIVE(proxyCtor)
{
    UNUSED();
    if (vm.newTarget.isUndefined())
        return typeError(vm, "Constructor Proxy requires 'new'");
    return makeProxy(vm, arg(args, argc, 0), arg(args, argc, 1));
}

NATIVE(proxyRevoke)
{
    UNUSED();
    Rare& r = *static_cast<JSFunction*>(data)->rare();
    if (isObject(r.boundTarget)) {
        if (ProxyState* ps = proxyOf(asObject(r.boundTarget))) {
            release(ps->handler);
            ps->handler = JSValue::null();
        }
        release(r.boundTarget);
        r.boundTarget = JSValue::undefined();
    }
    return JSValue::undefined();
}

NATIVE(proxyRevocable)
{
    UNUSED();
    JSValue proxy = makeProxy(vm, arg(args, argc, 0), arg(args, argc, 1));
    if (proxy.isException())
        return proxy;
    Handle proxyKeep = Handle::adopt(proxy);
    JSFunction* revoke = vm.newNativeFunction(vm.protos.function, proxyRevoke, nullptr, nullptr, 0);
    Handle revokeKeep = Handle::adopt(toJSValue(revoke));
    revoke->rareOf(vm).nativeData = revoke;
    retain(proxy);
    revoke->rareOf(vm).boundTarget = proxy;
    JSObject* result = vm.newObject(vm.protos.object);
    Handle resultKeep = Handle::adopt(toJSValue(result));
    Key pk = vm.internKey(std::string_view("proxy")), rk = vm.internKey(std::string_view("revoke"));
    retain(proxy);
    result->putOwn(vm, pk, proxy, attr::kDefault);
    retain(revokeKeep.get());
    result->putOwn(vm, rk, revokeKeep.get(), attr::kDefault);
    detail::decRef(pk);
    detail::decRef(rk);
    return resultKeep.take();
}

}  // namespace

void installProxy(VM& vm)
{
    JSObject* proto = vm.newObject(vm.protos.object);
    JSFunction* ctor = constructor(vm, "Proxy", proxyCtor, 2, proto);
    // `Proxy` has no `prototype`.
    {
        Key k = vm.atoms.prototype;
        ctor->deleteOwn(vm, k);
    }
    method(vm, ctor, "revocable", proxyRevocable, 2);
    release(toJSValue(proto));
}

}  // namespace bi
}  // namespace vm
}  // namespace internal
}  // namespace script
