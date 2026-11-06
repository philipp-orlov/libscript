#!/usr/bin/env python3
"""Differential fuzzing of the native tier: random C# programs, interpreted and compiled, must print the same thing.

usage: jit_fuzz.py <libscript tool> [count] [seed]
Prints each disagreement and keeps the program. Exits non-zero if there was any.
"""
import os, random, subprocess, sys, tempfile
from concurrent.futures import ThreadPoolExecutor

class Gen:
    def __init__(self, seed):
        self.r = random.Random(seed)
        self.classes = ['A', 'B', 'C']
        self.depth = 0
        self.nextVar = 0

    def pick(self, *xs): return self.r.choice(xs)

    def intConst(self):
        return self.pick('0', '1', '-1', '2', '3', '7', '31', '100', '-100', '65535', '2147483647', '(-2147483647 - 1)',
                         str(self.r.randint(-1000, 1000)))
    def longConst(self):
        return self.pick('0L', '1L', '-1L', '4294967296L', '9223372036854775807L', str(self.r.randint(-10**12, 10**12)) + 'L')
    def dblConst(self):
        return self.pick('0.0', '1.0', '-1.0', '0.5', '3.25', '1e300', '-1e-300', '1234.5678', str(round(self.r.uniform(-100, 100), 3)))

    def intExpr(self, v, d=3):
        ints = v['int']
        if d <= 0 or self.r.random() < 0.2:
            return self.r.choice(ints) if ints and self.r.random() < 0.75 else self.intConst()
        k = self.r.randint(0, 17)
        a = lambda: self.intExpr(v, d - 1)
        if k <= 3: return f"({a()} {self.pick('+','-','*')} {a()})"
        if k == 4: return f"({a()} {self.pick('&','|','^')} {a()})"
        if k == 5: return f"({a()} {self.pick('<<','>>')} ({a()} & 31))"
        if k == 6: return f"(({a()} | 1) == 0 ? 0 : {a()} {self.pick('/','%')} ({a()} | 1))"
        if k == 7: return f"(int)({self.longExpr(v, d - 1)})"
        if k == 8: return f"(int)({self.dblExpr(v, d - 1)})"
        if k == 9: return f"({self.boolExpr(v, d - 1)} ? {a()} : {a()})"
        if k == 10: return f"(- {a()})"
        if k == 11 and v['arr']: return f"{self.r.choice(v['arr'])}[({a()}) & 7]"
        if k == 12 and v['obj']: return f"{self.r.choice(v['obj'])}.I"
        if k == 13 and v['obj']: return f"{self.r.choice(v['obj'])}.Virt({a()})"
        if k == 14: return f"H.F{self.r.randint(0,3)}({a()}, {a()})"
        if k == 15: return f"(int)(byte)({a()})"
        if k == 16 and v['str']: return f"{self.r.choice(v['str'])}.Length"
        if k == 17 and v['fn']: return f"{self.r.choice(v['fn'])}({a()})"
        return a()

    def longExpr(self, v, d):
        if d <= 0 or self.r.random() < 0.3:
            return self.r.choice(v['long']) if v['long'] and self.r.random() < 0.7 else self.longConst()
        k = self.r.randint(0, 4)
        a = lambda: self.longExpr(v, d - 1)
        if k <= 1: return f"({a()} {self.pick('+','-','*','&','^')} {a()})"
        if k == 2: return f"((long){self.intExpr(v, d - 1)})"
        if k == 3: return f"({a()} {self.pick('<<','>>')} ({self.intExpr(v, d - 1)} & 63))"
        return f"((ulong)({a()}) > 5UL ? {a()} : {a()})"

    def dblExpr(self, v, d):
        if d <= 0 or self.r.random() < 0.3:
            return self.r.choice(v['dbl']) if v['dbl'] and self.r.random() < 0.7 else self.dblConst()
        k = self.r.randint(0, 5)
        a = lambda: self.dblExpr(v, d - 1)
        if k <= 1: return f"({a()} {self.pick('+','-','*','/')} {a()})"
        if k == 2: return f"((double){self.intExpr(v, d - 1)})"
        if k == 3: return f"Math.{self.pick('Sqrt','Abs','Floor','Ceiling','Truncate')}({a()})"
        if k == 4 and v['obj']: return f"{self.r.choice(v['obj'])}.D"
        return f"(- {a()})"

    def boolExpr(self, v, d):
        k = self.r.randint(0, 5)
        if k <= 1: return f"({self.intExpr(v, d)} {self.pick('<','<=','>','>=','==','!=')} {self.intExpr(v, d)})"
        if k == 2: return f"({self.dblExpr(v, d)} {self.pick('<','<=','>','>=','==','!=')} {self.dblExpr(v, d)})"
        if k == 3 and d > 0: return f"({self.boolExpr(v, d - 1)} {self.pick('&&','||')} {self.boolExpr(v, d - 1)})"
        if k == 4 and d > 0: return f"(!{self.boolExpr(v, d - 1)})"
        if k == 5 and v['obj']: return f"({self.r.choice(v['obj'])} {self.pick('==','!=')} null)"
        return f"({self.longExpr(v, d)} {self.pick('<','==','>')} {self.longExpr(v, d)})"

    def newVar(self):
        self.nextVar += 1
        return f"v{self.nextVar}"

    def stmts(self, v, n, ind, inLoop=False):
        out = []
        v = {k: list(x) for k, x in v.items()}
        for _ in range(n):
            out += self.stmt(v, ind, inLoop)
        return out

    def stmt(self, v, ind, inLoop):
        p = '    ' * ind
        k = self.r.randint(0, 21)
        self.depth += 1
        deep = self.depth > 3
        try:
            if k == 0:
                x = self.newVar(); e = self.intExpr(v); v['int'].append(x)
                return [f"{p}int {x} = {e};"]
            if k == 1:
                x = self.newVar(); e = self.longExpr(v, 3); v['long'].append(x)
                return [f"{p}long {x} = {e};"]
            if k == 2:
                x = self.newVar(); e = self.dblExpr(v, 3); v['dbl'].append(x)
                return [f"{p}double {x} = {e};"]
            if k == 3 and v['mut']:
                return [f"{p}{self.r.choice(v['mut'])} {self.pick('=','+=','-=','^=')} {self.intExpr(v)};"]
            if k == 4 and not deep:
                out = [f"{p}if ({self.boolExpr(v, 2)}) {{"] + self.stmts(v, 2, ind + 1, inLoop) + [f"{p}}} else {{"] + self.stmts(v, 2, ind + 1, inLoop) + [f"{p}}}"]
                return out
            if k == 5 and not deep:
                i = self.newVar(); w = {key: list(x) for key, x in v.items()}; w['int'].append(i)
                return [f"{p}for (int {i} = 0; {i} < {self.r.randint(0, 12)}; {i}++) {{"] + self.stmts(w, 3, ind + 1, True) + [f"{p}}}"]
            if k == 6:
                x = self.newVar(); e = self.intExpr(v); v['int'].append(x); v['mut'].append(x)
                return [f"{p}int {x} = {e};"]
            if k == 7:
                x = self.newVar(); e = self.intExpr(v); v['arr'].append(x)
                return [f"{p}int[] {x} = new int[8];", f"{p}{x}[{self.r.randint(0,7)}] = {e};"]
            if k == 8 and v['arr']:
                a = self.r.choice(v['arr'])
                return [f"{p}{a}[({self.intExpr(v)}) & 7] {self.pick('=','+=','^=')} {self.intExpr(v)};"]
            if k == 9:
                x = self.newVar(); c = self.r.choice(self.classes); e = self.intExpr(v); g = self.dblExpr(v, 2); v['obj'].append(x)
                return [f"{p}A {x} = new {c}({e}, {g});"]
            if k == 10 and v['obj']:
                o = self.r.choice(v['obj'])
                return [f"{p}{o}.I {self.pick('=','+=','*=')} {self.intExpr(v)};", f"{p}{o}.D = {self.dblExpr(v, 2)};"]
            if k == 11 and v['obj']:
                o = self.r.choice(v['obj']); q = self.r.choice(v['obj'])
                return [f"{p}{o}.Next = {q};", f"{p}Log({o}.Next == null ? 0 : {o}.Next.I);"]
            if k == 12 and not deep:
                # exceptions: the fault may be real
                x = self.newVar()
                body = [f"{p}    int {x} = {self.intExpr(v)};", f"{p}    if (({x} & 3) == 0) throw new Exception(\"t\" + ({x} & 15));"]
                fault = self.pick("", f"{p}    Log(new int[2][{x} & 3]);", f"{p}    A n{x} = null; if (({x} & 1) == 0) Log(n{x}.I);", f"{p}    Log(H.F0({x}, {x}) / ({x} & 1));")
                if fault: body.append(fault)
                return [f"{p}try {{"] + body + [f"{p}}} catch (Exception e{x}) {{", f"{p}    Log(e{x}.Message.Length);", f"{p}}} finally {{", f"{p}    Log(1);", f"{p}}}"]
            if k == 13:
                x = self.newVar(); e = self.intExpr(v); v['str'].append(x)
                return [f"{p}string {x} = \"s\" + ({e});", f"{p}Log({x}.Length);"]
            if k == 14 and v['str']:
                s = self.r.choice(v['str'])
                return [f"{p}Log(({s} + \"x\").Length + (int)({s} + \"yy\")[0] + ({s} == \"s1\" ? 1 : 0));"]
            if k == 15:
                x = self.newVar(); o = self.newVar(); e = self.intExpr(v); v['fn'].append(x)
                return [f"{p}int {o} = {e};", f"{p}Func<int, int> {x} = q => q + {o} * 3;", f"{p}{o} = {o} ^ 5;", f"{p}Log({x}(2));"]
            if k == 16:
                x = self.newVar()
                return [f"{p}List<int> {x} = new List<int>();", f"{p}{x}.Add({self.intExpr(v)});", f"{p}{x}.Add(7);", f"{p}{x}[0] = {x}[0] + {x}[1];", f"{p}Log({x}[0] + {x}.Count);"]
            if k == 17:
                x = self.newVar()
                return [f"{p}Dictionary<string, int> {x} = new Dictionary<string, int>();", f"{p}{x}[\"k\" + (({self.intExpr(v)}) & 3)] = 1;", f"{p}{x}[\"k1\"] = 5;", f"{p}Log({x}[\"k1\"] + ({x}.ContainsKey(\"k2\") ? 1 : 0) + {x}.Count);"]
            if k == 18:
                x = self.newVar()
                return [f"{p}object {x} = {self.pick(self.intExpr(v), self.dblExpr(v, 1), chr(34)+'str'+chr(34), 'null', 'new B(1, 2.0)')};", f"{p}Log({x} == null ? 9 : ({x} is int ? 1 : 2));", f"{p}Log(\"\" + {x});"]
            if k == 19 and inLoop:
                return [f"{p}if ({self.boolExpr(v, 1)}) {self.pick('break','continue')};"]
            if k == 20:
                x = self.newVar(); y = self.newVar()
                return [f"{p}int {x} = {self.intExpr(v)};", f"{p}H.Inc(ref {x});", f"{p}int {y};", f"{p}H.Out(out {y}, {x});", f"{p}Log({x} + {y});"]
            return [f"{p}Log({self.pick(self.intExpr(v), self.longExpr(v, 2), self.dblExpr(v, 2))});"]
        finally:
            self.depth -= 1

    def program(self):
        v = dict(int=['a', 'b'], long=[], dbl=[], arr=[], obj=[], str=[], fn=[], mut=['a', 'b'])
        body = self.stmts(v, self.r.randint(6, 18), 2)
        helpers = []
        for i in range(4):
            w = dict(int=['x', 'y'], long=[], dbl=[], arr=[], obj=[], str=[], fn=[], mut=[])
            e = self.intExpr(w, 2).replace('H.F', 'H.G')
            helpers.append(f"    public static int F{i}(int x, int y) {{ return {e}; }}")
            helpers.append(f"    public static int G{i}(int x, int y) {{ return x {self.pick('+','-','^')} y; }}")
        return f"""using System;
using System.Collections.Generic;
class A {{
    public int I; public double D; public A Next;
    public A(int i, double d) {{ I = i; D = d; }}
    public virtual int Virt(int x) {{ return I + x; }}
}}
class B : A {{
    public B(int i, double d) : base(i, d) {{ }}
    public override int Virt(int x) {{ return I * 3 - x; }}
}}
class C : B {{
    public C(int i, double d) : base(i + 1, d) {{ }}
    public override int Virt(int x) {{ return base.Virt(x) + 1; }}
}}
static class H {{
{chr(10).join(helpers)}
    public static void Inc(ref int x) {{ x = x + 1; }}
    public static void Out(out int x, int y) {{ x = y * 2; }}
}}
static class P {{
    static long sum = 0;
    static void Log(long x) {{ sum = sum * 31 + x; Console.WriteLine(x); }}
    static void Log(double x) {{ Console.WriteLine(x); }}
    static void Log(string x) {{ Console.WriteLine(x); }}
    public static int Run(int a, int b) {{
{chr(10).join(body)}
        return a ^ b;
    }}
    public static long Sum() {{ return sum; }}
}}
for (int i = 0; i < 3; i++) {{
    try {{ Console.WriteLine(P.Run(i, 7 - i)); }} catch (Exception e) {{ Console.WriteLine("caught " + e.Message); }}
}}
Console.WriteLine(P.Sum());
"""

def run(tool, flags, path):
    env = dict(os.environ)
    try:
        p = subprocess.run([tool] + flags + [path], capture_output=True, text=True, timeout=30, env=env)
    except subprocess.TimeoutExpired:
        return 'timeout', ''
    return p.returncode, p.stdout + p.stderr

def one(args):
    tool, seed, dirname = args
    source = Gen(seed).program()
    path = os.path.join(dirname, f"f{seed}.cs")
    with open(path, 'w') as f:
        f.write(source)
    base = run(tool, ['--interpret'], path)
    # A program the language rejects says nothing about the back end.
    if base[0] != 0 and ('error' in base[1][:200].lower() and 'exception' not in base[1][:200].lower()):
        os.unlink(path)
        return None
    bad = None
    for flags in (['--native'], ['--jit']):
        got = run(tool, flags, path)
        if got != base:
            bad = (seed, flags, path, base, got)
            break
    if not bad:
        os.unlink(path)
    return bad if bad else 'ok'

def main():
    tool = sys.argv[1]
    count = int(sys.argv[2]) if len(sys.argv) > 2 else 500
    start = int(sys.argv[3]) if len(sys.argv) > 3 else 1
    dirname = tempfile.mkdtemp(prefix='jitfuzz')
    ran = bad = 0
    with ThreadPoolExecutor(max_workers=os.cpu_count()) as pool:
        for result in pool.map(one, [(tool, s, dirname) for s in range(start, start + count)]):
            if result is None:
                continue
            ran += 1
            if result != 'ok':
                bad += 1
                seed, flags, path, base, got = result
                print(f"DISAGREE seed {seed} {flags} {path}: rc {base[0]} vs {got[0]}")
                a, b = base[1].split('\n'), got[1].split('\n')
                for i in range(max(len(a), len(b))):
                    x = a[i] if i < len(a) else '<end>'
                    y = b[i] if i < len(b) else '<end>'
                    if x != y:
                        print(f"  line {i}: {x[:100]!r} vs {y[:100]!r}")
                        break
    print(f"{ran} programs, {bad} disagreements, kept in {dirname}")
    sys.exit(1 if bad else 0)

main()
