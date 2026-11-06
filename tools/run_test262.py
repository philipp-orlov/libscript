#!/usr/bin/env python3
"""Run test262 through an engine CLI and record one JSON line per test.

usage: run_test262.py {libscript|jsc} <out.jsonl> [path-prefix ...]
Each test runs once: in strict mode if flagged onlyStrict, otherwise sloppy
(test262 asks for both; one mode keeps the numbers comparable across engines,
and libscript has no strict/sloppy split anyway).
"""
import json, os, re, resource, signal, subprocess, sys, tempfile, time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(os.environ.get('JSC_COMPARE', '.'))
T262 = Path(os.environ.get('T262', str(ROOT / 'test262')))
LIBSCRIPT = os.environ.get('LS', 'build/tools/libscript')
JSC = str(ROOT / 'jsc-pkg/root/usr/bin/jsc')
TIMEOUT = 20
MEM = 6 << 30
META = re.compile(r'/\*---(.*?)---\*/', re.S)
FS = T262 / 'harness'
_cache = {}

def harness(name):
    if name not in _cache:
        _cache[name] = (FS / name).read_text(encoding='utf-8')
    return _cache[name]

def parse_meta(src):
    m = META.search(src)
    if not m:
        return {}
    body, out, key = m.group(1), {}, None
    lines = body.split('\n')
    i = 0
    while i < len(lines):
        ln = lines[i]; i += 1
        k = re.match(r'^\s*(flags|includes|features):\s*\[(.*?)\]\s*$', ln)
        if k:
            out[k.group(1)] = [x.strip() for x in k.group(2).split(',') if x.strip()]
            continue
        k = re.match(r'^\s*(flags|includes|features):\s*$', ln)
        if k:
            items = []
            while i < len(lines) and re.match(r'^\s*-\s', lines[i]):
                items.append(lines[i].split('-', 1)[1].strip()); i += 1
            out[k.group(1)] = items
            continue
        k = re.match(r'^\s*negative:\s*$', ln)
        if k:
            neg = {}
            while i < len(lines) and re.match(r'^\s+\w+:', lines[i]):
                a, b = lines[i].strip().split(':', 1); neg[a] = b.strip(); i += 1
            out['negative'] = neg
    return out

def limits():
    resource.setrlimit(resource.RLIMIT_AS, (MEM, MEM))
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    os.setsid()

SHIM_LS = 'if (typeof print === "undefined") { var print = function(s) { console.log(String(s)); }; }\n'

def run_one(path, engine, tmpdir):
    rel = str(path.relative_to(T262))
    src = path.read_text(encoding='utf-8', errors='surrogateescape')
    meta = parse_meta(src)
    flags = set(meta.get('flags', []))
    neg = meta.get('negative')
    is_async = 'async' in flags
    is_module = 'module' in flags
    parts = []
    if 'raw' not in flags:
        if 'onlyStrict' in flags:
            parts.append('"use strict";\n')
        if engine == 'libscript':
            parts.append(SHIM_LS)
        parts += [harness('assert.js'), harness('sta.js')]
        if is_async:
            parts.append(harness('doneprintHandle.js'))
        for inc in meta.get('includes', []):
            try:
                parts.append(harness(inc))
            except FileNotFoundError:
                pass
    parts.append(src)
    suffix = '.mjs' if is_module and engine == 'jsc' else '.js'
    # JSC re-run: keep relative imports working by writing next to the test.
    tmp = (path.parent / ('__t262_' + path.stem + suffix)) if os.environ.get('T262_NEXT') else Path(tmpdir) / (rel.replace('/', '_') + suffix)
    tmp.write_text(''.join(parts), encoding='utf-8', errors='surrogateescape')
    if engine == 'jsc':
        cmd = [JSC, '--useDollarVM=0'] + os.environ.get('T262_FLAGS', '').split() + (['-m'] if is_module else []) + [str(tmp)]
    else:
        cmd = [LIBSCRIPT, str(tmp)]
    t0 = time.monotonic()
    timed_out = False
    try:
        p = subprocess.run(cmd, capture_output=True, timeout=TIMEOUT, preexec_fn=limits, cwd=tmpdir)
        rc, out, err = p.returncode, p.stdout.decode('utf-8', 'replace'), p.stderr.decode('utf-8', 'replace')
    except subprocess.TimeoutExpired as e:
        timed_out = True
        rc, out, err = -999, (e.stdout or b'').decode('utf-8', 'replace'), (e.stderr or b'').decode('utf-8', 'replace')
    dt = time.monotonic() - t0
    tmp.unlink(missing_ok=True)
    text = out + err
    compile_fail = (engine == 'libscript' and re.search(r'^libscript: .*?:\d+: ', err, re.M) is not None)
    if engine == 'libscript' and not compile_fail and rc == 1 and 'Unhandled exception' not in err and err.startswith('libscript:'):
        compile_fail = True
    status, why = 'fail', ''
    if timed_out:
        status, why = 'timeout', ''
    elif rc < 0:
        status, why = 'crash', 'signal %d' % -rc
    elif neg:
        want = neg.get('type', '')
        phase = neg.get('phase', '')
        if rc == 0:
            why = 'expected %s, none raised' % want
        elif engine == 'libscript' and compile_fail:
            if want == 'SyntaxError' and phase in ('parse', 'resolution'):
                status = 'pass'
            else:
                why = 'compile: ' + err.strip().split('\n')[0]
        elif re.search(r'\b%s\b' % re.escape(want), text):
            status = 'pass'
        else:
            why = 'wrong error: ' + err.strip().split('\n')[0][:200]
    elif rc != 0:
        why = err.strip().split('\n')[0][:300] or out.strip().split('\n')[-1][:300]
        if compile_fail:
            why = 'compile: ' + why
    elif is_async:
        if 'Test262:AsyncTestComplete' in out:
            status = 'pass'
        elif 'Test262:AsyncTestFailure' in out:
            why = 'async failure'
        else:
            why = 'async test never completed'
    else:
        status = 'pass'
    return {'t': rel, 's': status, 'why': why, 'dt': round(dt, 4), 'neg': bool(neg), 'async': is_async,
            'module': is_module, 'features': meta.get('features', []), 'includes': meta.get('includes', []),
            'compile': compile_fail}

def main():
    engine, outp = sys.argv[1], sys.argv[2]
    prefixes = sys.argv[3:] or ['test/built-ins', 'test/language', 'test/annexB']
    files = []
    for pre in prefixes:
        p = T262 / pre
        files += [p] if p.is_file() else sorted(x for x in p.rglob('*.js') if '_FIXTURE' not in x.name)
    if os.environ.get('T262_ONLY'):
        files = [T262 / l.strip() for l in open(os.environ['T262_ONLY'])]
    print(len(files), 'tests', flush=True)
    tmpdir = tempfile.mkdtemp(prefix='t262-', dir='/dev/shm')
    done = 0; t0 = time.time()
    with open(outp, 'w') as f, ThreadPoolExecutor(16) as ex:
        for r in ex.map(lambda p: run_one(p, engine, tmpdir), files):
            f.write(json.dumps(r) + '\n'); done += 1
            if done % 5000 == 0:
                print(done, round(time.time() - t0), flush=True)
    os.rmdir(tmpdir)
    print('done', done, round(time.time() - t0), flush=True)

main()
