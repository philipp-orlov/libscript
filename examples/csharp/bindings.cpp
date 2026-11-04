// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Exposing the host: static classes, C++ objects, and calls in both directions.
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "script/script.hpp"

namespace {

/// An ordinary C++ class, with no idea that a script exists.
class Vector
{
public:
    Vector(double x, double y) : x_(x), y_(y) {}

    double length() const { return std::sqrt(x_ * x_ + y_ * y_); }
    double x() const { return x_; }
    double y() const { return y_; }
    void scale(double by)
    {
        x_ *= by;
        y_ *= by;
    }
    std::string text() const
    {
        return "(" + std::to_string(x_) + ", " + std::to_string(y_) + ")";
    }

private:
    double x_;
    double y_;
};

}  // namespace

int main()
{
    script::Context ctx;

    // A static class of host functions. The C# signature of each member comes
    // from its C++ signature, so the compiler checks the calls.
    int logLines = 0;
    ctx.hostClass("Host")
        .method("Version", [] { return std::string("1.0"); })
        .method("Log",
                [&logLines](std::string message) {
                    ++logLines;
                    std::printf("[host] %s\n", message.c_str());
                })
        .method("Hypot", [](double a, double b) { return std::sqrt(a * a + b * b); });

    // A C++ class the script can construct and use.
    script::Class<Vector>(ctx, "Vector")
        .constructor<double, double>()
        .method("Length", &Vector::length)
        .method("Scale", &Vector::scale)
        .method("X", &Vector::x)
        .method("Y", &Vector::y)
        .method("Text", &Vector::text);

    ctx.eval(R"(
        Host.Log("running on libscript " + Host.Version());

        var v = new Vector(3, 4);
        Host.Log($"length = {v.Length()}");
        v.Scale(2);
        Host.Log($"scaled  = {v.Text()}, length = {v.Length()}");
        Host.Log($"hypot   = {Host.Hypot(5, 12)}");
    )",
             "vectors.cs");
    std::printf("the host logged %d lines\n", logLines);

    // The other direction: C++ holding a delegate the script made.
    script::Assembly assembly = ctx.compile(R"(
        static class Filters {
            public static System.Func<int, bool> Even() => n => n % 2 == 0;
        }
    )",
                                        "filters.cs");
    const script::Value isEven = ctx.call(assembly, "Filters.Even");
    for (int n = 1; n <= 4; ++n)
        std::printf("%d even? %s\n", n, isEven(n).toBool() ? "yes" : "no");

    // And objects: construct one the script declared, then poke at it.
    script::Assembly types = ctx.compile(R"(
        public class Settings {
            public string Name = "default";
            public int Retries = 3;
            public string Describe() => $"{Name} x{Retries}";
        }
    )",
                                     "settings.cs");
    script::Value settings = ctx.newObject(types, "Settings");
    settings.set("Name", "production");
    settings.set("Retries", 5);
    std::printf("settings: %s\n", settings.invoke("Describe").toString().c_str());
    return 0;
}
