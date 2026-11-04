# Python

libscript's Python is the everyday core of Python 3: functions, classes,
lists, dicts, sets, tuples, strings and f-strings, comprehensions, exceptions,
the built-in functions people actually call, and the `math` and `random`
modules. This page is the contract: if something is listed as supported it has
a test in `tests/python/`. A Python source is one the host names with a `.py`
extension, passes with `Language::Python`, or runs in a context whose
`Options::language` says so.

Python runs on the dynamic engine. The front end parses Python and lowers it
directly to the engine's ECMAScript syntax tree -- there is no Python
interpreter and no Python object model of its own. A Python list *is* an
engine array, a dict *is* a `Map`, a class *is* a class, a function *is* a
function; which is what lets a value cross to JavaScript, TypeScript or the
host with no conversion, and what the semantic notes at the end of this page
are about.

## Syntax

**Statements.** Assignment, chained (`a = b = 1`) and destructuring (`a, b =
b, a`, `first, *rest = xs`, `(a, b), c = ...`); augmented assignment (`+= -=
*= /= //= %= **= &= |= ^= <<= >>=`); annotated assignment (the annotation is
dropped); `if`/`elif`/`else`; `while`; `for ... in`; `break`, `continue`,
`pass`, `return`; `def` with defaults, `*args` and keyword arguments;
`lambda`; `class` with single inheritance, `__init__`, `super()`,
`@staticmethod`, `@classmethod`, `@property` and `@x.setter`; `try` /
`except [Type [as name]]` / `finally`, `raise`, bare `raise`; `assert`;
`del`; `global`, `nonlocal`; `import math`, `import random as r`,
`from math import sqrt, pi`; docstrings; `;`-separated simple statements.

**Expressions.** Arithmetic (`+ - * / // % **`), comparison chains (`a < b <=
c`), `is`, `is not`, `in`, `not in`, `and`, `or`, `not`, `x if c else y`,
bitwise operators, indexing with negative indices, slicing with steps,
attribute access, calls with `*args` and keyword arguments, list, tuple, dict
and set displays, list, dict and set comprehensions and generator
expressions (evaluated eagerly into a list), string concatenation of adjacent
literals, f-strings with conversions (`!r`), format specs (`{x:>8.2f}`,
nested `{x:{width}}`) and `=` self-documenting fields, `%`-formatting,
`str.format`.

**Literals.** Decimal, hex, octal and binary integers with `_`; floats;
single-, double- and triple-quoted strings with escapes; raw strings; `True`,
`False`, `None`.

**Not accepted, with a message that says so:** `yield` and generators,
`async`/`await`, `with`, `**kwargs` parameters and `**` in calls,
keyword-only parameters, multiple inheritance, decorators other than the four
above, `else` on loops and on `try`, `:=`, the `@` operator, complex and
bytes literals, `import` of modules other than `math`, `random`, `sys` and
`time`, `from ... import *`, multi-dimensional subscripts (`a[i, j]`).

## Built-in functions

`print` (`sep`, `end`), `len`, `range`, `str`, `repr`, `int` (with `base`),
`float`, `bool`, `list`, `tuple`, `dict`, `set`, `frozenset`, `abs`, `min`,
`max` (`key`, `default`), `sum`, `sorted` (`key`, `reverse`), `reversed`,
`enumerate` (`start`), `zip`, `map`, `filter`, `any`, `all`, `round` (half to
even), `divmod`, `pow`, `chr`, `ord`, `hex`, `oct`, `bin`, `isinstance`,
`issubclass`, `type`, `hasattr`, `getattr`, `setattr`, `delattr`, `callable`,
`iter`, `next`, `input`, `id`.

`input` reads through `Context::setReadLineHandler`; `print` writes through
`Context::setPrintHandler`.

## Methods

| Type | Methods |
|---|---|
| `str` | `upper` `lower` `capitalize` `title` `swapcase` `strip` `lstrip` `rstrip` `split` `rsplit` `splitlines` `partition` `rpartition` `join` `replace` `find` `rfind` `index` `rindex` `count` `startswith` `endswith` `isdigit` `isnumeric` `isdecimal` `isalpha` `isalnum` `isspace` `isupper` `islower` `center` `ljust` `rjust` `zfill` `format` |
| `list` | `append` `extend` `insert` `remove` `pop` `clear` `index` `count` `sort` (`key`, `reverse`) `reverse` `copy` |
| `dict` | `keys` `values` `items` `get` `pop` `setdefault` `update` `popitem` `clear` `copy` |
| `set` | `add` `remove` `discard` `pop` `clear` `copy` `update` `union` `intersection` `difference` `symmetric_difference` `issubset` `issuperset` `isdisjoint` |

Everything the engine's JavaScript gives these types is also there (`xs.push`,
`s.toUpperCase()`, `d.has(k)`), since they are the same objects.

## Exceptions

`Exception` (the engine's `Error`, so it also catches errors the engine
raises), `ValueError`, `TypeError`, `KeyError`, `IndexError`,
`ZeroDivisionError`, `RuntimeError`, `AssertionError`, `AttributeError`,
`NotImplementedError`, `StopIteration`, `LookupError`, `ArithmeticError`,
`NameError`, `OverflowError`, `RecursionError`, `EOFError`. `str(e)` is the
message. A user class `class E(Exception)` works as expected. An exception
that escapes reaches the host as `script::Exception` with a stack.

## Special methods

`__init__`, `__str__`, `__repr__`, `__eq__`, `__len__`, `__getitem__`,
`__setitem__`, `__delitem__`, `__contains__`, `__bool__`, `__iter__` (which
becomes `[Symbol.iterator]`), `__lt__` (used by `sorted`), `__format__`,
`__int__`, `__float__`, `__abs__`. Arithmetic dunders (`__add__` and friends)
are not consulted.

## Modules

| Module | Members |
|---|---|
| `math` | `pi` `e` `tau` `inf` `nan` `sqrt` `floor` `ceil` `trunc` `fabs` `sin` `cos` `tan` `asin` `acos` `atan` `atan2` `sinh` `cosh` `tanh` `exp` `log` `log2` `log10` `pow` `hypot` `degrees` `radians` `isnan` `isinf` `isfinite` `isclose` `gcd` `lcm` `factorial` `fsum` `prod` `comb` `copysign` `fmod` `modf` |
| `random` | `random` `randint` `randrange` `uniform` `choice` `shuffle` `sample` `seed` (a no-op) |
| `sys` | `argv` `maxsize` `version` `exit` |
| `time` | `time` `perf_counter` `monotonic` `sleep` (a no-op) |

## Host bindings

A host class bound with `hostClass` or `Class<T>` is a global: `Env.Greet("x")`
calls a static member, `Vec2(3, 4)` constructs, `v.length()` and `v.x` reach
members, and `isinstance(v, Vec2)` is true. A `std::vector` handed in is a
list, a `std::map` is an object, a `std::function` is callable. Python
functions and objects handed back to C++ are called with `Value::call` and
`Value::invoke`.

## Semantics worth knowing

Where Python and the engine's object model disagree, the front end sides with
Python at the places programs notice, and says so where it does not.

- **Truth is Python truth.** `if xs:`, `while n:`, `not x`, `x or default`
  and `x and y` treat `None`, `False`, `0`, `""`, `[]`, `{}` and `set()` as
  false (`__bool__` and `__len__` are consulted).
- **`==` compares by value** for lists, tuples, dicts and sets, and calls
  `__eq__`; `is` compares identity. `x == None` is `x is None`.
- **Numbers are doubles.** There is one numeric type: `10 / 2` is `5` and
  prints as `5`, not `5.0`; `str(3.0)` is `"3"`; integers above 2^53 lose
  precision; there are no big integers. `//` floors, `%` takes the sign of
  the divisor, `**` is `Math.pow`. Division by zero yields `inf` or `nan`
  rather than raising `ZeroDivisionError` (only `%` and `divmod` raise).
- **A dict is a `Map`**, keyed by identity for objects and by value for
  strings and numbers. `(1, 2)` as a key is a fresh list each time and will
  not be found again; use a string key.
- **A tuple is a list.** It is mutable and prints in brackets.
- **Late binding in closures.** A loop variable is one binding for the whole
  loop, so lambdas made in a loop or comprehension all see the final value,
  exactly as in Python.
- **Default arguments are evaluated per call**, not once at definition time,
  so `def f(x=[])` gets a fresh list each call.
- **A derived class whose `__init__` never calls `super().__init__`** gets
  the base constructor called first with no arguments.
- **Chained comparisons evaluate the middle operand twice** (`a < f() < c`
  calls `f` twice).
- **`a[i] += v` evaluates `a` and `i` twice.**
- **Keyword arguments** reach a function defined in the same evaluation by
  position, so its defaults apply. For a built-in they travel as a trailing
  marker object the built-in understands. For a method, or a function from
  another evaluation, they arrive as that marker object as the last argument.
- **A class defined in an earlier evaluation** is recognised by its
  capitalised name (`Point(1, 2)` constructs, `point(1, 2)` calls); a
  class name starting with a lowercase letter has to be defined in the same
  evaluation or called with `new` from JavaScript.
- **Module-level names are globals** of the context, shared with JavaScript
  and TypeScript and kept between evaluations. The value of an evaluation is
  its last expression statement (`6 * 7`), or nothing when that is an
  assignment.
- **Strings are UTF-16**, as everywhere in the engine: `len("é")` is 1 but a
  character outside the basic plane counts as 2.
- **`print(3.0)`** prints `3`, and `print(True)` prints `True`.

## Bound methods

Reading a method without calling it binds it, as Python says: `f = obj.method`,
`key=words.count`, `map(text.count, items)`. The built-in types' methods bind the
same way as your own classes'. An instance's own callable attribute and a function
read from a class or a module stay plain values.
