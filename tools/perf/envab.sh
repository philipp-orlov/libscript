#!/bin/sh
# envab.sh "ENV_A" "ENV_B" ROUNDS names...: later and first, best of ROUNDS, interleaved, core ${CPU:-16}, binary ${BIN}
A=$1; B=$2; R=$3; shift 3; W=${WL:-/tmp/wl/js3}; O=$(mktemp)
for n in "$@"; do
  for r in $(seq $R); do for m in A B; do
    if [ $m = A ]; then E=$A; else E=$B; fi
    env SCRIPT_CORE=vm $E timeout 300 taskset -c ${CPU:-16} ${BIN:-${VMB:-/tmp/b/vm}/tools/libscript} $W/$n.js 2>/dev/null | awk -v n=$n -v m=$m '/^RESULT/{s=0;c=0;for(i=4;i<=NF;i++){s+=$i;c++} printf "%s %s %.2f %.3f\n", n, m, $3, s/c}'
  done; done
done > $O
python3 "$(dirname "$0")/absum.py" $O; rm -f $O
