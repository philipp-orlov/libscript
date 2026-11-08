// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Map, Set, Promise, console, and the value renderer they share.

#include <algorithm>
#include <cmath>

#include "script/internal/dyn/bigint.hpp"
#include "script/internal/dyn/builtins.hpp"
#include "script/internal/dyn/numconv.hpp"
#include "script/internal/dyn/vm.hpp"

namespace script {
namespace internal {
namespace dyn {

namespace {

Value objectValue(DynObject* o) { return detail::Access::make(Type::Object, o); }

[[noreturn]] void throwValueOf(Runtime& rt, const Value& v) { throwValue(rt, v); }

/// A promise, and the two functions that settle it.
struct Capability
{
    Value promise;
    Value resolve;
    Value reject;
};

/// NewPromiseCapability: for Promise itself, a promise made directly; for anything else, what its constructor makes.
Capability newCapability(Runtime& rt, const Value& ctor)
{
    Capability cap;
    if (asObject(ctor) == rt.promiseCtor.get()) {
        cap.promise = makePromise(rt);
        cap.resolve = makeSettler(rt, cap.promise, false);
        cap.reject = makeSettler(rt, cap.promise, true);
        return cap;
    }
    DynFunction* fn = asFunction(ctor);
    if (!fn || !isConstructor(rt, fn))
        throwError(rt, ErrorCode::TypeError, "a promise capability needs a constructor");
    auto slots = std::make_shared<std::pair<Value, Value>>(Value::undefined(), Value::undefined());
    const Value executor = rt.makeFunction(String(), [slots](CallInfo& inner) -> Value {
        if (!slots->first.isUndefined() || !slots->second.isUndefined())
            inner.fail(ErrorCode::TypeError, "the promise capability's functions are given once");
        slots->first = inner.arg(0);
        slots->second = inner.arg(1);
        return Value::undefined();
    }, 2);
    cap.promise = constructValue(rt, ctor, {executor});
    if (!asFunction(slots->first) || !asFunction(slots->second))
        throwError(rt, ErrorCode::TypeError, "the promise constructor gave no functions to settle it");
    cap.resolve = slots->first;
    cap.reject = slots->second;
    return cap;
}

/// PromiseResolve.
Value promiseResolve(Runtime& rt, const Value& ctor, const Value& x)
{
    if (DynObject* o = asObject(x); o && o->cls == ObjClass::Promise) {
        const Value made = getProperty(rt, x, rt.atom.constructor.get());
        if (sameValue(made, ctor))
            return x;
    }
    const Capability cap = newCapability(rt, ctor);
    callValue(rt, cap.resolve, Value::undefined(), {x});
    return cap.promise;
}

enum class Combine
{
    All,
    AllSettled,
    Any,
    Race,
};

/// Promise.all, allSettled, any and race: one iteration, each element resolved through the constructor's own resolve.
Value combinator(CallInfo& ci, Combine kind)
{
    Runtime& rt = rtOf(ci);
    const Value ctor = ci.thisValue();
    const Capability cap = newCapability(rt, ctor);
    const auto rejectWith = [&](const Value& reason) {
        callValue(rt, cap.reject, Value::undefined(), {reason});
        return cap.promise;
    };
    Value resolveFn;
    Value iterator;
    try {
        resolveFn = getProperty(rt, ctor, rt.intern("resolve"));
        if (!asFunction(resolveFn))
            throwError(rt, ErrorCode::TypeError, "the constructor's resolve is not a function");
        iterator = getIterator(rt, ci.arg(0));
    } catch (Exception& e) {
        return rejectWith(e.value());
    }
    auto values = std::make_shared<std::vector<Value>>();
    auto remaining = std::make_shared<std::size_t>(1);
    const auto finish = [cap, values, kind](Runtime& rtc) {
        if (kind == Combine::Any) {
            const Value error = constructValue(rtc, getProperty(rtc, objectValue(rtc.globalObject.get()), rtc.intern("AggregateError")),
                                               {rtc.makeArray(*values), rtc.str("all promises were rejected")});
            callValue(rtc, cap.reject, Value::undefined(), {error});
            return;
        }
        callValue(rtc, cap.resolve, Value::undefined(), {rtc.makeArray(*values)});
    };
    for (std::size_t index = 0;; ++index) {
        Value next;
        try {
            if (!iteratorStep(rt, iterator, next)) {
                if (kind != Combine::Race && --*remaining == 0)
                    finish(rt);
                return cap.promise;
            }
        } catch (Exception& e) {
            return rejectWith(e.value());
        }
        try {
            const Value nextPromise = callValue(rt, resolveFn, ctor, {next});
            if (kind == Combine::Race) {
                invokeMethod(rt, nextPromise, rt.atom.then.get(), {cap.resolve, cap.reject});
                continue;
            }
            values->push_back(Value::undefined());
            ++*remaining;
            // Each element's function counts once, however often it is called.
            const auto element = [values, remaining, index, kind, finish](int mode) {
                auto called = std::make_shared<bool>(false);
                return [values, remaining, index, kind, finish, mode, called](CallInfo& inner) -> Value {
                    if (*called)
                        return Value::undefined();
                    *called = true;
                    Runtime& rtc = rtOf(inner);
                    if (kind == Combine::AllSettled) {
                        DynObject* entry = rtc.newObject(rtc.objectProto.get());
                        Value entryValue = detail::Access::adopt(Type::Object, entry);
                        entry->putOwn(rtc.intern("status"), rtc.str(mode == 0 ? "fulfilled" : "rejected"));
                        entry->putOwn(mode == 0 ? rtc.atom.value.get() : rtc.intern("reason"), inner.arg(0));
                        (*values)[index] = entryValue;
                    } else {
                        (*values)[index] = inner.arg(0);
                    }
                    if (--*remaining == 0)
                        finish(rtc);
                    return Value::undefined();
                };
            };
            Value onFulfilled, onRejected;
            switch (kind) {
                case Combine::All:
                    onFulfilled = rt.makeFunction(String(), element(0), 1);
                    onRejected = cap.reject;
                    break;
                case Combine::AllSettled:
                    onFulfilled = rt.makeFunction(String(), element(0), 1);
                    onRejected = rt.makeFunction(String(), element(1), 1);
                    break;
                default:
                    onFulfilled = cap.resolve;
                    onRejected = rt.makeFunction(String(), element(1), 1);
                    break;
            }
            invokeMethod(rt, nextPromise, rt.atom.then.get(), {onFulfilled, onRejected});
        } catch (Exception& e) {
            iteratorClose(rt, iterator);
            return rejectWith(e.value());
        }
    }
}

DynMap* thisCollection(CallInfo& ci, ObjClass expected)
{
    DynObject* o = asObject(ci.thisValue());
    if (!o || o->cls != expected)
        ci.fail(ErrorCode::TypeError,
                expected == ObjClass::Map ? "not a Map" : "not a Set");
    return static_cast<DynMap*>(o);
}

DynMap* newCollection(Runtime& rt, ObjClass cls, DynObject* proto)
{
    auto* m = rt.newCell<DynMap>();
    m->cls = cls;
    m->setProto(proto);
    return m;
}

Vec<Value> collectionEntries(Runtime& rt, DynMap* m, int mode)
{
    Vec<Value> out;
    for (const DynMap::Entry& e : m->entries) {
        if (!e.live)
            continue;
        if (mode == 0)
            out.push_back(e.key);
        else if (mode == 1)
            out.push_back(m->cls == ObjClass::Set ? e.key : e.value);
        else
            out.push_back(rt.makeArray({e.key, m->cls == ObjClass::Set ? e.key : e.value}));
    }
    return out;
}

/// The methods Map and Set share verbatim.
void installCollectionCommon(Runtime& rt, DynObject* proto, ObjClass cls)
{
    rt.getter(proto, "size", [cls](CallInfo& ci) {
        return Value(double(thisCollection(ci, cls)->liveCount));
    });
    rt.method(proto, "has", [cls](CallInfo& ci) {
        return Value(thisCollection(ci, cls)->find(ci.arg(0)) >= 0);
    }, 1);
    rt.method(proto, "delete", [cls](CallInfo& ci) {
        return Value(thisCollection(ci, cls)->erase(ci.arg(0)));
    }, 1);
    rt.method(proto, "clear", [cls](CallInfo& ci) {
        thisCollection(ci, cls)->clear();
        return Value::undefined();
    }, 0);
}

void installMap(Runtime& rt)
{
    DynObject* proto = rt.newObject(rt.objectProto.get());
    rt.mapProto.reset(proto, true);
    installCollectionCommon(rt, proto, ObjClass::Map);

    rt.makeConstructor("Map", [](CallInfo& ci) -> Value {
        Runtime& rt2 = rtOf(ci);
        if (!ci.isConstructorCall())
            ci.fail(ErrorCode::TypeError, "Map must be called with 'new'");
        DynMap* m = newCollection(rt2, ObjClass::Map, rt2.mapProto.get());
        Value result = detail::Access::adopt(Type::Object, m);
        if (!ci.arg(0).isNullish()) {
            for (const Value& pair : iterableToVector(rt2, ci.arg(0)))
                m->insert(getIndex(rt2, pair, 0), getIndex(rt2, pair, 1));
        }
        return result;
    }, 0, proto);

    rt.method(proto, "get", [](CallInfo& ci) {
        DynMap* m = thisCollection(ci, ObjClass::Map);
        const std::int64_t at = m->find(ci.arg(0));
        return at < 0 ? Value::undefined() : m->entries[std::size_t(at)].value;
    }, 1);
    rt.method(proto, "set", [](CallInfo& ci) {
        thisCollection(ci, ObjClass::Map)->insert(ci.arg(0), ci.arg(1));
        return ci.thisValue();
    }, 2);
    rt.intrinsics[Runtime::MapGet].reset(detail::Access::ptr(proto->findOwn(rt.intern("get"))->value));
    rt.intrinsics[Runtime::MapSet].reset(detail::Access::ptr(proto->findOwn(rt.intern("set"))->value));
    rt.intrinsics[Runtime::MapHas].reset(detail::Access::ptr(proto->findOwn(rt.intern("has"))->value));
    rt.method(proto, "forEach", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        DynMap* m = thisCollection(ci, ObjClass::Map);
        for (const Value& pair : collectionEntries(rt2, m, 2)) {
            callValue(rt2, ci.arg(0), ci.arg(1),
                      {getIndex(rt2, pair, 1), getIndex(rt2, pair, 0), ci.thisValue()});
        }
        return Value::undefined();
    }, 1);
    rt.method(proto, "keys", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        return makeIterator(rt2, collectionEntries(rt2, thisCollection(ci, ObjClass::Map), 0));
    }, 0);
    rt.method(proto, "values", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        return makeIterator(rt2, collectionEntries(rt2, thisCollection(ci, ObjClass::Map), 1));
    }, 0);
    rt.method(proto, "entries", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        return makeIterator(rt2, collectionEntries(rt2, thisCollection(ci, ObjClass::Map), 2));
    }, 0);
    proto->putOwn(rt.sym.iterator.get(), proto->findOwn(rt.intern("entries"))->value,
                  PropWritable | PropConfigurable);
}

void installSet(Runtime& rt)
{
    DynObject* proto = rt.newObject(rt.objectProto.get());
    rt.setProto.reset(proto, true);
    installCollectionCommon(rt, proto, ObjClass::Set);

    rt.makeConstructor("Set", [](CallInfo& ci) -> Value {
        Runtime& rt2 = rtOf(ci);
        if (!ci.isConstructorCall())
            ci.fail(ErrorCode::TypeError, "Set must be called with 'new'");
        DynMap* s = newCollection(rt2, ObjClass::Set, rt2.setProto.get());
        Value result = detail::Access::adopt(Type::Object, s);
        if (!ci.arg(0).isNullish()) {
            for (const Value& item : iterableToVector(rt2, ci.arg(0)))
                s->insert(item, item);
        }
        return result;
    }, 0, proto);

    rt.method(proto, "add", [](CallInfo& ci) {
        thisCollection(ci, ObjClass::Set)->insert(ci.arg(0), ci.arg(0));
        return ci.thisValue();
    }, 1);
    rt.intrinsics[Runtime::SetAdd].reset(detail::Access::ptr(proto->findOwn(rt.intern("add"))->value));
    rt.intrinsics[Runtime::SetHas].reset(detail::Access::ptr(proto->findOwn(rt.intern("has"))->value));
    rt.method(proto, "forEach", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        DynMap* s = thisCollection(ci, ObjClass::Set);
        for (const Value& item : collectionEntries(rt2, s, 0))
            callValue(rt2, ci.arg(0), ci.arg(1), {item, item, ci.thisValue()});
        return Value::undefined();
    }, 1);
    rt.method(proto, "values", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        return makeIterator(rt2, collectionEntries(rt2, thisCollection(ci, ObjClass::Set), 0));
    }, 0);
    proto->putOwn(rt.intern("keys"), proto->findOwn(rt.intern("values"))->value,
                  PropWritable | PropConfigurable);
    rt.method(proto, "entries", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        return makeIterator(rt2, collectionEntries(rt2, thisCollection(ci, ObjClass::Set), 2));
    }, 0);
    proto->putOwn(rt.sym.iterator.get(), proto->findOwn(rt.intern("values"))->value,
                  PropWritable | PropConfigurable);
}

DynPromise* thisPromise(CallInfo& ci)
{
    DynObject* o = asObject(ci.thisValue());
    if (!o || o->cls != ObjClass::Promise)
        ci.fail(ErrorCode::TypeError, "not a Promise");
    return static_cast<DynPromise*>(o);
}

void installPromise(Runtime& rt)
{
    DynObject* proto = rt.newObject(rt.objectProto.get());
    rt.promiseProto.reset(proto, true);

    DynFunction* ctor = rt.makeConstructor("Promise", [](CallInfo& ci) -> Value {
        Runtime& rt2 = rtOf(ci);
        if (!asFunction(ci.arg(0)))
            ci.fail(ErrorCode::TypeError, "Promise needs an executor function");
        Value promise = makePromise(rt2);
        auto* p = static_cast<DynPromise*>(asObject(promise));
        try {
            callValue(rt2, ci.arg(0), Value::undefined(),
                      {makeSettler(rt2, promise, false), makeSettler(rt2, promise, true)});
        } catch (const Exception& e) {
            rejectPromise(rt2, p, e.value());
        }
        return promise;
    }, 1, proto);
    rt.promiseCtor.reset(ctor);

    rt.method(proto, "then", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        DynPromise* self = thisPromise(ci);
        const Value ctor = speciesConstructor(rt2, ci.thisValue(), objectValue(rt2.promiseCtor.get()));
        self->handled = true;
        DynPromise::Reaction reaction;
        reaction.onFulfilled = asFunction(ci.arg(0)) ? ci.arg(0) : Value::undefined();
        reaction.onRejected = asFunction(ci.arg(1)) ? ci.arg(1) : Value::undefined();
        if (asObject(ctor) == rt2.promiseCtor.get()) {
            Value chained = makePromise(rt2);
            reaction.next.reset(static_cast<DynPromise*>(asObject(chained)));
            addReaction(rt2, self, std::move(reaction));
            return chained;
        }
        const Capability cap = newCapability(rt2, ctor);
        reaction.capResolve = cap.resolve;
        reaction.capReject = cap.reject;
        addReaction(rt2, self, std::move(reaction));
        return cap.promise;
    }, 2);

    rt.method(proto, "catch", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        return invokeMethod(rt2, ci.thisValue(), rt2.atom.then.get(), {Value::undefined(), ci.arg(0)});
    }, 1);

    rt.method(proto, "finally", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        if (!ci.thisValue().isObject())
            ci.fail(ErrorCode::TypeError, "Promise.prototype.finally called on something not an object");
        const Value ctor = speciesConstructor(rt2, ci.thisValue(), objectValue(rt2.promiseCtor.get()));
        const Value onFinally = ci.arg(0);
        if (!asFunction(onFinally))
            return invokeMethod(rt2, ci.thisValue(), rt2.atom.then.get(), {onFinally, onFinally});
        const auto after = [ctor, onFinally](bool rejected) {
            return [ctor, onFinally, rejected](CallInfo& inner) -> Value {
                Runtime& rtc = rtOf(inner);
                const Value outcome = inner.arg(0);
                const Value result = callValue(rtc, onFinally, Value::undefined(), {});
                const Value waited = promiseResolve(rtc, ctor, result);
                const Value pass = rtc.makeFunction(String(), [outcome, rejected](CallInfo& last) -> Value {
                    if (rejected)
                        throwValueOf(rtOf(last), outcome);
                    return outcome;
                }, 0);
                return invokeMethod(rtc, waited, rtc.atom.then.get(), {pass});
            };
        };
        const Value thenFinally = rt2.makeFunction(String(), after(false), 1);
        const Value catchFinally = rt2.makeFunction(String(), after(true), 1);
        return invokeMethod(rt2, ci.thisValue(), rt2.atom.then.get(), {thenFinally, catchFinally});
    }, 1);

    rt.method(ctor, "resolve", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        if (!ci.thisValue().isObject())
            ci.fail(ErrorCode::TypeError, "Promise.resolve called on something not an object");
        return promiseResolve(rt2, ci.thisValue(), ci.arg(0));
    }, 1);

    rt.method(ctor, "reject", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        const Capability cap = newCapability(rt2, ci.thisValue());
        callValue(rt2, cap.reject, Value::undefined(), {ci.arg(0)});
        return cap.promise;
    }, 1);

    rt.method(ctor, "all", [](CallInfo& ci) { return combinator(ci, Combine::All); }, 1);
    rt.method(ctor, "allSettled", [](CallInfo& ci) { return combinator(ci, Combine::AllSettled); }, 1);
    rt.method(ctor, "any", [](CallInfo& ci) { return combinator(ci, Combine::Any); }, 1);
    rt.method(ctor, "race", [](CallInfo& ci) { return combinator(ci, Combine::Race); }, 1);
    rt.method(ctor, "withResolvers", [](CallInfo& ci) -> Value {
        Runtime& rt2 = rtOf(ci);
        const Capability cap = newCapability(rt2, ci.thisValue());
        DynObject* out = rt2.newObject(rt2.objectProto.get());
        Value result = detail::Access::adopt(Type::Object, out);
        out->putOwn(rt2.intern("promise"), cap.promise);
        out->putOwn(rt2.intern("resolve"), cap.resolve);
        out->putOwn(rt2.intern("reject"), cap.reject);
        return result;
    }, 0);
    rt.method(ctor, "try", [](CallInfo& ci) -> Value {
        Runtime& rt2 = rtOf(ci);
        if (!ci.thisValue().isObject())
            ci.fail(ErrorCode::TypeError, "Promise.try called on something not an object");
        const Capability cap = newCapability(rt2, ci.thisValue());
        try {
            std::vector<Value> rest(ci.args().begin() + (ci.size() ? 1 : 0), ci.args().end());
            const Value result = callValue(rt2, ci.arg(0), Value::undefined(), rest);
            callValue(rt2, cap.resolve, Value::undefined(), {result});
        } catch (Exception& e) {
            callValue(rt2, cap.reject, Value::undefined(), {e.value()});
        }
        return cap.promise;
    }, 1);
    ctor->putOwn(rt.sym.species.get(), rt.makeFunction(ascii("get [Symbol.species]"), [](CallInfo& c) { return c.thisValue(); }, 0), PropConfigurable | PropAccessor);
}

void installConsole(Runtime& rt)
{
    DynObject* console = rt.newObject(rt.objectProto.get());
    rt.constant(rt.globalObject.get(), "console", detail::Access::adopt(Type::Object, console));

    auto emit = [](CallInfo& ci, bool isError) {
        Runtime& rt2 = rtOf(ci);
        String line;
        for (std::size_t i = 0; i < ci.size(); ++i) {
            if (i)
                line.push_back(u' ');
            const Value& v = ci.args()[i];
            if (v.isString())
                line += asString(v)->text();
            else
                line += inspectValue(rt2, v, 0);
        }
        line.push_back(u'\n');
        rt2.ctx.write(line, isError);
        return Value::undefined();
    };
    // Enumerable, as console's members are everywhere else (it is a WebIDL namespace, not an ECMAScript built-in): a copy
    // made with `Object.assign({}, console)` has them.
    const auto member = [&rt, console](const char* name, NativeFunction fn) {
        console->putOwn(rt.intern(name), rt.makeFunction(ascii(name), std::move(fn), 0), PropWritable | PropEnumerable | PropConfigurable);
    };
    for (const char* name : {"log", "info", "debug", "trace"})
        member(name, [emit](CallInfo& ci) { return emit(ci, false); });
    for (const char* name : {"warn", "error"})
        member(name, [emit](CallInfo& ci) { return emit(ci, true); });
}

}  // namespace

String inspectValue(Runtime& rt, const Value& v, int depth)
{
    switch (v.type()) {
        case Type::Undefined:
            return ascii("undefined");
        case Type::Null:
            return ascii("null");
        case Type::Bool:
            return ascii(boolOf(v) ? "true" : "false");
        case Type::Int:
        case Type::Long:
        case Type::Float:
        case Type::Char:
        case Type::Double:
            return numberToString(numberOf(v));
        case Type::String:
            return depth == 0 ? String(asString(v)->text())
                              : ascii("'") + String(asString(v)->text()) + ascii("'");
        case Type::BigInt:
            return bigint::toString(bigint::cellOf(v)) + ascii("n");
        case Type::Symbol:
            return ascii("Symbol(") + asSymbol(v)->description + ascii(")");
        default:
            break;
    }
    if (depth > 3)
        return ascii("...");

    if (DynFunction* fn = asFunction(v)) {
        const String name = fn->name;
        return name.empty() ? ascii("[Function (anonymous)]")
                            : ascii("[Function: ") + name + ascii("]");
    }
    DynObject* obj = asObject(v);
    if (obj->cls == ObjClass::Error) {
        const Value text = invokeMethod(rt, v, rt.atom.toString.get(), {});
        String out = toStringValue(rt, text);
        const Value stack = getProperty(rt, v, rt.atom.stack.get());
        if (stack.isString() && !asString(stack)->text().empty())
        {
            out.push_back(u'\n');
            out += asString(stack)->text();
        }
        return out;
    }
    if (DynArray* arr = asArray(v)) {
        String out = ascii("[ ");
        for (std::size_t i = 0; i < arr->size(); ++i) {
            if (i)
                out += ascii(", ");
            if (i >= 100) {
                out += ascii("... ") + numberToString(double(arr->size() - i)) +
                       ascii(" more");
                break;
            }
            out += inspectValue(rt, arr->at(i), depth + 1);
        }
        return arr->empty() ? ascii("[]") : out + ascii(" ]");
    }
    if (obj->cls == ObjClass::Map || obj->cls == ObjClass::Set) {
        auto* m = static_cast<DynMap*>(obj);
        String out = ascii(obj->cls == ObjClass::Map ? "Map(" : "Set(") +
                     numberToString(double(m->liveCount)) + ascii(") { ");
        bool first = true;
        for (const DynMap::Entry& e : m->entries) {
            if (!e.live)
                continue;
            if (!first)
                out += ascii(", ");
            first = false;
            out += inspectValue(rt, e.key, depth + 1);
            if (obj->cls == ObjClass::Map)
                out += ascii(" => ") + inspectValue(rt, e.value, depth + 1);
        }
        return out + ascii(" }");
    }
    if (obj->cls == ObjClass::Promise) {
        auto* p = static_cast<DynPromise*>(obj);
        if (p->state == DynPromise::State::Pending)
            return ascii("Promise { <pending> }");
        if (p->state == DynPromise::State::Rejected)
            return ascii("Promise { <rejected> ") + inspectValue(rt, p->result, depth + 1) +
                   ascii(" }");
        return ascii("Promise { ") + inspectValue(rt, p->result, depth + 1) + ascii(" }");
    }

    String prefix;
    if (obj->proto && obj->proto.get() != rt.objectProto.get()) {
        const Value ctorValue = getPropertyOfObject(rt, obj, rt.atom.constructor.get(), v);
        if (DynFunction* ctor = asFunction(ctorValue)) {
            if (!ctor->name.empty() && ctor->name != ascii("Object"))
                prefix = ctor->name + ascii(" ");
        }
    }
    String out = prefix + ascii("{ ");
    bool first = true;
    for (const Prop& p : obj->props) {
        GCObject* const key = obj->keyOf(p);
        if (!(obj->attrsOf(p) & PropEnumerable) || key->kind == CellKind::Symbol)
            continue;
        if (!first)
            out += ascii(", ");
        first = false;
        out += static_cast<StringCell*>(key)->text();
        out += u": ";
        out += obj->accessorOf(p) ? ascii("[Getter]") : inspectValue(rt, p.value, depth + 1);
    }
    return first ? prefix + ascii("{}") : out + ascii(" }");
}

// ---- WeakMap and WeakSet ----
// What they hold is on the key (`DynObject::Rare::weak`), tagged with the collection's id: see DynWeakCollection.

/// An object, or a symbol that `Symbol.for` did not make: what may be a weak key.
bool canBeHeldWeakly(Runtime& rt, const Value& v)
{
    if (asObject(v))
        return true;
    if (!v.isSymbol())
        return false;
    for (const auto& [name, symbol] : rt.symbolRegistry) {
        if (symbol == detail::Access::ptr(v))
            return false;
    }
    return true;
}

DynWeakCollection* thisWeak(CallInfo& ci, ObjClass expected)
{
    DynObject* o = asObject(ci.thisValue());
    if (!o || o->cls != expected)
        ci.fail(ErrorCode::TypeError, expected == ObjClass::WeakMap ? "not a WeakMap" : "not a WeakSet");
    return static_cast<DynWeakCollection*>(o);
}

const Value* weakFind(DynWeakCollection* c, const Value& key)
{
    if (DynObject* o = asObject(key)) {
        if (o->rare) {
            for (const DynObject::Rare::WeakSlot& w : o->rare->weak) {
                if (w.id == c->id)
                    return &w.value;
            }
        }
        return nullptr;
    }
    for (const auto& [k, v] : c->symbols) {
        if (detail::Access::ptr(k) == detail::Access::ptr(key))
            return &v;
    }
    return nullptr;
}

void weakPut(Runtime& rt, DynWeakCollection* c, const Value& key, const Value& value)
{
    if (DynObject* o = asObject(key)) {
        Vec<DynObject::Rare::WeakSlot>& weak = o->rareOf().weak;
        for (DynObject::Rare::WeakSlot& w : weak) {
            if (w.id == c->id) {
                w.value = value;
                return;
            }
        }
        // What collections that have gone held for it goes now.
        const auto& live = rt.ctx.liveWeakIds;
        weak.erase(std::remove_if(weak.begin(), weak.end(), [&live](const DynObject::Rare::WeakSlot& w) { return !live.count(w.id); }),
                   weak.end());
        weak.push_back({c->id, value});
        return;
    }
    for (auto& [k, v] : c->symbols) {
        if (detail::Access::ptr(k) == detail::Access::ptr(key)) {
            v = value;
            return;
        }
    }
    c->symbols.push_back({key, value});
}

bool weakErase(DynWeakCollection* c, const Value& key)
{
    if (DynObject* o = asObject(key)) {
        if (!o->rare)
            return false;
        Vec<DynObject::Rare::WeakSlot>& weak = o->rare->weak;
        for (std::size_t i = 0; i < weak.size(); ++i) {
            if (weak[i].id == c->id) {
                weak.erase(weak.begin() + std::ptrdiff_t(i));
                return true;
            }
        }
        return false;
    }
    for (std::size_t i = 0; i < c->symbols.size(); ++i) {
        if (detail::Access::ptr(c->symbols[i].first) == detail::Access::ptr(key)) {
            c->symbols.erase(c->symbols.begin() + std::ptrdiff_t(i));
            return true;
        }
    }
    return false;
}

void installWeakCollection(Runtime& rt, ObjClass cls)
{
    const bool isMap = cls == ObjClass::WeakMap;
    const char* name = isMap ? "WeakMap" : "WeakSet";
    DynObject* proto = rt.newObject(rt.objectProto.get());
    (isMap ? rt.weakMapProto : rt.weakSetProto).reset(proto, true);
    proto->putOwn(rt.sym.toStringTag.get(), rt.str(ascii(name)), PropConfigurable);
    rt.makeConstructor(name, [isMap, name](CallInfo& ci) -> Value {
        Runtime& rt2 = rtOf(ci);
        if (!ci.isConstructorCall())
            ci.fail(ErrorCode::TypeError, std::string(name) + " must be called with 'new'");
        auto* c = rt2.newCell<DynWeakCollection>();
        c->cls = isMap ? ObjClass::WeakMap : ObjClass::WeakSet;
        c->setProto((isMap ? rt2.weakMapProto : rt2.weakSetProto).get());
        c->id = rt2.ctx.nextWeakId++;
        rt2.ctx.liveWeakIds.insert(c->id);
        Value result = detail::Access::adopt(Type::Object, c);
        if (!ci.arg(0).isNullish()) {
            // Through `set` or `add` as the program sees them, which it may have replaced.
            const Value adder = getProperty(rt2, result, rt2.intern(isMap ? "set" : "add"));
            if (!asFunction(adder))
                ci.fail(ErrorCode::TypeError, std::string(name) + (isMap ? ".prototype.set" : ".prototype.add") + " is not a function");
            // One at a time, and an abrupt step closes the iterator.
            const Value iterator = getIterator(rt2, ci.arg(0));
            Value item;
            while (iteratorStep(rt2, iterator, item)) {
                try {
                    if (isMap) {
                        if (!item.isObject())
                            ci.fail(ErrorCode::TypeError, "WeakMap entries must be objects");
                        const Value key = getIndex(rt2, item, 0);
                        const Value value = getIndex(rt2, item, 1);
                        callValue(rt2, adder, result, {key, value});
                    } else {
                        callValue(rt2, adder, result, {item});
                    }
                } catch (...) {
                    try {
                        iteratorClose(rt2, iterator);
                    } catch (...) {
                    }
                    throw;
                }
            }
        }
        return result;
    }, 0, proto);
    if (isMap) {
        rt.method(proto, "get", [](CallInfo& ci) {
            const Value* v = weakFind(thisWeak(ci, ObjClass::WeakMap), ci.arg(0));
            return v ? *v : Value::undefined();
        }, 1);
        rt.method(proto, "set", [](CallInfo& ci) {
            Runtime& rt2 = rtOf(ci);
            DynWeakCollection* c = thisWeak(ci, ObjClass::WeakMap);
            if (!canBeHeldWeakly(rt2, ci.arg(0)))
                ci.fail(ErrorCode::TypeError, "invalid value used as a WeakMap key");
            weakPut(rt2, c, ci.arg(0), ci.arg(1));
            return ci.thisValue();
        }, 2);
        rt.method(proto, "getOrInsert", [](CallInfo& ci) {
            Runtime& rt2 = rtOf(ci);
            DynWeakCollection* c = thisWeak(ci, ObjClass::WeakMap);
            if (!canBeHeldWeakly(rt2, ci.arg(0)))
                ci.fail(ErrorCode::TypeError, "invalid value used as a WeakMap key");
            if (const Value* v = weakFind(c, ci.arg(0)))
                return *v;
            weakPut(rt2, c, ci.arg(0), ci.arg(1));
            return ci.arg(1);
        }, 2);
        rt.method(proto, "getOrInsertComputed", [](CallInfo& ci) {
            Runtime& rt2 = rtOf(ci);
            DynWeakCollection* c = thisWeak(ci, ObjClass::WeakMap);
            if (!canBeHeldWeakly(rt2, ci.arg(0)))
                ci.fail(ErrorCode::TypeError, "invalid value used as a WeakMap key");
            if (!asFunction(ci.arg(1)))
                ci.fail(ErrorCode::TypeError, "WeakMap.prototype.getOrInsertComputed: the callback is not a function");
            if (const Value* v = weakFind(c, ci.arg(0)))
                return *v;
            const Value value = callValue(rt2, ci.arg(1), Value::undefined(), {ci.arg(0)});
            // Whatever the callback stored under the key is overwritten.
            weakPut(rt2, c, ci.arg(0), value);
            return value;
        }, 2);
    } else {
        rt.method(proto, "add", [](CallInfo& ci) {
            Runtime& rt2 = rtOf(ci);
            DynWeakCollection* c = thisWeak(ci, ObjClass::WeakSet);
            if (!canBeHeldWeakly(rt2, ci.arg(0)))
                ci.fail(ErrorCode::TypeError, "invalid value used in a WeakSet");
            weakPut(rt2, c, ci.arg(0), Value(true));
            return ci.thisValue();
        }, 1);
    }
    rt.method(proto, "has", [cls](CallInfo& ci) { return Value(weakFind(thisWeak(ci, cls), ci.arg(0)) != nullptr); }, 1);
    rt.method(proto, "delete", [cls](CallInfo& ci) { return Value(weakErase(thisWeak(ci, cls), ci.arg(0))); }, 1);
}

void installCollections(Runtime& rt)
{
    installMap(rt);
    installSet(rt);
    installWeakCollection(rt, ObjClass::WeakMap);
    installWeakCollection(rt, ObjClass::WeakSet);
    installPromise(rt);
    installConsole(rt);
}

}  // namespace dyn
}  // namespace internal
}  // namespace script
