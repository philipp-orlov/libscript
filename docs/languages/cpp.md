# C++

libscript's C++ is the C++ a program written for the engine can reasonably
use: everything [C](c.md) has, plus classes with constructors, methods,
inheritance and virtual dispatch, references, `bool`, `new`, overloading and
default arguments, `auto`, range-for, `enum class`, namespaces, exceptions,
`std::string`, `std::vector`, `std::map` and the output streams. This page
is the contract: if something is listed as supported it has a test in
`tests/cpp/`. A C++ source is one the host names with a `.cpp`, `.cc`,
`.cxx`, `.hpp`, `.hh` or `.hxx` extension, passes with `Language::Cpp`, or
runs in a context whose `Options::language` says so.

C++ runs on the typed engine, through the same front end as C: one lexer and
one parser with a dialect flag, the way TypeScript and JavaScript share the
ECMAScript parser. The parser produces the C#-shaped syntax tree, so the
checker, the emitter, the interpreter, the code generators and the image
format are shared with C# and C; `Assembly::serialize` and `Context::load`
work for a C++ program, and `Context::Options::jit` applies. What the dialect
adds is a mapping from C++'s abstractions onto the engine's:

| C++ | Engine |
|---|---|
| `class` / `struct` with members | a class with fields, methods, constructors, a base class and a vtable |
| `T*`, `T&` to a class | the same reference |
| `T&` to a scalar or `std::string` (parameter) | `ref T` |
| `const T&` | by value |
| `new T(...)`, `new T[n]` | `new T(...)`, `new T[n]`; `delete` does nothing |
| `bool` | `bool` |
| `std::string` | `string` |
| `std::vector<T>` | `List<T>` |
| `std::map<K, V>`, `std::unordered_map<K, V>` | `Dictionary<K, V>` |
| `std::cout << a << b << std::endl` | one `libc.__cout` call per statement |
| `throw` / `try` / `catch` | the engine's exceptions |
| `enum class E { A }` | `int`; `E::A` is a constant |
| `namespace N { }` | flattened; `N::x` is `x` |

## Program shape

```cpp
#include <iostream>
#include <vector>
#include <string>

class Shape {
public:
    virtual double area() const = 0;
    virtual std::string name() const { return "shape"; }
};

class Rect : public Shape {
public:
    Rect(double w, double h) : w_(w), h_(h) {}
    double area() const override { return w_ * h_; }
    std::string name() const override { return "rect"; }
private:
    double w_, h_;
};

int main() {
    std::vector<Shape*> shapes;
    shapes.push_back(new Rect(2, 3));
    double total = 0;
    for (const Shape* s : shapes) {
        std::cout << s->name() << ": " << s->area() << std::endl;
        total += s->area();
    }
    return int(total);
}
```

`main` is the entry point; its return value is what `Context::eval` and
`Context::run` hand back, and `main` may fall off its end, returning 0.
Functions, classes and globals may appear in any order that C++ allows: a
class must be defined before it is used as a base or a member type, and a
member function may use members declared below it. Globals and static
members keep their values between runs of the same `Assembly`. Free
functions and globals become static members of one synthesized class, so a
stack trace reads `at C$.main (prog.cpp:12)`; a method reads `at Rect.area`.

## Classes

A class has fields (with default member initializers), methods, constructors
with initializer lists, static members, one base class and virtual
functions. `class` members are private by default and `struct` members
public; `public:`, `private:` and `protected:` are enforced at compile time.
Methods may be defined inside the class or out of line as `T X::m(...) { }`,
and constructors as `X::X(...) : ... { }`. A static data member declared in
the class is defined as `int X::count = 0;` outside it. `this` works, and a
member named bare inside a method is the member.

Inheritance is single and public. A method whose signature matches a
`virtual` method of a base class overrides it, `override` or not, and a
call through a base pointer or reference dispatches to the override. A pure
virtual function (`= 0`) makes the class abstract; constructing one is
refused. `Base::method()` inside a subclass calls the base's version.
`dynamic_cast<D*>(p)` gives the pointer or `nullptr`; `static_cast<D*>(p)`
throws on a mismatch.

What a class does not have: destructors that do anything (an empty one or
`= default` is accepted, since the collector owns the memory), operator
overloading, nested classes, templates, multiple inheritance, `friend`,
`= delete`, and copy semantics (see below).

## Values are references

The engine has no addresses and no copies of objects: a class instance, a
`std::vector`, a `std::map` and a `std::string` are each one heap object and
a variable holds a reference to it. That is exactly what a C++ pointer or
reference gives, so `Rect* p = new Rect(1, 2)`, `Rect& r = *p`, `p->w_`,
`r.w_` and `(*p).w_` all mean what they mean in C++, and `delete` is a no-op
because the collector frees the object when the last reference goes.

Where C++ would copy, this engine shares: `Rect b = a`, passing a class or a
container by value, returning one, `std::vector<T> v2 = v1`, and
`std::vector<T> v(n, value)` when `value` is itself a class or container. The
last is refused with a message; the others silently alias. A program that
needs a copy makes one field by field (or element by element). Strings are
immutable, so sharing a `std::string` is unobservable; `s += "x"` rebinds
`s`, and `s[i] = c` is refused.

## Types

Everything in [C's table](c.md#types), with these differences and additions:

| C++ | Engine |
|---|---|
| `bool` | `bool` (in C it is `int`); `true`/`false` are `bool` |
| `std::string`, `string` | `string`; `const char*` is the same type |
| `std::vector<T>` | `List<T>`; `T` may be any type but an array |
| `std::map<K, V>` | `Dictionary<K, V>`; `K` a number, `char`, `bool` or `std::string` |
| `std::pair<K, V>` | only as the variable of a range-for over a map: `kv.first`, `kv.second` |
| `auto` | the initializer's type; `auto&` and `const auto&` in a range-for |
| `T&` | see *Values are references* |
| `enum class E : int { ... }` | `int`; `E::A` and, for a plain `enum`, also `A` |
| `using Name = T;` | a typedef |
| `size_t`, `int32_t`, ... | as in C |

`std::` may be spelled or omitted (`using namespace std;` is accepted and
changes nothing). `const`, `constexpr`, `inline`, `static`, `explicit`,
`noexcept`, `mutable` and `volatile` are accepted; `constexpr` is `const`.

## Statements and expressions

Everything in C, plus: range-for over an array, a `std::vector`, a
`std::string` (characters) or a `std::map` (`for (auto& kv : m)` with
`kv.first` and `kv.second`; assigning `kv.second` writes the map);
`try`/`catch`/`throw`; `new` and `delete`; `static_cast`, `dynamic_cast`
and `const_cast`; functional casts `int(x)`, `double(n)`; direct
initialization `Point p(1, 2)`, `std::vector<int> v(n, 0)`; brace
initialization `Point p{1, 2}`, `std::vector<int> v = {1, 2, 3}`,
`std::map<std::string, int> m = {{"a", 1}}`, and a brace list as an
argument where a container or class is expected. Not supported: lambdas,
`goto`, the comma operator outside a `for` header, `reinterpret_cast`.

Function overloading resolves by the number of arguments and then by how
well their types match (an exact match beats a widening beats a narrowing);
an ambiguous call is refused. Default arguments work, and a default declared
in a prototype carries to the definition. A `T&` parameter takes a variable
of exactly `T`; `const T&` takes anything convertible.

## Exceptions

`throw std::runtime_error("message")` and the other `<stdexcept>` types
throw the engine's exceptions, which the host receives as `script::Exception`
with the C++ source line, and which a `catch` in the script can take:

| C++ | Engine | `ErrorCode` |
|---|---|---|
| `std::exception` | `System.Exception` | catches everything |
| `std::runtime_error`, `std::logic_error` | `InvalidOperationException` | `InvalidOperation` |
| `std::invalid_argument` | `ArgumentException` | `ArgumentException` |
| `std::out_of_range`, `std::domain_error`, `std::length_error`, `std::range_error` | `ArgumentOutOfRangeException` | `ArgumentOutOfRange` |
| `std::overflow_error` | `OverflowException` | `Overflow` |
| `std::bad_alloc` | `OutOfMemoryException` | `OutOfMemory` |

`catch (const std::exception& e)` catches the engine's own errors too -- a
division by zero, a null pointer, an index out of range, a host method that
threw `script::Error` -- and `e.what()` is the message. `catch (...)`
catches everything; `throw;` rethrows. `throw "text"` throws a
`std::exception` carrying the text. `v.at(i)`, `s.at(i)` and `m.at(k)` throw
`std::out_of_range` as they do in C++; `v[i]` and `s[i]` throw the same,
where C++ would not check.

## The standard library

`<cstdio>`, `<cstring>`, `<cstdlib>`, `<cctype>`, `<cmath>`, `<climits>`,
`<cfloat>`, `<cassert>`: the whole [C library](c.md#the-c-library), under its
usual names, with or without `std::`. `std::abs`, `std::min` and `std::max`
work on any numeric type. Then:

| Header | Supported |
|---|---|
| `<string>` | `size`, `length`, `empty`, `substr(pos[, n])`, `find`, `rfind` (`std::string::npos` is -1), `at`, `front`, `back`, `c_str`, `data`, `compare`, `starts_with`, `ends_with`, `contains`, `append`, `push_back`, `clear`, `+`, `+=`, `==`, `!=`, `<`, `<=`, `>`, `>=`, `s[i]`, `std::string(n, c)`, `std::to_string`, `std::stoi`, `std::stol`, `std::stoll`, `std::stod`, `std::stof` |
| `<vector>` | `push_back`, `emplace_back`, `pop_back`, `size`, `empty`, `clear`, `front`, `back`, `at`, `v[i]`, `reserve` (a no-op), `std::vector<T>(n)`, `(n, value)`, `{...}`, range-for |
| `<map>`, `<unordered_map>` | `m[k]` (inserts a default value, as C++ does), `at`, `count`, `contains`, `erase`, `size`, `empty`, `clear`, `{{k, v}, ...}`, range-for over `std::pair` |
| `<utility>` | `std::swap(a, b)` on two variables, fields or elements of one type |
| `<iostream>` | `std::cout`, `std::cerr`, `std::clog`, `<<`, `std::endl`, `std::flush`; numbers print as the default stream prints them (`%g` for doubles), `bool` as `1`/`0` |
| `<stdexcept>` | the types above |

Output goes through `Context::setPrintHandler`; `std::cerr` and `std::clog`
mark it as an error. There is no input (`std::cin`, `std::getline`), no
`std::sort` or other `<algorithm>` over iterators, no iterators at all
(`begin()`, `end()`, `insert`, `erase` on a vector), no `std::set`,
`std::array`, `std::list`, `std::deque`, smart pointers, `std::function`,
`std::optional`, `std::tuple` or `std::pair` outside a map loop. Each of
these is refused with a message that names it.

## Host bindings

A host class bound with `hostClass` or `Class<T>` is visible under its name.
`Env::Greet("x")` (or `Env.Greet`) calls a static member; `Vec2 v(3, 4)`,
`Vec2 v = Vec2(3, 4)`, `auto v = Vec2(3, 4)` and `new Vec2(3, 4)` construct
an instance; `v.Length()`, `v->X` and `Vec2::Unit()` reach its members; a
`Vec2&`, `const Vec2&` or `Vec2*` parameter is the same reference. A host
method taking a `std::vector<T>` is spelled `T[]` on the C# side; passing a
scripted `std::vector<T>` or a C array works, and the host's `std::vector<T>`
result arrives as a C array (`for (int x : result)` works; `.size()` does not
-- use `sizeof(a) / sizeof(a[0])`). A host `bool` is a `bool`. Members whose
C++ signature has no spelling here (a `std::map`, a `std::function`, a
`std::optional`) are not visible and are reported as missing.

## Semantics worth knowing

- **Sharing instead of copying**, as above. It is the one place a C++
  program will notice the engine.
- **`m[k]++` and `m[k] += v` yield the new value**, the way a prefix
  operator does; as statements they are exactly C++.
- **`std::to_string(2.5)` is `"2.5"`**, not `"2.500000"`; `std::cout` agrees
  with C++.
- **Integer arithmetic wraps, `char` is a UTF-16 code unit, `int` from
  `double` truncates**, as in [C](c.md#semantics-worth-knowing).
- **Uninitialised scalars are zero, class members are default-constructed,
  strings are empty and containers are empty**; a member of a class type
  that has constructors is left null until the initializer list constructs
  it, and reading it before that is a `NullReferenceException`.
- **The range-for variable is read-only**; `for (int& x : v) x *= 2;` is
  refused by the checker. Index the vector to change it.
- **`private` and `protected` are checked by the front end**, since the
  engine has no access control of its own.
- Everything under [C#](../limitations.md#c) about the typed engine --
  images, native code, `checked` arithmetic, culture -- applies to a C++
  program too.
