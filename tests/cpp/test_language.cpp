// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// C++ semantics as the front end delivers them: what C++ adds to C --
// classes, references, bool, overloading, defaults, auto, range-for,
// namespaces, enum class, new -- on top of the C the suite next door covers.

#include "lang.hpp"
#include "tests/test_util.hpp"

using cpp::output;
using cpp::run;

TEST(cIsStillThere)
{
    CHECK_EQ(run("int main() { return 6 * 7; }").toInt(), 42);
    CHECK_EQ(run("#include <cstdio>\nint sq(int x) { return x * x; }\nint main() { int a[3] = {1, 2, 3}; return sq(a[2]) + sizeof(int); }").toInt(), 13);
    CHECK_EQ(output("#include <cstdio>\nint main() { printf(\"%d-%s\", 7, \"x\"); return 0; }"), "7-x");
    CHECK_EQ(run("struct P { int x; int y; };\nint main() { P p = {1, 2}; P* q = &p; q->x = 5; return p.x + p.y; }").toInt(), 7);
    CHECK_EQ(run("void swap(int* a, int* b) { int t = *a; *a = *b; *b = t; }\nint main() { int x = 1, y = 2; swap(&x, &y); return x * 10 + y; }").toInt(), 21);
    // main may fall off its end.
    CHECK_EQ(run("int main() { int x = 1; }").toInt(), 0);
}

TEST(boolIsAType)
{
    CHECK_EQ(run("int main() { bool b = true; bool c = false; return b && !c; }").toInt(), 1);
    CHECK_EQ(run("bool big(int n) { return n > 10; }\nint main() { return big(11) + big(2) * 10; }").toInt(), 1);
    CHECK_EQ(run("int main() { bool b = 5; int n = b; return n; }").toInt(), 1);
    CHECK_EQ(run("int main() { bool b = false; b = 3 > 2; if (b) return 1; return 0; }").toInt(), 1);
    CHECK_EQ(run("int main() { bool flags[3]; flags[1] = true; return flags[1] ? 4 : 5; }").toInt(), 4);
    CHECK_EQ(output("#include <iostream>\nint main() { std::cout << true << false << (1 < 2); return 0; }"), "101");
    CHECK_EQ(run("int main() { bool a = true; bool b = a ? true : false; return b == a; }").toInt(), 1);
}

TEST(referencesAndOverloads)
{
    CHECK_EQ(run("void bump(int& n) { n++; }\nint main() { int x = 1; bump(x); bump(x); return x; }").toInt(), 3);
    CHECK_EQ(run("void swap(int& a, int& b) { int t = a; a = b; b = t; }\nint main() { int x = 1, y = 2; swap(x, y); return x * 10 + y; }").toInt(), 21);
    CHECK_EQ(run("double half(const double& d) { return d / 2; }\nint main() { return (int)half(9.0); }").toInt(), 4);
    CHECK_EQ(run("int f(int x) { return x; }\nint f(double d) { return (int)(d * 10); }\nint f(int a, int b) { return a + b; }\nint main() { return f(1) + f(0.5) + f(2, 3); }").toInt(), 11);
    CHECK_EQ(run("int area(int w, int h = 2) { return w * h; }\nint main() { return area(3) + area(3, 3); }").toInt(), 15);
    CHECK_EQ(run("#include <string>\nvoid add(std::string& s, const std::string& t) { s += t; }\nint main() { std::string s = \"a\"; add(s, \"b\"); return s.size(); }").toInt(), 2);
    CHECK_EQ(run("#include <vector>\nvoid fill(std::vector<int>& v) { v.push_back(4); }\nint main() { std::vector<int> v; fill(v); return v[0]; }").toInt(), 4);
    CHECK_EQ(run("#include <utility>\nint main() { int a = 1, b = 2; std::swap(a, b); return a * 10 + b; }").toInt(), 21);
}

TEST(classes)
{
    const char* point =
        "class Point {\n"
        "public:\n"
        "    double x, y;\n"
        "    Point(double a, double b) : x(a), y(b) {}\n"
        "    Point() : x(0), y(0) {}\n"
        "    double length() const { return sqrt(x * x + y * y); }\n"
        "    void scale(double k) { x *= k; y *= k; }\n"
        "    static Point origin() { return Point(0, 0); }\n"
        "};\n";
    CHECK_EQ(run((std::string("#include <cmath>\n") + point + "int main() { Point p(3, 4); return (int)p.length(); }").c_str()).toInt(), 5);
    CHECK_EQ(run((std::string("#include <cmath>\n") + point + "int main() { Point p = Point(3, 4); p.scale(2); return (int)p.length(); }").c_str()).toInt(), 10);
    CHECK_EQ(run((std::string("#include <cmath>\n") + point + "int main() { Point* p = new Point(6, 8); double l = p->length(); delete p; return (int)l; }").c_str()).toInt(), 10);
    CHECK_EQ(run((std::string("#include <cmath>\n") + point + "int main() { Point o = Point::origin(); Point d; return (int)(o.x + d.y); }").c_str()).toInt(), 0);
    CHECK_EQ(run("struct Counter { int n = 0; void bump() { n++; } int get() { return n; } };\nint main() { Counter c; c.bump(); c.bump(); return c.get(); }").toInt(), 2);
    CHECK_EQ(run("class Acc { int total; public: Acc() : total(0) {} void add(int v) { this->total += v; } int value() const { return total; } };\nint main() { Acc a; a.add(2); a.add(3); return a.value(); }").toInt(), 5);
    // Methods see each other and the free functions and globals.
    CHECK_EQ(run("int base = 100;\nint twice(int x) { return 2 * x; }\nclass W { public: int v = 1; int a() { return b() + twice(v) + base; } int b() { return v + 1; } };\nint main() { W w; return w.a(); }").toInt(), 104);
    // Out-of-line definitions and static members.
    CHECK_EQ(run("class Id { public: static int next; int value; Id(); };\nint Id::next = 10;\nId::Id() { value = next++; }\nint main() { Id a; Id b; return a.value * 100 + b.value + Id::next; }").toInt(), 1023);
    CHECK_EQ(run("struct S { int f(int x); };\nint S::f(int x) { return x * 3; }\nint main() { S s; return s.f(4); }").toInt(), 12);
    // Aggregates and brace initialisation.
    CHECK_EQ(run("struct P { int x; int y; };\nint main() { P p{1, 2}; P q = {3, 4}; return p.x + q.y; }").toInt(), 5);
    // Member objects are constructed with their owner.
    CHECK_EQ(run("struct In { int v = 7; };\nstruct Out { In in; int k = 1; };\nint main() { Out o; return o.in.v + o.k; }").toInt(), 8);
    CHECK_EQ(run("class Box { public: int w, h; Box(int a, int b) : w(a), h(b) {} };\nclass Room { public: Box box; Room() : box(2, 3) {} };\nint main() { Room r; return r.box.w * r.box.h; }").toInt(), 6);
}

TEST(inheritanceAndVirtualDispatch)
{
    const char* shapes =
        "class Shape {\n"
        "public:\n"
        "    virtual double area() const = 0;\n"
        "    virtual int sides() const { return 0; }\n"
        "    double doubled() const { return 2 * area(); }\n"
        "};\n"
        "class Rect : public Shape {\n"
        "public:\n"
        "    double w, h;\n"
        "    Rect(double a, double b) : w(a), h(b) {}\n"
        "    double area() const override { return w * h; }\n"
        "    int sides() const override { return 4; }\n"
        "};\n"
        "class Square : public Rect {\n"
        "public:\n"
        "    Square(double s) : Rect(s, s) {}\n"
        "    int sides() const override { return Rect::sides() + 100; }\n"
        "};\n"
        "class Circle : public Shape {\n"
        "public:\n"
        "    double r;\n"
        "    Circle(double x) : r(x) {}\n"
        "    double area() const { return 3 * r * r; }\n"
        "};\n";
    CHECK_EQ(run((std::string(shapes) + "int main() { Shape* s = new Rect(2, 3); return (int)s->area() + s->sides(); }").c_str()).toInt(), 10);
    CHECK_EQ(run((std::string(shapes) + "int main() { Square q(2); Shape& s = q; return (int)s.doubled() + q.sides(); }").c_str()).toInt(), 112);
    CHECK_EQ(run((std::string(shapes) + "#include <vector>\nint main() { std::vector<Shape*> all; all.push_back(new Rect(1, 2)); all.push_back(new Circle(1)); all.push_back(new Square(3)); double t = 0; for (Shape* s : all) t += s->area(); return (int)t; }").c_str()).toInt(), 14);
    CHECK_EQ(run((std::string(shapes) + "int main() { Shape* s = new Circle(2); Rect* r = dynamic_cast<Rect*>(s); Circle* c = dynamic_cast<Circle*>(s); return (r == nullptr) * 10 + (c != nullptr); }").c_str()).toInt(), 11);
    CHECK_EQ(run("class A { public: int v = 1; int get() { return v; } };\nclass B : public A { public: int w = 2; int sum() { return get() + v + w; } };\nint main() { B b; return b.sum() + b.v; }").toInt(), 5);
}

TEST(autoRangeForAndEnumClass)
{
    CHECK_EQ(run("int main() { auto x = 3; auto d = 2.5; auto s = \"ab\"; return x + (int)d + sizeof(s); }").toInt(), 13);
    CHECK_EQ(run("int main() { int a[] = {1, 2, 3}; int t = 0; for (int x : a) t += x; for (auto& x : a) t += x; return t; }").toInt(), 12);
    CHECK_EQ(run("#include <vector>\nint main() { std::vector<int> v = {4, 5, 6}; int t = 0; for (auto x : v) t += x; return t; }").toInt(), 15);
    CHECK_EQ(run("#include <string>\nint main() { std::string s = \"abc\"; int n = 0; for (char c : s) n += c - 'a'; return n; }").toInt(), 3);
    CHECK_EQ(run("enum class Color { Red, Green = 5, Blue };\nint main() { Color c = Color::Blue; switch (c) { case Color::Red: return 1; case Color::Blue: return 6; } return 0; }").toInt(), 6);
    CHECK_EQ(run("enum Dir { Up, Down };\nint main() { Dir d = Down; return d + Dir::Up; }").toInt(), 1);
    CHECK_EQ(run("namespace geo { int twice(int x) { return 2 * x; } }\nusing namespace geo;\nint main() { return geo::twice(2) + twice(3); }").toInt(), 10);
    CHECK_EQ(run("using Num = long;\nint main() { Num n = 1L << 40; return (int)(n >> 38); }").toInt(), 4);
    CHECK_EQ(run("int main() { int x = static_cast<int>(3.9); double d = static_cast<double>(x) / 2; return int(d * 10) + double(1); }").toInt(), 16);
    CHECK_EQ(run("int main() { int* p = nullptr; int a[2]; return (p == nullptr) + (a != nullptr); }").toInt(), 2);
    CHECK_EQ(run("int main() { int* a = new int[4]; a[3] = 9; int r = a[3]; delete[] a; return r; }").toInt(), 9);
}

TEST(exceptions)
{
    CHECK_EQ(run("#include <stdexcept>\nint main() { try { throw std::runtime_error(\"boom\"); } catch (const std::exception& e) { return e.what().size(); } return 0; }").toInt(), 4);
    CHECK_EQ(output("#include <iostream>\n#include <stdexcept>\nint check(int v) { if (v < 0) throw std::invalid_argument(\"negative\"); return v; }\n"
                    "int main() { try { check(1); check(-1); } catch (const std::invalid_argument& e) { std::cout << \"caught \" << e.what(); } return 0; }"),
             "caught negative");
    CHECK_EQ(run("#include <vector>\n#include <stdexcept>\nint main() { std::vector<int> v(2); try { return v.at(5); } catch (const std::out_of_range&) { return 1; } catch (...) { return 2; } }").toInt(), 1);
    CHECK_EQ(run("int main() { int z = 0; try { return 1 / z; } catch (...) { return 7; } }").toInt(), 7);
    CHECK_EQ(run("#include <stdexcept>\nint main() { try { try { throw std::logic_error(\"x\"); } catch (const std::out_of_range&) { return 1; } } catch (const std::exception&) { return 2; } return 3; }").toInt(), 2);
    cpp::Context ctx;
    CHECK_THROWS(ctx.eval("#include <stdexcept>\nint main() { throw std::runtime_error(\"unhandled\"); }"), cpp::ErrorCode::InvalidOperation);
    try {
        ctx.eval("#include <stdexcept>\nint main() {\n  throw std::runtime_error(\"where\");\n}", "prog.cpp");
        CHECK(false);
    } catch (const cpp::Exception& e) {
        CHECK(std::string(e.what()).find("where") != std::string::npos);
        CHECK(e.stack().find("prog.cpp:3") != std::string::npos);
    }
}

TEST(compileOnceRunMany)
{
    cpp::Context ctx;
    script::Assembly program = ctx.compile("class C { public: static int calls; };\nint C::calls = 0;\nint main() { return ++C::calls; }", "counter.cpp");
    CHECK(program.language() == script::Language::Cpp);
    CHECK_EQ(ctx.run(program).toInt(), 1);
    CHECK_EQ(ctx.run(program).toInt(), 2);
    const std::vector<std::uint8_t> image = program.serialize();
    CHECK(!image.empty());
    cpp::Context fresh;
    script::Assembly loaded = fresh.load(image.data(), image.size());
    CHECK_EQ(fresh.run(loaded).toInt(), 1);
}
