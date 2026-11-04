// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// What the parser accepts and what it refuses.
#include <memory>

#include "script/script.hpp"
#include "tests/test_util.hpp"

namespace {

void parses(const char* source)
{
    script::Context ctx;
    script::Assembly assembly;
    const std::vector<script::Diagnostic> diagnostics = ctx.diagnose(source, "<test>", &assembly);
    if (!diagnostics.empty())
        ::tst::fail(__FILE__, __LINE__, "unexpected diagnostic: " + diagnostics.front().message);
}

std::string syntaxError(const char* source)
{
    script::Context ctx;
    try {
        script::Assembly assembly;
        ctx.diagnose(source, "<test>", &assembly);
    } catch (const script::Error& error) {
        return error.what();
    }
    return "";
}

}  // namespace

TEST(declarations)
{
    parses("class A { }");
    parses("public sealed class A : object { }");
    parses("namespace N { class A { } }");
    parses("namespace N.M; class A { }");
    parses("using System; using System.Collections.Generic; class A { }");
    parses("interface I { int F(); int P { get; } }");
    parses("struct S { public int X; public S(int x) { X = x; } }");
    parses("enum E { A, B = 3, C }");
    parses("delegate void Handler(int value);");
    parses("static class C { public const int N = 5; public static readonly string S = \"x\"; }");
}

TEST(members)
{
    parses("class A { int a, b, c; }");
    parses("class A { public int P { get; set; } }");
    parses("class A { public int P { get { return 1; } } }");
    parses("class A { public int P => 42; }");
    parses("class A { public A() { } public A(int n) : this() { } }");
    parses("class A { public virtual void F() { } }");
    parses("class B { public void F(int a, string b = \"x\", params int[] rest) { } }");
    parses("class A { public int F() => 1; }");
}

TEST(statements)
{
    parses("int a = 1; a += 2;");
    parses("for (int i = 0, j = 1; i < j; i++, j--) { }");
    parses("foreach (var x in new int[] { 1 }) { }");
    parses("while (true) break;");
    parses("do { } while (false);");
    parses("try { } catch (System.Exception e) { } finally { }");
    parses("try { } catch { }");
    parses("switch (1) { case 1: break; default: break; }");
    parses("if (true) { } else if (false) { } else { }");
    parses("{ int shadowed = 1; }");
    parses("var list = new System.Collections.Generic.List<int>();");
}

TEST(expressions)
{
    parses("var a = 1 + 2 * 3 - 4 / 5 % 6;");
    parses("var a = (1 < 2) == (3 > 4);");
    parses("var a = 1 << 2 >> 3 & 4 | 5 ^ 6;");
    parses("var a = true ? 1 : 2;");
    parses("var a = new int[3];");
    parses("var a = new int[] { 1, 2, 3 };");
    parses("int[] a = { 1, 2, 3 };");
    parses("var a = \"x\" + 'y' + 1.5f + 2.5d + 10L + 0x1F + 0b1010 + 1_000;");
    parses("var a = @\"raw \"\" string\";");
    parses("var a = $\"interpolated {1 + 2} and {\"nested\"}\";");
    parses("System.Console.WriteLine(1);");
    parses("var a = (double)1;");
    parses("var a = new System.Exception(\"x\") as System.Exception;");
    parses("var a = default(int);");
    parses("var a = nameof(System);");
    parses("System.Func<int, int, int> f = (int x, int y) => x + y;");
    parses("System.Func<int, int> g = x => x;");
    parses("System.Func<int> h = () => 1;");
    parses("var i = new System.Collections.Generic.List<int> { 1, 2 };");
}

TEST(comments)
{
    parses("// a comment\nint a = 1; /* another\nspanning lines */ a++;");
    parses("#region here\nint a = 1;\n#endregion");
}

TEST(syntaxErrors)
{
    CHECK(!syntaxError("int a = ;").empty());
    CHECK(!syntaxError("class { }").empty());
    CHECK(!syntaxError("int a = 1").empty());
    CHECK(!syntaxError("if (true) { ").empty());
    CHECK(!syntaxError("var s = \"unterminated;").empty());
    CHECK(!syntaxError("var a = 1 +;").empty());
    // The message names the file and the position.
    CHECK(syntaxError("int a = 1").find("<test>(1,") != std::string::npos);
}

TEST(unsupportedConstructsAreRejectedClearly)
{
    CHECK(syntaxError("class A { public static A operator +(A a, A b) => a; }")
              .find("operator") != std::string::npos);
    CHECK(syntaxError("lock (this) { }").find("lock") != std::string::npos);
    CHECK(syntaxError("var d = 1.5m;").find("decimal") != std::string::npos);
}
