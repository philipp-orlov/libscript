// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// One Context, six languages, one set of host bindings.
//
// The host binds a static class and a C++ class exactly once. C#, C and C++
// see a static class and a sealed class with typed members, TypeScript sees
// the same with a declaration derived from the C++ signatures, JavaScript and
// Python see an object and a constructor on the global scope. A C++ instance
// the host hands out is the same object to all six.
#include <cmath>
#include <cstdio>
#include <string>

#include "script/script.hpp"

namespace {

struct Vec2
{
    double x = 0;
    double y = 0;
    Vec2() = default;
    Vec2(double a, double b) : x(a), y(b) {}
    double length() const { return std::sqrt(x * x + y * y); }
    void scale(double k)
    {
        x *= k;
        y *= k;
    }
};

struct Tally
{
    int calls = 0;
    int bump() { return ++calls; }
};

void show(const char* language, const script::Value& v)
{
    std::printf("  %-10s -> %s\n", language, v.inspect().c_str());
}

}  // namespace

int main()
{
    script::Context ctx;

    // Bound once ...
    ctx.hostClass("Env")
        .method("Greet", [](std::string who) { return "hello " + who; })
        .method("Twice", [](int n) { return n * 2; })
        .constant("Answer", 42);
    script::Class<Vec2>(ctx, "Vec2")
        .constructor<double, double>()
        .field("X", &Vec2::x)
        .field("Y", &Vec2::y)
        .method("Length", &Vec2::length)
        .method("Scale", &Vec2::scale);

    // ... visible everywhere. The language comes from the file extension.
    std::puts("a static host class:");
    show("C#", ctx.eval("return Env.Greet(\"C#\") + \" \" + Env.Twice(Env.Answer);", "a.cs"));
    show("TypeScript", ctx.eval("Env.Greet('TS') + ' ' + Env.Twice(Env.Answer)", "a.ts"));
    show("JavaScript", ctx.eval("Env.Greet('JS') + ' ' + Env.Twice(Env.Answer)", "a.js"));
    show("Python", ctx.eval("Env.Greet('Py') + ' ' + str(Env.Twice(Env.Answer))", "a.py"));
    show("C", ctx.eval("#include <string.h>\nint main() { return strlen(Env.Greet(\"C\")) + Env.Twice(Env.Answer); }", "a.c"));
    show("C++", ctx.eval("#include <string>\nint main() { std::string s = Env::Greet(\"C++\"); return s.size() + Env::Twice(Env::Answer); }", "a.cpp"));

    std::puts("a bound C++ class:");
    show("C#", ctx.eval("var v = new Vec2(3, 4); v.Scale(2); return v.Length();", "b.cs"));
    show("TypeScript", ctx.eval("const v: Vec2 = new Vec2(3, 4); v.Scale(2); v.Length()", "b.ts"));
    show("JavaScript", ctx.eval("const v = new Vec2(3, 4); v.Scale(2); v.Length()", "b.js"));
    show("Python", ctx.eval("v = Vec2(3, 4)\nv.Scale(2)\nv.Length()", "b.py"));
    show("C", ctx.eval("int main() { Vec2 v = Vec2(3, 4); v.Scale(2); return (int)v.Length(); }", "b.c"));
    show("C++", ctx.eval("int main() { auto v = new Vec2(3, 4); v->Scale(2); return int(v->Length()); }", "b.cpp"));

    // The typed languages check host calls before anything runs.
    std::puts("type errors, caught before running:");
    for (const script::Diagnostic& d : ctx.diagnose("Env.Twice(\"text\");", "c.cs"))
        std::printf("  %s\n", d.toString().c_str());
    for (const script::Diagnostic& d : ctx.diagnose("const s: string = Env.Twice(1)", "c.ts"))
        std::printf("  %s\n", d.toString().c_str());
    for (const char* source : {"int main() { return Env.Twice(\"text\"); }", "int main() { int n = Env::Greet(\"x\"); return n; }"}) {
        try {
            ctx.compile(source, std::string(source).find("::") == std::string::npos ? "c.c" : "c.cpp");
        } catch (const script::Error& e) {
            std::printf("  %s\n", e.what());
        }
    }

    // One C++ instance, handed to each language in turn.
    std::puts("one instance shared through the host:");
    Tally tally;
    script::Class<Tally>(ctx, "Tally").method("Bump", &Tally::bump);
    ctx.hostClass("Shared").method("Tally", [&tally] { return &tally; });
    ctx.eval("Shared.Tally().Bump();", "d.cs");
    ctx.eval("Shared.Tally().Bump()", "d.ts");
    ctx.global().set("tally", ctx.value(&tally));
    ctx.eval("tally.Bump(); tally.Bump()", "d.js");
    ctx.eval("tally.Bump()", "d.py");
    ctx.eval("int main() { Shared.Tally().Bump(); return 0; }", "d.c");
    ctx.eval("int main() { Tally* t = Shared::Tally(); t->Bump(); return 0; }", "d.cpp");
    std::printf("  bumped %d times\n", tally.calls);

    const script::Context::Stats s = ctx.stats();
    std::printf("\none heap: %llu objects, %llu strings, %llu bytes, %llu assemblies loaded\n",
                (unsigned long long)s.liveObjects, (unsigned long long)s.liveStrings,
                (unsigned long long)s.heapBytes, (unsigned long long)s.assemblies);
    return 0;
}
