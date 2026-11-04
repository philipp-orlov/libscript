// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The long run: a workload repeated until the heap has to prove it settles.
//
// The point of the engine's memory design is that a process which runs for
// weeks reaches a steady state. This test runs the same work over and over and
// asserts that the heap, the pool and the string table all plateau instead of
// creeping.
#include <cstdio>
#include <memory>

#include "script/script.hpp"
#include "tests/test_util.hpp"

namespace {

#if defined(__SANITIZE_ADDRESS__) || defined(__has_feature)
#if defined(__SANITIZE_ADDRESS__)
#define CS_UNDER_SANITIZER 1
#endif
#endif

#if defined(CS_UNDER_SANITIZER)
// The sanitizer supplies the allocator, so the figures this test asserts about
// are not the ones being measured. Keep it short and skip the plateau checks.
constexpr int kRounds = 20;
constexpr bool kCheckPlateau = false;
#else
constexpr int kRounds = 400;
constexpr bool kCheckPlateau = true;
#endif

const char* kWorkload = R"(
    using System.Collections.Generic;

    class Node {
        public int Value;
        public Node Next;
        public Node Peer;
        public Node(int v) { Value = v; }
    }

    static class Work {
        public static int Round(int seed) {
            var list = new List<Node>();
            for (int i = 0; i < 200; i++) {
                var n = new Node(seed + i);
                if (i > 0) {
                    n.Next = list[i - 1];
                    // A cycle on purpose: only the collector can reclaim this.
                    list[i - 1].Peer = n;
                }
                list.Add(n);
            }

            var index = new Dictionary<string, int>();
            for (int i = 0; i < 200; i++)
                index["key " + (seed + i)] = i;

            int total = 0;
            foreach (Node n in list) total += n.Value % 7;
            foreach (string key in index.Keys) total += index[key] % 3;

            var text = new System.Text.StringBuilder();
            for (int i = 0; i < 50; i++) text.Append(i.ToString("D4"));
            total += text.ToString().Length;

            System.Func<int, int> scale = x => x * 2 + seed;
            for (int i = 0; i < 100; i++) total += scale(i) % 5;

            try {
                if (seed % 17 == 0) throw new System.InvalidOperationException("periodic");
            } catch (System.InvalidOperationException) {
                total++;
            } finally {
                total++;
            }
            return total;
        }
    }
)";

}  // namespace

TEST(heapPlateaus)
{
    script::Context ctx;
    script::Assembly assembly = ctx.compile(kWorkload, "<soak>");

    int checksum = 0;
    std::uint64_t settled = 0;
    std::uint64_t settledPool = 0;
    for (int round = 0; round < kRounds; ++round) {
        checksum += ctx.call(assembly, "Work.Round", {script::Value(round)}).toInt();
        if (round == kRounds / 4) {
            ctx.collectGarbage();
            settled = ctx.stats().heapBytes;
            settledPool = ctx.stats().reservedBytes;
        }
    }
    ctx.collectGarbage();
    const script::Context::Stats stats = ctx.stats();

    CHECK(checksum != 0);
    if (!kCheckPlateau)
        return;
    // Three quarters of the run later, nothing should have grown much: the
    // cells come back to the pool and the pool hands them out again.
    CHECK(settled > 0);
    CHECK(stats.heapBytes < settled * 2 + (1u << 20));
    CHECK(stats.reservedBytes < settledPool * 2 + (1u << 20));
    CHECK(stats.gcRuns > 0);
    CHECK(stats.gcFreed > 0);
}

TEST(compilingRepeatedlyLeavesNothingBehind)
{
    script::Context ctx;
    const int rounds = kCheckPlateau ? 200 : 10;
    std::uint64_t settled = 0;
    for (int i = 0; i < rounds; ++i) {
        // A fresh assembly every time, immediately dropped. The arena the
        // checker used goes away with it; the heap should not notice.
        script::Assembly assembly = ctx.compile(
            "static class M { public static int F(int n) { return n * 2; } } return M.F(21);",
            "<repeat>");
        CHECK_EQ(ctx.run(assembly).toInt(), 42);
        if (i == rounds / 4) {
            ctx.collectGarbage();
            settled = ctx.stats().heapBytes;
        }
    }
    ctx.collectGarbage();
    if (kCheckPlateau)
        CHECK(ctx.stats().heapBytes < settled * 3 + (1u << 20));
}

TEST(manyContextsInSequence)
{
    const int rounds = kCheckPlateau ? 200 : 10;
    for (int i = 0; i < rounds; ++i) {
        script::Context ctx;
        CHECK_EQ(ctx.eval("int t = 0; for (int i = 0; i < 100; i++) t += i; return t;").toInt(),
                 4950);
    }
}

TEST(deepRecursionUnwindsCleanly)
{
    script::Context ctx;
    script::Assembly assembly = ctx.compile(R"(
        static class M {
            public static int Down(int n) {
                if (n == 0) throw new System.InvalidOperationException("bottom");
                try { return Down(n - 1); } finally { Sink.Note(); }
            }
        }
        static class Sink { public static int Count = 0; public static void Note() { Count++; } }
    )",
                                        "<deep>");
    for (int i = 0; i < (kCheckPlateau ? 200 : 10); ++i) {
        bool caught = false;
        try {
            ctx.call(assembly, "M.Down", {script::Value(60)});
        } catch (const script::Exception&) {
            caught = true;
        }
        CHECK(caught);
    }
    ctx.collectGarbage();
    CHECK(ctx.stats().heapBytes < (8u << 20));
}
