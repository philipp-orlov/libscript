#!/bin/sh
# soak.sh NAME [ITERATIONS]: run workload NAME for ITERATIONS (default 300) with a memory line per iteration
# (`memoryReport` of the command line tool), and print the growth of each figure after iteration 50.
# Not a timing. BIN: the binary (default: the release build).
n=$1; N=${2:-300}; W=${WL:-/tmp/wl/js3}; D=${SOAK:-/tmp/wl/soak}; mkdir -p $D
sed -e "s/\"iterationCount\":[0-9]*/\"iterationCount\":$N/" -e "s/for (let i = 0; i < [0-9]*; i++) {/for (let i = 0; i < $N; i++) { memoryReport(i);/" \
    -e "s/validate?.([0-9]*)/validate?.($N)/" $W/$n.js > $D/$n.js
SCRIPT_CORE=vm ${BIN:-${REL:-/tmp/b/rel/tools/libscript}} $D/$n.js 2> $D/$n.mem > /dev/null
python3 - $n $D/$n.mem <<'P'
import sys
rows = [l.split() for l in open(sys.argv[2]) if l.startswith('MEM ')]
keys = ['rss', 'heap', 'objects', 'strings', 'reserved', 'code', 'malloc', 'mfree']
v = [{k: int(r[r.index(k) + 1]) for k in keys} for r in rows]
if len(v) < 60:
    print('%-30s too few iterations (%d)' % (sys.argv[1], len(v))); sys.exit(1)
a, b = v[50], v[-1]
out = ' '.join('%s %+.1f%%' % (k, 100.0 * (b[k] - a[k]) / a[k] if a[k] else 0) for k in keys)
print('%-30s rss %6.1f MiB at 50, %6.1f MiB at %d (malloc in use %.1f, free %.1f) | %s' % (sys.argv[1], a['rss'] / 2**20, b['rss'] / 2**20, len(v) - 1, b['malloc'] / 2**20, b['mfree'] / 2**20, out))
P
