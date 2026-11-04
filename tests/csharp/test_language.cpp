// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The language itself: statements, expressions, classes, closures.
#include <memory>

#include "script/script.hpp"
#include "tests/test_util.hpp"

namespace {

/// A Value belongs to its Context and must not outlive it, so the contexts a
/// test makes are kept alive until the process ends. That is the whole reason
/// this is not a local variable.
script::Value run(const char* source)
{
    static std::vector<std::unique_ptr<script::Context>> contexts;
    contexts.push_back(std::make_unique<script::Context>());
    return contexts.back()->eval(source);
}

std::string output(const char* source)
{
    script::Context ctx;
    std::string out;
    ctx.setPrintHandler([&out](std::string_view text, bool) { out += text; });
    ctx.eval(source);
    return out;
}

std::string text(const script::Value& v)
{
    return v.toString();
}

}  // namespace

TEST(arithmetic)
{
    CHECK_EQ(run("return 1 + 2 * 3;").toInt(), 7);
    CHECK_EQ(run("return (1 + 2) * 3;").toInt(), 9);
    CHECK_EQ(run("return 7 / 2;").toInt(), 3);
    CHECK_EQ(run("return 7 % 3;").toInt(), 1);
    CHECK_EQ(run("return 7.0 / 2;").toDouble(), 3.5);
    CHECK_EQ(run("return -5 / 2;").toInt(), -2);
    CHECK_EQ(run("return 1L << 40;").toLong(), 1099511627776LL);
    CHECK_EQ(run("return 0xFF & 0x0F;").toInt(), 15);
    CHECK_EQ(run("return ~0;").toInt(), -1);
    CHECK_EQ(run("int x = 5; x += 3; x *= 2; return x;").toInt(), 16);
    CHECK_EQ(run("int x = 5; return x++ + x;").toInt(), 11);
    CHECK_EQ(run("int x = 5; return ++x + x;").toInt(), 12);
}

TEST(integerWidths)
{
    CHECK_EQ(run("byte b = 250; b += 10; return (int)b;").toInt(), 4);
    CHECK_EQ(run("return int.MaxValue + 1;").toInt(), -2147483647 - 1);
    CHECK_EQ(run("long big = 3000000000L; return big;").toLong(), 3000000000LL);
    CHECK_EQ(run("return (int)3.9;").toInt(), 3);
    CHECK_EQ(run("return (double)7 / 2;").toDouble(), 3.5);
    CHECK_EQ(run("char c = 'A'; c++; return c.ToString();").toString(), "B");
}

TEST(comparisonsAndLogic)
{
    CHECK(run("return 1 < 2;").toBool());
    CHECK(run("return !(1 > 2);").toBool());
    CHECK(run("return 1 < 2 && 3 > 2;").toBool());
    CHECK(run("return false || true;").toBool());
    CHECK(run("return \"a\" == \"a\";").toBool());
    CHECK(run("return \"a\" != \"b\";").toBool());
    CHECK(run("string s = null; return s == null;").toBool());
    CHECK_EQ(run("return 1 < 2 ? 10 : 20;").toInt(), 10);
    // && must not evaluate its right side when the left is false.
    CHECK_EQ(run("int n = 0; bool b = false && (n = 1) == 1; return n;").toInt(), 0);
}

TEST(controlFlow)
{
    CHECK_EQ(run("int t = 0; for (int i = 0; i < 10; i++) t += i; return t;").toInt(), 45);
    CHECK_EQ(run("int i = 0; while (i < 5) i++; return i;").toInt(), 5);
    CHECK_EQ(run("int i = 0; do { i++; } while (i < 3); return i;").toInt(), 3);
    CHECK_EQ(run("int t = 0; for (int i = 0; i < 10; i++) { if (i % 2 == 0) continue; t += i; } "
                 "return t;")
                 .toInt(),
             25);
    CHECK_EQ(run("int t = 0; for (int i = 0; i < 10; i++) { if (i == 5) break; t++; } return t;")
                 .toInt(),
             5);
    CHECK_EQ(run(R"(
        int Classify(int n) {
            switch (n) {
                case 0: return 100;
                case 1:
                case 2: return 200;
                default: return 300;
            }
        }
        return Classify(2) + Classify(0) + Classify(9);
    )")
                 .toInt(),
             600);
    CHECK_EQ(text(run(R"(
        string Name(string s) {
            switch (s) {
                case "a": return "alpha";
                case "b": return "beta";
                default: return "other";
            }
        }
        return Name("b");
    )")),
             "beta");
}

TEST(methodsAndOverloads)
{
    CHECK_EQ(run(R"(
        static class M {
            public static int Twice(int n) { return n * 2; }
            public static double Twice(double n) { return n * 2; }
            public static int Add(int a, int b = 10) => a + b;
        }
        return M.Twice(21) + M.Add(5) + (int)M.Twice(0.5);
    )")
                 .toInt(),
             58);
}

TEST(recursion)
{
    CHECK_EQ(run(R"(
        static class M {
            public static long Fib(int n) => n < 2 ? n : Fib(n - 1) + Fib(n - 2);
        }
        return M.Fib(20);
    )")
                 .toLong(),
             6765);
}

TEST(classesAndInheritance)
{
    CHECK_EQ(text(run(R"(
        class Animal {
            protected string name;
            public Animal(string n) { name = n; }
            public virtual string Speak() { return name + " makes a sound"; }
        }
        class Dog : Animal {
            public Dog(string n) : base(n) { }
            public override string Speak() { return name + " barks"; }
        }
        Animal a = new Dog("Rex");
        return a.Speak();
    )")),
             "Rex barks");
}

TEST(propertiesAndFields)
{
    CHECK_EQ(run(R"(
        class Counter {
            private int count;
            public int Count { get { return count; } }
            public int Step { get; set; }
            public Counter() { Step = 1; }
            public void Tick() { count += Step; }
        }
        var c = new Counter();
        c.Step = 5;
        c.Tick();
        c.Tick();
        return c.Count;
    )")
                 .toInt(),
             10);
}

TEST(structsAreCopied)
{
    CHECK_EQ(run(R"(
        struct Point {
            public int X;
            public int Y;
            public Point(int x, int y) { X = x; Y = y; }
        }
        Point a = new Point(1, 2);
        Point b = a;
        b.X = 99;
        return a.X;
    )")
                 .toInt(),
             1);
}

TEST(interfaces)
{
    CHECK_EQ(run(R"(
        interface IShape { int Area(); }
        class Square : IShape {
            private int side;
            public Square(int s) { side = s; }
            public int Area() { return side * side; }
        }
        IShape s = new Square(6);
        return s.Area();
    )")
                 .toInt(),
             36);
}

TEST(enums)
{
    CHECK_EQ(run(R"(
        enum Color { Red, Green = 10, Blue }
        Color c = Color.Blue;
        return (int)c;
    )")
                 .toInt(),
             11);
}

TEST(arrays)
{
    CHECK_EQ(run("int[] a = new int[4]; a[2] = 7; return a[2] + a.Length;").toInt(), 11);
    CHECK_EQ(run("int[] a = { 1, 2, 3 }; int t = 0; foreach (int n in a) t += n; return t;")
                 .toInt(),
             6);
    CHECK_EQ(run("string[] s = new string[] { \"a\", \"b\" }; return s[0] + s[1];").toString(),
             "ab");
    CHECK_EQ(run("int[] a = { 5, 6 }; a[0] += 10; return a[0];").toInt(), 15);
}

TEST(strings)
{
    CHECK_EQ(text(run("return \"abc\".ToUpper();")), "ABC");
    CHECK_EQ(run("return \"hello\".Length;").toInt(), 5);
    CHECK_EQ(text(run("return \"hello\".Substring(1, 3);")), "ell");
    CHECK_EQ(run("return \"hello\"[1].ToString();").toString(), "e");
    CHECK_EQ(text(run("int n = 42; return $\"n = {n}\";")), "n = 42");
    CHECK_EQ(text(run("double d = 3.14159; return $\"{d:F2}\";")), "3.14");
    CHECK_EQ(text(run("return string.Join(\",\", new string[] { \"a\", \"b\" });")), "a,b");
    CHECK_EQ(text(run("return 1 + \"x\" + 2;")), "1x2");
}

TEST(lambdasAndDelegates)
{
    CHECK_EQ(run(R"(
        Func<int, int> square = x => x * x;
        return square(7);
    )")
                 .toInt(),
             49);
    CHECK_EQ(run(R"(
        int total = 0;
        Action<int> add = n => { total += n; };
        add(3);
        add(4);
        return total;
    )")
                 .toInt(),
             7);
    CHECK_EQ(run(R"(
        static class M {
            public static Func<int, int> Adder(int by) {
                return x => x + by;
            }
        }
        var add5 = M.Adder(5);
        return add5(10);
    )")
                 .toInt(),
             15);
    CHECK_EQ(run(R"(
        delegate int Op(int a, int b);
        static class M { public static int Sum(int a, int b) { return a + b; } }
        Op op = M.Sum;
        return op(20, 22);
    )")
                 .toInt(),
             42);
}

TEST(exceptions)
{
    CHECK_EQ(text(run(R"(
        try {
            throw new Exception("boom");
        } catch (Exception e) {
            return e.Message;
        }
    )")),
             "boom");
    CHECK_EQ(run(R"(
        int n = 0;
        try {
            int z = 0;
            n = 10 / z;
        } catch (DivideByZeroException) {
            n = -1;
        } finally {
            n += 100;
        }
        return n;
    )")
                 .toInt(),
             99);
    CHECK_EQ(run(R"(
        static class M {
            public static int Deep(int n) {
                if (n == 0) throw new InvalidOperationException("bottom");
                return Deep(n - 1);
            }
        }
        try { M.Deep(5); } catch (InvalidOperationException) { return 1; }
        return 0;
    )")
                 .toInt(),
             1);
}

TEST(collections)
{
    CHECK_EQ(run(R"(
        var list = new List<int>();
        list.Add(1);
        list.Add(2);
        list.Add(3);
        int total = 0;
        foreach (int n in list) total += n;
        return total + list.Count;
    )")
                 .toInt(),
             9);
    CHECK_EQ(run(R"(
        var map = new Dictionary<string, int>();
        map["a"] = 1;
        map["b"] = 2;
        map["a"] += 10;
        return map["a"] + map.Count;
    )")
                 .toInt(),
             13);
    CHECK(run(R"(
        var map = new Dictionary<string, int>();
        map.Add("x", 1);
        return map.ContainsKey("x") && !map.ContainsKey("y");
    )")
              .toBool());
}

TEST(typeTests)
{
    CHECK(run(R"(
        object o = "text";
        return o is string;
    )")
              .toBool());
    CHECK_EQ(text(run(R"(
        object o = "text";
        string s = o as string;
        return s;
    )")),
             "text");
    CHECK_EQ(run(R"(
        object o = 42;
        return (int)o + 1;
    )")
                 .toInt(),
             43);
}

TEST(consoleOutput)
{
    CHECK_EQ(output("Console.WriteLine(\"one\"); Console.Write(\"two\");"), "one\ntwo");
    CHECK_EQ(output("Console.WriteLine(3 * 4);"), "12\n");
}

TEST(staticState)
{
    CHECK_EQ(run(R"(
        static class Registry {
            public static int Count = 0;
            public static void Bump() { Count++; }
        }
        Registry.Bump();
        Registry.Bump();
        return Registry.Count;
    )")
                 .toInt(),
             2);
}

TEST(refAndOut)
{
    CHECK_EQ(run(R"(
        static class M {
            public static void Swap(ref int a, ref int b) { int t = a; a = b; b = t; }
        }
        int x = 1, y = 2;
        M.Swap(ref x, ref y);
        return x * 10 + y;
    )")
                 .toInt(),
             21);
    CHECK_EQ(run(R"(
        int value;
        if (int.TryParse("123", out value)) return value;
        return -1;
    )")
                 .toInt(),
             123);
}

TEST(nullHandling)
{
    CHECK_EQ(text(run("string s = null; return s ?? \"fallback\";")), "fallback");
    // `?.` on a value-typed member yields null at run time; there are no
    // nullable value types, so the result is read through `??`.
    CHECK_EQ(run("string s = null; return s?.Length ?? -1;").toInt(), -1);
    CHECK_EQ(run("string s = \"abc\"; return s?.Length ?? 0;").toInt(), 3);
}
