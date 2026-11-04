// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The checker: what compiles, what does not, and what the message says.
#include <memory>

#include "script/script.hpp"
#include "tests/test_util.hpp"

namespace {

std::string firstError(const char* source)
{
    script::Context ctx;
    try {
        script::Assembly assembly;
        const std::vector<script::Diagnostic> diagnostics = ctx.diagnose(source, "<test>", &assembly);
        return diagnostics.empty() ? "" : diagnostics.front().message;
    } catch (const script::Error& error) {
        return error.what();
    }
}

std::uint32_t firstCode(const char* source)
{
    script::Context ctx;
    script::Assembly assembly;
    const std::vector<script::Diagnostic> diagnostics = ctx.diagnose(source, "<test>", &assembly);
    return diagnostics.empty() ? 0 : diagnostics.front().code;
}

void compiles(const char* source)
{
    const std::string error = firstError(source);
    if (!error.empty())
        ::tst::fail(__FILE__, __LINE__, "expected it to compile: " + error);
}

void rejects(const char* source)
{
    if (firstError(source).empty())
        ::tst::fail(__FILE__, __LINE__, std::string("expected an error from: ") + source);
}

}  // namespace

TEST(assignability)
{
    compiles("int a = 1; long b = a; double c = b; object o = c;");
    compiles("byte b = 200;");
    compiles("string s = null; object o = s;");
    rejects("int a = 1.5;");
    rejects("int a = 300L;");
    rejects("byte b = 300;");
    rejects("string s = 1;");
    rejects("int a = null;");
    compiles("int a = (int)1.5; long b = 300L; byte c = (byte)300;");
}

TEST(undefinedNames)
{
    CHECK_EQ(firstCode("return missing;"), 103u);
    CHECK_EQ(firstCode("Missing.Call();"), 103u);
    CHECK_EQ(firstCode("class A { } var a = new A(); a.Missing();"), 117u);
    CHECK_EQ(firstCode("UnknownType x = null;"), 246u);
}

TEST(operatorTypes)
{
    compiles("var a = 1 + 1.5;");
    compiles("var a = \"x\" + 1;");
    compiles("bool b = true && false;");
    rejects("bool b = 1 && 2;");
    rejects("var a = true + true;");
    rejects("var a = \"x\" - \"y\";");
    rejects("var a = 1.5 & 2.5;");
    rejects("if (1) { }");
}

TEST(methodResolution)
{
    compiles("class A { void F(int n) { } void F(string s) { } void G() { F(1); F(\"x\"); } }");
    rejects("class A { void F(int n) { } void G() { F(); } }");
    rejects("class A { void F(int n) { } void G() { F(1, 2); } }");
    rejects("class A { void F(int n) { } void G() { F(true); } }");
    compiles("class A { static void F() { } static void G() { F(); } }");
    rejects("class A { void F() { } static void G() { F(); } }");
}

TEST(returns)
{
    compiles("class A { int F() { return 1; } }");
    rejects("class A { int F() { return \"x\"; } }");
    rejects("class A { void F() { return 1; } }");
    rejects("class A { int F() { return; } }");
}

TEST(inheritanceRules)
{
    compiles("class A { public virtual int F() => 1; } class B : A { public override int F() => 2; }");
    rejects("class A { public int F() => 1; } class B : A { public override int F() => 2; }");
    rejects("sealed class A { } class B : A { }");
    compiles("interface I { void F(); } class A : I { public void F() { } }");
    rejects("interface I { void F(); } class A : I { }");
    rejects("interface I { void F(); } class A : I { public void F(int n) { } }");
    rejects("interface I { int P { get; } } class A : I { }");
}

TEST(assignmentTargets)
{
    compiles("int a = 1; a = 2;");
    compiles("class A { public int X; } var a = new A(); a.X = 1;");
    rejects("1 = 2;");
    rejects("const int a = 1; a = 2;");
    rejects("class A { public int X => 1; } var a = new A(); a.X = 2;");
}

TEST(constantsMustBeConstant)
{
    compiles("const int a = 1 + 2 * 3;");
    compiles("class A { const string S = \"x\"; }");
    rejects("class A { static int F() => 1; const int N = 2; }  const int M = 1 / 0;");
}

TEST(flowRules)
{
    rejects("break;");
    rejects("continue;");
    compiles("while (true) { break; }");
    compiles("switch (1) { case 1: break; }");
    rejects("switch (1) { case 1: case 1.5: break; }");
}

TEST(lambdaTyping)
{
    compiles("System.Func<int, int> f = x => x + 1;");
    rejects("System.Func<int, int> f = x => \"text\";");
    rejects("System.Func<int, int> f = (x, y) => x;");
    rejects("var f = x => x;");
}

TEST(diagnosticsCarryPosition)
{
    script::Context ctx;
    script::Assembly assembly;
    const std::vector<script::Diagnostic> diagnostics =
        ctx.diagnose("int a = 1;\nint b = \"text\";", "prog.cs", &assembly);
    CHECK_EQ(diagnostics.size(), std::size_t(1));
    CHECK_EQ(diagnostics[0].line, 2u);
    CHECK_EQ(diagnostics[0].file, std::string("prog.cs"));
    CHECK(diagnostics[0].toString().find("prog.cs(2,") == 0);
}

TEST(compileErrorCarriesEverything)
{
    script::Context ctx;
    bool caught = false;
    try {
        ctx.compile("int a = \"x\";\nint b = \"y\";", "two.cs");
    } catch (const script::CompileError& error) {
        caught = true;
        CHECK_EQ(error.diagnostics().size(), std::size_t(2));
    }
    CHECK(caught);
}
