# The vm core

The execution core for JavaScript, TypeScript and Python (`src/vm`). It is the default: a `Context` runs on it unless told to
use the dynamic engine (`src/dyn`), which stays in the tree as the oracle (`Options::core = Dyn`, and `SCRIPT_CORE=dyn` in builds
with `SCRIPT_JIT_ENV_OVERRIDE`). This page says what is here and how fast it is.

## Tiers

| Tier | Where | When | What it is |
|---|---|---|---|
| Interpreter | `interp.cpp` | always | computed-goto dispatch over register bytecode; writes the profiles (inline caches, `elemProf`, `callProf`, arithmetic) |
| Baseline (tier 1) | `baseline_x64.cpp` | after 200 calls, or about 1,000 loop rounds | one template per instruction, on the interpreter's frame; generators and async functions too |
| SSA tier | `ssa_build.cpp`, `ssa_emit.inc` | arm64: after 2,000 more calls or loop rounds | the default optimizing tier on arm64; a value graph with representations and a linear-scan allocator |
| Tier 2 | `opt_analysis.cpp`, `opt_emit.inc` | x86-64; on arm64 for what the SSA tier refuses or leaves (`ssa::preferTier2`) | templates over int32 and double "webs" in registers |
| Regexp compiler | `regexp_jit.cpp` | arm64 | native matchers for regular expressions |

Two bytecode-level inliners feed the compilers: merge inlining (`merge_inline.cpp`) and recursion in place
(`self_inline.cpp`). A function of a large program is compiled to bytecode at its first call (`CodeBlock::kLazy`, below).

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
| `src/vm/jit/`, `include/script/internal/vm/jit.hpp` | the compilers (x86-64 and arm64, Linux): `asm_x64.hpp` and `asm_a64.hpp` (two macro assemblers with one interface), `code_region.cpp` (dual-mapped code memory), `baseline_x64.cpp` (templates, calls, entry stubs, written once for both), `ssa.hpp`, `ssa_build.cpp`, `ssa_emit.inc` (the SSA tier), `opt_*` (tier 2), `merge_inline.cpp`, `self_inline.cpp` (the inliners), `regexp_jit.cpp` (regular expressions) |
| `src/vm/builtins.cpp`, `builtins_*.cpp` | the built-in objects: Object/Function/Array/String/Number/Math/Error/JSON (`builtins.cpp`), Symbol, iterator families and generator prototypes (`_symbol`), the rest of Object, Reflect, Array/String/Number/Math and BigInt (`_more`), the Array methods that call back (`_array`), Map/Set/Weak\*/WeakRef/FinalizationRegistry (`_collections`), RegExp (`_regexp`: the spec's `exec`/species paths, a fast path for an untouched RegExp, the legacy `RegExp.$1` statics, `RegExp.escape`), Promise with capabilities and the combinators, async functions and generators (`_promise`), Date (`_date`), ArrayBuffer (resizable, shared), typed arrays and DataView (`_typed`, `_typedarray`), Atomics, iterator helpers, Proxy (`_proxy`: callable proxies) |
| `src/vm/objmodel.cpp` | the internal methods of an object -- [[GetOwnProperty]], [[DefineOwnProperty]], [[Set]], [[GetPrototypeOf]], [[OwnPropertyKeys]] and the rest -- for ordinary objects and proxies, with every invariant check a proxy's traps are held to; `Object` and `Reflect` are built on them |
| `src/vm/python.cpp`, `src/vm/language.cpp` | Python and TypeScript on the new core |
| `tests/vm`, `tests/vm/snippets.inc` | unit tests; programs both cores must agree on |
| `tools/vm_fuzz.py`, `bench/micro` | differential fuzzer; micro-benchmarks that also run under JSC and node |
| `tools/perf/` | the scripts that build, gate and time the vm core against JetStream 3 (`tools/perf/README.md`) |

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
- Generators, async functions and async generators are compiled too. A `yield` or an `await` writes the state into the
  generator's cell; a resumption enters the compiled code at the instruction after it (`jitGenEnter`).
- Not compiled: functions of more than 20,000 instructions, and functions past `jit.codeCacheLimit` (tried again later).
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

## The SSA tier

On arm64 the SSA tier (`ssa.hpp`, `ssa_build.cpp`, `ssa_emit.inc`) compiles a hot function first. Tier 2 compiles the functions
it refuses: async functions, generators without a loop that does not yield, and `with` scopes. A generator's code is entered
only at the head of a loop that does not yield, from the baseline code, which runs each resumption and looks again after 64
back edges. A `yield` writes the frame and goes on in the baseline code at the `yield` (exit kind 3). `SCRIPT_VM_SSA` sets the mode: 0 off (tier 2 only), 1 every instruction through the baseline templates, 2 (the
default on arm64) with its own fast paths. It is off on x86-64, where it is not tested.

**The graph.** One value per instruction and register it writes, per register at the entry, and per phi. A `Mov` is a second
name for a value. A loop head has a phi for each register the loop writes; phis that join one value only are removed.

**The planner** (`ssaPlan`) gives each value a representation: int32, double or boxed. An arithmetic site takes it from its
profile. A site with an empty profile (code that only tier 1 has run: only the interpreter writes profiles) takes it from its
operands. A double operand widens an int32-profiled site to a double, and once an add or product of a function has failed, its
other int32 sites are decided by their operands; without these rules, each site would leave in turn, with a compile each. An
int32 add whose every use truncates to int32 has no overflow check.

**Registers.** A linear scan gives each value one register or one slot. A value across an instruction that the code does not do
in place (a template: it clobbers registers) lives in a slot. When no register is free, the value needed longest gives up its
register. A loop that clobbers nothing keeps the values it reads from before it in registers (the loop register cache).
The value a back edge gives a phi takes the phi's register when the phi is needed no more where that value is made
(`i = i + 1`, `s += x`), so the way round the loop has no move for it; until then that register goes only to a value
that is done before, or to one that finds no other.

**Ownership.** Each frame slot owns one count of what it holds. A value no slot owns is borrowed, and stays valid until an event
(a call, a release, an exit); `ssaSync` gives every live value an owner before one. An exit writes the live values into the frame
and goes on in the baseline code at that instruction.

**In place:** int32 and double arithmetic and comparisons, property reads (one shape or several, own or on a prototype), property
writes and adds, element reads and writes of arrays (holes as well, when `plainHoleGuard` holds) and of typed arrays, private
field reads, `typeof`, strict and loose equality, globals, upvalues, `let`/`const` checks. A plain constructor's `new` (stores of
its arguments, constants and static fields into new properties, class field initializers too) makes its object in place. A call
whose site has called one code is made directly, with its frame pushed from constants. Checks on the way into a loop (an array
it reads, a boxed loop variable) hold in all of it.

**Methods that name private fields** (`#x`) are merged into their callers like other small methods: the copy has the
`PrivateCache` of each instruction it takes over.

**It inlines nothing itself.** With the SSA tier, merge inlining (`mergeInline`) also takes a callee of up to 12 instructions at
up to 64 sites. Tier 2 compiles a function where it uses what the SSA tier lacks (`ssa::preferTier2`): a self-inlined recursion
(borrowed frames), a loop that calls a tiny closure that writes an upvalue or reads through a tiny getter (run in place), and a
small loop that reads and writes three or more properties of objects it does not change (kept in registers).

**Also in place:** `Math.sqrt` and the other one-double natives, `s.charCodeAt(i)`, `String.prototype` methods of a string,
`.length` of strings and arrays, `ModI` (a multiply by a magic number), and a global read that a loop which clobbers nothing
reads once on the way in. Values known never to be below zero (`nonNeg_`) need no sign or -0 checks.
A product has no -0 check when no use tells a negative zero from zero (`zeroOk_`): int32 sums with a value that is no
such product, an int32 difference it is taken from, relations, and uses that truncate.

**Direct calls.** A call between functions of SSA code checks the stack (frames, registers, `stackInit`) once, at the
function's entry and at its OSR entries, for the largest window any of its direct calls needs (`limitNeed_`). It does not
check `this` when the value is known to be an object. The tick (the interrupt check) stays at each call: a recursion with
no loop asks the host only there.

In a deferred frame a direct call also passes `this` and the first two arguments in registers (rax, rcx, rdx), beside
their frame slots. The callee's SSA code has a second way in for that (`Tier1Code::argsEntry`): a read of the slot would
wait for the caller's store. Baseline code has the same address for both ways in and ignores the registers. The result
comes back in rax and in the caller's frame slot, and the caller goes on from rax. One call and return is about 90
instructions; most of them write the frame record and check the callee.

**A property read with more shapes than its cache.** A read whose code has left for a shape the cache does not hold
is not given up to the template: the shapes of the cache are tested in place and anything else calls the runtime out
of line (`ssaColdRead`). The misses are counted (in `CodeBlock::callProf`, which holds nothing else for a read); at
256 the code makes way for a compile that takes the template there, which calls the runtime with less around the
call. delta-blue's `c.execute()` over constraints of many shapes is the case that gains.

**A property store under several shapes.** A store of a number whose cache holds writes to own data properties of
up to five shapes compares the shapes in turn and stores into the slot of the one it finds (`ssaPutProp`); before,
only a store with one shape was done in place. Load reuse forgets the slot of each of the shapes. A value that may be
a cell still runs as the template there: in place it needs every live value owned first.

**An element of an array literal** (`ArrayPush`) is stored in place behind the last while the array has room; a
literal is made with room for its elements. A spread that needs more leaves the code.

**Sums whose uses truncate.** An int32 add or subtract whose every use truncates (`| 0`, `&`, a shift, another such
add) wraps with no overflow check (`wrapOk_`). This also holds for a sum that the profile made a double (its place
has made a number past int32) when its operands are int32s or such sums: `(a + b + c) | 0` is three int32 adds, where
it was three double adds with a conversion for each term.

**A remainder of numbers.** `a % b` whose place has made a double (`n % 0` is NaN), or whose int32 code has left once,
is done in place: the integer remainder for two int32s (the first not below zero, the second above it), else `fmod`.
`!x` in place takes doubles too (zero and NaN are false).

**`x >>> 0`.** Where it has made numbers past int32, it is done in place: ToInt32 of the operand, read as unsigned,
into a double. Before, it ran as the template and called the runtime.

**Strict equality against a constant symbol or object.** A jump on `x === K` where `K` is a constant symbol or object
compares bits. A `switch` over such constants (Air's opcodes, 321 cases) is then two instructions and a constant for
each case, where the general comparison was 412 bytes of code for each.

**An element access that meets arrays and other objects.** A place that has met arrays of values and also other
objects (a typed array of a kind the template does not do in place, a string, any object) reads and stores the array
in place and calls the runtime out of line for the rest (`ssaElemMixed`). Before, such a place ran as the template.
Not with typed arrays of doubles, int32s or bytes, which the template does in place.

**`instanceof` in place.** `a instanceof F` with `F` a constant ordinary constructor whose `prototype` is an object in
its usual slot is a walk up the prototypes of `a` by their shapes (`ssaInstanceOf`). The code checks that `F` has the
shape and the `prototype` it was found with. It relies on no object having a `Symbol.hasInstance` and no proxy being a
prototype: the first of either moves the epoch. A left operand that is a proxy or a function leaves the code.

**The interrupt check in loops.** A back edge of SSA code looks at the host's flag only when the code was made while the
host had an interrupt handler. Code made without one has no check there: each back edge is a poll site
(`Tier2Code::pollSites`). When the host sets a handler, `VM::interruptArmed` writes a jump over the instruction at each
site; the loop then leaves for the baseline code at its head (exit kind 7), which counts down to the handler, and the
next compile makes code that looks. This takes four instructions and two loads out of each round of a loop. Code
that cannot be patched (x86-64) always looks.

**A value across calls.** Every instruction the SSA tier does through a template or a call clobbers its registers, so
a value needed across one lives in a stack slot. The exception is R21 (arm64 x25): R21 and R22 come back from every
call, helper and template as they went in (SSA code that uses them saves them where it is entered and puts them back on
each way out; no other code writes them). `ssaPlan` gives R21 to the values needed across such instructions that loops
read most, one at a time. Only one of the two registers is used this way: a callee saves and puts back both as a pair,
so a value in one waits for the value in the other across each call.

**A merged callee whose result is tested.** When the caller tests the result of a call at once (`if (o.m())`), each
return of the merged callee gets its own copy of the test (`mergeInline`), and a return reached from a test of the same
register (`return a || b`) jumps straight to where the caller's test would go. In the SSA code a compare whose boolean
only the jump at the end of its block reads makes no boolean (`ssaFusedJump`).

**Load reuse.** `ssaLoadElim` gives a second read of the same slot, box, upvalue or array element (plain or typed, the
same index value) the value of the first, while nothing between can have written it.

**The new back end (in progress).** `lir.hpp` is a lowered IR under the SSA graph: one block per SSA block, nodes in
SSA form with effects as heap regions, and the node that makes each SSA value. `lir_build.inc` builds it after the
representation plan (`SSA_LIR=dump` prints it, `SSA_LIR=off` turns its passes off). An element access in place is
lowered to its checks (`CheckBounds`, `CheckHole`) and a `LoadElem` or `StoreElem`; `ToNumeric` of a number is the
number itself; every other instruction is one `Generic` node, which writes the region `kWorld` when it can reach the
runtime. The emitter still makes the code one bytecode instruction at a time, and reads what the passes found.

*Check elimination* (`lir::checkElim`, lir_opt.cpp) is a forward data flow over the blocks: a check of an array and an
index holds after a node that made it, on every way, until a node writes `kWorld`. The head of a loop the baseline
code can enter starts with nothing. What the emitter does with the result:

- A store into a plain array, of an element that a read before it found (`a[i] = a[i] + x`, `w[j++] = l` after a read of
  `w[j]`): an int32 into an array of int32s, or a number into an array of doubles, is stored with no bounds check, no
  hole check and no path to the runtime. Such a store writes no `kWorld`, so load reuse and shape facts hold across
  it. A NaN or an array of another kind leaves the code.
- A typed array access after an access of the same element: no bounds check.
- A plain element read that a later access relies on leaves the code on a miss also when its result is boxed
  (`lirChecked_`).

Not done yet: property access and arithmetic as nodes, loop-invariant motion, range analysis, code from the LIR.

**What leaves for the runtime.** An instruction the code does not do in place runs as the baseline template, or as one
step of the interpreter (`scriptVmJitStep`, counted by `SCRIPT_VM_STEPS`). A relational or truth jump whose site failed is
decided by `ssaStepJump`: two strings by `compareStrings`, any other value by `truth`, with the live registers saved.
`charCodeAt` and `codePointAt` of a rope join it by a call (`scriptVmSsaFlatten`). `+` of two strings calls
`scriptVmJitConcat` from the template directly.

**Switches** (builds with `SCRIPT_JIT_ENV_OVERRIDE`): `SSA_DUMP` (bytecode and graph), `SSA_REPS` (representations),
`SSA_REFUSED`, `SSA_GENERIC` (each instruction compiled through a template, with its cache state), `SSA_SYNCALL`,
`SSA_NOREGS`, `SSA_NOEVICT`, `SSA_NONEW`, `SSA_NOPREFER` (no function left to tier 2), `SSA_PREFER=<mask>` (which rules),
`CALLDBG` (calls that reach `callPrepare`, per site), `WINDBG` (the call windows), `RETDBG` (what a return clears).
`T2EXITS` names the exits of both tiers, with the opcode and, for element exits, the caller.

## Deferred frames

`Context::Options::memory` picks how frames hold values (`VM::deferred`; docs/memory.md has the host's view). A counted frame
owns a count of each value in its registers. A deferred frame owns none. These rules keep a deferred frame sound:

1. Only cells, handles and the runtime's own variables hold counts. A count that reaches zero puts the cell in the zero-count
   table (`zctBase_`..`zctTop_`, marked by `gc == kInZct`; strings, symbols and bigints in `zctOld_`, marked by `inZct`).
2. The live registers hold counts through a shadow (`shadow_`): a copy of the registers as the last scan saw them, each cell
   in it holding one count until the next scan. `scanZct` first brings the shadow up to date (`syncShadow`): a register that
   changed counts its new cell, and its old cell loses the shadow's count (back to the table at zero). Then it frees what is
   still at zero. A deep stack that stays costs one compare a register, not a count a cell. Live registers are those of every
   frame, and those below `stackTop`. The scan writes undefined in the registers above them.
3. A collection brings the shadow up to date first (`rootsBegin`), and the table is empty while it runs.
4. A string with `StringCell::kSole` (rule 8) stays out of the shadow: it is counted for the scan only (`transient_`), so that
   no count stops it being appended to in place. When no frame is left, the shadow lets go of its counts (`scanAtHost`).
   When the shadow cannot grow, the scan counts the registers and takes the counts away again (`countRoots`).
5. A scan starts only in C++: an allocation, a budget, a collection, the return of a call from the host. Compiled code stores
   each live cell in a frame slot before it calls C++.
6. Compiled code owns no counts. It counts what it stores into cells. A value from C++ has lost its count (`handOver`), and
   nothing runs before that value is in a frame slot. A compiled `Ret` returns its result without a count, and `enterTier1`
   takes one for C++.
7. C++ code that puts an owned value in a register uses `setReg` or `setRegister`. Code that copies a value there uses `copyReg`
   or `copyRegister`. Code that hands a register to a native covers it with `stackTop`.
8. A count of 1 does not mean one holder. A string is appended to in place only when it has `StringCell::kSole`: one register
   holds it, and nothing else. The bytecode compiler emits `Append` for `s += t` and `s = s + t`, where `s` is a local that the
   text gives strings (`appendedNames`). It emits `Unshare`, which clears the bit, before each instruction that copies `s`.
   Any other sum into its own left operand grows a buffer that only slices show (`appendShared`).
9. A deferred frame sits on whatever the stack holds. The interpreter makes its registers undefined. Compiled code makes undefined
   those it may read before it writes them (`entryUndefs`). A caller makes undefined the parameters it has no argument for. No
   return and no call clears registers. A stale register never names a cell that is gone: only a scan frees, and it clears what
   is above the frames first. When no frame is left a free is immediate, and `stackInit` goes down to `stackTop`.

Tests run in both modes: `SCRIPT_VM_DEFERRED=0` and `=1` set the mode in a build with `SCRIPT_JIT_ENV_OVERRIDE`.
`SCRIPT_VM_AUDIT=1` checks every count at each scan. `SCRIPT_VM_ZCT=n` sets the table's least limit (0: every allocation scans).

## Young cells

With deferred frames, a new plain object, closure or box can be young (`VM::young`). Most of them die before the next scan. A
young cell is freed without a look at what it holds, and a store into it counts nothing. A cycle of young cells that nothing
holds (a closure and the box of the variable that holds it) goes at the next scan, with no cycle collection. These rules keep
it sound:

1. A young cell has the bit `VM::kYoungGc` in its `gc` word, its allocation site above bit 8, and is in the young list
   (`youngBase_`..`youngTop_`). A young cell is a plain object or a closure without `Rare`, or a box. Compiled code makes
   young cells (`allocObject`, and the `Closure` and `NewBox` templates through `allocCellHere`), as do `scriptVmJitNewThis`
   and `scriptVmSsaNewObject` for it. The runtime makes old ones.
2. A young cell holds no count of what its slots hold. Its own count is what old cells, handles and the runtime hold. A count
   of a young cell that reaches zero does nothing: the cell does not go into the zero-count table.
3. A store into a slot of a young object, or into a young box, neither counts the new value nor lets go of the old one. The
   interpreter's caches (`storeSlot`, `storeNewSlot`), its `PutBox` and `PutUpval`, and compiled code (`storeSlotCounted`,
   `storeBoxCounted`, `retainInto`, `ifYoung`) test for this. A young closure holds no count of its boxes. It does hold a
   count of its code, which the scan lets go of when the closure is dead.
4. Any other path makes the object old first (`VM::makeOld`): `findOwn`, `addProperty`, `deleteOwn`, `setAttrs`, `rareOf`,
   and a prototype change. An old cell takes a count of each cell its slots hold. A cell held by nothing then goes into the
   zero-count table.
5. A scan first marks the young cells that a live register holds or that have a count, and what they reach among the young
   (`forEachYoungValue`: properties, a closure's boxes, a box's value). It makes the marked cells old and frees the others
   (`mem::LocalHeap::FreeRun`: one update of a block for a row of dead cells in it). Then it counts the frames' cells and
   frees the table as before.
6. While a young cell exists, no free is immediate: a young cell may hold a cell without a count. A collection and the
   context's end see no young cell (`rootsBegin`, `releaseRoots`). A collection writes the `gc` word of the cells it looks
   at, so the runtime takes a cell for young only while the list holds any (`VM::youngHeader`).
7. The list has room for `VM::kYoungCells` cells. When it is full, compiled code makes old cells (into the zero-count table)
   until the next scan.
8. Between jobs, with no frame, the young cells are collected without a scan of the table (`collectYoungAtHost`): what no
   frame holds is freed at once again. A free that frees what a cell holds leaves a cell that is in the table there; the
   next scan frees it.
9. Each allocation site (`VM::allocSiteFor`, one per instruction) learns how many of its cells a scan found alive, from one
   cell in seven of the list. When seven of eight lived in a window of `VM::kSiteWindow` cells, the site makes old cells from
   then on (`AllocSite::old`): making a cell old costs more than counting its values while they are stored.
10. A young cell is not in the live-cell counts and does not compare the cell budgets when it is made (`uncountYoung`).
   It is counted when it lives on (`makeOld`, or a survivor of `collectYoung`). A dead young cell was never counted.
   `Context::stats()` adds the young cells that are still in the list (`VM::youngLive`).

What young cells do not cover yet: arrays, strings, generator and promise cells, and every cell that the runtime makes
(closures and boxes of the interpreter too). Those are counted from birth, go into the zero-count table at zero, and are
freed one at a time (`destroyFrom`). An old cell that dies holds counts of young cells until the scan frees it, so those
young cells live one scan longer.

A scan also runs when the zero-count table is full, which in a program that makes many arrays is long before the young list
is (cdjs: a scan for each 235 young cells before closures and boxes were young).

Young cells are on with deferred frames. `SCRIPT_VM_YOUNG=0` turns them off in a build with `SCRIPT_JIT_ENV_OVERRIDE`.

## Arrays of numbers

An array of values knows how many of its first elements are numbers (`JSArray::packed`: no hole and no cell below it).
It is a lower bound that every writer of `store` and `length` keeps (`noteStore`, `repack`), zero is always right, and it
is zero in a frozen array. The field fits in the eight bytes the heap's slot for an array had to spare.

- The optimizing compiler stores a number over a packed element with one compare and the store: the old value is not
  read (it is no hole, so nothing can intercept the store, and no cell, so there is nothing to let go of). A loop that
  strides through a large array then waits on no load.
- A number stored right after the packed ones adds to them, in the interpreter and in both compilers, so an array made
  with `new Array(n)` and filled in order is packed when the fill ends.
- Freeing an array, and the cycle collector's walk over one, skip the packed elements.

An element read that had not run when optimized code was made (the first loop of a function entered at a back edge
comes before the others) has that code make way for a new compile once it has run a while, so what it yields can be a
typed variable.

## Arrays of int32s and of doubles

An array that a program makes with nothing in it (`[]`, `new Array(n)`) stores its elements for what they are. The kind
is the class byte of the header (`ObjClass::IntArray`, `ObjClass::NumArray`). `JSObject::cls` says `Array` for each
kind, so a program sees no difference.

| Kind | A place holds | A hole is |
|---|---|---|
| of int32s | an int32 value | the hole value |
| of doubles | a double as it is | one NaN that no store leaves there (`JSArray::kNumHole`) |
| of values | any value | the hole value |

- An array of int32s becomes one of doubles at the first number with a fraction (also a NaN, an infinity, the negative
  zero).
- An array of int32s or of doubles becomes one of values at the first store of anything else. A whole number past int32
  is such a store into an array of int32s: an array of words for bit operations reads fastest as values.
- An array of values stays one. Compiled code that has seen an `Array` of values does not look again.
- Each kind has eight bytes for an element. A change of kind is made in place: no memory is asked for, no cell moves.
- A whole number read from an array of doubles is an int32 again (`JSArray::get`).
- An array of int32s or of doubles holds no cell. Freeing one, and the collector's walk, skip its elements.
- Such an array is never frozen and always extensible: `Object.freeze`, `seal` and `preventExtensions` make values of
  it first.
- Runtime code that knows values only asks for them (`JSArray::vals`), and the array is one of values from then on.
  Only code that goes by the kind reads `JSArray::store` itself.

The place that makes an array learns from it. An array has the number of its place (`JSArray::site`, in the four bytes
its cell had to spare; `VM::arraySites_`, at most 65,536). When what a program stores changes the kind, the place notes
it (`VM::noteKindChange`) and makes its next arrays of that kind from the start. So an array of objects is one of values
from its first element, and a row of a matrix is one of doubles before its first number.

Compiled code:

- The element profile (`CodeBlock::elemProf`) has a bit for each kind a place has met.
- Baseline code reads and stores an element of an array of int32s in place. An array of doubles, and `push` and `pop`
  of both kinds, go to four stubs that all functions share (`KindStubs`): a place costs about ten instructions of code.
- The SSA tier makes code for the kinds the place has met. A read of an array of doubles for a double result is the
  length test, the load and a NaN test (a NaN is a hole or a number; which, is found out of line).
- A loop whose instructions are all made by the SSA tier checks the kind once on the way in (`LoopGuards::kinds`), for an
  array that the loop does not replace and whose places met one kind only. Only the runtime changes a kind, and such a
  loop reaches the runtime by cold paths only. Each cold path checks the kinds again after its call, and leaves the
  code after the instruction when one has changed (exit kind 8).
- Another kind than the code was made for is no failure of the place: the profile gets the bit, and the code is made
  again (exit kinds 7 and 8, and kind 0 at an element access).

`SCRIPT_VM_KINDS=0` makes every array one of values, in a build with `SCRIPT_JIT_ENV_OVERRIDE`.

## Property read caches

A read site has one inline cache (own property, prototype property with a validity cell, in the word after the instruction) and,
when it sees more, a `PolyCache` of four entries reached through `InlineCache::pad`: own and inherited data properties and
getters. The interpreter and the helper `jitGetProp` probe it; tier 1 compiles a guard chain over the entries it has at compile
time (data entries only). Getters are not inline-cached in compiled code: they go to the helper, which calls them.

A site that sees more shapes than that is marked megamorphic (`InlineCache::flags`) and reads through one table for the VM
(2,048 entries, `VM::kMegaBits`), keyed by shape and name. Only marked sites use the table.

`o[k]` with a key that is no index has an element cache of four ways (`ElemCache`): key, shape, slot, holder, validity
cell, and whether the slot is an accessor. A getter entry calls the getter (`jitGetElem` through `VM::callHook`).

A literal `InitProp` (`{a: 1, b: 2}`) in baseline code whose cache was empty at compile time reads the cache when it runs
(shape before, slot index and shape after), for inline slots.

## Strings

- UTF-16 cells: flat, rope, slice or an appendable buffer. Strings of up to 32 units that `ContextImpl::str` makes are
  interned. A string used as a property name is made an atom then (`toPropertyKey`).
- `String(i)` of an integer in [0, 65536) gives the existing atom of that index, when one exists.
- `s += t` on a local that only strings reach appends in place (`Append`, `Unshare`; deferred frames, rule 8).
- An atom cannot be appended in place, so a change that interns more strings must leave out strings that will grow.

## Regular expressions

- Programs come from the dynamic engine's compiler (`dyn::regex::Program`); `regex::exec` runs them by backtracking.
- On arm64 a program that has run hot (`kHot`, weighted by the input's length) gets a native matcher (`regexp_jit.cpp`).
  It pushes no choice that cannot succeed: a choice whose first branch cannot begin with the next unit, a `^` past the
  start, a greedy loop followed by what its own units cannot begin.
- `planClean` finds places reached with only the next-start entry on the stack. A `Save` there writes with no undo
  entry, and a failure there goes on at the next start position. Only for programs of plain instructions.
- Start search: the first unit by a SIMD scan; without one, a unit that every match has at a fixed distance from its
  start is looked for first.
- Each regular-expression literal site keeps its compiled program in a table of 512 entries (`VM::regexpLiterals`). A literal
  `RegExp` that the program did not change reads its `lastIndex` by its shape (`VM::regexpLiteralShape`).
- A literal without `g` and `y` that is only the receiver of `test` or `exec` (`/^b/.test(s)`) is one object for its site
  (`Op::SharedRegExp`, `sharedRegExpLiteral`), while `RegExp.prototype` has its original methods. Such a call neither changes
  the object nor hands it to the program. `regexpTest` lets go of all shared objects before a replaced `exec` gets one as
  `this`, so the program sees such an object once only.
- Not done: a dispatch on the code point for Unicode alternations whose branches all begin with the same lead surrogate.

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
  a budget of their own (`VM::cellBudget_`: 131,072 to begin with, then what survived plus at least 32,768, or four times
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

## Speed against JavaScriptCore

State on 2026-11-26 (`649033b`). JetStream 3, the 41 JavaScript workloads that `tools/probes/jetstream.py` writes, release
build, arm64 (Cortex-X925, core 16), JavaScriptCore 2.52.6, best of two interleaved rounds. The ratio is libscript's time
over JSC's time; below 1.00 libscript is faster.

| | Later iterations | First iteration | Peak resident size |
|---|--:|--:|--:|
| Geometric mean of 41 | 1.30x | 0.70x | 0.21x |

- Furthest behind (later iterations): splay 2.63x, Sunspider 2.18x, js-tokens 1.93x, octane-code-load 1.90x, ML 1.90x,
  OfflineAssembler 1.84x, sync-fs 1.83x, doxbee-async 1.68x, WSL 1.64x, threejs 1.64x, UniPoker 1.61x, async-fs 1.52x.
- Ahead or level: stanford-crypto-aes 0.84x, crypto 0.85x, proxy-mobx 0.88x, navier-stokes 0.92x, gbemu 0.93x,
  Basic 0.97x, pdfjs 0.97x, stanford-crypto-pbkdf2 0.99x, mandreel 1.00x, delta-blue 1.00x.
- JSC's own times move between runs on some workloads (threejs 22.3 ms and 26.8 ms, js-tokens 15.8 ms and 13.2 ms in
  two runs of the same binary an hour apart), so one workload's ratio is good to about a tenth.
- Memory (`tools/perf/rss.sh`, `tools/perf/soak.sh`): no workload is above JSC's peak by more than 3%
  (json-stringify-inspector 1.03x). Over 300 iterations octane-code-load, pdfjs and proxy-vue grow, which keep data
  per iteration (JSC grows more on each of them); gbemu grows 7% and hash-map 39% in resident size between iterations
  50 and 299, both also before the arrays of int32s and doubles.
- First iterations are ahead on most workloads. Behind: json-stringify-inspector 2.59x, octane-code-load 2.14x,
  regexp-octane 1.84x, threejs 1.39x, WSL 1.26x, OfflineAssembler 1.13x.

### Where the time goes

(The table and the list below are from `4be6b64`, when the mean was 1.44x. Since then: closures and boxes are young
cells, the young sweep costs less, array buffers and the parser's nodes left the general allocator, and in SSA code a
direct call keeps its result and its first arguments in registers and one value stays in a register across calls.)

The breakdown of 2026-11-25 (`4be6b64`): each workload profiled once by cycles and once by instructions, samples sorted
into categories by symbol and by the perf map of compiled code. The bound is the geometric mean if a category took no time.

| Category | Mean share | Geomean if it took no time | Geomean if it took half |
|---|--:|--:|--:|
| Compiled code (SSA 39%, baseline 5%, stubs and matchers 2%) | 46% | 0.65x | 1.09x |
| Memory management (allocation 9%, freeing 6%, scans 2%, cycles 2%) | 19% | 1.17x | 1.30x |
| Runtime glue (property misses, calls, interpreter, arrays, numbers, JSON, other built-ins) | 17% | 1.18x | 1.31x |
| Strings and regular expressions in C++ | 9% | 1.30x | 1.37x |
| Generators, promises and their baseline code | 8% | 1.30x | 1.38x |
| Parser, bytecode compiler, machine-code compilers | 5% | 1.36x | 1.40x |

Against JSC, whole runs counted by `perf stat` on one core (JSC's compiler threads included):

| Geometric mean over 41 | libscript / JSC |
|---|--:|
| Instructions | 1.87x |
| Cycles | 1.42x |
| Branch misses | 0.65x |
| L1 data misses | 1.22x |

libscript runs more instructions per cycle than JSC on all 41 workloads (IPC 3.3-8.3 against 2.7-6.4).

Per later iteration in steady state (a run of 45 iterations less a run of 15, 40 workloads), libscript runs 2.94x JSC's
instructions and 2.60x its cycles. The JetStream later-iteration mean (1.43x) is lower because JSC's iterations 2-15 still
include its own tier-up compiles. JSC's top-tier code is about 2.6x faster than libscript's SSA code.

What SSA code spends its instructions on (instruction samples, arm64 skid included): spill loads and stores (gaussian-blur
16%, hash-map 18%, raytrace 10%), register moves (crypto 42%), tagging of doubles (navier-stokes 51%, raytrace 19%, ML 12%),
and guards. Seen in the hot loops: a value read twice is read and checked twice (`this_array[i]` in crypto's `am3`); a typed
array's length and data pointer are loaded again at each access; loop-invariant values live in spill slots, because the
SSA tier allocated from 9 general registers on arm64 (12 since x18, x25 and x26 were added) and keeps any value live across a runtime call in a slot for its whole
life; products keep their -0 checks when every use truncates.

What this means for the design:

1. The gap is path length: libscript runs about twice JSC's instructions for the same work, and not more slowly per
   instruction. The share of instructions in each category is the same as its share of cycles.
2. No one category closes the gap. 1.00x needs about 30% less time on the typical workload, for example: SSA code a
   third shorter, memory management and runtime glue each half as long.
3. SSA code leads on most compute workloads (gaussian-blur 95%, navier-stokes 91%, richards 87%, hash-map 83%, crypto 77%,
   stanford-crypto-aes 74%, raytrace 71%, gbemu 66%, ML 63%, delta-blue 61%).
4. Memory management is in almost every profile: frees one cell at a time (`destroyFrom`), the zero-count table's scans,
   and the cycle collector (all memory management: splay 47%, cdjs 33%). The young generation covers only plain objects that compiled code makes.
5. Compiled code calls the runtime through generic helpers that read the instruction and the frame again.
6. A resumed generator or async function runs baseline code (lazy-collections 35%, js-tokens 28%, Basic 26%).

### What held across the work

1. Measure the premise before building. Several designs failed on a counter or a bound: a traced old generation
   (marking cost more than freeing), Latin-1 strings, allocation removal for raytrace (the time was an FP stall), double
   fields without boxes (doubles are NaN-boxed already, as in JSC).
2. Count instructions to tell work from placement. A change of 1-4% on a workload that the change cannot touch is
   often placement: `perf stat -e instructions:u,cycles:u` shows the same instructions and more cycles.
3. Generic library paths in hot code paid well when replaced: `__udivti3`, `typeid ==`, `u16string_view ==`, names
   interned from C strings, function-local static guards.
4. Caches keyed by ids that are never used twice (shape ids carry a generation) need no counts. Caches keyed by cell
   addresses hold counts, or only steer a policy whose wrong answer is still correct.
5. Prefetching never helped in this allocator: reusing the slot freed last, still in cache, wins.
6. Heuristics that depend on timing make results fragile. proxy-mobx moved 7-8% with when a compile landed relative to
   a closure's creation (fixed by invalidating only the code that assumed one function per code).
7. JSC's own later times move 1-15% between runs on some workloads (richards, js-tokens, splay). Compare inside one run.

`tools/perf/README.md` has the method: builds, the gate, A/B timing, profiles.

## Known limits

- Tier 1 (the baseline compiler) compiles a function after 200 calls; the optimizing tier (the SSA tier on arm64, tier 2 for what
  it refuses and on x86-64) takes over after 2,000 more calls or loop iterations (`SCRIPT_VM_TIER1`, `SCRIPT_VM_TIER2` override in
  builds with `SCRIPT_JIT_ENV_OVERRIDE`; 0 turns a tier off). Tier 1 and tier 2 run on x86-64 and on arm64 (Linux).
- Speed: see "Speed against JavaScriptCore" above.
- The SSA tier and the regexp compiler run on arm64 only. x86-64 runs the baseline compiler and tier 2, with counted frames;
  it has not been run since the SSA tier became the default.
- Calls are counted, not measured in stack: a call deeper than `Options::stackLimit` frames (512 by default) is a
  `RangeError`. Recursion run in place can take one frame for up to three levels.
- Not compiled by tier 1: functions of more than 20,000 instructions.
- Arrays are dense: a length past what a gap of `JSArray::kMaxDenseGap` allows (`a.length = 2**32 - 1`) ends in
  `OutOfMemory` where the language would give a sparse array.
- Weak collections, `WeakRef` and `FinalizationRegistry` hold their targets for as long as the program can name the
  collection; the registry's callback never runs.
- A parameter list with a default has no separate environment for the body's `var`s (a closure in a default sees a body
  `var` of the same name).
- Direct `eval` `var`s do not reach a name that was resolved to an outer function's variable (see above); `eval` code does not
  check its `var`s against the lexical bindings between it and the variable scope.
- `$262.createRealm` and `$262.agent` do not exist: the vm core has one realm per context.
