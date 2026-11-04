// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// C semantics as the front end delivers them: arithmetic and conversions,
// control flow, functions, structs, arrays, strings, and the pointer idioms
// that map onto the engine.

#include "lang.hpp"
#include "tests/test_util.hpp"

using cc::output;
using cc::run;

TEST(arithmeticAndConversions)
{
    CHECK_EQ(run("int main() { return 7 / 2; }").toInt(), 3);
    CHECK_EQ(run("int main() { return -7 / 2; }").toInt(), -3);
    CHECK_EQ(run("int main() { return -7 % 3; }").toInt(), -1);
    CHECK_EQ(run("int main() { double d = 7 / 2; return (int)(d * 10); }").toInt(), 30);
    CHECK_EQ(run("int main() { double d = 7.0 / 2; return (int)(d * 10); }").toInt(), 35);
    CHECK_EQ(run("int main() { int i = 3.99; return i; }").toInt(), 3);
    CHECK_EQ(run("int main() { int i = -3.99; return i; }").toInt(), -3);
    CHECK_EQ(run("int main() { int i = 7; i += 2.6; return i; }").toInt(), 9);
    CHECK_EQ(run("int main() { double d = 1; d += 1; d /= 4; return (int)(d * 100); }").toInt(), 50);
    CHECK_EQ(run("int main() { char c = 'a'; c = c + 1; c++; c += 1; return c == 'd'; }").toInt(), 1);
    CHECK_EQ(run("int main() { char c = 65; int i = c; return i + 'B' - 'A'; }").toInt(), 66);
    CHECK_EQ(run("int main() { char d = '7'; return d - '0'; }").toInt(), 7);
    CHECK_EQ(run("int main() { unsigned char b = 250; b += 10; return b; }").toInt(), 4);
    CHECK_EQ(run("int main() { int big = 2147483647; big++; return big < 0; }").toInt(), 1);
    CHECK_EQ(run("int main() { long l = 1L << 40; return (int)(l >> 30); }").toInt(), 1024);
    CHECK_EQ(run("int main() { long l = 3000000000; return l > 0; }").toInt(), 1);
    CHECK_EQ(run("int main() { unsigned u = 5; int i = -1; return u + i == 4; }").toInt(), 1);
    CHECK_EQ(run("int main() { return 0xff & 0x0f | 1 << 4 ^ 3; }").toInt(), 0x0f | (16 ^ 3));
    CHECK_EQ(run("int main() { return ~0 == -1; }").toInt(), 1);
    CHECK_EQ(run("int main() { float f = 1.5f; return (int)(f * 2); }").toInt(), 3);
    CHECK_EQ(run("int main() { return (int)3.7 + (int)-3.7; }").toInt(), 0);
    CHECK_EQ(run("int main() { int x = 5; return x++ + x; }").toInt(), 11);
    CHECK_EQ(run("int main() { int x = 5; return ++x + x; }").toInt(), 12);
    CHECK_EQ(run("int main() { return sizeof(char) + sizeof(int) + sizeof(double) + sizeof(long); }").toInt(), 21);
    CHECK_EQ(run("int main() { int a[6]; return sizeof(a) / sizeof(a[0]); }").toInt(), 6);
    CHECK_EQ(run("int main() { return 1000000 * 3; }").toInt(), 3000000);
}

TEST(truthIsNumeric)
{
    CHECK_EQ(run("int main() { int a = 3, b = 4; return a < b; }").toInt(), 1);
    CHECK_EQ(run("int main() { int a = 3; int t = a > 1; int f = a > 5; return t * 10 + f; }").toInt(), 10);
    CHECK_EQ(run("int main() { int n = 5, count = 0; while (n) { n--; count++; } return count; }").toInt(), 5);
    CHECK_EQ(run("int main() { int x = 0; if (x) return 1; if (!x) return 2; return 3; }").toInt(), 2);
    CHECK_EQ(run("int main() { double d = 0.5; if (d) return 1; return 0; }").toInt(), 1);
    CHECK_EQ(run("int main() { char* s = 0; if (!s) return 1; return 0; }").toInt(), 1);
    CHECK_EQ(run("int main() { int a = 2, b = 0; return (a && b) + (a || b) * 2 + (!a) * 4 + (a && !b) * 8; }").toInt(), 10);
    CHECK_EQ(run("int main() { int hits = 0; for (int i = 0; i < 10; i++) hits += i % 3 == 0; return hits; }").toInt(), 4);
    CHECK_EQ(run("int main() { int x = 7; return x ? x > 5 : -1; }").toInt(), 1);
    CHECK_EQ(run("int main() { int x = 0; int y = x ? 1 : 2; return y; }").toInt(), 2);
    CHECK_EQ(run("int main() { int i = 0; while (1) { if (++i == 4) break; } return i; }").toInt(), 4);
}

TEST(functionsAndGlobals)
{
    CHECK_EQ(run("int add(int a, int b) { return a + b; }\nint main() { return add(2, add(3, 4)); }").toInt(), 9);
    CHECK_EQ(run("int fact(int n) { return n <= 1 ? 1 : n * fact(n - 1); }\nint main() { return fact(10); }").toInt(), 3628800);
    CHECK_EQ(run("int later(int);\nint main() { return later(1); }\nint later(int x) { return x + 1; }").toInt(), 2);
    CHECK_EQ(run("int main() { return before(3); }\nint before(int x) { return x * 2; }").toInt(), 6);
    CHECK_EQ(run("int counter = 0;\nvoid bump(void) { counter++; }\nint main() { bump(); bump(); return counter; }").toInt(), 2);
    CHECK_EQ(run("const int N = 4;\nint table[N];\nint main() { for (int i = 0; i < N; i++) table[i] = i * i; return table[3]; }").toInt(), 9);
    CHECK_EQ(run("int next(void) { static int n = 10; return n++; }\nint main() { next(); next(); return next(); }").toInt(), 12);
    CHECK_EQ(run("double half(int x) { return x / 2.0; }\nint main() { return (int)(half(5) * 10); }").toInt(), 25);
    CHECK_EQ(run("int take(double d) { return d; }\nint main() { return take(2.9) + take(1); }").toInt(), 3);
    CHECK_EQ(run("void nothing(void) { return; }\nint main() { nothing(); return 1; }").toInt(), 1);
    CHECK_EQ(run("int main(int argc, char** argv) { return argc + (argv == 0); }").toInt(), 1);
    CHECK(run("void main() { int x = 1; }").toInt() == 0);
    CHECK_EQ(run("int square(int x) { return x * x; }\nint apply_twice(int v) { return square(square(v)); }\nint main() { return apply_twice(3); }").toInt(), 81);
}

TEST(pointerParametersBecomeReferences)
{
    CHECK_EQ(run("void swap(int* a, int* b) { int t = *a; *a = *b; *b = t; }\nint main() { int x = 1, y = 2; swap(&x, &y); return x * 10 + y; }").toInt(), 21);
    CHECK_EQ(run("void divide(int a, int b, int* q, int* r) { *q = a / b; *r = a % b; }\nint main() { int q, r; divide(17, 5, &q, &r); return q * 10 + r; }").toInt(), 32);
    CHECK_EQ(run("void inc(int* p) { *p += 1; (*p)++; }\nvoid twice(int* p) { inc(p); inc(p); }\nint main() { int x = 0; twice(&x); return x; }").toInt(), 4);
    CHECK_EQ(run("void scale(double* d, double k) { *d *= k; }\nint main() { double v = 1.5; scale(&v, 4); return (int)v; }").toInt(), 6);
    CHECK_EQ(run("int sum(int* a, int n) { int s = 0; for (int i = 0; i < n; i++) s += a[i]; return s; }\nint main() { int a[] = {1, 2, 3, 4}; return sum(a, 4) + sum(&a[0], 2); }").toInt(), 13);
    CHECK_EQ(run("void fill(int a[], int n, int v) { for (int i = 0; i < n; i++) a[i] = v; }\nint main() { int a[3]; fill(a, 3, 7); return a[0] + a[2]; }").toInt(), 14);
    CHECK_EQ(run("int first(int* a) { return *a; }\nint main() { int x = 9; int a[2] = {9, 8}; int* p = a; return first(&x) + p[1]; }").toInt(), 17);
}

TEST(structs)
{
    const char* point =
        "typedef struct { double x, y; } Point;\n"
        "struct Rect { Point min; Point max; int tag; };\n"
        "double area(struct Rect* r) { return (r->max.x - r->min.x) * (r->max.y - r->min.y); }\n"
        "Point mid(Point a, Point b) { Point m = {(a.x + b.x) / 2, (a.y + b.y) / 2}; return m; }\n";
    CHECK_EQ(run((std::string(point) + "int main() { Point p = {3, 4}; return (int)(p.x * p.y); }").c_str()).toInt(), 12);
    CHECK_EQ(run((std::string(point) + "int main() { Point p; p.x = 1; p.y = 2; return (int)(p.x + p.y); }").c_str()).toInt(), 3);
    CHECK_EQ(run((std::string(point) + "int main() { struct Rect r; r.max.x = 4; r.max.y = 3; return (int)area(&r); }").c_str()).toInt(), 12);
    CHECK_EQ(run((std::string(point) + "int main() { Point p = {0, 0}, q = {4, 8}; Point m = mid(p, q); return (int)(m.x + m.y); }").c_str()).toInt(), 6);
    CHECK_EQ(run((std::string(point) + "int main() { Point m = mid((Point){1, 1}, (Point){3, 3}); return (int)m.x; }").c_str()).toInt(), 2);
    CHECK_EQ(run((std::string(point) + "int main() { struct Rect r = {{0, 0}, {2, 5}, 7}; return (int)area(&r) + r.tag; }").c_str()).toInt(), 17);
    CHECK_EQ(run((std::string(point) + "int main() { Point p = {.y = 5, .x = 1}; return (int)(p.x * 10 + p.y); }").c_str()).toInt(), 15);
    CHECK_EQ(run("struct S { int values[3]; int n; };\nint main() { struct S s; s.values[1] = 5; s.n = 2; return s.values[1] * s.n; }").toInt(), 10);
    CHECK_EQ(run("struct P { int x; };\nint main() { struct P pts[3]; pts[2].x = 9; return pts[2].x + pts[0].x; }").toInt(), 9);
    CHECK_EQ(run("struct P { int x, y; };\nint main() { struct P pts[] = {{1, 2}, {3, 4}}; return pts[1].x * pts[0].y; }").toInt(), 6);
    CHECK_EQ(run("#include <stdlib.h>\nstruct Node { int v; struct Node* next; };\n"
                 "struct Node* push(struct Node* head, int v) { struct Node* n = malloc(sizeof(struct Node)); n->v = v; n->next = head; return n; }\n"
                 "int main() { struct Node* list = NULL; for (int i = 1; i <= 4; i++) list = push(list, i); int s = 0; while (list != NULL) { s += list->v; list = list->next; } return s; }").toInt(), 10);
    CHECK_EQ(run("struct P { int x; };\nvoid set(struct P* p, int v) { p->x = v; }\nint main() { struct P p = {0}; set(&p, 3); struct P* q = &p; q->x++; return p.x; }").toInt(), 4);
    CHECK_EQ(run("struct P { int x; };\nint main() { struct P* p = NULL; return p == NULL && !p; }").toInt(), 1);
}

TEST(arraysAndStrings)
{
    CHECK_EQ(run("int main() { int a[5]; for (int i = 0; i < 5; i++) a[i] = i * 2; return a[4] + a[0]; }").toInt(), 8);
    CHECK_EQ(run("int main() { int a[] = {5, 6, 7}; return a[1] + sizeof(a) / sizeof(int); }").toInt(), 9);
    CHECK_EQ(run("int main() { int a[4] = {1}; return a[0] + a[1] + a[2] + a[3]; }").toInt(), 1);
    CHECK_EQ(run("int main() { double d[2] = {0}; return (int)(d[0] + d[1]); }").toInt(), 0);
    CHECK_EQ(run("int main() { int m[2][3] = {{1, 2, 3}, {4, 5, 6}}; int t = 0; for (int i = 0; i < 2; i++) for (int j = 0; j < 3; j++) t += m[i][j]; return t; }").toInt(), 21);
    CHECK_EQ(run("int main() { int m[3][3]; m[1][2] = 8; return m[1][2] + m[0][0]; }").toInt(), 8);
    CHECK_EQ(run("int main() { int n = 4; int a[n]; a[3] = 2; return a[3]; }").toInt(), 2);
    CHECK_EQ(run("int main() { char s[] = \"hello\"; return s[0] == 'h' && s[5] == '\\0' && sizeof(s) == 6; }").toInt(), 1);
    CHECK_EQ(run("int main() { char buf[8] = \"ab\"; buf[2] = 'c'; buf[3] = 0; return buf[2] + buf[7]; }").toInt(), 'c');
    CHECK_EQ(run("int main() { const char* s = \"text\"; return s[1] == 'e'; }").toInt(), 1);
    CHECK_EQ(run("#include <stdlib.h>\nint main() { int* a = malloc(3 * sizeof(int)); a[2] = 5; int* b = calloc(2, sizeof(int)); int r = a[2] + b[1]; free(a); free(b); return r; }").toInt(), 5);
    CHECK_EQ(run("int main() { int a[3] = {1, 2, 3}; int* p = a; p[0] = 10; return a[0]; }").toInt(), 10);
    CHECK_EQ(run("int table[3] = {7, 8, 9};\nint main() { return table[2]; }").toInt(), 9);
}

TEST(controlFlow)
{
    CHECK_EQ(run("int main() { int t = 0; for (int i = 0; i < 10; i++) { if (i % 2) continue; if (i > 6) break; t += i; } return t; }").toInt(), 12);
    CHECK_EQ(run("int main() { int i = 10, n = 0; do { n++; } while (--i > 5); return n; }").toInt(), 5);
    CHECK_EQ(run("int main() { int i, j; for (i = 0, j = 10; i < j; i++, j--) ; return i; }").toInt(), 5);
    CHECK_EQ(run("int grade(int s) { switch (s / 10) { case 10: case 9: return 4; case 8: return 3; case 7: return 2; default: return 0; } }\nint main() { return grade(95) * 100 + grade(85) * 10 + grade(40); }").toInt(), 430);
    CHECK_EQ(run("enum { A, B = 5, C };\nint main() { int x = C; switch (x) { case A: return 1; case B: return 2; case C: return 3; } return 0; }").toInt(), 3);
    CHECK_EQ(run("int main() { char c = 'b'; switch (c) { case 'a': return 1; case 'b': return 2; } return 0; }").toInt(), 2);
    CHECK_EQ(run("int main() { int x = 3; switch (x) { case 1: x = 10; break; case 3: { x = 30; break; } } return x; }").toInt(), 30);
    CHECK_EQ(run("#define LIMIT 3\nint main() { int n = 0; while (n < LIMIT) n++; return n; }").toInt(), 3);
    CHECK_EQ(run("int main() { int x = 5; { int x = 6; x++; } return x; }").toInt(), 5);
}

TEST(printfAndOutput)
{
    CHECK_EQ(output("#include <stdio.h>\nint main() { printf(\"%d %s %c %.2f %5d|%-3d|%03d %x %u %ld %%\\n\", 42, \"str\", 'z', 3.14159, 7, 8, 9, 255, 3, 1L << 33); return 0; }"),
             "42 str z 3.14     7|8  |009 ff 3 8589934592 %\n");
    CHECK_EQ(output("#include <stdio.h>\nint main() { char s[] = \"buf\"; printf(\"%s %d\\n\", s, (int)sizeof(s)); puts(\"line\"); puts(s); putchar('x'); putchar('\\n'); return 0; }"),
             "buf 4\nline\nbuf\nx\n");
    CHECK_EQ(output("#include <stdio.h>\nint main() { printf(\"%e|%g|%10.3f|%-8s|%+d|% d\\n\", 12345.678, 0.0001, 2.5, \"ab\", 5, 5); return 0; }"),
             "1.234568e+04|0.0001|     2.500|ab      |+5| 5\n");
    CHECK_EQ(output("#include <stdio.h>\nint main() { fprintf(stdout, \"out\"); fprintf(stderr, \"err\"); return 0; }"), "outerr");
    CHECK_EQ(output("#include <stdio.h>\nint main() { int n = printf(\"abc\"); printf(\"%d\", n); return 0; }"), "abc3");
    CHECK_EQ(output("#include <stdio.h>\nint main() { printf(\"%c%c\", 72, 'i'); return 0; }"), "Hi");
    CHECK_EQ(output("#include <stdio.h>\nint main() { int ok = 3 > 2; printf(\"%d %d\", ok, 3 < 2); return 0; }"), "1 0");
    CHECK_EQ(output("#include <stdio.h>\n#include <stdbool.h>\nint main() { bool b = true; if (b) printf(\"yes\"); return b; }"), "yes");
}

TEST(runtimeErrorsSurface)
{
    cc::Context ctx;
    CHECK_THROWS(ctx.eval("int main() { int a[2]; return a[5]; }"), cc::ErrorCode::IndexOutOfRange);
    CHECK_THROWS(ctx.eval("int main() { int z = 0; return 1 / z; }"), cc::ErrorCode::DivideByZero);
    CHECK_THROWS(ctx.eval("struct P { int x; };\nint main() { struct P* p = 0; return p->x; }"), cc::ErrorCode::NullReference);
    CHECK_THROWS(ctx.eval("#include <assert.h>\nint main() { assert(1 == 2); return 0; }"), cc::ErrorCode::InvalidOperation);
    CHECK_EQ(ctx.eval("#include <assert.h>\nint main() { assert(1 == 1); return 5; }").toInt(), 5);
    try {
        ctx.eval("int deep(int n) { return deep(n + 1); }\nint main() { return deep(0); }", "deep.c");
        CHECK(false);
    } catch (const cc::Error& e) {
        CHECK(e.code() == cc::ErrorCode::StackOverflow);
    }
    try {
        ctx.eval("int main() { int a[1]; return a[3]; }", "oob.c");
        CHECK(false);
    } catch (const cc::Exception& e) {
        CHECK(e.stack().find("oob.c:1") != std::string::npos);
    }
}

TEST(compileOnceRunMany)
{
    cc::Context ctx;
    script::Assembly program = ctx.compile("int calls = 0;\nint main() { return ++calls; }", "counter.c");
    CHECK(program.language() == script::Language::C);
    CHECK_EQ(ctx.run(program).toInt(), 1);
    CHECK_EQ(ctx.run(program).toInt(), 2);
    // Two programs coexist; each keeps its own globals.
    script::Assembly other = ctx.compile("int calls = 100;\nint main() { return ++calls; }", "other.c");
    CHECK_EQ(ctx.run(other).toInt(), 101);
    CHECK_EQ(ctx.run(program).toInt(), 3);
    // The typed engine's image works for C too.
    const std::vector<std::uint8_t> image = program.serialize();
    CHECK(!image.empty());
    cc::Context fresh;
    script::Assembly loaded = fresh.load(image.data(), image.size());
    CHECK_EQ(fresh.run(loaded).toInt(), 1);
}

TEST(topLevelProgramsDoNotAccumulate)
{
    script::Context ctx;
    for (int i = 0; i < 3000; ++i)
        CHECK_EQ(ctx.eval("int main() { return " + std::to_string(i % 100) + "; }", "<t>.c", script::Language::C).toInt(), i % 100);
    ctx.collectGarbage();
    CHECK(ctx.stats().assemblies <= 1);
}
