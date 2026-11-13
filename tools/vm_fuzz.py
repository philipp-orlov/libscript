#!/usr/bin/env python3
"""Differential fuzzing of the vm core against the dynamic engine, on what the vm core compiles.

usage: vm_fuzz.py <libscript tool> [count] [seed]

The tool must be built with SCRIPT_JIT_ENV_OVERRIDE. Each random program prints a line of values; the dynamic engine with
its interpreter and the vm core must print the same line. Prints each disagreement and keeps the program.
js_jit_fuzz.py --cores does the same with that fuzzer's programs once the vm core has classes, Map and typed arrays.
"""
import os, random, subprocess, sys, tempfile
from concurrent.futures import ThreadPoolExecutor


class Gen:
    def __init__(self, seed):
        self.r = random.Random(seed)
        self.n = 0

    def pick(self, *xs):
        return self.r.choice(xs)

    def name(self):
        self.n += 1
        return f"v{self.n}"

    def num(self):
        return self.pick('0', '1', '-1', '2', '3', '3.5', '7', '31', '100', '255', '65536', '2147483647', '-2147483648',
                         '4294967296', '1e21', '0.1', '-0', 'NaN', 'Infinity', str(self.r.randint(-1000, 1000)))

    def atom(self, vs):
        k = self.r.randint(0, 9)
        if vs['n'] and k <= 4:
            return self.r.choice(vs['n'])
        if k == 5:
            return self.pick('"s"', '""', '"12"', '"abc"', 'true', 'false', 'null', 'undefined')
        return self.num()

    def expr(self, vs, d=3):
        if d <= 0 or self.r.random() < 0.15:
            return self.atom(vs)
        a = lambda: self.expr(vs, d - 1)
        k = self.r.randint(0, 24)
        if k <= 3:
            return f"({a()} {self.pick('+', '-', '*', '/', '%', '**')} {a()})"
        if k == 4:
            return f"({a()} {self.pick('&', '|', '^', '<<', '>>', '>>>')} {a()})"
        if k == 5:
            return f"({a()} {self.pick('<', '<=', '>', '>=')} {a()})"
        if k == 6:
            return f"({a()} {self.pick('==', '!=', '===', '!==')} {a()})"
        if k == 7:
            return f"({a()} {self.pick('&&', '||', '??')} {a()})"
        if k == 8:
            return f"({a()} ? {a()} : {a()})"
        if k == 9:
            return f"({self.pick('!', '-', '+', '~', 'typeof ', 'void ')}{a()})"
        if k == 10 and vs['o']:
            return f"{self.r.choice(vs['o'])}.{self.pick('x', 'y', 'z', 'missing')}"
        if k == 11 and vs['a']:
            return f"{self.r.choice(vs['a'])}[{self.pick(a(), '0', '1', '2', '100', '-1')}]"
        if k == 12 and vs['a']:
            return f"{self.r.choice(vs['a'])}.length"
        if k == 13 and vs['f']:
            return f"{self.r.choice(vs['f'])}({a()}, {a()})"
        if k == 14:
            return f"String({a()}).length"
        if k == 15:
            return f"Math.{self.pick('floor', 'abs', 'sqrt', 'ceil', 'round', 'trunc')}({a()})"
        if k == 16:
            return f"[{a()}, {a()}, {a()}]"
        if k == 17:
            return f"({{x: {a()}, y: {a()}}})"
        if k == 18:
            return f"({a()}, {a()})"
        if k == 19 and vs['a']:
            return f"{self.r.choice(vs['a'])}.{self.pick('join', 'indexOf')}({a()})"
        if k == 20:
            return f"`t${{{a()}}}u`"
        if k == 21:
            return f"({a()} in {{x: 1, y: 2}})"
        if k == 22:
            return f"Number({a()})"
        if k == 23:
            return f"(function (q) {{ return q {self.pick('+', '*', '-')} {a()}; }})({a()})"
        return self.atom(vs)

    def stmts(self, vs, n, ind, loop=False, depth=0):
        out = []
        for _ in range(n):
            out += self.stmt(vs, ind, loop, depth)
        return out

    def stmt(self, vs, ind, loop, depth):
        p = '    ' * ind
        k = self.r.randint(0, 36)
        e = lambda: self.expr(vs)
        if k <= 2 or depth > 3:
            v = self.name()
            kind = self.pick('var', 'let', 'let', 'const')
            init = e()
            vs['n'].append(v)
            return [f"{p}{kind} {v} = {init};", f"{p}log({v});"]
        if k == 3 and vs['n']:
            v = self.r.choice(vs['n'])
            return [f"{p}try {{ {v} {self.pick('=', '+=', '-=', '*=', '|=', '??=')} {e()}; }} catch (e) {{ log(e.name); }}", f"{p}log({v});"]
        if k == 4:
            return [f"{p}log({e()});"]
        if k == 5:
            body = self.stmts(self.copy(vs), self.r.randint(1, 3), ind + 1, loop, depth + 1)
            els = self.stmts(self.copy(vs), self.r.randint(1, 2), ind + 1, loop, depth + 1)
            return [f"{p}if ({e()}) {{"] + body + [f"{p}}} else {{"] + els + [f"{p}}}"]
        if k == 6:
            i = self.name()
            body = self.stmts(self.copy(vs, [i]), self.r.randint(1, 3), ind + 1, True, depth + 1)
            return [f"{p}for (var {i} = 0; {i} < {self.r.randint(1, 8)}; {i}++) {{"] + body + [f"{p}}}"]
        if k == 7:
            i = self.name()
            body = self.stmts(self.copy(vs, [i]), self.r.randint(1, 3), ind + 1, True, depth + 1)
            return [f"{p}var {i} = 0;", f"{p}while ({i} < {self.r.randint(1, 6)}) {{", f"{p}    {i}++;"] + body + [f"{p}}}"]
        if k == 8:
            i = self.name()
            body = self.stmts(self.copy(vs, [i]), self.r.randint(1, 2), ind + 1, True, depth + 1)
            return [f"{p}for (let {i} = 0; {i} < {self.r.randint(1, 5)}; {i}++) {{"] + body + [f"{p}    fs.push(() => {i});", f"{p}}}"]
        if k == 9 and loop:
            return [f"{p}if ({e()}) {self.pick('break', 'continue')};"]
        if k == 10:
            v = self.name()
            body = self.expr(self.copy(vs, ['a', 'b']))
            vs['f'].append(v)
            return [f"{p}function {v}(a, b) {{ log(a); return {body}; }}"]
        if k == 11:
            v = self.name()
            init = f"{{x: {e()}, y: {e()}, m: function (t) {{ return this.x + t; }}}}"
            vs['o'].append(v)
            return [f"{p}var {v} = {init};", f"{p}log({v}.m({e()}));"]
        if k == 12:
            v = self.name()
            init = f"[{e()}, {e()}, {e()}]"
            vs['a'].append(v)
            return [f"{p}var {v} = {init};", f"{p}{v}.push({e()});", f"{p}log({v}.join());"]
        if k == 13 and vs['o']:
            o = self.r.choice(vs['o'])
            return [f"{p}{o}.{self.pick('x', 'y', 'z', 'w')} = {e()};", f"{p}log({o}.{self.pick('x', 'y', 'z', 'w')});"]
        if k == 14 and vs['a']:
            a = self.r.choice(vs['a'])
            return [f"{p}{a}[{self.pick('0', '1', '5', '2')}] = {e()};", f"{p}log({a}.length);"]
        if k == 15:
            body = self.stmts(self.copy(vs), self.r.randint(1, 2), ind + 1, loop, depth + 1)
            return [f"{p}try {{", f"{p}    {self.pick('throw ' + e(), 'null.x', 'undefinedName', 'log(1)')};"] + body + \
                   [f"{p}}} catch (err) {{ log(String(err && err.name || err)); }} finally {{ log('f'); }}"]
        if k == 16:
            return [f"{p}switch ({e()}) {{ case 1: log('one'); case 2: log('two'); break; case 'a': log('a'); break; default: log('d'); }}"]
        if k == 17:
            i = self.name()
            return [f"{p}lab{i}: for (var {i} = 0; {i} < 3; {i}++) {{ for (var {i}j = 0; {i}j < 3; {i}j++) {{ if ({i}j == {self.r.randint(0, 2)}) continue lab{i}; if ({i} == {self.r.randint(0, 3)}) break lab{i}; log({i} * 3 + {i}j); }} }}"]
        if k == 18:
            v = self.name()
            return [f"{p}var {v} = (function () {{ var c = {e()}; return function (d) {{ c = c + d; return c; }}; }})();", f"{p}log({v}({e()})); log({v}({e()}));"]
        if k == 19 and vs['a']:
            a = self.r.choice(vs['a'])
            return [f"{p}for (var k in {a}) log(k);", f"{p}for (var w of {a}) log(w);"]
        if k == 20 and vs['a']:
            a = self.r.choice(vs['a'])
            return [f"{p}log({a}.{self.pick('map', 'filter')}(function (x) {{ return {self.pick('x', 'x * 2', '!x', 'x + 1')}; }}).join());"]
        if k == 21:
            v = self.name()
            return [f"{p}class {v} {{ #p = {e()}; f = {e()}; static s = {e()}; constructor(a) {{ this.a = a; }} get g() {{ return this.#p; }} set g(x) {{ this.#p = x; }} m(t) {{ return this.a + t; }} static sm() {{ return {v}.s; }} }}",
                    f"{p}var {v}i = new {v}({e()}); {v}i.g = {e()};",
                    f"{p}log({v}i.g); log({v}i.f); log({v}.sm()); log({v}i.m({e()}));"]
        if k == 22:
            v = self.name()
            return [f"{p}class {v}A {{ constructor(a) {{ this.a = a; }} m() {{ return 'A' + this.a; }} }}",
                    f"{p}class {v}B extends {v}A {{ constructor(a) {{ super(a); this.b = {e()}; }} m() {{ return 'B' + super.m() + this.b; }} }}",
                    f"{p}log(new {v}B({e()}).m()); log(new {v}B(1) instanceof {v}A);"]
        if k == 23:
            v = self.name()
            return [f"{p}function* {v}(n) {{ for (var i = 0; i < n; i++) {{ var r = yield i * {e()}; if (r) log('got' + r); }} return 'done'; }}",
                    f"{p}var {v}g = {v}({self.r.randint(0, 4)}); log({v}g.next().value); log({v}g.next({e()}).value); log([...{v}({self.r.randint(0, 4)})].join());",
                    f"{p}log({v}g.return({e()}).done);"]
        if k == 24:
            v = self.name()
            return [f"{p}function* {v}() {{ try {{ yield 1; yield 2; }} finally {{ log('cleanup'); }} }}",
                    f"{p}for (var {v}x of {v}()) {{ log({v}x); break; }}",
                    f"{p}var {v}i = {v}(); {v}i.next(); log({v}i.return(5).value);"]
        if k == 25:
            return [f"{p}var [{self.name()}a, {self.name()}b = {self.pick('7', '"d"', 'null', '[1]')}, ...{self.name()}r] = [{e()}, undefined, {e()}, {e()}];",
                    f"{p}var {{x: dx = {self.pick('7', '"d"', 'null')}, ...drest}} = {{x: undefined, y: {e()}, z: {e()}}}; log(dx); log(Object.keys(drest).join());"]
        if k == 26 and vs['f']:
            f = self.r.choice(vs['f'])
            return [f"{p}log({f}(...[{e()}, {e()}])); log([...'ab', ...[{e()}], ...new Set([1, 2, 1])].join());"]
        if k == 27:
            v = self.name()
            return [f"{p}var {v} = new Map([[1, {e()}], ['a', {e()}]]); {v}.set(NaN, {e()}); {v}.delete(1);",
                    f"{p}log({v}.size); log([...{v}.keys()].join()); log({v}.get(NaN)); log({v}.has('a'));",
                    f"{p}var {v}s = new Set([{e()}, {e()}, 1, 1]); log({v}s.size); log([...{v}s].length);"]
        if k == 28:
            parsed = 'JSON.parse(\'{"u": [1, 2.5, {"v": null}], "w": "x"}\')'
            return [f"{p}log(JSON.stringify({{a: {e()}, b: [{e()}, undefined, null], c: 'q'}}));",
                    f"{p}log(JSON.stringify({parsed}));"]
        if k == 29:
            s_ = self.pick('"abc"', '"a,b,c"', '"Hello World"', '"  pad  "', '""')
            sep = self.pick('","', '""', '/o/', r'/\s+/')
            op = self.pick('toUpperCase()', 'trim()', 'padStart(8, "-")', 'slice(1, -1)', 'replace(/[aeiou]/g, "_")', 'indexOf("o")', 'at(-1)', 'repeat(2)')
            return [f"{p}log({s_}.split({sep}).join('|')); log({s_}.{op});"]
        if k == 30:
            t = self.name()
            kind = self.pick('Uint8Array', 'Int16Array', 'Float32Array', 'Uint8ClampedArray', 'Int32Array')
            return [f"{p}var {t} = new {kind}([{e()}, {e()}, {e()}]); log({t}.join()); {t}[1] = {e()}; log({t}[1]); log({t}.length);"]
        if k == 31:
            v = self.name()
            return [f"{p}var {v} = {{get g() {{ return {e()}; }}, set g(x) {{ this._g = x; }}, [`k${{1 + 1}}`]: {e()}, ['__proto__']: 1}}; {v}.g = {e()}; log({v}.g); log({v}._g); log({v}.k2);"]
        if k == 32 and vs['o']:
            o = self.r.choice(vs['o'])
            return [f"{p}log({o}?.x); log({o}?.missing?.deeper); log({o}.m?.({e()})); log(Object.keys({o}).join()); log(Object.entries({o}).length);"]
        if k == 33:
            v = self.name()
            return [f"{p}var {v} = {{ [Symbol.iterator]() {{ var i = 0; return {{ next() {{ return {{ value: i++, done: i > {self.r.randint(0, 4)} }}; }}, return() {{ log('closed'); return {{}}; }} }}; }} }};",
                    f"{p}log([...{v}].join()); for (var {v}x of {v}) {{ log({v}x); if ({v}x > 1) break; }}"]
        if k == 34:
            return [f"{p}function dflt{self.n}(a, b = a + 1, ...r) {{ return [a, b, r.length, arguments.length].join(); }} log(dflt{self.n}({e()})); log(dflt{self.n}({e()}, {e()}, {e()}));"]
        if k == 35:
            return [f"{p}log(`${{{e()}}}-${{{e()}}}`); log([{e()}, {e()}].sort().join()); log([3, 1, 2].sort((a, b) => b - a).join());",
                    f"{p}log(Array.from({{length: 3}}, (_, i) => i * {e()}).join()); log(Array.of({e()}, {e()}).length);"]
        if k == 36:
            return [f"{p}try {{ log(BigInt({self.r.randint(-50, 50)}) * 3n ** 20n); log(typeof 1n); log(10n / 3n); log(1n < 2); }} catch (err) {{ log(err.name); }}"]
        v = self.name()
        init = e()
        vs['n'].append(v)
        return [f"{p}var {v} = {init};", f"{p}log({v});"]

    def copy(self, vs, extra=()):
        return {'n': list(vs['n']) + list(extra), 'o': list(vs['o']), 'a': list(vs['a']), 'f': list(vs['f'])}

    def program(self):
        vs = {'n': [], 'o': [], 'a': [], 'f': []}
        body = self.stmts(vs, self.r.randint(6, 14), 1)
        return ("var out = [], fs = [];\n"
                "function show(x) { if (x === null) return 'null'; if (typeof x === 'object') return Array.isArray(x) ? '[' + x.join() + ']' : '{}'; "
                "if (typeof x === 'function') return 'fn'; if (typeof x === 'number' && x === 0 && 1 / x < 0) return '-0'; return String(x); }\n"
                "function log(x) { out.push(show(x)); }\n"
                "function run() {\n" + "\n".join(body) + "\n}\n"
                "try { run(); } catch (e) { out.push('THROW ' + (e && e.name)); }\n"
                "for (var i = 0; i < fs.length; i++) out.push(fs[i]());\n"
                "console.log(out.join('|'));\n")


def run(tool, env, path):
    e = dict(os.environ)
    e.update(env)
    try:
        p = subprocess.run([tool, path], capture_output=True, text=True, errors="replace", timeout=30, env=e)
    except subprocess.TimeoutExpired:
        return 'timeout', ''
    return p.returncode, p.stdout + p.stderr


def one(args):
    tool, seed, dirname = args
    path = os.path.join(dirname, f"f{seed}.js")
    with open(path, 'w') as f:
        f.write(Gen(seed).program())
    base = run(tool, {'SCRIPT_CORE': 'dyn', 'SCRIPT_JIT': 'off'}, path)
    got = run(tool, {'SCRIPT_CORE': 'vm', 'SCRIPT_VM_TIER1': os.environ.get('FUZZ_TIER1', '0')}, path)
    if base[0] != 0 and got[0] != 0:
        # Both refuse the program: the text of the message differs.
        os.unlink(path)
        return 'ok'
    if 'not supported' in got[1]:
        os.unlink(path)
        return None
    if got != base:
        return (seed, path, base, got)
    os.unlink(path)
    return 'ok'


def main():
    tool = sys.argv[1]
    count = int(sys.argv[2]) if len(sys.argv) > 2 else 500
    start = int(sys.argv[3]) if len(sys.argv) > 3 else 1
    dirname = tempfile.mkdtemp(prefix='vmfuzz')
    ran = bad = skipped = 0
    with ThreadPoolExecutor(max_workers=os.cpu_count()) as pool:
        for result in pool.map(one, [(tool, s, dirname) for s in range(start, start + count)]):
            if result is None:
                skipped += 1
                continue
            ran += 1
            if result != 'ok':
                bad += 1
                seed, path, base, got = result
                print(f"DISAGREE seed {seed} {path}: rc {base[0]} vs {got[0]}")
                a, b = base[1].split('|'), got[1].split('|')
                for i in range(max(len(a), len(b))):
                    x = a[i] if i < len(a) else '<end>'
                    y = b[i] if i < len(b) else '<end>'
                    if x != y:
                        print(f"  item {i}: dyn {x[:100]!r} vs vm {y[:100]!r}")
                        break
    print(f"{ran} programs, {bad} disagreements, {skipped} skipped, kept in {dirname}")
    sys.exit(1 if bad else 0)


main()
