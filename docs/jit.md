# Native code generation

The typed engine -- the machine C# runs on -- has two back ends,
`src/typed/jit/jit_arm64.cpp` and `src/typed/jit/jit_x64.cpp`, both emitting
machine code directly with no assembler or linker involved. The dynamic engine
has none: its instruction set inspects operand types at run time, which is the
opposite of what these generators need. The back ends cover the part of a C#
program where the interpreter's overhead dominates:
arithmetic, comparisons, conversions and control flow over primitive locals.
Everything else keeps interpreting, in the same program, with no boundary for
the host to think about.

## Asking for it

```cpp
script::Context::Options options;

// Ahead of time: compile everything the back end handles, at load.
options.compile.native = true;

// Or on demand: interpret until a method has earned it.
options.jit.mode = script::JitOptions::Mode::Tiered;  // or Eager, or Off
options.jit.threshold = 2000;                     // calls before compiling
options.jit.codeCacheLimit = 8u << 20;
options.jit.dumpCode = false;

script::Context ctx(options);
std::puts(ctx.jitBackend());            // "arm64", "x86-64" or "interpreter"
ctx.compileNative(assembly);            // or decide later

const script::Context::Stats s = ctx.stats();
s.methodsCompiled;                      // how many made it
s.nativeCodeBytes;                      // what the code cache costs
```

`libscript --native` and `libscript --jit` are those two switches from the command line,
and `libscript --stats` prints the counters. Asking is always safe: a host with no
back end, or a method outside the subset, keeps interpreting and produces the
same answer.

## What it is worth

From `examples/csharp/native.cpp` on arm64 (Cortex-X925, gcc 13, -O2):

| kernel | interpreted | native | speedup |
| --- | --- | --- | --- |
| integer mixing loop, 20M iterations | 2501 ms | 232 ms | 10.8x |
| escape-time loop over doubles | 561 us | 47 us | 11.8x |
| nested 64-bit arithmetic | 574 ms | 57 ms | 10.1x |

Ten times is what removing dispatch, `Value` construction and the per-opcode
bookkeeping buys. It is not what a register allocator buys -- there isn't one
yet, and every intermediate still goes through its stack slot in memory.

## The subset

`planBody` in `src/typed/jit/jit.cpp` decides. A method qualifies when all of:

* every local and parameter is a **primitive** -- `bool`, `char`, the integer
  types, `float`, `double` -- and none is captured by a lambda;
* the return type is primitive or `void`;
* every instruction is **arithmetic, bitwise, a shift, a comparison, a
  conversion, a constant, a local load or store, or a branch**;
* there are **no exception handlers, no calls, no allocation, no strings and no
  object or array access**;
* the evaluation stack **depth agrees on every path** into every instruction.

That boundary is not arbitrary. Nothing in the subset touches a reference
count, so generated code needs no GC maps, no write barriers and no safepoints
where the collector could see a half-formed frame -- the three things that make
a code generator hard to get right and impossible to debug. `JitOptions::dumpCode`
prints the reason a method was refused.

## How the generated code is shaped

* **The interpreter's frame is the calling convention.** `Value* frame` arrives
  in a register and stays in a callee-saved one, so `LdLoc n` is a load at the
  constant offset `n * sizeof(Value) + payloadOffset`.
* **Tags are never written.** A slot's static type does not change over a
  method, so generated code writes only the payload half and leaves the tag the
  caller stored. That is what removes `Value` construction from the inner loop.
  `internal::ValueLayout` measures the offsets at startup rather than assuming
  them, so a layout change cannot silently produce wrong code.
* **The evaluation stack lives in the native frame at constant offsets.**
  `planBody` proves the depth at every instruction, so there is no stack
  pointer to maintain at run time.
* **Conversions are resolved at compile time.** The same analysis carries the
  type of every stack entry, so `(double)x` is one `scvtf` rather than a tag
  test and a branch.
* **Nothing throws.** Generated code writes an `ErrorCode` into
  `typed::Runtime::nativeError` and returns; the trampoline in `invoke` raises it
  as an ordinary `script::Exception`. Division by zero and the interrupt hook work
  from compiled code without the back ends emitting a byte of unwind data.
* **Loops stay interruptible.** Every back edge calls `scriptJitPoll`, which is the
  interpreter's tick under another name: it checks the interrupt flag and the
  deadline, and asks for a return if either fired.

## The ABI

```cpp
void entry(typed::Runtime* rt, const MethodInfo* method, Value* frame,
           std::uint32_t argc, Value* result);
```

* `frame` points at `method->frameSize()` constructed slots: the receiver (for
  an instance method) at slot 0, then the parameters in declaration order, then
  the locals, already zeroed to the defaults in `MethodBody::localTypes`. The
  caller owns the storage and destroys it.
* `result` is a default-constructed `Value` the callee overwrites; a `void`
  method leaves it alone.
* `argc` is what the caller pushed. The compiler already checked the arity, so
  this is an assertion, not a dispatch input.
* `rt` is the typed engine's runtime, which borrows the owning context's heap. Every runtime helper -- allocation, casts, string
  concatenation, virtual dispatch, element access -- is an ordinary function in
  `vm.hpp` taking that pointer, so a back end can call what the interpreter
  calls and only open-code the arithmetic.

Returning through an out-parameter rather than by value is deliberate: `Value`
is not trivially copyable, so the C++ ABI would return it indirectly anyway,
by rules that differ between platforms. Passing the pointer explicitly means
the generated code does not have to encode any of them.

On arm64 the arguments are `x0, x1, x2, w3, x4` and the generated prologue
parks `frame` in `x19`, `rt` in `x20`, the stack base in `x21` and `result` in
`x22`. On System V x86-64 they are `rdi, rsi, rdx, ecx, r8`, held in `rbx`,
`r12`, `r13` and `r14`.

Publishing is one release store to `MethodInfo::code`, which every call site
reads with an acquire load. Bodies are immutable once emitted, so compiling one
while the interpreter is running it is safe.

## The code cache

Mapping is page-granular but allocation is not: a compiled method is a few
hundred bytes, so they are packed into 64 KiB blocks -- six methods to a
mapping is typical. A block is flipped back to writable while it is being
added to and to read+execute afterwards (a context is single-threaded, so
nothing is executing out of it at that moment), the instruction cache is
flushed on arm64, and the block is unmapped when the last method in it is
released. `codeCacheLimit` caps the total; past it, methods keep interpreting.

## Ahead-of-time

`JitEngine::compileAll` compiles everything in an assembly the back end
supports, which is what `CompileOptions::native` and `Mode::Eager` use.
Combined with [assembly images](compilation.md), that is a deployment where
the device never sees C# source and starts warm: compile to an image on the
build machine, ship the image, load it with `native = true` on startup.

## Widening it

The supported set is `isSupportedOp` in `src/typed/jit/jit.cpp`; the back ends are
switch statements over it. To add an instruction:

1. add it to `isSupportedOp`, and to `pushedType` if it produces a value;
2. add a case to `jit_arm64.cpp` and to `jit_x64.cpp`;
3. add it to `tests/csharp/test_jit.cpp`.

Step 3 is the point. Every test there runs the same program interpreted and
compiled and insists on the same answer over a spread of edge-case operands.
The interpreter is the oracle, which is why it stays even for what the back
ends can do.

Worth taking next, in order of what it buys:

* **field access** (`LdFld`, `StFld`) for primitive fields -- no refcount
  traffic, and it makes whole classes of method compilable;
* **array elements** (`LdElem`, `StElem`, `LdLen`) with the bounds check
  open-coded and the failure going through `nativeError`;
* **calls**, by calling `invoke()` rather than inventing a fast path, so the
  two tiers stay interchangeable;
* **reference-typed locals**, the first step that needs generated code to
  maintain reference counts -- worth doing through the helpers in `vm.hpp`
  rather than open-coded;
* **a register allocator**, which is where the next factor of two or three is,
  and which the constant-offset stack layout was chosen to make possible
  without rewriting the instruction selection.

## Building it

`SCRIPT_ENABLE_JIT` is on by default and needs `SCRIPT_LANG_CSHARP`; a build
without C# has no typed engine and turns it off. `-DSCRIPT_ENABLE_JIT=OFF`
leaves only the interpreter in the binary, which is what a platform without
executable pages wants; `jitBackend()` then reports `"interpreter"` and every
request is a no-op. The x86-64 back end targets the System V convention only -- on Windows
it reports itself unavailable rather than emitting something plausible and
wrong.
