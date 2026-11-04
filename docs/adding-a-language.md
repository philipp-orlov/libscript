# Adding a language

A language is a front end: it turns source into the input form of one of the
two engines and registers itself through one `LanguageInfo`. Nothing else in
the library names a language. This page is the recipe, using two hypothetical
front ends -- BASIC on the typed engine, and Python on the dynamic one, which
has since been written this way -- and it points at the real ones to copy
from: `src/lang/csharp/csharp.cpp` and `src/lang/c/c.cpp` on the typed
engine, `src/lang/javascript/javascript.cpp` and `src/lang/python/python.cpp`
on the dynamic one. A dialect of an existing language -- C++ over C,
TypeScript over JavaScript -- is cheaper still: one flag in the shared
parser and a registration of its own (`src/lang/cpp/cpp.cpp`).

## 1. Pick the engine

| If the language is | Use | Its input form | Copy from |
| --- | --- | --- | --- |
| statically typed, with classes, fields and methods resolved before it runs (BASIC with `Dim x As Integer`, a Java or Go subset) | the **typed engine**, `src/typed/` | the resolved AST in `include/script/internal/typed/ast.hpp`: every `TypeRef` bound to a `TypeInfo`, every local given a slot, every call bound to a `MethodInfo`, every conversion chosen -- the shape the C# checker leaves behind | `src/lang/csharp/` |
| dynamically typed, with objects as property bags and closures (Python, Lua, Ruby, a Lisp) | the **dynamic engine**, `src/dyn/` | the ECMAScript AST in `include/script/internal/dyn/ast.hpp`: one `Node` kind per construct, no type nodes | `src/lang/javascript/` |

The typed engine gives you typed bytecode, an interpreter with no dynamic
lookups, arm64 and x86-64 code generation, and portable images; the price is a
front end that resolves everything, which for C# is the 4,400-line checker.
The dynamic engine gives you prototypes, closures, `typeof`-style dynamism and
a compiler you call as one function; the price is that your language's object
model has to fit JavaScript's, which for Python's classes, dicts and lists it
largely does.

A typed language whose declarations map onto classes, arrays and static
members has a shortcut the C front end takes: build the *unresolved* C#-shaped
tree (`TypeDecl`, `MethodDecl`, `NameExpr` with names, `TypeRef` with paths)
and hand it to the C# `Checker`, which resolves names, lays out types and
picks overloads for you. The price is writing your language's implicit
conversions out explicitly, since the checker applies C#'s rules; the C
parser tracks a small type of its own for exactly that.

A language whose semantics fit neither -- a stack language, a logic language
-- needs a third engine, which is a larger project than a front end and out of
scope here.

## 2. Write the front end

```
src/lang/basic/
    lexer.cpp        # source -> tokens
    parser.cpp       # tokens -> your own syntax tree
    lower.cpp        # your tree -> the engine's input form
    basic.cpp        # the LanguageInfo
include/script/internal/basic/   # its headers, if it has any worth sharing
```

Everything up to `lower.cpp` is yours. For the dynamic engine, lowering means
building `dyn::Node`s: a Python `def` becomes an `NK::FunctionDecl` holding an
`NK::FunctionExpr`, a `class` an `NK::ClassDecl`, a list literal an
`NK::ArrayLit`, a dict an `NK::ObjectLit`, and so on; whatever has no
JavaScript spelling (`with` blocks, comprehensions, `yield`) is either
rewritten into what does, or refused with a `SyntaxError`. For the typed
engine, lowering is a checker: it produces `typed::CompilationUnit`,
`TypeDecl`, `MethodDecl` and the expression nodes with their `type`, `slot`
and `method` fields filled in, registers its types with the `TypeTable`
(`rt.types->adopt`) and its methods with the `AssemblyImpl`, exactly as
`Checker` does for C#. Read `src/lang/csharp/checker.cpp` before deciding on
the typed engine.

The engines' vocabulary is available to you: `Runtime::str`, `intern`,
`newCell`; the `TypeTable`'s `find`, `arrayOf`, `listOf`, `funcOf`,
`implicitlyConverts`; `dyn::compileProgram`; `typed::Emitter`. Host classes
the embedder bound are materialized by `rt.syncHostClasses()` -- call it
before resolving names, as both existing front ends do -- and appear as
`typed::Runtime::hostTypes` or on the dynamic engine's global object, with
`dyn::Runtime::hostDeclarations()` describing them if your checker wants to
read a declaration rather than a `TypeInfo`.

## 3. The registration

The skeleton of a dynamic-engine front end, which is all of
`src/lang/javascript/javascript.cpp` with the names changed:

```cpp
// src/lang/python/python.cpp
#include "script/internal/dyn/compiler.hpp"
#include "script/internal/dyn/runtime.hpp"
#include "script/internal/dyn/vm.hpp"
#include "script/internal/language.hpp"
#include "script/internal/python/parser.hpp"   // yours

namespace script {
namespace internal {
namespace dyn {

// Shared by every dynamic language: runs a compiled Program in the global scope.
Value runProgramUnit(ContextImpl& ctx, CompiledUnit& unit, const std::vector<Value>& args);

namespace {

std::shared_ptr<CompiledUnit> compilePython(ContextImpl& ctx, std::string_view source,
                                            std::string_view name,
                                            std::vector<Diagnostic>& diagnostics)
{
    Runtime& rt = ctx.dyn();
    rt.syncHostClasses();
    python::Parser parser(source, std::string(name));      // throws Error(SyntaxError)
    NodePtr program = python::lower(parser.parseModule());  // your tree -> dyn::Node
    // A front end with a checker reports here and returns null:
    //   diagnostics = python::check(*program, ...); if (!diagnostics.empty()) return nullptr;
    auto unit = std::make_shared<Program>();
    unit->language = Language::Python;
    unit->name = std::string(name);
    unit->code = compileProgram(rt, *program, std::string(name));
    return unit;
}

}  // namespace

const LanguageInfo& pythonLanguage() noexcept
{
    static const char* const extensions[] = {".py", nullptr};
    static const LanguageInfo info = [] {
        LanguageInfo l;
        l.id = Language::Python;
        l.name = "python";
        l.extensions = extensions;
        l.engine = EngineKind::Dynamic;
        l.compile = &compilePython;
        l.run = &runProgramUnit;
        l.declare = nullptr;   // or a hook that takes ambient declarations, as TypeScript has
        return l;
    }();
    return info;
}

}  // namespace dyn
}  // namespace internal
}  // namespace script
```

The three function pointers are the whole contract, declared in
`include/script/internal/language.hpp`:

| Hook | Contract |
| --- | --- |
| `compile` | Parse, check, lower, compile. Return the `CompiledUnit`; or fill `diagnostics` (each with `language` set) and return null when the program does not compile; or throw `Error(SyntaxError)` when it does not parse at all. Set `unit->language` and `unit->name`. |
| `run` | Run a unit this language compiled. Dynamic languages share `runProgramUnit`; a typed language does what `runCSharp` in `csharp.cpp` does -- `ensureInitialized`, then `invoke` the entry point. |
| `declare` | Optional. `Context::declare(text)` calls it on every language that has one; TypeScript appends the text to its ambient declarations. Leave null if the language has no such notion. |

A typed-engine front end returns a `typed::AssemblyImpl` (which is a
`CompiledUnit`) from `compile`, pushes it on `rt.assemblies`, and hands it to
`rt.jit->compileAll` when `options.compile.native` asks, as `compileCSharp`
does. An `AssemblyImpl` then serializes, loads and compiles to native code
with no further work from the front end.

## 4. Wire it in

Five edits, each a few lines:

1. **`include/script/language.hpp`** -- add the enumerator:
   ```cpp
   enum class Language : std::uint8_t { Auto = 0, CSharp, TypeScript, JavaScript, Python, C, Cpp };
   ```
   and the name in `toString`, `languageNamed` and the extension table
   (`kExtensions`) in `src/core/language.cpp`. The enum's order is `Auto`'s
   preference order; new languages go at the end.

2. **`src/core/language.cpp`** -- one block in the table:
   ```cpp
   #if SCRIPT_LANG_PYTHON
   namespace dyn { const LanguageInfo& pythonLanguage() noexcept; }
   #endif
   ...
   #if SCRIPT_LANG_PYTHON
           &dyn::pythonLanguage(),
   #endif
   ```

3. **`CMakeLists.txt`** -- one `script_language` call next to the others, and
   the option above them:
   ```cmake
   option(SCRIPT_LANG_PYTHON "Python: on the dynamic engine" ON)
   script_language(NAME python OPTION SCRIPT_LANG_PYTHON ENGINE DYNAMIC
       SOURCES
           src/lang/python/lexer.cpp
           src/lang/python/parser.cpp
           src/lang/python/lower.cpp
           src/lang/python/python.cpp)
   ```
   `ENGINE TYPED` or `ENGINE DYNAMIC` is what pulls the engine's sources in
   when the option is on (`cmake/ScriptLanguage.cmake`). Add the flag to the
   `foreach` that generates `config.hpp` and a `#define SCRIPT_LANG_PYTHON
   @SCRIPT_LANG_PYTHON_01@` line to `include/script/config.hpp.in`, so the
   embedder can ask at compile time.

4. **`tests/CMakeLists.txt`** -- `script_test_suites(python parser language builtins ...)`
   under `if(SCRIPT_LANG_PYTHON)`, with the suites in `tests/python/`. Copy
   `tests/javascript/lang.hpp` as `tests/python/lang.hpp` so every snippet in
   the directory runs as Python, and add the language to
   `tests/core/test_languages.cpp`, which walks `supportedLanguages()` and
   expects a `6 * 7` in each.

5. **`examples/CMakeLists.txt`** -- `script_examples(python)` under the same
   `if`; any `.cpp` dropped into `examples/python/` becomes an executable.

Then the docs: a `docs/languages/python.md` page, a row in the README's
options table, and the language's section in `docs/limitations.md`.

## 5. What you get for free

Once the `LanguageInfo` is in the table: `Context::eval`, `compile`,
`diagnose`, `run` and `evalFile` with the new `Language` or extension; the
`libscript` tool (`--lang py`, `.py` files, the REPL for a dynamic language);
`supported(Language::Python)`; every host binding the embedder declared, in
the shape your engine gives them; the heap, the limits, the interrupt hook and
the lock; `Assembly` for compile-once-run-many; and, on the typed engine,
images and native code.

## What the hook does not give you

* **A third engine.** The hook plugs a front end into an existing machine.
* **Cross-language calls.** Languages meet through the host, as they do today.
* **Automatic TypeScript-style declarations for your language.** The binding
  layer records each host member's C++ signature spelled for C# and for
  TypeScript (`HostMember::csharpReturn`, `typescriptReturn` and the parameter
  lists). A new typed language reads the C# spellings through
  `typed::Runtime::hostTypes`; a new checked dynamic language can render its
  own declarations from the same `HostMember` records the way
  `dyn::Runtime::hostDeclarations` does, or add a third spelling if the
  existing two do not fit.
