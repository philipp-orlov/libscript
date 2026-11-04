// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Exposing C++ types to scripted C++: a wrapped class with fields, methods
// and a static, a long-lived host service the script borrows, vectors
// crossing in both directions, and errors crossing both ways.
//
//   ./cpp_bindings

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "script/script.hpp"

namespace {

script::Context::Options languageOptions()
{
    script::Context::Options options;
    options.language = script::Language::Cpp;
    return options;
}

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
    static Vec3 zero() { return Vec3{}; }
};

/// A host-owned service the script only ever borrows.
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

    // A value type the script constructs and owns.
    script::Class<Vec3>(ctx, "Vec3")
        .constructor<double, double, double>()
        .field("x", &Vec3::x)
        .field("y", &Vec3::y)
        .field("z", &Vec3::z)
        .method("length", &Vec3::length)
        .method("add", &Vec3::add)
        .staticMethod("zero", &Vec3::zero)
        .install();

    // A service the host owns; the script gets a non-owning reference.
    script::Class<Telemetry>(ctx, "Telemetry")
        .method("record", &Telemetry::record)
        .method("count", &Telemetry::count)
        .method("values", &Telemetry::values)
        .install();
    Telemetry telemetry;
    ctx.hostClass("Host")
        .method("telemetry", [&telemetry] { return &telemetry; })
        .method("sum", [](std::vector<double> v) {
            double s = 0;
            for (double d : v)
                s += d;
            return s;
        });

    ctx.eval(R"(
        #include <iostream>
        #include <vector>

        Vec3 unit(const Vec3& v) {
            Vec3 u(v.x / v.length(), v.y / v.length(), v.z / v.length());
            return u;
        }

        int main() {
            Vec3 a(1, 2, 2);
            Vec3* b = new Vec3(0, 0, 1);
            a.add(b);
            Telemetry* t = Host::telemetry();       // the same object the host holds
            t->record("length", a.length());
            t->record("x", a.x);
            std::cout << "|a| = " << a.length() << ", unit x = " << unit(a).x
                      << ", zero = " << Vec3::zero().length() << std::endl;

            std::vector<double> mine = {1.5, 2.5};   // a script vector into a host std::vector
            std::cout << "host sum " << Host::sum(mine) << std::endl;
            double back = 0;
            for (double v : t->values())             // a host std::vector back as an array
                back += v;
            std::cout << t->count() << " samples totalling " << back << std::endl;
            return 0;
        }
    )", "vectors.cpp");
    std::printf("host sees %d samples\n", telemetry.count());

    // A host failure is an exception the script can catch, or let through.
    ctx.hostClass("Flaky").method("read", []() -> double {
        throw script::Error(script::ErrorCode::InvalidOperation, "sensor offline");
    });
    ctx.eval(R"(
        #include <iostream>
        #include <stdexcept>
        int main() {
            try {
                std::cout << Flaky::read() << std::endl;
            } catch (const std::exception& e) {
                std::cout << "script caught: " << e.what() << std::endl;
            }
            return 0;
        }
    )", "flaky.cpp");
    try {
        ctx.eval("int main() { Flaky::read(); return 0; }", "unhandled.cpp");
    } catch (const script::Exception& e) {
        std::printf("host caught: %s\n", e.what());
    }

    // A member the script cannot spell is missing, and says so.
    try {
        ctx.compile("int main() { Vec3 v(1, 2, 3); return v.lenght(); }", "typo.cpp");
    } catch (const script::Error& e) {
        std::printf("rejected: %s\n", e.what());
    }
    return 0;
}
