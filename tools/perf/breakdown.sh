#!/bin/sh
# breakdown.sh OUTDIR [EVENT]: profile each workload once on core ${CPU:-16} (event cycles:u or instructions:u), then
# run breakdown.py OUTDIR SUMFILE for the categories.
D=$1; E=${2:-cycles:u}; W=${WL:-/tmp/wl/js3}; mkdir -p $D
for f in $(ls $W/*.js | grep -v "FlightPlanner\|node_launcher\|validatorjs"); do
  n=$(basename $f .js)
  SCRIPT_CORE=vm SCRIPT_VM_PERFMAP=1 perf record -q -e $E -c 100000 -o $D/$n.data \
    taskset -c ${CPU:-16} ${BIN:-${VMB:-/tmp/b/vm}/tools/libscript} $f > $D/$n.out 2>&1
done
