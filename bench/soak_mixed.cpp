// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The long run itself: one context per thread, each language churning objects, strings, closures and containers, with
// bursts every so often and programs compiled from source all along. The process's resident size and the engines' own
// gauges are sampled throughout and must end flat. SCRIPT_SOAK_SECONDS sets the length.
//
//   SCRIPT_SOAK_SECONDS=86400 bench_soak_mixed
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "bench/soak.hpp"
#include "script/script.hpp"

namespace {

using script::Context;
using script::Language;
using script::Value;

const char* kJavaScript = R"((function (seed, burst) {
    var size = burst ? 20000 : 200 + seed % 300;
    var list = [];
    for (var i = 0; i < size; i++)
        list.push({ value: seed + i, name: "node number " + (seed + i), next: list[i - 1], tags: [i, i + 1] });
    list[0].next = list[size - 1];
    var index = new Map();
    for (var j = 0; j < size; j += 3) index.set("key " + (seed + j), list[j]);
    var total = 0;
    index.forEach(function (node) { total += node.value; });
    var fresh = {};
    for (var k = 0; k < 50; k++) fresh["name" + (seed + k)] = k;     // property names nobody will spell again
    var text = "";
    for (var n = 0; n < 40; n++) text += String(n * 0.25) + ",";
    return total + text.split(",").sort().join("|").length + JSON.stringify(fresh).length;
}))";

const char* kPython = R"(
def round_(seed, burst):
    size = 20000 if burst else 200 + seed % 300
    items = []
    for i in range(size):
        items.append({"value": seed + i, "name": "node number " + str(seed + i)})
    index = {}
    for j in range(0, size, 3):
        index["key " + str(seed + j)] = items[j]
    add = lambda x: (lambda y: x + y)
    total = 0
    for m in range(100):
        total = add(total)(m) % 1000003
    return total + len(index)
round_
)";

const char* kCSharp = R"(
using System.Collections.Generic;
using System.Text;
class Node {
    public int Value; public string Name; public Node Next; public Node Peer;
    public Node(int v) { Value = v; Name = "node number " + v; }
}
static class Work {
    public static int Round(int seed, bool burst) {
        int size = burst ? 20000 : 200 + seed % 300;
        var list = new List<Node>();
        for (int i = 0; i < size; i++) {
            var n = new Node(seed + i);
            if (i > 0) { n.Next = list[i - 1]; list[i - 1].Peer = n; }
            list.Add(n);
        }
        var index = new Dictionary<string, int>();
        for (int i = 0; i < size; i += 3) index["key " + (seed + i)] = i;
        var sb = new StringBuilder();
        for (int i = 0; i < 40; i++) sb.Append(i * 0.25).Append(',');
        var data = new double[256 + seed % 4096];
        return index.Count + sb.ToString().Length + data.Length;
    }
}
)";

constexpr std::uint64_t kBurstEvery = 256;

std::uint64_t soakJavaScript(Context& ctx, const std::atomic<bool>& stop)
{
    Value fn = ctx.eval(kJavaScript, "<soak>", Language::JavaScript);
    std::uint64_t rounds = 0;
    while (!stop.load(std::memory_order_relaxed)) {
        (void)fn.call({Value(double(rounds)), Value(rounds % kBurstEvery == 0)});
        ++rounds;
    }
    return rounds;
}

std::uint64_t soakPython(Context& ctx, const std::atomic<bool>& stop)
{
    Value fn = ctx.eval(kPython, "<soak>", Language::Python);
    std::uint64_t rounds = 0;
    while (!stop.load(std::memory_order_relaxed)) {
        (void)fn.call({Value(double(rounds)), Value(rounds % kBurstEvery == 0)});
        ++rounds;
    }
    return rounds;
}

std::uint64_t soakCSharp(Context& ctx, const std::atomic<bool>& stop)
{
    script::Assembly assembly = ctx.compile(kCSharp, "<soak>.cs", Language::CSharp);
    std::uint64_t rounds = 0;
    while (!stop.load(std::memory_order_relaxed)) {
        (void)ctx.call(assembly, "Work.Round", {Value(std::int32_t(rounds % 100000)), Value(rounds % kBurstEvery == 0)});
        ++rounds;
    }
    return rounds;
}

// A host that compiles what it is sent: every program is new, in every language.
std::uint64_t soakCompile(Context& ctx, const std::atomic<bool>& stop)
{
    std::uint64_t rounds = 0;
    while (!stop.load(std::memory_order_relaxed)) {
        const std::string n = std::to_string(rounds);
        // Top-level code only: a global `function` or a class would stay, as it is meant to.
        (void)ctx.eval("(function (a) { return { v" + n + ": a * " + n + " }; })(2).v" + n, "<soak>", Language::JavaScript);
        (void)ctx.eval("let x: number = " + n + "; x + 1", "<soak>", Language::TypeScript);
        (void)ctx.eval("def g(a):\n    return a + " + n + "\ng(1)\n", "<soak>", Language::Python);
        (void)ctx.eval("return " + n + " + 1;", "<soak>.cs", Language::CSharp);
        (void)ctx.eval("int main() { return " + n + "; }", "<soak>.c", Language::C);
        ++rounds;
    }
    return rounds;
}

std::uint64_t reserved(const std::vector<std::unique_ptr<Context>>& contexts)
{
    std::uint64_t total = 0;
    for (const auto& ctx : contexts)
        total += ctx->stats().reservedBytes;
    return total;
}

}  // namespace

int main()
{
    long seconds = 60;
    if (const char* text = std::getenv("SCRIPT_SOAK_SECONDS"))
        seconds = std::atol(text);

    std::vector<std::unique_ptr<Context>> contexts;
    for (int i = 0; i < 4; ++i)
        contexts.push_back(std::make_unique<Context>());

    bench::SoakOptions options;
    options.duration = std::chrono::seconds(seconds);
    options.sampleEvery = std::chrono::milliseconds(seconds > 600 ? 5000 : 500);
    options.threads = 4;
    options.gauges.emplace_back("reserved", [&] { return reserved(contexts); });
    options.gauges.emplace_back("heapBytes", [&] {
        std::uint64_t total = 0;
        for (const auto& ctx : contexts)
            total += ctx->stats().heapBytes;
        return total;
    });

    const bench::SoakResult result = bench::runSoak(options, [&](unsigned thread, const std::atomic<bool>& stop) {
        Context& ctx = *contexts[thread];
        switch (thread) {
            case 0: return soakJavaScript(ctx, stop);
            case 1: return soakPython(ctx, stop);
            case 2: return soakCSharp(ctx, stop);
            default: return soakCompile(ctx, stop);
        }
    });
    std::puts(result.report().c_str());

    // The resident set decides. Bursts on several threads overlap at random, so single samples scatter by more than a
    // leak moves them in a short run; the median of the last third against the middle third does not.
    const std::vector<double>& samples = result.series[0].samples;
    const std::size_t third = samples.size() / 3;
    const auto median = [](std::vector<double> part) {
        std::nth_element(part.begin(), part.begin() + std::ptrdiff_t(part.size() / 2), part.end());
        return part[part.size() / 2];
    };
    const double middle = median({samples.begin() + std::ptrdiff_t(third), samples.begin() + std::ptrdiff_t(2 * third)});
    const double last = median({samples.begin() + std::ptrdiff_t(2 * third), samples.end()});
    const double growth = (last - middle) / middle * 100;
    std::printf("resident median: %.1f MiB, then %.1f MiB (%+.1f %%)\n", middle / 1048576, last / 1048576, growth);
    if (growth > 5.0) {
        std::puts("FAILED: the resident size is still growing");
        return 1;
    }
    return 0;
}
