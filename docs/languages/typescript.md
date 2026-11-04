# TypeScript

libscript's TypeScript is TypeScript over the part of ES2020 that people
write by hand. This page is the contract: if something is listed as supported
it has a test in `tests/typescript/test_parser.cpp`, `test_types.cpp`,
`test_language.cpp` or `test_builtins.cpp`. A TypeScript source is one the
host names with a `.ts` or `.mts` extension, passes with
`Language::TypeScript`, or runs in a context whose `Options::language` says
so. It goes through the shared ECMAScript lexer and parser with the TypeScript
syntax on, then the structural checker, then the eraser, and what is left
runs on the dynamic engine exactly as [JavaScript](javascript.md) does; the
JavaScript page's syntax, library and semantics apply here unchanged. The
second half of this page is what the type checker does with the type syntax.

## Type syntax

**Annotations.** On variables, parameters, class members, function return types
and object type members. `let x: number`, `function f(a: string): void`,
`class C { n: number }`.

**Types.** Primitives (`number`, `string`, `boolean`, `symbol`, `object`,
`void`, `undefined`, `null`, `any`, `unknown`, `never`); literal types
(`'a'`, `42`, `-1`, `true`, `null`); arrays `T[]` and `readonly T[]`; tuples
`[A, B]` with optional, labelled and rest elements; unions `A | B` and
intersections `A & B`; object types `{ a: number; readonly b?: string }` with
call, construct and index signatures; function types `(a: A) => R` and
constructor types `new (a: A) => R`; `keyof T`; `T[K]`; `typeof name`;
parenthesised types; type predicates `x is T`.

**Declarations.** `type X = ...`, `interface X { ... }` with `extends`, `enum`
and `const enum`, `namespace` including the dotted `namespace A.B` form,
`declare` on any of them, and `export` inside a namespace.

**Generics.** `<T>`, `<T extends C>`, `<T = D>` on functions, methods, classes,
interfaces, type aliases, function types and arrow functions; type arguments at
call and construction sites, `f<T>(x)` and `new C<T>()`.

**Class modifiers.** `abstract` on the class and on members, `public`,
`private`, `protected`, `readonly`, `override`, `static`, `declare`,
`implements`, optional members `x?`, definite assignment `x!`, constructor
parameter properties `constructor(private readonly x: T)`, and bodyless member
signatures.

**Expressions.** `e as T`, `e satisfies T`, `e as const`, and the non-null
assertion `e!`.

Every TypeScript keyword but `enum` is contextual, so `type`, `interface`,
`namespace`, `declare`, `as`, `is`, `readonly`, `public` and the rest remain
legal variable, parameter and property names.

## Everything else

Syntax, the standard library and the run-time semantics are the JavaScript
page's, without exception: [javascript.md](javascript.md).

## Semantics worth knowing

- **Numbers are IEEE-754 doubles** and print exactly as the specification says,
  including `0.1 + 0.2 === 0.30000000000000004`, `1e21`, and `1e-7`.
- **Strings are UTF-16**, so `length`, indexing and `charCodeAt` agree with a
  browser. Conversion to and from UTF-8 happens only at the C++ boundary. Case
  conversion covers ASCII and Latin-1.
- **`for (let i ...)` gives each iteration its own binding**, so closures made
  in a loop capture the value of that iteration; `var` behaves the classic way.
- **Arrow functions have no `this` of their own** and inherit it, along with
  `super`, from the enclosing method.
- **Arrays are dense.** Assigning far past the end throws a `RangeError` rather
  than quietly allocating; deleting an element leaves `undefined`.
- **Everything runs in one mode.** There is no separate sloppy/strict split:
  assigning to an undeclared name creates a global, and writes to non-writable
  properties are ignored rather than throwing.

For what is deliberately absent, see [../limitations.md](../limitations.md).

## The type system

Every compile of a TypeScript source runs the program past the type checker
before anything executes. This document says what it understands, what it reports, and where it
deliberately stops.

### What is checked

```ts
type Shape =
    | { kind: 'circle'; radius: number }
    | { kind: 'rect'; width: number; height: number };

function area(s: Shape): number {
    if (s.kind === 'circle') return Math.PI * s.radius ** 2;
    return s.width * s.height;
}

const shapes: Shape[] = [{ kind: 'circle', radius: 2 }];
const total = shapes.reduce((sum, s) => sum + area(s), 0);
```

* **Primitives and literals.** `number`, `string`, `boolean`, `symbol`, `void`,
  `undefined`, `null`, `any`, `unknown`, `never`, and the literal types
  `'a'`, `42`, `true`.
* **Objects, interfaces and type aliases.** Compared structurally: a value fits
  a type when it has the members the type asks for. `extends` on an interface
  copies members in.
* **Arrays and tuples.** `T[]`, `readonly T[]`, `[A, B]`, and index signatures
  `{ [key: string]: T }`.
* **Unions and intersections.** A union source must satisfy the target through
  every constituent; a union target needs only one that fits. An intersection of
  object types is the union of their members.
* **Functions.** Arity, argument types, return types, optional parameters,
  default parameters, rest parameters. A callback that ignores trailing
  arguments still satisfies a signature that supplies them, which is what makes
  `xs.map(x => x)` type.
* **Generics.** Type parameters with constraints and defaults on functions,
  classes, interfaces and aliases; inference from arguments at the call site;
  explicit type arguments with `f<T>(x)`.
* **Classes.** Fields, methods, getters and setters, `static`, `readonly`,
  `abstract`, `public`/`private`/`protected`, `implements`, constructor
  parameter properties, and inheritance.
* **Enums and namespaces.** Both are types and values. A namespace's type is
  built from what its body exports.
* **Assertions.** `as`, which is rejected when the two types have no overlap;
  `satisfies`, which checks without changing the reported type; `as const`,
  which keeps literal types; and `!`, which removes `null` and `undefined`.

### Narrowing

The checker follows control flow through the forms that carry information:

```ts
function describe(v: string | number | null): string {
    if (v === null) return 'nothing';           // v: string | number
    if (typeof v === 'string') return v;        // v: string
    return v.toFixed(2);                        // v: number
}
```

* `typeof x === '...'`
* `x === null`, `x !== undefined`, and comparison against any literal
* truthiness, in `if`, `&&`, `||`, `?:` and `??`
* `x instanceof C`
* a discriminant: `s.kind === 'circle'` narrows `s`, not just `s.kind`
* a user-defined guard: `function isFoo(x: unknown): x is Foo`
* an early exit -- after `if (!ok) return;` the rest of the block knows `ok`

### Widening

A literal written in a mutable position widens, or nothing could be assigned to
it afterwards:

```ts
let n = 1;                 // number, so `n = 2` is fine
const xs = [1, 2];         // number[], so `xs.push(3)` is fine
const o = { a: 1 };        // { a: number }, so `o.a = 2` is fine
const p = { a: 1 } as const;   // { readonly a: 1 }, so `p.a = 2` is not
```

A contextual type stops the widening, which is how the literal members of a
discriminated union survive:

```ts
const shapes: Shape[] = [{ kind: 'circle', radius: 2 }];   // 'circle' stays
```

### Excess properties

An object literal written directly in a typed position is checked for properties
the target does not have. Structural typing allows the extra ones, but a literal
with nowhere else to go is almost always a typo:

```ts
interface Point { x: number; y: number }
const p: Point = { x: 1, y: 2, z: 3 };   // 'z' does not exist in type 'Point'
```

### Strictness

The defaults are `tsc`'s own, so a program with no annotations at all -- plain
JavaScript -- type-checks:

```cpp
script::Context::Options options;
options.check = script::CheckOptions::strict();   // strictNullChecks, noImplicitAny
options.check.enabled = false;                     // or turn checking off entirely
script::Context ctx(options);
```

| Option | Default | Effect |
|---|---|---|
| `enabled` | `true` | Run the checker at all |
| `strictNullChecks` | `false` | `null` and `undefined` belong only to types that admit them |
| `noImplicitAny` | `false` | A parameter whose type cannot be inferred is an error |

### Reporting

`Context::eval` and `Context::compile` throw `script::CompileError`, which
carries every diagnostic rather than just the first. `Context::diagnose`
returns them instead:

```cpp
for (const script::Diagnostic& d : ctx.diagnose(source, "rules.ts"))
    std::fprintf(stderr, "%s\n", d.toString().c_str());   // rules.ts(12): error: ...
```

### Telling the checker about the host

The checker cannot read a C++ signature, but the binding layer writes the
declaration for it: every `hostClass` is in force as `declare const Env: {
Now(): number; ... }` and every `Class<T>` as `declare class Vec2 { ... }`,
so `Env.Log(42)` where `Log` takes a `string` is a diagnostic. A name the
embedder installs directly on the global object has no declaration and is
typed `any`; `Context::declare` supplies one, and the declarations accumulate
and apply to every later evaluation:

```cpp
ctx.global().set("now", ctx.fn("now", [] { return currentMillis(); }));
ctx.declare("declare function now(): number;");
```

### Where it stops

Each evaluation is checked on its own. Types declared by an earlier `eval` are
not carried into the next one: keeping them would mean a table that grows for as
long as the process runs, which is the one thing the engine is built not to do.
`declare` -- and the declarations the bindings derive -- is the supported way
to make a type outlive one evaluation.

The following are parsed and erased but not modelled; they resolve to `any`
rather than being rejected:

* conditional types, mapped types, `infer`, template literal types
* `Partial`, `Readonly`, `Required` and the other mapped utility types
  (`Record<K, T>` is provided as an approximation with an unchecked key type)
* declaration merging beyond `interface` into `interface`
* function and method overload signatures
* modules, `import` and `export` (`export` is accepted inside a namespace)
* `unique symbol`, and symbol-keyed members other than `[Symbol.iterator]`

Two known imprecisions, both consequences of keeping the checker small:

* two type parameters with the same name and the same constraint in different
  declarations are the same type;
* a destructuring parameter with no annotation is `any` rather than a shape
  inferred from the pattern.
