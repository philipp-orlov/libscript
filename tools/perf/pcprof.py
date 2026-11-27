#!/usr/bin/env python3
"""pcprof.py: reports from the sample files that pcprof.sh writes (pcprof.so; no perf needed).

  pcprof.py flat FILE.pc [TOP]        time by function; compiled code by its perf-map name
  pcprof.py callers FILE.pc REGEX     who calls the functions that match (by the link register of each sample)
  pcprof.py jit FILE.pc [TOP]         compiled code by tier, opcode and kind (as t2prof.py; needs the T1MAP/T2MAP files)
  pcprof.py cats DIR [SUMFILE]        every workload by category (as breakdown.py); SUMFILE is a vsjsc.py output
"""
import bisect, collections, glob, math, os, pickle, re, subprocess, sys

CATS = [
 ('jit:ssa',      lambda s: s.startswith('js3:')),
 ('jit:t2',       lambda s: s.startswith('js2:')),
 ('jit:base',     lambda s: s.startswith('js:')),
 ('jit:other',    lambda s: s == '[jit]'),
 ('kernel',       lambda s: s.startswith('[vdso]') or s.startswith('[kernel]')),
 ('mem:cycles',   r'collectCycles|forEachSlot|markGray|scanGray|collectWhite|trial|Cycle|cycle|sweep|Sweep|children\(|markFrom|clearReferences'),
 ('mem:scan',     r'scanZct|syncShadow|countRoots|collectYoung|zct|Zct|rootsBegin|scanAtHost|youngMark|markYoung'),
 ('mem:free',     r'destroy|Destroy|freeCell|countFreed|releaseSlow|release\b|freePending|pending|::free|_int_free|cfree|operator delete|freePayload|unlinkCell|retire|\bfree\b'),
 ('mem:alloc',    r'alloc|Alloc|makeOld|addYoung|LocalHeap|SizeClassPool|SlabPool|payload|_int_malloc|malloc|operator new|countOld|chargeGrowth|growSlots|reserve|Vec<'),
 ('gen/async',    r'[Gg]enerator|resume|Resume|suspend|Suspend|Promise|promise|settle|[Jj]ob|async|Async|Await|await|IterNext|jitGen|Iterator|iterator'),
 ('regexp',       r'regex|RegExp|Regexp|regexp|Matcher|matcher'),
 ('parse',        r'Parser|Lexer|lexer|parser|Token|scan[A-Z]|ecmascript::'),
 ('bytecompile',  r'Compiler::|compileFunction|lazyEligible|capturedBelow|capturedWalk|resolveIn|Scope|scope'),
 ('jitcompile',   r'Baseline::|Ssa|ssa::|opt::|lir::|Assembler|A64|asm|Emitter|regalloc|plan[A-Z]|compileTier|mergeInline|selfInline|commitCode|CodeRegion'),
 ('strings',      r'String|string|concat|Concat|Rope|rope|flatten|numberToString|toPropertyKey|atomize|Atom|utf|UTF|hashBytes|memcpy|memmove|memcmp|memset|memchr|strlen|Char|char16|u16|StringTable'),
 ('props/ic',     r'findOwn|[gG]etProp|[sS]etProp|[pP]utProp|Shape|shape|mega|Mega|jitGetElem|jitPutElem|GetElem|PutElem|addProperty|Dictionary|rareOf|KeyIndex|defineOwn|getOwnProperty|forIn|ownKeys|lookup|Lookup|Inline[Cc]ache|elemCached|learn|Validity|validity|proto'),
 ('calls',        r'callPrepare|callInternal|VM::call|enterTier1|JitCall|jitCall|callPoly|construct|Construct|newThis|NewThis|invoke|callHook|Native|native|Bound|apply'),
 ('interp',       r'runImpl|Interp|interp|stepInstruction|JitStep|jitStep|scriptVmJit'),
 ('json',         r'JSON|Json|json|stringify|Stringify'),
 ('collections',  r'Map|Set|TableIter|Weak'),
 ('arrays',       r'Array|array|sort|Sort|Elems|elems'),
 ('bigint',       r'[Bb]ig[Ii]nt|limb|Limb'),
 ('numbers',      r'toNumber|ToNumber|toInt|double|Double|pow|fmod|Math|math|sqrt|sin|cos|exp|log|dtoa|Number'),
]
COMP = [(n, re.compile(r) if isinstance(r, str) else r) for n, r in CATS]
GROUPS = {'memory': ['mem:alloc', 'mem:free', 'mem:scan', 'mem:cycles'], 'compiled': ['jit:ssa', 'jit:t2', 'jit:base', 'jit:other'],
          'gen+base': ['gen/async', 'jit:base'], 'str+re': ['strings', 'regexp'], 'front': ['parse', 'bytecompile', 'jitcompile'],
          'glue': ['props/ic', 'calls', 'interp', 'arrays', 'numbers', 'json', 'collections', 'other']}


def classify(sym):
    for n, r in COMP:
        if callable(r):
            if r(sym): return n
        elif r.search(sym): return n
    return 'other'


_syms = {}
def symbols(path):
    """Sorted (address, name) of the text symbols of a binary or a shared library; cached beside the samples."""
    if path in _syms: return _syms[path]
    try:
        st = os.stat(path)
        cache = '/tmp/pcprof-syms-%s-%d-%d.pkl' % (os.path.basename(path), st.st_size, int(st.st_mtime))
        if os.path.exists(cache):
            _syms[path] = pickle.load(open(cache, 'rb'))
            return _syms[path]
        out = subprocess.run(['nm', '-C', '--defined-only', '-n', path], capture_output=True, text=True).stdout
        if not out.strip():
            out = subprocess.run(['nm', '-D', '-C', '--defined-only', '-n', path], capture_output=True, text=True).stdout
        t = []
        for l in out.splitlines():
            p = l.split(' ', 2)
            if len(p) == 3 and p[1] in 'tTwWiI': t.append((int(p[0], 16), p[2]))
        r = ([a for a, _ in t], [n for _, n in t])
        pickle.dump(r, open(cache, 'wb'))
    except OSError:
        r = ([], [])
    _syms[path] = r
    return r


class Profile:
    def __init__(self, path):
        self.maps, self.pcs, self.lrs = [], [], []
        for line in open(path):
            if line.startswith('M '):
                f = line.split()
                lo, hi = [int(x, 16) for x in f[1].split('-')]
                self.maps.append((lo, hi, int(f[3], 16), f[6] if len(f) > 6 else '[anon]'))
            elif line.startswith('P '):
                pass
            else:
                a, b = line.split()
                self.pcs.append(int(a, 16)); self.lrs.append(int(b, 16))
        self.maps.sort(); self.mlo = [m[0] for m in self.maps]
        base = path[:-3]
        # Code memory is used again after code is freed: the last code written at an address is the one named.
        last = {}
        if os.path.exists(base + '.map'):
            for l in open(base + '.map'):
                a, s, nm = l.rstrip('\n').split(' ', 2)
                last[int(a, 16)] = (int(a, 16), int(s, 16), nm)
        self.ranges = sorted(last.values()); self.rlo = [r[0] for r in self.ranges]
        self.base = base
        self.memo = {}

    def jit(self, pc):
        i = bisect.bisect_right(self.rlo, pc) - 1
        if i >= 0 and pc < self.ranges[i][0] + self.ranges[i][1]: return self.ranges[i]
        return None

    def sym(self, pc):
        s = self.memo.get(pc)
        if s is not None: return s
        r = self.jit(pc)
        if r: s = r[2]
        else:
            i = bisect.bisect_right(self.mlo, pc) - 1
            s = '[jit]'
            if i >= 0 and pc < self.maps[i][1]:
                lo, hi, off, name = self.maps[i]
                if name.startswith('/') and 'memfd' not in name:
                    a, n = symbols(name)
                    j = bisect.bisect_right(a, pc - lo + off) - 1
                    s = n[j] if j >= 0 else '[' + os.path.basename(name) + ']'
                elif name.startswith('['): s = name
            elif i < 0 or pc >= self.maps[i][1]:
                s = '[jit]' if self.ranges and self.rlo[0] - (1 << 30) < pc < self.rlo[-1] + (1 << 30) else '[unknown]'
        self.memo[pc] = s
        return s


def flat(path, top):
    p = Profile(path)
    c = collections.Counter(p.sym(pc) for pc in p.pcs)
    tot = len(p.pcs) or 1
    cat = collections.Counter()
    for s, v in c.items(): cat[classify(s)] += v
    print('%d samples; ' % tot + ', '.join('%s %.1f%%' % (k, 100.0 * v / tot) for k, v in cat.most_common(12)))
    for k, v in c.most_common(top): print('%6.2f%%  %s' % (100.0 * v / tot, k[:150]))


def callers(path, rx):
    p = Profile(path); r = re.compile(rx)
    c = collections.Counter(); n = 0
    for pc, lr in zip(p.pcs, p.lrs):
        if r.search(p.sym(pc)):
            c[p.sym(lr)] += 1; n += 1
    tot = len(p.pcs) or 1
    print('%d samples match (%.2f%% of all)' % (n, 100.0 * n / tot))
    for k, v in c.most_common(25): print('%6.2f%%  %s' % (100.0 * v / tot, k[:150]))


def load_tmap(path):
    m = collections.defaultdict(dict)
    try:
        for l in open(path):
            q = l.split()
            if len(q) < 5: continue
            rx = int(q[0], 16); off = int(q[4])
            m[rx][off] = (off, q[1], q[2], q[3], int(q[5]) if len(q) > 5 else -1, q[6] if len(q) > 6 else '-')
    except FileNotFoundError:
        pass
    return {rx: sorted(v.values()) for rx, v in m.items()}


def jit(path, top):
    p = Profile(path)
    maps = {'js': load_tmap(p.base + '.t1map'), 'js3': load_tmap(p.base + '.t2map')}
    total = len(p.pcs) or 1
    tier, rows, ops, cold = collections.Counter(), collections.Counter(), collections.Counter(), collections.Counter()
    interp = 0
    for pc in p.pcs:
        r = p.jit(pc)
        if not r:
            if 'runImpl' in p.sym(pc): interp += 1
            continue
        rx, _, nm = r
        t = nm.split(':')[0]
        tier[t] += 1
        im = maps.get(t, {}).get(rx)
        if not im: continue
        off = pc - rx
        offs = [x[0] for x in im]
        j = bisect.bisect_right(offs, off) - 1
        if j < 0:
            rows[(t, 'prologue', '', -1, '', '')] += 1
            continue
        o, name, line, op, idx, kind = im[j]
        rows[(t, name, line, idx, op, kind)] += 1
        ops[(t, op, kind)] += 1
        if off > im[-1][0] + 64: cold[t] += 1
    pct = lambda v: 100.0 * v / total
    names = {'js': 'tier 1', 'js2': 'tier 2', 'js3': 'SSA'}
    comp = sum(tier.values())
    print('samples %d' % total)
    print('tiers: interpreter %.1f%%, ' % pct(interp) + ', '.join('%s %.1f%%' % (names.get(t, t), pct(v)) for t, v in tier.most_common()) +
          ', runtime and other %.1f%%' % (100 - pct(interp) - pct(comp)))
    for t in ('js3', 'js'):
        if not tier[t]: continue
        print('\n%s: cold paths and exit stubs %.1f%% of the tier' % (names[t], 100.0 * cold[t] / tier[t]))
        bykind = collections.Counter()
        for (tt, op, kind), v in ops.items():
            if tt == t: bykind[kind] += v
        if t == 'js3':
            print('  by kind: ' + ', '.join('%s %.1f%%' % (k, 100.0 * v / tier[t]) for k, v in bykind.most_common()))
        print('  by opcode (share of the tier):')
        for (tt, op, kind), v in ops.most_common():
            if tt == t and 100.0 * v / tier[t] >= 1.0:
                print('    %-16s %s %5.1f%%' % (op, kind, 100.0 * v / tier[t]))
    print('\ntop rows (share of all samples):')
    for (t, name, line, idx, op, kind), v in rows.most_common(top):
        print('  %5.2f%% %-6s %-24s line %-6s @%-5s %-16s %s' % (pct(v), names.get(t, t), name[:24], line, idx, op, kind))


def cats(d, sumfile):
    ratios = {}
    if sumfile:
        for l in open(sumfile):
            m = re.match(r'(\S+)\s+later\s+([\d.]+)x', l)
            if m: ratios[m.group(1)] = float(m.group(2))
    rows = {}; others = collections.Counter()
    files = sorted(glob.glob(d + '/*.pc'))
    for f in files:
        n = os.path.basename(f)[:-3]
        p = Profile(f)
        acc = collections.Counter(); tot = len(p.pcs) or 1
        for s, v in collections.Counter(p.sym(pc) for pc in p.pcs).items():
            c = classify(s); acc[c] += v
            others[(c, s[:70] if not s.startswith('js') else s.split(':')[0])] += v * 100.0 / tot / len(files)
        for k in acc: acc[k] *= 100.0 / tot
        rows[n] = acc
    names = [c[0] for c in CATS] + ['other']
    print('%-28s' % 'workload' + ''.join('%8s' % c.split(':')[-1][:7] for c in names))
    for n, a in sorted(rows.items(), key=lambda x: -ratios.get(x[0], 0)):
        print('%-28s' % n + ''.join('%8.1f' % a[c] for c in names))
    ws = [n for n in rows if n in ratios]
    if ws:
        g = math.exp(sum(math.log(ratios[n]) for n in ws) / len(ws))
        print('\ngeomean %.3f over %d; bound if a category took no time (and its geomean gain):' % (g, len(ws)))
        res = []
        for c in names:
            gb = math.exp(sum(math.log(ratios[n] * max(0.01, 1 - rows[n][c] / 100)) for n in ws) / len(ws))
            res.append((gb, c, sum(rows[n][c] for n in ws) / len(ws)))
        for gb, c, mean in sorted(res):
            print('  %-12s mean share %5.1f%%  geomean -> %.3f  (%.1f%%)' % (c, mean, gb, 100 * (gb / g - 1)))
        print()
        for gname, cs in GROUPS.items():
            sh = lambda n: sum(rows[n][c] for c in cs) / 100
            gb = math.exp(sum(math.log(ratios[n] * max(0.01, 1 - sh(n))) for n in ws) / len(ws))
            gh = math.exp(sum(math.log(ratios[n] * (1 - 0.5 * sh(n))) for n in ws) / len(ws))
            print('  group %-9s mean %5.1f%%  all gone -> %.3f   half gone -> %.3f' % (gname, 100 * sum(sh(n) for n in ws) / len(ws), gb, gh))
    else:
        print()
        for c in names: print('  %-12s mean share %5.1f%%' % (c, sum(r[c] for r in rows.values()) / max(1, len(rows))))
    print()
    for cat in ['mem:alloc', 'mem:free', 'mem:scan', 'mem:cycles', 'strings', 'regexp', 'other', 'props/ic', 'calls', 'gen/async', 'arrays', 'numbers', 'interp']:
        print(cat)
        for (c, k), v in sorted(((k, v) for k, v in others.items() if k[0] == cat), key=lambda x: -x[1])[:8]:
            print('  %6.2f  %s' % (v, k))


mode = sys.argv[1] if len(sys.argv) > 1 else ''
if mode == 'flat': flat(sys.argv[2], int(sys.argv[3]) if len(sys.argv) > 3 else 30)
elif mode == 'callers': callers(sys.argv[2], sys.argv[3])
elif mode == 'jit': jit(sys.argv[2], int(sys.argv[3]) if len(sys.argv) > 3 else 30)
elif mode == 'cats': cats(sys.argv[2], sys.argv[3] if len(sys.argv) > 3 else None)
else: print(__doc__)
