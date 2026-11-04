# C#

libscript implements a subset of C#. This page is the whole of it; anything not
here is in [../limitations.md](../limitations.md). A C# source is one the
host names with a `.cs` extension, passes with `Language::CSharp`, or runs in
a context whose `Options::language` says so; it compiles to the typed engine
(see [../design.md](../design.md)).

## Program shape

A compilation unit is either a set of declarations, or top-level statements, or
both:

```csharp
using System;
using System.Collections.Generic;

int Twice(int n) => n * 2;          // a function beside the statements

var total = 0;
foreach (int n in new int[] { 1, 2, 3 })
    total += Twice(n);
Console.WriteLine(total);
return total;                        // hands a value back to the embedder
```

Top-level statements become `Program.Main`, and functions declared beside them
become static methods of the same class -- so they can be called before the
line that declares them. `return` from top level returns `object`, which is what
`Context::eval` gives back.

`namespace N { }` and file-scoped `namespace N;` both work. `using` brings a
namespace into scope; `System`, `System.Collections.Generic` and `System.Text`
are always in scope.

## Types

```csharp
bool  char  sbyte byte  short ushort  int uint  long ulong  float double
string  object  T[]
```

`decimal` is not supported. Declared types:

```csharp
class Account { }                       // reference type, single base + interfaces
struct Point { }                        // value type: copied on assignment
interface INamed { string Name { get; } }
enum Level { Low, High = 10 }           // int-backed by default
delegate int Op(int a, int b);
```

Conversions follow the language: the implicit numeric widenings, the implicit
constant conversion (`byte b = 1;`), boxing into `object`, and reference
conversions up the hierarchy. Everything else needs a cast, and a cast that
cannot hold throws `InvalidCastException`.

## Members

```csharp
class Account {
    private readonly string owner;              // field
    public const double Rate = 0.05;            // constant
    public static int Opened = 0;               // static field, initialized once

    public decimal Balance { get; private set; } // auto-property
    public string Owner => owner;                // expression-bodied property
    public int Age {                             // full property
        get { return age; }
        set { age = value; }
    }

    public Account(string o) { owner = o; Opened++; }
    public Account() : this("unknown") { }       // constructor chaining

    public virtual void Deposit(double amount) { }
    public override string ToString() => owner;
    public int Fee(int amount, int minimum = 10) => Math.Max(amount / 20, minimum);
    public static void Log(string format, params object[] args) { }
}
```

Instance and static fields, auto and full properties, constructors with
`: base(...)` and `: this(...)`, virtual/override/abstract methods, optional
parameters, `params` arrays, `ref` and `out` parameters, and overloading by
signature.

## Statements

`if`/`else`, `while`, `do`, `for`, `foreach`, `switch` (over constants or
patterns), `break`, `continue`, `return`, `throw`, `using`, local functions,
and `try`/`catch`/`finally` with `when` filters. A `return` inside a `try` runs
the `finally` blocks on its way out, as it must.

```csharp
try {
    risky();
} catch (InvalidOperationException e) {
    Console.WriteLine(e.Message);
} catch (Exception) {
    throw;                      // rethrow
} finally {
    cleanup();
}
```

Arrays can leave out their element type when the elements settle it:
`new[] { 1, 2.5 }` is a `double[]`, and `new[] { 1, "x" }` is an error. A `readonly`
field takes a value in its declaration or in a constructor of its own type, through
`this`; anywhere else, including `++` and a `ref` argument, is an error. Tuples work
as type arguments: `new List<(int, string)>()`.

`foreach` walks arrays, strings, `List<T>` and `Dictionary<K,V>`. A dictionary
yields `KeyValuePair<K,V>`, in insertion order, and takes the deconstructing form:

```csharp
foreach (var entry in counts) Console.WriteLine(entry.Key + ": " + entry.Value);
foreach (var (word, count) in counts) total += count;
```

The loop walks the entries as they were when it started (as `dictionary.Keys` and
`dictionary.Values` do), so changing the dictionary inside the loop does not throw
`InvalidOperationException` the way it does on .NET, and the loop does not see the
change. Each round allocates its `KeyValuePair` from the heap, which is a pool pop,
not a struct on the stack.

## Expressions

Everything you would expect: arithmetic with the language's promotion rules,
integer division that throws on zero, bitwise and shift operators, comparisons,
`&&` and `||` with short-circuiting, the conditional operator, `??` and `??=`,
`?.`, `is`, `is T x`, `as`, casts, `nameof`, `default`, `checked`/`unchecked`
(parsed, arithmetic wraps), `++`/`--` in both positions, and compound
assignment.

Object and collection initializers:

```csharp
var p = new Point { X = 1, Y = 2 };
var list = new List<int> { 1, 2, 3 };
var map = new Dictionary<string, int> { { "a", 1 }, ["b"] = 2 };
```

Strings:

```csharp
var s = "a" + 1 + true;                 // anything concatenates
var v = @"c:\raw\path";
var i = $"{name} scored {score:F1} of {total:N0}";
```

Lambdas and delegates:

```csharp
Func<int, int> square = x => x * x;
Action<string> log = m => { Console.WriteLine(m); };
Func<int, int> adder(int by) => x => x + by;    // captures `by`
Op op = Calculator.Add;                          // method group
```

A lambda captures variables by reference, the way C# does: the captured
variable moves to a heap box that the enclosing method and every lambda share.

## Generics

Declare them, and use the built-in ones:

```csharp
class Box<T> { public T Value; }
class Cache<K, V> where K : class { }
interface IStore<T> { void Put(T item); }

static T Larger<T>(T a, T b, Func<T, int> weigh) => weigh(a) >= weigh(b) ? a : b;
var bigger = Larger("apple", "fig", s => s.Length);   // T is inferred
var named = Larger<string>("a", "b", s => 1);         // or written out
```

Constraints are `where T : class`, `where T : struct`, `where T : new()` and
`where T : SomeBase`, and they are enforced where the type is used.

**Instantiation is reified.** `Box<int>` and `Box<string>` are separate types
built from separate copies of the declaration, so an `int` in a `Box<int>` is
an `int` -- it never boxes to satisfy `T`, and the instruction that adds two of
them is still `AddI4`. The cost is code size, and a declaration whose family is
infinite (`class Rec<T> { Rec<Rec<T>> f; }`) is refused rather than compiled
forever.

`List<T>`, `Dictionary<K,V>`, `Func<...>`, `Action<...>` and `IEnumerable<T>`
are built in and checked the same way -- `list.Add("x")` on a `List<int>` is a
compile error.

## Patterns

`is` and `switch` both take the full pattern grammar:

```csharp
if (o is int n and > 100)            // type, logical and relational
if (o is string { Length: > 0 } s)   // property, with a binding
if (o is null or "")                 // constant, either way
if (p is (var x, 0))                 // positional, through Deconstruct
if (a is [var first, .., var last])  // list, with a slice
if (o is not Circle)                 // negation

var band = n switch {                // a switch expression
    < 0 => "negative",
    0 => "zero",
    > 0 and < 10 => "small",
    _ => "large",
};

switch (shape) {                     // patterns in a switch statement
    case Circle c when c.R > 10: return "big";
    case Circle: return "circle";
    case null: return "nothing";
    default: return "?";
}
```

A pattern becomes the comparisons it stands for: nothing is looked up at run
time, and a binding is an ordinary local. A switch expression that matches
nothing throws `InvalidOperationException`, the way C# does.

## Tuples, deconstruction and records

```csharp
var pair = (1, "a");                       // (int, string)
var point = (x: 3, y: 4);                  // named elements
(int n, string s) Split(string t) => (t.Length, t.ToUpper());

var (length, upper) = Split("hey");        // deconstruction
var (a, _) = pair;                         // discarding a piece
foreach (var (k, v) in pairs) { }

record Money(string Currency, double Amount);
var m = new Money("EUR", 2.5);
var cheaper = m with { Amount = 1.0 };     // a copy with one change
m.Equals(new Money("EUR", 2.5));           // true: records compare by value
```

A record declaration is shorthand for init-only properties, the constructor,
`Deconstruct`, `ToString`, value `Equals`, `GetHashCode` and the copy `with`
uses -- and for any of those you write yourself, yours wins. A plain class can
take a primary constructor too, where the parameters become private fields.

## The syntax that came later

```csharp
int[] readings = [5, 3, 9];            // a collection expression
List<string> names = ["a", "b"];       // builds whatever it is assigned to

var newest = readings[^1];             // an index from the end
var middle = readings[1..3];           // a range; slicing copies

Dictionary<string, int> d = new();     // target-typed new

public string Name { get; init; }      // settable only while being built

var json = """
    {"quotes": "need no escaping"}
    """;                                // a raw string, indentation stripped

int.TryParse(text, out var parsed);    // out var
int.TryParse(text, out _);             // and discards

int Factorial(int n) =>                // a local function, which may recurse
    n <= 1 ? 1 : n * Factorial(n - 1);

using var file = Open(path);           // disposed at the end of the block
try { } catch (IOException e) when (e.Message.Length > 0) { }
var x = value ?? throw new Exception("required");
```

## The core library

| Type | What |
| --- | --- |
| `object` | `ToString`, `Equals`, `GetHashCode`, `ReferenceEquals` |
| `string` | `Length`, indexing, `Substring`, `IndexOf`, `LastIndexOf`, `Contains`, `StartsWith`, `EndsWith`, `ToUpper`, `ToLower`, `Trim`, `Replace`, `Split`, `ToCharArray`, `PadLeft`, `PadRight`, `CompareTo`, `Equals`, and the statics `IsNullOrEmpty`, `IsNullOrWhiteSpace`, `Concat`, `Join`, `Format`, `Empty` |
| `int`, `long`, `float`, `double` | `Parse`, `TryParse`, `ToString(format)`, `MaxValue`, `MinValue` |
| `char` | `IsDigit`, `IsLetter`, `IsWhiteSpace`, `ToUpper`, `ToLower` |
| `Math` | `Abs`, `Min`, `Max`, `Sqrt`, `Pow`, `Floor`, `Ceiling`, `Round`, `Truncate`, the trigonometric and logarithmic functions, `PI`, `E` |
| `Convert` | `ToInt32`, `ToInt64`, `ToDouble`, `ToBoolean`, `ToString` |
| `Console` | `Write`, `WriteLine`, `ReadLine` -- routed through the host's handlers, shared with the other languages |
| `List<T>` | `Add`, `Insert`, `Remove`, `RemoveAt`, `IndexOf`, `Contains`, `Clear`, `ToArray`, `Count`, indexing |
| `Dictionary<K,V>` | `Add`, `ContainsKey`, `Remove`, `TryGetValue`, `Clear`, `Keys`, `Values`, `Count`, indexing |
| `StringBuilder` | `Append`, `AppendLine`, `Clear`, `Length`, `ToString` |
| `Exception` | `Message`, `StackTrace`, `ToString`, and the standard derived types |

Number formatting supports the `D`, `F`, `N`, `X`, `E`, `P` and `G` specifiers
with an optional precision, in `ToString("F2")` and in interpolation holes.

## The host, from C#

A class the host declared with `hostClass` is a static class; one bound with
`Class<T>` is a sealed class with a constructor, instance methods, properties
and static members, all type-checked from the C++ signatures:

```csharp
var v = new Vec2(3, 4);      // Class<Vec2>(ctx, "Vec2").constructor<double, double>()
v.X = 10;                    // .field("X", &Vec2::x)
Env.Log(v.Length.ToString()); // .getter("Length", ...) and hostClass("Env").method("Log", ...)
Vec2 u = Vec2.Unit();        // .staticMethod("Unit", ...)
```

`Env.Log(42)` where `Log` takes a `string` is a compile error, exactly as it
would be for a method the program declared. See
[../embedding.md](../embedding.md).
