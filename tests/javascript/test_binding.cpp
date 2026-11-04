// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The C++ binding layer: values in, functions out, classes both ways.

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

}  // namespace

TEST(valuesConvertBothWays)
{
    js::Context ctx;
    CHECK_EQ(ctx.value(42).toDouble(), 42.0);
    CHECK_EQ(ctx.value(std::string("hi")).toString(), std::string("hi"));
    CHECK_EQ(ctx.value("literal").toString(), std::string("literal"));
    CHECK_EQ(ctx.value(true).toBool(), true);

    js::Value list = ctx.value(std::vector<int>{1, 2, 3});
    CHECK(list.isArray());
    CHECK_EQ(list.length(), 3u);
    CHECK_EQ(ctx.as<std::vector<int>>(list).at(2), 3);

    // A host `int` is a number: it is `===` to the literal, and a key to it.
    ctx.global().set("two", ctx.fn("two", [] { return 2; }));
    CHECK(ctx.eval("two() === 2 && two() == 2 && [2].indexOf(two()) === 0").toBool());

    js::Value map = ctx.value(std::map<std::string, double>{{"a", 1.5}});
    CHECK_EQ(map.get("a").toDouble(), 1.5);
    const std::map<std::string, double> roundTrip = ctx.as<std::map<std::string, double>>(map);
    CHECK_EQ(roundTrip.at("a"), 1.5);

    CHECK(ctx.value(std::optional<int>{}).isNull());
    CHECK_EQ(ctx.value(std::optional<int>{7}).toDouble(), 7.0);
}

TEST(nativeFunctionsAreCallableFromScript)
{
    js::Context ctx;
    ctx.global().set("add", ctx.fn("add", [](int a, int b) { return a + b; }));
    ctx.global().set("greet",
                     ctx.fn("greet", [](const std::string& n) { return "hello " + n; }));
    ctx.global().set("noop", ctx.fn("noop", [] {}));

    CHECK_EQ(ctx.eval("add(2, 3)").toDouble(), 5.0);
    CHECK_EQ(ctx.eval("greet('world')").toString(), std::string("hello world"));
    CHECK(ctx.eval("noop()").isUndefined());
    CHECK_EQ(ctx.eval("[1,2,3].map(x => add(x, 10)).join(',')").toString(),
             std::string("11,12,13"));
}

TEST(rawCallInfoGivesFullControl)
{
    js::Context ctx;
    ctx.global().set("sum", ctx.function("sum", [](js::CallInfo& ci) {
                         double total = 0;
                         for (const js::Value& v : ci.args())
                             total += v.toDouble();
                         return js::Value(total);
                     }));
    CHECK_EQ(ctx.eval("sum(1, 2, 3, 4)").toDouble(), 10.0);
}

TEST(nativeFunctionsCanThrowIntoScript)
{
    js::Context ctx;
    ctx.global().set("failing", ctx.function("failing", [](js::CallInfo& ci) -> js::Value {
                         ci.fail(js::ErrorCode::RangeError, "out of range");
                     }));
    CHECK_EQ(ctx.eval("try { failing() } catch (e) { e.name + ': ' + e.message }").toString(),
             std::string("RangeError: out of range"));
}

TEST(cppClassesBecomeJavaScriptClasses)
{
    js::Context ctx;
    js::Class<Vec2>(ctx, "Vec2")
        .constructor<double, double>()
        .field("x", &Vec2::x)
        .field("y", &Vec2::y)
        .method("length", &Vec2::length)
        .method("scale", &Vec2::scale)
        .method("describe", &Vec2::describe)
        .staticMethod("unit", &Vec2::unit)
        .install();

    CHECK_EQ(ctx.eval("new Vec2(3, 4).length()").toDouble(), 5.0);
    CHECK_EQ(ctx.eval("const v = new Vec2(1, 2); v.x = 10; v.x + v.y").toDouble(), 12.0);
    CHECK_EQ(ctx.eval("const v = new Vec2(1, 2); v.scale(3); v.describe('v')").toString(),
             std::string("v:3,6"));
    CHECK_EQ(ctx.eval("Vec2.unit().x").toDouble(), 1.0);
    CHECK_EQ(ctx.eval("new Vec2(0, 0) instanceof Vec2").toString(), std::string("true"));
}

TEST(cppObjectsCanBeHandedInAndOut)
{
    js::Context ctx;
    js::Class<Counter>(ctx, "Counter").method("bump", &Counter::bump).install();

    Counter counter;
    ctx.global().set("shared", ctx.value(&counter));
    ctx.eval("shared.bump(); shared.bump(); shared.bump()");
    CHECK_EQ(counter.value(), 3);

    auto owned = std::make_shared<Counter>();
    ctx.global().set("owned", ctx.value(owned));
    ctx.eval("owned.bump()");
    CHECK_EQ(owned->value(), 1);
    CHECK_EQ(owned.use_count(), 2L);
}

TEST(scriptFunctionsAreCallableFromCpp)
{
    js::Context ctx;
    js::Value fn = ctx.eval("(a, b) => a * b");
    CHECK(fn.isFunction());
    CHECK_EQ(fn(6, 7).toDouble(), 42.0);
    CHECK_EQ(fn.call({js::Value(2), js::Value(3)}).toDouble(), 6.0);

    auto multiply = ctx.as<std::function<int(int, int)>>(fn);
    CHECK_EQ(multiply(4, 5), 20);

    js::Value object = ctx.eval("({ n: 10, add(k) { return this.n + k; } })");
    CHECK_EQ(object.invoke("add", {js::Value(5)}).toDouble(), 15.0);
}

TEST(objectsAreBuiltAndInspectedFromCpp)
{
    js::Context ctx;
    js::Value config = ctx.object();
    config.set("name", "libjs");
    config.set("version", 1);
    config.set("tags", ctx.value(std::vector<std::string>{"small", "fast"}));

    ctx.global().set("config", config);
    CHECK_EQ(ctx.eval("config.name + config.version + config.tags[1]").toString(),
             std::string("libjs1fast"));
    CHECK_EQ(config.keys().size(), 3u);
    CHECK_EQ(config.toJson(), std::string("{\"name\":\"libjs\",\"version\":1,"
                                          "\"tags\":[\"small\",\"fast\"]}"));
    CHECK(config.hasOwn("name"));
    CHECK(config.remove("name"));
    CHECK(!config.hasOwn("name"));
}

TEST(accessorsBridgeToCppCallbacks)
{
    js::Context ctx;
    double backing = 3;
    js::Value target = ctx.object();
    target.defineAccessor(
        "value",
        ctx.function("get value", [&backing](js::CallInfo&) { return js::Value(backing); }),
        ctx.function("set value", [&backing](js::CallInfo& ci) {
            backing = ci.arg(0).toDouble();
            return js::Value();
        }));
    ctx.global().set("target", target);
    CHECK_EQ(ctx.eval("target.value").toDouble(), 3.0);
    ctx.eval("target.value = 9");
    CHECK_EQ(backing, 9.0);
}

TEST(hostAsyncWorkResolvesPromises)
{
    js::Context ctx;
    js::Value resolveFn;
    ctx.global().set("later", ctx.function("later", [&resolveFn](js::CallInfo& ci) {
                         resolveFn = ci.arg(0);
                         return js::Value();
                     }));
    ctx.eval("globalThis.out = ''; new Promise(r => later(r)).then(v => { out = 'got ' + v; })");
    resolveFn.call({ctx.value(7)});
    ctx.runJobs();
    CHECK_EQ(ctx.eval("out").toString(), std::string("got 7"));
}
