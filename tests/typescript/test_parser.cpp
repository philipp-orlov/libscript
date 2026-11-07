// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// What the grammar accepts and, just as importantly, what it rejects.
// Types are not the subject here, so checking is off: these sources name
// variables that were never declared, which is the point.

#include "lang.hpp"
#include "tests/test_util.hpp"

namespace {

ts::Context::Options syntaxOnly()
{
    ts::Context::Options options;
    options.check.enabled = false;
    return options;
}

void accepts(const char* source)
{
    ts::Context ctx(syntaxOnly());
    ctx.compile(source, "<accepts>");
}

void rejects(const char* source)
{
    ts::Context ctx(syntaxOnly());
    bool threw = false;
    try {
        ctx.compile(source, "<rejects>");
    } catch (const ts::Error& e) {
        threw = e.code() == ts::ErrorCode::SyntaxError;
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
    accepts(
        "x = y ||= z &&= w ?"
        "?= 1");
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
    accepts(
        "class A { constructor(x) { this.x = x; } get y() { return 1; } set y(v) {} "
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
}

TEST(errorMessagesCarryPositions)
{
    ts::Context ctx;
    try {
        ctx.compile("var a = 1;\nvar b = ;\n", "script.ts");
        CHECK(false);
    } catch (const ts::Error& e) {
        const std::string what = e.what();
        CHECK(what.find("script.ts:2") != std::string::npos);
    }
}

// ---- TypeScript syntax ----

TEST(typeAnnotationsParse)
{
    accepts("let a: number = 1, b: string = 'x', c: boolean = true;");
    accepts("let a: number[] = [], b: Array<string> = [], c: readonly number[] = [];");
    accepts("let t: [number, string, ...boolean[]] = [1, 'a'];");
    accepts("let u: 'a' | 'b' | 1 | true | null = null;");
    accepts("let f: (a: number, b?: string, ...rest: any[]) => void = () => {};");
    accepts("let g: new (a: number) => Date = Date;");
    accepts("let o: { a: number; readonly b?: string; [key: string]: any } = {};");
    accepts("let k: keyof { a: number } = 'a';");
    accepts("let q: typeof globalThis = globalThis;");
}

TEST(typeDeclarationsParse)
{
    accepts("type Id = number; type Pair<A, B = A> = [A, B];");
    accepts("interface A { x: number } interface B extends A { y(): void }");
    accepts("interface C { (a: number): string; new (a: number): C; [k: number]: any }");
    accepts("enum E { A, B = 2, C = 'c' } const enum F { X }");
    accepts("namespace N { export const x = 1; export namespace M { export const y = 2; } }");
    accepts("declare const version: string; declare function f(a: number): void;");
    accepts("declare class D { constructor(a: number); method(): void; }");
}

TEST(typedClassesParse)
{
    accepts("abstract class A<T> { abstract go(): T; protected readonly n: number = 1; }");
    accepts("class B implements Object { private x = 1; static y: number = 2; }");
    accepts("class C { constructor(private readonly a: number, public b = 2) {} }");
}

TEST(typedExpressionsParse)
{
    accepts("const a = (1 as any) as string;");
    accepts("const b = ({ x: 1 }) satisfies { x: number };");
    accepts("const c: number = (null! as number)!;");
    accepts("const d = <T,>(x: T): T => x;");
    accepts("function id<T>(x: T): T { return x; } const e = id<number>(1);");
    accepts("function isNum(x: unknown): x is number { return typeof x === 'number'; }");
}

TEST(typeSyntaxErrorsAreReported)
{
    rejects("let a: = 1");
    rejects("interface A { x }");
    rejects("type = ;");
    rejects("enum { A }");
    rejects("interface A extends { }");
    rejects("type T = A extends B ? C : D;");
}
