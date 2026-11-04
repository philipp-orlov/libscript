// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Compile once, ship the image, start instantly.
//
// This is the shape of a deployment where the C# never reaches the device:
// a build step compiles to an image, and the device loads bytes.
#include <cstdio>
#include <fstream>
#include <vector>

#include "script/script.hpp"

namespace {

const char* kSource = R"(
    using System.Collections.Generic;

    public class Reading {
        public string Sensor { get; set; }
        public double Value { get; set; }
        public Reading(string sensor, double value) {
            Sensor = sensor;
            Value = value;
        }
    }

    public static class Rules {
        public const double Limit = 80.0;

        public static string Evaluate(double celsius) {
            if (celsius > Limit) return "shutdown";
            if (celsius > Limit - 15) return "throttle";
            return "normal";
        }

        public static int CountAlerts(List<Reading> readings) {
            int alerts = 0;
            foreach (Reading r in readings) {
                if (Evaluate(r.Value) != "normal") alerts++;
            }
            return alerts;
        }
    }
)";

std::vector<std::uint8_t> buildStep()
{
    // The build machine: parse, check, lay out, emit, serialize.
    script::Context ctx;
    script::Assembly assembly = ctx.compile(kSource, "rules.cs");
    std::printf("compiled %zu methods, image %zu bytes\n", assembly.methods().size(),
                assembly.serialize().size());
    return assembly.serialize();
}

}  // namespace

int main()
{
    const std::vector<std::uint8_t> image = buildStep();
    {
        std::ofstream out("rules.csa", std::ios::binary);
        out.write(reinterpret_cast<const char*>(image.data()), std::streamsize(image.size()));
    }

    // The device: no parser, no checker, no source. Ask for native
    // compilation as well -- phase 1 keeps interpreting, and the same request
    // turns into machine code the day a back end lands.
    script::Context::Options options;
    options.jit.mode = script::JitOptions::Mode::Tiered;
    options.jit.threshold = 50;
    script::Context ctx(options);

    script::Assembly assembly = ctx.load(image.data(), image.size());
    std::printf("loaded %s, backend %s\n", assembly.entryPoint().empty()
                                               ? "a library"
                                               : assembly.entryPoint().c_str(),
                ctx.jitBackend());

    for (double celsius : {20.0, 70.0, 95.0}) {
        const script::Value verdict = ctx.call(assembly, "Rules.Evaluate", {script::Value(celsius)});
        std::printf("%5.1f C -> %s\n", celsius, verdict.toString().c_str());
    }

    // Hand it a list built in C# and let it walk it.
    script::Assembly helper = ctx.compile(R"(
        using System.Collections.Generic;
        static class Feed {
            public static List<Reading> Sample() {
                var readings = new List<Reading>();
                readings.Add(new Reading("intake", 22.5));
                readings.Add(new Reading("core", 91.0));
                readings.Add(new Reading("exhaust", 68.0));
                return readings;
            }
        }
    )",
                                      "feed.cs");
    const script::Value readings = ctx.call(helper, "Feed.Sample");
    const script::Value alerts = ctx.call(assembly, "Rules.CountAlerts", {readings});
    std::printf("%d of %d readings need attention\n", alerts.toInt(), readings.length());

    const script::Context::Stats stats = ctx.stats();
    std::printf("methods compiled to native: %llu\n",
                static_cast<unsigned long long>(stats.methodsCompiled));
    std::remove("rules.csa");
    return 0;
}
