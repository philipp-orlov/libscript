// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The C++ binding layer seen from scripted C++: static host classes, host
// instances, values crossing in both directions, and errors crossing the
// boundary.

#include <cmath>
#include <map>
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
    cpp::Context ctx;
    int calls = 0;
    ctx.hostClass("Env")
        .method("Greet", [](std::string name) { return "hello " + name; })
        .method("Add", [](int a, int b) { return a + b; })
        .method("Half", [](double d) { return d / 2; })
        .method("Tick", [&calls] { ++calls; })
        .method("Ready", [] { return true; })
        .method("Sum", [](std::vector<int> v) { int s = 0; for (int x : v) s += x; return s; })
        .constant("Answer", 42);
    CHECK_EQ(ctx.eval("#include <string>\nint main() { std::string s = Env::Greet(\"c++\"); return s.size(); }").toInt(), 9);
    CHECK_EQ(ctx.eval("int main() { return Env.Add(2, 3) + (int)Env::Half(9) + Env::Answer; }").toInt(), 51);
    CHECK_EQ(ctx.eval("int main() { Env::Tick(); if (Env::Ready()) Env::Tick(); bool r = Env::Ready(); return r; }").toInt(), 1);
    CHECK_EQ(calls, 2);
    CHECK_EQ(ctx.eval("#include <vector>\nint main() { std::vector<int> v = {1, 2, 3}; int a[] = {4, 5}; return Env::Sum(v) + Env::Sum(a); }").toInt(), 15);
    std::string out;
    ctx.setPrintHandler([&out](std::string_view text, bool) { out += text; });
    ctx.eval("#include <iostream>\nint main() { std::cout << Env::Greet(\"world\") << ' ' << Env::Add(1, 1) << std::endl; return 0; }");
    CHECK_EQ(out, std::string("hello world 2\n"));
}

TEST(hostInstances)
{
    cpp::Context ctx;
    cpp::Class<Vec2>(ctx, "Vec2")
        .constructor<double, double>()
        .field("X", &Vec2::x)
        .field("Y", &Vec2::y)
        .method("Length", &Vec2::length)
        .method("Scale", &Vec2::scale)
        .staticMethod("Unit", &Vec2::unit)
        .install();
    CHECK_EQ(ctx.eval("int main() { Vec2 v(3, 4); return (int)v.Length(); }").toInt(), 5);
    CHECK_EQ(ctx.eval("int main() { Vec2* v = new Vec2(1, 2); v->X = 10; return (int)(v->X + v->Y); }").toInt(), 12);
    CHECK_EQ(ctx.eval("int main() { auto v = Vec2(1, 2); v.Scale(3); return (int)(v.X * v.Y); }").toInt(), 18);
    CHECK_EQ(ctx.eval("int main() { Vec2 u = Vec2::Unit(); return (int)u.X; }").toInt(), 1);
    CHECK_EQ(ctx.eval("double len(const Vec2& v) { return v.Length(); }\nint main() { Vec2 v(6, 8); return (int)len(v); }").toInt(), 10);
    CHECK_EQ(ctx.eval("class Body { public: Vec2 pos; int id; Body() : pos(2, 0), id(1) {} };\nint main() { Body b; return (int)b.pos.X + b.id; }").toInt(), 3);
    CHECK_EQ(ctx.eval("#include <vector>\nint main() { std::vector<Vec2> vs; vs.push_back(Vec2(3, 4)); vs.push_back(Vec2(0, 1)); double t = 0; for (const Vec2& v : vs) t += v.Length(); return (int)t; }").toInt(), 6);
}

TEST(sharedInstanceAcrossTheBoundary)
{
    cpp::Context ctx;
    Counter shared;
    cpp::Class<Counter>(ctx, "Counter")
        .constructor<>()
        .method("Bump", &Counter::bump)
        .method("Value", &Counter::value)
        .install();
    ctx.hostClass("Host").method("Shared", [&shared] { return &shared; });
    CHECK_EQ(ctx.eval("int main() { Counter* c = Host::Shared(); for (int i = 0; i < 3; i++) c->Bump(); return c->Value(); }").toInt(), 3);
    CHECK_EQ(shared.n, 3);
    CHECK_EQ(ctx.eval("void bumpAll(Counter& c, int n) { while (n--) c.Bump(); }\nint main() { bumpAll(Host::Shared(), 2); return Host::Shared()->Value(); }").toInt(), 5);
}

TEST(errorsCrossBothWays)
{
    cpp::Context ctx;
    ctx.hostClass("Env").method("Fail", []() -> int {
        throw script::Error(cpp::ErrorCode::ArgumentException, "bad argument");
    });
    CHECK_THROWS(ctx.eval("int main() { Env::Fail(); return 0; }"), cpp::ErrorCode::ArgumentException);
    // A host failure is an exception the script may catch.
    CHECK_EQ(ctx.eval("#include <stdexcept>\nint main() { try { Env::Fail(); } catch (const std::invalid_argument& e) { return e.what().size(); } return 0; }").toInt(), 12);
    ctx.hostClass("Odd").method("Take", [](std::map<std::string, double>) { return 1; });
    try {
        ctx.compile("int main() { return Odd::Take(0); }");
        CHECK(false);
    } catch (const cpp::Error& e) {
        CHECK(e.code() == cpp::ErrorCode::SyntaxError);
        CHECK(std::string(e.what()).find("no member") != std::string::npos);
    }
}

TEST(limitsApply)
{
    cpp::Context::Options options;
    options.stackLimit = 100;
    cpp::Context ctx(options);
    CHECK_THROWS(ctx.eval("int down(int n) { return down(n + 1); }\nint main() { return down(0); }"), cpp::ErrorCode::StackOverflow);
    cpp::Context ctx2;
    int ticks = 0;
    ctx2.setInterruptHandler([&ticks] { return ++ticks > 2; });
    CHECK_THROWS(ctx2.eval("int main() { while (true) { } return 0; }"), cpp::ErrorCode::Interrupted);
}
