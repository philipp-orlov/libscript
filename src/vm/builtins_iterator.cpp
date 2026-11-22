// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The global Iterator: Iterator.from, Iterator.concat, and the helpers on %Iterator.prototype% (map, filter, take, drop,
// flatMap, and the consuming reduce, toArray, forEach, some, every, find).

#include <typeinfo>
#include <cmath>
#include <limits>

#include "builtins_util.hpp"
#include "script/internal/vm/convert.hpp"
#include "script/internal/vm/operators.hpp"

namespace script {
namespace internal {
namespace vm {
namespace bi {

namespace {

enum class Kind : std::uint8_t { Map, Filter, Take, Drop, FlatMap, Concat, Wrap };

struct Helper : NativeState
{
    Kind kind;
    enum State : std::uint8_t { Start, Yielded, Running, Done } state = Start;
    Handle iterator, next;     ///< the underlying iterator, and its `next`
    Handle fn;                 ///< map, filter and flatMap's function
    double remaining = 0;      ///< take and drop
    double counter = 0;
    Handle innerIterator, innerNext;                     ///< flatMap's and concat's current iterable
    bool inInner = false;
    Vec<Handle> iterables, methods;              ///< concat's
    std::size_t at = 0;

    void values(Vec<JSValue>& out) const override
    {
        for (const Handle* h : {&iterator, &next, &fn, &innerIterator, &innerNext})
            out.push_back(h->get());
        for (const Handle& h : iterables)
            out.push_back(h.get());
        for (const Handle& h : methods)
            out.push_back(h.get());
    }
    void clear(VM&) noexcept override
    {
        iterator = next = fn = innerIterator = innerNext = Handle();
        iterables.clear();
        methods.clear();
    }
};

Helper* helperOf(JSValue v)
{
    if (isObject(v) && asObject(v)->cls() == ObjClass::Iterator)
        if (Rare* r = asObject(v)->rare(); r && r->ext)
            return typeid(*r->ext) == typeid(Helper) ? static_cast<Helper*>(r->ext) : nullptr;
    return nullptr;
}

/// IteratorClose. With `throwing` the exception already pending stays and whatever closing raises is dropped; otherwise a
/// failure of closing is the exception. False when an exception is pending afterwards.
bool closeIterator(VM& vm, JSValue iterator, bool throwing)
{
    JSValue saved = JSValue::undefined();
    ErrorCode savedCode = ErrorCode::Ok;
    if (throwing) {
        saved = vm.exception;
        savedCode = vm.exceptionCode;
        vm.exception = JSValue::undefined();
        vm.exceptionCode = ErrorCode::Ok;
    }
    bool ok = true;
    JSValue ret = getProperty(vm, iterator, vm.atoms.return_);
    if (ret.isException()) {
        ok = false;
    } else {
        Handle retKeep = Handle::adopt(ret);
        if (!ret.isNullish()) {
            if (!isFunction(ret)) {
                vm.throwError(ErrorCode::TypeError, "iterator.return is not a function");
                ok = false;
            } else {
                JSValue r = vm.call(ret, iterator, nullptr, 0);
                if (r.isException()) {
                    ok = false;
                } else {
                    if (!throwing && !isObject(r)) {
                        vm.throwError(ErrorCode::TypeError, "iterator.return() did not return an object");
                        ok = false;
                    }
                    release(r);
                }
            }
        }
    }
    if (throwing) {
        vm.clearException();
        vm.exception = saved;
        vm.exceptionCode = savedCode;
        return false;
    }
    return ok;
}

/// GetIteratorDirect: the object itself and its `next`.
bool iteratorDirect(VM& vm, JSValue obj, Handle* iterator, Handle* next)
{
    *iterator = Handle::retained(obj);
    JSValue n = getProperty(vm, obj, vm.atoms.next);
    if (n.isException())
        return false;
    *next = Handle::adopt(n);
    return true;
}

/// GetIteratorFlattenable. `strings` accepts a string primitive.
bool iteratorFlattenable(VM& vm, JSValue obj, bool strings, Handle* iterator, Handle* next)
{
    if (!isObject(obj) && !(strings && isString(obj))) {
        vm.throwError(ErrorCode::TypeError, describe(vm, obj) + " is not an object");
        return false;
    }
    JSValue method = getProperty(vm, obj, vm.atoms.symIterator);
    if (method.isException())
        return false;
    Handle methodKeep = Handle::adopt(method);
    JSValue it;
    if (method.isNullish()) {
        it = obj;
        retain(it);
    } else {
        if (!isFunction(method)) {
            vm.throwError(ErrorCode::TypeError, "Symbol.iterator is not a function");
            return false;
        }
        it = vm.call(method, obj, nullptr, 0);
        if (it.isException())
            return false;
    }
    Handle itKeep = Handle::adopt(it);
    if (!isObject(it)) {
        vm.throwError(ErrorCode::TypeError, "iterator is not an object");
        return false;
    }
    if (!iteratorDirect(vm, it, iterator, next))
        return false;
    return true;
}

JSValue newHelperObject(VM& vm, Helper* state, JSObject* proto)
{
    // `proto` is one of the VM's own.
    std::unique_ptr<Helper> owned(state);
    JSObject* o = vm.newObject(proto);
    Handle keep = Handle::adopt(toJSValue(o));
    o->h.cls = std::uint8_t(ObjClass::Iterator);
    o->rareOf(vm).ext = owned.release();
    return keep.take();
}

/// The step of the underlying iterator of `h` with a failure closing it: 1 value in `*out`, 0 done, -1 exception.
int underlyingStep(VM& vm, Helper* h, JSValue* out)
{
    if (!isFunction(h->next.get())) {
        vm.throwError(ErrorCode::TypeError, "iterator.next is not a function");
        return -1;
    }
    return iteratorStep(vm, h->iterator.get(), h->next.get(), out);
}

NATIVE(helperNext);

/// One step of a helper: 1 with `*out`, 0 when it is done, -1 with an exception pending.
int helperStep(VM& vm, Helper* h, JSValue* out)
{
    JSValue v;
    switch (h->kind) {
        case Kind::Map: {
            const int s = underlyingStep(vm, h, &v);
            if (s <= 0)
                return s;
            Handle vk = Handle::adopt(v);
            JSValue args[2] = {v, JSValue::boxNumber(h->counter++)};
            JSValue r = vm.call(h->fn.get(), JSValue::undefined(), args, 2);
            if (r.isException()) {
                closeIterator(vm, h->iterator.get(), true);
                return -1;
            }
            *out = r;
            return 1;
        }
        case Kind::Filter: {
            for (;;) {
                const int s = underlyingStep(vm, h, &v);
                if (s <= 0)
                    return s;
                Handle vk = Handle::adopt(v);
                JSValue args[2] = {v, JSValue::boxNumber(h->counter++)};
                JSValue r = vm.call(h->fn.get(), JSValue::undefined(), args, 2);
                if (r.isException()) {
                    closeIterator(vm, h->iterator.get(), true);
                    return -1;
                }
                const bool keep = toBoolean(r);
                release(r);
                if (keep) {
                    retain(v);
                    *out = v;
                    return 1;
                }
            }
        }
        case Kind::Take: {
            if (h->remaining == 0) {
                if (!closeIterator(vm, h->iterator.get(), false))
                    return -1;
                return 0;
            }
            if (h->remaining != std::numeric_limits<double>::infinity())
                h->remaining -= 1;
            const int s = underlyingStep(vm, h, &v);
            if (s <= 0)
                return s;
            *out = v;
            return 1;
        }
        case Kind::Drop: {
            while (h->remaining > 0) {
                if (h->remaining != std::numeric_limits<double>::infinity())
                    h->remaining -= 1;
                const int s = underlyingStep(vm, h, &v);
                if (s <= 0)
                    return s;
                release(v);
            }
            const int s = underlyingStep(vm, h, &v);
            if (s <= 0)
                return s;
            *out = v;
            return 1;
        }
        case Kind::FlatMap: {
            for (;;) {
                if (h->inInner) {
                    JSValue inner;
                    const int s = iteratorStep(vm, h->innerIterator.get(), h->innerNext.get(), &inner);
                    if (s < 0) {
                        closeIterator(vm, h->iterator.get(), true);
                        return -1;
                    }
                    if (s == 1) {
                        *out = inner;
                        return 1;
                    }
                    h->inInner = false;
                    continue;
                }
                const int s = underlyingStep(vm, h, &v);
                if (s <= 0)
                    return s;
                Handle vk = Handle::adopt(v);
                JSValue args[2] = {v, JSValue::boxNumber(h->counter++)};
                JSValue mapped = vm.call(h->fn.get(), JSValue::undefined(), args, 2);
                if (mapped.isException()) {
                    closeIterator(vm, h->iterator.get(), true);
                    return -1;
                }
                Handle mk = Handle::adopt(mapped);
                if (!iteratorFlattenable(vm, mapped, false, &h->innerIterator, &h->innerNext)) {
                    closeIterator(vm, h->iterator.get(), true);
                    return -1;
                }
                h->inInner = true;
            }
        }
        case Kind::Concat: {
            for (;;) {
                if (h->inInner) {
                    JSValue inner;
                    const int s = iteratorStep(vm, h->innerIterator.get(), h->innerNext.get(), &inner);
                    if (s < 0)
                        return -1;
                    if (s == 1) {
                        *out = inner;
                        return 1;
                    }
                    h->inInner = false;
                    continue;
                }
                if (h->at >= h->iterables.size())
                    return 0;
                JSValue it = vm.call(h->methods[h->at].get(), h->iterables[h->at].get(), nullptr, 0);
                ++h->at;
                if (it.isException())
                    return -1;
                Handle itKeep = Handle::adopt(it);
                if (!isObject(it)) {
                    vm.throwError(ErrorCode::TypeError, "iterator is not an object");
                    return -1;
                }
                if (!iteratorDirect(vm, it, &h->innerIterator, &h->innerNext))
                    return -1;
                h->inInner = true;
            }
        }
        case Kind::Wrap: break;
    }
    return 0;
}

NATIVE(helperNext)
{
    UNUSED();
    Helper* h = helperOf(self);
    if (!h || h->kind == Kind::Wrap)
        return typeError(vm, "next method called on an incompatible receiver");
    if (h->state == Helper::Running)
        return typeError(vm, "Generator is already running");
    if (h->state == Helper::Done)
        return iterResult(vm, JSValue::undefined(), true);
    h->state = Helper::Running;
    JSValue out = JSValue::undefined();
    const int s = helperStep(vm, h, &out);
    if (s < 0) {
        h->state = Helper::Done;
        return JSValue::exception();
    }
    if (s == 0) {
        h->state = Helper::Done;
        return iterResult(vm, JSValue::undefined(), true);
    }
    h->state = Helper::Yielded;
    return iterResult(vm, out, false);
}

NATIVE(helperReturn)
{
    UNUSED();
    Helper* h = helperOf(self);
    if (!h || h->kind == Kind::Wrap)
        return typeError(vm, "return method called on an incompatible receiver");
    if (h->state == Helper::Running)
        return typeError(vm, "Generator is already running");
    if (h->state == Helper::Done)
        return iterResult(vm, JSValue::undefined(), true);
    h->state = Helper::Done;
    const bool wasInner = h->inInner;
    if (wasInner && !closeIterator(vm, h->innerIterator.get(), false)) {
        closeIterator(vm, h->iterator.get(), true);
        return JSValue::exception();
    }
    if (h->kind != Kind::Concat && isObject(h->iterator.get()) && !closeIterator(vm, h->iterator.get(), false))
        return JSValue::exception();
    return iterResult(vm, JSValue::undefined(), true);
}

// ----------------------------------------------------------------- helpers ---

JSValue makeHelper(VM& vm, JSValue self, Kind kind, JSValue fn, double count, const char* name)
{
    if (!isObject(self))
        return typeError(vm, std::string("Iterator.prototype.") + name + " called on non-object");
    auto h = std::make_unique<Helper>();
    h->kind = kind;
    h->remaining = count;
    h->iterator = Handle::retained(self);
    h->fn = Handle::retained(fn);
    JSValue n = getProperty(vm, self, vm.atoms.next);
    if (n.isException())
        return n;
    h->next = Handle::adopt(n);
    return newHelperObject(vm, h.release(), vm.protos.iteratorHelper);
}

NATIVE(iteratorMap)
{
    UNUSED();
    const auto which = reinterpret_cast<std::intptr_t>(data);  // 0 map, 1 filter, 4 flatMap
    static const char* const names[] = {"map", "filter", "take", "drop", "flatMap"};
    if (!isObject(self))
        return typeError(vm, std::string("Iterator.prototype.") + names[which] + " called on non-object");
    const JSValue fn = arg(args, argc, 0);
    if (!isFunction(fn)) {
        closeIterator(vm, self, true);
        vm.throwError(ErrorCode::TypeError, describe(vm, fn) + " is not a function");
        return JSValue::exception();
    }
    return makeHelper(vm, self, which == 0 ? Kind::Map : which == 1 ? Kind::Filter : Kind::FlatMap, fn, 0, names[which]);
}

/// take (data 2) and drop (3).
NATIVE(iteratorTake)
{
    UNUSED();
    const bool take = reinterpret_cast<std::intptr_t>(data) == 2;
    if (!isObject(self))
        return typeError(vm, std::string("Iterator.prototype.") + (take ? "take" : "drop") + " called on non-object");
    double limit;
    if (!toNumber(vm, arg(args, argc, 0), &limit)) {
        closeIterator(vm, self, true);
        return JSValue::exception();
    }
    if (limit != limit) {
        closeIterator(vm, self, true);
        return vm.throwError(ErrorCode::RangeError, "limit must be a number");
    }
    limit = std::trunc(limit);
    if (limit < 0) {
        closeIterator(vm, self, true);
        return vm.throwError(ErrorCode::RangeError, "limit must be non-negative");
    }
    return makeHelper(vm, self, take ? Kind::Take : Kind::Drop, JSValue::undefined(), limit, take ? "take" : "drop");
}

/// reduce (0), toArray (1), forEach (2), some (3), every (4), find (5).
NATIVE(iteratorConsume)
{
    UNUSED();
    static const char* const names[] = {"reduce", "toArray", "forEach", "some", "every", "find"};
    const int which = int(reinterpret_cast<std::intptr_t>(data));
    if (!isObject(self))
        return typeError(vm, std::string("Iterator.prototype.") + names[which] + " called on non-object");
    JSValue fn = arg(args, argc, 0);
    if (which != 1 && !isFunction(fn)) {
        closeIterator(vm, self, true);
        vm.throwError(ErrorCode::TypeError, describe(vm, fn) + " is not a function");
        return JSValue::exception();
    }
    Handle iterator, next;
    if (!iteratorDirect(vm, self, &iterator, &next))
        return JSValue::exception();
    JSArray* list = which == 1 ? vm.newArray(vm.protos.array) : nullptr;
    Handle listKeep = list ? Handle::adopt(toJSValue(list)) : Handle();
    Handle acc;
    double counter = 0;
    bool haveAcc = false;
    if (which == 0) {
        if (argc >= 2) {
            acc = Handle::retained(args[1]);
            haveAcc = true;
        }
    }
    for (;;) {
        JSValue v;
        if (!isFunction(next.get())) {
            vm.throwError(ErrorCode::TypeError, "iterator.next is not a function");
            return JSValue::exception();
        }
        const int s = iteratorStep(vm, iterator.get(), next.get(), &v);
        if (s < 0)
            return JSValue::exception();
        if (s == 0)
            break;
        Handle vk = Handle::adopt(v);
        if (which == 1) {
            retain(v);
            list->append(vm, v);
            continue;
        }
        if (which == 0 && !haveAcc) {
            acc = Handle::retained(v);
            haveAcc = true;
            counter = 1;
            continue;
        }
        JSValue r;
        if (which == 0) {
            JSValue callArgs[3] = {acc.get(), v, JSValue::boxNumber(counter++)};
            r = vm.call(fn, JSValue::undefined(), callArgs, 3);
        } else {
            JSValue callArgs[2] = {v, JSValue::boxNumber(counter++)};
            r = vm.call(fn, JSValue::undefined(), callArgs, 2);
        }
        if (r.isException()) {
            closeIterator(vm, iterator.get(), true);
            return r;
        }
        Handle rk = Handle::adopt(r);
        switch (which) {
            case 0: acc = Handle::retained(r); break;
            case 3:
                if (toBoolean(r)) {
                    if (!closeIterator(vm, iterator.get(), false))
                        return JSValue::exception();
                    return JSValue::boolean(true);
                }
                break;
            case 4:
                if (!toBoolean(r)) {
                    if (!closeIterator(vm, iterator.get(), false))
                        return JSValue::exception();
                    return JSValue::boolean(false);
                }
                break;
            case 5:
                if (toBoolean(r)) {
                    if (!closeIterator(vm, iterator.get(), false))
                        return JSValue::exception();
                    retain(v);
                    return v;
                }
                break;
            default: break;
        }
    }
    switch (which) {
        case 0:
            if (!haveAcc)
                return typeError(vm, "Reduce of empty iterator with no initial value");
            retain(acc.get());
            return acc.get();
        case 1: return listKeep.take();
        case 3: return JSValue::boolean(false);
        case 4: return JSValue::boolean(true);
        default: return JSValue::undefined();
    }
}

// ------------------------------------------------------------- the global ---

NATIVE(iteratorCtor)
{
    UNUSED();
    if (vm.newTarget.isUndefined() || !isObject(self))
        return typeError(vm, "Constructor Iterator requires 'new'");
    JSValue p = getProperty(vm, vm.newTarget, vm.atoms.prototype);
    if (p.isException())
        return p;
    const bool direct = isObject(p) && asObject(p) == vm.protos.iterator;
    release(p);
    if (direct)
        return typeError(vm, "Abstract class Iterator not directly constructable");
    retain(self);
    return self;
}

NATIVE(wrapNext)
{
    UNUSED();
    Helper* h = helperOf(self);
    if (!h || h->kind != Kind::Wrap)
        return typeError(vm, "next method called on an incompatible receiver");
    return vm.call(h->next.get(), h->iterator.get(), nullptr, 0);
}

NATIVE(wrapReturn)
{
    UNUSED();
    Helper* h = helperOf(self);
    if (!h || h->kind != Kind::Wrap)
        return typeError(vm, "return method called on an incompatible receiver");
    JSValue ret = getProperty(vm, h->iterator.get(), vm.atoms.return_);
    if (ret.isException())
        return ret;
    Handle retKeep = Handle::adopt(ret);
    if (ret.isNullish())
        return iterResult(vm, JSValue::undefined(), true);
    return vm.call(ret, h->iterator.get(), nullptr, 0);
}

NATIVE(iteratorFrom)
{
    UNUSED();
    Handle iterator, next;
    if (!iteratorFlattenable(vm, arg(args, argc, 0), true, &iterator, &next))
        return JSValue::exception();
    // One that already inherits from Iterator.prototype is used as it is.
    for (JSObject* p = asObject(iterator.get())->proto(vm); p; p = p->proto(vm))
        if (p == vm.protos.iterator) {
            retain(iterator.get());
            return iterator.get();
        }
    auto h = std::make_unique<Helper>();
    h->kind = Kind::Wrap;
    h->iterator = iterator;
    h->next = next;
    return newHelperObject(vm, h.release(), vm.protos.wrapForValidIterator);
}

NATIVE(iteratorConcat)
{
    UNUSED();
    auto h = std::make_unique<Helper>();
    h->kind = Kind::Concat;
    for (std::uint32_t i = 0; i < argc; ++i) {
        if (!isObject(args[i]))
            return typeError(vm, describe(vm, args[i]) + " is not an object");
        JSValue method = getProperty(vm, args[i], vm.atoms.symIterator);
        if (method.isException())
            return method;
        Handle mk = Handle::adopt(method);
        if (!isFunction(method))
            return typeError(vm, "Symbol.iterator is not a function");
        h->iterables.push_back(Handle::retained(args[i]));
        h->methods.push_back(Handle::retained(method));
    }
    return newHelperObject(vm, h.release(), vm.protos.iteratorHelper);
}

/// The accessors of Iterator.prototype.constructor and @@toStringTag: reading gives the value, writing makes (or changes) the
/// property on the receiver unless it is the prototype itself.
NATIVE(weirdAccessor)
{
    UNUSED();
    const auto which = reinterpret_cast<std::intptr_t>(data);  // 0 get constructor, 1 set constructor, 2 get tag, 3 set tag
    if (which == 0 || which == 2) {
        if (which == 2)
            return makeString(vm, std::string_view("Iterator"));
        Key k = vm.internKey(std::string_view("Iterator"));
        JSValue v = getProperty(vm, toJSValue(vm.global), k);
        detail::decRef(k);
        return v;
    }
    if (!isObject(self))
        return typeError(vm, "Iterator.prototype setter called on non-object");
    if (asObject(self) == vm.protos.iterator)
        return typeError(vm, "Cannot assign to read only property of Iterator.prototype");
    Key key = which == 1 ? vm.atoms.constructor : vm.atoms.symToStringTag;
    const JSValue value = arg(args, argc, 0);
    std::uint8_t attrs;
    if (!asObject(self)->findOwn(vm, key, &attrs)) {
        retain(value);
        if (!defineOwn(vm, asObject(self), key, value, attr::kDefault))
            return typeError(vm, "Cannot define property");
    } else if (!setProperty(vm, self, key, value, true)) {
        return JSValue::exception();
    }
    return JSValue::undefined();
}

}  // namespace

bool iteratorClose(VM& vm, JSValue iterator, bool throwing)
{
    return closeIterator(vm, iterator, throwing);
}

void installIteratorHelpers(VM& vm)
{
    JSObject* ip = vm.protos.iterator;
    JSFunction* ctor = constructor(vm, "Iterator", iteratorCtor, 0, ip);
    // `constructor` and @@toStringTag of %Iterator.prototype% are accessors, not data.
    ip->deleteOwn(vm, vm.atoms.constructor);
    auto accessor = [&](Key key, const char* name, std::intptr_t getterCode) {
        Key gname = vm.internKey(std::string_view(std::string("get ") + name));
        Key sname = vm.internKey(std::string_view(std::string("set ") + name));
        JSFunction* g = vm.newNativeFunction(vm.protos.function, weirdAccessor, reinterpret_cast<void*>(getterCode), gname, 0);
        Handle gk = Handle::adopt(toJSValue(g));
        JSFunction* s = vm.newNativeFunction(vm.protos.function, weirdAccessor, reinterpret_cast<void*>(getterCode + 1), sname, 1);
        Handle sk = Handle::adopt(toJSValue(s));
        defineAccessor(vm, ip, key, toJSValue(g), toJSValue(s), attr::kConfigurable);
        detail::decRef(gname);
        detail::decRef(sname);
    };
    accessor(vm.atoms.constructor, "constructor", 0);
    accessor(vm.atoms.symToStringTag, "[Symbol.toStringTag]", 2);
    method(vm, ctor, "from", iteratorFrom, 1);
    method(vm, ctor, "concat", iteratorConcat, 0);

    method(vm, ip, "map", iteratorMap, 1, reinterpret_cast<void*>(0));
    method(vm, ip, "filter", iteratorMap, 1, reinterpret_cast<void*>(1));
    method(vm, ip, "take", iteratorTake, 1, reinterpret_cast<void*>(2));
    method(vm, ip, "drop", iteratorTake, 1, reinterpret_cast<void*>(3));
    method(vm, ip, "flatMap", iteratorMap, 1, reinterpret_cast<void*>(4));
    method(vm, ip, "reduce", iteratorConsume, 1, reinterpret_cast<void*>(0));
    method(vm, ip, "toArray", iteratorConsume, 0, reinterpret_cast<void*>(1));
    method(vm, ip, "forEach", iteratorConsume, 1, reinterpret_cast<void*>(2));
    method(vm, ip, "some", iteratorConsume, 1, reinterpret_cast<void*>(3));
    method(vm, ip, "every", iteratorConsume, 1, reinterpret_cast<void*>(4));
    method(vm, ip, "find", iteratorConsume, 1, reinterpret_cast<void*>(5));

    JSObject* helperProto = vm.newObject(ip);
    vm.setIntrinsic(&vm.protos.iteratorHelper, helperProto);
    release(toJSValue(helperProto));
    method(vm, helperProto, "next", helperNext, 0);
    method(vm, helperProto, "return", helperReturn, 0);
    helperProto->putOwn(vm, vm.atoms.symToStringTag, makeString(vm, std::string_view("Iterator Helper")), attr::kConfigurable);

    JSObject* wrapProto = vm.newObject(ip);
    vm.setIntrinsic(&vm.protos.wrapForValidIterator, wrapProto);
    release(toJSValue(wrapProto));
    method(vm, wrapProto, "next", wrapNext, 0);
    method(vm, wrapProto, "return", wrapReturn, 0);
}

}  // namespace bi
}  // namespace vm
}  // namespace internal
}  // namespace script
