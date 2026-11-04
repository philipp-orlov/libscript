// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// A long run: heavy allocation, varied shapes, and a heap that settles.
//
// The engine is built for processes that stay up for weeks, which means the
// interesting number is not how fast the first round is but whether the
// thousandth costs the same memory as the tenth. This example runs a workload
// that allocates hard -- objects, cycles, strings, lists, dictionaries,
// closures, arrays, exceptions -- and prints the heap, the pool and the
// process's own resident size as it goes.
#include <chrono>
#include <cstdio>
#include <string>

#include "script/script.hpp"
#include "sys/proc.hpp"

#if defined(__linux__)
#include <unistd.h>
#endif

namespace {

/// What the operating system thinks the process is using. The engine's own
/// accounting is only convincing next to this.
std::size_t residentBytes()
{
    return sys::residentBytes();
}

const char* kWorkload = R"CS(
    using System.Collections.Generic;
    using System.Text;

    class Node {
        public int Value;
        public Node Next;
        public Node Peer;     // makes cycles on purpose
        public string Label;
        public Node(int v) {
            Value = v;
            Label = "node-" + v;
        }
    }

    class Sensor {
        public string Name { get; set; }
        public List<double> Samples { get; set; }
        public Sensor(string name) {
            Name = name;
            Samples = new List<double>();
        }
        public double Mean() {
            if (Samples.Count == 0) return 0;
            double total = 0;
            foreach (double s in Samples) total += s;
            return total / Samples.Count;
        }
    }

    static class Work {
        // Objects that die immediately: reference counting should reclaim
        // these without the collector ever being involved.
        public static int Churn(int seed) {
            int live = 0;
            for (int i = 0; i < 400; i++) {
                var n = new Node(seed + i);
                if ((i & 7) == 0) live += n.Label.Length;
            }
            return live;
        }

        // Objects that point at each other: only the cycle collector can get
        // these back.
        public static int Cycles(int seed) {
            var head = new Node(seed);
            var current = head;
            for (int i = 1; i < 200; i++) {
                var n = new Node(seed + i);
                current.Next = n;
                n.Peer = current;
                current = n;
            }
            current.Next = head;
            return current.Value;
        }

        // Strings, dictionaries, lists and a builder: the shapes a real
        // program spends its allocation on.
        public static int Collections(int seed) {
            var sensors = new List<Sensor>();
            var index = new Dictionary<string, Sensor>();
            for (int i = 0; i < 40; i++) {
                var s = new Sensor("sensor-" + (seed + i));
                for (int k = 0; k < 25; k++) s.Samples.Add((seed + k) * 0.125);
                sensors.Add(s);
                index[s.Name] = s;
            }

            var report = new StringBuilder();
            double total = 0;
            foreach (Sensor s in sensors) {
                report.Append(s.Name);
                report.Append("=");
                report.Append(s.Mean().ToString("F2"));
                report.Append(";");
                total += s.Mean();
            }
            foreach (string key in index.Keys) total += index[key].Samples.Count;
            return report.ToString().Length + (int)total;
        }

        // Closures capture variables into heap boxes, and delegates keep them
        // alive until the delegate itself dies.
        public static int Closures(int seed) {
            var actions = new List<System.Func<int, int>>();
            for (int i = 0; i < 50; i++) {
                int captured = seed + i;
                actions.Add(x => x * 2 + captured);
            }
            int total = 0;
            foreach (System.Func<int, int> f in actions) total += f(1) % 17;
            return total;
        }

        // Arrays of primitives, which store raw elements rather than Values.
        public static int Arrays(int seed) {
            int[] numbers = new int[4096];
            for (int i = 0; i < numbers.Length; i++) numbers[i] = (seed + i) % 251;
            double[] scaled = new double[1024];
            for (int i = 0; i < scaled.Length; i++) scaled[i] = numbers[i] * 0.5;
            int total = 0;
            foreach (int n in numbers) total += n & 3;
            return total + (int)scaled[7];
        }

        // Exceptions allocate an object and unwind through a finally.
        public static int Failures(int seed) {
            int handled = 0;
            for (int i = 0; i < 20; i++) {
                try {
                    if ((seed + i) % 3 == 0) throw new System.InvalidOperationException("round " + i);
                    handled += 1;
                } catch (System.InvalidOperationException e) {
                    handled += e.Message.Length % 5;
                } finally {
                    handled += 1;
                }
            }
            return handled;
        }

        public static int Round(int seed) {
            return Churn(seed) + Cycles(seed) + Collections(seed) + Closures(seed) +
                   Arrays(seed) + Failures(seed);
        }
    }
)CS";

}  // namespace

int main(int argc, char** argv)
{
    const int rounds = argc > 1 ? std::atoi(argv[1]) : 2000;

    script::Context::Options options;
    options.gcThreshold = 2u << 20;
    script::Context ctx(options);
    script::Assembly assembly = ctx.compile(kWorkload, "workload.cs");

    std::printf("round      heap KiB   pooled KiB   objects   strings   collections   RSS KiB\n");
    std::printf("--------------------------------------------------------------------------\n");

    const auto start = std::chrono::steady_clock::now();
    long long checksum = 0;
    std::size_t firstResident = 0;

    for (int round = 1; round <= rounds; ++round) {
        checksum += ctx.call(assembly, "Work.Round", {script::Value(round)}).toInt();

        const bool report = round == 1 || round == 10 || round % (rounds / 10 == 0 ? 1
                                                                                   : rounds / 10) == 0;
        if (!report)
            continue;
        const script::Context::Stats stats = ctx.stats();
        const std::size_t resident = residentBytes();
        if (round == 10)
            firstResident = resident;
        std::printf("%-10d %9llu %12llu %9llu %9llu %13llu %9zu\n", round,
                    (unsigned long long)(stats.heapBytes / 1024),
                    (unsigned long long)(stats.reservedBytes / 1024),
                    (unsigned long long)stats.liveObjects,
                    (unsigned long long)stats.strings, (unsigned long long)stats.gcRuns,
                    resident / 1024);
    }

    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    const script::Context::Stats stats = ctx.stats();

    std::printf("\n%d rounds in %.2f s (%.0f rounds/s), checksum %lld\n", rounds, seconds,
                rounds / seconds, checksum);
    std::printf("allocated %.1f MiB in total, %llu bytes live at the end\n",
                double(stats.totalAllocated) / (1024 * 1024),
                (unsigned long long)stats.heapBytes);
    std::printf("the cell pool holds %llu KiB for reuse across %llu collections\n",
                (unsigned long long)(stats.reservedBytes / 1024),
                (unsigned long long)stats.gcRuns);

    // The claim, stated as a number: the heap at the end is the heap in the
    // middle. Nothing accumulates, so nothing fragments.
    ctx.collectGarbage();
    const script::Context::Stats settled = ctx.stats();
    std::printf("after a final collection: %llu KiB heap, %llu objects, %llu KiB pooled\n",
                (unsigned long long)(settled.heapBytes / 1024),
                (unsigned long long)settled.liveObjects,
                (unsigned long long)(settled.reservedBytes / 1024));
    if (firstResident)
        std::printf("resident set grew %.1f%% between round 10 and the end\n",
                    100.0 * double(residentBytes()) / double(firstResident) - 100.0);
    return 0;
}
