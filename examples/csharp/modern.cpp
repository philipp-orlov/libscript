// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// What the language looks like now: the C# that arrived after version 6.
//
// Everything here compiles to the same bytecode the older syntax does -- a
// pattern becomes the comparisons it stands for, a record becomes the members
// it is shorthand for, a collection expression becomes the loop that fills the
// collection. None of it costs anything at run time that writing it out by
// hand would not have cost.
#include "script/script.hpp"

#include <cstdio>
#include <string>

namespace {

const char* kSource = R"CS(
using System;
using System.Collections.Generic;

// ---- C# 9: a record is its data ----------------------------------------
// One line declares the properties, the constructor, Deconstruct, ToString,
// value equality, a hash code, and the copy that `with` uses.
record Money(string Currency, double Amount)
{
    public string Show() => Amount + " " + Currency;
}

// ---- C# 9: init-only properties ----------------------------------------
// Settable while the object is being built, read-only afterwards.
class Settings
{
    public string Name { get; init; }
    public int Retries { get; init; }
    public override string ToString() => Name + " x" + Retries;
}

// ---- C# 12: a primary constructor on a plain class ---------------------
class Counter(string label)
{
    private int count;
    public void Tick() { count++; }
    public override string ToString() => label + "=" + count;
}

// ---- C# 2: generics, reified ------------------------------------------
// Stack<int> and Stack<string> are separate types; an int never boxes to
// satisfy T.
class Stack<T>
{
    private readonly List<T> items = new List<T>();
    public void Push(T item) { items.Add(item); }
    public T Pop()
    {
        T top = items[items.Count - 1];
        items.RemoveAt(items.Count - 1);
        return top;
    }
    public int Count => items.Count;
}

static class Rules
{
    // ---- C# 8: switch expressions over relational patterns -------------
    public static string Band(int n) => n switch
    {
        < 0 => "negative",
        0 => "zero",
        > 0 and < 10 => "small",
        >= 10 and < 100 => "medium",
        _ => "large",
    };

    // ---- C# 7-9: type, property, constant and logical patterns ---------
    public static string Describe(object o) => o switch
    {
        null => "nothing",
        int i when i % 2 == 0 => "even " + i,
        int i => "odd " + i,
        string { Length: 0 } => "an empty string",
        string s => "a string of " + s.Length,
        Money { Currency: "EUR", Amount: > 100 } m => "a lot of euros: " + m.Amount,
        Money { Currency: "EUR" } m => "euros: " + m.Amount,
        Money m => m.Show(),
        _ => "something else",
    };

    // ---- C# 11: list patterns -----------------------------------------
    public static string Summarize(int[] values) => values switch
    {
        [] => "empty",
        [var only] => "just " + only,
        [var first, var second] => first + " and " + second,
        [var first, .., var last] => first + " through " + last,
    };

    // ---- C# 2: a generic method, with the type argument inferred -------
    public static T Larger<T>(T a, T b, Func<T, int> weigh) =>
        weigh(a) >= weigh(b) ? a : b;
}

// ---- C# 9: top-level statements ----------------------------------------

// C# 12: collection expressions build whatever they are assigned to.
int[] readings = [5, 3, 9, 1, 7];
List<string> names = ["delta", "alpha", "charlie"];

// C# 8: indices from the end, and ranges.
int newest = readings[^1];
int[] middle = readings[1..4];
string headline = "quarterly report"[0..9];

// C# 7: tuples, with names, and deconstruction.
(string unit, double total) = ("EUR", 512.5);
var money = new Money(unit, total);
var (currency, amount) = money;

// C# 7: out var, discards, and a local function that can recurse.
int.TryParse("42", out var answer);
int.TryParse("not a number", out _);
int Factorial(int n) => n <= 1 ? 1 : n * Factorial(n - 1);

// C# 9: a copy with one thing changed. The original is untouched.
var discounted = money with { Amount = 99.0 };

// C# 11: a raw string keeps its quotes and drops the indentation.
var template = """
    {
      "currency": "EUR",
      "note": "no escaping needed"
    }
    """;

Console.WriteLine("bands:      " + Rules.Band(-1) + ", " + Rules.Band(0) + ", " +
                  Rules.Band(5) + ", " + Rules.Band(50) + ", " + Rules.Band(500));
Console.WriteLine("patterns:   " + Rules.Describe(null) + " | " + Rules.Describe(4) +
                  " | " + Rules.Describe("abc") + " | " + Rules.Describe(money));
Console.WriteLine("lists:      " + Rules.Summarize(new int[0]) + " | " +
                  Rules.Summarize(new int[] { 7 }) + " | " + Rules.Summarize(readings));
Console.WriteLine("ranges:     newest=" + newest + " middle=" + middle.Length +
                  " headline=" + headline);
Console.WriteLine("tuples:     " + currency + " " + amount);
Console.WriteLine("out/local:  " + answer + ", 5! = " + Factorial(5));
Console.WriteLine("with:       " + discounted + "  original untouched: " + money);
Console.WriteLine("equality:   " + money.Equals(new Money("EUR", 512.5)) +
                  " (records compare by value)");
Console.WriteLine("collection: " + names[1] + ", " + readings[0]);
Console.WriteLine("raw string:");
Console.WriteLine(template);

var stack = new Stack<string>();
stack.Push("first");
stack.Push("second");
Console.WriteLine("generics:   " + stack.Pop() + ", " + stack.Count + " left");

var settings = new Settings { Name = "retry-policy", Retries = 3 };
Console.WriteLine("init:       " + settings);

var counter = new Counter("hits");
counter.Tick();
counter.Tick();
Console.WriteLine("primary:    " + counter);

Console.WriteLine("inference:  " +
                  Rules.Larger("apple", "fig", s => s.Length));
return 0;
)CS";

}  // namespace

int main()
{
    script::Context ctx;
    try {
        ctx.eval(kSource, "modern.cs");
    } catch (const script::CompileError& error) {
        for (const script::Diagnostic& d : error.diagnostics())
            std::puts(d.toString().c_str());
        return 1;
    } catch (const script::Exception& error) {
        std::printf("unhandled: %s\n", error.what());
        return 1;
    }

    // Init-only means init-only: the checker refuses a later assignment, and
    // it refuses it before anything runs.
    std::puts("");
    const char* bad = R"CS(
        class S { public int N { get; init; } }
        var s = new S { N = 1 };
        s.N = 2;
    )CS";
    for (const script::Diagnostic& d : ctx.diagnose(bad, "bad.cs", nullptr))
        std::printf("refused:    %s\n", d.toString().c_str());
    return 0;
}
