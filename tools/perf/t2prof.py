#!/usr/bin/env python3
"""t2prof.py PERF_DATA T1MAP T2MAP [TOP]: what compiled code spends its time on.

Record with t2prof.sh (it sets SCRIPT_VM_PERFMAP=1, T1MAP and T2MAP). Prints:
  - the time share by tier (interpreter, tier 1, SSA, tier 2, runtime, other);
  - inside SSA code: the share by opcode and by n (native) / g (generic) / e (eliminated);
  - the top rows (function, line, bytecode index, opcode, kind);
  - the share of samples in exit stubs and cold paths (past the last instruction's offset).
"""
import bisect
import collections
import re
import subprocess
import sys


def load_map(path):
    """rx -> sorted list of (offset, name, line, op, index, kind)."""
    m = collections.defaultdict(dict)
    try:
        for l in open(path):
            p = l.split()
            if len(p) < 5:
                continue
            rx = int(p[0], 16)
            off = int(p[4])
            idx = int(p[5]) if len(p) > 5 else -1
            kind = p[6] if len(p) > 6 else '-'
            m[rx][off] = (off, p[1], p[2], p[3], idx, kind)
    except FileNotFoundError:
        pass
    return {rx: sorted(v.values()) for rx, v in m.items()}


def main():
    data, t1, t2 = sys.argv[1:4]
    top = int(sys.argv[4]) if len(sys.argv) > 4 else 30
    maps = {'js': load_map(t1), 'js3': load_map(t2)}
    out = subprocess.run(['perf', 'script', '-i', data, '-F', 'pid,ip,period'], capture_output=True, text=True).stdout
    samples = []
    pids = collections.Counter()
    for l in out.splitlines():
        p = l.split()
        if len(p) != 3:
            continue
        pids[p[0]] += 1
        samples.append((int(p[2], 16), int(p[1])))
    pid = pids.most_common(1)[0][0]
    ranges = []
    for l in open('/tmp/perf-%s.map' % pid):
        a, s, n = l.rstrip('\n').split(' ', 2)
        ranges.append((int(a, 16), int(s, 16), n.split(':')[0]))
    ranges.sort()
    starts = [r[0] for r in ranges]
    # Symbols of the samples that are not in compiled code: interpreter or runtime.
    syms = subprocess.run(['perf', 'report', '-i', data, '--stdio', '--sort', 'dso,sym', '-q'],
                          capture_output=True, text=True).stdout
    interp = 0.0
    kernel = 0.0
    for l in syms.splitlines():
        mm = re.match(r'\s*([\d.]+)%\s+(\S+)\s+\[([.k])\]\s+(.*)', l)
        if not mm:
            continue
        if mm.group(3) == 'k':
            kernel += float(mm.group(1))
        elif re.search(r'runImpl', mm.group(4)):
            interp += float(mm.group(1))
    total = sum(w for _, w in samples)
    tier = collections.Counter()
    rows = collections.Counter()
    ops = collections.Counter()
    cold = collections.Counter()
    for ip, w in samples:
        k = bisect.bisect_right(starts, ip) - 1
        if k < 0 or ip >= ranges[k][0] + ranges[k][1]:
            continue
        rx, _, t = ranges[k]
        tier[t] += w
        im = maps.get(t, {}).get(rx)
        if not im:
            continue
        off = ip - rx
        offs = [x[0] for x in im]
        j = bisect.bisect_right(offs, off) - 1
        if j < 0:
            rows[(t, 'prologue', '', -1, '', '')] += w
            continue
        o, name, line, op, idx, kind = im[j]
        rows[(t, name, line, idx, op, kind)] += w
        ops[(t, op, kind)] += w
    # Cold share: code past the end of the last instruction of each function. The last instruction is usually a
    # jump or a return; its body is short, so anything beyond 64 bytes after its start is counted as tails.
    for ip, w in samples:
        k = bisect.bisect_right(starts, ip) - 1
        if k < 0 or ip >= ranges[k][0] + ranges[k][1]:
            continue
        rx, _, t = ranges[k]
        im = maps.get(t, {}).get(rx)
        if im and ip - rx > im[-1][0] + 64:
            cold[t] += w
    pct = lambda v: 100.0 * v / total if total else 0.0
    names = {'js': 'tier 1', 'js2': 'tier 2', 'js3': 'SSA'}
    comp = sum(tier.values())
    print('samples %d' % total)
    print('tiers: interpreter %.1f%%, ' % interp + ', '.join('%s %.1f%%' % (names.get(t, t), pct(v)) for t, v in tier.most_common()) +
          ', runtime and other %.1f%% (kernel %.1f%%)' % (100 - interp - pct(comp) - kernel, kernel))
    for t in ('js3', 'js'):
        if not tier[t]:
            continue
        print('\n%s: cold paths and exit stubs %.1f%% of the tier' % (names[t], 100.0 * cold[t] / tier[t]))
        bykind = collections.Counter()
        for (tt, op, kind), v in ops.items():
            if tt == t:
                bykind[kind] += v
        if t == 'js3':
            print('  by kind: ' + ', '.join('%s %.1f%%' % (k, 100.0 * v / tier[t]) for k, v in bykind.most_common()))
        print('  by opcode (share of the tier):')
        for (tt, op, kind), v in ops.most_common():
            if tt == t and 100.0 * v / tier[t] >= 1.0:
                print('    %-16s %s %5.1f%%' % (op, kind, 100.0 * v / tier[t]))
    print('\ntop rows (share of all samples):')
    for (t, name, line, idx, op, kind), v in rows.most_common(top):
        print('  %5.2f%% %-6s %-24s line %-6s @%-5s %-16s %s' % (pct(v), names.get(t, t), name[:24], line, idx, op, kind))


main()
