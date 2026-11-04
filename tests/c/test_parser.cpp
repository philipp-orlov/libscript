// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// What the C grammar accepts, what it refuses, and what it says when it does.

#include "lang.hpp"
#include "tests/test_util.hpp"

namespace {

void accepts(const char* source)
{
    cc::Context ctx;
    ctx.compile(source, "<accepts>");
}

void rejects(const char* source, const char* fragment)
{
    cc::Context ctx;
    try {
        ctx.compile(source, "<rejects>");
    } catch (const cc::Error& e) {
        if (e.code() != cc::ErrorCode::SyntaxError && e.code() != cc::ErrorCode::CompileError)
            ::tst::fail(__FILE__, __LINE__, std::string("wrong error from: ") + source + "\n  " + e.what());
        if (std::string(e.what()).find(fragment) == std::string::npos)
            ::tst::fail(__FILE__, __LINE__,
                        std::string("message '") + e.what() + "' does not mention '" + fragment + "'");
        return;
    }
    ::tst::fail(__FILE__, __LINE__, std::string("expected an error from: ") + source);
}

}  // namespace

TEST(declarationsAndTypes)
{
    accepts("int main(void) { return 0; }");
    accepts("int main() { char c; unsigned char uc; short s; unsigned short us; int i; unsigned u; unsigned int ui; long l; long long ll; unsigned long ul; float f; double d; long double ld; return 0; }");
    accepts("int main() { const int n = 3; static int s = 1; volatile int v = 0; register int r = 1; return n + s + v + r; }");
    accepts("#include <stdint.h>\nint main() { int32_t a = 1; uint8_t b = 2; int64_t c = 3; size_t n = 4; return (int)(a + b + c + n); }");
    accepts("typedef int myint;\ntypedef myint* intptr;\nint main() { myint x = 1; return x; }");
    accepts("struct P { int x; int y; };\ntypedef struct { double a; } Q;\ntypedef struct Node { int v; struct Node* next; } Node;\nint main() { struct P p; Q q; Node n; return 0; }");
    accepts("enum Color { RED, GREEN = 4, BLUE };\nenum { A, B };\nint main() { enum Color c = BLUE; return c + A; }");
    accepts("int f(int);\nint f(int x) { return x; }\nint main() { return f(1); }");
    accepts("int g(void);\nint main() { return g(); }\nint g(void) { return 2; }");
    accepts("int a[3]; int b[2][3]; char s[] = \"hi\"; double d[] = {1, 2.5};\nint main() { return 0; }");
    accepts("int main() { int a[] = {1, 2, 3}, n = 3; return a[n - 1]; }");
}

TEST(preprocessor)
{
    accepts("#include <stdio.h>\n#include \"local.h\"\n#define N 10\n#define TWICE (N * 2)\n#ifndef GUARD\n#define GUARD\n#endif\n#pragma once\nint main() { return TWICE - N; }");
    accepts("#define LONG_MACRO 1 + \\\n    2\nint main() { return LONG_MACRO; }");
    accepts("/* block\n   comment */ int main() { // line comment\n return 0; }");
    rejects("#define SQ(x) ((x) * (x))\nint main() { return SQ(2); }", "function-like macros");
}

TEST(statementsAndExpressions)
{
    accepts("int main() { int i = 0; while (i < 3) i++; do { i--; } while (i > 0); for (;;) break; for (int j = 0, k = 9; j < k; j++, k--) ; return i; }");
    accepts("int main() { int x = 1; if (x) x = 2; else if (!x) x = 3; else { x = 4; } return x > 1 ? x : -x; }");
    accepts("int main() { int x = 2; switch (x) { case 1: return 1; case 2: case 3: x++; break; default: break; } return x; }");
    accepts("int main() { int a = 1; a += 1; a -= 1; a *= 2; a /= 2; a %= 3; a <<= 1; a >>= 1; a &= 1; a |= 2; a ^= 3; ++a; --a; a++; a--; return a; }");
    accepts("int main() { int a = 3, b = 4; return (a & b) | (a ^ b) | ~a | (a << 1) | (a >> 1) | !a | -a | +a; }");
    accepts("int main() { int a = 1; return a == 1 && a != 2 || a < 3 && a <= 3 || a > 0 && a >= 0; }");
    accepts("int main() { double d = (double)3 / 2; int i = (int)d; char c = (char)i; unsigned u = (unsigned)i; return (int)(d + i + c + u); }");
    accepts("int main() { return sizeof(int) + sizeof(double) + sizeof(char) + sizeof(long); }");
    accepts("int main() { int a[4]; return sizeof(a) / sizeof(a[0]); }");
    accepts("int main() { return 0x1F + 017 + 0b101 + 10u + 10L + 10UL + 'a' + '\\n' + '\\x41'; }");
    accepts("int main() { double d = 1.5 + 2e3 + .5 + 1.f; return (int)d; }");
    accepts("int main() { return \"ab\" \"cd\"[3] == 'd'; }");
}

TEST(pointersThatMap)
{
    accepts("void set(int* p) { *p = 1; }\nint main() { int x; set(&x); return x; }");
    accepts("int sum(int* a, int n) { int s = 0; for (int i = 0; i < n; i++) s += a[i]; return s; }\nint main() { int a[3] = {1, 2, 3}; return sum(a, 3) + sum(&a[0], 3); }");
    accepts("struct P { int x; };\nvoid bump(struct P* p) { p->x++; (*p).x++; }\nint main() { struct P p = {0}; bump(&p); return p.x; }");
    accepts("#include <stdlib.h>\nstruct N { int v; struct N* next; };\nint main() { struct N* n = malloc(sizeof(struct N)); struct N* m = (struct N*)malloc(sizeof *n); int* a = malloc(4 * sizeof(int)); int* b = calloc(4, sizeof(int)); free(n); free(a); return 0; }");
    accepts("int main() { const char* s = \"text\"; char* t = s; return s[0] + t[1]; }");
    accepts("void fill(int* p) { int t = *p; *p = t + 1; }\nvoid twice(int* p) { fill(p); fill(p); }\nint main() { int x = 0; twice(&x); return x; }");
}

TEST(unsupportedConstructsAreRefusedWithAReason)
{
    rejects("int main() { int x = 1; int* p = &x; return *p; }", "'&' is only allowed in a call argument");
    rejects("int main() { int a[2]; int* p = a; p++; return 0; }", "pointer arithmetic");
    rejects("int f(int* p) { return p[0] + *p; }\nint main() { return 0; }", "both dereferenced and indexed");
    rejects("int main() { void* p = 0; return 0; }", "void*");
    rejects("int main() { int (*fp)(int); return 0; }", "function pointers");
    rejects("union U { int i; float f; };\nint main() { return 0; }", "unions");
    rejects("int main() { goto done; done: return 0; }", "goto");
    rejects("int main() { int x = 1; switch (x) { case 1: x++; case 2: x--; break; } return x; }", "falling through");
    rejects("int main() { char s[4]; s = \"abc\"; return 0; }", "");
    rejects("int main() { int x = 1, 2; return x; }", "");
    rejects("int main() { int a = 1 return a; }", "expected ';'");
    rejects("int main() { return undefined_name; }", "undeclared identifier");
    rejects("int main() { return f(1); }", "undeclared identifier");
    rejects("int f(int a) { return a; }\nint main() { return f(1, 2); }", "takes 1 argument");
    rejects("void v(void) { }\nint main() { int x = v(); return x; }", "void value");
    rejects("int main() { exit(1); }", "exit()");
    rejects("int main() { int x; x = \"s\"; return 0; }", "cannot convert");
    rejects("int main() { struct P p; return 0; }", "");
    rejects("int main() { int b : 3; return 0; }", "");
}

TEST(diagnosticsNameTheFileAndLine)
{
    cc::Context ctx;
    try {
        ctx.compile("int main() {\n  int x = 1;\n  return y;\n}\n", "prog.c");
        CHECK(false);
    } catch (const cc::Error& e) {
        CHECK(std::string(e.what()).find("prog.c:3") != std::string::npos);
    }
    CHECK(ctx.diagnose("int main() { return 0; }", "ok.c").empty());
    // A checker error (the typed engine's) comes back as a C diagnostic.
    const std::vector<cc::Diagnostic> diagnostics =
        ctx.diagnose("int main() {\n  const int n = 3; n = 4; return n; }", "bad.c");
    CHECK(!diagnostics.empty());
    CHECK(diagnostics.front().language == script::Language::C);
    CHECK_EQ(diagnostics.front().file, std::string("bad.c"));
    CHECK_EQ(diagnostics.front().line, 2u);
    CHECK(diagnostics.front().toString().find("error C") == std::string::npos);
}
