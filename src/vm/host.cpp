// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "script/internal/vm/host.hpp"

#include <algorithm>
#include <memory>
#include <mutex>

#include "script/internal/vm/convert.hpp"
#include "script/internal/vm/operators.hpp"

namespace script {
namespace internal {
namespace vm {
namespace host {

namespace {

/// The context lock, for the length of one operation, when the context asks for it.
class Locked
{
public:
    explicit Locked(ContextImpl& rt) : mtx_(rt.options.threadSafe ? &rt.mtx : nullptr)
    {
        if (mtx_)
            mtx_->lock();
    }
    ~Locked()
    {
        if (mtx_)
            mtx_->unlock();
    }
    Locked(const Locked&) = delete;
    Locked& operator=(const Locked&) = delete;

private:
    std::recursive_mutex* mtx_;
};

VM& vmFor(const Value& v) noexcept
{
    return vmOf(detail::Access::ptr(v));
}

/// A result of the VM as a `Value`: raises what is pending.
Value finish(VM& vm, JSValue r)
{
    if (r.isException())
        raisePending(vm);
    Value out = toValue(vm, r);
    release(r);
    return out;
}

Key keyOf(VM& vm, std::string_view text)
{
    return vm.internKey(fromUtf8(text));
}

void quote(std::string& out, const std::string& s)
{
    out += '\'';
    out += s;
    out += '\'';
}

void render(VM& vm, JSValue v, int depth, std::string& out)
{
    if (isString(v)) {
        quote(out, toUtf8(asString(v)->text()));
        return;
    }
    if (!isObject(v)) {
        out += describe(vm, v);
        return;
    }
    if (isFunction(v)) {
        JSValue name = getProperty(vm, v, vm.atoms.name);
        std::string text = name.isException() ? "" : describe(vm, name);
        if (name.isException())
            vm.clearException();
        else
            release(name);
        out += text.empty() ? "[Function (anonymous)]" : "[Function: " + text + "]";
        return;
    }
    JSObject* o = asObject(v);
    if (o->cls() == ObjClass::Error) {
        JSValue s = toStringValue(vm, v);
        if (s.isException()) {
            vm.clearException();
            out += "[Error]";
        } else {
            out += toUtf8(asString(s)->text());
            release(s);
        }
        return;
    }
    if (depth > 2) {
        out += isArray(v) ? "[Array]" : "[Object]";
        return;
    }
    if (isArray(v)) {
        auto* a = static_cast<JSArray*>(o);
        out += "[";
        for (std::uint32_t i = 0; i < a->length; ++i) {
            out += i ? ", " : " ";
            if (a->elems[i].isHole())
                out += "<empty>";
            else
                render(vm, a->elems[i], depth + 1, out);
        }
        out += a->length ? " ]" : "]";
        return;
    }
    out += "{";
    bool first = true;
    o->forEachOwn(vm, [&](Key k, std::uint8_t attrs, JSValue value) {
        if (!(attrs & attr::kEnumerable) || k->kind != detail::CellKind::String)
            return;
        out += first ? " " : ", ";
        first = false;
        out += toUtf8(static_cast<StringCell*>(k)->text()) + ": ";
        if (attrs & attr::kAccessor)
            out += "[Getter/Setter]";
        else
            render(vm, value, depth + 1, out);
    });
    out += first ? "}" : " }";
}

std::vector<JSValue> argsOf(VM& vm, const std::vector<Value>& args, std::vector<Handle>& keep)
{
    std::vector<JSValue> out;
    for (const Value& a : args) {
        keep.push_back(Handle::adopt(fromValue(vm, a)));
        out.push_back(keep.back().get());
    }
    return out;
}

}  // namespace

std::string inspectValue(VM& vm, JSValue v)
{
    std::string out;
    render(vm, v, 0, out);
    return out;
}

ContextImpl& contextOf(const Value& v) noexcept
{
    return vmFor(v).ctx;
}

bool isArray(const Value& v) noexcept
{
    return vm::isArray(JSValue::cell(detail::Access::ptr(v)));
}

bool isFunction(const Value& v) noexcept
{
    return vm::isFunction(JSValue::cell(detail::Access::ptr(v)));
}

bool isError(const Value& v) noexcept
{
    return asObject(JSValue::cell(detail::Access::ptr(v)))->cls() == ObjClass::Error;
}

std::string typeName(const Value& v)
{
    return isFunction(v) ? "function" : script::toString(v.type());
}

double toDouble(const Value& v)
{
    VM& vm = vmFor(v);
    Locked lock(vm.ctx);
    Handle h = Handle::adopt(fromValue(vm, v));
    double d;
    if (!toNumber(vm, h.get(), &d))
        raisePending(vm);
    return d;
}

std::string toString(const Value& v)
{
    VM& vm = vmFor(v);
    Locked lock(vm.ctx);
    Handle h = Handle::adopt(fromValue(vm, v));
    JSValue s = toStringValue(vm, h.get());
    if (s.isException())
        raisePending(vm);
    std::string out = toUtf8(asString(s)->text());
    release(s);
    return out;
}

std::string inspect(const Value& v)
{
    VM& vm = vmFor(v);
    Locked lock(vm.ctx);
    Handle h = Handle::adopt(fromValue(vm, v));
    std::string out;
    render(vm, h.get(), 0, out);
    return out;
}

Value get(const Value& v, std::string_view key)
{
    VM& vm = vmFor(v);
    Locked lock(vm.ctx);
    Handle self = Handle::adopt(fromValue(vm, v));
    Key k = keyOf(vm, key);
    JSValue r = getProperty(vm, self.get(), k);
    detail::decRef(k);
    return finish(vm, r);
}

Value get(const Value& v, std::uint32_t index)
{
    VM& vm = vmFor(v);
    Locked lock(vm.ctx);
    Handle self = Handle::adopt(fromValue(vm, v));
    Key k = toPropertyKey(vm, JSValue::boxNumber(double(index)));
    JSValue r = getProperty(vm, self.get(), k);
    detail::decRef(k);
    return finish(vm, r);
}

Value get(const Value& v, const Value& key)
{
    VM& vm = vmFor(v);
    Locked lock(vm.ctx);
    Handle self = Handle::adopt(fromValue(vm, v));
    Handle kv = Handle::adopt(fromValue(vm, key));
    JSValue prim = toPrimitive(vm, kv.get(), true);
    if (prim.isException())
        raisePending(vm);
    Key k = toPropertyKey(vm, prim);
    release(prim);
    if (!k)
        raisePending(vm);
    JSValue r = getProperty(vm, self.get(), k);
    detail::decRef(k);
    return finish(vm, r);
}

namespace {

void store(VM& vm, JSValue self, Key k, const Value& value)
{
    Handle val = Handle::adopt(fromValue(vm, value));
    const bool ok = setProperty(vm, self, k, val.get(), true);
    detail::decRef(k);
    if (!ok && !vm.exception.isUndefined())
        raisePending(vm);
}

}  // namespace

void set(const Value& v, std::string_view key, const Value& value)
{
    VM& vm = vmFor(v);
    Locked lock(vm.ctx);
    Handle self = Handle::adopt(fromValue(vm, v));
    store(vm, self.get(), keyOf(vm, key), value);
}

void set(const Value& v, std::uint32_t index, const Value& value)
{
    VM& vm = vmFor(v);
    Locked lock(vm.ctx);
    Handle self = Handle::adopt(fromValue(vm, v));
    store(vm, self.get(), toPropertyKey(vm, JSValue::boxNumber(double(index))), value);
}

void set(const Value& v, const Value& key, const Value& value)
{
    VM& vm = vmFor(v);
    Locked lock(vm.ctx);
    Handle self = Handle::adopt(fromValue(vm, v));
    Handle kv = Handle::adopt(fromValue(vm, key));
    JSValue prim = toPrimitive(vm, kv.get(), true);
    if (prim.isException())
        raisePending(vm);
    Key k = toPropertyKey(vm, prim);
    release(prim);
    if (!k)
        raisePending(vm);
    store(vm, self.get(), k, value);
}

bool has(const Value& v, std::string_view key)
{
    VM& vm = vmFor(v);
    Locked lock(vm.ctx);
    Handle self = Handle::adopt(fromValue(vm, v));
    Key k = keyOf(vm, key);
    const bool r = hasProperty(vm, self.get(), k);
    detail::decRef(k);
    return r;
}

bool hasOwn(const Value& v, std::string_view key)
{
    VM& vm = vmFor(v);
    Locked lock(vm.ctx);
    JSObject* o = asObject(JSValue::cell(detail::Access::ptr(v)));
    Key k = keyOf(vm, key);
    bool r = o->findOwn(vm, k) != nullptr;
    std::uint32_t index;
    if (o->cls() == ObjClass::Array) {
        auto* a = static_cast<JSArray*>(o);
        r = r || k == vm.atoms.length || (arrayIndexOf(k, &index) && !a->at(index).isHole());
    }
    detail::decRef(k);
    return r;
}

bool remove(const Value& v, std::string_view key)
{
    VM& vm = vmFor(v);
    Locked lock(vm.ctx);
    Handle self = Handle::adopt(fromValue(vm, v));
    Key k = keyOf(vm, key);
    const bool r = deleteProperty(vm, self.get(), k, false);
    detail::decRef(k);
    return r;
}

std::vector<std::string> keys(const Value& v)
{
    VM& vm = vmFor(v);
    Locked lock(vm.ctx);
    JSObject* o = asObject(JSValue::cell(detail::Access::ptr(v)));
    std::vector<std::string> out;
    if (o->cls() == ObjClass::Array) {
        auto* a = static_cast<JSArray*>(o);
        for (std::uint32_t i = 0; i < a->length; ++i)
            if (!a->elems[i].isHole())
                out.push_back(std::to_string(i));
    }
    o->forEachOwn(vm, [&](Key k, std::uint8_t attrs, JSValue) {
        if ((attrs & attr::kEnumerable) && k->kind == detail::CellKind::String)
            out.push_back(toUtf8(static_cast<StringCell*>(k)->text()));
    });
    return out;
}

std::uint32_t length(const Value& v)
{
    if (isArray(v))
        return static_cast<JSArray*>(asObject(JSValue::cell(detail::Access::ptr(v))))->length;
    VM& vm = vmFor(v);
    Locked lock(vm.ctx);
    Handle self = Handle::adopt(fromValue(vm, v));
    JSValue r = getProperty(vm, self.get(), vm.atoms.length);
    if (r.isException())
        raisePending(vm);
    const std::uint32_t n = r.isNumber() ? std::uint32_t(r.asNumber()) : 0;
    release(r);
    return n;
}

void push(const Value& v, const Value& value)
{
    VM& vm = vmFor(v);
    Locked lock(vm.ctx);
    if (!isArray(v))
        throw Error(ErrorCode::NotSupported, "push needs an array");
    auto* a = static_cast<JSArray*>(asObject(JSValue::cell(detail::Access::ptr(v))));
    a->append(vm, fromValue(vm, value));
}

void defineProperty(const Value& v, std::string_view key, const Value& value, PropertyFlags flags)
{
    VM& vm = vmFor(v);
    Locked lock(vm.ctx);
    JSObject* o = asObject(JSValue::cell(detail::Access::ptr(v)));
    Key k = keyOf(vm, key);
    const bool ok = defineOwn(vm, o, k, fromValue(vm, value), std::uint8_t(flags));
    detail::decRef(k);
    if (!ok)
        throw Error(ErrorCode::TypeError, "Cannot redefine property: " + std::string(key));
}

void defineAccessor(const Value& v, std::string_view key, const Value& getterFn, const Value& setterFn, PropertyFlags flags)
{
    VM& vm = vmFor(v);
    Locked lock(vm.ctx);
    JSObject* o = asObject(JSValue::cell(detail::Access::ptr(v)));
    Handle getter = Handle::adopt(fromValue(vm, getterFn));
    Handle setter = Handle::adopt(fromValue(vm, setterFn));
    Key k = keyOf(vm, key);
    const bool ok = vm::defineAccessor(vm, o, k, getter.get(), setter.get(), std::uint8_t(std::uint8_t(flags) & ~attr::kWritable));
    detail::decRef(k);
    if (!ok)
        throw Error(ErrorCode::TypeError, "Cannot redefine property: " + std::string(key));
}

std::string toJson(const Value& v, int indent)
{
    VM& vm = vmFor(v);
    Locked lock(vm.ctx);
    Key json = keyOf(vm, "JSON"), stringify = keyOf(vm, "stringify");
    JSValue object = getProperty(vm, toJSValue(vm.global), json);
    detail::decRef(json);
    if (object.isException())
        raisePending(vm);
    Handle objectKeep = Handle::adopt(object);
    JSValue fn = getProperty(vm, object, stringify);
    detail::decRef(stringify);
    if (fn.isException())
        raisePending(vm);
    Handle fnKeep = Handle::adopt(fn);
    Handle value = Handle::adopt(fromValue(vm, v));
    const JSValue args[3] = {value.get(), JSValue::undefined(), JSValue::int32(indent < 0 ? 0 : indent > 10 ? 10 : indent)};
    JSValue r = vm.call(fn, object, args, 3);
    if (r.isException())
        raisePending(vm);
    Handle result = Handle::adopt(r);
    return isString(r) ? toUtf8(asString(r)->text()) : std::string();
}

Value prototype(const Value& v)
{
    VM& vm = vmFor(v);
    JSObject* p = asObject(JSValue::cell(detail::Access::ptr(v)))->proto(vm);
    return p ? detail::Access::make(Type::Object, reinterpret_cast<CellPtr>(p)) : Value::null();
}

void setPrototype(const Value& v, const Value& proto)
{
    VM& vm = vmFor(v);
    Locked lock(vm.ctx);
    JSObject* o = asObject(JSValue::cell(detail::Access::ptr(v)));
    JSObject* p = owns(proto) ? asObject(JSValue::cell(detail::Access::ptr(proto))) : nullptr;
    if (p && o->wouldCycle(vm, p))
        throw Error(ErrorCode::TypeError, "cyclic prototype chain");
    o->setProto(vm, p);
}

Value call(const Value& fn, const Value& thisValue, const std::vector<Value>& args)
{
    VM& vm = vmFor(fn);
    Locked lock(vm.ctx);
    ContextImpl::ActiveEngine active(vm.ctx, EngineKind::Vm);
    Handle callee = Handle::adopt(fromValue(vm, fn));
    Handle self = Handle::adopt(fromValue(vm, thisValue));
    std::vector<Handle> keep;
    std::vector<JSValue> argv = argsOf(vm, args, keep);
    return finish(vm, vm.call(callee.get(), self.get(), argv.data(), std::uint32_t(argv.size())));
}

Value invoke(const Value& v, std::string_view method, const std::vector<Value>& args)
{
    VM& vm = vmFor(v);
    Locked lock(vm.ctx);
    ContextImpl::ActiveEngine active(vm.ctx, EngineKind::Vm);
    Handle self = Handle::adopt(fromValue(vm, v));
    Key k = keyOf(vm, method);
    JSValue fn = getProperty(vm, self.get(), k);
    detail::decRef(k);
    if (fn.isException())
        raisePending(vm);
    Handle callee = Handle::adopt(fn);
    std::vector<Handle> keep;
    std::vector<JSValue> argv = argsOf(vm, args, keep);
    return finish(vm, vm.call(callee.get(), self.get(), argv.data(), std::uint32_t(argv.size())));
}

Value construct(const Value& fn, const std::vector<Value>& args)
{
    VM& vm = vmFor(fn);
    Locked lock(vm.ctx);
    ContextImpl::ActiveEngine active(vm.ctx, EngineKind::Vm);
    Handle callee = Handle::adopt(fromValue(vm, fn));
    std::vector<Handle> keep;
    std::vector<JSValue> argv = argsOf(vm, args, keep);
    return finish(vm, vm.construct(callee.get(), argv.data(), std::uint32_t(argv.size())));
}

void raisePending(VM& vm)
{
    JSValue thrown = vm.exception;
    vm.exception = JSValue::undefined();
    const ErrorCode raised = vm.exceptionCode;
    vm.exceptionCode = ErrorCode::Ok;
    Handle keep = Handle::adopt(thrown);
    std::string message;
    JSValue text = toStringValue(vm, thrown);
    if (text.isException()) {
        vm.clearException();
        message = "uncaught exception";
    } else {
        message = toUtf8(asString(text)->text());
        release(text);
    }
    // An error keeps its class across the boundary: the code is the one whose prototype it inherits from.
    ErrorCode code = ErrorCode::Exception;
    if (raised == ErrorCode::StackOverflow || raised == ErrorCode::Interrupted) {
        code = raised;
    } else if (isObject(thrown)) {
        for (JSObject* p = asObject(thrown)->proto(vm); p && code == ErrorCode::Exception; p = p->proto(vm)) {
            for (std::size_t i = 0; i < 48; ++i) {
                if (vm.errorProtos[i] == p && i != std::size_t(ErrorCode::Exception) && i != std::size_t(ErrorCode::StackOverflow) &&
                    i != std::size_t(ErrorCode::InternalError)) {
                    code = ErrorCode(i);
                    break;
                }
            }
        }
    }
    // The frames an error captured when it was made, without the heading line.
    std::string frames;
    if (isObject(thrown)) {
        JSValue stack = getProperty(vm, thrown, vm.internKey(std::string_view("stack")));
        if (!stack.isException()) {
            if (isString(stack)) {
                const std::string whole = toUtf8(asString(stack)->text());
                const std::size_t at = whole.find("\n    at ");
                if (at != std::string::npos)
                    frames = whole.substr(at + 1);
            }
            release(stack);
        } else {
            vm.clearException();
        }
    }
    Value value = toValue(vm, thrown);
    throw Exception(code, message, value, frames);
}


// ---------------------------------------------------------------- what Context makes ---

namespace {

/// A function the host gave (a `NativeFunction`), kept with the function object that calls it.
struct HostFunction : NativeState
{
    NativeFunction fn;
};

/// The C++ instance a wrapper object stands for: what `Context::native` and `attachNative` set.
struct HostInstance : NativeState
{
    const void* typeKey = nullptr;
    void* data = nullptr;
    void* owner = nullptr;
    Context::Finalizer finalizer = nullptr;
    ~HostInstance() override
    {
        if (finalizer) {
            Context::Finalizer run = finalizer;
            finalizer = nullptr;
            run(owner ? owner : data);
        }
    }
};

/// What a host function does with an exception of its own.
JSValue hostCall(VM& vm, JSValue self, const JSValue* args, std::uint32_t argc, void* data)
{
    auto* state = static_cast<HostFunction*>(data);
    ContextImpl& ctx = vm.ctx;
    ContextImpl::ArgBuffer buffer(ctx);
    std::vector<Value>& values = buffer.args;
    values.reserve(argc);
    for (std::uint32_t i = 0; i < argc; ++i)
        values.push_back(toValue(vm, args[i]));
    const Value thisValue = toValue(vm, self);
    const bool isNew = !vm.newTarget.isUndefined();
    CallInfo info = detail::Access::makeCallInfo(ctx.owner, &thisValue, &values, isNew);
    ContextImpl::ActiveEngine active(ctx, EngineKind::Vm);
    try {
        const Value result = state->fn(info);
        return fromValue(vm, result);
    } catch (const Exception& e) {
        vm.clearException();
        vm.exception = fromValue(vm, e.value());
        vm.exceptionCode = e.code();
    } catch (const Error& e) {
        vm.throwError(e.code(), e.what());
    } catch (const std::exception& e) {
        vm.throwError(ErrorCode::InternalError, e.what());
    }
    return JSValue::exception();
}

/// A function object for `fn`, with the count its creation gave.
JSFunction* newHostFunction(VM& vm, std::string_view name, NativeFunction fn, int arity)
{
    auto holder = std::make_unique<HostFunction>();
    holder->fn = std::move(fn);
    HostFunction* state = holder.get();
    Key key = vm.internKey(fromUtf8(name));
    JSFunction* f = vm.newNativeFunction(vm.protos.function, hostCall, state, key, std::uint32_t(arity < 0 ? 0 : arity));
    detail::decRef(key);
    f->rareOf(vm).ext = holder.release();
    // What a host hands over may well be called with `new`.
    f->flags |= JSFunction::kConstructor;
    return f;
}

Value adopt(VM& vm, JSValue created)
{
    Value out = toValue(vm, created);
    release(created);
    return out;
}

HostInstance* instanceOf(JSValue v) noexcept
{
    if (!isObject(v))
        return nullptr;
    Rare* r = asObject(v)->rare();
    return r && r->ext ? dynamic_cast<HostInstance*>(r->ext) : nullptr;
}

void installMember(VM& vm, JSObject* target, JSObject* proto, const HostClassInfo::Member& member)
{
    const HostMember& spec = member.spec;
    const int arity = int(spec.csharpParams.size());
    JSObject* where = spec.isStatic || spec.kind == HostMember::Constant ? target : proto;
    if (spec.kind == HostMember::Constructor) {
        auto* ctor = static_cast<JSFunction*>(target);
        static_cast<HostFunction*>(ctor->rareOf(vm).ext)->fn = member.fn;
        ctor->rareOf(vm).fnLength = std::uint32_t(arity);
        if (!(ctor->flags & JSFunction::kLazyProps)) {
            Key k = vm.atoms.length;
            if (JSValue* slot = ctor->findOwn(vm, k))
                *slot = JSValue::int32(arity);
        }
        return;
    }
    if (!where)
        return;
    Key key = vm.internKey(fromUtf8(spec.name));
    if (spec.kind == HostMember::Method) {
        JSFunction* f = newHostFunction(vm, spec.name, member.fn, arity);
        where->putOwn(vm, key, toJSValue(f), attr::kWritable | attr::kConfigurable);
    } else {
        JSFunction* getter = newHostFunction(vm, "get " + spec.name, member.fn, 0);
        Handle getterKeep = Handle::adopt(toJSValue(getter));
        JSValue setter = JSValue::undefined();
        Handle setterKeep;
        if (member.setter) {
            JSFunction* s = newHostFunction(vm, "set " + spec.name, member.setter, 1);
            setterKeep = Handle::adopt(toJSValue(s));
            setter = toJSValue(s);
        }
        defineAccessor(vm, where, key, toJSValue(getter), setter, attr::kConfigurable | attr::kEnumerable);
    }
    detail::decRef(key);
}

}  // namespace

void syncHostClasses(ContextImpl& ctx)
{
    VM& vm = ctx.vm();
    for (auto& owned : ctx.hostClasses) {
        HostClassInfo& info = *owned;
        auto it = vm.hostBindings.find(&info);
        if (it == vm.hostBindings.end()) {
            JSObject* target;
            JSObject* proto = nullptr;
            if (info.typeKey) {
                proto = vm.newObject(vm.protos.object);
                JSFunction* ctor = newHostFunction(
                    vm, info.name, [](CallInfo& ci) -> Value { ci.fail(ErrorCode::TypeError, "this class has no constructor"); }, 0);
                ctor->materialize(vm);
                detail::incRef(reinterpret_cast<CellPtr>(proto));
                ctor->putOwn(vm, vm.atoms.prototype, toJSValue(proto), 0);
                detail::incRef(reinterpret_cast<CellPtr>(ctor));
                proto->putOwn(vm, vm.atoms.constructor, toJSValue(ctor), attr::kWritable | attr::kConfigurable);
                target = ctor;
            } else {
                target = vm.newObject(vm.protos.object);
            }
            Key name = vm.internKey(fromUtf8(info.name));
            detail::incRef(reinterpret_cast<CellPtr>(target));
            vm.global->putOwn(vm, name, toJSValue(target), attr::kWritable | attr::kConfigurable);
            detail::decRef(name);
            it = vm.hostBindings.emplace(&info, std::make_pair(target, proto)).first;
        }
        std::size_t& done = info.installed[std::size_t(EngineKind::Vm) - 1];
        for (; done < info.members.size(); ++done)
            installMember(vm, it->second.first, it->second.second, info.members[done]);
    }
}

Value global(ContextImpl& ctx)
{
    VM& vm = ctx.vm();
    Locked lock(ctx);
    syncHostClasses(ctx);
    return toValue(vm, toJSValue(vm.global));
}

Value makeSymbol(ContextImpl& ctx, std::string_view description)
{
    VM& vm = ctx.vm();
    Locked lock(ctx);
    return adopt(vm, vm.newSymbol(fromUtf8(description), true));
}

Value makeObject(ContextImpl& ctx)
{
    VM& vm = ctx.vm();
    Locked lock(ctx);
    return adopt(vm, toJSValue(vm.newObject(vm.protos.object)));
}

Value makeArray(ContextImpl& ctx, std::size_t reserve)
{
    VM& vm = ctx.vm();
    Locked lock(ctx);
    return adopt(vm, toJSValue(vm.newArray(vm.protos.array, 0, std::uint32_t(std::min<std::size_t>(reserve, 1u << 20)))));
}

Value makeArray(ContextImpl& ctx, const std::vector<Value>& items)
{
    VM& vm = ctx.vm();
    Locked lock(ctx);
    JSArray* a = vm.newArray(vm.protos.array, 0, std::uint32_t(items.size()));
    Handle keep = Handle::adopt(toJSValue(a));
    for (const Value& item : items)
        a->append(vm, fromValue(vm, item));
    return toValue(vm, keep.get());
}

Value makeFunction(ContextImpl& ctx, std::string_view name, NativeFunction fn, int arity)
{
    VM& vm = ctx.vm();
    Locked lock(ctx);
    return adopt(vm, toJSValue(newHostFunction(vm, name, std::move(fn), arity)));
}

Value makeException(ContextImpl& ctx, ErrorCode code, std::string_view message)
{
    VM& vm = ctx.vm();
    Locked lock(ctx);
    return adopt(vm, vm.makeError(code, message));
}

Value parseJson(ContextImpl& ctx, std::string_view text)
{
    VM& vm = ctx.vm();
    Locked lock(ctx);
    Key json = keyOf(vm, "JSON"), parse = keyOf(vm, "parse");
    JSValue object = getProperty(vm, toJSValue(vm.global), json);
    detail::decRef(json);
    if (object.isException())
        raisePending(vm);
    Handle objectKeep = Handle::adopt(object);
    JSValue fn = getProperty(vm, object, parse);
    detail::decRef(parse);
    if (fn.isException())
        raisePending(vm);
    Handle fnKeep = Handle::adopt(fn);
    Handle source = Handle::adopt(makeString(vm, std::string_view(text)));
    const JSValue arg0 = source.get();
    JSValue r = vm.call(fn, object, &arg0, 1);
    return finish(vm, r);
}

Value hostWrapper(ContextImpl& ctx, const void* typeKey, void* instance, Context::Finalizer finalizer, void* owner)
{
    VM& vm = ctx.vm();
    Locked lock(ctx);
    syncHostClasses(ctx);
    JSObject* proto = vm.protos.object;
    if (const HostClassInfo* info = ctx.hostClassForKey(typeKey)) {
        const auto it = vm.hostBindings.find(info);
        if (it != vm.hostBindings.end() && it->second.second)
            proto = it->second.second;
    }
    JSObject* o = vm.newObject(proto);
    Handle keep = Handle::adopt(toJSValue(o));
    auto state = std::make_unique<HostInstance>();
    state->typeKey = typeKey;
    state->data = instance;
    state->owner = owner;
    state->finalizer = finalizer;
    o->rareOf(vm).ext = state.release();
    return toValue(vm, keep.get());
}

void attachNative(ContextImpl& ctx, const Value& object, const void* typeKey, void* instance, Context::Finalizer finalizer, void* owner)
{
    VM& vm = ctx.vm();
    Locked lock(ctx);
    Handle self = Handle::adopt(fromValue(vm, object));
    if (!isObject(self.get()))
        throw Error(ErrorCode::TypeError, "attachNative needs a JavaScript object");
    Rare& rare = asObject(self.get())->rareOf(vm);
    if (rare.ext) {
        delete rare.ext;
        rare.ext = nullptr;
    }
    auto state = std::make_unique<HostInstance>();
    state->typeKey = typeKey;
    state->data = instance;
    state->owner = owner;
    state->finalizer = finalizer;
    rare.ext = state.release();
}

Value hostPrototype(ContextImpl& ctx, const void* typeKey)
{
    VM& vm = ctx.vm();
    Locked lock(ctx);
    syncHostClasses(ctx);
    const HostClassInfo* info = ctx.hostClassForKey(typeKey);
    const auto it = info ? vm.hostBindings.find(info) : vm.hostBindings.end();
    if (it == vm.hostBindings.end() || !it->second.second)
        throw Error(ErrorCode::TypeError, "this C++ type has no script class; bind it with Class<T> first");
    return toValue(vm, toJSValue(it->second.second));
}

Value hostConstructor(ContextImpl& ctx, const void* typeKey)
{
    VM& vm = ctx.vm();
    Locked lock(ctx);
    syncHostClasses(ctx);
    const HostClassInfo* info = ctx.hostClassForKey(typeKey);
    const auto it = info ? vm.hostBindings.find(info) : vm.hostBindings.end();
    if (it == vm.hostBindings.end())
        throw Error(ErrorCode::TypeError, "this C++ type has no script class; bind it with Class<T> first");
    return toValue(vm, toJSValue(it->second.first));
}

bool isNative(const Value& v) noexcept
{
    return instanceOf(JSValue::cell(detail::Access::ptr(v))) != nullptr;
}

void* nativeHandle(const Value& v, const void* typeKey) noexcept
{
    const HostInstance* i = instanceOf(JSValue::cell(detail::Access::ptr(v)));
    return i && i->typeKey == typeKey ? i->data : nullptr;
}

}  // namespace host
}  // namespace vm
}  // namespace internal
}  // namespace script
