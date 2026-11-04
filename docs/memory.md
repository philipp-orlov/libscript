# Memory

The engine is built for processes that run for weeks. That rules out two
things: a stop-the-world pause proportional to the heap, and an allocator whose
free list slowly shreds. Neither happens here, and it happens once: both
engines allocate from one heap, hang their cells on one list, and are swept by
one collector.

## Reference counting first

Every heap cell -- a string, a C# instance or array, a JavaScript object,
function or symbol, a box a closure captured -- carries a count, and
`script::Value` maintains it on copy and destruction. When the last handle
goes away the cell is freed immediately: no pause, no delay, no growth in the
meantime. That covers the overwhelming majority of what a program allocates.

This is also what makes the C++ API safe without a handle scope. A `Value`
held in a member variable, a `std::vector` or a lambda capture is a real
reference; nothing can collect what it points at.

Releasing a long chain would recurse as deep as the chain, so it does not: a
cell whose count hits zero while another is being destroyed is threaded onto a
pending list and freed by the loop that is already running. Dropping a
300,000-node chain is a loop and costs no extra memory.

## A cycle collector for the rest

Object graphs have cycles -- a parent pointing at a child pointing back, a
delegate holding the object that holds the delegate, `state.fn = () => state`
-- and refcounting cannot see them. `ContextImpl::collectCycles` does, with
trial deletion over the one intrusive list every cell hangs on:

1. copy each cell's count, then subtract every reference the heap itself
   holds;
2. anything with a count left over is reachable from outside -- a `Value` on
   the C++ stack, the global object, a live frame of either engine, the job
   queue, an assembly's literals; mark from there;
3. what is unmarked is a cycle nothing can reach -- pin it, cut its edges, and
   let refcounting finish the job;
4. sweep the string table, then let each engine sweep its own: the dynamic
   engine drops registered symbols nothing holds, and both trim their frame
   pools;
5. return pool arenas that no longer hold a live cell.

The walk is proportional to the number of live cells, not to the size of the
heap or the age of the process, and it runs when a budget is crossed:
`Options::gcThreshold` bytes of growth, or a cell count, whichever comes first.
Both budgets are re-armed from what survived, so a program that legitimately
grows is not punished with ever more frequent collections, and one that churns
in place collects at a fixed rate however long it runs.

`Context::collectGarbage()` forces a collection. Most programs never need to
call it; a host with an idle point may call it there to hand back arenas after
a spike.

## Cells come from a heap of their own

Each context allocates its cells from one `mem::LocalHeap` (libcore). Mappings
of 2 MiB are cut into 64 KiB blocks, a block serves one size class -- 16-byte
steps up to 512 bytes, four steps per doubling up to 8 KiB -- and the block a
cell belongs to is a mask away. Each block has its own free list and a class
fills one block at a time, so allocation concentrates on blocks already in use
while the rest drain. A block that drains goes back at once, for any size class
to take. That is the property a free list threaded across blocks cannot have,
and it is why a burst of which a little survives does not shred the heap.

Freeing never makes a system call. After a collection `LocalHeap::trim` hands
memory to the OS: mappings with nothing live in them go, and idle blocks past a
reserve of sixteen blocks (or an eighth of the blocks in use, if more) give
their pages back while keeping the address range. It costs the number of partly
free mappings, not the number of objects.

The consequence is the one that matters: a process that allocates and frees
millions of cells reaches a steady state, whatever mix of languages made them.
The soak suites (`tests/csharp/test_soak.cpp`, `tests/typescript/test_soak.cpp`,
`tests/javascript/test_soak.cpp`) assert exactly that: after a warm-up they
repeat a mixed workload hundreds of times and check that the object count, byte
count, string count, heap reservation and the process's own resident size end
where they started.

## Buffers stay out of the process heap

Everything variable-length a cell or an interpreter holds -- a property table,
an array's elements, a `List<T>`'s items, a `Map`'s index, a frame's stack, a
collector's worklist, a temporary `String` -- comes from `internal::payloadPool`,
through `Vec<T>` and `String`: a `mem::SizeClassPool` of slabs per size class,
with a cache per thread, shared by the contexts of a process. Those buffers are
single contiguous blocks: property tables, `Dictionary<K,V>` and `Map`/`Set`
indexes are flat, open-addressed arrays whose tombstones are compacted once they
outnumber the living, so a table used as a queue does not grow for ever.
Containers that shrink well below their capacity hand it back. Buffers above
16 KiB, and cells above 8 KiB, are mappings of their own whose idle pages go back
to the OS; a collection ages them out.

A native function reads its arguments as a `std::vector`, because that is the
public API. A vector per call would be the hottest allocation there is, so
`ContextImpl::ArgBuffer` keeps one per nesting level and reuses it.

So in steady state the engine makes **no call to the general allocator at all**,
in any language, interpreted or compiled: `tests/core/test_steady_state.cpp`
counts `operator new` over a mixed workload and requires zero. A general allocator
that is never called in steady state cannot fragment in steady state.

Compiling is the exception, and deliberately so: tokens and tree nodes own
`std::vector`s, and what a compilation leaves behind (metadata, bytecode) lives
as long as its assembly. A host that compiles new programs for ever does call
`malloc`, in proportion to what it compiles and returning it all; see
[limitations.md](limitations.md).

## Growth accounting

Cells are charged when they are allocated, and a container that keeps growing
afterwards -- an array being pushed to in a loop, a property table filling up,
a `StringBuilder` -- charges the difference as it reallocates
(`GCObject::chargeGrowth`), so a loop that only ever grows one object still
reaches the collector and the memory limit. The accounting is recomputed
exactly during each collection, so it cannot drift.

## Strings

Strings are UTF-16, immutable, and one allocation: the characters follow the
header in the cell. The typed engine interns short ones (32 characters or fewer),
which is most names and most keys; the dynamic engine interns property names and
leaves other strings uninterned, because it compares them by content. Both use
the one table, a flat open-addressed array of cell pointers looked up by content
without building a key, with no node per entry and no tombstones. It holds one
reference; a string nothing else names has a count of exactly one, and
`sweepStrings` drops it during collection and sizes the table down. That is what
stops a program building strings or property names at run time from growing the
table for the life of the process.

Joining two strings writes the result straight into its cell (`ContextImpl::concat`).
A hash is computed the first time something asks, so a string that is only ever
built and printed never pays for one.

## Frames

Both interpreters keep one reusable frame per call depth and reuse the buffers
the previous call at that depth left behind, so a call allocates nothing in
steady state. The call depth itself is one counter in the context, shared by
both engines, and `Options::stackLimit` bounds it; a spike in depth is trimmed
back at the next collection.

## The compilers' memory

The C# parser and checker, and the C and C++ front ends, allocate tree nodes from
a `mem::ObjectArena`, drawing 64 KiB chunks from libcore's process-wide pool: no
node is ever released on its own, destructors run newest first when the
compilation ends, and the chunks go back in one go. The TypeScript checker's type
arena works the same way. Compiling touches the object heap only to intern the
literals the program contains and, for the dynamic languages, to build the
`FunctionCode` the program's closures will share, which is released with the last
closure over it. Checking a million programs frees every one of them in a million
bulk releases.

## What a compilation leaves behind

A dynamic program's code goes with its last closure. A typed unit is different:
names are global across units, so a unit that declares a type stays for as long
as the context does, the same way a top-level JavaScript declaration stays on the
global object. Generating new class names for ever therefore grows for ever, and
that is what the language says it should do.

A unit that is only top-level code declares nothing a later unit could name, so
it goes once nothing refers to it: no `Assembly` handle, no live delegate or
instance, no other unit. `Runtime::unloadUnusedUnits` checks during a collection,
and compiling starts a collection every few hundred such units, because evaluating
top-level code in a loop allocates too little to start one otherwise. So
`ctx.eval("return Env.Now();")` can run in a loop for as long as the process lives.

## Engine runtimes

An engine's runtime -- the `System.*` type table for C#, the intrinsic
prototypes and the global object for the ECMAScript languages -- is created
the first time a language that needs it is used, and only then. A context that
runs one language pays for one engine. The first evaluation in a language
therefore shows a one-time step in `liveObjects` (about 230 objects for the
JavaScript intrinsics); measure after it.

## Layout

A C# object is one allocation: `CSInstance` carries its field slots after its
header, and the slot index was decided at compile time. Arrays of primitives
store raw elements, not `Value`s: `int[1000000]` costs four megabytes, not
sixteen. A JavaScript object is its header, a flat property vector and, past
eight properties, a flat index. A host wrapper (`NativeCell`) is one small
cell whichever language reads it.

## Budgets

```cpp
script::Context::Options options;
options.memoryLimit = 64u << 20;   // bytes; 0 means unbounded
options.stackLimit  = 512;         // maximum call depth, across every language
options.gcThreshold = 4u << 20;    // heap growth between collections
script::Context ctx(options);
```

`memoryLimit` is checked on allocation. Crossing it triggers a collection
first; if the heap is still over budget, the allocation raises
`ErrorCode::OutOfMemory`. `stackLimit` raises `ErrorCode::StackOverflow`.
Neither can be caught by a program -- a `catch` around a runaway loop would
otherwise swallow the very thing trying to stop it -- so both unwind straight
to the host.

## What to expect

| Operation | Cost |
| --- | --- |
| C# field read/write | one indexed load/store |
| C# virtual call | one vtable indirection |
| C# interface call | a cached lookup on (concrete type, interface method) |
| JavaScript property read | a pointer-compared scan of a small table, or one hash probe, per prototype level |
| `new` | a free-list pop, plus zeroing the slots |
| acyclic garbage | freed at the last release |
| cyclic garbage | one walk of the live set, when a budget is crossed |
| a call, either engine | no allocation in steady state |

## Native code

Compiled C# methods are a separate budget from the object heap: 64 KiB
mappings that hold several methods each, capped by `JitOptions::codeCacheLimit`
and reported as `Stats::nativeCodeBytes`. Generated code does not allocate and
does not touch a reference count, so turning the JIT on changes nothing about
the numbers above.

## Over a long run

`bench/soak_mixed.cpp` is the long run itself (`-DSCRIPT_BUILD_BENCHMARKS=ON`,
`SCRIPT_SOAK_SECONDS=86400`): one context per thread in each language, bursts,
programs compiled from source all along, with the resident size sampled and
fitted. `bench/bench_longrun.cpp` reports time and heap calls per round.

[examples/csharp/longrun.cpp](../examples/csharp/longrun.cpp) runs a mixed
C# workload for thousands of rounds while the heap, the pool and the process's
resident set are sampled, and shows the heap oscillating between collections
and returning to the same band every time. `tests/core/test_polyglot.cpp`
checks the same property with both engines allocating into the one heap: after
a C# program and a JavaScript program each build and drop a thousand objects,
a collection leaves the object count where it started.

If a context does grow without bound, the usual causes are host-side: a
`Value` member that is never reassigned, a handler table a script keeps
appending to, top-level JavaScript declarations that live on the global object
by design, or promises that are created but never drained with `runJobs`.
`stats()` distinguishes them -- `liveObjects` climbing with `pendingJobs` at
zero points at retained values, the other way round at an undrained queue.
