// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "script/internal/vm/host.hpp"

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
    if (raised == ErrorCode::StackOverflow) {
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
    Value value = toValue(vm, thrown);
    throw Exception(code, message, value, std::string());
}

}  // namespace host
}  // namespace vm
}  // namespace internal
}  // namespace script
