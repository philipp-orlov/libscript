// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Proxy, and the object operations every object answers: an ordinary one from its own table, a proxy through its handler.
#pragma once

#include "script/internal/dyn/runtime.hpp"

namespace script {
namespace internal {
namespace dyn {

/// A proxy. It is a function cell so that one over a function is called and constructed like one; over anything else
/// it is no function at all, and says so with its class.
struct DynProxy : DynFunction
{
    Value target = Value::undefined();
    Value handler = Value::undefined();  ///< null once revoked
    bool constructible = false;

    void children(GcList& out) const override;
    void clearReferences() override;
};

DynProxy* asProxy(const DynObject* o) noexcept;

struct Descriptor
{
    bool hasValue = false, hasWritable = false, hasGet = false, hasSet = false, hasEnumerable = false, hasConfigurable = false;
    bool writable = false, enumerable = false, configurable = false;
    Value value = Value::undefined(), getter = Value::undefined(), setter = Value::undefined();
    bool isAccessor() const noexcept { return hasGet || hasSet; }
    bool isData() const noexcept { return hasValue || hasWritable; }
};

Descriptor toDescriptor(Runtime& rt, const Value& source);
Value descriptorToObject(Runtime& rt, const Descriptor& d);

// ---- the object operations, a proxy's handler answering for it
bool getOwnProperty(Runtime& rt, DynObject* o, GCObject* key, Descriptor& out);
bool defineOwnProperty(Runtime& rt, DynObject* o, GCObject* key, const Descriptor& d);
/// The prototype, or null: a value, since a trap may make one nothing else holds.
Value getPrototypeOf(Runtime& rt, DynObject* o);
bool setPrototypeOf(Runtime& rt, DynObject* o, DynObject* proto);
bool isExtensible(Runtime& rt, DynObject* o);
bool preventExtensions(Runtime& rt, DynObject* o);
/// Every own key, strings and symbols.
Vec<Value> ownPropertyKeys(Runtime& rt, DynObject* o);
/// IsArray, which sees through proxies.
bool isArray(Runtime& rt, const Value& v);

/// [[Set]] with a receiver that may be another object: what a proxy with no `set` trap and `Reflect.set` do.
bool setWithReceiver(Runtime& rt, DynObject* o, GCObject* key, const Value& v, const Value& receiver);

// ---- where the VM's own paths hand over to a proxy
Value proxyGet(Runtime& rt, DynProxy* p, GCObject* key, const Value& receiver);
bool proxySet(Runtime& rt, DynProxy* p, GCObject* key, const Value& v, const Value& receiver);
bool proxyHas(Runtime& rt, DynProxy* p, GCObject* key);
bool proxyDelete(Runtime& rt, DynProxy* p, GCObject* key);
Value proxyCall(Runtime& rt, DynProxy* p, const Value& self, const std::vector<Value>& args);
Value proxyConstruct(Runtime& rt, DynProxy* p, const std::vector<Value>& args, DynFunction* newTarget);

}  // namespace dyn
}  // namespace internal
}  // namespace script
