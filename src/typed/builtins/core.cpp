// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// System.Object, the exception hierarchy, Console, Convert and the static
// members of the primitive types.
#include "script/internal/typed/builtins.hpp"
#include "script/internal/typed/numconv.hpp"

namespace script {
namespace internal {
namespace typed {

namespace {

/// Give a type a field of its own, after its base class's.
FieldInfo* addField(TypeInfo* type, const char* name, const TypeInfo* fieldType)
{
    FieldInfo field;
    field.name = ascii(name);
    field.type = fieldType;
    field.owner = type;
    field.access = Access::Private;
    field.slot = type->instanceSlots++;
    type->fields.push_back(std::move(field));
    return &type->fields.back();
}

/// Make a method virtual and give it a vtable slot, so a program can override
/// it. `ToString` is the one every C# program overrides.
void makeVirtual(TypeInfo* type, MethodInfo* method)
{
    method->flags |= MethodVirtual;
    method->vtableSlot = std::int32_t(type->vtable.size());
    type->vtable.push_back(method);
}

TypeInfo* defineException(TypeTable& table, const char* name, const TypeInfo* base,
                          ErrorCode code)
{
    TypeInfo* type = table.create(TypeKind::Class, name);
    type->base = base;
    type->instanceSlots = base ? base->instanceSlots : 0;
    type->vtable = base ? base->vtable : std::vector<MethodInfo*>();
    table.registerException(code, type);

    const TypeInfo* stringType = table.core().stringType;
    table.addMethod(type, ".ctor", table.core().voidType, {}, MethodConstructor,
                    [](CallInfo& call) { return call.thisValue(); });
    table.addMethod(type, ".ctor", table.core().voidType,
                    {table.param("message", stringType)}, MethodConstructor,
                    [](CallInfo& call) {
                        auto* instance =
                            static_cast<CSInstance*>(detail::Access::ptr(call.thisValue()));
                        if (instance && instance->type) {
                            if (const FieldInfo* field =
                                    instance->type->findField(ascii("_message")))
                                instance->at(field->slot) = call.arg(0);
                        }
                        return Value();
                    });
    return type;
}

}  // namespace

void installCore(TypeTable& table)
{
    CoreTypes& core = table.mutableCore();
    TypeInfo* objectType = const_cast<TypeInfo*>(core.objectType);
    const TypeInfo* stringType = core.stringType;
    const TypeInfo* intType = core.intType;
    const TypeInfo* boolType = core.boolType;
    const TypeInfo* doubleType = core.doubleType;
    const TypeInfo* longType = core.longType;
    const TypeInfo* voidType = core.voidType;

    // ---- System.Object ----
    makeVirtual(objectType,
                table.addMethod(objectType, "ToString", stringType, {}, 0, [](CallInfo& call) {
                    Runtime& rt = runtimeOf(call);
                    const Value& self = call.thisValue();
                    const TypeInfo* type = typeOf(rt, self);
                    // Reached only when nothing overrode it, so the .NET
                    // answer -- the type's name -- is the right one.
                    if (self.isNumber() || self.isBool() || self.isChar() || self.isString())
                        return rt.str(toStringValue(rt, self));
                    return rt.str(type ? type->fullName : ascii("System.Object"));
                }));
    makeVirtual(objectType, table.addMethod(objectType, "Equals", boolType,
                                            {table.param("other", core.objectType)}, 0,
                                            [](CallInfo& call) {
                                                return Value(valueEquals(call.thisValue(),
                                                                         call.arg(0)));
                                            }));
    makeVirtual(objectType, table.addMethod(objectType, "GetHashCode", intType, {}, 0,
                                            [](CallInfo& call) {
                                                return Value(std::int32_t(
                                                    hashValue(call.thisValue()) & 0x7fffffffu));
                                            }));
    table.addMethod(objectType, "Equals", boolType,
                    {table.param("a", core.objectType), table.param("b", core.objectType)},
                    MethodStatic, [](CallInfo& call) {
                        return Value(valueEquals(call.arg(0), call.arg(1)));
                    });
    table.addMethod(objectType, "ReferenceEquals", boolType,
                    {table.param("a", core.objectType), table.param("b", core.objectType)},
                    MethodStatic, [](CallInfo& call) {
                        return Value(detail::Access::ptr(call.arg(0)) ==
                                     detail::Access::ptr(call.arg(1)));
                    });

    // ---- interfaces the engine knows about ----
    TypeInfo* enumerable = table.create(TypeKind::Interface, "System.Collections.IEnumerable");
    enumerable->base = objectType;
    core.enumerableType = enumerable;
    TypeInfo* disposable = table.create(TypeKind::Interface, "System.IDisposable");
    disposable->base = objectType;
    table.addMethod(disposable, "Dispose", voidType, {}, MethodVirtual | MethodAbstract, nullptr);
    core.disposableType = disposable;

    // `System.Array` exists for its statics; an array's own members -- Length
    // and indexing -- are instructions, not calls.
    TypeInfo* arrayType = table.create(TypeKind::Class, "System.Array");
    arrayType->base = objectType;
    arrayType->isStaticClass = true;
    table.addMethod(arrayType, "Slice", core.objectType,
                    {table.param("source", core.objectType), table.param("start", intType),
                     table.param("end", intType)},
                    MethodStatic, [](CallInfo& call) {
                        Runtime& rt = runtimeOf(call);
                        auto* source = asArray(call.arg(0));
                        if (!source)
                            fail(call, ErrorCode::NullReference, "the array is null");
                        const std::int32_t length = std::int32_t(source->length);
                        std::int32_t from = call.arg(1).toInt();
                        std::int32_t to = call.arg(2).toInt();
                        if (from < 0)
                            from = 0;
                        if (to > length)
                            to = length;
                        if (to < from)
                            to = from;
                        Value result = newArrayOf(rt, source->elemType, to - from);
                        auto* target = asArray(result);
                        for (std::int32_t i = from; i < to; ++i)
                            target->set(std::uint32_t(i - from), source->get(std::uint32_t(i)));
                        return result;
                    });

    // ---- the exception hierarchy ----
    TypeInfo* exception = table.create(TypeKind::Class, "System.Exception");
    exception->base = objectType;
    exception->vtable = objectType->vtable;
    addField(exception, "_message", stringType);
    addField(exception, "_stack", stringType);
    addField(exception, "_inner", nullptr);
    core.exceptionType = exception;
    table.registerException(ErrorCode::Exception, exception);

    table.addMethod(exception, ".ctor", voidType, {}, MethodConstructor,
                    [](CallInfo& call) { return call.thisValue(); });
    table.addMethod(exception, ".ctor", voidType, {table.param("message", stringType)},
                    MethodConstructor, [](CallInfo& call) {
                        auto* instance =
                            static_cast<CSInstance*>(detail::Access::ptr(call.thisValue()));
                        if (instance)
                            instance->at(0) = call.arg(0);
                        return Value();
                    });
    table.addProperty(exception, "Message", stringType,
                      [](CallInfo& call) {
                          auto* instance =
                              static_cast<CSInstance*>(detail::Access::ptr(call.thisValue()));
                          if (!instance || instance->at(0).isNull())
                              return runtimeOf(call).str(ascii("An exception occurred."));
                          return instance->at(0);
                      },
                      nullptr);
    table.addProperty(exception, "StackTrace", stringType,
                      [](CallInfo& call) {
                          auto* instance =
                              static_cast<CSInstance*>(detail::Access::ptr(call.thisValue()));
                          return instance ? instance->at(1) : Value();
                      },
                      nullptr);
    // `ToString` on an exception is its type and message, as on .NET.
    MethodInfo* exceptionToString =
        table.addMethod(exception, "ToString", stringType, {}, MethodOverride,
                        [](CallInfo& call) {
                            Runtime& rt = runtimeOf(call);
                            auto* instance =
                                static_cast<CSInstance*>(detail::Access::ptr(call.thisValue()));
                            String out = instance && instance->type ? instance->type->fullName
                                                                    : ascii("System.Exception");
                            if (instance && !instance->at(0).isNull())
                                out += ascii(": ") + String(textOf(instance->at(0)));
                            return rt.str(out);
                        });
    exceptionToString->vtableSlot = 0;
    if (!exception->vtable.empty())
        exception->vtable[0] = exceptionToString;

    struct ExceptionSpec
    {
        const char* name;
        ErrorCode code;
    };
    const ExceptionSpec specs[] = {
        {"System.SystemException", ErrorCode::Exception},
        {"System.NullReferenceException", ErrorCode::NullReference},
        {"System.InvalidCastException", ErrorCode::InvalidCast},
        {"System.IndexOutOfRangeException", ErrorCode::IndexOutOfRange},
        {"System.ArgumentException", ErrorCode::ArgumentException},
        {"System.ArgumentNullException", ErrorCode::ArgumentNull},
        {"System.ArgumentOutOfRangeException", ErrorCode::ArgumentOutOfRange},
        {"System.DivideByZeroException", ErrorCode::DivideByZero},
        {"System.OverflowException", ErrorCode::Overflow},
        {"System.InvalidOperationException", ErrorCode::InvalidOperation},
        {"System.NotSupportedException", ErrorCode::NotSupported},
        {"System.FormatException", ErrorCode::FormatException},
        {"System.Collections.Generic.KeyNotFoundException", ErrorCode::KeyNotFound},
        {"System.OutOfMemoryException", ErrorCode::OutOfMemory},
        {"System.StackOverflowException", ErrorCode::StackOverflow},
    };
    for (const ExceptionSpec& spec : specs)
        defineException(table, spec.name, exception, spec.code);

    // ---- System.Console ----
    TypeInfo* console = table.create(TypeKind::Class, "System.Console");
    console->base = objectType;
    console->isStaticClass = true;
    console->isSealed = true;

    auto write = [](CallInfo& call, bool newline) {
        Runtime& rt = runtimeOf(call);
        String text = call.size() ? toStringValue(rt, call.arg(0)) : String();
        if (newline)
            text.push_back(u'\n');
        rt.ctx.write(text, false);
        return Value();
    };
    for (const TypeInfo* argType : {core.objectType, stringType, intType, longType, doubleType,
                                    boolType, core.charType}) {
        table.addMethod(console, "WriteLine", voidType, {table.param("value", argType)},
                        MethodStatic,
                        [write](CallInfo& call) { return write(call, true); });
        table.addMethod(console, "Write", voidType, {table.param("value", argType)}, MethodStatic,
                        [write](CallInfo& call) { return write(call, false); });
    }
    table.addMethod(console, "WriteLine", voidType, {}, MethodStatic, [](CallInfo& call) {
        runtimeOf(call).write(ascii("\n"), false);
        return Value();
    });
    table.addMethod(console, "Error", voidType, {table.param("value", core.objectType)},
                    MethodStatic, [](CallInfo& call) {
                        Runtime& rt = runtimeOf(call);
                        rt.ctx.write(toStringValue(rt, call.arg(0)) + ascii("\n"), true);
                        return Value();
                    });
    table.addMethod(console, "ReadLine", stringType, {}, MethodStatic, [](CallInfo& call) {
        Runtime& rt = runtimeOf(call);
        std::string line;
        if (rt.ctx.readLine && rt.ctx.readLine(line))
            return rt.str(fromUtf8(line));
        return Value();
    });

    // ---- System.Convert ----
    TypeInfo* convert = table.create(TypeKind::Class, "System.Convert");
    convert->base = objectType;
    convert->isStaticClass = true;
    table.addMethod(convert, "ToInt32", intType, {table.param("value", core.objectType)},
                    MethodStatic, [](CallInfo& call) {
                        const Value& v = call.arg(0);
                        if (v.isString()) {
                            std::int64_t parsed = 0;
                            if (!parseInteger(textOf(v), parsed))
                                fail(call, ErrorCode::FormatException,
                                     "the input string was not in a correct format");
                            return Value(std::int32_t(parsed));
                        }
                        return Value(std::int32_t(v.toDouble()));
                    });
    table.addMethod(convert, "ToInt64", longType, {table.param("value", core.objectType)},
                    MethodStatic, [](CallInfo& call) {
                        const Value& v = call.arg(0);
                        if (v.isString()) {
                            std::int64_t parsed = 0;
                            if (!parseInteger(textOf(v), parsed))
                                fail(call, ErrorCode::FormatException,
                                     "the input string was not in a correct format");
                            return Value(parsed);
                        }
                        return Value(std::int64_t(v.toDouble()));
                    });
    table.addMethod(convert, "ToDouble", doubleType, {table.param("value", core.objectType)},
                    MethodStatic, [](CallInfo& call) {
                        const Value& v = call.arg(0);
                        if (v.isString()) {
                            double parsed = 0;
                            if (!parseDouble(textOf(v), parsed))
                                fail(call, ErrorCode::FormatException,
                                     "the input string was not in a correct format");
                            return Value(parsed);
                        }
                        return Value(v.toDouble());
                    });
    table.addMethod(convert, "ToBoolean", boolType, {table.param("value", core.objectType)},
                    MethodStatic,
                    [](CallInfo& call) { return Value(call.arg(0).toBool()); });
    table.addMethod(convert, "ToString", stringType, {table.param("value", core.objectType)},
                    MethodStatic, [](CallInfo& call) {
                        Runtime& rt = runtimeOf(call);
                        return rt.str(toStringValue(rt, call.arg(0)));
                    });

    // ---- the static members of the numeric types ----
    struct NumericSpec
    {
        const TypeInfo* type;
        bool integral;
    };
    const NumericSpec numerics[] = {
        {intType, true}, {longType, true}, {doubleType, false}, {core.floatType, false},
    };
    for (const NumericSpec& spec : numerics) {
        TypeInfo* type = const_cast<TypeInfo*>(spec.type);
        const bool integral = spec.integral;
        const TypeInfo* self = spec.type;
        table.addMethod(type, "Parse", self, {table.param("s", stringType)}, MethodStatic,
                        [integral, self](CallInfo& call) {
                            if (integral) {
                                std::int64_t parsed = 0;
                                if (!parseInteger(textOf(call.arg(0)), parsed))
                                    fail(call, ErrorCode::FormatException,
                                         "the input string was not in a correct format");
                                return self->kind == TypeKind::Long
                                           ? Value(parsed)
                                           : Value(std::int32_t(parsed));
                            }
                            double parsed = 0;
                            if (!parseDouble(textOf(call.arg(0)), parsed))
                                fail(call, ErrorCode::FormatException,
                                     "the input string was not in a correct format");
                            return self->kind == TypeKind::Float ? Value(float(parsed))
                                                                 : Value(parsed);
                        });
        table.addMethod(type, "TryParse", boolType,
                        {table.param("s", stringType),
                         table.param("result", self, ParamMode::Out)},
                        MethodStatic, [integral, self](CallInfo& call) {
                            bool ok = false;
                            Value result = defaultValue(self);
                            if (integral) {
                                std::int64_t parsed = 0;
                                ok = parseInteger(textOf(call.arg(0)), parsed);
                                if (ok)
                                    result = self->kind == TypeKind::Long
                                                 ? Value(parsed)
                                                 : Value(std::int32_t(parsed));
                            } else {
                                double parsed = 0;
                                ok = parseDouble(textOf(call.arg(0)), parsed);
                                if (ok)
                                    result = self->kind == TypeKind::Float ? Value(float(parsed))
                                                                           : Value(parsed);
                            }
                            storeRef(call.arg(1), result);
                            return Value(ok);
                        });
        table.addMethod(type, "ToString", stringType, {table.param("format", stringType)}, 0,
                        [](CallInfo& call) {
                            Runtime& rt = runtimeOf(call);
                            const Value& value = call.thisValue();
                            String out;
                            if (!formatNumber(value.toDouble(),
                                              !value.isDouble() && !value.isFloat(),
                                              value.toLong(), textOf(call.arg(0)), out))
                                fail(call, ErrorCode::FormatException,
                                     "the format string is not supported");
                            return rt.str(out);
                        });
    }
    table.addProperty(const_cast<TypeInfo*>(intType), "MaxValue", intType,
                      [](CallInfo&) { return Value(std::int32_t(2147483647)); }, nullptr, true);
    table.addProperty(const_cast<TypeInfo*>(intType), "MinValue", intType,
                      [](CallInfo&) { return Value(std::int32_t(-2147483647 - 1)); }, nullptr,
                      true);

    // ---- System.Char, as far as a program usually needs it ----
    TypeInfo* charType = const_cast<TypeInfo*>(core.charType);
    table.addMethod(charType, "IsDigit", boolType, {table.param("c", core.charType)},
                    MethodStatic, [](CallInfo& call) {
                        const std::int32_t c = call.arg(0).toInt();
                        return Value(c >= '0' && c <= '9');
                    });
    table.addMethod(charType, "IsLetter", boolType, {table.param("c", core.charType)},
                    MethodStatic, [](CallInfo& call) {
                        const std::int32_t c = call.arg(0).toInt();
                        return Value((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c > 127);
                    });
    table.addMethod(charType, "IsWhiteSpace", boolType, {table.param("c", core.charType)},
                    MethodStatic, [](CallInfo& call) {
                        const std::int32_t c = call.arg(0).toInt();
                        return Value(c == ' ' || c == '\t' || c == '\n' || c == '\r');
                    });
    table.addMethod(charType, "ToUpper", core.charType, {table.param("c", core.charType)},
                    MethodStatic, [](CallInfo& call) {
                        const std::int32_t c = call.arg(0).toInt();
                        return Value(char16_t(c >= 'a' && c <= 'z' ? c - 32 : c));
                    });
    table.addMethod(charType, "ToLower", core.charType, {table.param("c", core.charType)},
                    MethodStatic, [](CallInfo& call) {
                        const std::int32_t c = call.arg(0).toInt();
                        return Value(char16_t(c >= 'A' && c <= 'Z' ? c + 32 : c));
                    });
}

}  // namespace typed
}  // namespace internal
}  // namespace script
