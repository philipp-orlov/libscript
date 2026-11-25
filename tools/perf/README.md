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
