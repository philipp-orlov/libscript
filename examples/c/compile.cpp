// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Compile C once, ship the image, start instantly.
//
// C runs on the typed engine, so a C program has everything a C# assembly
// has: it serializes to a portable image another process loads without
// lexing, parsing or checking, and its functions can be called by name.
//
//   ./c_compile

#include <cmath>
#include <cstdio>
#include <fstream>
#include <vector>

#include "script/script.hpp"

namespace {

const char* kSource = R"(
    #include <math.h>

    #define LIMIT 80.0
    #define WINDOW 4

    static double history[WINDOW];
    static int filled = 0;

    /* A running mean over the last WINDOW readings. */
    double smooth(double reading) {
        history[filled % WINDOW] = reading;
        filled++;
        int n = filled < WINDOW ? filled : WINDOW;
        double sum = 0;
        for (int i = 0; i < n; i++) sum += history[i];
        return sum / n;
    }

    int verdict(double celsius) {
        if (celsius > LIMIT) return 2;          /* shutdown */
        if (celsius > LIMIT - 15) return 1;     /* throttle */
        return 0;                               /* normal */
    }

    int main(void) { return verdict(smooth(20.0)); }
)";

std::vector<std::uint8_t> buildStep()
{
    // The build machine: parse, check, lay out, emit, serialize.
    script::Context ctx;
    script::Assembly assembly = ctx.compile(kSource, "rules.c");
    std::printf("compiled %zu functions, image %zu bytes\n", assembly.methods().size(),
                assembly.serialize().size());
    return assembly.serialize();
}

}  // namespace

int main()
{
    const std::vector<std::uint8_t> image = buildStep();
    {
        std::ofstream out("rules.sxa", std::ios::binary);
        out.write(reinterpret_cast<const char*>(image.data()), std::streamsize(image.size()));
    }

    // The device: no parser, no checker, no source.
    script::Context::Options options;
    options.jit.mode = script::JitOptions::Mode::Tiered;
    options.jit.threshold = 50;
    script::Context ctx(options);

    script::Assembly assembly = ctx.load(image.data(), image.size());
    std::printf("loaded %s, backend %s\n", assembly.entryPoint().c_str(), ctx.jitBackend());

    // Every C function is a static method of the synthesized class `C$`,
    // callable by name; the file-scope statics keep their values between calls.
    // A day of readings: a slow ramp up past the limit and back down.
    const char* names[] = {"normal", "throttle", "shutdown"};
    int tally[3] = {0, 0, 0};
    int previous = 0;
    for (int minute = 0; minute < 240; ++minute) {
        const double celsius = 20 + 80 * std::sin(minute / 240.0 * 3.14159);
        const script::Value mean = ctx.call(assembly, "C$.smooth", {script::Value(celsius)});
        const int verdict = ctx.call(assembly, "C$.verdict", {mean}).toInt();
        ++tally[verdict];
        if (verdict != previous)
            std::printf("minute %3d: %5.1f C (mean %5.1f) -> %s\n", minute, celsius, mean.toDouble(), names[verdict]);
        previous = verdict;
    }
    std::printf("%d normal, %d throttle, %d shutdown\n", tally[0], tally[1], tally[2]);

    const script::Context::Stats stats = ctx.stats();
    std::printf("methods compiled to native: %llu\n",
                static_cast<unsigned long long>(stats.methodsCompiled));
    std::remove("rules.sxa");
    return 0;
}
