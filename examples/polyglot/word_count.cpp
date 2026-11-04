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
    const std::vector<std::string> words = {"the", "heap", "stays", "flat", "and", "the", "heap", "stays", "the", "same"};

    // Bound once. Every language sees it, and the typed ones check calls against these C++ signatures.
    ctx.hostClass("Host")
        .method("Count", [&] { return int(words.size()); })
        .method("Word", [&](int i) { return words.at(std::size_t(i)); })
        .method("Report", [](std::string who, std::string word, int n) { std::printf("%-10s %s x%d\n", who.c_str(), word.c_str(), n); });

    // One job in each language's own idiom: the most common word. The file name picks the language.
    const char* const programs[][2] = {
        {"top.cs", R"(using System.Collections.Generic;
            var seen = new Dictionary<string, int>();
            for (int i = 0; i < Host.Count(); i++) { var w = Host.Word(i); seen[w] = seen.ContainsKey(w) ? seen[w] + 1 : 1; }
            var top = ("", 0);
            foreach (var (w, n) in seen) if (n > top.Item2) top = (w, n);
            Host.Report("C#", top.Item1, top.Item2);)"},
        {"top.ts", R"(const seen = new Map<string, number>();
            for (let i = 0; i < Host.Count(); i++) seen.set(Host.Word(i), (seen.get(Host.Word(i)) ?? 0) + 1);
            const [w, n] = [...seen].reduce((a, b) => (b[1] > a[1] ? b : a));
            Host.Report("TypeScript", w, n);)"},
        {"top.js", R"(const seen = Array.from({ length: Host.Count() }, (_, i) => Host.Word(i)).reduce((c, w) => ({ ...c, [w]: (c[w] || 0) + 1 }), {});
            const [w, n] = Object.entries(seen).sort((a, b) => b[1] - a[1])[0];
            Host.Report("JavaScript", w, n);)"},
        {"top.py", R"(words = [Host.Word(i) for i in range(Host.Count())]
top = max(words, key=words.count)
Host.Report("Python", top, words.count(top)))"},
        {"top.c", R"(#include <string.h>
            int main() { int top = 0, most = 0, size = Host.Count();
                for (int i = 0, n = 0; i < size; i++, n = 0) {
                    for (int j = 0; j < size; j++) n += strcmp(Host.Word(i), Host.Word(j)) == 0;
                    if (n > most) { most = n; top = i; } }
                Host.Report("C", Host.Word(top), most); })"},
        {"top.cpp", R"(#include <map>
            #include <string>
            int main() { std::map<std::string, int> seen; std::string top; int most = 0;
                for (int i = 0; i < Host::Count(); i++) seen[Host::Word(i)]++;
                for (auto& e : seen) if (e.second > most) { top = e.first; most = e.second; }
                Host::Report("C++", top, most); })"},
    };
    for (const auto& program : programs)
        ctx.eval(program[1], program[0]);

    // A wrong call never runs.
    for (const script::Diagnostic& d : ctx.diagnose("Host.Report(\"x\", 42, 1);", "bad.ts"))
        std::puts(d.toString().c_str());
}
