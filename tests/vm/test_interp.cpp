// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The interpreter against the dynamic engine: every program in snippets.inc runs on both cores and must give the same
// value, or the same kind of error.
#include <string>
#include <vector>

#include "script/script.hpp"
#include "tests/test_util.hpp"

namespace {

struct Outcome
{
    bool threw = false;
    std::string text;
    std::string type;
    script::ErrorCode code = script::ErrorCode::Ok;
};

Outcome runOn(script::Context::Options::Core core, const char* source, bool runtimeCode = false, bool compiled = false)
{
    script::Context::Options options;
    options.runtimeCode = runtimeCode;
    if (compiled) {
        // Every function compiled when first called, and loops entered in the middle: the new core's baseline code.
        options.jit.mode = script::JitOptions::Mode::Eager;
    }
    options.language = script::Language::JavaScript;
    options.core = core;
    script::Context ctx(options);
    Outcome out;
    try {
        script::Value v = ctx.eval(source);
        out.text = v.toString();
        out.type = script::toString(v.type());
        // An int and a double that print alike are the same number to a program.
        if (out.type == "int" || out.type == "double" || out.type == "long" || out.type == "float")
            out.type = "number";
    } catch (const script::Error& e) {
        out.threw = true;
        out.code = e.code();
        out.text = e.what();
    }
    return out;
}

/// Programs that leave their result in `out` once the jobs have run.
const char* const kAsyncSnippets[] = {
#define S(text) text,
#include "snippets_async.inc"
#undef S
};

Outcome runAsyncOn(script::Context::Options::Core core, const char* source)
{
    script::Context::Options options;
    options.language = script::Language::JavaScript;
    options.core = core;
    script::Context ctx(options);
    Outcome out;
    try {
        ctx.eval(source);
        ctx.runJobs();
        out.text = ctx.eval("String(out)").toString();
    } catch (const script::Error& e) {
        out.threw = true;
        out.code = e.code();
        out.text = e.what();
    }
    return out;
}

const char* const kSnippets[] = {
#define S(text) text,
#include "snippets.inc"
#undef S
};

const char* const kJitSnippets[] = {
#define S(text) text,
#include "snippets_jit.inc"
#undef S
};

bool sameOutcome(const Outcome& want, const Outcome& got)
{
    return want.threw == got.threw && want.text == got.text && want.type == got.type && want.code == got.code;
}

}  // namespace

TEST(everySnippetAgreesWithTheDynamicEngine)
{
    int checked = 0, failed = 0;
    for (const char* source : kSnippets) {
        const Outcome want = runOn(script::Context::Options::Core::Dyn, source);
        const Outcome got = runOn(script::Context::Options::Core::Vm, source);
        ++checked;
        const bool same = want.threw == got.threw && want.text == got.text && want.type == got.type && want.code == got.code;
        if (!same) {
            ++failed;
            std::fprintf(stderr, "MISMATCH: %s\n   dyn: %s%s [%s]\n    vm: %s%s [%s]\n", source, want.threw ? "THROW " : "",
                         want.text.c_str(), want.type.c_str(), got.threw ? "THROW " : "", got.text.c_str(), got.type.c_str());
        }
    }
    std::fprintf(stderr, "%d snippets, %d differ\n", checked, failed);
    CHECK_EQ(failed, 0);
}

// The same programs with the baseline compiler on from the first call (Eager) and after a few calls (Tiered, threshold 3),
// and the programs of snippets_jit.inc that only compiled code could get wrong.
TEST(everySnippetAgreesInCompiledCode)
{
    int failed = 0;
    auto check = [&](const char* source, bool eager) {
        const Outcome want = runOn(script::Context::Options::Core::Dyn, source);
        script::Context::Options options;
        options.language = script::Language::JavaScript;
        options.core = script::Context::Options::Core::Vm;
        options.jit.mode = eager ? script::JitOptions::Mode::Eager : script::JitOptions::Mode::Tiered;
        options.jit.threshold = 3;
        Outcome got;
        {
            script::Context ctx(options);
            try {
                script::Value v = ctx.eval(source);
                got.text = v.toString();
                got.type = script::toString(v.type());
                if (got.type == "int" || got.type == "double" || got.type == "long" || got.type == "float")
                    got.type = "number";
            } catch (const script::Error& e) {
                got.threw = true;
                got.code = e.code();
                got.text = e.what();
            }
        }
        if (!sameOutcome(want, got)) {
            ++failed;
            std::fprintf(stderr, "MISMATCH (%s): %s\n   dyn: %s%s [%s]\n    vm: %s%s [%s]\n", eager ? "eager" : "tiered", source,
                         want.threw ? "THROW " : "", want.text.c_str(), want.type.c_str(), got.threw ? "THROW " : "",
                         got.text.c_str(), got.type.c_str());
        }
    };
    for (const char* source : kSnippets) {
        check(source, true);
        check(source, false);
    }
    for (const char* source : kJitSnippets) {
        check(source, true);
        check(source, false);
    }
    CHECK_EQ(failed, 0);
}

// Every function that qualifies is left to compile on its first call (SCRIPT_VM_LAZY, whatever the sizes): the same programs
// give the same answers, except that an error of the language found while compiling shows when the function is first called.
TEST(everySnippetAgreesWhenFunctionsAreCompiledOnTheirFirstCall)
{
    int failed = 0;
    auto run = [](const char* source, bool lazy) {
        if (lazy)
            setenv("SCRIPT_VM_LAZY", "1", 1);
        const Outcome out = runOn(script::Context::Options::Core::Vm, source);
        unsetenv("SCRIPT_VM_LAZY");
        return out;
    };
    auto check = [&](const char* source) {
        const Outcome want = run(source, false);
        if (want.threw && want.code == script::ErrorCode::SyntaxError)
            return;
        const Outcome got = run(source, true);
        if (!sameOutcome(want, got)) {
            ++failed;
            std::fprintf(stderr, "MISMATCH (lazy): %s\n   eager: %s%s\n    lazy: %s%s\n", source, want.threw ? "THROW " : "",
                         want.text.c_str(), got.threw ? "THROW " : "", got.text.c_str());
        }
    };
    for (const char* source : kSnippets)
        check(source);
    for (const char* source : kJitSnippets)
        check(source);
    // Closures over what is around them, at several levels, in generators and async functions, in a function called at once.
    check("function mk(n) { var c = 0; const s = n; let t = 1; function inc() { c += s; return c; } "
          "var f = function named(k) { return k <= 0 ? 0 : k + named(k - 1) + c + s + t; }; return { inc: inc, f: f }; } "
          "var m = mk(3); m.inc(); m.inc(); String(m.f(4))");
    check("function run() { var v = 5; function* g(n) { for (var i = 0; i < n; i++) { yield i * (function (q) { return q + v; })(i); } } "
          "return [...g(4)].join(); } run()");
    check("function outer() { var v = 1; function mid() { var w = 2; function inner() { return v + w; } return inner(); } return mid(); } String(outer())");
    check("function f() { return x; } var r; try { f(); } catch (e) { r = e.name; } let x = 1; r + f()");
    // A name a nested function declares for itself is not the variable around it; one it only declares in an inner block is.
    check("function f() { var e = 1, t = 2; var g = function (e) { var t = e + 1; return function () { return e + t; }; }; "
          "e++; t++; return g(10)() + ',' + e + t; } f()");
    check("function f() { var x = 1; function g() { { let x = 5; } return x; } x = 2; return g(); } String(f())");
    check("function f(n) { var r = function r(k) { return k ? k + r(k - 1) : 0; }; return r(n); } String(f(4))");
    check("function f() { var a = 1; function g(b) { return function () { return function () { return a + b; }; }; } a = 5; return g(2)()(); } String(f())");
    check("function f() { var v = 1; function g(p = function () { return v; }) { var v = 2; return p(); } v = 3; return g(); } String(f())");
    CHECK_EQ(failed, 0);
}

TEST(everyAsyncSnippetAgreesWithTheDynamicEngine)
{
    int failed = 0;
    for (const char* source : kAsyncSnippets) {
        const Outcome want = runAsyncOn(script::Context::Options::Core::Dyn, source);
        const Outcome got = runAsyncOn(script::Context::Options::Core::Vm, source);
        if (want.threw != got.threw || want.text != got.text) {
            ++failed;
            std::fprintf(stderr, "MISMATCH: %s\n   dyn: %s%s\n    vm: %s%s\n", source, want.threw ? "THROW " : "", want.text.c_str(),
                         got.threw ? "THROW " : "", got.text.c_str());
        }
    }
    CHECK_EQ(failed, 0);
}

// What the language says where the dynamic engine does not: these run on the new core alone.
TEST(whereTheDynamicEngineDeviatesFromTheLanguage)
{
    struct Expect
    {
        const char* source;
        const char* value;
    };
    const Expect expected[] = {
        // A read-only property on a prototype stops a store from creating an own property of the same name, even at a site
        // that learned to add.
        {"var P = {}; function mk() { var o = Object.create(P); o.x = 1; return o; } mk(); mk(); "
         "Object.defineProperty(P, 'x', {value: 7, writable: false}); var o = mk(); String(o.x)",
         "7"},
        // A read of a `let` in its dead zone from a function that compiled code runs.
        {"function outer(k) { function g() { return x; } if (k) return g(); let x = 1; return g(); } var s = 0; "
         "for (var i = 0; i < 600; i++) s += outer(0); var r; try { outer(1); } catch (e) { r = e.name; } s + ' ' + r",
         "600 ReferenceError"},
        // The same where the property is not among the object's slots in place.
        {"var P = {}; function mk(i) { var o = Object.create(P); o.a = 1; o.b = 2; o.c = 3; o.d = 4; o.e = 5; o.f = i; return o; } "
         "var r = []; for (var i = 0; i < 5; i++) r.push(mk(i).f); Object.defineProperty(P, 'f', { value: 7, writable: false }); "
         "var o = mk(9); r.push(o.f); r.push(Object.keys(o).join()); r.join()",
         "0,1,2,3,4,7,a,b,c,d,e"},
        // A property that is not enumerable hides an enumerable one of the same name further up the chain.
        {"var P = { a: 1, b: 2, shared: 3 }; var C = Object.create(P); C.c = 1; C.shared = 4; "
         "Object.defineProperty(C, 'b', { value: 9, enumerable: false }); C[2] = 'x'; C[1] = 'y'; var r = []; "
         "for (var k in C) r.push(k); r.join()",
         "1,2,c,shared,a"},
        // A bound function can be constructed, with its bound arguments in front.
        {"function P(a, b) { this.s = a + b; } var B = P.bind(null, 1); new B(2).s", "3"},
        // A `let` read in its own initialiser is in its dead zone.
        {"var r; try { let v = (function () { return v; })(); } catch (e) { r = e.name; } r", "ReferenceError"},
        // A `let` of one `switch` case is in its dead zone when another case is entered.
        {"function t(k) { switch (k) { case 0: let x = 1; return x; case 1: try { return typeof x; } catch (e) { return e.name; } } } "
         "t(0) + ':' + t(1)",
         "1:ReferenceError"},
        {"function t(k) { switch (k) { case 0: const x = 1; return x; case 1: try { return String(x); } catch (e) { return e.name; } } } t(1)",
         "ReferenceError"},
        // A set's iterator goes on over what was added after it started, and skips what was deleted.
        {"var s = new Set([1, 2, 3]); var it = s[Symbol.iterator](); s.delete(1); String(it.next().value)", "2"},
        {"var s = new Set(); s.add(1); s.add(2); for (var x of s) { if (x < 5) s.add(x + 2); } [...s].join()", "1,2,3,4,5,6"},
        // The stack text starts with the error's own; `trim` takes every space of the language.
        {"var e = new Error('m'); String(e.stack.indexOf('Error: m'))", "0"},
        {"'x'.padEnd(3, 'ab') + ' abc '.trim().length + '\\u2003x\\u3000'.trim().length", "xab31"},
        // Code made at run time, when the host allows it.
        {"var f = new Function('a', 'b', 'return a * b'); String(f(3, 4))", "12"},
        {"String(eval('1 + 2') + eval('var zz = 5; zz * 2'))", "13"},
    };
    for (const Expect& e : expected) {
        const Outcome got = runOn(script::Context::Options::Core::Vm, e.source, true);
        if (got.threw || got.text != e.value)
            std::fprintf(stderr, "wanted %s from %s, got %s%s\n", e.value, e.source, got.threw ? "THROW " : "", got.text.c_str());
        CHECK(!got.threw);
        CHECK_EQ(got.text, std::string(e.value));
    }
}
