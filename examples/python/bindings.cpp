// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Exposing C++ to Python: a wrapped class Python subclasses, a host service
// Python borrows, lists and dicts crossing both ways, and errors crossing
// both ways.
//
//   ./python_bindings

#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "script/script.hpp"

namespace {

script::Context::Options languageOptions()
{
    script::Context::Options options;
    options.language = script::Language::Python;
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
};

/// A host-owned service the script only borrows.
class Telemetry
{
public:
    void record(const std::string& name, double value) { samples_.push_back({name, value}); }
    int count() const { return int(samples_.size()); }
    std::vector<double> values() const
    {
        std::vector<double> out;
        for (const auto& s : samples_)
            out.push_back(s.second);
        return out;
    }

private:
    std::vector<std::pair<std::string, double>> samples_;
};

}  // namespace

int main()
{
    script::Context ctx(languageOptions());
    ctx.setPrintHandler([](std::string_view text, bool) { std::fwrite(text.data(), 1, text.size(), stdout); });

    // A C++ class is a Python class: constructed, subclassed, printed.
    script::Class<Vec2>(ctx, "Vec2")
        .constructor<double, double>()
        .field("x", &Vec2::x)
        .field("y", &Vec2::y)
        .method("length", &Vec2::length)
        .method("scale", &Vec2::scale)
        .install();

    // A service the host owns; Python gets a reference.
    script::Class<Telemetry>(ctx, "Telemetry")
        .method("record", &Telemetry::record)
        .method("count", &Telemetry::count)
        .method("values", &Telemetry::values)
        .install();
    Telemetry telemetry;
    ctx.global().set("telemetry", ctx.value(&telemetry));

    // Plain functions, and a list and a dict made in C++.
    ctx.global().set("clamp", ctx.fn("clamp", [](double v, double lo, double hi) {
        return v < lo ? lo : v > hi ? hi : v;
    }));
    ctx.global().set("readings", ctx.value(std::vector<double>{19.5, 21.0, 22.5}));
    ctx.global().set("limits", ctx.value(std::map<std::string, double>{{"lo", 20.0}, {"hi", 22.0}}));

    ctx.eval(R"(
class Named(Vec2):
    def __init__(self, name, x, y):
        super().__init__(x, y)
        self.name = name
    def __str__(self):
        return f'{self.name}=({self.x:g}, {self.y:g})'

v = Named('velocity', 3, 4)
v.scale(2)
print(v, 'length', v.length())

clamped = [clamp(r, limits['lo'], limits['hi']) for r in readings]
print('clamped', clamped)
for i, r in enumerate(clamped):
    telemetry.record(f'r{i}', r)
print('telemetry has', telemetry.count(), 'samples totalling', sum(telemetry.values()))
)", "bindings.py");
    std::printf("host sees %d samples\n", telemetry.count());

    // Python values come back as Values: a tuple is an array, a number a number.
    const script::Value stats = ctx.eval("(sum(clamped) / len(clamped), len(clamped))");
    std::printf("mean %.2f over %d readings\n", stats.get(0u).toDouble(), stats.get(1u).toInt());
    const script::Value fn = ctx.eval("def describe(v):\n    return f'{v.name} of length {v.length():.1f}'\ndescribe");
    std::printf("%s\n", fn.call({ctx.global().get("v")}).toString().c_str());

    // Errors cross both ways.
    ctx.global().set("failing", ctx.function("failing", [](script::CallInfo& ci) -> script::Value {
        ci.fail(script::ErrorCode::InvalidOperation, "sensor offline");
    }));
    ctx.eval("try:\n    failing()\nexcept Exception as e:\n    print('python caught:', e)\n", "caught.py");
    try {
        ctx.eval("raise ValueError('no reading')", "raise.py");
    } catch (const script::Exception& e) {
        std::printf("host caught: %s\n", e.what());
    }
    return 0;
}
