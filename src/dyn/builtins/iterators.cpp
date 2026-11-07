// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Iterator: the global, the helpers on its prototype, Iterator.from and Iterator.concat.

#include <cmath>

#include "script/internal/dyn/builtins.hpp"
#include "script/internal/dyn/vm.hpp"

namespace script {
namespace internal {
namespace dyn {

struct IteratorStateHolder
{
    Ref<DynObject> helperProto, wrapProto;
};

namespace {

enum class HelperKind : std::uint8_t
{
    Map,
    Filter,
    Take,
    Drop,
    FlatMap,
    Wrap,    ///< Iterator.from of something that is no Iterator: passes calls on
    Concat,
};

/// What a helper keeps between calls of `next`.
struct DynIteratorHelper : DynObject
{
    Value iterator = Value::undefined();  ///< the underlying one
    Value nextMethod = Value::undefined();
    Value fn = Value::undefined();
    Value inner = Value::undefined();      ///< flatMap's current inner iterator; concat's list of iterables
    Value innerNext = Value::undefined();
    double counter = 0;
    double limit = 0;
    std::size_t position = 0;  ///< concat: which iterable is next
    HelperKind kind = HelperKind::Map;
    bool started = false;
    bool done = false;
    bool running = false;

    void children(GcList& out) const override
    {
        DynObject::children(out);
        for (const Value* v : {&iterator, &nextMethod, &fn, &inner, &innerNext}) {
            if (GCObject* p = detail::Access::ptr(*v))
                out.push_back(p);
        }
    }
    void clearReferences() override
    {
        DynObject::clearReferences();
        iterator = nextMethod = fn = inner = innerNext = Value::undefined();
    }
};

Value objectValue(DynObject* o) { return detail::Access::make(Type::Object, o); }

/// GetIteratorDirect: `this`, and its `next` read once.
void direct(CallInfo& ci, Value& iterator, Value& next)
{
    if (!ci.thisValue().isObject())
        ci.fail(ErrorCode::TypeError, "an iterator helper needs an object for this");
    iterator = ci.thisValue();
    next = getProperty(rtOf(ci), iterator, rtOf(ci).atom.next.get());
}

/// IteratorStep with a `next` already read: false when done.
bool step(Runtime& rt, const Value& iterator, const Value& next, Value& out)
{
    const Value result = callValue(rt, next, iterator, {});
    if (!result.isObject())
        throwError(rt, ErrorCode::TypeError, "the iterator result is not an object");
    if (toBoolean(getProperty(rt, result, rt.atom.done.get())))
        return false;
    out = getProperty(rt, result, rt.atom.value.get());
    return true;
}

/// IteratorClose after an error: the error wins over whatever closing does.
[[noreturn]] void closeAndRethrow(Runtime& rt, const Value& iterator)
{
    std::exception_ptr pending = std::current_exception();
    iteratorClose(rt, iterator);
    std::rethrow_exception(pending);
}

/// IteratorClose on the way out normally: what `return` throws is thrown.
void closeNormally(Runtime& rt, const Value& iterator)
{
    const Value fn = getProperty(rt, iterator, rt.intern("return"));
    if (fn.isNullish())
        return;
    const Value result = callValue(rt, fn, iterator, {});
    if (!result.isObject())
        throwError(rt, ErrorCode::TypeError, "the iterator's return gave no object");
}

DynIteratorHelper* thisHelper(CallInfo& ci, bool wrap)
{
    DynObject* o = asObject(ci.thisValue());
    if (!o || o->cls != ObjClass::IteratorHelper)
        ci.fail(ErrorCode::TypeError, "not an iterator helper");
    auto* h = static_cast<DynIteratorHelper*>(o);
    if ((h->kind == HelperKind::Wrap) != wrap)
        ci.fail(ErrorCode::TypeError, "not this kind of iterator");
    return h;
}

/// GetIteratorFlattenable: an object's iterator, or the object itself when it has no Symbol.iterator.
void flattenable(Runtime& rt, const Value& v, bool strings, Value& iterator, Value& next)
{
    if (!v.isObject() && !(strings && v.isString()))
        throwError(rt, ErrorCode::TypeError, "an iterator or iterable object is needed");
    const Value method = getProperty(rt, v, rt.sym.iterator.get());
    if (method.isNullish()) {
        iterator = v;
    } else {
        if (!asFunction(method))
            throwError(rt, ErrorCode::TypeError, "Symbol.iterator is not a function");
        iterator = callValue(rt, method, v, {});
    }
    if (!iterator.isObject())
        throwError(rt, ErrorCode::TypeError, "the iterator is not an object");
    next = getProperty(rt, iterator, rt.atom.next.get());
}

/// The next value into `out`; false at the end.
bool helperNext(Runtime& rt, DynIteratorHelper* h, Value& out)
{
    for (;;) {
        Value item;
        switch (h->kind) {
            case HelperKind::Map: {
                if (!step(rt, h->iterator, h->nextMethod, item))
                    return false;
                const double index = h->counter++;
                try {
                    out = callValue(rt, h->fn, Value::undefined(), {item, Value(index)});
                    return true;
                } catch (...) {
                    closeAndRethrow(rt, h->iterator);
                }
            }
            case HelperKind::Filter: {
                if (!step(rt, h->iterator, h->nextMethod, item))
                    return false;
                const double index = h->counter++;
                bool keep;
                try {
                    keep = toBoolean(callValue(rt, h->fn, Value::undefined(), {item, Value(index)}));
                } catch (...) {
                    closeAndRethrow(rt, h->iterator);
                }
                if (keep) {
                    out = item;
                    return true;
                }
                continue;
            }
            case HelperKind::Take: {
                if (h->limit <= 0) {
                    closeNormally(rt, h->iterator);
                    return false;
                }
                if (h->limit != INFINITY)
                    --h->limit;
                if (!step(rt, h->iterator, h->nextMethod, item))
                    return false;
                out = item;
                return true;
            }
            case HelperKind::Drop: {
                while (h->limit > 0) {
                    if (h->limit != INFINITY)
                        --h->limit;
                    if (!step(rt, h->iterator, h->nextMethod, item))
                        return false;
                }
                if (!step(rt, h->iterator, h->nextMethod, item))
                    return false;
                out = item;
                return true;
            }
            case HelperKind::FlatMap: {
                if (!h->inner.isUndefined()) {
                    bool more;
                    try {
                        more = step(rt, h->inner, h->innerNext, item);
                    } catch (...) {
                        closeAndRethrow(rt, h->iterator);
                    }
                    if (more) {
                        out = item;
                        return true;
                    }
                    h->inner = Value::undefined();
                    continue;
                }
                if (!step(rt, h->iterator, h->nextMethod, item))
                    return false;
                const double index = h->counter++;
                try {
                    const Value mapped = callValue(rt, h->fn, Value::undefined(), {item, Value(index)});
                    flattenable(rt, mapped, false, h->inner, h->innerNext);
                } catch (...) {
                    closeAndRethrow(rt, h->iterator);
                }
                continue;
            }
            case HelperKind::Concat: {
                if (!h->iterator.isUndefined()) {
                    if (step(rt, h->iterator, h->nextMethod, item)) {
                        out = item;
                        return true;
                    }
                    h->iterator = Value::undefined();
                    continue;
                }
                const double count = lengthOfLike(rt, h->inner);
                if (double(h->position) >= count)
                    return false;
                const Value iterable = getIndex(rt, h->inner, std::uint32_t(h->position));
                ++h->position;
                const Value method = getProperty(rt, iterable, rt.sym.iterator.get());
                h->iterator = callValue(rt, method, iterable, {});
                if (!h->iterator.isObject())
                    throwError(rt, ErrorCode::TypeError, "the iterator is not an object");
                h->nextMethod = getProperty(rt, h->iterator, rt.atom.next.get());
                continue;
            }
            case HelperKind::Wrap: break;
        }
        return false;
    }
}

Value makeHelper(Runtime& rt, HelperKind kind, const Value& iterator, const Value& next, const Value& fn)
{
    auto* h = rt.newCell<DynIteratorHelper>();
    h->cls = ObjClass::IteratorHelper;
    h->setProto(kind == HelperKind::Wrap ? rt.iterators->wrapProto.get() : rt.iterators->helperProto.get());
    h->kind = kind;
    h->iterator = iterator;
    h->nextMethod = next;
    h->fn = fn;
    return detail::Access::adopt(Type::Object, h);
}

/// What `take` and `drop` take: a number that is not NaN or negative.
double limitOf(Runtime& rt, const Value& iterator, const Value& raw)
{
    double n;
    try {
        n = toNumber(rt, raw);
    } catch (...) {
        closeAndRethrow(rt, iterator);
    }
    if (std::isnan(n)) {
        iteratorClose(rt, iterator);
        throwError(rt, ErrorCode::RangeError, "the limit is not a number");
    }
    const double whole = std::isinf(n) ? n : std::trunc(n);
    if (whole < 0) {
        iteratorClose(rt, iterator);
        throwError(rt, ErrorCode::RangeError, "the limit is negative");
    }
    return whole;
}

}  // namespace

void installIteratorHelpers(Runtime& rt)
{
    rt.iterators = std::make_shared<IteratorStateHolder>();
    DynObject* proto = rt.iteratorProto.get();

    Value ctorValue = rt.makeFunction(ascii("Iterator"), [](CallInfo& ci) -> Value {
        Runtime& rt2 = rtOf(ci);
        if (!ci.isConstructorCall())
            ci.fail(ErrorCode::TypeError, "Iterator must be called with 'new'");
        // Abstract: only a subclass makes one.
        const Value made = ci.thisValue();
        if (DynObject* o = asObject(made); !o || o->proto.get() == rt2.iteratorProto.get())
            ci.fail(ErrorCode::TypeError, "Iterator is abstract: construct a subclass of it");
        return Value::undefined();
    }, 0);
    DynFunction* ctor = asFunction(ctorValue);
    ctor->isNativeConstructor = true;
    ctor->putOwn(rt.atom.prototype.get(), objectValue(proto), 0);
    rt.globalObject->putOwn(rt.intern("Iterator"), ctorValue, PropWritable | PropConfigurable);

    // `constructor` and Symbol.toStringTag are accessors that write an own property of whatever is not the prototype.
    const auto quirkyAccessor = [&](GCObject* key, Value shown) {
        Value getter = rt.makeFunction(ascii("get"), [shown](CallInfo&) { return shown; }, 0);
        Ref<GCObject> keyRef(key);
        Value setter = rt.makeFunction(ascii("set"), [keyRef](CallInfo& ci) -> Value {
            Runtime& rt2 = rtOf(ci);
            DynObject* self = asObject(ci.thisValue());
            if (!self)
                ci.fail(ErrorCode::TypeError, "the receiver is not an object");
            if (self == rt2.iteratorProto.get())
                ci.fail(ErrorCode::TypeError, "the Iterator prototype's own property cannot be set this way");
            if (self->findOwn(keyRef.get()))
                setProperty(rt2, ci.thisValue(), keyRef.get(), ci.arg(0));
            else
                self->putOwn(keyRef.get(), ci.arg(0));
            return Value::undefined();
        }, 1);
        Prop& p = proto->putOwn(key, getter, PropConfigurable | PropAccessor);
        proto->setSetter(p, setter);
    };
    quirkyAccessor(rt.sym.toStringTag.get(), rt.str("Iterator"));
    quirkyAccessor(rt.atom.constructor.get(), ctorValue);

    const auto lazy = [&](const char* name, HelperKind kind) {
        rt.method(proto, name, [kind](CallInfo& ci) -> Value {
            Runtime& rt2 = rtOf(ci);
            if (!ci.thisValue().isObject())
                ci.fail(ErrorCode::TypeError, "an iterator helper needs an object for this");
            const Value iterator = ci.thisValue();
            if (kind == HelperKind::Take || kind == HelperKind::Drop) {
                const double limit = limitOf(rt2, iterator, ci.arg(0));
                Value next;
                direct(ci, const_cast<Value&>(iterator), next);
                Value helper = makeHelper(rt2, kind, iterator, next, Value::undefined());
                static_cast<DynIteratorHelper*>(asObject(helper))->limit = limit;
                return helper;
            }
            if (!asFunction(ci.arg(0))) {
                iteratorClose(rt2, iterator);
                ci.fail(ErrorCode::TypeError, "the iterator helper needs a function");
            }
            Value it, next;
            direct(ci, it, next);
            return makeHelper(rt2, kind, it, next, ci.arg(0));
        }, 1);
    };
    lazy("map", HelperKind::Map);
    lazy("filter", HelperKind::Filter);
    lazy("take", HelperKind::Take);
    lazy("drop", HelperKind::Drop);
    lazy("flatMap", HelperKind::FlatMap);

    // The eager ones.
    const auto needFunction = [](CallInfo& ci) {
        if (!ci.thisValue().isObject())
            ci.fail(ErrorCode::TypeError, "an iterator method needs an object for this");
        if (!asFunction(ci.arg(0))) {
            iteratorClose(rtOf(ci), ci.thisValue());
            ci.fail(ErrorCode::TypeError, "the iterator method needs a function");
        }
    };
    rt.method(proto, "reduce", [](CallInfo& ci) -> Value {
        Runtime& rt2 = rtOf(ci);
        if (!ci.thisValue().isObject())
            ci.fail(ErrorCode::TypeError, "an iterator method needs an object for this");
        if (!asFunction(ci.arg(0))) {
            iteratorClose(rt2, ci.thisValue());
            ci.fail(ErrorCode::TypeError, "reduce needs a function");
        }
        Value it, next;
        direct(ci, it, next);
        Value acc;
        double counter = 0;
        if (ci.size() < 2) {
            if (!step(rt2, it, next, acc))
                ci.fail(ErrorCode::TypeError, "reduce of an empty iterator with no initial value");
            counter = 1;
        } else {
            acc = ci.arg(1);
        }
        Value item;
        while (step(rt2, it, next, item)) {
            try {
                acc = callValue(rt2, ci.arg(0), Value::undefined(), {acc, item, Value(counter++)});
            } catch (...) {
                closeAndRethrow(rt2, it);
            }
        }
        return acc;
    }, 1);
    rt.method(proto, "toArray", [](CallInfo& ci) -> Value {
        Runtime& rt2 = rtOf(ci);
        Value it, next;
        direct(ci, it, next);
        Vec<Value> items;
        Value item;
        while (step(rt2, it, next, item)) {
            checkLongLoop(rt2, items.size(), items.size() * sizeof(Value));
            items.push_back(item);
        }
        return rt2.makeArray(std::move(items));
    }, 0);
    // forEach, some, every, find: 0, 1, 2, 3.
    const auto consume = [needFunction](int mode) {
        return [needFunction, mode](CallInfo& ci) -> Value {
            Runtime& rt2 = rtOf(ci);
            needFunction(ci);
            Value it, next;
            direct(ci, it, next);
            Value item;
            double counter = 0;
            while (step(rt2, it, next, item)) {
                Value answer;
                try {
                    answer = callValue(rt2, ci.arg(0), Value::undefined(), {item, Value(counter++)});
                } catch (...) {
                    closeAndRethrow(rt2, it);
                }
                const bool truthy = toBoolean(answer);
                if ((mode == 1 || mode == 3) && truthy) {
                    closeNormally(rt2, it);
                    return mode == 1 ? Value(true) : item;
                }
                if (mode == 2 && !truthy) {
                    closeNormally(rt2, it);
                    return Value(false);
                }
            }
            return mode == 0 ? Value::undefined() : mode == 1 ? Value(false) : mode == 2 ? Value(true) : Value::undefined();
        };
    };
    rt.method(proto, "forEach", consume(0), 1);
    rt.method(proto, "some", consume(1), 1);
    rt.method(proto, "every", consume(2), 1);
    rt.method(proto, "find", consume(3), 1);

    // %IteratorHelperPrototype%
    DynObject* helperProto = rt.newObject(proto);
    rt.iterators->helperProto.reset(helperProto, true);
    helperProto->putOwn(rt.sym.toStringTag.get(), rt.str("Iterator Helper"), PropConfigurable);
    rt.method(helperProto, "next", [](CallInfo& ci) -> Value {
        Runtime& rt2 = rtOf(ci);
        DynIteratorHelper* h = thisHelper(ci, false);
        if (h->running)
            ci.fail(ErrorCode::TypeError, "the iterator helper is already running");
        if (h->done)
            return iterResult(rt2, Value::undefined(), true);
        const Value keep = ci.thisValue();
        h->running = true;
        h->started = true;
        Value value;
        bool more;
        try {
            more = helperNext(rt2, h, value);
        } catch (...) {
            h->running = false;
            h->done = true;
            throw;
        }
        h->running = false;
        if (!more) {
            h->done = true;
            return iterResult(rt2, Value::undefined(), true);
        }
        return iterResult(rt2, value, false);
    }, 0);
    rt.method(helperProto, "return", [](CallInfo& ci) -> Value {
        Runtime& rt2 = rtOf(ci);
        DynIteratorHelper* h = thisHelper(ci, false);
        if (h->running)
            ci.fail(ErrorCode::TypeError, "the iterator helper is running");
        const Value keep = ci.thisValue();
        if (!h->done) {
            h->done = true;
            if (h->kind == HelperKind::FlatMap && !h->inner.isUndefined()) {
                try {
                    closeNormally(rt2, h->inner);
                } catch (...) {
                    closeAndRethrow(rt2, h->iterator);
                }
            }
            if (!h->iterator.isUndefined())
                closeNormally(rt2, h->iterator);
        }
        return iterResult(rt2, Value::undefined(), true);
    }, 0);

    // %WrapForValidIteratorPrototype%
    DynObject* wrapProto = rt.newObject(proto);
    rt.iterators->wrapProto.reset(wrapProto, true);
    rt.method(wrapProto, "next", [](CallInfo& ci) -> Value {
        DynIteratorHelper* h = thisHelper(ci, true);
        return callValue(rtOf(ci), h->nextMethod, h->iterator, {});
    }, 0);
    rt.method(wrapProto, "return", [](CallInfo& ci) -> Value {
        Runtime& rt2 = rtOf(ci);
        DynIteratorHelper* h = thisHelper(ci, true);
        const Value fn = getProperty(rt2, h->iterator, rt2.intern("return"));
        if (fn.isNullish())
            return iterResult(rt2, Value::undefined(), true);
        return callValue(rt2, fn, h->iterator, {});
    }, 0);

    rt.method(ctor, "from", [](CallInfo& ci) -> Value {
        Runtime& rt2 = rtOf(ci);
        Value it, next;
        flattenable(rt2, ci.arg(0), true, it, next);
        // An Iterator already: as it is.
        for (DynObject* up = asObject(it)->proto.get(); up; up = up->proto.get()) {
            if (up == rt2.iteratorProto.get())
                return it;
        }
        return makeHelper(rt2, HelperKind::Wrap, it, next, Value::undefined());
    }, 1);
    rt.method(ctor, "concat", [](CallInfo& ci) -> Value {
        Runtime& rt2 = rtOf(ci);
        for (const Value& item : ci.args()) {
            if (!item.isObject())
                ci.fail(ErrorCode::TypeError, "Iterator.concat takes iterable objects");
            if (!asFunction(getProperty(rt2, item, rt2.sym.iterator.get())))
                ci.fail(ErrorCode::TypeError, "Iterator.concat takes iterable objects");
        }
        Value helper = makeHelper(rt2, HelperKind::Concat, Value::undefined(), Value::undefined(), Value::undefined());
        static_cast<DynIteratorHelper*>(asObject(helper))->inner = rt2.makeArray(ci.args());
        return helper;
    }, 0);
}

}  // namespace dyn
}  // namespace internal
}  // namespace script
