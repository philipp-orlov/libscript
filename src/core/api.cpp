// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The public API: Value, Context, Assembly. Every entry point takes the
// context's lock, decides which engine a value or a source belongs to, and
// hands over.
#include <cmath>
#include <cstdio>
#include <limits>
#include <sstream>

#include "script/script.hpp"
#include "base/status.hpp"
#include "sys/file.hpp"
#include "script/internal/context.hpp"

#if SCRIPT_ENGINE_TYPED
#include "script/internal/typed/module.hpp"
#include "script/internal/typed/numconv.hpp"
#include "script/internal/typed/runtime.hpp"
#include "script/internal/typed/vm.hpp"
#endif
#if SCRIPT_ENGINE_DYNAMIC
#include "script/internal/dyn/builtins.hpp"
#include "script/internal/dyn/numconv.hpp"
#include "script/internal/dyn/runtime.hpp"
#include "script/internal/dyn/vm.hpp"
#endif

namespace script {

using namespace internal;

// ------------------------------------------------------------- versions ---

const char* version() noexcept
{
    return SCRIPT_VERSION;
}

const char* toString(ErrorCode code) noexcept
{
    switch (code) {
        case ErrorCode::Ok: return "Ok";
        case ErrorCode::Exception: return "Exception";
        case ErrorCode::SyntaxError: return "SyntaxError";
        case ErrorCode::CompileError: return "CompileError";
        case ErrorCode::TypeError: return "TypeError";
        case ErrorCode::RangeError: return "RangeError";
        case ErrorCode::ReferenceError: return "ReferenceError";
        case ErrorCode::EvalError: return "EvalError";
        case ErrorCode::UriError: return "URIError";
        case ErrorCode::NullReference: return "NullReferenceException";
        case ErrorCode::InvalidCast: return "InvalidCastException";
        case ErrorCode::IndexOutOfRange: return "IndexOutOfRangeException";
        case ErrorCode::ArgumentException: return "ArgumentException";
        case ErrorCode::ArgumentNull: return "ArgumentNullException";
        case ErrorCode::ArgumentOutOfRange: return "ArgumentOutOfRangeException";
        case ErrorCode::DivideByZero: return "DivideByZeroException";
        case ErrorCode::Overflow: return "OverflowException";
        case ErrorCode::InvalidOperation: return "InvalidOperationException";
        case ErrorCode::NotSupported: return "NotSupportedException";
        case ErrorCode::KeyNotFound: return "KeyNotFoundException";
        case ErrorCode::FormatException: return "FormatException";
        case ErrorCode::InternalError: return "InternalError";
        case ErrorCode::StackOverflow: return "StackOverflowException";
        case ErrorCode::OutOfMemory: return "OutOfMemoryException";
        case ErrorCode::Interrupted: return "Interrupted";
        case ErrorCode::BadImage: return "BadImageFormatException";
    }
    return "Error";
}

const char* exceptionTypeName(ErrorCode code) noexcept
{
    switch (code) {
        case ErrorCode::NullReference: return "System.NullReferenceException";
        case ErrorCode::InvalidCast: return "System.InvalidCastException";
        case ErrorCode::IndexOutOfRange: return "System.IndexOutOfRangeException";
        case ErrorCode::ArgumentException: return "System.ArgumentException";
        case ErrorCode::ArgumentNull: return "System.ArgumentNullException";
        case ErrorCode::ArgumentOutOfRange: return "System.ArgumentOutOfRangeException";
        case ErrorCode::DivideByZero: return "System.DivideByZeroException";
        case ErrorCode::Overflow: return "System.OverflowException";
        case ErrorCode::InvalidOperation: return "System.InvalidOperationException";
        case ErrorCode::NotSupported: return "System.NotSupportedException";
        case ErrorCode::KeyNotFound: return "System.Collections.Generic.KeyNotFoundException";
        case ErrorCode::FormatException: return "System.FormatException";
        case ErrorCode::OutOfMemory: return "System.OutOfMemoryException";
        case ErrorCode::StackOverflow: return "System.StackOverflowException";
        default: return "System.Exception";
    }
}

const char* toString(Type type) noexcept
{
    switch (type) {
        case Type::Undefined: return "undefined";
        case Type::Null: return "null";
        case Type::Bool: return "bool";
        case Type::Char: return "char";
        case Type::Int: return "int";
        case Type::Long: return "long";
        case Type::Float: return "float";
        case Type::Double: return "double";
        case Type::String: return "string";
        case Type::Symbol: return "symbol";
        case Type::Object: return "object";
        case Type::Array: return "array";
        case Type::Delegate: return "delegate";
    }
    return "?";
}

std::string Diagnostic::toString() const
{
    std::string out = file + "(" + std::to_string(line);
    if (column)
        out += "," + std::to_string(column);
    out += "): error";
    if (code) {
        const char* prefix = language == Language::CSharp       ? "CS"
                             : language == Language::TypeScript ? "TS"
                             : language == Language::JavaScript ? "JS"
                             : language == Language::Python     ? "PY"
                             : language == Language::C          ? "C"
                                                                : "";
        out += std::string(" ") + prefix + std::to_string(code);
    }
    return out + ": " + message;
}

CompileError::CompileError(std::vector<Diagnostic> diagnostics)
    : Error(ErrorCode::CompileError,
            diagnostics.empty() ? "the program did not compile" : diagnostics.front().toString()),
      diagnostics_(std::move(diagnostics))
{}

// ------------------------------------------------------------ exception ---

Exception::Exception(ErrorCode code, std::string message, Value value, std::string stack)
    : Error(code, message), value_(std::move(value)), stack_(std::move(stack))
{
    if (Context* ctx = value_.context())
        alive_ = ctx->impl()->alive;
}

Exception::Exception(const Exception& o)
    : Error(o), stack_(o.stack_), alive_(o.alive_)
{
    if (alive_.expired() && o.value_.isReference())
        value_ = Value();  // the heap is gone; do not touch it
    else
        value_ = o.value_;
}

Exception& Exception::operator=(const Exception& o)
{
    if (this != &o) {
        release();
        Error::operator=(o);
        stack_ = o.stack_;
        alive_ = o.alive_;
        value_ = alive_.expired() && o.value_.isReference() ? Value() : o.value_;
    }
    return *this;
}

Exception::~Exception()
{
    release();
}

/// Forget the value without releasing it when its Context has already torn
/// its heap down: releasing would touch freed memory.
void Exception::release() noexcept
{
    if (value_.isReference() && alive_.expired())
        detail::Access::retag(value_, Type::Null);
}

// -------------------------------------------------------------- helpers ---

namespace {

/// Takes the context lock for the duration of a public call. Reentrant, so a
/// native function calling back into the API does not deadlock.
class Guard
{
public:
    explicit Guard(ContextImpl* impl)
        : mtx_(impl && impl->options.threadSafe ? &impl->mtx : nullptr)
    {
        if (mtx_)
            mtx_->lock();
    }
    ~Guard()
    {
        if (mtx_)
            mtx_->unlock();
    }
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;

private:
    std::recursive_mutex* mtx_;
};

GCObject* cellOf(const Value& v) noexcept
{
    return detail::Access::ptr(v);
}
ContextImpl* contextOf(const Value& v) noexcept
{
    GCObject* p = cellOf(v);
    return p ? p->rt : nullptr;
}
[[maybe_unused]] bool isTyped(const Value& v) noexcept
{
    GCObject* p = cellOf(v);
    return p && detail::isTypedCell(p->kind);
}
[[maybe_unused]] bool isDynamic(const Value& v) noexcept
{
    GCObject* p = cellOf(v);
    return p && detail::isDynamicCell(p->kind);
}

ContextImpl& need(const Value& v, const char* what)
{
    ContextImpl* rt = contextOf(v);
    if (!rt)
        throw Error(ErrorCode::NullReference, std::string(what) + " needs an object or a string");
    return *rt;
}

[[noreturn]] void notSupported(const char* what)
{
    throw Error(ErrorCode::NotSupported, std::string(what) + " is not supported for this value");
}

[[noreturn]] void noEngine(const char* what)
{
    throw Error(ErrorCode::NotSupported,
                std::string(what) + " needs an engine this build of libscript does not contain");
}

/// ECMAScript ToInt32: well defined for every double, including the ones a
/// plain cast is not.
std::int32_t toInt32(double d) noexcept
{
    if (!std::isfinite(d))
        return 0;
    d = std::trunc(d);
    d = std::fmod(d, 4294967296.0);
    if (d < 0)
        d += 4294967296.0;
    return std::int32_t(std::uint32_t(d));
}

std::int64_t toInt64(double d) noexcept
{
    if (!std::isfinite(d))
        return 0;
    if (d >= 9223372036854775807.0)
        return std::numeric_limits<std::int64_t>::max();
    if (d <= -9223372036854775808.0)
        return std::numeric_limits<std::int64_t>::min();
    return std::int64_t(d);
}

double stringToDouble(StringView text)
{
#if SCRIPT_ENGINE_DYNAMIC
    return dyn::stringToNumber(text);
#elif SCRIPT_ENGINE_TYPED
    double out = 0;
    return typed::parseDouble(text, out) ? out : std::nan("");
#else
    (void)text;
    return std::nan("");
#endif
}

/// `String(primitive)` for a value with no heap behind it.
std::string primitiveToString(const Value& v)
{
    switch (v.type()) {
        case Type::Undefined: return "undefined";
        case Type::Null: return "null";
        case Type::Bool: return detail::Access::boolean(v) ? "true" : "false";
        case Type::Char: return toUtf8(String(1, Char(detail::Access::i4(v))));
        case Type::Int: return std::to_string(detail::Access::i4(v));
        case Type::Long: return std::to_string(detail::Access::i8(v));
        case Type::Float: return toUtf8(numberToString(detail::Access::r4(v)));
        case Type::Double: return toUtf8(numberToString(detail::Access::r8(v)));
        default: return "";
    }
}

std::string quoteJson(StringView s)
{
    String out;
    appendJsonQuoted(s, out);
    return toUtf8(out);
}

}  // namespace

// ---------------------------------------------------------------- value ---

Context* Value::context() const noexcept
{
    ContextImpl* rt = contextOf(*this);
    return rt ? rt->owner : nullptr;
}

bool Value::isArray() const noexcept
{
    if (tag_ == Type::Array)
        return true;
#if SCRIPT_ENGINE_DYNAMIC
    return isDynamic(*this) && dyn::asArray(*this) != nullptr;
#else
    return false;
#endif
}

bool Value::isFunction() const noexcept
{
    if (tag_ == Type::Delegate)
        return true;
#if SCRIPT_ENGINE_DYNAMIC
    return isDynamic(*this) && dyn::asFunction(*this) != nullptr;
#else
    return false;
#endif
}

bool Value::isError() const noexcept
{
    GCObject* cell = cellOf(*this);
    if (!cell || !cell->rt)
        return false;
#if SCRIPT_ENGINE_TYPED
    if (detail::isTypedCell(cell->kind)) {
        typed::Runtime* rt = cell->rt->typedIfAny();
        const typed::TypeInfo* type = typed::typeOfCell(cell);
        return rt && type && rt->types->core().exceptionType &&
               type->derivesFrom(rt->types->core().exceptionType);
    }
#endif
#if SCRIPT_ENGINE_DYNAMIC
    if (dyn::DynObject* o = dyn::asObject(*this))
        return o->cls == dyn::ObjClass::Error;
#endif
    return false;
}

bool Value::isPromise() const noexcept
{
#if SCRIPT_ENGINE_DYNAMIC
    dyn::DynObject* o = dyn::asObject(*this);
    return o && o->cls == dyn::ObjClass::Promise;
#else
    return false;
#endif
}

bool Value::isNative() const noexcept
{
    GCObject* cell = cellOf(*this);
    if (!cell)
        return false;
    if (cell->kind == CellKind::Native)
        return true;
#if SCRIPT_ENGINE_DYNAMIC
    if (dyn::DynObject* o = dyn::asObject(*this))
        return o->nativeSlot != nullptr;
#endif
    return false;
}

std::string Value::typeName() const
{
    GCObject* cell = cellOf(*this);
    if (!cell)
        return script::toString(tag_);
#if SCRIPT_ENGINE_TYPED
    if (detail::isTypedCell(cell->kind) || cell->kind == CellKind::String) {
        if (typed::Runtime* rt = cell->rt ? cell->rt->typedIfAny() : nullptr)
            return rt->types->describe(typed::typeOf(*rt, *this));
    }
#endif
    if (isFunction())
        return "function";
    return script::toString(tag_);
}

bool Value::toBool() const noexcept
{
    switch (tag_) {
        case Type::Undefined:
        case Type::Null: return false;
        case Type::Bool: return detail::Access::boolean(*this);
        case Type::Char:
        case Type::Int: return detail::Access::i4(*this) != 0;
        case Type::Long: return detail::Access::i8(*this) != 0;
        case Type::Float: return detail::Access::r4(*this) != 0 && !std::isnan(detail::Access::r4(*this));
        case Type::Double:
            return detail::Access::r8(*this) != 0 && !std::isnan(detail::Access::r8(*this));
        case Type::String: return !static_cast<StringCell*>(cellOf(*this))->text().empty();
        default: return true;
    }
}

double Value::toDouble() const
{
    if (!heap())
        return tag_ == Type::Undefined ? std::nan("") : numberOf(*this);
    if (tag_ == Type::String)
        return stringToDouble(static_cast<StringCell*>(cellOf(*this))->text());
#if SCRIPT_ENGINE_DYNAMIC
    if (isDynamic(*this)) {
        ContextImpl& rt = need(*this, "toDouble");
        Guard guard(&rt);
        return dyn::toNumber(rt.dyn(), *this);
    }
#endif
    return 0;
}

std::int32_t Value::toInt() const
{
    switch (tag_) {
        case Type::Bool: return detail::Access::boolean(*this) ? 1 : 0;
        case Type::Char:
        case Type::Int: return detail::Access::i4(*this);
        case Type::Long: return std::int32_t(detail::Access::i8(*this));
        default: return toInt32(toDouble());
    }
}

std::int64_t Value::toLong() const
{
    switch (tag_) {
        case Type::Bool: return detail::Access::boolean(*this) ? 1 : 0;
        case Type::Char:
        case Type::Int: return detail::Access::i4(*this);
        case Type::Long: return detail::Access::i8(*this);
        default: return toInt64(toDouble());
    }
}

std::string Value::toString() const
{
    GCObject* cell = cellOf(*this);
    if (!cell)
        return primitiveToString(*this);
    if (cell->kind == CellKind::String)
        return toUtf8(static_cast<StringCell*>(cell)->text());
    ContextImpl& rt = need(*this, "toString");
    Guard guard(&rt);
#if SCRIPT_ENGINE_TYPED
    if (detail::isTypedCell(cell->kind))
        return toUtf8(typed::toStringValue(rt.typed(), *this));
#endif
#if SCRIPT_ENGINE_DYNAMIC
    if (detail::isDynamicCell(cell->kind))
        return toUtf8(dyn::toStringValue(rt.dyn(), *this));
#endif
    if (cell->kind == CellKind::Box)
        return static_cast<Box*>(cell)->value.toString();
    return script::toString(tag_);
}

std::string Value::inspect() const
{
    if (isString())
        return "\"" + toString() + "\"";
    GCObject* cell = cellOf(*this);
    if (!cell)
        return primitiveToString(*this);
    ContextImpl& rt = need(*this, "inspect");
    Guard guard(&rt);
#if SCRIPT_ENGINE_TYPED
    if (detail::isTypedCell(cell->kind)) {
        if (cell->kind == CellKind::Array) {
            auto* array = static_cast<typed::CSArray*>(cell);
            std::string out = "[";
            for (std::uint32_t i = 0; i < array->length; ++i) {
                if (i)
                    out += ", ";
                if (i == 16) {
                    out += "...";
                    break;
                }
                out += array->get(i).inspect();
            }
            return out + "]";
        }
        const typed::TypeInfo* type = typed::typeOfCell(cell);
        if (cell->kind == CellKind::Instance && type) {
            auto* instance = static_cast<typed::CSInstance*>(cell);
            std::string out = toUtf8(type->name) + " { ";
            bool first = true;
            for (const typed::TypeInfo* t = type; t; t = t->base) {
                for (const typed::FieldInfo& field : t->fields) {
                    if (!first)
                        out += ", ";
                    first = false;
                    out += toUtf8(field.name) + " = " + instance->at(field.slot).inspect();
                }
            }
            return out + " }";
        }
        return toString();
    }
#endif
#if SCRIPT_ENGINE_DYNAMIC
    if (detail::isDynamicCell(cell->kind))
        return toUtf8(dyn::inspectValue(rt.dyn(), *this, 0));
#endif
    return toString();
}

std::string Value::toJson(int indent) const
{
    GCObject* cell = cellOf(*this);
    if (!cell) {
        switch (tag_) {
            case Type::Undefined: return "";
            case Type::Float:
            case Type::Double: return std::isfinite(numberOf(*this)) ? primitiveToString(*this) : "null";
            case Type::Char: return quoteJson(String(1, Char(detail::Access::i4(*this))));
            default: return primitiveToString(*this);
        }
    }
    if (cell->kind == CellKind::String)
        return quoteJson(static_cast<StringCell*>(cell)->text());
#if SCRIPT_ENGINE_DYNAMIC
    if (detail::isDynamicCell(cell->kind)) {
        ContextImpl& rt = need(*this, "toJson");
        Guard guard(&rt);
        String out;
        const String pad(std::size_t(indent < 0 ? 0 : (indent > 10 ? 10 : indent)), u' ');
        if (!dyn::jsonStringify(rt.dyn(), *this, Value::undefined(), pad, out))
            return "";
        return toUtf8(out);
    }
#else
    (void)indent;
#endif
    notSupported("toJson");
}

bool Value::sameReference(const Value& o) const noexcept
{
    if (heap() || o.heap())
        return cellOf(*this) == cellOf(o) && tag_ == o.tag_;
    return strictEquals(o);
}

bool Value::strictEquals(const Value& o) const noexcept
{
    const bool numeric = (isNumber() || isChar()) && (o.isNumber() || o.isChar());
    if (numeric)
        return numberOf(*this) == numberOf(o);
    if (tag_ != o.tag_)
        return false;
    switch (tag_) {
        case Type::Undefined:
        case Type::Null: return true;
        case Type::Bool: return detail::Access::boolean(*this) == detail::Access::boolean(o);
        case Type::String: {
            auto* a = static_cast<StringCell*>(cellOf(*this));
            auto* b = static_cast<StringCell*>(cellOf(o));
            return a == b || (a && b && a->text() == b->text());
        }
        default: return cellOf(*this) == cellOf(o);
    }
}

bool Value::equals(const Value& o) const
{
#if SCRIPT_ENGINE_DYNAMIC
    if (isDynamic(*this) || isDynamic(o)) {
        ContextImpl* rt = contextOf(*this);
        if (!rt)
            rt = contextOf(o);
        Guard guard(rt);
        return dyn::looseEquals(rt->dyn(), *this, o);
    }
#endif
#if SCRIPT_ENGINE_TYPED
    return typed::valueEquals(*this, o);
#else
    return strictEquals(o) || (isNullish() && o.isNullish());
#endif
}

Value Value::get(std::string_view key) const
{
    ContextImpl& rt = need(*this, "get");
    Guard guard(&rt);
#if SCRIPT_ENGINE_DYNAMIC
    if (isDynamic(*this) || isString())
        return dyn::getProperty(rt.dyn(), *this, rt.intern(fromUtf8(key)));
#endif
#if SCRIPT_ENGINE_TYPED
    {
        typed::Runtime& t = rt.typed();
        const String name = fromUtf8(key);
        const typed::TypeInfo* type = typed::typeOf(t, *this);
        if (!type)
            throw Error(ErrorCode::NullReference, "the value is null");
        if (const typed::FieldInfo* field = type->findField(name)) {
            if (field->isConst)
                return field->constValue;
            if (field->isStatic)
                return type->assembly ? type->assembly->statics[field->slot] : Value();
            auto* instance = static_cast<typed::CSInstance*>(cellOf(*this));
            return instance ? instance->at(field->slot) : Value();
        }
        if (const typed::PropertyInfo* prop = type->findProperty(name)) {
            if (!prop->getter)
                throw Error(ErrorCode::NotSupported, "the property has no getter");
            Value self = *this;
            return typed::invoke(t, typed::resolveVirtual(t, self, prop->getter), &self, 1);
        }
        throw Error(ErrorCode::ArgumentException, "'" + t.types->describe(type) +
                                                      "' has no member '" + std::string(key) + "'");
    }
#else
    notSupported("get");
#endif
}

Value Value::get(std::uint32_t index) const
{
    GCObject* cell = cellOf(*this);
    if (!cell)
        throw Error(ErrorCode::NullReference, "the value is null");
    ContextImpl& rt = *cell->rt;
    Guard guard(&rt);
    switch (cell->kind) {
#if SCRIPT_ENGINE_TYPED
        case CellKind::Array: {
            auto* array = static_cast<typed::CSArray*>(cell);
            if (index >= array->length)
                throw Error(ErrorCode::IndexOutOfRange, "the index is outside the array");
            return array->get(index);
        }
        case CellKind::List: {
            auto* list = static_cast<typed::CSList*>(cell);
            if (index >= list->items.size())
                throw Error(ErrorCode::IndexOutOfRange, "the index is outside the list");
            return list->items[index];
        }
#endif
        case CellKind::String: {
            auto* text = static_cast<StringCell*>(cell);
            if (index >= text->text().size())
                throw Error(ErrorCode::IndexOutOfRange, "the index is outside the string");
            return Value(char16_t(text->text()[index]));
        }
#if SCRIPT_ENGINE_DYNAMIC
        case CellKind::Object: return dyn::getIndex(rt.dyn(), *this, index);
#endif
        default: notSupported("get by index");
    }
}

Value Value::get(const Value& key) const
{
    ContextImpl& rt = need(*this, "get");
    Guard guard(&rt);
#if SCRIPT_ENGINE_DYNAMIC
    if (isDynamic(*this) || isString())
        return dyn::getProperty(rt.dyn(), *this, dyn::toPropertyKey(rt.dyn(), key).get());
#endif
    if (key.isNumber())
        return get(std::uint32_t(key.toInt()));
    return get(std::string_view(key.toString()));
}

void Value::set(std::string_view key, const Value& v)
{
    ContextImpl& rt = need(*this, "set");
    Guard guard(&rt);
#if SCRIPT_ENGINE_DYNAMIC
    if (isDynamic(*this)) {
        dyn::setProperty(rt.dyn(), *this, rt.intern(fromUtf8(key)), v);
        return;
    }
#endif
#if SCRIPT_ENGINE_TYPED
    {
        typed::Runtime& t = rt.typed();
        const String name = fromUtf8(key);
        const typed::TypeInfo* type = typed::typeOf(t, *this);
        if (!type)
            throw Error(ErrorCode::NullReference, "the value is null");
        if (const typed::FieldInfo* field = type->findField(name)) {
            if (field->isConst)
                throw Error(ErrorCode::InvalidOperation, "the field is constant");
            if (field->isStatic) {
                if (type->assembly)
                    type->assembly->statics[field->slot] = v;
                return;
            }
            if (auto* instance = static_cast<typed::CSInstance*>(cellOf(*this)))
                instance->at(field->slot) = v;
            return;
        }
        if (const typed::PropertyInfo* prop = type->findProperty(name)) {
            if (!prop->setter)
                throw Error(ErrorCode::NotSupported, "the property has no setter");
            Value args[2] = {*this, v};
            typed::invoke(t, typed::resolveVirtual(t, args[0], prop->setter), args, 2);
            return;
        }
        throw Error(ErrorCode::ArgumentException, "'" + t.types->describe(type) +
                                                      "' has no member '" + std::string(key) + "'");
    }
#else
    (void)v;
    notSupported("set");
#endif
}

void Value::set(std::uint32_t index, const Value& v)
{
    GCObject* cell = cellOf(*this);
    if (!cell)
        throw Error(ErrorCode::NullReference, "the value is null");
    ContextImpl& rt = *cell->rt;
    Guard guard(&rt);
    switch (cell->kind) {
#if SCRIPT_ENGINE_TYPED
        case CellKind::Array: {
            auto* array = static_cast<typed::CSArray*>(cell);
            if (index >= array->length)
                throw Error(ErrorCode::IndexOutOfRange, "the index is outside the array");
            array->set(index, v);
            return;
        }
        case CellKind::List: {
            auto* list = static_cast<typed::CSList*>(cell);
            if (index >= list->items.size())
                throw Error(ErrorCode::IndexOutOfRange, "the index is outside the list");
            list->items[index] = v;
            return;
        }
#endif
#if SCRIPT_ENGINE_DYNAMIC
        case CellKind::Object: dyn::setIndex(rt.dyn(), *this, index, v); return;
#endif
        default: notSupported("set by index");
    }
}

void Value::set(const Value& key, const Value& v)
{
    ContextImpl& rt = need(*this, "set");
    Guard guard(&rt);
#if SCRIPT_ENGINE_DYNAMIC
    if (isDynamic(*this)) {
        dyn::setProperty(rt.dyn(), *this, dyn::toPropertyKey(rt.dyn(), key).get(), v);
        return;
    }
#endif
    if (key.isNumber())
        set(std::uint32_t(key.toInt()), v);
    else
        set(std::string_view(key.toString()), v);
}

bool Value::has(std::string_view key) const
{
    ContextImpl* rt = contextOf(*this);
    if (!rt)
        return false;
    Guard guard(rt);
#if SCRIPT_ENGINE_DYNAMIC
    if (isDynamic(*this) || isString())
        return dyn::hasProperty(rt->dyn(), *this, rt->intern(fromUtf8(key)));
#endif
#if SCRIPT_ENGINE_TYPED
    {
        const typed::TypeInfo* type = typed::typeOf(rt->typed(), *this);
        const String name = fromUtf8(key);
        return type && (type->findField(name) || type->findProperty(name));
    }
#else
    return false;
#endif
}

std::vector<std::string> Value::keys() const
{
    std::vector<std::string> names;
    ContextImpl* rt = contextOf(*this);
    if (!rt)
        return names;
    Guard guard(rt);
#if SCRIPT_ENGINE_DYNAMIC
    if (dyn::DynObject* obj = dyn::asObject(*this)) {
        for (const Value& k : dyn::ownKeys(rt->dyn(), obj, true, false))
            names.push_back(toUtf8(static_cast<StringCell*>(cellOf(k))->text()));
        return names;
    }
#endif
#if SCRIPT_ENGINE_TYPED
    if (isTyped(*this)) {
        const typed::TypeInfo* type = typed::typeOf(rt->typed(), *this);
        for (const typed::TypeInfo* t = type; t; t = t->base) {
            for (const typed::FieldInfo& field : t->fields)
                names.push_back(toUtf8(field.name));
            for (const typed::PropertyInfo& prop : t->props)
                names.push_back(toUtf8(prop.name));
        }
    }
#endif
    return names;
}

std::uint32_t Value::length() const
{
    GCObject* cell = cellOf(*this);
    if (!cell)
        return 0;
    switch (cell->kind) {
        case CellKind::String: return std::uint32_t(static_cast<StringCell*>(cell)->text().size());
#if SCRIPT_ENGINE_TYPED
        case CellKind::Array: return static_cast<typed::CSArray*>(cell)->length;
        case CellKind::List: return std::uint32_t(static_cast<typed::CSList*>(cell)->items.size());
#endif
#if SCRIPT_ENGINE_DYNAMIC
        case CellKind::Object: {
            Guard guard(cell->rt);
            return dyn::lengthOf(cell->rt->dyn(), *this);
        }
#endif
        default: return 0;
    }
}

void Value::push(const Value& v)
{
    GCObject* cell = cellOf(*this);
    if (!cell)
        throw Error(ErrorCode::NullReference, "the value is null");
    Guard guard(cell->rt);
#if SCRIPT_ENGINE_TYPED
    if (cell->kind == CellKind::List) {
        static_cast<typed::CSList*>(cell)->add(v);
        return;
    }
#endif
#if SCRIPT_ENGINE_DYNAMIC
    if (dyn::DynArray* arr = dyn::asArray(*this)) {
        arr->append(v);
        return;
    }
#endif
    (void)v;
    throw Error(ErrorCode::InvalidCast, "push needs an array or a list");
}

#if SCRIPT_ENGINE_DYNAMIC
namespace {
dyn::DynObject& needDynObject(const Value& v, const char* what)
{
    dyn::DynObject* obj = dyn::asObject(v);
    if (!obj)
        throw Error(ErrorCode::TypeError, std::string(what) + " needs a JavaScript object");
    return *obj;
}
}  // namespace
#endif

void Value::defineProperty(std::string_view key, const Value& v, PropertyFlags flags)
{
#if SCRIPT_ENGINE_DYNAMIC
    ContextImpl& rt = need(*this, "defineProperty");
    Guard guard(&rt);
    needDynObject(*this, "defineProperty").putOwn(rt.intern(fromUtf8(key)), v, std::uint8_t(flags));
#else
    (void)key;
    (void)v;
    (void)flags;
    noEngine("defineProperty");
#endif
}

void Value::defineAccessor(std::string_view key, const Value& getterFn, const Value& setterFn,
                           PropertyFlags flags)
{
#if SCRIPT_ENGINE_DYNAMIC
    ContextImpl& rt = need(*this, "defineAccessor");
    Guard guard(&rt);
    dyn::Prop& p = needDynObject(*this, "defineAccessor")
                       .putOwn(rt.intern(fromUtf8(key)), getterFn,
                               std::uint8_t(std::uint8_t(flags) | dyn::PropAccessor));
    p.setter = setterFn;
#else
    (void)key;
    (void)getterFn;
    (void)setterFn;
    (void)flags;
    noEngine("defineAccessor");
#endif
}

bool Value::hasOwn(std::string_view key) const
{
#if SCRIPT_ENGINE_DYNAMIC
    dyn::DynObject* obj = dyn::asObject(*this);
    if (!obj)
        return false;
    ContextImpl& rt = *cellOf(*this)->rt;
    Guard guard(&rt);
    return obj->findOwn(rt.intern(fromUtf8(key))) != nullptr;
#else
    return has(key);
#endif
}

bool Value::remove(std::string_view key)
{
#if SCRIPT_ENGINE_DYNAMIC
    ContextImpl& rt = need(*this, "remove");
    Guard guard(&rt);
    return dyn::deleteProperty(rt.dyn(), *this, rt.intern(fromUtf8(key)));
#else
    (void)key;
    noEngine("remove");
#endif
}

Value Value::prototype() const
{
#if SCRIPT_ENGINE_DYNAMIC
    dyn::DynObject* obj = dyn::asObject(*this);
    if (!obj || !obj->proto)
        return Value::null();
    return detail::Access::make(Type::Object, obj->proto.get());
#else
    return Value::null();
#endif
}

void Value::setPrototype(const Value& proto)
{
#if SCRIPT_ENGINE_DYNAMIC
    if (dyn::DynObject* obj = dyn::asObject(*this))
        obj->proto.reset(dyn::asObject(proto));
#else
    (void)proto;
    noEngine("setPrototype");
#endif
}

Value Value::call(const std::vector<Value>& args) const
{
    return callWithThis(Value::undefined(), args);
}

Value Value::callWithThis(const Value& thisValue, const std::vector<Value>& args) const
{
    ContextImpl& rt = need(*this, "call");
    Guard guard(&rt);
#if SCRIPT_ENGINE_TYPED
    if (tag_ == Type::Delegate) {
        std::vector<Value> copy = args;
        return typed::invokeDelegate(rt.typed(), *this, copy.data(), std::uint32_t(copy.size()));
    }
#endif
#if SCRIPT_ENGINE_DYNAMIC
    if (isDynamic(*this))
        return dyn::callValue(rt.dyn(), *this, thisValue, args);
#endif
    (void)thisValue;
    throw Error(ErrorCode::NotSupported, "the value is not callable");
}

Value Value::invoke(std::string_view method, const std::vector<Value>& args) const
{
    ContextImpl& rt = need(*this, "invoke");
    Guard guard(&rt);
#if SCRIPT_ENGINE_DYNAMIC
    if (isDynamic(*this) || isString())
        return dyn::invokeMethod(rt.dyn(), *this, rt.intern(fromUtf8(method)), args);
#endif
#if SCRIPT_ENGINE_TYPED
    {
        typed::Runtime& t = rt.typed();
        const typed::TypeInfo* type = typed::typeOf(t, *this);
        if (!type)
            throw Error(ErrorCode::NullReference, "the value is null");
        std::vector<typed::MethodInfo*> candidates;
        type->collectMethods(fromUtf8(method), candidates);
        for (typed::MethodInfo* candidate : candidates) {
            if (candidate->params.size() != args.size())
                continue;
            std::vector<Value> call;
            call.reserve(args.size() + 1);
            if (!candidate->isStatic())
                call.push_back(*this);
            for (const Value& arg : args)
                call.push_back(arg);
            const typed::MethodInfo* target =
                candidate->isStatic() ? candidate : typed::resolveVirtual(t, *this, candidate);
            return typed::invoke(t, target, call.data(), std::uint32_t(call.size()));
        }
        throw Error(ErrorCode::ArgumentException, "no method '" + std::string(method) +
                                                      "' takes " + std::to_string(args.size()) +
                                                      " argument(s)");
    }
#else
    (void)args;
    notSupported("invoke");
#endif
}

Value Value::construct(const std::vector<Value>& args) const
{
#if SCRIPT_ENGINE_DYNAMIC
    ContextImpl& rt = need(*this, "construct");
    Guard guard(&rt);
    return dyn::constructValue(rt.dyn(), *this, args);
#else
    (void)args;
    noEngine("construct");
#endif
}

void* Value::nativeHandle(const void* typeKey) const noexcept
{
    GCObject* cell = cellOf(*this);
    if (!cell)
        return nullptr;
    if (cell->kind == CellKind::Native) {
        auto* native = static_cast<NativeCell*>(cell);
        return native->slot.typeKey == typeKey ? native->slot.data : nullptr;
    }
#if SCRIPT_ENGINE_DYNAMIC
    if (dyn::DynObject* obj = dyn::asObject(*this)) {
        if (obj->nativeSlot && obj->nativeSlot->typeKey == typeKey)
            return obj->nativeSlot->data;
    }
#endif
    (void)typeKey;
    return nullptr;
}

// -------------------------------------------------------------- callinfo ---

namespace {

[[noreturn]] void raise(ContextImpl& rt, EngineKind engine, ErrorCode code,
                        const std::string& message)
{
#if SCRIPT_ENGINE_TYPED
    if (engine == EngineKind::Typed)
        typed::throwError(rt.typed(), code, message);
#endif
#if SCRIPT_ENGINE_DYNAMIC
    if (engine == EngineKind::Dynamic)
        dyn::throwError(rt.dyn(), code, message);
#endif
    (void)rt;
    (void)engine;
    throw Error(code, message);
}

Value makeExceptionFor(ContextImpl& rt, EngineKind engine, ErrorCode code,
                       std::string_view message)
{
#if SCRIPT_ENGINE_TYPED
    if (engine == EngineKind::Typed)
        return rt.typed().makeException(code, fromUtf8(message));
#endif
#if SCRIPT_ENGINE_DYNAMIC
    if (engine == EngineKind::Dynamic)
        return dyn::makeErrorValue(rt.dyn(), code, std::string(message));
#endif
    (void)rt;
    (void)engine;
    (void)message;
    noEngine("makeException");
}

}  // namespace

void CallInfo::fail(ErrorCode code, std::string_view message) const
{
    ContextImpl& rt = *ctx_->impl();
    raise(rt, rt.engineFor(Language::Auto), code, std::string(message));
}

// ------------------------------------------------------------- assembly ---

Assembly::Assembly() = default;
Assembly::Assembly(Assembly&&) noexcept = default;
Assembly::Assembly(const Assembly&) = default;
Assembly::Assembly(std::shared_ptr<CompiledUnit> impl) : impl_(std::move(impl)) {}

namespace {

/// Drop a unit under its context's lock: a JavaScript program's constants
/// are heap values, and releasing them races with a thread running in the
/// same context. After the context is gone the unit holds nothing live.
void dropUnit(std::shared_ptr<CompiledUnit>& unit)
{
    if (!unit)
        return;
    const std::shared_ptr<const bool> keep = unit->alive.lock();
    if (keep && unit->context) {
        Guard guard(unit->context);
        unit.reset();
    } else {
        unit.reset();
    }
}

}  // namespace

Assembly::~Assembly()
{
    dropUnit(impl_);
}

Assembly& Assembly::operator=(Assembly&& o) noexcept
{
    if (this != &o) {
        dropUnit(impl_);
        impl_ = std::move(o.impl_);
    }
    return *this;
}

Assembly& Assembly::operator=(const Assembly& o)
{
    if (this != &o) {
        dropUnit(impl_);
        impl_ = o.impl_;
    }
    return *this;
}

const std::string& Assembly::name() const noexcept
{
    static const std::string none;
    return impl_ ? impl_->name : none;
}

Language Assembly::language() const noexcept
{
    return impl_ ? impl_->language : Language::Auto;
}

std::vector<std::string> Assembly::methods() const
{
    return impl_ ? impl_->methods() : std::vector<std::string>();
}

std::string Assembly::entryPoint() const
{
    return impl_ ? impl_->entryPoint() : std::string();
}

std::vector<std::uint8_t> Assembly::serialize() const
{
    if (!impl_)
        return {};
#if SCRIPT_ENGINE_TYPED
    if (auto* image = dynamic_cast<typed::AssemblyImpl*>(impl_.get()))
        return typed::writeImage(*image);
#endif
    throw Error(ErrorCode::NotSupported,
                std::string("a ") + toString(impl_->language) + " assembly has no portable image");
}

std::size_t Assembly::imageSize() const noexcept
{
    return impl_ ? impl_->imageSize() : 0;
}

// -------------------------------------------------------------- context ---

Context::Context() : Context(Options{}) {}

namespace {

Context::Options withEnvironment(const Context::Options& options)
{
    Context::Options out = options;
#if defined(SCRIPT_JIT_ENV_OVERRIDE)
    if (const char* mode = std::getenv("SCRIPT_JIT")) {
        if (std::strcmp(mode, "eager") == 0) {
            out.jit.mode = JitOptions::Mode::Eager;
            out.compile.native = true;
        } else if (std::strncmp(mode, "tiered:", 7) == 0) {
            out.jit.mode = JitOptions::Mode::Tiered;
            out.jit.threshold = std::uint32_t(std::atoi(mode + 7));
        }
    }
#endif
    return out;
}

}  // namespace

Context::Context(const Options& options) : impl_(std::make_unique<ContextImpl>(*this, withEnvironment(options)))
{
    impl_->print = [](std::string_view text, bool isError) {
        std::FILE* out = isError ? stderr : stdout;
        std::fwrite(text.data(), 1, text.size(), out);
    };
    impl_->unhandledRejection = [impl = impl_.get()](const Value& reason) {
        std::string text;
        try {
            text = reason.inspect();
        } catch (...) {
            text = "<unprintable>";
        }
        if (impl->print)
            impl->print("Unhandled promise rejection: " + text + "\n", true);
    };
}

Context::~Context() = default;

Context::Context(Context&& other) noexcept : impl_(std::move(other.impl_))
{
    if (impl_)
        impl_->owner = this;
}

Context& Context::operator=(Context&& other) noexcept
{
    if (this != &other) {
        impl_ = std::move(other.impl_);
        if (impl_)
            impl_->owner = this;
    }
    return *this;
}

Context::Lock::Lock(Context& ctx) : ctx_(&ctx)
{
    if (ctx_->impl_->options.threadSafe)
        ctx_->impl_->mtx.lock();
}

Context::Lock::~Lock()
{
    if (ctx_->impl_->options.threadSafe)
        ctx_->impl_->mtx.unlock();
}

namespace {

/// The front end for a source: named outright, else by the file's extension,
/// else the context's default, else the first one the build contains.
const LanguageInfo& frontEndFor(ContextImpl& rt, std::string_view name, Language requested)
{
    Language language = requested;
    if (language == Language::Auto)
        language = languageOfFile(name);
    if (language == Language::Auto)
        language = rt.options.language;
    if (language == Language::Auto) {
        const auto& table = languageTable();
        if (table.empty())
            throw Error(ErrorCode::NotSupported, "this build of libscript contains no language");
        return *table.front();
    }
    const LanguageInfo* info = languageInfo(language);
    if (!info)
        throw Error(ErrorCode::NotSupported, std::string("this build of libscript has no ") +
                                                 toString(language) + " support");
    return *info;
}

const LanguageInfo& frontEndOf(const Assembly& assembly)
{
    if (!assembly)
        throw Error(ErrorCode::ArgumentException, "the assembly is empty");
    const LanguageInfo* info = languageInfo(assembly.language());
    if (!info)
        throw Error(ErrorCode::NotSupported, "the assembly's language is not in this build");
    return *info;
}

std::string readSource(const std::string& path)
{
    try {
        return sys::readWholeFile(path);
    } catch (const base::Error&) {
        throw Error(ErrorCode::ArgumentException, "cannot open '" + path + "'");
    }
}

}  // namespace

std::vector<Diagnostic> Context::diagnose(std::string_view source, std::string_view name,
                                          Assembly* out, Language language)
{
    Lock lock(*this);
    const LanguageInfo& front = frontEndFor(*impl_, name, language);
    impl_->lastEngine = front.engine;
    std::vector<Diagnostic> diagnostics;
    std::shared_ptr<CompiledUnit> unit = front.compile(*impl_, source, name, diagnostics);
    if (unit) {
        unit->context = impl_.get();
        unit->alive = impl_->alive;
        if (out)
            *out = Assembly(unit);
    }
    return diagnostics;
}

Assembly Context::compile(std::string_view source, std::string_view name, Language language)
{
    Assembly assembly;
    std::vector<Diagnostic> diagnostics = diagnose(source, name, &assembly, language);
    if (!diagnostics.empty())
        throw CompileError(std::move(diagnostics));
    return assembly;
}

Assembly Context::compileFile(const std::string& path, Language language)
{
    return compile(readSource(path), path, language);
}

Assembly Context::load(const void* data, std::size_t bytes)
{
    Lock lock(*this);
#if SCRIPT_ENGINE_TYPED
    typed::Runtime& rt = impl_->typed();
    rt.syncHostClasses();
    impl_->lastEngine = EngineKind::Typed;
    auto assembly = typed::readImage(rt, data, bytes);
    // An image does not say which typed language wrote it, and they all run
    // the same way; it belongs to whichever this build has.
    assembly->language = Language::Auto;
    for (Language candidate : {Language::CSharp, Language::C, Language::Cpp}) {
        if (internal::languageInfo(candidate)) {
            assembly->language = candidate;
            break;
        }
    }
    assembly->context = impl_.get();
    assembly->alive = impl_->alive;
    rt.assemblies.push_back(assembly);
    if (rt.jit && (impl_->options.compile.native ||
                   impl_->options.jit.mode == JitOptions::Mode::Eager))
        rt.jit->compileAll(rt, *assembly);
    return Assembly(assembly);
#else
    (void)data;
    (void)bytes;
    noEngine("load");
#endif
}

std::size_t Context::compileNative(const Assembly& assembly)
{
    Lock lock(*this);
#if SCRIPT_ENGINE_TYPED
    auto* image = dynamic_cast<typed::AssemblyImpl*>(assembly.impl());
    typed::Runtime* rt = impl_->typedIfAny();
    if (!image || !rt || !rt->jit)
        return 0;
    return rt->jit->compileAll(*rt, *image);
#else
    (void)assembly;
    return 0;
#endif
}

Value Context::run(const Assembly& assembly, const std::vector<Value>& args)
{
    Lock lock(*this);
    const LanguageInfo& front = frontEndOf(assembly);
    impl_->lastEngine = front.engine;
    return front.run(*impl_, *assembly.impl(), args);
}

Value Context::call(const Assembly& assembly, std::string_view qualifiedMethod,
                    const std::vector<Value>& args)
{
    Lock lock(*this);
#if SCRIPT_ENGINE_TYPED
    auto* image = dynamic_cast<typed::AssemblyImpl*>(assembly.impl());
    if (!image)
        throw Error(ErrorCode::ArgumentException, "call needs a C# assembly");
    typed::Runtime& rt = impl_->typed();
    rt.ensureInitialized();
    typed::MethodInfo* method = image->findMethod(fromUtf8(qualifiedMethod));
    if (!method)
        throw Error(ErrorCode::ArgumentException,
                    "no method '" + std::string(qualifiedMethod) + "' in the assembly");
    ContextImpl::ArgBuffer buffer(*impl_);
    std::vector<Value>& call = buffer.args;
    call.assign(args.begin(), args.end());
    return typed::invoke(rt, method, call.data(), std::uint32_t(call.size()));
#else
    (void)assembly;
    (void)qualifiedMethod;
    (void)args;
    noEngine("call");
#endif
}

Value Context::eval(std::string_view source, std::string_view name, Language language)
{
    // One lock across compile, run and the assembly's release, so another
    // thread never sees the heap between them.
    Lock lock(*this);
    Assembly assembly = compile(source, name, language);
    return run(assembly);
}

Value Context::evalFile(const std::string& path, Language language)
{
    Lock lock(*this);
    Assembly assembly = compileFile(path, language);
    return run(assembly);
}

void Context::declare(std::string_view declarations)
{
    Lock lock(*this);
    bool accepted = false;
    for (const LanguageInfo* info : languageTable()) {
        if (info->declare) {
            info->declare(*impl_, declarations);
            accepted = true;
        }
    }
    if (!accepted)
        throw Error(ErrorCode::NotSupported, "no language in this build takes declarations");
}

Value Context::global() const
{
#if SCRIPT_ENGINE_DYNAMIC
    Guard guard(impl_.get());
    return detail::Access::make(Type::Object, impl_->dyn().globalObject.get());
#else
    noEngine("global");
#endif
}

// ------------------------------------------------------------ factories ---

Value Context::str(std::string_view utf8)
{
    Lock lock(*this);
    return impl_->str(fromUtf8(utf8));
}

Value Context::symbol(std::string_view description)
{
#if SCRIPT_ENGINE_DYNAMIC
    Lock lock(*this);
    auto* s = impl_->newCell<dyn::SymbolCell>();
    s->description = fromUtf8(description);
    return detail::Access::adopt(Type::Symbol, s);
#else
    (void)description;
    noEngine("symbol");
#endif
}

Value Context::object()
{
#if SCRIPT_ENGINE_DYNAMIC
    Lock lock(*this);
    dyn::Runtime& rt = impl_->dyn();
    return detail::Access::adopt(Type::Object, rt.newObject(rt.objectProto.get()));
#else
    noEngine("object");
#endif
}

Value Context::array(std::size_t reserve)
{
#if SCRIPT_ENGINE_DYNAMIC
    Lock lock(*this);
    return detail::Access::adopt(Type::Object, impl_->dyn().newArray(reserve));
#else
    (void)reserve;
    noEngine("array");
#endif
}

Value Context::array(const std::vector<Value>& items)
{
    Lock lock(*this);
#if SCRIPT_ENGINE_TYPED
    // While the typed engine is running -- a host method it called is
    // returning a std::vector -- it wants a T[] (object[] for a mix), not a
    // JavaScript array. Without a dynamic engine that is the only choice.
    if (impl_->lastEngine == EngineKind::Typed || !SCRIPT_ENGINE_DYNAMIC) {
        Type element = items.empty() ? Type::Object : items.front().type();
        for (const Value& v : items) {
            if (v.type() != element)
                element = Type::Object;
        }
        Value array = newArray(element, std::int32_t(items.size()));
        for (std::size_t i = 0; i < items.size(); ++i)
            array.set(std::uint32_t(i), items[i]);
        return array;
    }
#endif
#if SCRIPT_ENGINE_DYNAMIC
    return impl_->dyn().makeArray(items);
#else
    (void)items;
    noEngine("array");
#endif
}

Value Context::function(std::string_view name, NativeFunction fn, int arity)
{
#if SCRIPT_ENGINE_DYNAMIC
    Lock lock(*this);
    return impl_->dyn().makeFunction(fromUtf8(name), std::move(fn), arity);
#else
    (void)name;
    (void)fn;
    (void)arity;
    noEngine("function");
#endif
}

Value Context::newObject(const Assembly& assembly, std::string_view typeName,
                         const std::vector<Value>& args)
{
    Lock lock(*this);
#if SCRIPT_ENGINE_TYPED
    typed::Runtime& rt = impl_->typed();
    auto* image = dynamic_cast<typed::AssemblyImpl*>(assembly.impl());
    const typed::TypeInfo* type = image ? image->findType(fromUtf8(typeName)) : nullptr;
    if (!type)
        type = rt.types->find(fromUtf8(typeName));
    if (!type)
        throw Error(ErrorCode::ArgumentException, "no type '" + std::string(typeName) + "'");
    rt.ensureInitialized();

    std::vector<typed::MethodInfo*> ctors;
    type->collectMethods(ascii(".ctor"), ctors);
    for (typed::MethodInfo* ctor : ctors) {
        if (ctor->params.size() != args.size())
            continue;
        std::vector<Value> call = args;
        return typed::construct(rt, ctor, call.data(), std::uint32_t(call.size()));
    }
    throw Error(ErrorCode::ArgumentException, "no matching constructor");
#else
    (void)assembly;
    (void)typeName;
    (void)args;
    noEngine("newObject");
#endif
}

Value Context::newArray(Type elementType, std::int32_t length)
{
    Lock lock(*this);
#if SCRIPT_ENGINE_TYPED
    typed::Runtime& rt = impl_->typed();
    const typed::CoreTypes& core = rt.types->core();
    const typed::TypeInfo* element = core.objectType;
    switch (elementType) {
        case Type::Bool: element = core.boolType; break;
        case Type::Char: element = core.charType; break;
        case Type::Int: element = core.intType; break;
        case Type::Long: element = core.longType; break;
        case Type::Float: element = core.floatType; break;
        case Type::Double: element = core.doubleType; break;
        case Type::String: element = core.stringType; break;
        default: break;
    }
    return typed::newArrayOf(rt, element, length);
#else
    (void)elementType;
    (void)length;
    noEngine("newArray");
#endif
}

Value Context::newArray(std::string_view elementType, std::int32_t length)
{
    Lock lock(*this);
#if SCRIPT_ENGINE_TYPED
    typed::Runtime& rt = impl_->typed();
    rt.syncHostClasses();
    const typed::TypeInfo* element = typed::resolveTypeName(rt, fromUtf8(elementType));
    if (!element)
        throw Error(ErrorCode::ArgumentException, "no type '" + std::string(elementType) + "'");
    return typed::newArrayOf(rt, element, length);
#else
    (void)elementType;
    (void)length;
    noEngine("newArray");
#endif
}

Value Context::makeException(ErrorCode kind, std::string_view message)
{
    Lock lock(*this);
    return makeExceptionFor(*impl_, impl_->engineFor(Language::Auto), kind, message);
}

void Context::fail(ErrorCode kind, std::string_view message)
{
    Lock lock(*this);
    raise(*impl_, impl_->engineFor(Language::Auto), kind, std::string(message));
}

Value Context::parseJson(std::string_view text)
{
#if SCRIPT_ENGINE_DYNAMIC
    Lock lock(*this);
    return dyn::jsonParse(impl_->dyn(), fromUtf8(text));
#else
    (void)text;
    noEngine("parseJson");
#endif
}

// ------------------------------------------------------------- bindings ---

Context::HostClass Context::hostClass(std::string_view name)
{
    Lock lock(*this);
    HostClassInfo* info = impl_->findHostClass(name);
    if (!info) {
        impl_->hostClasses.push_back(std::make_unique<HostClassInfo>());
        info = impl_->hostClasses.back().get();
        info->name = std::string(name);
    }
    return HostClass(*this, info);
}

void Context::declareHostMember(void* hostType, const HostMember& member, NativeFunction fn,
                                NativeFunction setter)
{
    Lock lock(*this);
    auto* info = static_cast<HostClassInfo*>(hostType);
    info->members.push_back(HostClassInfo::Member{member, std::move(fn), std::move(setter)});
}

void Context::bindClassKey(void* hostType, const void* typeKey)
{
    Lock lock(*this);
    static_cast<HostClassInfo*>(hostType)->typeKey = typeKey;
}

bool Context::isClassRegistered(const void* typeKey) const
{
    Guard guard(impl_.get());
    return impl_->hostClassForKey(typeKey) != nullptr;
}

Value Context::native(const void* typeKey, void* instance, Finalizer finalizer, void* owner)
{
    Lock lock(*this);
    auto* cell = impl_->newCell<NativeCell>();
    Value result = detail::Access::adopt(Type::Object, cell);
    cell->slot.typeKey = typeKey;
    cell->slot.data = instance;
    cell->slot.owner = owner;
    cell->slot.finalizer = finalizer;
#if SCRIPT_ENGINE_TYPED
    // The typed engine wants the class up front when it is around; otherwise
    // it looks the wrapper up the first time it meets it.
    if (typed::Runtime* rt = impl_->typedIfAny()) {
        rt->syncHostClasses();
        const auto it = rt->nativeClasses.find(typeKey);
        if (it != rt->nativeClasses.end())
            cell->typedType = it->second;
    }
#endif
    return result;
}

void Context::attachNative(const Value& object, const void* typeKey, void* instance,
                           Finalizer finalizer, void* owner)
{
#if SCRIPT_ENGINE_DYNAMIC
    Lock lock(*this);
    dyn::DynObject* obj = dyn::asObject(object);
    if (!obj)
        throw Error(ErrorCode::TypeError, "attachNative needs a JavaScript object");
    dyn::attachNativeSlot(*obj, typeKey, instance, finalizer, owner);
#else
    (void)object;
    (void)typeKey;
    (void)instance;
    (void)finalizer;
    (void)owner;
    noEngine("attachNative");
#endif
}

Value Context::classPrototype(const void* typeKey)
{
#if SCRIPT_ENGINE_DYNAMIC
    Lock lock(*this);
    dyn::Runtime& rt = impl_->dyn();
    rt.syncHostClasses();
    return rt.hostPrototype(typeKey);
#else
    (void)typeKey;
    noEngine("classPrototype");
#endif
}

Value Context::classConstructor(const void* typeKey)
{
#if SCRIPT_ENGINE_DYNAMIC
    Lock lock(*this);
    dyn::Runtime& rt = impl_->dyn();
    rt.syncHostClasses();
    return rt.hostConstructor(typeKey);
#else
    (void)typeKey;
    noEngine("classConstructor");
#endif
}

// --------------------------------------------------------- housekeeping ---

std::size_t Context::runJobs()
{
#if SCRIPT_ENGINE_DYNAMIC
    Lock lock(*this);
    dyn::Runtime* rt = impl_->dynIfAny();
    return rt ? rt->runJobs() : 0;
#else
    return 0;
#endif
}

void Context::collectGarbage()
{
    Lock lock(*this);
    impl_->collectCycles();
    // Asked for by the host: hand back what this thread keeps for the next small buffers too.
    internal::payload_cache::flushThisThread();
}

Context::Stats Context::stats() const
{
    Guard guard(impl_.get());
    Stats out;
    out.liveObjects = impl_->liveObjects;
    out.liveStrings = impl_->liveStrings;
    out.heapBytes = impl_->heapBytes;
    out.reservedBytes = impl_->heap.stats().committedBytes;
    out.totalAllocated = impl_->totalAllocated;
    out.gcRuns = impl_->gcRuns;
    out.gcFreed = impl_->gcFreed;
    out.strings = impl_->stringTable.size();
#if SCRIPT_ENGINE_TYPED
    if (typed::Runtime* rt = impl_->typedIfAny()) {
        out.assemblies = rt->assemblies.size();
        out.methodsCompiled = rt->jit ? rt->jit->compiledCount() : 0;
        out.nativeCodeBytes = rt->jit ? rt->jit->codeBytes() : 0;
    }
#endif
#if SCRIPT_ENGINE_DYNAMIC
    if (dyn::Runtime* rt = impl_->dynIfAny()) {
        out.pendingJobs = rt->jobs.size();
        out.methodsCompiled += rt->jitCompiled;
        out.nativeCodeBytes += rt->jitBackend ? rt->jitBackend->cache().used() : 0;
    }
#endif
    return out;
}

const char* Context::jitBackend() const noexcept
{
#if SCRIPT_ENGINE_TYPED
    typed::Runtime* rt = impl_->typedIfAny();
    return rt && rt->jit ? rt->jit->backendName() : "interpreter";
#else
    return "interpreter";
#endif
}

void Context::setInterruptHandler(std::function<bool()> handler)
{
    Lock lock(*this);
    impl_->interrupt = std::move(handler);
}

void Context::setPrintHandler(std::function<void(std::string_view, bool)> handler)
{
    Lock lock(*this);
    impl_->print = std::move(handler);
}

void Context::setReadLineHandler(std::function<bool(std::string&)> handler)
{
    Lock lock(*this);
    impl_->readLine = std::move(handler);
}

void Context::setUnhandledRejectionHandler(std::function<void(const Value&)> handler)
{
    Lock lock(*this);
    impl_->unhandledRejection = std::move(handler);
}

}  // namespace script
