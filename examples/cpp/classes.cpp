// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// A class hierarchy in scripted C++: an abstract base, overrides, virtual
// dispatch through base pointers, dynamic_cast, containers of objects, and a
// host contract the script reports through.
//
//   ./cpp_classes

#include <cstdio>
#include <string>
#include <vector>

#include "script/script.hpp"

namespace {

script::Context::Options languageOptions()
{
    script::Context::Options options;
    options.language = script::Language::Cpp;
    return options;
}

const char* kProgram = R"CPP(
    #include <cmath>
    #include <map>
    #include <string>
    #include <vector>

    class Shape {
    public:
        virtual double area() const = 0;
        virtual std::string kind() const { return "shape"; }
        double scaled(double k) const { return area() * k * k; }
    };

    class Rect : public Shape {
    public:
        Rect(double w, double h) : w_(w), h_(h) {}
        double area() const override { return w_ * h_; }
        std::string kind() const override { return "rect"; }
    protected:
        double w_, h_;
    };

    class Square : public Rect {
    public:
        Square(double s) : Rect(s, s) {}
        std::string kind() const override { return "square (" + Rect::kind() + ")"; }
    };

    class Circle : public Shape {
    public:
        Circle(double r) : r_(r) {}
        double area() const override { return M_PI * r_ * r_; }
        std::string kind() const override { return "circle"; }
    private:
        double r_;
    };

    int main() {
        std::vector<Shape*> shapes;
        shapes.push_back(new Rect(2, 3));
        shapes.push_back(new Square(2));
        shapes.push_back(new Circle(1));

        std::map<std::string, double> byKind;
        double total = 0;
        int squares = 0;
        for (Shape* s : shapes) {
            byKind[s->kind()] += s->area();          // inserts on first use, as C++ does
            total += s->scaled(1);                   // a non-virtual method calling a virtual one
            if (dynamic_cast<Square*>(s) != nullptr) squares++;
        }
        for (const auto& kv : byKind)
            Report::Line(kv.first, kv.second);
        Report::Line("total", total);
        return squares;
    }
)CPP";

}  // namespace

int main()
{
    script::Context ctx(languageOptions());

    // The one door out of the script: a static host class.
    ctx.hostClass("Report").method("Line", [](std::string label, double value) {
        std::printf("  %-20s %8.3f\n", label.c_str(), value);
    });

    std::puts("areas by kind:");
    const int squares = ctx.eval(kProgram, "shapes.cpp").toInt();
    std::printf("%d of them are squares\n", squares);

    // What the front end refuses, it refuses before anything runs.
    const char* attempts[] = {
        "class Shape { public: virtual double area() const = 0; };\nint main() { Shape s; return 0; }",
        "class A { int hidden = 1; };\nint main() { A a; return a.hidden; }",
        "class B { public: int v; };\nint main() { B b; return b.missing; }",
    };
    std::puts("\nrefused at compile time:");
    for (const char* source : attempts) {
        try {
            ctx.compile(source, "bad.cpp");
        } catch (const script::Error& e) {
            std::printf("  %s\n", e.what());
        }
    }
    return 0;
}
