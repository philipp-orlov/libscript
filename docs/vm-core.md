# The vm core

A second execution core for JavaScript, built beside the dynamic engine (`src/dyn`), which stays the default and the
oracle until the new one runs every JavaScript suite. The design is in `ls-jsc.md` (outside this repository); this page
says what is here.

## Using it

```sh
cmake -S . -B build/vm-dev -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DSCRIPT_JS_CORE=both -DSCRIPT_JIT_ENV_OVERRIDE=ON
SCRIPT_CORE=vm build/vm-dev/tools/libscript program.js      # run on the new core
SCRIPT_CORE=vm SCRIPT_VM_DUMP=1 build/vm-dev/tools/libscript program.js   # print the bytecode first
```

`SCRIPT_JS_CORE` is `dyn` (default), `vm` or `both`. `vm` builds the new core but keeps the old one as the default context
until the switch-over (`SCRIPT_VM_DEFAULT`, off). In code, `Context::Options::core = Context::Options::Core::Vm` picks it per
context; builds with `SCRIPT_JIT_ENV_OVERRIDE` also read `SCRIPT_CORE=dyn|vm`. `-DSCRIPT_VM_LANGS="javascript;typescript;python"`
(a cache variable: reconfigure to change it) says which languages the new core runs: TypeScript goes through the shared
parser, checker and type eraser and then the vm compiler (`src/vm/language.cpp`); Python runs the shared prelude on the vm
core's natives (`src/vm/python.cpp`). `Context::core()` says which core a context has.

## Layout

| Path | What |
|---|---|
| `include/script/internal/vm/value.hpp` | `JSValue`, eight bytes, NaN-boxed; `Handle` |
| `cell.hpp`, `shape.hpp`, `object.hpp` | cell header, shapes (the prototype is in the shape), objects, arrays, functions, boxes |
| `bytecode.hpp`, `opcodes.def` | register bytecode, code blocks, inline-cache records |
| `vm.hpp` | the runtime: heap, shape table, call stack, validity cells, errors |
| `operators.hpp`, `convert.hpp`, `host.hpp` | the language's operators; `Value` conversion; what the public `Value` API does with a new-core object |
| `src/vm/compiler.cpp` | AST to bytecode |
| `src/vm/interp.cpp` | the interpreter: computed-goto dispatch, calls without C++ recursion, inline caches |
| `src/vm/jit/`, `include/script/internal/vm/jit.hpp` | the baseline compiler (x86-64, Linux): `asm_x64.hpp` (macro assembler), `code_region.cpp` (dual-mapped code memory), `baseline_x64.cpp` (templates, calls, entry stubs) |
| `src/vm/builtins.cpp`, `builtins_*.cpp` | the built-in objects: Object/Function/Array/String/Number/Math/Error/JSON (`builtins.cpp`), Symbol, iterator families and generator prototypes (`_symbol`), the rest of Object, Reflect, Array/String/Number/Math and BigInt (`_more`), the Array methods that call back (`_array`), Map/Set/Weak\*/WeakRef/FinalizationRegistry (`_collections`), RegExp (`_regexp`: the spec's `exec`/species paths, a fast path for an untouched RegExp, the legacy `RegExp.$1` statics, `RegExp.escape`), Promise with capabilities and the combinators, async functions and generators (`_promise`), Date (`_date`), ArrayBuffer (resizable, shared), typed arrays and DataView (`_typed`, `_typedarray`), Atomics, iterator helpers, Proxy (`_proxy`: callable proxies) |
| `src/vm/objmodel.cpp` | the internal methods of an object -- [[GetOwnProperty]], [[DefineOwnProperty]], [[Set]], [[GetPrototypeOf]], [[OwnPropertyKeys]] and the rest -- for ordinary objects and proxies, with every invariant check a proxy's traps are held to; `Object` and `Reflect` are built on them |
| `src/vm/python.cpp`, `src/vm/language.cpp` | Python and TypeScript on the new core |
| `tests/vm`, `tests/vm/snippets.inc` | unit tests; programs both cores must agree on |
| `tools/vm_fuzz.py`, `bench/micro` | differential fuzzer; micro-benchmarks that also run under JSC and node |

## The baseline compiler

A function that has been called `jit.threshold` times (200 by default; 1 when `jit.mode` is `Eager`; `SCRIPT_VM_TIER1=n`
overrides it in builds with `SCRIPT_JIT_ENV_OVERRIDE`), or whose loop has gone round about a thousand times, is translated one
bytecode instruction at a time into machine code on the same frame the interpreter uses, so the two can hand a frame to one
another at any instruction: the interpreter enters compiled code at a loop's back edge (`LOOP_TICK`), compiled code runs
any instruction it has no template for in the interpreter (`stepInstruction`: one instruction of the top frame, which
includes what it calls), and an exception handler is entered by looking its label up (`scriptVmJitTarget`).

- Templates: moves and constants, int32 and double arithmetic and comparisons with the interpreter's fast paths (anything
  else, and every overflow, goes to an out-of-line stub that calls the same code the interpreter runs), jumps with the
  interrupt tick on back edges, strict equality, global reads from a fixed slot (guarded by `VM::globalEpoch`), property
  reads and writes and `InitProp` against the inline cache as it was at compile time (prototype hits hold the validity cell
  with a count of their own), `length`, array and typed-array elements, boxes and up-values, `Ret` (including constructors),
  and calls: a compiled script callee gets its frame pushed in line and is called directly; natives and the rest go through
  `scriptVmJitCall`; one-argument `Math` natives are called without any of that.
- Helpers (`jitGetProp` and the rest, in `interp.cpp`) are what the interpreter does for one instruction without the
  dispatch; stubs and templates without a fast path use them.
- Not compiled: generators and async functions (they stay in the interpreter), functions of more than 20,000 instructions.
- A C++ exception cannot cross compiled frames (they carry no unwind information), so it is parked in a thread-local and the
  compiled frames leave as for a script exception; `enterTier1` throws it again.
- Code lives in one dual-mapped region (`memfd`, a read-write view for the compiler and a read-execute view for running);
  where that is not available the function stays interpreted. `SCRIPT_VM_STEPS=1` counts what compiled code still runs in the
  interpreter, `=2` adds the sites.
- Tests: `everySnippetAgreesInCompiledCode` runs every snippet and `snippets_jit.inc` Eager and Tiered; `FUZZ_TIER1=1
  tools/vm_fuzz.py` fuzzes compiled code; the suites run with `SCRIPT_VM_TIER1=1`.

## Rules the code keeps

- Counts, not a tracing collector. A cell of the new family has a 16-byte header with the count at offset 8; byte 7 has
  its high bit set, which is how `detail::destroyUnreferenced` and the public API tell the families apart. Every register of
  a live frame owns its value.
- A cell is at most 8 KiB (larger storage is out of line), so `LocalHeap::ownerOf` finds the VM from any cell.
- Cycles are found by a collector that runs when the context's heap budget is passed, and also when the cells alive pass
  a budget of their own (`VM::cellBudget_`: 8192 to begin with, then a little more than what survived), because the heap
  budget does not see the buffers of arrays and strings. A minor collection looks at the young cells; a whole one runs when
  the cells have doubled since the last. The collector's own vectors come from the payload pool, so a steady-state workload
  makes no call to the general allocator (`core.steady_state`).
- A proxy is an object in dictionary mode whose class is `Proxy` (or a function, for a callable target): no inline cache
  can match it, so every read and write of one reaches its traps. The caches also refuse a prototype chain with a proxy in it.
- Cells are below 2^47. On a machine whose allocator may return higher addresses this aborts with a message; the address
  hint (LC-1) is still to do.
- Property instructions carry their inline cache in the word after them: own property, prototype property (with a validity
  cell that any change to the chain clears), add-a-property (shape before and after), global.

## What runs

Everything the JavaScript, TypeScript and Python suites ask of it (`ctest` with `SCRIPT_CORE=vm`: all 53 pass): classes
(fields, private names, static blocks, accessors, `super`), generators, async functions and generators (including `yield*` in
both), `for await`, destructuring, spread and rest, tagged templates, regular expressions (named groups, lookbehind, `d`, `v`),
BigInt, Symbol, Map/Set/WeakMap/WeakSet/WeakRef/FinalizationRegistry, Promise (the host drains the queue with
`Context::runJobs`), Proxy and Reflect, typed arrays (resizable buffers, `Float16Array`), DataView, Atomics, SharedArrayBuffer,
Date, JSON, iterator helpers, `eval` and `new Function` when the host allows run-time code, Annex B (function-in-block
hoisting, `__proto__`, `__defineGetter__` and kin, the legacy RegExp statics).

Direct `eval`: a sloppy function that has one gets a hidden object (`%evalenv#N`, made at entry, held in a box so closures
and nested `eval` reach it); the `var`s and functions its `eval` code declares go there, and a name that nothing declares is
looked up on those objects (innermost first) before the global object. Global code's `eval` declares on the global object
with configurable properties; global code's own `var`s and functions are not configurable. A name that is declared somewhere
(a local, an outer function's variable) is always that variable: an `eval` `var` that shadows an outer function's variable
from inside a nested function is not seen by name lookups that were resolved statically.

The host API (`Value`, `Context`, `Object`, `call`, `construct`, wrappers, `declare`, the cycle collector, `Interrupted`,
`StackOverflow`, memory limits) works on it; `javascript.*`, `typescript.*`, `python.*`, `core.*` and `vm.*` pass.

Generators keep their registers in a copy and resume through a nested `run`; an async function is a generator that a promise
drives; a generator's `return(v)` throws a marker that `finally` blocks see and `catch` blocks do not.

## test262

`tools/run_test262.py` with `SCRIPT_CORE=vm`: 38,718 of 48,795 pass on x86-64 (the old core: 36,984; 91 tests the old core passes fail here, 1,825 the other way). What fails on both cores is mostly `$262.createRealm` and `$262.agent` (cross-realm and threads), Intl-adjacent
Unicode property tables and a few features the shared front end does not parse. What the old core passes and this one does
not is a long tail of single tests: see the last snapshot's `cmp262.py` output.

## Known limits

- Tier 1 (the baseline compiler) is the default; tier 2 (the first optimizing form) is off unless `SCRIPT_VM_TIER2=<calls>` (a threshold; 0 is off) is set in a
  build with `SCRIPT_JIT_ENV_OVERRIDE`. aarch64 has no compiler yet and interprets (no aarch64 hardware was available).
- Not compiled by tier 1: generators and async functions (they stay in the interpreter), functions of more than 20,000
  instructions.
- Arrays are dense: a length past what a gap of `JSArray::kMaxDenseGap` allows (`a.length = 2**32 - 1`) ends in
  `OutOfMemory` where the language would give a sparse array.
- Weak collections, `WeakRef` and `FinalizationRegistry` hold their targets for as long as the program can name the
  collection; the registry's callback never runs.
- A parameter list with a default has no separate environment for the body's `var`s (a closure in a default sees a body
  `var` of the same name).
- Direct `eval` `var`s do not reach a name that was resolved to an outer function's variable (see above); `eval` code does not
  check its `var`s against the lexical bindings between it and the variable scope.
- `$262.createRealm` and `$262.agent` do not exist: the vm core has one realm per context.
