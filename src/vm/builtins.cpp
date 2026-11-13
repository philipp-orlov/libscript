// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The built-in objects the new core ships with so far: enough of Object, Function, Array, String, Number, Boolean, Math
// and the error classes to run the language's own tests. The rest of the standard library is ported from
// src/dyn/builtins (ls-jsc.md, 9.4) one file at a time.
#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstring>
#include <limits>

#include "script/internal/dyn/numconv.hpp"
#include "script/internal/dyn/unicode.hpp"
#include "script/internal/dyn/object.hpp"
#include "script/internal/vm/convert.hpp"
#include "script/internal/vm/host.hpp"
#include "script/internal/vm/object.hpp"
#include "script/internal/vm/operators.hpp"
#include "script/internal/vm/typed.hpp"
#include "builtins_util.hpp"

namespace script {
namespace internal {
namespace vm {

namespace {

using namespace bi;

JSArray* thisArray(VM& vm, JSValue self)
{
    if (!isObject(self) || !hasElems(asObject(self))) {
        typeError(vm, "this is not an array");
        return nullptr;
    }
    return static_cast<JSArray*>(asObject(self));
}

/// The array a read-only method works on: the receiver itself, or, for an array-like (an object with a `length`, a
/// string, a typed array), a copy of its elements that lives as long as this does.
struct ArrayView
{
    Handle holder;
    JSArray* a = nullptr;

    ArrayView(VM& vm, JSValue self)
    {
        if (hasElems(self)) {
            a = static_cast<JSArray*>(asObject(self));
            return;
        }
        if (self.isNullish()) {
            typeError(vm, "Array.prototype method called on incompatible receiver " + describe(vm, self));
            return;
        }
        double n = 0;
        if (isString(self)) {
            n = asString(self)->length;
        } else {
            JSValue len = getProperty(vm, self, vm.atoms.length);
            if (len.isException())
                return;
            const bool ok = toNumber(vm, len, &n);
            release(len);
            if (!ok)
                return;
            n = n != n ? 0 : std::max(0.0, std::trunc(n));
            if (n > 4294967295.0) {
                vm.throwError(ErrorCode::RangeError, "Invalid array length");
                return;
            }
        }
        JSArray* copy = vm.newArray(vm.protos.array, 0, std::uint32_t(std::min(n, 1048576.0)));
        holder = Handle::adopt(toJSValue(copy));
        for (std::uint32_t i = 0; i < std::uint32_t(n); ++i) {
            Key k = toPropertyKey(vm, JSValue::boxNumber(double(i)));
            if (!k)
                return;
            const bool has = isString(self) || hasProperty(vm, self, k);
            JSValue v = has ? getProperty(vm, self, k) : JSValue::hole();
            detail::decRef(k);
            if (v.isException())
                return;
            copy->append(vm, v);
        }
        a = copy;
    }
};

bool numberArg(VM& vm, JSValue v, double* out)
{
    return toNumber(vm, v, out);
}

/// A relative index, as `slice` and friends clamp it.
bool relativeIndex(VM& vm, JSValue v, std::int64_t length, std::int64_t fallback, std::int64_t* out)
{
    if (v.isUndefined()) {
        *out = fallback;
        return true;
    }
    double d;
    if (!toNumber(vm, v, &d))
        return false;
    if (d != d)
        d = 0;
    d = std::trunc(d);
    if (d < 0)
        d = std::max(0.0, double(length) + d);
    else
        d = std::min(d, double(length));
    *out = std::int64_t(d);
    return true;
}

// ------------------------------------------------------------------- global ---

NATIVE(consoleLog)
{
    UNUSED();
    std::string line;
    for (std::uint32_t i = 0; i < argc; ++i) {
        if (i)
            line += ' ';
        if (isObject(args[i]) && !isFunction(args[i])) {
            line += host::inspectValue(vm, args[i]);
            continue;
        }
        JSValue s = toStringValue(vm, args[i]);
        if (s.isException()) {
            vm.clearException();
            line += "[object]";
            continue;
        }
        line += toUtf8(asString(s)->text());
        release(s);
    }
    line += '\n';
    vm.ctx.write(fromUtf8(line), data != nullptr);
    return JSValue::undefined();
}

NATIVE(parseIntImpl)
{
    UNUSED();
    JSValue s = toStringValue(vm, arg(args, argc, 0));
    if (s.isException())
        return s;
    StringView text = asString(s)->text();
    std::size_t i = 0;
    while (i < text.size() && (text[i] == u' ' || text[i] == u'\t' || text[i] == u'\n' || text[i] == u'\r'))
        ++i;
    bool negative = false;
    if (i < text.size() && (text[i] == u'-' || text[i] == u'+'))
        negative = text[i++] == u'-';
    double radixD = 10;
    JSValue radixArg = arg(args, argc, 1);
    if (!radixArg.isUndefined() && !toNumber(vm, radixArg, &radixD)) {
        release(s);
        return JSValue::exception();
    }
    int radix = int(toInt32(radixD));
    if (radix == 0)
        radix = 10;
    if ((radix == 16 || radixArg.isUndefined()) && i + 1 < text.size() && text[i] == u'0' && (text[i + 1] == u'x' || text[i + 1] == u'X')) {
        radix = 16;
        i += 2;
    }
    JSValue result;
    if (radix < 2 || radix > 36) {
        result = JSValue::boxDouble(std::nan(""));
    } else {
        double n = 0;
        bool any = false;
        for (; i < text.size(); ++i) {
            const char16_t c = text[i];
            int digit;
            if (c >= u'0' && c <= u'9')
                digit = c - u'0';
            else if (c >= u'a' && c <= u'z')
                digit = c - u'a' + 10;
            else if (c >= u'A' && c <= u'Z')
                digit = c - u'A' + 10;
            else
                break;
            if (digit >= radix)
                break;
            n = n * radix + digit;
            any = true;
        }
        result = any ? JSValue::boxNumber(negative ? -n : n) : JSValue::boxDouble(std::nan(""));
    }
    release(s);
    return result;
}

NATIVE(parseFloatImpl)
{
    UNUSED();
    JSValue s = toStringValue(vm, arg(args, argc, 0));
    if (s.isException())
        return s;
    StringView text = asString(s)->text();
    std::size_t i = 0;
    while (i < text.size() && (text[i] == u' ' || text[i] == u'\t' || text[i] == u'\n' || text[i] == u'\r'))
        ++i;
    // The longest prefix that reads as a decimal literal.
    std::size_t end = i;
    bool digits = false, dot = false, exp = false;
    if (end < text.size() && (text[end] == u'-' || text[end] == u'+'))
        ++end;
    std::size_t sign = end;
    if (text.substr(end, 8) == u"Infinity") {
        release(s);
        return JSValue::boxDouble(text[i] == u'-' ? -std::numeric_limits<double>::infinity() : std::numeric_limits<double>::infinity());
    }
    std::size_t good = i;
    for (; end < text.size(); ++end) {
        const char16_t c = text[end];
        if (c >= u'0' && c <= u'9') {
            digits = true;
            good = end + 1;
        } else if (c == u'.' && !dot && !exp) {
            dot = true;
        } else if ((c == u'e' || c == u'E') && digits && !exp) {
            exp = true;
            if (end + 1 < text.size() && (text[end + 1] == u'-' || text[end + 1] == u'+'))
                ++end;
        } else {
            break;
        }
    }
    (void)sign;
    JSValue result = digits ? JSValue::boxNumber(dyn::stringToNumber(text.substr(i, good - i))) : JSValue::boxDouble(std::nan(""));
    release(s);
    return result;
}

NATIVE(isNaNImpl)
{
    UNUSED();
    double d;
    if (!toNumber(vm, arg(args, argc, 0), &d))
        return JSValue::exception();
    return JSValue::boolean(d != d);
}

NATIVE(isFiniteImpl)
{
    UNUSED();
    double d;
    if (!toNumber(vm, arg(args, argc, 0), &d))
        return JSValue::exception();
    return JSValue::boolean(std::isfinite(d));
}

// ------------------------------------------------------------------- Object ---

NATIVE(objectCtor)
{
    UNUSED();
    const JSValue v = arg(args, argc, 0);
    if (v.isNullish()) {
        if (isObject(self) && !vm.newTarget.isUndefined()) {
            retain(self);
            return self;
        }
        return toJSValue(newPlain(vm, vm.protos.object));
    }
    return toObject(vm, v);
}

NATIVE(objectKeys)
{
    UNUSED();
    JSValue target = arg(args, argc, 0);
    JSArray* out = vm.newArray(vm.protos.array);
    Handle keep = Handle::adopt(toJSValue(out));
    const int mode = int(reinterpret_cast<std::intptr_t>(data));  // 0 keys, 1 values, 2 entries
    if (target.isNullish())
        return typeError(vm, "Cannot convert undefined or null to object");
    if (isString(target)) {
        const std::uint32_t n = asString(target)->length;
        for (std::uint32_t i = 0; i < n; ++i) {
            JSValue idx = toStringValue(vm, JSValue::int32(std::int32_t(i)));
            if (mode == 0)
                out->append(vm, idx);
            else {
                JSValue ch = makeString(vm, asString(target)->text().substr(i, 1));
                if (mode == 1) {
                    release(idx);
                    out->append(vm, ch);
                } else {
                    JSArray* pair = vm.newArray(vm.protos.array);
                    pair->append(vm, idx);
                    pair->append(vm, ch);
                    out->append(vm, toJSValue(pair));
                }
            }
        }
        return keep.take();
    }
    if (!isObject(target))
        return keep.take();
    JSObject* o = asObject(target);
    if (proxyOf(o)) {
        std::vector<Handle> names;
        ownKeys(vm, o, true, false, true, names);
        if (!vm.exception.isUndefined())
            return JSValue::exception();
        for (Handle& name : names) {
            if (mode == 0) {
                retain(name.get());
                out->append(vm, name.get());
                continue;
            }
            Key k = static_cast<Key>(name.get().asCell());
            JSValue v = getProperty(vm, target, k);
            if (v.isException())
                return v;
            if (mode == 1) {
                out->append(vm, v);
            } else {
                JSArray* pair = vm.newArray(vm.protos.array);
                retain(name.get());
                pair->append(vm, name.get());
                pair->append(vm, v);
                out->append(vm, toJSValue(pair));
            }
        }
        return keep.take();
    }
    if (o->cls() == ObjClass::TypedArray) {
        auto* ta = static_cast<JSTypedArray*>(o);
        for (std::uint32_t i = 0; i < ta->length; ++i) {
            JSValue idx = toStringValue(vm, JSValue::int32(std::int32_t(i)));
            if (mode == 0) {
                out->append(vm, idx);
                continue;
            }
            JSValue v = typedGet(vm, ta, i);
            if (mode == 1) {
                release(idx);
                out->append(vm, v);
            } else {
                JSArray* pair = vm.newArray(vm.protos.array);
                pair->append(vm, idx);
                pair->append(vm, v);
                out->append(vm, toJSValue(pair));
            }
        }
        return keep.take();
    }
    std::vector<std::pair<std::uint32_t, Key>> indexed;
    std::vector<std::pair<Key, JSValue>> named;
    if (hasElems(o)) {
        auto* a = static_cast<JSArray*>(o);
        for (std::uint32_t i = 0; i < a->length; ++i)
            if (!a->elems[i].isHole())
                indexed.push_back({i, nullptr});
    }
    if (o->cls() == ObjClass::String) {
        JSValue text = unwrapped(toJSValue(o), ObjClass::String);
        if (isString(text))
            for (std::uint32_t i = 0; i < asString(text)->length; ++i)
                indexed.push_back({i, nullptr});
    }
    o->forEachOwn(vm, [&](Key k, std::uint8_t attrs, JSValue v) {
        if (!(attrs & attr::kEnumerable) || k->kind != detail::CellKind::String)
            return;
        std::uint32_t index;
        if (arrayIndexOf(k, &index))
            indexed.push_back({index, k});
        else
            named.push_back({k, v});
    });
    std::sort(indexed.begin(), indexed.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
    auto emit = [&](Key k, JSValue v) -> bool {
        retain(JSValue::cell(k));
        JSValue keyValue = JSValue::cell(k);
        if (mode == 0) {
            out->append(vm, keyValue);
            return true;
        }
        JSValue propValue = v;
        if (mode == 1) {
            retain(propValue);
            release(keyValue);
            out->append(vm, propValue);
            return true;
        }
        JSArray* pair = vm.newArray(vm.protos.array);
        pair->append(vm, keyValue);
        retain(propValue);
        pair->append(vm, propValue);
        out->append(vm, toJSValue(pair));
        return true;
    };
    for (auto& [index, key] : indexed) {
        Key k = key ? key : toPropertyKey(vm, JSValue::boxNumber(double(index)));
        JSValue v = hasElems(o) && !key ? static_cast<JSArray*>(o)->at(index) : JSValue::undefined();
        Handle ch;
        if (!key && o->cls() == ObjClass::String) {
            JSValue text = unwrapped(toJSValue(o), ObjClass::String);
            if (isString(text) && index < asString(text)->length) {
                ch = Handle::adopt(makeString(vm, asString(text)->text().substr(index, 1)));
                v = ch.get();
            }
        }
        if (key) {
            JSValue* slot = o->findOwn(vm, key);
            v = slot ? *slot : JSValue::undefined();
        }
        emit(k, v);
        if (!key)
            release(JSValue::cell(k));
    }
    for (auto& [k, v] : named)
        emit(k, v);
    return keep.take();
}

NATIVE(objectGetPrototypeOf)
{
    UNUSED();
    JSValue v = arg(args, argc, 0);
    if (v.isNullish())
        return typeError(vm, "Cannot convert undefined or null to object");
    JSObject* p = nullptr;
    if (isObject(v))
        p = asObject(v)->proto(vm);
    else if (v.isNumber())
        p = vm.protos.number;
    else if (isString(v))
        p = vm.protos.string;
    else if (v.isBool())
        p = vm.protos.boolean;
    if (!p)
        return JSValue::null();
    retain(toJSValue(p));
    return toJSValue(p);
}

NATIVE(objectSetPrototypeOf)
{
    UNUSED();
    JSValue o = arg(args, argc, 0), p = arg(args, argc, 1);
    if (!isObject(o)) {
        retain(o);
        return o;
    }
    if (!isObject(p) && !p.isNull())
        return typeError(vm, "Object prototype may only be an Object or null");
    if (isObject(p) && asObject(o)->wouldCycle(vm, asObject(p)))
        return typeError(vm, "cyclic prototype chain");
    asObject(o)->setProto(vm, isObject(p) ? asObject(p) : nullptr);
    retain(o);
    return o;
}

NATIVE(objectCreate)
{
    UNUSED();
    JSValue p = arg(args, argc, 0);
    if (!isObject(p) && !p.isNull())
        return typeError(vm, "Object prototype may only be an Object or null");
    JSValue made = toJSValue(newPlain(vm, isObject(p) ? asObject(p) : nullptr));
    const JSValue props = arg(args, argc, 1);
    if (props.isUndefined())
        return made;
    // The properties go through `Object.defineProperties`.
    Handle keep = Handle::adopt(made);
    Key objectKey = vm.internKey(std::string_view("Object")), definePropsKey = vm.internKey(std::string_view("defineProperties"));
    JSValue ctor = getProperty(vm, toJSValue(vm.global), objectKey);
    detail::decRef(objectKey);
    if (ctor.isException()) {
        detail::decRef(definePropsKey);
        return ctor;
    }
    Handle ctorKeep = Handle::adopt(ctor);
    JSValue fn = getProperty(vm, ctor, definePropsKey);
    detail::decRef(definePropsKey);
    if (fn.isException())
        return fn;
    Handle fnKeep = Handle::adopt(fn);
    const JSValue callArgs[2] = {made, props};
    JSValue r = vm.call(fn, ctor, callArgs, 2);
    if (r.isException())
        return r;
    release(r);
    return keep.take();
}

bool descriptorField(VM& vm, JSObject* d, const char* name, JSValue* out)
{
    Key key = vm.internKey(std::string_view(name));
    const bool has = hasProperty(vm, toJSValue(d), key);
    if (has) {
        *out = getProperty(vm, toJSValue(d), key);
    }
    detail::decRef(key);
    return has;
}

/// Define `key` on `o` as the descriptor object `d` says. Undefined, or the exception sentinel.
/// A property descriptor object, read the way ToPropertyDescriptor does: in order, noting which fields are there.
struct PropDesc
{
    bool hasValue = false, hasWritable = false, hasGet = false, hasSet = false, hasEnumerable = false, hasConfigurable = false;
    bool writable = false, enumerable = false, configurable = false;
    Handle value, get, set;
    bool isAccessor() const { return hasGet || hasSet; }
    bool isData() const { return hasValue || hasWritable; }
};

bool readPropDesc(VM& vm, JSObject* d, PropDesc* out)
{
    auto field = [&](const char* name, bool* has, Handle* keep) -> bool {
        JSValue v = JSValue::undefined();
        Key key = vm.internKey(std::string_view(name));
        const bool present = hasProperty(vm, toJSValue(d), key);
        if (present) {
            v = getProperty(vm, toJSValue(d), key);
            if (v.isException()) {
                detail::decRef(key);
                return false;
            }
            *keep = Handle::adopt(v);
        }
        detail::decRef(key);
        *has = present;
        return true;
    };
    Handle flag;
    if (!field("enumerable", &out->hasEnumerable, &flag))
        return false;
    out->enumerable = out->hasEnumerable && toBoolean(flag.get());
    flag = Handle();
    if (!field("configurable", &out->hasConfigurable, &flag))
        return false;
    out->configurable = out->hasConfigurable && toBoolean(flag.get());
    flag = Handle();
    if (!field("value", &out->hasValue, &out->value) || !field("writable", &out->hasWritable, &flag))
        return false;
    out->writable = out->hasWritable && toBoolean(flag.get());
    if (!field("get", &out->hasGet, &out->get))
        return false;
    if (out->hasGet && !out->get.get().isUndefined() && !isFunction(out->get.get())) {
        typeError(vm, "Getter must be a function: " + describe(vm, out->get.get()));
        return false;
    }
    if (!field("set", &out->hasSet, &out->set))
        return false;
    if (out->hasSet && !out->set.get().isUndefined() && !isFunction(out->set.get())) {
        typeError(vm, "Setter must be a function: " + describe(vm, out->set.get()));
        return false;
    }
    if (out->isAccessor() && out->isData()) {
        typeError(vm, "Invalid property descriptor. Cannot both specify accessors and a value or writable attribute");
        return false;
    }
    return true;
}

/// A property key as an error message shows it.
std::string keyText(Key key)
{
    if (key->kind == detail::CellKind::Symbol) {
        const auto* sym = static_cast<const dyn::SymbolCell*>(key);
        return "Symbol(" + toUtf8(sym->description) + ")";
    }
    return toUtf8(static_cast<StringCell*>(key)->text());
}

bool sameValueOf(JSValue a, JSValue b)
{
    if (a.isNumber() && b.isNumber()) {
        const double x = a.asNumber(), y = b.asNumber();
        if (x != x && y != y)
            return true;
        return x == y && std::signbit(x) == std::signbit(y);
    }
    return strictEquals(a, b);
}

JSValue definePropertyImpl(VM& vm, JSObject* o, Key key, JSObject* d)
{
    if (proxyOf(o))
        return proxyDefine(vm, o, key, d);
    PropDesc desc;
    if (!readPropDesc(vm, d, &desc))
        return JSValue::exception();
    const JSValue vv = desc.hasValue ? desc.value.get() : JSValue::undefined();
    auto reject = [&]() { return typeError(vm, "Cannot redefine property: " + keyText(key)); };

    // What an array's own `length` and indices, a typed array's elements and a String's characters allow is their own rule.
    std::uint32_t index;
    const bool indexKey = arrayIndexOf(key, &index);
    const bool exotic = (hasElems(o) && (indexKey || key == vm.atoms.length)) || o->cls() == ObjClass::TypedArray ||
                        (o->cls() == ObjClass::String && (indexKey || key == vm.atoms.length));
    if (exotic) {
        std::uint8_t existing = attr::kDefault;
        bool exists = false;
        if (hasElems(o)) {
            exists = key == vm.atoms.length || (indexKey && !static_cast<JSArray*>(o)->at(index).isHole());
            if (key == vm.atoms.length)
                existing = (o->h.flags & cellflags::kFrozen) ? 0 : attr::kWritable;
        }
        if (!exists)
            exists = o->findOwn(vm, key, &existing) != nullptr;
        std::uint8_t attrs = 0;
        auto bit = [&](bool has, bool value, std::uint8_t mask) {
            if (has ? value : (exists && (existing & mask)))
                attrs |= mask;
        };
        bit(desc.hasWritable, desc.writable, attr::kWritable);
        bit(desc.hasEnumerable, desc.enumerable, attr::kEnumerable);
        bit(desc.hasConfigurable, desc.configurable, attr::kConfigurable);
        bool ok;
        if (desc.isAccessor()) {
            ok = defineAccessor(vm, o, key, desc.get.get(), desc.set.get(), std::uint8_t(attrs & ~attr::kWritable));
        } else {
            JSValue value = vv;
            Handle current;
            if (!desc.hasValue) {
                JSValue got = exists ? getProperty(vm, toJSValue(o), key) : JSValue::undefined();
                if (got.isException())
                    return got;
                current = Handle::adopt(got);
                value = got;
            }
            retain(value);
            ok = defineOwn(vm, o, key, value, attrs);
        }
        return ok ? JSValue::undefined() : reject();
    }

    std::uint8_t cur = 0;
    JSValue* slot = o->findOwn(vm, key, &cur);
    if (!slot) {
        if (o->h.flags & cellflags::kNotExtensible)
            return typeError(vm, "Cannot define property " + keyText(key) + ", object is not extensible");
        std::uint8_t attrs = 0;
        if (desc.enumerable)
            attrs |= attr::kEnumerable;
        if (desc.configurable)
            attrs |= attr::kConfigurable;
        if (desc.isAccessor()) {
            if (!defineAccessor(vm, o, key, desc.hasGet ? desc.get.get() : JSValue::undefined(),
                                desc.hasSet ? desc.set.get() : JSValue::undefined(), attrs))
                return reject();
        } else {
            if (desc.writable)
                attrs |= attr::kWritable;
            retain(vv);
            if (!defineOwn(vm, o, key, vv, attrs))
                return reject();
        }
        return JSValue::undefined();
    }

    const bool curAccessor = (cur & attr::kAccessor) != 0;
    const bool curConfigurable = (cur & attr::kConfigurable) != 0;
    const bool curEnumerable = (cur & attr::kEnumerable) != 0;
    const bool curWritable = (cur & attr::kWritable) != 0;
    GetterSetter* curPair = curAccessor ? reinterpret_cast<GetterSetter*>(slot->asCell()) : nullptr;
    if (!curConfigurable) {
        if (desc.hasConfigurable && desc.configurable)
            return reject();
        if (desc.hasEnumerable && desc.enumerable != curEnumerable)
            return reject();
        const bool generic = !desc.isAccessor() && !desc.isData();
        if (!generic) {
            if (curAccessor != desc.isAccessor())
                return reject();
            if (curAccessor) {
                if (desc.hasGet && !sameValueOf(desc.get.get(), curPair->getter))
                    return reject();
                if (desc.hasSet && !sameValueOf(desc.set.get(), curPair->setter))
                    return reject();
            } else if (!curWritable) {
                if (desc.hasWritable && desc.writable)
                    return reject();
                if (desc.hasValue && !sameValueOf(vv, *slot))
                    return reject();
            }
        }
    }
    // Apply: what the descriptor does not mention stays, except across a change between data and accessor.
    std::uint8_t attrs = 0;
    if (desc.hasEnumerable ? desc.enumerable : curEnumerable)
        attrs |= attr::kEnumerable;
    if (desc.hasConfigurable ? desc.configurable : curConfigurable)
        attrs |= attr::kConfigurable;
    auto replaceValue = [&](JSValue fresh, std::uint8_t newAttrs) {
        // `fresh` carries a count that the slot takes.
        o->setAttrs(vm, key, newAttrs);
        JSValue* where = o->findOwn(vm, key);
        const JSValue old = *where;
        *where = fresh;
        release(old);
    };
    if (desc.isAccessor()) {
        const JSValue getter = desc.hasGet ? desc.get.get() : (curAccessor ? curPair->getter : JSValue::undefined());
        const JSValue setter = desc.hasSet ? desc.set.get() : (curAccessor ? curPair->setter : JSValue::undefined());
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
        replaceValue(JSValue::cell(reinterpret_cast<CellPtr>(gs)), std::uint8_t(attrs | attr::kAccessor));
        return JSValue::undefined();
    }
    if (desc.isData() || !curAccessor) {
        std::uint8_t dataAttrs = attrs;
        const bool writable = desc.hasWritable ? desc.writable : (curAccessor ? false : curWritable);
        if (writable)
            dataAttrs |= attr::kWritable;
        JSValue fresh = desc.hasValue ? vv : (curAccessor ? JSValue::undefined() : *slot);
        retain(fresh);
        replaceValue(fresh, dataAttrs);
        return JSValue::undefined();
    }
    // An accessor given only attributes.
    if (std::uint8_t(attrs | attr::kAccessor) != cur) {
        o->setAttrs(vm, key, std::uint8_t(attrs | attr::kAccessor));
    }
    return JSValue::undefined();
}

NATIVE(objectDefineProperty)
{
    UNUSED();
    JSValue target = arg(args, argc, 0), keyValue = arg(args, argc, 1), desc = arg(args, argc, 2);
    if (!isObject(target))
        return typeError(vm, "Object.defineProperty called on non-object");
    if (!isObject(desc))
        return typeError(vm, "Property description must be an object");
    JSValue prim = toPrimitive(vm, keyValue, true);
    if (prim.isException())
        return prim;
    Key key = toPropertyKey(vm, prim);
    release(prim);
    if (!key)
        return JSValue::exception();
    JSValue r = definePropertyImpl(vm, asObject(target), key, asObject(desc));
    detail::decRef(key);
    if (r.isException())
        return r;
    retain(target);
    return target;
}

NATIVE(objectFreeze)
{
    UNUSED();
    JSValue v = arg(args, argc, 0);
    if (isObject(v)) {
        JSObject* o = asObject(v);
        o->h.flags |= cellflags::kFrozen | cellflags::kNotExtensible;
        std::vector<Key> keys;
        o->forEachOwn(vm, [&](Key k, std::uint8_t, JSValue) {
            if (k->kind != detail::CellKind::Symbol || !static_cast<dyn::SymbolCell*>(k)->isPrivate)
                keys.push_back(k);
        });
        for (Key k : keys) {
            std::uint8_t attrs = 0;
            o->findOwn(vm, k, &attrs);
            std::uint8_t frozen = attrs & ~attr::kConfigurable;
            if (!(attrs & attr::kAccessor))
                frozen &= ~attr::kWritable;
            o->setAttrs(vm, k, frozen);
        }
    }
    retain(v);
    return v;
}

NATIVE(objectIsFrozen)
{
    UNUSED();
    JSValue v = arg(args, argc, 0);
    return JSValue::boolean(!isObject(v) || (asObject(v)->h.flags & cellflags::kFrozen));
}

NATIVE(objectAssign)
{
    UNUSED();
    JSValue target = arg(args, argc, 0);
    if (target.isNullish())
        return typeError(vm, "Cannot convert undefined or null to object");
    for (std::uint32_t i = 1; i < argc; ++i) {
        if (args[i].isNullish())
            continue;
        JSValue source = toObject(vm, args[i]);
        if (source.isException())
            return source;
        Handle sourceKeep = Handle::adopt(source);
        std::vector<Handle> keys;
        ownKeys(vm, asObject(source), true, true, true, keys);
        for (Handle& key : keys) {
            const Key k = static_cast<Key>(key.get().asCell());
            JSValue v = getProperty(vm, source, k);
            if (v.isException())
                return v;
            Handle valueKeep = Handle::adopt(v);
            if (!setProperty(vm, target, k, v, true))
                return JSValue::exception();
        }
    }
    retain(target);
    return target;
}

NATIVE(hasOwnProperty)
{
    UNUSED();
    JSValue prim = toPrimitive(vm, arg(args, argc, 0), true);
    if (prim.isException())
        return prim;
    Key key = toPropertyKey(vm, prim);
    release(prim);
    if (!key)
        return JSValue::exception();
    bool has = false;
    if (isObject(self)) {
        JSObject* o = asObject(self);
        std::uint32_t index;
        if (hasElems(o)) {
            auto* a = static_cast<JSArray*>(o);
            if (key == vm.atoms.length || (arrayIndexOf(key, &index) && !a->at(index).isHole()))
                has = true;
        }
        if (o->cls() == ObjClass::String) {
            JSValue text = unwrapped(self, ObjClass::String);
            if (isString(text) && (key == vm.atoms.length || (arrayIndexOf(key, &index) && index < asString(text)->length)))
                has = true;
        }
        has = has || o->findOwn(vm, key) != nullptr;
    } else if (isString(self)) {
        std::uint32_t index;
        has = key == vm.atoms.length || (arrayIndexOf(key, &index) && index < asString(self)->length);
    }
    detail::decRef(key);
    return JSValue::boolean(has);
}

NATIVE(objectToString)
{
    UNUSED();
    const char* tag = "[object Object]";
    if (self.isUndefined())
        tag = "[object Undefined]";
    else if (self.isNull())
        tag = "[object Null]";
    else if (isArray(self))
        tag = "[object Array]";
    else if (isObject(self) && asObject(self)->cls() == ObjClass::Arguments)
        tag = "[object Arguments]";
    else if (isFunction(self))
        tag = "[object Function]";
    else if (isObject(self) && asObject(self)->cls() == ObjClass::Error)
        tag = "[object Error]";
    else if (self.isNumber() || (isObject(self) && asObject(self)->cls() == ObjClass::Number))
        tag = "[object Number]";
    else if (isString(self) || (isObject(self) && asObject(self)->cls() == ObjClass::String))
        tag = "[object String]";
    else if (self.isBool() || (isObject(self) && asObject(self)->cls() == ObjClass::Boolean))
        tag = "[object Boolean]";
    else if (isObject(self) && asObject(self)->cls() == ObjClass::Date)
        tag = "[object Date]";
    else if (isObject(self) && asObject(self)->cls() == ObjClass::RegExp)
        tag = "[object RegExp]";
    if (!self.isNullish()) {
        JSValue custom = getProperty(vm, self, vm.atoms.symToStringTag);
        if (custom.isException())
            return custom;
        Handle keep = Handle::adopt(custom);
        if (isString(custom))
            return makeString(vm, StringView(String(u"[object ") + String(asString(custom)->text()) + u"]"));
    }
    return str(vm, tag);
}

NATIVE(objectValueOf)
{
    UNUSED();
    return toObject(vm, self);
}

NATIVE(isPrototypeOf)
{
    UNUSED();
    JSValue v = arg(args, argc, 0);
    if (!isObject(v) || !isObject(self))
        return JSValue::boolean(false);
    for (JSObject* p = asObject(v)->proto(vm); p; p = p->proto(vm))
        if (toJSValue(p) == self)
            return JSValue::boolean(true);
    return JSValue::boolean(false);
}

// ----------------------------------------------------------------- Function ---

NATIVE(functionCall)
{
    UNUSED();
    return vm.call(self, arg(args, argc, 0), argc > 1 ? args + 1 : nullptr, argc > 1 ? argc - 1 : 0);
}

NATIVE(functionApply)
{
    UNUSED();
    JSValue list = arg(args, argc, 1);
    std::vector<Handle> held;
    if (!list.isNullish() && !listFromArrayLike(vm, list, held))
        return JSValue::exception();
    std::vector<JSValue> values;
    for (const Handle& h : held)
        values.push_back(h.get());
    return vm.call(self, arg(args, argc, 0), values.data(), std::uint32_t(values.size()));
}

NATIVE(functionToString)
{
    UNUSED();
    if (!isFunction(self))
        return typeError(vm, "Function.prototype.toString requires that 'this' be a Function");
    auto* f = static_cast<JSFunction*>(asObject(self));
    std::string name;
    if (f->flags & JSFunction::kNative) {
        if (Rare* r = f->rare(); r && r->fnName)
            name = toUtf8(static_cast<StringCell*>(r->fnName)->text());
        return str(vm, "function " + name + "() { [native code] }");
    }
    if (f->code->source && f->code->srcEnd > f->code->srcStart && f->code->srcEnd <= f->code->source->size())
        return str(vm, std::string_view(*f->code->source).substr(f->code->srcStart, f->code->srcEnd - f->code->srcStart));
    if (f->code->name)
        name = toUtf8(static_cast<StringCell*>(f->code->name)->text());
    return str(vm, "function " + name + "() { [native code] }");
}

/// What a bound function runs: `data` is the function itself, which keeps what it is bound to in its rare state.
NATIVE(boundCall)
{
    UNUSED();
    auto* me = static_cast<JSFunction*>(data);
    Rare* r = me->rare();
    std::vector<JSValue> all;
    if (r->boundArgs)
        all.assign(r->boundArgs->elems, r->boundArgs->elems + r->boundArgs->length);
    all.insert(all.end(), args, args + argc);
    if (!vm.newTarget.isUndefined())
        return vm.construct(r->boundTarget, all.data(), std::uint32_t(all.size()));
    return vm.call(r->boundTarget, r->boundThis, all.data(), std::uint32_t(all.size()));
}

NATIVE(functionBind)
{
    UNUSED();
    if (!isFunction(self))
        return typeError(vm, "Bind must be called on a function");
    auto* target = static_cast<JSFunction*>(asObject(self));
    JSValue targetName = getProperty(vm, self, vm.atoms.name);
    if (targetName.isException())
        return targetName;
    std::string name = "bound " + (isString(targetName) ? toUtf8(asString(targetName)->text()) : std::string());
    release(targetName);
    Key nameKey = vm.internKey(StringView(fromUtf8(name)));
    const std::uint32_t bound = argc > 1 ? argc - 1 : 0;
    std::uint32_t length = 0;
    if (JSValue len = getProperty(vm, self, vm.atoms.length); len.isNumber()) {
        const double d = len.asNumber() - bound;
        length = d > 0 ? std::uint32_t(d) : 0;
    } else if (len.isException()) {
        detail::decRef(nameKey);
        return len;
    }
    JSFunction* f = vm.newNativeFunction(vm.protos.function, boundCall, nullptr, nameKey, length);
    detail::decRef(nameKey);
    Handle keep = Handle::adopt(toJSValue(f));
    Rare* r = f->rare();
    r->nativeData = f;
    retain(self);
    r->boundTarget = self;
    r->boundThis = arg(args, argc, 0);
    retain(r->boundThis);
    r->boundArgs = vm.newArray(vm.protos.array);
    if (bound)
        r->boundArgs->appendAll(vm, args + 1, bound);
    const bool constructible = (target->flags & JSFunction::kNative) ? (target->flags & JSFunction::kConstructor) != 0 : target->code->constructor;
    if (constructible)
        f->flags |= JSFunction::kConstructor;
    return keep.take();
}

// -------------------------------------------------------------------- Error ---

NATIVE(errorToString);

JSValue fillError(VM& vm, JSObject* o, JSValue message, JSValue options = JSValue::undefined())
{
    o->h.cls = std::uint8_t(ObjClass::Error);
    if (!message.isUndefined()) {
        JSValue text = toStringValue(vm, message);
        if (text.isException())
            return text;
        value(vm, o, "message", text);
    }
    if (isObject(options)) {
        Key causeKey = vm.internKey(std::string_view("cause"));
        if (hasProperty(vm, options, causeKey)) {
            JSValue cause = getProperty(vm, options, causeKey);
            if (cause.isException()) {
                detail::decRef(causeKey);
                return cause;
            }
            o->putOwn(vm, causeKey, cause, attr::kWritable | attr::kConfigurable);
        }
        detail::decRef(causeKey);
    }
    return JSValue::undefined();
}

/// `error.stack`: the error's text and a place to put the frames; made when asked for.
NATIVE(errorStackGetter)
{
    UNUSED();
    if (!isObject(self))
        return JSValue::undefined();
    JSValue text = errorToString(vm, self, nullptr, 0, nullptr);
    if (text.isException())
        return text;
    Handle keep = Handle::adopt(text);
    return makeString(vm, StringView(String(asString(text)->text()) + u"\n    at <anonymous>"));
}

NATIVE(errorStackSetter)
{
    UNUSED();
    if (isObject(self)) {
        Key k = vm.internKey(std::string_view("stack"));
        JSValue v = arg(args, argc, 0);
        retain(v);
        asObject(self)->putOwn(vm, k, v, attr::kWritable | attr::kConfigurable);
        detail::decRef(k);
    }
    return JSValue::undefined();
}

NATIVE(aggregateErrorCtor)
{
    UNUSED();
    JSObject* made;
    if (!vm.newTarget.isUndefined() && isObject(self)) {
        made = asObject(self);
        retain(self);
    } else {
        made = newPlain(vm, reinterpret_cast<JSObject*>(data));
    }
    Handle keep = Handle::adopt(toJSValue(made));
    JSValue r = fillError(vm, made, arg(args, argc, 1), arg(args, argc, 2));
    if (r.isException())
        return r;
    JSArray* errors = vm.newArray(vm.protos.array);
    Handle errorsKeep = Handle::adopt(toJSValue(errors));
    if (!forEachIterated(vm, arg(args, argc, 0), [&](JSValue v) {
            retain(v);
            errors->append(vm, v);
            return Step::Continue;
        }))
        return JSValue::exception();
    Key k = vm.internKey(std::string_view("errors"));
    retain(errorsKeep.get());
    made->putOwn(vm, k, errorsKeep.get(), attr::kWritable | attr::kConfigurable);
    detail::decRef(k);
    return keep.take();
}

NATIVE(errorCtor)
{
    UNUSED();
    JSObject* made;
    if (!vm.newTarget.isUndefined() && isObject(self)) {
        made = asObject(self);
        retain(self);
    } else {
        made = newPlain(vm, reinterpret_cast<JSObject*>(data));
    }
    Handle keep = Handle::adopt(toJSValue(made));
    JSValue r = fillError(vm, made, arg(args, argc, 0), arg(args, argc, 1));
    if (r.isException())
        return r;
    return keep.take();
}

NATIVE(errorToString)
{
    UNUSED();
    if (!isObject(self))
        return typeError(vm, "Error.prototype.toString requires that 'this' be an Object");
    Key nameKey = vm.internKey(std::string_view("name")), messageKey = vm.internKey(std::string_view("message"));
    JSValue name = getProperty(vm, self, nameKey), message = getProperty(vm, self, messageKey);
    detail::decRef(nameKey);
    detail::decRef(messageKey);
    if (name.isException() || message.isException()) {
        release(name.isException() ? JSValue::undefined() : name);
        return JSValue::exception();
    }
    std::string n = name.isUndefined() ? "Error" : describe(vm, name), m = message.isUndefined() ? "" : describe(vm, message);
    release(name);
    release(message);
    if (n.empty())
        return str(vm, m);
    if (m.empty())
        return str(vm, n);
    return str(vm, n + ": " + m);
}

// -------------------------------------------------------------------- Array ---

NATIVE(arrayCtor)
{
    UNUSED();
    JSArray* a = vm.newArray(vm.protos.array);
    Handle keep = Handle::adopt(toJSValue(a));
    if (argc == 1 && args[0].isNumber()) {
        const double d = args[0].asNumber();
        if (d < 0 || d != std::floor(d) || d > 4294967295.0)
            return vm.throwError(ErrorCode::RangeError, "Invalid array length");
        if (d > double(JSArray::kMaxDenseGap))
            return vm.throwError(ErrorCode::RangeError, "arrays are dense; this length is too large");
        a->resize(vm, std::uint32_t(d));
    } else {
        a->appendAll(vm, args, argc);
    }
    return keep.take();
}

NATIVE(arrayIsArray)
{
    UNUSED();
    JSValue v = arg(args, argc, 0);
    // A proxy is an array when what it stands for is.
    while (isObject(v) && proxyOf(asObject(v))) {
        ProxyState* ps = proxyOf(asObject(v));
        if (!isObject(ps->handler))
            return typeError(vm, "Cannot perform 'IsArray' on a proxy that has been revoked");
        v = ps->target;
    }
    return JSValue::boolean(isArray(v));
}

/// The array a mutating method works on: frozen arrays refuse.
static JSArray* thisMutableArray(VM& vm, JSValue self)
{
    JSArray* a = thisArray(vm, self);
    if (a && (a->h.flags & cellflags::kFrozen)) {
        vm.throwError(ErrorCode::TypeError, "the array is frozen");
        return nullptr;
    }
    return a;
}

NATIVE(arrayPushImpl)
{
    UNUSED();
    JSArray* a = thisMutableArray(vm, self);
    if (!a)
        return JSValue::exception();
    if (a->h.flags & cellflags::kNotExtensible)
        return typeError(vm, "Cannot add property, object is not extensible");
    a->appendAll(vm, args, argc);
    return JSValue::boxNumber(double(a->length));
}

NATIVE(arrayPopImpl)
{
    UNUSED();
    JSArray* a = thisMutableArray(vm, self);
    if (!a)
        return JSValue::exception();
    JSValue v = a->pop(vm);
    return v.isHole() ? JSValue::undefined() : v;
}

NATIVE(arrayShiftImpl)
{
    UNUSED();
    JSArray* a = thisMutableArray(vm, self);
    if (!a)
        return JSValue::exception();
    if (!a->length)
        return JSValue::undefined();
    JSValue v = a->elems[0];
    retain(v);
    a->removeRange(vm, 0, 1);
    return v.isHole() ? JSValue::undefined() : v;
}

NATIVE(arrayUnshiftImpl)
{
    UNUSED();
    JSArray* a = thisMutableArray(vm, self);
    if (!a)
        return JSValue::exception();
    a->insertGap(vm, 0, argc);
    for (std::uint32_t i = 0; i < argc; ++i) {
        retain(args[i]);
        a->elems[i] = args[i];
    }
    return JSValue::boxNumber(double(a->length));
}

NATIVE(arrayJoin)
{
    UNUSED();
    ArrayView view(vm, self);
    JSArray* a = view.a;
    if (!a)
        return JSValue::exception();
    String sep = u",";
    JSValue sepArg = arg(args, argc, 0);
    if (!sepArg.isUndefined()) {
        JSValue s = toStringValue(vm, sepArg);
        if (s.isException())
            return s;
        sep = String(asString(s)->text());
        release(s);
    }
    String out;
    retain(self);
    Handle keep = Handle::adopt(self);
    for (std::uint32_t i = 0; i < a->length; ++i) {
        if (i)
            out += sep;
        JSValue e = a->elems[i];
        if (e.isNullish() || e.isHole())
            continue;
        JSValue s = toStringValue(vm, e);
        if (s.isException())
            return s;
        out += asString(s)->text();
        release(s);
    }
    return makeString(vm, StringView(out));
}

NATIVE(arrayToString)
{
    UNUSED();
    if (isArray(self))
        return arrayJoin(vm, self, nullptr, 0, data);
    if (isObject(self)) {
        Key joinKey = vm.internKey(std::string_view("join"));
        JSValue join = getProperty(vm, self, joinKey);
        detail::decRef(joinKey);
        if (join.isException())
            return join;
        Handle keep = Handle::adopt(join);
        if (isFunction(join))
            return vm.call(join, self, nullptr, 0);
    }
    return objectToString(vm, self, args, argc, data);
}

NATIVE(arraySlice)
{
    UNUSED();
    ArrayView view(vm, self);
    JSArray* a = view.a;
    if (!a)
        return JSValue::exception();
    std::int64_t from, to;
    if (!relativeIndex(vm, arg(args, argc, 0), a->length, 0, &from) || !relativeIndex(vm, arg(args, argc, 1), a->length, a->length, &to))
        return JSValue::exception();
    JSArray* out = vm.newArray(vm.protos.array);
    Handle keep = Handle::adopt(toJSValue(out));
    for (std::int64_t i = from; i < to; ++i)
        out->appendAll(vm, &a->elems[i], 1);
    return keep.take();
}

NATIVE(arraySpliceImpl)
{
    UNUSED();
    JSArray* a = thisMutableArray(vm, self);
    if (!a)
        return JSValue::exception();
    std::int64_t start, count;
    if (!relativeIndex(vm, arg(args, argc, 0), a->length, 0, &start))
        return JSValue::exception();
    if (argc == 0) {
        count = 0;
    } else if (argc == 1) {
        count = a->length - start;
    } else {
        double d;
        if (!toNumber(vm, args[1], &d))
            return JSValue::exception();
        count = std::int64_t(std::min(std::max(d != d ? 0 : std::trunc(d), 0.0), double(a->length - start)));
    }
    JSArray* removed = vm.newArray(vm.protos.array);
    Handle keep = Handle::adopt(toJSValue(removed));
    for (std::int64_t i = 0; i < count; ++i)
        removed->appendAll(vm, &a->elems[start + i], 1);
    a->removeRange(vm, std::uint32_t(start), std::uint32_t(count));
    const std::uint32_t inserted = argc > 2 ? argc - 2 : 0;
    a->insertGap(vm, std::uint32_t(start), inserted);
    for (std::uint32_t i = 0; i < inserted; ++i) {
        retain(args[2 + i]);
        a->elems[start + i] = args[2 + i];
    }
    return keep.take();
}

NATIVE(arrayConcat)
{
    UNUSED();
    if (self.isNullish())
        return typeError(vm, "Array.prototype.concat called on null or undefined");
    JSArray* out = vm.newArray(vm.protos.array);
    Handle keep = Handle::adopt(toJSValue(out));
    for (std::uint32_t i = 0; i <= argc; ++i) {
        const JSValue e = i == 0 ? self : args[i - 1];
        bool spread = false;
        if (isObject(e)) {
            JSValue flag = getProperty(vm, e, vm.atoms.symIsConcatSpreadable);
            if (flag.isException())
                return flag;
            spread = flag.isUndefined() ? isArray(e) : toBoolean(flag);
            release(flag);
        }
        if (!spread) {
            out->appendAll(vm, &e, 1);
            continue;
        }
        ArrayView view(vm, e);
        if (!view.a)
            return JSValue::exception();
        if (std::uint64_t(out->length) + view.a->length > 0xFFFFFFFFull)
            return typeError(vm, "Invalid array length");
        out->appendAll(vm, view.a->elems, view.a->length);
    }
    return keep.take();
}

NATIVE(arrayReverseImpl)
{
    UNUSED();
    JSArray* a = thisMutableArray(vm, self);
    if (!a)
        return JSValue::exception();
    std::reverse(a->elems, a->elems + a->length);
    retain(self);
    return self;
}

/// The element `i` of the array `a` that `self` is (or a copy of what it is): a hole of a real array is looked up on its
/// prototype chain, as the specification's HasProperty and Get do. `*found` says whether there is one; false with an
/// exception pending when a getter threw.
bool elementOrInherited(VM& vm, JSValue self, JSArray* a, std::uint32_t i, JSValue* out, Handle* keep, bool* found)
{
    const JSValue e = a->at(i);
    if (!e.isHole()) {
        *out = e;
        *found = true;
        return true;
    }
    *found = false;
    if (!hasElems(self))
        return true;
    Key k = toPropertyKey(vm, JSValue::boxNumber(double(i)));
    if (!k)
        return false;
    // Only an object on the chain can have it: the own part of an array is what was just read.
    JSObject* start = asObject(self)->proto(vm);
    const bool anywhere = start && hasProperty(vm, toJSValue(start), k);
    if (anywhere || asObject(self)->findOwn(vm, k)) {
        JSValue v = getProperty(vm, self, k);
        if (v.isException()) {
            detail::decRef(k);
            return false;
        }
        *keep = Handle::adopt(v);
        *out = v;
        *found = true;
    }
    detail::decRef(k);
    return true;
}

NATIVE(arrayIndexOf)
{
    UNUSED();
    ArrayView view(vm, self);
    JSArray* a = view.a;
    if (!a)
        return JSValue::exception();
    const int mode = int(reinterpret_cast<std::intptr_t>(data));  // 0 indexOf, 1 lastIndexOf, 2 includes
    const JSValue needle = arg(args, argc, 0);
    const std::int64_t len = a->length;
    if (mode == 1) {
        std::int64_t from = len - 1;
        if (argc > 1) {
            double d;
            if (!toNumber(vm, args[1], &d))
                return JSValue::exception();
            d = d != d ? 0 : std::trunc(d);
            from = d < 0 ? std::int64_t(std::max(d + double(len), -1.0)) : std::int64_t(std::min(d, double(len) - 1));
        }
        for (std::int64_t i = from; i >= 0; --i) {
            JSValue e;
            Handle keep;
            bool found;
            if (!elementOrInherited(vm, self, a, std::uint32_t(i), &e, &keep, &found))
                return JSValue::exception();
            if (found && strictEquals(e, needle))
                return JSValue::int32(std::int32_t(i));
        }
        return JSValue::int32(-1);
    }
    std::int64_t from;
    if (!relativeIndex(vm, arg(args, argc, 1), len, 0, &from))
        return JSValue::exception();
    for (std::int64_t i = from; i < len; ++i) {
        JSValue e;
        Handle keep;
        bool found;
        if (!elementOrInherited(vm, self, a, std::uint32_t(i), &e, &keep, &found))
            return JSValue::exception();
        if (mode == 2 ? sameValueZero(found ? e : JSValue::undefined(), needle) : (found && strictEquals(e, needle)))
            return mode == 2 ? JSValue::boolean(true) : JSValue::int32(std::int32_t(i));
    }
    return mode == 2 ? JSValue::boolean(false) : JSValue::int32(-1);
}

/// forEach, map, filter, some, every, find, findIndex: one loop, the kind in `data`.
NATIVE(arrayIterate)
{
    UNUSED();
    ArrayView view(vm, self);
    JSArray* a = view.a;
    if (!a)
        return JSValue::exception();
    const int kind = int(reinterpret_cast<std::intptr_t>(data));
    const JSValue callback = arg(args, argc, 0), thisArg = arg(args, argc, 1);
    if (!isFunction(callback))
        return typeError(vm, describe(vm, callback) + " is not a function");
    retain(self);
    Handle keep = Handle::adopt(self);
    JSArray* out = nullptr;
    Handle outKeep;
    if (kind == 1 || kind == 2) {
        out = vm.newArray(vm.protos.array);
        outKeep = Handle::adopt(toJSValue(out));
    }
    // The callback may grow or shrink the array; the loop goes as far as it reached at the start.
    const std::uint32_t len = a->length;
    for (std::uint32_t i = 0; i < len; ++i) {
        JSValue element = a->at(i);
        const bool hole = element.isHole();
        if (hole && kind <= 4)
            continue;
        if (hole)
            element = JSValue::undefined();
        Handle e = Handle::retained(element);
        JSValue call[3] = {e.get(), JSValue::int32(std::int32_t(i)), self};
        JSValue r = vm.call(callback, thisArg, call, 3);
        if (r.isException())
            return r;
        Handle result = Handle::adopt(r);
        switch (kind) {
            case 0: break;
            case 1:
                if (out->length < i)
                    out->resize(vm, i);
                retain(r);
                out->setAt(vm, i, r);
                break;
            case 2:
                if (toBoolean(r)) {
                    retain(e.get());
                    out->append(vm, e.get());
                }
                break;
            case 3:
                if (toBoolean(r))
                    return JSValue::boolean(true);
                break;
            case 4:
                if (!toBoolean(r))
                    return JSValue::boolean(false);
                break;
            case 5:
                if (toBoolean(r))
                    return e.take();
                break;
            case 6:
                if (toBoolean(r))
                    return JSValue::int32(std::int32_t(i));
                break;
        }
    }
    switch (kind) {
        case 0:
        case 5: return JSValue::undefined();
        case 1:
            if (out->length < len)
                out->resize(vm, len);
            return outKeep.get().isUndefined() ? JSValue::undefined() : (retain(outKeep.get()), outKeep.get());
        case 2: return (retain(outKeep.get()), outKeep.get());
        case 3: return JSValue::boolean(false);
        case 4: return JSValue::boolean(true);
        default: return JSValue::int32(-1);
    }
}

NATIVE(arrayReduce)
{
    UNUSED();
    ArrayView view(vm, self);
    JSArray* a = view.a;
    if (!a)
        return JSValue::exception();
    const bool right = data != nullptr;
    const JSValue callback = arg(args, argc, 0);
    if (!isFunction(callback))
        return typeError(vm, describe(vm, callback) + " is not a function");
    retain(self);
    Handle keep = Handle::adopt(self);
    std::int64_t i = right ? std::int64_t(a->length) - 1 : 0;
    const std::int64_t step = right ? -1 : 1;
    Handle acc;
    if (argc >= 2) {
        acc = Handle::retained(args[1]);
    } else {
        while (i >= 0 && i < a->length && a->elems[i].isHole())
            i += step;
        if (i < 0 || i >= a->length)
            return typeError(vm, "Reduce of empty array with no initial value");
        acc = Handle::retained(a->elems[i]);
        i += step;
    }
    for (; i >= 0 && i < a->length; i += step) {
        if (a->elems[i].isHole())
            continue;
        Handle e = Handle::retained(a->elems[i]);
        JSValue call[4] = {acc.get(), e.get(), JSValue::int32(std::int32_t(i)), self};
        JSValue r = vm.call(callback, JSValue::undefined(), call, 4);
        if (r.isException())
            return r;
        acc = Handle::adopt(r);
    }
    return acc.take();
}

NATIVE(arrayFillImpl)
{
    UNUSED();
    JSArray* a = thisMutableArray(vm, self);
    if (!a)
        return JSValue::exception();
    std::int64_t from, to;
    if (!relativeIndex(vm, arg(args, argc, 1), a->length, 0, &from) || !relativeIndex(vm, arg(args, argc, 2), a->length, a->length, &to))
        return JSValue::exception();
    for (std::int64_t i = from; i < to; ++i)
        a->setAt(vm, std::uint32_t(i), (retain(arg(args, argc, 0)), arg(args, argc, 0)));
    retain(self);
    return self;
}

NATIVE(arraySortImpl)
{
    UNUSED();
    JSArray* a = thisMutableArray(vm, self);
    if (!a)
        return JSValue::exception();
    const JSValue compare = arg(args, argc, 0);
    if (!compare.isUndefined() && !isFunction(compare))
        return typeError(vm, "The comparison function must be either a function or undefined");
    retain(self);
    Handle keep = Handle::adopt(self);
    std::vector<JSValue> items;
    std::uint32_t undefinedCount = 0, holes = 0;
    for (std::uint32_t i = 0; i < a->length; ++i) {
        const JSValue e = a->elems[i];
        if (e.isHole())
            ++holes;
        else if (e.isUndefined())
            ++undefinedCount;
        else {
            retain(e);
            items.push_back(e);
        }
    }
    bool failed = false;
    std::vector<JSValue> keys;  // the strings of a default sort
    auto less = [&](JSValue x, JSValue y) -> bool {
        if (failed)
            return false;
        if (!compare.isUndefined()) {
            JSValue pair[2] = {x, y};
            JSValue r = vm.call(compare, JSValue::undefined(), pair, 2);
            if (r.isException()) {
                failed = true;
                return false;
            }
            double d;
            const bool ok = toNumber(vm, r, &d);
            release(r);
            if (!ok) {
                failed = true;
                return false;
            }
            return d < 0;
        }
        if (x.isInt32() && y.isInt32() && x.asInt32() >= 0 && y.asInt32() >= 0) {
            // Digits compare as text: 10 sorts before 9.
            char bx[16], by[16];
            std::snprintf(bx, sizeof bx, "%d", x.asInt32());
            std::snprintf(by, sizeof by, "%d", y.asInt32());
            return std::strcmp(bx, by) < 0;
        }
        JSValue sx = toStringValue(vm, x);
        if (sx.isException()) {
            failed = true;
            return false;
        }
        JSValue sy = toStringValue(vm, y);
        if (sy.isException()) {
            release(sx);
            failed = true;
            return false;
        }
        const bool r = compareStrings(sx, sy) < 0;
        release(sx);
        release(sy);
        return r;
    };
    std::stable_sort(items.begin(), items.end(), less);
    if (failed) {
        for (JSValue v : items)
            release(v);
        return JSValue::exception();
    }
    // Write back: sorted values, then undefined, then holes. Each slot gets the count the item carries.
    const std::uint32_t n = a->length;
    for (std::uint32_t i = 0; i < n; ++i) {
        const JSValue old = a->elems[i];
        a->elems[i] = JSValue::undefined();
        release(old);
    }
    std::uint32_t at = 0;
    for (JSValue v : items)
        a->elems[at++] = v;
    for (std::uint32_t i = 0; i < undefinedCount; ++i)
        a->elems[at++] = JSValue::undefined();
    for (std::uint32_t i = 0; i < holes; ++i)
        a->elems[at++] = JSValue::hole();
    a->h.flags &= std::uint8_t(~cellflags::kAllNumbers);
    retain(self);
    return self;
}

/// A mutating method of Array.prototype on an array-like that is not an array: it works on a copy of the elements, which
/// are then written back (and the length) to the object.
JSValue mutateGeneric(VM& vm, JSValue self, const JSValue* args, std::uint32_t argc, void* data, FastNative impl)
{
    if (hasElems(self))
        return impl(vm, self, args, argc, data);
    if (self.isNullish())
        return typeError(vm, "Array.prototype method called on incompatible receiver " + describe(vm, self));
    ArrayView view(vm, self);
    JSArray* copy = view.a;
    if (!copy)
        return JSValue::exception();
    const std::uint32_t before = copy->length;
    const JSValue copyValue = toJSValue(copy);
    JSValue result = impl(vm, copyValue, args, argc, data);
    if (result.isException())
        return result;
    Handle resultKeep = Handle::adopt(result);
    if (isObject(self)) {
        const std::uint32_t after = copy->length;
        for (std::uint32_t i = 0; i < after; ++i) {
            Key k = toPropertyKey(vm, JSValue::boxNumber(double(i)));
            if (!k)
                return JSValue::exception();
            const JSValue v = copy->at(i);
            const bool ok = v.isHole() ? deleteProperty(vm, self, k, true) : setProperty(vm, self, k, v, true);
            detail::decRef(k);
            if (!ok)
                return JSValue::exception();
        }
        for (std::uint32_t i = after; i < before; ++i) {
            Key k = toPropertyKey(vm, JSValue::boxNumber(double(i)));
            if (!k)
                return JSValue::exception();
            const bool ok = deleteProperty(vm, self, k, true);
            detail::decRef(k);
            if (!ok)
                return JSValue::exception();
        }
        if (!setProperty(vm, self, vm.atoms.length, JSValue::boxNumber(double(after)), true))
            return JSValue::exception();
    }
    if (resultKeep.get() == copyValue) {
        retain(self);
        return self;
    }
    return resultKeep.take();
}

NATIVE(arrayPush)
{
    return mutateGeneric(vm, self, args, argc, data, arrayPushImpl);
}

NATIVE(arrayPop)
{
    return mutateGeneric(vm, self, args, argc, data, arrayPopImpl);
}

NATIVE(arrayShift)
{
    return mutateGeneric(vm, self, args, argc, data, arrayShiftImpl);
}

NATIVE(arrayUnshift)
{
    return mutateGeneric(vm, self, args, argc, data, arrayUnshiftImpl);
}

NATIVE(arraySplice)
{
    return mutateGeneric(vm, self, args, argc, data, arraySpliceImpl);
}

NATIVE(arrayReverse)
{
    return mutateGeneric(vm, self, args, argc, data, arrayReverseImpl);
}

NATIVE(arrayFill)
{
    return mutateGeneric(vm, self, args, argc, data, arrayFillImpl);
}


NATIVE(arrayCopyWithinImpl)
{
    UNUSED();
    JSArray* a = thisMutableArray(vm, self);
    if (!a)
        return JSValue::exception();
    const std::int64_t len = a->length;
    std::int64_t to, from, end;
    if (!relativeIndex(vm, arg(args, argc, 0), len, 0, &to) || !relativeIndex(vm, arg(args, argc, 1), len, 0, &from) ||
        !relativeIndex(vm, arg(args, argc, 2), len, len, &end))
        return JSValue::exception();
    const std::int64_t count = std::min(end - from, len - to);
    if (count > 0) {
        std::vector<JSValue> slice;
        for (std::int64_t i = 0; i < count; ++i) {
            const JSValue v = a->at(std::uint32_t(from + i));
            retain(v);
            slice.push_back(v);
        }
        for (std::int64_t i = 0; i < count && to + i < std::int64_t(a->length); ++i) {
            const JSValue old = a->elems[to + i];
            a->elems[to + i] = slice[std::size_t(i)];
            release(old);
        }
        a->h.flags &= std::uint8_t(~cellflags::kAllNumbers);
    }
    retain(self);
    return self;
}

/// A fresh array of the elements of `self`, a hole read as undefined: what toReversed, toSorted, toSpliced and with start from.
JSArray* denseCopyOf(VM& vm, JSValue self, Handle& keep)
{
    ArrayView view(vm, self);
    JSArray* a = view.a;
    if (!a)
        return nullptr;
    JSArray* out = vm.newArray(vm.protos.array, 0, a->length);
    keep = Handle::adopt(toJSValue(out));
    for (std::uint32_t i = 0; i < a->length; ++i) {
        const JSValue v = a->elems[i].isHole() ? JSValue::undefined() : a->elems[i];
        retain(v);
        out->append(vm, v);
    }
    return out;
}

NATIVE(arrayToReversed)
{
    UNUSED();
    Handle keep;
    JSArray* out = denseCopyOf(vm, self, keep);
    if (!out)
        return JSValue::exception();
    std::reverse(out->elems, out->elems + out->length);
    return keep.take();
}

NATIVE(arrayToSorted)
{
    UNUSED();
    const JSValue compare = arg(args, argc, 0);
    if (!compare.isUndefined() && !isFunction(compare))
        return typeError(vm, "The comparison function must be either a function or undefined");
    Handle keep;
    JSArray* out = denseCopyOf(vm, self, keep);
    if (!out)
        return JSValue::exception();
    JSValue r = arraySortImpl(vm, toJSValue(out), args, argc, nullptr);
    if (r.isException())
        return r;
    release(r);
    return keep.take();
}

NATIVE(arrayToSpliced)
{
    UNUSED();
    Handle keep;
    JSArray* out = denseCopyOf(vm, self, keep);
    if (!out)
        return JSValue::exception();
    JSValue r = arraySpliceImpl(vm, toJSValue(out), args, argc, nullptr);
    if (r.isException())
        return r;
    release(r);
    return keep.take();
}

NATIVE(arrayWith)
{
    UNUSED();
    Handle keep;
    JSArray* out = denseCopyOf(vm, self, keep);
    if (!out)
        return JSValue::exception();
    double d;
    if (!toNumber(vm, arg(args, argc, 0), &d))
        return JSValue::exception();
    d = d != d ? 0 : std::trunc(d);
    const double index = d < 0 ? double(out->length) + d : d;
    if (index < 0 || index >= double(out->length))
        return vm.throwError(ErrorCode::RangeError, "Invalid index : " + describe(vm, arg(args, argc, 0)));
    const JSValue v = arg(args, argc, 1);
    retain(v);
    const JSValue old = out->elems[std::size_t(index)];
    out->elems[std::size_t(index)] = v;
    release(old);
    out->h.flags &= std::uint8_t(~cellflags::kAllNumbers);
    return keep.take();
}

NATIVE(arrayCopyWithin)
{
    return mutateGeneric(vm, self, args, argc, data, arrayCopyWithinImpl);
}

NATIVE(arraySort)
{
    return mutateGeneric(vm, self, args, argc, data, arraySortImpl);
}


// ------------------------------------------------------------------- String ---

/// `this` as a string cell, owned in `*out`.
bool thisString(VM& vm, JSValue self, Handle* out)
{
    if (self.isNullish()) {
        typeError(vm, "String.prototype method called on null or undefined");
        return false;
    }
    JSValue s = toStringValue(vm, self);
    if (s.isException())
        return false;
    *out = Handle::adopt(s);
    return true;
}

NATIVE(stringCtor)
{
    UNUSED();
    JSValue v = argc ? args[0] : JSValue::cell(vm.ctx.intern(u""));
    if (argc && isSymbol(v)) {
        if (!vm.newTarget.isUndefined())
            return typeError(vm, "Cannot convert a Symbol value to a string");
        const auto* sym = static_cast<const dyn::SymbolCell*>(v.asCell());
        return makeString(vm, StringView(u"Symbol(" + sym->description + u")"));
    }
    JSValue text = toStringValue(vm, v);
    if (text.isException() || vm.newTarget.isUndefined() || !isObject(self))
        return text;
    // `new String(v)`: the object `new` made becomes the wrapper.
    wrapPrimitive(vm, asObject(self), ObjClass::String, text);
    release(text);
    retain(self);
    return self;
}

NATIVE(stringFromCharCode)
{
    UNUSED();
    String out;
    for (std::uint32_t i = 0; i < argc; ++i) {
        double d;
        if (!toNumber(vm, args[i], &d))
            return JSValue::exception();
        out += char16_t(toUint32(d) & 0xFFFF);
    }
    return makeString(vm, StringView(out));
}

NATIVE(stringCharAt)
{
    UNUSED();
    Handle s;
    if (!thisString(vm, self, &s))
        return JSValue::exception();
    double d = 0;
    if (!toNumber(vm, arg(args, argc, 0), &d))
        return JSValue::exception();
    d = d != d ? 0 : std::trunc(d);
    StringView text = asString(s.get())->text();
    const bool code = data != nullptr;
    if (d < 0 || d >= double(text.size()))
        return code ? JSValue::boxDouble(std::nan("")) : makeString(vm, StringView());
    return code ? JSValue::int32(text[std::size_t(d)]) : makeString(vm, text.substr(std::size_t(d), 1));
}

NATIVE(stringIndexOf)
{
    UNUSED();
    Handle s;
    if (!thisString(vm, self, &s))
        return JSValue::exception();
    JSValue needle = toStringValue(vm, arg(args, argc, 0));
    if (needle.isException())
        return needle;
    Handle n = Handle::adopt(needle);
    StringView text = asString(s.get())->text(), sub = asString(needle)->text();
    const int mode = int(reinterpret_cast<std::intptr_t>(data));  // 0 indexOf, 1 lastIndexOf, 2 includes, 3 startsWith, 4 endsWith
    double from = 0;
    if (argc > 1 && !args[1].isUndefined() && !toNumber(vm, args[1], &from))
        return JSValue::exception();
    from = from != from ? 0 : std::trunc(from);
    const std::size_t start = std::size_t(std::min(std::max(from, 0.0), double(text.size())));
    switch (mode) {
        case 0: {
            const std::size_t at = text.find(sub, start);
            return JSValue::int32(at == StringView::npos ? -1 : std::int32_t(at));
        }
        case 1: {
            const std::size_t at = text.rfind(sub);
            return JSValue::int32(at == StringView::npos ? -1 : std::int32_t(at));
        }
        case 2: return JSValue::boolean(text.find(sub, start) != StringView::npos);
        case 3: return JSValue::boolean(text.substr(start, sub.size()) == sub);
        default: return JSValue::boolean(text.size() >= sub.size() && text.substr(text.size() - sub.size()) == sub);
    }
}

NATIVE(stringSlice)
{
    UNUSED();
    Handle s;
    if (!thisString(vm, self, &s))
        return JSValue::exception();
    StringView text = asString(s.get())->text();
    const int mode = int(reinterpret_cast<std::intptr_t>(data));  // 0 slice, 1 substring, 2 substr
    std::int64_t from, to;
    const std::int64_t n = std::int64_t(text.size());
    if (mode == 1) {
        double a = 0, b = double(n);
        if (!toNumber(vm, arg(args, argc, 0), &a))
            return JSValue::exception();
        if (argc > 1 && !args[1].isUndefined() && !toNumber(vm, args[1], &b))
            return JSValue::exception();
        a = std::min(std::max(a != a ? 0 : std::trunc(a), 0.0), double(n));
        b = std::min(std::max(b != b ? 0 : std::trunc(b), 0.0), double(n));
        from = std::int64_t(std::min(a, b));
        to = std::int64_t(std::max(a, b));
    } else if (mode == 2) {
        double len = double(n);
        if (!relativeIndex(vm, arg(args, argc, 0), n, 0, &from))
            return JSValue::exception();
        if (argc > 1 && !args[1].isUndefined() && !toNumber(vm, args[1], &len))
            return JSValue::exception();
        to = from + std::int64_t(std::min(std::max(len != len ? 0 : std::trunc(len), 0.0), double(n - from)));
    } else {
        if (!relativeIndex(vm, arg(args, argc, 0), n, 0, &from) || !relativeIndex(vm, arg(args, argc, 1), n, n, &to))
            return JSValue::exception();
    }
    if (to < from)
        to = from;
    return makeString(vm, text.substr(std::size_t(from), std::size_t(to - from)));
}

NATIVE(stringCase)
{
    UNUSED();
    Handle s;
    if (!thisString(vm, self, &s))
        return JSValue::exception();
    const StringView text = asString(s.get())->text();
    const bool upper = data != nullptr;
    String out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        char32_t c = text[i];
        if (c < 0x80) {
            // ASCII is the common case.
            if (upper && c >= u'a' && c <= u'z')
                c -= 32;
            else if (!upper && c >= u'A' && c <= u'Z')
                c += 32;
            out.push_back(char16_t(c));
            continue;
        }
        if (c >= 0xd800 && c <= 0xdbff && i + 1 < text.size() && text[i + 1] >= 0xdc00 && text[i + 1] <= 0xdfff)
            c = 0x10000 + ((c - 0xd800) << 10) + (text[++i] - 0xdc00);
        char32_t mapped[3];
        int n = 1;
        if (upper) {
            n = dyn::unicode::fullUpper(c, mapped);
        } else if (c == 0x130) {
            mapped[0] = u'i';
            mapped[1] = 0x307;
            n = 2;
        } else if (c == 0x3a3) {
            // Final sigma: after a letter and not before one.
            const bool afterLetter = i > 0 && dyn::unicode::isIdContinue(text[i - 1]);
            const bool beforeLetter = i + 1 < text.size() && dyn::unicode::isIdContinue(text[i + 1]);
            mapped[0] = afterLetter && !beforeLetter ? 0x3c2 : 0x3c3;
        } else {
            mapped[0] = dyn::unicode::simpleLower(c);
        }
        for (int k = 0; k < n; ++k) {
            const char32_t m = mapped[k];
            if (m >= 0x10000) {
                out.push_back(char16_t(0xd800 + ((m - 0x10000) >> 10)));
                out.push_back(char16_t(0xdc00 + ((m - 0x10000) & 0x3ff)));
            } else {
                out.push_back(char16_t(m));
            }
        }
    }
    return makeString(vm, StringView(out));
}

NATIVE(stringTrim)
{
    UNUSED();
    Handle s;
    if (!thisString(vm, self, &s))
        return JSValue::exception();
    StringView text = asString(s.get())->text();
    auto space = [](char16_t c) {
        return c == u' ' || (c >= 9 && c <= 13) || c == 0xA0 || c == 0xFEFF || c == 0x1680 || (c >= 0x2000 && c <= 0x200A) ||
               c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000;
    };
    const int mode = int(reinterpret_cast<std::intptr_t>(data));  // 0 both, 1 start, 2 end
    std::size_t a = 0, b = text.size();
    if (mode != 2)
        while (a < b && space(text[a]))
            ++a;
    if (mode != 1)
        while (b > a && space(text[b - 1]))
            --b;
    return makeString(vm, text.substr(a, b - a));
}

NATIVE(stringSplit)
{
    UNUSED();
    Handle s;
    if (!thisString(vm, self, &s))
        return JSValue::exception();
    StringView text = asString(s.get())->text();
    {
        const JSValue extra[1] = {arg(args, argc, 1)};
        bool handled;
        JSValue r = callSymbolMethod(vm, arg(args, argc, 0), vm.atoms.symSplit, s.get(), extra, 1, &handled);
        if (handled)
            return r;
    }
    JSArray* out = vm.newArray(vm.protos.array);
    Handle keep = Handle::adopt(toJSValue(out));
    JSValue sepArg = arg(args, argc, 0);
    if (sepArg.isUndefined()) {
        out->append(vm, makeString(vm, text));
        return keep.take();
    }
    JSValue sepValue = toStringValue(vm, sepArg);
    if (sepValue.isException())
        return sepValue;
    Handle sepKeep = Handle::adopt(sepValue);
    StringView sep = asString(sepValue)->text();
    double limit = 4294967295.0;
    if (argc > 1 && !args[1].isUndefined()) {
        if (!toNumber(vm, args[1], &limit))
            return JSValue::exception();
        limit = double(toUint32(limit));
    }
    auto push = [&](StringView piece) {
        if (double(out->length) < limit)
            out->append(vm, makeString(vm, piece));
    };
    if (sep.empty()) {
        for (std::size_t i = 0; i < text.size(); ++i)
            push(text.substr(i, 1));
        return keep.take();
    }
    std::size_t from = 0;
    for (;;) {
        const std::size_t at = text.find(sep, from);
        if (at == StringView::npos) {
            push(text.substr(from));
            break;
        }
        push(text.substr(from, at - from));
        from = at + sep.size();
    }
    return keep.take();
}

NATIVE(stringRepeat)
{
    UNUSED();
    Handle s;
    if (!thisString(vm, self, &s))
        return JSValue::exception();
    double n;
    if (!toNumber(vm, arg(args, argc, 0), &n))
        return JSValue::exception();
    if (n < 0 || std::isinf(n))
        return vm.throwError(ErrorCode::RangeError, "Invalid count value");
    StringView text = asString(s.get())->text();
    if (n * double(text.size()) > 1 << 28)
        return vm.throwError(ErrorCode::RangeError, "Invalid string length");
    String out;
    for (std::int64_t i = 0; i < std::int64_t(n); ++i)
        out += text;
    return makeString(vm, StringView(out));
}

NATIVE(stringPad)
{
    UNUSED();
    Handle s;
    if (!thisString(vm, self, &s))
        return JSValue::exception();
    double len;
    if (!toNumber(vm, arg(args, argc, 0), &len))
        return JSValue::exception();
    String fill = u" ";
    if (argc > 1 && !args[1].isUndefined()) {
        JSValue f = toStringValue(vm, args[1]);
        if (f.isException())
            return f;
        fill = String(asString(f)->text());
        release(f);
    }
    StringView text = asString(s.get())->text();
    const std::size_t target = len != len || len < 0 ? 0 : std::size_t(std::min(len, double(1 << 28)));
    if (target <= text.size() || fill.empty()) {
        retain(s.get());
        return s.get();
    }
    String pad;
    while (pad.size() < target - text.size())
        pad += fill;
    pad.resize(target - text.size());
    const bool atStart = data != nullptr;
    String out = atStart ? pad + String(text) : String(text) + pad;
    return makeString(vm, StringView(out));
}

JSValue stringReplaceText(VM& vm, const Handle& s, JSValue pat, JSValue replacement, bool all);

NATIVE(stringReplace)
{
    UNUSED();
    Handle s;
    if (!thisString(vm, self, &s))
        return JSValue::exception();
    const JSValue pat = arg(args, argc, 0);
    const bool all = data != nullptr;
    if (all && isObject(pat) && asObject(pat)->cls() == ObjClass::RegExp) {
        // `replaceAll` takes only a global RegExp.
        Key flagsKey = vm.internKey(std::string_view("flags"));
        JSValue flags = getProperty(vm, pat, flagsKey);
        detail::decRef(flagsKey);
        if (flags.isException())
            return flags;
        const bool global = isString(flags) && asString(flags)->text().find(u'g') != StringView::npos;
        release(flags);
        if (!global)
            return typeError(vm, "replaceAll must be called with a global RegExp");
    }
    const JSValue extra[1] = {arg(args, argc, 1)};
    bool handled;
    JSValue r = callSymbolMethod(vm, pat, vm.atoms.symReplace, s.get(), extra, 1, &handled);
    if (handled)
        return r;
    Handle text = Handle::adopt(toStringValue(vm, pat));
    if (text.get().isException())
        return JSValue::exception();
    return stringReplaceText(vm, s, text.get(), arg(args, argc, 1), all);
}

JSValue stringReplaceText(VM& vm, const Handle& s, JSValue pat, JSValue replacement, bool all)
{
    StringView text = asString(s.get())->text(), sub = asString(pat)->text();
    String out;
    std::size_t from = 0;
    for (;;) {
        const std::size_t at = text.find(sub, from);
        if (at == StringView::npos)
            break;
        out += text.substr(from, at - from);
        if (isFunction(replacement)) {
            JSValue callArgs[3] = {pat, JSValue::int32(std::int32_t(at)), s.get()};
            JSValue r = vm.call(replacement, JSValue::undefined(), callArgs, 3);
            if (r.isException())
                return r;
            JSValue rs = toStringValue(vm, r);
            release(r);
            if (rs.isException())
                return rs;
            out += asString(rs)->text();
            release(rs);
        } else {
            JSValue rs = toStringValue(vm, replacement);
            if (rs.isException())
                return rs;
            const StringView templ = asString(rs)->text();
            for (std::size_t i = 0; i < templ.size(); ++i) {
                if (templ[i] != u'$' || i + 1 >= templ.size()) {
                    out.push_back(templ[i]);
                    continue;
                }
                const char16_t n = templ[i + 1];
                if (n == u'$') {
                    out.push_back(u'$');
                } else if (n == u'&') {
                    out += sub;
                } else if (n == u'`') {
                    out += text.substr(0, at);
                } else if (n == u'\'') {
                    out += text.substr(at + sub.size());
                } else {
                    out.push_back(u'$');
                    continue;
                }
                ++i;
            }
            release(rs);
        }
        from = at + sub.size();
        if (sub.empty()) {
            if (from < text.size())
                out += text[from];
            ++from;
        }
        if (!all || from > text.size())
            break;
    }
    if (from <= text.size())
        out += text.substr(from);
    return makeString(vm, StringView(out));
}

NATIVE(stringConcat)
{
    UNUSED();
    Handle s;
    if (!thisString(vm, self, &s))
        return JSValue::exception();
    String out(asString(s.get())->text());
    for (std::uint32_t i = 0; i < argc; ++i) {
        JSValue t = toStringValue(vm, args[i]);
        if (t.isException())
            return t;
        out += asString(t)->text();
        release(t);
    }
    return makeString(vm, StringView(out));
}

NATIVE(stringLocaleCompare)
{
    UNUSED();
    Handle s;
    if (!thisString(vm, self, &s))
        return JSValue::exception();
    JSValue t = toStringValue(vm, arg(args, argc, 0));
    if (t.isException())
        return t;
    const int c = compareStrings(s.get(), t);
    release(t);
    return JSValue::int32(c);
}

NATIVE(stringToString)
{
    UNUSED();
    self = unwrapped(self, ObjClass::String);
    if (!isString(self))
        return typeError(vm, "String.prototype.toString requires that 'this' be a String");
    retain(self);
    return self;
}

// ------------------------------------------------------------ Number, Boolean ---

/// A primitive made by a constructor called with `new` becomes the wrapper object `new` made.
JSValue wrapped(VM& vm, JSValue self, ObjClass cls, JSValue primitive)
{
    if (vm.newTarget.isUndefined() || !isObject(self))
        return primitive;
    wrapPrimitive(vm, asObject(self), cls, primitive);
    release(primitive);
    retain(self);
    return self;
}

NATIVE(numberCtor)
{
    UNUSED();
    if (!argc)
        return wrapped(vm, self, ObjClass::Number, JSValue::int32(0));
    JSValue prim = toPrimitive(vm, args[0], false);
    if (prim.isException())
        return prim;
    if (isBigInt(prim)) {
        const double big = bigintToDouble(prim);
        release(prim);
        return wrapped(vm, self, ObjClass::Number, JSValue::boxNumber(big));
    }
    double d;
    const bool ok = toNumber(vm, prim, &d);
    release(prim);
    if (!ok)
        return JSValue::exception();
    return wrapped(vm, self, ObjClass::Number, JSValue::boxNumber(d));
}

NATIVE(numberIs)
{
    UNUSED();
    const JSValue v = arg(args, argc, 0);
    const int mode = int(reinterpret_cast<std::intptr_t>(data));  // 0 isInteger, 1 isNaN, 2 isFinite, 3 isSafeInteger
    if (!v.isNumber())
        return JSValue::boolean(false);
    const double d = v.asNumber();
    switch (mode) {
        case 0: return JSValue::boolean(std::isfinite(d) && d == std::trunc(d));
        case 1: return JSValue::boolean(d != d);
        case 2: return JSValue::boolean(std::isfinite(d));
        default: return JSValue::boolean(std::isfinite(d) && d == std::trunc(d) && std::fabs(d) <= 9007199254740991.0);
    }
}

NATIVE(numberToString)
{
    UNUSED();
    self = unwrapped(self, ObjClass::Number);
    if (!self.isNumber())
        return typeError(vm, "Number.prototype.toString requires that 'this' be a Number");
    double radix = 10;
    if (argc && !args[0].isUndefined() && !toNumber(vm, args[0], &radix))
        return JSValue::exception();
    if (radix < 2 || radix > 36)
        return vm.throwError(ErrorCode::RangeError, "toString() radix must be between 2 and 36");
    if (radix == 10)
        return toStringValue(vm, self);
    return makeString(vm, StringView(dyn::numberToStringRadix(self.asNumber(), int(radix))));
}

NATIVE(numberToFixed)
{
    UNUSED();
    self = unwrapped(self, ObjClass::Number);
    if (!self.isNumber())
        return typeError(vm, "Number.prototype.toFixed requires that 'this' be a Number");
    double digits = 0;
    if (argc && !toNumber(vm, args[0], &digits))
        return JSValue::exception();
    if (digits < 0 || digits > 100)
        return vm.throwError(ErrorCode::RangeError, "toFixed() digits argument must be between 0 and 100");
    return makeString(vm, StringView(dyn::numberToFixed(self.asNumber(), int(digits))));
}

NATIVE(numberValueOf)
{
    UNUSED();
    self = unwrapped(self, ObjClass::Number);
    if (!self.isNumber())
        return typeError(vm, "Number.prototype.valueOf requires that 'this' be a Number");
    return self;
}

NATIVE(booleanCtor)
{
    UNUSED();
    return wrapped(vm, self, ObjClass::Boolean, JSValue::boolean(toBoolean(arg(args, argc, 0))));
}

NATIVE(booleanToString)
{
    UNUSED();
    self = unwrapped(self, ObjClass::Boolean);
    if (!self.isBool())
        return typeError(vm, "Boolean.prototype.toString requires that 'this' be a Boolean");
    return str(vm, self.asBool() ? "true" : "false");
}

// --------------------------------------------------------------------- JSON ---

struct JsonWriter
{
    VM& vm;
    Handle replacerFn;
    std::vector<Handle> allow;
    bool hasAllow = false;
    String gap;
    String indent;
    String out;
    std::vector<JSObject*> stack;
    Key toJson = nullptr;  ///< held for the length of the call
};

void jsonQuote(String& out, StringView s)
{
    static const char16_t hex[] = u"0123456789abcdef";
    out.push_back(u'"');
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char16_t c = s[i];
        switch (c) {
            case u'"': out += u"\\\""; break;
            case u'\\': out += u"\\\\"; break;
            case u'\b': out += u"\\b"; break;
            case u'\f': out += u"\\f"; break;
            case u'\n': out += u"\\n"; break;
            case u'\r': out += u"\\r"; break;
            case u'\t': out += u"\\t"; break;
            default:
                if (c < 0x20) {
                    out += u"\\u00";
                    out.push_back(hex[c >> 4]);
                    out.push_back(hex[c & 15]);
                } else if (c >= 0xd800 && c <= 0xdfff) {
                    // A lone surrogate is written as an escape; a pair stands as it is.
                    const bool high = c < 0xdc00;
                    const bool paired = high ? (i + 1 < s.size() && s[i + 1] >= 0xdc00 && s[i + 1] <= 0xdfff)
                                             : (i > 0 && s[i - 1] >= 0xd800 && s[i - 1] < 0xdc00);
                    if (paired) {
                        out.push_back(c);
                    } else {
                        out += u"\\u";
                        out.push_back(hex[c >> 12]);
                        out.push_back(hex[(c >> 8) & 15]);
                        out.push_back(hex[(c >> 4) & 15]);
                        out.push_back(hex[c & 15]);
                    }
                } else {
                    out.push_back(c);
                }
        }
    }
    out.push_back(u'"');
}

// 0 for an exception, 1 for text written, 2 for nothing to write (undefined, a function, a symbol).
int jsonSerialize(JsonWriter& j, JSValue holder, JSValue key, JSValue input)
{
    VM& vm = j.vm;
    Handle value = Handle::retained(input);
    if (isObject(value.get()) || isBigInt(value.get())) {
        JSValue fn = getProperty(vm, value.get(), j.toJson);
        if (fn.isException())
            return 0;
        Handle fnKeep = Handle::adopt(fn);
        if (isFunction(fn)) {
            JSValue r = vm.call(fn, value.get(), &key, 1);
            if (r.isException())
                return 0;
            value = Handle::adopt(r);
        }
    }
    if (j.replacerFn.get() != JSValue::undefined()) {
        JSValue callArgs[2] = {key, value.get()};
        JSValue r = vm.call(j.replacerFn.get(), holder, callArgs, 2);
        if (r.isException())
            return 0;
        value = Handle::adopt(r);
    }
    if (isObject(value.get())) {
        // The wrapper objects of primitives serialize as what they wrap.
        switch (asObject(value.get())->cls()) {
            case ObjClass::Number: {
                double d;
                if (!toNumber(vm, value.get(), &d))
                    return 0;
                value = Handle::adopt(JSValue::boxNumber(d));
                break;
            }
            case ObjClass::String: {
                JSValue t = toStringValue(vm, value.get());
                if (t.isException())
                    return 0;
                value = Handle::adopt(t);
                break;
            }
            case ObjClass::Boolean:
            case ObjClass::BigInt: value = Handle::retained(unwrapped(value.get(), asObject(value.get())->cls())); break;
            default: break;
        }
    }
    const JSValue v = value.get();
    if (v.isNull()) {
        j.out += u"null";
        return 1;
    }
    if (v.isBool()) {
        j.out += v == JSValue::boolean(true) ? u"true" : u"false";
        return 1;
    }
    if (isString(v)) {
        jsonQuote(j.out, asString(v)->text());
        return 1;
    }
    if (v.isNumber()) {
        const double d = v.asNumber();
        j.out += std::isfinite(d) ? script::internal::numberToString(d) : String(u"null");
        return 1;
    }
    if (isBigInt(v)) {
        typeError(vm, "Do not know how to serialize a BigInt");
        return 0;
    }
    if (!isObject(v) || isFunction(v))
        return 2;

    JSObject* o = asObject(v);
    for (JSObject* seen : j.stack)
        if (seen == o) {
            typeError(vm, "Converting circular structure to JSON");
            return 0;
        }
    j.stack.push_back(o);
    const String stepback = j.indent;
    j.indent += j.gap;
    const bool array = isArray(v);
    std::vector<Handle> keys;
    if (array) {
        // Elements are read through `length`, as the spec says.
    } else if (j.hasAllow) {
        keys = j.allow;
    } else {
        ownKeys(vm, o, true, false, true, keys);
    }
    bool wroteAny = false;
    j.out.push_back(array ? u'[' : u'{');
    auto separator = [&]() {
        if (wroteAny)
            j.out.push_back(u',');
        wroteAny = true;
        if (!j.gap.empty()) {
            j.out.push_back(u'\n');
            j.out += j.indent;
        }
    };
    if (array) {
        JSValue lenValue = getProperty(vm, v, vm.atoms.length);
        if (lenValue.isException())
            return 0;
        const std::uint32_t length = std::uint32_t(lenValue.asNumber());
        for (std::uint32_t i = 0; i < length; ++i) {
            Handle idx = Handle::adopt(toStringValue(vm, JSValue::boxNumber(double(i))));
            Key k = toPropertyKey(vm, idx.get());
            if (!k)
                return 0;
            JSValue element = getProperty(vm, v, k);
            detail::decRef(k);
            if (element.isException())
                return 0;
            Handle elementKeep = Handle::adopt(element);
            separator();
            const std::size_t mark = j.out.size();
            const int r = jsonSerialize(j, v, idx.get(), element);
            if (r == 0)
                return 0;
            if (r == 2)
                j.out += u"null";
            (void)mark;
        }
    } else {
        for (const Handle& name : keys) {
            Key k = toPropertyKey(vm, name.get());
            if (!k)
                return 0;
            JSValue prop = getProperty(vm, v, k);
            detail::decRef(k);
            if (prop.isException())
                return 0;
            Handle propKeep = Handle::adopt(prop);
            const std::size_t before = j.out.size();
            const bool hadAny = wroteAny;
            separator();
            jsonQuote(j.out, asString(name.get())->text());
            j.out.push_back(u':');
            if (!j.gap.empty())
                j.out.push_back(u' ');
            const int r = jsonSerialize(j, v, name.get(), prop);
            if (r == 0)
                return 0;
            if (r == 2) {
                j.out.resize(before);
                wroteAny = hadAny;
            }
        }
    }
    j.indent = stepback;
    j.stack.pop_back();
    if (wroteAny && !j.gap.empty()) {
        j.out.push_back(u'\n');
        j.out += j.indent;
    }
    j.out.push_back(array ? u']' : u'}');
    return 1;
}

NATIVE(jsonStringify)
{
    UNUSED();
    JsonWriter j{vm, {}, {}, false, {}, {}, {}, {}, nullptr};
    j.toJson = vm.internKey(std::string_view("toJSON"));
    struct Release
    {
        Key k;
        ~Release() { detail::decRef(k); }
    } releaseKey{j.toJson};
    const JSValue replacer = arg(args, argc, 1);
    if (isFunction(replacer)) {
        j.replacerFn = Handle::retained(replacer);
    } else if (isArray(replacer)) {
        j.hasAllow = true;
        auto* a = static_cast<JSArray*>(asObject(replacer));
        for (std::uint32_t i = 0; i < a->length; ++i) {
            JSValue item = a->at(i);
            if (isString(item) || item.isNumber()) {
                Handle name = Handle::adopt(toStringValue(vm, item));
                bool dup = false;
                for (const Handle& e : j.allow)
                    dup = dup || stringsEqual(e.get(), name.get());
                if (!dup)
                    j.allow.push_back(std::move(name));
            }
        }
    }
    const JSValue space = arg(args, argc, 2);
    if (space.isNumber()) {
        const int n = int(std::max(0.0, std::min(10.0, std::trunc(space.asNumber()))));
        j.gap.assign(std::size_t(n), u' ');
    } else if (isString(space)) {
        j.gap = String(asString(space)->text().substr(0, 10));
    }
    JSObject* wrapper = vm.newObject(vm.protos.object);
    Handle wrapperKeep = Handle::adopt(toJSValue(wrapper));
    JSValue input = arg(args, argc, 0);
    retain(input);
    {
        Key emptyKey = vm.internKey(std::string_view());
        wrapper->putOwn(vm, emptyKey, input, attr::kDefault);
        detail::decRef(emptyKey);
    }
    Handle empty = Handle::adopt(makeString(vm, std::string_view()));
    const int r = jsonSerialize(j, toJSValue(wrapper), empty.get(), input);
    if (r == 0)
        return JSValue::exception();
    if (r == 2)
        return JSValue::undefined();
    return makeString(vm, StringView(j.out));
}

struct JsonReader
{
    VM& vm;
    StringView s;
    std::size_t i = 0;
    int depth = 0;

    JSValue fail(const char* what)
    {
        std::string message = std::string("JSON.parse: ") + what + " at position " + std::to_string(i);
        return vm.throwError(ErrorCode::SyntaxError, message);
    }
    void skip()
    {
        while (i < s.size() && (s[i] == u' ' || s[i] == u'\t' || s[i] == u'\n' || s[i] == u'\r'))
            ++i;
    }
    bool literal(const char* word)
    {
        std::size_t n = 0;
        while (word[n])
            ++n;
        if (s.size() - i < n)
            return false;
        for (std::size_t k = 0; k < n; ++k)
            if (s[i + k] != char16_t(word[k]))
                return false;
        i += n;
        return true;
    }
    // Owned; the exception sentinel on a syntax error.
    JSValue string(String* text)
    {
        ++i;  // the quote
        String out;
        for (;;) {
            if (i >= s.size())
                return fail("unterminated string");
            const char16_t c = s[i++];
            if (c == u'"')
                break;
            if (c < 0x20)
                return fail("bad control character in string");
            if (c != u'\\') {
                out.push_back(c);
                continue;
            }
            if (i >= s.size())
                return fail("unterminated string");
            const char16_t e = s[i++];
            switch (e) {
                case u'"': out.push_back(u'"'); break;
                case u'\\': out.push_back(u'\\'); break;
                case u'/': out.push_back(u'/'); break;
                case u'b': out.push_back(u'\b'); break;
                case u'f': out.push_back(u'\f'); break;
                case u'n': out.push_back(u'\n'); break;
                case u'r': out.push_back(u'\r'); break;
                case u't': out.push_back(u'\t'); break;
                case u'u': {
                    unsigned code = 0;
                    for (int k = 0; k < 4; ++k) {
                        if (i >= s.size())
                            return fail("bad escape");
                        const char16_t h = s[i++];
                        unsigned digit;
                        if (h >= u'0' && h <= u'9')
                            digit = h - u'0';
                        else if (h >= u'a' && h <= u'f')
                            digit = 10 + h - u'a';
                        else if (h >= u'A' && h <= u'F')
                            digit = 10 + h - u'A';
                        else
                            return fail("bad escape");
                        code = code * 16 + digit;
                    }
                    out.push_back(char16_t(code));
                    break;
                }
                default: return fail("bad escape");
            }
        }
        if (text)
            *text = out;
        return JSValue::undefined();
    }
    JSValue number()
    {
        const std::size_t start = i;
        if (s[i] == u'-')
            ++i;
        if (i >= s.size())
            return fail("bad number");
        if (s[i] == u'0') {
            ++i;
        } else if (s[i] >= u'1' && s[i] <= u'9') {
            while (i < s.size() && s[i] >= u'0' && s[i] <= u'9')
                ++i;
        } else {
            return fail("bad number");
        }
        if (i < s.size() && s[i] == u'.') {
            ++i;
            if (i >= s.size() || s[i] < u'0' || s[i] > u'9')
                return fail("bad number");
            while (i < s.size() && s[i] >= u'0' && s[i] <= u'9')
                ++i;
        }
        if (i < s.size() && (s[i] == u'e' || s[i] == u'E')) {
            ++i;
            if (i < s.size() && (s[i] == u'+' || s[i] == u'-'))
                ++i;
            if (i >= s.size() || s[i] < u'0' || s[i] > u'9')
                return fail("bad number");
            while (i < s.size() && s[i] >= u'0' && s[i] <= u'9')
                ++i;
        }
        std::string text;
        for (std::size_t k = start; k < i; ++k)
            text.push_back(char(s[k]));
        return JSValue::boxNumber(std::strtod(text.c_str(), nullptr));
    }
    JSValue value()
    {
        skip();
        if (i >= s.size())
            return fail("unexpected end of input");
        const char16_t c = s[i];
        if (c == u'"') {
            String text;
            JSValue r = string(&text);
            if (r.isException())
                return r;
            return makeString(vm, StringView(text));
        }
        if (c == u'-' || (c >= u'0' && c <= u'9'))
            return number();
        if (c == u't')
            return literal("true") ? JSValue::boolean(true) : fail("unexpected token");
        if (c == u'f')
            return literal("false") ? JSValue::boolean(false) : fail("unexpected token");
        if (c == u'n')
            return literal("null") ? JSValue::null() : fail("unexpected token");
        if (++depth > 2000)
            return fail("nesting too deep");
        struct Leave
        {
            int& d;
            ~Leave() { --d; }
        } leave{depth};
        if (c == u'[') {
            ++i;
            JSArray* a = vm.newArray(vm.protos.array);
            Handle keep = Handle::adopt(toJSValue(a));
            skip();
            if (i < s.size() && s[i] == u']') {
                ++i;
                return keep.take();
            }
            for (;;) {
                JSValue item = value();
                if (item.isException())
                    return item;
                a->append(vm, item);
                skip();
                if (i < s.size() && s[i] == u',') {
                    ++i;
                    continue;
                }
                if (i < s.size() && s[i] == u']') {
                    ++i;
                    return keep.take();
                }
                return fail("expected ',' or ']'");
            }
        }
        if (c == u'{') {
            ++i;
            JSObject* o = vm.newObject(vm.protos.object);
            Handle keep = Handle::adopt(toJSValue(o));
            skip();
            if (i < s.size() && s[i] == u'}') {
                ++i;
                return keep.take();
            }
            for (;;) {
                skip();
                if (i >= s.size() || s[i] != u'"')
                    return fail("expected a property name");
                String name;
                JSValue r = string(&name);
                if (r.isException())
                    return r;
                skip();
                if (i >= s.size() || s[i] != u':')
                    return fail("expected ':'");
                ++i;
                JSValue item = value();
                if (item.isException())
                    return item;
                Handle itemKeep = Handle::adopt(item);
                Key key = vm.internKey(StringView(name));
                retain(item);
                defineOwn(vm, o, key, item, attr::kDefault);
                detail::decRef(key);
                skip();
                if (i < s.size() && s[i] == u',') {
                    ++i;
                    continue;
                }
                if (i < s.size() && s[i] == u'}') {
                    ++i;
                    return keep.take();
                }
                return fail("expected ',' or '}'");
            }
        }
        return fail("unexpected token");
    }
};

// The reviver walks the parsed value bottom-up. Owned result, or the exception sentinel.
JSValue jsonRevive(VM& vm, JSValue holder, Key key, JSValue reviver)
{
    JSValue got = getProperty(vm, holder, key);
    if (got.isException())
        return got;
    Handle value = Handle::adopt(got);
    if (isObject(value.get())) {
        JSValue self = value.get();
        std::vector<Handle> names;
        if (isArray(self)) {
            JSValue len = getProperty(vm, self, vm.atoms.length);
            if (len.isException())
                return len;
            const std::uint32_t n = std::uint32_t(len.asNumber());
            for (std::uint32_t k = 0; k < n; ++k)
                names.push_back(Handle::adopt(toStringValue(vm, JSValue::boxNumber(double(k)))));
        } else {
            JSValue list = objectKeys(vm, JSValue::undefined(), &self, 1, nullptr);
            if (list.isException())
                return list;
            Handle keep = Handle::adopt(list);
            auto* a = static_cast<JSArray*>(asObject(list));
            for (std::uint32_t k = 0; k < a->length; ++k)
                names.push_back(Handle::retained(a->at(k)));
        }
        for (const Handle& name : names) {
            Key k = toPropertyKey(vm, name.get());
            if (!k)
                return JSValue::exception();
            JSValue r = jsonRevive(vm, self, k, reviver);
            if (r.isException()) {
                detail::decRef(k);
                return r;
            }
            if (r.isUndefined())
                deleteProperty(vm, self, k, false);
            else
                defineOwn(vm, asObject(self), k, r, attr::kDefault);
            detail::decRef(k);
        }
    }
    JSValue keyValue = JSValue::cell(key);
    JSValue callArgs[2] = {keyValue, value.get()};
    return vm.call(reviver, holder, callArgs, 2);
}

NATIVE(jsonParse)
{
    UNUSED();
    Handle text = Handle::adopt(toStringValue(vm, arg(args, argc, 0)));
    if (text.get().isException())
        return JSValue::exception();
    JsonReader reader{vm, asString(text.get())->text()};
    JSValue result = reader.value();
    if (result.isException())
        return result;
    Handle parsed = Handle::adopt(result);
    reader.skip();
    if (reader.i != reader.s.size())
        return reader.fail("unexpected token after the value");
    const JSValue reviver = arg(args, argc, 1);
    if (!isFunction(reviver))
        return parsed.take();
    JSObject* root = vm.newObject(vm.protos.object);
    Handle rootKeep = Handle::adopt(toJSValue(root));
    Key empty = vm.internKey(std::string_view());
    retain(parsed.get());
    root->putOwn(vm, empty, parsed.get(), attr::kDefault);
    JSValue r = jsonRevive(vm, toJSValue(root), empty, reviver);
    detail::decRef(empty);
    return r;
}

// --------------------------------------------------------------------- Math ---

NATIVE(mathUnary)
{
    UNUSED();
    double d;
    if (!toNumber(vm, arg(args, argc, 0), &d))
        return JSValue::exception();
    using F = double (*)(double);
    return JSValue::boxNumber(reinterpret_cast<F>(data)(d));
}

double mathRound(double d)
{
    if (!std::isfinite(d) || d == 0)
        return d;
    // Not `floor(d + 0.5)`, which is wrong for the largest value under a half.
    const double f = std::floor(d);
    const double r = d - f >= 0.5 ? f + 1 : f;
    return r == 0 && d < 0 ? -0.0 : r;
}
double mathSign(double d)
{
    return d != d ? d : d > 0 ? 1 : d < 0 ? -1 : d;
}
double mathFloor(double d) { return std::floor(d); }
double mathCeil(double d) { return std::ceil(d); }
double mathTrunc(double d) { return std::trunc(d); }
double mathAbs(double d) { return std::fabs(d); }
double mathSqrt(double d) { return std::sqrt(d); }
double mathSin(double d) { return std::sin(d); }
double mathCos(double d) { return std::cos(d); }
double mathTan(double d) { return std::tan(d); }
double mathAtan(double d) { return std::atan(d); }
double mathExp(double d) { return std::exp(d); }
double mathLog(double d) { return std::log(d); }
double mathLog2(double d) { return std::log2(d); }
double mathLog10(double d) { return std::log10(d); }
double mathCbrt(double d) { return std::cbrt(d); }

NATIVE(mathMinMax)
{
    UNUSED();
    const bool isMax = data != nullptr;
    double result = isMax ? -std::numeric_limits<double>::infinity() : std::numeric_limits<double>::infinity();
    bool nan = false;
    for (std::uint32_t i = 0; i < argc; ++i) {
        double d;
        if (!toNumber(vm, args[i], &d))
            return JSValue::exception();
        if (d != d)
            nan = true;
        else if (isMax ? (d > result || (d == 0 && result == 0 && !std::signbit(d))) : (d < result || (d == 0 && result == 0 && std::signbit(d))))
            result = d;
    }
    return JSValue::boxNumber(nan ? std::nan("") : result);
}

NATIVE(mathPow)
{
    UNUSED();
    return arithmetic(vm, Op::Exp, arg(args, argc, 0), arg(args, argc, 1));
}

NATIVE(mathAtan2)
{
    UNUSED();
    double y, x;
    if (!toNumber(vm, arg(args, argc, 0), &y) || !toNumber(vm, arg(args, argc, 1), &x))
        return JSValue::exception();
    return JSValue::boxNumber(std::atan2(y, x));
}

NATIVE(mathRandom)
{
    UNUSED();
    static std::uint64_t state = 0x9E3779B97F4A7C15ull;
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return JSValue::boxDouble(double(state >> 11) / 9007199254740992.0);
}

NATIVE(mathHypot)
{
    UNUSED();
    double sum = 0;
    for (std::uint32_t i = 0; i < argc; ++i) {
        double d;
        if (!toNumber(vm, args[i], &d))
            return JSValue::exception();
        sum += d * d;
    }
    return JSValue::boxNumber(std::sqrt(sum));
}

NATIVE(mathImul)
{
    UNUSED();
    double a, b;
    if (!toNumber(vm, arg(args, argc, 0), &a) || !toNumber(vm, arg(args, argc, 1), &b))
        return JSValue::exception();
    return JSValue::int32(std::int32_t(std::uint32_t(toInt32(a)) * std::uint32_t(toInt32(b))));
}

NATIVE(performanceNow)
{
    UNUSED();
    const auto since = std::chrono::steady_clock::now().time_since_epoch();
    return JSValue::boxNumber(double(std::chrono::duration_cast<std::chrono::nanoseconds>(since).count()) / 1e6);
}

}  // namespace

// ------------------------------------------------------------------ install ---

JSObject* VM::errorProto(ErrorCode code) const noexcept
{
    const std::size_t i = std::size_t(code);
    if (i < 48 && errorProtos[i])
        return errorProtos[i];
    return errorProtos[std::size_t(ErrorCode::Exception)];
}

JSValue VM::makeError(ErrorCode code, std::string_view message)
{
    JSObject* o = newObject(errorProto(code));
    Handle keep = Handle::adopt(toJSValue(o));
    JSValue text = makeString(*this, message);
    value(*this, o, "message", text);
    o->h.cls = std::uint8_t(ObjClass::Error);
    return keep.take();
}

JSValue VM::throwError(ErrorCode code, std::string_view message)
{
    clearException();
    exception = makeError(code, message);
    exceptionCode = code;
    return JSValue::exception();
}

void VM::installBuiltins()
{
    // The two prototypes everything else hangs from.
    JSObject* objectProto = newObject(static_cast<JSObject*>(nullptr), 7);
    setIntrinsic(&protos.object, objectProto);
    release(toJSValue(objectProto));
    // `Function.prototype` is itself a function: it can be called and does nothing.
    JSObject* functionProto = newNativeFunction(
        objectProto, [](VM&, JSValue, const JSValue*, std::uint32_t, void*) -> JSValue { return JSValue::undefined(); }, nullptr,
        nullptr, 0);
    setIntrinsic(&protos.function, functionProto);
    release(toJSValue(functionProto));
    JSObject* globalObject = newObject(objectProto, 15);
    global = globalObject;
    value(*this, globalObject, "globalThis", (retain(toJSValue(global)), toJSValue(global)));
    value(*this, globalObject, "undefined", JSValue::undefined(), 0);
    value(*this, globalObject, "NaN", JSValue::boxDouble(std::nan("")), 0);
    value(*this, globalObject, "Infinity", JSValue::boxDouble(std::numeric_limits<double>::infinity()), 0);

    method(*this, objectProto, "hasOwnProperty", hasOwnProperty, 1);
    method(*this, objectProto, "toString", objectToString, 0);
    method(*this, objectProto, "valueOf", objectValueOf, 0);
    method(*this, objectProto, "isPrototypeOf", isPrototypeOf, 1);
    method(*this, functionProto, "call", functionCall, 1);
    method(*this, functionProto, "apply", functionApply, 2);
    method(*this, functionProto, "toString", functionToString, 0);
    method(*this, functionProto, "bind", functionBind, 1);

    JSFunction* objectCtorFn = constructor(*this, "Object", objectCtor, 1, objectProto);
    method(*this, objectCtorFn, "keys", objectKeys, 1, reinterpret_cast<void*>(0));
    method(*this, objectCtorFn, "values", objectKeys, 1, reinterpret_cast<void*>(1));
    method(*this, objectCtorFn, "entries", objectKeys, 1, reinterpret_cast<void*>(2));
    method(*this, objectCtorFn, "getPrototypeOf", objectGetPrototypeOf, 1);
    method(*this, objectCtorFn, "setPrototypeOf", objectSetPrototypeOf, 2);
    method(*this, objectCtorFn, "create", objectCreate, 2);
    method(*this, objectCtorFn, "defineProperty", objectDefineProperty, 3);
    method(*this, objectCtorFn, "freeze", objectFreeze, 1);
    method(*this, objectCtorFn, "isFrozen", objectIsFrozen, 1);
    method(*this, objectCtorFn, "assign", objectAssign, 2);

    // `Function(...)` and the global `eval`: both compile at run time, in the global scope.
    {
        JSObject* fp = functionProto;
        Key key = internKey(std::string_view("Function"));
        JSFunction* f = newNativeFunction(functionProto, [](VM& vm, JSValue, const JSValue* args, std::uint32_t argc, void*) -> JSValue {
            if (!vm.ctx.options.runtimeCode)
                return vm.throwError(ErrorCode::EvalError,
                                     "the Function constructor is disabled: the engine runs only the source its host gives it");
            String params;
            for (std::uint32_t i = 0; i + 1 < argc; ++i) {
                JSValue p = toStringValue(vm, args[i]);
                if (p.isException())
                    return p;
                if (i)
                    params += u",";
                params += asString(p)->text();
                release(p);
            }
            String body;
            if (argc) {
                JSValue b = toStringValue(vm, args[argc - 1]);
                if (b.isException())
                    return b;
                body = String(asString(b)->text());
                release(b);
            }
            const String text = u"(function anonymous(" + params + u"\n) {\n" + body + u"\n})";
            return vm.evalSource(toUtf8(text), "<function>");
        }, nullptr, key, 1);
        f->flags |= JSFunction::kConstructor;
        f->materialize(*this);
        retain(toJSValue(fp));
        f->putOwn(*this, atoms.prototype, toJSValue(fp), 0);
        retain(toJSValue(f));
        fp->putOwn(*this, atoms.constructor, toJSValue(f), attr::kWritable | attr::kConfigurable);
        globalObject->putOwn(*this, key, toJSValue(f), attr::kWritable | attr::kConfigurable);
        detail::decRef(key);
        method(*this, globalObject, "eval", [](VM& vm, JSValue, const JSValue* args, std::uint32_t argc, void*) -> JSValue {
            // `SetEvalScope` marks a call that is a direct `eval` of the function running.
            const bool direct = vm.evalCode && vm.frameCount == vm.evalFrames && vm.frameBase[vm.frameCount - 1].code == vm.evalCode;
            vm.evalCode = nullptr;
            if (!vm.ctx.options.runtimeCode)
                return vm.throwError(ErrorCode::EvalError, "eval is disabled: the engine runs only the source its host gives it");
            const JSValue x = arg(args, argc, 0);
            if (!isString(x)) {
                retain(x);
                return x;
            }
            if (direct)
                return vm.evalDirect(x);
            return vm.evalSource(toUtf8(asString(x)->text()), "<eval>");
        }, 1);
    }

    // Errors.
    JSObject* errorProtoObject = newObject(objectProto);
    errorProtos[std::size_t(ErrorCode::Exception)] = errorProtoObject;  // holds the creation count
    value(*this, errorProtoObject, "name", str(*this, "Error"));
    value(*this, errorProtoObject, "message", str(*this, ""));
    method(*this, errorProtoObject, "toString", errorToString, 0);
    {
        Key key = internKey(std::string_view("stack"));
        Key gname = internKey(std::string_view("get stack")), sname = internKey(std::string_view("set stack"));
        JSFunction* g = newNativeFunction(functionProto, errorStackGetter, nullptr, gname, 0);
        Handle gk = Handle::adopt(toJSValue(g));
        JSFunction* st = newNativeFunction(functionProto, errorStackSetter, nullptr, sname, 1);
        Handle sk = Handle::adopt(toJSValue(st));
        defineAccessor(*this, errorProtoObject, key, toJSValue(g), toJSValue(st), attr::kConfigurable);
        detail::decRef(key);
        detail::decRef(gname);
        detail::decRef(sname);
    }
    {
        JSFunction* base = constructor(*this, "Error", errorCtor, 1, errorProtoObject);
        (void)base;
        // `data` is the prototype the constructor makes errors of when called without `new`.
        Rare* r = base->rare();
        r->nativeData = errorProtoObject;
    }
    struct Named
    {
        ErrorCode code;
        const char* name;
    };
    for (Named n : {Named{ErrorCode::TypeError, "TypeError"}, Named{ErrorCode::RangeError, "RangeError"},
                    Named{ErrorCode::ReferenceError, "ReferenceError"}, Named{ErrorCode::SyntaxError, "SyntaxError"},
                    Named{ErrorCode::EvalError, "EvalError"}, Named{ErrorCode::UriError, "URIError"}}) {
        JSObject* p = newObject(errorProtoObject);
        errorProtos[std::size_t(n.code)] = p;
        value(*this, p, "name", str(*this, n.name));
        value(*this, p, "message", str(*this, ""));
        JSFunction* f = constructor(*this, n.name, errorCtor, 1, p);
        f->rare()->nativeData = p;
        // Each constructor inherits from `Error`.
        Key errorKey = internKey(std::string_view("Error"));
        JSValue* base = globalObject->findOwn(*this, errorKey);
        detail::decRef(errorKey);
        if (base)
            f->setProto(*this, asObject(*base));
    }
    errorProtos[std::size_t(ErrorCode::StackOverflow)] = errorProtos[std::size_t(ErrorCode::RangeError)];
    retain(toJSValue(errorProtos[std::size_t(ErrorCode::RangeError)]));
    errorProtos[std::size_t(ErrorCode::InternalError)] = errorProtoObject;
    retain(toJSValue(errorProtoObject));
    // InternalError and AggregateError: classes of their own that inherit from Error.
    for (const char* extra : {"InternalError", "AggregateError"}) {
        JSObject* p = newObject(errorProtoObject);
        Handle protoKeep = Handle::adopt(toJSValue(p));
        value(*this, p, "name", str(*this, extra));
        value(*this, p, "message", str(*this, ""));
        const bool aggregate = extra[0] == 'A';
        JSFunction* f = constructor(*this, extra, aggregate ? aggregateErrorCtor : errorCtor, aggregate ? 2 : 1, p);
        f->rare()->nativeData = p;
        Key errorKey = internKey(std::string_view("Error"));
        JSValue* base = globalObject->findOwn(*this, errorKey);
        detail::decRef(errorKey);
        if (base)
            f->setProto(*this, asObject(*base));
    }

    // Array.
    JSArray* arrayProtoObject = newArray(objectProto);
    setIntrinsic(&protos.array, arrayProtoObject);
    release(toJSValue(arrayProtoObject));
    JSObject* ap = arrayProtoObject;
    JSFunction* arrayCtorFn = constructor(*this, "Array", arrayCtor, 1, ap);
    method(*this, arrayCtorFn, "isArray", arrayIsArray, 1);
    method(*this, ap, "push", arrayPush, 1);
    method(*this, ap, "pop", arrayPop, 0);
    method(*this, ap, "shift", arrayShift, 0);
    method(*this, ap, "unshift", arrayUnshift, 1);
    method(*this, ap, "join", arrayJoin, 1);
    method(*this, ap, "toString", arrayToString, 0);
    method(*this, ap, "slice", arraySlice, 2);
    method(*this, ap, "splice", arraySplice, 2);
    method(*this, ap, "concat", arrayConcat, 1);
    method(*this, ap, "reverse", arrayReverse, 0);
    method(*this, ap, "indexOf", arrayIndexOf, 1, reinterpret_cast<void*>(0));
    method(*this, ap, "lastIndexOf", arrayIndexOf, 1, reinterpret_cast<void*>(1));
    method(*this, ap, "includes", arrayIndexOf, 1, reinterpret_cast<void*>(2));
    method(*this, ap, "copyWithin", arrayCopyWithin, 2);
    method(*this, ap, "toReversed", arrayToReversed, 0);
    method(*this, ap, "toSorted", arrayToSorted, 1);
    method(*this, ap, "toSpliced", arrayToSpliced, 2);
    method(*this, ap, "with", arrayWith, 2);
    method(*this, ap, "forEach", arrayIterate, 1, reinterpret_cast<void*>(0));
    method(*this, ap, "map", arrayIterate, 1, reinterpret_cast<void*>(1));
    method(*this, ap, "filter", arrayIterate, 1, reinterpret_cast<void*>(2));
    method(*this, ap, "some", arrayIterate, 1, reinterpret_cast<void*>(3));
    method(*this, ap, "every", arrayIterate, 1, reinterpret_cast<void*>(4));
    method(*this, ap, "find", arrayIterate, 1, reinterpret_cast<void*>(5));
    method(*this, ap, "findIndex", arrayIterate, 1, reinterpret_cast<void*>(6));
    method(*this, ap, "reduce", arrayReduce, 1, nullptr);
    method(*this, ap, "reduceRight", arrayReduce, 1, reinterpret_cast<void*>(1));
    method(*this, ap, "fill", arrayFill, 1);
    method(*this, ap, "sort", arraySort, 1);

    // String.
    JSObject* sp = newObject(objectProto);
    setIntrinsic(&protos.string, sp);
    release(toJSValue(sp));
    JSFunction* stringCtorFn = constructor(*this, "String", stringCtor, 1, sp);
    method(*this, stringCtorFn, "fromCharCode", stringFromCharCode, 1);
    method(*this, sp, "charAt", stringCharAt, 1);
    method(*this, sp, "charCodeAt", stringCharAt, 1, reinterpret_cast<void*>(1));
    method(*this, sp, "indexOf", stringIndexOf, 1, reinterpret_cast<void*>(0));
    method(*this, sp, "lastIndexOf", stringIndexOf, 1, reinterpret_cast<void*>(1));
    method(*this, sp, "includes", stringIndexOf, 1, reinterpret_cast<void*>(2));
    method(*this, sp, "startsWith", stringIndexOf, 1, reinterpret_cast<void*>(3));
    method(*this, sp, "endsWith", stringIndexOf, 1, reinterpret_cast<void*>(4));
    method(*this, sp, "slice", stringSlice, 2, reinterpret_cast<void*>(0));
    method(*this, sp, "substring", stringSlice, 2, reinterpret_cast<void*>(1));
    method(*this, sp, "substr", stringSlice, 2, reinterpret_cast<void*>(2));
    method(*this, sp, "toLowerCase", stringCase, 0, nullptr);
    method(*this, sp, "toUpperCase", stringCase, 0, reinterpret_cast<void*>(1));
    method(*this, sp, "trim", stringTrim, 0, reinterpret_cast<void*>(0));
    method(*this, sp, "trimStart", stringTrim, 0, reinterpret_cast<void*>(1));
    method(*this, sp, "trimEnd", stringTrim, 0, reinterpret_cast<void*>(2));
    method(*this, sp, "split", stringSplit, 2);
    method(*this, sp, "repeat", stringRepeat, 1);
    method(*this, sp, "padStart", stringPad, 2, reinterpret_cast<void*>(1));
    method(*this, sp, "padEnd", stringPad, 2, nullptr);
    method(*this, sp, "replace", stringReplace, 2, nullptr);
    method(*this, sp, "replaceAll", stringReplace, 2, reinterpret_cast<void*>(1));
    method(*this, sp, "concat", stringConcat, 1);
    method(*this, sp, "localeCompare", stringLocaleCompare, 1);
    method(*this, sp, "toString", stringToString, 0);
    method(*this, sp, "valueOf", stringToString, 0);

    // Number and Boolean.
    JSObject* np = newObject(objectProto);
    setIntrinsic(&protos.number, np);
    release(toJSValue(np));
    JSFunction* numberCtorFn = constructor(*this, "Number", numberCtor, 1, np);
    method(*this, numberCtorFn, "isInteger", numberIs, 1, reinterpret_cast<void*>(0));
    method(*this, numberCtorFn, "isNaN", numberIs, 1, reinterpret_cast<void*>(1));
    method(*this, numberCtorFn, "isFinite", numberIs, 1, reinterpret_cast<void*>(2));
    method(*this, numberCtorFn, "isSafeInteger", numberIs, 1, reinterpret_cast<void*>(3));
    value(*this, numberCtorFn, "MAX_SAFE_INTEGER", JSValue::boxNumber(9007199254740991.0), 0);
    value(*this, numberCtorFn, "MIN_SAFE_INTEGER", JSValue::boxNumber(-9007199254740991.0), 0);
    value(*this, numberCtorFn, "MAX_VALUE", JSValue::boxDouble(std::numeric_limits<double>::max()), 0);
    value(*this, numberCtorFn, "MIN_VALUE", JSValue::boxDouble(5e-324), 0);
    value(*this, numberCtorFn, "EPSILON", JSValue::boxDouble(std::numeric_limits<double>::epsilon()), 0);
    value(*this, numberCtorFn, "POSITIVE_INFINITY", JSValue::boxDouble(std::numeric_limits<double>::infinity()), 0);
    value(*this, numberCtorFn, "NEGATIVE_INFINITY", JSValue::boxDouble(-std::numeric_limits<double>::infinity()), 0);
    value(*this, numberCtorFn, "NaN", JSValue::boxDouble(std::nan("")), 0);
    method(*this, np, "toString", numberToString, 1);
    method(*this, np, "toFixed", numberToFixed, 1);
    method(*this, np, "valueOf", numberValueOf, 0);
    JSObject* bp = newObject(objectProto);
    setIntrinsic(&protos.boolean, bp);
    release(toJSValue(bp));
    constructor(*this, "Boolean", booleanCtor, 1, bp);
    method(*this, bp, "toString", booleanToString, 0);
    method(*this, bp, "valueOf", [](VM& vm, JSValue self, const JSValue*, std::uint32_t, void*) -> JSValue {
        self = unwrapped(self, ObjClass::Boolean);
        if (!self.isBool())
            return vm.throwError(ErrorCode::TypeError, "Boolean.prototype.valueOf requires that 'this' be a Boolean");
        return self;
    }, 0);

    // JSON.
    {
        JSObject* json = newObject(objectProto);
        Key key = internKey(std::string_view("JSON"));
        retain(toJSValue(json));
        globalObject->putOwn(*this, key, toJSValue(json), attr::kWritable | attr::kConfigurable);
        detail::decRef(key);
        method(*this, json, "stringify", jsonStringify, 3);
        method(*this, json, "parse", jsonParse, 2);
        release(toJSValue(json));
    }
    // Math.
    JSObject* math = newObject(objectProto);
    {
        Key key = internKey(std::string_view("Math"));
        retain(toJSValue(math));
        globalObject->putOwn(*this, key, toJSValue(math), attr::kWritable | attr::kConfigurable);
        detail::decRef(key);
    }
    struct Unary
    {
        const char* name;
        double (*fn)(double);
    };
    for (Unary u : {Unary{"floor", mathFloor}, Unary{"ceil", mathCeil}, Unary{"round", mathRound}, Unary{"trunc", mathTrunc},
                    Unary{"abs", mathAbs}, Unary{"sqrt", mathSqrt}, Unary{"sin", mathSin}, Unary{"cos", mathCos},
                    Unary{"tan", mathTan}, Unary{"atan", mathAtan}, Unary{"exp", mathExp}, Unary{"log", mathLog},
                    Unary{"log2", mathLog2}, Unary{"log10", mathLog10}, Unary{"cbrt", mathCbrt}, Unary{"sign", mathSign}})
        method(*this, math, u.name, mathUnary, 1, reinterpret_cast<void*>(u.fn));
    method(*this, math, "max", mathMinMax, 2, reinterpret_cast<void*>(1));
    method(*this, math, "min", mathMinMax, 2, nullptr);
    method(*this, math, "pow", mathPow, 2);
    method(*this, math, "atan2", mathAtan2, 2);
    method(*this, math, "random", mathRandom, 0);
    method(*this, math, "hypot", mathHypot, 2);
    method(*this, math, "imul", mathImul, 2);
    value(*this, math, "PI", JSValue::boxDouble(3.141592653589793), 0);
    value(*this, math, "E", JSValue::boxDouble(2.718281828459045), 0);
    value(*this, math, "LN2", JSValue::boxDouble(0.6931471805599453), 0);
    value(*this, math, "LN10", JSValue::boxDouble(2.302585092994046), 0);
    value(*this, math, "SQRT2", JSValue::boxDouble(1.4142135623730951), 0);
    release(toJSValue(math));

    // console, print, and the number parsers.
    JSObject* console = newObject(objectProto);
    {
        Key key = internKey(std::string_view("console"));
        retain(toJSValue(console));
        globalObject->putOwn(*this, key, toJSValue(console), attr::kWritable | attr::kConfigurable);
        detail::decRef(key);
    }
    method(*this, console, "log", consoleLog, 0);
    method(*this, console, "info", consoleLog, 0);
    method(*this, console, "debug", consoleLog, 0);
    method(*this, console, "error", consoleLog, 0, reinterpret_cast<void*>(1));
    method(*this, console, "warn", consoleLog, 0, reinterpret_cast<void*>(1));
    release(toJSValue(console));
    method(*this, globalObject, "print", consoleLog, 0);
    {
        // The clock of `performance`; `Date` is installed with the rest at the end.
        JSObject* performance = newObject(objectProto);
        method(*this, performance, "now", performanceNow, 0);
        Key key = internKey(std::string_view("performance"));
        globalObject->putOwn(*this, key, toJSValue(performance), attr::kWritable | attr::kConfigurable);
        detail::decRef(key);
    }
    JSFunction* pi = method(*this, globalObject, "parseInt", parseIntImpl, 2);
    JSFunction* pf = method(*this, globalObject, "parseFloat", parseFloatImpl, 1);
    method(*this, globalObject, "isNaN", isNaNImpl, 1);
    method(*this, globalObject, "isFinite", isFiniteImpl, 1);
    value(*this, numberCtorFn, "parseInt", (retain(toJSValue(pi)), toJSValue(pi)));
    value(*this, numberCtorFn, "parseFloat", (retain(toJSValue(pf)), toJSValue(pf)));
    installSymbolAndIterators(*this);
    installMore(*this);
    installCollections(*this);
    installRegExp(*this);
    installPromise(*this);
    installDate(*this);
    installTypedArrays(*this);
    installProxy(*this);
    installExtras(*this);
    installIteratorHelpers(*this);
    installAtomics(*this);
    returnMarker = toJSValue(newObject(static_cast<JSObject*>(nullptr)));
}

namespace bi {
JSValue definePropertyFromDescriptor(VM& vm, JSObject* o, Key key, JSObject* descriptor)
{
    return definePropertyImpl(vm, o, key, descriptor);
}
}  // namespace bi

FastNative mathUnaryNative() noexcept
{
    return mathUnary;
}

}  // namespace vm
}  // namespace internal
}  // namespace script
