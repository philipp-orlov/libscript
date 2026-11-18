#!/usr/bin/env python3
"""Share of the samples in optimizing-tier code that fall on reference counting (arm64 build).

Usage: rc_share.py TOOL SCRIPT [CPU]
Runs SCRIPT under `perf record` with SCRIPT_VM_CODEDUMP, then counts the samples whose instruction is part of a count update
(load count, add or subtract one, store count) or a cell-tag test (`tst xN, x24`), against all samples in tier-2 code."""
import collections, glob, os, re, subprocess, sys, tempfile

tool, script = sys.argv[1], sys.argv[2]
cpu = sys.argv[3] if len(sys.argv) > 3 else "15"
tmp = tempfile.mkdtemp(prefix="rcshare_")
env = dict(os.environ, SCRIPT_CORE="vm", SCRIPT_VM_CODEDUMP=tmp)
data = os.path.join(tmp, "perf.data")
subprocess.run(["taskset", "-c", cpu, "perf", "record", "-o", data, "-q", tool, script], env=env, capture_output=True)
ips = [int(l, 16) for l in subprocess.run(["perf", "script", "-i", data, "-F", "ip"], capture_output=True, text=True).stdout.split()]
funcs = []
for f in glob.glob(os.path.join(tmp, "t2-*@0x*.bin")):
    funcs.append((int(re.search(r"@0x([0-9a-f]+)\.bin", f).group(1), 16), os.path.getsize(f), f))
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
inT2 = rc = 0
for ip in ips:
    for a, sz, f in funcs:
        if a <= ip < a + sz:
            inT2 += 1
            rows, idx = dis(f)
            i = idx.get(ip - a)
            if i is None:
                break
            hit = bool(re.match(r"tst\s+x\d+, x24", rows[i]))
            for k in range(max(0, i - 2), min(len(rows) - 2, i + 1)):
                if (re.match(r"ldr\s+w17, \[x\d+, #8\]", rows[k]) and re.match(r"(adds|subs)\s+w17, w17, #0x1", rows[k + 1])
                        and re.match(r"str\s+w17, \[x\d+, #8\]", rows[k + 2])):
                    hit = True
            rc += hit
            break
print(f"{os.path.basename(script)}: {len(ips)} samples, {100 * inT2 / max(1, len(ips)):.0f}% in tier 2, "
      f"{100 * rc / max(1, inT2):.1f}% of those on reference counting")
