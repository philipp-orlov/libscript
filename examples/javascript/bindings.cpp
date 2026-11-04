// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Exposing C++ types to script: classes, instances, ownership, callbacks.
//
//   ./bindings

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "script/script.hpp"

namespace {

/// Every snippet in this example is JavaScript, so the context says so once
/// instead of every `eval` naming a file.
script::Context::Options languageOptions()
{
    script::Context::Options options;
    options.language = script::Language::JavaScript;
    return options;
}

}  // namespace

namespace {

struct Vec3
{
    double x = 0, y = 0, z = 0;

    double length() const { return std::sqrt(x * x + y * y + z * z); }
    void add(const Vec3* other)
    {
        x += other->x;
        y += other->y;
        z += other->z;
    }
    std::string toText() const
    {
        char buf[64];
        std::snprintf(buf, sizeof buf, "(%g, %g, %g)", x, y, z);
        return buf;
    }
    static Vec3 zero() { return Vec3{}; }
};

/// A long-lived host object the script only ever borrows.
class Telemetry
{
public:
    void record(const std::string& name, double value) { samples_.push_back({name, value}); }
    std::size_t count() const { return samples_.size(); }
    double total() const
    {
        double sum = 0;
        for (const auto& s : samples_)
            sum += s.second;
        return sum;
    }

private:
    std::vector<std::pair<std::string, double>> samples_;
};

}  // namespace

int main()
{
    script::Context ctx(languageOptions());

    // A value type: JavaScript owns each instance it constructs.
    script::Class<Vec3>(ctx, "Vec3")
        .constructor<double, double, double>()
        .field("x", &Vec3::x)
        .field("y", &Vec3::y)
        .field("z", &Vec3::z)
        .method("length", &Vec3::length)
        .method("add", &Vec3::add)
        .method("toString", &Vec3::toText)
        .staticMethod("zero", &Vec3::zero)
        .install();

    // A host-owned service: the script gets a non-owning reference.
    script::Class<Telemetry>(ctx, "Telemetry")
        .method("record", &Telemetry::record)
        .getter("count", &Telemetry::count)
        .getter("total", &Telemetry::total)
        .install();

    Telemetry telemetry;
    ctx.global().set("telemetry", ctx.value(&telemetry));

    ctx.eval(R"(
        const a = new Vec3(1, 2, 2);
        const b = new Vec3(0, 0, 1);
        a.add(b);
        telemetry.record('length', a.length());
        telemetry.record('x', a.x);
        console.log(`a = ${a}, |a| = ${a.length()}`);
        console.log(`zero = ${Vec3.zero()}`);
        console.log(`samples: ${telemetry.count}, total: ${telemetry.total}`);
    )");

    std::printf("host sees %zu samples totalling %g\n", telemetry.count(), telemetry.total());

    // Shared ownership: the wrapper holds a shared_ptr, so the instance
    // outlives the C++ scope that created it.
    {
        auto shared = std::make_shared<Telemetry>();
        shared->record("boot", 1);
        ctx.global().set("shared", ctx.value(shared));
    }
    ctx.eval("shared.record('after-scope', 2); console.log('shared total:', shared.total)");

    // Script functions as ordinary C++ callables.
    auto pick = ctx.as<std::function<std::string(std::vector<std::string>)>>(
        ctx.eval("items => items.sort().at(-1)"));
    std::printf("last item: %s\n", pick({"pear", "apple", "plum"}).c_str());
    return 0;
}
