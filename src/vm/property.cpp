// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Property access through the prototype chain: the slow paths every inline cache falls back on.
#include "script/internal/vm/census.hpp"
#include "script/internal/vm/convert.hpp"
#include "script/internal/vm/object.hpp"
#include "script/internal/vm/typed.hpp"

namespace script {
namespace internal {
namespace vm {

namespace {

bool isOldKind(JSValue v, detail::CellKind k) noexcept
{
    return v.isCell() && !isVmCell(v.asCell()) && v.asCell()->kind == k;
}

JSValue number(std::uint32_t n) noexcept
{
    return n <= 0x7FFFFFFFu ? JSValue::int32(std::int32_t(n)) : JSValue::boxDouble(double(n));
}

JSValue callAccessor(VM& vm, JSValue fn, JSValue self, const JSValue* args, std::uint32_t argc)
{
    if (!vm.callHook)
        return vm.throwError(ErrorCode::TypeError, "no function can be called: the interpreter is not running");
    // The accessor may delete or replace the property that holds it.
    retain(fn);
    const JSValue result = vm.callHook(vm, fn, self, args, argc, vm.callHookData);
    release(fn);
    return result;
}

/// Where a read on `base` starts: the object itself, or the prototype its primitive reads through (null: nothing).
bool readStart(VM& vm, JSValue base, JSObject** start, Key key, JSValue* direct)
{
    if (isObject(base)) {
        *start = asObject(base);
        return false;
    }
    *start = nullptr;
    if (base.isNumber())
        *start = vm.protos.number;
    else if (base.isBool())
        *start = vm.protos.boolean;
    else if (isOldKind(base, detail::CellKind::String)) {
        *start = vm.protos.string;
        auto* s = static_cast<StringCell*>(base.asCell());
        std::uint32_t index;
        if (key == vm.atoms.length) {
            *direct = number(s->length);
            return true;
        }
        if (arrayIndexOf(key, &index) && index < s->length) {
            Value one = vm.ctx.str(s->text().substr(index, 1));
            *direct = fromValue(vm, one);
            return true;
        }
    } else if (isOldKind(base, detail::CellKind::Symbol))
        *start = vm.protos.symbol;
    else if (isOldKind(base, detail::CellKind::BigInt))
        *start = vm.protos.bigint;
    return false;
}

}  // namespace

bool arrayIndexOf(Key key, std::uint32_t* index) noexcept
{
    if (key->kind != detail::CellKind::String)
        return false;
    const StringView text = static_cast<StringCell*>(key)->text();
    if (text.empty() || text.size() > 10)
        return false;
    if (text[0] == u'0')
        return text.size() == 1 && ((*index = 0), true);
    std::uint64_t n = 0;
    for (Char c : text) {
        if (c < u'0' || c > u'9')
            return false;
        n = n * 10 + (c - u'0');
    }
    if (n >= 0xFFFFFFFFull)
        return false;
    *index = std::uint32_t(n);
    return true;
}

JSValue getPropertyFrom(VM& vm, JSObject* start, JSValue receiver, Key key)
{
    for (JSObject* o = start; o; o = o->proto(vm)) {
        if (proxyOf(o))
            return proxyGet(vm, o, key, receiver);
        if (o->cls() == ObjClass::TypedArray) {
            auto* t = static_cast<JSTypedArray*>(o);
            if (key == vm.atoms.length)
                return number(t->length);
            std::uint32_t index;
            if (arrayIndexOf(key, &index))
                return index < t->length ? typedGet(vm, t, index) : JSValue::undefined();
        }
        if (o->cls() == ObjClass::Array) {
            auto* a = static_cast<JSArray*>(o);
            if (key == vm.atoms.length)
                return number(a->length);
            std::uint32_t index;
            if (arrayIndexOf(key, &index)) {
                const JSValue v = a->at(index);
                if (!v.isHole()) {
                    retain(v);
                    return v;
                }
            }
        }
        std::uint8_t attrs;
        JSValue* slot = o->findOwn(vm, key, &attrs);
        if (!slot)
            continue;
        if (attrs & attr::kAccessor) {
            const auto* gs = reinterpret_cast<const GetterSetter*>(slot->asCell());
            if (gs->getter.isUndefined())
                return JSValue::undefined();
            return callAccessor(vm, gs->getter, receiver, nullptr, 0);
        }
        retain(*slot);
        return *slot;
    }
    return JSValue::undefined();
}

JSValue getProperty(VM& vm, JSValue base, Key key)
{
    if (base.isNullish()) {
        SCRIPT_VM_CENSUS(ThrowCount);
        return vm.throwError(ErrorCode::TypeError, "Cannot read properties of " +
                                                       std::string(base.isNull() ? "null" : "undefined"));
    }
    JSObject* o;
    JSValue direct = JSValue::undefined();
    if (readStart(vm, base, &o, key, &direct)) {
        return direct;
    }
    if (!o && !isObject(base) && base.isCell() && !isVmCell(base.asCell()) &&
        base.asCell()->kind == detail::CellKind::Object)
        return vm.throwError(ErrorCode::TypeError, "an object of the dynamic engine is not readable by the vm core");
    return getPropertyFrom(vm, o, base, key);
}

bool hasProperty(VM& vm, JSValue base, Key key)
{
    JSObject* o;
    JSValue direct = JSValue::undefined();
    if (!isObject(base)) {
        // `in` is for objects; a primitive is the caller's TypeError.
        vm.throwError(ErrorCode::TypeError, "Cannot use 'in' operator on a primitive");
        return false;
    }
    (void)direct;
    for (o = asObject(base); o; o = o->proto(vm)) {
        if (proxyOf(o))
            return proxyHas(vm, o, key) > 0;
        if (o->cls() == ObjClass::TypedArray) {
            std::uint32_t index;
            if (arrayIndexOf(key, &index))
                return index < static_cast<JSTypedArray*>(o)->length;
        }
        if (o->cls() == ObjClass::Array) {
            auto* a = static_cast<JSArray*>(o);
            std::uint32_t index;
            if (key == vm.atoms.length || (arrayIndexOf(key, &index) && !a->at(index).isHole()))
                return true;
        }
        if (o->findOwn(vm, key))
            return true;
    }
    return false;
}

namespace {

bool refuse(VM& vm, bool strict, const char* message)
{
    if (strict)
        vm.throwError(ErrorCode::TypeError, message);
    return false;
}

bool setArrayLength(VM& vm, JSArray* a, JSValue value, bool strict)
{
    if (!value.isNumber() || value.asNumber() < 0 || value.asNumber() != double(std::uint32_t(value.asNumber()))) {
        if (strict)
            vm.throwError(ErrorCode::RangeError, "Invalid array length");
        return false;
    }
    a->resize(vm, std::uint32_t(value.asNumber()));
    return true;
}

}  // namespace

bool setProperty(VM& vm, JSValue base, Key key, JSValue value, bool strict)
{
    if (base.isNullish()) {
        SCRIPT_VM_CENSUS(ThrowCount);
        vm.throwError(ErrorCode::TypeError, "Cannot set properties of " + std::string(base.isNull() ? "null" : "undefined"));
        return false;
    }
    if (!isObject(base))
        return refuse(vm, strict, "Cannot create property on a primitive value");
    JSObject* receiver = asObject(base);
    if (proxyOf(receiver))
        return proxySet(vm, receiver, key, value, base, strict);
    if (receiver->cls() == ObjClass::TypedArray) {
        std::uint32_t index;
        if (arrayIndexOf(key, &index))
            return typedSet(vm, static_cast<JSTypedArray*>(receiver), double(index), value);
    }
    if (receiver->cls() == ObjClass::Array) {
        auto* a = static_cast<JSArray*>(receiver);
        std::uint32_t index;
        if (key == vm.atoms.length)
            return setArrayLength(vm, a, value, strict);
        if (arrayIndexOf(key, &index)) {
            if (receiver->h.flags & cellflags::kFrozen)
                return refuse(vm, strict, "Cannot assign to read only property of a frozen array");
            if (index >= a->length && (receiver->h.flags & cellflags::kNotExtensible))
                return refuse(vm, strict, "Cannot add property, object is not extensible");
            retain(value);
            if (a->setAt(vm, index, value))
                return true;
            return refuse(vm, strict, "Cannot create a sparse array element");
        }
    }
    for (JSObject* p = receiver; p; p = p->proto(vm)) {
        if (p != receiver && proxyOf(p))
            return proxySet(vm, p, key, value, base, strict);
        std::uint8_t attrs;
        JSValue* slot = p->findOwn(vm, key, &attrs);
        if (!slot)
            continue;
        if (attrs & attr::kAccessor) {
            const auto* gs = reinterpret_cast<const GetterSetter*>(slot->asCell());
            if (gs->setter.isUndefined())
                return refuse(vm, strict, "Cannot set property which has only a getter");
            const JSValue r = callAccessor(vm, gs->setter, base, &value, 1);
            if (r.isException())
                return false;
            release(r);
            return true;
        }
        if (!(attrs & attr::kWritable))
            return refuse(vm, strict, "Cannot assign to read only property");
        if (p == receiver) {
            retain(value);
            const JSValue old = *slot;
            *slot = value;
            release(old);
            return true;
        }
        break;
    }
    if (receiver->h.flags & cellflags::kNotExtensible)
        return refuse(vm, strict, "Cannot add property, object is not extensible");
    retain(value);
    receiver->putOwn(vm, key, value, attr::kDefault);
    return true;
}

bool deleteProperty(VM& vm, JSValue base, Key key, bool strict)
{
    if (base.isNullish()) {
        vm.throwError(ErrorCode::TypeError, "Cannot convert undefined or null to object");
        return false;
    }
    if (!isObject(base))
        return true;
    JSObject* o = asObject(base);
    if (proxyOf(o))
        return proxyDelete(vm, o, key, strict) > 0;
    if (o->cls() == ObjClass::TypedArray) {
        std::uint32_t index;
        if (arrayIndexOf(key, &index))
            return index >= static_cast<JSTypedArray*>(o)->length || refuse(vm, strict, "Cannot delete property of a typed array");
    }
    if (o->cls() == ObjClass::Array) {
        auto* a = static_cast<JSArray*>(o);
        std::uint32_t index;
        if (key == vm.atoms.length)
            return refuse(vm, strict, "Cannot delete property 'length'");
        if (arrayIndexOf(key, &index)) {
            if (index < a->length && !a->elems[index].isHole()) {
                if (o->h.flags & (cellflags::kFrozen | cellflags::kNotExtensible))
                    if (o->h.flags & cellflags::kFrozen)
                        return refuse(vm, strict, "Cannot delete property of a frozen array");
                const JSValue old = a->elems[index];
                a->elems[index] = JSValue::hole();
                a->h.flags &= std::uint8_t(~cellflags::kAllNumbers);
                release(old);
            }
            return true;
        }
    }
    if (o->deleteOwn(vm, key))
        return true;
    return refuse(vm, strict, "Cannot delete a non-configurable property");
}

bool defineOwn(VM& vm, JSObject* object, Key key, JSValue value, std::uint8_t attrs)
{
    if (object->cls() == ObjClass::TypedArray) {
        std::uint32_t index;
        if (arrayIndexOf(key, &index)) {
            const bool ok = index < static_cast<JSTypedArray*>(object)->length &&
                            typedSet(vm, static_cast<JSTypedArray*>(object), double(index), value);
            release(value);
            return ok;
        }
    }
    if (object->cls() == ObjClass::Array) {
        auto* a = static_cast<JSArray*>(object);
        std::uint32_t index;
        if (key == vm.atoms.length) {
            if (!value.isNumber() || value.asNumber() < 0 || value.asNumber() != double(std::uint32_t(value.asNumber())))
                return false;
            a->resize(vm, std::uint32_t(value.asNumber()));
            release(value);
            return true;
        }
        if (arrayIndexOf(key, &index)) {
            // Dense elements always have the default attributes; anything else would need a sparse array.
            if (attrs != attr::kDefault) {
                release(value);
                return false;
            }
            return a->setAt(vm, index, value);
        }
    }
    std::uint8_t old = 0;
    if (object->findOwn(vm, key, &old)) {
        const bool sameAttrs = old == attrs;
        if (!(old & attr::kConfigurable) && (!sameAttrs || (old & attr::kAccessor) || !(old & attr::kWritable))) {
            release(value);
            return false;
        }
    } else if (object->h.flags & cellflags::kNotExtensible) {
        release(value);
        return false;
    }
    object->putOwn(vm, key, value, std::uint8_t(attrs & ~attr::kAccessor));
    return true;
}

bool defineAccessor(VM& vm, JSObject* object, Key key, JSValue getter, JSValue setter, std::uint8_t attrs)
{
    std::uint8_t old = 0;
    if (object->findOwn(vm, key, &old)) {
        if (!(old & attr::kConfigurable))
            return false;
    } else if (object->h.flags & cellflags::kNotExtensible) {
        return false;
    }
    retain(getter);
    retain(setter);
    GetterSetter* gs;
    try {
        gs = vm.newGetterSetter(getter, setter);
    } catch (...) {
        release(getter);
        release(setter);
        throw;
    }
    // Accessors have no `writable`.
    object->putOwn(vm, key, JSValue::cell(reinterpret_cast<CellPtr>(gs)),
                   std::uint8_t((attrs & ~attr::kWritable) | attr::kAccessor));
    return true;
}

}  // namespace vm
}  // namespace internal
}  // namespace script
