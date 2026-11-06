#!/bin/sh
# The training run for a build configured with -DSCRIPT_PGO=generate: exercises both engines' hot paths on the kernels
# in bench/kernels. usage: tools/pgo-train.sh <path to the libscript tool>
set -eu
tool=$1
root=$(dirname "$0")/..
for f in "$root"/bench/kernels/js/*.js "$root"/bench/kernels/cs/*.cs; do
    "$tool" "$f" >/dev/null
done
