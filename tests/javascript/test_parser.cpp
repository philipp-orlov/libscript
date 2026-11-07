// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// What the grammar accepts and, just as importantly, what it rejects.

#include "lang.hpp"
#include "tests/test_util.hpp"

namespace {

void accepts(const char* source)
{
    js::Context ctx;
    ctx.compile(source, "<accepts>");
}

void rejects(const char* source)
{
    js::Context ctx;
    bool threw = false;
    try {
        ctx.compile(source, "<rejects>");
    } catch (const js::Error& e) {
        threw = e.code() == js::ErrorCode::SyntaxError;
    }
    if (!threw)
        ::tst::fail(__FILE__, __LINE__, std::string("expected a syntax error from: ") + source);
}

}  // namespace

TEST(literalsAndOperators)
{
    accepts("1 + 2 * 3 ** 4 % 5");
    accepts("0x1f + 0b1010 + 0o17 + 1_000_000 + 1e-7 + .5");
    accepts("'a' + \"b\" + `c${1 + 2}d`");
    accepts("a ?? b, c?.d, e?.[f], g?.()");
    accepts("x = y ||= z &&= w ?" "?= 1");
    accepts("a < b <= c > d >= e == f != g === h !== i");
    accepts("~a & b | c ^ d << e >> f >>> g");
    accepts("typeof a, void b, delete c.d, !e, -f, +g, ++h, i--");
}

TEST(statements)
{
    accepts("if (a) b(); else { c(); }");
    accepts("for (let i = 0; i < 10; i++) continue;");
    accepts("for (const k in o) {} for (const v of a) {}");
    accepts("while (a) { break; } do { x(); } while (b)");
    accepts("switch (a) { case 1: case 2: b(); break; default: c(); }");
    accepts("try { a(); } catch (e) { b(); } finally { c(); }");
    accepts("try { a(); } catch { b(); }");
    accepts("try { a(); } finally { c(); }");
    accepts("outer: for (;;) { inner: for (;;) { break outer; } }");
    accepts("throw new Error('x')");
}

TEST(functionsAndClasses)
{
    accepts("function f(a, b = 1, ...rest) { return a; }");
    accepts("const g = (a, b) => a + b; const h = x => x; const k = () => ({ a: 1 });");
    accepts("class A { constructor(x) { this.x = x; } get y() { return 1; } set y(v) {} "
            "static make() {} field = 2; static shared = 3; }");
    accepts("class B extends A { constructor() { super(); } m() { return super.m(); } }");
    accepts("({ a, b: 1, [c]: 2, d() {}, get e() { return 1; }, ...f })");
    accepts("const { a, b: { c }, ...rest } = o; const [x, , y = 1, ...z] = arr;");
}

TEST(automaticSemicolons)
{
    accepts("var a = 1\nvar b = 2\na + b");
    accepts("function f() { return\n1 }");
    accepts("let x = 1; { let y = 2 }");
}

TEST(syntaxErrorsAreReported)
{
    rejects("var = 1");
    rejects("function () {}");
    rejects("if (a");
    rejects("class {}");
    rejects("a b c");
    rejects("const x");
    rejects("try { }");
    rejects("/(/g");
    rejects("1n");
}

TEST(errorMessagesCarryPositions)
{
    js::Context ctx;
    try {
        ctx.compile("var a = 1;\nvar b = ;\n", "script.js");
        CHECK(false);
    } catch (const js::Error& e) {
        const std::string what = e.what();
        CHECK(what.find("script.js:2") != std::string::npos);
    }
}
