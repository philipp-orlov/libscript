#!/usr/bin/env python3
"""Differential fuzzing of native code for JavaScript: random programs, interpreted and compiled, must print the same.

usage: js_jit_fuzz.py <libscript tool> [count] [seed]. Prints each disagreement and keeps the program.
"""
import os, random, subprocess, sys, tempfile
from concurrent.futures import ThreadPoolExecutor

class Gen:
    def __init__(self, seed):
        self.r = random.Random(seed)
        self.n = 0
        self.depth = 0

    def pick(self, *xs): return self.r.choice(xs)
    def var(self):
        self.n += 1
        return f"v{self.n}"

    def num(self):
        return self.pick('0', '1', '-1', '2', '3.5', '7', '31', '100', '65536', '2147483647', '-2147483648', '4294967296',
                         '1e21', '0.1', 'NaN', 'Infinity', str(self.r.randint(-1000, 1000)))

    def expr(self, s, d=3):
        names = s['any'] + s['num']
        if d <= 0 or self.r.random() < 0.2:
            k = self.r.randint(0, 6)
            if names and k <= 3:
                return self.r.choice(names)
            if k == 4:
                return self.pick('"str"', '""', '"12"', 'true', 'false', 'null', 'undefined')
            return self.num()
        a = lambda: self.expr(s, d - 1)
        k = self.r.randint(0, 22)
        if k <= 3: return f"({a()} {self.pick('+','-','*','/','%')} {a()})"
        if k == 4: return f"({a()} {self.pick('&','|','^','<<','>>','>>>')} {a()})"
        if k == 5: return f"({a()} {self.pick('<','<=','>','>=')} {a()})"
        if k == 6: return f"({a()} {self.pick('==','!=','===','!==')} {a()})"
        if k == 7: return f"({a()} {self.pick('&&','||','??')} {a()})"
        if k == 8: return f"({a()} ? {a()} : {a()})"
        if k == 9: return f"(!{a()})"
        if k == 10: return f"(- {a()})"
        if k == 11: return f"(~{a()})"
        if k == 12: return f"(typeof {a()})"
        if k == 13 and s['obj']: return f"{self.r.choice(s['obj'])}.{self.pick('x','y','z','missing')}"
        if k == 14 and s['arr']: return f"{self.r.choice(s['arr'])}[{self.pick(a(), '0', '1', '2', '100', '-1')}]"
        if k == 15 and s['arr']: return f"{self.r.choice(s['arr'])}.length"
        if k == 16 and s['fn']: return f"{self.r.choice(s['fn'])}({a()}, {a()})"
        if k == 17 and s['obj']: return f"{self.r.choice(s['obj'])}.m({a()})"
        if k == 18: return f"String({a()}).length"
        if k == 19: return f"f{self.r.randint(0, 3)}({a()}, {a()})"
        if k == 20: return f"Math.{self.pick('floor','abs','sqrt')}({a()})"
        if k == 21: return f"[{a()}, {a()}]"
        return f"({{x: {a()}, y: {a()}}})"

    def stmts(self, s, n, ind, loop=False):
        s = {k: list(v) for k, v in s.items()}
        out = []
        for _ in range(n):
            out += self.stmt(s, ind, loop)
        return out

    def stmt(self, s, ind, loop):
        p = '    ' * ind
        k = self.r.randint(0, 28)
        self.depth += 1
        deep = self.depth > 3
        try:
            if k == 0:
                v = self.var(); e = self.expr(s); s['any'].append(v)
                return [f"{p}var {v} = {e};"]
            if k == 1:
                v = self.var(); e = self.pick('0', '1', '2.5', '100'); s['num'].append(v)
                return [f"{p}var {v} = {e};"]
            if k == 2 and s['num']:
                v = self.r.choice(s['num']); o = self.r.choice(s['num'])
                return [f"{p}{v} = {v} {self.pick('+','-','*')} {o};"]
            if k == 3 and s['any']:
                v = self.r.choice(s['any'])
                return [f"{p}{v} = {self.expr(s)};"]
            if k == 4 and not deep:
                return [f"{p}if ({self.expr(s, 2)}) {{"] + self.stmts(s, 2, ind + 1, loop) + [f"{p}}} else {{"] + self.stmts(s, 2, ind + 1, loop) + [f"{p}}}"]
            if k == 5 and not deep:
                i = self.var(); w = {key: list(x) for key, x in s.items()}; w['num'].append(i)
                return [f"{p}for (var {i} = 0; {i} < {self.r.randint(0, 9)}; {i}++) {{"] + self.stmts(w, 3, ind + 1, True) + [f"{p}}}"]
            if k == 6:
                v = self.var(); e = self.expr(s); g = self.expr(s); s['obj'].append(v)
                return [f"{p}var {v} = new C({e}, {g});"]
            if k == 7 and s['obj']:
                o = self.r.choice(s['obj'])
                return [f"{p}{o}.{self.pick('x','y','z')} = {self.expr(s)};"]
            if k == 8:
                v = self.var(); e = self.expr(s); s['arr'].append(v)
                return [f"{p}var {v} = [1, {e}, 3];"]
            if k == 9 and s['arr']:
                a = self.r.choice(s['arr'])
                return [f"{p}{a}[{self.pick('0', '1', '2', '3', self.expr(s, 1))}] = {self.expr(s)};"]
            if k == 10 and s['arr']:
                a = self.r.choice(s['arr'])
                return [f"{p}{a}.push({self.expr(s)});", f"{p}log({a}.length);"]
            if k == 11:
                v = self.var(); o = self.var(); e = self.expr(s); s['fn'].append(v)
                return [f"{p}var {o} = {e};", f"{p}var {v} = function (a, b) {{ {o} = {o} + 1; return a + {o}; }};", f"{p}log({v}(1, 2));"]
            if k == 12:
                return [f"{p}log({self.expr(s)});"]
            if k == 13 and loop:
                return [f"{p}if ({self.expr(s, 1)}) {self.pick('break', 'continue')};"]
            if k == 14:
                v = self.var(); e = self.expr(s)
                return [f"{p}for (var {v} of [1, {e}, 'k']) {{ log({v}); }}"]
            if k == 15:
                v = self.var(); s['any'].append(v); e = self.expr(s)
                return [f"{p}var {v} = 0; {v} += {e};"]
            if k == 16 and s['obj']:
                o = self.r.choice(s['obj']); v = self.var()
                return [f"{p}var {v} = {o}; {v}.z = 1; log({v}.z + {o}.x);"]
            if k == 17:
                v = self.var(); e = self.expr(s)
                return [f"{p}let {v} = {e}; const c{v} = () => {v}; log(c{v}());"]
            if k == 18:
                return [f"{p}gt = {self.expr(s)}; log(gt);"]
            if k == 19:
                v = self.var(); e = self.expr(s)
                return [f"{p}var o{v} = {{ a: {e}, b: 2, 'c d': 3 }};", f"{p}for (var {v} in o{v}) {{ log({v}); log(o{v}[{v}]); }}"]
            if k == 20:
                v = self.var(); e = self.expr(s)
                return [f"{p}var {v} = function (x, ...rest) {{ log(rest.length); return rest.length ? rest[0] : x; }};",
                        f"{p}log({v}({e})); log({v}(1, {e}, 3));"]
            if k == 21:
                v = self.var(); e = self.expr(s)
                return [f"{p}var {v} = function () {{ log(arguments.length); return arguments[1]; }};",
                        f"{p}log({v}({e})); log({v}(1, {e}, 3));"]
            if k == 22:
                v = self.var(); e = self.expr(s)
                return [f"{p}var {v} = new C({e}, 2);", f"{p}log({v} instanceof C); log('x' in {v}); log(delete {v}.x); log('x' in {v}); log({v}.x);"]
            if k == 23:
                v = self.var(); e = self.expr(s)
                return [f"{p}var {v} = {{ n: {e}, get g() {{ return this.n; }}, set g(w) {{ this.n = w; }}, m() {{ return this.n; }}, ['k' + 1]: 7 }};",
                        f"{p}log({v}.g); {v}.g = 5; log({v}.m()); log({v}.k1);"]
            if k == 24:
                v = self.var(); e = self.expr(s)
                return [f"{p}class {v} extends C {{ constructor(q) {{ super(q, 1); }} get w() {{ return this.y; }} m(v) {{ return super.m(v); }} }}",
                        f"{p}log(new {v}({e}).m(3)); log(new {v}(1).w);"]
            if k == 26 and s['arr']:
                a = self.r.choice(s['arr'])
                return [f"{p}log({a}.pop()); log({a}.length); log({a}.push({self.expr(s)}, 1));"]
            if k == 27:
                v = self.var(); e = self.expr(s)
                return [f"{p}var {v} = String({e}) + 'q\u00e9';", f"{p}log({v}.charCodeAt({self.pick('0', '1', '-1', '99', '1.5', self.expr(s, 1))}));",
                        f"{p}log(String.fromCharCode({self.pick('65', '300', '-1', '1e10', self.expr(s, 1))}));"]
            if k == 25:
                v = self.var(); e = self.expr(s)
                return [f"{p}var {v} = [1, {e}]; {v}[0] += 2; {v}['x'] = {e}; log({v}[0]); log(typeof nope{v}); log(typeof {v});"]
            return [f"{p}log({self.expr(s)});"]
        finally:
            self.depth -= 1

    def program(self):
        s = dict(any=['a', 'b'], num=[], obj=[], arr=[], fn=[])
        body = self.stmts(s, self.r.randint(6, 16), 1)
        helpers = []
        for i in range(4):
            h = dict(any=['p', 'q'], num=[], obj=[], arr=[], fn=[])
            e = self.expr(h, 2)
            e = e.replace('f0(', 'g(').replace('f1(', 'g(').replace('f2(', 'g(').replace('f3(', 'g(')
            helpers.append(f"function f{i}(p, q) {{ return {e}; }}")
        return f"""var out = [];
function show(x) {{
    if (x === null) return 'null';
    if (typeof x === 'object') return Array.isArray(x) ? '[' + x.length + ']' : '{{}}';
    if (typeof x === 'function') return 'fn';
    return String(x);
}}
function log(x) {{ out.push(show(x)); }}
var gt = 0;
function g(p, q) {{ return p; }}
class C {{
    constructor(x, y) {{ this.x = x; this.y = y; }}
    m(v) {{ return this.x === undefined ? v : v; }}
}}
{chr(10).join(helpers)}
function run(a, b) {{
{chr(10).join(body)}
    return a;
}}
for (var i = 0; i < 40; i++) {{
    try {{ log(run(i, i * 2)); }} catch (e) {{ log('caught ' + e.message); }}
    if (out.length > 3000) out.length = 3000;
}}
console.log(out.join('|'));
"""

def run(tool, env, path):
    e = dict(os.environ)
    e.update(env)
    try:
        p = subprocess.run([tool] + (['--interpret'] if env.get('X_INTERP') else []) + [path], capture_output=True, text=True, timeout=30, env=e)
    except subprocess.TimeoutExpired:
        return 'timeout', ''
    return p.returncode, p.stdout + p.stderr

def one(args):
    tool, seed, dirname = args
    source = Gen(seed).program()
    path = os.path.join(dirname, f"f{seed}.js")
    with open(path, 'w') as f:
        f.write(source)
    base = run(tool, {'X_INTERP': '1'}, path)
    if base[0] != 0 and 'exception' not in base[1][:200].lower():
        os.unlink(path)
        return None
    bad = None
    for env in ({'SCRIPT_JIT': 'tiered:1'}, {'SCRIPT_JIT': 'tiered:5'}, {'SCRIPT_JIT': 'tiered:30'}):
        got = run(tool, env, path)
        if got != base:
            bad = (seed, env, path, base, got)
            break
    if not bad:
        os.unlink(path)
    return bad if bad else 'ok'

def main():
    tool = sys.argv[1]
    count = int(sys.argv[2]) if len(sys.argv) > 2 else 500
    start = int(sys.argv[3]) if len(sys.argv) > 3 else 1
    dirname = tempfile.mkdtemp(prefix='jsjit')
    ran = bad = 0
    with ThreadPoolExecutor(max_workers=os.cpu_count()) as pool:
        for result in pool.map(one, [(tool, s, dirname) for s in range(start, start + count)]):
            if result is None:
                continue
            ran += 1
            if result != 'ok':
                bad += 1
                seed, env, path, base, got = result
                print(f"DISAGREE seed {seed} {env} {path}: rc {base[0]} vs {got[0]}")
                a, b = base[1].split('|'), got[1].split('|')
                for i in range(max(len(a), len(b))):
                    x = a[i] if i < len(a) else '<end>'
                    y = b[i] if i < len(b) else '<end>'
                    if x != y:
                        print(f"  item {i}: {x[:80]!r} vs {y[:80]!r}")
                        break
    print(f"{ran} programs, {bad} disagreements, kept in {dirname}")
    sys.exit(1 if bad else 0)

main()
