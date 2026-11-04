// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The five-minute tour, in C: run a program, read its result, hand it a host
// class, compile once and run many times.
//
//   ./c_quickstart

#include <cstdio>
#include <string>

#include "script/script.hpp"

namespace {

struct Sensor
{
    double reading = 20.5;
    double read() const { return reading; }
    void calibrate(double offset) { reading += offset; }
};

script::Context::Options languageOptions()
{
    script::Context::Options options;
    options.language = script::Language::C;
    return options;
}

}  // namespace

int main()
{
    script::Context ctx(languageOptions());
    ctx.setPrintHandler([](std::string_view text, bool) { std::fwrite(text.data(), 1, text.size(), stdout); });

    // 1. Run a program; main's return value comes back.
    std::printf("main returned %d\n", ctx.eval("int main() { return 6 * 7; }").toInt());

    // 2. printf goes wherever the host says. Structs, arrays and pointer
    //    parameters work the way a C programmer expects.
    ctx.eval(R"(
        #include <stdio.h>
        #include <math.h>
        typedef struct { double x, y; } Point;
        double dist(Point* a, Point* b) { return sqrt(pow(a->x - b->x, 2) + pow(a->y - b->y, 2)); }
        void swap(int* a, int* b) { int t = *a; *a = *b; *b = t; }
        int main() {
            Point p = {0, 0}, q = {3, 4};
            int lo = 9, hi = 1;
            swap(&lo, &hi);
            printf("distance %.1f, sorted %d %d\n", dist(&p, &q), lo, hi);
            return 0;
        }
    )", "geometry.c");

    // 3. A C++ class, callable from C as `Sensor s = Sensor(); s.Read()`.
    script::Class<Sensor>(ctx, "Sensor")
        .constructor<>()
        .method("Read", &Sensor::read)
        .method("Calibrate", &Sensor::calibrate)
        .install();
    ctx.eval(R"(
        #include <stdio.h>
        int main() {
            Sensor s = Sensor();
            s.Calibrate(0.5);
            printf("sensor reads %.1f\n", s.Read());
            return 0;
        }
    )", "sensor.c");

    // 4. Compile once, run many times: globals persist between runs.
    script::Assembly counter = ctx.compile("int runs = 0;\nint main() { return ++runs; }", "counter.c");
    for (int i = 0; i < 3; ++i)
        std::printf("run %d\n", ctx.run(counter).toInt());

    // 5. Errors carry the file and line.
    try {
        ctx.eval("int main() { int a[2]; return a[5]; }", "bounds.c");
    } catch (const script::Exception& e) {
        std::printf("caught %s\n%s", e.what(), e.stack().c_str());
    }
    return 0;
}
