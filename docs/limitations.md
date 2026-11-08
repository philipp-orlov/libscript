# Limitations

What libscript does not do, and why. Everything a front end refuses, it
refuses at compile time with a message that says so -- no engine accepts a
program and then behaves differently from its language.

## Library-wide

* **Languages meet only through the host.** A C# program cannot call a
  JavaScript function and a TypeScript program cannot `new` a C# class. They
  share a heap, so a value the host takes from one and hands to the other --
  a string, a number, a wrapped C++ object -- works without conversion, and a
  host class bound once is callable from all of them; that is the bridge.
* **Two wrapper shapes.** A C++ instance the host or C# hands over is one
  `NativeCell` every language reads. A JavaScript `new Vec2()` has to give the
  constructor a real JavaScript object as `this`, so it makes one carrying the
  same wrapper state -- JavaScript and TypeScript use it fully, C# does not
  accept it as a `Vec2`.
* **A `Value` must not outlive its `Context`,** and two contexts never share
  one. `script::Exception` is the deliberate exception: it may outlive the
  context, and then `value()` reads as `null`.
* **`std::vector`, `std::map`, `std::unordered_map` and `std::function` build
  JavaScript containers** when they go into script. Coming out they read any
  array (C# or JavaScript), any object, any callable. A C# host method whose
  signature takes a `std::vector<T>` is spelled `T[]` and can be passed a C#
  array; one returning a `std::vector<T>` needs the dynamic engine to be in the
  build, and throws `Error(NotSupported)` otherwise. A `std::map` or
  `std::function` parameter is `object` to C#.
* **`Auto` picks the first enabled language** -- C#, then TypeScript,
  JavaScript, Python, C, C++ -- when neither the call, the file extension nor
  `Options::language` says. Name your files or set the option in a polyglot
  host.
* **The index overloads of `Value::get` and `Value::set` take `std::uint32_t`.**
  A literal `0` is ambiguous with the `const char*` and `Value` overloads;
  write `0u`.
* **A dynamic-language `Assembly` has no image.** `serialize()` throws
  `Error(NotSupported)` for TypeScript, JavaScript and Python; keep the
  `Assembly` and `run` it again instead. C and C++ programs, being typed, do have one.
* **An engine's first use pays for its runtime.** The C# type table, or the
  JavaScript intrinsics (about 230 objects), are built by the first compile in
  that language. Warm up before measuring.
* **Source and data have depth limits.** Expressions may nest 500 levels and
  chain 2000 operators or member accesses; JSON nests 1000 levels. The
  front ends and the checkers also stop at the thread's real stack limit, so a
  small thread stack lowers these. See "Running code you did not write" in
  `embedding.md`.
* **`OutOfMemory`, `StackOverflow` and `Interrupted` unwind to the host** and
  cannot be caught by any program; `stackLimit` counts frames of every engine
  together.
* **Host-side primitives print the JSON way.** `Value(true).toString()` is
  `"true"` whichever language will read it; inside C#, `true.ToString()` is
  still `"True"`.

## C#

libscript implements a subset of C#.

### Not supported, by design

| Construct | Note |
| --- | --- |
| `async` / `await` | there is no scheduler and no event loop; a `Context` is synchronous |
| threads, `lock`, `volatile` | a `Context` is single-threaded; run one per thread |
| `unsafe`, pointers, `stackalloc`, `fixed` | the point of an embedded engine is that a program cannot reach outside itself |
| reflection, `typeof`, attributes | no metadata is exposed at run time, which is also what keeps images small |
| the file system, the network, processes, the clock | no ambient authority; the host exposes what it wants through `hostClass` |
| `goto` | unstructured jumps have no place in a body a code generator has to reason about |

### Not supported yet

| Construct | Note |
| --- | --- |
| iterators (`yield return`) | parsed and refused; it needs a state-machine rewrite in the checker |
| LINQ | needs iterators first; the query keywords are not recognised |
| extension methods | `using static` is parsed but its members are not yet in scope |
| nullable value types (`int?`) | reference types are nullable; value types are not |
| `decimal` | there is no 128-bit decimal type in the runtime |
| operator overloading, user conversions, indexers, events | each one is a resolution rule, not a runtime feature; they can be added |
| nested and partial types | flat namespaces only |
| anonymous types | needs a synthesized type per shape |
| default interface methods | an interface method with a body is accepted but not inherited |
| variance (`in`/`out` on a type parameter) | parsed and ignored: reified instantiation gives it nothing to describe |
| generic math (`static abstract` interface members) | needs constrained static dispatch |
| spread elements in a collection expression (`[..other]`) | refused; add the items in a loop |
| multi-dimensional arrays (`int[,]`) | jagged arrays (`int[][]`) are the supported shape |
| checked overflow by default | `checked` is parsed and arithmetic wraps unless `CompileOptions::checkedArithmetic` is on |
| `string.Format` beyond `{0}` | positional holes only; use interpolation, which supports format specifiers |

### Behavioural differences

* **`?.` on a value-typed member** yields null rather than `int?`, so it has to
  be consumed with `??`: `s?.Length ?? 0`.
* **Generics are reified.** `Box<int>` and `Box<string>` are separate types
  built from separate copies of the declaration, so a value type never boxes to
  satisfy a type parameter and the typed bytecode stays typed. The cost is code
  size, and a declaration whose instantiation does not terminate -- `class
  Rec<T> { Rec<Rec<T>> f; }` -- is refused at a depth cap rather than hanging.
* **A tuple is a reference type.** `==` on one is reference equality; `Equals`
  compares element by element. C# uses a struct, where `==` is element-wise.
* **An exception filter runs after unwinding**, not before. Which handler runs
  is identical; the only difference is the order a `finally` between the throw
  and the handler runs in.
* **A record's equality covers its state** -- the positional parameters, fields
  and settable auto-properties. A computed property is not state and is left
  out, which is what C# does.
* **`int.MinValue / -1` and `int.MinValue % -1` do not throw.** C# raises
  `OverflowException`; here the quotient wraps to `int.MinValue` and the
  remainder is 0, the same in the interpreter and in native code, on every
  platform.
* **Converting a `double` to an integer follows ECMAScript's rule, not .NET's.**
  NaN and both infinities give 0. A finite value is converted to a 64-bit
  `long` first, saturating at its range, and a narrower type keeps the low
  bits of that: `(int)1e10` is 1410065408. The interpreter and
  the arm64 back end agree on this (checked by differential fuzzing); the
  x86-64 back end has not been brought in line and may give a different
  result for an infinity or a value beyond 2^63.
* **A `uint` or `ulong` prints as unsigned only while its static type is known.**
  `u.ToString()`, `"a" + u`, `s += u` and `$"{u}"` are right. A value boxed to
  `object` (an `object` variable, a `string.Format` argument) carries no
  unsigned marker, so `o.ToString()` prints 4000000000u as -294967296.
* **A slice copies.** `a[1..3]` allocates; there is no span.
* **A `switch` compiles to a comparison chain**, not a jump table.
* **Interface dispatch is a cached lookup**, not an interface table. The cache
  is per (concrete type, interface method) and never invalidates.
* **`Console` is the host's**, routed through `setPrintHandler` and
  `setReadLineHandler`. There is no console unless the host provides one.
* **Culture is invariant.** Number formatting and parsing do not consult a
  locale.
* **Stack traces need `CompileOptions::debugInfo`**, which is on by default.

### Compilation units and images

* A `compile` call produces one assembly; several coexist in a context and
  see each other. **Order matters:** a unit is checked against what is already
  loaded, so it must be compiled after the ones it uses; there is no forward
  reference across units and no cycles. Each type keeps its statics in the
  assembly that declared it. Static initializers run before the first use of
  the context, across every loaded unit. A name may be declared once across
  all units.
* Images load in dependency order too, and a missing dependency is
  `ErrorCode::BadImage`.
* **An image is trusted input.** The loader validates structure and a
  64-bit checksum, not type safety. The checksum catches an image that was cut
  short or damaged in storage; it does not authenticate it, so compile the
  image yourself or receive it over a channel you trust. Untrusted *source* is
  safe -- that is what the checker is for. Images written before the checksum
  existed (format version 1) are refused as `BadImage`; compile them again.

### Native code

The back ends compile a subset and the interpreter covers the rest, so this is
a performance boundary rather than a correctness one: primitives only (a
method with a reference-typed local, a call, an allocation, a string operation,
field or array access or an exception handler is left to the interpreter); no
register allocation yet; x86-64 is System V only, and on Windows the back end
reports itself unavailable. `SCRIPT_ENABLE_JIT=OFF` is the build for a
platform that will not give out executable pages. See [jit.md](jit.md).

## TypeScript

### Not modelled in the type system

| Missing | Why | What to do instead |
|---|---|---|
| **Conditional and mapped types, `infer`, template literal types** | Each is a small type-level language of its own, and together they are a larger body of code than the rest of the checker. | Write the concrete types out. `Record<K, T>` is provided as an approximation. |
| **`Partial`, `Readonly`, `Required`, `Pick`, `Omit`** | All are mapped types. | Declare the variant you need as its own interface. |
| **Overload signatures** | The declaration/implementation pairing rule is most of the work, and a union parameter covers the common case. | A single signature with a union or optional parameters. |
| **Modules, `import`, `export`** | Module resolution is a host policy question, not an engine one. | Evaluate each file with `eval`; declarations land on the global object. `export` is accepted, and meaningful, inside a namespace. |
| **Declaration merging** | Only `interface` into `interface` merges. | -- |
| **Types that outlive one evaluation** | A persistent type table grows for as long as the process runs, which is the one thing the engine is built not to do. | `Context::declare`, whose declarations do apply to every later evaluation; host bindings declare themselves. |

These are parsed and erased rather than rejected, and resolve to `any`. Two
known imprecisions: two type parameters with the same name and constraint in
different declarations are the same type, and a destructuring parameter with
no annotation is `any` rather than a shape inferred from the pattern.

### Approximations

- **`private` and `protected` are checked, not enforced.** They are erased, so
  a script that reaches a private member through `as any` or a computed key
  gets it. The security boundary is the `Context`.
- Everything under [JavaScript](#javascript) below applies to the erased
  program too.

## JavaScript

Applies to TypeScript as well, since the erased program runs on the same
machine.

### Not implemented

| Missing | Why | What to do instead |
|---|---|---|
| **Modules (`import` / `export`, `import()`)** | Module resolution is a host policy question, not an engine one. | Evaluate each file with `eval`; declarations land on the global object. `examples/javascript/plugin.cpp` shows a registration-based host. |
| **`with`** | Legacy, and it defeats static name resolution, which everything fast here depends on. | -- |
| **`WeakMap`, `WeakSet`, `WeakRef`, `FinalizationRegistry`** | Reference counting frees on the last release; weak tables need the collector to report each release, which it does not yet. | A `Map` keyed by an id the host hands out. |
| **`Temporal`, `Intl`, locale-aware formatting** | Would pull in time zone and locale databases. | `toLocaleString` falls back to the plain form. |
| **`Array.fromAsync`, `Iterator.zip`, `Iterator.prototype.chunks` and similar recent proposals** | Not yet. | -- |
| **Timers, file system, network, `process`** | Not the engine's job. | Expose exactly the host capabilities you intend to grant. |

`eval` and `new Function(source)` exist but are **off by default**: compiling text a script makes is what a sandbox
is usually for preventing. A host that wants them sets `Context::Options::runtimeCode = true`; until then both raise
an `EvalError`. The command-line tool turns them on (`--no-eval` turns them off). A function that calls `eval`
directly keeps its variables in cells and runs in the interpreter; a `var` declared by such a string inside a function
stays in the string's own scope, as in strict code.

Regular expressions backtrack, as every engine's do, so a pattern can take exponential time on a crafted input. The
matcher polls the interrupt hook and charges its stack to `memoryLimit`, so a host can bound it like any other loop.

### Approximations

- **One execution mode.** There is no sloppy/strict split. Assigning to an
  undeclared name creates a global, writes to non-writable properties are
  ignored instead of throwing, and duplicate parameter names are accepted.
- **No temporal dead zone.** `let` and `const` are not observably uninitialised
  before their declaration; reading one early yields `undefined` rather than
  throwing a `ReferenceError`.
- **`const` at the top level is not enforced.** Top-level declarations become
  global object properties, and the write barrier lives on the binding, not the
  property. `const` inside a function is enforced at compile time.
- **A namespace exports by value** (TypeScript): the object a namespace
  evaluates to is built once, so reassigning an exported `var` later is not
  visible through it.
- **Block-scoped function declarations** are hoisted to the enclosing function
  rather than to the block.
- **Arrays are dense, with holes.** An assignment more than about four million
  elements past the end raises a `RangeError` instead of switching to a sparse
  representation. Holes are real: `delete a[i]` makes one, and `in`, the keys
  and the methods that skip holes see it.
- **`Date` is minimal:** `Date.now`, construction from epoch milliseconds,
  `getTime`, `getFullYear`, `toISOString`, `toJSON`, `valueOf`. There is no
  parsing, no local timezone and no component setters.
- **`arguments.callee`** is not the function.
- **A host wrapper has no own properties.** `shared.x = 1` on an object the
  host or C# handed over is a `TypeError` unless `x` is a bound field with a
  setter; wrappers made by a JavaScript `new` are ordinary objects.

### Operational bounds

- Interpreter recursion is capped by `Options::stackLimit` (default 512), not
  by the OS stack. A very deep native/script sandwich can still be expensive:
  budget roughly a kilobyte of C++ stack per script frame.
- The interrupt handler is polled every few thousand instructions, so a single
  pathological built-in call -- `'x'.repeat(1e8)` -- is not interruptible
  mid-operation. Sizes are capped where that could matter.
- A context's cell heap gives memory back after a collection, but a block
  holding one live cell stays, so survivors scattered through a burst pin a
  block each until they die. The block is refilled first, so this does not grow.
  The payload pool's slabs for buffers up to 16 KiB are reused but not returned
  to the OS: their resident size follows the process's peak for each size. See
  [memory.md](memory.md).
- Compiling calls the general allocator: tree nodes own vectors and strings, and
  metadata and bytecode live as long as their assembly. A host that compiles new
  programs for ever therefore keeps `malloc` busy, returning all of it, with no
  growth seen in the soak; running what is already compiled does not call it.
- Source is parsed to a full syntax tree before checking and compilation, so
  peak memory during `eval` is proportional to the size of the script. The
  checker's arena is released when the check ends.
- TypeScript checking parses the built-in declarations, plus the declarations
  derived from the host bindings, once per check. `check.enabled = false`
  removes that for a source that has already been checked.

## Python

libscript implements the everyday core of Python 3 on the dynamic engine, by
lowering to the ECMAScript syntax tree. [languages/python.md](languages/python.md)
lists what is in; this is what is out, and where the object model shows.

### Not supported, by design

| Construct | Note |
| --- | --- |
| generators, `yield`, `async`/`await` | a suspension point, which the dynamic VM does not have (see JavaScript above); write an iterator class with `__iter__` |
| `with` | there is nothing to manage: no files, no locks |
| `import` of anything but `math`, `random`, `sys`, `time` | there is no module system and no file system; the host exposes what it wants through `hostClass` |
| `**kwargs`, keyword-only parameters, `**` in calls | keyword arguments are matched by position at compile time, which a dictionary of them defeats |
| multiple inheritance, metaclasses, decorators other than `staticmethod`, `classmethod`, `property`, `x.setter` | one prototype chain, decided at class creation |
| `else` on loops and on `try`, `:=`, `@`, complex and bytes literals, `a[i, j]` | rare, and each a rewrite of its own; refused with a message naming it |
| big integers, `int`/`float` as distinct types | numbers are doubles; `10 / 2` prints `5` |
| tuples as dict keys | a tuple is a list and a `Map` keys objects by identity |
| arithmetic dunders (`__add__` ...), `__hash__`, `__call__`, `__enter__` | not consulted; `__eq__`, `__len__`, `__getitem__`, `__contains__`, `__iter__`, `__str__`, `__repr__`, `__bool__`, `__lt__` are |
| division by zero raising | `/` and `//` give `inf`; `%` and `divmod` raise `ZeroDivisionError` |

### Approximations

* **A chained comparison evaluates its middle operand twice**, and `a[i] += v`
  evaluates `a` and `i` twice.
* **Default arguments are evaluated per call**, not at definition.
* **A derived `__init__` that never calls `super().__init__()`** has the base
  constructor called first with no arguments, because the engine's `this`
  does not exist before it.
* **Keyword arguments to a method, or to a function from another
  evaluation,** arrive as one trailing marker object; only the built-ins and
  functions of the same evaluation take them by name.
* **A class from an earlier evaluation is recognised by a capital letter.**
* **`str(3.0)` is `3`**, `print(1e21)` is `1e+21`.
* **The REPL is line by line**, so a multi-line Python block cannot be typed
  into `libscript --lang py`; evaluate a file.
* Everything under [JavaScript](#javascript) applies to the lowered program,
  including the string encoding, array density and the operational bounds.

## C

libscript implements a subset of C on the typed engine, through the C#
checker. [languages/c.md](languages/c.md) lists what is in and how pointers are
mapped; this is what is out.

### Not supported, by design

| Construct | Note |
| --- | --- |
| pointer arithmetic, `void*`, casts between pointer types, `&` outside a call argument | there are no addresses; a pointer is a reference to a struct, an array or a string |
| function pointers | the typed engine has delegates, but a C function is a static method with no value form here |
| unions, bit-fields | one layout per class |
| `goto`, labels, `case` fall-through into a non-empty case | unstructured jumps have no place in a body a code generator has to reason about; add `break` |
| function-like macros, conditional compilation | the preprocessor here is `#define NAME value` and `#include` dropped |
| `exit`, `scanf`, `getchar`, files, `time`, `strcpy` and the mutating string functions | no ambient authority, no input stream, and strings are immutable (`char buf[N]` is the mutable thing) |
| variadic user functions, `long double` precision, `sizeof(bool) == 1` | `bool` is `int`; `long double` is `double`; only `libc` is variadic |

### Behavioural differences

* **Structs are reference types.** `b = a`, passing a struct and returning
  one share the object; a copy has to be made field by field.
* **A scalar pointer parameter is `ref` or an array by how the body uses it**,
  and a caller has to write `&x` for a `ref` one; the address of a global or
  an element cannot be passed by reference.
* **`char` is 16-bit and unsigned.**
* **Runtime errors (index out of range, null struct, division by zero,
  `assert`) are exceptions** with the C source line, not undefined behaviour,
  and cannot be caught from C.
* **`unsigned` arithmetic with `int`** widens to 64 bits before narrowing back,
  which gives the same wrapped result as C.
* Everything under [C#](#c) about the typed engine -- images, native code,
  `checked` arithmetic, culture -- applies to a C program too.

## C++

libscript implements a subset of C++ on the typed engine, as the C front
end's dialect, through the C# checker. [languages/cpp.md](languages/cpp.md)
lists what is in and how classes, references, containers and exceptions are
mapped; this is what is out.

### Not supported, by design

| Construct | Note |
| --- | --- |
| copy semantics: `b = a`, pass by value, return by value, `vector<T> v2 = v1` | there is one object and references to it; the engine has no copy constructors. A copy is made field by field. `vector<T>(n, value)` with a class or container value is refused rather than aliased |
| destructors with a body, RAII | the collector owns the memory and frees an object when its last reference goes, not at scope end; an empty destructor or `= default` is accepted |
| templates, `template<...>`, `typename` in a declaration | the typed engine has generics, but instantiating a C++ template needs a C++ checker; the containers the engine provides (`vector`, `map`) are the generic types on offer |
| operator overloading | no user-defined operators on the typed engine |
| iterators: `begin()`, `end()`, `insert`, `erase`, `std::sort`, `<algorithm>` | there is no iterator object model; index, use a range-for, or write the loop |
| lambdas, `std::function`, function pointers | the typed engine has delegates, but a C++ closure's capture rules would need their own checker |
| multiple inheritance, nested classes, `friend`, `= delete`, `explicit` conversions, `mutable` semantics | one base class per class; the rest is accepted as syntax or refused with a reason |
| `std::set`, `std::array`, `std::list`, `std::deque`, `std::optional`, `std::tuple`, smart pointers, `std::pair` outside a map loop | not modelled; a `std::vector` or a class covers the cases the engine targets |
| `std::cin`, `std::getline`, files, `<chrono>`, `<thread>`, `<random>` beyond `rand` | no ambient authority and no input stream; the host hands in what the program needs |
| `reinterpret_cast`, pointer arithmetic, `void*`, `sizeof` of a class as its layout | there are no addresses |
| `goto`, the comma operator, `#define` with parameters, conditional compilation | as in C |

### Behavioural differences

* **Everything is a reference**, so aliasing appears wherever C++ would have
  copied; see [languages/cpp.md](languages/cpp.md#values-are-references).
* **`private` and `protected` are enforced by the front end**, not the
  engine, and only at the point of access in scripted C++; a host language
  sees every member.
* **`v[i]` and `s[i]` are bounds-checked** and throw `std::out_of_range`
  where C++ has undefined behaviour; `std::string::npos` is -1; `m[k]++` as a
  value yields the new value.
* **The range-for variable is read-only.** `for (int& x : v) x *= 2;` is a
  compile error; index the vector.
* **A `T&` parameter takes a variable of exactly `T`**, since it is a `ref`
  parameter underneath; `const T&` takes anything convertible.
* **`std::to_string(double)` is the shortest round-trip spelling**, not the
  six fixed decimals of the standard library; `std::cout` matches C++.
* **`bool` is a real type** (in C it is `int`), and `char` is a 16-bit code
  unit, as in C.
* Everything under [C#](#c) about the typed engine -- images, native code,
  `checked` arithmetic, culture -- applies to a C++ program too.
