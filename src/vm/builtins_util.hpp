// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Helpers the built-in installers share.
#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "script/internal/vm/convert.hpp"
#include "script/internal/vm/object.hpp"
#include "script/internal/vm/operators.hpp"

namespace script {
namespace internal {
namespace vm {
namespace bi {

#define NATIVE(name) JSValue name(VM& vm, JSValue self, const JSValue* args, std::uint32_t argc, void* data)
#define UNUSED() \
    (void)vm;    \
    (void)self;  \
    (void)args;  \
    (void)argc;  \
    (void)data

inline JSValue arg(const JSValue* args, std::uint32_t argc, std::uint32_t i) noexcept
{
    return i < argc ? args[i] : JSValue::undefined();
}

inline JSValue str(VM& vm, std::string_view text)
{
    return makeString(vm, text);
}

inline JSValue typeError(VM& vm, const std::string& message)
{
    return vm.throwError(ErrorCode::TypeError, message);
}

/// An own, non-enumerable function property.
inline JSFunction* method(VM& vm, JSObject* target, const char* name, FastNative fn, std::uint32_t length, void* data = nullptr)
{
    Key key = vm.internKey(std::string_view(name));
    JSFunction* f = vm.newNativeFunction(vm.protos.function, fn, data, key, length);
    // The property takes the creation count; the caller's pointer is good for as long as the property is.
    target->putOwn(vm, key, toJSValue(f), attr::kWritable | attr::kConfigurable);
    detail::decRef(key);
    return f;
}

inline void value(VM& vm, JSObject* target, const char* name, JSValue v, std::uint8_t attrs = attr::kWritable | attr::kConfigurable)
{
    Key key = vm.internKey(std::string_view(name));
    target->putOwn(vm, key, v, attrs);
    detail::decRef(key);
}

inline JSObject* newPlain(VM& vm, JSObject* proto)
{
    return vm.newObject(proto);
}

/// A constructor function with its prototype object, installed on the global object.
inline JSFunction* constructor(VM& vm, const char* name, FastNative fn, std::uint32_t length, JSObject* proto)
{
    Key key = vm.internKey(std::string_view(name));
    JSFunction* f = vm.newNativeFunction(vm.protos.function, fn, nullptr, key, length);
    f->materialize(vm);
    f->flags |= JSFunction::kConstructor;
    retain(toJSValue(proto));
    f->putOwn(vm, vm.atoms.prototype, toJSValue(proto), 0);
    retain(toJSValue(f));
    proto->putOwn(vm, vm.atoms.constructor, toJSValue(f), attr::kWritable | attr::kConfigurable);
    vm.global->putOwn(vm, key, toJSValue(f), attr::kWritable | attr::kConfigurable);
    detail::decRef(key);
    return f;
}

/// An iterator made by native code: `step` yields the next value (owned) and says whether there was one.
struct IteratorState : NativeState
{
    /// 1: `*out` is the next value; 0: finished; -1: an exception is pending.
    virtual int step(VM& vm, JSValue* out) = 0;
};
/// An iterator object over `state`, which it takes. Owned.
JSValue newNativeIterator(VM& vm, IteratorState* state);
/// `{ value, done }`, taking the count of `value`. Owned.
JSValue iterResult(VM& vm, JSValue value, bool done);
/// Install `Symbol`, the iterator prototypes and the generator prototype; the array and string iterators.
void installSymbolAndIterators(VM& vm);
/// The next value of an iterator through its `next`: 1 with `*out` owned, 0 when it is done, -1 on an exception.
int iteratorStep(VM& vm, JSValue iterator, JSValue next, JSValue* out);
/// `src[Symbol.iterator]()` and its `next`, both owned; false with an exception pending when `src` is not iterable.
bool getIterator(VM& vm, JSValue src, Handle* iterator, Handle* next);
/// Run `fn(value)` over what `src` iterates (a value, owned by the caller of `fn` for the call): return true to go on,
/// false to stop. False from this with an exception pending when iterating failed or `fn` raised it (`fn` returns
/// `Stop`).
enum class Step { Continue, Stop, Fail };
template <class F>
bool forEachIterated(VM& vm, JSValue src, F&& fn)
{
    if (isArray(src)) {
        Handle keep = Handle::retained(src);
        auto* a = static_cast<JSArray*>(asObject(src));
        for (std::uint32_t i = 0; i < a->length; ++i) {
            Handle e = Handle::retained(a->at(i).isHole() ? JSValue::undefined() : a->at(i));
            const Step s = fn(e.get());
            if (s == Step::Fail)
                return false;
            if (s == Step::Stop)
                return true;
        }
        return true;
    }
    Handle iterator, next;
    if (!getIterator(vm, src, &iterator, &next))
        return false;
    for (;;) {
        JSValue v;
        const int step = iteratorStep(vm, iterator.get(), next.get(), &v);
        if (step < 0)
            return false;
        if (step == 0)
            return true;
        Handle e = Handle::adopt(v);
        const Step s = fn(e.get());
        if (s == Step::Fail)
            return false;
        if (s == Step::Stop)
            return true;
    }
}
/// `base[i]`, owned; the exception sentinel when a getter throws.
JSValue getIndex(VM& vm, JSValue base, std::uint32_t i);
/// Own property keys of `o` in the order the language gives them, each an owned string or symbol value.
void ownKeys(VM& vm, JSObject* o, bool strings, bool symbols, bool onlyEnumerable, std::vector<Handle>& out);
/// `Object.defineProperty` on a key: undefined, or the exception sentinel.
JSValue definePropertyFromDescriptor(VM& vm, JSObject* o, Key key, JSObject* descriptor);
/// The remaining built-ins: Object, Reflect, Array, String, Number and Math beyond the first set.
void installMore(VM& vm);
/// Map, Set, WeakMap, WeakSet.
void installCollections(VM& vm);
/// A new RegExp object (a literal's, or `new RegExp`): owned, or the exception sentinel with a SyntaxError.
JSValue newRegExp(VM& vm, StringView pattern, StringView flags);
void installRegExp(VM& vm);
/// If `pattern` is an object with a `Symbol.<which>` method, call it with (string, extra) and say so in `*handled`.
JSValue callSymbolMethod(VM& vm, JSValue pattern, Key symbol, JSValue string, const JSValue* extra, std::uint32_t extraCount,
                         bool* handled);
/// Begin an async function whose frame is in `generator` (taking its count): runs to the first `await` and gives back the
/// promise of the result. Owned.
JSValue asyncStart(VM& vm, JSValue generator);
void installPromise(VM& vm);
/// The object an async generator function's call makes: it queues `next`, `throw` and `return` and drives the frame in
/// `generator` (which it takes). Owned.
JSValue asyncGeneratorStart(VM& vm, JSValue generator, JSObject* proto);
/// `await`: the promise to wait on for `value`, owned.
/// The async-from-sync wrapper `for await` puts around a sync iterator: owned iterator and its `next`.
JSValue asyncFromSyncIterator(VM& vm, JSValue iterator, JSValue next);
void installDate(VM& vm);
void installTypedArrays(VM& vm);
void installProxy(VM& vm);
/// The keys a proxy's `ownKeys` trap (or its target) gives.
bool proxyOwnKeys(VM& vm, JSObject* proxy, std::vector<Handle>& out);
/// `getOwnPropertyDescriptor` through the proxy: a descriptor object, undefined, or the exception sentinel.
JSValue proxyOwnDescriptor(VM& vm, JSObject* proxy, Key key);
JSValue proxyDefine(VM& vm, JSObject* proxy, Key key, JSObject* descriptor);
/// A descriptor object for an own property of an ordinary object, or undefined.
JSValue ownDescriptorObject(VM& vm, JSObject* o, Key key);
/// A global property, taking the count of `v`.
inline void defineGlobal(VM& vm, const char* name, JSValue v)
{
    Key key = vm.internKey(std::string_view(name));
    vm.global->putOwn(vm, key, v, attr::kWritable | attr::kConfigurable);
    detail::decRef(key);
}
/// An own non-enumerable method named by a symbol.
inline JSFunction* symbolMethod(VM& vm, JSObject* target, Key symbol, const char* name, FastNative fn, std::uint32_t length,
                                void* data = nullptr)
{
    Key fnName = vm.internKey(std::string_view(name));
    JSFunction* f = vm.newNativeFunction(vm.protos.function, fn, data, fnName, length);
    detail::decRef(fnName);
    target->putOwn(vm, symbol, toJSValue(f), attr::kWritable | attr::kConfigurable);
    return f;
}

}  // namespace bi
}  // namespace vm
}  // namespace internal
}  // namespace script
