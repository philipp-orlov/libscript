// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Object, Function, Symbol, the Error hierarchy and the global functions.

#include <algorithm>
#include <cmath>

#include "script/internal/dyn/builtins.hpp"
#include "script/internal/dyn/numconv.hpp"
#include "script/internal/dyn/vm.hpp"

namespace script {
namespace internal {
namespace dyn {

namespace {

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
        case ObjClass::StringObject: return "String";
        case ObjClass::NumberObject: return "Number";
        case ObjClass::BooleanObject: return "Boolean";
        default: return "Object";
    }
}

void installObject(Runtime& rt)
{
    DynObject* proto = rt.objectProto.get();

    rt.method(proto, "hasOwnProperty", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        Ref<GCObject> key = toPropertyKey(rt2, ci.arg(0));
        DynObject* self = asObject(ci.thisValue());
        if (!self)
            return Value(false);
        if (self->cls == ObjClass::Array && key->kind == CellKind::String) {
            std::uint32_t index = 0;
            if (arrayIndexOf(static_cast<StringCell*>(key.get())->text(), index))
                return Value(index < static_cast<DynArray*>(self)->elements.size());
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
        DynObject* self = asObject(ci.thisValue());
        if (!self)
            return Value(false);
        Ref<GCObject> key = toPropertyKey(rt2, ci.arg(0));
        const Prop* p = self->findOwn(key.get());
        return Value(p && (self->attrsOf(*p) & PropEnumerable));
    }, 1);

    rt.method(proto, "toString", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        if (ci.thisValue().isUndefined())
            return rt2.str("[object Undefined]");
        if (ci.thisValue().isNull())
            return rt2.str("[object Null]");
        DynObject* self = asObject(ci.thisValue());
        const char* tag = self ? classTag(self->cls) : "Object";
        return rt2.str(ascii(std::string("[object ") + tag + "]"));
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
        DynObject* self = asObject(toObject(rt2, ci.arg(0)));
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
            for (std::size_t i = 0; i < self->props.size(); ++i)
                self->setAttrsAt(i, std::uint8_t(self->attrsAt(i) & ~(PropWritable | PropConfigurable)));
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
            buffer.args.assign(arr->elements.begin(), arr->elements.end());
        } else if (!ci.arg(1).isNullish()) {
            const Vec<Value> items = iterableToVector(rt2, ci.arg(1));
            buffer.args.assign(items.begin(), items.end());
        }
        return callValue(rt2, ci.thisValue(), ci.arg(0), buffer.args);
    }, 2);

    rt.intrinsics[Runtime::FunctionApply].reset(detail::Access::ptr(proto->findOwn(rt.intern("apply"))->value));

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
        ci.fail(ErrorCode::TypeError,
                "the Function constructor is disabled; the engine never compiles from strings at "
                "runtime");
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

    rt.method(ctor, "for", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        const String key = toStringValue(rt2, ci.arg(0));
        auto it = rt2.symbolRegistry.find(key);
        if (it != rt2.symbolRegistry.end())
            return detail::Access::make(Type::Symbol, it->second);
        auto* s = rt2.newCell<SymbolCell>();
        s->description = key;
        s->registered = true;
        rt2.symbolRegistry.emplace(key, s);
        return detail::Access::adopt(Type::Symbol, s);
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
        const String s = toStringValue(rt2, ci.arg(0));
        const int radix = ci.arg(1).isUndefined() ? 0 : int(toNumber(rt2, ci.arg(1)));
        return Value(parseIntPrefix(s, radix));
    }, 2);

    rt.method(g, "parseFloat", [](CallInfo& ci) {
        Runtime& rt2 = rtOf(ci);
        return Value(parseFloatPrefix(toStringValue(rt2, ci.arg(0))));
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
}

}  // namespace dyn
}  // namespace internal
}  // namespace script
