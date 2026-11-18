#!/usr/bin/env python3
"""Samples in optimizing-tier code by bytecode instruction, and how many of them are reference counting (arm64 build).

Usage: rc_by_op.py TOOL SCRIPT [CPU]
Runs SCRIPT under `perf record` with SCRIPT_VM_CODEDUMP and T2MAP (where each instruction's code starts), then prints the
hottest (function line, opcode) pairs, and totals per opcode: samples, of which on count updates and cell-tag tests."""
import bisect, collections, glob, os, re, subprocess, sys, tempfile

tool, script = sys.argv[1], sys.argv[2]
cpu = sys.argv[3] if len(sys.argv) > 3 else "15"
tmp = tempfile.mkdtemp(prefix="rcbyop_")
mapfile = os.path.join(tmp, "map.txt")
env = dict(os.environ, SCRIPT_CORE="vm", SCRIPT_VM_CODEDUMP=tmp, T2MAP=mapfile)
data = os.path.join(tmp, "perf.data")
subprocess.run(["taskset", "-c", cpu, "perf", "record", "-o", data, "-q", tool, script], env=env, capture_output=True)
ips = [int(x, 16) for x in subprocess.run(["perf", "script", "-i", data, "-F", "ip"], capture_output=True, text=True).stdout.split()]
maps = collections.defaultdict(list)
for l in open(mapfile):
    a, _, line, op, off = l.split()
    maps[int(a, 16)].append((int(off), op, int(line)))
sizes = {int(re.search(r"@0x([0-9a-f]+)", f).group(1), 16): (f, os.path.getsize(f)) for f in glob.glob(os.path.join(tmp, "t2-*@0x*.bin"))}
cache = {}
def dis(f):
    if f not in cache:
        out = subprocess.run(["aarch64-linux-gnu-objdump", "-D", "-b", "binary", "-m", "aarch64", f], capture_output=True, text=True).stdout
        rows, idx = [], {}
        for l in out.splitlines():
            m = re.match(r"\s*([0-9a-f]+):\s+[0-9a-f]{8}\s+(.*)", l)
            if m:
                idx[int(m.group(1), 16)] = len(rows)
                rows.append(m.group(2).split("//")[0].strip())
        cache[f] = (rows, idx)
    return cache[f]
tot, rc, n = collections.Counter(), collections.Counter(), 0
for ip in ips:
    for a, (f, sz) in sizes.items():
        if a <= ip < a + sz:
            n += 1
            offs = sorted(maps[a])
            k = bisect.bisect_right([o for o, _, _ in offs], ip - a) - 1
            key = (offs[k][2], offs[k][1]) if k >= 0 else (0, "prologue")
            tot[key] += 1
            rows, idx = dis(f)
            i = idx.get(ip - a)
            if i is not None:
                hit = bool(re.match(r"tst\s+x\d+, x24", rows[i]))
                for j in range(max(0, i - 2), min(len(rows) - 2, i + 1)):
                    if (re.match(r"ldr\s+w17, \[x\d+, #8\]", rows[j]) and re.match(r"(adds|subs)\s+w17, w17, #0x1", rows[j + 1])
                            and re.match(r"str\s+w17, \[x\d+, #8\]", rows[j + 2])):
                        hit = True
                rc[key] += hit
            break
print(f"{n} of {len(ips)} samples in tier 2")
for k, v in tot.most_common(15):
    print(f"  line {k[0]:5d} {k[1]:12s} {v:5d} samples, {rc[k]:4d} on counting")
byop, byrc = collections.Counter(), collections.Counter()
for (l, op), v in tot.items():
    byop[op] += v
    byrc[op] += rc[(l, op)]
print("per opcode:", ", ".join(f"{o} {v}/{byrc[o]}" for o, v in byop.most_common(12)))
