// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The C++ binding layer seen from Python: host functions, host classes,
// values crossing in both directions, and errors crossing the boundary.

#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "lang.hpp"
#include "tests/test_util.hpp"

namespace {

struct Vec2
{
    double x = 0;
    double y = 0;

    double length() const { return std::sqrt(x * x + y * y); }
    void scale(double k)
    {
        x *= k;
        y *= k;
    }
    std::string describe(const std::string& tag) const
    {
        return tag + ":" + std::to_string(int(x)) + "," + std::to_string(int(y));
    }
    static Vec2 unit() { return Vec2{1, 0}; }
};

struct Counter
{
    int n = 0;
    void bump() { ++n; }
    int value() const { return n; }
};

void bindVec2(py::Context& ctx)
{
    py::Class<Vec2>(ctx, "Vec2")
        .constructor<double, double>()
        .field("x", &Vec2::x)
        .field("y", &Vec2::y)
        .method("length", &Vec2::length)
        .method("scale", &Vec2::scale)
        .method("describe", &Vec2::describe)
        .staticMethod("unit", &Vec2::unit)
        .install();
}

}  // namespace

TEST(hostFunctionsAreCallable)
{
    py::Context ctx;
    ctx.global().set("add", ctx.fn("add", [](int a, int b) { return a + b; }));
    ctx.global().set("greet", ctx.fn("greet", [](const std::string& n) { return "hello " + n; }));
    ctx.global().set("noop", ctx.fn("noop", [] {}));
    CHECK_EQ(ctx.eval("add(2, 3)").toDouble(), 5.0);
    CHECK_EQ(ctx.eval("greet('world')").toString(), std::string("hello world"));
    CHECK(ctx.eval("noop()").isUndefined());
    CHECK_EQ(ctx.eval("str(noop())").toString(), std::string("None"));
    CHECK_EQ(ctx.eval("','.join(str(add(x, 10)) for x in [1, 2, 3])").toString(), std::string("11,12,13"));
    CHECK_EQ(ctx.eval("def twice(f, v):\n    return f(f(v, 1), 1)\ntwice(add, 5)").toInt(), 7);
}

TEST(hostStaticClasses)
{
    py::Context ctx;
    ctx.hostClass("Env")
        .method("Greet", [](std::string name) { return "hello " + name; })
        .method("Sum", [](std::vector<double> xs) {
            double t = 0;
            for (double x : xs)
                t += x;
            return t;
        });
    CHECK_EQ(ctx.eval("Env.Greet('py').upper()").toString(), std::string("HELLO PY"));
    CHECK_EQ(ctx.eval("Env.Sum([1, 2, 3.5])").toDouble(), 6.5);
    CHECK_EQ(ctx.eval("Env.Sum([x * x for x in range(4)])").toDouble(), 14.0);
}

TEST(cppClassesBecomePythonClasses)
{
    py::Context ctx;
    bindVec2(ctx);
    CHECK_EQ(ctx.eval("Vec2(3, 4).length()").toDouble(), 5.0);
    CHECK_EQ(ctx.eval("v = Vec2(1, 2)\nv.x = 10\nv.x + v.y").toDouble(), 12.0);
    CHECK_EQ(ctx.eval("v = Vec2(1, 2)\nv.scale(3)\nv.describe('v')").toString(), std::string("v:3,6"));
    CHECK_EQ(ctx.eval("Vec2.unit().x").toDouble(), 1.0);
    CHECK_EQ(ctx.eval("str(isinstance(Vec2(0, 0), Vec2))").toString(), std::string("True"));
    CHECK_EQ(ctx.eval("vs = [Vec2(i, 0) for i in range(3)]\nsum(v.x for v in vs)").toDouble(), 3.0);
    CHECK_EQ(ctx.eval("class Named(Vec2):\n    def __init__(self, name):\n        super().__init__(2, 0)\n        self.name = name\n    def __str__(self):\n        return f'{self.name}:{self.length()}'\nstr(Named('n'))").toString(),
             std::string("n:2"));
}

TEST(hostInstancesCrossTheBoundary)
{
    py::Context ctx;
    py::Class<Counter>(ctx, "Counter")
        .constructor<>()
        .method("bump", &Counter::bump)
        .method("value", &Counter::value)
        .install();
    Counter shared;
    ctx.global().set("shared", ctx.value(&shared));
    ctx.eval("for _ in range(3):\n    shared.bump()");
    CHECK_EQ(shared.n, 3);
    CHECK_EQ(ctx.eval("shared.value()").toInt(), 3);
    ctx.eval("def bumpAll(counters):\n    for c in counters:\n        c.bump()\nbumpAll([shared, shared])");
    CHECK_EQ(shared.n, 5);
}

TEST(valuesConvertBothWays)
{
    py::Context ctx;
    py::Value list = ctx.eval("[1, 2, 3]");
    CHECK(list.isArray());
    CHECK_EQ(ctx.as<std::vector<int>>(list).at(2), 3);
    CHECK_EQ(ctx.eval("{'a': 1.5}").isObject(), true);
    CHECK_EQ(ctx.eval("(1, 'two')").get(1u).toString(), std::string("two"));
    CHECK(ctx.eval("None").isNull());
    CHECK_EQ(ctx.eval("True").toBool(), true);
    CHECK_EQ(ctx.eval("'text'").toString(), std::string("text"));
    ctx.global().set("host_list", ctx.value(std::vector<int>{4, 5, 6}));
    CHECK_EQ(ctx.eval("sum(host_list) + len(host_list) + host_list[-1]").toDouble(), 24.0);
    ctx.global().set("host_map", ctx.value(std::map<std::string, double>{{"k", 2.5}}));
    CHECK_EQ(ctx.eval("host_map['k'] * 2").toDouble(), 5.0);
}

TEST(pythonFunctionsAreCallableFromCpp)
{
    py::Context ctx;
    py::Value fn = ctx.eval("def scale(v, k=2):\n    return [x * k for x in v]\nscale");
    CHECK(fn.isFunction());
    py::Value out = fn.call({ctx.value(std::vector<int>{1, 2}), py::Value(3)});
    CHECK_EQ(ctx.as<std::vector<int>>(out).at(1), 6);
    py::Value obj = ctx.eval("class T:\n    def __init__(self):\n        self.n = 41\n    def next(self):\n        self.n += 1\n        return self.n\nT()");
    CHECK_EQ(obj.invoke("next", {}).toInt(), 42);
    CHECK_EQ(obj.get("n").toInt(), 42);
}

TEST(errorsCrossBothWays)
{
    py::Context ctx;
    ctx.global().set("failing", ctx.function("failing", [](py::CallInfo& ci) -> py::Value {
                         ci.fail(py::ErrorCode::RangeError, "out of range");
                     }));
    CHECK_EQ(ctx.eval("try:\n    failing()\nexcept Exception as e:\n    r = 'caught ' + str(e)\nr").toString(),
             std::string("caught out of range"));
    try {
        ctx.eval("raise KeyError('k')");
        CHECK(false);
    } catch (const py::Exception& e) {
        CHECK(std::string(e.what()).find("k") != std::string::npos);
        CHECK(e.value().isError());
    }
    // A compile error names the file and line.
    try {
        ctx.eval("x = (", "bad.py");
        CHECK(false);
    } catch (const py::Error& e) {
        CHECK(e.code() == py::ErrorCode::SyntaxError);
        CHECK(std::string(e.what()).find("bad.py:1") != std::string::npos);
    }
}

TEST(limitsApply)
{
    py::Context::Options options;
    options.stackLimit = 100;
    py::Context ctx(options);
    CHECK_THROWS(ctx.eval("def down(n):\n    return down(n + 1)\ndown(0)"), py::ErrorCode::StackOverflow);
    bool interrupted = false;
    py::Context ctx2;
    ctx2.setInterruptHandler([&] {
        interrupted = true;
        return true;
    });
    CHECK_THROWS(ctx2.eval("while True:\n    pass"), py::ErrorCode::Interrupted);
    CHECK(interrupted);
}
