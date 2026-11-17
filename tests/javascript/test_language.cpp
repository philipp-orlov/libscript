// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Language semantics: scoping, closures, classes, control flow, exceptions.

#include <limits>

#include "lang.hpp"
#include "tests/test_util.hpp"

namespace {

std::string run(const char* source)
{
    js::Context ctx;
    return ctx.eval(source, "<test>").toString();
}

double num(const char* source)
{
    js::Context ctx;
    return ctx.eval(source, "<test>").toDouble();
}

/// What the dynamic engine's native code and lazy compile do, which another core does not have: a test about them stands aside.
bool otherCore()
{
    js::Context ctx;
    return ctx.core() != script::Context::Options::Core::Dyn;
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
    CHECK_EQ(num("'12' * 2"), 24.0);
}

TEST(equalityAndTruthiness)
{
    CHECK_EQ(run("1 == '1'"), std::string("true"));
    CHECK_EQ(run("1 === '1'"), std::string("false"));
    CHECK_EQ(run("null == undefined"), std::string("true"));
    CHECK_EQ(run("null === undefined"), std::string("false"));
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

TEST(closuresSeeBindingsDeclaredBelowThem)
{
    // Every binding of a scope exists before the first closure in it is
    // compiled, so a reference from inside the closure to a `let`, `const`,
    // `class` or sibling function declared further down is not a global.
    CHECK_EQ(num("function f() { function g() { return x; } const x = 1; return g(); } f()"),
             1.0);
    CHECK_EQ(num("function f() { const g = () => y; let y = 2; return g(); } f()"), 2.0);
    CHECK_EQ(num("function f() { const set = v => { n = v; }; let n = 0; set(3); return n; } f()"),
             3.0);
    CHECK_EQ(num("function f() { function mk() { return new K(4).v; }"
                 " class K { constructor(v) { this.v = v; } } return mk(); } f()"),
             4.0);
    CHECK_EQ(num("function f() { function g() { return a + c; }"
                 " const { a, b: [c] } = { a: 1, b: [4] }; return g(); } f()"),
             5.0);
    CHECK_EQ(run("function f() { function even(n) { return n === 0 || odd(n - 1); }"
                 " function odd(n) { return n !== 0 && even(n - 1); }"
                 " return `${even(10)}${odd(7)}`; } f()"),
             std::string("truetrue"));
    CHECK_EQ(run("function f() { const r = a(); function a() { return b(); }"
                 " function b() { return 'B'; } return r; } f()"),
             std::string("B"));
    // The same holds inside blocks, switch bodies and loop bodies, where each
    // run of the block gets a fresh binding.
    CHECK_EQ(num("function f() { { const g = () => z; const z = 6; return g(); } } f()"), 6.0);
    CHECK_EQ(run("function f(k) { switch (k) { case 1: const g = () => s;"
                 " case 2: let s = 'S'; return g(); } } f(1)"),
             std::string("S"));
    CHECK_EQ(run("const fs = []; for (let i = 0; i < 3; i++) { const g = () => z;"
                 " const z = i * 10; fs.push(g); } fs.map(g => g()).join(',')"),
             std::string("0,10,20"));
    CHECK_EQ(run("const fs = []; let i = 0; while (i < 2) { fs.push(() => v); let v = i++; }"
                 " fs.map(g => g()).join(',')"),
             std::string("0,1"));
    CHECK_EQ(run("let r = []; { const g = () => x; const x = 1; r.push(g()); }"
                 " { const g = () => x; const x = 2; r.push(g()); } r.join(',')"),
             std::string("1,2"));
    // A `const` is enforced even for a closure compiled above its declaration: a TypeError when the assignment runs, and
    // nothing when it does not.
    CHECK_EQ(run("function f() { const g = () => { c = 2; }; const c = 1; try { g(); } catch (e) { return e.name + c; } } f()"),
             std::string("TypeError1"));
    CHECK_EQ(run("function f(x) { const c = 1; if (x) c = 2; return c; } String(f(false))"), std::string("1"));
}

TEST(blockFunctionsShadowInsideTheirBlock)
{
    // Block-level function declarations hoist to the enclosing function, but
    // one that shares a name with a top-level declaration still takes effect
    // where it is written.
    CHECK_EQ(num("function f() { function g() { return 1; } { function g() { return 2; }"
                 " return g(); } } f()"),
             2.0);
}

TEST(argumentsObjectCanBeCaptured)
{
    CHECK_EQ(num("function f() { const g = () => arguments[0]; const q = 1; return g() + q; }"
                 " f(41)"),
             42.0);
    CHECK_EQ(num("function f() { return (() => arguments.length)(); } f(1, 2, 3)"), 3.0);
    CHECK_EQ(num("function f(a) { arguments[0] = 9; return a; } f(1)"), 1.0);
}

TEST(parametersSurviveACapturedThis)
{
    // The `this` box used by inner arrows must not take a parameter's slot.
    CHECK_EQ(num("function f(a, b) { return (() => this.x + a + b)(); } f.call({ x: 1 }, 2, 3)"),
             6.0);
    CHECK_EQ(num("class A { constructor(v) { this.v = v; this.g = () => this.v; } }"
                 " new A(5).g()"),
             5.0);
    CHECK_EQ(run("function f(a, ...r) { return (() => `${this.x}${a}${r}`)(); }"
                 " f.call({ x: 1 }, 2, 3, 4)"),
             std::string("123,4"));
    CHECK_EQ(num("function f(a = () => this.x) { return a(); } f.call({ x: 7 })"), 7.0);
}

TEST(namedFunctionExpressionsBindTheirName)
{
    CHECK_EQ(num("(function fact(n) { return n <= 1 ? 1 : n * fact(n - 1); })(5)"), 120.0);
    CHECK_EQ(run("const f = function g() { return typeof g; }; `${f()}|${typeof g}`"),
             std::string("function|undefined"));
    CHECK_EQ(num("const f = function g(g) { return g; }; f(3)"), 3.0);
    CHECK_EQ(num("const f = function g() { var g = 4; return g; }; f()"), 4.0);
    CHECK_EQ(num("const f = function g() { return (() => g)() === f; }; f() ? 1 : 0"), 1.0);
    // Methods are not named function expressions.
    CHECK_EQ(run("const o = { m() { return typeof m; } }; o.m()"), std::string("undefined"));
}

TEST(templateLiteralsUseToString)
{
    CHECK_EQ(run("const o = { toString() { return 'S'; }, valueOf() { return 42; } };"
                 " `${o}|${o + ''}|${[1, 2]}|${null}|${undefined}`"),
             std::string("S|42|1,2|null|undefined"));
}

TEST(forInWithoutADeclaration)
{
    CHECK_EQ(run("var k, r = ''; for (k in { a: 1, b: 2 }) r += k; r"), std::string("ab"));
    CHECK_EQ(run("const o = {}; for (o.k in { z: 1 }) {} o.k"), std::string("z"));
    CHECK_EQ(run("var k, r = ''; for (k in { a: 1 }) r += ('a' in { a: 1 }); r"),
             std::string("true"));
    CHECK_EQ(run("let r = 0; for (r = ('x' in { x: 1 }) ? 1 : 0; r < 3; r++) {} r"),
             std::string("3"));
}

TEST(callDepthOverflowIsARangeError)
{
    CHECK_EQ(run("function f(n) { return f(n + 1); }"
                 " try { f(0); } catch (e) { `${e instanceof RangeError}|${e.name}`; }"),
             std::string("true|RangeError"));
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
    CHECK_EQ(num("function f({ a = 1, b = 2 } = {}) { return a + b; } f() + f({ a: 10 })"),
             15.0);
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
            constructor(name) { this.name = name; }
            describe() { return `a ${this.name}`; }
            get label() { return this.name.toUpperCase(); }
            static of(name) { return new Shape(name); }
        }
        class Circle extends Shape {
            radius = 2;
            constructor() { super('circle'); }
            describe() { return super.describe() + ` of radius ${this.radius}`; }
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
    js::Context ctx;
    bool caught = false;
    try {
        ctx.eval("throw new RangeError('too big')");
    } catch (const js::Exception& e) {
        caught = true;
        CHECK_EQ(e.value().get("message").toString(), std::string("too big"));
        CHECK(std::string(e.what()).find("too big") != std::string::npos);
    }
    CHECK(caught);
}

TEST(deepRecursionIsCaughtNotCrashed)
{
    js::Context::Options options;
    options.stackLimit = 64;
    js::Context ctx(options);
    bool caught = false;
    try {
        ctx.eval("function f(n) { return f(n + 1); } f(0)");
    } catch (const js::Exception&) {
        caught = true;
    }
    CHECK(caught);
}

TEST(interruptHandlerStopsRunawayLoops)
{
    js::Context ctx;
    int ticks = 0;
    ctx.setInterruptHandler([&ticks] { return ++ticks > 4; });
    bool caught = false;
    try {
        ctx.eval("while (true) {}");
    } catch (const js::Exception&) {
        caught = true;
    }
    CHECK(caught);
}

TEST(anInterruptHandlerSetAfterALoopWasCompiledStopsIt)
{
    // Optimized loops look at whether a handler is set, not at a count, while there is none: one set afterwards (or
    // taken away and set again) must still stop the same code, in every tier.
    for (int mode = 0; mode < 3; ++mode) {
        script::Context::Options options;
        if (mode == 0)
            options.jit.mode = script::JitOptions::Mode::Off;
        else if (mode == 1)
            options.jit.threshold = 1;
        else
            options.jit.mode = script::JitOptions::Mode::Eager;
        js::Context ctx(options);
        ctx.eval("function spin(n) { var s = 0; for (var i = 0; i < n; i++) s = (s + i) | 0; return s; }"
                 "for (var k = 0; k < 50; k++) spin(100000);");
        for (int round = 0; round < 2; ++round) {
            int asked = 0;
            ctx.setInterruptHandler([&asked] { return ++asked > 3; });
            bool caught = false;
            try {
                ctx.eval("spin(1e15)");
            } catch (const js::Exception&) {
                caught = true;
            }
            CHECK(caught);
            CHECK(asked > 3);
            ctx.setInterruptHandler(nullptr);
            CHECK_EQ(ctx.eval("spin(1000)").toString(), std::string("499500"));
        }
    }
}

TEST(evalKeepsGlobalStateBetweenCalls)
{
    js::Context ctx;
    ctx.eval("var total = 0; function add(n) { total += n; }");
    ctx.eval("add(2); add(3)");
    CHECK_EQ(ctx.eval("total").toDouble(), 5.0);
}

// ---- found by the comparison with JavaScriptCore; each of these used to hang or crash the host ----

namespace {

std::string messageOf(const char* source)
{
    js::Context ctx;
    try {
        ctx.eval(source, "<test>");
    } catch (const std::exception& e) {
        return e.what();
    }
    return "<no exception>";
}

}  // namespace

TEST(aPrototypeCycleIsRefused)
{
    CHECK(messageOf("var a = {}, b = Object.create(a); Object.setPrototypeOf(a, b); a.nope").find("cyclic") !=
          std::string::npos);
    CHECK(messageOf("var a = {}; Object.setPrototypeOf(a, a)").find("cyclic") != std::string::npos);
    CHECK_EQ(num("var a = {x: 1}, b = {}; Object.setPrototypeOf(b, a); b.x"), 1.0);
}

TEST(functionPrototypeIsCallableButNotAConstructor)
{
    CHECK(messageOf("new Function.prototype").find("not a constructor") != std::string::npos);
    CHECK_EQ(run("String(Function.prototype())"), "undefined");
    CHECK_EQ(run("typeof Function.prototype"), "function");
}

TEST(superCallsWithNothingToCallAreErrors)
{
    CHECK(messageOf("class C extends null { constructor() { super(); } } new C()").find("super") != std::string::npos);
    CHECK(messageOf("class A {} class B extends A { constructor() { var f = () => super(); f(); f(); } } new B()")
              .find("super") != std::string::npos);
    CHECK(messageOf("class A {} class B extends A { constructor() { } } new B()").find("super") != std::string::npos);
}

TEST(aHugeArrayLengthIsARangeError)
{
    CHECK(messageOf("var x = []; x.length = 4294967295").find("dense") != std::string::npos);
    CHECK(messageOf("new Array(4294967295)").find("dense") != std::string::npos);
}

TEST(frozenArraysStayFrozen)
{
    CHECK_EQ(num("var a = Object.freeze([1, 2]); a[0] = 9; a[0]"), 1.0);
    CHECK_EQ(num("var a = Object.freeze([1, 2]); a[5] = 9; a.length = 0; a.length"), 2.0);
    for (const char* mutation : {"a.push(3)", "a.pop()", "a.shift()", "a.unshift(0)", "a.splice(0, 1)", "a.reverse()",
                                 "a.fill(0)", "a.sort()"}) {
        const std::string source = std::string("var a = Object.freeze([1, 2]); ") + mutation;
        CHECK(messageOf(source.c_str()).find("frozen") != std::string::npos);
    }
}

TEST(thisInAPlainCallFollowsStrictness)
{
    CHECK_EQ(run("function f() { return this; } String(f() === globalThis)"), "true");
    CHECK_EQ(run("function f() { 'use strict'; return this; } String(f())"), "undefined");
    CHECK_EQ(run("'use strict'; function f() { return this; } String(f())"), "undefined");
    CHECK_EQ(run("class K { m() { return this; } } var m = new K().m; String(m())"), "undefined");
    CHECK_EQ(run("function f() { return () => this; } String(f()() === globalThis)"), "true");
}

TEST(aPropertyCacheSeesThroughNeitherShapesNorArrays)
{
    // One site, several shapes: the remembered index must never answer for the wrong key.
    CHECK_EQ(num("function get(o) { return o.x; } get({x: 1}); get({y: 5, x: 2}); get({x: 3}) + get({z: 0, x: 4})"), 7.0);
    // An array's `length` is not a table entry, even when it shares a site with an object that has one.
    CHECK_EQ(num("function len(o) { return o.length; } len({length: 7}); len([1, 2, 3])"), 3.0);
    CHECK_EQ(num("function set(o, v) { o.length = v; } var a = [1, 2, 3]; set({length: 0}, 1); set(a, 1); a.length"), 1.0);
    // Writes respect read-only properties and frozen objects on the fast path too.
    CHECK_EQ(num("function set(o) { o.x = 9; } var a = {x: 1}; set(a); var b = Object.freeze({x: 1}); set(b); b.x"), 1.0);
}

TEST(appendingToAStringLeavesEveryOtherHolderAlone)
{
    // Native code grows a string in place when only the local holds it; anything else that sees it keeps the old text.
    const char* body = "function f(n) { var s = 'abcdefghijklmnopqrstuvwxyz0123456789'; var keep = [];"
                       " for (var i = 0; i < n; i++) { var t = s; s += 'x'; keep.push(t.length); }"
                       " var u = s; s += 'y'; return keep[n - 1] + ':' + u.length + ':' + s.length; }"
                       " var r; for (var j = 0; j < 400; j++) r = f(50); r";
    CHECK_EQ(run(body), std::string("85:86:87"));
    CHECK_EQ(run("function g(a, b) { return a.length + ':' + b.length; }"
                 " function f() { var s = 'abcdefghijklmnopqrstuvwxyz0123456789'; var r = '';"
                 " for (var i = 0; i < 3; i++) r = g(s, s += 'z'); return r; }"
                 " var r; for (var j = 0; j < 400; j++) r = f(); r"),
             std::string("38:39"));
    CHECK_EQ(run("function f() { var s = 'abcdefghijklmnopqrstuvwxyz0123456789'; for (var i = 0; i < 40; i++) s += s.length;"
                 " return s.length; } var r; for (var j = 0; j < 400; j++) r = f(); r"),
             std::string("124"));
}

TEST(aVarReadBeforeItsFirstNumberIsUndefined)
{
    // Native code keeps such a local as a bare number once nothing can read it before a number is stored.
    CHECK_EQ(run("function f(n) { var s; var r = typeof s; for (var i = 0; i < n; i++) s = i; return r + s; }"
                 " var r; for (var j = 0; j < 400; j++) r = f(3); r"),
             std::string("undefined2"));
    CHECK_EQ(run("function f(n) { var s = 5; s = undefined; var r = String(s); for (var i = 0; i < n; i++) s = i; return r + s; }"
                 " var r; for (var j = 0; j < 400; j++) r = f(3); r"),
             std::string("undefined2"));
    CHECK_EQ(run("function f(n) { var s = 1; if (n > 5) s = undefined; return String(s); }"
                 " var r = ''; for (var j = 0; j < 400; j++) r = f(j % 10); r"),
             std::string("undefined"));
}

TEST(callbacksFromBuiltInsUnwindAndRecurse)
{
    // A built-in enters a compiled callback directly; throwing out of it, or calling back in, has to behave the same.
    CHECK_EQ(run("var hit = 0; function f(x) { if (x === 250) throw new Error('at ' + x); hit++; return x * 2; }"
                 " var a = []; for (var i = 0; i < 300; i++) a.push(i); var r = '';"
                 " for (var k = 0; k < 3; k++) { try { a.map(f); } catch (e) { r = e.message; } } r + ':' + hit"),
             std::string("at 250:750"));
    CHECK_EQ(run("function inner(x) { return [x, x + 1].reduce(function (p, q) { return p + q; }, 0); }"
                 " var a = []; for (var i = 0; i < 300; i++) a.push(i);"
                 " var s = 0; for (var k = 0; k < 3; k++) s = a.map(inner).reduce(function (p, q) { return p + q; }, 0); s"),
             std::string("90000"));
    CHECK_EQ(run("var a = []; for (var i = 0; i < 500; i++) a.push((i * 7919) % 500);"
                 " a.sort(function (p, q) { return p - q; }); a[0] + ',' + a[250] + ',' + a[499]"),
             std::string("0,250,499"));
    CHECK_EQ(run("function deep(n) { return n === 0 ? 0 : [n].map(function (x) { return deep(x - 1) + 1; })[0]; }"
                 " try { deep(100000); 'no' } catch (e) { e instanceof RangeError ? 'range' : String(e) }"),
             std::string("range"));
}

TEST(objectLiteralsKeepTheirKeysInOrder)
{
    // Native code lays a literal's plain keys straight into its table; anything else in the literal must still land right.
    CHECK_EQ(run("function f(i) { var o = {a: i, b: 2, ...{c: 3, a: 9}, d: 4, ['e' + i]: 5, get g() { return 6; }, h: 7, a: 8};"
                 " return Object.keys(o).join() + '|' + o.a + o.b + o.c + o.d + o['e' + i] + o.g + o.h; }"
                 " var r; for (var j = 0; j < 400; j++) r = f(1); r"),
             std::string("a,b,c,d,e1,g,h|8234567"));
    CHECK_EQ(run("function f(i) { var o = {x: i, y: i + 1, x: i + 2}; return Object.keys(o).join() + o.x + o.y; }"
                 " var r; for (var j = 0; j < 400; j++) r = f(1); r"),
             std::string("x,y32"));
    CHECK_EQ(run("function f(i) { var o = {p0: 0, p1: 1, p2: 2, p3: 3, p4: 4, p5: 5, p6: 6, p7: 7, p8: 8, p9: i};"
                 " return o.p9 + o.p8 + o.p0 + Object.keys(o).length; } var r; for (var j = 0; j < 400; j++) r = f(5); r"),
             std::string("23"));
}

TEST(nativeCodeHandsBackWhatItDidNotExpect)
{
    // Native code assumes the one shape a site has seen and that a field holding numbers keeps doing so; when that stops
    // being true the interpreter carries on from the same instruction, with every local and stack entry as it was.
    CHECK_EQ(run("function f(o) { return o.x + 1; } var r = 0; for (var i = 0; i < 600; i++) r += f({x: i});"
                 " r + ':' + f({y: 1, x: 5}) + ':' + f({x: 'a'}) + ':' + f({get x() { return 7; }})"),
             std::string("180300:6:a1:8"));
    CHECK_EQ(run("function f(o, a, b) { var s = 'k'; var n = a * 2; return n + (b + o.x * (a - o.y)) + s; }"
                 " var r; for (var i = 0; i < 600; i++) r = f({x: 2, y: 1}, 3, 4); r + ':' + f({x: 2, y: 'z'}, 3, 4)"
                 " + ':' + f({y: 1, x: 2}, 3, 4)"),
             std::string("14k:NaNk:14k"));
    CHECK_EQ(run("function sum(list) { var t = 0; for (var i = 0; i < list.length; i++) t += list[i].v; return t; }"
                 " var xs = []; for (var i = 0; i < 3000; i++) xs.push(i % 1000 == 999 ? {w: 0, v: 1} : {v: 1});"
                 " var r = 0; for (var k = 0; k < 20; k++) r += sum(xs); r"),
             std::string("60000"));
    CHECK_EQ(run("function f(o) { var g = function() { return o.x; }; return o.x + g(); }"
                 " var r = ''; for (var i = 0; i < 2000; i++) r = f(i % 3 ? {x: 1} : {z: 0, x: 2}); r"),
             std::string("2"));
    CHECK_EQ(run("function P() {} P.prototype.get = function() { return this.a + this.b; };"
                 " function f(p) { return p.get(); } var r = 0; for (var i = 0; i < 600; i++) { var p = new P(); p.a = 1; p.b = 2;"
                 " r += f(p); } var q = new P(); q.b = 2; q.a = 1; r + ':' + f(q) + ':' + P.prototype.get.call({a: 'x', b: 1})"),
             std::string("1800:3:x1"));
    CHECK_EQ(run("function B(x) { this.x = x; this.y = x * 2; } var r = 0; for (var i = 0; i < 600; i++) r += new B(i).y;"
                 " Object.defineProperty(B.prototype, 'y', { set: function(v) { this.z = v + 1; }, get: function() {"
                 " return this.z; } }); r + ':' + new B(3).y"),
             std::string("359400:7"));
    CHECK_EQ(run("function f(o) { var t = o.x; if (o.y) throw new Error('y' + t); return t; } var r = 0;"
                 " for (var i = 0; i < 600; i++) r += f({x: 1, y: 0});"
                 " try { f({x: 5, y: 1, z: 0}); } catch (e) { r = r + e.message; } r"),
             std::string("600y5"));
    // A parameter only ever given numbers is taken to be one, on entry and on entering a loop part way through.
    CHECK_EQ(run("function k(n, m) { var s = 0; for (var i = 0; i < n; i++) s += m; return s; }"
                 " k(5000, 1) + ':' + k(3, 'a') + ':' + k(4000, 2) + ':' + k(2) + ':' + k(5000, 1)"),
             std::string("5000:0aaa:8000:NaN:5000"));
    CHECK_EQ(run("function g(n, m) { var s = ''; for (var i = 0; i < n; i++) s = s + m; return s.length; }"
                 " var r = 0; for (var j = 0; j < 20; j++) r += g(2, 1); r + ':' + g(5000, 'ab') + ':' + g(3000, 7)"),
             std::string("40:10000:3000"));
    CHECK_EQ(run("function h(a, b) { return a < b ? a - b : b * a; } var r = 0; for (var i = 0; i < 3000; i++) r += h(i, 50);"
                 " r + ':' + h('b', 'a') + ':' + h('2', 3) + ':' + h({valueOf: function() { return 9; }}, 4)"),
             std::string("224862475:NaN:-1:36"));
    // A local native code keeps as a number, which the interpreter makes a string after a bail, then enters the loop.
    CHECK_EQ(run("var out = []; function run(a, b) { var v = 0; v += (7 - {x: b, y: 870}) ? 5 : {x: 1, y: v};"
                 " for (var k = 0; k < 500; k++) out.push(Math.sqrt(String(v).length)); return a; }"
                 " for (var i = 0; i < 300; i++) run(i, i * 2); out.length + ':' + out.every(function(x) { return x === 4; })"),
             std::string("150000:true"));
    // Whole numbers in native code: a remainder by a constant is a multiply, and an answer that is no int32 -- an
    // overflow, minus zero -- is the interpreter's to give.
    CHECK_EQ(run("function f(x) { var t = 0; for (var i = x; i < x + 3000; i++) t = (t + i % 7 + i % 10 + i % 3 + i % 1000"
                 " + i % 65536 + i % 641 + i % 2147483647) | 0; return t; }"
                 " var r = []; for (var k = 0; k < 6; k++) r.push(f([0, 1, 99991, 2147480000, 1234567, 65530][k])); r.join()"),
             std::string("11436304,11442744,414775341,186181523,-415072550,208466060"));
    CHECK_EQ(run("function f(b) { return 1 / ((-b) % String(0).length); } var r; for (var i = 0; i < 400; i++) r = f(i * 2);"
                 " r + ':' + f(0)"),
             std::string("-Infinity:-Infinity"));
    CHECK_EQ(run("function g(n) { var a = 1, z = 0; for (var i = 0; i < n; i++) { a = a * 3; z = i * -1; } return a + ':' + (1 / z); }"
                 " var r; for (var j = 0; j < 300; j++) r = g(1); r + '|' + g(30) + '|' + g(1)"),
             std::string("3:-Infinity|205891132094649:-0.034482758620689655|3:-Infinity"));
    CHECK_EQ(run("'use strict'; function A() { this.n = 1; } A.prototype.inc = function(k) { this.n += k.d; return this; };"
                 " var a = new A(); for (var i = 0; i < 3000; i++) a.inc(i < 2500 ? {d: 1} : {e: 0, d: 2}); a.n"),
             std::string("3501"));
}

TEST(smallMethodsRunInPlaceOfTheirCalls)
{
    // A small method of a known receiver is inlined where it is called, behind a check that it is still that method.
    CHECK_EQ(run("function P(x) { this.x = x; } P.prototype.get = function() { return this.x; };"
                 " function sum(list) { var t = 0; for (var i = 0; i < list.length; i++) t += list[i].get(); return t; }"
                 " var xs = []; for (var i = 0; i < 500; i++) xs.push(new P(i)); var r = 0; for (var k = 0; k < 20; k++)"
                 " r += sum(xs); xs[7] = {get: function() { return 'a'; }}; var q = new P(1); q.y = 2; xs[8] = q;"
                 " r + ':' + sum(xs).slice(0, 8)"),
             std::string("2495000:21a19101"));
    // The method replaced, its field turning into a string, a getter in its place: each the interpreter's, or the call's.
    CHECK_EQ(run("function P(x) { this.x = x; } P.prototype.get = function() { return this.x * 2; };"
                 " function f(p) { return p.get(); } var r = 0; for (var i = 0; i < 600; i++) r += f(new P(i));"
                 " var s = r + ':' + f(new P('3')); P.prototype.get = function() { return -1; };"
                 " s + ':' + f(new P(4)) + ':' + f({get: function() { return 9; }})"),
             std::string("359400:6:-1:9"));
    // Missing arguments read as undefined, an assigned parameter is the callee's own, and two levels nest.
    CHECK_EQ(run("var o = { a: function(x, y) { y = y || 10; return x + y; }, b: function(x) { return this.a(x) * 2; } };"
                 " function f(i) { return o.b(i); } var r = 0; for (var i = 0; i < 600; i++) r += f(i); r"),
             std::string("371400"));
    // An exception from inlined code unwinds the caller too, and the trace names both.
    CHECK_EQ(run("function P() { this.n = 0; } P.prototype.check = function check(v) { if (v > 500) throw new Error('big ' + v);"
                 " return v; }; function outer(p, v) { return p.check(v) + 1; } var p = new P(), r = 0;"
                 " try { for (var i = 0; i < 1000; i++) r += outer(p, i); } catch (e) {"
                 " r = r + ':' + e.message + ':' + (e.stack.indexOf('check') >= 0) + (e.stack.indexOf('outer') >= 0); } r"),
             std::string("125751:big 501:truetrue"));
}

TEST(globalsAreReadWhereTheyAreWhileNothingMovesThem)
{
    // Native code reads and writes a global in place while the global object's table stays as it was compiled against.
    CHECK_EQ(run("globalThis.g = 1, h = 0; function f() { h = h + g; return h; } for (var i = 0; i < 600; i++) f();"
                 " for (var j = 0; j < 300; j++) globalThis['extra' + j] = j; var a = f();"
                 " delete globalThis.extra5; var b = f(); g = 'x'; var c = f();"
                 " Object.defineProperty(globalThis, 'g', {get: function() { return 100; }, configurable: true}); var d = f();"
                 " a + ':' + b + ':' + c + ':' + d"),
             std::string("601:602:602x:602x100"));
    CHECK_EQ(run("var count = 0; function bump() { count = count + 1; } for (var i = 0; i < 600; i++) bump();"
                 " Object.freeze(globalThis); for (var i = 0; i < 600; i++) bump(); count"),
             std::string("600"));
}

TEST(calledFunctionsAreTheOnesTheCodeExpects)
{
    // Native code takes a global or a method to be the function it was when compiled, and keeps that one alive: replacing
    // it, even from inside a call to it, hands the call back, and the old function runs to its end.
    CHECK_EQ(run("function rec(n) { if (n === 3) { rec = function (m) { return -m; }; } return n <= 0 ? 0 : n + rec(n - 1); }"
                 " var s = 0; for (var j = 0; j < 500; j++) s += rec(2); var before = rec(5); s + ':' + before + ':' + rec(4)"),
             std::string("1500:10:-4"));
    CHECK_EQ(run("function P() { this.v = 1; } P.prototype.get = function () { return this.v; };"
                 " function sum(o, n) { var s = 0; for (var i = 0; i < n; i++) s += o.get(); return s; }"
                 " var o = new P(); var a = sum(o, 1000); P.prototype.get = function () { return 10; }; var b = sum(o, 1000);"
                 " o.get = function () { return 100; }; var c = sum(o, 10);"
                 " var x = 0; function f() { g = null; return 5; } function g() { return f(); }"
                 " for (var i = 0; i < 600; i++) { g = function () { return 1; }; x += g(); }"
                 " var h = function () { h = 0; return 7; }; var y = 0; for (var i = 0; i < 300; i++) { var t = h; y += t(); h = t; }"
                 " a + ':' + b + ':' + c + ':' + x + ':' + y"),
             std::string("1000:10000:1000:600:2100"));
}

TEST(integersAndParametersKeepTheirMeaningInNativeCode)
{
    // Native code keeps integers unchecked in 64 bits and parameters unboxed. Expected values are from JavaScriptCore.
    CHECK_EQ(run(R"JS(var out=[];
function a1(x,y){return (x+y)|0;}
function a2(x,y){return x+y;}
function a3(x){return x*31+7;}
function a4(x){return (x*31)|0;}
function a5(x,y){return x*y;}
function a6(x){return x>>>0;}
function a7(x){return (x>>>1)+1;}
function a8(x){var s=0;for(var i=0;i<3;i++){s=s+x;}return s;}
function a9(x){x=x+1;return x;}
function a10(x){x="s"+x;return x;}
function a11(x,y){return arguments.length+(y===undefined?100:y);}
function a12(a,i){var t=0;for(var k=0;k<2;k++){t+=a[i+k];}return t;}
function a13(x){return (x&0xffff)*(x&0xffff);}
function a14(x){return ((x&0xffffff)*(x&0xffffff))|0;}
function a15(x){return -x*0;}
function a16(x,y){return (x%y)*x;}
function a17(x){var i=x;i++;return i;}
function a18(x){return 1/(x*0);}
function a19(x){return 1/((x&3)*(x>>31));}
function a20(n){if(n>1000)n=1000;var s=0;while(--n>=0){s=(s*33+n)|0;}return s;}
function a21(x){var r=x>>>0;return r>2147483647?'big':r;}
function a22(x){return (x-1)>=0;}
function a23(x){return [x+1,x-1,x*2,x>>>0];}
var A=[1,2,3,4,5,6];
var cases=[[1,2],[2147483647,1],[-2147483648,-1],[0.5,1.5],[-0,0],[1e20,1],['a',1],[3],[],[NaN,1],[4294967295,1],[-1,3],[null,2],[true,true]];
var fns=[a1,a2,a3,a4,a5,a6,a7,a8,a9,a10,a11,a13,a14,a15,a16,a17,a18,a19,a20,a21,a22,a23];
function show(v){ if(typeof v==='number'&&v===0&&1/v<0)return '-0'; if(Array.isArray(v))return '['+v.map(show).join()+']'; return String(v); }
for(var f=0;f<fns.length;f++){ var fn=fns[f];
 for(var rep=0;rep<400;rep++) fn(rep%50,rep%7+1);
 var row=[]; for(var c=0;c<cases.length;c++){ row.push(show(fn.apply(null,cases[c]))); }
 for(var rep=0;rep<400;rep++) fn(rep,rep+1);
 for(var c=0;c<cases.length;c++){ row.push(show(fn.apply(null,cases[c]))); }
 out.push(row.join(' '));
}
for(var rep=0;rep<400;rep++) a12(A,rep%3);
out.push(a12(A,4)+' '+a12(A,5)+' '+a12(A,1.5)+' '+a12(A,'1'));
var big=0;for(var i=0;i<3000;i++){big=(big+2147483647)|0;big=(big*1)|0;} out.push(big);
var u=0;for(var i=0;i<1000;i++){u=(u+4000000000)>>>0;} out.push(u);
var v=1;for(var i=0;i<1000;i++){v=v*3;if(v>1e15)v=v%1000003;} out.push(v);
var w=0;for(var i=0;i<1000;i++){var q=i*65536*65536;w+=q&1;w+=(q>>>0);} out.push(w);
out.join('\n'))JS"),
             std::string(R"JS(3 -2147483648 2147483647 2 0 1661992960 0 0 0 0 0 2 2 2 3 -2147483648 2147483647 2 0 1661992960 0 0 0 0 0 2 2 2
3 2147483648 -2147483649 2 0 100000000000000000000 a1 NaN NaN NaN 4294967296 2 2 2 3 2147483648 -2147483649 2 0 100000000000000000000 a1 NaN NaN NaN 4294967296 2 2 2
38 66571993064 -66571993081 22.5 7 3.1e+21 NaN 100 NaN NaN 133143986152 -24 7 38 38 66571993064 -66571993081 22.5 7 3.1e+21 NaN 100 NaN NaN 133143986152 -24 7 38
31 2147483617 -2147483648 15 0 -17825792 0 93 0 0 -31 -31 0 31 31 2147483617 -2147483648 15 0 -17825792 0 93 0 0 -31 -31 0 31
2 2147483647 2147483648 0.75 -0 100000000000000000000 NaN NaN NaN NaN 4294967295 -3 0 1 2 2147483647 2147483648 0.75 -0 100000000000000000000 NaN NaN NaN NaN 4294967295 -3 0 1
1 2147483647 2147483648 0 0 1661992960 0 3 0 0 4294967295 4294967295 0 1 1 2147483647 2147483648 0 0 1661992960 0 3 0 0 4294967295 4294967295 0 1
1 1073741824 1073741825 1 1 830996481 1 2 1 1 2147483648 2147483648 1 1 1 1073741824 1073741825 1 1 830996481 1 2 1 1 2147483648 2147483648 1 1
3 6442450941 -6442450944 1.5 0 300000000000000000000 0aaa 9 NaN NaN 12884901885 -3 0 3 3 6442450941 -6442450944 1.5 0 300000000000000000000 0aaa 9 NaN NaN 12884901885 -3 0 3
2 2147483648 -2147483647 1.5 1 100000000000000000000 a1 4 NaN NaN 4294967296 0 1 2 2 2147483648 -2147483647 1.5 1 100000000000000000000 a1 4 NaN NaN 4294967296 0 1 2
s1 s2147483647 s-2147483648 s0.5 s0 s100000000000000000000 sa s3 sundefined sNaN s4294967295 s-1 snull strue s1 s2147483647 s-2147483648 s0.5 s0 s100000000000000000000 sa s3 sundefined sNaN s4294967295 s-1 snull strue
4 3 1 3.5 2 3 3 101 100 3 3 5 4 3 4 3 1 3.5 2 3 3 101 100 3 3 5 4 3
1 4294836225 0 0 0 0 0 9 0 0 4294836225 4294836225 0 1 1 4294836225 0 0 0 0 0 9 0 0 4294836225 4294836225 0 1
1 -33554431 0 0 0 0 0 9 0 0 -33554431 -33554431 0 1 1 -33554431 0 0 0 0 0 9 0 0 -33554431 -33554431 0 1
-0 -0 0 -0 0 -0 NaN -0 NaN NaN -0 0 -0 -0 -0 -0 0 -0 0 -0 NaN -0 NaN NaN -0 0 -0 -0
1 0 0 0.25 NaN 0 NaN NaN NaN NaN 0 1 0 0 1 0 0 0.25 NaN 0 NaN NaN NaN NaN 0 1 0 0
2 2147483648 -2147483647 1.5 1 100000000000000000000 NaN 4 NaN NaN 4294967296 0 1 2 2 2147483648 -2147483647 1.5 1 100000000000000000000 NaN 4 NaN NaN 4294967296 0 1 2
Infinity Infinity -Infinity Infinity -Infinity Infinity NaN Infinity NaN NaN Infinity -Infinity Infinity Infinity Infinity Infinity -Infinity Infinity -Infinity Infinity NaN Infinity NaN NaN Infinity -Infinity Infinity Infinity
Infinity Infinity -Infinity Infinity Infinity Infinity Infinity Infinity Infinity Infinity -0.3333333333333333 -0.3333333333333333 Infinity Infinity Infinity Infinity -Infinity Infinity Infinity Infinity Infinity Infinity Infinity Infinity -0.3333333333333333 -0.3333333333333333 Infinity Infinity
0 -1338350932 0 0 0 -1338350932 0 2211 0 0 -1338350932 0 0 0 0 -1338350932 0 0 0 -1338350932 0 2211 0 0 -1338350932 0 0 0
1 2147483647 big 0 0 1661992960 0 3 0 0 big big 0 1 1 2147483647 big 0 0 1661992960 0 3 0 0 big big 0 1
true true false false false true false true false false true false false true true true false false false true false true false false true false false true
[2,0,2,1] [2147483648,2147483646,4294967294,2147483647] [-2147483647,-2147483649,-4294967296,2147483648] [1.5,-0.5,1,0] [1,-1,-0,0] [100000000000000000000,100000000000000000000,200000000000000000000,1661992960] [a1,NaN,NaN,0] [4,2,6,3] [NaN,NaN,NaN,0] [NaN,NaN,NaN,0] [4294967296,4294967294,8589934590,4294967295] [0,-2,-2,4294967295] [1,-1,0,0] [2,0,2,1] [2,0,2,1] [2147483648,2147483646,4294967294,2147483647] [-2147483647,-2147483649,-4294967296,2147483648] [1.5,-0.5,1,0] [1,-1,-0,0] [100000000000000000000,100000000000000000000,200000000000000000000,1661992960] [a1,NaN,NaN,0] [4,2,6,3] [NaN,NaN,NaN,0] [NaN,NaN,NaN,0] [4294967296,4294967294,8589934590,4294967295] [0,-2,-2,4294967295] [1,-1,0,0] [2,0,2,1]
11 NaN NaN NaN
-3000
1385447424
2073222
0)JS"));
}

TEST(jsonSeesElementsAndKeysAsItShould)
{
    CHECK_EQ(run("var seen = []; JSON.stringify([5, {toJSON: function (k) { seen.push(typeof k + k); return 1; }}],"
                 " function (k, v) { seen.push(typeof k + ':' + k); return v; }); seen.join()"),
             std::string("string:,string:0,string1,string:1"));
    CHECK_EQ(run("JSON.stringify({a: 1, b: undefined, c: function () {}, d: [undefined, 2], e: {f: -3, g: 1.5}})"),
             std::string("{\"a\":1,\"d\":[null,2],\"e\":{\"f\":-3,\"g\":1.5}}"));
    CHECK_EQ(run("var r = []; ['01', '-', '1.', '1e', '-01', '.5'].forEach(function (t) { try { JSON.parse(t); r.push('ok'); }"
                 " catch (e) { r.push(e.name); } }); r.join() + JSON.parse('[0,-0.5,1e2,12]').join()"),
             std::string("SyntaxError,SyntaxError,SyntaxError,SyntaxError,SyntaxError,SyntaxError0,-0.5,100,12"));
}

TEST(instanceofWalksTheChainInPlaceWhileNothingChanges)
{
    // Expected values are from JavaScriptCore.
    CHECK_EQ(run(R"JS(function A(){} function B(){} B.prototype=Object.create(A.prototype); function C(){}
function t(x){return (x instanceof A)+':'+(x instanceof B);}
var xs=[new A,new B,new C,1,'s',null,undefined,{},[],function(){}]; var r='';
for(var i=0;i<600;i++) for(var j=0;j<xs.length;j++) r=t(xs[j]);
var o=[];for(var j=0;j<xs.length;j++)o.push(t(xs[j]));
B.prototype=new C; o.push(t(new B)); o.push(t(xs[1]));
Object.defineProperty(A,Symbol.hasInstance,{value:function(v){return v===1;}}); o.push(t(1)); o.push(t(xs[0]));
A.prototype=3; try{ o.push(new C instanceof C); o.push(xs[2] instanceof B);}catch(e){o.push(e.name)}
o.join(' '))JS"),
             std::string("true:false true:true false:false false:false false:false false:false false:false false:false false:false false:false false:true true:false true:false false:false true false"));
}

TEST(constructorsRunInPlaceAsTheyWouldAnywhere)
{
    // Expected values are from JavaScriptCore.
    CHECK_EQ(run(R"JS(var out=[];
function P(a,b){this.a=a;this.b=b;}
P.prototype.sum=function(){return this.a+this.b;};
function R(x){this.x=x;return {other:x};}
function Q(x){this.x=x;return 5;}
function Nope(x){if(x>600)throw new Error('big'+x);this.x=x;}
function Self(){this.me=this;this.n=1;}
function Few(a,b,c){this.a=a;this.b=b;this.c=c;}
function Cond(f){if(f)this.y=1;else this.z=2;this.w=3;}
function run(i){var p=new P(i,2),r=new R(i),q=new Q(i),s=new Self(),f=new Few(i),c=new Cond(i%2);
 return p.sum()+':'+r.other+':'+r.x+':'+q.x+':'+s.me.n+':'+f.a+':'+f.c+':'+Object.keys(c).join('')+':'+(p instanceof P)+':'+(p.constructor===P);}
var r;for(var i=0;i<800;i++)r=run(i);out.push(r);
for(var i=590;i<610;i++){try{var n=new Nope(i);r=n.x;}catch(e){r=e.message;}}out.push(r);
var oldProto=P.prototype;
P.prototype={sum:function(){return -1;}};out.push(new P(1,2).sum());out.push(run(1));
P.prototype=oldProto;out.push(new P(1,2).sum());
P.prototype=3;var w=new P(1,2);out.push(Object.getPrototypeOf(w)===Object.prototype);
P=function(){this.k=9;};out.push(new P().k);
function G(v){this.v=v;} Object.defineProperty(G.prototype,'v',{set:function(x){this._v=x+1;},get:function(){return this._v;}});
for(var i=0;i<600;i++){r=new G(i).v;}out.push(r);
function H(v){this.v=v;} for(var i=0;i<600;i++){r=new H(i).v;} Object.defineProperty(H.prototype,'v',{set:function(x){this._v=x*2;},get:function(){return this._v;}});out.push(new H(4).v);
function F2(v){this.v=v;} for(var i=0;i<600;i++){r=new F2(i);} Object.freeze(F2.prototype); out.push(new F2(4).v);
var total=0;function Cnt(){total++;this.a=total;}for(var i=0;i<700;i++){new Cnt();}out.push(total);
out.join('\n'))JS"),
             std::string(R"JS(801:799:undefined:799:1:799:undefined:yw:true:true
big609
-1
-1:1:undefined:1:1:1:undefined:yw:true:false
3
true
9
600
8
4
700)JS"));
}

TEST(argumentsForwardedWithoutAnArrayAreStillArguments)
{
    // Expected values are from JavaScriptCore.
    CHECK_EQ(run(R"JS(var out=[];
function tgt(a,b,c){return [this.v,a,b,c,arguments.length].join();}
var o={v:1,f:function(){return tgt.apply(this,arguments);},g:function(){return this.h.apply(this,arguments);},h:function(a){return 'h'+a+arguments.length;}};
var r;for(var i=0;i<500;i++){r=o.f(i,2);r=o.g(i);}out.push(r,o.f(),o.f(1,2,3,4));
function viaOther(){return Math.max.apply(null,arguments);}for(var i=0;i<500;i++)r=viaOther(i,2,9);out.push(r);
function notApply(f){return f(1,arguments);}
out.push(notApply(function(a,b){return a+':'+b.length+':'+b[0].name;}));
for(var i=0;i<500;i++)r=notApply(function(a,b){return a+':'+b.length;});out.push(r);
var ap=Function.prototype.apply;function sw(){return this.m.apply(this,arguments);}var s={m:function(){return arguments.length;}};
for(var i=0;i<500;i++)r=sw.call(s,1,2);out.push(r);
s.m={apply:function(t,a){return 'custom'+a.length+a[1];}};out.push(sw.call(s,1,2));
function th(){return thrower.apply(this,arguments);}function thrower(a){if(a>400)throw new Error('e'+a);return a;}
for(var i=398;i<403;i++){try{r=th(i);}catch(e){r=e.message;}out.push(r);}
function bindy(){var f=function(){return arguments.length;};return f.apply(null,arguments);}for(var i=0;i<500;i++)r=bindy(1,2,3);out.push(r);
out.join('\n'))JS"),
             std::string(R"JS(h4991
1,,,,0
1,1,2,3,4
499
1:1:
1:1
2
custom22
398
399
400
e401
e402
3)JS"));
}

TEST(applyPassesAnArrayItsElements)
{
    // Native code calls straight through `apply` with an array nothing else holds; a shared one must still be copied.
    CHECK_EQ(run("function C(a, b) { this.s = a + ':' + b + ':' + arguments.length; }"
                 " function make() { var o = Object.create(C.prototype); C.apply(o, arguments); return o.s; }"
                 " var r; for (var i = 0; i < 400; i++) r = make(i, 2, 3); r"),
             std::string("399:2:3"));
    CHECK_EQ(run("var shared = [1, 2, 3]; function f(a, b, c) { shared.length = 0; shared.push(9); return a + b + c; }"
                 " function g() { shared = [1, 2, 3]; return f.apply(null, shared); }"
                 " var r; for (var i = 0; i < 400; i++) r = g(); r"),
             std::string("6"));
    CHECK_EQ(run("function f() { return this.v + arguments.length; } function g(n) { return f.apply({v: n}, [1, 2]); }"
                 " var r; for (var i = 0; i < 400; i++) r = g(i); r"),
             std::string("401"));
    CHECK_EQ(run("class K { constructor() {} } function g() { try { K.apply(null, []); return 'no'; } catch (e) { return e.name; } }"
                 " var r; for (var i = 0; i < 400; i++) r = g(); r"),
             std::string("TypeError"));
}

TEST(arraysOfNumbersBehaveLikeAnyOtherArray)
{
    // What they turn into, and when, is not for a program to see.
    CHECK_EQ(run("var a = [1, 2]; a.push('x'); a.push(3); a.join()"), std::string("1,2,x,3"));
    CHECK_EQ(run("var u = new Array(3); u[1] = 2; String(u[0]) + u[1] + u.length"), std::string("undefined23"));
    CHECK_EQ(run("var u = [1, 2]; u[0] = undefined; u[1] = null; String(u[0]) + String(u[1])"), std::string("undefinednull"));
    CHECK_EQ(run("[undefined].includes(undefined) + ':' + [undefined].indexOf(undefined) + ':' + [NaN].includes(NaN) + ':' + [NaN].indexOf(NaN)"),
             std::string("true:0:true:-1"));
    CHECK_EQ(run("var m = [1, 2, 3, 4, 5]; var r = m.splice(1, 2, 'a'); r.join() + '/' + m.join()"), std::string("2,3/1,a,4,5"));
    CHECK_EQ(run("var s = [1, 2]; s.unshift(0); s.unshift('z'); s.shift() + s.join()"), std::string("z0,1,2"));
    CHECK_EQ(run("var l = [1, 2, 3]; l.length = 1; l.length = 2; String(l[1]) + l.length"), std::string("undefined2"));
    CHECK_EQ(run("var c = [1].concat([2], ['x'], 3); c.join() + c.length"), std::string("1,2,x,34"));
    CHECK_EQ(run("JSON.stringify([1, undefined, 2.5])"), std::string("[1,null,2.5]"));
    CHECK_EQ(run("Math.max.apply(null, [1, 5, 3]) + ':' + Math.max(...[2, 8])"), std::string("5:8"));
    // In a cell that has room for them, and past it.
    CHECK_EQ(run("var o = []; for (var i = 0; i < 40; i++) { var a = [i, i + 1, i + 2]; a.push({}); a.push(i); o = a; } o[0] + ':' + o[4] + ':' + o.length"),
             std::string("39:39:5"));
    // Hot code that met only numbers, when something else arrives.
    CHECK_EQ(run("function get(a, i) { return a[i]; } var n = [1, 2, 3, 4], t = 0; for (var i = 0; i < 2000; i++) t += get(n, i & 3);"
                 " t + ':' + get(n, 9) + ':' + get(['s'], 0) + ':' + get({0: 'o'}, 0)"),
             std::string("5000:undefined:s:o"));
    CHECK_EQ(run("function set(a, i, v) { a[i] = v; } var n = [0, 0, 0, 0]; for (var i = 0; i < 2000; i++) set(n, i & 3, i);"
                 " set(n, 1, 'x'); for (i = 0; i < 2000; i++) set(n, i & 1 ? 2 : 3, i); n.join()"),
             std::string("1996,x,1999,1998"));
    CHECK_EQ(run("var w = new Array(4), t = 0; w[0] = 1; for (var i = 0; i < 2000; i++) { var x = w[i & 3]; t += x === undefined ? 1 : x; } t"),
             std::string("2000"));
    CHECK_EQ(run("var q = []; for (var i = 0; i < 2000; i++) { q.push(i % 2 ? 's' : i); if (q.length > 9) { q.pop(); q.pop(); } } q.join()"),
             std::string("0,s,2,s,4,s,6,s"));
    CHECK_EQ(run("var p = [], bad = 0; for (var i = 0; i < 2000; i++) { p.push(undefined); if (p.pop() !== undefined) bad++; } bad + ':' + p.length"),
             std::string("0:0"));
    // What holds a reference is seen by the collector, and what does not, freed.
    CHECK_EQ(run("var c = [1]; c[0] = c; c[0] === c"), std::string("true"));
}

TEST(whatALoopKnowsAheadOfItStaysTrue)
{
    // Asked once ahead of the loop; these change it inside, each in a way a loop could be caught out by.
    CHECK_EQ(run("function f(o) { var t = 0; for (var i = 0; i < 3000; i++) { t += o.x; if (i == 2500) o.y = 1; } return t + o.y; } f({x: 1})"),
             std::string("3001"));
    CHECK_EQ(run("function f(o, g) { var t = 0; for (var i = 0; i < 3000; i++) { t += o.x; if (i == 2500) g(o); } return t; }"
                 " f({x: 1}, function (o) { delete o.x; o.x = 2; })"),
             std::string("3499"));
    CHECK_EQ(run("function f(a) { var t = 0; for (var i = 0; i < 3000; i++) { t += a[i & 1]; if (i == 2500) a[0] = 'z'; } return t; }"
                 " typeof f([1, 2])"),
             std::string("string"));
    CHECK_EQ(run("function f(a) { for (var i = 0; i < 3000; i++) { a[i & 1] = i; if (i == 2500) a.push(0); } return a.length; } f([1, 2])"),
             std::string("3"));
    CHECK_EQ(run("function f(a) { var t = 0; for (var i = 0; i < 3000; i++) { if (i == 2500) a.length = 0; t += a[0] === undefined ? 1 : 0; } return t; } f([1, 2])"),
             std::string("500"));
    CHECK_EQ(run("function f(a, b) { var t = 0; for (var i = 0; i < 3000; i++) t += b[i & 1]; a.push('x'); for (i = 0; i < 3000; i++) t += i & 1 ? 0 : 1; return t + ':' + a[2]; }"
                 " var s = [1, 2]; f(s, s)"),
             std::string("6000:x"));
    // Not what the loop was told to expect at all, on the way in.
    CHECK_EQ(run("function f(o, n) { var t = 0; for (var i = 0; i < n; i++) t += o.x; return t; }"
                 " for (var k = 0; k < 2000; k++) f({x: 1}, 2); f(null, 0) + f({y: 1, x: 2}, 3) + f('s', 0)"),
             std::string("6"));
    CHECK_EQ(run("function f(a, n) { var t = 0; for (var i = 0; i < n; i++) t += a[i]; return t; }"
                 " for (var k = 0; k < 2000; k++) f([1, 2], 2); f(null, 0) + f(['1'], 1) + f({0: 5}, 1)"),
             std::string("0015"));
    CHECK_EQ(run("var d = 0; for (var i = 0; i < 3000; i++) d += Math.sqrt(4); var s = Math.sqrt; Math.sqrt = function () { return 1; };"
                 " for (i = 0; i < 3000; i++) d += Math.sqrt(4); Math.sqrt = s; d + Math.sqrt('9')"),
             std::string("9003"));
}

TEST(whatALoopStoresInItsColdPathsCounts)
{
    // A field read ahead of the loop as one thing, turned into a number by the out-of-the-way store of a number over it.
    CHECK_EQ(run("function f(o) { var k = o.x, t = 0; for (var i = 0; i < 3000; i++) { o.x = i; t = o.x; } return typeof k + ':' + t; }"
                 " var r; for (var c = 0; c < 400; c++) r = f({x: {}}); r"),
             std::string("object:2999"));
    CHECK_EQ(run("function f(o) { var k = o.x, t = 0; for (var i = 0; i < 3000; i++) { t += typeof o.x === 'number' ? 1 : 0; o.x = i; } return t; }"
                 " var r; for (var c = 0; c < 400; c++) r = f({x: 's'}); r"),
             std::string("2999"));
}

TEST(parseIntOfANumberIsWhatItsTextWouldGive)
{
    CHECK_EQ(run("parseInt(3.9) + ',' + parseInt(-3.9) + ',' + parseInt(0) + ',' + (1 / parseInt(-0))"), std::string("3,-3,0,Infinity"));
    CHECK_EQ(run("1 / parseInt(-0.5)"), std::string("-Infinity"));
    // Below a millionth and from 1e21 up the text has an exponent, and stops at it.
    CHECK_EQ(run("parseInt(0.0000005) + ',' + parseInt(1e21) + ',' + parseInt(-1.5e300)"), std::string("5,1,-1"));
    CHECK_EQ(run("parseInt(1e20) + ',' + parseInt(9007199254740993)"), std::string("100000000000000000000,9007199254740992"));
    CHECK_EQ(run("String(parseInt(NaN)) + ',' + parseInt(Infinity) + ',' + parseInt(10, 2) + ',' + parseInt(15.9, 8)"), std::string("NaN,NaN,2,13"));
    CHECK_EQ(run("parseInt('1' + '0'.repeat(400)) + ',' + parseInt('-1' + '0'.repeat(400)) + ',' + parseInt('541421576552814340') + ',' + parseInt('99999999999999999999999')"),
             std::string("Infinity,-Infinity,541421576552814340,1e+23"));
    // Whatever the number, the same as parsing what it prints as.
    CHECK_EQ(run("var bad = 0, x = 1.7;"
                 "for (var i = 0; i < 20000; i++) { x = (x * 1103.515245 + 12345.678) % 1e7; var e = (i % 90) - 45;"
                 " var d = (i & 1 ? -1 : 1) * x * Math.pow(10, e); var a = parseInt(d), b = parseInt(String(d));"
                 " if (a !== a ? b === b : (a !== b || 1 / a !== 1 / b)) bad++; }"
                 "[9.999999999999999e22, 1e21, 5e-7, -9.5e-7, 1e-6, 0.000001, 123456789012345680000, -1e300, 2e-323]"
                 ".forEach(function (d) { if (parseInt(d) !== parseInt(String(d))) bad++; }); bad"),
             std::string("0"));
}

TEST(mathOnBareNumbersAnswersWhatTheStandardSays)
{
    const auto at = [](const std::string& expr, const std::string& a, const std::string& b) {
        const std::string source = "function f(a, b) { return " + expr + "; } var y; for (var i = 0; i < 3000; i++) f(i, i + 1); y = f(" + a + ", " + b + "); 1 / y === -Infinity ? '-0' : String(y)";
        return run(source.c_str());
    };
    CHECK_EQ(at("Math.max(a, b)", "0", "-0"), std::string("0"));
    CHECK_EQ(at("Math.max(a, b)", "-0", "0"), std::string("0"));
    CHECK_EQ(at("Math.max(a, b)", "-0", "-0"), std::string("-0"));
    CHECK_EQ(at("Math.min(a, b)", "0", "-0"), std::string("-0"));
    CHECK_EQ(at("Math.min(a, b)", "-0", "0"), std::string("-0"));
    CHECK_EQ(at("Math.max(a, b)", "NaN", "1"), std::string("NaN"));
    CHECK_EQ(at("Math.max(a, b)", "1", "NaN"), std::string("NaN"));
    CHECK_EQ(at("Math.min(a, b)", "NaN", "1"), std::string("NaN"));
    CHECK_EQ(at("Math.min(a, b)", "2", "1"), std::string("1"));
    CHECK_EQ(at("Math.max(a, b)", "2", "'7'"), std::string("7"));
    CHECK_EQ(at("Math.pow(a, b)", "1", "Infinity"), std::string("NaN"));
    CHECK_EQ(at("Math.pow(a, b)", "-1", "-Infinity"), std::string("NaN"));
    CHECK_EQ(at("Math.pow(a, b)", "1", "NaN"), std::string("NaN"));
    CHECK_EQ(at("Math.pow(a, b)", "NaN", "0"), std::string("1"));
    CHECK_EQ(at("a ** b", "1", "Infinity"), std::string("NaN"));
    CHECK_EQ(at("Math.pow(a, b)", "2", "10"), std::string("1024"));
    CHECK_EQ(at("Math.round(a)", "-0.2", "0"), std::string("-0"));
    CHECK_EQ(at("Math.round(a)", "0.49999999999999994", "0"), std::string("0"));
    CHECK_EQ(at("Math.round(a)", "2.5", "0"), std::string("3"));
    CHECK_EQ(at("Math.round(a)", "-2.5", "0"), std::string("-2"));
    CHECK_EQ(at("Math.atan2(a, b)", "0", "1"), std::string("0"));
    CHECK_EQ(at("parseInt(a)", "'12px'", "0"), std::string("12"));
    CHECK_EQ(at("parseInt(a)", "1e21", "0"), std::string("1"));
    CHECK_EQ(at("parseInt(a)", "-0.5", "0"), std::string("-0"));
    // A program's own `max` is no business of the built-in's.
    CHECK_EQ(run("function f(a, b) { return Math.max(a, b); } for (var i = 0; i < 3000; i++) f(i, 1);"
                 "Math.max = function () { return 'mine'; }; f(1, 2)"),
             std::string("mine"));
}

TEST(aMethodOfAStringFollowingAnOverflowStaysAMethodOfAString)
{
    // The check on the product is left until the read of `charCodeAt`, and is what fails.
    CHECK_EQ(run("function f(s) { var h = 0; for (var i = 0; i < s.length; i++) h = (h * 31 + s.charCodeAt(i)) | 0; return h; }"
                 "var s = ''; for (var i = 0; i < 300; i++) s += String.fromCharCode(97 + i % 26);"
                 "var y; for (var r = 0; r < 50; r++) y = f(s); y"),
             std::string("-2071830186"));
    // And with it blamed, something other than a string, or a method changed under it, still does what it should.
    CHECK_EQ(run("function f(o, a) { return ((a * 2147483647) + o.m(1)) | 0; }"
                 "String.prototype.m = function (x) { return x + 1; };"
                 "var y; for (var i = 0; i < 2000; i++) y = f('s', i);"
                 "var z = f({ m: function () { return 5; } }, 1); String.prototype.m = function () { return 9; };"
                 "y + ',' + z + ',' + f('s', 1)"),
             std::string("2147481651,-2147483644,-2147483640"));
}

TEST(mapsAndSetsCalledFromNativeCode)
{
    CHECK_EQ(run("function f(m, k, v) { m.set(k, v); return m.get(k) + ':' + m.has(k) + ':' + m.has('nope') + ':' + m.get('nope'); }"
                 "var m = new Map(), y; for (var i = 0; i < 3000; i++) y = f(m, 'k' + (i & 7), i);"
                 "y + ',' + m.size + ',' + f(m, 0, 'z') + ',' + m.get(-0) + ',' + f(m, NaN, 1) + ',' + f(m, {}, 2) + ',' + m.size"),
             std::string("2999:true:false:undefined,8,z:true:false:undefined,z,1:true:false:undefined,2:true:false:undefined,11"));
    // A key that compares by what it says, however it was made; and a chain of `set`, which answers with the map.
    CHECK_EQ(run("function f(m, a, b) { return m.set(a + b, 1).set(a + b, 2).get(a + b); }"
                 "var m = new Map(), y; for (var i = 0; i < 3000; i++) y = f(m, 'x', i & 1); y + ',' + m.size"),
             std::string("2,2"));
    CHECK_EQ(run("function f(s, k) { return s.add(k).has(k) && s.has(k); } var s = new Set(), y; for (var i = 0; i < 3000; i++) y = f(s, i & 3);"
                 "y + ',' + s.size + ',' + s.has(4) + ',' + Array.from(s).join()"),
             std::string("true,4,false,0,1,2,3"));
    // Anything else that has these methods; the wrong kind of table; methods replaced.
    CHECK_EQ(run("function f(m, k) { return m.get(k); } for (var i = 0; i < 3000; i++) f(new Map(), i);"
                 "var r = f({ get: function (k) { return k + 1; } }, 1); try { f(Object.create(Map.prototype), 1); } catch (e) { r += ',' + e.name; }"
                 "try { Map.prototype.get.call(new Set(), 1); } catch (e) { r += ',' + e.name; }"
                 "Map.prototype.get = function () { return 'mine'; }; r + ',' + f(new Map(), 1)"),
             std::string("2,TypeError,TypeError,mine"));
    // What the table holds lives as long as it does, and comes back with a count of its own.
    CHECK_EQ(run("function f(m, i) { m.set(i & 15, { n: i }); return m.get(i & 15); } var m = new Map(), y; for (var i = 0; i < 5000; i++) y = f(m, i);"
                 "y.n + m.get(0).n"),
             std::string("9991"));
}

TEST(arrayCallbacksGoAsFarAsTheArrayReachedAtTheStart)
{
    CHECK_EQ(run("var big = []; for (var i = 0; i < 20; i++) big.push(i);"
                 "big.filter(function (x, i, arr) { arr[i + 1] = 'z'; return true; }).join('') + ',' + big.length"),
             std::string("0zzzzzzzzzzzzzzzzzzz,21"));
    CHECK_EQ(run("[1, 2, 3, 4, 5].map(function (x, i, arr) { if (i == 1) arr.pop(); return x * 2 + arr.length; }).join()"),
             std::string("7,8,10,12,"));
    CHECK_EQ(run("var q = [1, 2, 3, 4]; var seen = [];"
                 "String(q.find(function (x, i, arr) { if (i == 0) arr.length = 2; seen.push(String(x)); return false; })) + ',' + seen.join(':') +"
                 "',' + [1, 2, 3].map(function (x, i, arr) { arr.length = 1; return x; }).length"),
             std::string("undefined,1:2:undefined:undefined,3"));
    CHECK_EQ(run("[3, 'x', {}, null, 0, NaN, '', []].filter(function (x) { return x; }).length + ',' +"
                 "[1, 2, 3].reduce(function (p, x, i, arr) { return p + x * i + arr.length; }, 10)"),
             std::string("4,27"));
}

TEST(newOfAConstructorThatOnlyForwardsItsArguments)
{
    // Not small enough to go in place, so called; its object is made before it is.
    CHECK_EQ(run("function V() { this.init.apply(this, arguments); } V.prototype.init = function (x, y) { this.x = x; this.y = y; };"
                 "function mk(i) { return new V(i, i + 1); } var s = 0; for (var i = 0; i < 3000; i++) { var v = mk(i); s += v.x + v.y; }"
                 "var before = mk(1); V.prototype = { init: function (x) { this.x = -x; }, tag: 'new' }; var after = mk(5);"
                 "s + ',' + (before instanceof V) + ',' + (after instanceof V) + ',' + after.x + ',' + after.tag + ',' + before.tag"),
             std::string("9000000,false,true,-5,new,undefined"));
    // A prototype that is not an object, a constructor that returns one of its own, and a function given a property.
    CHECK_EQ(run("function V() { this.a = 1; for (var i = 0; i < 2; i++) this.a += i; } function mk() { return new V(); }"
                 "for (var i = 0; i < 3000; i++) mk(); V.prototype = 5; var p = mk(); V.extra = 1; V.prototype = { k: 2 }; var q = mk();"
                 "(Object.getPrototypeOf(p) === Object.prototype) + ',' + q.k + ',' + q.a"),
             std::string("true,2,2"));
    CHECK_EQ(run("function V(o) { this.a = 1; for (var i = 0; i < 2; i++) this.a += i; if (o) return o; } function mk(o) { return new V(o); }"
                 "for (var i = 0; i < 3000; i++) mk(); var mine = { m: 1 }; (mk(mine) === mine) + ',' + mk(5).a"),
             std::string("true,2"));
}

TEST(whatWasLearnedAboutAPrototypeThatIsGoneIsForgotten)
{
    // Each call makes a constructor whose prototype is replaced, and the old one freed, while a read of `init` through
    // the code they share remembers where it was found.
    CHECK_EQ(run("function run() {"
                 " function N(a) { var r = this.init(a); if (r) return r; } N.prototype.init = function (x) { this.x = x; }; var s = '';"
                 " var m = function (i) { return new N(i); };"
                 " for (var i = 0; i < 300; i++) { if (i === 75) N.prototype = { tag: 'late', init: function (x) { this.x = 'L' + x; } };"
                 "  try { var o = m(i); s = String(o.x) + ':' + o.tag + ':' + (o instanceof N); } catch (e) { s = e.name; } }"
                 " return s; }"
                 "var r = []; for (var k = 0; k < 40; k++) r.push(run()); r[39] + ',' + r.length"),
             std::string("L299:late:true,40"));
}

TEST(capturedVariablesOfEveryKindMadeInNativeCode)
{
    // A captured variable lives in a box, made as the function starts or the loop goes round, whatever it holds.
    CHECK_EQ(run("function mk(v, w) { var u; var n = null; var o = { k: v }; var f = function () { return [String(v), String(w), String(u), String(n), o.k]; };"
                 " v = w; return f; }"
                 "var all = []; var kinds = [1.5, 'str', true, null, undefined, { x: 1 }, [2], -0];"
                 "for (var i = 0; i < 4000; i++) { var f = mk(kinds[i & 7], kinds[(i + 3) & 7]); if (i >= 3992) all.push(f().join(':')); }"
                 "all.join('|')"),
             std::string("null:null:undefined:null:1.5|undefined:undefined:undefined:null:str|"
                         "[object Object]:[object Object]:undefined:null:true|2:2:undefined:null:|0:0:undefined:null:|"
                         "1.5:1.5:undefined:null:[object Object]|str:str:undefined:null:2|true:true:undefined:null:0"));
    CHECK_EQ(run("function counters() { var fs = []; for (var i = 0; i < 3; i++) { let j = i; fs.push(function () { return j++; }); } return fs; }"
                 "var t = 0; for (var r = 0; r < 3000; r++) { var fs = counters(); t += fs[0]() + fs[1]() + fs[2]() + fs[2](); } t"),
             std::string("18000"));
}

TEST(aLoopThatKeepsFieldsInRegistersPutsThemBackOnEveryWayOut)
{
    // Leaving for the interpreter part way round: the counter overflows after the last pass has changed both fields.
    CHECK_EQ(run("function P() { this.a = 1; this.b = 2; }"
                 "function k(o, s, n) { for (var i = s; i <= n; i++) { o.a = o.b + 1; o.b = o.a + 1; } return o.a + ',' + o.b; }"
                 "var r; for (var m = 0; m < 500; m++) r = k(new P(), 0, 1);"
                 "r + '|' + k(new P(), 2147483643, 2147483647)"),
             std::string("5,6|11,12"));
    // A field that held a string is released the first time round, and the loop carries on with it in a register.
    CHECK_EQ(run("function Q() { this.a = 's'; this.b = 2; }"
                 "function k(o, n) { for (var i = 0; i < n; i++) { o.b = o.b + 1; o.a = o.b * 2; } return o.a + ',' + o.b; }"
                 "var u; for (var m = 0; m < 300; m++) u = k(new Q(), 50); u"),
             std::string("104,52"));
    // Leaving through a value that stops being a number.
    CHECK_EQ(run("function P() { this.a = 1; this.b = 2; }"
                 "function k(o, xs) { for (var i = 0; i < xs.length; i++) { o.a = o.b + xs[i]; o.b = o.a - 1; } return o.a + ',' + o.b; }"
                 "var r; for (var m = 0; m < 300; m++) r = k(new P(), [1, 2, 3, 4, 5, 6, 7, 8]);"
                 "r + '|' + k(new P(), [1, 2, 3, 'x'])"),
             std::string("31,30|5x,NaN"));
}

TEST(aCounterCapturedByAClosureCountsExactly)
{
    // Two closures of one function, inlined; then one of them reached through a function that returns a string.
    CHECK_EQ(run("function mk() { var c = 0; return function (d) { c += d; return c; }; }"
                 "function k(n, f, g) { var s = 0; for (var i = 0; i < n; i++) s = f(i & 1) + g(1); return s; }"
                 "var v; for (var j = 0; j < 50; j++) v = k(1000, mk(), mk());"
                 "var f = mk(), g = mk(); k(100, f, g);"
                 "v + '|' + k(10, function (d) { return f(d) + '!'; }, g)"),
             std::string("1500|55!110"));
}

TEST(whatALoopReadsThroughAGlobalIsCheckedBeforeItAndStillHolds)
{
    // An array in a global, read in a loop; then another array; then something that is not one.
    CHECK_EQ(run("var os = [{ x: 1 }, { y: 0, x: 2 }];"
                 "function k(n) { var s = 0; for (var i = 0; i < n; i++) s += os[i & 1].x; return s; }"
                 "var r = 0; for (var m = 0; m < 200; m++) r += k(100);"
                 "os = [{ x: 10 }, { x: 20 }]; r += k(10);"
                 "os = { length: 0 }; try { r += k(2); } catch (e) { r += 'E'; } r"),
             std::string("30150E"));
    // A method of a global object, replaced once the loop is compiled.
    CHECK_EQ(run("function h(n) { var s = 0; for (var i = 0; i < n; i++) s += Math.sqrt(i); return Math.round(s); }"
                 "var a = 0; for (var m = 0; m < 100; m++) a = h(100);"
                 "var keep = Math.sqrt; Math.sqrt = function (x) { return 1; }; var b = h(100); Math.sqrt = keep; a + '|' + b"),
             std::string("661|100"));
    // A constructor made in place, then a setter on its prototype for a field it adds.
    CHECK_EQ(run("function T(l, r) { this.l = l; this.r = r; }"
                 "function mk(d) { return d === 0 ? new T(null, null) : new T(mk(d - 1), mk(d - 1)); }"
                 "function chk(t) { return t.l === null ? 1 : 1 + chk(t.l) + chk(t.r); }"
                 "var c = 0; for (var m = 0; m < 50; m++) c += chk(mk(6));"
                 "Object.defineProperty(T.prototype, 'r', { set: function (v) { this.rr = v; }, get: function () { return this.rr; } });"
                 "c + '|' + Object.keys(new T(1, 2)).join()"),
             std::string("6350|l,rr"));
    // The same, where native code made every object of that prototype: it is a prototype all the same.
    CHECK_EQ(run("function T(l, r) { this.l = l; this.r = r; } function f() { return new T(1, 2); }"
                 "var c = 0; for (var m = 0; m < 500; m++) c += f().l;"
                 "Object.defineProperty(T.prototype, 'r', { set: function (v) { this.rr = v; }, get: function () { return this.rr; } });"
                 "c + '|' + Object.keys(f()).join()"),
             std::string("500|l,rr"));
}

TEST(aSortOfNumbersAsksTheComparatorWhatItWouldSay)
{
    // A difference either way needs no call; anything else is called, its answer made a number, its throw let through
    // with the array as it was. Several rounds, so that the comparators are compiled for the later ones.
    CHECK_EQ(run("var xs = [3, -0, 0, Infinity, -Infinity, 2, 1e300, -1e300, 5, 0.5, -2];"
                 "function show(a) { return a.map(function (x) { return Object.is(x, -0) ? '-0' : String(x); }).join(); }"
                 "var r = [];"
                 "for (var k = 0; k < 30; k++) {"
                 " r.push(show(xs.slice().sort(function (p, q) { return p - q; })));"
                 " r.push(show(xs.slice().sort(function (p, q) { return q - p; })));"
                 " r.push(show(xs.slice().sort(function (p, q) { return String(p - q); })));"
                 " var ys = xs.slice();"
                 " try { ys.sort(function (p, q) { if (p === 5 || q === 5) throw 'x'; return p - q + 0; }); } catch (e) { r.push(e + show(ys)); } }"
                 "r.slice(-4).join('|')"),
             std::string("-Infinity,-1e+300,-2,-0,0,0.5,2,3,5,1e+300,Infinity|Infinity,1e+300,5,3,2,0.5,-0,0,-2,-1e+300,-Infinity|"
                         "-Infinity,-1e+300,-2,-0,0,0.5,2,3,5,1e+300,Infinity|x3,-0,0,Infinity,-Infinity,2,1e+300,-1e+300,5,0.5,-2"));
}

TEST(whatEveryWayIntoAJoinKnowsOfATagStaysKnown)
{
    // Numbers, booleans, strings and objects from both arms of a choice, and from every return of an inlined function:
    // known to be what they are, and counted, or not, as such.
    CHECK_EQ(run("function f(i, x) { var d = (i & 1 ? -1 : 1) * x; return d; }"
                 "function g(i) { return (i & 4) != 0 || i == 3; }"
                 "function h(i) { return i & 1 ? 'a' + i : 'b'; }"
                 "function k(i) { return i & 1 ? { v: i } : { v: -i }; }"
                 "var s = 0, t = 0, u = '', w = 0;"
                 "for (var i = 0; i < 3000; i++) { s += f(i, 1.5); if (g(i)) t++; u = h(i); w += k(i).v; }"
                 "s + '|' + t + '|' + u + '|' + w"),
             std::string("0|1501|a2999|1500"));
}

TEST(callbacksOfArrayMethodsCalledFromNativeCode)
{
    // What `forEach`, `map`, `filter` and `reduce` do when the callback changes the array, drops what it was given, throws
    // or is no function: the same from the first round, interpreted, to the last, in native code.
    CHECK_EQ(run(
                 "function show(a){return JSON.stringify(a);}\n"
                 "function run(k){\n"
                 " var r=[];\n"
                 " // numbers, objects, strings, undefined\n"
                 " r.push(show([1,2,3].map(function(x,i,a){return x*2+i+a.length;})));\n"
                 " r.push(show([{v:1},{v:2}].map(function(o){return o.v;})));\n"
                 " r.push(show(['a','b'].map(function(s){return s+s;})));\n"
                 " r.push(show([1,undefined,3].map(function(x){return x===undefined?'u':x;})));\n"
                 " r.push(show([1,2,3,4].filter(function(x){return x&1;})));\n"
                 " r.push(show([{v:1},{v:0},{v:2}].filter(function(o){return o.v;}).map(function(o){return o.v;})));\n"
                 " r.push(show(['',\"x\",0,null,NaN,{},[]].filter(function(x){return x;})));\n"
                 " r.push(String([1,2,3].reduce(function(p,x){return p+x;},10)));\n"
                 " r.push(String([1,2,3].reduce(function(p,x,i,a){return p+'/'+x+i+a.length;},'s')));\n"
                 " r.push(show([{v:1},{v:2}].reduce(function(p,o){return {v:p.v+o.v};},{v:0})));\n"
                 " var seen=0; [5,6].forEach(function(x,i){seen+=x*i;}); r.push(String(seen));\n"
                 " var t={m:3}; r.push(show([1,2].map(function(x){return x*this.m;},t)));\n"
                 " var u=0; [1].forEach(function(){u=this.q;},{q:7}); r.push(String(u));\n"
                 " var w; [1].forEach(function(){'use strict';w=this;}); r.push(String(w));\n"
                 " // the callback changes the array\n"
                 " var a=[1,2,3,4]; var seen2=[]; a.forEach(function(x){seen2.push(x); if(x==1){a.pop();a.pop();}}); r.push(show(seen2)+show(a));\n"
                 " a=[1,2]; seen2=[]; a.forEach(function(x){seen2.push(x); if(a.length<5)a.push(x+10);}); r.push(show(seen2)+show(a));\n"
                 " a=[1,2,3]; r.push(String(a.map(function(x){a.length=1;return x;}).length));\n"
                 " a=[1,2,3]; r.push(show(a.filter(function(x){a[2]='s';return true;})));\n"
                 " a=[{v:1},{v:2}]; r.push(String(a.map(function(o,i){a[0]=null;a[1]=null;return o?o.v:'n';}).join()));\n"
                 " a=[1,2,3]; r.push(String(a.reduce(function(p,x){a.length=2;return p+x;},0)));\n"
                 " a=[[1],[2]]; r.push(String(a.reduce(function(p,x){a.pop();return p+x[0];},0)));\n"
                 " // thrown through\n"
                 " try { [1,2,3].map(function(x){if(x==2)throw 'e'+x;return x;}); } catch(e){ r.push(e); }\n"
                 " try { [{},{}].filter(function(o){throw o===null?'n':'f';}); } catch(e){ r.push(e); }\n"
                 " try { [1].reduce(function(p,x){throw 'r';},{}); } catch(e){ r.push(e); }\n"
                 " // not what was guessed\n"
                 " try { [1].map(3); } catch(e){ r.push(e.name); }\n"
                 " try { [1].forEach(undefined); } catch(e){ r.push(e.name); }\n"
                 " try { r.push(String(Array.prototype.map.call({length:2,0:'a',1:'b'},function(x){return x;}).length)); } catch(e){ r.push(e.name); }\n"
                 " r.push(String([3,4].map(Math.sqrt).length));\n"
                 " r.push(show([1,4].map(Number)));\n"
                 " // grown past a cell's room, and mixed kinds in the result\n"
                 " var big=[]; for(var i=0;i<40;i++) big.push(i);\n"
                 " r.push(String(big.map(function(x){return x&1?x:'s'+x;}).length));\n"
                 " r.push(String(big.filter(function(x){return x%3==0;}).reduce(function(p,x){return p+x;},0)));\n"
                 " return r.join('|');\n"
                 "}\n"
                 "var last=''; var bad=0;\n"
                 "for (var k=0;k<400;k++){ var now; try { now=run(k); } catch(e){ now='threw '+e; } if(k&&now!==last) bad++; last=now; }\n"
                 "bad + ' ' + last;\n"),
             std::string("0 [5,8,11]|[1,2]|[\"aa\",\"bb\"]|[1,\"u\",3]|[1,3]|[1,2]|[\"x\",{},[]]|16|s/103/213/323|{\"v\":3}|6|[3,6]|7|undefined|"
                         "[1,2][1,2]|[1,2][1,2,11,12]|3|[1,2,\"s\"]|1,n|3|1|e2|f|r|TypeError|TypeError|2|2|[1,4]|40|273"));
}

TEST(classesAsTheLanguageHasThem)
{
    CHECK_EQ(run("var log = [];"
                 "class A { #x = 1; y = 2; static s = 3; static #p = 4; #m() { return this.#x; } get #g() { return 10; } set #g(v) { this.#x = v; } static #sm() { return 5; }"
                 " static get P() { return A.#p + A.#sm(); } t() { this.#g = 7; return [this.#x, this.#m(), this.#g, #x in this, #x in {}].join(); }"
                 " ['comp' + 1] = 9; get ['acc']() { return 'A'; } static { log.push('block', this.s); } }"
                 "log.push(new A().t(), A.P, JSON.stringify(new A()), new A().acc, Object.keys(new A()).join());"
                 "try { A.prototype.t.call({}); } catch (e) { log.push(e.name); }"
                 "class B extends A { constructor() { try { this; } catch (e) { log.push(e.name); } super(); this.z = 1; log.push(new.target === B); } }"
                 "log.push(new B().z, new B().y);"
                 "class Arr extends Array { sum() { return this.reduce(function (a, b) { return a + b; }, 0); } }"
                 "var ar = new Arr(); ar.push(1, 2); log.push(ar.sum(), ar instanceof Arr, Array.isArray(ar), ar.length);"
                 "class E1 extends Error { constructor(m) { super(m); this.k = 1; } } var e1 = new E1('hi'); log.push(e1.message, e1 instanceof E1, e1 instanceof Error);"
                 "class C1 extends A { constructor() { var f = () => super(); f(); try { f(); } catch (e) { log.push(e.name); } } } log.push(new C1().y);"
                 "class D1 extends A { constructor() { return { o: 1 }; } } log.push(new D1().o);"
                 "class D2 extends A { constructor() { } } try { new D2(); } catch (e) { log.push(e.name); }"
                 "class F1 { a = this; b = () => this; static x = this; } var f1 = new F1(); log.push(f1.a === f1, f1.b() === f1, F1.x === F1);"
                 "function Old() { log.push(new.target === Old, new.target === undefined); } new Old(); Old();"
                 "var o = { get ['x' + 1]() { return 1; }, set ['x' + 1](v) { } }; log.push(o.x1);"
                 "log.join('|')"),
             std::string("block|3|7,7,10,true,false|9|{\"y\":2,\"comp1\":9}|A|y,comp1|TypeError|ReferenceError|true|ReferenceError|true|1|2|3|true|true|2|hi|true|true|"
                         "ReferenceError|2|1|ReferenceError|true|true|true|true|false|false|true|1"));
    CHECK(messageOf("class A { m() { return this.#nope; } }").find("#nope") != std::string::npos);
    CHECK(messageOf("class A { #x; #x; }").find("#x") != std::string::npos);
}

TEST(generatorsStopAndCarryOn)
{
    CHECK_EQ(run("var log = [];"
                 "function* g(a) { var x = yield a; try { var y = yield x + 1; return y * 2; } finally { log.push('fin'); } }"
                 "var i = g(1); log.push(JSON.stringify([i.next(), i.next(10), i.next(21), i.next()]));"
                 "var j = g(1); j.next(); j.next(5); log.push(JSON.stringify(j.return(9)));"
                 "var k = g(1); k.next(); k.next(5); try { k.throw('boom'); } catch (e) { log.push(e); }"
                 "function* d() { var s = yield* g(7); log.push('s' + s); yield* [1, 2]; return 'end'; }"
                 "log.push([...d()].join());"
                 "log.join('|')"),
             std::string("fin|[{\"value\":1,\"done\":false},{\"value\":11,\"done\":false},{\"value\":42,\"done\":true},{\"done\":true}]|"
                         "fin|{\"value\":9,\"done\":true}|fin|boom|fin|sNaN|7,NaN,1,2"));
    // A `for-of` closes its iterator on the way out however it leaves, and not when it runs out.
    CHECK_EQ(run("var log = [];"
                 "function* it(n) { try { yield 1; yield 2; } finally { log.push(n); } }"
                 "for (var v of it('break')) break;"
                 "(function () { for (var v of it('return')) return v; })();"
                 "try { for (var v of it('throw')) throw 'x'; } catch (e) { log.push(e); }"
                 "outer: for (var a of [1]) { for (var v of it('outer')) continue outer; }"
                 "for (var v of it('out')) ;"
                 "var plain = 0; function sum(a) { var s = 0; for (var v of a) s += v; return s; }"
                 "for (var n = 0; n < 2000; n++) plain = sum([1, 2, 3]);"
                 "log.join() + '|' + sum(it('notArray')) + '|' + log.length"),
             std::string("break,return,throw,x,outer,out|3|7"));
    CHECK_EQ(run("function* g() {} var e = ''; try { new g(); } catch (x) { e = x.name; }"
                 "Object.prototype.toString.call(g()) + e + (Object.getPrototypeOf(g()) === g.prototype)"),
             std::string("[object Generator]TypeErrortrue"));
}

TEST(asyncFunctionsWaitAndComeBack)
{
    // What the jobs do is seen from a global, read once the host has run them.
    script::Context ctx;
    ctx.eval("var log = [];"
             "async function af(x) { var a = await x; var b = await new Promise(function (r) { r(a + 1); });"
             " try { await Promise.reject('rj'); } catch (e) { b += e; } return b; }"
             "af(1).then(function (v) { log.push('af' + v); });"
             "async function bad() { throw 'no'; } bad().catch(function (e) { log.push(e); });"
             "async function* ag() { var s = yield 1; log.push('sent' + s); return 4; }"
             "var it = ag(); it.next().then(function (r) { log.push(r.value); return it.next('S'); })"
             " .then(function (r) { log.push(r.value + '' + r.done); });"
             "(async () => { for await (var y of [Promise.resolve(5), 6]) log.push(y); })();"
             "log.push('sync');",
             "<t>.js", script::Language::JavaScript);
    ctx.runJobs();
    const std::string all = ctx.eval("log.slice().sort().join()", "<t>.js", script::Language::JavaScript).toString();
    CHECK_EQ(all, std::string("1,4true,5,6,af2rj,no,sentS,sync"));
}

TEST(regularExpressionsMatchAsTheLanguageSaysTheyDo)
{
    js::Context ctx;
    const auto text = [&](const char* source) { return ctx.eval(source, "<test>").toString(); };
    CHECK_EQ(text("String(/a(b+)c/.exec('xabbbc'))"), std::string("abbbc,bbb"));
    CHECK_EQ(text("String(/(z)((a+)?(b+)?(c))*/.exec('zaacbbbcac'))"), std::string("zaacbbbcac,z,ac,a,,c"));
    CHECK_EQ(text("/(?<y>\\d{4})-(?<m>\\d\\d)/.exec('on 2026-11').groups.m"), std::string("11"));
    CHECK_EQ(text("/(?<=\\$)\\d+/.exec('cost $42')[0]"), std::string("42"));
    CHECK_EQ(text("String(/(?=(a+))a*b\\1/.exec('baaabac'))"), std::string("aba,a"));
    CHECK_EQ(text("'aBc'.replace(/b/i, '[$&]')"), std::string("a[B]c"));
    CHECK_EQ(text("'a1b22'.replace(/\\d+/g, m => m.length)"), std::string("a1b2"));
    CHECK_EQ(text("'a-b_c'.split(/[-_]/).join('+')"), std::string("a+b+c"));
    CHECK_EQ(text("'abc'.split(/(?:)/).length + ''"), std::string("3"));
    CHECK_EQ(text("[...'a1b2'.matchAll(/\\d/g)].map(m => m.index).join()"), std::string("1,3"));
    CHECK_EQ(text("/^.$/u.test('\\u{1F600}') + ',' + /^.$/.test('\\u{1F600}')"), std::string("true,false"));
#if SCRIPT_UNICODE_PROPERTIES
    CHECK_EQ(text("/[\\p{L}--[a-z]]/v.test('a') + ',' + /[\\p{L}&&[a-z]]/v.test('a')"), std::string("false,true"));
#endif
    CHECK_EQ(text("/(?i:a)b/.test('Ab') + ',' + /(?i:a)b/.test('AB')"), std::string("true,false"));
    CHECK_EQ(text("var r = /a/g; r.exec('aa'); r.lastIndex + ',' + r.flags + ',' + r"), std::string("1,g,/a/g"));
    CHECK_EQ(text("var x = 4, g = 2; String(x / 2 / g) + (x /g/ 1)"), std::string("12"));
    CHECK_EQ(text("var hit = 0; if (x) /re/.test('re') && hit++; {}/re/.test('re') && hit++; hit + ''"), std::string("2"));
}

TEST(regularExpressionsTheProgramHasInterferedWithDoWhatItAsks)
{
    js::Context ctx;
    CHECK_EQ(ctx.eval("var r = /a/g; r.exec = function () { return null; }; 'aaa'.replace(r, 'b')", "<test>").toString(), std::string("aaa"));
    CHECK_EQ(ctx.eval("class Upper extends RegExp { exec(s) { var m = super.exec(s); if (m) m[0] = m[0].toUpperCase(); return m; } }"
                      "'xay'.match(new Upper('a'))[0]", "<test>").toString(), std::string("A"));
    CHECK_EQ(ctx.eval("var o = /a/; Object.defineProperty(o, 'lastIndex', { writable: false }); var threw = false;"
                      "try { 'a'.match(/a/g); /a/g.exec('a'); var f = /a/g; Object.freeze(f); f.exec('a'); } catch (e) { threw = e instanceof TypeError; } threw + ''", "<test>").toString(), std::string("true"));
    CHECK_EQ(ctx.eval("var saved = RegExp.prototype.exec; var calls = 0;"
                      "RegExp.prototype.exec = function (s) { calls++; return saved.call(this, s); };"
                      "/a/.test('a'); 'a'.replace(/a/, 'b'); 'a'.split(/a/); RegExp.prototype.exec = saved; String(calls > 2)", "<test>").toString(), std::string("true"));
    CHECK_EQ(ctx.eval("try { new RegExp('('); } catch (e) { e.name }", "<test>").toString(), std::string("SyntaxError"));
}

TEST(typedArraysAreViewsOfOneBuffer)
{
    js::Context ctx;
    const auto text = [&](const char* source) { return ctx.eval(source, "<test>").toString(); };
    CHECK_EQ(text("var b = new ArrayBuffer(8); var i = new Int32Array(b); i[1] = -1; var u = new Uint8Array(b, 4, 2); u[0] + ',' + u.length + ',' + new DataView(b).getInt32(4, true)"), std::string("255,2,-1"));
    CHECK_EQ(text("String(new Uint8ClampedArray([300, -1, 1.5, 2.5]))"), std::string("255,0,2,2"));
    CHECK_EQ(text("var f = new Float64Array(2); f[5] = 1; f[1] = 0.5; [f[5], f.join('|'), 5 in f].join()"), std::string(",0|0.5,false"));
    CHECK_EQ(text("var r = new ArrayBuffer(4, { maxByteLength: 16 }); var t = new Uint8Array(r); r.resize(8); var n = r.transfer(); [t.length, r.detached, n.byteLength].join()"), std::string("0,true,8"));
    CHECK_EQ(text("String(new BigInt64Array([1n, -2n])) + new Uint8Array([72, 105]).toHex()"), std::string("1,-24869"));
    // Native code reads and writes each kind, and finds a buffer that shrank or went away.
    CHECK_EQ(text("function rd(x, i) { return x[i]; } function wr(x, i, v) { x[i] = v; }"
                  "var kinds = [Int8Array, Uint8Array, Int16Array, Uint16Array, Int32Array, Uint32Array, Float32Array, Float64Array];"
                  "var out = []; for (var k of kinds) { var a = new k(4); for (var r = 0; r < 3000; r++) { wr(a, r & 3, r - 1500.5); rd(a, r & 3); } out.push(rd(a, 3)); } out.join()"),
             std::string("-38,218,1498,1498,1498,1498,1498.5,1498.5"));
    CHECK_EQ(text("var rb = new ArrayBuffer(16, { maxByteLength: 32 }); var iv = new Int32Array(rb); iv[3] = 9;"
                  "for (var r = 0; r < 3000; r++) rd(iv, 3); rb.resize(4); String(rd(iv, 3)) + iv.length"), std::string("undefined1"));
    CHECK_EQ(text("function sum(x) { var s = 0; for (var k = 0; k < x.length; k++) s += x[k]; return s; }"
                  "var big = new Float64Array(100).fill(2); for (var q = 0; q < 3000; q++) sum(big); sum(big) + ',' + sum([4, 5])"), std::string("200,9"));
}

TEST(bigIntsAreIntegersOfAnySize)
{
    js::Context ctx;
    const auto text = [&](const char* source) { return ctx.eval(source, "<test>").toString(); };
    CHECK_EQ(text("String(2n ** 64n)"), std::string("18446744073709551616"));
    CHECK_EQ(text("[-7n / 2n, -7n % 2n, (-5n) >> 1n, ~5n, 5n & -2n, BigInt.asIntN(8, 255n)].join()"), std::string("-3,-1,-3,-6,4,-1"));
    CHECK_EQ(text("[1n == 1, 2n > 1, '3' == 3n, typeof 1n].join()"), std::string("true,true,true,bigint"));
    CHECK_EQ(text("var e; try { 1n + 1; } catch (x) { e = x.name; } e"), std::string("TypeError"));
    CHECK_EQ(text("function m(a, b) { return a * b; } for (var i = 0; i < 20000; i++) m(i, 2); String(m(3n, 4n))"), std::string("12"));
}

TEST(evalIsOffUnlessTheHostTurnsItOn)
{
    js::Context off;
    CHECK_EQ(off.eval("var name; try { eval('1'); } catch (e) { name = e.name; } name", "<test>").toString(), std::string("EvalError"));
    CHECK_EQ(off.eval("var name2; try { new Function('return 1'); } catch (e) { name2 = e.name; } name2", "<test>").toString(), std::string("EvalError"));

    script::Context::Options options;
    options.runtimeCode = true;
    script::Context on(options);
    const auto text = [&](const char* source) { return on.eval(source, "<test>.js", script::Language::JavaScript).toString(); };
    CHECK_EQ(text("function f(a) { var b = 10; return eval('a + b'); } String(f(5))"), std::string("15"));
    CHECK_EQ(text("function g() { var x = 1; eval('x = 42'); return x; } String(g())"), std::string("42"));
    CHECK_EQ(text("function h() { var x = 3; return function () { return eval('x * 2'); }; } String(h()())"), std::string("6"));
    CHECK_EQ(text("function t() { return eval('this.k + arguments.length'); } t.call({ k: 'k' }, 1, 2)"), std::string("k2"));
    CHECK_EQ(text("var indirect = eval; function l() { var q = 1; try { return indirect('q'); } catch (e) { return e.name; } } l()"), std::string("ReferenceError"));
    CHECK_EQ(text("String(new Function('a', 'b', 'return a * b')(6, 7))"), std::string("42"));
    CHECK_EQ(text("var n; try { eval('1 +'); } catch (e) { n = e.name; } n"), std::string("SyntaxError"));
}

TEST(aProxyAnswersThroughItsHandler)
{
    js::Context ctx;
    const auto text = [&](const char* source) { return ctx.eval(source, "<test>").toString(); };
    CHECK_EQ(text("var p = new Proxy({ a: 1 }, { get(t, k) { return k in t ? t[k] : 42; }, set(t, k, v) { t[k] = v * 2; return true; } }); p.b = 5; [p.a, p.x, p.b].join()"), std::string("1,42,10"));
    CHECK_EQ(text("var f = new Proxy(function (a, b) { return a + b; }, { apply(t, self, args) { return t(...args) * 10; } }); typeof f + f(1, 2)"), std::string("function30"));
    CHECK_EQ(text("var r = Proxy.revocable({}, {}); r.revoke(); var n; try { r.proxy.x; } catch (e) { n = e.name; } n"), std::string("TypeError"));
    CHECK_EQ(text("var fixed = {}; Object.defineProperty(fixed, 'k', { value: 1 }); var m; try { new Proxy(fixed, { get() { return 2; } }).k; } catch (e) { m = e.name; } m"), std::string("TypeError"));
    CHECK_EQ(text("Object.keys(new Proxy({}, { ownKeys() { return ['x', 'y']; }, getOwnPropertyDescriptor() { return { value: 0, enumerable: true, configurable: true }; } })).join()"), std::string("x,y"));
    CHECK_EQ(text("var inherits = Object.create(new Proxy({}, { get(t, k) { return 'up ' + String(k); } })); inherits.here"), std::string("up here"));
}

TEST(arraysHaveHolesAndPropertiesAtIndices)
{
    js::Context ctx;
    const auto text = [&](const char* source) { return ctx.eval(source, "<test>").toString(); };
    CHECK_EQ(text("var a = [1, , 3]; [1 in a, a.indexOf(undefined), a.includes(undefined), Object.keys(a).join('')].join()"), std::string("false,-1,true,02"));
    CHECK_EQ(text("var c = []; c[5] = 1; var n = 0; c.forEach(() => n++); [4 in c, n, c.length].join()"), std::string("false,1,6"));
    CHECK_EQ(text("var u = [1, 2]; u[1] = undefined; String(1 in u)"), std::string("true"));
    CHECK_EQ(text("var g = [1, 2, 3]; Object.defineProperty(g, '1', { get() { return 'got'; } }); g[1] + g.length"), std::string("got3"));
    CHECK_EQ(text("String([3, , 1, undefined].sort().length)"), std::string("4"));
}

TEST(aFunctionsPrototypeIsMadeWhenFirstAskedFor)
{
    js::Context ctx;
    const auto text = [&](const char* source) { return ctx.eval(source, "<test>").toString(); };
    CHECK_EQ(text("function F(a) { this.a = a; } var f = new F(1); [f instanceof F, F.prototype.constructor === F, f.a].join()"), std::string("true,true,1"));
    CHECK_EQ(text("Object.getOwnPropertyNames(function g() {}).join()"), std::string("length,name,prototype"));
    CHECK_EQ(text("String((function () {}).hasOwnProperty('prototype'))"), std::string("true"));
    CHECK_EQ(text("Object.defineProperty(Function.prototype, 'prototype', { get() { return 100; }, configurable: true });"
                  "var p = (function () {}).prototype; delete Function.prototype.prototype; String(p !== 100)"), std::string("true"));
    CHECK_EQ(text("function H() {} H.prototype.m = function () { return 7; }; String(new H().m())"), std::string("7"));
}

TEST(aClosureThatCallsItselfKeepsNoCycle)
{
    js::Context ctx;
    ctx.eval("function run() { var out = 0; for (var i = 0; i < 3; i++) { var loop = function (n) { return n ? loop(n - 1) + 1 : i; }; out += loop(2); } return out; }"
             "for (var r = 0; r < 20000; r++) run();", "<test>");
    CHECK_EQ(ctx.eval("String(run())", "<test>").toString(), std::string("9"));
    // The dynamic engine breaks that cycle as it is made; the other core leaves it to its collector.
    if (!otherCore())
        CHECK(ctx.stats().gcRuns == 0);
    // Kept past its loop, it is still itself.
    CHECK_EQ(ctx.eval("function keep() { var kept = []; for (var i = 0; i < 3; i++) { var q = function () { return q; }; kept.push(q); } return kept[0]() === kept[2]; } String(keep())", "<test>").toString(),
             std::string("true"));
}

TEST(aDelegateOfAnAsyncGeneratorThatThrowsIsTheGeneratorsToCatch)
{
    script::Context ctx;
    ctx.eval("var log = [];"
             "var it = { [Symbol.asyncIterator]() { return this; }, next() { return { get value() { throw 'vt'; }, done: false }; } };"
             "async function* g() { try { yield* it; } catch (e) { log.push('c' + e); yield 'after'; } }"
             "var i = g(); i.next().then(function (r) { log.push(r.value); return i.next(); }).then(function (r) { log.push(r.done); });"
             "async function* h() { yield* it; } h().next().catch(function (e) { log.push('rej' + e); });",
             "<t>.js", script::Language::JavaScript);
    ctx.runJobs();
    CHECK_EQ(ctx.eval("log.slice().sort().join()", "<t>.js", script::Language::JavaScript).toString(), std::string("after,cvt,rejvt,true"));
}

TEST(aConstructorKnownOnEveryWayIntoAJoinIsKnownAfterIt)
{
    CHECK_EQ(run("function P(a, d) { this.a = a; this.d = d; }"
                 "function build(n) { return n === 0 ? null : new P(n, n & 1 ? new P(-n, null) : build(n - 1)); }"
                 "function count(p) { var c = 0; while (p) { c += p.a; p = p.d; } return c; }"
                 "var t = 0; for (var i = 0; i < 3000; i++) t += count(build(i & 15));"
                 "var Q = P; P = function (a, d) { this.a = 2 * a; this.d = d; }; P.prototype = Q.prototype;"
                 "t + ',' + count(build(4)) + ',' + (build(3) instanceof Q)"),
             std::string("10484,8,true"));
}

TEST(callingThroughCallIsCalling)
{
    CHECK_EQ(run("function g(a, b) { return this.k + a * b; } function f(o, a) { return g.call(o, a, 2); }"
                 "var y; for (var i = 0; i < 3000; i++) y = f({ k: 1 }, i); y"),
             std::string("5999"));
    // No arguments, too few, too many, and `this` left to the callee to make sense of.
    CHECK_EQ(run("function g(a, b) { return String(this === globalThis) + a + b + arguments.length; }"
                 "function f(x) { return g.call(undefined, x) + g.call(null) + g.call(undefined, 1, 2, 3); }"
                 "var y; for (var i = 0; i < 3000; i++) y = f(i); y"),
             std::string("true2999undefined1trueundefinedundefined0true123"));
    CHECK_EQ(run("function g() { return this === globalThis; } function f(t) { return g.call(t); } var y; for (var i = 0; i < 3000; i++) y = f(null); y + ',' + f(undefined) + ',' + f({})"),
             std::string("true,true,false"));
    // Not a function, or a `call` that is not the built-in; and a built-in called through it.
    CHECK_EQ(run("function f(o) { return o.call(1, 2); } function g(a) { return a; } for (var i = 0; i < 3000; i++) f(g);"
                 "var r = f({ call: function (x, y) { return x + y; } }); try { f({ call: 5 }); } catch (e) { r += ':' + e.name; } r"),
             std::string("3:TypeError"));
    // A function called through `call` and put in place of it, then `call` itself replaced.
    CHECK_EQ(run("function run() { var v = function (a, b) { return String(a) + String(b) + arguments.length; };"
                 " var c = function () { return v.call(null, {}, 2.5); }; var y; for (var i = 0; i < 300; i++) y = c();"
                 " v.call = function () { return 'own'; }; return y + ',' + c(); }"
                 "var r; for (var i = 0; i < 40; i++) r = run(); r"),
             std::string("[object Object]2.52,own"));
    CHECK_EQ(run("function f(s) { return String.prototype.toUpperCase.call(s); } var y; for (var i = 0; i < 3000; i++) y = f('ab'); y"),
             std::string("AB"));
    CHECK_EQ(run("function g() { return arguments.length; } function f() { return g.call(this, arguments); } var y; for (var i = 0; i < 3000; i++) y = f(1, 2); y"),
             std::string("1"));
    CHECK_EQ(run("function g(a) { throw new Error(String(a)); } function f(a) { try { g.call(null, a); } catch (e) { return e.message; } } var y; for (var i = 0; i < 3000; i++) y = f(i); y"),
             std::string("2999"));
}

TEST(pushGrowingOrChangingAnArrayInPlace)
{
    CHECK_EQ(run("function f(a, v) { return a.push(v); } var s = 0;"
                 "for (var i = 0; i < 3000; i++) { var a = []; for (var j = 0; j < 20; j++) s += f(a, j & 1 ? j : 'x' + j); }"
                 "var b = [1, 2]; f(b, 3); f(b, 'four'); f(b, null); f(b, undefined); s + ',' + b.join() + ',' + b.length"),
             std::string("630000,1,2,3,four,,,6"));
    CHECK_EQ(run("function f(a, v) { return a.push(v); } for (var i = 0; i < 3000; i++) f([], {});"
                 "var r = ''; try { f(Object.freeze([]), 1); } catch (e) { r = e.name; } r"),
             std::string("TypeError"));
    // A value pushed by way of the path that grows the array, in each form the compiler may have it.
    CHECK_EQ(run("function f(a, x) { a.push(x * 2); a.push(7); a.push(x | 0); a.push(true); return a; }"
                 "var y; for (var i = 0; i < 3000; i++) y = f([], i); y.join() + ',' + f(new Array(), 1.5).join()"),
             std::string("5998,7,2999,true,3,7,1,true"));
    // What it holds keeps it alive; what it replaces in a full array does not go.
    CHECK_EQ(run("function f(a, v) { a.push(v); } var a = []; for (var i = 0; i < 5000; i++) f(a, { n: i }); a[4999].n + a[17].n"),
             std::string("5016"));
}

TEST(aStoreMayAddAPropertyToObjectsOfSeveralShapes)
{
    // Fields that arrive in either order, through one store each, and objects that have them already.
    CHECK_EQ(run("function N() {} N.prototype.l = null; N.prototype.r = null;"
                 "function setL(n, v) { n.l = v; } function setR(n, v) { n.r = v; }"
                 "var s = 0; for (var i = 0; i < 6000; i++) { var a = new N();"
                 " if (i & 1) { setL(a, 1); setR(a, 2); } else { setR(a, 3); setL(a, 4); }"
                 " if (i & 2) { setL(a, 5); setR(a, 6); }"
                 " s += a.l * 10 + a.r; if (Object.keys(a).length != 2) s = -1e9; }"
                 "s + ',' + Object.keys((function () { var b = new N(); setR(b, 1); setL(b, 1); return b; })()).join()"),
             std::string("250500,r,l"));
    // Something on the prototype chain that changes what a store does: what the site learnt no longer holds.
    CHECK_EQ(run("function N() {} function set(n, v) { n.x = v; return n; }"
                 "var seen = 0; var s = 0; for (var i = 0; i < 4000; i++) { var a = new N(); if (i & 1) a.y = 1; set(a, 2); s += a.x; }"
                 "Object.defineProperty(N.prototype, 'x', { set: function (v) { seen = v; }, get: function () { return 7; } });"
                 "var a = new N(); set(a, 9); var b = new N(); b.y = 1; set(b, 8);"
                 "s + ',' + seen + ',' + a.x + ',' + Object.keys(a).length + ',' + Object.keys(b).length"),
             std::string("8000,8,7,0,1"));
    // More shapes than a site keeps, and one of them frozen.
    CHECK_EQ(run("function set(n, v) { n.z = v; return n; }"
                 "var s = 0; for (var i = 0; i < 4000; i++) { var a = {}; a['p' + (i & 7)] = 1; set(a, i & 1); s += a.z; }"
                 "var f = Object.freeze({ a: 1 }); try { set(f, 1); } catch (e) {}"
                 "s + ',' + f.z"),
             std::string("2000,undefined"));
}

TEST(aProgramMayHaveAnyNumberOfWaysToGrowAnObject)
{
    // More classes with statics, and literals starting differently, than a shape's limit for names worked out at run time.
    std::string source = "var all = [], lits = [];";
    for (int i = 0; i < 200; ++i) {
        const std::string n = std::to_string(i);
        source += "function C" + n + "() {} C" + n + ".first" + n + " = " + n + "; C" + n + ".common = " + n + "; all.push(C" + n + ");";
        source += "lits.push({ a" + n + ": " + n + ", b: 1 });";
    }
    source += "function read(c) { return c.common; } function lit(o) { return o.b; }"
              "var s = 0; for (var r = 0; r < 200; r++) for (var i = 0; i < 200; i++) s += read(all[i]) + lit(lits[i]);"
              "s + ',' + C7.first7 + ',' + C199.first199 + ',' + lits[150].a150 + ',' + Object.keys(C3).join();";
    CHECK_EQ(run(source.c_str()), std::string("4020000,7,199,150,first3,common"));
    // Keys worked out at run time still make dictionaries that behave.
    CHECK_EQ(run("var ds = []; for (var i = 0; i < 500; i++) { var d = {}; d['k' + i] = i; d.common = 1; ds.push(d); }"
                 "var s = 0; for (var i = 0; i < 500; i++) { s += ds[i]['k' + i] + ds[i].common; delete ds[i].common; }"
                 "s + ',' + ds[9].common + ',' + Object.keys(ds[499]).join()"),
             std::string("125250,undefined,k499"));
}

TEST(aCheckWithAWayRoundItProvesNothingAfterwards)
{
    // Read as a string the long way, then read again: the second is not to take it for an array.
    CHECK_EQ(run("function h(n) { var t = '0123456789ABCDEF'; return t[n & 15] + t[n & 15]; }"
                 " var y; for (var c = 0; c < 4000; c++) y = h(c & 127); y"),
             std::string("FF"));
    CHECK_EQ(run("function h(n) { var t = '0123456789ABCDEF'; return '%' + t[(n >> 4) & 15] + t[n & 15]; }"
                 " var y; for (var c = 0; c < 4000; c++) y = h(c & 255); y"),
             std::string("%9F"));
    CHECK_EQ(run("function h(o, n) { return o[n] + ':' + o[n]; }"
                 " var y; for (var c = 0; c < 4000; c++) y = h(c & 1 ? 'ab' : {0: 'p', 1: 'q'}, c & 1); y"),
             std::string("b:b"));
    CHECK_EQ(run("function h(o, n) { o[n] = 1; o[n] = 2; return o[n]; }"
                 " var y; for (var c = 0; c < 4000; c++) y = h(c & 1 ? {} : [0, 0], c & 1); y"),
             std::string("2"));
    CHECK_EQ(run("function h(o, v) { o.push(v); o.push(v); return o.length; }"
                 " var y, s = { n: 0, push: function () { this.n++; }, length: 7 }; for (var c = 0; c < 4000; c++) y = h(c & 1 ? s : [], c); y"),
             std::string("7"));
}

TEST(aJumpOutOfASwitchLeavesNothingOfItBehind)
{
    // The value a `switch` tests, and what a `for-in` walks, stay on the stack while they run: a `break` or `continue` that
    // goes past them takes them off, in the interpreter and in native code alike.
    const char* program =
        "function f(n) { var out = [];"
        "  outer: for (var i = 0; i < n; i++) { for (var k in { a: 1, b: 2 }) { switch (i % 4) {"
        "    case 0: out.push('c' + k); continue outer;"
        "    case 1: out.push('b' + k); break outer;"
        "    case 2: try { switch (k) { case 'a': return out.join() + 'r'; } } finally { out.push('f'); } } } }"
        "  return out.join(); }"
        "function g(it) { for (var x of it) { switch (x) { case 2: for (var y of [7]) { switch (y) { case 7: return x * y; } } } } return -1; }"
        "var s = 0, t = 0; for (var j = 0; j < 3000; j++) { s += f(j % 3).length; t += g([1, 2, 3]); }"
        "[f(1), f(2), f(3), s, t].join(' ')";
    CHECK_EQ(run(program), std::string("ca ca,ba ca,ba 7000 42000"));
}

TEST(aLargeFunctionCompiledElsewhereGivesTheSameAnswers)
{
    // Long enough that its compile goes to the other thread, while the calls go on being interpreted.
    std::string body = "function big(n) { var s = n | 0;";
    for (int i = 0; i < 800; ++i)
        body += " s = (s * 31 + " + std::to_string(i) + " + (n & " + std::to_string(i & 15) + ")) | 0;";
    body += " return s; } var total = 0; function batch() { for (var i = 0; i < 500; i++) total = (total + big(i)) | 0; return total; }";
    script::Context::Options off;
    off.jit.mode = script::JitOptions::Mode::Off;
    js::Context interpreted(off);
    interpreted.eval(body.c_str(), "<test>");
    js::Context ctx;
    ctx.eval(body.c_str(), "<test>");
    bool same = true;
    for (int round = 0; round < 200 && same; ++round) {
        same = interpreted.eval("String(batch())", "<test>").toString() == ctx.eval("String(batch())", "<test>").toString();
        if (ctx.stats().methodsCompiled > 0 && round > 20)
            break;
    }
    CHECK(same);
    // A context can go while its compile is still under way.
    for (int i = 0; i < 4; ++i) {
        js::Context brief;
        brief.eval(body.c_str(), "<test>");
        brief.eval("batch()", "<test>");
    }
}

TEST(appendingToACapturedStringInNativeCodeGrowsItInPlace)
{
    if (otherCore())
        return;
    // `s` is a cell, since a closure shares it; native code appending to it holds a copy of its own, which is no reason to
    // copy the string. Copying would allocate the square of its length.
    js::Context ctx;
    ctx.eval("function f(n) { var s = ''; for (var i = 0; i < n; i++) s += String.fromCharCode(65 + i % 26);"
             " var g = function () { return s.length; }; return g(); }"
             "for (var r = 0; r < 300; r++) f(64);", "<test>");
    const std::uint64_t before = ctx.stats().totalAllocated;
    CHECK_EQ(ctx.eval("String(f(20000))", "<test>").toString(), std::string("20000"));
    CHECK(ctx.stats().totalAllocated - before < (8u << 20));
}

TEST(aFunctionInlinedByIdentityStaysThatFunction)
{
    // `init` is read through a prototype that goes away when `N.prototype` is replaced, and native code for `N` has a
    // copy of the first one in it. Another function made later must not come to be at its address and pass for it.
    const char* program =
        "var out = [];"
        "function run() { function N(a) { this.init(a); } N.prototype.init = function (x) { this.x = x; }; var o;"
        "  for (var v = 0; v < 300; v++) { if (v === 24) N.prototype = { init: function (x) { this.x = 'L' + x; } }; o = new N(v); }"
        "  out.push(o.x); }"
        "for (var i = 0; i < 40; i++) run(); out.every(function (x) { return x === 'L299'; }) ? 'same' : out.join()";
    for (std::uint32_t threshold : {2u, 50u}) {
        script::Context::Options options;
        options.jit.threshold = threshold;
        js::Context ctx(options);
        CHECK_EQ(ctx.eval(program, "<test>").toString(), std::string("same"));
    }
}

TEST(aRegisteredSymbolOutlivesACollection)
{
    // The registry holds a count of its own: a collection that drops the registry's does not drop the program's.
    js::Context ctx;
    ctx.eval("const BRAND = Symbol.for('brand'); var tagged = {}; tagged[BRAND] = 1;", "<test>");
    ctx.collectGarbage();
    ctx.eval("for (var i = 0; i < 1000; i++) ({ junk: [i] });", "<test>");
    ctx.collectGarbage();
    CHECK_EQ(ctx.eval("String(tagged[BRAND] === 1 && BRAND === Symbol.for('brand') && Symbol.keyFor(BRAND) === 'brand')", "<test>").toString(),
             std::string("true"));
}

TEST(aSpreadInsideAClosureSeesTheVariablesAroundIt)
{
    // `{ ...o }` in an arrow: `o` and `this` are captured like any other mention.
    CHECK_EQ(run("class L { constructor(p) { this.r = 5; this.v = p.map(x => typeof x === 'object' ? { ...x, ...x.prep(this.r) } : x); } }"
                 " function f() { const o = { a: 1 }; return () => ({ ...o, b: 2 }); }"
                 " JSON.stringify([new L([{ prep: r => ({ r }) }, 3]).v, f()()])"),
             std::string("[[{\"r\":5},3],{\"a\":1,\"b\":2}]"));
}

TEST(aFunctionIsItsSourceText)
{
    CHECK_EQ(run("function add(a, b) { return a + b; } class K { m() { return 1; } } const o = { get p() { return 1; } };"
                 " [add, K, K.prototype.m, x => x * 2, Object.getOwnPropertyDescriptor(o, 'p').get, Math.max].map(String).join('|')"),
             std::string("function add(a, b) { return a + b; }|class K { m() { return 1; } }|m() { return 1; }|x => x * 2|get p() { return 1; }|"
                         "function max() { [native code] }"));
}

TEST(argumentsIsAnArgumentsObject)
{
    CHECK_EQ(run("function f() { return [Object.prototype.toString.call(arguments), Array.isArray(arguments), typeof arguments.push,"
                 " [...arguments].join(), JSON.stringify(arguments)].join(' '); } f(1, 2)"),
             std::string("[object Arguments] false undefined 1,2 {\"0\":1,\"1\":2}"));
}

TEST(weakCollectionsHoldWhatTheirKeysHold)
{
    js::Context ctx;
    CHECK_EQ(ctx.eval("var wm = new WeakMap(), ws = new WeakSet(), k = {}; wm.set(k, 1); ws.add(k);"
                      " [wm.get(k), wm.has({}), ws.has(k), wm.delete(k), wm.has(k), wm.getOrInsert(k, 7)].join()", "<test>").toString(),
             std::string("1,false,true,true,false,7"));
    // A value that refers back to its key goes with it.
    ctx.eval("for (var i = 0; i < 20000; i++) { var key = { i: i }; wm.set(key, { back: key, pad: [1, 2, 3] }); } key = null;", "<test>");
    ctx.collectGarbage();
    CHECK(ctx.stats().liveObjects < 5000);
}

TEST(aDateHasItsCalendar)
{
    CHECK_EQ(run("var d = new Date(Date.UTC(2024, 1, 29, 13, 5, 9, 7)); d.setUTCDate(d.getUTCDate() + 1);"
                 " [d.toISOString(), d.getUTCDay(), Date.parse('2020-01-01'), Date.UTC(2020, 0, 1), new Date(NaN) + '',"
                 " new Date(8.64e15 + 1).getTime(), d.toUTCString()].join('|')"),
             std::string("2024-03-01T13:05:09.007Z|5|1577836800000|1577836800000|Invalid Date|NaN|Fri, 01 Mar 2024 13:05:09 GMT"));
}

TEST(objectLiteralProtoSetsThePrototype)
{
    CHECK_EQ(run("var base = { hi() { return 'hi'; } }; var a = { __proto__: base, x: 1 }, c = { ['__proto__']: base };"
                 " [a.hi(), Object.keys(a).join(), Object.keys(c).join(), ({}).__proto__ === Object.prototype].join()"),
             std::string("hi,x,__proto__,true"));
}

TEST(superPropertiesCanBeAssigned)
{
    CHECK_EQ(run("var log = []; class B { set x(v) { log.push(v); this._x = v; } get x() { return this._x || 10; } }"
                 " class A extends B { m() { super.x = 1; super.x += 5; super['y'] = 2; return [this._x, this.y, log.join()].join(); } }"
                 " new A().m()"),
             std::string("6,2,1,6"));
}

TEST(aComputedKeyCacheStaysRight)
{
    // `o[k]` and `#x` learn one key and one shape; another key, another shape, a frozen object or one that has become a
    // prototype goes the long way, and the answers stay those of the language.
    CHECK_EQ(run("const s1 = Symbol('a'), s2 = Symbol('b');"
                 " function get(o, k) { return o[k]; } function put(o, k, v) { o[k] = v; return o[k]; }"
                 " const a = { [s1]: 1, [s2]: 2 }, b = { x: 0, [s1]: 3, [s2]: 4 }; let t = 0;"
                 " for (let i = 0; i < 3000; i++) t += get(a, s1) + get(i & 1 ? a : b, i & 2 ? s1 : s2) + put(a, s1, i) - i;"
                 " const f = Object.freeze({ [s1]: 5 }); for (let i = 0; i < 10; i++) put(f, s1, 9);"
                 " const p = { [s1]: 1 }; const child = Object.create(p); put(p, s1, 7);"
                 " [t, get(f, s1), child[s1]].join()"),
             std::string("5627252,5,7"));
    CHECK_EQ(run("class C { #x = 1; static has(o) { return #x in o; } get(o) { return o.#x; } set(o, v) { o.#x = v; } }"
                 " class D { #x = 2; get(o) { return o.#x; } } const c = new C(), d = new D(); let t = 0;"
                 " for (let i = 0; i < 3000; i++) { c.set(c, i); t += c.get(c) + d.get(d); }"
                 " let threw = false; try { c.get(d); } catch (e) { threw = e instanceof TypeError; } [t, threw, C.has(d)].join()"),
             std::string("4504500,true,false"));
}

TEST(anAnchoredPatternTriesOnlyTheStart)
{
    // `/^#/` on a long input that does not start with `#`: one try, not a scan of the input for a `#` further on, which
    // made a lexer that slices its input a token at a time quadratic. The answers are the same either way.
    CHECK_EQ(run("var s = 'x'.repeat(1000) + '#'; [/^#/.test(s), /^x+#/.test(s), /^#/m.test('a\\n#'), s.match(/^(x+)#/)[1].length,"
                 " /^a|^b/.test('cab')].join()"),
             std::string("false,true,true,1000,false"));
}

TEST(typeofAndStringEqualityAgreeEverywhere)
{
    // typeof gives shared strings and short constants are interned, so native code compares many strings by pointer;
    // strings made at run time, which are not interned, still compare by their text.
    CHECK_EQ(run("function kind(x) { return typeof x === 'number' ? 'n' : typeof x === 'string' ? 's' : typeof x == 'boolean' ? 'b' :"
                 " typeof x === 'undefined' ? 'u' : typeof x === 'function' ? 'f' : typeof x; }"
                 " function same(a, b) { return a === b; } let out = '', n = 0;"
                 " for (let i = 0; i < 3000; i++) { out = [1, 'a', true, undefined, kind, null, {}, 2n].map(kind).join('');"
                 " if (same('ab', 'a' + (i >= 0 ? 'b' : 'c')) && !same('ab', 'abc'.slice(0, 1)) && same(typeof i, 'num' + 'ber')) n++; }"
                 " out + ',' + n"),
             std::string("nsbufobjectobjectbigint,3000"));
}

TEST(nativeCodeCatchesWhatItsTryThrows)
{
    // A `try`/`catch` is native code: what a call or a `throw` in it raises lands in the `catch`, with the locals and
    // whatever a `switch` or a loop around it keeps on the stack as they were; what the `catch` itself throws goes on out.
    const char* program =
        "function parse(s) { try { return JSON.parse(s).v; } catch (e) { return 'bad:' + e.name; } }"
        "function nested(i) { let r = 0; try { try { if (i % 3 === 0) throw new Error('a' + i); r += 1; } catch (e) { r += 10;"
        "  if (i % 6 === 0) throw e; } r += 100; } catch (e2) { r += 1000 + e2.message.length; } return r; }"
        "function sw(k) { switch (k) { case 1: try { throw k; } catch (e) { return 'c' + e; } default: return 'd'; } }"
        "function keeps() { let a = 'x', b = [1]; try { a = a + 'y'; b.push(2); undefinedFn(); } catch (e) { return a + b.length + (e instanceof ReferenceError); } }"
        "function rethrows() { try { throw { w: null }; } catch ({ w: { x, y } }) {} }"
        "let out; for (let k = 0; k < 2000; k++) { let r; try { rethrows(); } catch (e) { r = e.name; }"
        "  out = [parse('{\"v\":1}'), parse('{'), nested(k), sw(1), sw(2), keeps(), r]; } out.join(' ')";
    for (const char* mode : {"tiered", "eager"}) {
        script::Context::Options options;
        if (std::string(mode) == "eager")
            options.jit.mode = script::JitOptions::Mode::Eager;
        js::Context ctx(options);
        CHECK_EQ(ctx.eval(program, "<test>").toString(), std::string("1 bad:SyntaxError 101 c1 d xy2true TypeError"));
    }
}

TEST(nativeCodeRunsFinallyEveryWayOut)
{
    // A `finally` is native code: entered from the end of its `try`, a `return`, `break` or `continue` through it, or an
    // exception, it ends going where that way was going; one entered by an exception raises it again.
    const char* program =
        "var log = [];"
        "function a(i) { try { if (i & 1) throw new Error('e'); return 'r' + i; } finally { log.push('f' + i); } }"
        "function b() { let s = 0; for (let k = 0; k < 4; k++) { try { if (k === 2) continue; if (k === 3) break; s += k; } finally { s += 10; } } return s; }"
        "function c(i) { try { try { throw i; } finally { log.push('in'); } } catch (e) { return 'caught' + e; } finally { log.push('out'); } }"
        "function d() { try { return 1; } finally { return 2; } }"
        "function g(i) { try { try { return 'x'; } finally { if (i % 2) throw new TypeError('t'); } } catch (z) { return z.name; } }"
        "let out; for (let k = 0; k < 2000; k++) { log = []; let ra; try { ra = a(k); } catch (err) { ra = err.message; }"
        "  out = [ra, b(), c(k), d(), g(k), log.join('.')]; } out.join(' ')";
    for (bool eager : {false, true}) {
        script::Context::Options options;
        if (eager)
            options.jit.mode = script::JitOptions::Mode::Eager;
        js::Context ctx(options);
        CHECK_EQ(ctx.eval(program, "<test>").toString(), std::string("e 41 caught1999 2 TypeError f1999.in.out"));
    }
}

TEST(nativeForOfClosesWhatItLeaves)
{
    // A for-of over anything is native code: a `break` or `return` out of it calls the iterator's return(), and an
    // exception closes it on the way out.
    const char* program =
        "var log = [];"
        "function* gen(n) { try { for (let i = 0; i < n; i++) yield i; } finally { log.push('c' + n); } }"
        "function sum(it) { let s = 0; for (const x of it) s += x; return s; }"
        "function firstOver(it, k) { for (const x of it) if (x > k) return x; return -1; }"
        "function brk(it) { let s = 0; for (const x of it) { if (x === 2) break; s += x; } return s; }"
        "function thr(it) { try { for (const x of it) { if (x === 1) throw new Error('boom'); } } catch (e) { return e.message; } }"
        "let out; for (let k = 0; k < 2000; k++) { log = []; out = [sum([1, 2, 3]), sum(new Set([4, 5])), sum(gen(4)), firstOver(gen(5), 1),"
        "  brk(gen(6)), thr(gen(7)), log.join(',')]; } out.join(' ')";
    for (bool eager : {false, true}) {
        script::Context::Options options;
        if (eager)
            options.jit.mode = script::JitOptions::Mode::Eager;
        js::Context ctx(options);
        CHECK_EQ(ctx.eval(program, "<test>").toString(), std::string("6 9 6 2 1 boom c4,c5,c6,c7"));
    }
}

TEST(aBigProgramCompilesAFunctionWhenItIsFirstCalled)
{
    if (otherCore())
        return;
    // Past 64 KiB of source a plain function is compiled on its first call, from its text, against what it captured
    // when it was made: the answers are the ones compiling it at once would give.
    std::string program = "/*" + std::string(70000, ' ') + "*/";
    const std::string pad = "/*" + std::string(100, ' ') + "*/";
    program +=
        "let a = 1; const k = 5;"
        "function outer(x) {"
        "  let b = 2;                                                               "
        "  function inner(y) { b += y; return a + b + x + k;" + pad + " }"
        "  function fact(n) { return n <= 1 ? 1 : n * fact(n - 1);" + pad + " }"
        "  var g = function* gen() { yield b; yield* [1, 2];" + pad + " };"
        "  return [inner(3), inner(4), fact(5), [...g()].join(), arguments.length, inner.length, typeof fact];"
        "}"
        "function cycle() { var loop = function (n) { return n ? loop(n - 1) + 1 : 0;" + pad + " };"
        "  return loop(3);" + pad + " }"
        "function late() { return eventually + 1;" + pad + " }"
        "function broken() { break;" + pad + " }"
        "let eventually = 41;";
    js::Context ctx;
    ctx.eval(program.c_str(), "<test>");
    CHECK_EQ(ctx.eval("outer(10, 20).join(' ')", "<test>").toString(), std::string("21 25 120 9,1,2 2 1 function"));
    CHECK_EQ(ctx.eval("String(outer.length) + ' ' + outer.toString().slice(0, 16)", "<test>").toString(), std::string("1 function outer(x"));
    for (int i = 0; i < 2000; ++i)
        ctx.eval("cycle()", "<test>");
    CHECK_EQ(ctx.eval("String(cycle())", "<test>").toString(), std::string("3"));
    CHECK(ctx.stats().gcRuns == 0);
    CHECK_EQ(ctx.eval("String(late())", "<test>").toString(), std::string("42"));
    // What only compiling finds is a SyntaxError of the call, and of every call after.
    CHECK_EQ(ctx.eval("var seen = []; for (var t = 0; t < 2; t++) { try { broken(); } catch (e) { seen.push(e.name); } } seen.join()", "<test>").toString(),
             std::string("SyntaxError,SyntaxError"));
}

TEST(classFieldsGoOnEveryShapeTheyMeet)
{
    // A base class's fields go on objects of each class derived from it, so their sites see several shapes; `new` of a
    // class with fields is made in place, its fields first. A class called without `new` still throws.
    const char* program =
        "class Vec { #x = 0; #y = 0; z = 1; constructor(x, y) { this.#x = x; this.#y = y; } get x() { return this.#x; }"
        "  add(v) { return new Vec(this.#x + v.#x, this.#y + v.#y); } }"
        "class Base { #pos = null; tag = 'b'; constructor(p) { this.#pos = p; } get pos() { return this.#pos; } }"
        "class A extends Base { #r = 0; constructor(p, r) { super(p); this.#r = r; } get r() { return this.#r; } }"
        "class B extends Base { n = null; constructor(p, n) { super(p); this.n = n; } }"
        "function bare() { try { return Vec(1, 2); } catch (e) { return e.name; } }"
        "let s = 0, v = new Vec(0, 0), last = '';"
        "for (let i = 0; i < 3000; i++) { v = v.add(new Vec(1, 2)); const a = new A(v, i); const b = new B(a, i);"
        "  s += a.r + b.pos.pos.x + b.n + v.z + (b.tag === 'b' ? 1 : 0); last = bare(); }"
        "[s, v.x, Object.keys(new B(1, 2)).join(), last].join(' ')";
    for (bool eager : {false, true}) {
        script::Context::Options options;
        if (eager)
            options.jit.mode = script::JitOptions::Mode::Eager;
        js::Context ctx(options);
        CHECK_EQ(ctx.eval(program, "<test>").toString(), std::string("13504500 3000 tag,n TypeError"));
    }
}

TEST(nativeCodeDoesBigIntArithmetic)
{
    // A function with bigint constants is compiled, and its operators go through the interpreter's own arithmetic; mixing
    // a bigint with a number, or `>>>` on one, is still a TypeError.
    const char* program =
        "function f(n) { let a = 1n, b = 0n; for (let i = 0; i < n; i++) { const t = a; a = a + b; b = t; a = a * 3n - 2n;"
        "  a = a % 1000000007n; b = -b; b = ~b; b++; } return [a, b, a === 1n, a < 5n, typeof a, 7n ** 3n, 100n >> 2n, 5n & 3n].join(); }"
        "function g(x) { return x - 1; }"
        "function h(x) { return x >>> 1n; }"
        "let r, s = 0, e1 = '', e2 = ''; for (let k = 0; k < 300; k++) { r = f(50); s += g(k); }"
        "try { g(10n); } catch (e) { e1 = e.name; } try { h(4n); } catch (e) { e2 = e.name; }"
        "[r, s, e1, e2].join(' ')";
    for (bool eager : {false, true}) {
        script::Context::Options options;
        if (eager)
            options.jit.mode = script::JitOptions::Mode::Eager;
        js::Context ctx(options);
        CHECK_EQ(ctx.eval(program, "<test>").toString(), std::string("392460006,800087529,false,false,bigint,343,25,1 44550 TypeError TypeError"));
    }
}

TEST(aCoroutinesLoopsRunInNativeCodeInItsOwnFrame)
{
    // A loop in a generator or an async function that does not suspend is compiled; native code hands the coroutine back
    // to the interpreter where it suspends, with every local it has, as `yield*` in a loop needs.
    const char* program =
        "function* fill() { let c = 1; while (true) { let s = 0; for (let i = 0; i < 3000; i++) s += (i + c) % 7; c++;"
        "  const got = yield s; if (got === 'stop') { for (let j = 0; j < 3000; j++) s -= 1; return s; } } }"
        "const g = fill(); let t = 0; for (let k = 0; k < 50; k++) t += g.next().value; const last = g.next('stop').value;"
        "const step = { *process(st) { st.n++; let w = 0; for (let i = 0; i < 2000; i++) w += i; if (st.n % 3 == 0) yield w; } };"
        "function* program(st) { let max = 40; while (st.line != null) { if (st.line > max) { st.line = null; continue; }"
        "  st.line++; let x = 0; for (let i = 0; i < 2000; i++) x ^= i; yield* step.process(st); } }"
        "let u = 0; for (let r = 0; r < 5; r++) { const st = { line: 1, n: 0 }; for (const v of program(st)) u += v; u += st.n; }"
        "function* thrower() { let a = 0; for (let i = 0; i < 5000; i++) { a += i; if (i == 4000) throw new Error('at ' + a); } yield a; }"
        "let caught = ''; try { thrower().next(); } catch (e) { caught = e.message; }"
        "async function af(n) { let s = 0; for (let i = 0; i < n; i++) s += i; await null; for (let i = 0; i < n; i++) s -= 2 * i; return s; }"
        "let fromAsync = 0; (async () => { for (let k = 0; k < 20; k++) fromAsync += await af(3000); })();"
        "[t, last, u, caught].join(' ')";
    for (bool eager : {false, true}) {
        script::Context::Options options;
        if (eager)
            options.jit.mode = script::JitOptions::Mode::Eager;
        js::Context ctx(options);
        const std::string got = ctx.eval(program, "<test>").toString();
        ctx.runJobs();
        CHECK_EQ(got + " " + ctx.eval("String(fromAsync)", "<test>").toString(), std::string("449998 5998 129935200 at 8002000 -89970000"));
    }
}

TEST(aPromiseResolvedWithAPromiseTakesItsTicks)
{
    // Resolving with a promise of the engine's own, whose `then` is the built-in one, is a reaction made by a job: the
    // same ticks as calling `then` with resolving functions, which a subclass still gets.
    js::Context ctx;
    ctx.eval("var log = []; const a = Promise.resolve(1); const b = new Promise(r => r(a)); b.then(v => log.push('b' + v));"
             "a.then(v => log.push('a1')).then(() => log.push('a2')).then(() => log.push('a3'));"
             "const c = Promise.reject(new Error('x')); const d = new Promise(r => r(c)); d.catch(e => log.push('d:' + e.message));"
             "class P2 extends Promise {} const e = P2.resolve(5); const f = new Promise(r => r(e)); f.then(v => log.push('f' + v));"
             "(async () => { const v = await new Promise(r => r(Promise.resolve(7))); log.push('w' + v); })();", "<test>");
    ctx.runJobs();
    CHECK_EQ(ctx.eval("log.join(' ')", "<test>").toString(), std::string("a1 a2 b1 a3 d:x f5 w7"));
}

TEST(aStringBuiltPieceByPieceIsJoinedOnce)
{
    // A long `+` makes a rope; reading it, from the interpreter or native code, as a key or in a comparison joins it.
    js::Context ctx;
    ctx.eval("var o = { out: '' }; function emit(t) { o.out = o.out + t; }"
             "for (var i = 0; i < 30000; i++) emit('<li>' + i + '</li>');"
             "function sum(s) { var h = 0; for (var i = 0; i < s.length; i++) h = (h * 31 + s.charCodeAt(i)) | 0; return h; }"
             "var a = 'x'.repeat(40), b = a + 'y'.repeat(40), c = b + b, m = {}; m[c] = 1;"
             "var r = [o.out.length, sum(o.out), sum(c), c === b + b, m['x'.repeat(40) + 'y'.repeat(40) + b],"
             "         o.out.slice(-11), c[79] + c[80]].join(' ');", "<test>");
    CHECK_EQ(ctx.eval("r", "<test>").toString(), std::string("408890 1705946264 1838404864 true 1 >29999</li> yx"));
}

TEST(nativeCodeNoFunctionEntersIsFreed)
{
    if (otherCore())
        return;
    // Code a function stops entering, after a bail, goes back to the cache once no frame is in it: a small cache keeps
    // taking new code for as long as old code is let go.
    script::Context::Options options;
    options.jit.mode = script::JitOptions::Mode::Eager;
    options.jit.codeCacheLimit = 256u << 10;
    options.runtimeCode = true;
    js::Context ctx(options);
    ctx.eval("var fs = [], total = 0;"
             "for (var k = 0; k < 300; k++)"
             "  fs.push(new Function('x', 'var s = 0; for (var i = 0; i < 40; i++) s += x * i + ' + k + '; return s;'));"
             "for (var round = 0; round < 4; round++) for (var k = 0; k < fs.length; k++) {"
             "  for (var j = 0; j < 30; j++) total += fs[k](j);"
             "  var r = fs[k](round % 2 ? 'a' : {}); total += typeof r === 'string' ? r.length : 1; }", "<test>");
    CHECK_EQ(ctx.eval("total", "<test>").toString(), std::string("622441200"));
    // Each function is compiled twice: for numbers, and again once it is given something else. A cache that kept what it
    // stopped entering fills at about 350; the stack is read conservatively, so a word left over in a frame can keep
    // some code a while longer, and more of that in a sanitizer's larger frames.
    CHECK(ctx.stats().methodsCompiled >= 390);
}

TEST(nativeCodeOfAFunctionThatIsGoneIsFreed)
{
    // A function no one can call any more gives its code back: a small cache keeps taking new code for as long as the
    // functions made before are dropped, here 400 of them in a cache that holds about 120.
    script::Context::Options options;
    options.jit.mode = script::JitOptions::Mode::Eager;
    options.jit.codeCacheLimit = 256u << 10;
    options.runtimeCode = true;
    js::Context ctx(options);
    ctx.eval("var total = 0;"
             "for (var k = 0; k < 400; k++) {"
             "  var f = new Function('x', 'var s = 0; for (var i = 0; i < 40; i++) s += x * i + ' + k + '; return s;');"
             "  for (var j = 0; j < 3; j++) total += f(j); }", "<test>");
    CHECK_EQ(ctx.eval("total", "<test>").toString(), std::string("10512000"));
    // The stack is read conservatively, so a word left over in a frame can keep some code a while longer. Nothing
    // compiled at all is a build or a run with no native code.
    if (ctx.stats().methodsCompiled > 0)
        CHECK(ctx.stats().methodsCompiled >= 380);
}

TEST(nativeHigherOrderLoopsSkipHoles)
{
    // `map`, `forEach`, `filter` and `reduce` in native code do not call the callback for a hole, in an array of numbers
    // or of values, and `map` leaves the hole in its result: the first element as much as any.
    const char* program =
        "function f(a) { var seen = []; var m = a.map(function (x, i) { seen.push(i); return x * 2; });"
        "  a.forEach(function (x, i) { seen.push('e' + i); }); var fl = a.filter(function (x) { return x > 1; });"
        "  var r = a.reduce(function (s, x) { return s + x; }, 0);"
        "  return [seen.join(''), m.length, 0 in m, 3 in m, m.join(':'), fl.join(':'), r].join(' '); }"
        "var out; for (var k = 0; k < 3000; k++) { var a = new Array(3); a.push(1, 2);"
        "  if (k & 1) a.push({ valueOf: function () { return 5; } }); a[6] = 4; out = f(a) + ' ' + f([7, , 8]); } out";
    for (bool eager : {false, true}) {
        script::Context::Options options;
        if (eager)
            options.jit.mode = script::JitOptions::Mode::Eager;
        js::Context ctx(options);
        CHECK_EQ(ctx.eval(program, "<test>").toString(),
                 std::string("3456e3e4e5e6 7 false true :::2:4:10:8 2:[object Object]:4 12 02e0e2 3 true false 14::16 7:8 15"));
    }
}

TEST(anInlinedFunctionGivesItsCallerAnInt32)
{
    // A small function inlined whose every return is an int32 gives the caller that int32, with no double in between;
    // a bail in its body that comes back with anything else, an overflow, minus zero, a string, an exception, has the
    // interpreter carry on from after the call. Parameters it never writes take numbers as the caller has them.
    const char* program =
        "function add(a, b) { return a + b; } function half(a) { return a / 2; } function neg(a) { return -a; }"
        "function pick(a, b) { if (a > 1000) throw new Error('big ' + a); return a - b; }"
        "function run(n, big) { var t = 0, h = 0, s = '', z = 0, caught = 0, objs = 0;"
        "  for (var i = 0; i < n; i++) { t = add(t, i) & 1023; h += add(i, big) | 0; if (i === n - 3) s = add('x', i);"
        "    z = neg(i & 1 ? 0 : 0); try { objs += pick(i & 2047, 1); } catch (e) { caught++; } h += half(i) & 3; }"
        "  return [t, h, s, 1 / z, caught, objs, add(2147483647, 1), add(1.5, 2), add({ valueOf() { return 7; } }, 1)].join(','); }"
        "var out = []; for (var k = 0; k < 6; k++) out.push(run(3000, k < 3 ? 1 : 2147483000)); out.join(' | ')";
    const std::string a = "68,4506000,x2997,-Infinity,1047,951223,2147483648,3.5,8";
    const std::string b = "68,-3659309577192,x2997,-Infinity,1047,951223,2147483648,3.5,8";
    for (bool eager : {false, true}) {
        script::Context::Options options;
        if (eager)
            options.jit.mode = script::JitOptions::Mode::Eager;
        js::Context ctx(options);
        CHECK_EQ(ctx.eval(program, "<test>").toString(), a + " | " + a + " | " + a + " | " + b + " | " + b + " | " + b);
    }
}

TEST(theInterpreterRunsCallsInItsOwnLoop)
{
    // A call from interpreted code to interpreted code is entered by the same loop, and so is `new` of a plain function:
    // the function called may live only on the caller's stack, an exception goes back through the frames until one has
    // a handler, deep recursion uses no native stack, a loop counter in a local steps in place, and a constructor that
    // returns no object gives the one it made.
    const char* program =
        "function d(v) { return (() => v)(); }"
        "function thrower(n) { if (n === 0) throw new Error('deep'); return thrower(n - 1) + 1; }"
        "function catcher(n) { try { return thrower(n); } catch (e) { return e.message + n; } }"
        "function depth(n) { return n === 0 ? 0 : 1 + depth(n - 1); }"
        "function count(n) { var t = 0; for (var i = 0; i < n; i++) t++; for (var j = n; j > 0; j--) t--; return t; }"
        "function self() { return this === undefined ? 'u' : typeof this; }"
        "function A(x) { this.x = x; } function B(x) { this.x = x; return { y: x }; } function C(x) { this.x = x; return 5; }"
        "function D() { this.t = new.target === D; } function E(n) { if (n > 2) throw new Error('E' + n); this.n = n; }"
        "function F(n) { this.v = n ? new F(n - 1).v + 1 : 0; } A.prototype.get = function () { return this.x; };"
        "function newE(n) { try { return new E(n).n; } catch (e) { return e.message; } }"
        "[d(7), catcher(20), depth(400), count(50), self(), [1, 2, 3].map(x => d(x * 2)).join(''), new A(3).get(),"
        " new B(4).y, new C(5).x, new D().t, new F(10).v, newE(1), newE(3), new E(1) instanceof E,"
        " (2 ** 40 + 7) & 15, (2 ** 53 + 2) >> 0, -1e19 | 0].join(',')";
    for (bool off : {true, false}) {
        script::Context::Options options;
        if (off)
            options.jit.mode = script::JitOptions::Mode::Off;
        js::Context ctx(options);
        CHECK_EQ(ctx.eval(program, "<test>").toString(),
                 std::string("7,deep20,400,0,object,246,3,4,5,true,10,1,E3,true,7,2,1981284352"));
    }
}

TEST(aLongPieceOfAStringSharesItsCharacters)
{
    // Cutting the front off a long text over and over shares the characters that are left instead of copying them; a
    // short piece is a string of its own, and a piece of a piece points into the first string.
    const char* program =
        "var src = ''; for (var i = 0; i < 20000; i++) src += 'token' + (i % 10) + ' ';"
        "var n = 0, s = src; while (s.length) { var m = s.indexOf(' '); n += s.slice(0, m).length; s = s.slice(m + 1); }"
        "var a = src.slice(100, 300), b = a.slice(10, 150), c = b.substring(5, 100) + '|' + src.substr(src.length - 70);"
        "[n, s.length, a.length, b.length, c.length, c.slice(0, 20), c.charCodeAt(3), b === src.slice(110, 250),"
        " JSON.stringify(c).length, src.slice(7).slice(7).slice(7).length, ({ [src.slice(1, 200)]: 1 })[src.substring(1, 200)]].join(',')";
    for (bool eager : {false, true}) {
        script::Context::Options options;
        if (eager)
            options.jit.mode = script::JitOptions::Mode::Eager;
        js::Context ctx(options);
        CHECK_EQ(ctx.eval(program, "<test>").toString(), std::string("120000,0,200,140,166,en6 token7 token8 to,32,true,168,139979,1"));
    }
}

TEST(theNameOfAFunctionExpressionIsReadOnly)
{
    // The binding is immutable: a write is ignored in sloppy code and throws in strict code; a declaration is no
    // such binding, and a parameter or a var of the same name hides it.
    const char* program =
        "var out = [];"
        "var a = function me() { me = 1; me += 2; me++; return typeof me; }; out.push(a());"
        "var b = function me() { 'use strict'; try { me = 1; return 'no'; } catch (e) { return e.constructor.name; } }; out.push(b());"
        "var c = function me() { return (() => { me = 1; return typeof me; })(); }; out.push(c());"
        "function decl() { decl = 1; return typeof decl; } out.push(decl());"
        "var d = function me(me) { me = 1; return typeof me; }; out.push(d(0));"
        "var e = function me() { var me = 1; me = 2; return me; }; out.push(e());"
        "var f = function me(n) { return n ? me(n - 1) + 1 : 0; }; out.push(f(3));"
        "out.join(' ')";
    for (auto mode : {script::JitOptions::Mode::Off, script::JitOptions::Mode::Eager}) {
        script::Context::Options options;
        options.jit.mode = mode;
        js::Context ctx(options);
        CHECK_EQ(ctx.eval(program, "<test>").toString(), std::string("function TypeError function number number 2 3"));
    }
}

TEST(htmlLikeCommentsAreComments)
{
    // Annex B.1.1: `<!--` comments to the end of the line anywhere; `-->` does so only at the start of a line, after
    // space or comments, so `x --> 0` is still a decrement and a comparison.
    CHECK_EQ(run("var z = 1 <!-- 5\n; z"), std::string("1"));
    CHECK_EQ(run("-->first line\nvar a = 2;\n  --> after space\n/* a\n comment */ --> after a comment\na"), std::string("2"));
    CHECK_EQ(run("var x = 3; var r = x --> 2; [r, x].join()"), std::string("true,2"));
    CHECK_EQ(run("var y = 5; var t = y<!--y\n; t"), std::string("5"));
    CHECK_EQ(run("var s = '<!-- not a comment'; var u = '-->'; s.length + u.length"), std::string("21"));
}

TEST(aUnaryOperatorBeforeExponentiationIsASyntaxError)
{
    // `-2 ** 2` is an early error; parentheses, an update expression or a unary operator on the right are fine.
    CHECK_EQ(run("var a = 1; [(-2) ** 2, 2 ** -1, ++a ** 2, -(2 ** 2), 2 ** 3 ** 2, (typeof 2) ** 2, a-- ** 2].join()"),
             std::string("4,0.5,4,-4,512,NaN,4"));
    for (const char* source : {"-2 ** 2", "typeof 2 ** 2", "!1 ** 2", "~1 ** 2", "+1 ** 2", "void 0 ** 2",
                               "delete a.b ** 2", "2 * -3 ** 0", "1 + -2 ** 2", "(async () => await 1 ** 2)"}) {
        bool refused = false;
        try {
            js::Context ctx;
            ctx.eval(source, "<test>");
        } catch (const std::exception&) {
            refused = true;
        }
        CHECK_EQ(refused, true);
    }
}

TEST(aStackCapturedFromCompiledCodeNamesTheFunctionsInOrder)
{
    // The frames of compiled code are found by address among all the compiled functions: a stack keeps at most 16
    // frames, innermost first, so 12 functions and the one that makes the Error fit in it.
    const char* program =
        "var fns = [];"
        "for (var n = 0; n < 12; n++) fns.push(new Function('next', 'return function f' + n + '(k) { return next(k); }'));"
        "function leaf() { return new Error('x').stack; }"
        "var chain = leaf;"
        "for (var n = 0; n < 12; n++) chain = fns[n](chain);"
        "var stack = ''; for (var i = 0; i < 60; i++) stack = chain(i);"
        "var at = [stack.indexOf(' leaf ')]; for (var n = 0; n < 12; n++) at.push(stack.indexOf(' f' + n + ' '));"
        "var ordered = at[0] >= 0; for (var n = 1; n < at.length; n++) if (at[n] <= at[n - 1]) ordered = false;"
        "ordered";
    for (auto mode : {script::JitOptions::Mode::Off, script::JitOptions::Mode::Eager}) {
        script::Context::Options options;
        options.jit.mode = mode;
        options.runtimeCode = true;
        js::Context ctx(options);
        CHECK_EQ(ctx.eval(program, "<test>").toString(), std::string("true"));
    }
}

TEST(tokensReadOneAfterAnotherKeepNothingOfTheLastOne)
{
    // The parser reads every token into the one it has: a template's chunks, a string's value, a number's value and a
    // regular expression's text must not show in the token after, and a slash after `)` or `}` is a division or a
    // regular expression by what the parser wants.
    const char* program =
        "var a = 8, b = 2, g = 2, out = [];"
        "out.push(`x${a}y${`in${b}ner`}z`, 'str' + 1, 0x10 + 1.5e1, 017 + 1);"
        "out.push((a) / b / g, (a)\n/ 2 /g);"
        "if (true) { out.push(1) }\n/b/g.test('b') && out.push('re-after-brace');"
        "var r = /[/]=/.test('/=') + '|' + 'a=b'.replace(/=/, '-') + '|' + (a /= 2) + '|' + a;"
        "out.push(r, `${'a'}${'b'}`.length, `line\nbreak`.length, String.raw`\\n${1}`);"
        "var tag = (s, ...v) => s.raw.join('|') + v.join(',');"
        "out.push(tag`a${1}b${2}c`, tag`x`, tag`${a}`);"
        "out.join(' ')";
    CHECK_EQ(run(program), std::string("x8yin2nerz str1 31 16 2 2 1 re-after-brace true|a-b|4|4 2 10 \\n1 a|b|c1,2 x |4"));
}

TEST(aCallOfEvalAnywhereInAFunctionMakesItsVariablesVisibleToEval)
{
    // The parser tells the compiler which functions hold an `eval` call, in a parameter default, an arrow's body, a
    // method, a field initializer or a class heritage as much as in a statement; one that has none is not slowed.
    const char* program =
        "var out = [];"
        "function inArrow() { var x = 1; var g = () => eval('x + 1'); return g(); } out.push(inArrow());"
        "function inDefault() { var y = 2; function h(a = eval('y * 3')) { return a; } return h(); } out.push(inDefault());"
        "function inArrowDefault() { var z = 4; var g = (a = eval('z + 1')) => a; return g(); } out.push(inArrowDefault());"
        "function inMethod() { var m = 5; var o = { f() { return eval('m'); } }; return o.f(); } out.push(inMethod());"
        "function inClass() { var c = 6; class K { static v = eval('c * 2'); get g() { return eval('c'); } } return K.v + ':' + new K().g; } out.push(inClass());"
        "function inHeritage() { var b = 7; class B { constructor() { this.q = 1; } } class D extends (eval('b'), B) {} return new D().q; } out.push(inHeritage());"
        "function nested() { var n = 8; return (() => () => eval('n'))()(); } out.push(nested());"
        "function noEval() { var w = 9; var g = () => w; return g() + (function () { return 1; })(); } out.push(noEval());"
        "function shadowed() { var e = 1; { var eval2 = 2; } return [1, 2].map(i => i + e).join(''); } out.push(shadowed());"
        "out.join(' ')";
    for (auto mode : {script::JitOptions::Mode::Off, script::JitOptions::Mode::Eager}) {
        script::Context::Options options;
        options.jit.mode = mode;
        options.runtimeCode = true;
        js::Context ctx(options);
        CHECK_EQ(ctx.eval(program, "<test>").toString(), std::string("2 6 5 5 12:6 1 8 10 23"));
    }
}

TEST(aBindingUsedAboveItsDeclarationInTheSameFunctionIsAReferenceError)
{
    // The dead zone of `let`, `const` and `class`, for what the compiler can see: a use above the declaration in one
    // function, in a read, a write, a typeof, an initializer or a default of the same pattern. A closure's use is a
    // different function's, and a later use is no use in the zone.
    const char* program =
        "var out = [];"
        "function t(f) { try { out.push(String(f())); } catch (e) { out.push(e.constructor.name); } }"
        "t(function () { x; let x = 1; });"
        "t(function () { let q = q + 1; });"
        "t(function () { const k = k; });"
        "t(function () { typeof z; let z; });"
        "t(function () { w = 1; let w; });"
        "t(function () { w2++; let w2 = 0; });"
        "t(function () { return new C(); class C {} });"
        "t(function () { let { m = n, n } = {}; return m; });"
        "t(function () { { let a = 1; a += 1; return a; } });"
        "t(function () { let { m, n = m } = { m: 3 }; return n; });"
        "t(function () { for (var i = 0; i < 2; i++) { let y = i; if (i) return y; } });"
        "t(function () { function f() { return v; } let v = 2; return f(); });"
        "t(function () { let c = 1; { let c2 = c; return c2; } });"
        "t(function () { { let b = 1; } return typeof b; });"
        "t(function () { const o = 1; try { o = 2; } catch (e) { return e.constructor.name; } });"
        "out.join(' ')";
    for (auto mode : {script::JitOptions::Mode::Off, script::JitOptions::Mode::Eager}) {
        script::Context::Options options;
        options.jit.mode = mode;
        js::Context ctx(options);
        CHECK_EQ(ctx.eval(program, "<test>").toString(),
                 std::string("ReferenceError ReferenceError ReferenceError ReferenceError ReferenceError ReferenceError "
                             "ReferenceError ReferenceError 2 3 1 2 1 undefined TypeError"));
    }
}

TEST(aGeneratorIsSteppedWithoutAResultObject)
{
    // for-of over a generator whose next is the one generators start with resumes it directly; a next replaced on the
    // generators' prototype, on the function's or on the generator itself is called as it should be, and yield*,
    // break, throw and destructuring behave as ever.
    const char* program =
        "var out = []; function* g(n) { for (let i = 0; i < n; i++) yield i; return 'r'; }"
        "function* d() { yield 'a'; const r = yield* g(2); yield r; } out.push([...d()].join());"
        "function* h() { try { yield 1; yield 2; } finally { out.push('fin'); } } for (const x of h()) { out.push(x); break; }"
        "function* t() { yield 1; throw new Error('boom'); } try { for (const x of t()) out.push(x); } catch (e) { out.push(e.message); }"
        "var gp = Object.getPrototypeOf(g).prototype, saved = gp.next;"
        "gp.next = function () { var r = saved.call(this); if (!r.done) r.value *= 10; return r; };"
        "out.push([...g(3)].join()); gp.next = saved; out.push([...g(3)].join());"
        "g.prototype.next = function () { return { done: true }; }; out.push([...g(3)].join() + '|'); delete g.prototype.next;"
        "var inst = g(3); inst.next = function () { return { done: true }; }; out.push([...inst].join() + '|', [...g(2)].join());"
        "const [p, q, ...rest] = g(5); out.push(p, q, rest.join('')); out.join(' ')";
    for (bool off : {true, false}) {
        script::Context::Options options;
        if (off)
            options.jit.mode = script::JitOptions::Mode::Off;
        js::Context ctx(options);
        CHECK_EQ(ctx.eval(program, "<test>").toString(), std::string("a,0,1,r 1 fin 1 boom 0,10,20 0,1,2 | | 0,1 0 1 234"));
    }
}

TEST(nativeCodeConstructsAClassDirectly)
{
    // new of a class from native code makes the object, runs the field initializers and enters the constructor's native
    // code; a derived class, a generator, a method and a call without new still take the long way, which refuses or
    // throws as it should, and an initializer that throws lets the object go.
    const char* program =
        "var out = []; class V { #x; y = 2; constructor(x) { this.#x = x; } get x() { return this.#x; } }"
        "class W extends V { z = 3; constructor(x) { super(x * 2); } }"
        "class T { a = (() => { if (T.boom) throw new Error('init'); return 1; })(); } function* G() { yield 1; }"
        "var s = 0; for (var i = 0; i < 3000; i++) { var v = new V(i); s += v.x + v.y; var w = new W(i); s += w.x + w.z; }"
        "out.push(s); T.boom = false; for (var i = 0; i < 500; i++) new T();"
        "T.boom = true; try { for (var i = 0; i < 500; i++) new T(); } catch (e) { out.push(e.message); }"
        "try { for (var i = 0; i < 500; i++) new G(); } catch (e) { out.push(e.constructor.name); }"
        "var m = { f() {} }; try { for (var i = 0; i < 500; i++) new m.f(); } catch (e) { out.push(e.constructor.name); }"
        "try { for (var i = 0; i < 500; i++) V(1); } catch (e) { out.push(e.constructor.name); } out.join(' ')";
    for (bool eager : {false, true}) {
        script::Context::Options options;
        if (eager)
            options.jit.mode = script::JitOptions::Mode::Eager;
        js::Context ctx(options);
        CHECK_EQ(ctx.eval(program, "<test>").toString(), std::string("13510500 init TypeError TypeError TypeError"));
    }
}

TEST(aPropertyAbsentFromTheWholeChainIsCachedAndForgottenWhenItAppears)
{
    // A read of a property no object on the chain has is learned by its site; whatever gives the receiver or anything
    // on its chain the property, or changes the chain, is seen by the interpreter and by native code alike.
    const char* program =
        "var out = [], k; function S(v) { out.push(String(v)); }"
        "function ra(o) { return o.missing; } var a = {p: 1}; for (k = 0; k < 400; k++) ra(a); S(ra(a)); a.missing = 1; S(ra(a));"
        "function rb(o) { return o.missing; } function P() {} var b = new P(); for (k = 0; k < 400; k++) rb(b);"
        "Object.getPrototypeOf(b).missing = 2; S(rb(b));"
        "function rc(o) { return o.missing; } var c = {q: 1}; for (k = 0; k < 400; k++) rc(c);"
        "Object.prototype.missing = 3; S(rc(c)); delete Object.prototype.missing; S(rc(c));"
        "function rd(o) { return o.missing; } var d = {r: 1}; for (k = 0; k < 400; k++) rd(d);"
        "Object.setPrototypeOf(d, {missing: 4}); S(rd(d));"
        "function re(o) { return o.missing; } var m2 = {}, m1 = Object.create(m2), e = Object.create(m1);"
        "for (k = 0; k < 400; k++) re(e); Object.setPrototypeOf(m1, {missing: 5}); S(re(e));"
        "function rf(o) { return o.missing; } var fp = {}, f = Object.create(fp); for (k = 0; k < 400; k++) rf(f);"
        "Object.defineProperty(fp, 'missing', {get() { return 6; }, configurable: true}); S(rf(f));"
        "function rg(o) { return o.missing; } var hits = 0, px = new Proxy({}, {get() { hits++; return undefined; }}),"
        "  g = Object.create(px); for (k = 0; k < 400; k++) rg(g); S(rg(g)); S(hits);"
        "function rl(o) { return o.length; } function r3(o) { return o['3']; } var arr = [0, 1, 2];"
        "for (k = 0; k < 400; k++) { rl(arr); r3(arr); } S(rl(arr)); S(r3(arr)); arr.push(9); S(rl(arr)); S(r3(arr));"
        "function ri(o) { return o.missing; } var five = [{missing: 'i1'}, {a: 1}, {b: 1}, {missing: 'i2', c: 1}, {d: 1}];"
        "for (k = 0; k < 400; k++) ri(five[k % 5]); for (k = 0; k < 5; k++) S(ri(five[k]));"
        "class A {} class B extends A {} class C extends B {} class D extends C { m() { return super.missing; } }"
        "function rj(o) { return o.missing; } var j = new C(), dd = new D();"
        "for (k = 0; k < 400; k++) { rj(j); dd.m(); } S(rj(j)); S(dd.m());"
        "A.prototype.missing = 'j'; S(rj(j)); S(dd.m()); B.prototype.missing = 'js'; S(rj(j)); S(dd.m());"
        "out.join(' ')";
    for (int mode = 0; mode < 3; ++mode) {
        script::Context::Options options;
        if (mode == 0)
            options.jit.mode = script::JitOptions::Mode::Off;
        else if (mode == 1)
            options.jit.threshold = 1;
        else
            options.jit.mode = script::JitOptions::Mode::Eager;
        js::Context ctx(options);
        CHECK_EQ(ctx.eval(program, "<test>").toString(),
                 std::string("undefined 1 2 3 undefined 4 5 6 undefined 401 3 undefined 4 9 i1 undefined undefined i2 "
                             "undefined undefined undefined j j js js"));
    }
}

TEST(aReadSiteThatMeetsTooManyShapesKeepsWhatItKnows)
{
    // Six shapes in turn at one read, then one with the property up the chain, one with a getter there, and one without
    // it, all in turn: a site past its room stops starting again, and every answer is still right, before and after
    // a prototype gains the property.
    const char* program =
        "var out = [], k, s = 0; function S(v) { out.push(String(v)); }"
        "function rx(o) { return o.x; }"
        "var objs = [{x: 1}, {x: 2, a: 0}, {x: 3, b: 0}, {x: 4, c: 0}, {x: 5, d: 0}, {x: 6, e: 0}];"
        "for (k = 0; k < 2400; k++) s += rx(objs[k % 6]); S(s);"
        "var o7 = Object.create({x: 7}); S(rx(o7)); objs.push(o7);"
        "var o8 = Object.create({get x() { return 8; }}); S(rx(o8)); objs.push(o8);"
        "var o9 = Object.create({y: 1}); objs.push(o9);"
        "s = 0; for (k = 0; k < 900; k++) { var v = rx(objs[k % 9]); s += v === undefined ? 100 : v; } S(s);"
        "Object.getPrototypeOf(o9).x = 9; s = 0; for (k = 0; k < 900; k++) s += rx(objs[k % 9]); S(s);"
        "objs[0].x = 10; S(rx(objs[0]));"
        "out.join(' ')";
    for (int mode = 0; mode < 3; ++mode) {
        script::Context::Options options;
        if (mode == 0)
            options.jit.mode = script::JitOptions::Mode::Off;
        else if (mode == 1)
            options.jit.threshold = 1;
        else
            options.jit.mode = script::JitOptions::Mode::Eager;
        js::Context ctx(options);
        CHECK_EQ(ctx.eval(program, "<test>").toString(), std::string("8400 7 8 13600 4500 10"));
    }
}

TEST(nativeCodeReadsWhatASiteLearnedAfterItWasCompiled)
{
    // Code compiled before its read site had learned anything reads what the site learns later, a first shape, a second
    // one, a shape changed by a delete and an add; anything else, up the chain, a getter, a string, still reads right.
    const char* program =
        "var out = [], k, s = 0, t = 0; function S(v) { out.push(String(v)); }"
        "function f(o) { return o === null ? -1 : o.x; }"
        "for (k = 0; k < 400; k++) s += f(null);"
        "var a = {x: 2}; for (k = 0; k < 400; k++) s += f(a);"
        "var b = {y: 0, x: 3}; for (k = 0; k < 400; k++) s += f(b);"
        "S(s); S(f({x: 'str'})); S(f(Object.create({x: 'up'}))); S(f({get x() { return 'got'; }})); S(f('abc'));"
        "function g(o) { return o === null ? 0 : o.x; }"
        "for (k = 0; k < 400; k++) g(null);"
        "var c = {x: 1, z: 2};"
        "for (k = 0; k < 400; k++) { t += g(c); if (k % 50 === 49) { delete c.x; t += g(c) === undefined ? 100 : 0; c.x = k; } }"
        "S(t); out.join(' ')";
    for (int mode = 0; mode < 3; ++mode) {
        script::Context::Options options;
        if (mode == 0)
            options.jit.mode = script::JitOptions::Mode::Off;
        else if (mode == 1)
            options.jit.threshold = 1;
        else
            options.jit.mode = script::JitOptions::Mode::Eager;
        js::Context ctx(options);
        CHECK_EQ(ctx.eval(program, "<test>").toString(), std::string("1600 str up got undefined 70500"));
    }
}

TEST(nativeCodeReadsAPrototypesPropertyASiteLearnedAfterItWasCompiled)
{
    // The same for a property up the chain: read while the prototypes stay as they were when the site learned it, and
    // whatever changes them, a new value, a getter, a delete, a property higher up, a new prototype, is seen.
    const char* program =
        "var out = [], k, s = 0; function S(v) { out.push(String(v)); }"
        "function f(o) { return o === null ? 0 : o.v; }"
        "for (k = 0; k < 400; k++) f(null);"
        "function P() {} P.prototype.v = 2; var p = new P();"
        "for (k = 0; k < 400; k++) s += f(p); S(s);"
        "P.prototype.v = 3; S(f(p)); Object.defineProperty(P.prototype, 'v', {get() { return 'g'; }, configurable: true}); S(f(p));"
        "delete P.prototype.v; S(f(p)); for (k = 0; k < 400; k++) f(p); S(f(p));"
        "Object.prototype.v = 'top'; S(f(p)); delete Object.prototype.v; S(f(p));"
        "p.v = 'own'; S(f(p)); Object.setPrototypeOf(p, {v: 'other'}); delete p.v; S(f(p));"
        "out.join(' ')";
    for (int mode = 0; mode < 3; ++mode) {
        script::Context::Options options;
        if (mode == 0)
            options.jit.mode = script::JitOptions::Mode::Off;
        else if (mode == 1)
            options.jit.threshold = 1;
        else
            options.jit.mode = script::JitOptions::Mode::Eager;
        js::Context ctx(options);
        CHECK_EQ(ctx.eval(program, "<test>").toString(), std::string("800 3 g undefined undefined top undefined own other"));
    }
}

TEST(nativeCodeCallsTheGetterItsSiteKnowsNow)
{
    // A getter on a prototype, read from native code after the prototypes changed and the site learned it again, a getter
    // replaced, turned into a value, deleted, and one that throws.
    const char* program =
        "var out = [], k, s = 0; function S(v) { out.push(String(v)); }"
        "class A { get v() { return this.k; } } function f(o) { return o.v; }"
        "var a = new A(); a.k = 1; for (k = 0; k < 400; k++) s += f(a); S(s);"
        "A.prototype.other = 1; s = 0; for (k = 0; k < 400; k++) s += f(a); S(s);"
        "Object.defineProperty(A.prototype, 'v', {get() { return 'new'; }, configurable: true}); S(f(a)); S(f(a));"
        "Object.defineProperty(A.prototype, 'v', {value: 'data', configurable: true}); S(f(a)); S(f(a));"
        "delete A.prototype.v; S(f(a)); S(f(a));"
        "Object.defineProperty(A.prototype, 'v', {get() { throw new Error('boom'); }, configurable: true});"
        "try { f(a); } catch (e) { S(e.message); } out.join(' ')";
    for (int mode = 0; mode < 3; ++mode) {
        script::Context::Options options;
        if (mode == 0)
            options.jit.mode = script::JitOptions::Mode::Off;
        else if (mode == 1)
            options.jit.threshold = 1;
        else
            options.jit.mode = script::JitOptions::Mode::Eager;
        js::Context ctx(options);
        CHECK_EQ(ctx.eval(program, "<test>").toString(), std::string("400 400 new new data data undefined undefined boom"));
    }
}

TEST(retiredCodeIsSweptWithoutReadingTheStackAtEveryCompile)
{
    // Two hundred functions compiled for numbers and again for strings, all of it under a recursion 2,000 frames deep:
    // the answers are right and the retired code that no frame is in still goes back, with the stack read only every
    // megabyte of retired code.
    script::Context::Options options;
    options.jit.mode = script::JitOptions::Mode::Eager;
    options.runtimeCode = true;
    options.stackLimit = 4000;
    js::Context ctx(options);
    ctx.eval("var fs = [];"
             "for (var k = 0; k < 200; k++)"
             "  fs.push(new Function('x', 'var s = 0; for (var i = 0; i < 40; i++) s = s + x + (i + ' + k + '); return s;'));"
             "function work() { var total = 0;"
             "  for (var k = 0; k < fs.length; k++) { for (var j = 0; j < 30; j++) total += fs[k](j);"
             "    var r = fs[k]('a'); total += r.length; } return total; }"
             "function deep(n) { return n > 0 ? deep(n - 1) : work(); }", "<test>");
    CHECK_EQ(ctx.eval("deep(2000)", "<test>").toString(), std::string("32068925"));
    CHECK_EQ(ctx.eval("work()", "<test>").toString(), std::string("32068925"));
    CHECK(ctx.stats().nativeCodeBytes < (std::uint64_t(4) << 20));
}

TEST(privateMethodsAreCalledThroughWhatTheirSiteKnows)
{
    // Private fields read and written and private methods called in hot loops, from a class made twice so that two private
    // names of the same spelling exist, #x in o, and a private method called on an object of the other class, which has
    // to throw a TypeError from native code as from the interpreter.
    const char* program =
        "var out = [], k; function S(v) { out.push(String(v)); }"
        "function make(tag) {"
        "  return class P {"
        "    #a = 1; #b = 2; #c = tag;"
        "    #sum() { return this.#a + this.#b; }"
        "    #scale(f) { this.#a *= f; return this; }"
        "    step() { this.#b += 1; return this.#sum(); }"
        "    grow(f) { return this.#scale(f).#sum(); }"
        "    static has(o) { return #a in o; }"
        "    static peek(o) { return o.#c; }"
        "    static callSum(o) { return o.#sum(); }"
        "  };"
        "}"
        "var A = make('A'), B = make('B'), a = new A(), b = new B(), t = 0;"
        "for (k = 0; k < 3000; k++) { t += a.step(); t += b.step(); if (k % 1000 === 999) t += a.grow(2); }"
        "S(t); S(A.has(a)); S(A.has(b)); S(A.peek(a)); S(B.peek(b));"
        "var errs = 0; for (k = 0; k < 600; k++) { try { A.callSum(k < 590 ? a : b); } catch (e) { if (e instanceof TypeError) errs++; } }"
        "S(errs); try { A.peek({}); } catch (e) { S(e.constructor.name); }"
        "out.join(' ')";
    for (int mode = 0; mode < 3; ++mode) {
        script::Context::Options options;
        if (mode == 0)
            options.jit.mode = script::JitOptions::Mode::Off;
        else if (mode == 1)
            options.jit.threshold = 1;
        else
            options.jit.mode = script::JitOptions::Mode::Eager;
        js::Context ctx(options);
        CHECK_EQ(ctx.eval(program, "<test>").toString(), std::string("9031020 true false A B 10 TypeError"));
    }
}

TEST(keyedReadsWithNamesNumbersAndOddKeys)
{
    // o[k] with k undefined, null, a boolean or the same words as strings, a symbol, names made at run time on an object
    // used as a dictionary, an index into an array, a typed array and a string, numbers as keys, -0, and a proxy.
    const char* program =
        "var out = []; function S(v) { out.push(String(v)); }"
        "var o = {}; o[undefined] = 'u'; o[null] = 'n'; o[true] = 't'; o[false] = 'f';"
        "var keys = [undefined, null, true, false, 'undefined', 'null', 'true', 'false'];"
        "function g(o, k) { return o[k]; } var s = '';"
        "for (var i = 0; i < 4000; i++) s = g(o, keys[i % 8]) + s.slice(0, 15); S(s);"
        "S(Object.keys(o).join()); var sym = Symbol('s'); o[sym] = 'sym'; S(g(o, sym));"
        "var d = {}; for (i = 0; i < 40; i++) d['k' + i] = i; var t = 0; for (i = 0; i < 4000; i++) t += g(d, 'k' + (i % 40)); S(t);"
        "S(g([1, 2, 3], 1)); S(g(new Uint8Array([4, 5]), 1)); S(g('str', 2)); S(g({7: 'seven'}, 7)); S(g({'7': 'x'}, '7')); S(g({7.5: 'f'}, 7.5)); S(g({0: 'z'}, -0));"
        "S(g(new Proxy({}, {get(t, k) { return typeof k + ':' + String(k); }}), undefined));"
        "out.join(' ')";
    for (int mode = 0; mode < 3; ++mode) {
        script::Context::Options options;
        if (mode == 0)
            options.jit.mode = script::JitOptions::Mode::Off;
        else if (mode == 1)
            options.jit.threshold = 1;
        else
            options.jit.mode = script::JitOptions::Mode::Eager;
        js::Context ctx(options);
        CHECK_EQ(ctx.eval(program, "<test>").toString(),
                 std::string("ftnuftnuftnuftnu undefined,null,true,false sym 78000 2 5 r seven x f z string:undefined"));
    }
}

TEST(fieldInitializersRunAsNativeCodeWhenNewIsDoneByTheHelper)
{
    // Objects made by new on a class known only at run time: six fields, two calling a function, one reading this, one
    // that throws on a late construction; a derived class whose base has fields, a private one among them.
    const char* program =
        "var out = [], k, calls = 0; function S(v) { out.push(String(v)); }"
        "function next() { return ++calls; } function build(C, x) { return new C(x); }"
        "class F {"
        "  a = 1; b = next(); c = 'c'; d = this.a + 10; e = next() * 2;"
        "  f = (() => { if (F.boom && calls > F.boom) throw new Error('init ' + calls); return 0; })();"
        "  constructor(x) { this.x = x; }"
        "}"
        "var s = 0; for (k = 0; k < 3000; k++) { var o = build(F, k); s += o.a + o.b + o.d + o.e + o.x + o.f; } S(s); S(calls);"
        "class Base { #p = 5; q = this.#p * 2; get p() { return this.#p; } }"
        "class Derived extends Base { r = this.q + 1; constructor() { super(); this.t = this.r + this.p; } }"
        "var u = 0; for (k = 0; k < 3000; k++) { var d = build(Derived); u += d.q + d.r + d.t + d.p; } S(u);"
        "F.boom = calls + 2; var made = 0;"
        "try { for (k = 0; k < 10; k++) { build(F, k); made++; } } catch (e) { S(e.message); } S(made);"
        "out.join(' ')";
    for (int mode = 0; mode < 3; ++mode) {
        script::Context::Options options;
        if (mode == 0)
            options.jit.mode = script::JitOptions::Mode::Off;
        else if (mode == 1)
            options.jit.threshold = 1;
        else
            options.jit.mode = script::JitOptions::Mode::Eager;
        js::Context ctx(options);
        CHECK_EQ(ctx.eval(program, "<test>").toString(), std::string("31540500 6000 126000 init 6004 1"));
    }
}

TEST(aBuiltInCalledThroughCallTakesAnyReceiver)
{
    // f.call(x, a) with f a built-in done in place, Array.prototype.push here: x is the receiver, whatever native code
    // holds it as, a number in a register included, and the code done in place reads its tag.
    const char* program =
        "var P = Array.prototype.push; function h(o, i) { return P.call(o, i); }"
        "var g = [], like = {length: 2}, r = [], k;"
        "for (k = 0; k < 3000; k++) h(k % 2 ? g : like, k);"
        "for (k = 0; k < 300; k++) { try { r.push(h(k % 3 ? 5 : [], k)); } catch (e) { r.push(e.constructor.name); } }"
        "[g.length, like.length, r.slice(0, 6).join()].join(' ')";
    for (int mode = 0; mode < 3; ++mode) {
        script::Context::Options options;
        if (mode == 0)
            options.jit.mode = script::JitOptions::Mode::Off;
        else if (mode == 1)
            options.jit.threshold = 1;
        else
            options.jit.mode = script::JitOptions::Mode::Eager;
        js::Context ctx(options);
        CHECK_EQ(ctx.eval(program, "<test>").toString(), std::string("1500 1502 1,1,1,1,1,1"));
    }
}

TEST(derivedClassConstructorsRunAsNativeCode)
{
    // Derived class constructors in native code: fields, super calls up two levels, new.target, returning an object, a
    // number or nothing before super(), this read before super(), super() twice, from an arrow function, a loop after it,
    // Reflect.construct with another new.target, extending Array, and an exception in the arguments of super().
    const char* program =
        "var out = [], k; function S(v) { out.push(String(v)); }"
        "class A { constructor(x) { this.x = x; } sum() { return this.x; } }"
        "class B extends A { y = 2; constructor(x) { super(x + 1); this.z = this.x * 10; } sum() { return super.sum() + this.y + this.z; } }"
        "class C extends B { constructor() { const before = new.target === C; super(3); this.w = before ? 1 : 0; } }"
        "var s = 0; for (k = 0; k < 3000; k++) { var b = new B(k % 7), c = new C(); s += b.sum() + c.sum() + c.w; } S(s);"
        "class D extends A { constructor(v) { if (v === 1) return {plain: true}; if (v === 2) return 5; if (v === 3) return; super(v); } }"
        "for (k = 0; k < 600; k++) new D(10); S(JSON.stringify(new D(1)));"
        "try { new D(2); } catch (e) { S(e.constructor.name); }"
        "try { new D(3); } catch (e) { S(e.constructor.name); }"
        "class E extends A { constructor(early) { if (early) this.q = 1; super(7); } }"
        "for (k = 0; k < 600; k++) new E(false);"
        "try { new E(true); } catch (e) { S(e.constructor.name); }"
        "class F extends A { constructor() { super(1); super(2); } }"
        "try { for (k = 0; k < 600; k++) new F(); } catch (e) { S(e.constructor.name); }"
        "class G extends A { constructor(...xs) { const f = () => super(xs.length); f(); this.t = new.target.name; } }"
        "var g; for (k = 0; k < 600; k++) g = new G(1, 2, 3); S(g.x + g.t);"
        "class H extends A { constructor() { super(5); for (let i = 0; i < 50; i++) this.x += i; } }"
        "var h; for (k = 0; k < 300; k++) h = new H(); S(h.x);"
        "class I extends C {} var i1 = Reflect.construct(B, [4], C); S(Object.getPrototypeOf(i1) === C.prototype); S(new I().w);"
        "class J extends Array { constructor(n) { super(n); this.tag = 'j'; } } var j; for (k = 0; k < 600; k++) j = new J(3); S(j.length + j.tag + Array.isArray(j));"
        "class K extends A { constructor(o) { super(o.v); } } var bad = 0; for (k = 0; k < 600; k++) { try { new K(k === 500 ? null : {v: k}); } catch (e) { bad++; } } S(bad);"
        "out.join(' ')";
    for (int mode = 0; mode < 3; ++mode) {
        script::Context::Options options;
        if (mode == 0)
            options.jit.mode = script::JitOptions::Mode::Off;
        else if (mode == 1)
            options.jit.threshold = 1;
        else
            options.jit.mode = script::JitOptions::Mode::Eager;
        js::Context ctx(options);
        CHECK_EQ(ctx.eval(program, "<test>").toString(), std::string("278934 {\"plain\":true} TypeError ReferenceError ReferenceError ReferenceError 3G 1230 true 0 3jtrue 1"));
    }
}

TEST(superWithSpreadArgumentsInNativeCode)
{
    // super(...xs), super(...arguments) and two spreads in a derived constructor that runs as native code.
    const char* program =
        "var out = [], k; function S(v) { out.push(String(v)); }"
        "class A { constructor(...xs) { this.n = xs.length; this.s = xs.reduce((a, b) => a + b, 0); } }"
        "class B extends A { constructor(...xs) { super(...xs, 1); } }"
        "class C extends A { constructor() { super(...arguments); } }"
        "class D extends B { constructor(a, b) { super(...[a, b], ...'xy'); } }"
        "var t = 0; for (k = 0; k < 3000; k++) { var b = new B(k, 2), c = new C(1, 2, 3), d = new D(1, 2); t += b.n + b.s + c.n + c.s + d.n; }"
        "S(t); S(new D(5, 6).s); out.join(' ')";
    for (int mode = 0; mode < 3; ++mode) {
        script::Context::Options options;
        if (mode == 0)
            options.jit.mode = script::JitOptions::Mode::Off;
        else if (mode == 1)
            options.jit.threshold = 1;
        else
            options.jit.mode = script::JitOptions::Mode::Eager;
        js::Context ctx(options);
        CHECK_EQ(ctx.eval(program, "<test>").toString(), std::string("4558500 11xy1"));
    }
}

TEST(superCalledTwiceIsAReferenceErrorOnceTheSuperConstructorReturns)
{
    // super(super()) and a second super() caught in the constructor: the binding of this is checked after the super
    // constructor has run, and the first object stays this.
    const char* program =
        "class Base {}"
        "var r = [];"
        "for (var k = 0; k < 600; k++) {"
        "  try { new (class C extends Base { constructor() { super(super()); } })(); r.push('none'); } catch (e) { r.push(e.constructor.name); }"
        "}"
        "class T extends Base { constructor() { super(); try { super(); } catch (e) { this.caught = e.constructor.name; } } }"
        "var t; for (k = 0; k < 600; k++) t = new T();"
        "[r[0], r[599], t.caught, t instanceof T].join(' ')";
    for (int mode = 0; mode < 3; ++mode) {
        script::Context::Options options;
        if (mode == 0)
            options.jit.mode = script::JitOptions::Mode::Off;
        else if (mode == 1)
            options.jit.threshold = 1;
        else
            options.jit.mode = script::JitOptions::Mode::Eager;
        js::Context ctx(options);
        CHECK_EQ(ctx.eval(program, "<test>").toString(), std::string("ReferenceError ReferenceError ReferenceError true"));
    }
}

TEST(namesAreFoundAmongManyLocalsAndScopes)
{
    // A function with more locals than are looked at one by one: the innermost of a name is found, shadowing in blocks and
    // loops, in a top level of forty variables and in a function of fourteen.
    const char* program =
        "var src = 'var out = [];';"
        "for (var i = 0; i < 40; i++) src += 'var v' + i + ' = ' + i + ';';"
        "src += '{ let v3 = \"inner\"; { let v3 = \"deeper\"; out.push(v3); } out.push(v3); } out.push(v3);';"
        "src += 'function f(v5) { let v7 = v5 * 2; { const v7 = \"c\"; out.push(v7); } return v7 + v39; } out.push(f(4));';"
        "src += 'for (let v0 = 100; v0 < 102; v0++) out.push(v0); out.push(v0); out.join(\",\")';"
        "var r = (0, eval)(src);"
        "var many = function () { var a0 = 0, a1 = 1, a2 = 2, a3 = 3, a4 = 4, a5 = 5, a6 = 6, a7 = 7, a8 = 8, a9 = 9, a10 = 10, a11 = 11, a12 = 12, a13 = 13;"
        "  { let a13 = 'x'; { let a0 = 'y'; r += a0 + a13; } r += a0; } return a13 + a12; };"
        "r + '|' + many()";
    script::Context::Options options;
    options.runtimeCode = true;
    js::Context ctx(options);
    CHECK_EQ(ctx.eval(program, "<test>").toString(), std::string("deeper,inner,3,c,47,100,101,0|25"));
}

TEST(whatNestedFunctionsMentionIsKeptInCellsByEveryRule)
{
    // Which locals nested functions mention, and so live in cells: through closures, arrows (this, super, new.target,
    // arguments), classes and fields, parameter defaults, shadowing blocks, loops, and names that are only property keys.
    const char* program =
        "var out = []; function S(v) { out.push(String(v)); }"
        "function counter() { var n = 0; return function () { return ++n; }; } var c = counter(); c(); S(c());"
        "function arrowThis() { var f = () => this.v; return f; } S(arrowThis.call({v: 'at'})());"
        "function nested() { var x = 1; function inner() { return () => x + 1; } x = 5; return inner()(); } S(nested());"
        "class B { m() { return 'Bm'; } } class D extends B { m() { var f = () => super.m() + '!'; return f(); } } S(new D().m());"
        "function NT() { var g = () => new.target === NT; return g(); } S(new NT() instanceof NT);"
        "class F { x = 1; y = () => this.x + 1; static s = (() => 'st')(); } S(new F().y() + F.s);"
        "function params(a, b = () => a) { a = 9; return b(); } S(params(3));"
        "function shadow() { var v = 'outer'; { let v = 'inner'; var g = () => v; } return g() + v; } S(shadow());"
        "var obj = { k: 1, m() { return [1, 2].map(x => x + this.k); } }; S(obj.m());"
        "function loop() { var fs = []; for (let i = 0; i < 3; i++) fs.push(() => i); return fs.map(f => f()).join(''); } S(loop());"
        "function arrowInFn() { return function () { return (() => this)(); }.call('t2'); } S(arrowInFn());"
        "function classInArrow() { return (() => class { z = this; })(); } S(typeof new (classInArrow.call({}))().z);"
        "function deep() { var a = 1; return () => () => () => a++; } var d3 = deep()()(); d3(); S(d3());"
        "function prop() { var q = 7; return { q, r: q, [q]: 'c' }; } S(JSON.stringify(prop()));"
        "function mem() { var key = 'k'; var o = {k: 'mk'}; return () => o[key] + o.key; } S(mem()());"
        "function argsCap() { return () => arguments[0]; } S(argsCap('ar')());"
        "out.join(' ')";
    for (int mode = 0; mode < 3; ++mode) {
        script::Context::Options options;
        if (mode == 0)
            options.jit.mode = script::JitOptions::Mode::Off;
        else if (mode == 1)
            options.jit.threshold = 1;
        else
            options.jit.mode = script::JitOptions::Mode::Eager;
        js::Context ctx(options);
        CHECK_EQ(ctx.eval(program, "<test>").toString(), std::string("2 at 6 Bm! true 2st 9 innerouter 2,3 012 t2 object 2 {\"7\":\"c\",\"q\":7,\"r\":7} mkundefined ar"));
    }
}

TEST(closuresOfOneFunctionShareTheirNameButEachKeepsItsOwnProperty)
{
    // Every closure of a function is named by the same string, made once; its name property is still its own to change.
    const char* program =
        "var out = [];"
        "function mk(i) { return function inner() { return i; }; }"
        "var fs = []; for (var k = 0; k < 5000; k++) fs.push(mk(k));"
        "out.push(fs[3].name, fs[4999].name, fs[10](), (() => 1).name, (function () {}).name);"
        "class K { static name2() {} } out.push(K.name, K.name2.name);"
        "var anon = function () {}; out.push(anon.name); var f2 = function named() {}; out.push(f2.name);"
        "fs[0].name = 'x'; out.push(fs[0].name, fs[1].name, Object.getOwnPropertyDescriptor(fs[1], 'name').configurable);"
        "Object.defineProperty(fs[2], 'name', {value: 'changed'}); out.push(fs[2].name, fs[5].name);"
        "async function af() {} function* gf() {} out.push(af.name, gf.name);"
        "out.join(\" \")";
    for (int mode = 0; mode < 3; ++mode) {
        script::Context::Options options;
        if (mode == 0)
            options.jit.mode = script::JitOptions::Mode::Off;
        else if (mode == 1)
            options.jit.threshold = 1;
        else
            options.jit.mode = script::JitOptions::Mode::Eager;
        js::Context ctx(options);
        CHECK_EQ(ctx.eval(program, "<test>").toString(), std::string("inner inner 10   K name2 anon named inner inner true changed inner af gf"));
    }
}

TEST(functionsTakeTheNameOfAComputedKeyOrAnAccessor)
{
    // SetFunctionName: a method, accessor or anonymous function under a computed key is named by the key (a symbol's in
    // brackets, an accessor's after get or set); a class with a static name method keeps that.
    const char* program =
        "var s = Symbol('q'), e = Symbol(), out = [];"
        "var o = {['a' + 1]() {}, [s]() {}, [e]() {}, ['f']: function () {}, ['ar']: () => 1, ['n']: function named() {}, get ['g' + 1]() {}, set ['g' + 1](v) {}, [2]: class {}, ['k']: class { static name() {} }};"
        "var d = Object.getOwnPropertyDescriptor(o, 'g1');"
        "out.push(o.a1.name, o[s].name, o[e].name, o.f.name, o.ar.name, o.n.name, d.get.name, d.set.name, o[2].name, typeof o.k.name);"
        "class C { ['m' + 2]() {} static get ['s' + 3]() {} ['x'] = function () {}; static ['y'] = () => 0; }"
        "out.push(C.prototype.m2.name, Object.getOwnPropertyDescriptor(C, 's3').get.name, new C().x.name, C.y.name);"
        "out.join(\",\")";
    for (int mode = 0; mode < 3; ++mode) {
        script::Context::Options options;
        if (mode == 0)
            options.jit.mode = script::JitOptions::Mode::Off;
        else if (mode == 1)
            options.jit.threshold = 1;
        else
            options.jit.mode = script::JitOptions::Mode::Eager;
        js::Context ctx(options);
        CHECK_EQ(ctx.eval(program, "<test>").toString(), std::string("a1,[q],,f,ar,named,get g1,set g1,2,function,m2,get s3,x,y"));
    }
}

TEST(aFinallyWithValuesUnderItRunsInNativeCode)
{
    // A try/finally inside a switch or a for-in has values under it on the stack: each way into the finally brings them
    // and each way back finds them; a JumpSub after a throw is never taken.
    const char* program =
        "var out = [], log = [];"
        "function sw(x) { switch (x) { case 1: try { log.push('t1'); return 'r1'; } finally { log.push('f1'); } case 2: try { log.push('t2'); break; } finally { log.push('f2'); } default: try { if (x > 5) throw new Error('big'); } catch (e) { log.push('c'); } finally { log.push('fd'); } } return 'end' + x; }"
        "function forin(o) { var s = ''; for (var k in o) { try { if (k === 'b') continue; if (k === 'd') break; s += k; } finally { s += '.'; } } return s; }"
        "function nested(n) { var t = 0; for (var i = 0; i < n; i++) { switch (i % 3) { case 0: try { t += 1; } finally { t += 10; } break; case 1: try { try { t += 2; if (i % 7 == 1) throw i; } finally { t += 20; } } catch (e) { t += 100; } break; default: t += 3; } } return t; }"
        "function dead(x) { switch (x) { case 1: try { throw \"d\"; } finally { log.push(\"fd2\"); } case 2: return 2; } }"
        "function throwsThrough(x) { switch (x) { case 1: try { throw 'boom'; } finally { log.push('ft'); } } }"
        "for (var k = 0; k < 3000; k++) { out[0] = sw(k % 8); out[1] = forin({a: 1, b: 2, c: 3, d: 4, e: 5}); out[2] = nested(50); out[4] = dead(2); try { dead(1); } catch (e) { out[5] = e; } try { throwsThrough(1); } catch (e) { out[3] = e; } }"
        "out.push(log.slice(-6).join(''), log.length);"
        "out.join(\" \")";
    for (int mode = 0; mode < 3; ++mode) {
        script::Context::Options options;
        if (mode == 0)
            options.jit.mode = script::JitOptions::Mode::Off;
        else if (mode == 1)
            options.jit.threshold = 1;
        else
            options.jit.mode = script::JitOptions::Mode::Eager;
        js::Context ctx(options);
        CHECK_EQ(ctx.eval(program, "<test>").toString(), std::string("end7 a..c.. 909 boom 2 d fd2ftcfdfd2ft 10500"));
    }
}

TEST(aForOfWithReturnAndContinueKeepsItsIteratorInNativeCode)
{
    // A for-of's continue polls inside its try, before the loop head's join moves the iterator to registers of its own.
    const char* program =
        "function verify(nodes, set) {"
        "    let preps = [];"
        "    for (let n of nodes) { if (n > 2) preps.push(n); }"
        "    for (let p of preps) { if (p > 100) return {result: false}; }"
        "    let c = 0;"
        "    for (let a of set) { if (a < 0) return {result: false, why: 'neg'}; if (a & 1) continue; c++; }"
        "    return {result: true, c};"
        "}"
        "var r, s = new Set([1, 2, 3, 4]); for (var i = 0; i < 5000; i++) r = verify([1, 2, 3, 4, 5], s);"
        "JSON.stringify(r)";
    for (int mode = 0; mode < 3; ++mode) {
        script::Context::Options options;
        if (mode == 0)
            options.jit.mode = script::JitOptions::Mode::Off;
        else if (mode == 1)
            options.jit.threshold = 1;
        else
            options.jit.mode = script::JitOptions::Mode::Eager;
        js::Context ctx(options);
        CHECK_EQ(ctx.eval(program, "<test>").toString(), std::string("{\"result\":true,\"c\":2}"));
    }
}

TEST(aLoopChecksItsTypedArraysOnceAndStillSeesThemReplaced)
{
    // A loop over global typed arrays checks each once ahead of it; replaced by another kind, a plain array or from a
    // call inside the loop, the access finds out.
    const char* program =
        "var out = [];"
        "var buf = new ArrayBuffer(4096), heapU16 = new Uint16Array(buf), heap16 = new Int16Array(buf);"
        "for (var i = 0; i < 2048; i++) heapU16[i] = i;"
        "function sum(n) { var s = 0; for (var k = 0; k < n; k++) { s = (s + heapU16[k & 1023]) | 0; heap16[(k & 511) + 1024] = k; } return s; }"
        "function swap() { heapU16 = new Uint8Array(4096); }"
        "function mixed(n) { var s = 0; for (var k = 0; k < n; k++) { if (k == 500) swap(); s = (s + heapU16[k & 1023]) | 0; } return s; }"
        "var t = 0; for (var r = 0; r < 300; r++) t = (t + sum(2000)) | 0; out.push(t);"
        "var saved = heapU16; heapU16 = new Float64Array(2048); heapU16[3] = 0.5; out.push(sum(4));"
        "heapU16 = [10, 20, 30, 40]; out.push(sum(4)); heapU16 = saved;"
        "out.push(mixed(1000)); heapU16 = saved;"
        "var det = new ArrayBuffer(64), view = new Uint16Array(det); heapU16 = view; out.push(sum(8));"
        "if (typeof structuredClone === 'function') { try { structuredClone(det, { transfer: [det] }); } catch (e) {} }"
        "out.push(sum(3)); heapU16 = saved;"
        "for (var r = 0; r < 200; r++) t = (t + sum(1500)) | 0; out.push(t, heap16[1100]);"
        "out.join(\" \")";
    for (int mode = 0; mode < 3; ++mode) {
        script::Context::Options options;
        if (mode == 0)
            options.jit.mode = script::JitOptions::Mode::Off;
        else if (mode == 1)
            options.jit.threshold = 1;
        else
            options.jit.mode = script::JitOptions::Mode::Eager;
        js::Context ctx(options);
        CHECK_EQ(ctx.eval(program, "<test>").toString(), std::string("299872800 0 100 124750 0 0 427238000 1100"));
    }
}

TEST(smallFunctionsRunInPlaceAndGiveWayWhenTheirGuessesFail)
{
    // Small callees run inside their caller's optimized code; another callee, an overflow, a string or a missing argument
    // sends the call back to the interpreter, with the caller as it was.
    const char* program =
        "var out=[];"
        "function mk(){var c=0;return function(d){c+=d;return c;};}"
        "function k1(n,f){var s=0;for(var i=0;i<n;i++){s=f(i&1);}return s;}"
        "var f=mk(); out.push(k1(5000,f));"
        "out.push(k1(10, function(d){ return \"x\"+d; }));"
        "var g=mk(); out.push(k1(5000,g));"
        "var h=mk(); h(2147483000); out.push(k1(3000,h));"
        "var s=mk(); s(\"str\"); out.push(k1(5,s));"
        "class P{constructor(x){this.x=x;} get2(o){return this.x*o.x;} }"
        "function k2(n,a,b){var t=0;for(var i=0;i<n;i++) t+=a.get2(b); return t;}"
        "var a=new P(3), b=new P(4); out.push(k2(5000,a,b));"
        "out.push(k2(3,a,{x:0.5})); out.push(k2(3,a,{y:1,x:\"7\"})); out.push(k2(2,a,{x:-0}).toString());"
        "out.push(1/k2(1,new P(0),new P(-5)));"
        "var big=new P(1<<20); out.push(k2(2,big,big));"
        "function sl(){ return this === undefined ? \"u\" : typeof this; }"
        "function k3(n){ var r; for (var i=0;i<n;i++) r=sl(); return r; }"
        "out.push(k3(5000));"
        "function add3(a,b,c){ return a+b+(c===undefined?100:c); }"
        "function k4(n){ var t=0; for (var i=0;i<n;i++) t+=add3(i,1); return t; }"
        "out.push(k4(5000));"
        "function id(x){ return x; }"
        "function k5(n,v){ var r; for (var i=0;i<n;i++) r=id(v); return r; }"
        "out.push(k5(5000,{q:1}).q, k5(10,\"s\"), k5(10,1.5));"
        "var o2={v:1, inc(){ this.v = this.v + 1; return this.v; }};"
        "function k6(n){ var t=0; for (var i=0;i<n;i++) t+=o2.inc(); return t; }"
        "out.push(k6(5000), o2.v);"
        "out.join(\" \")";
    for (int mode = 0; mode < 3; ++mode) {
        script::Context::Options options;
        if (mode == 0)
            options.jit.mode = script::JitOptions::Mode::Off;
        else if (mode == 1)
            options.jit.threshold = 1;
        else
            options.jit.mode = script::JitOptions::Mode::Eager;
        js::Context ctx(options);
        CHECK_EQ(ctx.eval(program, "<test>").toString(), std::string("2500 x1 2500 2147484500 0str01010 60000 4.5 63 0 Infinity 2199023255552 object 13002500 1 s 1.5 12507500 5001"));
    }
}

TEST(gettersAndSuperReadsFromCompiledCode)
{
    // Getters are called straight from compiled code and `super.x` reads are cached by slot: throwing getters, getters that
    // replace themselves, changed prototypes and deep recursion behave as in the interpreter.
    const char* program =
        "var out=[];"
        "class A{constructor(v){this.v=v;} get g(){ if (this.v===7) throw new Error(\"seven\"); return this.v*2; }}"
        "function run(o){ var s=0; for (var i=0;i<20;i++) s+=o.g; return s; }"
        "var a=new A(3); for (var k=0;k<300;k++) run(a);"
        "out.push(run(a));"
        "try { run(new A(7)); } catch(e){ out.push(e.message); }"
        "Object.defineProperty(A.prototype, \"g\", {get(){ return 100; }, configurable:true});"
        "out.push(run(a));"
        "var deep = { get d(){ return this.d + 1; } };"
        "function rd(o){ return o.d; }"
        "try { rd(deep); } catch(e) { out.push(e instanceof RangeError); }"
        "class B{ get h(){ var x = 1; Object.defineProperty(B.prototype, \"h\", {value: 5}); return x; } }"
        "function rh(o){ var s=0; for (var i=0;i<3;i++) s+=o.h; return s; }"
        "for (var k=0;k<200;k++) rh(new B());"
        "out.push(rh(new B()));"
        "class P{ m(){ return 1; } } class Q extends P{ m(){ return super.m()+10; } }"
        "var q=new Q(); var t=0; for (var k=0;k<5000;k++) t+=q.m();"
        "P.prototype.m = function(){ return 2; }; t += q.m();"
        "Object.setPrototypeOf(Q.prototype, { m(){ return 3; } }); t += q.m();"
        "out.push(t);"
        "var obj = { __proto__: { x: 1 }, f(){ return super.x; } }; var u=0; for (var k=0;k<3000;k++) u+=obj.f();"
        "Object.getPrototypeOf(obj).x = 5; u+=obj.f(); out.push(u);"
        "out.join()";
    for (int mode = 0; mode < 3; ++mode) {
        script::Context::Options options;
        if (mode == 0)
            options.jit.mode = script::JitOptions::Mode::Off;
        else if (mode == 1)
            options.jit.threshold = 1;
        else
            options.jit.mode = script::JitOptions::Mode::Eager;
        js::Context ctx(options);
        CHECK_EQ(ctx.eval(program, "<test>").toString(), std::string("120,seven,2000,true,15,55025,3005"));
    }
}

TEST(methodsGettersAndSuperCallsInlineInsideEachOther)
{
    // A method that calls `super.add` and a getter run inside their caller, the one inside the other; replacing the getter,
    // the method on the prototype or the prototype itself is noticed.
    const char* program =
        "var out=[];"
        "class V{constructor(x,y){this.x=x;this.y=y;}add(o){return this.x*o.x+this.y*o.y;}get m(){return this.x;}}"
        "class W extends V{add(o){return super.add(o)+1;}}"
        "function kernel(n,a,b){var s=0;for(var i=0;i<n;i++){s+=a.add(b);s+=b.add(a);s+=a.m;}return s;}"
        "var a=new V(1,2), b=new W(3,4);"
        "out.push(kernel(5000,a,b));"
        "Object.defineProperty(V.prototype,\"m\",{get(){return 1000;},configurable:true}); out.push(kernel(2,a,b));"
        "V.prototype.add=function(o){return -1;}; out.push(kernel(2,a,b));"
        "Object.setPrototypeOf(W.prototype,{add(o){return 50;}}); out.push(kernel(2,a,b));"
        "class X{ get p(){ return this.q * 2; } constructor(){ this.q = 1; } }"
        "function kx(n,o){ var s=0; for (var i=0;i<n;i++) s+=o.p; return s; }"
        "var xo=new X(); out.push(kx(5000,xo)); xo.q=1.5; out.push(kx(3,xo)); xo.q=\"s\"; out.push(kx(2,xo));"
        "var yo=Object.create(xo); yo.q=7; out.push(kx(3,yo));"
        "out.join(\" \")";
    for (int mode = 0; mode < 3; ++mode) {
        script::Context::Options options;
        if (mode == 0)
            options.jit.mode = script::JitOptions::Mode::Off;
        else if (mode == 1)
            options.jit.threshold = 1;
        else
            options.jit.mode = script::JitOptions::Mode::Eager;
        js::Context ctx(options);
        CHECK_EQ(ctx.eval(program, "<test>").toString(), std::string("120000 2046 1998 2100 10000 9 NaN 42"));
    }
}

TEST(callsOfGlobalFunctionsFromOptimizedCode)
{
    // A global function called from optimized code is checked at the call and its frame pushed from constants; replacing
    // the function, a throw, running out of stack and an exit between the read of the global and the call all behave.
    const char* program =
        "var out=[];"
        "function sq(x){ return x*x; }"
        "function k1(n){ var s=0; for (var i=0;i<n;i++){ s=(s+sq(i&7))|0; if (i===3000) sq=function(x){return -x;}; } return s; }"
        "out.push(k1(6000));"
        "function thr(x){ if (x===777) throw new Error(\"e\"+x); return x; }"
        "function k2(n){ var s=0; for (var i=0;i<n;i++) s+=thr(i); return s; }"
        "try { k2(1000); } catch(e){ out.push(e.message); }"
        "function rec(n){ return n===0?0:1+rec(n-1); }"
        "try { out.push(rec(1e6)); } catch(e){ out.push(e instanceof RangeError); }"
        "out.push(rec(400));"
        "function ov(x){ return x; }"
        "function k3(n){ var s=0, b=2147483000; for (var i=0;i<n;i++){ s+=ov(b+i); } return s; }"
        "out.push(k3(3000));"
        "var cnt=0; function g(){ cnt++; if (cnt===2000) { globalThis.g = function(){ return 100; }; } return 1; }"
        "function k4(n){ var s=0; for (var i=0;i<n;i++) s+=g(); return s; }"
        "out.push(k4(4000));"
        "function self(){ return this === undefined ? 1 : 2; }"
        "function k5(n){ var s=0; for (var i=0;i<n;i++) s+=self(); return s; }"
        "out.push(k5(3000));"
        "function one(x){ return 1; } var fn = one;"
        "function k6(n){ var s=0, b=2147482000; for (var i=0;i<n;i++){ if (i===1000) fn=function(x){return 2;}; s+=fn(b+i); } return s; }"
        "out.push(k6(3000));"
        "out.join(\" \")";
    for (int mode = 0; mode < 3; ++mode) {
        script::Context::Options options;
        if (mode == 0)
            options.jit.mode = script::JitOptions::Mode::Off;
        else if (mode == 1)
            options.jit.threshold = 1;
        else
            options.jit.mode = script::JitOptions::Mode::Eager;
        js::Context ctx(options);
        CHECK_EQ(ctx.eval(program, "<test>").toString(), std::string("42000 e777 true 400 6442453498500 202000 6000 5000"));
    }
}

TEST(constructorCallsFromCompiledCode)
{
    // `new` of a compiled function pushes its frame from compiled code: results that are objects or not, derived classes,
    // `new.target`, a prototype that is not an object, a throw, an arrow and `arguments` all behave.
    const char* program =
        "var out=[];"
        "function P(x){ this.x=x; }"
        "function R(x){ this.x=x; return {y:x}; }"
        "function Q(x){ this.x=x; return 5; }"
        "function NT(){ this.t = new.target === NT; }"
        "class A{ constructor(v){ this.v=v; } }"
        "class B extends A{ constructor(v){ super(v*2); this.w=1; } }"
        "function T(x){ if (x===99999) throw new Error(\"bad\"+x); this.x=x; }"
        "function k(n){ var s=0; for (var i=0;i<n;i++){ s+=new P(i).x + new R(i).y + new Q(i).x + (new NT().t?1:0) + new A(i).v + new B(i).v + new T(i).x; } return s; }"
        "out.push(k(3000));"
        "P.prototype = 7; out.push(Object.getPrototypeOf(new P(1)) === Object.prototype);"
        "P.prototype = {z:3}; out.push(new P(1).z);"
        "try { k(1000); for (var i=0;i<1000;i++) new T(i===500?99999:i); } catch(e){ out.push(e.message); }"
        "function Sloppy(){ this.g = (this instanceof Sloppy); }"
        "var c=0; for (var i=0;i<3000;i++) if (new Sloppy().g) c++; out.push(c);"
        "var arrow = () => 1; try { for (var i=0;i<3000;i++) new arrow(); } catch(e){ out.push(e instanceof TypeError); }"
        "function Args(){ this.n = arguments.length; } var an=0; for (var i=0;i<3000;i++) an+=new Args(1,2).n; out.push(an);"
        "out.join(\" \")";
    for (int mode = 0; mode < 3; ++mode) {
        script::Context::Options options;
        if (mode == 0)
            options.jit.mode = script::JitOptions::Mode::Off;
        else if (mode == 1)
            options.jit.threshold = 1;
        else
            options.jit.mode = script::JitOptions::Mode::Eager;
        js::Context ctx(options);
        CHECK_EQ(ctx.eval(program, "<test>").toString(), std::string("31492500 true 3 bad99999 3000 true 6000"));
    }
}

TEST(stringLengthsMethodsAndCharacterCodesFromCompiledCode)
{
    // Compiled code reads a string's length, the methods of String.prototype and `charCodeAt` itself: ropes, slices, wrong
    // indices, String objects and replaced or added methods behave.
    const char* program =
        "var out=[];"
        "function h(s){ var x=0; for (var i=0;i<s.length;i++) x=(x*31+s.charCodeAt(i))|0; return x; }"
        "var base=\"abcdefghij\"; var rope=\"\"; for (var i=0;i<50;i++) rope+=base;"
        "for (var k=0;k<200;k++) h(base);"
        "out.push(h(base), h(rope), h(rope.slice(3,40)), h(rope.substring(7)), h(\"\"));"
        "function at(s,i){ return s.charCodeAt(i); }"
        "for (var k=0;k<3000;k++) at(\"xyz\",1);"
        "out.push(at(\"xyz\",5), at(\"xyz\",-1), at(\"xyz\",1.5), at(new String(\"abc\"),1), at(\"é☃\",1));"
        "var sp=String.prototype.charCodeAt; String.prototype.charCodeAt=function(){ return 42; }; out.push(at(\"abc\",0)); String.prototype.charCodeAt=sp;"
        "function len(s){ return s.length; } for (var k=0;k<3000;k++) len(\"abcd\"); out.push(len(\"hello\"), len([1,2,3]), len({length:9}), len(new String(\"ab\")));"
        "function up(s){ return s.toUpperCase(); } for (var k=0;k<3000;k++) up(\"a\"); String.prototype.toUpperCase=function(){return \"Z\";}; out.push(up(\"q\"));"
        "String.prototype.newm = function(){ return this.length*2; }; function nm(s){ return s.newm(); } for (var k=0;k<3000;k++) nm(\"ab\"); out.push(nm(\"abc\"));"
        "out.join(\" \")";
    for (int mode = 0; mode < 3; ++mode) {
        script::Context::Options options;
        if (mode == 0)
            options.jit.mode = script::JitOptions::Mode::Off;
        else if (mode == 1)
            options.jit.threshold = 1;
        else
            options.jit.mode = script::JitOptions::Mode::Eager;
        js::Context ctx(options);
        CHECK_EQ(ctx.eval(program, "<test>").toString(),
                 std::string("-634317659 1473919994 -362063818 -798229026 0 NaN NaN 121 98 9731 42 5 3 9 2 Z 6"));
    }
}

TEST(chainsOfReadsFromOptimizedCode)
{
    // `os[i & 3].x` and `a.b.c.d` pass their objects along without registers; a new shape, a missing element, a getter
    // for the global or an overflow beside the chain sends it back to the interpreter from its start.
    const char* program =
        "var out=[];"
        "var os=[{x:1},{y:0,x:2},{z:0,y:0,x:3},{w:0,z:0,y:0,x:4}];"
        "function k1(n){var s=0;for(var i=0;i<n;i++){s+=os[i&3].x;}return s;}"
        "out.push(k1(5000));"
        "os[2]={q:1,x:30}; out.push(k1(8));"
        "os[1]=undefined; try { k1(8); } catch(e){ out.push(e.constructor.name); }"
        "os=[{x:5}]; out.push(k1(1));"
        "var arr=[1,,3]; os=[{x:1},{x:2},{x:3},{x:4}];"
        "Object.defineProperty(globalThis, \"os2\", { get(){ return [{x:7},{x:7},{x:7},{x:7}]; }, configurable:true });"
        "function k2(n){var s=0;for(var i=0;i<n;i++){s+=os2[i&3].x;}return s;} out.push(k2(3000));"
        "var deep={a:{b:{c:{d:5}}}}; function k3(n){var s=0;for(var i=0;i<n;i++) s+=deep.a.b.c.d; return s;} out.push(k3(5000)); deep.a.b={c:{d:1}}; out.push(k3(3)); deep.a.b.c.d=\"z\"; out.push(k3(2));"
        "var big=[{x:2147483647}]; function k4(n){var s=0; for (var i=0;i<n;i++){ var j=(i+2147483640)|0; s+=big[0].x; } return s;} out.push(k4(5000));"
        "function k5(n,o){var s=0;for(var i=0;i<n;i++){ var t=i+2147483000; s+=o[0].x; } return s;} out.push(k5(3000,[{x:1}]));"
        "out.join(\" \")";
    for (int mode = 0; mode < 3; ++mode) {
        script::Context::Options options;
        if (mode == 0)
            options.jit.mode = script::JitOptions::Mode::Off;
        else if (mode == 1)
            options.jit.threshold = 1;
        else
            options.jit.mode = script::JitOptions::Mode::Eager;
        js::Context ctx(options);
        CHECK_EQ(ctx.eval(program, "<test>").toString(), std::string("12500 74 TypeError 5 21000 25000 3 0zz 10737418235000 3000"));
    }
}

TEST(smallConstructorsRunInPlace)
{
    // `new T(a, b)` of a constructor that only stores into `this` makes the object in place; a new prototype, a setter on
    // it, a replaced constructor, constants, missing arguments and a constructor returning an object all behave.
    const char* program =
        "var out=[];"
        "function T(l,r){this.l=l;this.r=r;}"
        "function mk(n){ var t=null; for (var i=0;i<n;i++) t=new T(t,i); return t; }"
        "function len(t){ var c=0; while (t) { c+=t.r; t=t.l; } return c; }"
        "out.push(len(mk(5000)));"
        "T.prototype.sum=function(){ return this.r; }; out.push(mk(3).sum());"
        "T.prototype={kind:\"new\"}; out.push(mk(2).kind, Object.getPrototypeOf(mk(1))===T.prototype);"
        "Object.defineProperty(T.prototype, \"l\", {set(v){ out.push(\"setter\"); }, configurable:true}); var x=mk(1); out.push(x.hasOwnProperty(\"l\"));"
        "T.prototype={}; "
        "function U(a){ this.a=a; this.b=7; this.c=undefined; }"
        "function mku(n){ var s=0; for (var i=0;i<n;i++){ var u=new U(i); s+=u.a+u.b; } return s; } out.push(mku(5000), mku(3));"
        "function V(a,b){ this.a=a; this.b=b; } function mkv(n){ var r; for (var i=0;i<n;i++) r=new V(i); return r; } out.push(mkv(3000).b, \"b\" in mkv(2));"
        "var keep=T; function mkt(n){ var r; for (var i=0;i<n;i++) r=new T(i,i); return r; } mkt(3000); T=function(l,r){ this.z=1; }; out.push(mkt(2).z);"
        "function W(x){ this.x=x; return {w:1}; } function mkw(n){ var r; for (var i=0;i<n;i++) r=new W(i); return r; } out.push(mkw(3000).w);"
        "out.join(\" \")";
    for (int mode = 0; mode < 3; ++mode) {
        script::Context::Options options;
        if (mode == 0)
            options.jit.mode = script::JitOptions::Mode::Off;
        else if (mode == 1)
            options.jit.threshold = 1;
        else
            options.jit.mode = script::JitOptions::Mode::Eager;
        js::Context ctx(options);
        CHECK_EQ(ctx.eval(program, "<test>").toString(), std::string("12497500 2 new true setter false 12532500 24  true 1 1"));
    }
}

TEST(inlinedCallsReadTheirArgumentsWhereTheyAre)
{
    // The callee, `this` and the arguments of an inlined call are not copied into its registers; another callee, a
    // replaced method, another receiver or argument, and `this` read after the call all behave.
    const char* program =
        "var out=[];"
        "function mk(){var c=0;return function(d){c+=d;return c;};}"
        "function k1(n,f,g){var s=0;for(var i=0;i<n;i++){s=f(i&1)+g(1);}return s;}"
        "out.push(k1(5000,mk(),mk())); out.push(k1(3,mk(),function(d){return \"s\"+d;}));"
        "class P{constructor(x){this.x=x;} m(o){return this.x+o;}}"
        "function k2(n,a){var t=0;for(var i=0;i<n;i++){t+=a.m(i);}return t;}"
        "var a=new P(2); out.push(k2(5000,a)); P.prototype.m=function(o){return -o;}; out.push(k2(3,a));"
        "out.push(k2(3,{x:1,m(o){return 100;}}), k2(2,new P(1.5)));"
        "function k3(n,a){var t=0;for(var i=0;i<n;i++){t+=a.m(i); t+=a.x;}return t;} P.prototype.m=function(o){return this.x*o;};"
        "out.push(k3(4000,new P(3)), k3(2,new P(\"q\")));"
        "function k4(n,o){ var r=0; for (var i=0;i<n;i++){ var self=o; r+=self.m(1); r+=self.x; } return r; } out.push(k4(3000,new P(2)));"
        "out.join(\" \")";
    for (int mode = 0; mode < 3; ++mode) {
        script::Context::Options options;
        if (mode == 0)
            options.jit.mode = script::JitOptions::Mode::Off;
        else if (mode == 1)
            options.jit.threshold = 1;
        else
            options.jit.mode = script::JitOptions::Mode::Eager;
        js::Context ctx(options);
        CHECK_EQ(ctx.eval(program, "<test>").toString(), std::string("7500 1s1 12507500 -3 300 -1 24006000 NaNqNaNq 12000"));
    }
}

TEST(pushAndPopFromCompiledCode)
{
    // `push` and `pop` on a plain array are done by compiled code; frozen and non-extensible arrays, holes, array-likes,
    // a replaced `push` and arrays of objects behave as in the interpreter.
    const char* program =
        "var out=[];"
        "function k(n){ var a=[]; for (var i=0;i<n;i++) a.push(i%3?i:\"s\"+i); var s=0; while (a.length){ var v=a.pop(); s+= typeof v===\"number\"? v : v.length; } return s; }"
        "out.push(k(5000));"
        "var fr=Object.freeze([1,2]); try { fr.push(3); } catch(e){ out.push(e.constructor.name); } try { fr.pop(); } catch(e){ out.push(e.constructor.name); }"
        "var ne=[1,2]; Object.preventExtensions(ne); try { ne.push(3); } catch(e){ out.push(e.constructor.name); } out.push(ne.pop(), ne.length);"
        "var h=[1,,3]; out.push(h.pop(), h.pop(), h.pop(), h.length);"
        "Array.prototype[1]=\"P\"; var h2=[1,,3]; h2.pop(); out.push(h2.pop()); delete Array.prototype[1];"
        "var al={length:2,0:\"a\",1:\"b\"}; out.push(Array.prototype.pop.call(al), al.length);"
        "var nums=[]; for (var i=0;i<3000;i++) nums.push(i*0.5); out.push(nums.length, nums[2999]);"
        "function p2(a,v){ return a.push(v); } var arr=[]; for (var i=0;i<3000;i++) p2(arr,i); out.push(p2(arr,\"x\"), arr.length);"
        "var saved=Array.prototype.push; Array.prototype.push=function(){ return -1; }; out.push(p2([],1)); Array.prototype.push=saved;"
        "var t=[]; for (var i=0;i<100;i++) t.push({i:i}); var c=0; while (t.length) c+=t.pop().i; out.push(c);"
        "out.join(\" \")";
    for (int mode = 0; mode < 3; ++mode) {
        script::Context::Options options;
        if (mode == 0)
            options.jit.mode = script::JitOptions::Mode::Off;
        else if (mode == 1)
            options.jit.threshold = 1;
        else
            options.jit.mode = script::JitOptions::Mode::Eager;
        js::Context ctx(options);
        CHECK_EQ(ctx.eval(program, "<test>").toString(),
                 std::string("8339630 TypeError TypeError TypeError 2 1 3  1 0  b 1 3000 1499.5 3001 3001 4950"));
    }
}

TEST(aProductOfInt32sThatIsANegativeZeroLeavesTheInt32s)
{
    // `a * b` of int32s that is zero is -0 when either operand is negative, whether the operands are in registers or not.
    const char* program =
        "function f(a,b){ var s=0; for (var i=0;i<3000;i++){ var x = a*b; s += 1/x; } return s; }"
        "var r1 = [f(-5,0), f(5,0), f(0,-5), f(-3,2)].join(\" \");"
        "function g(n){ var s=0; var a=-1,b=0,c=0,d=0,e=0,f1=0,g1=0,h=0,j=0; for (var i=0;i<n;i++){ a=-((i&3)+1); b+=a; c+=b; d+=c; e+=d; f1+=e; g1+=f1; h+=g1; var z = a*j; s += 1/z; } return [s, h]; }"
        "r1 + \" \" + g(100).join(\",\")";
    for (int mode = 0; mode < 3; ++mode) {
        script::Context::Options options;
        if (mode == 0)
            options.jit.mode = script::JitOptions::Mode::Off;
        else if (mode == 1)
            options.jit.threshold = 1;
        else
            options.jit.mode = script::JitOptions::Mode::Eager;
        js::Context ctx(options);
        CHECK_EQ(ctx.eval(program, "<test>").toString(),
                 std::string("-Infinity Infinity -Infinity -500.0000000000219 -Infinity,-58855775225"));
    }
}

TEST(storesIntoHolesAndTypedArraysFromCompiledCode)
{
    // A store into a hole of a plain array skips the prototype chain only while no prototype has an index property; typed
    // array stores convert, wrap and ignore what is out of range.
    const char* program =
        "var log = [];"
        "var b = new Array(5); function fill(a) { for (var i = 0; i < a.length; i++) a[i] = i; return a; }"
        "for (var k = 0; k < 3000; k++) fill(new Array(8));"
        "Object.defineProperty(Object.prototype, 2, { set(v) { log.push(\"o\" + v); }, configurable: true });"
        "var r1 = fill(new Array(5)).join();"
        "delete Object.prototype[2];"
        "Object.defineProperty(Array.prototype, 3, { set(v) { log.push(\"a\" + v); }, configurable: true });"
        "var r2 = fill(new Array(5)).join();"
        "delete Array.prototype[3]; Array.prototype.length = 0;"
        "var r3 = fill(new Array(5)).join();"
        "var e = new Float64Array(4), u = new Uint8Array(4), q = new Int32Array(4);"
        "function put(t, v) { for (var i = -1; i <= t.length; i++) t[i] = v; var s = 0; for (var i = 0; i < t.length; i++) s += t[i]; return s; }"
        "for (var k = 0; k < 3000; k++) { put(e, 1.5); put(u, 300); put(q, 7); }"
        "[r1, r2, r3, log.join(), put(e, 0.25), put(u, 257), put(q, 2.5e9), put(q, -3)].join(\" \")";
    for (int mode = 0; mode < 3; ++mode) {
        script::Context::Options options;
        if (mode == 0)
            options.jit.mode = script::JitOptions::Mode::Off;
        else if (mode == 1)
            options.jit.threshold = 1;
        else
            options.jit.mode = script::JitOptions::Mode::Eager;
        js::Context ctx(options);
        CHECK_EQ(ctx.eval(program, "<test>").toString(), std::string("0,1,,3,4 0,1,2,,4 0,1,2,3,4 o2,a3 1 4 -7179869184 -12"));
    }
}

TEST(aLocalGivenOnlyNumbersStaysANumberAndFindsOutOtherwise)
{
    // A local the interpreter has only ever given numbers is a number in native code even where what is stored has no
    // known kind: a string, an object, -0, a fraction or one too large for an int32 later bails, and is right.
    const char* program =
        "var out = [], arr = [1, 2, 3, {}, 'x', 2.5, -0, 1e10, NaN];"
        "function f(a, n) { var r, s = 0, z; for (var i = 0; i < n; i++) { r = a[i % 3]; s = (s + r) | 0; } z = a[(n % 9)]; r = z; return [s, typeof r, 1 / r]; }"
        "for (var k = 0; k < 3000; k++) out[0] = f(arr, 50).join();"
        "for (var k = 4; k < 9; k++) out.push(f(arr, k + 9 * 3).join());"
        "var t = 0; function g(o) { var x, y = 0; for (var i = 0; i < 100; i++) { x = o.v; y = (y + x) | 0; } return y + '/' + x; }"
        "for (var k = 0; k < 2000; k++) t = g({v: k & 7}); out.push(t, g({v: 'q'}), g({v: 1.5}), g({v: -0}), g({}));"
        "out.join(\" \")";
    for (int mode = 0; mode < 3; ++mode) {
        script::Context::Options options;
        if (mode == 0)
            options.jit.mode = script::JitOptions::Mode::Off;
        else if (mode == 1)
            options.jit.threshold = 1;
        else
            options.jit.mode = script::JitOptions::Mode::Eager;
        js::Context ctx(options);
        CHECK_EQ(ctx.eval(program, "<test>").toString(), std::string("99,number,0.4 61,string,NaN 63,number,0.4 66,number,-Infinity 67,number,1e-10 69,number,NaN 700/7 0/q 100/1.5 0/0 0/undefined"));
    }
}

TEST(typeofAndNamesCompareByPointerInNativeCode)
{
    // Strict equality with a string constant: another interned string is equal only if it is the same; no string at all
    // is not equal; a string made at run time is compared by its text.
    const char* program =
        "var out = [];"
        "function kind(x) { switch (typeof x) { case 'undefined': return 0; case 'object': return x ? 1 : 2; case 'number': return 3; case 'string': return 4; case 'boolean': return 5; case 'function': return 6; default: return 7; } }"
        "function nm(s) { if (s === 'alpha') return 1; if (s !== 'beta') return 2; return 3; }"
        "var t = 0; for (var i = 0; i < 20000; i++) t += kind([1, 'a', null, {}, undefined, true, kind, Symbol()][i & 7]) + nm(['alpha', 'beta', 'gam' + 'ma', 'al' + 'pha', 5, null][i % 6]);"
        "out.push(t, nm('al' + 'pha'), nm('be' + 'ta'), nm(1), kind(1n));"
        "out.join(\" \")";
    for (int mode = 0; mode < 3; ++mode) {
        script::Context::Options options;
        if (mode == 0)
            options.jit.mode = script::JitOptions::Mode::Off;
        else if (mode == 1)
            options.jit.threshold = 1;
        else
            options.jit.mode = script::JitOptions::Mode::Eager;
        js::Context ctx(options);
        CHECK_EQ(ctx.eval(program, "<test>").toString(), std::string("106667 1 3 2 7"));
    }
}

TEST(polymorphicReadsAndPrototypeMethodsFromCompiledCode)
{
    // Objects of one shape with different numbers of inline slots, a prototype that grows and whose method and getter are
    // replaced under compiled callers, receivers of a few shapes, and a receiver register written after its method read.
    const char* program =
        "var out = [];"
        "function P(x, y) { this.x = x; this.y = y; }"
        "function mkLit(x, y) { var o = {}; o.x = x; o.y = y; return o; }"
        "function sum(o) { return o.x + o.y; }"
        "var s = 0; for (var i = 0; i < 20000; i++) s += sum(new P(i, 1));"
        "for (var i = 0; i < 20000; i++) s += sum(i & 1 ? new P(i, 2) : mkLit(i, 3)); out.push(s);"
        "class A { constructor(v) { this.v = v; } f() { return this.v + 1; } get g() { return this.v * 2; } }"
        "class B extends A { f() { return super.f() + 10; } }"
        "function run(a, b, n) { var t = 0; for (var i = 0; i < n; i++) t += a.f() + b.f() + a.g; return t; }"
        "var a = new A(1), b = new B(2); out.push(run(a, b, 50000));"
        "for (var k = 0; k < 20; k++) A.prototype['p' + k] = k; out.push(run(a, b, 50000));"
        "A.prototype.f = function () { return 100; }; out.push(run(a, b, 50000));"
        "Object.defineProperty(A.prototype, 'g', { get() { return -1; }, configurable: true }); out.push(run(a, b, 50000));"
        "function Q(a) { this.a = a; this.b = a; } function R(a) { this.z = 0; this.a = a; this.b = a; }"
        "function dot(p, q) { return p.a * q.a + p.b * q.b + p.a; }"
        "var qs = [new Q(1), new R(2), new Q(3), new R(4)], u = 0;"
        "for (var i = 0; i < 100000; i++) u += dot(qs[i & 3], qs[(i + 1) & 3]); out.push(u);"
        "qs.push({ a: 5, b: 6 }); qs.push({ b: 7, a: 8 });"
        "for (var i = 0; i < 100000; i++) u += dot(qs[i % 6], qs[(i + 1) % 6]); out.push(u);"
        "function W() { this.k = 1; } W.prototype.m = function () { return this.k; };"
        "function sk(n) { var w = new W(), t = 0; for (var i = 0; i < n; i++) { t += w.m(); if (i == n - 2) w = { k: 5, m: function () { return this.k * 2; } }; } return t; }"
        "out.push(sk(30000), sk(30000));"
        "out.join(\" \")";
    for (int mode = 0; mode < 3; ++mode) {
        script::Context::Options options;
        if (mode == 0)
            options.jit.mode = script::JitOptions::Mode::Off;
        else if (mode == 1)
            options.jit.threshold = 1;
        else
            options.jit.mode = script::JitOptions::Mode::Eager;
        js::Context ctx(options);
        CHECK_EQ(ctx.eval(program, "<test>").toString(),
                 std::string("400050000 850000 850000 10600000 10450000 1450000 4849958 30009 30009"));
    }
}

TEST(logicalOperatorsInTestsAreControlFlow)
{
    // `&&`, `||` and `!` in the test of a loop (made at its top and its bottom), an `if`, and a conditional: what is
    // evaluated, in which order, and where it goes.
    const char* program =
        "var out = [];"
        "function f(a, b, c) { var n = 0; while (a < 10 && (b > 0 || c)) { a++; if (!(a & 1) || !c) n += a; else if (!(b && c)) n -= 1; b--; } return n; }"
        "out.push(f(0, 5, 0), f(0, 5, 1), f(0, 0, 1), f(3, 2, ''), f(0, 20, 'x'));"
        "var k = 0; do { k++; } while (!(k >= 5) && k !== 3 || k < 2); out.push(k);"
        "for (var i = 0, s = 0; i < 10 && !(s > 20) || i == 0; i++) s += i; out.push(i, s);"
        "var side = []; function t(v) { side.push(v); return v; }"
        "if (t(0) || t(null) || !t(1) && t(2)) out.push('y'); else out.push('n');"
        "while (t(1) && !t(0) && t(false)) {}"
        "out.push(side.join(','), (0 || !1) ? 1 : 2, !(1 && 0) ? 3 : 4);"
        "out.join(\" \")";
    for (int mode = 0; mode < 3; ++mode) {
        script::Context::Options options;
        if (mode == 0)
            options.jit.mode = script::JitOptions::Mode::Off;
        else if (mode == 1)
            options.jit.threshold = 1;
        else
            options.jit.mode = script::JitOptions::Mode::Eager;
        js::Context ctx(options);
        CHECK_EQ(ctx.eval(program, "<test>").toString(), std::string("15 30 29 9 30 3 7 21 n 0,,1,1,0,false 2 3"));
    }
}

TEST(arrayLiteralsMadeInCompiledCode)
{
    // Literals of a few elements made in place (numbers, strings, objects, holes, spread), then grown, sorted and kept.
    const char* program =
        "var out=[]; "
        "function mk(i){ return [i, i+0.5, 'x'+i, [i], {v:i}, null, undefined, i]; } "
        "function sm(i){ return [i]; } "
        "function two(a,b){ return [a,b]; } "
        "function holes(i){ return [i,,i]; } "
        "function sp(a){ return [0, ...a, 9]; } "
        "var t=0, keep=[]; "
        "for (var i=0;i<30000;i++){ var a=mk(i), b=sm(i), c=two(i,'s'), d=holes(i), e=sp(b); t+=a.length+b[0]+c.length+d.length+(1 in d?1:0)+e.length; if(i%5000==0) keep.push(a,b,c,d,e); } "
        "out.push(t, JSON.stringify(keep.slice(0,5))); "
        "var n=two(3,1); n.sort(); var m=two('b',2); m.sort(); var q=two(10,9); q.sort(function(x,y){return x-y;}); "
        "out.push(n.join(), m.join(), q.join()); "
        "var w=two(1,{}); w.sort(function(x,y){return 0;}); out.push(typeof w[1]); "
        "var big=[]; for (var i=0;i<20000;i++){ var z=sm(i); z.push(i,i,i,i,i); big.push(z); } out.push(big[19999].length, big[123][5]); "
        "out.join(' | ') ";
    for (int mode = 0; mode < 3; ++mode) {
        script::Context::Options options;
        if (mode == 0)
            options.jit.mode = script::JitOptions::Mode::Off;
        else if (mode == 1)
            options.jit.threshold = 1;
        else
            options.jit.mode = script::JitOptions::Mode::Eager;
        js::Context ctx(options);
        CHECK_EQ(ctx.eval(program, "<test>").toString(), std::string("450465000 | [[0,0.5,\"x0\",[0],{\"v\":0},null,null,0],[0],[0,\"s\"],[0,null,0],[0,0,9]] | 1,3 | 2,b | 9,10 | object | 6 | 123"));
    }
}

TEST(stringsAppendedInPlaceAndCharactersMadeInCompiledCode)
{
    // `s += t` copies into a string only its register holds; a string kept elsewhere, interned or appended to itself is
    // left as it was. `String.fromCharCode` of a character below 256 is the string the VM keeps for it.
    const char* program =
        "var out=[]; "
        "function f(n){var s=\"\", keep=[]; for(var i=0;i<n;i++){ s+=String.fromCharCode(97+i%26); if(i%50==0){ keep.push(s); } if(i%77==0) s+=s.length; if(i%91==0){ var t=s; s+=\"!\"; keep.push(t); } } return [s.length, keep.length, keep[3], keep[keep.length-1].length, s.slice(-10)];} "
        "out.push(f(3000).join(',')); "
        "function g(){ var a=\"ab\", b=a; for (var i=0;i<100;i++){ a+=\"c\"; } return [a.length, b, a===b]; } "
        "out.push(g().join(',')); "
        "function h(){ var s=\"x\"; for (var i=0;i<12;i++) s+=s; return s.length; } out.push(h()); "
        "function k(){ var s=\"pre\"; var o={}; for (var i=0;i<300;i++){ s+=(i&1?\"y\":String.fromCharCode(i)); o[s.slice(0,4)]=1; } return [s.length, Object.keys(o).length, s.charCodeAt(100)]; } "
        "out.push(k().join(',')); "
        "function m(){ var s=\"\"; for (var i=0;i<1000;i++){ s+=String.fromCharCode(i*7); } var c=0; for (var i=0;i<s.length;i++) c=(c*31+s.charCodeAt(i))|0; return c; } "
        "out.push(m()); "
        "var lit=\"hello\"; function q(){ var s=lit; for (var i=0;i<40;i++) s+=\"!\"; return [s.length, lit]; } out.push(q().join(',')); "
        "out.join(' | ') ";
    for (int mode = 0; mode < 3; ++mode) {
        script::Context::Options options;
        if (mode == 0)
            options.jit.mode = script::JitOptions::Mode::Off;
        else if (mode == 1)
            options.jit.threshold = 1;
        else
            options.jit.mode = script::JitOptions::Mode::Eager;
        js::Context ctx(options);
        CHECK_EQ(ctx.eval(program, "<test>").toString(), std::string("3173,93,a1!bcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz80abcdefghijklmn,3124,abcdefghij | 102,ab,false | 4096 | 303,1,121 | -356477524 | 45,hello"));
    }
}

TEST(jsonParseMakesObjectsAlongKnownShapesAndReadsShortDecimalsExactly)
{
    // Repeated names, names that are array indexes, `__proto__` as an own name, objects of many and few properties; and
    // decimals read without the library that must be what it would read.
    const char* program =
        "var out=[]; "
        "var docs=['{\"a\":1,\"b\":2,\"a\":3}','{\"b\":1,\"a\":2,\"1\":3,\"0\":4,\"01\":5,\"x\":{\"a\":1,\"b\":2,\"a\":5}}','{\"__proto__\":{\"p\":1},\"q\":2}','[{\"a\":1,\"b\":2,\"c\":3,\"d\":4,\"e\":5,\"f\":6,\"g\":7,\"h\":8,\"i\":9,\"j\":10},{\"a\":1},{}]','{\"a\":-0.0,\"b\":1.25e2,\"c\":\"\\\\u0041\\\\n\",\"d\":[1,2,{\"z\":null}],\"e\":true,\"f\":false}']; "
        "for (var r=0;r<3;r++) for (var k=0;k<docs.length;k++){ var v=JSON.parse(docs[k]); out.push(JSON.stringify(v), Object.keys(v).join(','), Object.getPrototypeOf(v)===Object.prototype); if (v.__proto__ && v.q) out.push(v.p===undefined, Object.getOwnPropertyNames(v).join(',')); } "
        "var big='['; for (var i=0;i<300;i++) big+=(i?',':'')+'{\"id\":'+i+',\"k'+(i%7)+'\":'+(i*0.25)+',\"s\":\"'+i+'\"}'; big+=']'; "
        "var a=JSON.parse(big), t=0; for (var i=0;i<a.length;i++) t+=a[i].id+(a[i]['k'+(i%7)]||0); out.push(t, JSON.stringify(a[299])); "
        "out.join(' | ') ";
    const char* numbers =
        "var xs=[\"0.1\",\"0.5\",\"-0.0\",\"12.5\",\"3.14159265358979\",\"123456789012345\",\"1234567890123456\",\"0.000000000000001\",\"99999.99999\",\"1e5\",\"-1.5e-3\",\"0.30000000000000004\",\"9007199254740993\",\"1.7976931348623157e308\",\"5e-324\",\"-12345.678901234\",\"100.0\",\"0\",\"-0\",\"4294967296.5\"]; "
        "var out=[]; for (var k=0;k<xs.length;k++){ var v=JSON.parse(xs[k]); out.push(String(v), Object.is(v,-0)); } "
        "var r=0; for (var i=0;i<20000;i++){ r+=JSON.parse((i*0.37).toFixed(5)) } out.push(r); "
        "out.join(' ') ";
    for (int mode = 0; mode < 2; ++mode) {
        script::Context::Options options;
        if (mode == 0)
            options.jit.mode = script::JitOptions::Mode::Off;
        else
            options.jit.mode = script::JitOptions::Mode::Eager;
        js::Context ctx(options);
        CHECK_EQ(ctx.eval(program, "<test>").toString(), std::string("{\"a\":3,\"b\":2} | a,b | true | {\"0\":4,\"1\":3,\"b\":1,\"a\":2,\"01\":5,\"x\":{\"a\":5,\"b\":2}} | 0,1,b,a,01,x | true | {\"__proto__\":{\"p\":1},\"q\":2} | __proto__,q | true | true | __proto__,q | [{\"a\":1,\"b\":2,\"c\":3,\"d\":4,\"e\":5,\"f\":6,\"g\":7,\"h\":8,\"i\":9,\"j\":10},{\"a\":1},{}] | 0,1,2 | false | {\"a\":0,\"b\":125,\"c\":\"A\\n\",\"d\":[1,2,{\"z\":null}],\"e\":true,\"f\":false} | a,b,c,d,e,f | true | {\"a\":3,\"b\":2} | a,b | true | {\"0\":4,\"1\":3,\"b\":1,\"a\":2,\"01\":5,\"x\":{\"a\":5,\"b\":2}} | 0,1,b,a,01,x | true | {\"__proto__\":{\"p\":1},\"q\":2} | __proto__,q | true | true | __proto__,q | [{\"a\":1,\"b\":2,\"c\":3,\"d\":4,\"e\":5,\"f\":6,\"g\":7,\"h\":8,\"i\":9,\"j\":10},{\"a\":1},{}] | 0,1,2 | false | {\"a\":0,\"b\":125,\"c\":\"A\\n\",\"d\":[1,2,{\"z\":null}],\"e\":true,\"f\":false} | a,b,c,d,e,f | true | {\"a\":3,\"b\":2} | a,b | true | {\"0\":4,\"1\":3,\"b\":1,\"a\":2,\"01\":5,\"x\":{\"a\":5,\"b\":2}} | 0,1,b,a,01,x | true | {\"__proto__\":{\"p\":1},\"q\":2} | __proto__,q | true | true | __proto__,q | [{\"a\":1,\"b\":2,\"c\":3,\"d\":4,\"e\":5,\"f\":6,\"g\":7,\"h\":8,\"i\":9,\"j\":10},{\"a\":1},{}] | 0,1,2 | false | {\"a\":0,\"b\":125,\"c\":\"A\\n\",\"d\":[1,2,{\"z\":null}],\"e\":true,\"f\":false} | a,b,c,d,e,f | true | 56062.5 | {\"id\":299,\"k5\":74.75,\"s\":\"299\"}"));
        CHECK_EQ(ctx.eval(numbers, "<test>").toString(), std::string("0.1 false 0.5 false 0 true 12.5 false 3.14159265358979 false 123456789012345 false 1234567890123456 false 1e-15 false 99999.99999 false 100000 false -0.0015 false 0.30000000000000004 false 9007199254740992 false 1.7976931348623157e+308 false 5e-324 false -12345.678901234 false 100 false 0 false 0 true 4294967296.5 false 73996300"));
    }
}

TEST(jsonStringifyKeepsTheNamesOfShapesWhileNoScriptRuns)
{
    // A toJSON that changes objects of a shape already written, a getter, replacers, indentation, nesting.
    const char* program =
        "var out=[]; "
        "function P(a,b){this.a=a;this.b=b;} "
        "var arr=[]; for (var i=0;i<6;i++) arr.push({a:i,b:'s'+i,c:{a:i,b:[i]}}); "
        "out.push(JSON.stringify(arr)); "
        "out.push(JSON.stringify(arr,null,2).length, JSON.stringify(arr,['a','c']), JSON.stringify(arr,function(k,v){ return k==='b'? undefined : v; })); "
        "var x={a:1,b:2}, y={a:3,b:4}, z={a:5,b:6}; "
        "y.toJSON=function(){ x.c=9; delete z.a; z.q=7; return {a:'y'}; }; "
        "out.push(JSON.stringify([x,y,x,z,{a:1,b:2}])); "
        "var g={a:1,b:2}; Object.defineProperty(g,'b',{get:function(){ return 'got'; },enumerable:true}); "
        "out.push(JSON.stringify([{a:0,b:1},g,{a:2,b:3}])); "
        "var m={a:1,b:2}; var n={a:{toJSON:function(){ m.a=100; m.b=200; return 'T'; }},b:m}; "
        "out.push(JSON.stringify([m,n,m])); "
        "var deep={a:1,b:2}; for (var i=0;i<20;i++) deep={a:deep,b:i}; "
        "out.push(JSON.stringify(deep).length); "
        "var w={a:1,b:2}; var nested={a:w,b:{a:w,b:{a:w,b:w}}}; out.push(JSON.stringify(nested)); "
        "out.push(JSON.stringify({a:new Number(3),b:new String('x'),c:new Boolean(false)}), JSON.stringify({b:1,a:2,1:3})); "
        "out.join(' | ') ";
    js::Context ctx;
    CHECK_EQ(ctx.eval(program, "<test>").toString(), std::string("[{\"a\":0,\"b\":\"s0\",\"c\":{\"a\":0,\"b\":[0]}},{\"a\":1,\"b\":\"s1\",\"c\":{\"a\":1,\"b\":[1]}},{\"a\":2,\"b\":\"s2\",\"c\":{\"a\":2,\"b\":[2]}},{\"a\":3,\"b\":\"s3\",\"c\":{\"a\":3,\"b\":[3]}},{\"a\":4,\"b\":\"s4\",\"c\":{\"a\":4,\"b\":[4]}},{\"a\":5,\"b\":\"s5\",\"c\":{\"a\":5,\"b\":[5]}}] | 590 | [{\"a\":0,\"c\":{\"a\":0}},{\"a\":1,\"c\":{\"a\":1}},{\"a\":2,\"c\":{\"a\":2}},{\"a\":3,\"c\":{\"a\":3}},{\"a\":4,\"c\":{\"a\":4}},{\"a\":5,\"c\":{\"a\":5}}] | [{\"a\":0,\"c\":{\"a\":0}},{\"a\":1,\"c\":{\"a\":1}},{\"a\":2,\"c\":{\"a\":2}},{\"a\":3,\"c\":{\"a\":3}},{\"a\":4,\"c\":{\"a\":4}},{\"a\":5,\"c\":{\"a\":5}}] | [{\"a\":1,\"b\":2},{\"a\":\"y\"},{\"a\":1,\"b\":2,\"c\":9},{\"b\":6,\"q\":7},{\"a\":1,\"b\":2}] | [{\"a\":0,\"b\":1},{\"a\":1,\"b\":\"got\"},{\"a\":2,\"b\":3}] | [{\"a\":1,\"b\":2},{\"a\":\"T\",\"b\":{\"a\":100,\"b\":200}},{\"a\":100,\"b\":200}] | 263 | {\"a\":{\"a\":1,\"b\":2},\"b\":{\"a\":{\"a\":1,\"b\":2},\"b\":{\"a\":{\"a\":1,\"b\":2},\"b\":{\"a\":1,\"b\":2}}}} | {\"a\":3,\"b\":\"x\",\"c\":false} | {\"1\":3,\"b\":1,\"a\":2}"));
}

TEST(callsOfALoopsUnchangedFunctionsAreCheckedBeforeIt)
{
    // Closures called in loops whose registers the loop does not write: checked once before the loop, also on the way in
    // from the baseline code, and a closure of other code there leaves; a register the loop writes is checked each time.
    // An upvalue seen to hold a number is stored over without letting go of it, and one that becomes a string is let go.
    const char* program =
        "var out=[]; "
        "function mk(k){var c=0;return function(d){c+=d*k;return c;};} "
        "function mk2(){var c=\"\";return function(d){c+=d;return c.length;};} "
        "function run(f,g,n){var s=0;for(var i=0;i<n;i++){s=f(i&1)+g(1);}return s;} "
        "for (var r=0;r<30;r++) run(mk(1),mk(2),1000); "
        "out.push(run(mk(1),mk(3),5000), run(mk2(),mk(1),3000), run(mk(2),mk2(),3000)); "
        "function swap(n){var f=mk(1),s=0;for(var i=0;i<n;i++){s+=f(1); if(i==n>>1) f=mk2();}return s;} "
        "for (var r=0;r<20;r++) swap(200); out.push(swap(4001)); "
        "function nest(n){var f=mk(1),g=mk(5),s=0;for(var i=0;i<n;i++){for(var j=0;j<3;j++){s+=f(j)+g(1);} if(i%100==99) g=mk(7);}return s;} "
        "out.push(nest(2000), nest(10)); "
        "function once(){var f=mk(3),s=0;for(var i=0;i<200000;i++){s=(s+f(1))|0;}return s;} out.push(once()); "
        "function up(){var c=0;function add(d){c+=d;return c;} var s=0; for(var i=0;i<5000;i++){ s=add(1); if(i==2500){ c=\"x\"; } } return [typeof s, s.length, c.length]; } "
        "out.push(up().join('/')); "
        "out.join(' ') ";
    for (int mode = 0; mode < 3; ++mode) {
        script::Context::Options options;
        if (mode == 0)
            options.jit.mode = script::JitOptions::Mode::Off;
        else if (mode == 1)
            options.jit.threshold = 1;
        else
            options.jit.mode = script::JitOptions::Mode::Eager;
        js::Context ctx(options);
        CHECK_EQ(ctx.eval(program, "<test>").toString(), std::string("17500 6000 6000 4004001 24229700 2770 -129242144 string/2500/2500"));
    }
}

TEST(shapesAndChainsALoopTakesAsKnownHoldOrTheLoopIsCompiledWithout)
{
    // Loops whose objects' shapes and prototype chains are checked before them: a call, a getter, a delete, an accessor, a
    // new prototype or a property added inside the loop, and objects of other shapes on the way in.
    const char* program =
        "var out=[]; "
        "function P(){this.a=1;this.b=2;} "
        "function grow(o,i){ if(i==500) o.z=9; } "
        "function k1(o,n){var s=0;for(var i=0;i<n;i++){ s+=o.a+o.b; grow(o,i); o.a=o.b+1; }return s;} "
        "for (var r=0;r<20;r++) k1(new P(),100); out.push(k1(new P(),2000)); "
        "var gp={get g(){ this.q=1; return 3; }}; "
        "function k2(o,n){var s=0;for(var i=0;i<n;i++){ s+=o.a; if(i==700) s+=o.g; o.a=i&7; }return s;} "
        "for (var r=0;r<20;r++) k2(Object.assign(Object.create(gp),{a:1}),100); out.push(k2(Object.assign(Object.create(gp),{a:1}),2000)); "
        "function A(v){this.v=v;} A.prototype.f=function(){return this.v;}; "
        "function k3(o,n){var s=0;for(var i=0;i<n;i++){ s+=o.f(); if(i==600) swapf(); }return s;} "
        "function swapf(){ A.prototype.f=function(){return 100;}; } "
        "for (var r=0;r<20;r++) k3(new A(1),100); A.prototype.f=function(){return this.v;}; out.push(k3(new A(2),2000)); "
        "function k4(o,n){var s=0;for(var i=0;i<n;i++){ s+=o.a+(o.b|0); if(i==800) delete o.b; }return s;} "
        "for (var r=0;r<20;r++) k4(new P(),100); out.push(k4(new P(),2000)); "
        "function k5(o,n){var s=0;for(var i=0;i<n;i++){ s+=o.a; o.a=i; if(i==900) Object.defineProperty(o,'a',{get:function(){return 5;},set:function(v){},configurable:true}); }return s;} "
        "for (var r=0;r<20;r++) k5(new P(),100); out.push(k5(new P(),2000)); "
        "function Q(){this.b=2;this.a=1;} "
        "function k6(o,n){var s=0;for(var i=0;i<n;i++){ s+=o.a*2+o.b; o.b=i&3; }return s;} "
        "for (var r=0;r<30;r++) k6(new P(),200); out.push(k6(new Q(),3000), k6(new P(),3000), k6({a:5,b:6},3000)); "
        "function k7(){var o=new P(),s=0;for(var i=0;i<300000;i++){ s=(s+o.a+o.b)|0; o.b=(o.b+1)&15; if(i==150000) o.c=1; }return s;} "
        "out.push(k7()); "
        "function B(){} B.prototype.m=function(){return 1;}; "
        "function k8(o,n){var s=0;for(var i=0;i<n;i++){ s+=o.m(); if(i==400) Object.setPrototypeOf(o,{m:function(){return 10;}}); }return s;} "
        "for (var r=0;r<20;r++) k8(new B(),100); out.push(k8(new B(),2000)); "
        "out.join(' ') ";
    for (int mode = 0; mode < 3; ++mode) {
        script::Context::Options options;
        if (mode == 0)
            options.jit.mode = script::JitOptions::Mode::Off;
        else if (mode == 1)
            options.jit.threshold = 1;
        else
            options.jit.mode = script::JitOptions::Mode::Eager;
        js::Context ctx(options);
        CHECK_EQ(ctx.eval(program, "<test>").toString(), std::string("9998 6997 141102 3602 410046 10499 10499 34503 2550000 16391"));
    }
}

TEST(aSlotSeenHoldingANumberIsStoredOverWithoutALookAtTheOldValue)
{
    // Slots read and stored as numbers, then given an object through another register, a call, the same object under two
    // names, or holding a string or an object when the loop begins.
    const char* program =
        "var out=[]; "
        "function P(){this.a=1;this.b=2;} "
        "function setObj(o){ o.a={v:1}; } "
        "function k1(o,p,n){var s=0;for(var i=0;i<n;i++){ s+=o.b; o.a=i; p.a=i*2; if(i==300) setObj(p); s+= (typeof o.a=='object')?100:o.a; o.a=7; }return s;} "
        "for (var r=0;r<20;r++){ var x=new P(); k1(x,new P(),50); k1(x,x,50); } "
        "var x=new P(); out.push(k1(x,x,1000), JSON.stringify(x)); var y=new P(); out.push(k1(y,new P(),1000), JSON.stringify(y)); "
        "function k2(o,n){var s=0;for(var i=0;i<n;i++){ s+=o.a+o.b; o.b=o.a+1; o.a=o.b-1; }return s;} "
        "for (var r=0;r<20;r++) k2(new P(),100); "
        "var z=new P(); z.b={q:1}; try { out.push(k2(z,20)); } catch(e) { out.push('E'); } out.push(JSON.stringify(z)); "
        "var w=new P(); w.a='s'; out.push(k2(w,10), JSON.stringify(w)); "
        "function k3(o,n){var t=0;for(var i=0;i<n;i++){ o.a=i; t+=o.a; o.a=o; t+= (o.a===o)?1:0; o.a=3; }return t;} "
        "for (var r=0;r<20;r++) k3(new P(),100); var v=new P(); out.push(k3(v,1000), v.a); "
        "function N(){this.x=1;this.y={k:2};} "
        "function k4(o,n){var s=0;for(var i=0;i<n;i++){ s+=o.x; o.x=i; s+=o.y.k; }return s;} "
        "for (var r=0;r<20;r++) k4(new N(),100); out.push(k4(new N(),1000)); "
        "out.join(' ') ";
    for (int mode = 0; mode < 3; ++mode) {
        script::Context::Options options;
        if (mode == 0)
            options.jit.mode = script::JitOptions::Mode::Off;
        else if (mode == 1)
            options.jit.threshold = 1;
        else
            options.jit.mode = script::JitOptions::Mode::Eager;
        js::Context ctx(options);
        CHECK_EQ(ctx.eval(program, "<test>").toString(), std::string("1000500 {\"a\":7,\"b\":2} 501500 {\"a\":7,\"b\":2} 01[object Object]3333333333333333333 {\"a\":1,\"b\":2} 0s2NaNs1NaNNaNNaNNaNNaNNaNNaNNaN {\"a\":null,\"b\":null} 500500 3 500502"));
    }
}

TEST(objectsReadFromPropertiesAndComparedWithLiteralsInCompiledCode)
{
    // Objects read from data properties into registers and into a direct call's arguments, then a getter or another shape
    // there, and the called global replaced; strict comparisons with null, undefined, true and false loaded just before.
    const char* program =
        "var out=[]; "
        "function T(l,r){this.l=l;this.r=r;} "
        "function chk(t){return t.l===null?1:1+chk(t.l)+chk(t.r);} "
        "function mk(d){return d===0?new T(null,null):new T(mk(d-1),mk(d-1));} "
        "for (var r=0;r<30;r++) chk(mk(6)); "
        "out.push(chk(mk(10))); "
        "var odd=new T(null,null); Object.defineProperty(odd,'l',{get:function(){ out.push('g'); return null; }}); out.push(chk(new T(odd,new T(null,null)))); "
        "var other={r:null,l:null}; out.push(chk(new T(other,odd))); "
        "var saved=chk; chk=function(t){ return 1000; }; out.push(saved(new T(new T(null,null),null))); "
        "chk=saved; "
        "function eqs(a){ var n=0; if (a===null) n+=1; if (a!==undefined) n+=2; if (a===true) n+=4; if (a!==false) n+=8; return n; } "
        "for (var r=0;r<500;r++) eqs(r&1?null:true); "
        "out.push([null,undefined,true,false,0,'',{},NaN].map(eqs).join(',')); "
        "function first(o){ return o.a; } var objs=[{a:{v:1}},{a:'s'},{b:1,a:[2]},{a:null}]; "
        "for (var r=0;r<400;r++) first(objs[r&1]); "
        "out.push(objs.map(function(o){ var v=first(o); return typeof v; }).join(',')); "
        "out.join(' ') ";
    for (int mode = 0; mode < 3; ++mode) {
        script::Context::Options options;
        if (mode == 0)
            options.jit.mode = script::JitOptions::Mode::Off;
        else if (mode == 1)
            options.jit.threshold = 1;
        else
            options.jit.mode = script::JitOptions::Mode::Eager;
        js::Context ctx(options);
        CHECK_EQ(ctx.eval(program, "<test>").toString(), std::string("2047 g 3 g 3 2001 11,8,14,2,10,10,10,10 object,string,object,object"));
    }
}

TEST(globalsAndMathFunctionsALoopReadAreReadBeforeItWhileNothingChangesThem)
{
    // A global array and Math.sqrt read once before loops that call nothing; a loop that replaces either through a call,
    // through globalThis, or through a store to Math under another name.
    const char* program =
        "var out=[]; "
        "var os=[{x:1},{y:0,x:2},{z:0,y:0,x:3},{w:0,z:0,y:0,x:4}]; "
        "function k1(n){var s=0;for(var i=0;i<n;i++){s+=os[i&3].x;}return s;} "
        "for (var r=0;r<20;r++) k1(1000); out.push(k1(20000)); "
        "function swapOs(){ os=[{x:10},{x:20},{x:30},{x:40}]; } "
        "function k2(n){var s=0;for(var i=0;i<n;i++){s+=os[i&3].x; if(i==5000) swapOs();}return s;} "
        "for (var r=0;r<20;r++) { k2(100); os=[{x:1},{y:0,x:2},{z:0,y:0,x:3},{w:0,z:0,y:0,x:4}]; } out.push(k2(10000)); "
        "function k3(n){var s=0;for(var i=0;i<n;i++){s+=os[i&3].x; if(i==3000) globalThis.os=[{x:7},{x:7},{x:7},{x:7}];}return s;} "
        "for (var r=0;r<20;r++) { k3(100); } out.push(k3(10000)); "
        "var M=Math; var realSqrt=Math.sqrt; "
        "function k4(n){var s=0;for(var i=0;i<n;i++){ s+=Math.sqrt(i*1.0); if(i==4000) M.sqrt=function(x){return 1;}; }return Math.round(s);} "
        "for (var r=0;r<20;r++) { k4(100); Math.sqrt=realSqrt; } out.push(k4(8000)); Math.sqrt=realSqrt; "
        "var O={sqrt:Math.sqrt}; "
        "function k5(n,o){var s=0;for(var i=0;i<n;i++){ s+=Math.sqrt(4.0); o.v=i; }return s;} "
        "for (var r=0;r<20;r++) k5(100,{v:0}); out.push(k5(1000,{v:0})); "
        "function k6(n){var s=0;for(var i=0;i<n;i++){ s+=Math.sqrt(9.0); if(i==500){ var m=Math; m.sqrt=4; } } return s;} "
        "for (var r=0;r<20;r++) { try{ k6(100);}catch(e){} Math.sqrt=realSqrt; } "
        "try { out.push(k6(1000)); } catch(e) { out.push(e instanceof TypeError); } Math.sqrt=realSqrt; "
        "out.join(' ') ";
    for (int mode = 0; mode < 3; ++mode) {
        script::Context::Options options;
        if (mode == 0)
            options.jit.mode = script::JitOptions::Mode::Off;
        else if (mode == 1)
            options.jit.threshold = 1;
        else
            options.jit.mode = script::JitOptions::Mode::Eager;
        js::Context ctx(options);
        CHECK_EQ(ctx.eval(program, "<test>").toString(), std::string("50000 137491 124003 172685 2000 true"));
    }
}

TEST(numberReadsALoopNeverStoresOverAreReadOnceBeforeIt)
{
    // A property read in a loop that stores only to others, the same object under another name stored to, other kinds of
    // number or a string on the way in, and a store through a call.
    const char* program =
        "var out=[]; "
        "function B(x,v){this.x=x;this.v=v;} "
        "function k1(a,bs,n){var s=0;for(var i=0;i<n;i++){var b=bs[i&1]; s+=a.x*b.x; b.v+=a.x;}return s;} "
        "for (var r=0;r<20;r++){ var a=new B(1.5,0); k1(a,[new B(2.5,0),new B(3.5,0)],100); } "
        "var a=new B(1.5,0), bs=[new B(2.5,0),a]; out.push(k1(a,bs,1000), a.v, bs[0].v); "
        "function k2(a,bs,n){var s=0;for(var i=0;i<n;i++){var b=bs[i&1]; s+=a.x; b.x=i*0.5;}return s;} "
        "for (var r=0;r<20;r++){ k2(new B(1.5,0),[new B(2.5,0),new B(3.5,0)],100); } "
        "var c=new B(1.5,0); out.push(k2(c,[new B(0,0),c],1000), c.x); "
        "function k3(a,n){var s=0;for(var i=0;i<n;i++){ s+=a.x; }return s;} "
        "for (var r=0;r<20;r++) k3(new B(2,0),100); out.push(k3(new B(2,0),1000), k3(new B(2.5,0),1000), k3(new B('s',0),3)); "
        "function setx(o){ o.x=100; } "
        "function k4(a,n){var s=0;for(var i=0;i<n;i++){ s+=a.x; if(i==500) setx(a); }return s;} "
        "for (var r=0;r<20;r++) k4(new B(1,0),100); out.push(k4(new B(1,0),1000)); "
        "out.join(' ') ";
    for (int mode = 0; mode < 3; ++mode) {
        script::Context::Options options;
        if (mode == 0)
            options.jit.mode = script::JitOptions::Mode::Off;
        else if (mode == 1)
            options.jit.threshold = 1;
        else
            options.jit.mode = script::JitOptions::Mode::Eager;
        js::Context ctx(options);
        CHECK_EQ(ctx.eval(program, "<test>").toString(), std::string("3000 750 750 249004 499.5 2000 2500 0sss 50401"));
    }
}

TEST(arrayCallbackMethodsReadAndWriteInPlaceAsTheSpecSays)
{
    // map, filter and their kin over holes, a source that grows or shrinks, a subclass, an index setter added to
    // Array.prototype meanwhile, array-likes, typed arrays, and a species that gives a frozen array (same as JSC).
    const char* program =
        "var out=[]; "
        "var a=[1,,3,4,,6]; "
        "out.push(JSON.stringify(a.map(function(x){return x*2;})), JSON.stringify(a.filter(function(x){return x>2;}))); "
        "var b=[1,2,3,4,5]; out.push(JSON.stringify(b.map(function(x,i,arr){ if(i==1) arr.push(9); if(i==2) arr.pop(); return x+i; })), b.length); "
        "var c=[1,2,3,4,5,6]; out.push(JSON.stringify(c.filter(function(x,i,arr){ if(i==2) arr.length=4; return true; }))); "
        "class MyArr extends Array {} var m=MyArr.from([1,2,3]); var mm=m.map(function(x){return x+1;}); out.push(mm instanceof MyArr, JSON.stringify(mm)); "
        "var log=[]; var d=[1,2,3,4]; var r=d.map(function(x,i){ if(i==1) Object.defineProperty(Array.prototype, 3, {set:function(v){log.push('set'+v);}, get:function(){return 'g';}, configurable:true}); return x*10; }); "
        "out.push(JSON.stringify(r), log.join()); delete Array.prototype[3]; "
        "var e=[1,,3]; var f=e.map(function(x){return x;}); out.push(1 in f, f.length); "
        "out.push(JSON.stringify(Array.prototype.map.call({length:3,0:'a',2:'c'},function(x){return x+'!';}))); "
        "out.push(JSON.stringify(Array.prototype.filter.call(new Int8Array([1,-2,3]),function(x){return x>0;}))); "
        "var g=[3,1,2]; out.push(g.every(function(x){return x>0;}), g.some(function(x){return x>2;}), g.find(function(x){return x<3;}), g.findIndex(function(x){return x==2;}), g.findLast(function(x){return x>1;}), g.findLastIndex(function(x){return x>5;})); "
        "var cnt=0; [1,2,3].forEach(function(){cnt++;}); out.push(cnt); "
        "var big=[]; for (var i=0;i<1000;i++) big.push(i); var t=0; for (var r2=0;r2<50;r2++) t+=big.map(function(x){return x*2;}).filter(function(x){return x%3===0;}).reduce(function(p,x){return p+x;},0); out.push(t); "
        "var objs=[{v:1},{v:2}]; var mapped=objs.map(function(o){return o;}); out.push(mapped[0]===objs[0], JSON.stringify(objs.filter(function(o){return o.v>1;}))); "
        "Object.defineProperty(Array, Symbol.species, {get:function(){ return function(n){ var x=[]; Object.freeze(x); return x; }; }, configurable:true}); "
        "try { [1,2].map(function(x){return x;}); out.push('nofail'); } catch(err) { out.push(err instanceof TypeError); } "
        "delete Array[Symbol.species]; "
        "out.join(' | ') ";
    for (int mode = 0; mode < 2; ++mode) {
        script::Context::Options options;
        if (mode == 0)
            options.jit.mode = script::JitOptions::Mode::Off;
        else
            options.jit.mode = script::JitOptions::Mode::Eager;
        js::Context ctx(options);
        CHECK_EQ(ctx.eval(program, "<test>").toString(), std::string("[2,null,6,8,null,12] | [3,4,6] | [1,3,5,7,9] | 5 | [1,2,3,4] | true | [2,3,4] | [10,20,30,40] |  | false | 3 | [\"a!\",null,\"c!\"] | [1,3] | true | true | 1 | 2 | 2 | -1 | 3 | 16683300 | true | [{\"v\":2}] | true"));
    }
}

TEST(numbersOfAFewDecimalsPrintAsTheShortestFormSays)
{
    // Whole numbers and numbers of up to four decimals take a short way to their digits; the rest the general one. The
    // expected text is JavaScriptCore's for the same program.
    const char* program =
        "var xs=[0.1,0.5,12.5,0.05,1.25,-3.75,100,1e15,123456789.0625,0.0001,0.00015,1.5e-5,0.3,0.30000000000000004,2.675,1/3,-0.0025,999999999999.9999,1099511.6277,4.35,0.07,7e-7,1e21,123e-20,5e-324,2**31+0.5,-1234.5678,0.1+0.2,1.0000000000000002,65.53500000000001]; "
        "var out=[]; for (var i=0;i<xs.length;i++) out.push(String(xs[i])); "
        "for (var i=0;i<20000;i++){ var v=(i*0.37-500)/((i%7)+1); out.push(String(v)); } "
        "var s=out.join(','); var h=0; for (var i=0;i<s.length;i++) h=(h*31+s.charCodeAt(i))|0; out.slice(0,30).join(' ') + ' ' + h + ' ' + s.length ";
    js::Context ctx;
    CHECK_EQ(ctx.eval(program, "<test>").toString(), std::string("0.1 0.5 12.5 0.05 1.25 -3.75 100 1000000000000000 123456789.0625 0.0001 0.00015 0.000015 0.3 0.30000000000000004 2.675 0.3333333333333333 -0.0025 999999999999.9999 1099511.6277 4.35 0.07 7e-7 1e+21 1.23e-18 5e-324 2147483648.5 -1234.5678 0.30000000000000004 1.0000000000000002 65.53500000000001 1977904470 268958"));
}

TEST(aProductByAConstantThatOnlyFeedsATruncationWraps)
{
    // Hash loops of the form (h * k + x) | 0: the product wraps in 32 bits as the truncation would make it; a product used
    // whole, and one that would be -0, are left exact. The expected text is JavaScriptCore's for the same program.
    const char* program =
        "var out=[]; "
        "function h1(s){var h=0;for(var i=0;i<s.length;i++)h=(h*31+s.charCodeAt(i))|0;return h;} "
        "function h2(a){var h=7;for(var i=0;i<a.length;i++)h=(h*-1000003 ^ a[i])|0;return h;} "
        "function h3(a){var h=0;for(var i=0;i<a.length;i++){h=(h*33+a[i])|0;}return h;} "
        "function h4(a){var h=1;for(var i=0;i<a.length;i++){var t=h*31; h=(t+a[i])|0; if(i==5) out.push(t);}return h;} "
        "function h5(a){var h=0, z=[];for(var i=0;i<a.length;i++){ z.push(1/((a[i]*-5)|0)); h=(h*-7+a[i])|0;}return [h,z.slice(0,3).join()];} "
        "var s=''; for (var i=0;i<5000;i++) s+=String.fromCharCode(32+(i*7)%90); "
        "var arr=[]; for (var i=0;i<5000;i++) arr.push((i*2654435761)%1000003 - 500000); "
        "for (var r=0;r<10;r++){ h1(s); h2(arr); h3(arr); h4(arr); h5([0,1,2,3]); } "
        "out.push(h1(s), h2(arr), h3(arr), h4(arr), h5([0,1,-2,3]).join('/'), h1('hello world'), h3([2147483647,2147483647,-2147483648])); "
        "out.join(' ') ";
    for (int mode = 0; mode < 3; ++mode) {
        script::Context::Options options;
        if (mode == 0)
            options.jit.mode = script::JitOptions::Mode::Off;
        else if (mode == 1)
            options.jit.threshold = 1;
        else
            options.jit.mode = script::JitOptions::Mode::Eager;
        js::Context ctx(options);
        CHECK_EQ(ctx.eval(program, "<test>").toString(), std::string("9575379603 9575379603 9575379603 9575379603 9575379603 9575379603 9575379603 9575379603 9575379603 9575379603 9575379603 -1411265306 1697116766 1944858387 1862405418 66/Infinity,-0.2,0.1 1794106052 2147482526"));
    }
}

TEST(aMethodALoopCallsIsReadOnceBeforeItWhileNothingStoresThere)
{
    // Methods read from a prototype for inlined calls in loops: one that stores a number over the method through another
    // name, one that replaces it through a call.
    const char* program =
        "var out=[]; "
        "function P(v){this.v=v;} P.prototype.f=function(){return this.v;}; "
        "function k(o,pr,n){var s=0;for(var i=0;i<n;i++){ s+=o.f(); if(i==n-3) pr.f=7; }return s;} "
        "for (var r=0;r<20;r++){ k(new P(1),{f:0},100); } "
        "try { out.push(k(new P(2),P.prototype,500)); } catch(e) { out.push(e instanceof TypeError); } "
        "P.prototype.f=function(){return this.v*10;}; "
        "function k2(o,n){var s=0;for(var i=0;i<n;i++){ s+=o.f(); }return s;} "
        "for (var r=0;r<20;r++) k2(new P(1),100); out.push(k2(new P(3),1000)); "
        "function swap(){ P.prototype.f=function(){return -1;}; } "
        "function k3(o,n){var s=0;for(var i=0;i<n;i++){ s+=o.f(); if(i==300) swap(); }return s;} "
        "for (var r=0;r<20;r++){ P.prototype.f=function(){return this.v;}; k3(new P(1),100); } "
        "P.prototype.f=function(){return this.v;}; out.push(k3(new P(4),1000)); "
        "out.join(' ') ";
    for (int mode = 0; mode < 3; ++mode) {
        script::Context::Options options;
        if (mode == 0)
            options.jit.mode = script::JitOptions::Mode::Off;
        else if (mode == 1)
            options.jit.threshold = 1;
        else
            options.jit.mode = script::JitOptions::Mode::Eager;
        js::Context ctx(options);
        CHECK_EQ(ctx.eval(program, "<test>").toString(), std::string("true 30000 505"));
    }
}

TEST(aSlotSeenHoldingAnInt32IsReadWithoutALookUntilADoubleMayGoThere)
{
    // Int32 slots read without a check after an int32 store or read; a double stored to the same object under another name,
    // inside and outside loops, and a double there when the loop begins.
    const char* program =
        "var out=[]; "
        "function P(){this.a=1;this.b=2;} "
        "function k1(o,p,n){var s=0;for(var i=0;i<n;i++){ o.a=o.b+1; s+=o.a; p.a=(i==n-5)?1.5:i; s+=o.a; o.b=o.a|0; }return s;} "
        "for (var r=0;r<20;r++) k1(new P(),new P(),100); "
        "var x=new P(); out.push(k1(x,x,1000), x.a, x.b); var y=new P(); out.push(k1(y,new P(),1000), y.a); "
        "function k2(o,p){ var s=o.a; o.a=s+1; p.a=0.25; return o.a+s; } "
        "for (var r=0;r<200;r++) k2(new P(), new P()); "
        "var z=new P(); out.push(k2(z,z), k2(new P(), new P())); "
        "function k3(o,n){var s=0;for(var i=0;i<n;i++){ s+=o.a; o.a=s&7; }return s;} "
        "for (var r=0;r<20;r++) k3(new P(),100); var w=new P(); w.a=2.5; out.push(k3(w,1000)); "
        "out.join(' ') ";
    for (int mode = 0; mode < 3; ++mode) {
        script::Context::Options options;
        if (mode == 0)
            options.jit.mode = script::JitOptions::Mode::Off;
        else if (mode == 1)
            options.jit.threshold = 1;
        else
            options.jit.mode = script::JitOptions::Mode::Eager;
        js::Context ctx(options);
        CHECK_EQ(ctx.eval(program, "<test>").toString(), std::string("997015.5 999 999 1005000 1002 1.25 3 8.5"));
    }
}

TEST(selfRecursionRunsInPlace)
{
    // A function calling itself by its global name runs those calls in place one level deep (self_inline.cpp): results,
    // guards (the global reassigned), missing arguments, uninitialised variables, strict this, exceptions, overflow, doubles.
    const char* program =
        "var out = [];"
        "function fib(n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }"
        "var s = 0;"
        "for (var i = 0; i < 300; i++) s += fib(12);"
        "out.push(s);"
        "function deep(n) { if (n == 0) throw new Error(\"bottom\"); return deep(n - 1) + 1; }"
        "for (var i = 0; i < 3000; i++) { try { deep(i % 7 + 1); } catch (e) { if (i == 2999) out.push(e.stack.split(\"\\n\").length); } }"
        "function cnt(n) { return n <= 0 ? 0 : cnt(n - 1) + 1; }"
        "for (var i = 0; i < 3000; i++) cnt(5);"
        "var keep = cnt;"
        "cnt = function (n) { return 100; };"
        "out.push(keep(5));"
        "function miss(a, b) { if (a <= 0) return b === undefined ? 1 : 2; return miss(a - 1) * 10 + (b === undefined ? 3 : 4); }"
        "for (var i = 0; i < 3000; i++) miss(3, 1);"
        "out.push(miss(3, 1));"
        "function un(n) { var x; if (n > 0) { x = un(n - 1); } return x === undefined ? 0 : x + 1; }"
        "for (var i = 0; i < 3000; i++) un(4);"
        "out.push(un(6));"
        "function st(n) { \"use strict\"; return n == 0 ? typeof this : st(n - 1); }"
        "for (var i = 0; i < 3000; i++) st(3);"
        "out.push(st(3));"
        "function T(l, r) { this.l = l; this.r = r; }"
        "function mk(d) { return d === 0 ? new T(null, null) : new T(mk(d - 1), mk(d - 1)); }"
        "function chk(t) { return t.l === null ? 1 : 1 + chk(t.l) + chk(t.r); }"
        "var c = 0;"
        "for (var i = 0; i < 400; i++) c += chk(mk(i % 6));"
        "out.push(c);"
        "function thrower(n) { if (n == 3) throw n; return thrower(n + 1); }"
        "var caught = 0;"
        "for (var i = 0; i < 3000; i++) { try { thrower(0); } catch (e) { caught += e; } }"
        "out.push(caught);"
        "function dbl(n) { return n < 1 ? 0.5 : dbl(n - 1) + dbl(n - 1); }"
        "var d = 0;"
        "for (var i = 0; i < 2000; i++) d += dbl(4);"
        "out.push(d);"
        "function big(n) { return n == 0 ? 2147483647 : big(n - 1) + 1; }"
        "for (var i = 0; i < 3000; i++) big(1);"
        "out.push(big(3));"
        "function strs(n) { return n == 0 ? \"\" : strs(n - 1) + n; }"
        "for (var i = 0; i < 3000; i++) strs(3);"
        "out.push(strs(9));"
        "out.join(\",\");";
    for (int mode = 0; mode < 3; ++mode) {
        script::Context::Options options;
        if (mode == 0)
            options.jit.mode = script::JitOptions::Mode::Off;
        else if (mode == 1)
            options.jit.threshold = 1;
        else
            options.jit.mode = script::JitOptions::Mode::Eager;
        js::Context ctx(options);
        CHECK_EQ(ctx.eval(program, "<test>").toString(), std::string("43200,7,101,1334,6,undefined,7946,9000,16000,2147483650,123456789"));
    }
}

TEST(selfRecursionRunInPlaceKeepsItsFramesInTraces)
{
    // A call run in place has no frame of its own: the trace shows it with its own line, then the call's.
    const char* program =
        "function deep2(n) {\n"
        "  if (n == 0)\n"
        "    throw new Error(\"b\");\n"
        "  var r = deep2(n - 1);\n"
        "  return r + 1;\n"
        "}\n"
        "var last;\n"
        "for (var i = 0; i < 3000; i++) { try { deep2(i % 5); } catch (e) { last = e.stack; } }\n"
        "last;\n";
    for (int mode = 0; mode < 3; ++mode) {
        script::Context::Options options;
        if (mode == 0)
            options.jit.mode = script::JitOptions::Mode::Off;
        else if (mode == 1)
            options.jit.threshold = 1;
        else
            options.jit.mode = script::JitOptions::Mode::Eager;
        js::Context ctx(options);
        CHECK_EQ(ctx.eval(program, "<test>").toString(), std::string("Error: b\n    at deep2 (<test>:3)\n    at deep2 (<test>:4)\n    at deep2 (<test>:4)\n    at deep2 (<test>:4)\n    at deep2 (<test>:4)\n    at <program> (<test>:8)"));
    }
}

TEST(aReturnedRegisterGivesItsCountToTheResult)
{
    // The optimized code returns a register the frame clears with that register's count: with many registers holding
    // cells the clear is a call into the runtime, which must not let the result go.
    const char* program =
        "function many(a) {"
        "    var o1 = { x: a }, o2 = { x: 2 }, o3 = { x: 3 }, o4 = { x: 4 }, o5 = { x: 5 }, o6 = { x: 6 };"
        "    var o7 = { x: 7 }, o8 = { x: 8 }, o9 = { x: 9 }, o10 = { x: 10 }, o11 = { x: 11 };"
        "    var r = { s: o1.x + o2.x + o3.x + o4.x + o5.x + o6.x + o7.x + o8.x + o9.x + o10.x + o11.x, k: o5 };"
        "    return r;"
        "}"
        "var keep = [], t = 0;"
        "for (var i = 0; i < 5000; i++) { var r = many(i); t += r.s + r.k.x; if (i % 1000 == 0) keep.push(r); }"
        "for (var j = 0; j < 100000; j++) ({ y: j });"
        "keep.map(function (r) { return r.s + \":\" + r.k.x; }).join(\",\") + \"|\" + t;";
    for (int mode = 0; mode < 3; ++mode) {
        script::Context::Options options;
        if (mode == 0)
            options.jit.mode = script::JitOptions::Mode::Off;
        else if (mode == 1)
            options.jit.threshold = 1;
        else
            options.jit.mode = script::JitOptions::Mode::Eager;
        js::Context ctx(options);
        CHECK_EQ(ctx.eval(program, "<test>").toString(), std::string("65:5,1065:5,2065:5,3065:5,4065:5|12847500"));
    }
}

TEST(aFunctionRunInPlaceThatReplacesItselfLeavesTheCopy)
{
    // A function run in place that assigns its global name a new function part way down: the calls after see the new one.
    const char* program =
        "function rf(n) { if (n == 30) rf = function (m) { return 1000; }; return n <= 0 ? 0 : rf(n - 1) + 1; }"
        "var orig = rf, s = 0;"
        "for (var i = 0; i < 3000; i++) s += rf(i % 20);"
        "var r1 = orig(35);"
        "var r2 = rf(5);"
        "function sw(n) { return n <= 0 ? 0 : (n == 7 ? (sw = function (m) { return -100; }, 0) : 0) + sw(n - 1) + 1; }"
        "var orig2 = sw, t = 0;"
        "for (var i = 0; i < 3000; i++) t += sw(i % 6);"
        "s + \",\" + r1 + \",\" + r2 + \",\" + t + \",\" + orig2(10) + \",\" + sw(3);";
    for (int mode = 0; mode < 3; ++mode) {
        script::Context::Options options;
        if (mode == 0)
            options.jit.mode = script::JitOptions::Mode::Off;
        else if (mode == 1)
            options.jit.threshold = 1;
        else
            options.jit.mode = script::JitOptions::Mode::Eager;
        js::Context ctx(options);
        CHECK_EQ(ctx.eval(program, "<test>").toString(), std::string("28500,1006,1000,7500,-96,-100"));
    }
}

TEST(aPropertyReadAgainSeesStoresThroughOtherNames)
{
    // A number read of a property again in the block takes the first read's value only while nothing between can have
    // changed it: a store to the property through another name of the object, the register reassigned, a setter.
    const char* program =
        "function P(x, m) { this.x = x; this.m = m; }"
        "function f(a, b, k) { var s = a.m * 2; b.m = k; s += a.m; var c = a; a = b; s += a.m * 3 + c.m; return s; }"
        "var p = new P(1, 1.5), q = new P(2, 2.5), t = 0;"
        "for (var i = 0; i < 20000; i++) { t += f(p, q, i * 0.5); t += f(p, p, 0.25); p.m = 1.5; }"
        "var o = { m: 1.25 };"
        "var w = { set m(v) { o.m = v * 2; }, get m() { return 7; } };"
        "function g(a, b) { var s = a.m; b.m = 3; return s + a.m; }"
        "for (var i = 0; i < 20000; i++) { t += g(o, w); o.m = 1.25; }"
        "t;";
    for (int mode = 0; mode < 3; ++mode) {
        script::Context::Options options;
        if (mode == 0)
            options.jit.mode = script::JitOptions::Mode::Off;
        else if (mode == 1)
            options.jit.threshold = 1;
        else
            options.jit.mode = script::JitOptions::Mode::Eager;
        js::Context ctx(options);
        CHECK_EQ(ctx.eval(program, "<test>").toString(), std::string("300335000"));
    }
}

TEST(aLoopReadingAFieldAndAGlobalKeepsBoth)
{
    // A loop that reads a property it never stores to and a global array keeps each in a stack slot of its own before it.
    const char* program =
        "var G = [1, 2, 3, 4, 5, 6, 7, 8];"
        "function f(o, n) { var s = 0; for (var i = 0; i < n; i++) { s += o.x + G[i & 7]; } return s; }"
        "var o = { x: 0.5, y: 2 }, r = 0;"
        "for (var k = 0; k < 3000; k++) r += f(o, 100);"
        "r;";
    for (int mode = 0; mode < 3; ++mode) {
        script::Context::Options options;
        if (mode == 0)
            options.jit.mode = script::JitOptions::Mode::Off;
        else if (mode == 1)
            options.jit.threshold = 1;
        else
            options.jit.mode = script::JitOptions::Mode::Eager;
        js::Context ctx(options);
        CHECK_EQ(ctx.eval(program, "<test>").toString(), std::string("1476000"));
    }
}

TEST(optimizedNumbersNaNsCountersAndNestedLoopChecks)
{
    // Doubles boxed without the NaN test only when they cannot hold an odd NaN (a typed array's can), a counter stepped in place
    // that overflows, the checks of an inner loop that leave out the outer loop's (entered part way, Math.sqrt replaced), and
    // a property known to hold a number that later does not.
    const char* program =
        "var buf = new ArrayBuffer(8), dv = new DataView(buf), f64 = new Float64Array(buf);"
        "dv.setUint32(0, 0xffffffff); dv.setUint32(4, 0xffffffff);"
        "function P() { this.v = 0.5; this.w = 1.5; }"
        "function nanStore(o, n, k) { for (var i = 0; i < n; i++) { o.v = f64[0] + k; o.w = (o.w - o.w) * Infinity; } return o; }"
        "var o = new P(), out = [];"
        "for (var r = 0; r < 300; r++) nanStore(o, 50, r);"
        "var m = new Map(); m.set(o.v, 1); m.set(o.w, 2);"
        "out.push(typeof o.v, o.v !== o.v, Object.is(o.w, NaN), m.size, m.get(NaN), [o.v].includes(NaN), String(o.w + 1));"
        "function count(from, n) { var s = 0; for (var i = from; i < from + n; i++) s += i & 7; return s; }"
        "var c = 0; for (var r = 0; r < 300; r++) c += count(2147483600, 40) + count(2147483640, 10);"
        "out.push(c);"
        "function nest(bs, n) { var t = 0; for (var i = 0; i < n; i++) { var a = bs[i % bs.length]; for (var j = 0; j < 30; j++) t += Math.sqrt(a.x + j) + a.y; } return t; }"
        "var bs = [{ x: 1, y: 2.5 }, { x: 4, y: 0.25 }], t = 0;"
        "for (var r = 0; r < 200; r++) t += nest(bs, 20);"
        "var keep = Math.sqrt; Math.sqrt = function (x) { return x; }; t += nest(bs, 20); Math.sqrt = keep; t += nest(bs, 20);"
        "bs[1].y = 'z'; out.push(String(nest(bs, 3)).slice(0, 12)); bs[1].y = 0.25;"
        "out.push(Math.round(t * 1000));"
        "function field(q, n) { var s = 0; for (var i = 0; i < n; i++) { s += q.a * 2; q.a = q.a + 1; } return s; }"
        "var q = { a: 1.5 }, fs = 0; for (var r = 0; r < 400; r++) { q.a = 1.5; fs += field(q, 20); }"
        "q.a = 'x'; out.push(field(q, 2)); out.push(fs);"
        "out.join(',');";
    for (int mode = 0; mode < 3; ++mode) {
        script::Context::Options options;
        if (mode == 0)
            options.jit.mode = script::JitOptions::Mode::Off;
        else if (mode == 1)
            options.jit.threshold = 1;
        else
            options.jit.mode = script::JitOptions::Mode::Eager;
        js::Context ctx(options);
        CHECK_EQ(ctx.eval(program, "<test>").toString(), std::string("number,true,true,1,2,true,NaN,50700,187.08284521,653197101,NaN,176000"));
    }
}

TEST(loopElementsHeldWithoutACount)
{
    // A loop that only reads elements of arrays it does not change, and only uses them as objects of properties, holds them
    // without a count: counted again on the way out of the loop (the last one outlives the array), on a way out to the
    // interpreter (a getter that throws, an element of another shape), in nested loops and in a loop entered part way.
    const char* program =
        "function B(x) { this.x = x; this.y = x * 0.5; }"
        "var out = [];"
        "function scan(arr, n) { var s = 0, b; for (var j = 0; j < n; j++) { b = arr[j]; s += b.x * b.y; b.y = s * 0.001; } return b; }"
        "var arr = []; for (var k = 0; k < 64; k++) arr.push(new B(k + 0.5));"
        "var last; for (var r = 0; r < 400; r++) last = scan(arr, 64);"
        "arr.length = 0; arr = null;"
        "out.push(Math.round(last.x * 1000), Math.round(last.y * 1000));"
        "var arr2 = []; for (var k = 0; k < 64; k++) arr2.push(new B(k + 0.25));"
        "arr2[40] = { x: 2, get y() { if (this.t++ > 3) throw new Error('stop'); return 1.5; }, t: 0 };"
        "var caught = 0;"
        "for (var r = 0; r < 20; r++) { try { scan(arr2, 64); } catch (e) { caught++; } }"
        "out.push(caught);"
        "arr2[40] = new B(3);"
        "arr2[50] = { y: 2.5, x: 'str' };"
        "out.push(String(scan(arr2, 64).y).slice(0, 10));"
        "function nest(bs, m) { var t = 0; for (var i = 0; i < bs.length; i++) { var a = bs[i]; for (var j = i + 1; j < bs.length; j++) { var b = bs[j]; t += a.x * b.x; b.y += m; } } return t; }"
        "var bs = []; for (var k = 0; k < 8; k++) bs.push(new B(k));"
        "var tt = 0; for (var r = 0; r < 3000; r++) tt += nest(bs, 0.5);"
        "out.push(tt, Math.round(bs[7].y));"
        "function longRun(arr, n) { var s = 0; for (var j = 0; j < n; j++) { var b = arr[j & 63]; s += b.x; } return s; }"
        "var arr3 = []; for (var k = 0; k < 64; k++) arr3.push(new B(k));"
        "out.push(longRun(arr3, 3000000));"
        "out.join(',');";
    for (int mode = 0; mode < 3; ++mode) {
        script::Context::Options options;
        if (mode == 0)
            options.jit.mode = script::JitOptions::Mode::Off;
        else if (mode == 1)
            options.jit.threshold = 1;
        else
            options.jit.mode = script::JitOptions::Mode::Eager;
        js::Context ctx(options);
        CHECK_EQ(ctx.eval(program, "<test>").toString(), std::string("63500,0,16,NaN,966000,10504,94500000"));
    }
}

TEST(callsNewAndReturnsInOptimizedCode)
{
    // Optimized code runs calls only (`new` and its loops stay in the baseline code), a return reached by a jump clears only what its
    // path wrote (a parameter given no argument is undefined in the next call), and globals added after the code was made move
    // the slots it reads.
    const char* program =
        "var out = [];"
        "function P(x) { this.v = x; return x * 2; }"
        "var s = 0, objs = 0;"
        "for (var i = 0; i < 30000; i++) { s += P(i & 7); var o = new P(i & 3); objs += o.v; }"
        "out.push(s, objs);"
        "function L(n) { var t = 0; for (var k = 0; k < n; k++) t += k; this.t = t; return t; }"
        "var lt = 0; for (var i = 0; i < 300; i++) lt += L(10) + new L(5000).t;"
        "out.push(lt);"
        "function f(a, b) { if (a < 3) return a; var c = a - 1; if (b === undefined) { b = c; return b; } return a + b; }"
        "var fs = 0; for (var i = 0; i < 30000; i++) fs += f(i & 7) + f(i & 7, 1) + f(1);"
        "out.push(fs);"
        "function g(n, m) { m = n * 2; if (n < 1) return m; return g(n - 1) + m; }"
        "var gs = 0; for (var i = 0; i < 3000; i++) gs += g(i & 15);"
        "out.push(gs);"
        "function h(a, b, c) { return typeof b + typeof c; }"
        "var hs = ''; for (var i = 0; i < 3000; i++) { h(1, 2, 3); hs = h(1); } out.push(hs);"
        "var G0 = 1; function readG() { return G0 + (typeof G150 === 'number' ? G150 : 0); }"
        "var rs = 0; for (var i = 0; i < 20000; i++) rs += readG();"
        "for (var k = 1; k < 300; k++) this['G' + k] = k;"
        "for (var i = 0; i < 20000; i++) rs += readG();"
        "G0 = 5; for (var i = 0; i < 2000; i++) rs += readG();"
        "out.push(rs);"
        "function fib(n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }"
        "var fb = fib(24); var keep = fib; fib = function (n) { return 1; }; out.push(fb, keep(10)); fib = keep; out.push(fib(20));"
        "out.join(',');";
    for (int mode = 0; mode < 3; ++mode) {
        script::Context::Options options;
        if (mode == 0)
            options.jit.mode = script::JitOptions::Mode::Off;
        else if (mode == 1)
            options.jit.threshold = 1;
        else
            options.jit.mode = script::JitOptions::Mode::Eager;
        js::Context ctx(options);
        CHECK_EQ(ctx.eval(program, "<test>").toString(), std::string("210000,45000,3749263500,240000,254488,undefinedundefined,3350000,46368,2,6765"));
    }
}

TEST(directCallsSetNewTargetOnlyForCalleesThatReadIt)
{
    // A direct call leaves the frame's new.target unset for a callee that never reads it; one that does sees undefined.
    const char* program =
        "function nt(x) { return new.target === undefined ? x : -x; }"
        "function plain(x) { return x + 1; }"
        "function caller(n) { var s = 0; for (var i = 0; i < n; i++) { s += nt(i) + plain(i); } return s; }"
        "var t = 0; for (var r = 0; r < 300; r++) t += caller(100);"
        "var o = new nt(3); t += (o instanceof nt) ? 1 : 0;"
        "for (var r = 0; r < 300; r++) { t += caller(10); new nt(1); }"
        "t;";
    for (int mode = 0; mode < 3; ++mode) {
        script::Context::Options options;
        if (mode == 0)
            options.jit.mode = script::JitOptions::Mode::Off;
        else if (mode == 1)
            options.jit.threshold = 1;
        else
            options.jit.mode = script::JitOptions::Mode::Eager;
        js::Context ctx(options);
        CHECK_EQ(ctx.eval(program, "<test>").toString(), std::string("3030001"));
    }
}

TEST(treesComparedFreedAndBuiltInPlace)
{
    // A property compared with null or undefined and then dead is read by the jump itself (a getter, other shapes, exits),
    // trees and chains are freed without recursion (with strings, arrays and properties out of line among the values), and
    // a `new` whose window registers are unwritten fills them when it takes the long way.
    const char* program =
        "var out = [];"
        "function T(l, r) { this.l = l; this.r = r; }"
        "function mk(d) { return d === 0 ? new T(null, null) : new T(mk(d - 1), mk(d - 1)); }"
        "function chk(t) { return t.l === null ? 1 : 1 + chk(t.l) + chk(t.r); }"
        "var s = 0; for (var i = 0; i < 200; i++) s += chk(mk(6));"
        "out.push(s);"
        "var odd = new T(null, null); Object.defineProperty(odd, 'l', { get: function () { return null; } });"
        "out.push(chk(new T(new T(null, null), odd)));"
        "function isU(o) { return o.v === undefined ? 'u' : o.v !== true ? 'n' : 't'; }"
        "var marks = ''; var objs = [{ v: undefined }, { v: true }, { v: 3 }, { w: 1, v: true }];"
        "for (var i = 0; i < 4000; i++) marks = isU(objs[i & 3]) + marks.slice(0, 7);"
        "out.push(marks);"
        "function N(a, b, c) { this.a = a; this.b = b; this.c = c; }"
        "function mix(n) { var keep = null; for (var i = 0; i < n; i++) { var x = new N('s' + i, [i, i + 1], keep); if (i % 7 === 0) x.extra = { k: i }; keep = (i % 3) ? x : null; } return keep; }"
        "var m = null; for (var r = 0; r < 200; r++) m = mix(500);"
        "out.push(m.a, m.b[1], m.c === null ? 'null' : m.c.a);"
        "function L(prev, v) { this.p = prev; this.v = v; }"
        "function chain(n) { var t = null; for (var i = 0; i < n; i++) t = new L(t, i); return t; }"
        "var sum = 0; for (var r = 0; r < 50; r++) { var c = chain(20000); while (c) { sum += c.v; c = c.p; } }"
        "out.push(sum);"
        "out.join(',');";
    for (int mode = 0; mode < 3; ++mode) {
        script::Context::Options options;
        if (mode == 0)
            options.jit.mode = script::JitOptions::Mode::Off;
        else if (mode == 1)
            options.jit.threshold = 1;
        else
            options.jit.mode = script::JitOptions::Mode::Eager;
        js::Context ctx(options);
        CHECK_EQ(ctx.eval(program, "<test>").toString(), std::string("25400,3,tntutntu,s499,500,null,9999500000"));
    }
}

TEST(aMathCallRunAgainAfterItsResultChangedKindHasItsArgument)
{
    // `Math.round(a)` whose results were all int32s is compiled with an int32 result and, `Math` being a global nothing
    // else reads, without writing the registers of `Math` and its method. A result of another kind (-0) leaves the code
    // from before those reads and runs the call again: with the argument it was given, which no one reads after the call.
    const char* program =
        "function r(a) { return Math.round(a); }"
        "var out = [];"
        "for (var k = 0; k < 6; k++) {"
        "  for (var i = 0; i < 4000; i++) r(i * 0.25);"
        "  out.push(1 / r(-0), 1 / r(-0.2), 1 / r(0.4), 1 / r(-0.5), r(2.5), r(-7.5));"
        "}"
        "out.join(',')";
    std::string want;
    for (int k = 0; k < 6; ++k)
        want += std::string(k ? "," : "") + "-Infinity,-Infinity,Infinity,-Infinity,3,-7";
    for (int mode = 0; mode < 3; ++mode) {
        script::Context::Options options;
        if (mode == 0)
            options.jit.mode = script::JitOptions::Mode::Off;
        else if (mode == 1)
            options.jit.threshold = 1;
        else
            options.jit.mode = script::JitOptions::Mode::Eager;
        js::Context ctx(options);
        CHECK_EQ(ctx.eval(program, "<test>").toString(), want);
    }
}

TEST(aFrameEnteredFromANativeStartsWithUndefinedRegisters)
{
    // `f.call(undefined)` from a function whose frame began inside its caller's: the frame of `f` sits where that caller
    // had temporaries (an object literal's values). Its parameters given no argument are undefined all the same, in
    // optimized code too, which reads them without a look; and its arguments object is let go of when it returns.
    const char* program =
        "var out = [];"
        "function run(a, b) {"
        "  var v5 = [1, String([({x: 1, y: b}), a, ({x: ({x: a, y: 1}), y: ({x: b, y: b})})].reduce(function (p, x, i) { return p + b; }, b)), 3];"
        "  var v6 = function (a, b) { return String(a) + String(b) + arguments.length; }, v8 = 0;"
        "  var cv6 = function () { return v6.call(undefined); };"
        "  for (var v7 = 0; v7 < 300; v7++) v8 = String(cv6());"
        "  out.push(v8); v6 = {}; try { out.push(String(cv6())); } catch (e) { out.push(e.name); }"
        "}"
        "for (var i = 0; i < 40; i++) run(i, i * 2);"
        "out.filter(function (x) { return x !== 'undefinedundefined0' && x !== 'TypeError'; }).length + ':' + out.length";
    for (int mode = 0; mode < 3; ++mode) {
        script::Context::Options options;
        if (mode == 0)
            options.jit.mode = script::JitOptions::Mode::Off;
        else if (mode == 1)
            options.jit.threshold = 1;
        else
            options.jit.mode = script::JitOptions::Mode::Eager;
        js::Context ctx(options);
        CHECK_EQ(ctx.eval(program, "<test>").toString(), std::string("0:80"));
    }
}

TEST(int32SlotsALoopKeepsInRegisters)
{
    // A loop that reads and stores int32 properties of an object it holds keeps them in registers where the assembler
    // has spare ones, loads them before the loop and writes them back on every way out: at its end, an early return,
    // an overflow into a double, a store of a fraction, another name for the same object (an element read), an
    // accessor or a string in the slot.
    const char* program =
        "var out = []; "
        "function P() { this.a = 1; this.b = 2; this.c = 3; this.d = 4; } "
        "function spin(o, n) { for (var i = 0; i < n; i++) { o.a = o.b + 1; o.b = o.c + 1; o.c = o.d - 1; o.d = o.a - 1; } return o.a + o.b + o.c + o.d; } "
        "out.push(spin(new P(), 30000)); "
        "function grow(o, n) { for (var i = 0; i < n; i++) { o.a = o.a + o.b; o.b = o.b + 1; } return o.a; } "
        "var g = new P(); out.push(grow(g, 20000), grow(g, 70000), g.b); "
        "function alias(o, os, n) { var s = 0; for (var i = 0; i < n; i++) { o.a = o.a + 1; var q = os[i & 1]; q.b = q.b + 2; s += o.b; } return s + o.a; } "
        "var x = new P(), y = new P(); out.push(alias(x, [y, y], 20000), alias(x, [y, x], 20000), x.a, x.b, y.b); "
        "function early(o, n) { for (var i = 0; i < n; i++) { o.c = o.c + 3; if (o.c > 50000) return i; } return -1; } "
        "var e = new P(); out.push(early(e, 100000), e.c); "
        "function frac(o, n) { for (var i = 0; i < n; i++) { o.d = o.d + 1; if (i === n - 5) o.d = 0.5; } return o.d; } "
        "out.push(frac(new P(), 20000), frac(new P(), 20000)); "
        "function acc(o, n) { for (var i = 0; i < n; i++) o.a = o.a + 1; return o.a; } "
        "var h = new P(); acc(h, 20000); Object.defineProperty(h, 'a', { get: function () { return 7; }, set: function (v) { out.push('set'); }, configurable: true }); out.push(acc(h, 2)); "
        "var z = new P(); z.a = 'x'; out.push(acc(z, 3), acc(new P(), 20000)); "
        "out.join(',');";
    for (int mode = 0; mode < 3; ++mode) {
        script::Context::Options options;
        if (mode == 0)
            options.jit.mode = script::JitOptions::Mode::Off;
        else if (mode == 1)
            options.jit.threshold = 1;
        else
            options.jit.mode = script::JitOptions::Mode::Eager;
        js::Context ctx(options);
        CHECK_EQ(ctx.eval(program, "<test>").toString(),
                 std::string("14,200030001,4050135001,90002,60001,200080001,40001,20002,60002,16665,50001,4.5,4.5,set,set,7,x111,20001"));
    }
}

TEST(looseEqualityInCompiledCode)
{
    // `==` and `!=` are decided in compiled code for int32s, numbers, null and undefined, booleans and two objects; a
    // string, a symbol, a bigint or two kinds that convert go the long way. Every pair agrees in every tier.
    const char* program =
        "var o = {}, p = {}, s1 = 'ab', s2 = 'a' + 'b', sym = Symbol(), big = 10n;"
        "var vals = [0, -0, 1, 1.5, NaN, Infinity, -1, null, undefined, true, false, o, p, s1, s2, '', '1', '0', sym, big,"
        "            1n, [], [1], 2147483647, 4294967296];"
        "function eq(a, b) { return a == b; } function ne(a, b) { return a != b; }"
        "var first = '', last = '';"
        "for (var rep = 0; rep < 300; rep++) {"
        "  var s = '';"
        "  for (var i = 0; i < vals.length; i++) for (var j = 0; j < vals.length; j++)"
        "    s += (eq(vals[i], vals[j]) ? 1 : 0) + (ne(vals[i], vals[j]) ? 2 : 0);"
        "  if (rep === 0) first = s; else last = s;"
        "}"
        "var ones = 0; for (var k = 0; k < first.length; k++) ones += first[k] === '1' ? 1 : 0;"
        "(first === last) + ':' + ones + ':' + first.length";
    for (int mode = 0; mode < 3; ++mode) {
        script::Context::Options options;
        if (mode == 0)
            options.jit.mode = script::JitOptions::Mode::Off;
        else if (mode == 1)
            options.jit.threshold = 1;
        else
            options.jit.mode = script::JitOptions::Mode::Eager;
        js::Context ctx(options);
        CHECK_EQ(ctx.eval(program, "<test>").toString(), std::string("true:74:625"));
    }
}

TEST(typeofNotAndTruthInCompiledCode)
{
    // `typeof`, `!`, a jump on any value and `x++` (ToNumeric) have templates in compiled code; every kind of value,
    // proxies and bigints included, gives what the interpreter gives, in every tier.
    const char* program =
        "var vals = [0, -0, 1, 1.5, NaN, Infinity, -1, null, undefined, true, false, {}, [], function () {}, '', 'x', Symbol(), 0n, 1n, new Proxy(function () {}, {}), new Proxy({}, {}), 2147483647, 1e300, -1e-300]; "
        "function t(v) { return typeof v; } "
        "function n(v) { return !v; } "
        "function j(v) { if (v) return 1; return 0; } "
        "function k(v) { var x = v; x++; return x; } "
        "var first = '', last = ''; "
        "for (var rep = 0; rep < 2000; rep++) { "
        "  var s = ''; "
        "  for (var i = 0; i < vals.length; i++) { "
        "    s += t(vals[i]) + (n(vals[i]) ? 'T' : 'F') + j(vals[i]); "
        "    if (typeof vals[i] !== 'symbol' && typeof vals[i] !== 'bigint') { try { s += String(k(vals[i])); } catch (e) { s += 'E'; } } "
        "  } "
        "  if (rep === 0) first = s; else last = s; "
        "} "
        "'' + (first === last) + ' ' + first";
    for (int mode = 0; mode < 3; ++mode) {
        script::Context::Options options;
        if (mode == 0)
            options.jit.mode = script::JitOptions::Mode::Off;
        else if (mode == 1)
            options.jit.threshold = 1;
        else
            options.jit.mode = script::JitOptions::Mode::Eager;
        js::Context ctx(options);
        CHECK_EQ(ctx.eval(program, "<test>").toString(), std::string("true numberT01numberT01numberF12numberF12.5numberT0NaNnumberF1InfinitynumberF10objectT01undefinedT0NaNbooleanF12booleanT01objectF1NaNobjectF11functionF1NaNstringT01stringF1NaNsymbolF1bigintT0bigintF1functionF1NaNobjectF1NaNnumberF12147483648numberF11e+300numberF11"));
    }
}

TEST(newOfClassesAndFunctionsInCompiledCode)
{
    // `new` finds `prototype` in slot 2 of any function whose shape says so (classes with statics, fields, derived
    // classes, a constructor returning an object, new.target, a primitive or read-only `prototype`), and a class with no
    // field initialiser starts without the interpreter. Every tier gives node's answers.
    const char* program =
        "var out = []; "
        "class V { static add(a, b) { return new V(a.x + b.x); } constructor(x) { this.x = x; } get twice() { return this.x * 2; } } "
        "class F { y = 7; static s = 1; constructor(x) { this.x = x; } } "
        "class B { constructor(x) { this.b = x; } } "
        "class D extends B { z = 3; constructor(x) { super(x + 1); this.d = x; } } "
        "class R { constructor() { return { made: 'other' }; } } "
        "class T { constructor() { this.t = new.target === T; } } "
        "function P(a) { this.a = a; } "
        "function Q(a) { this.q = a; } Q.prototype = 5; "
        "function W(a) { this.w = a; } Object.defineProperty(W, 'prototype', { writable: false }); "
        "function mk(n) { "
        "  var s = 0, t = ''; "
        "  for (var i = 0; i < n; i++) { "
        "    var v = V.add(new V(i), new V(1)); s += v.x + v.twice; "
        "    var f = new F(i); s += f.x + f.y; "
        "    var d = new D(i); s += d.b + d.d + d.z; "
        "    var r = new R(); t = r.made; "
        "    var tt = new T(); if (!tt.t) t = 'bad'; "
        "    var p = new P(i); s += p.a; "
        "    var q = new Q(i); s += q.q + (Object.getPrototypeOf(q) === Object.prototype ? 1 : 100); "
        "    var w = new W(i); s += w.w + (Object.getPrototypeOf(w) === W.prototype ? 1 : 100); "
        "  } "
        "  return s + t; "
        "} "
        "out.push(mk(3000), mk(5)); "
        "V.prototype.extra = function () { return 1; }; out.push(new V(2).extra()); "
        "F.prototype = null; out.push(Object.getPrototypeOf(new F(1)) === F.prototype); "
        "try { V(1); } catch (e) { out.push(e.name); } "
        "out.join(',');";
    for (int mode = 0; mode < 3; ++mode) {
        script::Context::Options options;
        if (mode == 0)
            options.jit.mode = script::JitOptions::Mode::Off;
        else if (mode == 1)
            options.jit.threshold = 1;
        else
            options.jit.mode = script::JitOptions::Mode::Eager;
        js::Context ctx(options);
        CHECK_EQ(ctx.eval(program, "<test>").toString(), std::string("40534500other,170other,1,true,TypeError"));
    }
}

TEST(instanceofWithoutALookForHasInstance)
{
    // `instanceof` skips the look for `Symbol.hasInstance` while no object has had one and no proxy is on the right
    // side's chain; one defined later (a static method, defineProperty), a proxy's trap, a bound function, a primitive
    // and a non-object `prototype` still behave, in every tier.
    const char* program =
        "var out = []; "
        "class A {} class B extends A {} function F() {} var bound = F.bind(null); "
        "var pf = new Proxy(function () {}, { get: function (t, k) { return k === Symbol.hasInstance ? function () { return true; } : t[k]; } }); "
        "function chk(n) { "
        "  var s = ''; "
        "  for (var i = 0; i < n; i++) { "
        "    var b = new B(), f = new F(); "
        "    s = '' + (b instanceof A) + (b instanceof B) + (f instanceof A) + (f instanceof F) + (f instanceof bound) + (1 instanceof F) + "
        "        (null instanceof Object) + ([] instanceof Array) + ({} instanceof pf) + (b instanceof Object); "
        "  } "
        "  return s; "
        "} "
        "out.push(chk(3000)); "
        "function G() {} G.prototype = 3; "
        "try { ({}) instanceof G; out.push('no'); } catch (e) { out.push(e.name); } "
        "try { ({}) instanceof {}; out.push('no'); } catch (e) { out.push(e.name); } "
        "class Even { static [Symbol.hasInstance](x) { return x % 2 === 0; } } "
        "out.push(chk(3000), 2 instanceof Even, 3 instanceof Even); "
        "Object.defineProperty(A, Symbol.hasInstance, { value: function () { return false; } }); "
        "out.push(chk(10)); "
        "out.join(',');";
    for (int mode = 0; mode < 3; ++mode) {
        script::Context::Options options;
        if (mode == 0)
            options.jit.mode = script::JitOptions::Mode::Off;
        else if (mode == 1)
            options.jit.threshold = 1;
        else
            options.jit.mode = script::JitOptions::Mode::Eager;
        js::Context ctx(options);
        CHECK_EQ(ctx.eval(program, "<test>").toString(), std::string("truetruefalsetruetruefalsefalsetruetruetrue,TypeError,TypeError,truetruefalsetruetruefalsefalsetruetruetrue,true,false,falsefalsefalsetruetruefalsefalsetruetruetrue"));
    }
}

TEST(holeReadsOfPlainArraysAreUndefined)
{
    // A read of a hole, or past the end, of an array with no properties of its own and the usual prototypes, which have
    // no elements, is undefined without a look; elements put on Array.prototype or Object.prototype, an accessor at an
    // index, a named property, a subclass and another prototype are still seen, in every tier.
    const char* program =
        "var out = []; "
        "function rd(a, n) { var s = ''; for (var r = 0; r < n; r++) { s = ''; for (var i = 0; i < 6; i++) s += String(a[i]) + '|'; } return s; } "
        "var a = new Array(5); a[1] = 1; a[3] = null; "
        "out.push(rd(a, 3000)); "
        "var b = [0, 1, 2, 3]; delete b[2]; out.push(rd(b, 3000)); "
        "var c = new Array(4); c.foo = 1; out.push(rd(c, 3000)); "
        "var d = new Array(4); Object.defineProperty(d, 2, { get: function () { return 'got'; }, configurable: true }); out.push(rd(d, 3000)); "
        "class MyArr extends Array {} var e = new MyArr(4); out.push(rd(e, 3000)); "
        "Array.prototype[2] = 'ap'; out.push(rd(a, 3000), rd(b, 10)); "
        "delete Array.prototype[2]; Object.prototype[4] = 'op'; out.push(rd(a, 3000)); "
        "delete Object.prototype[4]; out.push(rd(a, 3000)); "
        "var f = new Array(3); Object.setPrototypeOf(f, { 1: 'own proto' }); out.push(rd(f, 3000)); "
        "out.join(' ');";
    for (int mode = 0; mode < 3; ++mode) {
        script::Context::Options options;
        if (mode == 0)
            options.jit.mode = script::JitOptions::Mode::Off;
        else if (mode == 1)
            options.jit.threshold = 1;
        else
            options.jit.mode = script::JitOptions::Mode::Eager;
        js::Context ctx(options);
        CHECK_EQ(ctx.eval(program, "<test>").toString(), std::string("undefined|1|undefined|null|undefined|undefined| 0|1|undefined|3|undefined|undefined| undefined|undefined|undefined|undefined|undefined|undefined| undefined|undefined|got|undefined|undefined|undefined| undefined|undefined|undefined|undefined|undefined|undefined| undefined|1|ap|null|undefined|undefined| 0|1|ap|3|undefined|undefined| undefined|1|undefined|null|op|undefined| undefined|1|undefined|null|undefined|undefined| undefined|own proto|undefined|undefined|undefined|undefined|"));
    }
}

TEST(int32VariablesTakeDoublesThatHoldInt32s)
{
    // `x++` (ToNumeric) keeps an int32 variable an int32; a read into an int32 variable takes a double that holds an int32
    // exactly (optimized code boxes what a double variable holds as a double) and leaves for -0, a fraction or a value
    // past 32 bits. In every tier, node's answers.
    const char* program =
        "var out = []; "
        "function fill(a, n, k) { for (var i = 0; i < n; i++) a[i] = (i * k) / 2; return a; } "
        "function sum(a, w, n) { var c = 0, j = 0; for (var i = 0; i < n; ) { var l = a[i++] & 0x3fff; c = (c + l + (w[j++] | 0)) | 0; } return c; } "
        "function mix(a, n) { var s = 0; for (var i = 0; i < n; i++) { var x = a[i]; s += x; } return s; } "
        "var a = fill([], 1000, 2), w = fill([], 1000, 4); "
        "var r = 0; for (var k = 0; k < 50; k++) r = sum(a, w, 1000); "
        "out.push(r, mix(a, 1000)); "
        "a[3] = -0; a[4] = 0.5; a[5] = 1e10; a[6] = -7; "
        "out.push(sum(a, w, 1000), mix(a, 1000), 1 / mix([-0], 1)); "
        "for (var k = 0; k < 50; k++) r = sum(a, w, 1000); "
        "out.push(r, mix(fill([], 1000, 3), 1000)); "
        "out.join(',');";
    for (int mode = 0; mode < 3; ++mode) {
        script::Context::Options options;
        if (mode == 0)
            options.jit.mode = script::JitOptions::Mode::Off;
        else if (mode == 1)
            options.jit.threshold = 1;
        else
            options.jit.mode = script::JitOptions::Mode::Eager;
        js::Context ctx(options);
        CHECK_EQ(ctx.eval(program, "<test>").toString(), std::string("1498500,499500,1524075,10000499475.5,Infinity,1524075,749250"));
    }
}

TEST(callsOverDeadTemporariesKeepCounts)
{
    // A call's frame sits on the caller's registers above its arguments; dead temporaries there that hold objects are let
    // go of by compiled code (or the long way, for a last count). Nothing leaks and nothing is freed early, in every tier.
    const char* program =
        "function P(v) { this.v = v; } P.prototype.get = function () { return this.v; };"
        "P.prototype.add = function (o) { return new P(this.v + o.get()); };"
        "function id(x) { return x; }"
        "function round(n) { var s = 0, acc = new P(0); for (var i = 0; i < n; i++) {"
        "  var a = new P(i), b = { w: a }, c = [b, a];"
        "  acc = acc.add(id(c[1])); s += b.w.get() + id(a).get(); } return s + acc.get(); }";
    for (int mode = 0; mode < 3; ++mode) {
        script::Context::Options options;
        if (mode == 0)
            options.jit.mode = script::JitOptions::Mode::Off;
        else if (mode == 1)
            options.jit.threshold = 1;
        else
            options.jit.mode = script::JitOptions::Mode::Eager;
        js::Context ctx(options);
        ctx.eval(program, "<test>");
        CHECK_EQ(ctx.eval("round(5000)").toString(), std::string("37492500"));
        ctx.collectGarbage();
        const auto before = ctx.stats().liveObjects;
        for (int k = 0; k < 5; ++k)
            CHECK_EQ(ctx.eval("round(5000)").toString(), std::string("37492500"));
        ctx.collectGarbage();
        CHECK(ctx.stats().liveObjects <= before + 16);
    }
}

TEST(strictEqualityOfCellsAndDoublesInCompiledCode)
{
    // `===` decides two objects, two symbols, a string against a string of another length or another interned one, and
    // doubles (NaN, -0) in compiled code; strings of one length not both interned, and bigints, go the long way.
    const char* program =
        "var strs = ['abc', 'ab' + 'c', String.fromCharCode(97, 98, 99), 'abd', 'ab', '', 'x'.repeat(100),"
        "            'x'.repeat(99) + 'x', Symbol.iterator, Symbol('s'), 1n, 2n - 1n, {}, [], 1.5, 3 / 2, NaN, 0, -0, null];"
        "function js(a, b) { if (a === b) return 1; return 0; }"
        "function jv(a, b) { return a !== b; }"
        "var r = '';"
        "for (var rep = 0; rep < 300; rep++) { r = '';"
        "  for (var i = 0; i < strs.length; i++) for (var j = 0; j < strs.length; j++) r += js(strs[i], strs[j]) + (jv(strs[i], strs[j]) ? 2 : 0); }"
        "r";
    std::string want;
    {
        script::Context::Options options;
        options.jit.mode = script::JitOptions::Mode::Off;
        js::Context ctx(options);
        want = ctx.eval(program, "<test>").toString();
    }
    CHECK_EQ(want.size(), std::size_t(400));
    for (int mode = 1; mode < 3; ++mode) {
        script::Context::Options options;
        if (mode == 1)
            options.jit.threshold = 1;
        else
            options.jit.mode = script::JitOptions::Mode::Eager;
        js::Context ctx(options);
        CHECK_EQ(ctx.eval(program, "<test>").toString(), want);
    }
}

TEST(anAddedPropertyGoesInPlaceOnlyWhereTheObjectHasRoom)
{
    // Objects of one shape can have different numbers of inline slots: `new G` (a constructor that stores four or more
    // properties on `this`) has eight, Object.create three. A store site that learned to add a property in place on
    // one must not write past the end of the other: it takes the long way there, in every tier.
    const char* program =
        "function G() { this.a = 1; this.b = 2; this.c = 3; this.d = 4; } "
        "function abcd(o) { o.a = 1; o.b = 2; o.c = 3; o.d = 4; return o; } "
        "function grow(o, n) { o.e = n; o.f = n + 1; o.g = n + 2; return o; } "
        "var out = [], s = 0, keep = []; "
        "for (var i = 0; i < 3000; i++) { "
        "  var y = grow(new G(), i), x = grow(abcd(Object.create(G.prototype)), i); "
        "  s += x.e + x.f + x.g + y.g + x.a + x.d; "
        "  if (i % 100 === 0) keep.push(x, y, { pad: i, q: [i] }); "
        "} "
        "var t = 0; for (var i = 0; i < keep.length; i++) t += (keep[i].g | 0) + (keep[i].pad | 0) + (keep[i].q ? keep[i].q[0] : 0); "
        "out.push(s, t, JSON.stringify(keep[0]), Object.keys(keep[1]).join('')); "
        "out.join(' ');";
    for (int mode = 0; mode < 3; ++mode) {
        script::Context::Options options;
        if (mode == 0)
            options.jit.mode = script::JitOptions::Mode::Off;
        else if (mode == 1)
            options.jit.threshold = 1;
        else
            options.jit.mode = script::JitOptions::Mode::Eager;
        js::Context ctx(options);
        CHECK_EQ(ctx.eval(program, "<test>").toString(), std::string("18024000 174120 {\"a\":1,\"b\":2,\"c\":3,\"d\":4,\"e\":0,\"f\":1,\"g\":2} abcdefg"));
    }
}

TEST(aFunctionWithMoreThan65535NamesAndConstants)
{
    // LoadK names its constant with 32 bits, and an object literal whose function has used up its 65535 names stores the
    // rest by a key held in a register: a literal of 70,000 properties with 70,000 different double values compiles and
    // runs, in every tier.
    std::string program = "function make() { return {";
    for (int i = 0; i < 70000; ++i)
        program += "k" + std::to_string(i) + ": " + std::to_string(i) + ".5,";
    program += "}; } var o = make(); var s = 0; for (var k in o) s += o[k];"
               "Object.keys(o).length + ',' + o.k0 + ',' + o.k65534 + ',' + o.k65535 + ',' + o.k69999 + ',' + s";
    for (int mode = 0; mode < 3; ++mode) {
        script::Context::Options options;
        if (mode == 0)
            options.jit.mode = script::JitOptions::Mode::Off;
        else if (mode == 1)
            options.jit.threshold = 1;
        else
            options.jit.mode = script::JitOptions::Mode::Eager;
        js::Context ctx(options);
        CHECK_EQ(ctx.eval(program, "<test>").toString(), std::string("70000,0.5,65534.5,65535.5,69999.5,2450000000"));
    }
}

TEST(aCallFindsUndefinedPastItsCallersEnd)
{
    // A frame sits on the caller's registers above its arguments, and a call it makes in turn can reach past its end into
    // the registers of the caller's caller. Those must be undefined there: a parameter given no argument read a dead
    // temporary of a frame further down (Babel's _arrayLikeToArray, `function n(l, n)`, called with one argument from an
    // inner function, made an array of a function).
    const char* program =
        "var mods = { 966: function (l) { function n(l, n) { (null == n || n > l.length) && (n = l.length);"
        "  for (var u = 0, e = Array(n); u < n; u++) e[u] = l[u]; return e }"
        "  var u, e = [{ a: 1 }].concat(function (l) { if (Array.isArray(l)) return n(l) }"
        "    (u = Array.from({ length: 3 }, function (l, n) { return { i: n } })) || function () { throw new TypeError('x') }());"
        "  l.exports = { W: e } } };"
        "var m = { exports: {} }; mods[966](m); JSON.stringify(m.exports.W)";
    for (int mode = 0; mode < 3; ++mode) {
        script::Context::Options options;
        if (mode == 0)
            options.jit.mode = script::JitOptions::Mode::Off;
        else if (mode == 1)
            options.jit.threshold = 1;
        else
            options.jit.mode = script::JitOptions::Mode::Eager;
        js::Context ctx(options);
        CHECK_EQ(ctx.eval(program, "<test>").toString(), std::string("[{\"a\":1},{\"i\":0},{\"i\":1},{\"i\":2}]"));
    }
}

TEST(numbersPackedAtTheStartOfAnArrayStayKnown)
{
    // An array knows how many of its first elements are numbers (`JSArray::packed`), and compiled code stores a number
    // over one of them without a look at what was there. Everything that puts something else there, makes a hole,
    // moves elements, shortens or freezes the array must be seen: objects and strings among the numbers, delete,
    // length, push and pop, shift and unshift, splice, reverse, sort, copyWithin, an accessor at an index, and objects
    // overwritten by numbers (which are let go of: the soak tests count them).
    const char* program =
        "var out = []; "
        "function fill(a, n, v) { for (var i = 0; i < n; i++) a[i] = v; } "
        "function sum(a) { var s = 0; for (var i = 0; i < a.length; i++) { var x = a[i]; if (typeof x === 'number') s += x; else if (x) s += 1000; } return s; } "
        "function bump(a, k) { for (var r = 0; r < k; r++) for (var i = 0; i < a.length; i++) a[i] = a[i] + 1; } "
        "function put(a, i, v) { a[i] = v; } "
        "var a = new Array(3000); fill(a, 3000, 1); bump(a, 3); out.push(sum(a)); "
        "var o = { n: 7 }; for (var i = 0; i < 3000; i++) put(a, i % 3000, i % 7 === 0 ? o : i); "
        "out.push(sum(a)); fill(a, 3000, 0.5); out.push(sum(a)); "
        "for (var i = 0; i < 3000; i += 2) put(a, i, 'str' + i); fill(a, 3000, 2); out.push(sum(a)); "
        "a[10] = o; delete a[20]; bump(a, 1); out.push(String(a[10]).slice(0, 15), a[20] !== a[20], a[21]); "
        "fill(a, 3000, 3); a.length = 1000; a.push(o); a.pop(); a.pop(); fill(a, a.length, 4); out.push(sum(a), a.length); "
        "a.shift(); a.unshift(o, 1); fill(a, a.length, 5); out.push(sum(a)); "
        "a.splice(5, 2, o, o, o); bump(a, 1); fill(a, a.length, 6); a.reverse(); fill(a, a.length, 6); out.push(sum(a)); "
        "var b = [3, 1, o, 2]; b.sort(function (x, y) { return (typeof x === 'number' ? x : 9) - (typeof y === 'number' ? y : 9); }); fill(b, 4, 1); out.push(sum(b)); "
        "var f = [1, 2, 3, 4, 5, 6]; fill(f, 6, 9); Object.freeze(f); try { fill(f, 6, 7); } catch (e) { out.push('thrown'); } for (var r = 0; r < 3000; r++) put(f, r % 6, 7); out.push(f.join('')); "
        "var g = []; for (var i = 0; i < 3000; i++) g.push(i); fill(g, 3000, 1.5); g.copyWithin(0, 10, 20); g[5] = [g.length]; fill(g, 3000, 2.5); out.push(sum(g)); "
        "var h = [0, 1, 2, 3]; Object.defineProperty(h, 1, { get: function () { return 42; }, set: function (v) { out.push('set' + v); }, configurable: true }); fill(h, 4, 8); out.push(h[0], h[1], h[2]); "
        "var keep = []; for (var r = 0; r < 200; r++) { var t = []; for (var i = 0; i < 50; i++) t.push({ k: i }); fill(t, 50, r); keep = t; } out.push(sum(keep)); "
        "out.join(',');";
    for (int mode = 0; mode < 3; ++mode) {
        script::Context::Options options;
        if (mode == 0)
            options.jit.mode = script::JitOptions::Mode::Off;
        else if (mode == 1)
            options.jit.threshold = 1;
        else
            options.jit.mode = script::JitOptions::Mode::Eager;
        js::Context ctx(options);
        CHECK_EQ(ctx.eval(program, "<test>").toString(),
                 std::string("12000,4284858,1500,6000,[object Object],true,3,3996,999,5000,6006,4,999999,7500,set8,8,42,8,9950"));
    }
}

TEST(anInlinedNewTakesTheCountOfArgumentsThatDie)
{
    // An inlined `new` moves an argument's count into the object only when its register dies there: not for one passed
    // twice, nor one still used after.
    const char* program =
        "function P(a, b) { this.a = a; this.b = b; }"
        "function twice(n) { var r, s = 0; for (var i = 0; i < n; i++) { var x = { v: i }; r = new P(x, x); s += r.a.v + r.b.v; } return s; }"
        "function after(n) { var s = 0; for (var i = 0; i < n; i++) { var x = { v: i }; var r = new P(x, null); x.v = -1; s += r.a.v; } return s; }"
        "function chainP(n) { var t = null; for (var i = 0; i < n; i++) t = new P(t, i); var c = 0; while (t) { c += t.b; t = t.a; } return c; }"
        "var out = 0; for (var k = 0; k < 50; k++) out += twice(200) + after(200) + chainP(300);"
        "out;";
    for (int mode = 0; mode < 3; ++mode) {
        script::Context::Options options;
        if (mode == 0)
            options.jit.mode = script::JitOptions::Mode::Off;
        else if (mode == 1)
            options.jit.threshold = 1;
        else
            options.jit.mode = script::JitOptions::Mode::Eager;
        js::Context ctx(options);
        CHECK_EQ(ctx.eval(program, "<test>").toString(), std::string("4222500"));
    }
}

TEST(loopPromotedSlotsSeeEveryOtherWayToTheObject)
{
    // A number slot a loop reads and stores through a register it does not write is kept in a stack slot: an element read
    // that turns out to be the same object leaves the code (writing it back), as does an element of another shape, and a
    // loop entered part way loads it first.
    const char* program =
        "function B(x, v) { this.x = x; this.v = v; this.m = 1.5; }"
        "function step(bs, dt) { var n = bs.length; for (var i = 0; i < n; i++) { var a = bs[i]; for (var j = 0; j < n; j++) { var b = bs[j]; var d = a.x - b.x; a.v -= d * b.m * dt; b.v += d * a.m * dt; } } }"
        "function sum(bs) { var s = 0; for (var i = 0; i < bs.length; i++) s += bs[i].v; return Math.round(s * 1e6); }"
        "var out = [];"
        "var bs = []; for (var k = 0; k < 6; k++) bs.push(new B(k * 1.25, 0.5 * k));"
        "for (var r = 0; r < 3000; r++) step(bs, 0.01);"
        "out.push(sum(bs), Math.round(bs[2].v * 1e6));"
        "var odd = { x: 3, v: 1, m: 2, extra: 1 }; bs[3] = odd;"
        "for (var r = 0; r < 50; r++) step(bs, 0.01);"
        "out.push(sum(bs));"
        "bs[3] = new B(1, 2); bs[4].v = 'str';"
        "try { step(bs, 0.01); out.push(typeof bs[4].v, String(bs[0].v).slice(0, 6)); } catch (e) { out.push(e.name); }"
        "var big = []; for (var k = 0; k < 400; k++) big.push(new B(k * 0.5, k));"
        "step(big, 0.001); out.push(sum(big));"
        "function self(bs) { for (var i = 0; i < bs.length; i++) { var a = bs[i]; for (var j = 0; j < bs.length; j++) { var b = bs[i]; a.v += b.m; } } }"
        "var ss = [new B(1, 0), new B(2, 0)]; for (var r = 0; r < 2000; r++) self(ss); out.push(ss[0].v, ss[1].v);"
        "out.join(',');";
    for (int mode = 0; mode < 3; ++mode) {
        script::Context::Options options;
        if (mode == 0)
            options.jit.mode = script::JitOptions::Mode::Off;
        else if (mode == 1)
            options.jit.threshold = 1;
        else
            options.jit.mode = script::JitOptions::Mode::Eager;
        js::Context ctx(options);
        CHECK_EQ(ctx.eval(program, "<test>").toString(), std::string("7500000,338500000,344500000,number,1716.4,79800000000,6000,6000"));
    }
}

TEST(recursionThatChangesNoGlobalChecksItOnce)
{
    // A function that calls itself and can change no global checks its global and its function once, at the entry: called
    // under another name after the global changed, as one of two closures of one code, or one that does write a global.
    const char* program =
        "var out = [];"
        "function fib(n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }"
        "for (var r = 0; r < 30; r++) fib(15);"
        "var g = fib; var orig = fib; fib = function (n) { return 100; }; out.push(g(5));"
        "fib = orig; out.push(fib(20));"
        "function f2(n) { return n < 2 ? n : f2(n - 1) + f2(n - 2); }"
        "var other = function (n) { return n < 2 ? n : f2(n - 1) + f2(n - 2); };"
        "for (var r = 0; r < 30; r++) f2(15);"
        "var mk = function () { return function self(n) { return n < 2 ? n : self(n - 1) + self(n - 2); }; };"
        "var s1 = mk(), s2 = mk(); out.push(s1(18), s2(18));"
        "function h(n) { if (n < 2) return n; if (n === 9) G = 1; return h(n - 1) + h(n - 2); }"
        "var G = 0; for (var r = 0; r < 30; r++) h(14); out.push(h(16), G);"
        "function big(n) { return n < 2 ? n : big(n - 1) + big(n - 2) + 1073741823; }"
        "out.push(big(8));"
        "out.join(',');";
    for (int mode = 0; mode < 3; ++mode) {
        script::Context::Options options;
        if (mode == 0)
            options.jit.mode = script::JitOptions::Mode::Off;
        else if (mode == 1)
            options.jit.threshold = 1;
        else
            options.jit.mode = script::JitOptions::Mode::Eager;
        js::Context ctx(options);
        CHECK_EQ(ctx.eval(program, "<test>").toString(), std::string("200,6765,2584,2584,987,1,35433480180"));
    }
}

TEST(newInPlaceChecksItsConstructorOncePerPath)
{
    // A `new` made in place after another of the same constructor in a function that can change neither does not check it
    // again: a new prototype, a setter on it, a replaced constructor between calls are still seen.
    const char* program =
        "var out = [];"
        "function T(l, r) { this.l = l; this.r = r; }"
        "function mk(d) { return d === 0 ? new T(null, null) : new T(mk(d - 1), mk(d - 1)); }"
        "function count(t) { return t === null ? 0 : 1 + count(t.l) + count(t.r); }"
        "var s = 0; for (var i = 0; i < 300; i++) s += count(mk(5));"
        "out.push(s);"
        "T.prototype.kind = 'tree'; out.push(mk(3).l.r.kind, count(mk(6)));"
        "T.prototype = { other: 1 }; var m = mk(4); out.push(m.other, m.l.other, Object.getPrototypeOf(m.r) === T.prototype);"
        "var seen = 0; Object.defineProperty(T.prototype, 'r', { set: function (v) { seen++; }, configurable: true });"
        "var m2 = mk(3); out.push(seen, m2.hasOwnProperty('r'), m2.hasOwnProperty('l'));"
        "var Old = T; T = function (l, r) { this.z = 1; this.l = l; this.r = r; }; var m3 = mk(3); out.push(m3.z, m3.l.z, count(m3));"
        "T = Old; delete T.prototype.r; out.push(count(mk(7)));"
        "out.join(',');";
    for (int mode = 0; mode < 3; ++mode) {
        script::Context::Options options;
        if (mode == 0)
            options.jit.mode = script::JitOptions::Mode::Off;
        else if (mode == 1)
            options.jit.threshold = 1;
        else
            options.jit.mode = script::JitOptions::Mode::Eager;
        js::Context ctx(options);
        CHECK_EQ(ctx.eval(program, "<test>").toString(), std::string("18900,tree,127,1,1,true,15,false,true,1,1,15,255"));
    }
}

TEST(recursionThatHoldsReadsWithoutCountsKeepsThemWhenAGetterRuns)
{
    // A function that changes nothing and calls only itself holds what it reads from its parameters without a count, and passes
    // it on so; a getter met deep down (the code has left) replaces what the frames above still read after their calls.
    const char* program =
        "function N(l,r,v){this.l=l;this.r=r;this.v=v;}"
        "function mk(d,v){return d===0?new N(null,null,v):new N(mk(d-1,v*2),mk(d-1,v*2+1),v);}"
        "function f(t){var a=t.l; return a===null?1:f(a)+f(t.r)+a.v;}"
        "var s=0; for (var i=0;i<20000;i++) s+=f(mk(4,1));"
        "var res=[s];"
        "for (var depth=1; depth<8; depth++) {"
        "  var b=mk(8,1), parent=b; for (var k=0;k<depth;k++) parent=parent.l;"
        "  var X=parent.l, once=true;"
        "  Object.defineProperty(X,'r',{get:function(){ if(once){once=false; parent.l=mk(1,7); X=null;} var junk=[]; for(var q=0;q<200;q++) junk.push(new N(q,q,q)); keep.push(junk); return mk(1,5);}});"
        "  X=null;"
        "  var keep=[];"
        "  res.push(f(b));"
        "}"
        "res.join(' ')";
    for (int mode = 0; mode < 3; ++mode) {
        script::Context::Options options;
        if (mode == 0)
            options.jit.mode = script::JitOptions::Mode::Off;
        else if (mode == 1)
            options.jit.threshold = 1;
        else
            options.jit.mode = script::JitOptions::Mode::Eager;
        js::Context ctx(options);
        CHECK_EQ(ctx.eval(program, "<test>").toString(), std::string("5120000 59068 62572 64140 64892 65288 65547 65536"));
    }
}

TEST(recursionThatHoldsReadsWithoutCountsKeepsAParameterAGetterDrops)
{
    // The node a frame was given, held without a count all the way up, is dropped by a getter of its own: the frame goes on with it.
    const char* program =
        "function T(l,r){this.l=l;this.r=r;}"
        "function mk(d){return d===0?new T(null,null):new T(mk(d-1),mk(d-1));}"
        "function chk(t){return t.l===null?1:1+chk(t.l)+chk(t.r);}"
        "var s=0; for (var i=0;i<20000;i++) s+=chk(mk(4));"
        "var res=[], keep=[];"
        "for (var depth=1; depth<8; depth++) {"
        "  var b=mk(9), parent=b; for (var k=0;k<depth;k++) parent=parent.l;"
        "  var P=parent.l, once=true;"
        "  Object.defineProperty(P,'l',{get:function(){ if(once){once=false; parent.l=null; P=null;} for(var q=0;q<50;q++) keep.push(new T('x','x')); return mk(1);}});"
        "  P=null;"
        "  res.push(chk(b));"
        "}"
        "s + \" \" + res.join(\" \")";
    for (int mode = 0; mode < 3; ++mode) {
        script::Context::Options options;
        if (mode == 0)
            options.jit.mode = script::JitOptions::Mode::Off;
        else if (mode == 1)
            options.jit.threshold = 1;
        else
            options.jit.mode = script::JitOptions::Mode::Eager;
        js::Context ctx(options);
        CHECK_EQ(ctx.eval(program, "<test>").toString(), std::string("620000 899 963 995 1011 1019 1023 1025"));
    }
}

TEST(newOfAGlobalConstructorReadBeforeItsArgumentsKeepsWhatWasRead)
{
    // A recursive function whose `new` reads its constructor from a global, then calls itself for the arguments: the read is
    // left out while nothing else can run; a `valueOf` met in the arguments replaces the global, and the `new` still uses the old.
    const char* program =
        "function T(l,r){this.l=l;this.r=r;}"
        "function U(l,r){this.l=l;this.r=r;this.u=1;}"
        "function mk(d){return d===0?new T(null,null):new T(mk(d-1),mk(d-1));}"
        "function count(t){return t.l===null?1:1+count(t.l)+count(t.r);}"
        "var s=0; for (var i=0;i<3000;i++) s+=count(mk(4));"
        "var Old=T, calls=0;"
        "var o={valueOf:function(){ calls++; if(calls===1) T=U; return 3; }};"
        "var t=mk(o);"
        "var out=[s, t instanceof Old, t.l instanceof U, t.r instanceof U, count(t), calls];"
        "T=Old; out.push(count(mk(5)), mk(2) instanceof Old);"
        "out.join(' ')";
    for (int mode = 0; mode < 3; ++mode) {
        script::Context::Options options;
        if (mode == 0)
            options.jit.mode = script::JitOptions::Mode::Off;
        else if (mode == 1)
            options.jit.threshold = 1;
        else
            options.jit.mode = script::JitOptions::Mode::Eager;
        js::Context ctx(options);
        CHECK_EQ(ctx.eval(program, "<test>").toString(), std::string("93000 true true true 15 2 63 true"));
    }
}

TEST(aDoubleReadFromAFieldTellsAnInt32AndWhatIsNoNumber)
{
    // Fields read as doubles in a loop: then an int32 in one (converted out of line), the largest int32, NaN, a string, undefined.
    const char* program =
        "function P(x,y){this.x=x;this.y=y;}"
        "function k(ps,n){var s=0.5;for(var i=0;i<n;i++){var p=ps[i&3];s+=p.x*0.5+p.y;}return s;}"
        "var ps=[new P(1.5,2.25),new P(3.5,0.125),new P(5.25,1.5),new P(7.5,2.5)];"
        "var r=0; for (var m=0;m<300;m++) r=k(ps,400);"
        "var out=[r];"
        "ps[1].x=3; out.push(k(ps,40));"
        "ps[2].y=-7; ps[3].x=2147483647; out.push(k(ps,40));"
        "ps[0].x=NaN; out.push(String(k(ps,8)));"
        "ps[0].x='s'; out.push(String(k(ps,8)));"
        "ps[0].x=undefined; out.push(String(k(ps,8)));"
        "out.join(' ')";
    for (int mode = 0; mode < 3; ++mode) {
        script::Context::Options options;
        if (mode == 0)
            options.jit.mode = script::JitOptions::Mode::Off;
        else if (mode == 1)
            options.jit.threshold = 1;
        else
            options.jit.mode = script::JitOptions::Mode::Eager;
        js::Context ctx(options);
        CHECK_EQ(ctx.eval(program, "<test>").toString(), std::string("1525.5 150.5 10737418263 NaN NaN NaN"));
    }
}

TEST(aCounterBelowAnInt32StepsWithoutAnOverflowCheck)
{
    // A loop counter just found below an int32 steps by one with no overflow test; below or at the largest int32, below a
    // double past it, or stepped in the body too, it still turns into a double.
    const char* program =
        "function f(n){var c=0;for(var j=2147483640;j<n;j++)c++;return c;}"
        "function g(n){var c=0;for(var j=2147483640;j<=n;j++){c++;if(c>20)break;}return c+':'+j;}"
        "function h(n){var c=0;for(var j=2147483640;j<n;j++){c++;if(c>20)break;}return c+':'+j;}"
        "function w(n){var c=0;for(var j=2147483630;j<n;j++){c++;j+=3;if(c>20)break;}return c+':'+j;}"
        "function z(n){var c=0,j=0;while(j<n){j++;c+=j&1;}return c+':'+j;}"
        "var r=[]; for (var m=0;m<3000;m++){ r[0]=f(2147483647); r[1]=g(2147483647); r[2]=h(2147483648.5); r[3]=w(2147483647); r[4]=z(100); }"
        "r.push(f(10), g(10), h(-5), w(2147483647.5), z(2147483647 > 1 ? 7 : 0));"
        "r.join(' ')";
    for (int mode = 0; mode < 3; ++mode) {
        script::Context::Options options;
        if (mode == 0)
            options.jit.mode = script::JitOptions::Mode::Off;
        else if (mode == 1)
            options.jit.threshold = 1;
        else
            options.jit.mode = script::JitOptions::Mode::Eager;
        js::Context ctx(options);
        CHECK_EQ(ctx.eval(program, "<test>").toString(), std::string("7 8:2147483648 9:2147483649 5:2147483650 50:100 0 0:2147483640 0:2147483640 5:2147483650 4:7"));
    }
}

TEST(aMethodReadOfAnInlinedCallInAChainStillFillsItsReceiver)
{
    // `this.a.b.get()` after a join: the read of the receiver both ends a chain of reads and fills the inlined call's
    // `this`; it must still put the receiver where the inlined body reads it.
    const char* program =
        "function Rep(n){this.captured=[];for(var i=0;i<n;i++)this.captured.push(i);}"
        "Rep.prototype.get=function(){return this.captured;};"
        "function St(c){this.checker=c;this.h=false;}"
        "St.prototype.f=function(tc){this.h=this.h||(tc.p&&(this.checker.rep.get().length));"
        "return this.checker.rep.captured.length;};"
        "var s=0,t=0,sts=[];"
        "for(var r=0;r<200;r++){var st=new St({rep:new Rep(r%3)});sts.push(st);"
        "for(var k=0;k<50;k++)s+=st.f({p:k&1});t+=st.h?1:0;}"
        "s+' '+t+' '+sts.length";
    for (int mode = 0; mode < 3; ++mode) {
        script::Context::Options options;
        if (mode == 0)
            options.jit.mode = script::JitOptions::Mode::Off;
        else if (mode == 1)
            options.jit.threshold = 1;
        else
            options.jit.mode = script::JitOptions::Mode::Eager;
        js::Context ctx(options);
        CHECK_EQ(ctx.eval(program, "<test>").toString(), std::string("9950 133 200"));
    }
}
