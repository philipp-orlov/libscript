// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// System.Math.
#include <algorithm>
#include <cmath>

#include "script/internal/typed/builtins.hpp"

namespace script {
namespace internal {
namespace typed {

void installMath(TypeTable& table)
{
    const CoreTypes& core = table.core();
    TypeInfo* math = table.create(TypeKind::Class, "System.Math");
    math->base = core.objectType;
    math->isStaticClass = true;
    math->isSealed = true;

    const TypeInfo* d = core.doubleType;
    const TypeInfo* i = core.intType;
    const TypeInfo* l = core.longType;

    auto unary = [&table, math, d](const char* name, double (*fn)(double)) {
        table.addMethod(math, name, d, {table.param("value", d)}, MethodStatic,
                        [fn](CallInfo& call) { return Value(fn(call.arg(0).toDouble())); });
    };
    unary("Sqrt", [](double x) { return std::sqrt(x); });
    unary("Abs", [](double x) { return std::fabs(x); });
    unary("Floor", [](double x) { return std::floor(x); });
    unary("Ceiling", [](double x) { return std::ceil(x); });
    unary("Round", [](double x) { return std::nearbyint(x); });
    unary("Truncate", [](double x) { return std::trunc(x); });
    unary("Sin", [](double x) { return std::sin(x); });
    unary("Cos", [](double x) { return std::cos(x); });
    unary("Tan", [](double x) { return std::tan(x); });
    unary("Asin", [](double x) { return std::asin(x); });
    unary("Acos", [](double x) { return std::acos(x); });
    unary("Atan", [](double x) { return std::atan(x); });
    unary("Exp", [](double x) { return std::exp(x); });
    unary("Log", [](double x) { return std::log(x); });
    unary("Log10", [](double x) { return std::log10(x); });
    unary("Log2", [](double x) { return std::log2(x); });

    table.addMethod(math, "Pow", d, {table.param("x", d), table.param("y", d)}, MethodStatic,
                    [](CallInfo& call) {
                        return Value(std::pow(call.arg(0).toDouble(), call.arg(1).toDouble()));
                    });
    table.addMethod(math, "Atan2", d, {table.param("y", d), table.param("x", d)}, MethodStatic,
                    [](CallInfo& call) {
                        return Value(std::atan2(call.arg(0).toDouble(), call.arg(1).toDouble()));
                    });

    // The integer overloads matter: without them `Math.Max(a, b)` on two ints
    // would go through `double` and come back wrong at the far end of the
    // range.
    table.addMethod(math, "Abs", i, {table.param("value", i)}, MethodStatic, [](CallInfo& call) {
        const std::int32_t v = call.arg(0).toInt();
        return Value(v < 0 ? -v : v);
    });
    table.addMethod(math, "Abs", l, {table.param("value", l)}, MethodStatic, [](CallInfo& call) {
        const std::int64_t v = call.arg(0).toLong();
        return Value(v < 0 ? -v : v);
    });
    table.addMethod(math, "Max", i, {table.param("a", i), table.param("b", i)}, MethodStatic,
                    [](CallInfo& call) {
                        return Value(std::max(call.arg(0).toInt(), call.arg(1).toInt()));
                    });
    table.addMethod(math, "Min", i, {table.param("a", i), table.param("b", i)}, MethodStatic,
                    [](CallInfo& call) {
                        return Value(std::min(call.arg(0).toInt(), call.arg(1).toInt()));
                    });
    table.addMethod(math, "Max", l, {table.param("a", l), table.param("b", l)}, MethodStatic,
                    [](CallInfo& call) {
                        return Value(std::max(call.arg(0).toLong(), call.arg(1).toLong()));
                    });
    table.addMethod(math, "Min", l, {table.param("a", l), table.param("b", l)}, MethodStatic,
                    [](CallInfo& call) {
                        return Value(std::min(call.arg(0).toLong(), call.arg(1).toLong()));
                    });
    table.addMethod(math, "Max", d, {table.param("a", d), table.param("b", d)}, MethodStatic,
                    [](CallInfo& call) {
                        return Value(std::max(call.arg(0).toDouble(), call.arg(1).toDouble()));
                    });
    table.addMethod(math, "Min", d, {table.param("a", d), table.param("b", d)}, MethodStatic,
                    [](CallInfo& call) {
                        return Value(std::min(call.arg(0).toDouble(), call.arg(1).toDouble()));
                    });

    FieldInfo pi;
    pi.name = ascii("PI");
    pi.type = d;
    pi.owner = math;
    pi.access = Access::Public;
    pi.isStatic = true;
    pi.isConst = true;
    pi.constValue = Value(3.14159265358979323846);
    math->statics.push_back(pi);

    FieldInfo e;
    e.name = ascii("E");
    e.type = d;
    e.owner = math;
    e.access = Access::Public;
    e.isStatic = true;
    e.isConst = true;
    e.constValue = Value(2.71828182845904523536);
    math->statics.push_back(e);
}

}  // namespace typed
}  // namespace internal
}  // namespace script
