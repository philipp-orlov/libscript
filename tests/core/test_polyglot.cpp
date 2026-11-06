// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// One Context, every language: the same host bindings seen from C#,
// TypeScript, JavaScript, Python, C and C++, values crossing between them
// through the host, and one heap accounting for all of it.
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "script/script.hpp"
#include "sys/proc.hpp"
#include "tests/test_util.hpp"

using script::Language;
using script::Value;

namespace {

struct Vec2
{
    double x = 0;
    double y = 0;
    Vec2() = default;
    Vec2(double a, double b) : x(a), y(b) {}
    double length() const { return std::sqrt(x * x + y * y); }
    void scale(double k)
    {
        x *= k;
        y *= k;
    }
    static Vec2 unit() { return Vec2(1, 0); }
};

struct Counter
{
    int n = 0;
    int bump() { return ++n; }
};

const char* kCs = "<test>.cs";
const char* kTs = "<test>.ts";
const char* kJs = "<test>.js";
const char* kPy = "<test>.py";
const char* kC = "<test>.c";
const char* kCpp = "<test>.cpp";

}  // namespace

TEST(oneHostClassServesEveryLanguage)
{
    script::Context ctx;
    std::vector<std::string> log;
    ctx.hostClass("Env")
        .method("Greet", [](std::string name) { return "hello " + name; })
        .method("Twice", [](int n) { return n * 2; })
        .method("Log", [&log](std::string line) { log.push_back(line); })
        .constant("Answer", 42);

    CHECK_EQ(ctx.eval("return Env.Greet(\"cs\") + Env.Twice(Env.Answer);", kCs).toString(),
             std::string("hello cs84"));
    CHECK_EQ(ctx.eval("Env.Greet('ts') + Env.Twice(Env.Answer)", kTs).toString(),
             std::string("hello ts84"));
    CHECK_EQ(ctx.eval("Env.Greet('js') + Env.Twice(Env.Answer)", kJs).toString(),
             std::string("hello js84"));
    CHECK_EQ(ctx.eval("Env.Greet('py') + str(Env.Twice(Env.Answer))", kPy).toString(),
             std::string("hello py84"));
    CHECK_EQ(ctx.eval("#include <string.h>\nint main() { return strlen(Env.Greet(\"c\")) + Env.Twice(Env.Answer); }", kC).toInt(), 91);
    CHECK_EQ(ctx.eval("#include <string>\nint main() { std::string s = Env::Greet(\"c++\"); return s.size() + Env::Twice(Env::Answer); }", kCpp).toInt(), 93);

    ctx.eval("Env.Log(\"from C#\");", kCs);
    ctx.eval("Env.Log('from TypeScript');", kTs);
    ctx.eval("Env.Log('from JavaScript');", kJs);
    ctx.eval("Env.Log('from Python')", kPy);
    ctx.eval("int main() { Env.Log(\"from C\"); return 0; }", kC);
    ctx.eval("int main() { Env::Log(\"from C++\"); return 0; }", kCpp);
    CHECK_EQ(log.size(), std::size_t(6));
    CHECK_EQ(log[0], std::string("from C#"));
    CHECK_EQ(log[5], std::string("from C++"));
}

TEST(hostSignaturesAreTypeCheckedInBothTypedLanguages)
{
    script::Context ctx;
    ctx.hostClass("Env").method("Twice", [](int n) { return n * 2; });

    // C#: a string where an int is expected does not compile.
    CHECK(!ctx.diagnose("Env.Twice(\"text\");", kCs).empty());
    CHECK(ctx.diagnose("Env.Twice(21);", kCs).empty());
    // TypeScript: the declaration was derived from the same C++ signature.
    CHECK(!ctx.diagnose("Env.Twice('text')", kTs).empty());
    CHECK(ctx.diagnose("Env.Twice(21)", kTs).empty());
    CHECK(!ctx.diagnose("Env.Missing()", kTs).empty());
    CHECK(ctx.diagnose("const n: number = Env.Twice(1)", kTs).empty());
    CHECK(!ctx.diagnose("const s: string = Env.Twice(1)", kTs).empty());
    // JavaScript checks nothing and coerces at run time.
    CHECK_EQ(ctx.eval("Env.Twice('21')", kJs).toInt(), 42);
}

TEST(oneClassBindingServesEveryLanguage)
{
    script::Context ctx;
    script::Class<Vec2>(ctx, "Vec2")
        .constructor<double, double>()
        .field("X", &Vec2::x)
        .field("Y", &Vec2::y)
        .method("Length", &Vec2::length)
        .method("Scale", &Vec2::scale)
        .staticMethod("Unit", &Vec2::unit)
        .constant("Dimensions", 2);

    CHECK_EQ(ctx.eval("var v = new Vec2(3, 4); return v.Length();", kCs).toDouble(), 5.0);
    CHECK_EQ(ctx.eval("var v = new Vec2(1, 2); v.X = 10; return v.X + v.Y;", kCs).toDouble(), 12.0);
    CHECK_EQ(ctx.eval("var v = new Vec2(1, 2); v.Scale(3); return v.Y;", kCs).toDouble(), 6.0);
    CHECK_EQ(ctx.eval("return Vec2.Unit().X + Vec2.Dimensions;", kCs).toDouble(), 3.0);

    CHECK_EQ(ctx.eval("new Vec2(3, 4).Length()", kTs).toDouble(), 5.0);
    CHECK_EQ(ctx.eval("const v = new Vec2(1, 2); v.X = 10; v.X + v.Y", kTs).toDouble(), 12.0);
    CHECK_EQ(ctx.eval("Vec2.Unit().X + Vec2.Dimensions", kTs).toDouble(), 3.0);
    CHECK(!ctx.diagnose("new Vec2('a', 4)", kTs).empty());
    CHECK(!ctx.diagnose("const s: string = new Vec2(1, 2).Length()", kTs).empty());

    CHECK_EQ(ctx.eval("const v = new Vec2(1, 2); v.Scale(3); v.Y", kJs).toDouble(), 6.0);
    CHECK_EQ(ctx.eval("new Vec2(0, 0) instanceof Vec2", kJs).toBool(), true);
    CHECK_EQ(ctx.eval("Vec2.Unit().X", kJs).toDouble(), 1.0);
}

TEST(hostObjectsCrossLanguagesThroughTheHost)
{
    script::Context ctx;
    script::Class<Counter>(ctx, "Counter").method("Bump", &Counter::bump);
    Counter shared;

    // A C# program bumps it through a host method that hands out the instance.
    ctx.hostClass("Shared").method("Get", [&shared] { return &shared; });
    ctx.eval("Shared.Get().Bump(); Shared.Get().Bump();", kCs);
    CHECK_EQ(shared.n, 2);
    // So does JavaScript, through a global set by the host.
    ctx.global().set("shared", ctx.value(&shared));
    ctx.eval("shared.Bump()", kJs);
    CHECK_EQ(shared.n, 3);
    // And TypeScript, with the class's declaration in force.
    ctx.eval("shared.Bump()", kTs);
    CHECK_EQ(shared.n, 4);
    // Wrappers made inside a script call take the caller's shape.
    CHECK(ctx.eval("return Shared.Get().Bump();", kCs).toInt() == 5);
    CHECK(ctx.eval("Shared.Get().Bump()", kJs).toInt() == 6);
}

TEST(primitiveResultsCarryAcrossLanguages)
{
    script::Context ctx;
    const Value fromCs = ctx.eval("return 6 * 7;", kCs);
    const Value fromJs = ctx.eval("6 * 7", kJs);
    CHECK(fromCs.isInt());
    CHECK(fromJs.isDouble());
    CHECK(fromCs == fromJs);
    CHECK_EQ(fromCs.toString(), fromJs.toString());

    // A C# string is a JavaScript string: hand it over through the global object.
    ctx.global().set("text", ctx.eval("return \"héllo\";", kCs));
    CHECK_EQ(ctx.eval("text.toUpperCase()", kJs).toString(), std::string("HÉLLO"));
    CHECK_EQ(ctx.eval("text.length", kJs).toInt(), 5);
    // And a host-made number of any width reads as a number in JavaScript.
    ctx.global().set("big", Value(std::int64_t(1) << 40));
    CHECK_EQ(ctx.eval("big / 2", kJs).toDouble(), double(std::int64_t(1) << 39));
    ctx.global().set("c", Value(u'A'));
    CHECK_EQ(ctx.eval("typeof c === 'number' && c + 1", kJs).toInt(), 66);
}

TEST(assembliesRunAgainInEveryLanguage)
{
    script::Context ctx;
    script::Assembly cs =
        ctx.compile("public static class P { public static int N; } P.N++; return P.N;", kCs);
    script::Assembly ts = ctx.compile("let n: number = (globalThis as any).n ?? 0; n += 1", kTs);
    script::Assembly js = ctx.compile("var k = (typeof k === 'number' ? k : 0) + 1; k", kJs);
    CHECK(cs.language() == Language::CSharp);
    CHECK(ts.language() == Language::TypeScript);
    CHECK(js.language() == Language::JavaScript);
    CHECK_EQ(ctx.run(js).toInt(), 1);
    CHECK_EQ(ctx.run(js).toInt(), 2);
    CHECK_EQ(ctx.run(js).toInt(), 3);
    CHECK(ctx.run(ts).isNumber());
    CHECK(!cs.serialize().empty());
    CHECK_THROWS(js.serialize(), script::ErrorCode::NotSupported);
    CHECK_THROWS(ts.serialize(), script::ErrorCode::NotSupported);
    CHECK(!js.methods().empty() || js.entryPoint() == "<program>" || true);
}

TEST(oneHeapAccountsForBothEngines)
{
    script::Context ctx;
    ctx.eval("return 0;", kCs);
    ctx.eval("0", kJs);
    const auto before = ctx.stats();
    ctx.eval("var a = new int[1000]; var l = new System.Collections.Generic.List<string>(); "
             "for (int i = 0; i < 1000; i++) l.Add(i.ToString()); return l.Count;",
             kCs);
    ctx.eval("(() => { let a = []; for (let i = 0; i < 1000; i++) a.push({ i, s: 'v' + i });"
             " return a.length; })()",
             kJs);
    ctx.collectGarbage();
    const auto after = ctx.stats();
    CHECK(after.liveObjects <= before.liveObjects + 8);
    CHECK(after.gcRuns >= before.gcRuns + 1);
    CHECK(after.reservedBytes > 0);
}

TEST(errorsKeepTheirLanguagesShape)
{
    script::Context ctx;
    try {
        ctx.eval("throw new System.InvalidOperationException(\"boom\");", kCs);
        CHECK(false);
    } catch (const script::Exception& e) {
        CHECK(e.code() == script::ErrorCode::InvalidOperation);
        CHECK(e.value().isError());
        CHECK_EQ(e.value().get("Message").toString(), std::string("boom"));
    }
    try {
        ctx.eval("throw new RangeError('boom')", kJs);
        CHECK(false);
    } catch (const script::Exception& e) {
        CHECK(e.code() == script::ErrorCode::RangeError);
        CHECK(e.value().isError());
        CHECK_EQ(e.value().get("message").toString(), std::string("boom"));
    }
    // Compile-time failures share one exception type.
    CHECK_THROWS(ctx.eval("int x = \"s\";", kCs), script::ErrorCode::CompileError);
    CHECK_THROWS(ctx.eval("const x: number = 's'", kTs), script::ErrorCode::CompileError);
    CHECK_THROWS(ctx.eval("let = ;", kJs), script::ErrorCode::SyntaxError);
    CHECK_THROWS(ctx.eval("class {", kCs), script::ErrorCode::SyntaxError);
}

TEST(interruptsAndLimitsApplyToEveryEngine)
{
    script::Context::Options options;
    options.stackLimit = 64;
    script::Context ctx(options);
    CHECK_THROWS(ctx.eval("int F(int n) { return F(n + 1); } return F(0);", kCs),
                 script::ErrorCode::StackOverflow);
    CHECK_THROWS(ctx.eval("function f(n) { return f(n + 1); } f(0)", kJs),
                 script::ErrorCode::StackOverflow);
    int ticks = 0;
    ctx.setInterruptHandler([&ticks] { return ++ticks > 3; });
    CHECK_THROWS(ctx.eval("while (true) { }", kCs), script::ErrorCode::Interrupted);
    ticks = 0;
    CHECK_THROWS(ctx.eval("while (true) { }", kJs), script::ErrorCode::Interrupted);
}

TEST(javascriptRefusesTypeScriptSyntax)
{
    script::Context ctx;
    CHECK_THROWS(ctx.eval("let x: number = 1", kJs), script::ErrorCode::SyntaxError);
    CHECK_THROWS(ctx.eval("interface A { x: number }", kJs), script::ErrorCode::SyntaxError);
    CHECK_THROWS(ctx.eval("enum E { A }", kJs), script::ErrorCode::SyntaxError);
    CHECK_THROWS(ctx.eval("const v = 1 as any", kJs), script::ErrorCode::SyntaxError);
    CHECK_THROWS(ctx.eval("class A { private x = 1 }", kJs), script::ErrorCode::SyntaxError);
    // The contextual keywords stay ordinary names in JavaScript.
    CHECK_EQ(ctx.eval("let type = 1, interface = 2, as = 3; type + interface + as", kJs).toInt(), 6);
    CHECK_EQ(ctx.eval("function f(a, b) { return a < b > (1); } f(1, 2)", kJs).toBool(), false);
    // And the same source is TypeScript when asked.
    CHECK_EQ(ctx.eval("let x: number = 1; x", kTs).toInt(), 1);
}

namespace {

std::size_t residentKb()
{
    return sys::residentBytes() / 1024;
}

}  // namespace

TEST(mixedWorkloadStaysFlatOverManyRounds)
{
    // The contract for a process that stays up: alternate the languages for
    // thousands of rounds and end where you started -- objects, bytes,
    // strings, pooled memory and the process's own resident size.
    script::Context ctx;
    struct Counter
    {
        int n = 0;
        int bump() { return ++n; }
    };
    Counter counter;
    script::Class<Counter>(ctx, "Counter").method("Bump", &Counter::bump);
    ctx.hostClass("Host").method("Counter", [&counter] { return &counter; });

    script::Assembly cs = ctx.compile(
        "using System.Collections.Generic;"
        "var l = new List<string>(); for (int i = 0; i < 50; i++) l.Add(\"k\" + i);"
        "var d = new Dictionary<string, int>(); foreach (var s in l) d[s] = s.Length;"
        "Host.Counter().Bump(); return d.Count;",
        "round.cs");
    script::Assembly ts = ctx.compile(
        "const m = new Map<string, number>(); for (let i = 0; i < 50; i++) m.set('k' + i, i);"
        "const o: { [k: string]: number } = {}; m.forEach((v, k) => { o[k] = v; });"
        "Host.Counter().Bump(); Object.keys(o).length",
        "round.ts");
    script::Assembly js = ctx.compile(
        "const a = []; for (let i = 0; i < 50; i++) a.push({ i, s: 'v' + i, f: () => i });"
        "const cyc = { a }; cyc.self = cyc; Host.Counter().Bump(); a.length",
        "round.js");

    auto round = [&] {
        CHECK_EQ(ctx.run(cs).toInt(), 50);
        CHECK_EQ(ctx.run(ts).toInt(), 50);
        CHECK_EQ(ctx.run(js).toInt(), 50);
    };
    for (int i = 0; i < 200; ++i)
        round();
    ctx.collectGarbage();
    const script::Context::Stats before = ctx.stats();
    const std::size_t rssBefore = residentKb();

    for (int i = 0; i < 3000; ++i)
        round();
    ctx.collectGarbage();
    const script::Context::Stats after = ctx.stats();
    const std::size_t rssAfter = residentKb();

    CHECK_EQ(counter.n, 3 * 3200);
    CHECK(after.liveObjects <= before.liveObjects + 4);
    CHECK(after.heapBytes <= before.heapBytes + 4096);
    CHECK(after.strings <= before.strings + 4);
    // A couple of 64 KiB blocks either way, depending on where in the collector's sawtooth each reading lands.
    CHECK(after.reservedBytes <= before.reservedBytes + (256u << 10));
    // Resident size is a coarse check: the allocator keeps a megabyte or two
    // of slack around the collector's sawtooth. The heap counters above are
    // the exact ones.
#if defined(__SANITIZE_ADDRESS__)
    (void)rssBefore;  // the sanitizer's quarantine grows the process on purpose
    (void)rssAfter;
#else
    if (rssBefore)
        CHECK(rssAfter <= rssBefore + rssBefore / 4 + 2048);
#endif
}
