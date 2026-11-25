import re,sys,glob,collections
cats=[
 ('memory', r'destroy|freeZct|collectCycles|collectYoung|syncShadow|countRoots|clearReferences|children\(|forEachSlot|allocCell|allocPayload|freePayload|LocalHeap|payload_cache|destroyUnreferenced|destroyCell|link\(|unlink|countFreed|_int_free|_int_malloc|malloc|free$|cfree|operator new|operator delete|SlabPool|makeOld|zctPush|releaseCode'),
 ('regexp', r'regexp|RegExp|Matcher|regex|exec\b|backtrack'),
 ('strings', r'String|string|concat|Rope|flatten|numberToString|toPropertyKey|ascii|utf|hashBytes|memcpy|memmove|memcmp|strlen|Char'),
 ('props', r'findOwn|getProperty|setProperty|Shape::|rootShapeFor|megaLearn|megaGet|IcMiss|jitGetProp|jitPutProp|jitGetElem|jitPutElem|addProperty|Dictionary|indexOf|rareOf|proxyOf|KeyIndex|tableOf|defineOwn|getOwnProperty|forIn|ownKeys|getPropertyFrom|lookup|transition'),
 ('calls', r'callPrepare|callInternal|VM::call|enterTier1|scriptVmJitCall|callPoly|construct|jitCall|newThis|invoke|Native|native'),
 ('generators', r'Gen|generator|resume|suspend|iterator|Iterator|yield|Promise|promise|settle|enqueueJob|runJobs|async'),
 ('compile', r'compile|Compiler|Parser|parse|lex|Lexer|emit|A64::|Assembler|opt::|ssa|Ssa|merge|tier2|Tier2|tier1|Tier1|plan|analy'),
 ('bigint', r'[Bb]ig[Ii]nt|limb'),
 ('numbers', r'toNumber|toInt|double|pow|fmod|Math|math|sqrt|sin|cos|exp|log'),
 ('interp', r'runImpl|interp|step|Step'),
]
def classify(dso,sym):
    if dso.startswith('[k') or 'kernel' in dso: return 'kernel'
    if sym.startswith('0x') or 'perf-' in dso or dso.startswith('[unknown') or dso.startswith('memfd') or 'jit' in dso.lower(): return 'compiled code'
    for c,rx in cats:
        if re.search(rx,sym): return c
    return 'runtime other'
out={}
for f in sorted(glob.glob(sys.argv[1]+'/*.txt')):
    n=f.split('/')[-1][:-4]
    acc=collections.Counter(); other=collections.Counter()
    for l in open(f):
        m=re.match(r'\s*([\d.]+)%\s+(\S+)\s+\[[.k]\]\s+(.*)',l)
        if not m: continue
        p=float(m.group(1)); dso=m.group(2); sym=m.group(3).strip()
        c=classify(dso,sym); acc[c]+=p
        if c=='runtime other': other[sym[:60]]+=p
    out[n]=(acc,other)
    top=', '.join('%s %.0f'%(c,v) for c,v in acc.most_common(6))
    print('%-28s %s'%(n,top))
