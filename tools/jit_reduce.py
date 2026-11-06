#!/usr/bin/env python3
"""Shrink a program on which the interpreter and native code disagree, by removing lines while they still do.
usage: jit_reduce.py <libscript tool> <program.cs> [flag]"""
import os, subprocess, sys
tool, path = sys.argv[1], sys.argv[2]
flag = sys.argv[3] if len(sys.argv) > 3 else '--native'
ext = path.rsplit('.', 1)[-1]
scratch = '/tmp/jit_reduce.' + ext
def run(lines, flags):
    open(scratch, 'w').write('\n'.join(lines))
    env = dict(os.environ)
    # A flag of the form env:SCRIPT_JIT=tiered:1 is for a build with SCRIPT_JIT_ENV_OVERRIDE.
    plain = []
    for f in flags:
        if f.startswith('env:'):
            k, v = f[4:].split('=', 1)
            env[k] = v
        else:
            plain.append(f)
    try:
        p = subprocess.run([tool] + plain + [scratch], capture_output=True, text=True, timeout=20, env=env)
    except subprocess.TimeoutExpired:
        return 'timeout', ''
    return p.returncode, p.stdout + p.stderr.replace(scratch, '')
def bad(lines):
    a = run(lines, ['--interpret'])
    if a[0] != 0 and 'error' in a[1][:300]:
        return False
    return a != run(lines, [flag])
lines = open(path).read().split('\n')
assert bad(lines)
# A statement at a time, braces and all, until nothing more can go.
for _ in range(4):
    i = 0
    while i < len(lines):
        depth, j = 0, i
        while j < len(lines):
            depth += lines[j].count('{') - lines[j].count('}')
            j += 1
            if depth <= 0:
                break
        attempt = lines[:i] + lines[j:]
        if depth == 0 and bad(attempt):
            lines = attempt
        else:
            i += 1
print('\n'.join(lines))
a, b = run(lines, ['--interpret']), run(lines, [flag])
print('--- interpreter', a[0], a[1][:400]); print('--- native', b[0], b[1][:400])
