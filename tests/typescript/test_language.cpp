// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Language semantics: scoping, closures, classes, control flow, exceptions.

#include <limits>

#include "lang.hpp"
#include "tests/test_util.hpp"

namespace {

std::string run(const char* source)
{
    ts::Context ctx;
    return ctx.eval(source, "<test>").toString();
}

double num(const char* source)
{
    ts::Context ctx;
    return ctx.eval(source, "<test>").toDouble();
}

}  // namespace

TEST(arithmeticFollowsTheSpec)
{
    CHECK_EQ(num("1 + 2 * 3"), 7.0);
    CHECK_EQ(num("2 ** 3 ** 2"), 512.0);
    CHECK_EQ(num("7 % 3"), 1.0);
    CHECK_EQ(num("-7 % 3"), -1.0);
    CHECK_EQ(num("1 / 0"), std::numeric_limits<double>::infinity());
    CHECK_EQ(run("0.1 + 0.2"), std::string("0.30000000000000004"));
    CHECK_EQ(run("1e21"), std::string("1e+21"));
    CHECK_EQ(run("1e-7"), std::string("1e-7"));
    CHECK_EQ(run("(255).toString(16)"), std::string("ff"));
    CHECK_EQ(num("5 | 0"), 5.0);
    CHECK_EQ(num("-1 >>> 28"), 15.0);
}

TEST(stringsConcatenateAndCoerce)
{
    CHECK_EQ(run("'a' + 1"), std::string("a1"));
    CHECK_EQ(run("1 + '1'"), std::string("11"));
    CHECK_EQ(run("`x${1 + 1}y${'z'}`"), std::string("x2yz"));
    CHECK_EQ(run("[1,2,3] + ''"), std::string("1,2,3"));
    CHECK_EQ(run("({}) + ''"), std::string("[object Object]"));
    // Multiplying a string is a coercion the checker refuses to guess at, so
    // the cast is the program saying it meant it.
    CHECK_EQ(num("('12' as any) * 2"), 24.0);
}

TEST(equalityAndTruthiness)
{
    CHECK_EQ(run("1 == ('1' as any)"), std::string("true"));
    CHECK_EQ(run("(1 as any) === '1'"), std::string("false"));
    CHECK_EQ(run("null == undefined"), std::string("true"));
    CHECK_EQ(run("(null as any) === undefined"), std::string("false"));
    CHECK_EQ(run("NaN === NaN"), std::string("false"));
    CHECK_EQ(run("0 ? 'y' : 'n'"), std::string("n"));
    CHECK_EQ(run("'' || 'fallback'"), std::string("fallback"));
    CHECK_EQ(run("0 ?? 'fallback'"), std::string("0"));
    CHECK_EQ(run("null ?? 'fallback'"), std::string("fallback"));
}

TEST(closuresCaptureVariables)
{
    CHECK_EQ(num("function counter() { let n = 0; return () => ++n; }\n"
                 "const c = counter(); c(); c(); c()"),
             3.0);
    CHECK_EQ(num("const fns = []; for (let i = 0; i < 3; i++) fns.push(() => i);\n"
                 "fns[0]() + fns[1]() + fns[2]()"),
             3.0);
    CHECK_EQ(num("const fns = []; for (var i = 0; i < 3; i++) fns.push(() => i);\n"
                 "fns[0]() + fns[1]() + fns[2]()"),
             9.0);
}

TEST(arrowsInheritThis)
{
    CHECK_EQ(num("const o = { n: 41, run() { return [1].map(() => this.n + 1)[0]; } }; o.run()"),
             42.0);
}

TEST(destructuringUnpacks)
{
    CHECK_EQ(run("const [a, , b = 9, ...c] = [1, 2]; `${a}|${b}|${c.length}`"),
             std::string("1|9|0"));
    CHECK_EQ(run("const { x, y: { z }, ...rest } = { x: 1, y: { z: 2 }, w: 3, v: 4 };"
                 "`${x}${z}${Object.keys(rest).join('')}`"),
             std::string("12wv"));
    CHECK_EQ(num("function f({ a = 1, b = 2 } = {}) { return a + b; } f() + f({ a: 10 })"), 15.0);
    CHECK_EQ(run("let a = 1, b = 2; [a, b] = [b, a]; `${a}${b}`"), std::string("21"));
}

TEST(spreadAndRest)
{
    CHECK_EQ(num("function sum(...n) { return n.reduce((a, b) => a + b, 0); } sum(...[1,2,3], 4)"),
             10.0);
    CHECK_EQ(run("JSON.stringify({ ...{ a: 1 }, b: 2 })"), std::string("{\"a\":1,\"b\":2}"));
    CHECK_EQ(run("[...'abc'].join('-')"), std::string("a-b-c"));
}

TEST(classesAndInheritance)
{
    CHECK_EQ(run(R"(
        class Shape {
            constructor(public name: string) {}
            describe(): string { return `a ${this.name}`; }
            get label(): string { return this.name.toUpperCase(); }
            static of(name: string): Shape { return new Shape(name); }
        }
        class Circle extends Shape {
            radius = 2;
            constructor() { super('circle'); }
            describe(): string { return super.describe() + ` of radius ${this.radius}`; }
        }
        const c = new Circle();
        `${c.describe()}|${c.label}|${Shape.of('box').name}|${c instanceof Shape}`
    )"),
             std::string("a circle of radius 2|CIRCLE|box|true"));
}

TEST(controlFlow)
{
    CHECK_EQ(num("let n = 0; outer: for (let i = 0; i < 5; i++) "
                 "{ for (let j = 0; j < 5; j++) { if (j === 2) continue outer; n++; } } n"),
             10.0);
    CHECK_EQ(num("let n = 0; for (const k in { a: 1, b: 2 }) n++; n"), 2.0);
    CHECK_EQ(num("let n = 0; for (const v of [1, 2, 3]) n += v; n"), 6.0);
    CHECK_EQ(run("function grade(n) { switch (true) { case n > 8: return 'A'; "
                 "case n > 5: return 'B'; default: return 'C'; } } grade(9) + grade(6) + grade(1)"),
             std::string("ABC"));
}

TEST(exceptionsUnwindAndRunFinally)
{
    CHECK_EQ(run("let log = ''; try { log += 'a'; throw new Error('x'); } "
                 "catch (e) { log += 'b' + e.message; } finally { log += 'c'; } log"),
             std::string("abxc"));
    CHECK_EQ(num("function f() { try { return 1; } finally { } } f()"), 1.0);
    CHECK_EQ(run("let log = ''; function f() { try { return 'r'; } finally { log += 'f'; } } "
                 "const r = f(); log + r"),
             std::string("fr"));
    CHECK_EQ(run("let log = ''; for (let i = 0; i < 3; i++) { try { if (i === 1) continue; "
                 "log += i; } finally { log += '.'; } } log"),
             std::string("0..2."));
    CHECK_EQ(run("try { null.x } catch (e) { e instanceof TypeError ? 'typeerror' : 'other' }"),
             std::string("typeerror"));
}

TEST(uncaughtExceptionsReachCpp)
{
    ts::Context ctx;
    bool caught = false;
    try {
        ctx.eval("throw new RangeError('too big')");
    } catch (const ts::Exception& e) {
        caught = true;
        CHECK_EQ(e.value().get("message").toString(), std::string("too big"));
        CHECK(std::string(e.what()).find("too big") != std::string::npos);
    }
    CHECK(caught);
}

TEST(deepRecursionIsCaughtNotCrashed)
{
    ts::Context::Options options;
    options.stackLimit = 64;
    ts::Context ctx(options);
    bool caught = false;
    try {
        ctx.eval("function f(n: number): number { return f(n + 1); } f(0)");
    } catch (const ts::Exception&) {
        caught = true;
    }
    CHECK(caught);
}

TEST(interruptHandlerStopsRunawayLoops)
{
    ts::Context ctx;
    int ticks = 0;
    ctx.setInterruptHandler([&ticks] { return ++ticks > 4; });
    bool caught = false;
    try {
        ctx.eval("while (true) {}");
    } catch (const ts::Exception&) {
        caught = true;
    }
    CHECK(caught);
}

TEST(evalKeepsGlobalStateBetweenCalls)
{
    ts::Context ctx;
    ctx.eval("var total = 0; function add(n) { total += n; }");
    ctx.eval("add(2); add(3)");
    CHECK_EQ(ctx.eval("total").toDouble(), 5.0);
}
