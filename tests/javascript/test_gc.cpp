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

TEST(aBoundedFreeLeavesTheRestForLater)
{
    // `freeBudget`: letting go of a long list frees a part of it at a time; the rest goes as the program allocates, and
    // all of it when the host collects.
    js::Context::Options options;
    options.memory = js::Context::Options::Memory::Counted;
    options.freeBudget = 1000;
    js::Context ctx(options);
    if (ctx.core() != script::Context::Options::Core::Vm)
        return;  // the vm core's memory options
    ctx.eval("0");
    const auto base = ctx.stats().liveObjects;
    ctx.eval("globalThis.l = null; for (let i = 0; i < 200000; i++) l = { next: l };");
    CHECK(ctx.stats().liveObjects >= base + 200000);
    ctx.eval("l = null");
    CHECK(ctx.stats().liveObjects >= base + 150000);  // most of it waits
    ctx.eval("for (let i = 0; i < 50; i++) ({ i });");  // each allocation frees a part
    const auto after = ctx.stats().liveObjects;
    CHECK(after < base + 200000);
    ctx.collectGarbage();
    CHECK(ctx.stats().liveObjects < base + 100);
    // Still correct afterwards: what is made and dropped again goes the same way.
    CHECK_EQ(ctx.eval("let s = 0; for (let k = 0; k < 3; k++) { let m = null; for (let i = 0; i < 20000; i++) m = { v: i, n: m };"
                      " for (let p = m; p; p = p.n) s += p.v; m = null; } s").toDouble(), 3.0 * 199990000.0);
}

TEST(manualCyclesWaitForTheHost)
{
    js::Context::Options options;
    options.cycles = js::Context::Options::Cycles::Manual;
    options.gcThreshold = 1u << 12;
    js::Context ctx(options);
    if (ctx.core() != script::Context::Options::Core::Vm)
        return;  // the vm core's memory options
    ctx.eval("function makeCycle() { const a = {}; const b = { a }; a.b = b; return null; }"
             "for (let i = 0; i < 20000; i++) makeCycle();");
    const auto before = ctx.stats();
    CHECK(before.gcRuns == 0);
    ctx.collectGarbage();
    const auto after = ctx.stats();
    CHECK(after.gcRuns == 1);
    CHECK(after.gcFreed >= 20000);
    CHECK(after.liveObjects < before.liveObjects);
}

TEST(spreadArgumentsLiveUntilTheirCallIsOver)
{
    // Their registers reach past the caller's, and the list they came from has lost its own: a native that runs other
    // code before it reads them, an arguments object, a rest parameter.
    const char* program =
        "var out = []; "
        "function mk(i) { return { i: i, toString() { var g = []; for (var k = 0; k < 300; k++) g.push({ k: k, s: 'x' + k }); return 's' + i; } }; } "
        "function list(n) { var a = []; for (var i = 0; i < n; i++) a.push(mk(i)); return a; } "
        "function go(n) { return String.prototype.concat.call('', ...list(n)); } "
        "for (var r = 0; r < 60; r++) { var s = go(40); if (r % 20 == 0) out.push(s.length); } "
        "function direct(n) { return ''.concat(...list(n)); } "
        "for (var r = 0; r < 60; r++) { var s = direct(40); if (r % 20 == 0) out.push(s.length, s.slice(0, 12)); } "
        "function take(a, b) { var g = []; for (var k = 0; k < 200; k++) g.push({ k }); return arguments.length + ':' + arguments[35].i + ':' + a.i + b.i; } "
        "for (var r = 0; r < 60; r++) { var s = take(...list(40)); if (r % 20 == 0) out.push(s); } "
        "class A { constructor(...xs) { var g = []; for (var k = 0; k < 200; k++) g.push({ k }); this.n = xs.length + xs[xs.length - 1].i; } } "
        "for (var r = 0; r < 60; r++) { var s = new A(...list(40)).n; if (r % 20 == 0) out.push(s); } "
        "out.join(' ') ";
    for (int mode = 0; mode < 2; ++mode) {
        script::Context::Options options;
        options.language = script::Language::JavaScript;
        options.memory = mode ? script::Context::Options::Memory::Deferred : script::Context::Options::Memory::Counted;
        script::Context ctx(options);
        CHECK_EQ(ctx.eval(program, "<test>").toString(),
                 std::string("110 110 110 110 s0s1s2s3s4s5 110 s0s1s2s3s4s5 110 s0s1s2s3s4s5 40:35:01 40:35:01 40:35:01 79 79 79"));
    }
}

TEST(deferredGarbageIsBoundedAndGoneWhenTheHostIsBack)
{
    // Frames hold values without counts: garbage waits for a scan, but not for a collection, and none waits once no script
    // runs. Values the host drops then go at once.
    script::Context::Options options;
    options.language = script::Language::JavaScript;
    options.memory = script::Context::Options::Memory::Deferred;
    options.cycles = script::Context::Options::Cycles::Manual;
    script::Context ctx(options);
    if (ctx.core() != script::Context::Options::Core::Vm)
        return;  // the vm core's memory options
    ctx.eval("0");
    const auto before = ctx.stats();
    ctx.eval("var s = 0; for (let i = 0; i < 200000; i++) { const o = { a: [i], b: 'k' + i + '-' }; s += o.a[0] + o.b.length; }");
    ctx.eval("function nest(n) { const o = { n, s: 'deep' + n }; return n ? nest(n - 1) + o.n + o.s.length : 0; } nest(300);");
    const auto after = ctx.stats();
    CHECK(after.gcRuns == before.gcRuns);
    CHECK(after.liveObjects < before.liveObjects + 200);
    CHECK(after.liveStrings < before.liveStrings + 200);
    {
        std::vector<script::Value> held;
        for (int i = 0; i < 5000; ++i)
            held.push_back(ctx.eval("({ x: [1, 2, 3] })"));
        CHECK(ctx.stats().liveObjects > after.liveObjects + 9000);
    }
    CHECK(ctx.stats().liveObjects < after.liveObjects + 200);
}
