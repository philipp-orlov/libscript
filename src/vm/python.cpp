// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Python on the new core: the Python parser lowers a program to the ECMAScript syntax tree, the vm compiler turns it into
// bytecode, and the helpers the lowered program calls (`__py`: indexing, `len`, truth tests, output) are natives here. The
// prelude that defines the built-in functions and the rest is the one the other core runs, as JavaScript.
#include <cmath>

#include "script/internal/dyn/numconv.hpp"
#include "script/internal/python/parser.hpp"
#include "script/internal/python/prelude.hpp"
#include "script/internal/vm/compiler.hpp"
#include "script/internal/vm/convert.hpp"
#include "script/internal/vm/host.hpp"
#include "script/internal/vm/operators.hpp"
#include "script/internal/vm/vm.hpp"
#include "builtins_util.hpp"

namespace script {
namespace internal {
namespace vm {

namespace {

using bi::arg;

/// What `typeof` says; the messages of the helpers use it as the dynamic engine's do.
const char* typeName(JSValue v)
{
    if (v.isUndefined())
        return "undefined";
    if (v.isNull())
        return "object";
    if (v.isBool())
        return "boolean";
    if (v.isNumber())
        return "number";
    if (isString(v))
        return "string";
    if (isBigInt(v))
        return "bigint";
    if (isSymbol(v))
        return "symbol";
    return isFunction(v) ? "function" : "object";
}

/// Raise a Python exception class the prelude defined, so `except IndexError` catches what `__py.getitem` raises.
JSValue raise(VM& vm, const char* className, const std::string& message)
{
    Key name = vm.internKey(std::string_view(className));
    JSValue* slot = vm.global->findOwn(vm, name);
    detail::decRef(name);
    if (!slot || !isFunction(*slot))
        return vm.throwError(ErrorCode::TypeError, message);
    JSValue text = makeString(vm, std::string_view(message));
    JSValue error = vm.construct(*slot, &text, 1);
    release(text);
    if (error.isException())
        return error;
    release(vm.exception);
    vm.exception = error;
    vm.exceptionCode = ErrorCode::Exception;
    return JSValue::exception();
}

bool hasMethod(VM& vm, JSValue v, const char* name)
{
    if (!isObject(v))
        return false;
    Key key = vm.internKey(std::string_view(name));
    const bool has = hasProperty(vm, v, key);
    detail::decRef(key);
    return has;
}

JSValue invoke(VM& vm, JSValue self, const char* name, const JSValue* args, std::uint32_t argc)
{
    Key key = vm.internKey(std::string_view(name));
    JSValue f = getProperty(vm, self, key);
    detail::decRef(key);
    if (f.isException())
        return f;
    Handle keep = Handle::adopt(f);
    if (!isFunction(f))
        return vm.throwError(ErrorCode::TypeError, std::string("'") + name + "' is not a function");
    return vm.call(f, self, args, argc);
}

/// A list or string index: an integer, negative counting from the end. False with an exception pending on a bad kind.
int resolveIndex(VM& vm, JSValue index, std::size_t length, std::size_t* out)
{
    if (!index.isNumber()) {
        raise(vm, "TypeError", std::string("indices must be integers, not ") + typeName(index));
        return -1;
    }
    const double d = index.asNumber();
    if (d != std::floor(d)) {
        raise(vm, "TypeError", "indices must be integers, not float");
        return -1;
    }
    const double i = d < 0 ? d + double(length) : d;
    if (i < 0 || i >= double(length))
        return 0;
    *out = std::size_t(i);
    return 1;
}

JSValue pyGetitem(VM& vm, JSValue, const JSValue* args, std::uint32_t argc, void*)
{
    const JSValue base = arg(args, argc, 0), index = arg(args, argc, 1);
    if (hasElems(base) && asObject(base)->cls() == ObjClass::Array) {
        auto* a = static_cast<JSArray*>(asObject(base));
        std::size_t i = 0;
        const int found = resolveIndex(vm, index, a->length, &i);
        if (found < 0)
            return JSValue::exception();
        if (!found)
            return raise(vm, "IndexError", "list index out of range");
        const JSValue e = a->at(std::uint32_t(i));
        retain(e);
        return e.isHole() ? JSValue::undefined() : e;
    }
    if (isString(base)) {
        std::size_t i = 0;
        const int found = resolveIndex(vm, index, asString(base)->length, &i);
        if (found < 0)
            return JSValue::exception();
        if (!found)
            return raise(vm, "IndexError", "string index out of range");
        return makeString(vm, asString(base)->text().substr(i, 1));
    }
    if (isObject(base) && asObject(base)->cls() == ObjClass::Map) {
        JSValue out;
        if (!bi::mapLookup(base, index, &out))
            return raise(vm, "KeyError", describe(vm, index));
        return out;
    }
    if (hasMethod(vm, base, "__getitem__"))
        return invoke(vm, base, "__getitem__", &index, 1);
    if (base.isNullish())
        return raise(vm, "TypeError", "'NoneType' object is not subscriptable");
    // A host object or a plain JavaScript object: ordinary property access.
    Key key = toPropertyKey(vm, index);
    if (!key)
        return JSValue::exception();
    JSValue v = getProperty(vm, base, key);
    detail::decRef(key);
    return v;
}

JSValue pySetitem(VM& vm, JSValue, const JSValue* args, std::uint32_t argc, void*)
{
    const JSValue base = arg(args, argc, 0), index = arg(args, argc, 1), value = arg(args, argc, 2);
    if (hasElems(base) && asObject(base)->cls() == ObjClass::Array) {
        auto* a = static_cast<JSArray*>(asObject(base));
        std::size_t i = 0;
        const int found = resolveIndex(vm, index, a->length, &i);
        if (found < 0)
            return JSValue::exception();
        if (!found)
            return raise(vm, "IndexError", "list assignment index out of range");
        retain(value);
        a->setAt(vm, std::uint32_t(i), value);
        retain(value);
        return value;
    }
    if (isObject(base) && asObject(base)->cls() == ObjClass::Map) {
        bi::mapStore(base, index, value);
        retain(value);
        return value;
    }
    if (isString(base))
        return raise(vm, "TypeError", "'str' object does not support item assignment");
    if (hasMethod(vm, base, "__setitem__")) {
        const JSValue pair[2] = {index, value};
        return invoke(vm, base, "__setitem__", pair, 2);
    }
    if (base.isNullish())
        return raise(vm, "TypeError", "'NoneType' object does not support item assignment");
    Key key = toPropertyKey(vm, index);
    if (!key)
        return JSValue::exception();
    const bool ok = setProperty(vm, base, key, value, true);
    detail::decRef(key);
    if (!ok)
        return JSValue::exception();
    retain(value);
    return value;
}

JSValue pyLen(VM& vm, JSValue, const JSValue* args, std::uint32_t argc, void*)
{
    const JSValue v = arg(args, argc, 0);
    if (hasElems(v) && asObject(v)->cls() == ObjClass::Array)
        return JSValue::boxNumber(double(static_cast<JSArray*>(asObject(v))->length));
    if (isString(v))
        return JSValue::boxNumber(double(asString(v)->length));
    for (ObjClass cls : {ObjClass::Map, ObjClass::Set}) {
        const double n = bi::collectionSize(v, cls);
        if (n >= 0)
            return JSValue::boxNumber(n);
    }
    if (hasMethod(vm, v, "__len__"))
        return invoke(vm, v, "__len__", nullptr, 0);
    if (isObject(v) && hasProperty(vm, v, vm.atoms.length))
        return getProperty(vm, v, vm.atoms.length);
    return raise(vm, "TypeError", std::string("object of type '") + typeName(v) + "' has no len()");
}

/// Python truth: None, False, zero, NaN, the empty string and the empty container are false; everything else is true.
JSValue pyTruthy(VM& vm, JSValue, const JSValue* args, std::uint32_t argc, void*)
{
    const JSValue v = arg(args, argc, 0);
    if (hasElems(v) && asObject(v)->cls() == ObjClass::Array)
        return JSValue::boolean(static_cast<JSArray*>(asObject(v))->length != 0);
    for (ObjClass cls : {ObjClass::Map, ObjClass::Set}) {
        const double n = bi::collectionSize(v, cls);
        if (n >= 0)
            return JSValue::boolean(n != 0);
    }
    if (isObject(v)) {
        if (hasMethod(vm, v, "__bool__")) {
            JSValue r = invoke(vm, v, "__bool__", nullptr, 0);
            if (r.isException())
                return r;
            const bool t = toBoolean(r);
            release(r);
            return JSValue::boolean(t);
        }
        if (hasMethod(vm, v, "__len__")) {
            JSValue r = invoke(vm, v, "__len__", nullptr, 0);
            if (r.isException())
                return r;
            double d = 0;
            const bool ok = toNumber(vm, r, &d);
            release(r);
            if (!ok)
                return JSValue::exception();
            return JSValue::boolean(d != 0);
        }
        return JSValue::boolean(true);
    }
    return JSValue::boolean(toBoolean(v));
}

JSValue pyWrite(VM& vm, JSValue, const JSValue* args, std::uint32_t argc, void*)
{
    JSValue text = toStringValue(vm, arg(args, argc, 0));
    if (text.isException())
        return text;
    vm.ctx.write(asString(text)->text(), false);
    release(text);
    return JSValue::undefined();
}

/// `__py.fixed(x, digits)` and `__py.exponential(x, digits)`: Python cuts an exact tie to the even digit, where
/// Number.prototype.toFixed takes the larger.
JSValue pyFormat(VM& vm, JSValue, const JSValue* args, std::uint32_t argc, void* data)
{
    double x = 0, digits = 0;
    if (!toNumber(vm, arg(args, argc, 0), &x) || !toNumber(vm, arg(args, argc, 1), &digits))
        return JSValue::exception();
    const String text = data ? dyn::numberToExponential(x, int(digits), dyn::RoundTies::ToEven)
                             : dyn::numberToFixed(x, int(digits), dyn::RoundTies::ToEven);
    return makeString(vm, StringView(text));
}

JSValue pyInput(VM& vm, JSValue, const JSValue* args, std::uint32_t argc, void*)
{
    if (!arg(args, argc, 0).isUndefined()) {
        JSValue prompt = toStringValue(vm, args[0]);
        if (prompt.isException())
            return prompt;
        vm.ctx.write(asString(prompt)->text(), false);
        release(prompt);
    }
    std::string line;
    if (!vm.ctx.readLine || !vm.ctx.readLine(line))
        return raise(vm, "EOFError", "EOF when reading a line");
    return makeString(vm, std::string_view(line));
}

/// `__py.new(f, args...)`: construct when `f` is a class, call otherwise. Used for a capitalised name the parser could not
/// resolve statically.
JSValue pyNew(VM& vm, JSValue, const JSValue* args, std::uint32_t argc, void*)
{
    const JSValue callee = arg(args, argc, 0);
    if (!isFunction(callee))
        return raise(vm, "TypeError", std::string("'") + typeName(callee) + "' object is not callable");
    const auto* f = static_cast<const JSFunction*>(asObject(callee));
    const bool isClass = !(f->flags & JSFunction::kNative) && (f->code->flags & CodeBlock::kClassConstructor);
    const JSValue* rest = argc > 1 ? args + 1 : nullptr;
    const std::uint32_t restCount = argc > 1 ? argc - 1 : 0;
    if (isClass)
        return vm.construct(callee, rest, restCount);
    return vm.call(callee, JSValue::undefined(), rest, restCount);
}

void installPrelude(VM& vm)
{
    Key marker = vm.internKey(std::string_view("__py"));
    const bool installed = vm.global->findOwn(vm, marker) != nullptr;
    if (installed) {
        detail::decRef(marker);
        return;
    }
    JSObject* py = vm.newObject(vm.protos.object);
    Handle keep = Handle::adopt(toJSValue(py));
    bi::method(vm, py, "getitem", pyGetitem, 2);
    bi::method(vm, py, "setitem", pySetitem, 3);
    bi::method(vm, py, "len", pyLen, 1);
    bi::method(vm, py, "truthy", pyTruthy, 1);
    bi::method(vm, py, "write", pyWrite, 1);
    bi::method(vm, py, "input", pyInput, 0);
    bi::method(vm, py, "new", pyNew, 1);
    bi::method(vm, py, "fixed", pyFormat, 2, nullptr);
    bi::method(vm, py, "exponential", pyFormat, 2, reinterpret_cast<void*>(1));
    retain(keep.get());
    vm.global->putOwn(vm, marker, keep.get(), 0);
    detail::decRef(marker);
    JSValue r = vm.evalSource(python::prelude(), "<python>");
    if (r.isException())
        host::raisePending(vm);
    release(r);
}

struct Program : CompiledUnit
{
    CodeBlock* code = nullptr;
    VM* vm = nullptr;

    ~Program() override
    {
        if (!vm || alive.expired())
            return;
        vm->removeCodeRoot(&code);
        if (code)
            vm->releaseCode(code);
    }
    std::size_t imageSize() const noexcept override { return code ? code->code.size() * sizeof(Ins) : 0; }
};

std::shared_ptr<CompiledUnit> compilePython(ContextImpl& ctx, std::string_view source, std::string_view name,
                                            std::vector<Diagnostic>&)
{
    VM& vm = ctx.vm();
    host::syncHostClasses(ctx);
    installPrelude(vm);
    python::Parser parser(source, std::string(name));
    dyn::NodePtr tree = parser.parseModule();
    auto unit = std::make_shared<Program>();
    unit->language = Language::Python;
    unit->name = std::string(name);
    unit->context = &ctx;
    unit->alive = ctx.alive;
    unit->vm = &vm;
    unit->code = compileProgram(vm, *tree, name, std::make_shared<const std::string>(source));
    vm.addCodeRoot(&unit->code);
    return unit;
}

Value runPython(ContextImpl& ctx, CompiledUnit& unit, const std::vector<Value>&)
{
    VM& vm = ctx.vm();
    auto& program = static_cast<Program&>(unit);
    if (!program.code)
        throw Error(ErrorCode::InvalidOperation, "the program's context has ended");
    ContextImpl::ActiveEngine active(ctx, EngineKind::Vm);
    host::syncHostClasses(ctx);
    JSFunction* main = vm.newFunction(vm.protos.function, program.code, nullptr, 0);
    Handle keep = Handle::adopt(toJSValue(main));
    JSValue result = vm.call(keep.get(), toJSValue(vm.global), nullptr, 0);
    if (result.isException())
        host::raisePending(vm);
    Value out = toValue(vm, result);
    release(result);
    return out;
}

}  // namespace

const LanguageInfo& pythonLanguage() noexcept
{
    static const char* const extensions[] = {".py", nullptr};
    static const LanguageInfo info = [] {
        LanguageInfo l;
        l.id = Language::Python;
        l.name = "python";
        l.extensions = extensions;
        l.engine = EngineKind::Vm;
        l.compile = &compilePython;
        l.run = &runPython;
        return l;
    }();
    return info;
}

}  // namespace vm
}  // namespace internal
}  // namespace script
