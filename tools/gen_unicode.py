#!/usr/bin/env python3
"""Generate src/dyn/unicode_tables.inc from the Unicode Character Database.

usage: gen_unicode.py <directory of UCD text files> <out.inc>

Sets of code points go out as bytes: gap, length, gap, length, ... in LEB128, which takes a table of 3,000 ranges to
about 6 KB. Everything past the first `#if` is what `\\p{...}` needs, and a Compact build leaves it out.
"""
import re, sys
from pathlib import Path

ucd = Path(sys.argv[1])
MAX = 0x10FFFF


def lines(name):
    for raw in (ucd / name).read_text(encoding='utf-8').splitlines():
        body = raw.split('#')[0].strip()
        if body:
            yield [f.strip() for f in body.split(';')]


def cprange(text):
    if '..' in text:
        a, b = text.split('..')
        return int(a, 16), int(b, 16)
    return int(text, 16), int(text, 16)


def toRanges(points):
    out = []
    for p in sorted(points):
        if out and out[-1][1] + 1 == p:
            out[-1][1] = p
        else:
            out.append([p, p])
    return out


def merged(ranges):
    out = []
    for a, b in sorted(ranges):
        if out and a <= out[-1][1] + 1:
            out[-1][1] = max(out[-1][1], b)
        else:
            out.append([a, b])
    return out


def leb(n):
    out = bytearray()
    while True:
        b = n & 0x7f
        n >>= 7
        if n:
            out.append(b | 0x80)
        else:
            out.append(b)
            return out


def encode(ranges):
    out = bytearray()
    at = 0
    for a, b in ranges:
        out += leb(a - at)
        out += leb(b - a)
        at = b + 1
    return bytes(out)


blob = bytearray()


def place(ranges):
    data = encode(merged(ranges))
    start = len(blob)
    blob.extend(data)
    return start, len(data)


# ---- general category
gcRanges = {}
first = None
for f in lines('UnicodeData.txt'):
    cp = int(f[0], 16)
    if f[1].endswith(', First>'):
        first = cp
        continue
    a = first if f[1].endswith(', Last>') else cp
    first = None
    gcRanges.setdefault(f[2], []).append((a, cp))
assigned = merged([r for v in gcRanges.values() for r in v])
cn = []
at = 0
for a, b in assigned:
    if a > at:
        cn.append((at, a - 1))
    at = b + 1
if at <= MAX:
    cn.append((at, MAX))
gcRanges['Cn'] = cn

# ---- names of values
gcNames, gcGroups, scNames = {}, {}, {}
for raw in (ucd / 'PropertyValueAliases.txt').read_text(encoding='utf-8').splitlines():
    body, _, comment = raw.partition('#')
    f = [x.strip() for x in body.split(';')]
    if len(f) < 3:
        continue
    if f[0] == 'gc':
        gcNames[f[1]] = f[1:]
        if comment.strip():
            gcGroups[f[1]] = [x.strip() for x in comment.split('|')]
    elif f[0] == 'sc':
        scNames[f[1]] = f[1:]

# ---- scripts
longToShort = {names[1]: short for short, names in scNames.items()}
scripts = {}
scriptOf = {}
for f in lines('Scripts.txt'):
    a, b = cprange(f[0])
    scripts.setdefault(longToShort[f[1]], []).append((a, b))
# What no script lists is Script=Unknown.
unknown = []
at = 0
for a, b in merged([r for v in scripts.values() for r in v]):
    if a > at:
        unknown.append((at, a - 1))
    at = b + 1
if at <= MAX:
    unknown.append((at, MAX))
scripts['Zzzz'] = unknown
extRanges = []
for f in lines('ScriptExtensions.txt'):
    a, b = cprange(f[0])
    extRanges.append((a, b, f[1].split()))
overridden = [(a, b) for a, b, _ in extRanges]
scx = {}


def withoutOverridden(ranges):
    # Script_Extensions is the Script value, except on what ScriptExtensions.txt lists.
    points = merged(overridden)
    out = []
    for a, b in merged(ranges):
        at = a
        for x, y in points:
            if y < at or x > b:
                continue
            if x > at:
                out.append((at, x - 1))
            at = y + 1
        if at <= b:
            out.append((at, b))
    return out


for name, ranges in scripts.items():
    scx[name] = withoutOverridden(ranges)
for a, b, names in extRanges:
    for n in names:
        scx.setdefault(n, []).append((a, b))

# ---- binary properties
binaryAliases = {
    'ASCII_Hex_Digit': 'AHex', 'Alphabetic': 'Alpha', 'Bidi_Control': 'Bidi_C', 'Bidi_Mirrored': 'Bidi_M',
    'Case_Ignorable': 'CI', 'Cased': '', 'Changes_When_Casefolded': 'CWCF', 'Changes_When_Casemapped': 'CWCM',
    'Changes_When_Lowercased': 'CWL', 'Changes_When_NFKC_Casefolded': 'CWKCF', 'Changes_When_Titlecased': 'CWT',
    'Changes_When_Uppercased': 'CWU', 'Dash': '', 'Default_Ignorable_Code_Point': 'DI', 'Deprecated': 'Dep',
    'Diacritic': 'Dia', 'Emoji': '', 'Emoji_Component': 'EComp', 'Emoji_Modifier': 'EMod',
    'Emoji_Modifier_Base': 'EBase', 'Emoji_Presentation': 'EPres', 'Extended_Pictographic': 'ExtPict',
    'Extender': 'Ext', 'Grapheme_Base': 'Gr_Base', 'Grapheme_Extend': 'Gr_Ext', 'Hex_Digit': 'Hex',
    'IDS_Binary_Operator': 'IDSB', 'IDS_Trinary_Operator': 'IDST', 'ID_Continue': 'IDC', 'ID_Start': 'IDS',
    'Ideographic': 'Ideo', 'Join_Control': 'Join_C', 'Logical_Order_Exception': 'LOE', 'Lowercase': 'Lower',
    'Math': '', 'Noncharacter_Code_Point': 'NChar', 'Pattern_Syntax': 'Pat_Syn', 'Pattern_White_Space': 'Pat_WS',
    'Quotation_Mark': 'QMark', 'Radical': '', 'Regional_Indicator': 'RI', 'Sentence_Terminal': 'STerm',
    'Soft_Dotted': 'SD', 'Terminal_Punctuation': 'Term', 'Unified_Ideograph': 'UIdeo', 'Uppercase': 'Upper',
    'Variation_Selector': 'VS', 'White_Space': 'space', 'XID_Continue': 'XIDC', 'XID_Start': 'XIDS',
}
binary = {name: [] for name in binaryAliases}
for fname in ['PropList.txt', 'DerivedCoreProperties.txt', 'DerivedBinaryProperties.txt',
              'DerivedNormalizationProps.txt', 'emoji-data.txt']:
    for f in lines(fname):
        if len(f) == 2 and f[1] in binary:
            binary[f[1]].append(cprange(f[0]))
missing = [k for k, v in binary.items() if not v]
assert not missing, missing

# ---- case
upper, lower, fold = {}, {}, {}
for f in lines('UnicodeData.txt'):
    cp = int(f[0], 16)
    if f[12]:
        upper[cp] = int(f[12], 16)
    if f[13]:
        lower[cp] = int(f[13], 16)
for f in lines('CaseFolding.txt'):
    if f[1] in ('C', 'S'):
        fold[int(f[0], 16)] = int(f[2], 16)
specialUpper = {}
for raw in (ucd / 'SpecialCasing.txt').read_text(encoding='utf-8').splitlines():
    f = [x.strip() for x in raw.split('#')[0].split(';')]
    if len(f) == 5 and f[4] == '':  # no condition
        mapped = [int(x, 16) for x in f[3].split()]
        if len(mapped) > 1:
            specialUpper[int(f[0], 16)] = mapped


def runs(mapping):
    """(start, count, stride, delta): a run of code points with one distance to what they map to."""
    out = []
    for cp in sorted(mapping):
        d = mapping[cp] - cp
        if out:
            s = out[-1]
            stride = cp - s[0] if s[1] == 1 else s[2]
            if s[3] == d and stride <= 2 and cp == s[0] + s[1] * stride and s[1] < 65535:
                s[2] = stride
                s[1] += 1
                continue
        out.append([cp, 1, 1, d])
    return out


w = []
w.append('// Generated by tools/gen_unicode.py from the Unicode Character Database, version 17.0.0. Do not edit.\n')


def bytesOut(name, data):
    w.append('const unsigned char %s[] = {' % name)
    for i in range(0, len(data), 32):
        w.append(','.join(str(x) for x in data[i:i + 32]) + ',')
    w.append('};\n')


def caseTable(name, mapping):
    w.append('const CaseRun %s[] = {' % name)
    for a, n, s, d in runs(mapping):
        w.append('{0x%x,%d,%d,%d},' % (a, n, s, d))
    w.append('};\n')


caseTable('kUpperRuns', upper)
caseTable('kLowerRuns', lower)
caseTable('kFoldRuns', fold)
w.append('const SpecialCase kSpecialUpper[] = {')
for cp, m in sorted(specialUpper.items()):
    m = m + [0] * (3 - len(m))
    w.append('{0x%x,{0x%x,0x%x,0x%x}},' % (cp, m[0], m[1], m[2]))
w.append('};\n')
bytesOut('kIdStartData', encode(merged(binary['ID_Start'])))
bytesOut('kIdContinueData', encode(merged(binary['ID_Continue'])))

w.append('#if SCRIPT_UNICODE_PROPERTIES\n')
entries = {'gc': [], 'sc': [], 'scx': [], 'bin': []}
groups = []
for short, names in gcNames.items():
    if short in gcGroups:
        groups.append(('|'.join(names), gcGroups[short]))
        continue
    off, n = place(gcRanges.get(short, []))
    entries['gc'].append(('|'.join(names), off, n, short))
for short, names in sorted(scNames.items()):
    off, n = place(scripts.get(short, []))
    entries['sc'].append(('|'.join(names), off, n))
    off, n = place(scx.get(short, []))
    entries['scx'].append(('|'.join(names), off, n))
for name, alias in binaryAliases.items():
    off, n = place(binary[name])
    entries['bin'].append((name + ('|' + alias if alias else ''), off, n))


def propTable(name, rows):
    w.append('const PropertyEntry %s[] = {' % name)
    for r in rows:
        w.append('{"%s",%d,%d},' % (r[0], r[1], r[2]))
    w.append('};\n')


propTable('kGcTable', entries['gc'])
propTable('kScriptTable', entries['sc'])
propTable('kScriptExtTable', entries['scx'])
propTable('kBinaryTable', entries['bin'])
shortIndex = {r[3]: i for i, r in enumerate(entries['gc'])}
w.append('const GcGroup kGcGroups[] = {')
for names, members in groups:
    expanded = []
    for m in members:
        expanded += [x for x in gcGroups.get(m, [m])]
    mask = 0
    for m in expanded:
        mask |= 1 << shortIndex[m]
    w.append('{"%s",0x%xu},' % (names, mask))
w.append('};\n')
bytesOut('kPropertyData', bytes(blob))
w.append('#endif  // SCRIPT_UNICODE_PROPERTIES\n')
Path(sys.argv[2]).write_text('\n'.join(w))
print('runs', len(runs(upper)), len(runs(lower)), len(runs(fold)), 'special', len(specialUpper), 'props bytes', len(blob))
