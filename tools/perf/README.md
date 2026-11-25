# Performance scripts

Scripts to build, gate and time the vm core against JetStream 3 workloads.
Each script reads its paths from these environment variables:

| Variable | Meaning | Default |
|---|---|---|
| `VMB` | build directory with `SCRIPT_JIT_ENV_OVERRIDE=ON` | `/tmp/b/vm` |
| `WL` | directory of the workload files (`*.js`) | `/tmp/wl/js3` |
| `JSC` | the `jsc` binary | `../build/jsc-compare/jsc-pkg/root/usr/bin/jsc` beside the checkout |
| `CPU` | the core for timing | `16` |

Make the workloads with `tools/probes/jetstream.py`.

| Script | Use |
|---|---|
| `mk.sh` | Build `$VMB`. Fails if the build fails. |
| `gate2.sh` | ctest in 21 configurations (memory modes and tier modes). |
| `fz.sh SEED [N]` | Both fuzzers, both memory modes, two tier settings. |
| `binab.sh BIN_A BIN_B ROUNDS OUT [names]` | A/B of two binaries, interleaved, best of ROUNDS. |
| `envab.sh "ENV_A" "ENV_B" ROUNDS names` | One binary (`BIN`), two environments. |
| `absum.py OUT` | Summary of a `binab.sh` output file. |
| `vsjsc3.sh OUT [names]` | JSC, the release build (`REL`) and the `$VMB` build, two rounds. |
| `vsjsc.py OUT [l\|d]` | Ratios against JSC from a `vsjsc3.sh` output file. |
| `cat.py DIR` | Sort flat `perf report` text files in DIR into categories. |
| `t2prof.py` | Time of compiled code by tier and by instruction (see the script). |

Time only on one fast core with nothing else running.

## Builds

- `$VMB`: `cmake -S libscript -B /tmp/b/vm -DCMAKE_BUILD_TYPE=RelWithDebInfo -DSCRIPT_JS_CORE=both -DSCRIPT_JIT_ENV_OVERRIDE=ON -DSCRIPT_ENABLE_LTO=OFF`.
  Build all targets (`mk.sh`): a partial target does not link the library again.
- `REL`: a Release build (`-O3 -DNDEBUG`) without the environment switches, target `script_cli`. Use it only for the
  comparison with JSC.

## Gate for each commit

1. Build, then run `gate2.sh` (about 25 minutes, all cores). Expect 21 lines of "100% tests passed".
2. Run `fz.sh SEED` (about 6,000 programs). Expect "0 disagreements" six times.
3. For each change of behavior, add a program to `tests/vm/snippets_jit.inc` and compare its output with node.

WARNING: Do not build while a gate or a timing runs. The gate reads the binaries while it runs.

- A snippet cannot use `new Function`: the test harness turns run-time code off.
- A recursion in a snippet must stay below the interpreter's frame limit (`SCRIPT_VM_TIER1=0` runs it there).

## Timing

1. Time on one fast core (15-19 on this machine) with nothing else running.
2. Compare two binaries with `binab.sh`, interleaved. Two rounds over the 41 workloads take about 35 minutes.
3. Read results with the noise in mind. Most workloads repeat within 0.5%. cdjs moves about 5% (use five rounds).
   json-stringify-inspector, richards and ML move 2-3% with code placement alone.
4. Before you blame a change for a rise, count instructions on both binaries:
   `perf stat -e instructions:u,cycles:u`. The same count with more cycles is placement.
5. Keep a change only when it moves a workload past its noise.

## Profiles

- `perf record -e cycles:u --call-graph fp` unwinds through compiled code; `--call-graph dwarf` does not.
- `SCRIPT_VM_PERFMAP=1` names compiled code for perf. `T1MAP` and `T2MAP` name it by function, bytecode index and kind
  (n: in place, g: generic, e: eliminated); `t2prof.sh NAME DIR` uses them.
- `SCRIPT_VM_CODEDUMP=DIR` writes each compiled function (`t1-*` baseline, `t2-*` optimized).
- On arm64, `perf annotate` puts a load's miss on the next instruction.
- To find how often something happens, add a temporary counter that prints at exit, and remove it before the commit.
  `SCRIPT_CENSUS=1` prints the counters that are in the tree (why code enters the runtime).
