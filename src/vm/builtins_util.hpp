// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Helpers the built-in installers share.
#pragma once

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>

#include "script/internal/vm/convert.hpp"
#include "script/internal/vm/object.hpp"
#include "script/internal/vm/operators.hpp"

namespace script::internal::vm {
/// A stable sort that takes its scratch space from the payload pool; `std::stable_sort` asks the general allocator.
template <class T, class Less>
void mergeSort(T* first, T* last, Less less)
{
    const std::size_t n = std::size_t(last - first);
    if (n < 2)
        return;
    for (T* run = first; run < last; run += 8) {  // insertion sort on short runs
        T* end = std::min(run + 8, last);
        for (T* i = run + 1; i < end; ++i) {
            T v = *i;
            T* j = i;
            while (j > run && less(v, *(j - 1))) {
                *j = *(j - 1);
                --j;
            }
            *j = v;
        }
    }
    if (n <= 8)
        return;
    Vec<T> scratch(n);
    T* from = first;
    T* to = scratch.data();
    for (std::size_t width = 8; width < n; width *= 2) {
        for (std::size_t lo = 0; lo < n; lo += 2 * width) {
            const std::size_t mid = std::min(lo + width, n), hi = std::min(lo + 2 * width, n);
            std::size_t a = lo, b = mid, o = lo;
            while (a < mid && b < hi)
                to[o++] = less(from[b], from[a]) ? from[b++] : from[a++];
            while (a < mid)
                to[o++] = from[a++];
            while (b < hi)
                to[o++] = from[b++];
        }
        std::swap(from, to);
    }
    if (from != first)
        std::copy(from, from + n, first);
}
}  // namespace script::internal::vm

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
    int family = 0;  ///< an `IteratorFamily`, set by `newNativeIterator`
    /// 1: `*out` is the next value; 0: finished; -1: an exception is pending.
    virtual int step(VM& vm, JSValue* out) = 0;
};
/// Which prototype a native iterator has: its own for each of these, `next` checking the receiver is of its kind.
enum IteratorFamily : int { kPlainIterator = 0, kArrayIterator, kStringIterator, kMapIterator, kSetIterator, kRegExpStringIterator, kIteratorFamilies };
/// An iterator object over `state`, which it takes. Owned.
JSValue newNativeIterator(VM& vm, IteratorState* state, int family = kPlainIterator);
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
/// IteratorClose of `iterator`: with `throwing` the exception already pending stays and what closing raises is dropped.
bool iteratorClose(VM& vm, JSValue iterator, bool throwing);
template <class F>
bool forEachIterated(VM& vm, JSValue src, F&& fn)
{
    if (arrayIterationIntact(vm, src)) {
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
        if (s == Step::Fail) {
            iteratorClose(vm, iterator.get(), true);
            return false;
        }
        if (s == Step::Stop)
            return iteratorClose(vm, iterator.get(), false);
    }
}
/// `base[i]`, owned; the exception sentinel when a getter throws.
JSValue getIndex(VM& vm, JSValue base, std::uint32_t i);
/// Own property keys of `o` in the order the language gives them, each an owned string or symbol value.
bool ownKeys(VM& vm, JSObject* o, bool strings, bool symbols, bool onlyEnumerable, Vec<Handle>& out);
/// The elements of an array-like, each counted by its handle; false with an exception pending.
bool listFromArrayLike(VM& vm, JSValue list, Vec<Handle>& out);
/// `Object.defineProperty` on a key: undefined, or the exception sentinel.
JSValue definePropertyFromDescriptor(VM& vm, JSObject* o, Key key, JSObject* descriptor);
/// The remaining built-ins: Object, Reflect, Array, String, Number and Math beyond the first set.
void installMore(VM& vm);
/// Map, Set, WeakMap, WeakSet.
void installCollections(VM& vm);
/// A Map or a Set as the embedding sees it: its size, or -1 when `v` is not one of that class.
double collectionSize(JSValue v, ObjClass cls);
/// `map.get(key)`: 1 with `*out` owned, 0 when the key is absent.
int mapLookup(JSValue map, JSValue key, JSValue* out);
/// `map.set(key, value)` for a Map; the caller keeps its counts.
bool mapStore(JSValue map, JSValue key, JSValue value);
/// A new RegExp object (a literal's, or `new RegExp`): owned, or the exception sentinel with a SyntaxError.
JSValue newRegExp(VM& vm, StringView pattern, StringView flags);
/// A regular expression literal: its pattern and flags are string constants of the code.
JSValue newRegExpLiteral(VM& vm, JSValue pattern, JSValue flags);
void installRegExp(VM& vm);
void installArrayCallbacks(VM& vm);
/// What the typed array methods share with the constructors and the array iterators.
JSTypedArray* typedArrayOf(JSValue v);
bool typedArrayOutOfBounds(const JSTypedArray* t);
JSTypedArray* newTypedArrayOf(VM& vm, ElemKind kind, std::uint32_t length);
JSValue makeArrayIterator(VM& vm, JSValue target, int kind);
void installTypedArrayMethods(VM& vm, JSObject* taProto, JSObject* taCtor);
/// The mutators of Array.prototype for a receiver that is not a plain array: the language's own steps, element by element.
JSValue arrayFillGeneric(VM& vm, JSValue self, const JSValue* args, std::uint32_t argc);
JSValue arrayCopyWithinGeneric(VM& vm, JSValue self, const JSValue* args, std::uint32_t argc);
JSValue arrayReverseGeneric(VM& vm, JSValue self);
JSValue arraySortGeneric(VM& vm, JSValue self, const JSValue* args, std::uint32_t argc);
/// Sort a dense array in place with `compare` (or by default); an owned value, or the exception sentinel.
JSValue sortDenseArray(VM& vm, JSValue array, JSValue compare);
/// `ArraySpeciesCreate(original, length)`, owned; the exception sentinel on a failure.
JSValue arraySpeciesCreate(VM& vm, JSValue original, double length);
/// Note what a plain RegExp counts on staying as it is; once all that goes on the prototype and the constructor is there.
void recordRegExpOriginals(VM& vm);
/// `IsRegExp`: an object with a truthy `Symbol.match` or that is a RegExp. `*ok` is false with an exception pending.
bool isRegExpValue(VM& vm, JSValue v, bool* ok);
/// `SpeciesConstructor(object, fallback)`, owned; the exception sentinel on a failure.
JSValue speciesConstructor(VM& vm, JSValue object, JSValue fallback);
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
/// The URI functions, String's HTML methods and String.raw, groupBy.
void installExtras(VM& vm);
/// The global Iterator and the iterator helpers.
void installIteratorHelpers(VM& vm);
void installAtomics(VM& vm);
/// The trap `name` of a proxy's handler: a function, undefined for none, or the exception sentinel.
JSValue proxyTrap(VM& vm, ProxyState* ps, const char* name);
/// A property descriptor, with the fields it names marked.
struct PropDesc
{
    bool hasValue = false, hasWritable = false, hasGet = false, hasSet = false, hasEnumerable = false, hasConfigurable = false;
    bool writable = false, enumerable = false, configurable = false;
    Handle value, get, set;
    bool isAccessor() const { return hasGet || hasSet; }
    bool isData() const { return hasValue || hasWritable; }
};
/// ToPropertyDescriptor of an object, in the order the language reads its fields; false with an exception pending.
bool readPropertyDescriptor(VM& vm, JSObject* d, PropDesc* out);
/// ValidateAndApplyPropertyDescriptor on an object that is not a proxy. A refusal is a TypeError with `*rejected` set.
/// Undefined, or the exception sentinel.
JSValue defineOrdinary(VM& vm, JSObject* o, Key key, const PropDesc& desc, bool* rejected);

// The internal methods of an object, proxies included, with the checks the language makes of a proxy's traps. Each gives
// 1 or 0 for the answer, or -1 with an exception pending.
int objGetOwnProperty(VM& vm, JSObject* o, Key key, PropDesc* out);
int objDefineOwnProperty(VM& vm, JSObject* o, Key key, const PropDesc& desc);
int objHasProperty(VM& vm, JSObject* o, Key key);
int objSet(VM& vm, JSObject* o, Key key, JSValue value, JSValue receiver);
int objDelete(VM& vm, JSObject* o, Key key);
/// The prototype as an owned value, an object or null.
int objGetPrototypeOf(VM& vm, JSObject* o, Handle* out);
int objSetPrototypeOf(VM& vm, JSObject* o, JSValue proto);
/// IsArray: 1, 0, or -1 for a revoked proxy (with the TypeError pending); a proxy is an array when its target is.
int isArrayValue(VM& vm, JSValue v);
/// Why a prototype was refused, as an error message.
std::string prototypeRefusal(VM& vm, JSObject* o);
int objIsExtensible(VM& vm, JSObject* o);
int objPreventExtensions(VM& vm, JSObject* o);
/// Every own key, strings then symbols, in the order the language gives them.
bool objOwnKeys(VM& vm, JSObject* o, Vec<Handle>& out);
/// A fresh descriptor object for `d` (FromPropertyDescriptor), owned.
JSValue descriptorToObject(VM& vm, const PropDesc& d);
/// The same as `getOwnPropertyDescriptor`: a descriptor object, undefined, or the exception sentinel.
JSValue objOwnDescriptorObject(VM& vm, JSObject* o, Key key);
/// SetIntegrityLevel and TestIntegrityLevel; `frozen` for the stronger level.
int setIntegrity(VM& vm, JSObject* o, bool frozen);
int testIntegrity(VM& vm, JSObject* o, bool frozen);
/// What `Object.keys` and the like want of the keys: `strings`, `symbols`, and only the enumerable ones when asked.
bool objOwnKeysFiltered(VM& vm, JSObject* o, bool strings, bool symbols, bool onlyEnumerable, Vec<Handle>& out);
/// The own property `key` of an ordinary object as a complete descriptor, if there is one.
int ordinaryOwnProperty(VM& vm, JSObject* o, Key key, PropDesc* out);
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
