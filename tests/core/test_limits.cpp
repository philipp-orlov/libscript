// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Hostile and pathological input. A program the host did not write -- deeply nested source, a cycle in data, a size
// chosen to exhaust memory, a callback that changes the array it is called from -- must end in an error the host can
// catch, never in a crash and never in an allocation the memory limit did not see.
#include <pthread.h>

#include <chrono>
#include <string>

#include "script/script.hpp"
#include "tests/test_util.hpp"

namespace {

using script::Context;
using script::Error;
using script::ErrorCode;
using script::Language;

std::string repeat(const std::string& unit, std::size_t n)
{
    std::string out;
    out.reserve(unit.size() * n);
    for (std::size_t i = 0; i < n; ++i)
        out += unit;
    return out;
}

/// Run `source` and return the error code it ended with, or `fallthrough` when it ran to the end.
ErrorCode outcome(Context& ctx, const std::string& source, const char* name, ErrorCode fallthrough = ErrorCode::Ok)
{
    try {
        (void)ctx.eval(source, name);
    } catch (const Error& error) {
        return error.code();
    }
    return fallthrough;
}

bool isRefusal(ErrorCode code)
{
    return code == ErrorCode::SyntaxError || code == ErrorCode::CompileError || code == ErrorCode::StackOverflow;
}

struct Job
{
    std::string source;
    std::string name;
    ErrorCode code = ErrorCode::Ok;
    bool finished = false;
};

void* runJob(void* arg)
{
    Job* job = static_cast<Job*>(arg);
    Context ctx;
    job->code = outcome(ctx, job->source, job->name.c_str());
    job->finished = true;
    return nullptr;
}

/// Run a job on a thread whose stack is only `bytes` long, the way a host with small worker threads would.
Job runOnSmallStack(std::string source, std::string name, std::size_t bytes)
{
    Job job;
    job.source = std::move(source);
    job.name = std::move(name);
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, bytes);
    pthread_t thread;
    if (pthread_create(&thread, &attr, runJob, &job) != 0) {
        pthread_attr_destroy(&attr);
        ::tst::fail(__FILE__, __LINE__, "could not start a thread");
    }
    pthread_join(thread, nullptr);
    pthread_attr_destroy(&attr);
    return job;
}

}  // namespace

// ----------------------------------------------------------- nesting ---

#if SCRIPT_LANG_JAVASCRIPT
TEST(javascriptRefusesSourceNestedTooDeeply)
{
    Context ctx;
    CHECK(isRefusal(outcome(ctx, repeat("(", 10000) + "1" + repeat(")", 10000), "deep.js")));
    CHECK(isRefusal(outcome(ctx, repeat("[", 10000) + repeat("]", 10000), "deep.js")));
    CHECK(isRefusal(outcome(ctx, "1" + repeat("+1", 10000), "long.js")));
    CHECK(isRefusal(outcome(ctx, "var a = {}; a" + repeat(".b", 10000), "long.js")));
    // What real code does stays well inside the limits.
    CHECK_EQ(int(outcome(ctx, repeat("(", 100) + "1" + repeat(")", 100), "ok.js")), int(ErrorCode::Ok));
    CHECK_EQ(int(outcome(ctx, "1" + repeat("+1", 1500), "ok.js")), int(ErrorCode::Ok));
}

TEST(jsonRefusesDeepAndCyclicData)
{
    Context ctx;
    CHECK(outcome(ctx, "JSON.parse('" + repeat("[", 10000) + repeat("]", 10000) + "')", "deep.js") != ErrorCode::Ok);
    CHECK(outcome(ctx, "var o = {}; o.o = o; JSON.stringify(o)", "cycle.js") == ErrorCode::TypeError);
    CHECK(outcome(ctx, "var a = []; a.push(a); JSON.stringify(a)", "cycle.js") == ErrorCode::TypeError);
    CHECK(outcome(ctx,
                  "var a = []; for (var i = 0; i < 10000; i++) a = [a]; JSON.stringify(a)", "deep.js") !=
          ErrorCode::Ok);
    // Shared, not cyclic, is fine.
    CHECK_EQ(int(outcome(ctx, "var s = [1]; JSON.stringify([s, s, {a: s}])", "shared.js")), int(ErrorCode::Ok));
    CHECK_EQ(int(outcome(ctx, "JSON.parse('" + repeat("[", 500) + repeat("]", 500) + "')", "ok.js")),
             int(ErrorCode::Ok));
}

TEST(flatAcceptsInfinityAndRefusesAbsurdNesting)
{
    Context ctx;
    CHECK_EQ(int(outcome(ctx, "[1,[2,[3,[4]]]].flat(Infinity).length", "flat.js")), int(ErrorCode::Ok));
    CHECK_EQ(Context().eval("[1,[2,[3,[4]]]].flat(Infinity).length", "flat.js").toInt(), 4);
    CHECK(outcome(ctx, "var a = []; for (var i = 0; i < 100000; i++) a = [a]; a.flat(Infinity)", "deep.js") !=
          ErrorCode::Ok);
}
#endif

#if SCRIPT_LANG_PYTHON
TEST(pythonRefusesSourceNestedTooDeeply)
{
    Context ctx;
    CHECK(isRefusal(outcome(ctx, "x = " + repeat("[", 10000) + repeat("]", 10000) + "\n", "deep.py")));
    CHECK(isRefusal(outcome(ctx, "x = 1" + repeat("+1", 10000) + "\n", "long.py")));
}

TEST(pythonListMultiplicationIsLinear)
{
    // It used to concatenate the whole result on every pass, which is quadratic: this took minutes.
    Context ctx;
    const auto begin = std::chrono::steady_clock::now();
    CHECK_EQ(ctx.eval("len([0] * 300000)", "mul.py").toInt(), 300000);
    CHECK_EQ(ctx.eval("len([1, 2, 3] * 100000)", "mul.py").toInt(), 300000);
    CHECK_EQ(ctx.eval("len([0] * 0) + len([0] * -3)", "mul.py").toInt(), 0);
    const auto elapsed = std::chrono::steady_clock::now() - begin;
#if !defined(__SANITIZE_ADDRESS__) && !defined(__SANITIZE_THREAD__)
    CHECK(elapsed < std::chrono::seconds(10));  // a sanitizer build is far too slow for a wall-clock bound
#else
    (void)elapsed;
#endif
}
#endif

#if SCRIPT_LANG_CSHARP
TEST(csharpRefusesSourceNestedTooDeeply)
{
    Context ctx;
    CHECK(isRefusal(outcome(ctx, "class P { static int Main() { return " + repeat("(", 10000) + "1" + repeat(")", 10000) + "; } }",
                            "deep.cs")));
    CHECK(isRefusal(outcome(ctx, "class P { static int Main() { return 1" + repeat("+1", 10000) + "; } }", "long.cs")));
}
#endif

#if SCRIPT_LANG_C
TEST(cRefusesSourceNestedTooDeeply)
{
    Context ctx;
    CHECK(isRefusal(outcome(ctx, "int main() { return " + repeat("(", 10000) + "1" + repeat(")", 10000) + "; }", "deep.c")));
    CHECK(isRefusal(outcome(ctx, "int main() { return 1" + repeat("+1", 10000) + "; }", "long.c")));
}
#endif

// ------------------------------------------------------- small stacks ---

#if SCRIPT_LANG_JAVASCRIPT
TEST(smallThreadStackGivesAnErrorNotACrash)
{
    // 256 KiB is what some hosts give a worker thread; the default call limit of 512 needs more than that.
    const std::size_t stack = 256 * 1024;
    Job recursion = runOnSmallStack("function f() { return f(); } f()", "recurse.js", stack);
    CHECK(recursion.finished);
    CHECK(recursion.code == ErrorCode::StackOverflow);

    Job nesting = runOnSmallStack(repeat("(", 5000) + "1" + repeat(")", 5000), "deep.js", stack);
    CHECK(nesting.finished);
    CHECK(isRefusal(nesting.code));

    Job chain = runOnSmallStack("1" + repeat("+1", 1500), "long.js", stack);
    CHECK(chain.finished);
    CHECK(chain.code == ErrorCode::Ok || isRefusal(chain.code));
}
#endif

#if SCRIPT_LANG_CSHARP
TEST(smallThreadStackGivesAnErrorNotACrashInCsharp)
{
    const std::size_t stack = 256 * 1024;
    Job recursion = runOnSmallStack("class P { static int F(int x) { return F(x + 1); } static int Main() { return F(0); } }",
                                    "recurse.cs", stack);
    CHECK(recursion.finished);
    CHECK(recursion.code == ErrorCode::StackOverflow);

    Job chain = runOnSmallStack("class P { static int Main() { return 1" + repeat("+1", 1500) + "; } }", "long.cs", stack);
    CHECK(chain.finished);
    CHECK(chain.code == ErrorCode::Ok || isRefusal(chain.code));
}

TEST(csharpStringAndListSizesRespectTheMemoryLimit)
{
    Context::Options options;
    options.memoryLimit = 32u << 20;
    Context ctx(options);
    // Each of these built gigabytes in a temporary before the limit was consulted.
    CHECK(outcome(ctx, "class A { static int Main() { return \"x\".PadLeft(2000000000).Length; } }", "a.cs") ==
          ErrorCode::OutOfMemory);
    CHECK(outcome(ctx, "class B { static int Main() { return \"x\".PadRight(2000000000).Length; } }", "b.cs") ==
          ErrorCode::OutOfMemory);
    CHECK(outcome(ctx,
                  "using System.Collections.Generic; class C { static int Main() { var l = new List<int>(2000000000); return l.Count; } }",
                  "c.cs") == ErrorCode::OutOfMemory);
    CHECK(outcome(ctx,
                  "class D { static int Main() { string big = \"abcdefghij\"; for (int i = 0; i < 18; i++) big = big + big;"
                  " string t = \"aaaaaaaaaa\"; for (int i = 0; i < 12; i++) t = t + t; return t.Replace(\"a\", big).Length; } }",
                  "d.cs") == ErrorCode::OutOfMemory);
    // The ordinary forms still work, including the overloads with a padding character.
    CHECK_EQ(ctx.eval("class E { static string Main() { return \"7\".PadLeft(3, (char)48) + \"|\" + \"ab\".PadRight(4) + \"|\" + \"toolong\".PadLeft(3); } }",
                      "e.cs")
                 .toString(),
             std::string("007|ab  |toolong"));
}
#endif

#if SCRIPT_LANG_CPP
TEST(cppStringFillRespectsTheMemoryLimit)
{
    Context::Options options;
    options.memoryLimit = 32u << 20;
    Context ctx(options);
    CHECK(outcome(ctx, "#include <string>\nint main() { std::string s(2000000000, (char)97); return (int)s.size(); }", "s.cpp") ==
          ErrorCode::OutOfMemory);
    CHECK_EQ(ctx.eval("#include <string>\nint main() { std::string s(5, (char)97); return (int)s.size(); }", "ok.cpp").toInt(), 5);
}
#endif

#if SCRIPT_LANG_CSHARP
TEST(interpreterAndNativeCodeAgreeOnConversions)
{
    // The same program must give the same answer whether a method is interpreted or compiled, or it changes
    // behaviour the moment it gets hot. These two conversions used to differ: an infinity converted to an integer was
    // 0 interpreted and a saturated value native, and (double)(uint)-1 was 1.8e19 interpreted and 4294967295 native.
    const char* program =
        "class P { static string Main() {"
        "  double big = 1e300; double inf = big * big; double ninf = -inf; double nan = inf - inf;"
        "  int minusOne = -1; long wide = -9223372036854775807L;"
        "  return ((int)inf).ToString() + \",\" + ((int)ninf).ToString() + \",\" + ((int)nan).ToString() + \",\" +"
        "         ((long)inf).ToString() + \",\" + ((long)ninf).ToString() + \",\" + ((short)inf).ToString() + \",\" +"
        "         ((double)(uint)minusOne).ToString() + \",\" + ((double)(uint)(minusOne * 2)).ToString() + \",\" +"
        "         ((int)(float)inf).ToString() + \",\" + ((double)(ulong)wide).ToString(); } }";
    std::string results[2];
    for (int native = 0; native < 2; ++native) {
        Context::Options options;
        options.jit.mode = native ? script::JitOptions::Mode::Eager : script::JitOptions::Mode::Off;
        Context ctx(options);
        results[native] = ctx.eval(program, "convert.cs").toString();
    }
    CHECK_EQ(results[0], results[1]);
    CHECK(results[0].find("4294967295") != std::string::npos);
    CHECK(results[0].rfind("0,0,0,0,0,0,", 0) == 0);
}

TEST(unsignedValuesPrintAsUnsigned)
{
    // A uint is an int slot and a ulong a long slot, so printing them by tag showed 4000000000u as -294967296.
    const char* program =
        "class P { static string Main() {"
        "  uint u = 4000000000u; ulong ul = 18446744073709551615UL; ulong big = 10000000000000000000UL;"
        "  string s = \"n=\"; s += u; s += \",\"; s += ul;"
        "  return u.ToString() + \",\" + ul.ToString() + \",\" + big + \",\" + (u + u) + \",\" + $\"{u}|{ul}\" + \",\" + s +"
        "         \",\" + (ushort)65535 + \",\" + (-3) + \",\" + 5000000000L; } }";
    const std::string expected =
        "4000000000,18446744073709551615,10000000000000000000,3705032704,4000000000|18446744073709551615,"
        "n=4000000000,18446744073709551615,65535,-3,5000000000";
    for (int native = 0; native < 2; ++native) {
        Context::Options options;
        options.jit.mode = native ? script::JitOptions::Mode::Eager : script::JitOptions::Mode::Off;
        Context ctx(options);
        CHECK_EQ(ctx.eval(program, "unsigned.cs").toString(), expected);
    }
}

TEST(jitPollsTheInterruptHandlerSparingly)
{
    // Native code used to call the handler on every loop iteration -- a clock read each time -- where the
    // interpreter asks once in 4096 instructions.
    Context::Options options;
    options.jit.mode = script::JitOptions::Mode::Eager;
    Context ctx(options);
    long polls = 0;
    ctx.setInterruptHandler([&] { ++polls; return false; });
    ctx.eval("class P { static long Main() { long s = 0; for (int i = 0; i < 5000000; i++) s += i; return s; } }", "loop.cs");
    CHECK(polls < 20000);
    // And it must still stop a loop that never ends.
    long calls = 0;
    ctx.setInterruptHandler([&] { return ++calls > 50; });
    CHECK(outcome(ctx, "class Q { static void Main() { long s = 0; while (true) s++; } }", "forever.cs") ==
          ErrorCode::Interrupted);
}
#endif

#if SCRIPT_LANG_JAVASCRIPT
TEST(anInterruptHandlerThatEntersTheContextStopsNativeLoops)
{
    // Native code in a loop has what it knows about its objects in registers, so it cannot go on from a change made under it.
    Context ctx;
    ctx.eval("var a = [1, 2, 3];", "setup.js");
    std::uint64_t seen = 0;
    ctx.setInterruptHandler([&] {
        seen = ctx.stats().liveObjects;  // a look is fine
        return false;
    });
    CHECK_EQ(ctx.eval("var t = 0; for (var i = 0; i < 200000; i++) t += a[i & 1]; t", "look.js").toInt(), 300000);
    CHECK(seen > 0);
    ctx.setInterruptHandler([&] {
        ctx.eval("a.length = 0", "under.js");
        return false;
    });
    CHECK(outcome(ctx, "var t = 0; for (var i = 0; i < 200000; i++) t += a[i & 1]; t", "touch.js") == ErrorCode::Interrupted);
}
#endif

// ------------------------------------------------------- memory limit ---

#if SCRIPT_LANG_JAVASCRIPT
TEST(memoryLimitHoldsForSizesAProgramChooses)
{
    Context::Options options;
    options.memoryLimit = 32u << 20;
    Context ctx(options);
    // Each of these used to allocate gigabytes before anything looked at the limit.
    CHECK(outcome(ctx, "new Array(1e9).length", "big.js") == ErrorCode::OutOfMemory);
    CHECK(outcome(ctx, "var a = []; a.length = 4294967295; a.length", "big.js") == ErrorCode::OutOfMemory);
    CHECK(outcome(ctx, "new Array(1e8).join('xxxxxxxx').length", "big.js") == ErrorCode::OutOfMemory);
    CHECK(outcome(ctx, "var a = new Array(2e6).fill(0); a.concat(a, a, a, a, a, a, a, a, a)", "big.js") ==
          ErrorCode::OutOfMemory);

    // The loop that builds {length: 1e9} must stop at the limit, and quickly.
    const auto begin = std::chrono::steady_clock::now();
    CHECK(outcome(ctx, "Array.from({length: 1e9}).length", "big.js") == ErrorCode::OutOfMemory);
    CHECK(std::chrono::steady_clock::now() - begin < std::chrono::seconds(10));

    // The context is still usable afterwards, and ordinary work fits.
    CHECK_EQ(ctx.eval("var a = []; for (var i = 0; i < 1000; i++) a.push(i); a.length", "ok.js").toInt(), 1000);
}

TEST(infiniteIteratorsStopAtTheLimits)
{
    // Each step of the iterator is a short call, too short to reach the interpreter's own poll, so the loop that
    // collects the values has to check by itself. It used to run until the process was killed.
    const char* source =
        "var it = {[Symbol.iterator]() { return { next() { return { value: 1, done: false }; } }; }};"
        "Array.from(it).length";
    {
        Context::Options options;
        options.memoryLimit = 32u << 20;
        Context ctx(options);
        CHECK(outcome(ctx, source, "inf.js") == ErrorCode::OutOfMemory);
    }
    {
        Context ctx;
        int polls = 0;
        ctx.setInterruptHandler([&] { return ++polls > 3; });
        CHECK(outcome(ctx, source, "inf.js") == ErrorCode::Interrupted);
    }
}

TEST(interruptStopsLongBuiltins)
{
    Context ctx;
    int polls = 0;
    ctx.setInterruptHandler([&] { return ++polls > 3; });
    CHECK(outcome(ctx, "Array.from({length: 4000000000}).length", "long.js") == ErrorCode::Interrupted);
}

TEST(stringSizesAreCheckedWithoutWrapping)
{
    Context ctx;
    // 4 * 2**62 wraps to zero in 64 bits, which used to pass the size check and then loop for ever.
    CHECK(outcome(ctx, "'abcd'.repeat(Math.pow(2, 62))", "wrap.js") == ErrorCode::RangeError);
    CHECK_EQ(ctx.eval("''.repeat(Math.pow(2, 62)).length", "empty.js").toInt(), 0);
    CHECK_EQ(ctx.eval("'ab'.repeat(3)", "ok.js").toString(), std::string("ababab"));
    // A pad length past the limit must say so instead of quietly returning the unpadded string.
    CHECK(outcome(ctx, "'x'.padStart(1e10)", "pad.js") == ErrorCode::RangeError);
    CHECK_EQ(ctx.eval("'7'.padStart(3, '0')", "ok.js").toString(), std::string("007"));
}

// ---------------------------------------- callbacks that change the array ---

TEST(arrayMethodsSurviveCallbacksThatShrinkTheArray)
{
    // Run under the sanitizers these read past the end of the array before the fix.
    Context ctx;
    CHECK_EQ(ctx.eval("var a = [1, 2, 3, 4, 5, 6, 7, 8]; a.reduce(function (s, x) { a.length = 1; return s + x; }, 0)",
                      "reduce.js")
                 .toInt(),
             1);
    CHECK_EQ(ctx.eval("var a = [1, 2, 3, 4, 5, 6, 7, 8]; a.reduceRight(function (s, x) { a.length = 1; return s + x; }, 0)",
                      "reduceRight.js")
                 .toInt(),
             9);  // 8 first, then the elements that are gone are skipped, then index 0
    CHECK_EQ(int(outcome(ctx,
                         "var a = []; for (var i = 0; i < 64; i++) a.push(64 - i);"
                         "a.sort(function (x, y) { a.length = 0; return x - y; });",
                         "sort.js")),
             int(ErrorCode::Ok));
    CHECK_EQ(int(outcome(ctx,
                         "var a = []; for (var i = 0; i < 64; i++) a.push(64 - i);"
                         "a.sort(function (x, y) { a.push(1, 2, 3); return x - y; });",
                         "sort.js")),
             int(ErrorCode::Ok));
    CHECK_EQ(ctx.eval("var a = [3, 1, 2]; a.sort(function (x, y) { return x - y; }); a.join(',')", "sorted.js")
                 .toString(),
             std::string("1,2,3"));
}
#endif
