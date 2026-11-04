// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The README's first example: one host API, the same job in all six languages, and a wrong call caught before it runs.
#include <cstdio>
#include <string>
#include <vector>

#include "script/script.hpp"

int main()
{
    script::Context ctx;

    // One host API, bound once. Every language sees it, and the typed ones check calls against these C++ signatures.
    const std::vector<std::string> words = {"the", "heap", "stays", "flat", "and", "the", "heap", "stays", "the", "same"};
    ctx.hostClass("Host")
        .method("Count", [&] { return int(words.size()); })
        .method("Word", [&](int i) { return words.at(std::size_t(i)); })
        .method("Report", [](std::string language, std::string word, int count) {
            std::printf("%-11s most common word: %-6s x%d\n", language.c_str(), word.c_str(), count);
        });

    // The same job in each language's own idiom: find the most common word. The file name picks the language.
    struct Program { const char* file; const char* source; };
    const Program programs[] = {
        {"top.cs", R"(
            using System.Collections.Generic;
            var counts = new Dictionary<string, int>();
            string best = "";
            for (int i = 0; i < Host.Count(); i++) {
                string w = Host.Word(i);
                counts[w] = counts.ContainsKey(w) ? counts[w] + 1 : 1;
                if (best == "" || counts[w] > counts[best]) best = w;
            }
            Host.Report("C#", best, counts[best]);
        )"},
        {"top.ts", R"(
            const counts = new Map<string, number>();
            for (let i = 0; i < Host.Count(); i++) {
                const w: string = Host.Word(i);
                counts.set(w, (counts.get(w) ?? 0) + 1);
            }
            const [best, n] = [...counts].reduce((a, b) => (b[1] > a[1] ? b : a));
            Host.Report("TypeScript", best, n);
        )"},
        {"top.js", R"(
            const words = Array.from({ length: Host.Count() }, (_, i) => Host.Word(i));
            const counts = words.reduce((c, w) => ({ ...c, [w]: (c[w] || 0) + 1 }), {});
            const [best, n] = Object.entries(counts).sort((a, b) => b[1] - a[1])[0];
            Host.Report("JavaScript", best, n);
        )"},
        {"top.py", R"(
counts = {}
for i in range(Host.Count()):
    w = Host.Word(i)
    counts[w] = counts.get(w, 0) + 1
best = max(counts, key=lambda w: counts[w])
Host.Report("Python", best, counts[best])
)"},
        {"top.c", R"(
            #include <string.h>
            int main() {
                int n = Host.Count(), bestCount = 0;
                const char* best = "";
                for (int i = 0; i < n; i++) {
                    int count = 0;
                    for (int j = 0; j < n; j++)
                        if (strcmp(Host.Word(i), Host.Word(j)) == 0) count++;
                    if (count > bestCount) { bestCount = count; best = Host.Word(i); }
                }
                Host.Report("C", best, bestCount);
                return 0;
            }
        )"},
        {"top.cpp", R"(
            #include <map>
            #include <string>
            int main() {
                std::map<std::string, int> counts;
                for (int i = 0; i < Host::Count(); i++)
                    counts[Host::Word(i)]++;
                std::string best;
                int bestCount = 0;
                for (auto& entry : counts)
                    if (entry.second > bestCount) { best = entry.first; bestCount = entry.second; }
                Host::Report("C++", best, bestCount);
                return 0;
            }
        )"},
    };

    for (const Program& program : programs) {
        script::Assembly assembly = ctx.compile(program.source, program.file);  // compile once ...
        ctx.run(assembly);                                                      // ... run as often as you like
    }

    // A wrong call never runs: where the language has types, the C++ signature rejects it at compile time.
    for (const char* file : {"bad.cs", "bad.ts"})
        for (const script::Diagnostic& d : ctx.diagnose("Host.Report(\"x\", 42, 1);", file))
            std::puts(d.toString().c_str());
}
