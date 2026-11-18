#!/usr/bin/env python3
"""Compare engines on the probes of probes.js: the same program, the best of three timed runs each, checksums checked.

usage: run.py [--tool PATH_TO_LIBSCRIPT] [--engine NAME=COMMAND ...] [--cpu N] [--only SUBSTRING]

Engines default to libscript (with SCRIPT_CORE=vm when it was built with SCRIPT_JIT_ENV_OVERRIDE) and `node` when on PATH. Add
`--engine jsc=/path/to/jsc` for JavaScriptCore; the first engine named is the one under test: each ratio is its time over the other engine's, so above 1 it is slower.
Pin to a fast core with --cpu (taskset), as the machine mixes core types.
"""
import argparse, math, os, shutil, subprocess, sys, tempfile

here = os.path.dirname(os.path.abspath(__file__))


def run_engine(name, command, program, cpu, env):
    argv = (["taskset", "-c", str(cpu)] if cpu is not None and shutil.which("taskset") else []) + command + [program]
    proc = subprocess.run(argv, capture_output=True, text=True, env=env)
    results = {}
    for line in proc.stdout.splitlines():
        parts = line.split("\t")
        if len(parts) == 4:
            results[parts[0]] = (parts[1], parts[2], float(parts[3]))
    if proc.returncode != 0 or not results:
        sys.exit(f"{name} failed ({proc.returncode}):\n{proc.stderr[-2000:]}")
    return results


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tool", help="the libscript command-line tool")
    ap.add_argument("--engine", action="append", default=[], help="NAME=COMMAND of another engine, e.g. jsc=/usr/bin/jsc")
    ap.add_argument("--cpu", type=int, default=None)
    ap.add_argument("--only", default="")
    args = ap.parse_args()

    engines = []  # (name, command, env)
    if args.tool:
        env = dict(os.environ, SCRIPT_CORE="vm")
        engines.append(("libscript", [args.tool], env))
    for spec in args.engine:
        name, _, command = spec.partition("=")
        engines.append((name, command.split(), os.environ))
    if shutil.which("node") and not any(n == "node" for n, _, _ in engines):
        engines.append(("node", ["node"], os.environ))
    if len(engines) < 2:
        sys.exit("need at least two engines (--tool and node on PATH, or --engine)")

    source = open(os.path.join(here, "probes.js")).read() + open(os.path.join(here, "harness.js")).read()
    if args.only:
        source = source.replace("globalThis.__probes = probes;", "globalThis.__probes = probes.filter(p => p.name.includes(%r));" % args.only)
    with tempfile.TemporaryDirectory() as d:
        program = os.path.join(d, "probes_all.js")
        open(program, "w").write(source)
        results = [(n, run_engine(n, c, program, args.cpu, e)) for n, c, e in engines]

    test_name, test = results[0]
    bases = results[1:]
    header = f"{'probe':52} {'group':11} {test_name:>10} " + " ".join(f"{n:>10} {test_name[:3] + '/' + n[:4]:>9}" for n, _ in bases)
    print(header)
    ratios = {n: [] for n, _ in bases}
    groups = {}
    bad = 0
    for name in test:
        group, checksum, ms = test[name]
        row = f"{name[:52]:52} {group:11} {ms:10.1f} "
        for n, res in bases:
            if name not in res:
                row += f"{'-':>10} {'-':>9} "
                continue
            g, c, t = res[name]
            ok = c == checksum
            bad += 0 if ok else 1
            ratio = ms / t if t > 0 else float("inf")  # above 1: the engine under test is slower
            if t >= 1.0:
                ratios[n].append(ratio)
                groups.setdefault((n, group), []).append(ratio)
            row += f"{t:10.1f} {ratio:8.2f}x{'' if ok else '!'}"
        print(row)
    print()
    for n, _ in bases:
        r = ratios[n]
        if r:
            print(f"{test_name} vs {n}: geometric mean {math.exp(sum(map(math.log, r)) / len(r)):.2f}x slower over {len(r)} probes "
                  f"(median {sorted(r)[len(r) // 2]:.2f}x, worst {max(r):.2f}x)")
        for (gn, group), rs in sorted(groups.items()):
            if gn == n:
                print(f"  {group:11} {math.exp(sum(map(math.log, rs)) / len(rs)):6.2f}x  ({len(rs)})")
    if bad:
        print(f"\n{bad} checksum(s) differ (marked !): those probes do not compute the same thing in both engines")


main()
