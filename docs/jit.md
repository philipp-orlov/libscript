# Native code generation

The typed engine -- the machine C#, C and C++ run on -- compiles to machine code by default once a method gets hot. On arm64
that is an optimizing compiler; x86-64 still has the older template generator (`src/typed/jit/jit_x64.cpp`), which covers
arithmetic over primitive locals and leaves the rest to the interpreter. The dynamic engine has neither yet.

## Asking for it

```cpp
script::Context::Options options;

// Ahead of time: compile everything the back end handles, at load.
options.compile.native = true;

// Or on demand: interpret until a method has earned it.
options.jit.mode = script::JitOptions::Mode::Tiered;  // or Eager, or Off
options.jit.threshold = 200;                      // calls or loop iterations before compiling
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

C# kernels against JavaScriptCore 2.52.6 running the same programs in JavaScript (arm64, Cortex-X925, median wall-clock
seconds of a whole process, so warm-up included; lower is better). `bench/kernels` has them.

| kernel | interpreted before | native, default build | JSC default | .NET 10 |
| --- | ---: | ---: | ---: | ---: |
| `int_loop` | 3.663 | 0.030 | 0.056 | 0.046 |
| `float_mandel` | 0.659 | 0.011 | 0.015 | 0.029 |
| `fib_calls` | 1.784 | 0.026 | 0.057 | 0.051 |
| `prop_rw` | 1.505 | 0.009 | 0.021 | 0.052 |
| `method_call` | 1.026 | 0.007 | 0.062 | 0.024 |
| `closure` | 2.034 | 0.020 | 0.028 | 0.038 |
| `nbody` | 3.553 | 0.023 | 0.040 | 0.042 |
| `sieve_array` | 2.407 | 0.030 | 0.063 | 0.053 |
| `alloc_small` | 1.027 | 0.108 | 0.098 | 0.054 |
| `tree_gc` | 1.809 | 0.132 | 0.108 | 0.123 |
| `string_ops` | 0.621 | 0.023 | 0.052 | 0.034 |
| `map_dict` | 0.983 | 0.235 | 0.412 | 0.190 |

The geometric mean is 0.54 of JSC's time. A build with `SCRIPT_PROFILE=Performance` and profile data brings `alloc_small`
under JSC's time as well; `tree_gc` stays about 1.2x behind in every build.

## The passes

A method goes through three, each in a file of its own under `src/typed/jit/`, plus the thin part for each machine:

* **`lower.cpp`, bytecode to IR.** The evaluation stack runs at compile time, so an operand is a register, a constant or a
  local being read. Compare-and-branch fuses, constants fold and methods of up to 96 bytes of bytecode inline, including
  constructors, which then skip what a new object does not need.
* **`optimize.cpp`, on the IR.** Common subexpressions within a block; in a call-free innermost loop, a field of an object
  the loop does not reassign stays in a register and is written back on every way out; loops are rotated so that an
  iteration takes one branch; copies disappear.
* **`regalloc.cpp`.** Linear scan. Each value has one location for its whole life, and what goes to memory when registers
  run out is what is touched least per instruction, weighted by loop depth.
* **`jit_arm64.cpp`, the machine.** IR in, words out. `jit_stubs_arm64.cpp` is the assembly at the edge of native code.

A program's methods are compiled together with everything they call directly, so those calls are plain branches.

## What it compiles

Everything in the language, in principle. `lowerMethod` refuses a method with a reason, and it keeps interpreting:

* a value that is null on one way into a join and an `int` on another, which `a?.Length` makes;
* a method of `object` itself, since what it dispatches to takes `this` in no agreed way;
* a lambda with more than four captures, a concatenation of more than three parts;
* a protected region entered with values on the evaluation stack;
* methods whose frame would exceed 4000 bytes.

`--dump-native` prints what was refused and why, along with the code that was made.

## Reference counts in native code

Generated code keeps counts itself, and most of the work is not doing so. A callee borrows its arguments, so a parameter it
never assigns costs nothing. A read of a field holds no count until something could overwrite that field; a call to a method
that stores no reference anywhere (`storesNoReferences`) does not count as that. An argument that was owned just for the
call moves into the field a constructor stores it in. The cycle collector needs nothing from native frames: a count held in a
register looks like any other reference from outside the heap, so there are no stack maps.

## Exceptions

Generated code never unwinds. Raising records the error in `Runtime::nativeError` and the method returns, and each caller
tests that field after the call. Under a `try`, the same exit goes to a landing block that asks the runtime whether a handler
may take it, instead of returning. `finally` is entered with a continuation number, negative when an exception is passing
through. The engine's own limits -- an interrupt, a stack overflow, the memory limit -- are not catchable, as in the
interpreter.

## The native calling convention

Only generated code and the stubs use it:

* integer and reference arguments in `x0`-`x7`, floats in `d0`-`d7`, the rest in stack words, in order with `this` first; a
  result in `x0` or `d0`;
* a value of static type `object` or an interface is two integer words, its tag then its payload, and comes back in `x0`
  (payload) and `x1` (tag);
* `x20` holds the runtime; `x14` the delegate at a call through one; `x15` the method at a call through its entry;
* `x16`, `x17`, `d30` and `d31` are scratch;
* `x29` chains the frames, which is how a stack trace finds them.

Every `MethodInfo` starts with `fastEntry` at one stub, which compiles the method or marshals the call into `invoke`, and C++
gets in through `scriptJitEnter`. Natives go through the same stub unless they are intrinsics: `Math.Sqrt` and friends are
instructions, and `List`, `Dictionary` and `StringBuilder` have direct helpers.

## Tiering

`Mode::Tiered` is the default: a method compiles after `threshold` calls or back edges (200). A hot loop in a method called
once enters native code at the loop head, with its locals read out of the interpreter's frame. `Mode::Off` and `--interpret`
ask for none.

## Testing it

The interpreter is the oracle. `tests/csharp/test_jit.cpp` runs programs both ways. `tools/jit_fuzz.py` makes random C#
programs -- arithmetic, objects, virtual calls, closures, collections, exceptions -- and insists they print the same
interpreted, compiled up front and tiered; `tools/jit_reduce.py` shrinks a disagreement. A build with
`-DSCRIPT_JIT_ENV_OVERRIDE=ON` lets `SCRIPT_JIT=eager` or `SCRIPT_JIT=tiered:N` run the entire test suite against native code.

Two things differ from the interpreter on purpose: the depth limit is a budget of stack rather than a count, because native
frames are far smaller; and a leaf method with no frame of its own has none in a stack trace.

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

## Building it

`SCRIPT_PROFILE` picks how big a build is. Sizes are the stripped command line tool with every language in.

| profile | what is in it | size |
| --- | --- | ---: |
| `Compact` | interpreters only, `-Os`, link-time optimization, no executable pages | 1.5 MB |
| `Standard` | adds the native code generators; the default | 3.0 MB |
| `Performance` | adds link-time optimization, and profile data if given (`-DSCRIPT_PGO=generate`, `tools/pgo-train.sh`, `-DSCRIPT_PGO=use`) | 1.9 MB |

`SCRIPT_ENABLE_JIT`, `SCRIPT_ENABLE_LTO` and `SCRIPT_PGO` can each be set on their own. Without a code generator, `jitBackend()`
reports `"interpreter"` and every request is a no-op: that is what a platform without executable pages wants.
