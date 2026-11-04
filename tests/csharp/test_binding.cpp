// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The C++ binding layer: host classes, wrapped objects, values crossing over.
#include <memory>
#include <string>

#include "script/script.hpp"
#include "tests/test_util.hpp"

namespace {

struct Counter
{
    int value = 0;

    void add(int n) { value += n; }
    int get() const { return value; }
    std::string describe(const std::string& prefix) const
    {
        return prefix + ":" + std::to_string(value);
    }
};

}  // namespace

TEST(hostFunctions)
{
    script::Context ctx;
    ctx.hostClass("Env")
        .method("Double", [](int n) { return n * 2; })
        .method("Greet", [](std::string name) { return "hello " + name; })
        .method("Sum", [](double a, double b) { return a + b; })
        .method("Flag", [] { return true; })
        .method("Squares", [](int n) {
            std::vector<int> out;
            for (int i = 1; i <= n; ++i)
                out.push_back(i * i);
            return out;
        });

    CHECK_EQ(ctx.eval("return Env.Double(21);").toInt(), 42);
    // A std::vector result is a typed array to a typed caller.
    CHECK_EQ(ctx.eval("int[] a = Env.Squares(3); return a.Length * 100 + a[2];").toInt(), 309);
    CHECK_EQ(ctx.eval("return Env.Greet(\"world\");").toString(), "hello world");
    CHECK_EQ(ctx.eval("return Env.Sum(1.5, 2.5);").toDouble(), 4.0);
    CHECK(ctx.eval("return Env.Flag();").toBool());
}

TEST(hostFunctionsAreTypeChecked)
{
    script::Context ctx;
    ctx.hostClass("Env").method("Double", [](int n) { return n * 2; });

    script::Assembly assembly;
    const std::vector<script::Diagnostic> diagnostics =
        ctx.diagnose("Env.Double(\"text\");", "<test>", &assembly);
    CHECK_EQ(diagnostics.size(), std::size_t(1));

    script::Assembly other;
    CHECK(!ctx.diagnose("Env.Missing();", "<test>", &other).empty());
}

TEST(hostState)
{
    script::Context ctx;
    int counter = 0;
    ctx.hostClass("Env").method("Bump", [&counter](int by) { counter += by; });
    ctx.eval("Env.Bump(3); Env.Bump(4);");
    CHECK_EQ(counter, 7);
}

TEST(wrappedClasses)
{
    script::Context ctx;
    script::Class<Counter>(ctx, "Counter")
        .constructor<>()
        .method("Add", &Counter::add)
        .method("Get", &Counter::get)
        .method("Describe", &Counter::describe);

    CHECK_EQ(ctx.eval(R"(
        var c = new Counter();
        c.Add(2);
        c.Add(5);
        return c.Get();
    )")
                 .toInt(),
             7);
    CHECK_EQ(ctx.eval(R"(
        var c = new Counter();
        c.Add(9);
        return c.Describe("n");
    )")
                 .toString(),
             "n:9");
}

TEST(valuesFromTheHost)
{
    script::Context ctx;
    script::Value text = ctx.str("hello");
    CHECK_EQ(text.toString(), "hello");
    CHECK(text.isString());

    script::Value numbers = ctx.newArray(script::Type::Int, 3);
    numbers.set(0u, script::Value(10));
    numbers.set(2u, script::Value(30));
    CHECK_EQ(numbers.length(), 3u);
    CHECK_EQ(numbers.get(0u).toInt(), 10);
    CHECK_EQ(numbers.get(1u).toInt(), 0);
    CHECK_EQ(numbers.get(2u).toInt(), 30);
}

TEST(callingIntoTheAssembly)
{
    script::Context ctx;
    script::Assembly assembly = ctx.compile(R"(
        static class Library {
            public static int Add(int a, int b) { return a + b; }
            public static string Name() { return "library"; }
        }
    )",
                                        "<lib>");
    CHECK_EQ(ctx.call(assembly, "Library.Add", {script::Value(2), script::Value(3)}).toInt(), 5);
    CHECK_EQ(ctx.call(assembly, "Library.Name").toString(), "library");
}

TEST(objectsFromTheHost)
{
    script::Context ctx;
    script::Assembly assembly = ctx.compile(R"(
        public class Point {
            public int X;
            public int Y;
            public Point(int x, int y) { X = x; Y = y; }
            public int Sum() { return X + Y; }
        }
    )",
                                        "<lib>");
    script::Value point = ctx.newObject(assembly, "Point", {script::Value(3), script::Value(4)});
    CHECK_EQ(point.get("X").toInt(), 3);
    CHECK_EQ(point.invoke("Sum").toInt(), 7);
    point.set("Y", script::Value(10));
    CHECK_EQ(point.invoke("Sum").toInt(), 13);
    CHECK(point.has("X"));
    CHECK_EQ(point.keys().size(), std::size_t(2));
    CHECK_EQ(point.typeName(), "Point");
}

TEST(delegatesCrossOver)
{
    script::Context ctx;
    script::Assembly assembly = ctx.compile(R"(
        static class Library {
            public static System.Func<int, int> Doubler() { return x => x * 2; }
        }
    )",
                                        "<lib>");
    script::Value doubler = ctx.call(assembly, "Library.Doubler");
    CHECK(doubler.isFunction());
    CHECK_EQ(doubler.call({script::Value(21)}).toInt(), 42);
    CHECK_EQ(doubler(4).toInt(), 8);
}

TEST(printAndReadHandlers)
{
    script::Context ctx;
    std::string written;
    ctx.setPrintHandler([&written](std::string_view text, bool) { written += text; });
    ctx.setReadLineHandler([](std::string& line) {
        line = "typed";
        return true;
    });
    ctx.eval("Console.WriteLine(Console.ReadLine());");
    CHECK_EQ(written, "typed\n");
}

TEST(interruptStopsRunawayCode)
{
    script::Context ctx;
    int ticks = 0;
    ctx.setInterruptHandler([&ticks] { return ++ticks > 2; });
    CHECK_THROWS(ctx.eval("while (true) { }"), script::ErrorCode::Interrupted);
}

TEST(memoryLimitIsEnforced)
{
    script::Context::Options options;
    options.memoryLimit = 2u << 20;
    script::Context ctx(options);
    CHECK_THROWS(ctx.eval(R"(
        var list = new List<string>();
        for (int i = 0; i < 1000000; i++) list.Add("padding padding padding " + i);
    )"),
                 script::ErrorCode::OutOfMemory);
}

TEST(stackLimitIsEnforced)
{
    script::Context::Options options;
    options.stackLimit = 64;
    script::Context ctx(options);
    CHECK_THROWS(ctx.eval(R"(
        static class M { public static int Down(int n) { return Down(n + 1); } }
        return M.Down(0);
    )"),
                 script::ErrorCode::StackOverflow);
}
