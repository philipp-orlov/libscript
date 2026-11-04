# Design

libscript is one library, one namespace and one public API in front of two
machines and six front ends:

```
        C# source ──> lexer ──> parser ─────────────┐
         C source ──> lexer ──> parser + C typing ──┤
       C++ source ──> the same, C++ dialect on ─────┤
                                                    ▼
                                    C#-shaped AST ──> checker ──> resolved, typed AST ──> Emitter ──> typed bytecode ──> typed VM
                                                                                                                            └─> arm64 / x86-64
TypeScript source ──> lexer ──> parser ──> checker ──> eraser ──┐
JavaScript source ──> lexer ──> parser ─────────────────────────┤
    Python source ──> lexer ──> parser + lowering ──────────────┤
                                                                ▼
                                                  ECMAScript AST ──> compiler ──> dynamic bytecode ──> dynamic VM

                       ─── one heap, one Value, one binding layer, one Context ───
```

The lexers and parsers are per language, except that C and C++ share one
with a dialect flag, as TypeScript and JavaScript do. The TypeScript checker
and eraser are per language; the typed checker serves every language that
produces the C#-shaped tree. Everything downstream of an engine's input form is shared by
the languages on that engine, and everything that is not a machine -- the
heap, the value model, the bindings, the API, the tool -- is shared by all of
them.

## Why two engines and not one

The typed engine and the dynamic engine are not two copies of the same idea.
The typed bytecode has `AddI4`, `AddR8`, `LdFld slot` and `CallVirt method`:
every operand type and every member slot was decided by the checker, and the
VM has no hash table on an object, no string comparison in a call and no
type test in an arithmetic instruction. That is what makes the interpreter
fast and the code generators a switch statement. The dynamic bytecode has
`Add`, `GetProp atom` and `Call argc`: the operand types are inspected at run
time, properties live in per-object tables behind a prototype chain, and
closures capture boxed variables by reference. That is what JavaScript
*means*.

Running JavaScript on the typed machine would require adding a dynamic object
model, prototype dispatch, `typeof`, loose equality, `arguments`, iterators and
the rest to it -- which is to say, writing the JavaScript engine again inside
the C# engine and paying for the indirection in both. Running C# on the dynamic
machine would throw away every static fact the checker established. So the
machines stay separate, and the merge takes everything that was genuinely
duplicated between the three original libraries: the cell header, the pool,
the cycle collector, the string table, the value representation, the UTF-16
strings, the binding templates, the context, the lock, the limits, the
interrupt hook, the diagnostics, the tests' harness and the command line.

## The core

**`Value`** (`include/script/value.hpp`) is a 16-byte tagged union. The tags
are the union of what the languages need: `Undefined`, `Null`, `Bool`,
`Char`, `Int`, `Long`, `Float`, `Double`, then the heap references `String`,
`Symbol`, `Object`, `Array` and `Delegate`. A C# program never produces
`Undefined` or `Symbol`; a JavaScript program never produces `Array` or
`Delegate` tags (its arrays and functions are `Object`s) and always produces
`Double` for a number. The default-constructed `Value` is `null`, which is
what the typed engine means by "nothing"; JavaScript's `undefined` is spelled
`Value::undefined()` wherever the dynamic engine means it.

**The heap** (`include/script/internal/heap.hpp`, `src/core/heap.cpp`).
Every cell starts with one `GCObject` header: a refcount, a `CellKind`, the
collector's scratch fields, the pool slot size and the owning context. The
kinds are grouped -- `String` and `Box` are shared, `Instance` .. `Builder`
belong to the typed engine, `Native` is the host wrapper both engines read,
`Symbol` and `Object` belong to the dynamic engine -- so the public API can
tell from a cell which engine to ask about it. libcore's `mem::LocalHeap`
carves mappings into blocks by size class; `ContextImpl::collectCycles` is the
trial-deletion collector; the string table interns strings and sweeps the
ones nothing else names. [memory.md](memory.md) has the mechanics.

**`ContextImpl`** (`include/script/internal/context.hpp`) owns the heap, the
string table, the options, the lock, the handlers, the shared call depth and
the host class records, and holds one runtime per engine behind a
`unique_ptr`. A runtime is created the first time a language that needs it
compiles or wraps something, so a context that only ever runs C# never builds
a JavaScript global object and a context that only ever runs JavaScript never
installs `System.*`. Each runtime implements `EngineRuntime`: release its roots
before the heap is torn down, sweep its private tables after a collection,
and describe its live frames for a stack trace.

## The typed engine

`src/typed/`, namespace `script::internal::typed`. Its input is the resolved
AST in `include/script/internal/typed/ast.hpp` with every name bound, every
slot assigned and every conversion chosen. The `Emitter` walks it once into
`MethodBody` bytecode inside an `AssemblyImpl`: types with flattened vtables,
methods with handler and line tables, reference tables, literals and the
static store. The VM in `vm.cpp` is a stack machine over that instruction set;
`invoke` is the one entry point the interpreter, the API and the back ends
share. `jit/` holds the tiering policy, the executable-page cache and the two
code generators, which compile the primitive-arithmetic-and-control-flow
subset of a method to machine code and leave the rest interpreting
([jit.md](jit.md)). `module.cpp` writes and reads the portable image
([compilation.md](compilation.md)). The core library -- `string`, `Math`,
`List<T>`, `Dictionary<K,V>`, the exceptions -- lives in `builtins/`, written
against the same metadata a C# class produces.

`typed::Runtime` holds the `TypeTable`, the loaded assemblies, the host types,
the interface dispatch cache, the JIT engine and the frame pool. It borrows the
heap through one-line forwarders (`newCell`, `str`, `intern`), so the engine's
sources read as they did when the heap was its own.

## The dynamic engine

`src/dyn/`, namespace `script::internal::dyn`. Its input is the ECMAScript
AST in `include/script/internal/dyn/ast.hpp` -- one `Node` type with four
child slots, a list and a type slot -- with no TypeScript-only node left in
it. `compiler.cpp` turns a program into a tree of `FunctionCode`: bytecode,
constants, interned atoms, handler table, nested functions, upvalue
descriptors. Locals a closure captures live in `Box` cells; the rest live in
the frame. The VM in `vm.cpp` is a stack machine with prototype-chain property
lookup, one C++ frame per script frame, `try`/`finally` as a subroutine, and a
microtask queue for promises. `object.cpp` is the property table, the dense
array, the `Map`/`Set` store; `builtins/` is the standard library.

`dyn::Runtime` holds the atoms and well-known symbols, the intrinsic
prototypes and constructors, the global object, the error prototypes, the job
queue and the frame pool, and the same heap forwarders. Its `str` does not
intern: JavaScript compares strings by content and makes them by the million.

## The front ends

| Language | Where | Pipeline |
| --- | --- | --- |
| C# | `src/lang/csharp/` | `Lexer` -> `Parser` -> `Checker` (name resolution, layout, generics by cloning, records, patterns lowered) -> the typed `Emitter` |
| TypeScript | `src/lang/ecmascript/` + `src/lang/typescript/` | shared `Lexer` and `Parser` with the TypeScript syntax on -> `checkProgram` (structural types, narrowing) -> `erase` -> the dynamic compiler |
| JavaScript | `src/lang/ecmascript/` + `src/lang/javascript/` | the same `Lexer` and `Parser` with the TypeScript syntax off -> the dynamic compiler |
| Python | `src/lang/python/` | its own `Lexer` (indentation, f-strings) and `Parser`, which lowers straight to the ECMAScript AST -> the dynamic compiler; a `__py` helper object and a JavaScript prelude supply the built-ins ([languages/python.md](languages/python.md)) |
| C | `src/lang/c/` | its own `Lexer` (with `#define`) and `Parser`, which tracks C types well enough to insert C's implicit conversions and produces the C#-shaped tree -> the shared `Checker` -> the typed `Emitter`; `libc` is a static class of natives ([languages/c.md](languages/c.md)) |
| C++ | `src/lang/c/` + `src/lang/cpp/` | the C `Lexer` and `Parser` with the C++ dialect on (`cxx.cpp`): classes become classes, `T&` becomes `ref`, `std::string`/`vector`/`map` become `string`/`List`/`Dictionary`, a `std::cout` chain becomes one `libc.__cout` call, exceptions are the engine's -> the same `Checker` and `Emitter` ([languages/cpp.md](languages/cpp.md)) |

The ECMAScript parser is one parser with a dialect flag. With it off, every
TypeScript-only construct -- type annotations, `as`, generics, `enum`,
`interface`, `namespace`, `declare`, access modifiers, non-null `!` -- is
refused exactly where JavaScript would refuse it, and the contextual keywords
(`type`, `interface`, `as`, `readonly` ...) are plain identifiers. The cost
of sharing is a few dead branches in a JavaScript-only build; the gain is that
a fix to the JavaScript grammar is a fix to the TypeScript grammar.

Each front end is one `LanguageInfo` (`include/script/internal/language.hpp`):
an id, a name, the file extensions, the engine it runs on, and three function
pointers -- `compile` (source in, `CompiledUnit` out, diagnostics on failure),
`run`, and optionally `declare` for ambient declarations. `src/core/language.cpp`
holds the table, one `#if SCRIPT_LANG_*` block per language, in the order
`Auto` prefers them: C#, TypeScript, JavaScript, Python, C. The public
`Context` never names a language: it picks a `LanguageInfo` by explicit
`Language`, by file extension, by `Options::language`, or by that order, and
calls through. [adding-a-language.md](adding-a-language.md) is the recipe for
the next one.

Three of the six front ends ride on another's machinery, which is the point of
the split. Python has JavaScript's object model underneath (a list is an
array, a dict a `Map`, a class a class) and needs no checker, so it is a
parser that builds the ECMAScript tree plus a prelude; the places where
Python's semantics differ -- truth, `==`, indexing, `in`, a dozen method names
-- go through helpers on one `__py` object. C is a statically typed language
whose declarations map onto C# classes, arrays and static members, so its
parser builds the C#-shaped tree with C's implicit conversions written out
(a `double` into an `int` is a cast, a number in an `if` is `!= 0`) and lets
the C# checker resolve names, layout and overloads. That is why the whole C#
front end except its registration -- lexer, parser, checker, cloner, record
expansion -- is built with the typed engine rather than with the
`SCRIPT_LANG_CSHARP` option: the checker synthesizes record members by
parsing C# text, so a C-only build carries the C# parser too. C++ is the C
front end one step further: its classes, references and containers all have
a C# spelling (a class, `ref`, `List<T>`, `Dictionary<K,V>`), so the same
parser produces them behind a dialect flag and the checker does the rest;
what has no C# spelling that the engine also lacks -- copy semantics,
templates, operator overloading, iterators -- is what the dialect refuses.

## Host bindings

`Context::hostClass` and `Class<T>` (`include/script/binding.hpp`) record
each member as a `HostMember`: its kind (method, constructor, property,
constant), whether it is static, and its C++ signature spelled twice -- once
the way C# spells it (`int`, `string`, `Vec2[]`) and once the way TypeScript
does (`number`, `string`, `Vec2[]`). `ContextImpl::hostClasses` is the single
record. Nothing is materialized until an engine needs it: before every compile
and before wrapping an instance, `typed::Runtime::syncHostClasses` creates the
missing `TypeInfo`s and native `MethodInfo`s, and `dyn::Runtime::syncHostClasses`
creates the missing constructor functions, prototypes and accessors on the
global object. Each keeps a per-class count of how many members it has
installed, so declaring a binding costs nothing in an engine that is never
used and a member added later is picked up by the next compile.

TypeScript gets its types from the same record: `dyn::Runtime::hostDeclarations`
renders every host class as `declare class Vec2 { constructor(a0: number, a1:
number); Length(): number; X: number; static Unit(): Vec2; }` or `declare
const Env: { Greet(a0: string): string; }` and prepends it to the ambient
declarations the checker starts from. `Context::declare` adds anything the
embedder wants on top.

**The wrapper cell.** A C++ instance handed to script -- by the host through
`ctx.value(&x)`, by a C# `new` of a bound class, by a host method returning a
pointer -- becomes one `NativeCell`: a `NativeSlot` (type key, pointer,
finalizer, owner) plus the typed engine's `TypeInfo`, filled in when that
engine first meets it. The typed VM resolves it to the bound class; the
dynamic VM reads it through the class's prototype (`hostProtoOf` in the
property paths), so `shared.Bump()` works in JavaScript on an object C# made
and the other way round. The one asymmetry is a JavaScript `new Vec2()`: a
constructor's `this` has to be a real JavaScript object, so it is an ordinary
object carrying the same slot, which C# does not accept as a `Vec2`.

`CallInfo::fail` and `Context::makeException` shape the error object for the
engine that is running -- `ContextImpl::activeEngine` is set around every
host call -- so a host method raises a `System.ArgumentException` when C#
called it and a `TypeError` when JavaScript did.

## What lives where

| Path | What |
| --- | --- |
| `include/script/*.hpp` | the public API: `Value`, `Context`, `Assembly`, `Language`, the binding layer; `script.hpp` includes all of it |
| `include/script/config.hpp` | generated by CMake: which languages, engines and the JIT this build contains |
| `include/script/internal/` | the core's own headers: heap, context, the language hook |
| `include/script/internal/typed/`, `src/typed/` | the typed engine: metadata, types, object model, emitter, VM, image, JIT, `System.*` |
| `include/script/internal/dyn/`, `src/dyn/` | the dynamic engine: AST, bytecode, compiler, VM, object model, standard library |
| `src/core/` | the heap, strings, the language table, the public API |
| `src/lang/csharp/` | the C# lexer, parser, checker, AST cloner and record expansion, built with the typed engine for every language that produces its tree; and the C# registration, which is what the C# option adds |
| `src/lang/ecmascript/` | the lexer and parser both ECMAScript languages share |
| `src/lang/typescript/` | the structural type checker, the eraser, the type arena, the built-in declarations, registration |
| `src/lang/javascript/` | registration |
| `include/script/internal/python/`, `src/lang/python/` | the Python lexer, the parser that lowers to the ECMAScript AST, the `__py` natives and the prelude, registration |
| `include/script/internal/c/`, `src/lang/c/` | the C and C++ lexer with its preprocessor, the parser that builds the C#-shaped tree (`parser.cpp` for C, `cxx.cpp` for what C++ adds), the `libc` natives, the shared compile hook and the C registration |
| `src/lang/cpp/` | the C++ registration |
| `tests/core/`, `tests/csharp/`, `tests/typescript/`, `tests/javascript/`, `tests/python/`, `tests/c/`, `tests/cpp/` | one executable per suite; a language's suites go with its option |
| `examples/<language>/`, `examples/polyglot/` | one executable per file |
| `tools/script_tool.cpp` | `libscript`, the command line |
| `bench/` | what a long run costs: time and heap calls per round, and the soak |
| `cmake/ScriptLanguage.cmake` | the build side of the language hook |

## What comes from libcore

The engine is about languages; the machinery under it is shared with the rest of
the family, and nothing of it is written here a second time. Allocators
(`mem::LocalHeap`, `mem::ObjectArena`, `mem::PoolAllocator`, `mem::SizeClassPool`),
Unicode and number conversion (`base/utf16.hpp`, `base/number.hpp`, `base/ascii.hpp`),
hashes (`base::hashBytes64`, `base::hashPointer`), pages for the code generators
(`sys::vmMap`, `sys::vmProtectExecutable`), `sys::readWholeFile`,
`sys::residentBytes`, the build flags and the test harness.
`libcore/buildkit/scripts/check_no_dups.sh` fails when one of those is written again.

## Invariants worth knowing

* **A `Value` belongs to its `Context`.** It must not outlive it, and two
  contexts never share one. The one exception is made deliberately: a
  `script::Exception` may outlive the context it came from, and then its
  `value()` reads as `null` instead of touching freed memory.
* **The default `Value` is `null`; `undefined` is explicit.** The dynamic engine
  spells `Value::undefined()` wherever the language means it, including the
  `undefined` global, uninitialised `let`s, missing properties and the result
  of a `void` host function.
* **Numbers from the dynamic engine are `Double`; any numeric tag is accepted.**
  A host can hand `Value(42)` (an `Int`) or `Value(u'A')` (a `Char`) to a
  JavaScript program and arithmetic, `typeof` and equality treat it as a
  number. The `Double` case stays the fast path.
* **One call depth across engines.** `stackLimit` counts every frame in the
  context, whichever machine pushed it; each engine keeps its own frame pool.
* **Metadata is immutable once emitted.** The only mutable part of a typed
  `MethodInfo` is its tiering state, which is atomic.
* **Every C# name is resolved before anything runs;** every JavaScript
  property is looked up by interned pointer. Neither engine compares strings
  on a hot path.
* **Contexts share no global state.** The language table and the extension
  table are the only statics, and both are immutable after load. One context
  per thread is the way to get parallelism; one context shared between
  threads serializes on its lock.
