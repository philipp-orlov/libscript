// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The code generator, checked against the interpreter.
//
// Every case here runs the same program twice -- once interpreted, once
// compiled -- and insists on the same answer. That is the point of keeping the
// interpreter: it is the oracle a back end is measured against. On a host with
// no code generator the two contexts are both interpreting and the tests still
// pass, which is what makes this safe to run everywhere.
#include <chrono>
#include <memory>
#include <string>

#include "script/script.hpp"
#include "tests/test_util.hpp"

namespace {

struct Pair
{
    script::Context interpreted;
    script::Context compiled;
    script::Assembly interpretedAssembly;
    script::Assembly compiledAssembly;

    explicit Pair(const char* source)
        : interpreted(plainOptions()), compiled(nativeOptions())
    {
        interpretedAssembly = interpreted.compile(source, "<jit>");
        compiledAssembly = compiled.compile(source, "<jit>");
    }

    static script::Context::Options plainOptions()
    {
        script::Context::Options options;
        options.jit.mode = script::JitOptions::Mode::Off;
        return options;
    }
    static script::Context::Options nativeOptions()
    {
        script::Context::Options options;
        options.compile.native = true;
        return options;
    }

    /// Both answers, as text, so any difference shows rather than being
    /// rounded away.
    void same(const char* method, const std::vector<script::Value>& args)
    {
        const std::string a = interpreted.call(interpretedAssembly, method, args).toString();
        const std::string b = compiled.call(compiledAssembly, method, args).toString();
        if (a != b)
            ::tst::fail(__FILE__, __LINE__,
                        std::string("interpreted ") + method + " = " + a + ", compiled = " + b);
    }

    bool usedNativeCode() const { return compiled.stats().methodsCompiled > 0; }
};

const char* kArithmetic = R"CS(
    static class K {
        public static int AddI(int a, int b) => a + b;
        public static int SubI(int a, int b) => a - b;
        public static int MulI(int a, int b) => a * b;
        public static int DivI(int a, int b) => a / b;
        public static int RemI(int a, int b) => a % b;
        public static int NegI(int a) => -a;
        public static int NotI(int a) => ~a;
        public static int Bits(int a, int b) => (a & b) | (a ^ b);
        public static int Shifts(int a, int b) => (a << b) + (a >> b);
        public static uint ShiftsU(uint a, int b) => a >> b;
        public static long AddL(long a, long b) => a + b;
        public static long MulL(long a, long b) => a * b;
        public static long DivL(long a, long b) => a / b;
        public static long RemL(long a, long b) => a % b;
        public static ulong DivUL(ulong a, ulong b) => a / b;
        public static double AddD(double a, double b) => a + b;
        public static double DivD(double a, double b) => a / b;
        public static double NegD(double a) => -a;
        public static float MulF(float a, float b) => a * b;
        public static float NegF(float a) => -a;
    }
)CS";

const char* kConversions = R"CS(
    static class K {
        public static long ToLong(int a) => (long)a;
        public static int ToInt(long a) => (int)a;
        public static double ToDouble(int a) => (double)a;
        public static double LongToDouble(long a) => (double)a;
        public static double UlongToDouble(ulong a) => (double)a;
        public static int DoubleToInt(double a) => (int)a;
        public static long DoubleToLong(double a) => (long)a;
        public static float ToFloat(double a) => (float)a;
        public static double FloatToDouble(float a) => (double)a;
        public static byte ToByte(int a) => (byte)a;
        public static sbyte ToSByte(int a) => (sbyte)a;
        public static short ToShort(int a) => (short)a;
        public static ushort ToUShort(int a) => (ushort)a;
        public static char ToChar(int a) => (char)a;
        public static uint ToUInt(long a) => (uint)a;
        public static long UIntToLong(uint a) => (long)a;
    }
)CS";

const char* kComparisons = R"CS(
    static class K {
        public static bool LtI(int a, int b) => a < b;
        public static bool LeI(int a, int b) => a <= b;
        public static bool GtI(int a, int b) => a > b;
        public static bool GeI(int a, int b) => a >= b;
        public static bool EqI(int a, int b) => a == b;
        public static bool NeI(int a, int b) => a != b;
        public static bool LtU(uint a, uint b) => a < b;
        public static bool GeU(uint a, uint b) => a >= b;
        public static bool LtL(long a, long b) => a < b;
        public static bool EqL(long a, long b) => a == b;
        public static bool LtD(double a, double b) => a < b;
        public static bool LeD(double a, double b) => a <= b;
        public static bool GtD(double a, double b) => a > b;
        public static bool EqD(double a, double b) => a == b;
        public static bool NeD(double a, double b) => a != b;
        public static bool LtF(float a, float b) => a < b;
        public static bool Not(bool a) => !a;
        public static bool Both(int a, int b) => a > 0 && b > 0;
        public static bool Either(int a, int b) => a > 0 || b > 0;
    }
)CS";

const char* kLoops = R"CS(
    static class K {
        public static int Sum(int n) {
            int total = 0;
            for (int i = 0; i < n; i++) total += i;
            return total;
        }
        public static long Fib(int n) {
            long a = 0, b = 1;
            for (int i = 0; i < n; i++) { long t = a + b; a = b; b = t; }
            return a;
        }
        public static int Collatz(int n) {
            int steps = 0;
            while (n != 1) {
                n = (n % 2 == 0) ? n / 2 : 3 * n + 1;
                steps++;
            }
            return steps;
        }
        public static double Mandel(double cx, double cy, int limit) {
            double x = 0, y = 0;
            int i = 0;
            while (i < limit && x * x + y * y <= 4.0) {
                double t = x * x - y * y + cx;
                y = 2.0 * x * y + cy;
                x = t;
                i++;
            }
            return (double)i;
        }
        public static int Nested(int n) {
            int total = 0;
            for (int i = 0; i < n; i++) {
                for (int j = 0; j < n; j++) {
                    if (j % 3 == 0) continue;
                    if (i * j > 400) break;
                    total += i ^ j;
                }
            }
            return total;
        }
        public static int Branchy(int a, int b) {
            if (a > b) return a - b;
            if (a == b) return 0;
            if (b - a > 100) return 100;
            do { a++; } while (a < b);
            return a;
        }
    }
)CS";

}  // namespace

TEST(backendReportsItself)
{
    script::Context ctx;
    const std::string backend = ctx.jitBackend();
    CHECK(backend == "arm64" || backend == "x86-64" || backend == "interpreter");
}

TEST(integerArithmeticMatches)
{
    Pair pair(kArithmetic);
    const int values[] = {0, 1, -1, 7, -7, 1000, -1000, 2147483647, -2147483647 - 1};
    for (int a : values) {
        for (int b : values) {
            pair.same("K.AddI", {script::Value(a), script::Value(b)});
            pair.same("K.SubI", {script::Value(a), script::Value(b)});
            pair.same("K.MulI", {script::Value(a), script::Value(b)});
            pair.same("K.Bits", {script::Value(a), script::Value(b)});
            if (b != 0) {
                pair.same("K.DivI", {script::Value(a), script::Value(b)});
                pair.same("K.RemI", {script::Value(a), script::Value(b)});
            }
        }
        pair.same("K.NegI", {script::Value(a)});
        pair.same("K.NotI", {script::Value(a)});
        for (int shift : {0, 1, 7, 31})
            pair.same("K.Shifts", {script::Value(a), script::Value(shift)});
        for (int shift : {0, 1, 31})
            pair.same("K.ShiftsU", {script::Value(std::uint32_t(a)), script::Value(shift)});
    }
}

TEST(longArithmeticMatches)
{
    Pair pair(kArithmetic);
    const std::int64_t values[] = {0, 1, -1, 3000000000LL, -3000000000LL,
                                   9223372036854775807LL, -9223372036854775807LL - 1};
    for (std::int64_t a : values) {
        for (std::int64_t b : values) {
            pair.same("K.AddL", {script::Value(a), script::Value(b)});
            pair.same("K.MulL", {script::Value(a), script::Value(b)});
            if (b != 0) {
                pair.same("K.DivL", {script::Value(a), script::Value(b)});
                pair.same("K.RemL", {script::Value(a), script::Value(b)});
                pair.same("K.DivUL", {script::Value(std::uint64_t(a)), script::Value(std::uint64_t(b))});
            }
        }
    }
}

TEST(floatingArithmeticMatches)
{
    Pair pair(kArithmetic);
    const double values[] = {0.0, 1.0, -1.5, 3.14159, 1e100, -1e-100, 1.0 / 3.0};
    for (double a : values) {
        for (double b : values) {
            pair.same("K.AddD", {script::Value(a), script::Value(b)});
            pair.same("K.DivD", {script::Value(a), script::Value(b)});
            pair.same("K.MulF", {script::Value(float(a)), script::Value(float(b))});
        }
        pair.same("K.NegD", {script::Value(a)});
        pair.same("K.NegF", {script::Value(float(a))});
    }
}

TEST(conversionsMatch)
{
    Pair pair(kConversions);
    const int ints[] = {0, 1, -1, 127, 128, 255, 256, 32767, 65535, -2147483647 - 1, 2147483647};
    for (int a : ints) {
        pair.same("K.ToLong", {script::Value(a)});
        pair.same("K.ToDouble", {script::Value(a)});
        pair.same("K.ToByte", {script::Value(a)});
        pair.same("K.ToSByte", {script::Value(a)});
        pair.same("K.ToShort", {script::Value(a)});
        pair.same("K.ToUShort", {script::Value(a)});
        pair.same("K.ToChar", {script::Value(a)});
        pair.same("K.UIntToLong", {script::Value(std::uint32_t(a))});
    }
    const std::int64_t longs[] = {0, 1, -1, 4294967296LL, -4294967296LL, 9223372036854775807LL};
    for (std::int64_t a : longs) {
        pair.same("K.ToInt", {script::Value(a)});
        pair.same("K.ToUInt", {script::Value(a)});
        pair.same("K.LongToDouble", {script::Value(a)});
        pair.same("K.UlongToDouble", {script::Value(std::uint64_t(a))});
    }
    const double reals[] = {0.0, 1.9, -1.9, 1e9, -1e9, 3.99999};
    for (double a : reals) {
        pair.same("K.DoubleToInt", {script::Value(a)});
        pair.same("K.DoubleToLong", {script::Value(a)});
        pair.same("K.ToFloat", {script::Value(a)});
        pair.same("K.FloatToDouble", {script::Value(float(a))});
    }
}

TEST(comparisonsMatch)
{
    Pair pair(kComparisons);
    const int ints[] = {-5, -1, 0, 1, 5, 2147483647, -2147483647 - 1};
    for (int a : ints) {
        for (int b : ints) {
            pair.same("K.LtI", {script::Value(a), script::Value(b)});
            pair.same("K.LeI", {script::Value(a), script::Value(b)});
            pair.same("K.GtI", {script::Value(a), script::Value(b)});
            pair.same("K.GeI", {script::Value(a), script::Value(b)});
            pair.same("K.EqI", {script::Value(a), script::Value(b)});
            pair.same("K.NeI", {script::Value(a), script::Value(b)});
            pair.same("K.LtU", {script::Value(std::uint32_t(a)), script::Value(std::uint32_t(b))});
            pair.same("K.GeU", {script::Value(std::uint32_t(a)), script::Value(std::uint32_t(b))});
            pair.same("K.LtL", {script::Value(std::int64_t(a)), script::Value(std::int64_t(b))});
            pair.same("K.EqL", {script::Value(std::int64_t(a)), script::Value(std::int64_t(b))});
            pair.same("K.Both", {script::Value(a), script::Value(b)});
            pair.same("K.Either", {script::Value(a), script::Value(b)});
        }
    }
    const double reals[] = {-1.0, 0.0, 1.0, 1e308};
    for (double a : reals) {
        for (double b : reals) {
            pair.same("K.LtD", {script::Value(a), script::Value(b)});
            pair.same("K.LeD", {script::Value(a), script::Value(b)});
            pair.same("K.GtD", {script::Value(a), script::Value(b)});
            pair.same("K.EqD", {script::Value(a), script::Value(b)});
            pair.same("K.NeD", {script::Value(a), script::Value(b)});
            pair.same("K.LtF", {script::Value(float(a)), script::Value(float(b))});
        }
    }
    pair.same("K.Not", {script::Value(true)});
    pair.same("K.Not", {script::Value(false)});
}

TEST(loopsMatch)
{
    Pair pair(kLoops);
    for (int n : {0, 1, 10, 1000, 100000})
        pair.same("K.Sum", {script::Value(n)});
    for (int n : {0, 1, 10, 90})
        pair.same("K.Fib", {script::Value(n)});
    for (int n : {1, 7, 27, 97})
        pair.same("K.Collatz", {script::Value(n)});
    for (int n : {0, 5, 40})
        pair.same("K.Nested", {script::Value(n)});
    pair.same("K.Mandel", {script::Value(-0.5), script::Value(0.5), script::Value(500)});
    pair.same("K.Mandel", {script::Value(0.3), script::Value(0.6), script::Value(500)});
    for (int a : {-5, 0, 5, 50}) {
        for (int b : {-5, 0, 5, 200})
            pair.same("K.Branchy", {script::Value(a), script::Value(b)});
    }
}

TEST(compiledCodeStillThrowsOnDivisionByZero)
{
    script::Context::Options options;
    options.compile.native = true;
    script::Context ctx(options);
    script::Assembly assembly = ctx.compile(R"CS(
        static class K {
            public static int Div(int a, int b) => a / b;
            public static long DivL(long a, long b) => a / b;
        }
    )CS",
                                        "<jit>");
    CHECK_EQ(ctx.call(assembly, "K.Div", {script::Value(10), script::Value(2)}).toInt(), 5);
    CHECK_THROWS(ctx.call(assembly, "K.Div", {script::Value(1), script::Value(0)}),
                 script::ErrorCode::DivideByZero);
    CHECK_THROWS(
        ctx.call(assembly, "K.DivL", {script::Value(std::int64_t(1)), script::Value(std::int64_t(0))}),
        script::ErrorCode::DivideByZero);
    // And the context is still usable afterwards.
    CHECK_EQ(ctx.call(assembly, "K.Div", {script::Value(9), script::Value(3)}).toInt(), 3);
}

TEST(compiledLoopsStillStop)
{
    script::Context::Options options;
    options.compile.native = true;
    script::Context ctx(options);
    int ticks = 0;
    ctx.setInterruptHandler([&ticks] { return ++ticks > 1000; });
    script::Assembly assembly = ctx.compile(R"CS(
        static class K {
            public static int Spin() {
                int n = 0;
                while (true) n++;
                return n;
            }
        }
    )CS",
                                        "<jit>");
    CHECK_THROWS(ctx.call(assembly, "K.Spin"), script::ErrorCode::Interrupted);
}

TEST(unsupportedMethodsFallBack)
{
    script::Context::Options options;
    options.compile.native = true;
    script::Context ctx(options);
    // Strings, objects and calls are outside the subset; the method has to
    // keep working anyway.
    script::Assembly assembly = ctx.compile(R"CS(
        static class K {
            public static string Join(string a, string b) => a + b;
            public static int Length(string s) => s.Length;
            public static int Pure(int n) => n * 2;
        }
    )CS",
                                        "<jit>");
    CHECK_EQ(ctx.call(assembly, "K.Join", {ctx.str("a"), ctx.str("b")}).toString(), "ab");
    CHECK_EQ(ctx.call(assembly, "K.Length", {ctx.str("abc")}).toInt(), 3);
    CHECK_EQ(ctx.call(assembly, "K.Pure", {script::Value(21)}).toInt(), 42);
}

TEST(tieringCompilesOnlyWhatGetsHot)
{
    script::Context::Options options;
    options.jit.mode = script::JitOptions::Mode::Tiered;
    options.jit.threshold = 20;
    script::Context ctx(options);
    script::Assembly assembly = ctx.compile(R"CS(
        static class K {
            public static int Warm(int n) => n + 1;
            public static int Cold(int n) => n - 1;
        }
    )CS",
                                        "<jit>");

    CHECK_EQ(ctx.stats().methodsCompiled, std::uint64_t(0));
    int total = 0;
    for (int i = 0; i < 200; ++i)
        total += ctx.call(assembly, "K.Warm", {script::Value(i)}).toInt();
    CHECK_EQ(total, 20100);
    if (std::string(ctx.jitBackend()) != "interpreter") {
        CHECK_EQ(ctx.stats().methodsCompiled, std::uint64_t(1));
        CHECK(ctx.stats().nativeCodeBytes > 0);
    }
    // Still correct after the switch to native code.
    CHECK_EQ(ctx.call(assembly, "K.Warm", {script::Value(41)}).toInt(), 42);
    CHECK_EQ(ctx.call(assembly, "K.Cold", {script::Value(43)}).toInt(), 42);
}

TEST(nativeCodeIsFasterWhereItApplies)
{
    Pair pair(kLoops);
    if (!pair.usedNativeCode())
        return;  // no back end for this host; nothing to measure

    // A sanitizer build runs the interpreter orders of magnitude slower, and
    // the ratio it would measure is not the one being claimed. Keep it short.
#if defined(__SANITIZE_ADDRESS__) || defined(CS_SANITIZERS)
    const int iterations = 20000;
#else
    const int iterations = 2000000;
#endif
    auto time = [iterations](script::Context& ctx, script::Assembly& assembly) {
        const auto start = std::chrono::steady_clock::now();
        ctx.call(assembly, "K.Sum", {script::Value(iterations)});
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    };
    const double interpreted = time(pair.interpreted, pair.interpretedAssembly);
    const double compiled = time(pair.compiled, pair.compiledAssembly);
    // A loose bound: the point is that compiling won, not by how much.
    CHECK(compiled < interpreted);
}
