#!/bin/sh
# rss.sh OUT [NAME...]: peak resident size (KiB) of each workload under jsc (j) and the release build (l).
# Not a timing: the workloads run in parallel.
OUT=$1; shift; W=${WL:-/tmp/wl/js3}
J=${JSC:-$(cd "$(dirname "$0")/../../.." && pwd)/build/jsc-compare/jsc-pkg/root/usr/bin/jsc}
L="${*:-$(ls $W/*.js | xargs -n1 basename | sed 's/.js$//' | grep -v -e node_launcher -e FlightPlanner -e validatorjs)}"
T=$(mktemp -d)
for n in $L; do echo $n; done | xargs -P ${JOBS:-8} -I{} sh -c "
  /usr/bin/time -f '{} j %M' -o $T/{}.j $J $W/{}.js > /dev/null 2>&1
  /usr/bin/time -f '{} l %M' -o $T/{}.l ${REL:-/tmp/b/rel/tools/libscript} $W/{}.js > /dev/null 2>&1"
cat $T/*.j $T/*.l | grep -v "^Command" > $OUT; rm -r $T
python3 - $OUT <<'P'
import sys, math, collections
d = collections.defaultdict(dict)
for l in open(sys.argv[1]):
    p = l.split()
    if len(p) == 3: d[p[0]][p[1]] = int(p[2])
r = sorted(((v['l'] / v['j'], n, v) for n, v in d.items() if 'l' in v and 'j' in v), reverse=True)
for x, n, v in r:
    print('%-32s %6.2fx  (%7.1f MiB vs %7.1f MiB)' % (n, x, v['l'] / 1024, v['j'] / 1024))
print('%d geomean of peak resident size, libscript / JSC: %.3f' % (len(r), math.exp(sum(math.log(x) for x, _, _ in r) / len(r))))
P
