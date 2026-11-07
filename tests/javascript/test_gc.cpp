// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Reference counting reclaims immediately; the cycle collector picks up the
// rest. These tests watch the heap counters rather than the wall clock.

#include "lang.hpp"
#include "tests/test_util.hpp"

TEST(acyclicGarbageGoesAwayWithoutCollecting)
{
    js::Context ctx;
    ctx.eval("0");  // the first evaluation creates the engine's intrinsics
    const auto before = ctx.stats();
    ctx.eval("for (let i = 0; i < 2000; i++) { const o = { a: i, b: [i, i] }; o.a; }");
    const auto after = ctx.stats();
    CHECK(after.liveObjects < before.liveObjects + 200);
    CHECK(after.gcRuns == before.gcRuns);
}

TEST(whatThingsWeighStaysWhatItIs)
{
    // The memory an embedder pays per object and per element is a promise, so this fails when it grows: a change here is
    // to be argued for, not noticed in a benchmark afterwards.
    js::Context ctx;
    ctx.eval("0");
    const auto weigh = [&](const char* setup) {
        const auto before = ctx.stats().heapBytes;
        ctx.eval(setup);
        return (ctx.stats().heapBytes - before) / 100000;
    };
    // A small object: 80 bytes of header and 16 a property, in the one cell; and the array holding them, about 21 each.
    CHECK(weigh("globalThis.o = []; function P(a, b) { this.a = a; this.b = b; }"
                "for (let i = 0; i < 100000; i++) o.push(new P(i, i));") <= 112 + 24);
    ctx.eval("o = null");
    // A number in an array: 8 bytes, or at most twice that for the room to grow.
    CHECK(weigh("globalThis.n = []; for (let i = 0; i < 100000; i++) n.push(i);") <= 24);
    ctx.eval("n = null");
    CHECK(weigh("globalThis.h = new Array(100000);") <= 8);
    ctx.eval("h = null");
    // A small array: one cell, with its elements in it.
    CHECK(weigh("globalThis.s = []; for (let i = 0; i < 100000; i++) s.push([i, i]);") <= 96 + 32 + 24);
}

TEST(cyclesAreReclaimedByTheCollector)
{
    js::Context ctx;
    ctx.eval("globalThis.keep = null;"
             "function makeCycle() { const a = {}; const b = { a }; a.b = b; return null; }"
             "for (let i = 0; i < 500; i++) makeCycle();");
    const auto leaked = ctx.stats();
    ctx.collectGarbage();
    const auto collected = ctx.stats();
    CHECK(collected.liveObjects < leaked.liveObjects);
    CHECK(collected.gcFreed >= 500);
}

TEST(closureCyclesAreReclaimed)
{
    js::Context ctx;
    ctx.eval("function build() { const state = {}; state.fn = () => state; return null; }"
             "for (let i = 0; i < 300; i++) build();");
    const auto leaked = ctx.stats();
    ctx.collectGarbage();
    CHECK(ctx.stats().liveObjects < leaked.liveObjects);
}

TEST(valuesHeldFromCppSurviveCollection)
{
    js::Context ctx;
    js::Value kept = ctx.eval("({ marker: 'alive', nested: { deep: [1, 2, 3] } })");
    ctx.collectGarbage();
    ctx.collectGarbage();
    CHECK_EQ(kept.get("marker").toString(), std::string("alive"));
    CHECK_EQ(kept.get("nested").get("deep").get(2u).toDouble(), 3.0);
}

TEST(theHeapShrinksBackToAQuietBaseline)
{
    js::Context ctx;
    ctx.eval("globalThis.big = [];"
             "for (let i = 0; i < 5000; i++) big.push({ i, s: 'x' + i });");
    const auto loaded = ctx.stats();
    ctx.eval("big = null");
    ctx.collectGarbage();
    CHECK(ctx.stats().liveObjects < loaded.liveObjects / 2);
}

TEST(memoryLimitsAreEnforced)
{
    js::Context::Options options;
    options.memoryLimit = 4u << 20;
    options.gcThreshold = 1u << 12;
    js::Context ctx(options);
    bool stopped = false;
    try {
        ctx.eval("const a = []; for (let i = 0; i < 5000000; i++) a.push({ i });");
    } catch (const js::Error&) {
        stopped = true;
    }
    CHECK(stopped);
}

TEST(growingOneContainerStillHitsTheLimit)
{
    // Pushing numbers allocates no cells at all, so only the interpreter's
    // periodic check can notice this one.
    js::Context::Options options;
    options.memoryLimit = 4u << 20;
    js::Context ctx(options);
    bool stopped = false;
    try {
        ctx.eval("const a = []; for (let i = 0; i < 50000000; i++) a.push(i);");
    } catch (const js::Error&) {
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
        js::Context ctx;
        js::Class<Tracked>(ctx, "Tracked").install();
        ctx.global().set("make", ctx.function("make", [&destroyed](js::CallInfo& ci) {
                             return ci.context().native(
                                 js::typeKey<Tracked>(), new Tracked{&destroyed},
                                 [](void* p) { delete static_cast<Tracked*>(p); });
                         }));
        ctx.eval("for (let i = 0; i < 10; i++) make();");
    }
    CHECK_EQ(destroyed, 10);
}
