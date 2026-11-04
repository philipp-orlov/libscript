// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The language as it stands after C# 6: generics, patterns, records, and the
// syntax that arrived with them.
//
// Every case here runs a program and checks what it produced. The point is not
// that the syntax parses -- it is that the lowering is right, because all of
// this becomes the same handful of instructions the older syntax did.
#include "script/script.hpp"
#include "tests/test_util.hpp"

#include <string>

namespace {

/// Run a program and return what it gave back, as text.
std::string run(const std::string& source)
{
    static std::vector<std::unique_ptr<script::Context>> keep;
    keep.push_back(std::make_unique<script::Context>());
    return keep.back()->eval(source, "<modern>").toString();
}

/// The first diagnostic a program produces, or "" when it compiles.
std::string reject(const std::string& source)
{
    script::Context ctx;
    const std::vector<script::Diagnostic> found = ctx.diagnose(source, "<modern>", nullptr);
    return found.empty() ? std::string() : found.front().message;
}

}  // namespace

// ------------------------------------------------------------- generics ---

TEST(genericTypeIsInstantiatedPerArgument)
{
    CHECK_EQ(run(R"CS(
        class Box<T> { public T V; public Box(T v) { V = v; } }
        var a = new Box<int>(41);
        var b = new Box<string>("hi");
        return (a.V + 1) + b.V;
    )CS"),
             "42hi");
}

TEST(genericTypeArgumentsAreNotErased)
{
    // If T were erased to object, this would box and the addition would fail.
    CHECK_EQ(run(R"CS(
        class Adder<T> { public T A; public T B; }
        var a = new Adder<int>();
        a.A = 20; a.B = 22;
        return a.A + a.B;
    )CS"),
             "42");
}

TEST(genericTypeCanReferToItself)
{
    CHECK_EQ(run(R"CS(
        class Node<T> { public T Value; public Node<T> Next; }
        var head = new Node<int>();
        head.Value = 1;
        head.Next = new Node<int>();
        head.Next.Value = 2;
        return head.Value + head.Next.Value;
    )CS"),
             "3");
}

TEST(genericInterfaceAndImplementation)
{
    CHECK_EQ(run(R"CS(
        interface IStore<T> { void Put(T v); T Get(); }
        class One<T> : IStore<T> {
            private T held;
            public void Put(T v) { held = v; }
            public T Get() { return held; }
        }
        IStore<string> s = new One<string>();
        s.Put("kept");
        return s.Get();
    )CS"),
             "kept");
}

TEST(genericMethodInfersItsTypeArguments)
{
    CHECK_EQ(run(R"CS(
        static class U {
            public static T Pick<T>(bool first, T a, T b) => first ? a : b;
            public static string Join<A, B>(A a, B b) => a + "/" + b;
        }
        return U.Pick(true, 7, 9) + " " + U.Pick(false, "x", "y") + " " + U.Join(1, true);
    )CS"),
             "7 y 1/True");
}

TEST(genericMethodTakesExplicitTypeArguments)
{
    CHECK_EQ(run(R"CS(
        static class U { public static string Name<T>(T v) => "" + v; }
        return U.Name<int>(5) + U.Name<bool>(true);
    )CS"),
             "5True");
}

TEST(genericMethodWorksThroughRefParameters)
{
    CHECK_EQ(run(R"CS(
        static class U { public static void Swap<T>(ref T a, ref T b) { T t = a; a = b; b = t; } }
        int x = 1, y = 2;
        U.Swap(ref x, ref y);
        return "" + x + y;
    )CS"),
             "21");
}

TEST(constraintsAreEnforced)
{
    CHECK(reject(R"CS(
        class OnlyRef<T> where T : class { }
        var bad = new OnlyRef<int>();
    )CS")
              .find("reference type") != std::string::npos);
    CHECK(reject(R"CS(
        class OnlyVal<T> where T : struct { }
        var bad = new OnlyVal<string>();
    )CS")
              .find("value type") != std::string::npos);
}

TEST(nestedGenericArgumentsWork)
{
    CHECK_EQ(run(R"CS(
        using System.Collections.Generic;
        class Pair<K, V> { public K A; public V B; }
        var list = new List<Pair<string, int>>();
        var p = new Pair<string, int>();
        p.A = "n"; p.B = 4;
        list.Add(p);
        return list[0].A + list[0].B;
    )CS"),
             "n4");
}

// ------------------------------------------------------------- patterns ---

TEST(typeAndConstantPatternsMatch)
{
    const std::string program = R"CS(
        static class P {
            public static string F(object o) {
                if (o is null) return "null";
                if (o is int n) return "int " + n;
                if (o is string s) return "str " + s;
                return "other";
            }
        }
        return P.F(null) + "|" + P.F(3) + "|" + P.F("a") + "|" + P.F(true);
    )CS";
    CHECK_EQ(run(program), "null|int 3|str a|other");
}

TEST(relationalAndLogicalPatternsMatch)
{
    CHECK_EQ(run(R"CS(
        static class P {
            public static string F(int n) {
                if (n is < 0) return "neg";
                if (n is 0 or 1) return "bit";
                if (n is > 1 and < 10) return "small";
                if (n is not < 100) return "big";
                return "mid";
            }
        }
        return P.F(-1) + P.F(0) + P.F(1) + P.F(5) + P.F(50) + P.F(500);
    )CS"),
             "negbitbitsmallmidbig");
}

TEST(narrowingFlowsThroughAnAndPattern)
{
    CHECK_EQ(run(R"CS(
        static class P {
            public static string F(object o) => o is int n and > 100 ? "big " + n : "no";
        }
        return P.F(500) + "|" + P.F(5) + "|" + P.F("x");
    )CS"),
             "big 500|no|no");
}

TEST(propertyPatternsMatch)
{
    CHECK_EQ(run(R"CS(
        class P { public string Name = ""; public int Age; }
        static class T {
            public static string F(object o) {
                if (o is P { Age: > 17, Name: "ann" }) return "adult ann";
                if (o is P { Age: > 17 } p) return "adult " + p.Name;
                if (o is P { Name: var n }) return "minor " + n;
                return "?";
            }
        }
        var a = new P(); a.Name = "ann"; a.Age = 30;
        var b = new P(); b.Name = "bob"; b.Age = 30;
        var c = new P(); c.Name = "kid"; c.Age = 8;
        return T.F(a) + "|" + T.F(b) + "|" + T.F(c) + "|" + T.F(1);
    )CS"),
             "adult ann|adult bob|minor kid|?");
}

TEST(listPatternsMatch)
{
    CHECK_EQ(run(R"CS(
        static class P {
            public static string F(int[] a) => a switch {
                [] => "none",
                [var only] => "one:" + only,
                [var x, var y] => "two:" + x + y,
                [var head, .., var tail] => "many:" + head + tail,
            };
        }
        return P.F(new int[0]) + "|" + P.F(new int[] { 1 }) + "|" +
               P.F(new int[] { 1, 2 }) + "|" + P.F(new int[] { 1, 2, 3, 4 });
    )CS"),
             "none|one:1|two:12|many:14");
}

TEST(positionalPatternsUseDeconstruct)
{
    CHECK_EQ(run(R"CS(
        record Point(int X, int Y);
        static class P {
            public static string F(Point p) => p switch {
                (0, 0) => "origin",
                (var x, 0) => "on x at " + x,
                (0, var y) => "on y at " + y,
                (var x, var y) => x + "," + y,
            };
        }
        return P.F(new Point(0,0)) + "|" + P.F(new Point(3,0)) + "|" +
               P.F(new Point(0,4)) + "|" + P.F(new Point(1,2));
    )CS"),
             "origin|on x at 3|on y at 4|1,2");
}

TEST(switchStatementTakesPatternsAndGuards)
{
    CHECK_EQ(run(R"CS(
        class A { }
        class B : A { public int N; }
        static class P {
            public static string F(A a) {
                switch (a) {
                    case B b when b.N > 10: return "big b";
                    case B: return "b";
                    case null: return "null";
                    default: return "a";
                }
            }
        }
        var small = new B(); var big = new B(); big.N = 20;
        return P.F(small) + "|" + P.F(big) + "|" + P.F(null) + "|" + P.F(new A());
    )CS"),
             "b|big b|null|a");
}

TEST(switchStatementStillTakesConstantsAndBreaks)
{
    CHECK_EQ(run(R"CS(
        enum Color { Red, Green, Blue }
        static class P {
            public static string F(Color c) {
                string r = "";
                switch (c) {
                    case Color.Red: r = "r"; break;
                    case Color.Green: r = "g"; break;
                    default: r = "?"; break;
                }
                return r;
            }
        }
        return P.F(Color.Red) + P.F(Color.Green) + P.F(Color.Blue);
    )CS"),
             "rg?");
}

TEST(switchExpressionThrowsWhenNothingMatches)
{
    bool threw = false;
    try {
        run(R"CS(
            static class P { public static int F(int n) => n switch { 1 => 1, 2 => 2 }; }
            return P.F(3);
        )CS");
    } catch (const script::Exception&) {
        threw = true;
    }
    CHECK(threw);
}

TEST(patternSubjectIsEvaluatedOnce)
{
    CHECK_EQ(run(R"CS(
        static class P {
            public static int Calls = 0;
            public static int Next() { Calls++; return 7; }
        }
        var band = P.Next() switch { < 5 => "low", _ => "high" };
        return band + P.Calls;
    )CS"),
             "high1");
}

// --------------------------------------------------------------- tuples ---

TEST(tuplesCarryValuesAndNames)
{
    CHECK_EQ(run(R"CS(
        var t = (1, "a");
        var named = (x: 3, y: 4);
        return t.Item1 + t.Item2 + " " + named.x + named.y + " " + named.Item1;
    )CS"),
             "1a 34 3");
}

TEST(tuplePrintsAndComparesByValue)
{
    CHECK_EQ(run(R"CS(
        var a = (1, "x");
        var b = (1, "x");
        return a + " " + a.Equals(b);
    )CS"),
             "(1, x) True");
}

TEST(tupleTypeCanBeWrittenOut)
{
    CHECK_EQ(run(R"CS(
        static class P { public static (int, string) F() => (7, "s"); }
        (int n, string s) r = P.F();
        return r.n + r.s;
    )CS"),
             "7s");
}

TEST(deconstructionTakesTuplesApart)
{
    CHECK_EQ(run(R"CS(
        var (a, b) = (1, "x");
        (int c, string d) = (2, "y");
        return "" + a + b + c + d;
    )CS"),
             "1x2y");
}

TEST(deconstructionUsesADeconstructMethod)
{
    CHECK_EQ(run(R"CS(
        class P {
            public int X = 1; public int Y = 2;
            public void Deconstruct(out int x, out int y) { x = X; y = Y; }
        }
        var (x, y) = new P();
        return "" + x + y;
    )CS"),
             "12");
}

TEST(deconstructionDiscardsWhatItDoesNotWant)
{
    CHECK_EQ(run(R"CS(
        var (a, _) = (1, 2);
        return a;
    )CS"),
             "1");
}

// --------------------------------------------------------------- records ---

TEST(recordGetsItsMembers)
{
    CHECK_EQ(run(R"CS(
        record Money(string Currency, double Amount);
        var m = new Money("EUR", 2.5);
        var (c, a) = m;
        return m + " " + c + a;
    )CS"),
             "Money { Currency = EUR, Amount = 2.5 } EUR2.5");
}

TEST(recordsCompareByValue)
{
    CHECK_EQ(run(R"CS(
        record P(int X, int Y);
        var a = new P(1, 2);
        var b = new P(1, 2);
        var c = new P(1, 3);
        return a.Equals(b) + "" + a.Equals(c) + (a.GetHashCode() == b.GetHashCode());
    )CS"),
             "TrueFalseTrue");
}

TEST(withCopiesAndChanges)
{
    CHECK_EQ(run(R"CS(
        record P(int X, int Y);
        var a = new P(1, 2);
        var b = a with { Y = 9 };
        return a + " " + b;
    )CS"),
             "P { X = 1, Y = 2 } P { X = 1, Y = 9 }");
}

TEST(recordCanAddItsOwnMembers)
{
    CHECK_EQ(run(R"CS(
        record P(int X, int Y) {
            public int Sum => X + Y;
            public override string ToString() => "P:" + Sum;
        }
        return new P(1, 2) + "";
    )CS"),
             "P:3");
}

TEST(initOnlyPropertiesCloseAfterConstruction)
{
    CHECK_EQ(run(R"CS(
        class S { public int N { get; init; } public string T { get; init; } }
        var s = new S { N = 1, T = "x" };
        return s.N + s.T;
    )CS"),
             "1x");
    CHECK(reject(R"CS(
        class S { public int N { get; init; } }
        var s = new S { N = 1 };
        s.N = 2;
    )CS")
              .find("init-only") != std::string::npos);
}

TEST(primaryConstructorOnAPlainClass)
{
    CHECK_EQ(run(R"CS(
        class Counter(string label) {
            private int n;
            public void Tick() { n++; }
            public override string ToString() => label + "=" + n;
        }
        var c = new Counter("hits");
        c.Tick(); c.Tick();
        return c + "";
    )CS"),
             "hits=2");
}

// ------------------------------------------------- collections and ranges ---

TEST(collectionExpressionsBuildTheTarget)
{
    CHECK_EQ(run(R"CS(
        using System.Collections.Generic;
        int[] a = [1, 2, 3];
        List<string> b = ["x", "y"];
        return a.Length + "" + a[2] + " " + b.Count + b[1];
    )CS"),
             "33 2y");
}

TEST(indicesCountFromTheEnd)
{
    CHECK_EQ(run(R"CS(
        int[] a = [1, 2, 3, 4];
        return a[^1] + " " + a[^4] + " " + "hello"[^1];
    )CS"),
             "4 1 o");
}

TEST(rangesSlice)
{
    CHECK_EQ(run(R"CS(
        int[] a = [1, 2, 3, 4, 5];
        var m = a[1..4];
        return m.Length + ":" + m[0] + m[1] + m[2] + " " + "hello world"[6..] + " " +
               "hello"[..2];
    )CS"),
             "3:234 world he");
}

TEST(targetTypedNewTakesItsTypeFromContext)
{
    CHECK_EQ(run(R"CS(
        using System.Collections.Generic;
        Dictionary<string, int> d = new();
        d["a"] = 1;
        List<int> l = new();
        l.Add(2);
        return d["a"] + l[0];
    )CS"),
             "3");
}

// --------------------------------------------------------------- strings ---

TEST(rawStringsKeepQuotesAndDropIndentation)
{
    CHECK_EQ(run("var s = \"\"\"\n    {\"a\": 1}\n    line two\n    \"\"\";\nreturn s;"),
             "{\"a\": 1}\nline two");
    CHECK_EQ(run("return \"\"\"one line\"\"\";"), "one line");
}

// ----------------------------------------------------- statements and flow ---

TEST(usingStatementDisposesOnTheWayOut)
{
    CHECK_EQ(run(R"CS(
        class R : System.IDisposable {
            public static string Log = "";
            private string n;
            public R(string name) { n = name; Log += "+" + n; }
            public void Dispose() { Log += "-" + n; }
        }
        using (var a = new R("a")) { R.Log += "!"; }
        return R.Log;
    )CS"),
             "+a!-a");
}

TEST(usingDeclarationDisposesAtTheEndOfItsBlock)
{
    CHECK_EQ(run(R"CS(
        class R : System.IDisposable {
            public static string Log = "";
            private string n;
            public R(string name) { n = name; Log += "+" + n; }
            public void Dispose() { Log += "-" + n; }
        }
        static class P {
            public static void Go() {
                using var a = new R("a");
                R.Log += "!";
            }
        }
        P.Go();
        return R.Log;
    )CS"),
             "+a!-a");
}

TEST(usingDisposesWhenTheBodyThrows)
{
    CHECK_EQ(run(R"CS(
        class R : System.IDisposable {
            public static string Log = "";
            public void Dispose() { Log += "closed"; }
        }
        try { using (var a = new R()) { throw new System.Exception("x"); } }
        catch (System.Exception) { R.Log += "|caught"; }
        return R.Log;
    )CS"),
             "closed|caught");
}

TEST(exceptionFiltersChooseAHandler)
{
    CHECK_EQ(run(R"CS(
        static class P {
            public static string F(int n) {
                try {
                    throw new System.InvalidOperationException("code " + n);
                } catch (System.Exception e) when (n > 5) {
                    return "big: " + e.Message;
                } catch (System.Exception e) {
                    return "small: " + e.Message;
                }
            }
        }
        return P.F(9) + "|" + P.F(1);
    )CS"),
             "big: code 9|small: code 1");
}

TEST(localFunctionsSeeTheirOwnNameAndEachOther)
{
    CHECK_EQ(run(R"CS(
        static class P {
            public static string Go(int n) {
                bool Even(int k) => k == 0 ? true : Odd(k - 1);
                bool Odd(int k) => k == 0 ? false : Even(k - 1);
                int Fact(int k) => k <= 1 ? 1 : k * Fact(k - 1);
                return Even(n) + ":" + Fact(n);
            }
        }
        return P.Go(5);
    )CS"),
             "False:120");
}

TEST(outVarAndDiscardsDeclareAtTheCallSite)
{
    CHECK_EQ(run(R"CS(
        int.TryParse("42", out var ok);
        bool bad = int.TryParse("no", out _);
        return ok + " " + bad;
    )CS"),
             "42 False");
}

TEST(throwWorksAsAnExpression)
{
    CHECK_EQ(run(R"CS(
        static class P {
            public static string F(string s) => s ?? throw new System.Exception("null!");
        }
        try { P.F(null); } catch (System.Exception e) { return e.Message; }
        return "?";
    )CS"),
             "null!");
}
