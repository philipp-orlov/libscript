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
        k = self.r.randint(0, 26)
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
        if 23 <= k <= 26 and d > 1:
            # A callback that may change the array it is called for, take what it was given out of it, or throw.
            base = self.r.choice(s['arr']) if s['arr'] and self.r.random() < 0.6 else f"[{a()}, {a()}, {a()}]"
            inner = {n: list(v) for n, v in s.items()}
            inner['any'] = inner['any'] + ['x', 'i']
            body = self.expr(inner, d - 2)
            effect = self.pick('', '', '', f"{base}.pop(), ", f"{base}.push(i), ", f"{base}[0] = null, ", f"{base}.length = 1, ",
                               "(i == 1 ? nope.y : 0), ") if base in s['arr'] else self.pick('', '', '(i == 1 ? nope.y : 0), ')
            f = f"function (x, i) {{ return ({effect}{body}); }}"
            if k == 23: return f"String({base}.map({f}))"
            if k == 24: return f"String({base}.filter({f}))"
            if k == 25: return f"String({base}.reduce(function (p, x, i) {{ return ({effect}p + {body}); }}, {a()}))"
            return f"String({base}.forEach({f}))"
        return f"({{x: {a()}, y: {a()}}})"

    def stmts(self, s, n, ind, loop=False):
        s = {k: list(v) for k, v in s.items()}
        out = []
        for _ in range(n):
            out += self.stmt(s, ind, loop)
        return out

    def stmt(self, s, ind, loop):
        p = '    ' * ind
        k = self.r.randint(0, 35)
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
            if k in (29, 30, 31):
                return self.hotElements(s, p, k)
            if k in (32, 33, 34, 35):
                return self.hotCalls(s, p, k)
            if k == 25:
                v = self.var(); e = self.expr(s)
                return [f"{p}var {v} = [1, {e}]; {v}[0] += 2; {v}['x'] = {e}; log({v}[0]); log(typeof nope{v}); log(typeof {v});"]
            return [f"{p}log({self.expr(s)});"]
        finally:
            self.depth -= 1

    def hotElements(self, s, p, k):
        """Long enough to be compiled. What is indexed changes kind, or changes under the code, part of the way through."""
        v = self.var(); i = self.var(); r = self.var(); e = self.expr(s, 1)
        kinds = ["'abcd'", '[1, 2, 3, 4]', "[1, 'x', {}, null]", 'new Array(4)', "{0: 'p', 1: 'q'}", '[1.5]', '[]', f'[{e}, 2]',
                 'new Int8Array([1, -2, 3, 4])', 'new Uint32Array([5, 6, 7, 8])', 'new Float32Array([0.1, 2, 3, 4])', 'new Float64Array(4)']
        if k == 29:
            chosen = [self.r.choice(kinds) for _ in range(self.r.randint(1, 3))]
            read = self.pick('o[n] + ":" + o[n]', 'o[n]', '"%" + o[n & 1] + o[n]', 'o[n] === o[n + 1]')
            return [f"{p}var {v} = function (o, n) {{ return {read}; }}, {r};",
                    f"{p}for (var {i} = 0; {i} < 400; {i}++) {{ var o{v} = [{', '.join(chosen)}][{i} % {len(chosen)}]; {r} = {v}(o{v}, {i} & 3); }}",
                    f"{p}log({r});"]
        if k == 30:
            late = self.pick("'s'", '{}', 'undefined', 'null', '2.5', e)
            base = self.r.choice(['[1, 2, 3, 4]', 'new Array(4)', '[0, 0, 0, 0]', 'new Int16Array(4)', 'new Uint8Array(4)', 'new Float64Array(4)', 'new Int32Array(4)'])
            # Partway through, a typed array's buffer may be detached under the compiled loop.
            change = f'{v}[{self.r.randint(0, 3)}] = {late}' if not base.startswith('new I') and not base.startswith('new U') and not base.startswith('new F') or self.r.random() < 0.5 else f'{v}.buffer.transfer()'
            return [f"{p}var {v} = {base};",
                    f"{p}for (var {i} = 0; {i} < 500; {i}++) {{ {v}[{i} & 3] = {self.pick(i, f'{i} * 0.5', e)}; if ({i} === {self.r.randint(0, 499)}) {change}; }}",
                    f"{p}log({v}.join()); log({v}.pop()); {v}.push({late}); log({v}.length); log(String({v}[3]));"]
        start = self.pick('{}', "'s'", 'null', '1', e)
        return [f"{p}var {v} = {{x: {start}, y: 1}}, {r} = 0;",
                f"{p}var k{v} = {v}.x;",
                f"{p}for (var {i} = 0; {i} < 500; {i}++) {{ {v}.x = {i}; {r} += {v}.x; {v}.y = {self.pick(i, chr(39) + 's' + chr(39))}; }}",
                f"{p}log(typeof k{v}); log({r}); log({v}.y);"]

    def hotCalls(self, s, p, k):
        """Built-ins done in place, `.call`, and stores that add properties, hot, with the odd value to surprise them."""
        v = self.var(); i = self.var(); r = self.var(); e = self.expr(s, 1)
        odd = ['0', '-0', 'NaN', 'Infinity', '-Infinity', '1', '-1', '0.5', '-0.5', '2.5', '1e21', '1e-7', "'7'", 'null', 'undefined', '{}', e]
        if k == 32:
            f = self.pick('Math.max(a, b)', 'Math.min(a, b)', 'Math.pow(a, b)', 'Math.atan2(a, b)', 'a ** b', 'Math.round(a)', 'parseInt(a)',
                          'Math.sin(a)', 'Math.cos(a)', 'Math.tan(a)', 'Math.atan(a)', 'Math.exp(a)', 'Math.log(a)')
            x, y = self.r.choice(odd), self.r.choice(odd)
            late = self.r.randint(0, 299)
            return [f"{p}var {v} = function (a, b) {{ return {f}; }}, {r} = 0;",
                    f"{p}for (var {i} = 0; {i} < 300; {i}++) {{ {r} = {v}({i} * 0.25, 3 - {i}); if ({i} === {late}) log({r}); }}",
                    f"{p}{r} = {v}({x}, {y}); log(1 / {r}); log({r});"]
        if k == 33:
            self_ = self.r.choice(['undefined', 'null', '{k: 1}', 'globalThis', '[]'])
            args = ', '.join(self.r.choice(odd) for _ in range(self.r.randint(0, 3)))
            target = self.pick('function (a, b) { return String(a) + String(b) + arguments.length; }',
                               'function () { return this === globalThis; }', 'Math.max', 'String.prototype.concat',
                               'function (a) { return a + 1; }')
            late = self.pick(f'{v}.call = function () {{ return "own"; }};', f'{v} = {{}};', f'{v} = {{call: 5}};', '')
            call_args = f'{self_}, {args}' if args else self_
            return [f"{p}var {v} = {target}, {r} = 0;",
                    f"{p}var c{v} = function () {{ return {v}.call({call_args}); }};",
                    f"{p}for (var {i} = 0; {i} < 300; {i}++) {{ {r} = String(c{v}()); }}",
                    f"{p}log({r}); {late} try {{ log(String(c{v}())); }} catch (x{v}) {{ log(x{v}.name); }}"]
        if k == 34:
            names = ['a', 'b', 'c', 'd', 'e', 'f']
            self.r.shuffle(names)
            orders = [names[:self.r.randint(0, 5)] for _ in range(self.r.randint(2, 5))]
            builds = ', '.join('function (o) { ' + ''.join(f'o.{n} = 1; ' for n in order) + '}' for order in orders)
            late = self.pick(f"Object.defineProperty(N{v}.prototype, 'z', {{ set: function (x) {{ this.got = x; }}, get: function () {{ return 'g'; }}, configurable: true }});",
                             f"N{v}.prototype.z = 'proto';", f"Object.freeze(N{v}.prototype);", f"N{v}.prototype = {{z: 'other'}};", '')
            at = self.r.randint(0, 399)
            return [f"{p}function N{v}() {{}} var b{v} = [{builds}], {r} = '';",
                    f"{p}var s{v} = function (o, x) {{ o.z = x; return o; }};",
                    f"{p}for (var {i} = 0; {i} < 400; {i}++) {{ if ({i} === {at}) {{ {late} }} var o{v} = new N{v}(); b{v}[{i} % {len(orders)}](o{v}); s{v}(o{v}, {i}); s{v}(o{v}, {i} + 1); {r} = Object.keys(o{v}).join() + ':' + o{v}.z + ':' + o{v}.got; }}",
                    f"{p}log({r});"]
        if self.r.random() < 0.15:
            late = self.pick(f"N{v}.prototype = {{tag: 'late', init: function (x) {{ this.x = 'L' + x; }}}};", f"N{v}.prototype = 5;",
                             f"N{v}.x = 1;", f"Object.setPrototypeOf(N{v}.prototype, null);", f"N{v}.prototype.init = function () {{ return {{own: 1}}; }};",
                             f"delete N{v}.prototype.init;", '')
            at = self.r.randint(0, 299)
            body = self.pick('this.init.apply(this, arguments);', 'var r = this.init(a); if (r) return r;', 'this.x = a; for (var k = 0; k < 2; k++) this.x += k;')
            return [f"{p}function N{v}(a) {{ {body} }} N{v}.prototype.init = function (x) {{ this.x = x; }}; var {r} = '';",
                    f"{p}var m{v} = function (i) {{ return new N{v}(i); }};",
                    f"{p}for (var {i} = 0; {i} < 300; {i}++) {{ if ({i} === {at}) {{ {late} }} try {{ var o{v} = m{v}({i}); {r} = String(o{v}.x) + ':' + o{v}.tag + ':' + (o{v} instanceof N{v}) + ':' + o{v}.own; }} catch (x{v}) {{ {r} = x{v}.name; }} }}",
                    f"{p}log({r});"]
        if self.r.random() < 0.4:
            kinds = ['new Map()', 'new Set()', '{}', 'Object.create(Map.prototype)', '[]']
            coll = self.r.choice(kinds)
            keys = ', '.join(self.r.choice(odd) for _ in range(self.r.randint(1, 4)))
            act = self.pick('o.get(k)', 'o.has(k)', 'o.set(k, n).size', 'o.add(k).size', 'o.set(k, n).get(k)', 'o.size')
            return [f"{p}var {v} = function (o, k, n) {{ return {act}; }}, {r} = 0, l{v} = {coll}, q{v} = [{keys}];",
                    f"{p}for (var {i} = 0; {i} < 300; {i}++) {{ try {{ {r} = {v}(l{v}, q{v}[{i} % q{v}.length], {i}); }} catch (x{v}) {{ {r} = x{v}.name; }} }}",
                    f"{p}log(String({r})); log(String(l{v}.size));"]
        start = self.pick('[]', 'new Array()', 'new Array(3)', "['x']", '[1.5, 2.5, 3.5, 4.5]', 'Object.freeze([])', "{push: function (x) { return 'mine'; }}")
        vals = ', '.join(self.r.choice(['{}', "'s'", 'null', 'undefined', '1', '1.5', 'NaN', '-0', e, f'{i}', f'{i} * 0.5', f'{i} | 0', 'true']) for _ in range(self.r.randint(1, 3)))
        return [f"{p}var {v} = function (a, x) {{ return a.push(x); }}, {r} = 0, l{v} = null;",
                f"{p}for (var {i} = 0; {i} < 300; {i}++) {{ if ({i} % 50 === 0) l{v} = {start}; var q{v} = [{vals}]; try {{ {r} = {v}(l{v}, q{v}[{i} % q{v}.length]); }} catch (x{v}) {{ {r} = x{v}.name; }} }}",
                f"{p}log({r}); log(String(l{v}.length)); if (l{v}.join) log(l{v}.map(function (x) {{ return 1 / x; }}).join());"]

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
        p = subprocess.run([tool] + (['--interpret'] if env.get('X_INTERP') else []) + [path], capture_output=True, text=True, errors="replace", timeout=30, env=e)
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
