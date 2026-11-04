// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// What the C++ grammar accepts, what it refuses, and what it says when it does.

#include "lang.hpp"
#include "tests/test_util.hpp"

namespace {

void accepts(const char* source)
{
    cpp::Context ctx;
    ctx.compile(source, "<accepts>");
}

void rejects(const char* source, const char* fragment)
{
    cpp::Context ctx;
    try {
        ctx.compile(source, "<rejects>");
    } catch (const cpp::Error& e) {
        if (e.code() != cpp::ErrorCode::SyntaxError && e.code() != cpp::ErrorCode::CompileError)
            ::tst::fail(__FILE__, __LINE__, std::string("wrong error from: ") + source + "\n  " + e.what());
        if (std::string(e.what()).find(fragment) == std::string::npos)
            ::tst::fail(__FILE__, __LINE__,
                        std::string("message '") + e.what() + "' does not mention '" + fragment + "'");
        return;
    }
    ::tst::fail(__FILE__, __LINE__, std::string("expected an error from: ") + source);
}

}  // namespace

TEST(declarations)
{
    accepts("#include <iostream>\n#include <vector>\n#include <string>\nusing namespace std;\nint main() { return 0; }");
    accepts("class A; class B { public: A* a = nullptr; }; class A { public: int v; };\nint main() { B b; return 0; }");
    accepts("struct S { int x = 1; static const int N = 3; int f() const noexcept { return x + N; } };\nint main() { S s; return s.f(); }");
    accepts("class C { public: explicit C(int) {} C() = default; virtual ~C() = default; };\nint main() { return 0; }");
    accepts("namespace a { namespace b { int f() { return 1; } } }\nint main() { return a::b::f(); }");
    accepts("namespace { int hidden = 2; }\nint main() { return hidden; }");
    accepts("using std::string; using std::cout;\nint main() { return 0; }");
    accepts("constexpr int N = 4; inline int f() { return N; } static int g() { return f(); }\nint main() { return g(); }");
    accepts("int f(int, double = 1.5, const char* = \"x\");\nint f(int a, double b, const char* c) { return a; }\nint main() { return f(1); }");
    accepts("enum class E : int { A = 1, B }; enum Plain { X, Y };\nint main() { E e = E::B; return X + (int)e; }");
    accepts("#include <vector>\n#include <map>\nint main() { std::vector<std::vector<std::map<int, std::string>>> deep; return deep.size(); }");
}

TEST(statements)
{
    accepts("int main() { for (int i = 0; i < 3; ++i) {} int a[2] = {1, 2}; for (int& x : a) {} for (const auto& x : a) {} return 0; }");
    accepts("int main() { try { throw std::runtime_error(\"x\"); } catch (std::exception& e) {} catch (...) {} return 0; }");
    accepts("int main() { int* p = new int[3]; delete[] p; return 0; }");
    accepts("int main() { bool b = true; b = !b; b = b && true || false; if (b) return 1; else return 0; }");
    accepts("int main() { int x = 3; auto y = x * 2; const auto z = y; return z; }");
    accepts("struct P { int x; }; int main() { P p{1}; P q(p); P* r = &p; r->x++; (*r).x--; return p.x + q.x; }");
}

TEST(unsupportedConstructsAreRefusedWithAReason)
{
    rejects("template <class T> T id(T x) { return x; }\nint main() { return id(1); }", "templates");
    rejects("int main() { auto f = [](int x) { return x; }; return f(1); }", "lambdas");
    rejects("struct V { int x; V operator+(const V& o) { return V{x + o.x}; } };\nint main() { return 0; }", "operator overloading");
    rejects("class R { public: int fd; ~R() { fd = 0; } };\nint main() { return 0; }", "destructor");
    rejects("#include <vector>\nint main() { std::vector<int> v; for (auto it = v.begin(); it != v.end(); ++it) {} return 0; }", "iterators");
    rejects("#include <vector>\n#include <algorithm>\nint main() { std::vector<int> v; std::sort(v.begin(), v.end()); return 0; }", "std::sort");
    rejects("int main() { int x = 1; int& r = x; return r; }", "reference to int must be a parameter");
    rejects("class A {}; class B {}; class C : public A, public B {};\nint main() { return 0; }", "multiple inheritance");
    rejects("class Outer { class Inner { }; };\nint main() { return 0; }", "nested classes");
    rejects("union U { int i; float f; };\nint main() { return 0; }", "unions");
    rejects("int main() { int x; std::cin >> x; return x; }", "std::cin");
    rejects("class Shape { public: virtual int f() = 0; };\nint main() { Shape s; return 0; }", "abstract");
    rejects("class P { int secret = 1; public: int open = 2; };\nint main() { P p; return p.secret; }", "");
    rejects("struct P { int x; };\nint main() { P p; return p.y; }", "no member 'y'");
    rejects("int f(int a) { return a; }\nint main() { return f(\"s\"); }", "no overload of 'f'");
    rejects("int f(int a) { return a; }\nint f(double b) { return 1; }\nint main() { return f(\"s\"); }", "no overload");
    rejects("void bump(int& n) { n++; }\nint main() { bump(3); return 0; }", "reference to int");
    rejects("#include <string>\nint main() { std::string s = \"a\"; return s + 1; }", "std::string can only be added");
    rejects("#include <vector>\nint main() { std::vector<int> v; v.foo(); return 0; }", "std::vector has no member 'foo'");
    rejects("#include <map>\nint main() { std::map<int, int> m; auto it = m.find(1); return 0; }", "iterators");
    rejects("class X { public: X(int a) {} };\nint main() { X x; return 0; }", "");
    rejects("int main() { int a = 1 return a; }", "expected ';'");
    rejects("int main() { return undefined_name; }", "undeclared identifier");
    rejects("struct S { int x; };\nint main() { S s; s.x.y = 1; return 0; }", "not a struct");
    rejects("int main() { goto done; done: return 0; }", "goto");
    rejects("int main() { int x = 1; int* p = &x; p++; return *p; }", "");
    rejects("class Q { public: static int n; };\nint main() { Q::m = 1; return 0; }", "no static member 'm'");
    rejects("#include <vector>\nstruct P { int x; };\nint main() { std::vector<P> v(3, P{1}); return 0; }", "shared value");
}

TEST(diagnosticsNameTheFileAndLine)
{
    cpp::Context ctx;
    try {
        ctx.compile("int main() {\n  int x = 1;\n  return y;\n}\n", "prog.cpp");
        CHECK(false);
    } catch (const cpp::Error& e) {
        CHECK(std::string(e.what()).find("prog.cpp:3") != std::string::npos);
    }
    CHECK(ctx.diagnose("int main() { return 0; }", "ok.cpp").empty());
    const std::vector<cpp::Diagnostic> diagnostics =
        ctx.diagnose("int main() {\n  const int n = 3; n = 4; return n; }", "bad.cpp");
    CHECK(!diagnostics.empty());
    CHECK(diagnostics.front().language == script::Language::Cpp);
    CHECK_EQ(diagnostics.front().file, std::string("bad.cpp"));
    CHECK_EQ(diagnostics.front().line, 2u);
}
