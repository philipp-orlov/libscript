#!/bin/sh
# The training run for a build configured with -DSCRIPT_PGO=generate: exercises both engines' hot paths on the kernels
# in bench/kernels, and then on any programs given after the tool. usage: tools/pgo-train.sh <libscript tool> [program...]
#
# The kernels alone are not enough for programs that load and parse a lot of code or lean on the built-ins: a profile of
# kernels marks the front end and the runtime helpers cold, and on JetStream such a build was 4% slower than none (2026-11).
# With the programs of bench/train as well (none of them a JetStream test) it is 2.1% faster on later iterations and 3.1%
# faster on first iterations over the 41 JetStream 3 workloads (2026-11-27). Programs given after the tool add to that.
set -eu
tool=$1
root=$(dirname "$0")/..
for f in "$root"/bench/kernels/js/*.js "$root"/bench/kernels/cs/*.cs; do
    "$tool" "$f" >/dev/null
done
# The front end and the built-ins: programs of bench/train (text compiled at run time, strings, regular expressions,
# collections, generators, promises, classes, BigInt, typed arrays, objects made and dropped).
for f in "$root"/bench/train/js/*.js; do
    "$tool" "$f" >/dev/null
done
# What native code leans on: the helpers it calls and the stubs it goes through.
for f in "$root"/bench/kernels/cs/*.cs "$root"/bench/kernels/c/*.c; do
    "$tool" --native "$f" >/dev/null
done
shift
for f in "$@"; do
    "$tool" "$f" >/dev/null
done
