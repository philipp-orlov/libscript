// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Native code: asking for it, seeing what came of it, and proving it is the
// same program.
//
// The engine interprets by default. `CompileOptions::native` compiles every
// method the back end handles as soon as the assembly exists;
// `JitOptions::Mode::Tiered` waits until a method is hot. Either way the
// answer has to be identical to the interpreter's -- which is what this
// example checks before it reports a speedup.
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include "script/script.hpp"

namespace {

const char* kKernels = R"CS(
    static class Kernels {
        // A tight integer loop: the case a code generator exists for.
        public static long Checksum(int n) {
            long sum = 0;
            for (int i = 1; i <= n; i++) {
                sum += i * (i % 7) - (i >> 3);
                if ((i & 1023) == 0) sum ^= i;
            }
            return sum;
        }

        // Floating point, branches, and a loop that exits early.
        public static int Escape(double cx, double cy, int limit) {
            double x = 0, y = 0;
            int i = 0;
            while (i < limit && x * x + y * y <= 4.0) {
                double t = x * x - y * y + cx;
                y = 2.0 * x * y + cy;
                x = t;
                i++;
            }
            return i;
        }

        // A nested loop over 64-bit arithmetic. Some chains climb past what
        // an `int` holds, which is why `n` is a `long`.
        public static int LongestCollatz(int limit) {
            int best = 0;
            for (int i = 1; i < limit; i++) {
                long n = i;
                int steps = 0;
                while (n != 1) {
                    n = (n % 2 == 0) ? n / 2 : 3 * n + 1;
                    steps++;
                }
                if (steps > best) best = steps;
            }
            return best;
        }

        // Outside the subset: it touches strings, so it keeps interpreting.
        public static string Describe(int n) => "n=" + n;
    }
)CS";

double seconds(script::Context& ctx, script::Assembly& assembly, const char* method,
               const std::vector<script::Value>& args, script::Value& result)
{
    const auto start = std::chrono::steady_clock::now();
    result = ctx.call(assembly, method, args);
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

}  // namespace

int main()
{
    // One context interprets; the other is asked for native code up front.
    script::Context interpreter;
    script::Context::Options nativeOptions;
    nativeOptions.compile.native = true;
    script::Context native(nativeOptions);

    std::printf("back end for this host: %s\n", native.jitBackend());

    script::Assembly interpreted = interpreter.compile(kKernels, "kernels.cs");
    script::Assembly compiled = native.compile(kKernels, "kernels.cs");

    const script::Context::Stats stats = native.stats();
    std::printf("compiled %llu of %zu methods, %llu bytes of code cache\n\n",
                (unsigned long long)stats.methodsCompiled, compiled.methods().size(),
                (unsigned long long)stats.nativeCodeBytes);

    struct Case
    {
        const char* method;
        std::vector<script::Value> args;
    };
    const Case cases[] = {
        {"Kernels.Checksum", {script::Value(20000000)}},
        {"Kernels.Escape", {script::Value(-0.743643887037151), script::Value(0.13182590420533), script::Value(4000000)}},
        {"Kernels.LongestCollatz", {script::Value(60000)}},
        {"Kernels.Describe", {script::Value(42)}},
    };

    // Milliseconds for the long kernels, microseconds for the short one, so
    // that neither reads as "0.0".
    auto duration = [](double sec, char* out, std::size_t n) -> const char* {
        if (sec >= 0.001)
            std::snprintf(out, n, "%.1f ms", sec * 1000);
        else
            std::snprintf(out, n, "%.1f us", sec * 1000000);
        return out;
    };

    std::printf("%-22s %12s %12s %8s  result\n", "method", "interpreted", "native", "speedup");
    std::printf("----------------------------------------------------------------------\n");
    for (const Case& test : cases) {
        script::Value a;
        script::Value b;
        const double slow = seconds(interpreter, interpreted, test.method, test.args, a);
        const double fast = seconds(native, compiled, test.method, test.args, b);
        const bool same = a.toString() == b.toString();
        char slowText[32];
        char fastText[32];
        std::printf("%-22s %12s %12s %7.1fx  %s%s\n", test.method,
                    duration(slow, slowText, sizeof slowText),
                    duration(fast, fastText, sizeof fastText), slow / fast, a.toString().c_str(),
                    same ? "" : "  <-- MISMATCH");
    }

    // Tiering: nothing is compiled until a method has earned it.
    std::printf("\ntiered compilation:\n");
    script::Context::Options tiered;
    tiered.jit.mode = script::JitOptions::Mode::Tiered;
    tiered.jit.threshold = 100;
    script::Context warm(tiered);
    script::Assembly hot = warm.compile(kKernels, "kernels.cs");
    std::printf("  after compiling:      %llu methods native\n",
                (unsigned long long)warm.stats().methodsCompiled);
    for (int i = 0; i < 50; ++i)
        warm.call(hot, "Kernels.LongestCollatz", {script::Value(200)});
    std::printf("  after 50 calls:       %llu methods native\n",
                (unsigned long long)warm.stats().methodsCompiled);
    for (int i = 0; i < 200; ++i)
        warm.call(hot, "Kernels.LongestCollatz", {script::Value(200)});
    std::printf("  after 250 calls:      %llu methods native\n",
                (unsigned long long)warm.stats().methodsCompiled);
    std::printf("  and it still answers: %s\n",
                warm.call(hot, "Kernels.LongestCollatz", {script::Value(200)}).toString().c_str());

    // Generated code is still C# code: it raises the exceptions the language
    // says it should, and it can still be interrupted.
    std::printf("\nnative code keeps its promises:\n");
    script::Assembly guards = native.compile(
        "static class G { public static int Div(int a, int b) => a / b; "
        "public static int Spin() { int n = 0; while (true) n++; return n; } }",
        "guards.cs");
    try {
        native.call(guards, "G.Div", {script::Value(1), script::Value(0)});
    } catch (const script::Exception& error) {
        std::printf("  division by zero -> %s\n", error.what());
    }
    int ticks = 0;
    native.setInterruptHandler([&ticks] { return ++ticks > 100; });
    try {
        native.call(guards, "G.Spin");
    } catch (const script::Error& error) {
        std::printf("  runaway loop     -> %s\n", error.what());
    }
    return 0;
}
