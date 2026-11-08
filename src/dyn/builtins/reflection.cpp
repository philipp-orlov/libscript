// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Property descriptors, and what is built on them: the descriptor-taking parts of `Object`, and `Reflect`.
#include <cmath>

#include "script/internal/dyn/builtins.hpp"
#include "script/internal/dyn/proxy.hpp"
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


}  // namespace

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
static bool defineOnArray(Runtime& rt, DynArray* array, GCObject* key, const Descriptor& d, bool& handled)
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
    // What an element cannot be, a named property of that name can: the element becomes a hole, so a read of the index
    // finds the property, which starts out as the element was.
    if (d.isAccessor() || (d.hasWritable && !d.writable) || (d.hasEnumerable && !d.enumerable) || (d.hasConfigurable && !d.configurable)) {
        const std::size_t at = std::size_t(index);
        if (array->findOwn(key))
            return true;
        if (at < array->size() && array->hasElement(at)) {
            if (array->frozen)
                return true;
            const Value current = array->at(at);
            array->makeHole(at);
            array->dropShape();
            array->putOwn(key, current, PropDefault);
        } else if (at >= array->size()) {
            if (!array->extensible) {
                handled = true;
                return false;
            }
            array->resize(at + 1);
            array->makeHole(at);
        }
        return true;
    }
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
bool ordinaryDefineOwn(Runtime& rt, DynObject* self, GCObject* key, const Descriptor& d)
{
    if (key == rt.atom.prototype.get())
        ensurePrototype(rt, self);
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

bool ordinaryGetOwn(Runtime& rt, DynObject* self, GCObject* key, Descriptor& out)
{
    if (key == rt.atom.prototype.get())
        ensurePrototype(rt, self);
    out = Descriptor();
    const auto data = [&](const Value& v, bool writable, bool enumerable, bool configurable) {
        out.hasValue = out.hasWritable = out.hasEnumerable = out.hasConfigurable = true;
        out.value = v;
        out.writable = writable;
        out.enumerable = enumerable;
        out.configurable = configurable;
        return true;
    };
    if (self->cls == ObjClass::TypedArray) {
        auto* ta = static_cast<DynTypedArray*>(self);
        double index = 0;
        if (numericKey(key, index)) {
            if (!typedArrayHas(ta, index))
                return false;
            return data(typedArrayGet(rt, ta, std::size_t(index)), true, true, true);
        }
    }
    if (self->cls == ObjClass::Array && key == rt.atom.length.get())
        return data(Value(double(static_cast<DynArray*>(self)->size())), !self->frozen, false, false);
    if (self->cls == ObjClass::Array && key->kind == CellKind::String) {
        auto* array = static_cast<DynArray*>(self);
        std::uint32_t index = 0;
        if (arrayIndexOf(static_cast<StringCell*>(key)->text(), index) && index < array->size() && array->hasElement(index))
            return data(array->at(index), !self->frozen, true, !self->frozen);
    }
    if (self->cls == ObjClass::StringObject && key->kind == CellKind::String) {
        if (StringCell* text = asString(static_cast<DynWrapper*>(self)->primitive)) {
            std::uint32_t index = 0;
            if (key == rt.atom.length.get())
                return data(Value(double(text->text().size())), false, false, false);
            if (arrayIndexOf(static_cast<StringCell*>(key)->text(), index) && index < text->text().size())
                return data(rt.str(text->text().substr(index, 1)), false, true, false);
        }
    }
    const Prop* p = self->findOwn(key);
    if (!p)
        return false;
    const std::uint8_t attrs = self->attrsOf(*p);
    if (attrs & PropAccessor) {
        out.hasGet = out.hasSet = out.hasEnumerable = out.hasConfigurable = true;
        out.getter = p->value;
        out.setter = self->setterOf(*p);
        out.enumerable = attrs & PropEnumerable;
        out.configurable = attrs & PropConfigurable;
        return true;
    }
    return data(p->value, attrs & PropWritable, attrs & PropEnumerable, attrs & PropConfigurable);
}

namespace {

Value fromProperty(Runtime& rt, DynObject* self, GCObject* key)
{
    Descriptor d;
    if (!getOwnProperty(rt, self, key, d))
        return Value::undefined();
    return descriptorToObject(rt, d);
}

bool defineOwn(Runtime& rt, DynObject* self, GCObject* key, const Descriptor& d) { return defineOwnProperty(rt, self, key, d); }

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
    // ObjectDefineProperties: every descriptor read first, then all defined.
    const auto defineAll = [](Runtime& rt2, DynObject* self, const Value& source) {
        const Value props = toObject(rt2, source);
        std::vector<std::pair<Ref<GCObject>, Descriptor>> all;
        for (const Value& k : ownPropertyKeys(rt2, asObject(props))) {
            Ref<GCObject> key = toPropertyKey(rt2, k);
            Descriptor present;
            if (!getOwnProperty(rt2, asObject(props), key.get(), present) || !present.enumerable)
                continue;
            all.emplace_back(key, toDescriptor(rt2, getProperty(rt2, props, key.get())));
        }
        for (const auto& [key, d] : all) {
            if (!defineOwn(rt2, self, key.get(), d))
                throwError(rt2, ErrorCode::TypeError, "the property cannot be redefined that way");
        }
    };
    rt.method(object, "defineProperties", [defineAll](CallInfo& ci) {
        DynObject* self = objectArg(ci, 0, "Object.defineProperties");
        defineAll(rtOf(ci), self, ci.arg(1));
        return ci.arg(0);
    }, 2);
    rt.method(object, "create", [defineAll](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        if (!ci.arg(0).isObject() && !ci.arg(0).isNull())
            ci.fail(ErrorCode::TypeError, "Object.create needs an object or null for the prototype");
        DynObject* out = rt2.newObject(asObject(ci.arg(0)));
        if (ci.arg(0).isNull())
            out->setProto(nullptr);
        Value result = detail::Access::adopt(Type::Object, out);
        if (!ci.arg(1).isUndefined())
            defineAll(rt2, out, ci.arg(1));
        return result;
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
        // HasOwnProperty itself: not a `hasOwnProperty` the object may lack (one made with a null prototype) or replace.
        const Value target = toObject(rt2, ci.arg(0));
        Ref<GCObject> key = toPropertyKey(rt2, ci.arg(1));
        Descriptor d;
        return Value(getOwnProperty(rt2, asObject(target), key.get(), d));
    }, 2);
    rt.method(object, "preventExtensions", [](CallInfo& ci) {
        if (DynObject* self = asObject(ci.arg(0))) {
            if (!preventExtensions(rtOf(ci), self))
                ci.fail(ErrorCode::TypeError, "the object would not stop growing");
        }
        return ci.arg(0);
    }, 1);
    // SetIntegrityLevel and TestIntegrityLevel the long way, for a proxy; an ordinary object has its own quick ones.
    const auto integrity = [](Runtime& rt2, DynObject* self, bool frozen) {
        if (!preventExtensions(rt2, self))
            throwError(rt2, ErrorCode::TypeError, "the object would not stop growing");
        for (const Value& k : ownPropertyKeys(rt2, self)) {
            Ref<GCObject> key = toPropertyKey(rt2, k);
            Descriptor d;
            d.hasConfigurable = true;
            if (frozen) {
                Descriptor current;
                if (!getOwnProperty(rt2, self, key.get(), current))
                    continue;
                if (current.isData()) {
                    d.hasWritable = true;
                }
            }
            if (!defineOwnProperty(rt2, self, key.get(), d))
                throwError(rt2, ErrorCode::TypeError, "a property would not be fixed");
        }
    };
    const auto tested = [](Runtime& rt2, DynObject* self, bool frozen) {
        if (isExtensible(rt2, self))
            return false;
        for (const Value& k : ownPropertyKeys(rt2, self)) {
            Ref<GCObject> key = toPropertyKey(rt2, k);
            Descriptor d;
            if (!getOwnProperty(rt2, self, key.get(), d))
                continue;
            if (d.configurable || (frozen && d.isData() && d.writable))
                return false;
        }
        return true;
    };
    rt.method(object, "seal", [integrity](CallInfo& ci) {
        if (DynObject* self = asObject(ci.arg(0))) {
            if (asProxy(self))
                integrity(rtOf(ci), self, false);
            else
                lockProperties(self, false);
        }
        return ci.arg(0);
    }, 1);
    rt.method(object, "freeze", [integrity](CallInfo& ci) {
        if (DynObject* self = asObject(ci.arg(0))) {
            if (asProxy(self)) {
                integrity(rtOf(ci), self, true);
                return ci.arg(0);
            }
            self->frozen = true;
            self->extensible = false;
            self->dropShape();
            for (std::size_t i = 0; i < self->props.size(); ++i) {
                GCObject* key = self->keyAt(i);
                if (key->kind != CellKind::Symbol || !static_cast<SymbolCell*>(key)->isPrivate)
                    self->setAttrsAt(i, std::uint8_t(self->attrsAt(i) & ~(PropWritable | PropConfigurable)));
            }
        }
        return ci.arg(0);
    }, 1);
    rt.method(object, "isExtensible", [](CallInfo& ci) {
        DynObject* self = asObject(ci.arg(0));
        return Value(self && isExtensible(rtOf(ci), self));
    }, 1);
    rt.method(object, "isSealed", [tested](CallInfo& ci) {
        DynObject* self = asObject(ci.arg(0));
        if (self && asProxy(self))
            return Value(tested(rtOf(ci), self, false));
        return Value(!self || self->frozen || allLocked(self, false));
    }, 1);
    rt.method(object, "isFrozen", [tested](CallInfo& ci) {
        DynObject* self = asObject(ci.arg(0));
        if (self && asProxy(self))
            return Value(tested(rtOf(ci), self, true));
        return Value(!self || self->frozen || allLocked(self, true));
    }, 1);
    rt.method(object, "getPrototypeOf", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        const Value self = toObject(rt2, ci.arg(0));
        return getPrototypeOf(rt2, asObject(self));
    }, 1);
    rt.method(object, "setPrototypeOf", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        if (ci.arg(0).isNullish())
            ci.fail(ErrorCode::TypeError, "Object.setPrototypeOf needs an object");
        if (!ci.arg(1).isNull() && !ci.arg(1).isObject())
            ci.fail(ErrorCode::TypeError, "a prototype is an object or null");
        if (DynObject* self = asObject(ci.arg(0))) {
            for (DynObject* up = asObject(ci.arg(1)); up && !up->fnKindIsProxy(); up = up->proto.get()) {
                if (up == self)
                    ci.fail(ErrorCode::TypeError, "cyclic prototype chain");
            }
            if (!setPrototypeOf(rt2, self, asObject(ci.arg(1))))
                ci.fail(ErrorCode::TypeError, "the prototype cannot be set: the object cannot change");
        }
        return ci.arg(0);
    }, 2);
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
        return getPrototypeOf(rtOf(ci), objectArg(ci, 0, "Reflect.getPrototypeOf"));
    }, 1);
    rt.method(reflect, "has", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        objectArg(ci, 0, "Reflect.has");
        Ref<GCObject> key = toPropertyKey(rt2, ci.arg(1));
        return Value(hasProperty(rt2, ci.arg(0), key.get()));
    }, 2);
    rt.method(reflect, "isExtensible", [](CallInfo& ci) { return Value(isExtensible(rtOf(ci), objectArg(ci, 0, "Reflect.isExtensible"))); }, 1);
    rt.method(reflect, "ownKeys", [](CallInfo& ci) -> Value {
        Runtime& rt2 = rtOf(ci);
        DynObject* self = objectArg(ci, 0, "Reflect.ownKeys");
        const Vec<Value> keys = ownPropertyKeys(rt2, self);
        return rt2.makeArray(std::vector<Value>(keys.begin(), keys.end()));
    }, 1);
    rt.method(reflect, "preventExtensions", [](CallInfo& ci) {
        DynObject* self = objectArg(ci, 0, "Reflect.preventExtensions");
        return Value(preventExtensions(rtOf(ci), self));
    }, 1);
    rt.method(reflect, "set", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        DynObject* self = objectArg(ci, 0, "Reflect.set");
        Ref<GCObject> key = toPropertyKey(rt2, ci.arg(1));
        if (asProxy(self) || (ci.size() > 3 && !(ci.arg(3).isObject() && asObject(ci.arg(3)) == self)))
            return Value(setWithReceiver(rt2, self, key.get(), ci.arg(2), ci.size() > 3 ? ci.arg(3) : ci.arg(0)));
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
        return Value(setPrototypeOf(rtOf(ci), self, asObject(ci.arg(1))));
    }, 2);
    installProxy(rt);
}

}  // namespace dyn
}  // namespace internal
}  // namespace script
