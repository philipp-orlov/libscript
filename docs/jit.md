# Native code generation

The typed engine -- the machine C#, C and C++ run on -- compiles to machine code by default once a method gets hot, on arm64
and on x86-64 with the System V convention. The tables below were measured on arm64. The x86-64 back end has only been run
under qemu here, where it passes the same tests and the same fuzzing; its speed has not been measured.

## Asking for it

```cpp
script::Context::Options options;

// Ahead of time: compile everything the back end handles, at load.
options.compile.native = true;

// Or on demand: interpret until a method has earned it.
options.jit.mode = script::JitOptions::Mode::Tiered;  // or Eager, or Off
options.jit.threshold = 200;                      // calls or loop iterations before compiling
options.jit.codeCacheLimit = 32u << 20;  // the default
options.jit.dumpCode = false;
options.jit.background = true;   // JavaScript, TypeScript, Python: big compiles on a thread of their own

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
| `int_loop` | 3.663 | 0.031 | 0.058 | 0.046 |
| `float_mandel` | 0.659 | 0.012 | 0.016 | 0.029 |
| `fib_calls` | 1.784 | 0.033 | 0.057 | 0.051 |
| `prop_rw` | 1.505 | 0.010 | 0.021 | 0.052 |
| `method_call` | 1.026 | 0.008 | 0.063 | 0.024 |
| `closure` | 2.034 | 0.021 | 0.030 | 0.038 |
| `nbody` | 3.553 | 0.024 | 0.042 | 0.042 |
| `sieve_array` | 2.407 | 0.032 | 0.064 | 0.053 |
| `alloc_small` | 1.027 | 0.100 | 0.105 | 0.054 |
| `tree_gc` | 1.809 | 0.116 | 0.114 | 0.123 |
| `string_ops` | 0.621 | 0.022 | 0.055 | 0.034 |
| `map_dict` | 0.983 | 0.210 | 0.392 | 0.190 |

The geometric mean is 0.54 of JSC's time. `alloc_small` and `tree_gc` are level with JSC, inside its own run-to-run spread. What
is left there is memory: a typed instance carries a 48-byte header and 16 bytes a field, against JSC's 16 and 8, so a tree of
them touches more cache lines. Small instances are made in native code off the allocator's free list.

## The passes

A method goes through three, each in a file of its own under `src/typed/jit/`, plus the thin part for each machine:

* **`lower.cpp`, bytecode to IR.** The evaluation stack runs at compile time, so an operand is a register, a constant or a
  local being read. Compare-and-branch fuses, constants fold and methods of up to 96 bytes of bytecode inline, including
  constructors, which then skip what a new object does not need.
* **`optimize.cpp`, on the IR.** Common subexpressions within a block, and past a label with one way in; in an innermost
  loop with no call that comes back, a field of an object the loop does not reassign stays in a register, written back on
  every way out, a bail included, and around a release in cold code; loops are rotated so that an iteration takes one
  branch; copies disappear; a number goes through memory as a double, with no move between register files.
* **`regalloc.cpp`.** Each value has one location for its whole life, and what goes to memory when registers run out is
  what is touched least per instruction, weighted by loop depth. A value prefers the register of one it is copied from or
  to, which makes the copy nothing.
* **`jit_arm64.cpp` and `jit_x64.cpp`, the machine.** IR in, bytes out. `jit_stubs_arm64.cpp` and `jit_stubs_x64.cpp` are the
  assembly at the edge of native code.

A program's methods are compiled together with everything they call directly, so those calls are plain branches.

## What it compiles

Everything in the language, in principle. `lowerMethod` refuses a method with a reason, and it keeps interpreting:

* a value that is null on one way into a join and an `int` on another, which `a?.Length` makes;
* a method of `object` itself, since what it dispatches to takes `this` in no agreed way;
* a lambda with more than four captures, a concatenation of more than three parts;
* a protected region entered with values on the evaluation stack;
* methods whose frame would exceed 4000 bytes.

`--dump-native` prints what was refused and why, along with the code that was made.

## JavaScript, TypeScript and Python

The dynamic engine has its own lowering (`src/dyn/jit_lower.cpp`) onto the same IR, passes and back ends. It compiles
what the interpreter has seen, and when that turns out wrong it hands the call back:

* **Bailing out.** A guard that fails sends the call to the interpreter at the same instruction, with every local, cell
  and stack entry as it was. A bail is a table, not code: one routine per function saves the registers, and `scriptDynBail`
  rebuilds the frame from a `DeoptInfo` that says where each value lives. Code that bails more than a few times is thrown
  away and compiled again from what the interpreter has learnt since; the instructions that bailed get no guess the next
  time. A function under 1 KB of bytecode gets 12 such rounds, a larger one 3, and then it assumes nothing. Entering a loop
  part way through checks what the code assumes of `this`, parameters and number locals. A property site the interpreter
  has never reached is a guess too: reaching it bails.
* **Property access** uses the interpreter's sites. A site that has seen one shape checks it once, and the shape is then
  known for that object until a join or anything that can run code; a site that has seen a few shapes dispatches on them,
  stores included, whether they overwrite the property or add it. A shape may be followed by any number of names that are
  spelled in the source, but only 32 worked out at run time, so an object used as a dictionary drops its shape.
  A property up the prototype chain is checked against `protoEpoch`, a getter is called directly, and a store that adds
  a property appends in place. A field that has only held numbers loads straight into a float register, and a number
  stored over a number writes only its bits. Methods read off a primitive string are read from `String.prototype` in place.
* **Objects** keep their first properties in their own cell, so a literal or a `new` object is one allocation. The header
  is 80 bytes and a property 16, being only its value: its name and attributes are the shape's, or for the few objects with
  no shape, in a table behind the `rare` pointer, with the other things few objects need. New plain objects are written in
  native code, popped off the allocator's free list or carved from its current block, while no collection is due and no
  memory limit is set: those of `{...}`, of a constructor compiled in place, and of `new F` for an `F` known when compiled.
  The boxes captured variables live in are made the same way, and an array literal's elements are stored in place.
* **Arrays** hold bare doubles at 8 bytes each while every element is a number or `undefined`, which is a reserved NaN
  (`kUndefinedElement`), so `new Array(n)` starts out that way. The first element that needs a `Value` turns the array into one
  of values at 16 bytes for good. A few elements live in the array's own cell. The interpreter records what each access
  site has met (`FunctionCode::elemSaw`), and a site that has met only numbers loads them straight into a float register. Code
  that cannot be bothered with the difference calls `DynArray::values()`, which makes the array one of values.
* **Loops.** A loop entered from above comes in through a pre-header, where the interpreter also hands over, which checks the
  shapes and arrays of locals the loop does not assign. If nothing in the loop can change them -- no call that returns, no
  cold block that rejoins through an ordinary label, no store at those offsets -- what was checked holds all the way round and is
  not asked again (`findQuietLoops`). An interrupt handler runs in the middle of such a loop, which is why it may only look.
* **Numbers.** Arithmetic and comparisons on operands of unknown type check that they are numbers and run the code for
  numbers. A parameter only ever given numbers is a number local, assigned or not; a wrong guess is found on entry
  (`scriptDynEntryMiss`), before anything has happened. Number locals are int32s in a general register while that holds. `+`,
  `-` and small `*` of integers go unchecked in 64 bits while the answer is exact in a double (`Rep::Exact`), so `(a + b) | 0`
  has no overflow check at all; whatever else uses the value settles it to an int32 or a double. Where the machine has it,
  ToInt32 is `fjcvtzs`. A global holding a number that the function does not assign is a constant behind a check.
* **Calls** go straight to native code. A function read from a global or a property is *pinned*: checked to be the one it was
  when compiled, kept alive by the code (`FunctionCode::jitKeep`), and so called with no count and nothing else checked.
  A small, loop-free callee is inlined: one known that way, two levels of itself if it is recursive, or at a call through a
  local, whatever `noteCall` saw the site call, by the code it has, closures included. A constructor goes in place of `new`
  with its object's shape known from the start, and a small method that only reads takes no counts on its receiver.
  `f.apply(x, arguments)`, where that is all `arguments` is for, passes the function's own arguments with no array.
  Built-ins that call back into script enter compiled callbacks directly (`CallbackLoop`).
* **Globals** are read and written where they are while `globalEpoch` says the global object's table has not moved.
* **Built-ins done in place**, when the callee is still the built-in it was: `Math.sqrt`, `abs`, `floor`, `ceil`, `trunc`
  (an instruction, with nothing held and no call left to get in the way of a loop); `Math.max`, `min` (a compare, ties and
  NaN the long way), `pow`, `atan2`, `round`, `sin`, `cos`, `tan`, `atan`, `exp`, `log` and `parseInt` of a number (a direct
  call on bare doubles); `push` (growing the array or changing its form without calling the built-in), `pop`, `charCodeAt`,
  `String.fromCharCode`, `f.apply`, `f.call` (a call of `f`, inlined like any other), `Map` and `Set` `get`, `set`, `has` and
  `add` (the table's own lookup), `instanceof` of a plain function, and `length`. `s += x` on a
  local nothing else holds grows the string in its own cell.

**What it keeps alive.** Holding a count on the names and prototypes every object shares is a serial chain of memory
operations, so some are *pinned*, uncounted, until the context ends: the name of any property in a shape (shapes are bounded by
`kMaxShapes`), a prototype after 1024 objects or once compiled code makes objects of it with `new`, the built-in prototypes,
and the functions and prototypes compiled code expects. A pinned cell's count says nothing, and reaching zero does not free it.
Any other prototype that is freed, or emptied by the cycle collector, moves `protoEpoch` on, so nothing that learnt where a
property was on it is used again.
A program that makes many large short-lived classes keeps what those reach for longer than it strictly needs.

The allocator is by live range with holes, most-used value first, uses in cold code counting for next to nothing, and spill
slots shared; the optimizer works through cold blocks, keeps object headers and values apart, forwards stores to loads, folds
branches on known constants, moves constants out of call-free loops and removes dead values.

It refuses a function with exception handlers or a `finally`, which keeps running in the interpreter. `--dump-native` lists what
it skipped.

Measured the same way as the table above, against JavaScriptCore running identical programs (`bench/kernels/js`):

| kernel | interpreted | native, default build | JSC | JSC, LLInt only |
| --- | ---: | ---: | ---: | ---: |
| `int_loop` | 3.550 | 0.034 | 0.058 | 1.043 |
| `float_mandel` | 0.479 | 0.013 | 0.016 | 0.100 |
| `fib_calls` | 2.147 | 0.058 | 0.057 | 0.633 |
| `prop_rw` | 1.394 | 0.029 | 0.021 | 0.259 |
| `method_call` | 1.217 | 0.058 | 0.063 | 1.072 |
| `poly_prop` | 2.114 | 0.031 | 0.026 | 0.767 |
| `closure` | 1.609 | 0.043 | 0.030 | 0.540 |
| `nbody` | 4.140 | 0.054 | 0.042 | 0.820 |
| `sieve_array` | 8.778 | 0.072 | 0.064 | 0.298 |
| `array_push_hof` | 0.241 | 0.039 | 0.025 | 0.159 |
| `sort_cmp` | 0.585 | 0.093 | 0.078 | 0.178 |
| `alloc_small` | 1.154 | 0.129 | 0.105 | 0.398 |
| `tree_gc` | 2.816 | 0.178 | 0.114 | 0.849 |
| `string_ops` | 0.965 | 0.096 | 0.055 | 0.345 |
| `json_rt` | 0.233 | 0.120 | 0.105 | 0.105 |
| `map_dict` | 0.968 | 0.570 | 0.392 | 0.553 |

That is 1.19x JSC's time overall and 0.17x the time of JSC with its JIT off. `int_loop`, `float_mandel` and `method_call` are
ahead of JSC and `fib_calls` level with it; its top tier is still ahead on the rest. On Octane the gap is 6.1x (it was 125x before
any native code), with `earley-boyer` and `gbemu` furthest behind, at 12.4x and 11.7x. Memory goes the other way: JSC uses 6x as
much on these kernels and 8.5x on Octane, which `tests/javascript/test_gc.cpp` and `harness/mem_gate.py` are there to keep so.

`tools/js_jit_fuzz.py` is the dynamic engine's fuzzer, run against a build with `SCRIPT_JIT_ENV_OVERRIDE` at several
thresholds.

### Typed arrays

An element read or write at a site that has only met one kind of typed array is a load or store of that kind's width,
after checking the class, the kind and the current length -- read each time, since a buffer can be detached or resized
under the loop. `Float16Array`, `Uint8ClampedArray` and the BigInt kinds, and a site that has met several kinds, call a
helper that reads the index as a number without making a property name of it.

### Large functions

A function of tens of thousands of instructions, as Emscripten writes them, needs:

- **Frames:** on arm64 a frame may be up to 32 KB.
- **Branches:** a function too big for the ±1 MB conditional branches is emitted again with each conditional branch to a
  label turned into its inverse over an unconditional one.
- **Exit pads:** identical exit pads, those of helpers that raised, are shared.
- **Bail records:** a bail, and a loop entry, carries only the locals the interpreter can still read.
- **One retry for wrong guesses:** a wrong guess about what a local holds is noted while lowering carries on, so one more
  lowering puts right every slot it found.
- **Allocation:** the register allocator keeps each register's occupied ranges in a tree, hands out spill slots by a
  linear scan, and works out liveness with a worklist over only the values that cross blocks.
- **Guessing rounds:** a function may be recompiled with fewer guesses up to twelve times, whatever its size. Without
  any guesses a large function compiles to many times the code.

### Array callbacks

`forEach`, `map`, `filter` and `reduce` with an initial value are emitted as a loop where the built-in is known, and call the
callback as native code calls any function. Guards ahead of the loop fall back to calling the built-in. Nothing inside
bails, because the interpreter cannot take over part way through a built-in: whatever is out of the ordinary goes through a
helper and comes back. The array is read afresh each time round, as the callback may change it.

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

Only generated code and the stubs use it. On arm64 (`jit_x64.cpp` opens with what it does instead):

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

`Mode::Tiered` is the default: a method compiles after `threshold` calls or back edges (200). A JavaScript, TypeScript or
Python function waits one more threshold for each 8 KB of its bytecode, up to eight, since its compile costs that much more. A hot loop in a method called
once enters native code at the loop head, with its locals read out of the interpreter's frame. For that, the loops of the
call also have to have run 64 bytes of loop body for each byte of the function: a short loop in a long function, such as an
initializer, would not pay back a compile of the whole of it. `Mode::Off` and `--interpret` ask for none.

A function whose IR is 3000 instructions or more is lowered on the calling thread, which is where the profile and the heap
are, and handed to the context's one worker thread for the rest: loop optimization, register allocation and the machine
code, which read nothing else. The interpreter goes on with the function meanwhile, and the next call or every 256th back
edge looks whether the code is ready, and puts it in place. Native code checks what it assumes as it runs, so code that
comes a little later than the profile it was made from is as safe as code that has been running a while. Mandreel, whose
largest functions take 50 to 300 ms each to compile, scores 3.5x what it did with every compile done in line.

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
