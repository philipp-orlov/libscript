#!/bin/sh
# binab.sh BIN_A BIN_B ROUNDS OUT [names...]: later and first per workload, best of ROUNDS, interleaved, core ${CPU:-16}
A=$1; B=$2; R=$3; OUT=$4; shift 4; W=${WL:-/tmp/wl/js3}
L="${*:-$(ls $W/*.js | xargs -n1 basename | sed 's/.js$//' | grep -v -e node_launcher -e FlightPlanner -e validatorjs)}"
: > $OUT
for r in $(seq $R); do for n in $L; do for m in A B; do
  if [ $m = A ]; then X=$A; else X=$B; fi
  SCRIPT_CORE=vm timeout 300 taskset -c ${CPU:-16} $X $W/$n.js 2>/dev/null | awk -v n=$n -v m=$m '/^RESULT/{s=0;c=0;for(i=4;i<=NF;i++){s+=$i;c++} printf "%s %s %.2f %.3f\n", n, m, $3, s/c}' >> $OUT
done; done; done
python3 "$(dirname "$0")/absum.py" $OUT
