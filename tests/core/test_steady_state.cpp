// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The contract that keeps the process heap from shredding over months: once a workload has warmed up, running it again
// makes no call to the general allocator at all, in any language. Cells come from the context's heap and buffers from
// the payload pool, and neither of those is malloc.
#include <vector>

#include "bench/alloc_budget.hpp"
#include "script/script.hpp"
#include "tests/test_util.hpp"

namespace {

using script::Context;
using script::Language;
using script::Value;

constexpr int kWarmup = 64;
constexpr int kRounds = 64;

template <class Round>
void expectNoHeapCalls(Context& ctx, Round round, int warmup = kWarmup)
{
    for (int i = 0; i < warmup; ++i)
        round();
    ctx.collectGarbage();
    for (int i = 0; i < kWarmup; ++i)
        round();
    bench::AllocScope scope;
    for (int i = 0; i < kRounds; ++i)
        round();
    const unsigned long long calls = scope.calls();
    if (calls != 0)
        ::tst::fail(__FILE__, __LINE__, "heap calls per round: " + std::to_string(double(calls) / kRounds));
}

const char* kDynamicBody = R"(
    var total = 0;
    var list = [];
    for (var i = 0; i < 100; i++) {
        var node = { value: seed + i, name: "a node with a long name " + (seed + i), next: list[i - 1] };
        list.push(node);
    }
    list[0].next = list[99];
    var index = new Map();
    for (var j = 0; j < 100; j++) index.set("key " + j, list[j]);
    for (var k = 0; k < 100; k++) total += index.get("key " + k).value;
    var add = function (x) { return function (y) { return x + y; }; };
    for (var m = 0; m < 100; m++) total = add(total)(m) % 1000003;
    var text = "";
    for (var n = 0; n < 50; n++) text += String(n * 0.5) + ",";
    var words = text.split(",");
    words.sort();
    return total + words.join("|").length + JSON.stringify({ a: [1, 2, 3], b: "x" }).length;
)";

}  // namespace

#if SCRIPT_LANG_JAVASCRIPT
TEST(javascriptMakesNoHeapCalls)
{
    Context ctx;
    Value fn = ctx.eval("(function (seed) {" + std::string(kDynamicBody) + "})", "<test>", Language::JavaScript);
    const std::vector<Value> args{Value(7.0)};
    expectNoHeapCalls(ctx, [&] { (void)fn.call(args); });
}
#endif

#if SCRIPT_LANG_TYPESCRIPT
TEST(typescriptMakesNoHeapCalls)
{
    Context ctx;
    Value fn = ctx.eval("(function (seed: number): number {" + std::string(kDynamicBody) + "})", "<test>",
                        Language::TypeScript);
    const std::vector<Value> args{Value(7.0)};
    expectNoHeapCalls(ctx, [&] { (void)fn.call(args); });
}
#endif

#if SCRIPT_LANG_PYTHON
TEST(pythonMakesNoHeapCalls)
{
    Context ctx;
    Value fn = ctx.eval(R"(
def round_(seed):
    total = 0
    items = []
    for i in range(100):
        items.append({"value": seed + i, "name": "a node with a long name " + str(seed + i)})
    index = {}
    for j in range(100):
        index["key " + str(j)] = items[j]
    for k in range(100):
        total += index["key " + str(k)]["value"]
    add = lambda x: (lambda y: x + y)
    for m in range(100):
        total = add(total)(m) % 1000003
    return total
round_
)", "<test>", Language::Python);
    const std::vector<Value> args{Value(7.0)};
    expectNoHeapCalls(ctx, [&] { (void)fn.call(args); });
}
#endif

#if SCRIPT_LANG_CSHARP
namespace {
const char* kCSharp = R"(
    using System.Collections.Generic;
    using System.Text;
    class Node {
        public int Value; public string Name; public Node Next; public Node Peer;
        public Node(int v) { Value = v; Name = "a node with a long name " + v; }
    }
    static class Work {
        static int Square(int x) { return x * x; }
        public static int Round(int seed) {
            var list = new List<Node>();
            for (int i = 0; i < 100; i++) {
                var n = new Node(seed + i);
                if (i > 0) { n.Next = list[i - 1]; list[i - 1].Peer = n; }
                list.Add(n);
            }
            var index = new Dictionary<string, int>();
            for (int i = 0; i < 100; i++) index["key " + i] = i;
            int total = 0;
            for (int i = 0; i < 100; i++) total += index["key " + i] + list[i].Value;
            var sb = new StringBuilder();
            for (int i = 0; i < 50; i++) sb.Append(i * 0.5).Append(',');
            var data = new int[64];
            for (int i = 0; i < data.Length; i++) data[i] = i * seed;
            System.Func<int, int> twice = x => x * 2 + seed;
            for (int i = 0; i < 100; i++) total = (twice(total) + Square(i)) % 1000003;
            return total + sb.ToString().Length + data[seed & 63];
        }
    }
)";
}  // namespace

TEST(csharpMakesNoHeapCalls)
{
    Context ctx;
    script::Assembly assembly = ctx.compile(kCSharp, "<test>.cs", Language::CSharp);
    const std::vector<Value> args{Value(7)};
    expectNoHeapCalls(ctx, [&] { (void)ctx.call(assembly, "Work.Round", args); });
}

TEST(jittedCsharpMakesNoHeapCalls)
{
    Context::Options options;
    options.jit.mode = script::JitOptions::Mode::Tiered;
    options.jit.threshold = 10;  // compiling allocates, and only once: have it done within the warm-up
    Context ctx(options);
    script::Assembly assembly = ctx.compile(kCSharp, "<test>.cs", Language::CSharp);
    const std::vector<Value> args{Value(7)};
    expectNoHeapCalls(ctx, [&] { (void)ctx.call(assembly, "Work.Round", args); });
    CHECK(ctx.stats().methodsCompiled > 0 || std::string(ctx.jitBackend()) == "interpreter");
}
#endif

TEST(hostStringsStayOutOfTheHeapToo)
{
    Context ctx;
    expectNoHeapCalls(ctx, [&] {
        for (int i = 0; i < 100; ++i)
            (void)ctx.str("a host string long enough to be heap allocated by a std::string or two");
    });
}
