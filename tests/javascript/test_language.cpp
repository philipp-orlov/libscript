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
    // A `const` is enforced even for a closure compiled above its declaration.
    js::Context ctx;
    bool threw = false;
    try {
        ctx.eval("function f() { const g = () => { c = 2; }; const c = 1; g(); } f()", "<test>");
    } catch (const js::Error& e) {
        threw = e.code() == js::ErrorCode::SyntaxError;
    }
    CHECK(threw);
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
    } catch (const js::Exception& e) {
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
    CHECK(messageOf("class A {} class B extends A { constructor() { var f = () => super(); f(); } } new B()")
              .find("super") != std::string::npos);
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
