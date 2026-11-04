// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The C++ binding layer seen from C: static host classes, host instances,
// values crossing in both directions, and errors crossing the boundary.

#include <cmath>
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
    static Vec2 unit() { return Vec2{1, 0}; }
};

struct Counter
{
    int n = 0;
    void bump() { ++n; }
    int value() const { return n; }
};

}  // namespace

TEST(staticHostClasses)
{
    cc::Context ctx;
    int calls = 0;
    ctx.hostClass("Env")
        .method("Greet", [](std::string name) { return "hello " + name; })
        .method("Add", [](int a, int b) { return a + b; })
        .method("Half", [](double d) { return d / 2; })
        .method("Tick", [&calls] { ++calls; })
        .method("Ready", [] { return true; });
    CHECK_EQ(ctx.eval("#include <string.h>\nint main() { return strlen(Env.Greet(\"c\")); }").toInt(), 7);
    CHECK_EQ(ctx.eval("int main() { return Env.Add(2, 3) + (int)Env.Half(9); }").toInt(), 9);
    CHECK_EQ(ctx.eval("int main() { int n = Env.Half(3); return n; }").toInt(), 1);
    CHECK_EQ(ctx.eval("int main() { Env.Tick(); Env.Tick(); if (Env.Ready()) Env.Tick(); return Env.Ready(); }").toInt(), 1);
    CHECK_EQ(calls, 3);
    std::string out;
    ctx.setPrintHandler([&out](std::string_view text, bool) { out += text; });
    ctx.eval("#include <stdio.h>\nint main() { printf(\"%s %d\\n\", Env.Greet(\"world\"), Env.Add(1, 1)); return 0; }");
    CHECK_EQ(out, std::string("hello world 2\n"));
}

TEST(hostInstances)
{
    cc::Context ctx;
    cc::Class<Vec2>(ctx, "Vec2")
        .constructor<double, double>()
        .field("X", &Vec2::x)
        .field("Y", &Vec2::y)
        .method("Length", &Vec2::length)
        .method("Scale", &Vec2::scale)
        .staticMethod("Unit", &Vec2::unit)
        .install();
    CHECK_EQ(ctx.eval("int main() { Vec2 v = Vec2(3, 4); return (int)v.Length(); }").toInt(), 5);
    CHECK_EQ(ctx.eval("int main() { Vec2 v = Vec2(1, 2); v.X = 10; return (int)(v.X + v.Y); }").toInt(), 12);
    CHECK_EQ(ctx.eval("int main() { Vec2 v = Vec2(1, 2); v.Scale(3); return (int)(v.X * v.Y); }").toInt(), 18);
    CHECK_EQ(ctx.eval("int main() { Vec2 u = Vec2.Unit(); return (int)u.X; }").toInt(), 1);
    CHECK_EQ(ctx.eval("double len(Vec2* v) { return v->Length(); }\nint main() { Vec2 v = Vec2(6, 8); return (int)len(&v); }").toInt(), 10);
    CHECK_EQ(ctx.eval("struct Body { Vec2 pos; int id; };\nint main() { struct Body b; b.pos = Vec2(2, 0); b.id = 1; return (int)b.pos.X + b.id; }").toInt(), 3);
}

TEST(sharedInstanceAcrossTheBoundary)
{
    cc::Context ctx;
    Counter shared;
    cc::Class<Counter>(ctx, "Counter")
        .constructor<>()
        .method("Bump", &Counter::bump)
        .method("Value", &Counter::value)
        .install();
    ctx.hostClass("Host").method("Shared", [&shared] { return &shared; });
    CHECK_EQ(ctx.eval("int main() { Counter c = Host.Shared(); for (int i = 0; i < 3; i++) c.Bump(); return c.Value(); }").toInt(), 3);
    CHECK_EQ(shared.n, 3);
    CHECK_EQ(ctx.eval("void bumpAll(Counter c, int n) { while (n--) c.Bump(); }\nint main() { bumpAll(Host.Shared(), 2); return Host.Shared().Value(); }").toInt(), 5);
}

TEST(errorsCrossBothWays)
{
    cc::Context ctx;
    ctx.hostClass("Env").method("Fail", []() -> int {
        throw script::Error(cc::ErrorCode::ArgumentException, "bad argument");
    });
    CHECK_THROWS(ctx.eval("int main() { Env.Fail(); return 0; }"), cc::ErrorCode::ArgumentException);
    // A host member C cannot spell is not visible, and says so.
    ctx.hostClass("Odd").method("Take", [](std::map<std::string, double>) { return 1; });
    try {
        ctx.compile("int main() { return Odd.Take(0); }");
        CHECK(false);
    } catch (const cc::Error& e) {
        CHECK(e.code() == cc::ErrorCode::SyntaxError);
        CHECK(std::string(e.what()).find("no member") != std::string::npos);
    }
}

TEST(limitsApply)
{
    cc::Context::Options options;
    options.stackLimit = 100;
    cc::Context ctx(options);
    CHECK_THROWS(ctx.eval("int down(int n) { return down(n + 1); }\nint main() { return down(0); }"), cc::ErrorCode::StackOverflow);
    cc::Context ctx2;
    int ticks = 0;
    ctx2.setInterruptHandler([&ticks] { return ++ticks > 2; });
    CHECK_THROWS(ctx2.eval("int main() { while (1) { } return 0; }"), cc::ErrorCode::Interrupted);
}
