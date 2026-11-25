#!/bin/sh
# t2prof.sh NAME OUTDIR: profile one workload on core ${CPU:-16} and write OUTDIR/NAME.prof (see t2prof.py)
N=$1; D=$2; W=${WL:-/tmp/wl/js3}; mkdir -p $D; rm -f $D/$N.t1map $D/$N.t2map
SCRIPT_CORE=vm SCRIPT_VM_PERFMAP=1 T1MAP=$D/$N.t1map T2MAP=$D/$N.t2map perf record -q -e cycles:u -c ${PERIOD:-100000} -o $D/$N.data \
  taskset -c ${CPU:-16} ${BIN:-${VMB:-/tmp/b/vm}/tools/libscript} $W/$N.js > /dev/null 2>&1
python3 "$(dirname "$0")/t2prof.py" $D/$N.data $D/$N.t1map $D/$N.t2map > $D/$N.prof
