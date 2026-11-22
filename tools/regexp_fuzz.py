#!/usr/bin/env python3
# Copyright (c) 2026 Philipp Orlov
# SPDX-License-Identifier: Apache-2.0
"""regexp_fuzz.py TOOL COUNT SEED: random patterns on random inputs, the vm core's native matchers against the matcher."""
import os, random, subprocess, sys, tempfile
from concurrent.futures import ThreadPoolExecutor

class Gen:
    def __init__(self, seed):
        self.r = random.Random(seed)
        self.groups = 0
        self.unicode = False

    def ch(self):
        pool = ['a', 'b', 'c', 'A', 'B', '1', '2', ' ', '_', '-', '\\n', '\\u00e9', '\\u0411', '\\u2028', 'k', 's']
        if self.unicode:
            pool += ['\\u{1f0a1}', '\\u{1f0a2}', '\\ud83c']
        return self.r.choice(pool[:6]) if self.r.random() < 0.7 else self.r.choice(pool)

    def cls(self):
        parts = []
        for _ in range(self.r.randint(1, 4)):
            k = self.r.randint(0, 5)
            parts.append(self.r.choice(['a-c', 'A-Z', '0-9', '\\d', '\\w', '\\s', 'a-z', '\\u0400-\\u04ff', '\\W', '\\D']) if k < 3 else self.ch().replace('-', '\\-'))
        if self.unicode and self.r.random() < 0.3:
            parts.append('\\u{1f0a1}-\\u{1f0ae}')
        return '[' + ('^' if self.r.random() < 0.3 else '') + ''.join(parts) + ']'

    def atom(self, d):
        k = self.r.randint(0, 19)
        if k < 7 or d <= 0:
            return self.ch()
        if k < 10:
            return self.cls()
        if k == 10:
            return '.'
        if k == 11:
            return self.r.choice(['\\d', '\\w', '\\s', '\\S', '\\D', '\\W'])
        if k == 12:
            return self.r.choice(['^', '$', '\\b', '\\B'])
        if k in (13, 14, 15):
            self.groups += 1
            return '(' + self.alt(d - 1) + ')'
        if k == 16:
            return '(?:' + self.alt(d - 1) + ')'
        if k == 17 and self.groups:
            return '\\' + str(self.r.randint(1, self.groups))
        if k == 18:
            return self.r.choice(['(?=', '(?!', '(?<=', '(?<!']) + self.alt(d - 1) + ')'
        return '(?:' + self.alt(d - 1) + ')'

    def term(self, d):
        a = self.atom(d)
        if self.r.random() < 0.4 and a not in ('^', '$', '\\b', '\\B') and not a.startswith(('(?=', '(?!', '(?<')):
            q = self.r.choice(['*', '+', '?', '{2}', '{1,3}', '{0,2}', '{2,}', '{0}', '{3,5}'])
            a += q + ('?' if self.r.random() < 0.3 else '')
        return a

    def seq(self, d):
        return ''.join(self.term(d) for _ in range(self.r.randint(1, 4)))

    def alt(self, d):
        return '|'.join(self.seq(d) for _ in range(self.r.choice([1, 1, 2, 2, 3, 4])))

    def text(self):
        units = ['a', 'b', 'c', 'A', 'B', '1', '2', ' ', '_', '-', '\\n', '\\u00e9', '\\u0411', '\\u2028', 'K', 's', '\\u212a', '\\u{1f0a1}', '\\u{1f0a2}', '\\ud83c', '\\udca1']
        n = self.r.choice([0, 1, 2, 3, 5, 8, 13, 21, 40, 90])
        return ''.join(self.r.choice(units[:7]) if self.r.random() < 0.8 else self.r.choice(units) for _ in range(n))

    def program(self):
        out = ['var out = [];', 'function show(m) { return m ? m.index + ":" + Array.prototype.map.call(m, function (x) { return JSON.stringify(x); }).join() : "-"; }']
        for k in range(12):
            self.groups = 0
            flags = ''.join(f for f in 'gimsuy' if self.r.random() < (0.35 if f in 'gi' else 0.2))
            self.unicode = 'u' in flags
            pat = self.alt(3)
            texts = [self.text() for _ in range(8)]
            out.append('(function () { var re; try { re = new RegExp("%s", "%s"); } catch (e) { out.push("bad"); return; }' % (pat.replace('\\', '\\\\'), flags))
            out.append('var t = [%s];' % ', '.join('"%s"' % t for t in texts))
            out.append('for (var r = 0; r < 4; r++) for (var i = 0; i < t.length; i++) { re.lastIndex = r; var m = re.exec(t[i]); out.push(show(m) + "/" + re.lastIndex);'
                       ' re.lastIndex = 0; out.push(re.test(t[i]), t[i].replace(re, "<$&>")); if (r == 3) out.push(t[i].split(re, 6).map(JSON.stringify).join()); } })();')
        out.append('console.log(out.join("|"));')
        return '\n'.join(out)

def run(tool, jit, path):
    e = dict(os.environ, SCRIPT_CORE='vm')
    if not jit:
        e['SCRIPT_VM_TIER1'] = '0'
    try:
        p = subprocess.run([tool, path], capture_output=True, text=True, errors='replace', timeout=20, env=e)
    except subprocess.TimeoutExpired:
        return 'timeout', ''
    return p.returncode, p.stdout + p.stderr

def one(args):
    tool, seed, d = args
    path = os.path.join(d, f'r{seed}.js')
    with open(path, 'w') as f:
        f.write(Gen(seed).program())
    a, b = run(tool, False, path), run(tool, True, path)
    if a[0] == 'timeout' or a == b:
        os.unlink(path)
        return None if a[0] == 'timeout' else 'ok'
    x, y = a[1].split('|'), b[1].split('|')
    k = next((k for k in range(min(len(x), len(y))) if x[k] != y[k]), -1)
    return f'DISAGREE seed {seed} {path}: rc {a[0]} vs {b[0]}, item {k}: {x[k][:90]!r} vs {y[k][:90]!r}'

def main():
    tool, count, start = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
    d = tempfile.mkdtemp(prefix='rxfuzz')
    ran = bad = 0
    with ThreadPoolExecutor(max_workers=os.cpu_count()) as pool:
        for r in pool.map(one, [(tool, s, d) for s in range(start, start + count)]):
            if r is None:
                continue
            ran += 1
            if r != 'ok':
                bad += 1
                print(r)
    print(f'{ran} programs, {bad} disagreements, kept in {d}')
    sys.exit(1 if bad else 0)

main()
