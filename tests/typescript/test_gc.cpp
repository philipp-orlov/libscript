// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Reference counting reclaims immediately; the cycle collector picks up the
// rest. These tests watch the heap counters rather than the wall clock.

#include "lang.hpp"
#include "tests/test_util.hpp"

TEST(acyclicGarbageGoesAwayWithoutCollecting)
{
    ts::Context ctx;
    ctx.eval("0");  // the first evaluation creates the engine's intrinsics
    const auto before = ctx.stats();
    ctx.eval("for (let i = 0; i < 2000; i++) { const o = { a: i, b: [i, i] }; o.a; }");
    const auto after = ctx.stats();
    CHECK(after.liveObjects < before.liveObjects + 200);
    CHECK(after.gcRuns == before.gcRuns);
}

TEST(cyclesAreReclaimedByTheCollector)
{
    ts::Context ctx;
    ctx.eval(
        "var keep = null;"
        "function makeCycle() { const a: any = {}; const b = { a }; a.b = b; return null; }"
        "for (let i = 0; i < 500; i++) makeCycle();");
    const auto leaked = ctx.stats();
    ctx.collectGarbage();
    const auto collected = ctx.stats();
    CHECK(collected.liveObjects < leaked.liveObjects);
    CHECK(collected.gcFreed >= 500);
}

TEST(closureCyclesAreReclaimed)
{
    ts::Context ctx;
    ctx.eval(
        "function build() { const state: any = {}; state.fn = () => state; return null; }"
        "for (let i = 0; i < 300; i++) build();");
    const auto leaked = ctx.stats();
    ctx.collectGarbage();
    CHECK(ctx.stats().liveObjects < leaked.liveObjects);
}

TEST(valuesHeldFromCppSurviveCollection)
{
    ts::Context ctx;
    ts::Value kept = ctx.eval("({ marker: 'alive', nested: { deep: [1, 2, 3] } })");
    ctx.collectGarbage();
    ctx.collectGarbage();
    CHECK_EQ(kept.get("marker").toString(), std::string("alive"));
    CHECK_EQ(kept.get("nested").get("deep").get(2u).toDouble(), 3.0);
}

TEST(theHeapShrinksBackToAQuietBaseline)
{
    ts::Context ctx;
    ctx.eval(
        "var big: any[] = [];"
        "for (let i = 0; i < 5000; i++) big.push({ i, s: 'x' + i });");
    const auto loaded = ctx.stats();
    ctx.eval("big = null");
    ctx.collectGarbage();
    CHECK(ctx.stats().liveObjects < loaded.liveObjects / 2);
}

TEST(memoryLimitsAreEnforced)
{
    ts::Context::Options options;
    options.memoryLimit = 4u << 20;
    options.gcThreshold = 1u << 12;
    ts::Context ctx(options);
    bool stopped = false;
    try {
        ctx.eval("const a = []; for (let i = 0; i < 5000000; i++) a.push({ i });");
    } catch (const ts::Error&) {
        stopped = true;
    }
    CHECK(stopped);
}

TEST(growingOneContainerStillHitsTheLimit)
{
    // Pushing numbers allocates no cells at all, so only the interpreter's
    // periodic check can notice this one.
    ts::Context::Options options;
    options.memoryLimit = 4u << 20;
    ts::Context ctx(options);
    bool stopped = false;
    try {
        ctx.eval("const a = []; for (let i = 0; i < 50000000; i++) a.push(i);");
    } catch (const ts::Error&) {
        stopped = true;
    }
    CHECK(stopped);
    CHECK(ctx.stats().heapBytes < (16u << 20));
}

TEST(nativeInstancesAreFinalized)
{
    struct Tracked
    {
        int* counter;
        ~Tracked() { ++*counter; }
    };
    int destroyed = 0;
    {
        ts::Context ctx;
        ts::Class<Tracked>(ctx, "Tracked").install();
        ctx.global().set("make", ctx.function("make", [&destroyed](ts::CallInfo& ci) {
            return ci.context().native(
                ts::typeKey<Tracked>(), new Tracked{&destroyed},
                [](void* p) { delete static_cast<Tracked*>(p); });
        }));
        ctx.eval("for (let i = 0; i < 10; i++) make();");
    }
    CHECK_EQ(destroyed, 10);
}
