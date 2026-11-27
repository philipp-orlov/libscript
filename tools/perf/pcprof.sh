#!/bin/sh
# pcprof.sh OUTDIR [NAME...]: profile workloads with pcprof.so (no perf needed) on core ${CPU:-16}; then run
# pcprof.py flat OUTDIR/NAME.pc, pcprof.py jit OUTDIR/NAME.pc or pcprof.py cats OUTDIR [SUMFILE].
D=$1; shift; W=${WL:-/tmp/wl/js3}; mkdir -p $D; H="$(dirname "$0")"
[ -f $D/pcprof.so ] || cc -O2 -shared -fPIC -o $D/pcprof.so $H/pcprof.c || exit 1
L="${*:-$(ls $W/*.js | xargs -n1 basename | sed 's/.js$//' | grep -v -e node_launcher -e FlightPlanner -e validatorjs)}"
for n in $L; do
  rm -f $D/$n.t1map $D/$n.t2map
  SCRIPT_CORE=vm SCRIPT_VM_PERFMAP=1 T1MAP=$D/$n.t1map T2MAP=$D/$n.t2map PCPROF_OUT=$D/$n.pc LD_PRELOAD=$D/pcprof.so \
    taskset -c ${CPU:-16} ${BIN:-${VMB:-/tmp/b/vm}/tools/libscript} $W/$n.js > $D/$n.out 2>&1
  p=$(sed -n 's/^P //p' $D/$n.pc); [ -f /tmp/perf-$p.map ] && mv /tmp/perf-$p.map $D/$n.map
done
