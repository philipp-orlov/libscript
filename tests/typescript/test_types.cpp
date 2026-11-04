// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The type checker: what it accepts, what it reports, and what the erased
// program does afterwards.

#include "lang.hpp"
#include "tests/test_util.hpp"

namespace {

/// Type-check `source` and return the first diagnostic, or "" when it is clean.
std::string firstError(const char* source, bool strict = false)
{
    ts::Context::Options options;
    if (strict)
        options.check = ts::CheckOptions::strict();
    ts::Context ctx(options);
    const std::vector<ts::Diagnostic> diagnostics = ctx.diagnose(source, "<t>");
    return diagnostics.empty() ? std::string() : diagnostics.front().message;
}

void wellTyped(const char* source)
{
    const std::string error = firstError(source);
    if (!error.empty())
        ::tst::fail(__FILE__, __LINE__, "unexpected error: " + error + "\n  in: " + source);
}

/// Assert that `source` is rejected, and that the message mentions `because`.
void illTyped(const char* source, const char* because)
{
    const std::string error = firstError(source);
    if (error.empty())
        ::tst::fail(__FILE__, __LINE__, std::string("expected a type error from: ") + source);
    if (error.find(because) == std::string::npos)
        ::tst::fail(__FILE__, __LINE__,
                    "wrong error: " + error + "\n  expected it to mention: " + because);
}

std::string run(const char* source)
{
    ts::Context ctx;
    try {
        return ctx.eval(source, "<t>").toString();
    } catch (const ts::Error& e) {
        // Copy the message out while the Context that owns the thrown value is
        // still alive.
        throw std::runtime_error(e.what());
    }
}

}  // namespace

TEST(primitivesAndLiterals)
{
    wellTyped("const a: number = 1; const b: string = 'x'; const c: boolean = true;");
    wellTyped("let n: number | string = 1; n = 'x';");
    wellTyped("const t: [number, string] = [1, 'a'];");
    wellTyped("const r: readonly number[] = [1, 2];");

    illTyped("const a: number = 'x';", "not assignable");
    illTyped("const t: [number, string] = [1, 2];", "not assignable");
    illTyped("let n: 'a' | 'b' = 'c';", "not assignable");
    illTyped("const a: number = 1; a.toUpperCase();", "does not exist");
}

TEST(literalsWidenWhereTheyMust)
{
    // `let x = 1` is a number, or nothing could ever be assigned to it; the
    // same value behind `as const` keeps its literal type.
    wellTyped("let x = 1; x = 2;");
    wellTyped("const xs = [1, 2]; xs.push(3);");
    wellTyped("const o = { a: 1 }; o.a = 2;");
    illTyped("const p = { a: 1 } as const; p.a = 2;", "not assignable");
}

TEST(objectsAreComparedStructurally)
{
    wellTyped(
        "interface P { x: number; y: number }\n"
        "const p: P = { x: 1, y: 2 };\n"
        "function len(q: { x: number; y: number }): number { return q.x + q.y; }\n"
        "len(p);");
    wellTyped(
        "interface A { x: number } interface B extends A { y: number }\n"
        "const b: B = { x: 1, y: 2 }; const a: A = b;");
    illTyped("interface P { x: number } const p: P = { x: 1, z: 2 };", "does not exist in type");
    illTyped("interface P { x: number } const p: P = {};", "not assignable");
    wellTyped("interface P { x?: number } const p: P = {};");
    wellTyped("const bag: { [key: string]: number } = { a: 1, b: 2 }; bag.c = 3;");
}

TEST(functionsCheckTheirArgumentsAndReturns)
{
    wellTyped("function add(a: number, b: number): number { return a + b; } add(1, 2);");
    wellTyped(
        "function greet(name: string, punct = '!'): string { return name + punct; }\n"
        "greet('a'); greet('a', '?');");
    wellTyped("function sum(...n: number[]): number { return n.length; } sum(1, 2, 3);");

    illTyped("function add(a: number, b: number): number { return a + b; } add(1);",
             "expected 2 argument");
    illTyped("function add(a: number, b: number): number { return a + b; } add(1, 2, 3);",
             "at most 2 argument");
    illTyped("function add(a: number, b: number): number { return a + b; } add(1, 'x');",
             "argument 2");
    illTyped("function f(): number { return 'x'; }", "return type mismatch");
    illTyped("const n: number = 1; n();", "not callable");
}

TEST(returnTypesAreInferred)
{
    wellTyped("function f() { return 1; } const n: number = f();");
    wellTyped("function f(b: boolean) { return b ? 1 : 'x'; } const v: number | string = f(true);");
    wellTyped("function f() {} const v: void = f();");
    illTyped("function f() { return 1; } const s: string = f();", "not assignable");
    // A function may be used above its own declaration, and may call itself.
    wellTyped(
        "const n: number = fact(3);\n"
        "function fact(k: number): number { return k <= 1 ? 1 : k * fact(k - 1); }");
}

TEST(genericsInferFromArguments)
{
    wellTyped("function id<T>(x: T): T { return x; } const n: number = id(1);");
    wellTyped("function id<T>(x: T): T { return x; } const s: string = id<string>('x');");
    wellTyped("function first<T>(xs: T[]): T { return xs[0]; } const n: number = first([1, 2]);");
    wellTyped("const doubled: number[] = [1, 2].map(x => x * 2);");
    wellTyped("const joined: string = ['a', 'b'].filter(s => s > 'a').join('');");
    illTyped("function id<T>(x: T): T { return x; } const s: string = id(1);", "not assignable");
    illTyped("function take<T extends string>(x: T): T { return x; } take(1);",
             "type argument mismatch");
}

TEST(narrowingFollowsControlFlow)
{
    wellTyped(
        "function f(v: string | number): number {\n"
        "    if (typeof v === 'string') return v.length;\n"
        "    return v;\n"
        "}");
    wellTyped(
        "function f(v: string | null): number {\n"
        "    if (!v) return 0;\n"
        "    return v.length;\n"
        "}");
    wellTyped(
        "type S = { kind: 'a'; a: number } | { kind: 'b'; b: string };\n"
        "function f(s: S): string {\n"
        "    if (s.kind === 'a') return String(s.a);\n"
        "    return s.b;\n"
        "}");
    wellTyped(
        "function isNum(x: unknown): x is number { return typeof x === 'number'; }\n"
        "function f(x: unknown): number { return isNum(x) ? x + 1 : 0; }");
    illTyped("function f(v: string | number): number { return v.length; }", "does not exist");
    illTyped(
        "type S = { kind: 'a'; a: number } | { kind: 'b'; b: string };\n"
        "function f(s: S): string { if (s.kind === 'a') return s.b; return s.b; }",
        "does not exist");
}

TEST(strictNullChecksAreOptional)
{
    // Off by default, exactly as `tsc` behaves without the flag.
    wellTyped("const s: string = null;");
    CHECK(!firstError("const s: string = null;", true).empty());
    CHECK(firstError("const s: string | null = null; if (s) s.length;", true).empty());
    CHECK(!firstError("function f(s: string | null) { return s.length; }", true).empty());
    CHECK(firstError("function f(s: string | null) { return s!.length; }", true).empty());
    CHECK(!firstError("function f(a) { return a; }", true).empty());
}

TEST(classesCheckTheirMembers)
{
    wellTyped(
        "class C {\n"
        "    private n = 0;\n"
        "    constructor(public label: string) {}\n"
        "    bump(by: number): number { return this.n += by; }\n"
        "}\n"
        "const c = new C('x'); c.bump(1); const s: string = c.label;");
    wellTyped(
        "interface Named { name(): string }\n"
        "class C implements Named { name(): string { return 'c'; } }");
    wellTyped(
        "class A { go(): number { return 1; } }\n"
        "class B extends A { extra(): number { return this.go() + 1; } }\n"
        "const n: number = new B().extra();");
    illTyped("class C { n = 0 } new C().missing;", "does not exist");
    illTyped("class C { constructor(public n: number) {} } new C('x');", "argument 1");
    illTyped(
        "interface Named { name(): string }\n"
        "class C implements Named { label(): string { return 'c'; } }",
        "does not implement");
}

TEST(enumsAreTypesAndValues)
{
    wellTyped("enum Color { Red, Green, Blue } const c: Color = Color.Green;");
    wellTyped("enum Mode { Read = 'r', Write = 'w' } const m: Mode = Mode.Read;");
    illTyped("enum Color { Red, Green } const c: Color = 'Red';", "not assignable");

    CHECK_EQ(run("enum Color { Red, Green = 5, Blue } `${Color.Red}${Color.Green}${Color.Blue}`"),
             std::string("056"));
    // A numeric enum maps values back to names; a string enum does not.
    CHECK_EQ(run("enum Color { Red, Green = 5 } Color[5]"), std::string("Green"));
    CHECK_EQ(run("enum Mode { Read = 'r' } `${Mode.Read}${(Mode as any)['r'] === undefined}`"),
             std::string("rtrue"));
    // An initialiser runs exactly once even though the value is stored twice.
    CHECK_EQ(run("var n = 0; function next() { return ++n; }\n"
                 "enum E { A = next(), B = next() } `${E.A}${E.B}${n}`"),
             std::string("122"));
}

TEST(namespacesBecomeObjects)
{
    wellTyped("namespace N { export const x = 1; }");
    CHECK_EQ(run("namespace N { export const x = 1; export function f() { return x + 1; } }\n"
                 "`${N.x}${N.f()}`"),
             std::string("12"));
    CHECK_EQ(run("namespace A.B { export const v = 7; } `${A.B.v}`"), std::string("7"));
    CHECK_EQ(run("namespace N { const hidden = 1; export const seen = hidden + 1; }\n"
                 "`${N.seen}${typeof (N as any).hidden}`"),
             std::string("2undefined"));
}

TEST(assertionsAndSatisfies)
{
    wellTyped("const n = ('x' as unknown) as number;");
    wellTyped("const o = { a: 1 } satisfies { a: number };");
    illTyped("const n = 'x' as number;", "can never hold");
    illTyped("const o = { a: 1 } satisfies { b: number };", "does not satisfy");
    // `satisfies` checks but does not change the type it reports.
    wellTyped("const o = { a: 1 } satisfies { a: number }; const n: number = o.a;");
}

TEST(typesDisappearBeforeExecution)
{
    // Everything below has run-time meaning only where TypeScript says it does.
    CHECK_EQ(run("interface I { x: number }\n"
                 "type T = I;\n"
                 "declare const absent: number;\n"
                 "const v: T = { x: 41 };\n"
                 "(v.x as number) + 1"),
             std::string("42"));
    CHECK_EQ(run("class P { constructor(private a: number, public b: number) {} "
                 "sum(): number { return this.a + this.b; } }\n"
                 "const p = new P(1, 2); `${p.sum()}${p.b}`"),
             std::string("32"));
    CHECK_EQ(run("class Q { constructor(public tag: string) {} }\n"
                 "class R extends Q { constructor(public n: number) { super('r'); } }\n"
                 "const r = new R(1); `${r.tag}${r.n}`"),
             std::string("r1"));
    CHECK_EQ(run("abstract class A { abstract go(): number; twice(): number { return 2; } }\n"
                 "class B extends A { go(): number { return 1; } }\n"
                 "`${new B().go()}${new B().twice()}`"),
             std::string("12"));
}

TEST(checkingCanBeTurnedOff)
{
    ts::Context::Options options;
    options.check.enabled = false;
    ts::Context ctx(options);
    // Nonsense to the checker, perfectly good JavaScript to the engine.
    CHECK_EQ(ctx.eval("const o = {}; o.x = 1; o.x + 1").toDouble(), 2.0);
}

TEST(diagnosticsCarryFileAndLine)
{
    ts::Context ctx;
    const std::vector<ts::Diagnostic> diagnostics =
        ctx.diagnose("const a: number = 1;\nconst b: string = a;", "sample.ts");
    CHECK_EQ(diagnostics.size(), std::size_t(1));
    CHECK_EQ(diagnostics.front().line, 2u);
    CHECK_EQ(diagnostics.front().file, std::string("sample.ts"));
    CHECK(diagnostics.front().toString().rfind("sample.ts(2)", 0) == 0);

    bool threw = false;
    try {
        ctx.eval("const n: number = 'x';", "bad.ts");
    } catch (const ts::CompileError& e) {
        threw = true;
        CHECK_EQ(e.code(), ts::ErrorCode::CompileError);
        CHECK_EQ(e.diagnostics().size(), std::size_t(1));
    }
    CHECK(threw);
}

TEST(hostGlobalsAreVisibleToTheChecker)
{
    ts::Context ctx;
    ctx.global().set("hostValue", ctx.fn("hostValue", [] { return 7; }));
    // The checker cannot read a C++ signature, so a host name is `any` -- but
    // it does exist, and using it is not an error.
    CHECK_EQ(ctx.eval("hostValue() + 1").toDouble(), 8.0);
    CHECK(!ctx.diagnose("notInstalled()").empty());
}

TEST(theCheckerSurvivesPathologicalInput)
{
    // Recursive types must not make assignability or printing diverge.
    wellTyped(
        "interface Node { next: Node; value: number }\n"
        "function walk(n: Node): number { return n.next ? walk(n.next) : n.value; }");
    wellTyped(
        "interface A { b: B } interface B { a: A }\n"
        "function f(a: A): B { return a.b; } function g(b: B): A { return b.a; }");
    wellTyped(
        "type Deep = { a: { b: { c: { d: number } } } };\n"
        "const d: Deep = { a: { b: { c: { d: 1 } } } };");
}

TEST(ambientDeclarationsDescribeTheHost)
{
    ts::Context ctx;
    ctx.global().set("now", ctx.fn("now", [] { return 1.0; }));
    // Without a declaration the host name exists but has no type.
    CHECK(ctx.diagnose("now().toFixed(2)").empty());
    ctx.declare(
        "declare function now(): number;\n"
        "interface Clock { readonly ticks: number }\n"
        "declare const clock: Clock;");
    CHECK(ctx.diagnose("now().toFixed(2)").empty());
    CHECK(!ctx.diagnose("now().toUpperCase()").empty());
    CHECK(!ctx.diagnose("const n: number = clock.tick;").empty());
    CHECK(ctx.diagnose("const n: number = clock.ticks;").empty());
    // Declarations accumulate and outlive each evaluation.
    ctx.declare("declare const extra: string;");
    CHECK(ctx.diagnose("extra.length + clock.ticks").empty());
}

TEST(contextualTypesFlowIntoLiterals)
{
    wellTyped(
        "type S = { kind: 'a' } | { kind: 'b' };\n"
        "const all: S[] = [{ kind: 'a' }, { kind: 'b' }];");
    wellTyped(
        "function f(cb: (n: number) => string): string { return cb(1); }\n"
        "f(n => n.toFixed(2));");
    wellTyped("const pair: [string, number] = ['a', 1];");
    illTyped(
        "type S = { kind: 'a' } | { kind: 'b' };\n"
        "const all: S[] = [{ kind: 'c' }];",
        "not assignable");
}

TEST(iterationKnowsWhatItYields)
{
    wellTyped("for (const c of 'abc') { const s: string = c; }");
    wellTyped("for (const n of [1, 2]) { const v: number = n; }");
    wellTyped(
        "const m = new Map<string, number>();\n"
        "for (const [k, v] of m) { const a: string = k; const b: number = v; }");
    wellTyped("const s = new Set<number>(); s.add(1); for (const n of s) { const v: number = n; }");
    illTyped("for (const n of [1, 2]) { const v: string = n; }", "not assignable");
}
