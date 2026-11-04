// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The core library: strings, Math, collections, Console, exceptions.
#include <memory>

#include "script/script.hpp"
#include "tests/test_util.hpp"

namespace {

script::Value run(const char* source)
{
    static std::vector<std::unique_ptr<script::Context>> contexts;
    contexts.push_back(std::make_unique<script::Context>());
    return contexts.back()->eval(source);
}

std::string text(const char* source)
{
    return run(source).toString();
}

}  // namespace

TEST(stringMethods)
{
    CHECK_EQ(text("return \"Hello World\".ToLower();"), "hello world");
    CHECK_EQ(text("return \"  pad  \".Trim();"), "pad");
    CHECK_EQ(text("return \"a-b-c\".Replace(\"-\", \"+\");"), "a+b+c");
    CHECK_EQ(run("return \"a,b,c\".Split(',').Length;").toInt(), 3);
    CHECK_EQ(text("return \"a,b,c\".Split(',')[1];"), "b");
    CHECK_EQ(run("return \"hello\".IndexOf(\"ll\");").toInt(), 2);
    CHECK_EQ(run("return \"hello\".IndexOf(\"zz\");").toInt(), -1);
    CHECK(run("return \"hello\".StartsWith(\"he\") && \"hello\".EndsWith(\"lo\");").toBool());
    CHECK(run("return \"hello\".Contains(\"ell\");").toBool());
    CHECK_EQ(text("return \"x\".PadLeft(4);"), "   x");
    CHECK_EQ(run("return \"abc\".CompareTo(\"abd\");").toInt(), -1);
    CHECK(run("return string.IsNullOrEmpty(\"\") && !string.IsNullOrEmpty(\"x\");").toBool());
    CHECK_EQ(text("return string.Format(\"{0}-{1}\", 1, \"two\");"), "1-two");
    CHECK_EQ(run("return \"abc\".ToCharArray().Length;").toInt(), 3);
}

TEST(stringBuilder)
{
    CHECK_EQ(text(R"(
        var sb = new System.Text.StringBuilder();
        for (int i = 0; i < 4; i++) sb.Append(i);
        return sb.ToString();
    )"),
             "0123");
    CHECK_EQ(run(R"(
        var sb = new System.Text.StringBuilder();
        sb.Append("abc");
        return sb.Length;
    )")
                 .toInt(),
             3);
}

TEST(math)
{
    CHECK_EQ(run("return Math.Abs(-5);").toInt(), 5);
    CHECK_EQ(run("return Math.Max(2, 9);").toInt(), 9);
    CHECK_EQ(run("return Math.Min(2L, 9L);").toLong(), 2);
    CHECK_EQ(run("return Math.Sqrt(16.0);").toDouble(), 4.0);
    CHECK_EQ(run("return Math.Pow(2, 10);").toDouble(), 1024.0);
    CHECK_EQ(run("return Math.Floor(3.7);").toDouble(), 3.0);
    CHECK(run("return Math.PI > 3.14 && Math.PI < 3.15;").toBool());
}

TEST(parsingAndConverting)
{
    CHECK_EQ(run("return int.Parse(\"42\");").toInt(), 42);
    CHECK_EQ(run("return double.Parse(\"1.5\");").toDouble(), 1.5);
    CHECK_EQ(run("return Convert.ToInt32(\"7\") + Convert.ToInt32(3.9);").toInt(), 10);
    CHECK_EQ(text("return Convert.ToString(42);"), "42");
    CHECK_EQ(run(R"(
        int n;
        bool ok = int.TryParse("nope", out n);
        return ok ? 1 : n - 1;
    )")
                 .toInt(),
             -1);
}

TEST(numberFormatting)
{
    CHECK_EQ(text("return (1.0 / 3.0).ToString(\"F3\");"), "0.333");
    CHECK_EQ(text("return 255.ToString(\"X\");"), "FF");
    CHECK_EQ(text("return 42.ToString(\"D5\");"), "00042");
    CHECK_EQ(text("return (1.5).ToString();"), "1.5");
    CHECK_EQ(text("return 1e20.ToString();"), "1E+20");
    CHECK_EQ(text("return (0.1 + 0.2).ToString();"), "0.30000000000000004");
}

TEST(listOperations)
{
    CHECK_EQ(run(R"(
        var list = new List<string>();
        list.Add("a");
        list.Insert(0, "b");
        list.Add("c");
        list.Remove("c");
        return list.Count * 100 + list.IndexOf("a");
    )")
                 .toInt(),
             201);
    CHECK_EQ(run(R"(
        var list = new List<int> { 3, 1, 2 };
        list[0] = 9;
        return list[0] + list.Count;
    )")
                 .toInt(),
             12);
    CHECK_EQ(run(R"(
        var list = new List<int>();
        list.Add(4);
        return list.ToArray()[0];
    )")
                 .toInt(),
             4);
}

TEST(dictionaryOperations)
{
    CHECK_EQ(run(R"(
        var map = new Dictionary<string, int> { { "a", 1 }, { "b", 2 } };
        int total = 0;
        foreach (string key in map.Keys) total += map[key];
        return total;
    )")
                 .toInt(),
             3);
    CHECK_EQ(run(R"(
        var map = new Dictionary<int, string>();
        map[1] = "one";
        string found;
        if (map.TryGetValue(1, out found)) return found.Length;
        return -1;
    )")
                 .toInt(),
             3);
    CHECK_EQ(run(R"(
        var map = new Dictionary<string, int>();
        map["x"] = 1;
        map.Remove("x");
        return map.Count;
    )")
                 .toInt(),
             0);
}

TEST(objectMembers)
{
    CHECK_EQ(text("return 42.ToString();"), "42");
    CHECK_EQ(text("return true.ToString();"), "True");
    CHECK_EQ(text(R"(
        class Money {
            private int amount;
            public Money(int a) { amount = a; }
            public override string ToString() { return amount + " cents"; }
        }
        object m = new Money(7);
        return m.ToString();
    )"),
             "7 cents");
    CHECK(run("object a = \"x\"; object b = \"x\"; return a.Equals(b);").toBool());
}

TEST(exceptionTypes)
{
    CHECK_EQ(run(R"(
        try { int[] a = new int[1]; a[5] = 1; }
        catch (IndexOutOfRangeException) { return 1; }
        return 0;
    )")
                 .toInt(),
             1);
    CHECK_EQ(run(R"(
        try { string s = null; return s.Length; }
        catch (NullReferenceException) { return 2; }
    )")
                 .toInt(),
             2);
    CHECK_EQ(run(R"(
        try { object o = "x"; int n = (int)o; return n; }
        catch (InvalidCastException) { return 3; }
    )")
                 .toInt(),
             3);
    CHECK_EQ(run(R"(
        try { int.Parse("not a number"); }
        catch (FormatException) { return 4; }
        return 0;
    )")
                 .toInt(),
             4);
    CHECK_EQ(run(R"(
        var map = new Dictionary<string, int>();
        try { return map["missing"]; }
        catch (KeyNotFoundException) { return 5; }
    )")
                 .toInt(),
             5);
}

TEST(exceptionsReachTheHost)
{
    script::Context ctx;
    bool caught = false;
    try {
        ctx.eval("throw new System.InvalidOperationException(\"from script\");");
    } catch (const script::Exception& error) {
        caught = true;
        CHECK_EQ(error.code(), script::ErrorCode::InvalidOperation);
        CHECK(std::string(error.what()).find("from script") != std::string::npos);
        CHECK(!error.stack().empty());
    }
    CHECK(caught);
}

TEST(charHelpers)
{
    CHECK(run("return char.IsDigit('7') && char.IsLetter('x');").toBool());
    CHECK_EQ(text("return char.ToUpper('a').ToString();"), "A");
}

namespace {
const char* const kCounts = R"(
    using System.Collections.Generic;
    var d = new Dictionary<string, int>();
    d["b"] = 2; d["a"] = 1; d["c"] = 3; d["gone"] = 9;
    d.Remove("gone");
)";
std::string foreachCase(const std::string& body)
{
    script::Context ctx;
    return ctx.eval(std::string(kCounts) + body, "<t>.cs", script::Language::CSharp).toString();
}
}  // namespace

TEST(foreachWalksADictionaryInInsertionOrder)
{
    CHECK_EQ(foreachCase(R"(string s = ""; foreach (var e in d) s += e.Key + e.Value; return s;)"), std::string("b2a1c3"));
    CHECK_EQ(foreachCase(R"(string s = ""; foreach (KeyValuePair<string, int> e in d) s += e.Key; return s;)"), std::string("bac"));
    CHECK_EQ(foreachCase(R"(string s = ""; foreach (var e in d) s += e.ToString(); return s;)"), std::string("[b, 2][a, 1][c, 3]"));
}

TEST(foreachDeconstructs)
{
    CHECK_EQ(foreachCase(R"(string s = ""; foreach (var (k, v) in d) s += k + v; return s;)"), std::string("b2a1c3"));
    CHECK_EQ(foreachCase(R"(int t = 0; foreach (var (_, v) in d) t += v; return t;)"), std::string("6"));
    CHECK_EQ(foreachCase(R"(string s = ""; foreach ((string k, int v) in d) s += k; return s;)"), std::string("bac"));
    // Any tuple sequence, and nested loops that each need their own hidden variable.
    CHECK_EQ(foreachCase(R"(var l = new List<(int, int)>(); l.Add((1, 2)); l.Add((3, 4));
                            int t = 0; foreach (var (a, b) in l) foreach (var (c, e) in l) t += a * c + b * e; return t;)"),
             std::string("52"));
}

TEST(foreachOverADictionaryHasOrdinaryControlFlow)
{
    CHECK_EQ(foreachCase(R"(int t = 0; foreach (var e in d) { if (e.Key == "a") continue; if (e.Key == "c") break; t += e.Value; } return t;)"), std::string("2"));
    CHECK_EQ(foreachCase(R"(int t = 0; var empty = new Dictionary<int, int>(); foreach (var e in empty) t++; return t;)"), std::string("0"));
    CHECK_EQ(foreachCase(R"(var fs = new List<System.Func<int>>(); foreach (var e in d) fs.Add(() => e.Value);
                            int t = 0; foreach (var f in fs) t = t * 10 + f(); return t;)"), std::string("213"));
}

TEST(foreachWalksTheEntriesAsTheyWereWhenItStarted)
{
    CHECK_EQ(foreachCase(R"(int n = 0; foreach (var e in d) { d.Remove(e.Key); d[e.Key + "x"] = 1; n++; } return n * 10 + d.Count;)"), std::string("33"));
}

TEST(aKeyValuePairIsReadOnlyAndTyped)
{
    script::Context ctx;
    CHECK(!ctx.diagnose(std::string(kCounts) + "foreach (var e in d) e.Value = 1;", "<t>.cs", nullptr, script::Language::CSharp).empty());
    CHECK(!ctx.diagnose(std::string(kCounts) + "foreach (KeyValuePair<int, int> e in d) { }", "<t>.cs", nullptr, script::Language::CSharp).empty());
    CHECK(!ctx.diagnose(std::string(kCounts) + "foreach (var (a, b, c) in d) { }", "<t>.cs", nullptr, script::Language::CSharp).empty());
    CHECK(!ctx.diagnose("foreach (var x in 5) { }", "<t>.cs", nullptr, script::Language::CSharp).empty());
}

TEST(aNullDictionaryThrowsInForeach)
{
    script::Context ctx;
    CHECK_THROWS(ctx.eval("System.Collections.Generic.Dictionary<string, int> d = null; foreach (var e in d) { } return 1;", "<t>.cs", script::Language::CSharp),
                 script::ErrorCode::NullReference);
}

TEST(foreachOverADictionarySurvivesAnImage)
{
    script::Context ctx;
    const std::vector<std::uint8_t> image =
        ctx.compile(std::string(kCounts) + R"(string s = ""; foreach (var (k, v) in d) s += k + v; return s;)", "<t>.cs",
                    script::Language::CSharp).serialize();
    script::Context other;
    CHECK_EQ(other.run(other.load(image.data(), image.size())).toString(), std::string("b2a1c3"));
}

TEST(foreachOverADictionaryInAGenericMethod)
{
    script::Context ctx;
    CHECK_EQ(ctx.eval(R"(
        using System.Collections.Generic;
        static class G { public static int Sum<K>(Dictionary<K, int> m) { int t = 0; foreach (var (_, v) in m) t += v; return t; } }
        var d = new Dictionary<string, int>(); d["a"] = 4; d["b"] = 5;
        return G.Sum(d);
    )", "<t>.cs", script::Language::CSharp).toInt(), 9);
}
