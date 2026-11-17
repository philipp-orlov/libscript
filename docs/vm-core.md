# The vm core

The execution core for JavaScript, TypeScript and Python (`src/vm`). It is the default: a `Context` runs on it unless told to
use the dynamic engine (`src/dyn`), which stays in the tree as the oracle (`Options::core = Dyn`, and `SCRIPT_CORE=dyn` in builds
with `SCRIPT_JIT_ENV_OVERRIDE`). The design is in `ls-jsc.md` (outside this repository); this page says what is here.

## Using it

```sh
cmake -S . -B build/vm-dev -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DSCRIPT_JS_CORE=both -DSCRIPT_JIT_ENV_OVERRIDE=ON
SCRIPT_CORE=vm build/vm-dev/tools/libscript program.js      # run on the new core
SCRIPT_CORE=vm SCRIPT_VM_DUMP=1 build/vm-dev/tools/libscript program.js   # print the bytecode first
```

`SCRIPT_JS_CORE` is `vm` (default: builds the new core and makes it the default), `both` (the same; the name says that the old core
is built too, which it is in every setting but `dyn`) or `dyn` (the old core only). In code, `Context::Options::core` picks a core
per context; `Auto` is the new core when it is built (`SCRIPT_VM_DEFAULT`).
`-DSCRIPT_VM_LANGS="javascript;typescript;python"`
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
| `src/vm/jit/`, `include/script/internal/vm/jit.hpp` | the compilers (x86-64 and arm64, Linux): `asm_x64.hpp` and `asm_a64.hpp` (two macro assemblers with one interface), `code_region.cpp` (dual-mapped code memory), `baseline_x64.cpp` (templates, calls, entry stubs, written once for both) |
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

## arm64

Both compilers are written against the interface of `asm_x64.hpp`. On arm64 the same templates are assembled by
`asm_a64.hpp`, which has that interface operation for operation:

- A register name of the interface stands for a fixed arm64 register (the table is at the top of the file). The
  argument registers of the two calling conventions line up, so a call into the runtime needs no moves before it;
  `callC` moves the result into place after it, and `jmpC` is the form for a stub that ends in the runtime's function.
- An operation with a memory operand is a load, the operation and a store. Addresses the instruction cannot encode go
  through a scratch register.
- A conditional jump is encoded from the condition and from what set the flags last (a subtraction, an addition, a
  logical operation, a multiplication, a floating-point comparison): the carry after a subtraction is the other way
  round, and an unordered comparison reads as equal on x86-64. Operations that set flags there and not here (`or`, `xor`,
  shifts) are followed by a test only when the next thing emitted reads the flags. A jump on flags that nothing left
  fails the compile, and the function stays where it was.
- The templates push single 8-byte values, which arm64's stack pointer does not allow. The stack they see is `x28`;
  `sp` moves with it, so nothing below it is live when a signal arrives, and is never the base of an access. A call
  keeps the caller's return address in the slot where x86-64 has the callee's, so the layout and the alignment at each
  call are the ones the templates were written for.
- `cvttsd2si` gives the smallest integer for a NaN or a value out of range, as x86-64 does: guards compare with it.
- A conditional branch reaches one megabyte: a function whose code is larger is compiled again with each such branch
  as a branch over a jump.
- `commitCode` copies code into the region and tells the instruction cache.
- A register move is taken back when the operation after it reads and overwrites the move's destination: the operation
  is emitted on the move's source (the interface's two operands become three). The assembler also knows which
  registers are zero-extended, so boxing an int32 is one instruction.
- arm64 has registers the interface does not: seven integer ones (`r16`-`r22`) and seven for doubles (`xmm16`-`22`).
  The optimizing compiler's int32 variables have three of them besides `r8`-`r11`; a loop's promoted slots (below)
  live in the others instead of stack slots, which also lets a loop promote int32 slots, each in an integer register.
  Its int32 arithmetic goes from the operands' registers into the result's (`kThreeOperand`).

## The optimizing compiler

Tier 2 (`opt_analysis.cpp`, `opt_emit.inc`, `opt_inline.inc`) recompiles a function that stays hot in tier 1. It keeps int32 and
double values ("webs") in registers, and the rest of the frame as tier 1 does; a guard that fails leaves for the interpreter at
that instruction (an "exit"), and the next compile does not guess there. What it does besides arithmetic:

- Property reads and writes against the inline cache with the slot address fixed at compile time (one to five shapes);
  array and typed-array elements of the one kind a site has seen (`CodeBlock::elemProf`); `ToInt32` wrapping of `(a + b) | 0`
  without an overflow check.
- Inlining: a call, a getter read or `new` of a small function runs its body in place (`opt_inline.inc`). Bodies are
  straight-line (property reads, arithmetic, up-values, `super.x`, nested calls two deep, stores into a fresh `this` for
  constructors); every guard comes before the body's first side effect, so a failed one runs the call again in the
  interpreter. Callees come from a constant (`f` a global) or the call site's profile (`CodeBlock::callProf`, by `uid`).
- Direct calls of global functions: the callee is checked at the call and its frame pushed from constants; the registers
  that held the callee and `this` are never written.
- Registers that only carry a value to its one reader are not written: read chains (`os[i & 3].x`), the window of an
  inlined call, `Math.sqrt` and its kin. A failed guard in such a stretch runs it again from its start, which has no effects.
- `Math` one-argument functions in line (`sqrtsd`), `charCodeAt`, `push` and `pop`, string `length` and String.prototype
  methods (also in tier 1).
- A guard that fails three times in one place retires the code; code made before parts of the function had run makes way
  for a new compile after those parts have run a while (exit kind 2), up to four times, each given longer.
- Facts about frame registers, kept through straight-line code: an object's shape, its inline slot count (exact, or a
  lower bound), one of a few shapes, an array, a function of a known code, a slot that holds a number (so a store over it
  has nothing to release), a validity cell already checked.
- Loops (`planLoopFacts`): a register the loop never writes holds the same value all through it. The functions it calls
  through such registers are checked once before the loop (their code cannot change). When nothing in the loop calls out
  (no helper, no generic template, no direct call: `sideDoors_`), the loop also has checked before it the shapes of the
  objects in those registers, the chains its prototype reads rely on, the arrays it indexes, the number slots it stores to,
  the globals that head its read chains and its `Math` functions, and it reads once, before the loop, a number property
  that no store in it writes. Should a loop planned that way turn out to call out, the function is compiled again without
  that for it (`hoistBroken_`). The checks run where the loop is entered: falling into its header, OSR, and the tick stub
  of its back edge.
- Calls: the callee's registers come in `rdi` with its frame; a returned number is boxed in `rax` without a count; the
  caller takes a number result from `rax`; a call saves only the variables live after it. `VM::frameTop` points one past
  the top frame, so a push or a pop is one store.
- Recursion in place (`self_inline.cpp`): a small function that calls itself by its global name gets, when it turns hot, a
  copy of its block in which each such call is a guard (`JNotCallee`: the callee is the running function), the function's
  own instructions with their registers where the call's frame would have put them, and the call the long way; two levels
  deep, three for a function of at most 20 instructions (`fib`). The copy is ordinary bytecode (the interpreter and tier 1 run it too) and starts cold so the interpreter profiles
  it; stack traces show each call run in place (`CodeBlock::inlineSites`). Tier 2 skips the global read before such a
  guard and checks the global's slot and the frame's function, leaving the code when either differs; a guard (or a direct
  call of the same function) after one with nothing between that may run script does not look again.
- Which frame registers may hold a cell where (`findCells`, flow-sensitive): returns and call windows store over the
  others without a look, and a `LoadUndef` over no cell that nothing reads is dropped. A returned register the frame clears
  gives its count to the result; `Mov a, b` followed by `LoadUndef b` (or the return) moves the value with its count.
- A number read of a property that the block read before from the same register, with nothing between that may change it,
  takes the first read's value from its home slot (`planReadReuse`).
- Double arithmetic takes three operands with AVX (`vaddsd` and kin); a 32-bit load of the stack slot just stored from the
  same register is only a zero-extension.
- Number reads: a double falls straight through (one subtraction of the offset tests that it is a number), an int32
  converts out of line; the number test is left out where the slot is known to hold one. A double web that can only hold
  the NaNs arithmetic makes from boxed numbers (`planTame`: not one read from a typed array or returned by a native) is
  boxed without the NaN test, since neither the canonical NaN nor the processor's default one runs into the tags. An int32
  counter is stepped in its own register, and stepped back out of line when it overflows.
- An inner loop leaves out the checks an enclosing loop made before it (`guardsCovered`); a way in that did not come
  through them (OSR, the return from an interrupt) goes through `preLoopFull_`, which makes them first. A number slot that a
  hoisted read checks is not checked again, and two reads of one slot share one hoisted value.
- Borrowed registers (`planBorrows`): in such a loop, a register set only to elements of arrays the loop does not change,
  and read only as the object of a property, holds them without a count: the arrays keep them. It is let go of and set to
  undefined before the loop (it is dead there), counted again after the loop's last jump, on every exit to the interpreter
  (exits are kept apart by what they own), and before an interrupt throws; a loop inside one gets no OSR entry. This saves
  two count updates per element, and the stall a decrement of the last element causes when the processor runs it ahead
  of the increment of the same cell.
- The object a property read or a number store leaves in rax serves the next access of the same register while only code
  known to leave rax alone has been emitted (`raxObj_`).
- Optimized code runs calls only: a frame `new` made enters the baseline code (the interpreter, `scriptVmJitCall`, the
  baseline `New` sequence and OSR into tier 2 all see to it), so a return has no `this` to look for.
- Returns clear less (`planDirty`): a register no path to the return has written is undefined still. A forward `Jmp` to a
  `Ret` (or to `Mov a, b; Ret a`) makes a return of its own, which clears only what its path wrote, so the early return of
  `n < 2 ? n : ...` stores one register. A parameter given no argument is cleared only if the function writes it.
- Moves join the variables they connect when one ends where the other begins (`opt_analysis.cpp`): the two share a register
  or a slot and the `Mov` emits nothing. The peephole sees across instruction boundaries nothing jumps to
  (`bindFallthrough`), so a variable stored to its slot by one instruction is not loaded again by the next.
- `new` of a small constructor in place: when the callee, `this` and the arguments come from instructions without effects
  (`new T(null, null)`), their registers are not written (`planWindows`, `newCallee_`): the code reads the global itself,
  and the long way fills them first. An argument whose register dies there gives its count to the object.
- `x = o.p; y = null; if (x !== y)` with both registers dead after the jump: the jump reads the property itself and
  compares bits (`planTestReads`); neither register is written, and a return does not clear them.
- Slow paths of inlined `new` and direct calls are emitted after the function's code (`deferCold`), each with the
  variables' state at the point it is entered from.
- Global slots are addressed directly: the global object's slot storage moves only with `VM::globalEpoch`
  (`JSObject::growSlots` bumps it), which every global access checks first. A direct call writes the frame's `new.target`
  and generator only for a callee that reads them, and leaves alone the registers of its window no path to it has written.
- Loop slot promotion (`LoopPromo`): a number property of an object a loop holds in a register it never writes, read and
  stored in the loop through that register only, lives in a stack slot as a double; it is loaded before the loop and
  written back at the loop's exits, its exit stubs and its tick stub. An element read in the loop that yields the same
  object leaves the code (`promoGuards_`).
- Pure functions (`planNewChecked`: only typed arithmetic, data reads, calls of itself and `new` made in place): a call of
  itself cannot change a global, so only the first guard on each path checks the global and the frame's function, and an
  inlined `new` checks its constructor (global slot, shape, prototype, validity) only at the first such `new` on a path.
- Recursion that holds what it reads without counts (`planFnBorrows`, `scriptVmBorrowGate`): in a pure function that never
  writes its parameters, a register that only ever holds a parameter, a global, or a property read from one of those (or
  no cell at all) holds it without a count for the whole call, and passes it so to its calls of itself, which set
  `Frame::kArgsBorrowed` on the callee's frame. What could run other code there -- a failed guard, a slow path -- first
  calls the gate, which counts what that frame and every frame up the chain of borrowed arguments hold and marks them
  `kOwned`; each marked frame leaves its code right after its call returns (`exitOwnedAfter`). Such a function calls its
  own entry while the code is still the function's entry (`BorrowInfo::retired`), and makes the stack ready and takes the
  interrupt check without leaving. A call of itself that comes back unmarked ran nothing else: what is known of the
  frame's objects stays known across it, and the global read of a `new`'s constructor before the arguments is left out
  (`calleeReads_`: an exit between makes it first; the `new`'s long way reads it and clears it after).
- Facts carry into a block that only one forward jump reaches (`planSinglePreds`, `snapFacts`): `t.l === null ? ... : f(t.l)`
  reads `t.l` the second time without a guard.
- A counter stepped by one where every way to the step found it below an int32 (`planBoundedSteps`) has no overflow test.
- An object guard that also fixes the inline slot count tests the kind and the count with one 16-bit compare; a double
  read from a field is told from an int32 on its own bits and the offset taken off in place; `Math.sqrt` is `vsqrtsd` and
  `cvtsi2sd` zeroes its register first, so neither waits on what the register held before.
- Returns clear less: a `LoadUndef` that makes a register undefined takes it off what a return must clear, and the long
  way of a `new` made in place clears the window registers it writes.

`SCRIPT_VM_CODEDUMP=dir` writes each function's code (`t2-N-name@address.bin`, for `objdump -b binary -mi386:x86-64`);
`T2DBG`, `T2EXITS`, `T2INL`, `T2CHAIN`, `T2WIN`, `T2MATH`, `T2HOIST` (also the registers held without counts), `T2CELLS`,
`T2UNDEF`, `T2DIRTY` print what the compiler decided and where code leaves, `T2CODE` the bytecode it compiles (self-inlined
copies included) and `T2NOFNBORROW` turns the recursion gate off; `SCRIPT_VM_SELFINLINE=n` sets how deep recursion runs in
place (0: not at all).

Elsewhere for speed: `&&`, `||` and `!` in a test are jumps, not values; arrays keep their first four elements in the
cell; array literals of up to four elements are made in compiled code; `s += t` appends in place into a string only its
register holds; `JSON.parse` follows known shape transitions and reads short decimals without the library, and
`JSON.stringify` keeps the quoted names of a shape while no script runs; a cycle collection that finds little to free
waits until the cells have grown fourfold; `String.fromCharCode`, `Array.prototype.map`/`filter`/`forEach` and short
decimals in `Number.prototype.toString` have fast paths; `MulI` wraps when its result only reaches `ToInt32`.

## Arrays of numbers

An array knows how many of its first elements are numbers (`JSArray::packed`: no hole and no cell below it). It is a
lower bound that every writer of `elems` and `length` keeps (`noteStore`, `repack`), zero is always right, and it is zero
in a frozen array. The field fits in the eight bytes the heap's slot for an array had to spare.

- The optimizing compiler stores a number over a packed element with one compare and the store: the old value is not
  read (it is no hole, so nothing can intercept the store, and no cell, so there is nothing to let go of). A loop that
  strides through a large array then waits on no load.
- A number stored right after the packed ones adds to them, in the interpreter and in both compilers, so an array made
  with `new Array(n)` and filled in order is packed when the fill ends.
- Freeing an array, and the cycle collector's walk over one, skip the packed elements.

An element read that had not run when optimized code was made (the first loop of a function entered at a back edge
comes before the others) has that code make way for a new compile once it has run a while, so what it yields can be a
typed variable.

## Property read caches

A read site has one inline cache (own property, prototype property with a validity cell, in the word after the instruction) and,
when it sees more, a `PolyCache` of four entries reached through `InlineCache::pad`: own and inherited data properties and
getters. The interpreter and the helper `jitGetProp` probe it; tier 1 compiles a guard chain over the entries it has at compile
time (data entries only). Getters are not inline-cached in compiled code: they go to the helper, which calls them.

## Rules the code keeps

- Counts, not a tracing collector. A cell of the new family has a 16-byte header with the count at offset 8; byte 7 has
  its high bit set, which is how `detail::destroyUnreferenced` and the public API tell the families apart. Every register of
  a live frame owns its value, but for a borrowed register inside its loop (above), which the arrays it reads keep alive,
  and the registers a pure recursive function holds without counts until the gate counts them (above). The cycle
  collector counts only references from cells, so a register held without a count changes nothing for it.
- A cell is at most 8 KiB (larger storage is out of line), so `LocalHeap::ownerOf` finds the VM from any cell.
- A chain of frees does not recurse (`VM::destroy`): a plain object whose values are all in its cell is freed in the
  loop itself, the counters summed and set once, and the first value it frees is freed next; the rest wait in `pending_`.
  The cycle collector reads such an object's values in place too.
- Cycles are found by a collector that runs when the context's heap budget is passed, and also when the cells alive pass
  a budget of their own (`VM::cellBudget_`: 131,072 to begin with, then a little more than what survived, or four times
  as many when the collection found little), because the heap budget does not see the buffers of arrays and strings. A minor
  collection looks at the young cells; a whole one runs when the cells have doubled since the last. The collector's own vectors come from the payload pool, so a steady-state workload
  makes no call to the general allocator (`core.steady_state`).
- A proxy is an object in dictionary mode whose class is `Proxy` (or a function, for a callable target): no inline cache
  can match it, so every read and write of one reaches its traps. The caches also refuse a prototype chain with a proxy in it.
- Cells are below 2^47. The first heap of the process asks libcore (`sys::vmSetAddressCeiling`) to map below that limit
  (LC-1), from a window the kernel's own placement leaves alone, so it holds on aarch64 Linux too. If the window is full
  the kernel chooses and a cell above the limit aborts with a message.
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

- Tier 1 (the baseline compiler) compiles a function after 200 calls; tier 2 (the first optimizing form: int32 and double webs in
  registers) takes over after 2,000 more calls or loop iterations (`SCRIPT_VM_TIER1`, `SCRIPT_VM_TIER2` override in builds with
  `SCRIPT_JIT_ENV_OVERRIDE`; 0 turns a tier off). Both tiers run on x86-64 and on arm64 (Linux).
- Speed: against JavaScriptCore 2.52.6 with all its tiers, the 16 kernels of `bench/kernels/js` take 0.89x its time in
  geometric mean (Xeon Gold 5420+, median of eleven runs of each engine in turn on one core): `sort_cmp` 0.49,
  `float_mandel` 0.70, `alloc_small` 0.71, `string_ops` 0.86, `int_loop` 0.87, `map_dict` 0.91, `array_push_hof` 0.93,
  `prop_rw` 0.93, `fib_calls` 0.96, `closure` 0.97, `nbody` 0.97, `tree_gc` 0.97, `json_rt` 1.01, `sieve_array` 1.02,
  `poly_prop` 1.03, `method_call` 1.07. What is left: `nbody` is bound by the branch port (two tests per double field read,
  four per object guard), `tree_gc` by touching each freed node (counting frees one by one where a tracing collector
  touches none), and a call still pushes a whole frame in memory. Measure with both engines on the same core: the cores of
  this machine run at different clocks under load, which moved single-core comparisons by up to 6%.
- Calls are counted, not measured in stack: a call deeper than `Options::stackLimit` frames (512 by default) is a
  `RangeError`. Recursion run in place can take one frame for up to three levels.
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
