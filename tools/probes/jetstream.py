#!/usr/bin/env python3
"""Run the JavaScript workloads of JetStream 3 in several engines, each workload as one standalone program.

usage: jetstream.py --jetstream DIR --jsc PATH [--tool PATH] [--engine NAME=COMMAND ...] [--tests a,b,c] [--iterations N] [--cpu N]

JetStream's own driver needs engine features that libscript does not have (a shell with several global objects), so this
script uses JSC once to ask the driver for what it would run (the scripts of each workload, in order) and then runs those
scripts as they are, in each engine, with a small shim that collects the time of every iteration. Workloads that load binary
resources (WebAssembly, large data files) are skipped.

Per workload it reports the first iteration (start-up and warm-up) and the mean of the later ones (peak), in milliseconds, and the
ratio of each engine's time to the baseline engine's. Needs: LD_LIBRARY_PATH set for an unpacked jsc, if that is how it is run.
"""
import argparse, math, os, re, shutil, subprocess, sys, tempfile

SHIM = r"""
var top = { currentResolve: function (r) { __finish(r); }, currentReject: function (e) { __fail(e); } };
var self = this;
if (typeof performance === "undefined") { performance = { now: function () { return Date.now(); } }; }
var __name = %(name)r;
function __out(s) { if (typeof console !== "undefined" && console.log) console.log(s); else print(s); }
function __finish(r) { __out("RESULT\t" + __name + "\t" + Array.prototype.join.call(r, " ")); }
function __fail(e) { __out("FAIL\t" + __name + "\t" + String(e && e.stack || e).split("\n")[0]); }
"""

NODE_LAUNCHER = r"""
const vm = require("vm"), fs = require("fs");
process.on("uncaughtException", e => { console.log("FAIL\t" + process.argv[3] + "\t" + String(e && e.stack || e).split("\n")[0]); });
process.on("unhandledRejection", e => { console.log("FAIL\t" + process.argv[3] + "\t" + String(e && e.stack || e).split("\n")[0]); });
vm.runInThisContext(fs.readFileSync(process.argv[2], "utf8"), { filename: process.argv[2] });
"""


def env_with(extra):
    env = dict(os.environ)
    env.update(extra)
    return env


def dump_workloads(jetstream, jsc, only, iterations, outdir):
    """Ask the driver, run by jsc, for the scripts of each workload; write one program per workload."""
    names = only
    if not names:
        out = subprocess.run([jsc, "cli-dump.js", "--", "--dump-test-list"], cwd=jetstream, capture_output=True, text=True).stdout
        names = [l.strip() for l in out.splitlines() if l.strip() and not l.startswith(("Starting", "Running", "Error", "Using"))]
    programs = {}
    for name in names:
        if "wasm" in name.lower():
            continue
        proc = subprocess.run([jsc, "cli-dump.js", "--", f"--test={name}", f"--iteration-count={iterations}", "--worst-case-count=1"], cwd=jetstream,
                              capture_output=True, text=True, timeout=300)
        text = proc.stdout
        if "@@@BEGIN" not in text:
            print(f"skip {name}: driver gave nothing", file=sys.stderr)
            continue
        body = text.split("@@@BEGIN", 1)[1].split("@@@END", 1)[0]
        scripts = body.split("@@@SCRIPT")[1:]
        joined = "\n;\n".join(scripts)
        preload = re.search(r"preload: \{\s*__proto__: throwOnAccess\(\"JetStream.preload\"\),\s*(.*?)\s*\},\s*resources:", joined, re.S)
        if preload and preload.group(1).strip():
            print(f"skip {name}: loads binary resources", file=sys.stderr)
            continue
        if "read(" in joined and "binary" in joined and "getBinary" in joined and "ShellPrefetchedResources" in joined and "workloads/" in joined and False:
            continue
        path = os.path.join(outdir, name + ".js")
        open(path, "w").write(SHIM % {"name": name} + "\n;\n" + joined)
        programs[name] = path
    return programs


def run_engine(command, program, name, cpu, env, timeout):
    argv = (["taskset", "-c", str(cpu)] if cpu is not None and shutil.which("taskset") else []) + command
    try:
        proc = subprocess.run(argv, capture_output=True, text=True, env=env, timeout=timeout, cwd=os.path.dirname(program))
    except subprocess.TimeoutExpired:
        return None, "timeout"
    for line in proc.stdout.splitlines():
        if line.startswith("RESULT\t"):
            parts = line.split("\t")
            return [float(x) for x in parts[2].split()], None
        if line.startswith("FAIL\t"):
            return None, line.split("\t", 2)[2][:90]
    return None, (proc.stderr.strip().splitlines() or ["no result (exit %d)" % proc.returncode])[-1][:90]


def summarize(times):
    first = times[0]
    rest = times[1:] or times
    return first, sum(rest) / len(rest)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--jetstream", required=True)
    ap.add_argument("--jsc", required=True)
    ap.add_argument("--tool")
    ap.add_argument("--engine", action="append", default=[])
    ap.add_argument("--tests", default="")
    ap.add_argument("--iterations", type=int, default=20)
    ap.add_argument("--cpu", type=int)
    ap.add_argument("--timeout", type=int, default=900)
    args = ap.parse_args()

    outdir = tempfile.mkdtemp(prefix="js3_")
    jetstream = os.path.abspath(args.jetstream)
    programs = dump_workloads(jetstream, args.jsc, [t for t in args.tests.split(",") if t], args.iterations, outdir)
    launcher = os.path.join(outdir, "node_launcher.js")
    open(launcher, "w").write(NODE_LAUNCHER)

    engines = []  # (name, function(program, name) -> command, env)
    jsc = args.jsc
    if args.tool:
        engines.append(("libscript", lambda p, n: [args.tool, p], env_with({"SCRIPT_CORE": "vm"})))
    engines.append(("jsc", lambda p, n: [jsc, p], os.environ))
    if shutil.which("node"):
        engines.append(("node", lambda p, n: ["node", launcher, p, n], os.environ))
    for spec in args.engine:
        name, _, command = spec.partition("=")
        engines.append((name, lambda p, n, c=command.split(): c + [p], os.environ))

    results = {}  # name -> engine -> (first, mean) or error text
    for name, path in sorted(programs.items()):
        results[name] = {}
        for ename, command, env in engines:
            times, err = run_engine(command(path, name), path, name, args.cpu, env, args.timeout)
            results[name][ename] = summarize(times) if times else err
        print(f"done {name}", file=sys.stderr, flush=True)

    base = "jsc"
    names = [e for e, _, _ in engines]
    print(f"{'workload':34} " + " ".join(f"{e + ' first':>14} {e + ' mean':>13}" for e in names))
    peak = {e: [] for e in names}
    first = {e: [] for e in names}
    for name in sorted(results):
        row = f"{name:34} "
        for e in names:
            r = results[name][e]
            row += f"{'-':>14} {r[:13]:>13} " if isinstance(r, str) else f"{r[0]:14.1f} {r[1]:13.2f} "
        print(row)
        b = results[name].get(base)
        if isinstance(b, tuple):
            for e in names:
                r = results[name][e]
                if isinstance(r, tuple):
                    first[e].append(r[0] / b[0])
                    peak[e].append(r[1] / b[1])
    print()
    for e in names:
        if e == base or not peak[e]:
            continue
        gm = lambda xs: math.exp(sum(map(math.log, xs)) / len(xs))
        print(f"{e} vs {base} over {len(peak[e])} workloads: first iteration {gm(first[e]):.2f}x, later iterations {gm(peak[e]):.2f}x "
              f"(time ratio; above 1 is slower)")
    for name in sorted(results):
        for e in names:
            if isinstance(results[name][e], str):
                print(f"  {name} / {e}: {results[name][e]}")


main()
