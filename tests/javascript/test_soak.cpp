// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Long-run behaviour: after the warm-up, repeating a workload must not move
// the heap, the atom table, the pool reservation or the process's resident
// size. These are the tests that would catch a slow leak or a heap that
// gradually shreds.

#include <cstdio>
#include <string>
#include <vector>

#include "lang.hpp"
#include "sys/proc.hpp"
#include "tests/test_util.hpp"

namespace {

// Sanitizers cost roughly fifty times the run time, and their quarantine and
// redzones deliberately hold on to freed memory, so resident size says nothing
// about the engine there. The assertions are about steady state rather than
// absolute counts, so the shape of every test holds at a smaller volume.
#if defined(__SANITIZE_ADDRESS__)
constexpr bool kSanitized = true;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
constexpr bool kSanitized = true;
#else
constexpr bool kSanitized = false;
#endif
#else
constexpr bool kSanitized = false;
#endif

constexpr int kSlowdown = kSanitized ? 8 : 1;

constexpr int scaled(int n)
{
    return n / kSlowdown > 4 ? n / kSlowdown : 4;
}

constexpr int kWarmupRounds = 20;
constexpr int kSoakRounds = scaled(400);
/// One pool arena; the reservation is expected to move in these steps.
constexpr std::uint64_t CellPoolBlock = 64u << 10;
/// mem::LocalHeap::Options::minIdleBlocks of them.
constexpr std::uint64_t kIdleReserve = 16 * CellPoolBlock;

/// Resident set size in bytes, or 0 where the number would not mean anything.
std::size_t residentBytes()
{
    return kSanitized ? 0 : sys::residentBytes();
}

/// A round of work that leaves nothing behind: objects, arrays, strings,
/// closures, classes, maps, exceptions, promises and a reference cycle.
const char* kMixedWorkload = R"(
    (function round(n) {
        class Point {
            constructor(x, y) { this.x = x; this.y = y; }
            get norm() { return Math.sqrt(this.x ** 2 + this.y ** 2); }
            toString() { return `(${this.x}, ${this.y})`; }
        }
        const points = [];
        for (let i = 0; i < 50; i++)
            points.push(new Point(i, n - i));

        const byParity = new Map();
        for (const p of points) {
            const key = p.x % 2 ? 'odd' : 'even';
            if (!byParity.has(key)) byParity.set(key, []);
            byParity.get(key).push(p.toString());
        }

        const summary = {
            total: points.reduce((sum, p) => sum + p.norm, 0),
            labels: [...byParity.keys()].sort(),
            sample: points.slice(0, 3).map(p => ({ ...p, tag: `p${p.x}` })),
        };
        const encoded = JSON.stringify(summary);
        const decoded = JSON.parse(encoded);

        // A cycle only the collector can break.
        const a = { name: 'a' };
        const b = { name: 'b', a };
        a.b = b;

        // A closure cycle.
        const state = { hits: 0 };
        state.bump = () => ++state.hits;
        state.bump();

        try {
            null.boom;
        } catch (e) {
            if (!(e instanceof TypeError)) throw e;
        }

        Promise.resolve(decoded.labels.length).then(v => v + 1);
        return encoded.length;
    })
)";

}  // namespace

TEST(steadyStateHeapDoesNotDrift)
{
    js::Context ctx;
    js::Value round = ctx.eval(kMixedWorkload, "<workload>");

    for (int i = 0; i < kWarmupRounds; ++i) {
        round.call({js::Value(i)});
        ctx.runJobs();
    }
    ctx.collectGarbage();
    const js::Context::Stats base = ctx.stats();

    for (int i = 0; i < kSoakRounds; ++i) {
        round.call({js::Value(i)});
        ctx.runJobs();
    }
    ctx.collectGarbage();
    const js::Context::Stats after = ctx.stats();

    CHECK(after.liveObjects <= base.liveObjects + 64);
    CHECK(after.liveStrings <= base.liveStrings + 64);
    CHECK(after.heapBytes <= base.heapBytes + (64u << 10));
    CHECK(after.strings <= base.strings + 32);
    CHECK_EQ(after.pendingJobs, std::uint64_t(0));
}

TEST(repeatedCompilationDoesNotAccumulate)
{
    js::Context ctx;
    auto compileAndRun = [&ctx](int i) {
        const std::string source =
            "(function () { const marker" + std::to_string(i) + " = { id: " + std::to_string(i) +
            ", name: 'run" + std::to_string(i) + "' }; return marker" + std::to_string(i) +
            ".id; })()";
        ctx.eval(source, "<generated>");
    };

    for (int i = 0; i < kWarmupRounds; ++i)
        compileAndRun(i);
    ctx.collectGarbage();
    const js::Context::Stats base = ctx.stats();

    for (int i = 0; i < scaled(2000); ++i)
        compileAndRun(i);
    ctx.collectGarbage();
    const js::Context::Stats after = ctx.stats();

    // Every one of those programs interned fresh names. None of them survive.
    CHECK(after.strings <= base.strings + 32);
    CHECK(after.liveObjects <= base.liveObjects + 32);
    CHECK(after.heapBytes <= base.heapBytes + (32u << 10));
}

TEST(dynamicPropertyNamesAreReclaimed)
{
    js::Context ctx;
    ctx.eval("globalThis.build = function (n) {"
             "  const o = {};"
             "  for (let i = 0; i < n; i++) o['field_' + i] = i;"
             "  return Object.keys(o).length;"
             "}");
    ctx.eval("build(200)");
    ctx.collectGarbage();
    const js::Context::Stats base = ctx.stats();

    for (int i = 0; i < scaled(50); ++i)
        ctx.eval("build(2000)");
    ctx.collectGarbage();
    const js::Context::Stats after = ctx.stats();

    CHECK(after.strings <= base.strings + 64);
    CHECK(after.heapBytes <= base.heapBytes + (64u << 10));
}

TEST(poolReservationStabilises)
{
    js::Context ctx;
    js::Value round = ctx.eval(kMixedWorkload, "<workload>");
    auto soak = [&](int rounds) {
        for (int i = 0; i < rounds; ++i) {
            round.call({js::Value(i)});
            ctx.runJobs();
        }
        ctx.collectGarbage();
        return ctx.stats().reservedBytes;
    };

    const std::uint64_t settled = soak(scaled(200));
    CHECK(settled > 0);
    const std::uint64_t midway = soak(kSoakRounds);
    const std::uint64_t later = soak(kSoakRounds * 4);

    // Slots are recycled, so a repeated workload reuses blocks instead of asking
    // the OS for new ones. The first rounds may still fill the reserve of idle
    // blocks the heap keeps for the next burst; after that, five times the work
    // must not move the reservation.
    CHECK(midway <= settled + kIdleReserve);
    CHECK(later <= midway + (kSanitized ? kIdleReserve : CellPoolBlock));  // scaled down, the reserve is still filling
}

TEST(burstsReturnTheirMemory)
{
    js::Context ctx;
    ctx.eval("globalThis.hold = null");
    ctx.collectGarbage();
    const js::Context::Stats quiet = ctx.stats();

    ctx.eval("hold = []; for (let i = 0; i < " + std::to_string(scaled(200000)) +
             "; i++) hold.push({ i, s: 'v' + i });");
    const js::Context::Stats loaded = ctx.stats();
    CHECK(loaded.heapBytes > quiet.heapBytes * 4);

    ctx.eval("hold = null");
    ctx.collectGarbage();
    const js::Context::Stats released = ctx.stats();

    CHECK(released.heapBytes < quiet.heapBytes + (256u << 10));
    CHECK(released.liveObjects < quiet.liveObjects + 64);
    // The pool gives whole blocks back once a size class falls idle.
    CHECK(released.reservedBytes < loaded.reservedBytes / 2);
}

TEST(deepStructuresFreeWithoutOverflowingTheStack)
{
    const int depth = scaled(300000);
    js::Context ctx;
    ctx.eval("globalThis.head = null;"
             "for (let i = 0; i < " + std::to_string(depth) + "; i++) head = { i, next: head };");
    const js::Context::Stats loaded = ctx.stats();
    CHECK(loaded.liveObjects > std::uint64_t(depth));

    // Dropping the head releases the whole chain in one go. Freeing that
    // recursively would run the C++ stack out well before the end.
    ctx.eval("head = null");
    const js::Context::Stats released = ctx.stats();
    CHECK(released.liveObjects < 1000);
}

TEST(mapAndSetChurnStaysBounded)
{
    js::Context ctx;
    ctx.eval("globalThis.cache = new Map(); globalThis.seen = new Set();");
    ctx.eval("globalThis.churn = function (rounds) {"
             "  for (let i = 0; i < rounds; i++) {"
             "    const key = 'k' + (i % 500);"
             "    cache.set(key, { i });"
             "    seen.add(i % 500);"
             "    if (i % 3 === 0) { cache.delete(key); seen.delete(i % 500); }"
             "  }"
             "  return cache.size + seen.size;"
             "}");
    ctx.eval("churn(5000)");
    ctx.collectGarbage();
    const js::Context::Stats base = ctx.stats();

    for (int i = 0; i < scaled(40); ++i)
        ctx.eval("churn(5000)");
    ctx.collectGarbage();
    const js::Context::Stats after = ctx.stats();

    // Tombstones are swept, so 200k operations over 500 keys stay flat.
    CHECK(after.heapBytes <= base.heapBytes + (128u << 10));
    CHECK(after.liveObjects <= base.liveObjects + 1024);
}

TEST(arraysChangingFormStayBounded)
{
    // In a cell and out, numbers turning into values, growing and shrinking: nothing of it may be left behind.
    js::Context ctx;
    ctx.eval(R"(
        globalThis.formsRound = (function round(n) {
            const small = [];
            for (let i = 0; i < 64; i++) small.push([i, i + 1]);
            for (let i = 0; i < 64; i++) small[i].push({ i });
            const grown = [];
            for (let i = 0; i < 2000; i++) grown.push(i);
            grown[17] = 'text';
            const sparse = new Array(5000);
            sparse[9] = n;
            sparse.length = 10;
            sparse.push({}, undefined, 'x');
            while (grown.length > 40) grown.pop();
            grown.sort();
            const nums = [];
            for (let i = 0; i < 500; i++) nums.push(500 - i);
            nums.sort((a, b) => a - b);
            nums.splice(3, 400, 'y');
            return nums.concat(grown, sparse).map(x => x).filter(x => x !== 0).length + small.length;
        });
    )");
    for (int i = 0; i < kWarmupRounds; ++i)
        ctx.eval("formsRound(1)");
    ctx.collectGarbage();
    const auto before = ctx.stats();
    const std::size_t residentBefore = residentBytes();
    for (int i = 0; i < kSoakRounds; ++i)
        ctx.eval("formsRound(1)");
    ctx.collectGarbage();
    const auto after = ctx.stats();
    CHECK(after.liveObjects <= before.liveObjects + 8);
    CHECK(after.heapBytes <= before.heapBytes + 64 * 1024);
    if (residentBefore)
        CHECK(residentBytes() <= residentBefore + 4 * 1024 * 1024);
}

TEST(promiseChurnDrainsCompletely)
{
    js::Context ctx;
    ctx.eval("globalThis.step = function (n) {"
             "  return Promise.resolve(n).then(v => v + 1).then(v => ({ v })).catch(() => null);"
             "}");
    for (int i = 0; i < kWarmupRounds; ++i) {
        ctx.eval("step(1)");
        ctx.runJobs();
    }
    ctx.collectGarbage();
    const js::Context::Stats base = ctx.stats();

    for (int i = 0; i < scaled(2000); ++i) {
        ctx.eval("step(1)");
        ctx.runJobs();
    }
    ctx.collectGarbage();
    const js::Context::Stats after = ctx.stats();

    CHECK_EQ(after.pendingJobs, std::uint64_t(0));
    CHECK(after.liveObjects <= base.liveObjects + 64);
    CHECK(after.heapBytes <= base.heapBytes + (64u << 10));
}

TEST(manyContextsComeAndGoCleanly)
{
    const std::size_t before = residentBytes();
    for (int i = 0; i < scaled(300); ++i) {
        js::Context ctx;
        ctx.global().set("n", js::Value(i));
        ctx.eval("const acc = []; for (let k = 0; k < 200; k++) acc.push({ k, n }); acc.length");
        ctx.eval("(function () { const a = {}; a.self = a; return 1; })()");
    }
    const std::size_t after = residentBytes();
    if (before && after)
        CHECK(after < before + (16u << 20));
}

TEST(residentMemoryStabilisesOverALongRun)
{
    js::Context ctx;
    js::Value round = ctx.eval(kMixedWorkload, "<workload>");
    for (int i = 0; i < scaled(200); ++i) {
        round.call({js::Value(i)});
        ctx.runJobs();
    }
    ctx.collectGarbage();
    const std::size_t settled = residentBytes();

    for (int i = 0; i < scaled(4000); ++i) {
        round.call({js::Value(i)});
        ctx.runJobs();
    }
    ctx.collectGarbage();
    const std::size_t after = residentBytes();

    // Twenty times the work must not move the process's footprint.
    if (settled && after)
        CHECK(after < settled + (8u << 20));
}
