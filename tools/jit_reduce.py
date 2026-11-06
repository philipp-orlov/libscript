#!/usr/bin/env python3
"""Shrink a program on which the interpreter and native code disagree, by removing lines while they still do.
usage: jit_reduce.py <libscript tool> <program.cs> [flag]"""
import subprocess, sys
tool, path = sys.argv[1], sys.argv[2]
flag = sys.argv[3] if len(sys.argv) > 3 else '--native'
def run(lines, flags):
    open('/tmp/jit_reduce.cs', 'w').write('\n'.join(lines))
    try:
        p = subprocess.run([tool] + flags + ['/tmp/jit_reduce.cs'], capture_output=True, text=True, timeout=20)
    except subprocess.TimeoutExpired:
        return 'timeout', ''
    return p.returncode, p.stdout + p.stderr.replace('/tmp/jit_reduce.cs', '')
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
