// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// A rule set where every rule is written in the language its author likes.
//
// Each rule is compiled once into an `Assembly`, then run against thousands
// of events without parsing or checking anything again. The host presents the
// event through one bound class; each language reads it its own way. This is
// the shape of a long-running daemon: compile at start-up, run for ever, and
// watch the heap stay where it is.
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include "script/script.hpp"

namespace {

/// One reading from one sensor in one zone of a building.
struct Reading
{
    std::string sensor;
    double value = 0;
    int zone = 0;
};

// The reading under evaluation. A host class reads it, so rules never hold a
// reference to it and nothing is allocated per event on the script side of
// the boundary.
Reading current;

struct Rule
{
    std::string name;
    script::Assembly code;
};

}  // namespace

int main()
{
    script::Context ctx;
    ctx.hostClass("Ev")
        .method("Sensor", [] { return current.sensor; })
        .method("Value", [] { return current.value; })
        .method("Zone", [] { return current.zone; });

    std::vector<Rule> rules;
    rules.push_back({"overheating (C#)", ctx.compile("return Ev.Value() > 80.0;", "hot.cs")});
    rules.push_back({"cold basement (TypeScript)",
                     ctx.compile("const zone: number = Ev.Zone(); zone === 0 && Ev.Value() < 45", "cold.ts")});
    rules.push_back({"watch list (JavaScript)",
                     ctx.compile("['pump-3', 'fan-1'].includes(Ev.Sensor())", "watch.js")});
    rules.push_back({"top floor warm (Python)", ctx.compile("Ev.Zone() == 2 and Ev.Value() > 70", "warm.py")});
    rules.push_back({"mid range (C)",
                     ctx.compile("int main() { double v = Ev.Value(); return v >= 60 && v <= 100; }", "mid.c")});
    rules.push_back({"fan check (C++)",
                     ctx.compile("#include <string>\nint main() { std::string s = Ev::Sensor(); "
                                 "return s.starts_with(\"fan\") && Ev::Value() < 50; }",
                                 "fan.cpp")});

    const char* sensors[] = {"pump-3", "fan-1", "valve-2", "fan-2"};
    std::vector<int> hits(rules.size(), 0);
    const int events = 20000;
    const script::Context::Stats before = ctx.stats();
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < events; ++i) {
        current.sensor = sensors[i % 4];
        current.value = 40 + (i * 7) % 120;
        current.zone = i % 3;
        for (std::size_t r = 0; r < rules.size(); ++r) {
            if (ctx.run(rules[r].code).toBool())
                ++hits[r];
        }
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    const script::Context::Stats after = ctx.stats();

    for (std::size_t r = 0; r < rules.size(); ++r)
        std::printf("%-28s fired %d times\n", rules[r].name.c_str(), hits[r]);
    std::printf("%d events x %zu rules in %.1f ms\n", events, rules.size(),
                std::chrono::duration<double, std::milli>(elapsed).count());
    std::printf("heap before %llu bytes / %llu objects, after %llu bytes / %llu objects, "
                "%llu collections\n",
                (unsigned long long)before.heapBytes, (unsigned long long)before.liveObjects,
                (unsigned long long)after.heapBytes, (unsigned long long)after.liveObjects,
                (unsigned long long)(after.gcRuns - before.gcRuns));
    return 0;
}
