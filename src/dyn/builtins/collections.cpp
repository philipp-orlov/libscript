// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Map, Set, Promise, console, and the value renderer they share.

#include <algorithm>
#include <cmath>

#include "script/internal/dyn/builtins.hpp"
#include "script/internal/dyn/numconv.hpp"
#include "script/internal/dyn/vm.hpp"

namespace script {
namespace internal {
namespace dyn {

namespace {

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
        Value chained = makePromise(rt2);
        auto* next = static_cast<DynPromise*>(asObject(chained));
        self->handled = true;
        DynPromise::Reaction reaction;
        reaction.onFulfilled = ci.arg(0);
        reaction.onRejected = ci.arg(1);
        reaction.next.reset(next);
        addReaction(rt2, self, std::move(reaction));
        return chained;
    }, 2);

    rt.method(proto, "catch", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        return invokeMethod(rt2, ci.thisValue(), rt2.atom.then.get(), {Value::undefined(), ci.arg(0)});
    }, 1);

    rt.method(proto, "finally", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        const Value fn = ci.arg(0);
        Value wrap = rt2.makeFunction(ascii(""), [fn](CallInfo& inner) {
            if (asFunction(fn))
                callValue(rtOf(inner), fn, Value::undefined(), {});
            return inner.arg(0);
        }, 1);
        return invokeMethod(rt2, ci.thisValue(), rt2.atom.then.get(), {wrap, wrap});
    }, 1);

    rt.method(ctor, "resolve", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        if (DynObject* o = asObject(ci.arg(0)); o && o->cls == ObjClass::Promise)
            return ci.arg(0);
        Value promise = makePromise(rt2);
        resolvePromise(rt2, static_cast<DynPromise*>(asObject(promise)), ci.arg(0));
        return promise;
    }, 1);

    rt.method(ctor, "reject", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        Value promise = makePromise(rt2);
        rejectPromise(rt2, static_cast<DynPromise*>(asObject(promise)), ci.arg(0));
        return promise;
    }, 1);

    rt.method(ctor, "all", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        const Vec<Value> items = iterableToVector(rt2, ci.arg(0));
        Value promise = makePromise(rt2);
        auto* target = static_cast<DynPromise*>(asObject(promise));
        auto results = std::make_shared<std::vector<Value>>(items.size());
        auto remaining = std::make_shared<std::size_t>(items.size());
        if (items.empty()) {
            resolvePromise(rt2, target, rt2.makeArray({}));
            return promise;
        }
        for (std::size_t i = 0; i < items.size(); ++i) {
            Value one = invokeMethod(rt2, detail::Access::make(Type::Object, rt2.promiseCtor.get()),
                                     rt2.intern("resolve"), {items[i]});
            Value onOk = rt2.makeFunction(ascii(""), [promise, results, remaining, i](
                                                         CallInfo& inner) {
                (*results)[i] = inner.arg(0);
                if (--*remaining == 0) {
                    Runtime& rtc = rtOf(inner);
                    resolvePromise(rtc, static_cast<DynPromise*>(asObject(promise)),
                                   rtc.makeArray(*results));
                }
                return Value::undefined();
            }, 1);
            invokeMethod(rt2, one, rt2.atom.then.get(), {onOk, makeSettler(rt2, promise, true)});
        }
        return promise;
    }, 1);

    // `Promise.allSettled` and `Promise.any`: what each settles to says when all of it is known.
    const auto gather = [](CallInfo& ci, int kind) -> Value {
        Runtime& rt2 = rtOf(ci);
        Value promise = makePromise(rt2);
        auto* target = static_cast<DynPromise*>(asObject(promise));
        Vec<Value> items;
        try {
            items = iterableToVector(rt2, ci.arg(0));
        } catch (Exception& e) {
            rejectPromise(rt2, target, e.value());
            return promise;
        }
        auto results = std::make_shared<std::vector<Value>>(items.size());
        auto remaining = std::make_shared<std::size_t>(items.size());
        const auto finish = [promise, results, kind](Runtime& rtc) {
            auto* p = static_cast<DynPromise*>(asObject(promise));
            if (kind == 0) {
                resolvePromise(rtc, p, rtc.makeArray(*results));
                return;
            }
            const Value error = constructValue(rtc, getProperty(rtc, detail::Access::make(Type::Object, rtc.globalObject.get()), rtc.intern("AggregateError")),
                                               {rtc.makeArray(*results), rtc.str(ascii("all promises were rejected"))});
            rejectPromise(rtc, p, error);
        };
        if (items.empty()) {
            finish(rt2);
            return promise;
        }
        for (std::size_t i = 0; i < items.size(); ++i) {
            const auto settle = [results, remaining, i, kind, finish](bool fulfilled) {
                return [results, remaining, i, kind, finish, fulfilled](CallInfo& inner) -> Value {
                    Runtime& rtc = rtOf(inner);
                    if (kind == 0) {
                        DynObject* entry = rtc.newObject(rtc.objectProto.get());
                        Value entryValue = detail::Access::adopt(Type::Object, entry);
                        entry->putOwn(rtc.intern("status"), rtc.str(ascii(fulfilled ? "fulfilled" : "rejected")));
                        entry->putOwn(fulfilled ? rtc.atom.value.get() : rtc.intern("reason"), inner.arg(0));
                        (*results)[i] = entryValue;
                    } else {
                        (*results)[i] = inner.arg(0);
                    }
                    if (--*remaining == 0)
                        finish(rtc);
                    return Value::undefined();
                };
            };
            Value one = invokeMethod(rt2, detail::Access::make(Type::Object, rt2.promiseCtor.get()), rt2.intern("resolve"), {items[i]});
            const Value onOk = kind == 0 ? rt2.makeFunction(ascii(""), settle(true), 1) : makeSettler(rt2, promise, false);
            invokeMethod(rt2, one, rt2.atom.then.get(), {onOk, rt2.makeFunction(ascii(""), settle(false), 1)});
        }
        return promise;
    };
    rt.method(ctor, "allSettled", [gather](CallInfo& ci) { return gather(ci, 0); }, 1);
    rt.method(ctor, "any", [gather](CallInfo& ci) { return gather(ci, 1); }, 1);
    rt.method(ctor, "withResolvers", [](CallInfo& ci) -> Value {
        Runtime& rt2 = rtOf(ci);
        Value promise = makePromise(rt2);
        DynObject* out = rt2.newObject(rt2.objectProto.get());
        Value result = detail::Access::adopt(Type::Object, out);
        out->putOwn(rt2.intern("promise"), promise);
        out->putOwn(rt2.intern("resolve"), makeSettler(rt2, promise, false));
        out->putOwn(rt2.intern("reject"), makeSettler(rt2, promise, true));
        return result;
    }, 0);
    rt.method(ctor, "try", [](CallInfo& ci) -> Value {
        Runtime& rt2 = rtOf(ci);
        Value promise = makePromise(rt2);
        auto* target = static_cast<DynPromise*>(asObject(promise));
        try {
            std::vector<Value> rest(ci.args().begin() + (ci.size() ? 1 : 0), ci.args().end());
            resolvePromise(rt2, target, callValue(rt2, ci.arg(0), Value::undefined(), rest));
        } catch (Exception& e) {
            rejectPromise(rt2, target, e.value());
        }
        return promise;
    }, 1);

    rt.method(ctor, "race", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        Value promise = makePromise(rt2);
        for (const Value& item : iterableToVector(rt2, ci.arg(0))) {
            Value one = invokeMethod(rt2, detail::Access::make(Type::Object, rt2.promiseCtor.get()),
                                     rt2.intern("resolve"), {item});
            invokeMethod(rt2, one, rt2.atom.then.get(),
                         {makeSettler(rt2, promise, false), makeSettler(rt2, promise, true)});
        }
        return promise;
    }, 1);
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
    for (const char* name : {"log", "info", "debug", "trace"})
        rt.method(console, name, [emit](CallInfo& ci) { return emit(ci, false); }, 0);
    for (const char* name : {"warn", "error"})
        rt.method(console, name, [emit](CallInfo& ci) { return emit(ci, true); }, 0);
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

void installCollections(Runtime& rt)
{
    installMap(rt);
    installSet(rt);
    installPromise(rt);
    installConsole(rt);
}

}  // namespace dyn
}  // namespace internal
}  // namespace script
