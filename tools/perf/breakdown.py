#!/usr/bin/env python3
"""breakdown.py DIR SUMFILE: time by category for each workload profiled by breakdown.sh, and the geometric mean of the
ratios in SUMFILE (a vsjsc.py output) if a category took no time, or half its time. Compiled code is found by the perf
map that SCRIPT_VM_PERFMAP=1 writes (/tmp/perf-PID.map, so read the profiles on the machine that made them)."""
import re, sys, glob, collections, math
D = sys.argv[1]
cats = [
 ('jit:ssa',      lambda d, s: s.startswith('js3:')),
 ('jit:t2',       lambda d, s: s.startswith('js2:')),
 ('jit:base',     lambda d, s: s.startswith('js:')),
 ('jit:other',    lambda d, s: False),
 ('kernel',       lambda d, s: 'kernel' in d),
 ('loader',       lambda d, s: 'ld-linux' in d),
 ('mem:cycles',   r'collectCycles|forEachSlot|markGray|scanGray|collectWhite|trial|Cycle|cycle|sweep|Sweep|children\(|markFrom|clearReferences'),
 ('mem:scan',     r'scanZct|syncShadow|countRoots|collectYoung|zct|Zct|rootsBegin|scanAtHost|youngMark|markYoung'),
 ('mem:free',     r'destroy|Destroy|freeCell|countFreed|releaseSlow|release\b|freePending|pending|::free|_int_free|cfree|operator delete|freePayload|unlinkCell|retire'),
 ('mem:alloc',    r'alloc|Alloc|makeOld|addYoung|LocalHeap|SizeClassPool|SlabPool|payload|_int_malloc|malloc|operator new|countOld|chargeGrowth|growSlots|reserve|Vec<'),
 ('gen/async',    r'[Gg]enerator|resume|Resume|suspend|Suspend|Promise|promise|settle|[Jj]ob|async|Async|Await|await|IterNext|jitGen|Iterator|iterator'),
 ('regexp',       r'regex|RegExp|Regexp|regexp|Matcher|matcher'),
 ('parse',        r'Parser|Lexer|lexer|parser|Token|scan[A-Z]|ecmascript::'),
 ('bytecompile',  r'Compiler::|compileFunction|lazyEligible|capturedBelow|resolveIn|Scope|scope'),
 ('jitcompile',   r'Baseline::|Ssa|ssa::|opt::|Assembler|A64|asm|Emitter|regalloc|plan[A-Z]|compileTier|mergeInline|selfInline|commitCode|CodeRegion'),
 ('strings',      r'String|string|concat|Concat|Rope|rope|flatten|numberToString|toPropertyKey|atomize|Atom|utf|UTF|hashBytes|memcpy|memmove|memcmp|memset|strlen|Char|char16|u16|StringTable'),
 ('props/ic',     r'findOwn|[gG]etProp|[sS]etProp|[pP]utProp|Shape|shape|mega|Mega|jitGetElem|jitPutElem|GetElem|PutElem|addProperty|Dictionary|rareOf|KeyIndex|defineOwn|getOwnProperty|forIn|ownKeys|lookup|Lookup|Inline[Cc]ache|elemCached|learn|Validity|validity|proto'),
 ('calls',        r'callPrepare|callInternal|VM::call|enterTier1|JitCall|jitCall|callPoly|construct|Construct|newThis|NewThis|invoke|callHook|Native|native|Bound|apply'),
 ('interp',       r'runImpl|Interp|interp|stepInstruction|JitStep|jitStep|scriptVmJit'),
 ('json',         r'JSON|Json|json|stringify|Stringify'),
 ('collections',  r'Map|Set|TableIter|Weak'),
 ('arrays',       r'Array|array|sort|Sort|Elems|elems'),
 ('bigint',       r'[Bb]ig[Ii]nt|limb|Limb'),
 ('numbers',      r'toNumber|ToNumber|toInt|double|Double|pow|fmod|Math|math|sqrt|sin|cos|exp|log|dtoa|Number'),
]
comp = [(n, re.compile(r) if isinstance(r, str) else r) for n, r in cats]
def classify(d, s):
    for n, r in comp:
        if callable(r):
            if r(d, s): return n
        elif r.search(s): return n
    return 'other'
ratios = {}
for l in open(sys.argv[2]):
    m = re.match(r'(\S+)\s+later\s+([\d.]+)x', l)
    if m: ratios[m.group(1)] = float(m.group(2))
rows = {}
others = collections.Counter()
import subprocess, bisect, os
for f in sorted(glob.glob(D + '/*.data')):
    n = f.split('/')[-1][:-5]
    out = subprocess.run(['perf', 'script', '-i', f, '-F', 'pid,ip,sym,dso,period'], capture_output=True, text=True).stdout
    samples = []
    pids = collections.Counter()
    for l in out.splitlines():
        m = re.match(r'\s*(\d+)\s+(\d+)\s+([0-9a-f]+)\s+(.*?)\s+\((.*)\)\s*$', l)
        if not m: continue
        pids[m.group(1)] += 1
        samples.append((m.group(1), int(m.group(3), 16), m.group(4), m.group(5)))
    pid = pids.most_common(1)[0][0]
    ranges = []
    if os.path.exists('/tmp/perf-%s.map' % pid):
        for l in open('/tmp/perf-%s.map' % pid):
            a_, s_, nm = l.rstrip('\n').split(' ', 2)
            ranges.append((int(a_, 16), int(s_, 16), nm.split(':')[0] + ':'))
    ranges.sort(); starts = [r[0] for r in ranges]
    acc = collections.Counter(); oth = collections.Counter()
    for p, ip, sym, dso in samples:
        if p != pid: continue
        c = None
        if 'memfd' in dso or dso.startswith('[unknown') or sym == '[unknown]':
            i = bisect.bisect_right(starts, ip) - 1
            if i >= 0 and ip < ranges[i][0] + ranges[i][1]:
                c = classify(dso, ranges[i][2])
            else:
                c = 'jit:other'
        else:
            c = classify(dso, sym)
        acc[c] += 1
        oth[(c, sym[:60])] += 1
    tot = sum(acc.values()) or 1
    for k in acc: acc[k] *= 100.0 / tot
    rows[n] = acc
    for k, v in oth.items(): others[k] += v * 100.0 / tot / 41
names = [c[0] for c in cats] + ['other']
print('%-28s' % 'workload' + ''.join('%8s' % c.split(':')[-1][:7] for c in names))
for n, a in sorted(rows.items(), key=lambda x: -ratios.get(x[0], 0)):
    print('%-28s' % n + ''.join('%8.1f' % a[c] for c in names))
ws = [n for n in rows if n in ratios]
g = math.exp(sum(math.log(ratios[n]) for n in ws) / len(ws))
print('\ngeomean %.3f over %d; bound if a category took no time (and its geomean gain):' % (g, len(ws)))
res = []
for c in names:
    gb = math.exp(sum(math.log(ratios[n] * max(0.01, 1 - rows[n][c] / 100)) for n in ws) / len(ws))
    mean = sum(rows[n][c] for n in ws) / len(ws)
    res.append((gb, c, mean))
for gb, c, mean in sorted(res):
    print('  %-12s mean share %5.1f%%  geomean -> %.3f  (%.1f%%)' % (c, mean, gb, 100 * (gb / g - 1)))
groups = {'memory': ['mem:alloc','mem:free','mem:scan','mem:cycles'], 'compiled': ['jit:ssa','jit:t2','jit:base','jit:other'],
 'gen+base': ['gen/async','jit:base'], 'str+re': ['strings','regexp'], 'front': ['parse','bytecompile','jitcompile'],
 'glue': ['props/ic','calls','interp','arrays','numbers','json','collections','other']}
print()
for gname, cs in groups.items():
    gb = math.exp(sum(math.log(ratios[n] * max(0.01, 1 - sum(rows[n][c] for c in cs) / 100)) for n in ws) / len(ws))
    gh = math.exp(sum(math.log(ratios[n] * (1 - 0.5*sum(rows[n][c] for c in cs) / 100)) for n in ws) / len(ws))
    print('  group %-9s mean %5.1f%%  all gone -> %.3f   half gone -> %.3f' % (gname, sum(sum(rows[n][c] for c in cs) for n in ws)/len(ws), gb, gh))
print()
for cat in ['mem:alloc','mem:free','mem:scan','mem:cycles','strings','other','props/ic','calls','gen/async']:
    print(cat)
    for (c,k), v in sorted(((k,v) for k,v in others.items() if k[0]==cat), key=lambda x:-x[1])[:8]:
        print('  %6.2f  %s' % (v, k))
