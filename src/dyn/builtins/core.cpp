// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Object, Function, Symbol, the Error hierarchy and the global functions.

#include <algorithm>
#include <cmath>
#include <cstring>

#include "script/internal/dyn/builtins.hpp"
#include "script/internal/dyn/proxy.hpp"
#include "script/internal/dyn/parser.hpp"
#include "script/internal/dyn/numconv.hpp"
#include "script/internal/dyn/vm.hpp"

namespace script {
namespace internal {
namespace dyn {

namespace {

Value functionFromStrings(Runtime& rt, CallInfo& ci, const char* keyword);

Value objectValue(DynObject* o)
{
    return detail::Access::make(Type::Object, o);
}

const char* classTag(ObjClass cls)
{
    switch (cls) {
        case ObjClass::Array: return "Array";
        case ObjClass::Function: return "Function";
        case ObjClass::Error: return "Error";
        case ObjClass::Date: return "Date";
        case ObjClass::RegExp: return "RegExp";
        case ObjClass::StringObject: return "String";
        case ObjClass::NumberObject: return "Number";
        case ObjClass::BooleanObject: return "Boolean";
        default: return "Object";
    }
}

void installObject(Runtime& rt)
{
    DynObject* proto = rt.objectProto.get();

    // Annex B's `__proto__`: an accessor on Object.prototype that reads and writes the prototype.
    {
        GCObject* key = rt.intern("__proto__");
        proto->putOwn(key, rt.makeFunction(ascii("get __proto__"), [](CallInfo& ci) -> Value {
            Runtime& rt2 = rtOf(ci);
            const Value held = toObject(rt2, ci.thisValue());
            DynObject* self = asObject(held);
            return self ? getPrototypeOf(rt2, self) : Value::null();
        }, 0), PropConfigurable | PropAccessor);
        proto->setSetterOf(key, rt.makeFunction(ascii("set __proto__"), [](CallInfo& ci) -> Value {
            Runtime& rt2 = rtOf(ci);
            if (ci.thisValue().isNullish())
                ci.fail(ErrorCode::TypeError, "Object.prototype.__proto__ called on null or undefined");
            const Value& next = ci.arg(0);
            DynObject* self = asObject(ci.thisValue());
            if (!self || (!next.isNull() && !asObject(next)))
                return Value::undefined();
            if (!setPrototypeOf(rt2, self, asObject(next)))
                ci.fail(ErrorCode::TypeError, "the prototype cannot be set");
            return Value::undefined();
        }, 1));
    }

    rt.method(proto, "hasOwnProperty", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        Ref<GCObject> key = toPropertyKey(rt2, ci.arg(0));
        // Kept, since for a primitive it is the only count on the wrapper made for it.
        const Value held = toObject(rt2, ci.thisValue());
        DynObject* self = asObject(held);
        if (!self)
            return Value(false);
        if (self->fnKindIsProxy()) {
            Descriptor d;
            return Value(getOwnProperty(rt2, self, key.get(), d));
        }
        if (key.get() == rt2.atom.prototype.get())
            ensurePrototype(rt2, self);
        if (self->cls == ObjClass::Array && key->kind == CellKind::String) {
            std::uint32_t index = 0;
            if (arrayIndexOf(static_cast<StringCell*>(key.get())->text(), index))
                return Value(index < static_cast<DynArray*>(self)->size());
        }
        return Value(self->findOwn(key.get()) != nullptr);
    }, 1);

    rt.method(proto, "isPrototypeOf", [](CallInfo& ci) {
        DynObject* self = asObject(ci.thisValue());
        DynObject* other = asObject(ci.arg(0));
        if (!self || !other)
            return Value(false);
        for (DynObject* c = other->proto.get(); c; c = c->proto.get()) {
            if (c == self)
                return Value(true);
        }
        return Value(false);
    }, 1);

    rt.method(proto, "propertyIsEnumerable", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        Ref<GCObject> key = toPropertyKey(rt2, ci.arg(0));
        const Value object = toObject(rt2, ci.thisValue());
        Descriptor d;
        return Value(getOwnProperty(rt2, asObject(object), key.get(), d) && d.enumerable);
    }, 1);

    rt.method(proto, "toString", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        if (ci.thisValue().isUndefined())
            return rt2.str("[object Undefined]");
        if (ci.thisValue().isNull())
            return rt2.str("[object Null]");
        const Value object = toObject(rt2, ci.thisValue());
        const Value named = getProperty(rt2, object, rt2.sym.toStringTag.get());
        if (named.type() == Type::String)
            return rt2.str(ascii("[object ") + toStringValue(rt2, named) + ascii("]"));
        return rt2.str(ascii(std::string("[object ") + classTag(asObject(object)->cls) + "]"));
    }, 0);

    rt.method(proto, "valueOf", [](CallInfo& ci) { return ci.thisValue(); }, 0);

    rt.method(proto, "toLocaleString", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        return invokeMethod(rt2, ci.thisValue(), rt2.atom.toString.get(), {});
    }, 0);

    DynFunction* ctor = rt.makeConstructor("Object", [](CallInfo& ci) -> Value {
        Runtime& rt2 = rtOf(ci);
        if (ci.arg(0).isNullish())
            return objectValue(rt2.newObject(rt2.objectProto.get()));
        return toObject(rt2, ci.arg(0));
    }, 1, proto);
    rt.objectCtor.reset(ctor);

    rt.method(ctor, "keys", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        Value self = toObject(rt2, ci.arg(0));
        return rt2.makeArray(ownKeys(rt2, asObject(self), true, false));
    }, 1);

    rt.method(ctor, "values", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        Value self = toObject(rt2, ci.arg(0));
        Vec<Value> out;
        for (const Value& k : ownKeys(rt2, asObject(self), true, false))
            out.push_back(getProperty(rt2, self, toPropertyKey(rt2, k).get()));
        return rt2.makeArray(std::move(out));
    }, 1);

    rt.method(ctor, "entries", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        Value self = toObject(rt2, ci.arg(0));
        Vec<Value> out;
        for (const Value& k : ownKeys(rt2, asObject(self), true, false)) {
            out.push_back(rt2.makeArray({k, getProperty(rt2, self, toPropertyKey(rt2, k).get())}));
        }
        return rt2.makeArray(std::move(out));
    }, 1);

    rt.method(ctor, "fromEntries", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        DynObject* out = rt2.newObject(rt2.objectProto.get());
        Value result = detail::Access::adopt(Type::Object, out);
        for (const Value& pair : iterableToVector(rt2, ci.arg(0))) {
            Ref<GCObject> key = toPropertyKey(rt2, getIndex(rt2, pair, 0));
            out->putOwn(key.get(), getIndex(rt2, pair, 1));
        }
        return result;
    }, 1);

    rt.method(ctor, "assign", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        Value target = toObject(rt2, ci.arg(0));
        for (std::size_t i = 1; i < ci.size(); ++i) {
            const Value& src = ci.args()[i];
            if (src.isNullish())
                continue;
            DynObject* from = asObject(src);
            if (!from)
                continue;
            for (const Value& k : ownKeys(rt2, from, true, true)) {
                Ref<GCObject> key = toPropertyKey(rt2, k);
                setProperty(rt2, target, key.get(), getProperty(rt2, src, key.get()));
            }
        }
        return target;
    }, 2);

    rt.method(ctor, "create", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        DynObject* out = rt2.newObject(asObject(ci.arg(0)));
        if (ci.arg(0).isNull())
            out->setProto(nullptr);
        Value result = detail::Access::adopt(Type::Object, out);
        if (DynObject* descriptors = asObject(ci.arg(1))) {
            for (const Value& k : ownKeys(rt2, descriptors, true, true)) {
                Ref<GCObject> key = toPropertyKey(rt2, k);
                const Value desc = getProperty(rt2, ci.arg(1), key.get());
                out->putOwn(key.get(), getProperty(rt2, desc, rt2.atom.value.get()));
            }
        }
        return result;
    }, 2);

    rt.method(ctor, "getPrototypeOf", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        const Value held = toObject(rt2, ci.arg(0));
        DynObject* self = asObject(held);
        return self && self->proto ? objectValue(self->proto.get()) : Value::null();
    }, 1);

    rt.method(ctor, "setPrototypeOf", [](CallInfo& ci) {
        if (DynObject* self = asObject(ci.arg(0))) {
            DynObject* next = asObject(ci.arg(1));
            // Every lookup walks this chain without polling, so a cycle would hang the host beyond the reach of the
            // interrupt handler. The chain is acyclic by construction, which keeps the walk from needing a check.
            for (DynObject* cursor = next; cursor; cursor = cursor->proto.get()) {
                if (cursor == self)
                    ci.fail(ErrorCode::TypeError, "cyclic prototype chain");
            }
            if (next != self->proto.get() && !self->extensible)
                ci.fail(ErrorCode::TypeError, "the object is not extensible");
            self->setProto(next);
        }
        return ci.arg(0);
    }, 2);

    rt.method(ctor, "getOwnPropertyNames", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        Value self = toObject(rt2, ci.arg(0));
        return rt2.makeArray(ownKeys(rt2, asObject(self), false, false));
    }, 1);

    rt.method(ctor, "defineProperty", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        DynObject* self = asObject(ci.arg(0));
        if (!self)
            ci.fail(ErrorCode::TypeError, "Object.defineProperty needs an object");
        Ref<GCObject> key = toPropertyKey(rt2, ci.arg(1));
        const Value desc = ci.arg(2);
        std::uint8_t attrs = 0;
        if (toBoolean(getProperty(rt2, desc, rt2.intern("writable"))))
            attrs |= PropWritable;
        if (toBoolean(getProperty(rt2, desc, rt2.intern("enumerable"))))
            attrs |= PropEnumerable;
        if (toBoolean(getProperty(rt2, desc, rt2.intern("configurable"))))
            attrs |= PropConfigurable;
        const Value getterFn = getProperty(rt2, desc, rt2.intern("get"));
        const Value setterFn = getProperty(rt2, desc, rt2.intern("set"));
        if (!getterFn.isUndefined() || !setterFn.isUndefined()) {
            Prop& p = self->putOwn(key.get(), getterFn, std::uint8_t(attrs | PropAccessor));
            self->setSetter(p, setterFn);
        } else {
            self->putOwn(key.get(), getProperty(rt2, desc, rt2.atom.value.get()), attrs);
        }
        return ci.arg(0);
    }, 3);

    rt.method(ctor, "getOwnPropertyDescriptor", [](CallInfo& ci) -> Value {
        Runtime& rt2 = rtOf(ci);
        DynObject* self = asObject(ci.arg(0));
        if (!self)
            return Value::undefined();
        Ref<GCObject> key = toPropertyKey(rt2, ci.arg(1));
        const Prop* p = self->findOwn(key.get());
        if (!p)
            return Value::undefined();
        DynObject* out = rt2.newObject(rt2.objectProto.get());
        Value result = detail::Access::adopt(Type::Object, out);
        const std::uint8_t attrs = self->attrsOf(*p);
        if (attrs & PropAccessor) {
            out->putOwn(rt2.intern("get"), p->value);
            out->putOwn(rt2.intern("set"), self->setterOf(*p));
        } else {
            out->putOwn(rt2.atom.value.get(), p->value);
            out->putOwn(rt2.intern("writable"), Value((attrs & PropWritable) != 0));
        }
        out->putOwn(rt2.intern("enumerable"), Value((attrs & PropEnumerable) != 0));
        out->putOwn(rt2.intern("configurable"), Value((attrs & PropConfigurable) != 0));
        return result;
    }, 2);

    rt.method(ctor, "freeze", [](CallInfo& ci) {
        if (DynObject* self = asObject(ci.arg(0))) {
            self->frozen = true;
            self->extensible = false;
            self->dropShape();
            for (std::size_t i = 0; i < self->props.size(); ++i) {
                GCObject* key = self->keyAt(i);
                if (key->kind != CellKind::Symbol || !static_cast<SymbolCell*>(key)->isPrivate)
                    self->setAttrsAt(i, std::uint8_t(self->attrsAt(i) & ~(PropWritable | PropConfigurable)));
            }
        }
        return ci.arg(0);
    }, 1);

    rt.method(ctor, "isFrozen", [](CallInfo& ci) {
        DynObject* self = asObject(ci.arg(0));
        return Value(!self || self->frozen);
    }, 1);

    rt.method(ctor, "preventExtensions", [](CallInfo& ci) {
        if (DynObject* self = asObject(ci.arg(0))) {
            self->extensible = false;
            self->dropShape();
        }
        return ci.arg(0);
    }, 1);

    rt.method(ctor, "is", [](CallInfo& ci) {
        const Value a = ci.arg(0);
        const Value b = ci.arg(1);
        if (a.isNumber() && b.isNumber()) {
            const double x = numberOf(a);
            const double y = numberOf(b);
            if (std::isnan(x) && std::isnan(y))
                return Value(true);
            if (x == 0 && y == 0)
                return Value(std::signbit(x) == std::signbit(y));
        }
        return Value(strictEquals(a, b));
    }, 2);
}

void installFunction(Runtime& rt)
{
    DynObject* proto = rt.functionProto.get();

    rt.method(proto, "call", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        ContextImpl::ArgBuffer buffer(rt2.ctx);
        buffer.args.assign(ci.args().begin() + (ci.size() ? 1 : 0), ci.args().end());
        return callValue(rt2, ci.thisValue(), ci.arg(0), buffer.args);
    }, 1);

    rt.method(proto, "apply", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        ContextImpl::ArgBuffer buffer(rt2.ctx);
        if (DynArray* arr = asArray(ci.arg(1))) {
            buffer.args.reserve(arr->size());
            for (std::size_t i = 0; i < arr->size(); ++i)
                buffer.args.push_back(arr->at(i));
        } else if (!ci.arg(1).isNullish()) {
            const Vec<Value> items = iterableToVector(rt2, ci.arg(1));
            buffer.args.assign(items.begin(), items.end());
        }
        return callValue(rt2, ci.thisValue(), ci.arg(0), buffer.args);
    }, 2);

    rt.intrinsics[Runtime::FunctionApply].reset(detail::Access::ptr(proto->findOwn(rt.intern("apply"))->value));
    rt.intrinsics[Runtime::FunctionCall].reset(detail::Access::ptr(proto->findOwn(rt.intern("call"))->value));

    rt.method(proto, "bind", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        DynFunction* target = asFunction(ci.thisValue());
        if (!target)
            ci.fail(ErrorCode::TypeError, "bind needs a function");
        DynFunction* bound = rt2.newFunction();
        Value result = detail::Access::adopt(Type::Object, bound);
        bound->fnKind = FnKind::Bound;
        bound->boundTarget.reset(target);
        bound->boundThis = ci.arg(0);
        if (ci.size() > 1)
            bound->boundArgs.assign(ci.args().begin() + 1, ci.args().end());
        bound->name = ascii("bound ") + target->name;
        bound->putOwn(rt2.atom.name.get(), rt2.str(bound->name), PropConfigurable);
        bound->putOwn(rt2.atom.length.get(),
                      Value(double(std::max<int>(0, target->arity - int(bound->boundArgs.size())))),
                      PropConfigurable);
        return result;
    }, 1);

    rt.method(proto, "toString", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        DynFunction* self = asFunction(ci.thisValue());
        const String name = self ? self->name : String();
        return rt2.str(ascii("function ") + name + ascii("() { [native code] }"));
    }, 0);

    DynFunction* ctor = rt.makeConstructor("Function", [](CallInfo& ci) -> Value {
        Runtime& rt2 = rtOf(ci);
        if (!rt2.ctx.options.runtimeCode)
            ci.fail(ErrorCode::EvalError, "the Function constructor is disabled: the engine runs only the source its host gives it");
        return functionFromStrings(rt2, ci, "function");
    }, 1, proto);
    rt.functionCtor.reset(ctor);
}

void installSymbol(Runtime& rt)
{
    DynObject* proto = rt.newObject(rt.objectProto.get());
    rt.symbolProto.reset(proto, true);

    rt.method(proto, "toString", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        SymbolCell* self = asSymbol(ci.thisValue());
        if (!self) {
            if (DynObject* o = asObject(ci.thisValue());
                o && o->cls == ObjClass::SymbolObject)
                self = asSymbol(static_cast<DynWrapper*>(o)->primitive);
        }
        if (!self)
            ci.fail(ErrorCode::TypeError, "not a symbol");
        return rt2.str(ascii("Symbol(") + self->description + ascii(")"));
    }, 0);

    rt.getter(proto, "description", [](CallInfo& ci) -> Value {
        Runtime& rt2 = rtOf(ci);
        SymbolCell* self = asSymbol(ci.thisValue());
        return self ? rt2.str(self->description) : Value::undefined();
    });

    DynFunction* ctor = rt.makeConstructor("Symbol", [](CallInfo& ci) -> Value {
        Runtime& rt2 = rtOf(ci);
        if (ci.isConstructorCall())
            ci.fail(ErrorCode::TypeError, "Symbol is not a constructor");
        auto* s = rt2.newCell<SymbolCell>();
        if (!ci.arg(0).isUndefined())
            s->description = toStringValue(rt2, ci.arg(0));
        return detail::Access::adopt(Type::Symbol, s);
    }, 1, proto);

    rt.constant(ctor, "iterator", detail::Access::make(Type::Symbol, rt.sym.iterator.get()));
    rt.constant(ctor, "asyncIterator",
                detail::Access::make(Type::Symbol, rt.sym.asyncIterator.get()));
    rt.constant(ctor, "toPrimitive", detail::Access::make(Type::Symbol, rt.sym.toPrimitive.get()));
    rt.constant(ctor, "toStringTag", detail::Access::make(Type::Symbol, rt.sym.toStringTag.get()));
    rt.constant(ctor, "hasInstance", detail::Access::make(Type::Symbol, rt.sym.hasInstance.get()));
    rt.constant(ctor, "match", detail::Access::make(Type::Symbol, rt.sym.match.get()));
    rt.constant(ctor, "matchAll", detail::Access::make(Type::Symbol, rt.sym.matchAll.get()));
    rt.constant(ctor, "replace", detail::Access::make(Type::Symbol, rt.sym.replace.get()));
    rt.constant(ctor, "search", detail::Access::make(Type::Symbol, rt.sym.search.get()));
    rt.constant(ctor, "split", detail::Access::make(Type::Symbol, rt.sym.split.get()));
    rt.constant(ctor, "species", detail::Access::make(Type::Symbol, rt.sym.species.get()));
    rt.constant(ctor, "unscopables", detail::Access::make(Type::Symbol, rt.sym.unscopables.get()));
    rt.constant(ctor, "isConcatSpreadable", detail::Access::make(Type::Symbol, rt.sym.isConcatSpreadable.get()));

    rt.method(ctor, "for", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        const String key = toStringValue(rt2, ci.arg(0));
        auto it = rt2.symbolRegistry.find(key);
        if (it != rt2.symbolRegistry.end())
            return detail::Access::make(Type::Symbol, it->second);
        auto* s = rt2.newCell<SymbolCell>();
        s->description = key;
        s->registered = true;
        // The registry holds a count of its own, which `afterCollect` drops once it is the only one.
        rt2.symbolRegistry.emplace(key, s);
        return detail::Access::make(Type::Symbol, s);
    }, 1);
    rt.method(ctor, "keyFor", [](CallInfo& ci) {
        if (!ci.arg(0).isSymbol())
            ci.fail(ErrorCode::TypeError, "Symbol.keyFor: not a symbol");
        const auto* s = static_cast<const SymbolCell*>(detail::Access::ptr(ci.arg(0)));
        return s->registered ? rtOf(ci).str(s->description) : Value::undefined();
    }, 1);
}

void installErrors(Runtime& rt)
{
    auto define = [&rt](const char* name, ErrorCode code, DynObject* parentProto,
                        DynFunction* parentCtor) {
        DynObject* proto = rt.newObject(parentProto);
        Value protoValue = detail::Access::adopt(Type::Object, proto);
        rt.constant(proto, "name", rt.str(name));
        rt.constant(proto, "message", rt.str(""));

        DynFunction* ctor = rt.makeConstructor(name, [](CallInfo& ci) -> Value {
            Runtime& rt2 = rtOf(ci);
            Value self = ci.thisValue();
            DynObject* obj = asObject(self);
            if (!ci.isConstructorCall() || !obj || obj == rt2.globalObject.get()) {
                obj = rt2.newObject(rt2.errorProto.get());
                self = detail::Access::adopt(Type::Object, obj);
            }
            obj->cls = ObjClass::Error;
            if (!ci.arg(0).isUndefined())
                obj->putOwn(rt2.atom.message.get(), rt2.str(toStringValue(rt2, ci.arg(0))),
                            PropWritable | PropConfigurable);
            obj->putOwn(rt2.atom.stack.get(), rt2.str(fromUtf8(rt2.captureStack())),
                        PropWritable | PropConfigurable);
            return self;
        }, 1, proto);
        if (parentCtor)
            ctor->setProto(parentCtor);
        rt.errorProtos[int(code)] = Ref<DynObject>(proto);
        return std::make_pair(proto, ctor);
    };

    auto base = define("Error", ErrorCode::Exception, rt.objectProto.get(), nullptr);
    rt.errorProto.reset(base.first);
    rt.method(base.first, "toString", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        const String name = toStringValue(rt2, getProperty(rt2, ci.thisValue(), rt2.atom.name.get()));
        const Value msg = getProperty(rt2, ci.thisValue(), rt2.atom.message.get());
        const String text = toStringValue(rt2, msg);
        if (text.empty())
            return rt2.str(name);
        return rt2.str(name + ascii(": ") + text);
    }, 0);

    define("TypeError", ErrorCode::TypeError, base.first, base.second);
    auto range = define("RangeError", ErrorCode::RangeError, base.first, base.second);
    // Exhausting the call depth is reported the way engines report a stack
    // overflow: as a RangeError.
    rt.errorProtos[int(ErrorCode::StackOverflow)] = Ref<DynObject>(range.first);
    define("SyntaxError", ErrorCode::SyntaxError, base.first, base.second);
    define("ReferenceError", ErrorCode::ReferenceError, base.first, base.second);
    define("EvalError", ErrorCode::EvalError, base.first, base.second);
    define("URIError", ErrorCode::UriError, base.first, base.second);
    define("InternalError", ErrorCode::InternalError, base.first, base.second);

    {
        DynObject* proto = rt.newObject(base.first);
        Value protoValue = detail::Access::adopt(Type::Object, proto);
        rt.constant(proto, "name", rt.str("AggregateError"));
        rt.constant(proto, "message", rt.str(""));
        DynFunction* ctor = rt.makeConstructor("AggregateError", [protoValue](CallInfo& ci) -> Value {
            Runtime& rt2 = rtOf(ci);
            Value self = ci.thisValue();
            DynObject* obj = asObject(self);
            if (!ci.isConstructorCall() || !obj || obj == rt2.globalObject.get()) {
                obj = rt2.newObject(asObject(protoValue));
                self = detail::Access::adopt(Type::Object, obj);
            }
            obj->cls = ObjClass::Error;
            if (!ci.arg(1).isUndefined())
                obj->putOwn(rt2.atom.message.get(), rt2.str(toStringValue(rt2, ci.arg(1))), PropWritable | PropConfigurable);
            const Vec<Value> errors = iterableToVector(rt2, ci.arg(0));
            obj->putOwn(rt2.intern("errors"), rt2.makeArray(std::vector<Value>(errors.begin(), errors.end())), PropWritable | PropConfigurable);
            obj->putOwn(rt2.atom.stack.get(), rt2.str(fromUtf8(rt2.captureStack())), PropWritable | PropConfigurable);
            return self;
        }, 2, proto);
        ctor->setProto(base.second);
    }
}

/// `new Function(p1, ..., body)` and its kin: the parameters and the body are checked apart, so neither can close what
/// the other opened, and then compiled together at the top level.
Value functionFromStrings(Runtime& rt, CallInfo& ci, const char* keyword)
{
    String params;
    for (std::size_t i = 0; i + 1 < ci.size(); ++i) {
        if (i)
            params.push_back(u',');
        params += toStringValue(rt, ci.arg(i));
    }
    const String body = ci.size() ? toStringValue(rt, ci.arg(ci.size() - 1)) : String();
    const auto check = [&](const String& source) {
        try {
            const std::string utf8 = toUtf8(source);
            Parser parser(utf8, "<function>", false);
            parser.parseProgram();
        } catch (const Error& e) {
            throwError(rt, ErrorCode::SyntaxError, e.what());
        }
    };
    check(ascii("(") + ascii(keyword) + ascii(" anonymous(") + params + ascii("\n) {})"));
    check(ascii("(") + ascii(keyword) + ascii(" anonymous() {\n") + body + ascii("\n})"));
    const String source = ascii("(") + ascii(keyword) + ascii(" anonymous(") + params + ascii("\n) {\n") + body + ascii("\n})");
    return evalString(rt, rt.str(source), nullptr, nullptr);
}

void installGlobals(Runtime& rt)
{
    DynObject* g = rt.globalObject.get();
    rt.constant(g, "globalThis", objectValue(g));
    g->putOwn(rt.intern("undefined"), Value::undefined(), 0);
    g->putOwn(rt.intern("NaN"), Value(std::nan("")), 0);
    g->putOwn(rt.intern("Infinity"), Value(std::numeric_limits<double>::infinity()), 0);

    rt.method(g, "parseInt", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        if (ci.arg(0).isDouble() && ci.arg(1).isUndefined())
            return Value(parseIntOfNumber(detail::Access::r8(ci.arg(0))));
        const String s = toStringValue(rt2, ci.arg(0));
        const int radix = ci.arg(1).isUndefined() ? 0 : int(toNumber(rt2, ci.arg(1)));
        return Value(parseIntPrefix(s, radix));
    }, 2);

    rt.intrinsics[Runtime::ParseInt].reset(detail::Access::ptr(g->findOwn(rt.intern("parseInt"))->value));

    rt.method(g, "parseFloat", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        return Value(parseFloatPrefix(toStringValue(rt2, ci.arg(0))));
    }, 1);

    rt.method(g, "eval", [](CallInfo& ci) { return evalString(rtOf(ci), ci.arg(0), nullptr, nullptr); }, 1);
    rt.intrinsics[Runtime::Eval].reset(detail::Access::ptr(g->findOwn(rt.intern("eval"))->value));

    // ---- URIs: UTF-8 in percent escapes
    static const char kHex[] = "0123456789ABCDEF";
    const auto encode = [](CallInfo& ci, const char* alsoUnescaped) -> Value {
        Runtime& rt2 = rtOf(ci);
        const String s = toStringValue(rt2, ci.arg(0));
        String out;
        for (std::size_t i = 0; i < s.size(); ++i) {
            const Char c = s[i];
            const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                               (c < 128 && (std::strchr("-_.!~*'()", int(c)) || std::strchr(alsoUnescaped, int(c))) && c != 0);
            if (plain) {
                out.push_back(c);
                continue;
            }
            char32_t code = c;
            if (c >= 0xdc00 && c <= 0xdfff)
                ci.fail(ErrorCode::UriError, "URI malformed: a lone surrogate");
            if (c >= 0xd800 && c <= 0xdbff) {
                if (i + 1 >= s.size() || s[i + 1] < 0xdc00 || s[i + 1] > 0xdfff)
                    ci.fail(ErrorCode::UriError, "URI malformed: a lone surrogate");
                code = 0x10000 + ((char32_t(c) - 0xd800) << 10) + (s[++i] - 0xdc00);
            }
            unsigned char bytes[4];
            int n;
            if (code < 0x80) {
                bytes[0] = std::uint8_t(code);
                n = 1;
            } else if (code < 0x800) {
                bytes[0] = std::uint8_t(0xc0 | (code >> 6));
                bytes[1] = std::uint8_t(0x80 | (code & 63));
                n = 2;
            } else if (code < 0x10000) {
                bytes[0] = std::uint8_t(0xe0 | (code >> 12));
                bytes[1] = std::uint8_t(0x80 | ((code >> 6) & 63));
                bytes[2] = std::uint8_t(0x80 | (code & 63));
                n = 3;
            } else {
                bytes[0] = std::uint8_t(0xf0 | (code >> 18));
                bytes[1] = std::uint8_t(0x80 | ((code >> 12) & 63));
                bytes[2] = std::uint8_t(0x80 | ((code >> 6) & 63));
                bytes[3] = std::uint8_t(0x80 | (code & 63));
                n = 4;
            }
            for (int k = 0; k < n; ++k) {
                out.push_back(u'%');
                out.push_back(Char(kHex[bytes[k] >> 4]));
                out.push_back(Char(kHex[bytes[k] & 15]));
            }
        }
        return rt2.str(out);
    };
    const auto hexAt = [](const String& s, std::size_t i) -> int {
        const auto digit = [](Char c) -> int {
            if (c >= '0' && c <= '9')
                return c - '0';
            if (c >= 'a' && c <= 'f')
                return c - 'a' + 10;
            if (c >= 'A' && c <= 'F')
                return c - 'A' + 10;
            return -1;
        };
        if (i + 1 >= s.size())
            return -1;
        const int a = digit(s[i]);
        const int b = digit(s[i + 1]);
        return a < 0 || b < 0 ? -1 : a * 16 + b;
    };
    const auto decode = [hexAt](CallInfo& ci, const char* reserved) -> Value {
        Runtime& rt2 = rtOf(ci);
        const String s = toStringValue(rt2, ci.arg(0));
        String out;
        for (std::size_t i = 0; i < s.size(); ++i) {
            if (s[i] != u'%') {
                out.push_back(s[i]);
                continue;
            }
            const std::size_t start = i;
            const int b = hexAt(s, i + 1);
            if (b < 0)
                ci.fail(ErrorCode::UriError, "URI malformed");
            i += 2;
            if (b < 0x80) {
                if (b && std::strchr(reserved, b))
                    out += s.substr(start, 3);
                else
                    out.push_back(Char(b));
                continue;
            }
            int n = (b & 0xe0) == 0xc0 ? 2 : (b & 0xf0) == 0xe0 ? 3 : (b & 0xf8) == 0xf0 ? 4 : 0;
            if (!n)
                ci.fail(ErrorCode::UriError, "URI malformed");
            char32_t code = char32_t(b & (0xff >> (n + 1)));
            for (int k = 1; k < n; ++k) {
                if (i + 1 >= s.size() || s[i + 1] != u'%')
                    ci.fail(ErrorCode::UriError, "URI malformed");
                const int more = hexAt(s, i + 2);
                if (more < 0 || (more & 0xc0) != 0x80)
                    ci.fail(ErrorCode::UriError, "URI malformed");
                code = (code << 6) | char32_t(more & 63);
                i += 3;
            }
            const char32_t least = n == 2 ? 0x80 : n == 3 ? 0x800 : 0x10000;
            if (code < least || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff))
                ci.fail(ErrorCode::UriError, "URI malformed");
            if (code > 0xffff) {
                out.push_back(Char(0xd800 + ((code - 0x10000) >> 10)));
                out.push_back(Char(0xdc00 + ((code - 0x10000) & 0x3ff)));
            } else {
                out.push_back(Char(code));
            }
        }
        return rt2.str(out);
    };
    rt.method(g, "encodeURIComponent", [encode](CallInfo& ci) { return encode(ci, ""); }, 1);
    rt.method(g, "encodeURI", [encode](CallInfo& ci) { return encode(ci, ";/?:@&=+$,#"); }, 1);
    rt.method(g, "decodeURIComponent", [decode](CallInfo& ci) { return decode(ci, ""); }, 1);
    rt.method(g, "decodeURI", [decode](CallInfo& ci) { return decode(ci, ";/?:@&=+$,#"); }, 1);
    rt.method(g, "escape", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        const String s = toStringValue(rt2, ci.arg(0));
        String out;
        for (Char c : s) {
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || (c < 128 && c && std::strchr("@*_+-./", int(c)))) {
                out.push_back(c);
            } else if (c < 256) {
                out.push_back(u'%');
                out.push_back(Char(kHex[c >> 4]));
                out.push_back(Char(kHex[c & 15]));
            } else {
                out += ascii("%u");
                for (int shift = 12; shift >= 0; shift -= 4)
                    out.push_back(Char(kHex[(c >> shift) & 15]));
            }
        }
        return rt2.str(out);
    }, 1);
    rt.method(g, "unescape", [hexAt](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        const String s = toStringValue(rt2, ci.arg(0));
        String out;
        for (std::size_t i = 0; i < s.size(); ++i) {
            if (s[i] == u'%') {
                if (i + 5 < s.size() + 0 && s[i + 1] == u'u') {
                    const int hi = hexAt(s, i + 2);
                    const int lo = hexAt(s, i + 4);
                    if (hi >= 0 && lo >= 0) {
                        out.push_back(Char(hi * 256 + lo));
                        i += 5;
                        continue;
                    }
                }
                const int b = hexAt(s, i + 1);
                if (b >= 0) {
                    out.push_back(Char(b));
                    i += 2;
                    continue;
                }
            }
            out.push_back(s[i]);
        }
        return rt2.str(out);
    }, 1);

    rt.method(g, "isNaN", [](CallInfo& ci) {
        return Value(std::isnan(toNumber(rtOf(ci), ci.arg(0))));
    }, 1);

    rt.method(g, "isFinite", [](CallInfo& ci) {
        return Value(std::isfinite(toNumber(rtOf(ci), ci.arg(0))));
    }, 1);

    rt.method(g, "structuredClone", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        String text;
        if (!jsonStringify(rt2, ci.arg(0), Value::undefined(), StringView(), text))
            return Value::undefined();
        return jsonParse(rt2, text);
    }, 1);
}

}  // namespace

void installCore(Runtime& rt)
{
    installObject(rt);
    installFunction(rt);
    installSymbol(rt);
    installErrors(rt);
    installGlobals(rt);
}

void installBuiltins(Runtime& rt)
{
    installCore(rt);
    installArray(rt);
    installString(rt);
    installMathJson(rt);
    installCollections(rt);
    installGenerators(rt);
    installReflection(rt);
    installRegExp(rt);
    installBigInt(rt);
    installTypedArrays(rt);
    installIteratorHelpers(rt);
}

}  // namespace dyn
}  // namespace internal
}  // namespace script
