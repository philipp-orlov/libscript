// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Symbol, the iterator protocol's prototypes, array and string iterators, and the generator prototype.
#include <cmath>

#include "script/internal/dyn/object.hpp"
#include "script/internal/vm/object.hpp"
#include "builtins_util.hpp"

namespace script {
namespace internal {
namespace vm {
namespace bi {

JSValue iterResult(VM& vm, JSValue value, bool done)
{
    JSObject* o = vm.newObject(vm.protos.object);
    Handle keep = Handle::adopt(toJSValue(o));
    o->putOwn(vm, vm.atoms.value, value, attr::kDefault);
    o->putOwn(vm, vm.atoms.done, JSValue::boolean(done), attr::kDefault);
    return keep.take();
}

int iteratorStep(VM& vm, JSValue iterator, JSValue next, JSValue* out)
{
    JSValue res = vm.call(next, iterator, nullptr, 0);
    if (res.isException())
        return -1;
    if (!isObject(res)) {
        vm.throwError(ErrorCode::TypeError, "Iterator result " + describe(vm, res) + " is not an object");
        release(res);
        return -1;
    }
    Handle keep = Handle::adopt(res);
    JSValue d = getProperty(vm, res, vm.atoms.done);
    if (d.isException())
        return -1;
    const bool done = toBoolean(d);
    release(d);
    if (done)
        return 0;
    JSValue v = getProperty(vm, res, vm.atoms.value);
    if (v.isException())
        return -1;
    *out = v;
    return 1;
}

bool getIterator(VM& vm, JSValue src, Handle* iterator, Handle* next)
{
    if (src.isNullish()) {
        vm.throwError(ErrorCode::TypeError, describe(vm, src) + " is not iterable");
        return false;
    }
    JSValue method = getProperty(vm, src, vm.atoms.symIterator);
    if (method.isException())
        return false;
    Handle methodKeep = Handle::adopt(method);
    if (!isFunction(method)) {
        vm.throwError(ErrorCode::TypeError, describe(vm, src) + " is not iterable");
        return false;
    }
    JSValue it = vm.call(method, src, nullptr, 0);
    if (it.isException())
        return false;
    *iterator = Handle::adopt(it);
    if (!isObject(it)) {
        vm.throwError(ErrorCode::TypeError, "Result of the Symbol.iterator method is not an object");
        return false;
    }
    JSValue n = getProperty(vm, it, vm.atoms.next);
    if (n.isException())
        return false;
    *next = Handle::adopt(n);
    return true;
}

namespace {

NATIVE(iteratorNext)
{
    UNUSED();
    if (isObject(self) && asObject(self)->cls() == ObjClass::Iterator) {
        Rare* r = asObject(self)->rare();
        if (r && r->ext) {
            auto* state = static_cast<IteratorState*>(r->ext);
            if (state->family != int(reinterpret_cast<std::intptr_t>(data)))
                return typeError(vm, "next method called on an incompatible receiver");
            JSValue out = JSValue::undefined();
            const int step = state->step(vm, &out);
            if (step < 0)
                return JSValue::exception();
            if (step == 0)
                return iterResult(vm, JSValue::undefined(), true);
            return iterResult(vm, out, false);
        }
    }
    return typeError(vm, "next method called on an incompatible receiver");
}

NATIVE(returnThis)
{
    UNUSED();
    retain(self);
    return self;
}

struct ArrayIter : IteratorState
{
    JSValue target;
    std::uint32_t index = 0;
    int kind;  // 0 keys, 1 values, 2 entries
    bool finished = false;

    ArrayIter(JSValue t, int k) : target(t), kind(k) { retain(target); }
    ~ArrayIter() override { release(target); }
    void values(Vec<JSValue>& out) const override { out.push_back(target); }
    void clear(VM&) noexcept override
    {
        release(target);
        target = JSValue::undefined();
    }
    int step(VM& vm, JSValue* out) override
    {
        if (finished)
            return 0;
        std::uint32_t length;
        if (JSTypedArray* ta = typedArrayOf(target)) {
            if (typedArrayOutOfBounds(ta)) {
                vm.throwError(ErrorCode::TypeError, "the typed array is out of bounds");
                return -1;
            }
            length = ta->length;
        } else if (isArray(target)) {
            length = static_cast<JSArray*>(asObject(target))->length;
        } else if (isString(target)) {
            length = asString(target)->length;
        } else {
            JSValue len = getProperty(vm, target, vm.atoms.length);
            if (len.isException())
                return -1;
            double d = 0;
            const bool ok = toNumber(vm, len, &d);
            release(len);
            if (!ok)
                return -1;
            length = d > 0 ? std::uint32_t(std::min(d, 4294967295.0)) : 0;
        }
        if (index >= length) {
            finished = true;
            return 0;
        }
        const std::uint32_t i = index++;
        if (kind == 0) {
            *out = JSValue::boxNumber(double(i));
            return 1;
        }
        JSValue element;
        if (isArray(target) && !static_cast<JSArray*>(asObject(target))->at(i).isHole()) {
            element = static_cast<JSArray*>(asObject(target))->at(i);
            retain(element);
        } else {
            Key k = toPropertyKey(vm, JSValue::boxNumber(double(i)));
            if (!k)
                return -1;
            element = getProperty(vm, target, k);
            detail::decRef(k);
            if (element.isException())
                return -1;
        }
        if (kind == 1) {
            *out = element;
            return 1;
        }
        JSArray* pair = vm.newArray(vm.protos.array);
        Handle keep = Handle::adopt(toJSValue(pair));
        pair->append(vm, JSValue::boxNumber(double(i)));
        pair->append(vm, element);
        *out = keep.take();
        return 1;
    }
};

struct StringIter : IteratorState
{
    JSValue text;
    std::uint32_t index = 0;

    explicit StringIter(JSValue t) : text(t) { retain(text); }
    ~StringIter() override { release(text); }
    void values(Vec<JSValue>& out) const override { out.push_back(text); }
    void clear(VM&) noexcept override
    {
        release(text);
        text = JSValue::undefined();
    }
    int step(VM& vm, JSValue* out) override
    {
        if (!isString(text))
            return 0;
        StringCell* s = asString(text);
        if (index >= s->length)
            return 0;
        const StringView v = s->text();
        std::size_t n = 1;
        if (v[index] >= 0xd800 && v[index] < 0xdc00 && index + 1 < s->length && v[index + 1] >= 0xdc00 && v[index + 1] < 0xe000)
            n = 2;
        *out = makeString(vm, v.substr(index, n));
        index += std::uint32_t(n);
        return 1;
    }
};

NATIVE(arrayIterator)
{
    UNUSED();
    if (self.isNullish())
        return typeError(vm, "Array.prototype iterator called on null or undefined");
    return newNativeIterator(vm, new ArrayIter(self, int(reinterpret_cast<std::intptr_t>(data))), kArrayIterator);
}

NATIVE(stringIterator)
{
    UNUSED();
    if (self.isNullish())
        return typeError(vm, "String.prototype[Symbol.iterator] called on null or undefined");
    JSValue text = toStringValue(vm, self);
    if (text.isException())
        return text;
    Handle keep = Handle::adopt(text);
    return newNativeIterator(vm, new StringIter(text), kStringIterator);
}

NATIVE(generatorResume)
{
    UNUSED();
    return vm.resumeGenerator(self, int(reinterpret_cast<std::intptr_t>(data)), arg(args, argc, 0));
}

// ------------------------------------------------------------------- Symbol ---

dyn::SymbolCell* thisSymbol(VM& vm, JSValue self)
{
    self = unwrapped(self, ObjClass::Symbol);
    if (isSymbol(self))
        return static_cast<dyn::SymbolCell*>(self.asCell());
    typeError(vm, "Symbol.prototype method called on an incompatible receiver");
    return nullptr;
}

NATIVE(symbolCtor)
{
    UNUSED();
    if (!vm.newTarget.isUndefined())
        return typeError(vm, "Symbol is not a constructor");
    const JSValue d = arg(args, argc, 0);
    if (d.isUndefined())
        return vm.newSymbol(StringView(), false);
    JSValue text = toStringValue(vm, d);
    if (text.isException())
        return text;
    Handle keep = Handle::adopt(text);
    return vm.newSymbol(asString(text)->text(), true);
}

NATIVE(symbolToString)
{
    UNUSED();
    dyn::SymbolCell* s = thisSymbol(vm, self);
    if (!s)
        return JSValue::exception();
    return makeString(vm, StringView(String(u"Symbol(") + s->description + u")"));
}

NATIVE(symbolValueOf)
{
    UNUSED();
    if (!thisSymbol(vm, self))
        return JSValue::exception();
    self = unwrapped(self, ObjClass::Symbol);
    retain(self);
    return self;
}

NATIVE(symbolDescription)
{
    UNUSED();
    dyn::SymbolCell* s = thisSymbol(vm, self);
    if (!s)
        return JSValue::exception();
    if (!s->hasDescription)
        return JSValue::undefined();
    return makeString(vm, StringView(s->description));
}

NATIVE(symbolFor)
{
    UNUSED();
    JSValue text = toStringValue(vm, arg(args, argc, 0));
    if (text.isException())
        return text;
    Handle keep = Handle::adopt(text);
    const String name(asString(text)->text());
    auto it = vm.symbolRegistry.find(name);
    if (it != vm.symbolRegistry.end()) {
        retain(JSValue::cell(it->second));
        return JSValue::cell(it->second);
    }
    JSValue made = vm.newSymbol(StringView(name), true);
    static_cast<dyn::SymbolCell*>(made.asCell())->registered = true;
    retain(made);  // the registry's own
    vm.symbolRegistry.emplace(name, static_cast<Key>(made.asCell()));
    return made;
}

NATIVE(symbolKeyFor)
{
    UNUSED();
    const JSValue s = arg(args, argc, 0);
    if (!isSymbol(s))
        return typeError(vm, "Symbol.keyFor: not a symbol");
    auto* cell = static_cast<dyn::SymbolCell*>(s.asCell());
    return cell->registered ? makeString(vm, StringView(cell->description)) : JSValue::undefined();
}

}  // namespace

JSValue makeArrayIterator(VM& vm, JSValue target, int kind)
{
    return newNativeIterator(vm, new ArrayIter(target, kind), kArrayIterator);
}

JSValue newNativeIterator(VM& vm, IteratorState* state, int family)
{
    std::unique_ptr<IteratorState> owned(state);
    owned->family = family;
    JSObject* o = vm.newObject(vm.protos.iteratorFamilies[family] ? vm.protos.iteratorFamilies[family] : vm.protos.nativeIterator);
    Handle keep = Handle::adopt(toJSValue(o));
    o->h.cls = std::uint8_t(ObjClass::Iterator);
    o->rareOf(vm).ext = owned.release();
    return keep.take();
}

void installSymbolAndIterators(VM& vm)
{
    JSObject* objectProto = vm.protos.object;

    // %IteratorPrototype%, and what array, map and set iterators inherit.
    JSObject* iteratorProto = vm.newObject(objectProto);
    vm.setIntrinsic(&vm.protos.iterator, iteratorProto);
    release(toJSValue(iteratorProto));
    symbolMethod(vm, vm.protos.iterator, vm.atoms.symIterator, "[Symbol.iterator]", returnThis, 0);
    JSObject* nativeProto = vm.newObject(vm.protos.iterator);
    vm.setIntrinsic(&vm.protos.nativeIterator, nativeProto);
    release(toJSValue(nativeProto));
    method(vm, nativeProto, "next", iteratorNext, 0);
    nativeProto->putOwn(vm, vm.atoms.symToStringTag, makeString(vm, std::string_view("Iterator")), attr::kConfigurable);
    {
        const char* const tags[] = {nullptr, "Array Iterator", "String Iterator", "Map Iterator", "Set Iterator", "RegExp String Iterator"};
        for (int family = 1; family < kIteratorFamilies; ++family) {
            JSObject* proto = vm.newObject(vm.protos.iterator);
            vm.setIntrinsic(&vm.protos.iteratorFamilies[family], proto);
            release(toJSValue(proto));
            method(vm, proto, "next", iteratorNext, 0, reinterpret_cast<void*>(std::intptr_t(family)));
            proto->putOwn(vm, vm.atoms.symToStringTag, makeString(vm, std::string_view(tags[family])), attr::kConfigurable);
        }
    }

    // Generators.
    JSObject* generatorProto = vm.newObject(vm.protos.iterator);
    vm.setIntrinsic(&vm.protos.generator, generatorProto);
    release(toJSValue(generatorProto));
    method(vm, generatorProto, "next", generatorResume, 1, reinterpret_cast<void*>(0));
    method(vm, generatorProto, "throw", generatorResume, 1, reinterpret_cast<void*>(1));
    method(vm, generatorProto, "return", generatorResume, 1, reinterpret_cast<void*>(2));
    generatorProto->putOwn(vm, vm.atoms.symToStringTag, makeString(vm, std::string_view("Generator")), attr::kConfigurable);

    // Arrays and strings.
    JSObject* ap = vm.protos.array;
    JSFunction* values = method(vm, ap, "values", arrayIterator, 0, reinterpret_cast<void*>(1));
    method(vm, ap, "keys", arrayIterator, 0, reinterpret_cast<void*>(0));
    method(vm, ap, "entries", arrayIterator, 0, reinterpret_cast<void*>(2));
    retain(toJSValue(values));
    ap->putOwn(vm, vm.atoms.symIterator, toJSValue(values), attr::kWritable | attr::kConfigurable);
    vm.origArrayValues = toJSValue(values);
    if (JSValue* next = vm.protos.nativeIterator->findOwn(vm, vm.atoms.next))
        vm.origIteratorNext = *next;
    {
        JSObject* argumentsProto = vm.newObject(vm.protos.object);
        vm.setIntrinsic(&vm.protos.arguments, argumentsProto);
        release(toJSValue(argumentsProto));
        retain(toJSValue(values));
        argumentsProto->putOwn(vm, vm.atoms.symIterator, toJSValue(values), attr::kWritable | attr::kConfigurable);
    }
    symbolMethod(vm, vm.protos.string, vm.atoms.symIterator, "[Symbol.iterator]", stringIterator, 0);

    // Symbol.
    JSObject* sp = vm.newObject(objectProto);
    vm.setIntrinsic(&vm.protos.symbol, sp);
    release(toJSValue(sp));
    JSFunction* ctor = constructor(vm, "Symbol", symbolCtor, 0, sp);
    ctor->flags &= ~JSFunction::kConstructor;
    struct Well
    {
        const char* name;
        Key key;
    };
    for (const Well& w : {Well{"iterator", vm.atoms.symIterator}, Well{"asyncIterator", vm.atoms.symAsyncIterator},
                          Well{"toPrimitive", vm.atoms.symToPrimitive}, Well{"toStringTag", vm.atoms.symToStringTag},
                          Well{"hasInstance", vm.atoms.symHasInstance}, Well{"species", vm.atoms.symSpecies},
                          Well{"match", vm.atoms.symMatch}, Well{"matchAll", vm.atoms.symMatchAll},
                          Well{"replace", vm.atoms.symReplace}, Well{"search", vm.atoms.symSearch},
                          Well{"split", vm.atoms.symSplit}, Well{"isConcatSpreadable", vm.atoms.symIsConcatSpreadable},
                          Well{"unscopables", vm.atoms.symUnscopables}}) {
        retain(JSValue::cell(w.key));
        value(vm, ctor, w.name, JSValue::cell(w.key), 0);
    }
    method(vm, ctor, "for", symbolFor, 1);
    method(vm, ctor, "keyFor", symbolKeyFor, 1);
    method(vm, sp, "toString", symbolToString, 0);
    method(vm, sp, "valueOf", symbolValueOf, 0);
    {
        Key key = vm.internKey(std::string_view("description"));
        Key fnName = vm.internKey(std::string_view("description"));
        JSFunction* getter = vm.newNativeFunction(vm.protos.function, symbolDescription, nullptr, fnName, 0);
        Handle keep = Handle::adopt(toJSValue(getter));
        defineAccessor(vm, sp, key, toJSValue(getter), JSValue::undefined(), attr::kConfigurable);
        detail::decRef(key);
        detail::decRef(fnName);
    }
    sp->putOwn(vm, vm.atoms.symToStringTag, makeString(vm, std::string_view("Symbol")), attr::kConfigurable);
}

}  // namespace bi
}  // namespace vm
}  // namespace internal
}  // namespace script
