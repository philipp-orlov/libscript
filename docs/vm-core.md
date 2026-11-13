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
until milestone M0. In code, `Context::Options::core = Context::Options::Core::Vm` picks it per context. TypeScript and
Python are refused on it (`NotSupported`); they run on the old core.

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
| `src/vm/builtins.cpp` | the built-in objects so far |
| `tests/vm`, `tests/vm/snippets.inc` | unit tests; programs both cores must agree on |
| `tools/vm_fuzz.py`, `bench/micro` | differential fuzzer; micro-benchmarks that also run under JSC and node |

## Rules the code keeps

- Counts, not a tracing collector. A cell of the new family has a 16-byte header with the count at offset 8; byte 7 has
  its high bit set, which is how `detail::destroyUnreferenced` and the public API tell the families apart. Every register of
  a live frame owns its value.
- A cell is at most 8 KiB (larger storage is out of line), so `LocalHeap::ownerOf` finds the VM from any cell.
- Cells are below 2^47. On a machine whose allocator may return higher addresses this aborts with a message; the address
  hint (LC-1) is still to do.
- Property instructions carry their inline cache in the word after them: own property, prototype property (with a validity
  cell that any change to the chain clears), add-a-property (shape before and after), global.

## Known limits

Not compiled: generators, async functions, tagged templates, regular expressions, BigInt, `with`, private names and
computed instance-field names in classes, `super` and `new.target` inside arrows. Missing built-ins: Symbol, Map, Set,
WeakMap, Promise, Date (only `Date.now`), typed arrays, Proxy, Reflect, `eval`. In a derived constructor `this` exists
before `super()` runs (it is made from `new.target.prototype`), so reading it early gives the object, not a
ReferenceError. Cycles of new-family cells are freed only
when the context ends. Shapes, and the prototypes they hold, are never freed.
