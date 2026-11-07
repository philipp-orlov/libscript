// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Property descriptors, and what is built on them: the descriptor-taking parts of `Object`, and `Reflect`.
#include <cmath>

#include "script/internal/dyn/builtins.hpp"
#include "script/internal/dyn/typedarray.hpp"
#include "script/internal/dyn/numconv.hpp"
#include "script/internal/dyn/vm.hpp"

namespace script {
namespace internal {
namespace dyn {

bool sameValue(const Value& a, const Value& b)
{
    if (a.type() == Type::Double && b.type() == Type::Double) {
        const double x = detail::Access::r8(a), y = detail::Access::r8(b);
        if (std::isnan(x) || std::isnan(y))
            return std::isnan(x) && std::isnan(y);
        return x == y && std::signbit(x) == std::signbit(y);
    }
    return strictEquals(a, b);
}

namespace {

struct Descriptor
{
    bool hasValue = false, hasWritable = false, hasGet = false, hasSet = false, hasEnumerable = false, hasConfigurable = false;
    bool writable = false, enumerable = false, configurable = false;
    Value value = Value::undefined(), getter = Value::undefined(), setter = Value::undefined();
    bool isAccessor() const noexcept { return hasGet || hasSet; }
    bool isData() const noexcept { return hasValue || hasWritable; }
};

Descriptor toDescriptor(Runtime& rt, const Value& source)
{
    if (!source.isObject())
        throwError(rt, ErrorCode::TypeError, "a property descriptor must be an object");
    Descriptor d;
    const auto read = [&](const char* name, bool& has, Value& out) {
        GCObject* key = rt.intern(name);
        has = hasProperty(rt, source, key);
        if (has)
            out = getProperty(rt, source, key);
    };
    Value flag;
    read("enumerable", d.hasEnumerable, flag);
    d.enumerable = d.hasEnumerable && toBoolean(flag);
    read("configurable", d.hasConfigurable, flag);
    d.configurable = d.hasConfigurable && toBoolean(flag);
    read("value", d.hasValue, d.value);
    read("writable", d.hasWritable, flag);
    d.writable = d.hasWritable && toBoolean(flag);
    read("get", d.hasGet, d.getter);
    if (d.hasGet && !d.getter.isUndefined() && !asFunction(d.getter))
        throwError(rt, ErrorCode::TypeError, "a getter must be a function");
    read("set", d.hasSet, d.setter);
    if (d.hasSet && !d.setter.isUndefined() && !asFunction(d.setter))
        throwError(rt, ErrorCode::TypeError, "a setter must be a function");
    if (d.isAccessor() && d.isData())
        throwError(rt, ErrorCode::TypeError, "a property descriptor cannot have both a value and an accessor");
    return d;
}

/// An array's elements are not named properties. What can be said of them, a dense array can only take as a plain element.
bool defineOnArray(Runtime& rt, DynArray* array, GCObject* key, const Descriptor& d, bool& handled)
{
    handled = false;
    if (key == rt.atom.length.get()) {
        handled = true;
        if (d.isAccessor() || (d.hasConfigurable && d.configurable) || (d.hasEnumerable && d.enumerable))
            return false;
        if (d.hasValue) {
            const double length = toNumber(rt, d.value);
            if (length < 0 || length != std::floor(length) || length > 4294967295.0)
                throwError(rt, ErrorCode::RangeError, "invalid array length");
            setProperty(rt, detail::Access::make(Type::Object, array), key, Value(length));
        }
        return true;
    }
    if (key->kind != CellKind::String)
        return true;
    const double index = toNumber(rt, detail::Access::make(Type::String, key));
    if (!(index >= 0) || index != std::floor(index) || index >= 4294967295.0 ||
        numberToString(index) != static_cast<StringCell*>(key)->text())
        return true;
    // What an element cannot be, a named property of that name can: a read of the index finds it.
    if (d.isAccessor() || (d.hasWritable && !d.writable) || (d.hasEnumerable && !d.enumerable) || (d.hasConfigurable && !d.configurable))
        return true;
    if (std::size_t(index) >= array->size() && !d.hasWritable && !d.hasEnumerable && !d.hasConfigurable)
        return true;
    handled = true;
    if (!array->extensible && std::size_t(index) >= array->size())
        return false;
    if (array->frozen)
        return !d.hasValue || (std::size_t(index) < array->size() && strictEquals(array->at(std::size_t(index)), d.value));
    if (d.hasValue || std::size_t(index) >= array->size())
        setProperty(rt, detail::Access::make(Type::Object, array), key, d.value);
    return true;
}

/// `[[DefineOwnProperty]]`: false where what is there does not allow it.
bool defineOwn(Runtime& rt, DynObject* self, GCObject* key, const Descriptor& d)
{
    if (self->cls == ObjClass::TypedArray) {
        auto* ta = static_cast<DynTypedArray*>(self);
        double index = 0;
        if (numericKey(key, index)) {
            if (!typedArrayHas(ta, index))
                return false;
            if ((d.hasConfigurable && !d.configurable) || (d.hasEnumerable && !d.enumerable) || d.isAccessor() || (d.hasWritable && !d.writable))
                return false;
            if (d.hasValue)
                typedArraySet(rt, ta, index, d.value);
            return true;
        }
    }
    if (self->cls == ObjClass::Array) {
        bool handled = false;
        const bool result = defineOnArray(rt, static_cast<DynArray*>(self), key, d, handled);
        if (handled)
            return result;
    }
    Prop* current = self->findOwn(key);
    if (!current) {
        if (!self->extensible)
            return false;
        std::uint8_t attrs = std::uint8_t((d.enumerable ? PropEnumerable : 0) | (d.configurable ? PropConfigurable : 0));
        if (d.isAccessor()) {
            Prop& p = self->putOwn(key, d.getter, std::uint8_t(attrs | PropAccessor));
            self->setSetter(p, d.setter);
        } else {
            self->putOwn(key, d.value, std::uint8_t(attrs | (d.writable ? PropWritable : 0)));
        }
        return true;
    }
    const std::uint8_t old = self->attrsOf(*current);
    const bool wasAccessor = (old & PropAccessor) != 0;
    const bool wasConfigurable = (old & PropConfigurable) != 0;
    if (!wasConfigurable) {
        if (d.configurable || (d.hasEnumerable && d.enumerable != ((old & PropEnumerable) != 0)))
            return false;
        if (d.isAccessor() != wasAccessor && (d.isAccessor() || d.isData()))
            return false;
        if (wasAccessor) {
            if (d.hasGet && !strictEquals(d.getter, current->value))
                return false;
            if (d.hasSet && !strictEquals(d.setter, self->setterOf(*current)))
                return false;
        } else if (!(old & PropWritable)) {
            if (d.writable)
                return false;
            if (d.hasValue && !sameValue(d.value, current->value))
                return false;
        }
    }
    std::uint8_t attrs = std::uint8_t(old & (PropEnumerable | PropConfigurable));
    if (d.hasEnumerable)
        attrs = std::uint8_t((attrs & ~PropEnumerable) | (d.enumerable ? PropEnumerable : 0));
    if (d.hasConfigurable)
        attrs = std::uint8_t((attrs & ~PropConfigurable) | (d.configurable ? PropConfigurable : 0));
    if (d.isAccessor() || (wasAccessor && !d.isData())) {
        const Value getter = d.hasGet ? d.getter : wasAccessor ? current->value : Value::undefined();
        const Value setter = d.hasSet ? d.setter : wasAccessor ? self->setterOf(*current) : Value::undefined();
        Prop& p = self->putOwn(key, getter, std::uint8_t(attrs | PropAccessor));
        self->setSetter(p, setter);
        return true;
    }
    const bool writable = d.hasWritable ? d.writable : !wasAccessor && (old & PropWritable) != 0;
    const Value value = d.hasValue ? d.value : wasAccessor ? Value::undefined() : current->value;
    self->putOwn(key, value, std::uint8_t(attrs | (writable ? PropWritable : 0)));
    return true;
}

Value fromProperty(Runtime& rt, DynObject* self, GCObject* key)
{
    if (self->cls == ObjClass::TypedArray) {
        auto* ta = static_cast<DynTypedArray*>(self);
        double index = 0;
        if (numericKey(key, index)) {
            if (!typedArrayHas(ta, index))
                return Value::undefined();
            DynObject* out = rt.newObject(rt.objectProto.get());
            Value result = detail::Access::adopt(Type::Object, out);
            out->putOwn(rt.atom.value.get(), typedArrayGet(rt, ta, std::size_t(index)));
            out->putOwn(rt.intern("writable"), Value(true));
            out->putOwn(rt.intern("enumerable"), Value(true));
            out->putOwn(rt.intern("configurable"), Value(true));
            return result;
        }
    }
    if (self->cls == ObjClass::Array && key == rt.atom.length.get()) {
        DynObject* out = rt.newObject(rt.objectProto.get());
        Value result = detail::Access::adopt(Type::Object, out);
        out->putOwn(rt.atom.value.get(), Value(double(static_cast<DynArray*>(self)->size())));
        out->putOwn(rt.intern("writable"), Value(!self->frozen));
        out->putOwn(rt.intern("enumerable"), Value(false));
        out->putOwn(rt.intern("configurable"), Value(false));
        return result;
    }
    if (self->cls == ObjClass::Array && key->kind == CellKind::String) {
        auto* array = static_cast<DynArray*>(self);
        const double index = toNumber(rt, detail::Access::make(Type::String, key));
        if (index >= 0 && index < double(array->size()) && index == std::floor(index) && array->hasElement(std::size_t(index)) && numberToString(index) == static_cast<StringCell*>(key)->text()) {
            DynObject* out = rt.newObject(rt.objectProto.get());
            Value result = detail::Access::adopt(Type::Object, out);
            out->putOwn(rt.atom.value.get(), array->at(std::size_t(index)));
            out->putOwn(rt.intern("writable"), Value(!self->frozen));
            out->putOwn(rt.intern("enumerable"), Value(true));
            out->putOwn(rt.intern("configurable"), Value(!self->frozen));
            return result;
        }
    }
    const Prop* p = self->findOwn(key);
    if (!p)
        return Value::undefined();
    DynObject* out = rt.newObject(rt.objectProto.get());
    Value result = detail::Access::adopt(Type::Object, out);
    const std::uint8_t attrs = self->attrsOf(*p);
    if (attrs & PropAccessor) {
        out->putOwn(rt.intern("get"), p->value);
        out->putOwn(rt.intern("set"), self->setterOf(*p));
    } else {
        out->putOwn(rt.atom.value.get(), p->value);
        out->putOwn(rt.intern("writable"), Value((attrs & PropWritable) != 0));
    }
    out->putOwn(rt.intern("enumerable"), Value((attrs & PropEnumerable) != 0));
    out->putOwn(rt.intern("configurable"), Value((attrs & PropConfigurable) != 0));
    return result;
}

DynObject* objectArg(CallInfo& ci, std::size_t i, const char* who)
{
    DynObject* o = asObject(ci.arg(i));
    if (!o)
        ci.fail(ErrorCode::TypeError, std::string(who) + " needs an object");
    return o;
}

bool isPrivateKey(GCObject* key) noexcept
{
    return key->kind == CellKind::Symbol && static_cast<SymbolCell*>(key)->isPrivate;
}

/// Every own property's flags, as `seal` and `freeze` set them.
void lockProperties(DynObject* self, bool alsoReadOnly)
{
    self->extensible = false;
    self->dropShape();
    for (std::size_t i = 0; i < self->props.size(); ++i) {
        if (isPrivateKey(self->keyAt(i)))
            continue;
        std::uint8_t attrs = self->attrsAt(i);
        attrs = std::uint8_t(attrs & ~PropConfigurable);
        if (alsoReadOnly && !(attrs & PropAccessor))
            attrs = std::uint8_t(attrs & ~PropWritable);
        self->setAttrsAt(i, attrs);
    }
}

bool allLocked(DynObject* self, bool alsoReadOnly)
{
    if (self->extensible)
        return false;
    if (self->cls == ObjClass::Array && !static_cast<DynArray*>(self)->empty() && !self->frozen)
        return false;
    for (std::size_t i = 0; i < self->props.size(); ++i) {
        if (isPrivateKey(self->keyAt(i)))
            continue;
        const std::uint8_t attrs = self->attrsAt(i);
        if (attrs & PropConfigurable)
            return false;
        if (alsoReadOnly && !(attrs & PropAccessor) && (attrs & PropWritable))
            return false;
    }
    return true;
}

}  // namespace

void installReflection(Runtime& rt)
{
    DynObject* object = asObject(getProperty(rt, detail::Access::make(Type::Object, rt.globalObject.get()), rt.intern("Object")));

    rt.method(object, "defineProperty", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        DynObject* self = objectArg(ci, 0, "Object.defineProperty");
        Ref<GCObject> key = toPropertyKey(rt2, ci.arg(1));
        if (!defineOwn(rt2, self, key.get(), toDescriptor(rt2, ci.arg(2))))
            ci.fail(ErrorCode::TypeError, "the property cannot be redefined that way");
        return ci.arg(0);
    }, 3);
    rt.method(object, "defineProperties", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        DynObject* self = objectArg(ci, 0, "Object.defineProperties");
        const Value props = toObject(rt2, ci.arg(1));
        std::vector<std::pair<Ref<GCObject>, Descriptor>> all;
        for (const Value& k : ownKeys(rt2, asObject(props), true, true)) {
            Ref<GCObject> key = toPropertyKey(rt2, k);
            all.emplace_back(key, toDescriptor(rt2, getProperty(rt2, props, key.get())));
        }
        for (const auto& [key, d] : all) {
            if (!defineOwn(rt2, self, key.get(), d))
                ci.fail(ErrorCode::TypeError, "the property cannot be redefined that way");
        }
        return ci.arg(0);
    }, 2);
    rt.method(object, "getOwnPropertyDescriptor", [](CallInfo& ci) -> Value {
        Runtime& rt2 = rtOf(ci);
        const Value target = toObject(rt2, ci.arg(0));
        Ref<GCObject> key = toPropertyKey(rt2, ci.arg(1));
        return fromProperty(rt2, asObject(target), key.get());
    }, 2);
    rt.method(object, "getOwnPropertyDescriptors", [](CallInfo& ci) -> Value {
        Runtime& rt2 = rtOf(ci);
        const Value target = toObject(rt2, ci.arg(0));
        DynObject* out = rt2.newObject(rt2.objectProto.get());
        Value result = detail::Access::adopt(Type::Object, out);
        for (const Value& k : ownKeys(rt2, asObject(target), false, true)) {
            Ref<GCObject> key = toPropertyKey(rt2, k);
            out->putOwn(key.get(), fromProperty(rt2, asObject(target), key.get()));
        }
        return result;
    }, 1);
    rt.method(object, "getOwnPropertySymbols", [](CallInfo& ci) -> Value {
        Runtime& rt2 = rtOf(ci);
        const Value target = toObject(rt2, ci.arg(0));
        std::vector<Value> out;
        for (const Value& k : ownKeys(rt2, asObject(target), false, true)) {
            if (k.type() == Type::Symbol)
                out.push_back(k);
        }
        return rt2.makeArray(out);
    }, 1);
    rt.method(object, "hasOwn", [](CallInfo& ci) -> Value {
        Runtime& rt2 = rtOf(ci);
        const Value target = toObject(rt2, ci.arg(0));
        Ref<GCObject> key = toPropertyKey(rt2, ci.arg(1));
        return invokeMethod(rt2, target, rt2.intern("hasOwnProperty"), {detail::Access::make(key->kind == CellKind::Symbol ? Type::Symbol : Type::String, key.get())});
    }, 2);
    rt.method(object, "preventExtensions", [](CallInfo& ci) {
        if (DynObject* self = asObject(ci.arg(0))) {
            self->extensible = false;
            self->dropShape();
        }
        return ci.arg(0);
    }, 1);
    rt.method(object, "seal", [](CallInfo& ci) {
        if (DynObject* self = asObject(ci.arg(0)))
            lockProperties(self, false);
        return ci.arg(0);
    }, 1);
    rt.method(object, "isExtensible", [](CallInfo& ci) {
        DynObject* self = asObject(ci.arg(0));
        return Value(self && self->extensible);
    }, 1);
    rt.method(object, "isSealed", [](CallInfo& ci) {
        DynObject* self = asObject(ci.arg(0));
        return Value(!self || self->frozen || allLocked(self, false));
    }, 1);
    rt.method(object, "isFrozen", [](CallInfo& ci) {
        DynObject* self = asObject(ci.arg(0));
        return Value(!self || self->frozen || allLocked(self, true));
    }, 1);
    rt.method(object, "groupBy", [](CallInfo& ci) -> Value {
        Runtime& rt2 = rtOf(ci);
        if (ci.arg(0).isNullish())
            ci.fail(ErrorCode::TypeError, "Object.groupBy needs an iterable");
        if (!asFunction(ci.arg(1)))
            ci.fail(ErrorCode::TypeError, "Object.groupBy needs a function");
        DynObject* out = rt2.newObject(nullptr);
        out->setProto(nullptr);
        Value result = detail::Access::adopt(Type::Object, out);
        double i = 0;
        for (const Value& item : iterableToVector(rt2, ci.arg(0))) {
            Ref<GCObject> key = toPropertyKey(rt2, callValue(rt2, ci.arg(1), Value::undefined(), {item, Value(i++)}));
            const Prop* group = out->findOwn(key.get());
            if (!group)
                group = &out->putOwn(key.get(), rt2.makeArray({}));
            asArray(group->value)->append(item);
        }
        return result;
    }, 2);

    DynObject* reflect = rt.newObject(rt.objectProto.get());
    const Value reflectValue = detail::Access::adopt(Type::Object, reflect);
    rt.globalObject->putOwn(rt.intern("Reflect"), reflectValue, PropWritable | PropConfigurable);
    reflect->putOwn(rt.sym.toStringTag.get(), rt.str(ascii("Reflect")), PropConfigurable);
    rt.method(reflect, "apply", [](CallInfo& ci) -> Value {
        Runtime& rt2 = rtOf(ci);
        if (!asFunction(ci.arg(0)))
            ci.fail(ErrorCode::TypeError, "Reflect.apply needs a function");
        std::vector<Value> args;
        const Value list = ci.arg(2);
        if (!list.isObject())
            ci.fail(ErrorCode::TypeError, "Reflect.apply needs an array-like");
        const double length = toNumber(rt2, getProperty(rt2, list, rt2.atom.length.get()));
        for (double i = 0; i < length; ++i)
            args.push_back(getProperty(rt2, list, toPropertyKey(rt2, Value(i)).get()));
        return callValue(rt2, ci.arg(0), ci.arg(1), args);
    }, 3);
    rt.method(reflect, "construct", [](CallInfo& ci) -> Value {
        Runtime& rt2 = rtOf(ci);
        DynFunction* target = asFunction(ci.arg(0));
        DynFunction* newTarget = ci.size() > 2 ? asFunction(ci.arg(2)) : target;
        if (!target || !newTarget || !isConstructor(rt2, target) || !isConstructor(rt2, newTarget))
            ci.fail(ErrorCode::TypeError, "Reflect.construct needs constructors");
        const Value list = ci.arg(1);
        if (!list.isObject())
            ci.fail(ErrorCode::TypeError, "Reflect.construct needs an array-like");
        std::vector<Value> args;
        const double length = toNumber(rt2, getProperty(rt2, list, rt2.atom.length.get()));
        for (double i = 0; i < length; ++i)
            args.push_back(getProperty(rt2, list, toPropertyKey(rt2, Value(i)).get()));
        return constructValue(rt2, ci.arg(0), args, newTarget);
    }, 2);
    rt.method(reflect, "defineProperty", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        DynObject* self = objectArg(ci, 0, "Reflect.defineProperty");
        Ref<GCObject> key = toPropertyKey(rt2, ci.arg(1));
        return Value(defineOwn(rt2, self, key.get(), toDescriptor(rt2, ci.arg(2))));
    }, 3);
    rt.method(reflect, "deleteProperty", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        objectArg(ci, 0, "Reflect.deleteProperty");
        Ref<GCObject> key = toPropertyKey(rt2, ci.arg(1));
        return Value(deleteProperty(rt2, ci.arg(0), key.get()));
    }, 2);
    rt.method(reflect, "get", [](CallInfo& ci) -> Value {
        Runtime& rt2 = rtOf(ci);
        DynObject* self = objectArg(ci, 0, "Reflect.get");
        Ref<GCObject> key = toPropertyKey(rt2, ci.arg(1));
        return getPropertyOfObject(rt2, self, key.get(), ci.size() > 2 ? ci.arg(2) : ci.arg(0));
    }, 2);
    rt.method(reflect, "getOwnPropertyDescriptor", [](CallInfo& ci) -> Value {
        Runtime& rt2 = rtOf(ci);
        DynObject* self = objectArg(ci, 0, "Reflect.getOwnPropertyDescriptor");
        Ref<GCObject> key = toPropertyKey(rt2, ci.arg(1));
        return fromProperty(rt2, self, key.get());
    }, 2);
    rt.method(reflect, "getPrototypeOf", [](CallInfo& ci) -> Value {
        DynObject* self = objectArg(ci, 0, "Reflect.getPrototypeOf");
        return self->proto ? detail::Access::make(Type::Object, self->proto.get()) : Value::null();
    }, 1);
    rt.method(reflect, "has", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        objectArg(ci, 0, "Reflect.has");
        Ref<GCObject> key = toPropertyKey(rt2, ci.arg(1));
        return Value(hasProperty(rt2, ci.arg(0), key.get()));
    }, 2);
    rt.method(reflect, "isExtensible", [](CallInfo& ci) { return Value(objectArg(ci, 0, "Reflect.isExtensible")->extensible); }, 1);
    rt.method(reflect, "ownKeys", [](CallInfo& ci) -> Value {
        Runtime& rt2 = rtOf(ci);
        DynObject* self = objectArg(ci, 0, "Reflect.ownKeys");
        const Vec<Value> keys = ownKeys(rt2, self, false, true);
        return rt2.makeArray(std::vector<Value>(keys.begin(), keys.end()));
    }, 1);
    rt.method(reflect, "preventExtensions", [](CallInfo& ci) {
        DynObject* self = objectArg(ci, 0, "Reflect.preventExtensions");
        self->extensible = false;
        self->dropShape();
        return Value(true);
    }, 1);
    rt.method(reflect, "set", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        DynObject* self = objectArg(ci, 0, "Reflect.set");
        Ref<GCObject> key = toPropertyKey(rt2, ci.arg(1));
        const Prop* own = self->findOwn(key.get());
        if (own && !self->accessorOf(*own) && !(self->attrsOf(*own) & PropWritable))
            return Value(false);
        if (!own && !self->extensible && !hasProperty(rt2, ci.arg(0), key.get()))
            return Value(false);
        setProperty(rt2, ci.size() > 3 ? ci.arg(3) : ci.arg(0), key.get(), ci.arg(2));
        return Value(true);
    }, 3);
    rt.method(reflect, "setPrototypeOf", [](CallInfo& ci) {
        DynObject* self = objectArg(ci, 0, "Reflect.setPrototypeOf");
        if (!ci.arg(1).isNull() && !ci.arg(1).isObject())
            ci.fail(ErrorCode::TypeError, "a prototype is an object or null");
        DynObject* next = asObject(ci.arg(1));
        if (self->proto.get() == next)
            return Value(true);
        if (!self->extensible)
            return Value(false);
        for (DynObject* up = next; up; up = up->proto.get()) {
            if (up == self)
                return Value(false);
        }
        self->setProto(next);
        return Value(true);
    }, 2);
}

}  // namespace dyn
}  // namespace internal
}  // namespace script
