// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Exposing C++ to scripted C: host functions on a static class, a wrapped
// C++ class used as a struct with methods, arrays crossing in both
// directions, and errors crossing the boundary.
//
//   ./c_bindings

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "script/script.hpp"

namespace {

script::Context::Options languageOptions()
{
    script::Context::Options options;
    options.language = script::Language::C;
    return options;
}

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

/// A host-owned accumulator the script only borrows.
struct Log
{
    std::vector<std::string> lines;
    void add(const std::string& line) { lines.push_back(line); }
    int count() const { return int(lines.size()); }
};

}  // namespace

int main()
{
    script::Context ctx(languageOptions());
    ctx.setPrintHandler([](std::string_view text, bool) { std::fwrite(text.data(), 1, text.size(), stdout); });

    // Free functions, from C's point of view: static members of a host class.
    ctx.hostClass("Env")
        .method("Greet", [](std::string who) { return "hello " + who; })
        .method("Clamp", [](double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; })
        .method("Sum", [](std::vector<int> values) {
            int total = 0;
            for (int v : values)
                total += v;
            return total;
        })
        .method("Squares", [](int n) {
            std::vector<int> out;
            for (int i = 1; i <= n; ++i)
                out.push_back(i * i);
            return out;
        })
        .constant("Limit", 100);

    // A C++ class: `Vec2 v = Vec2(3, 4); v.Scale(2); v.Length()`.
    script::Class<Vec2>(ctx, "Vec2")
        .constructor<double, double>()
        .field("X", &Vec2::x)
        .field("Y", &Vec2::y)
        .method("Length", &Vec2::length)
        .method("Scale", &Vec2::scale)
        .staticMethod("Unit", &Vec2::unit)
        .install();

    // A host-owned object handed in by reference.
    Log log;
    script::Class<Log>(ctx, "Log").method("Add", &Log::add).method("Count", &Log::count).install();
    ctx.hostClass("Host").method("Log", [&log] { return &log; });

    ctx.eval(R"(
        #include <stdio.h>

        double project(Vec2* v, Vec2* onto) { return (v->X * onto->X + v->Y * onto->Y) / onto->Length(); }

        int main() {
            printf("%s, limit %d\n", Env.Greet("C"), Env.Limit);
            printf("clamped %.1f\n", Env.Clamp(140.5, 0, Env.Limit));

            int mine[] = {1, 2, 3, 4};                 /* a C array into a host std::vector */
            int* theirs = Env.Squares(3);              /* a host std::vector back as an array */
            printf("sum %d, squares %d %d %d\n", Env.Sum(mine), theirs[0], theirs[1], theirs[2]);

            Vec2 v = Vec2(3, 4);
            v.Scale(2);
            printf("length %.1f, along x %.1f\n", v.Length(), project(&v, Vec2.Unit()));

            Log log = Host.Log();                      /* the same object the host holds */
            log.Add("from C");
            log.Add("twice");
            return log.Count();
        }
    )", "bindings.c");
    std::printf("host log has %d line(s); first is \"%s\"\n", log.count(), log.lines.front().c_str());

    // Errors cross both ways: a host failure surfaces with the C line, and
    // a member C cannot spell is reported at compile time.
    ctx.hostClass("Flaky").method("Read", []() -> int {
        throw script::Error(script::ErrorCode::InvalidOperation, "sensor offline");
    });
    try {
        ctx.eval("int main() { return Flaky.Read(); }", "flaky.c");
    } catch (const script::Exception& e) {
        std::printf("host error reached C: %s\n%s", e.what(), e.stack().c_str());
    }
    ctx.hostClass("Odd").method("Take", [](std::map<std::string, double>) { return 0; });
    try {
        ctx.compile("int main() { return Odd.Take(0); }", "odd.c");
    } catch (const script::Error& e) {
        std::printf("rejected: %s\n", e.what());
    }
    return 0;
}
