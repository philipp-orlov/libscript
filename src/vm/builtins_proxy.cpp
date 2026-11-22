// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Proxy: an object whose property operations go to its handler's traps, and to its target where there is no trap.
#include "builtins_util.hpp"
#include "script/internal/dyn/object.hpp"

namespace script {
namespace internal {
namespace vm {
namespace bi {

}  // namespace bi

using namespace bi;

ProxyState* proxyOf(JSObject* o) noexcept
{
    if (o->cls() == ObjClass::Proxy || (o->cls() == ObjClass::Function && (o->h.flags & cellflags::kHasRare))) {
        Rare* r = o->rare();
        if (r && r->ext)
            return typeid(*r->ext) == typeid(ProxyState) ? static_cast<ProxyState*>(r->ext) : nullptr;
    }
    return nullptr;
}

namespace bi {

namespace {

/// What a callable proxy runs when called or constructed.
NATIVE(proxyCall)
{
    UNUSED();
    auto* me = static_cast<JSFunction*>(data);
    ProxyState* ps = proxyOf(me);
    const bool construct = !vm.newTarget.isUndefined();
    JSValue trap = proxyTrap(vm, ps, construct ? "construct" : "apply");
    if (trap.isException())
        return trap;
    Handle trapKeep = Handle::adopt(trap);
    if (trap.isUndefined()) {
        if (construct) {
            return vm.construct(ps->target, args, argc, vm.newTarget);
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
        JSValue callArgs[3] = {ps->target, listKeep.get(), vm.newTarget};
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
    // In dictionary mode no inline cache can match its shape, so every read and write of a proxy reaches its traps.
    o->toDictionary(vm);
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
        ctor->materialize(vm);
        ctor->setAttrs(vm, vm.atoms.prototype, attr::kConfigurable);
        ctor->deleteOwn(vm, vm.atoms.prototype);
    }
    method(vm, ctor, "revocable", proxyRevocable, 2);
    release(toJSValue(proto));
}

}  // namespace bi
}  // namespace vm
}  // namespace internal
}  // namespace script
