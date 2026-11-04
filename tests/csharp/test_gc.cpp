// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Memory: reference counting, the cycle collector, the cell pool.
#include <memory>

#include "script/script.hpp"
#include "tests/test_util.hpp"

namespace {

std::uint64_t liveAfter(const char* source)
{
    script::Context ctx;
    ctx.eval(source);
    ctx.collectGarbage();
    return ctx.stats().liveObjects;
}

}  // namespace

TEST(acyclicGarbageGoesImmediately)
{
    script::Context ctx;
    ctx.eval(R"(
        class Node { public int Value; }
        for (int i = 0; i < 1000; i++) {
            var n = new Node();
            n.Value = i;
        }
    )");
    // Nothing above survives the loop, and nothing above is cyclic, so the
    // objects are gone without the collector having to run at all.
    const script::Context::Stats stats = ctx.stats();
    CHECK(stats.liveObjects < 100);
    CHECK_EQ(stats.gcRuns, std::uint64_t(0));
}

TEST(cyclesAreCollected)
{
    script::Context ctx;
    ctx.eval(R"(
        class Node { public Node Next; }
        for (int i = 0; i < 500; i++) {
            var a = new Node();
            var b = new Node();
            a.Next = b;
            b.Next = a;
        }
    )");
    const std::uint64_t before = ctx.stats().liveObjects;
    ctx.collectGarbage();
    const script::Context::Stats after = ctx.stats();
    CHECK(before > 500);
    CHECK(after.liveObjects < 100);
    CHECK(after.gcFreed >= 1000);
}

TEST(selfReferenceIsACycle)
{
    CHECK(liveAfter(R"(
        class Node { public Node Self; }
        for (int i = 0; i < 100; i++) { var n = new Node(); n.Self = n; }
    )") < 50);
}

TEST(closuresThatCaptureThemselvesAreCollected)
{
    CHECK(liveAfter(R"(
        class Holder { public System.Func<int> Fn; }
        for (int i = 0; i < 100; i++) {
            var h = new Holder();
            h.Fn = () => h.GetHashCode();
        }
    )") < 50);
}

TEST(liveDataSurvivesCollection)
{
    script::Context ctx;
    script::Assembly assembly = ctx.compile(R"(
        static class Keep {
            public static System.Collections.Generic.List<string> Items =
                new System.Collections.Generic.List<string>();
            public static void Fill() {
                for (int i = 0; i < 200; i++) Items.Add("item " + i);
            }
            public static int Count() { return Items.Count; }
        }
    )",
                                        "<keep>");
    ctx.call(assembly, "Keep.Fill");
    ctx.collectGarbage();
    CHECK_EQ(ctx.call(assembly, "Keep.Count").toInt(), 200);
}

TEST(stringsAreInternedAndSwept)
{
    script::Context ctx;
    ctx.eval(R"(
        for (int i = 0; i < 2000; i++) {
            string s = "prefix " + i;
        }
    )");
    const std::uint64_t before = ctx.stats().strings;
    ctx.collectGarbage();
    const std::uint64_t after = ctx.stats().strings;
    CHECK(after <= before);
    CHECK(after < 1000);
}

TEST(poolReusesMemory)
{
    script::Context ctx;
    ctx.eval("class Node { public int V; } for (int i = 0; i < 5000; i++) { var n = new Node(); }");
    const script::Context::Stats first = ctx.stats();
    ctx.eval("class Node2 { public int V; } for (int i = 0; i < 5000; i++) { var n = new Node2(); }");
    const script::Context::Stats second = ctx.stats();
    // The second burst is served out of the blocks the first one left behind.
    CHECK(second.reservedBytes <= first.reservedBytes * 2);
    CHECK(second.totalAllocated > first.totalAllocated);
}

TEST(arraysOfPrimitivesAreCompact)
{
    script::Context ctx;
    const std::uint64_t before = ctx.stats().heapBytes;
    ctx.compile("static class M { public static int[] Make() { return new int[100000]; } }",
                "<x>");
    script::Assembly assembly = ctx.compile(
        "static class M { public static int[] Make() { return new int[100000]; } }", "<y>");
    script::Value array = ctx.call(assembly, "M.Make");
    const std::uint64_t after = ctx.stats().heapBytes;
    CHECK_EQ(array.length(), 100000u);
    // Four bytes an element, not the sixteen a boxed Value would cost.
    CHECK(after - before < 1u << 20);
}

TEST(statsAddUp)
{
    script::Context ctx;
    ctx.eval("class A { } var a = new A();");
    const script::Context::Stats stats = ctx.stats();
    CHECK(stats.totalAllocated > 0);
    CHECK(stats.assemblies == 1);
    // Nothing was asked to be compiled, so no code memory was spent whatever
    // back end this build has.
    CHECK_EQ(stats.methodsCompiled, std::uint64_t(0));
    CHECK_EQ(stats.nativeCodeBytes, std::uint64_t(0));
}

TEST(contextsAreIndependent)
{
    script::Context first;
    script::Context second;
    first.eval("class A { public int X; } var a = new A();");
    CHECK_EQ(second.stats().liveObjects, std::uint64_t(0));
    CHECK_EQ(second.eval("return 1;").toInt(), 1);
}

TEST(topLevelEvaluationsDoNotAccumulate)
{
    script::Context ctx;
    for (int i = 0; i < 5000; ++i)
        CHECK_EQ(ctx.eval("return " + std::to_string(i) + " + 1;", "<t>.cs", script::Language::CSharp).toInt(), i + 1);
    ctx.collectGarbage();
    CHECK(ctx.stats().assemblies <= 1);
    CHECK(ctx.stats().strings < 64);
}

TEST(aUnitStaysWhileADelegateFromItLives)
{
    script::Context ctx;
    script::Value twice = ctx.eval("System.Func<int, int> f = x => x * 2; return f;", "<t>.cs", script::Language::CSharp);
    for (int i = 0; i < 2000; ++i)
        ctx.eval("return 1;", "<t>.cs", script::Language::CSharp);
    ctx.collectGarbage();
    CHECK_EQ(twice.call({script::Value(21)}).toInt(), 42);
    twice = script::Value();
    ctx.collectGarbage();
    CHECK(ctx.stats().assemblies <= 1);
}

TEST(aHeldHandleKeepsItsUnit)
{
    script::Context ctx;
    script::Assembly held = ctx.compile("return 7;", "<t>.cs", script::Language::CSharp);
    for (int i = 0; i < 2000; ++i)
        ctx.eval("return 1;", "<t>.cs", script::Language::CSharp);
    ctx.collectGarbage();
    CHECK_EQ(ctx.run(held).toInt(), 7);
}

TEST(aUnitThatDeclaresATypeStaysForLaterUnits)
{
    script::Context ctx;
    (void)ctx.compile("public class Keep { public static int V = 5; }", "<k>.cs", script::Language::CSharp);
    for (int i = 0; i < 2000; ++i)
        ctx.eval("return 1;", "<t>.cs", script::Language::CSharp);
    ctx.collectGarbage();
    CHECK_EQ(ctx.eval("return Keep.V;", "<t>.cs", script::Language::CSharp).toInt(), 5);
}

TEST(aUnitAnotherUnitCallsStays)
{
    script::Context ctx;
    (void)ctx.compile("return 11;", "<a>.cs", script::Language::CSharp);
    (void)ctx.compile("public static class Caller { public static int F() { return (int)Program.Main(); } }", "<b>.cs",
                      script::Language::CSharp);
    for (int i = 0; i < 2000; ++i)
        ctx.eval("return 1;", "<t>.cs", script::Language::CSharp);
    ctx.collectGarbage();
    (void)ctx.compile("public static class Check { public static int G() { return Caller.F(); } }", "<c>.cs", script::Language::CSharp);
    CHECK_EQ(ctx.eval("return Check.G();", "<t>.cs", script::Language::CSharp).toInt(), 11);
}
