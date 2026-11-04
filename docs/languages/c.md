# C

libscript's C is the C a program written for the engine can reasonably use:
functions, the numeric types, structs, arrays, strings, the control
statements, the preprocessor's `#define` constants, and a slice of the
standard library. This page is the contract: if something is listed as
supported it has a test in `tests/c/`. A C source is one the host names with
a `.c` or `.h` extension, passes with `Language::C`, or runs in a context
whose `Options::language` says so.

C runs on the typed engine, the one C# runs on. The front end parses C and
produces the same resolved syntax tree the C# front end does, so the checker,
the emitter, the interpreter, the code generators and the image format are
shared; `Assembly::serialize` and `Context::load` work for a C program, and
`Context::Options::jit` applies. What the front end adds is C's silent
conversions -- an `int` from a `double`, a `char` from an `int`, a number or
a pointer where a condition is wanted, a comparison where a number is wanted
-- and a mapping from C's memory model onto the engine's objects, described
under *Pointers* below.

## Program shape

```c
#include <stdio.h>
#define N 4

typedef struct { double x, y; } Point;

static int calls = 0;

double dot(Point* a, Point* b) { return a->x * b->x + a->y * b->y; }

int main(void) {
    Point p = {1, 2}, q = {3, 4};
    int squares[N];
    for (int i = 0; i < N; i++) squares[i] = i * i;
    printf("%.1f %d\n", dot(&p, &q), squares[N - 1]);
    return ++calls;
}
```

`main` is the entry point; its return value is what `Context::eval` and
`Context::run` hand back (`void main` gives 0). Functions may be defined in
any order. Globals keep their values between runs of the same `Assembly`.
Every function and global becomes a static member of one synthesized class,
so a stack trace reads `at C$.main (prog.c:12)`.

## Types

| C | Engine |
|---|---|
| `char`, `signed char`, `int8_t` | `char` (a 16-bit code unit; arithmetic promotes to `int` as in C) |
| `unsigned char`, `uint8_t` | `byte` |
| `short`, `unsigned short` | `short`, `ushort` |
| `int`, `unsigned`, `size_t`, `int32_t`, `uint32_t` | `int`, `uint` (32-bit, wrapping) |
| `long`, `long long`, `unsigned long`, `int64_t`, `uint64_t` | `long`, `ulong` (64-bit) |
| `float`, `double`, `long double` | `float`, `double` |
| `_Bool`, `bool` | `int` (`true` is 1) |
| `enum` | `int`; enumerators are constants |
| `struct T`, `struct T*` | one class `T`; see *Pointers* |
| `T[N]`, `T[]`, `T*` (variables and fields) | an array `T[]` |
| `char*`, `const char*` | an immutable string |
| `char[N]` | a mutable char array |
| `T*` (parameter) | `ref T` or `T[]`; see *Pointers* |

`typedef` names, `struct` tags, and `typedef struct { ... } Name;` all work.
`const` is honoured where the engine can (a `const` scalar with a constant
initializer is a constant, usable in a `case` label or an array length);
`static`, `extern`, `volatile`, `register` and `inline` are accepted. `sizeof`
gives the sizes a 64-bit machine has (`sizeof(int)` is 4, pointers and
`long` are 8), and `sizeof(arr) / sizeof(arr[0])` is the array's length.

## Pointers

There is no address arithmetic in the engine, so a pointer is whichever engine
reference the program uses it as:

- **A struct is a class.** `struct T x;` allocates one; `struct T* p = &x` is
  another reference to it; `p->f`, `(*p).f` and `x.f` are the same access;
  `malloc(sizeof(struct T))` (with or without a cast) is a fresh `T`;
  `free(p)` does nothing, because the collector owns the memory. A struct
  field of struct type is allocated with its parent. Passing or assigning a
  struct shares it rather than copying it -- the one place a C program will
  notice.
- **An array is an array.** `int a[10]`, `int* p = a`, `int* p = malloc(n *
  sizeof(int))` and `calloc(n, sizeof(int))` are all `int[]`; indexing is
  bounds-checked; two-dimensional arrays are arrays of arrays. `&a[0]` where an
  array is expected is the array.
- **A `char*` is a string.** Literals, `strlen`, `strcmp` and `printf("%s")`
  work; `s[i]` reads one character and reads `'\0'` one past the end, so
  `while (s[i])` terminates; writing through a `char*` is refused. A `char
  buf[N]` is a mutable array, and `printf("%s", buf)`, `puts(buf)` and
  `strlen(buf)` read it up to its NUL.
- **A scalar pointer parameter is `ref` or an array**, decided by how the
  function body uses it: `*p` makes it a reference (`void swap(int* a, int*
  b)`, called as `swap(&x, &y)`), `p[i]` makes it an array (`int sum(int* a,
  int n)`, called as `sum(a, n)`), and passing it on to another function
  makes it whatever that function's parameter is. A body that does both is
  refused. A reference argument has to be `&local` or `&parameter`; the address
  of a global or of an element cannot be taken.

Anything else -- `p + 1`, `p++`, `void*`, function pointers, a pointer stored
in a local without a call to give it meaning (`int* p = &x`), casts between
pointer types, unions, bit-fields -- is refused at compile time with a message
that names the construct.

## Statements and expressions

`if`/`else`, `while`, `do`/`while`, `for` (with a declaration or a comma
list in the header), `switch` with `case`/`default` (stacked labels are
fine; falling from one non-empty case into the next is refused), `break`,
`continue`, `return`, blocks, declarations anywhere in a block, static locals,
compound literals (`(Point){1, 2}`), designated initializers (`{.x = 1}`),
array initializers (`{1, 2, 3}`, `{0}`, a string for a `char[]`), the full
operator set including the comma operator in `for` headers and expression
statements, `sizeof`, casts, `? :`. Not supported: `goto` and labels, the
comma operator elsewhere, variable-length arrays of structs.

## The preprocessor

`#include` lines are dropped: the library below is always present. `#define
NAME tokens` defines an object-like macro, expanded wherever the name appears
(a macro may use earlier macros; a backslash continues the line). `#undef`
works. `#if`, `#ifdef`, `#ifndef`, `#else`, `#endif`, `#pragma`, `#error`
and `#line` are skipped, so the code in every branch is compiled: an include
guard guards nothing here, and there is no conditional compilation.
Function-like macros are refused.

## The C library

Available under their usual names, whichever header a program includes:

| Header | Functions |
|---|---|
| `stdio.h` | `printf`, `fprintf(stdout|stderr, ...)`, `puts`, `putchar`; `%d %i %u %x %X %o %f %F %e %E %g %G %c %s %p %%` with flags, width, precision and `*`; `stdout`, `stderr`, `EOF` |
| `string.h` | `strlen`, `strcmp`, `strncmp`, `strstr` and `strchr` (returning an index, or -1) |
| `stdlib.h` | `malloc`, `calloc`, `free` (as described above), `atoi`, `atol`, `atof`, `abs`, `labs`, `rand`, `srand`, `RAND_MAX`, `EXIT_SUCCESS`, `EXIT_FAILURE`, `NULL` |
| `ctype.h` | `isdigit`, `isalpha`, `isalnum`, `isspace`, `isupper`, `islower`, `ispunct`, `toupper`, `tolower` |
| `math.h` | `fabs`, `sqrt`, `cbrt`, `pow`, `exp`, `log`, `log2`, `log10`, `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `atan2`, `sinh`, `cosh`, `tanh`, `floor`, `ceil`, `round`, `trunc`, `fmod`, `fmin`, `fmax`, `hypot`, `isnan`, `isinf`, `M_PI`, `M_E`, `M_SQRT2`, `INFINITY`, `NAN` |
| `limits.h`, `float.h` | `INT_MAX`, `INT_MIN`, `UINT_MAX`, `LONG_MAX`, `LONG_MIN`, `LLONG_*`, `CHAR_*`, `SHRT_*`, `UCHAR_MAX`, `DBL_MAX`, `DBL_MIN`, `DBL_EPSILON`, `FLT_MAX`, `FLT_EPSILON` |
| `assert.h` | `assert(expr)`, which raises `InvalidOperation` |
| `stdbool.h`, `stdint.h`, `stddef.h` | `bool`, `true`, `false`, the fixed-width names, `size_t`, `NULL` |

Output goes through `Context::setPrintHandler` (`fprintf(stderr, ...)` marks
it as an error). There is no input, no file, no `exit`, no `time`, no `scanf`,
no `strcpy` (strings are immutable; use a `char[]` and assign characters).

## Host bindings

A host class bound with `hostClass` or `Class<T>` is visible under its name.
`Env.Greet("x")` calls a static member and `Env.Add(1, 2)` converts its
arguments as a C call would; `Vec2 v = Vec2(3, 4);` constructs an instance,
`v.Length()` and `v.X` reach its members, `Vec2.Unit()` calls a static one; a
`Vec2*` parameter is the same as a `Vec2` parameter. Members whose C++
signature has no C spelling (a `std::map`, a `std::function`, a
`std::optional`) are not visible and are reported as missing.

## Semantics worth knowing

- **Integer arithmetic wraps** at 32 or 64 bits, `/` truncates, `%` takes the
  dividend's sign, shifts are two's complement, exactly as the engine's C#
  does and as C on a 64-bit machine does.
- **`char` is a UTF-16 code unit** and unsigned. `'a' + 1` is an `int`;
  assigning it back to a `char` truncates.
- **An `int` from a `double` truncates towards zero**, as in C.
- **Conversions between `unsigned int` and `int`** follow the engine's
  promotion: `unsigned + int` is a 64-bit `long`, then narrowed on
  assignment. The result is the same wrapped value C computes.
- **A comparison is a `bool` inside the engine** and becomes `1` or `0` the
  moment a program stores or adds it.
- **Struct assignment aliases** rather than copies (see *Pointers*).
- **A runtime error is an exception**: an index out of range, a division by
  zero, a null struct pointer, an `assert` and the stack limit all reach the
  host as `script::Exception` with the C source line, and cannot be caught
  from C.
- **Uninitialised variables are zero** (or null), not garbage.
