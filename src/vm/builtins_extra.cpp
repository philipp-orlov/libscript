// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// What ECMAScript has beyond the first set of built-ins: the URI functions and escape/unescape, the HTML methods of
// String, String.raw and the well-formedness checks, Object.groupBy and Map.groupBy, getOrInsert of the weak map, and the
// error classes AggregateError and InternalError.

#include <cmath>
#include <string>

#include "builtins_util.hpp"
#include "script/internal/vm/convert.hpp"
#include "script/internal/vm/operators.hpp"
#include "script/internal/vm/typed.hpp"

namespace script {
namespace internal {
namespace vm {
namespace bi {

namespace {

// ------------------------------------------------------------------ strings ---

JSValue named(VM& vm, JSValue object, const char* name)
{
    Key k = vm.internKey(std::string_view(name));
    JSValue v = getProperty(vm, object, k);
    detail::decRef(k);
    return v;
}

JSFunction* makeNative(VM& vm, const char* name, FastNative fn, void* data, std::uint32_t length)
{
    Key k = vm.internKey(std::string_view(name));
    JSFunction* f = vm.newNativeFunction(vm.protos.function, fn, data, k, length);
    detail::decRef(k);
    return f;
}

/// The text `this` is converted to, after the nullish check every String.prototype method makes.
bool thisText(VM& vm, JSValue self, const char* method, Handle* out)
{
    if (self.isNullish()) {
        vm.throwError(ErrorCode::TypeError, std::string("String.prototype.") + method + " called on null or undefined");
        return false;
    }
    JSValue s = toStringValue(vm, self);
    if (s.isException())
        return false;
    *out = Handle::adopt(s);
    return true;
}

struct HtmlMethod
{
    const char* name;
    const char* tag;
    const char* attribute;
};
constexpr HtmlMethod kHtml[] = {
    {"anchor", "a", "name"},      {"big", "big", nullptr},       {"blink", "blink", nullptr},   {"bold", "b", nullptr},
    {"fixed", "tt", nullptr},     {"fontcolor", "font", "color"}, {"fontsize", "font", "size"}, {"italics", "i", nullptr},
    {"link", "a", "href"},        {"small", "small", nullptr},   {"strike", "strike", nullptr}, {"sub", "sub", nullptr},
    {"sup", "sup", nullptr},
};

NATIVE(stringHtml)
{
    UNUSED();
    const HtmlMethod& m = kHtml[reinterpret_cast<std::intptr_t>(data)];
    Handle text;
    if (!thisText(vm, self, m.name, &text))
        return JSValue::exception();
    String out = u"<";
    for (const char* c = m.tag; *c; ++c)
        out.push_back(Char(*c));
    if (m.attribute) {
        JSValue v = toStringValue(vm, arg(args, argc, 0));
        if (v.isException())
            return v;
        Handle value = Handle::adopt(v);
        out.push_back(u' ');
        for (const char* c = m.attribute; *c; ++c)
            out.push_back(Char(*c));
        out += u"=\"";
        for (Char c : asString(v)->text()) {
            if (c == u'"')
                out += u"&quot;";
            else
                out.push_back(c);
        }
        out.push_back(u'"');
    }
    out.push_back(u'>');
    out += asString(text.get())->text();
    out += u"</";
    for (const char* c = m.tag; *c; ++c)
        out.push_back(Char(*c));
    out.push_back(u'>');
    return makeString(vm, StringView(out));
}

NATIVE(stringWellFormed)
{
    UNUSED();
    const bool repair = data != nullptr;
    Handle text;
    if (!thisText(vm, self, repair ? "toWellFormed" : "isWellFormed", &text))
        return JSValue::exception();
    const StringView s = asString(text.get())->text();
    String out;
    bool good = true;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const Char c = s[i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < s.size() && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF) {
            out.push_back(c);
            out.push_back(s[++i]);
        } else if (c >= 0xD800 && c <= 0xDFFF) {
            good = false;
            out.push_back(0xFFFD);
        } else {
            out.push_back(c);
        }
    }
    if (!repair)
        return JSValue::boolean(good);
    return makeString(vm, StringView(out));
}

NATIVE(stringRaw)
{
    UNUSED();
    if (!isObject(arg(args, argc, 0)))
        return typeError(vm, "Cannot convert undefined or null to object");
    JSValue raw = named(vm, args[0], "raw");
    if (raw.isException())
        return raw;
    Handle rawKeep = Handle::adopt(raw);
    if (!isObject(raw))
        return typeError(vm, "Cannot convert undefined or null to object");
    JSValue len = getProperty(vm, raw, vm.atoms.length);
    if (len.isException())
        return len;
    double count = 0;
    const bool ok = toNumber(vm, len, &count);
    release(len);
    if (!ok)
        return JSValue::exception();
    count = count != count ? 0 : std::trunc(count);
    String out;
    for (double i = 0; i < count; ++i) {
        Key k = toPropertyKey(vm, JSValue::boxNumber(i));
        JSValue part = getProperty(vm, raw, k);
        detail::decRef(k);
        if (part.isException())
            return part;
        JSValue text = toStringValue(vm, part);
        release(part);
        if (text.isException())
            return text;
        out += asString(text)->text();
        release(text);
        if (i + 1 < count && std::uint32_t(i) + 1 < argc) {
            JSValue sub = toStringValue(vm, args[std::uint32_t(i) + 1]);
            if (sub.isException())
                return sub;
            out += asString(sub)->text();
            release(sub);
        }
    }
    return makeString(vm, StringView(out));
}

// ---------------------------------------------------------------------- URI ---

constexpr char kUnreservedMarks[] = "-_.!~*'()";
constexpr char kReservedSet[] = ";/?:@&=+$,#";

bool isAlnum(Char c) noexcept
{
    return (c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z') || (c >= u'0' && c <= u'9');
}

bool inSet(Char c, const char* set) noexcept
{
    for (; *set; ++set)
        if (Char(*set) == c)
            return true;
    return false;
}

int hexValue(Char c) noexcept
{
    if (c >= u'0' && c <= u'9')
        return c - u'0';
    if (c >= u'a' && c <= u'f')
        return c - u'a' + 10;
    if (c >= u'A' && c <= u'F')
        return c - u'A' + 10;
    return -1;
}

/// encodeURI (data 0) and encodeURIComponent (1).
NATIVE(uriEncode)
{
    UNUSED();
    JSValue v = toStringValue(vm, arg(args, argc, 0));
    if (v.isException())
        return v;
    Handle keep = Handle::adopt(v);
    const StringView s = asString(v)->text();
    const bool component = data != nullptr;
    String out;
    static const char kHex[] = "0123456789ABCDEF";
    for (std::size_t i = 0; i < s.size(); ++i) {
        const Char c = s[i];
        if (isAlnum(c) || inSet(c, kUnreservedMarks) || (!component && inSet(c, kReservedSet))) {
            out.push_back(c);
            continue;
        }
        std::uint32_t cp = c;
        if (c >= 0xDC00 && c <= 0xDFFF)
            return vm.throwError(ErrorCode::UriError, "URI malformed");
        if (c >= 0xD800 && c <= 0xDBFF) {
            if (i + 1 >= s.size() || s[i + 1] < 0xDC00 || s[i + 1] > 0xDFFF)
                return vm.throwError(ErrorCode::UriError, "URI malformed");
            cp = 0x10000 + ((c - 0xD800) << 10) + (s[++i] - 0xDC00);
        }
        std::uint8_t bytes[4];
        int n = 0;
        if (cp < 0x80) {
            bytes[n++] = std::uint8_t(cp);
        } else if (cp < 0x800) {
            bytes[n++] = std::uint8_t(0xC0 | (cp >> 6));
            bytes[n++] = std::uint8_t(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            bytes[n++] = std::uint8_t(0xE0 | (cp >> 12));
            bytes[n++] = std::uint8_t(0x80 | ((cp >> 6) & 0x3F));
            bytes[n++] = std::uint8_t(0x80 | (cp & 0x3F));
        } else {
            bytes[n++] = std::uint8_t(0xF0 | (cp >> 18));
            bytes[n++] = std::uint8_t(0x80 | ((cp >> 12) & 0x3F));
            bytes[n++] = std::uint8_t(0x80 | ((cp >> 6) & 0x3F));
            bytes[n++] = std::uint8_t(0x80 | (cp & 0x3F));
        }
        for (int k = 0; k < n; ++k) {
            out.push_back(u'%');
            out.push_back(Char(kHex[bytes[k] >> 4]));
            out.push_back(Char(kHex[bytes[k] & 15]));
        }
    }
    return makeString(vm, StringView(out));
}

/// decodeURI (data 0) and decodeURIComponent (1).
NATIVE(uriDecode)
{
    UNUSED();
    JSValue v = toStringValue(vm, arg(args, argc, 0));
    if (v.isException())
        return v;
    Handle keep = Handle::adopt(v);
    const StringView s = asString(v)->text();
    const bool component = data != nullptr;
    String out;
    auto malformed = [&]() { return vm.throwError(ErrorCode::UriError, "URI malformed"); };
    for (std::size_t i = 0; i < s.size(); ++i) {
        const Char c = s[i];
        if (c != u'%') {
            out.push_back(c);
            continue;
        }
        const std::size_t start = i;
        auto byteAt = [&](std::size_t at, int* out2) {
            if (at + 2 >= s.size() || s[at] != u'%')
                return false;
            const int hi = hexValue(s[at + 1]), lo = hexValue(s[at + 2]);
            if (hi < 0 || lo < 0)
                return false;
            *out2 = hi * 16 + lo;
            return true;
        };
        int b;
        if (!byteAt(i, &b))
            return malformed();
        i += 2;
        if (b < 0x80) {
            const Char ch = Char(b);
            if (!component && inSet(ch, kReservedSet)) {
                out.append(s.substr(start, 3));
            } else {
                out.push_back(ch);
            }
            continue;
        }
        int extra;
        std::uint32_t cp;
        if ((b & 0xE0) == 0xC0) {
            extra = 1;
            cp = std::uint32_t(b & 0x1F);
        } else if ((b & 0xF0) == 0xE0) {
            extra = 2;
            cp = std::uint32_t(b & 0x0F);
        } else if ((b & 0xF8) == 0xF0) {
            extra = 3;
            cp = std::uint32_t(b & 0x07);
        } else {
            return malformed();
        }
        for (int k = 0; k < extra; ++k) {
            int cont;
            if (!byteAt(i + 1, &cont) || (cont & 0xC0) != 0x80)
                return malformed();
            i += 3;
            cp = (cp << 6) | std::uint32_t(cont & 0x3F);
        }
        const std::uint32_t minimum = extra == 1 ? 0x80 : extra == 2 ? 0x800 : 0x10000;
        if (cp < minimum || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
            return malformed();
        if (cp < 0x10000) {
            out.push_back(Char(cp));
        } else {
            cp -= 0x10000;
            out.push_back(Char(0xD800 + (cp >> 10)));
            out.push_back(Char(0xDC00 + (cp & 0x3FF)));
        }
    }
    return makeString(vm, StringView(out));
}

NATIVE(escapeFn)
{
    UNUSED();
    JSValue v = toStringValue(vm, arg(args, argc, 0));
    if (v.isException())
        return v;
    Handle keep = Handle::adopt(v);
    static const char kHex[] = "0123456789ABCDEF";
    String out;
    for (Char c : asString(v)->text()) {
        if (isAlnum(c) || inSet(c, "@*_+-./")) {
            out.push_back(c);
        } else if (c < 256) {
            out.push_back(u'%');
            out.push_back(Char(kHex[c >> 4]));
            out.push_back(Char(kHex[c & 15]));
        } else {
            out += u"%u";
            for (int shift = 12; shift >= 0; shift -= 4)
                out.push_back(Char(kHex[(c >> shift) & 15]));
        }
    }
    return makeString(vm, StringView(out));
}

NATIVE(unescapeFn)
{
    UNUSED();
    JSValue v = toStringValue(vm, arg(args, argc, 0));
    if (v.isException())
        return v;
    Handle keep = Handle::adopt(v);
    const StringView s = asString(v)->text();
    String out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        Char c = s[i];
        if (c == u'%') {
            if (i + 5 < s.size() + 0 && s[i + 1] == u'u' && hexValue(s[i + 2]) >= 0 && hexValue(s[i + 3]) >= 0 &&
                hexValue(s[i + 4]) >= 0 && hexValue(s[i + 5]) >= 0) {
                c = Char((hexValue(s[i + 2]) << 12) | (hexValue(s[i + 3]) << 8) | (hexValue(s[i + 4]) << 4) | hexValue(s[i + 5]));
                i += 5;
            } else if (i + 2 < s.size() && hexValue(s[i + 1]) >= 0 && hexValue(s[i + 2]) >= 0) {
                c = Char(hexValue(s[i + 1]) * 16 + hexValue(s[i + 2]));
                i += 2;
            }
        }
        out.push_back(c);
    }
    return makeString(vm, StringView(out));
}

// ----------------------------------------------------------------- groupBy ---

/// Object.groupBy (data 0) and Map.groupBy (1).
NATIVE(groupBy)
{
    UNUSED();
    const bool toMap = data != nullptr;
    const JSValue items = arg(args, argc, 0), callback = arg(args, argc, 1);
    if (items.isNullish())
        return typeError(vm, describe(vm, items) + " is not iterable");
    if (!isFunction(callback))
        return typeError(vm, describe(vm, callback) + " is not a function");
    JSValue result;
    if (toMap) {
        JSValue ctor = named(vm, toJSValue(vm.global), "Map");
        if (ctor.isException())
            return ctor;
        Handle ctorKeep = Handle::adopt(ctor);
        result = vm.construct(ctor, nullptr, 0);
    } else {
        result = toJSValue(vm.newObject(static_cast<JSObject*>(nullptr)));
    }
    if (result.isException())
        return result;
    Handle out = Handle::adopt(result);
    double index = 0;
    bool failed = false;
    const bool ok = forEachIterated(vm, items, [&](JSValue element) {
        JSValue call[2] = {element, JSValue::boxNumber(index++)};
        JSValue key = vm.call(callback, JSValue::undefined(), call, 2);
        if (key.isException()) {
            failed = true;
            return Step::Stop;
        }
        Handle keyKeep = Handle::adopt(key);
        JSValue group;
        if (toMap) {
            JSValue getFn = named(vm, result, "get");
            Handle getKeep = Handle::adopt(getFn);
            JSValue found = vm.call(getFn, result, &key, 1);
            if (found.isException()) {
                failed = true;
                return Step::Stop;
            }
            if (found.isUndefined()) {
                group = toJSValue(vm.newArray(vm.protos.array));
                JSValue setFn = named(vm, result, "set");
                Handle setKeep = Handle::adopt(setFn);
                JSValue pair[2] = {key, group};
                JSValue r = vm.call(setFn, result, pair, 2);
                if (r.isException()) {
                    release(group);
                    failed = true;
                    return Step::Stop;
                }
                release(r);
            } else {
                group = found;
            }
        } else {
            JSValue prim = toPrimitive(vm, key, true);
            if (prim.isException()) {
                failed = true;
                return Step::Stop;
            }
            Key k = toPropertyKey(vm, prim);
            release(prim);
            if (!k) {
                failed = true;
                return Step::Stop;
            }
            JSValue* slot = asObject(result)->findOwn(vm, k);
            if (slot) {
                group = *slot;
                retain(group);
            } else {
                group = toJSValue(vm.newArray(vm.protos.array));
                retain(group);
                asObject(result)->putOwn(vm, k, group, attr::kDefault);
            }
            detail::decRef(k);
        }
        Handle groupKeep = Handle::adopt(group);
        retain(element);
        static_cast<JSArray*>(asObject(group))->append(vm, element);
        return Step::Continue;
    });
    if (!ok || failed)
        return JSValue::exception();
    return out.take();
}

/// `self[name](...args)`, owned.
JSValue callMethod(VM& vm, JSValue self, const char* name, const JSValue* args, std::uint32_t argc)
{
    JSValue fn = named(vm, self, name);
    if (fn.isException())
        return fn;
    Handle keep = Handle::adopt(fn);
    if (!isFunction(fn))
        return typeError(vm, std::string(name) + " is not a function");
    return vm.call(fn, self, args, argc);
}

/// getOrInsert (data 0) and getOrInsertComputed (1) of Map and WeakMap.
NATIVE(getOrInsert)
{
    UNUSED();
    const bool computed = data != nullptr;
    const JSValue key = arg(args, argc, 0), second = arg(args, argc, 1);
    if (computed && !isFunction(second))
        return typeError(vm, describe(vm, second) + " is not a function");
    JSValue has = callMethod(vm, self, "has", &key, 1);
    if (has.isException())
        return has;
    const bool found = toBoolean(has);
    release(has);
    if (found)
        return callMethod(vm, self, "get", &key, 1);
    JSValue v = second;
    Handle made;
    if (computed) {
        JSValue r = vm.call(second, JSValue::undefined(), &key, 1);
        if (r.isException())
            return r;
        made = Handle::adopt(r);
        v = r;
    }
    const JSValue pair[2] = {key, v};
    JSValue set = callMethod(vm, self, "set", pair, 2);
    if (set.isException())
        return set;
    release(set);
    retain(v);
    return v;
}

NATIVE(mathF16round)
{
    UNUSED();
    double d;
    if (!toNumber(vm, arg(args, argc, 0), &d))
        return JSValue::exception();
    return JSValue::boxNumber(halfToDouble(doubleToHalf(d)));
}

/// The constructors of generator, async and async generator functions: Function with another head. Data 1 `function*`,
/// 2 `async function`, 3 `async function*`.
NATIVE(functionKindCtor)
{
    UNUSED();
    if (!vm.ctx.options.runtimeCode)
        return vm.throwError(ErrorCode::EvalError, "the Function constructor is disabled: the engine runs only the source its host gives it");
    static const char* const heads[] = {"", "function*", "async function", "async function*"};
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
    const String head = fromUtf8(heads[reinterpret_cast<std::intptr_t>(data)]);
    const String text = u"(" + head + u" anonymous(" + params + u"\n) {\n" + body + u"\n})";
    return vm.evalSource(toUtf8(text), "<function>");
}

NATIVE(returnThis)
{
    UNUSED();
    retain(self);
    return self;
}

}  // namespace

JSValue speciesConstructor(VM& vm, JSValue object, JSValue fallback)
{
    Handle ctor = Handle::adopt(getProperty(vm, object, vm.atoms.constructor));
    if (ctor.get().isException())
        return ctor.get();
    if (ctor.get().isUndefined()) {
        retain(fallback);
        return fallback;
    }
    if (!isObject(ctor.get()))
        return vm.throwError(ErrorCode::TypeError, "object.constructor is not an object");
    Handle species = Handle::adopt(getProperty(vm, ctor.get(), vm.atoms.symSpecies));
    if (species.get().isException())
        return species.get();
    if (species.get().isNullish()) {
        retain(fallback);
        return fallback;
    }
    if (isConstructor(species.get()))
        return species.take();
    return vm.throwError(ErrorCode::TypeError, "object.constructor[Symbol.species] is not a constructor");
}

void installExtras(VM& vm)
{
    {
        Key k = vm.internKey(std::string_view("Math"));
        JSValue* slot = vm.global->findOwn(vm, k);
        detail::decRef(k);
        if (slot && isObject(*slot))
            method(vm, asObject(*slot), "f16round", mathF16round, 1);
    }
    // GeneratorFunction, AsyncFunction and AsyncGeneratorFunction: not globals, but what such functions inherit from.
    {
        Key fk = vm.internKey(std::string_view("Function"));
        JSValue* functionCtor = vm.global->findOwn(vm, fk);
        detail::decRef(fk);
        struct Kind
        {
            const char* name;
            JSObject** slot;
            std::intptr_t code;
            JSObject* prototypeOf;  ///< what the `prototype` property of the function proto is, if any
        };
        for (const Kind& kind : {Kind{"GeneratorFunction", &vm.protos.generatorFunction, 1, vm.protos.generator},
                                 Kind{"AsyncFunction", &vm.protos.asyncFunction, 2, nullptr},
                                 Kind{"AsyncGeneratorFunction", &vm.protos.asyncGeneratorFunction, 3, vm.protos.asyncGenerator}}) {
            JSObject* proto = vm.newObject(vm.protos.function);
            vm.setIntrinsic(kind.slot, proto);
            release(toJSValue(proto));
            Key name = vm.internKey(std::string_view(kind.name));
            JSFunction* ctor = vm.newNativeFunction(vm.protos.function, functionKindCtor, reinterpret_cast<void*>(kind.code), name, 1);
            detail::decRef(name);
            ctor->flags |= JSFunction::kConstructor;
            ctor->materialize(vm);
            if (functionCtor && isObject(*functionCtor))
                ctor->setProto(vm, asObject(*functionCtor));
            retain(toJSValue(proto));
            ctor->putOwn(vm, vm.atoms.prototype, toJSValue(proto), 0);
            proto->putOwn(vm, vm.atoms.constructor, toJSValue(ctor), attr::kConfigurable);
            proto->putOwn(vm, vm.atoms.symToStringTag, makeString(vm, std::string_view(kind.name)), attr::kConfigurable);
            if (kind.prototypeOf) {
                retain(toJSValue(kind.prototypeOf));
                proto->putOwn(vm, vm.atoms.prototype, toJSValue(kind.prototypeOf), attr::kConfigurable);
                retain(toJSValue(proto));
                kind.prototypeOf->putOwn(vm, vm.atoms.constructor, toJSValue(proto), attr::kConfigurable);
            }
        }
    }
    // The tags of the namespace objects, and `Symbol.species` of the constructors that have one.
    for (const char* name : {"JSON", "Reflect"}) {
        Key k = vm.internKey(std::string_view(name));
        JSValue* slot = vm.global->findOwn(vm, k);
        detail::decRef(k);
        if (slot && isObject(*slot) && !asObject(*slot)->findOwn(vm, vm.atoms.symToStringTag))
            asObject(*slot)->putOwn(vm, vm.atoms.symToStringTag, makeString(vm, std::string_view(name)), attr::kConfigurable);
    }
    for (const char* name : {"Array", "Promise", "RegExp", "Map", "Set", "ArrayBuffer"}) {
        Key k = vm.internKey(std::string_view(name));
        JSValue* slot = vm.global->findOwn(vm, k);
        detail::decRef(k);
        if (!slot || !isObject(*slot) || asObject(*slot)->findOwn(vm, vm.atoms.symSpecies))
            continue;
        JSFunction* getter = makeNative(vm, "get [Symbol.species]", returnThis, nullptr, 0);
        Handle keep = Handle::adopt(toJSValue(getter));
        defineAccessor(vm, asObject(*slot), vm.atoms.symSpecies, toJSValue(getter), JSValue::undefined(), attr::kConfigurable);
    }
    if (isObject(vm.arrayConstructor))
        if (JSValue* slot = asObject(vm.arrayConstructor)->findOwn(vm, vm.atoms.symSpecies)) {
            vm.arraySpecies = *slot;
            retain(vm.arraySpecies);
        }
    if (isObject(vm.promiseCtor))
        if (JSValue* slot = asObject(vm.promiseCtor)->findOwn(vm, vm.atoms.symSpecies)) {
            vm.promiseSpecies = *slot;
            retain(vm.promiseSpecies);
        }
    {
        Key k = vm.internKey(std::string_view("console"));
        JSValue* slot = vm.global->findOwn(vm, k);
        detail::decRef(k);
        Key logKey = vm.internKey(std::string_view("log"));
        if (slot && isObject(*slot))
            if (JSValue* log = asObject(*slot)->findOwn(vm, logKey))
                if (isFunction(*log)) {
                    Key tk = vm.internKey(std::string_view("trace"));
                    retain(*log);
                    asObject(*slot)->putOwn(vm, tk, *log, attr::kWritable | attr::kConfigurable | attr::kEnumerable);
                    detail::decRef(tk);
                }
        detail::decRef(logKey);
    }
    for (JSObject* proto : {vm.protos.weakMap, vm.protos.map}) {
        if (!proto)
            continue;
        method(vm, proto, "getOrInsert", getOrInsert, 2);
        method(vm, proto, "getOrInsertComputed", getOrInsert, 2, reinterpret_cast<void*>(1));
    }
    JSObject* sp = vm.protos.string;
    for (std::size_t i = 0; i < sizeof(kHtml) / sizeof(kHtml[0]); ++i)
        method(vm, sp, kHtml[i].name, stringHtml, kHtml[i].attribute ? 1 : 0, reinterpret_cast<void*>(i));
    method(vm, sp, "isWellFormed", stringWellFormed, 0);
    method(vm, sp, "toWellFormed", stringWellFormed, 0, reinterpret_cast<void*>(1));

    Key stringKey = vm.internKey(std::string_view("String"));
    JSValue* stringCtor = vm.global->findOwn(vm, stringKey);
    detail::decRef(stringKey);
    if (stringCtor && isObject(*stringCtor))
        method(vm, asObject(*stringCtor), "raw", stringRaw, 1);

    defineGlobal(vm, "encodeURI", toJSValue(makeNative(vm, "encodeURI", uriEncode, nullptr, 1)));
    defineGlobal(vm, "encodeURIComponent", toJSValue(makeNative(vm, "encodeURIComponent", uriEncode, reinterpret_cast<void*>(1), 1)));
    defineGlobal(vm, "decodeURI", toJSValue(makeNative(vm, "decodeURI", uriDecode, nullptr, 1)));
    defineGlobal(vm, "decodeURIComponent", toJSValue(makeNative(vm, "decodeURIComponent", uriDecode, reinterpret_cast<void*>(1), 1)));
    defineGlobal(vm, "escape", toJSValue(makeNative(vm, "escape", escapeFn, nullptr, 1)));
    defineGlobal(vm, "unescape", toJSValue(makeNative(vm, "unescape", unescapeFn, nullptr, 1)));

    Key objectKey = vm.internKey(std::string_view("Object"));
    JSValue* objectCtor = vm.global->findOwn(vm, objectKey);
    detail::decRef(objectKey);
    if (objectCtor && isObject(*objectCtor))
        method(vm, asObject(*objectCtor), "groupBy", groupBy, 2);
    Key mapKey = vm.internKey(std::string_view("Map"));
    JSValue* mapCtor = vm.global->findOwn(vm, mapKey);
    detail::decRef(mapKey);
    if (mapCtor && isObject(*mapCtor))
        method(vm, asObject(*mapCtor), "groupBy", groupBy, 2, reinterpret_cast<void*>(1));
    installArrayCallbacks(vm);
    recordRegExpOriginals(vm);
}

}  // namespace bi
}  // namespace vm
}  // namespace internal
}  // namespace script
