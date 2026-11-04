// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// What a long run costs. Each workload runs a warm-up, then a timed stretch, and reports the time per round, the calls
// to the process heap per round (the number that decides whether the general allocator fragments over weeks), and how
// the resident set and the engine's own counters moved during the stretch.
//
//   bench_longrun [rounds]
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

#include "bench/alloc_budget.hpp"
#include "sys/proc.hpp"
#if __has_include("script/script.hpp")
#include "script/script.hpp"
#else
#include "scriptx/scriptx.hpp"
namespace script = scriptx;
#endif

namespace {

const char* kJavaScript = R"(
(function round(seed) {
    var total = 0;
    var list = [];
    for (var i = 0; i < 200; i++) {
        var node = { value: seed + i, name: "node" + (seed + i), next: list[i - 1] };
        list.push(node);
    }
    list[0].next = list[199];                         // a cycle, for the collector
    var index = new Map();
    for (var j = 0; j < 200; j++) index.set("key " + j, list[j]);
    for (var k = 0; k < 200; k++) total += index.get("key " + k).value;
    var add = function (x) { return function (y) { return x + y; }; };
    for (var m = 0; m < 200; m++) total = add(total)(m) % 1000003;
    var text = "";
    for (var n = 0; n < 50; n++) text += String(n * 0.5) + ",";
    return total + text.split(",").length + JSON.stringify({ a: [1, 2, 3], b: "x" }).length;
}))";

const char* kPython = R"(
def round_(seed):
    total = 0
    items = []
    for i in range(200):
        items.append({"value": seed + i, "name": "node" + str(seed + i)})
    index = {}
    for j in range(200):
        index["key " + str(j)] = items[j]
    for k in range(200):
        total += index["key " + str(k)]["value"]
    add = lambda x: (lambda y: x + y)
    for m in range(200):
        total = add(total)(m) % 1000003
    return total
round_
)";

const char* kCSharp = R"(
using System.Collections.Generic;
using System.Text;
class Node {
    public int Value; public string Name; public Node Next; public Node Peer;
    public Node(int v) { Value = v; Name = "node" + v; }
}
static class Work {
    public static int Round(int seed) {
        var list = new List<Node>();
        for (int i = 0; i < 200; i++) {
            var n = new Node(seed + i);
            if (i > 0) { n.Next = list[i - 1]; list[i - 1].Peer = n; }
            list.Add(n);
        }
        var index = new Dictionary<string, int>();
        for (int i = 0; i < 200; i++) index["key " + i] = i;
        int total = 0;
        for (int i = 0; i < 200; i++) total += index["key " + i] + list[i].Value;
        var sb = new StringBuilder();
        for (int i = 0; i < 50; i++) sb.Append(i * 0.5).Append(',');
        var data = new int[64];
        for (int i = 0; i < data.Length; i++) data[i] = i * seed;
        return total + sb.ToString().Length + data[seed & 63];
    }
}
)";

struct Row {
    const char* name;
    double nsPerRound;
    double heapCallsPerRound;
    long long residentDelta;
    long long reservedDelta;
    long long heapDelta;
    std::uint64_t resident;
};
std::vector<Row> rows;

void measure(const char* name, script::Context& ctx, int rounds, const std::function<void(int)>& round) {
    for (int i = 0; i < 200; ++i)
        round(i);
    ctx.collectGarbage();
    const auto before = ctx.stats();
    const std::size_t rssBefore = sys::residentBytes();
    const auto start = std::chrono::steady_clock::now();
    unsigned long long calls = 0;
    {
        bench::AllocScope scope;
        for (int i = 0; i < rounds; ++i)
            round(i + 200);
        calls = scope.calls();
    }
    const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count();
    ctx.collectGarbage();
    const auto after = ctx.stats();
    const std::size_t rssAfter = sys::residentBytes();
    rows.push_back({name, ns / rounds, double(calls) / rounds, (long long)rssAfter - (long long)rssBefore,
                    (long long)after.reservedBytes - (long long)before.reservedBytes,
                    (long long)after.heapBytes - (long long)before.heapBytes, rssAfter});
}

}  // namespace

int main(int argc, char** argv) {
    const int rounds = argc > 1 ? std::atoi(argv[1]) : 2000;
    {
        script::Context ctx;
        script::Value fn = ctx.eval(kJavaScript, "<bench>", script::Language::JavaScript);
        measure("javascript", ctx, rounds, [&](int i) { (void)fn.call({script::Value(double(i))}); });
    }
    {
        script::Context ctx;
        script::Value fn = ctx.eval(kPython, "<bench>", script::Language::Python);
        measure("python", ctx, rounds, [&](int i) { (void)fn.call({script::Value(double(i))}); });
    }
    {
        script::Context ctx;
        script::Assembly assembly = ctx.compile(kCSharp, "<bench>.cs", script::Language::CSharp);
        measure("csharp", ctx, rounds, [&](int i) { (void)ctx.call(assembly, "Work.Round", {script::Value(i)}); });
    }
    {
        script::Context::Options options;
        options.jit.mode = script::JitOptions::Mode::Tiered;
        script::Context ctx(options);
        script::Assembly assembly = ctx.compile(kCSharp, "<bench>.cs", script::Language::CSharp);
        measure("csharp-jit", ctx, rounds, [&](int i) { (void)ctx.call(assembly, "Work.Round", {script::Value(i)}); });
    }
    {
        // The host that compiles what it is sent: a new program every round.
        script::Context ctx;
        measure("js-compile", ctx, rounds / 2, [&](int i) {
            const std::string source = "var r = 0; for (var i = 0; i < 10; i++) { r += i * " + std::to_string(i) + "; } r";
            (void)ctx.eval(source, "<bench>", script::Language::JavaScript);
        });
    }
    {
        script::Context ctx;
        measure("cs-compile", ctx, rounds / 20, [&](int i) {
            const std::string source = "static class P { static int Main() { return " + std::to_string(i) + "; } }";
            (void)ctx.eval(source, "<bench>.cs", script::Language::CSharp);
        });
    }
    {
        script::Context ctx;
        script::Value fn = ctx.eval(R"((function (seed) {
            var n = 0;
            for (var i = 0; i < 1000; i++) n += ("" + (seed + i * 0.125)).length;
            return n;
        }))", "<bench>", script::Language::JavaScript);
        measure("js-num2str", ctx, rounds / 10, [&](int i) { (void)fn.call({script::Value(double(i))}); });
    }
    {
        script::Context ctx;
        script::Value fn = ctx.eval(R"((function (seed) {
            var parts = [];
            for (var i = 0; i < 1000; i++) parts.push("a rather long string number " + (i + seed));
            return parts.join("|").length;
        }))", "<bench>", script::Language::JavaScript);
        measure("js-strings", ctx, rounds / 10, [&](int i) { (void)fn.call({script::Value(double(i))}); });
    }
    std::printf("%-12s %12s %11s %10s %10s %10s %9s\n", "workload", "us/round", "new/round", "drss(KiB)", "dresv(KiB)",
                "dheap(KiB)", "rss(MiB)");
    for (const Row& r : rows)
        std::printf("%-12s %12.2f %11.1f %10lld %10lld %10lld %9.1f\n", r.name, r.nsPerRound / 1000, r.heapCallsPerRound,
                    r.residentDelta / 1024, r.reservedDelta / 1024, r.heapDelta / 1024, r.resident / 1048576.0);
    return 0;
}
