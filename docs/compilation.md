# Compilation and assembly images

Compiling and running are separate steps in every language, and the thing in
between is a first-class object:

```cpp
script::Assembly rules = ctx.compile(source, "rules.cs");
script::Value verdict = ctx.call(rules, "Rules.Evaluate", {script::Value(91.0)});

script::Assembly handler = ctx.compile(source, "handler.js");
ctx.run(handler);           // and again, without parsing again
ctx.run(handler);
```

`Assembly::language()` says what it holds; `name()`, `methods()`,
`entryPoint()` and `imageSize()` describe it. A `run` of a TypeScript or
JavaScript assembly executes the whole program in the global scope and returns
the completion value of its last statement; a `run` of a C# assembly calls its
entry point.

## C# images

A C# assembly also has a portable form:

```cpp
std::vector<std::uint8_t> image = rules.serialize();
// ... later, elsewhere, in another process ...
script::Assembly loaded = ctx.load(image.data(), image.size());
ctx.call(loaded, "Rules.Evaluate", {script::Value(91.0)});
```

Or from the command line:

```
$ libscript -c rules.sxa rules.cs      # compile to an image
$ libscript -r rules.sxa               # run one
$ libscript --dump rules.cs            # disassemble what it compiled to
```

`serialize()` on a TypeScript or JavaScript assembly throws
`Error(NotSupported)`: the dynamic engine's compiled program is a tree of
`FunctionCode` with live interned atoms in it, and no image format exists for
it yet. Compile once and keep the `Assembly` instead.

## What a C# assembly is

Everything the VM needs and nothing it does not:

* **types** -- kind, base, interfaces, field slots, the flattened vtable,
  properties, constants;
* **methods** -- signature, flags, frame size, local types, exception handler
  table, line table, and the bytecode;
* **reference tables** -- what the instruction operands index;
* **literals** -- the string constants;
* **statics** -- the backing store for every static field the assembly declares.

Every name in the program has already been resolved into one of those. There is
no symbol table at run time, because nothing looks anything up by name.

## The image format

```
magic   "CSIL"
version u32
strings count, then each as (u32 length, UTF-8 bytes)
body    u32 length, then:
          type count, method count      -- so a loader can pre-create both
          type definitions
          method definitions, bodies included
          the type and method reference tables
          literals, statics, entry point, static initializers, names
```

Little-endian throughout, flat, and pooled: a name shared by a hundred methods
is stored once and every use of it is four bytes. The rules demo in
`examples/csharp/compile.cpp` -- two classes, an enum, eight methods -- is
about two kilobytes.

A reference to something the assembly defines is an index into its own table. A
reference to anything else -- the core library, a host class, a `List<int>` --
is a name, resolved when the image is loaded. That is what makes an image
portable between processes without pinning it to one build of the engine:
`Context::load` reconstructs `List<int>` if this context has not needed one
yet, materializes the host classes the embedder has declared, and refuses the
image with `ErrorCode::BadImage` if something it needs is genuinely missing.

Loading does no lexing, no parsing, no type checking and no layout. It reads
the tables and fixes up references.

## What loading does not check

The loader validates structure -- magic, version, bounds, resolvable references
-- but it does not verify that the bytecode is type-correct. An image is
trusted input, like a shared library: compile it yourself, or receive it over a
channel you trust. Untrusted *source* is a different matter and is safe; see
the sandboxing sections of [embedding.md](embedding.md).

## Determinism

Two compilations of the same source produce byte-identical images
(`imageIsStable` in `tests/csharp/test_module.cpp`), which makes them cacheable
and comparable across a build.

## Compile options

```cpp
script::Context::Options options;
options.compile.checkedArithmetic = true;   // integer overflow throws instead of wrapping
options.compile.debugInfo = true;           // line numbers, so exceptions carry a stack
options.compile.optimize = true;            // fold constants before emitting
options.compile.native = true;              // also compile to machine code, at compile
options.check = script::CheckOptions::strict();   // TypeScript: how strictly it is checked
script::Context ctx(options);
```

`compile` is read by the C# front end, `check` by the TypeScript one. Turning
`debugInfo` off drops the line tables, which is most of what an image carries
beyond code. `check.enabled = false` skips type checking for a TypeScript
source that was already checked.

## More than one unit

A C# program does not have to be one string. Each `compile` call is a unit, and
the units see each other:

```cpp
script::Assembly model = ctx.compile(readFile("model.cs"), "model.cs");
script::Assembly rules = ctx.compile(readFile("rules.cs"), "rules.cs");
script::Assembly main  = ctx.compile(readFile("main.cs"),  "main.cs");
ctx.run(main);
```

The rules, which are the rules an image loader follows too:

* **Compile in dependency order.** A unit is checked against what is already in
  the context, so `rules.cs` can use `model.cs` but not the reverse. There is
  no forward reference across units, and no cycles.
* **Each type keeps its statics at home**, in the assembly that declared it,
  wherever they are read from.
* **Static initializers run before the first call into the context**, for every
  loaded unit -- not just the one being entered -- so a unit that only holds a
  table is initialized even if nothing has called into it yet.
* **Names are global across units.** Declaring the same class twice is a
  duplicate-definition error.
* **Each unit serializes separately**, and the images load in the same order
  the sources compiled in. A missing dependency is `ErrorCode::BadImage`.

[examples/csharp/modules.cpp](../examples/csharp/modules.cpp) does exactly
that, including the error you get for compiling out of order.

TypeScript and JavaScript have their own version of the same thing: every
program's top-level declarations land on the global object, so a sequence of
`eval`s builds up a shared scope, and `Context::declare` carries a TypeScript
type from one compile to the next.

## And then native code

An image is also what the code generator consumes: the unit of compilation is a
`MethodInfo` out of a loaded assembly, whether that assembly came from source
or from bytes. `options.compile.native = true` compiles every method the back
end supports as part of the compile or load; `ctx.compileNative(assembly)` is
the same thing, later. Either way the methods outside the supported subset keep
interpreting, and either call is a no-op for a TypeScript or JavaScript
assembly. See [jit.md](jit.md).
