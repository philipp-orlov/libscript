#!/bin/sh
# vsjsc3.sh OUT [NAME...]: jsc (j), the release build (l) and the $VMB build (d), best of two rounds, interleaved, core ${CPU:-16}
OUT=$1; shift; : > $OUT; W=${WL:-/tmp/wl/js3}
J=${JSC:-$(cd "$(dirname "$0")/../../.." && pwd)/build/jsc-compare/jsc-pkg/root/usr/bin/jsc}
L="${*:-$(ls $W/*.js | xargs -n1 basename | sed 's/.js$//' | grep -v node_launcher)}"
for round in 1 2; do for n in $L; do
  for m in j l d; do
    case $m in
      j) r=$(timeout 300 taskset -c ${CPU:-16} $J $W/$n.js 2>/dev/null);;
      l) r=$(timeout 300 taskset -c ${CPU:-16} ${REL:-/tmp/b/rel/tools/libscript} $W/$n.js 2>/dev/null);;
      d) r=$(SCRIPT_CORE=vm timeout 300 taskset -c ${CPU:-16} ${VMB:-/tmp/b/vm}/tools/libscript $W/$n.js 2>/dev/null);;
    esac
    echo "$round $n $m $(echo "$r" | awk '/^RESULT/{s=0;c=0;for(i=4;i<=NF;i++){s+=$i;c++} printf "%.2f %.3f", $3, s/c}')" >> $OUT
  done; done; done
