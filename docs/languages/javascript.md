# JavaScript

libscript's JavaScript is most of the language people write by hand. This
page is the contract: if something is listed as supported it has a test in
`tests/javascript/test_language.cpp` or `tests/javascript/test_builtins.cpp`.
A JavaScript source is one the host names with a `.js`, `.mjs` or `.cjs`
extension, passes with `Language::JavaScript`, or runs in a context whose
`Options::language` says so. It goes through the ECMAScript lexer and parser
that TypeScript also uses, with the TypeScript syntax switched off, and
straight into the dynamic engine's compiler; nothing is type-checked.

## Syntax

**Declarations.** `var`, `let`, `const`, function declarations (hoisted to the
top of their function), class declarations.

**Literals.** Decimal, hex (`0x`), octal (`0o`), binary (`0b`) and exponent
numbers with `_` separators; single- and double-quoted strings with `\n`,
`\xNN`, `\uNNNN` and `\u{...}` escapes; template literals with `${}`
substitution; array and object literals with shorthand properties, computed
keys, methods, getters, setters and `...` spread.

**Operators.** The full set: `+ - * / % **`, `= += -= *= /= %= **=`,
`&& || ?? ! & | ^ ~ << >> >>>` and their assignment forms, `&&= ||= ??=`,
`== != === !== < <= > >=`, `? :`, `,`, `typeof`, `void`, `delete`, `in`,
`instanceof`, `++`/`--` in both positions, `?.` for optional member access,
index and call.

**Functions.** Declarations, expressions, arrow functions with expression or
block bodies, default parameters, rest parameters, spread at call sites,
closures, `arguments` in non-arrow functions.

**Classes.** `class`, `extends`, `constructor`, instance and static methods,
getters and setters, instance and static fields, `super.member` and `super()`,
computed method names.

**Destructuring.** Array and object patterns in declarations, assignments,
parameters and `for..of` heads, with defaults, nesting, holes and `...rest` for
both arrays and objects.

**Statements.** `if`/`else`, `for`, `for..in`, `for..of`, `while`, `do..while`,
`switch` with fallthrough, `break`/`continue` with and without labels,
`return`, `throw`, `try`/`catch`/`finally` with an optional catch binding,
blocks, `debugger` (a no-op), automatic semicolon insertion.

**Iteration.** The iterator protocol throughout: `for..of`, spread, array and
object destructuring, `Array.from` and the `Map`/`Set` constructors all go
through `Symbol.iterator`, and any object that implements it participates.

## The standard library

| Global | Members |
|---|---|
| `Object` | `keys` `values` `entries` `fromEntries` `assign` `create` `freeze` `isFrozen` `preventExtensions` `is` `defineProperty` `getOwnPropertyDescriptor` `getOwnPropertyNames` `getPrototypeOf` `setPrototypeOf`; prototype: `hasOwnProperty` `isPrototypeOf` `propertyIsEnumerable` `toString` `valueOf` |
| `Function` | prototype: `call` `apply` `bind` `toString` |
| `Array` | `isArray` `from` `of`; prototype: `push` `pop` `shift` `unshift` `splice` `slice` `concat` `join` `reverse` `fill` `sort` `at` `indexOf` `lastIndexOf` `includes` `find` `findIndex` `forEach` `map` `filter` `some` `every` `reduce` `reduceRight` `flat` `flatMap` `keys` `values` `entries` `toString` |
| `String` | `fromCharCode` `fromCodePoint`; prototype: `charAt` `charCodeAt` `codePointAt` `at` `concat` `indexOf` `lastIndexOf` `includes` `startsWith` `endsWith` `slice` `substring` `substr` `toUpperCase` `toLowerCase` `trim` `trimStart` `trimEnd` `repeat` `padStart` `padEnd` `split` `replace` `replaceAll` `localeCompare` `normalize` `toString` `valueOf` |
| `Number` | `MAX_SAFE_INTEGER` `MIN_SAFE_INTEGER` `MAX_VALUE` `MIN_VALUE` `EPSILON` `POSITIVE_INFINITY` `NEGATIVE_INFINITY` `NaN` `isInteger` `isSafeInteger` `isFinite` `isNaN` `parseInt` `parseFloat`; prototype: `toString(radix)` `toFixed` `toPrecision` `toExponential` `valueOf` |
| `Boolean` | prototype: `toString` `valueOf` |
| `Math` | `PI` `E` `LN2` `LN10` `LOG2E` `LOG10E` `SQRT2` `SQRT1_2`, `abs` `floor` `ceil` `round` `trunc` `sign` `sqrt` `cbrt` `exp` `expm1` `log` `log2` `log10` `log1p` `sin` `cos` `tan` `asin` `acos` `atan` `atan2` `sinh` `cosh` `tanh` `pow` `hypot` `fround` `max` `min` `random` |
| `JSON` | `parse` `stringify(value, replacer, space)`, honouring `toJSON` |
| `Map`, `Set` | `size` `get` `set` `add` `has` `delete` `clear` `forEach` `keys` `values` `entries` `[Symbol.iterator]`, with SameValueZero keys |
| `Symbol` | `Symbol(description)` `Symbol.for`, `iterator` `asyncIterator` `toPrimitive` `toStringTag` `hasInstance`, `description`, `toString` |
| `Promise` | `resolve` `reject` `all` `race`; prototype: `then` `catch` `finally` |
| `Error` | `Error` `TypeError` `RangeError` `SyntaxError` `ReferenceError` `EvalError` `URIError` `InternalError`, each with `name`, `message` and `stack` |
| `Date` | `now`; prototype: `getTime` `getFullYear` `toISOString` `toJSON` `toString` `valueOf` |
| `console` | `log` `info` `debug` `trace` `warn` `error`, routed through the host print handler |
| globals | `globalThis` `undefined` `NaN` `Infinity` `parseInt` `parseFloat` `isNaN` `isFinite` `structuredClone` |

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

- **Numbers from the host may carry any numeric tag.** `Value(42)` is an `Int`
  and `Value(u'A')` a `Char` when the host makes them; a JavaScript program
  sees both as numbers (`typeof` says `"number"`, `'A'` is `65`). Every number
  the program itself produces is a `Double`.
- **A `void` host function returns `undefined`,** and a missing argument to a
  host function reads as `undefined`.

## What is not there yet

Generators, async functions, async generators, `for await`, classes with private
members, static blocks, computed accessors and `new.target`, tagged templates,
`Reflect`, array-likes in `Array.prototype` and regular expressions (lookbehind,
named groups, modifiers, `\p{...}` and the set notation of `v`, but not the
properties of strings such as `\p{RGI_Emoji}`), `BigInt`, and `ArrayBuffer`
with the typed arrays, `DataView` and `Atomics` (one thread: nothing ever waits) are
all there. These are not yet:
`Proxy`, iterator helpers, `with`,
modules through `import()`, `Temporal`. `eval` and `new Function` stay out by
design: the engine runs only what its host gives it.

Functions that use generators or async, and derived class constructors, run in
the interpreter. A `for-of` runs in native code only over a plain array, whose
iterator has nothing to close.

A regular expression runs in a matcher of its own, which backtracks on its own stack
and interprets a small instruction set: it has no native code, and that is the widest
remaining gap to engines that do. A Compact build leaves out the 43 KB of tables behind
`\p{...}`; a pattern that names a property is then a syntax error, apart from `Any`
and `ASCII`.

## What is not JavaScript

The parser is shared with TypeScript, so it knows the TypeScript syntax and
refuses it here, where JavaScript would: type annotations (`let x: number`),
`as` and `satisfies`, the non-null `!`, generics on functions, classes and call
sites, `enum` and `const enum`, `interface`, `type` aliases, `namespace`,
`declare`, `abstract`, `implements`, the access modifiers `public`,
`private`, `protected`, `readonly` and `override`, constructor parameter
properties, optional members and parameters (`x?`), definite assignment
(`x!`), index signatures, and bodyless method signatures in a class. Each is a
`SyntaxError`.

The TypeScript contextual keywords are ordinary names in JavaScript: `let type
= 1, interface = 2, as = 3;` is fine, and `a < b > (1)` is two comparisons, not
a generic call.

## The host, from JavaScript

A class the host declared with `hostClass` is an object on the global scope
holding functions; one bound with `Class<T>` is a constructor function with a
prototype, so `new Vec2(3, 4)`, `v.X = 10`, `v.Length`, `Vec2.Unit()` and
`v instanceof Vec2` all work. An instance the host or C# handed over reads its
members through the same prototype but has no own properties of its own. See
[../embedding.md](../embedding.md).

For what is deliberately absent, see [../limitations.md](../limitations.md).
