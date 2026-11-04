// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The five-minute tour: run a script, read a result, expose a function.
//
//   ./quickstart

#include <cstdio>

#include "script/script.hpp"

namespace {

/// Every snippet in this example is JavaScript, so the context says so once
/// instead of every `eval` naming a file.
script::Context::Options languageOptions()
{
    script::Context::Options options;
    options.language = script::Language::JavaScript;
    return options;
}

}  // namespace

int main()
{
    script::Context ctx(languageOptions());

    // 1. Run some script and take the completion value.
    const double answer = ctx.eval("6 * 7").toDouble();
    std::printf("6 * 7 = %g\n", answer);

    // 2. Expose a C++ function. Arguments and results convert automatically.
    ctx.global().set("slugify", ctx.fn("slugify", [](std::string text) {
        for (char& c : text)
            c = (c == ' ') ? '-' : char(std::tolower(static_cast<unsigned char>(c)));
        return text;
    }));
    std::printf("%s\n", ctx.eval("slugify('Hello Embedded World')").toString().c_str());

    // 3. Script state persists between evaluations, so a REPL just works.
    ctx.eval("const inventory = new Map([['apples', 3], ['pears', 0]]);");
    ctx.eval("function inStock() { return [...inventory].filter(([, n]) => n > 0).map(([k]) => k); }");
    std::printf("in stock: %s\n", ctx.eval("inStock().join(', ')").toString().c_str());

    // 4. Call back into script from C++.
    script::Value formatter = ctx.eval("({ money: n => `$${n.toFixed(2)}` })");
    std::printf("%s\n", formatter.invoke("money", {script::Value(12.5)}).toString().c_str());

    // 5. Errors cross the boundary with their value and stack intact.
    try {
        ctx.eval("function inner() { throw new RangeError('too far'); }\ninner();", "demo.js");
    } catch (const script::Exception& e) {
        std::printf("caught %s\n%s", e.what(), e.stack().c_str());
    }

    const script::Context::Stats stats = ctx.stats();
    std::printf("\n%llu live objects, %llu strings, %llu bytes\n",
                (unsigned long long)stats.liveObjects, (unsigned long long)stats.liveStrings,
                (unsigned long long)stats.heapBytes);
    return 0;
}
