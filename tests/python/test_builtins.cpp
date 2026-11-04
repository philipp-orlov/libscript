// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The built-in functions and the Python spellings of the list, string, dict
// and set methods, as the prelude defines them.

#include <memory>
#include <vector>

#include "lang.hpp"
#include "tests/test_util.hpp"

namespace {

py::Value run(const char* source)
{
    static std::vector<std::unique_ptr<py::Context>> contexts;
    contexts.push_back(std::make_unique<py::Context>());
    return contexts.back()->eval(source, "<test>");
}

std::string text(const char* source)
{
    return run(source).toString();
}

double num(const char* source)
{
    return run(source).toDouble();
}

}  // namespace

TEST(conversions)
{
    CHECK_EQ(num("int('42') + int(' -7 ') + int(3.9) + int(-3.9) + int(True) + int()"), 42 - 7 + 3 - 3 + 1);
    CHECK_EQ(num("int('ff', 16) + int('101', 2) + int('1_000')"), 255 + 5 + 1000);
    CHECK_EQ(num("float('2.5') + float(' 1e2 ') + float(3) + float()"), 105.5);
    CHECK_EQ(text("str(float('inf')) + str(float('-inf')) + str(float('nan'))"), "inf-infnan");
    CHECK_EQ(text("str(bool(0)) + str(bool('x')) + str(bool([])) + str(bool(None)) + str(bool())"), "FalseTrueFalseFalseFalse");
    CHECK_EQ(text("str(list('abc')) + str(list({'k': 1})) + str(list(range(3))) + str(tuple([1])) + str(list())"), "['a', 'b', 'c']['k'][0, 1, 2][1][]");
    CHECK_EQ(text("str(sorted(set([3, 1, 3]))) + str(dict([[1, 2]]) == {1: 2})"), "[1, 3]True");
    CHECK_EQ(text("str(chr(65)) + str(ord('a')) + hex(255) + bin(5) + oct(8) + hex(-1)"), "A970xff0b1010o10-0x1");
    CHECK_EQ(text("str(round(2.5)) + str(round(3.5)) + str(round(-2.5)) + str(round(2.345, 2)) + str(round(7, 0))"), "24-22.357");
    CHECK_EQ(text("str(abs(-3)) + str(abs(2.5)) + str(divmod(17, 5)) + str(divmod(-7, 2)) + str(pow(2, 10)) + str(pow(2, 10, 1000))"), "32.5[3, 2][-4, 1]102424");
    CHECK_EQ(text("str(type(1) is int) + str(type(1.5) is float) + str(type('s') is str) + str(type([]) is list) + str(type({}) is dict) + str(type(None) is type(None))"), "TrueTrueTrueTrueTrueTrue");
    CHECK_EQ(text("str(isinstance(1, (str, int))) + str(isinstance(True, int)) + str(isinstance(1.5, float)) + str(isinstance([], (list, tuple))) + str(isinstance({}, dict)) + str(isinstance(1, object))"), "TrueFalseTrueTrueTrueTrue");
}

TEST(sequenceFunctions)
{
    CHECK_EQ(num("len('abc') + len([1, 2]) + len({'a': 1}) + len({1, 2, 3}) + len(range(10)) + len(())"), 3 + 2 + 1 + 3 + 10 + 0);
    CHECK_EQ(text("str(range(5)[2]) + str(list(range(0))) + str(list(range(3, 0, -1))) + str(list(range(0, 10, 4)))"), "2[][3, 2, 1][0, 4, 8]");
    CHECK_EQ(num("sum([1, 2, 3]) + sum(range(4)) + sum([0.5, 0.5]) + sum([], 10)"), 6 + 6 + 1 + 10);
    CHECK_EQ(text("str(sum([[1], [2]], []))"), "[1, 2]");
    CHECK_EQ(text("str(min(3, 1, 2)) + str(max([4, 9, 2])) + str(min('bca')) + str(max(['aa', 'b'], key=len)) + str(min([], default=-1))"), "19aaa-1");
    CHECK_EQ(text("str(sorted([3, 1, 2])) + str(sorted('cab')) + str(sorted([3, 1, 2], reverse=True)) + str(sorted(['bb', 'a', 'ccc'], key=len)) + str(sorted([[2, 'b'], [1, 'z'], [2, 'a']]))"),
             "[1, 2, 3]['a', 'b', 'c'][3, 2, 1]['a', 'bb', 'ccc'][[1, 'z'], [2, 'a'], [2, 'b']]");
    CHECK_EQ(text("str(list(reversed([1, 2, 3]))) + ''.join(reversed('abc')) + str(list(enumerate('ab'))) + str(list(enumerate([9], start=5)))"), "[3, 2, 1]cba[[0, 'a'], [1, 'b']][[5, 9]]");
    CHECK_EQ(text("str(list(zip([1, 2, 3], 'ab'))) + str(list(zip())) + str(list(map(str, [1, 2]))) + str(list(map(lambda a, b: a + b, [1, 2], [10, 20])))"), "[[1, 'a'], [2, 'b']][]['1', '2'][11, 22]");
    CHECK_EQ(text("str(list(filter(lambda v: v > 1, [1, 2, 3]))) + str(list(filter(None, [0, '', 'x', [], [1]])))"), "[2, 3]['x', [1]]");
    CHECK_EQ(text("str(any([0, 0, 1])) + str(any([])) + str(all([1, 'a'])) + str(all([])) + str(all(x > 0 for x in [1, -1]))"), "TrueFalseTrueTrueFalse");
    CHECK_EQ(num("it = iter([1, 2])\nnext(it) + next(it) + next(it, 10)"), 13.0);
    CHECK_EQ(text("str(callable(len)) + str(callable(1)) + str(hasattr([], 'append')) + str(getattr({'a': 1}, 'size')) + str(getattr(None, 'x', 'dflt'))"), "TrueFalseTrue1dflt");
}

TEST(stringMethods)
{
    CHECK_EQ(text("'Hello World'.upper() + 'Hello'.lower() + 'hello world'.title() + 'hELLO'.capitalize() + 'aB'.swapcase()"), "HELLO WORLDhelloHello WorldHelloAb");
    CHECK_EQ(text("'  pad  '.strip() + '|' + '  pad  '.lstrip() + '|' + '  pad  '.rstrip() + '|' + 'xxhixx'.strip('x')"), "pad|pad  |  pad|hi");
    CHECK_EQ(text("str('a b  c'.split()) + str('a,b,,c'.split(',')) + str('a b c d'.split(' ', 1)) + str('a,b,c'.split(',', maxsplit=1)) + str(' x '.split())"), "['a', 'b', 'c']['a', 'b', '', 'c']['a', 'b c d']['a', 'b,c']['x']");
    CHECK_EQ(text("str('a\\nb\\r\\nc'.splitlines()) + str('k=v'.partition('=')) + str('no'.partition('='))"), "['a', 'b', 'c']['k', '=', 'v']['no', '', '']");
    CHECK_EQ(text("', '.join(['a', 'b']) + '-'.join('xyz') + ''.join([]) + '+'.join(str(n) for n in range(3))"), "a, bx-y-z0+1+2");
    CHECK_EQ(text("'aXbXc'.replace('X', '-') + 'aaa'.replace('a', 'b', 2) + 'abc'.replace('', '.')"), "a-b-cbba.a.b.c.");
    CHECK_EQ(text("str('hello'.find('l')) + str('hello'.find('z')) + str('hello'.rfind('l')) + str('hello'.index('e')) + str('hello'.count('l')) + str('aaa'.count('aa'))"), "2-13121");
    CHECK_EQ(text("str('abc'.startswith('ab')) + str('abc'.endswith(('c', 'z'))) + str('abc'.startswith(('x', 'a'))) + str('abc'.endswith('b'))"), "TrueTrueTrueFalse");
    CHECK_EQ(text("str('123'.isdigit()) + str('12a'.isdigit()) + str('abc'.isalpha()) + str('ab1'.isalnum()) + str(' \\t'.isspace()) + str('ABC'.isupper()) + str('abc'.islower()) + str(''.isdigit())"), "TrueFalseTrueTrueTrueTrueTrueFalse");
    CHECK_EQ(text("'ab'.center(6, '*') + '|' + 'ab'.ljust(4) + '|' + 'ab'.rjust(4, '.') + '|' + '-7'.zfill(4) + '42'.zfill(5)"), "**ab**|ab  |..ab|-00700042");
    CHECK_EQ(text("'x'.join(['1', '2']) + str(len('héllo')) + 'héllo'[1] + 'abc'[-1] + 'abcdef'[1:-1:2]"), "1x25\u00e9cbd");
    CHECK_EQ(text("str('a' < 'b') + str('abc' == 'abc') + str('b' in 'abc') + str('abc' * 2) + str(3 * 'x')"), "TrueTrueTrueabcabcxxx");
}

TEST(numberFormatting)
{
    CHECK_EQ(text("f'{1234567.891:,.2f}|{42:08.3f}|{-5:+d}|{5:+d}|{3:5d}|{3:<5d}|{3:^5d}|{-3:05d}'"), "1,234,567.89|0042.000|-5|+5|    3|3    |  3  |-0003");
    CHECK_EQ(text("f'{0.000012345:e}|{12345.678:.3e}|{1e100:g}|{123.456:g}|{0.00001:g}|{1234567:g}'"), "1.234500e-05|1.235e+04|1e+100|123.456|1e-05|1.23457e+06");
    CHECK_EQ(text("f'{255:x}|{255:X}|{255:#x}|{8:o}|{5:b}|{5:#b}|{65:c}'"), "ff|FF|0xff|10|101|0b101|A");
    CHECK_EQ(text("f'{\"text\":>8}|{\"text\":<8}|{\"text\":^8}|{\"text\":.2}|{\"t\":*^5}'"), "    text|text    |  text  |te|**t**");
    CHECK_EQ(text("f'{0.25:.1%}|{1:.0%}|{True}|{None}|{[1, 2]}|{3.0}|{2.50}'"), "25.0%|100%|True|None|[1, 2]|3|2.5");
    CHECK_EQ(text("v = 42\nf'{v=}|{v = }'"), "v=42|v = 42");
    CHECK_EQ(text("'%5.1f|%-6s|%03d|%+d|%%|%c|%r|%e' % (3.14159, 'ab', 7, 3, 65, 'q', 1234.5)"), "  3.1|ab    |007|+3|%|A|'q'|1.234500e+03");
}

TEST(dictAndSetMethods)
{
    CHECK_EQ(text("d = {'b': 2, 'a': 1}\nstr(sorted(d)) + str(sorted(d.items())) + str(d.get('a')) + str(d.get('q', 'none')) + str(d.get('q'))"), "['a', 'b'][['a', 1], ['b', 2]]1noneNone");
    CHECK_EQ(text("d = {}\nd.setdefault('k', []).append(1)\nd.setdefault('k', []).append(2)\nd.update(z=9)\nd.update([('y', 8)])\nstr(d)"), "{'k': [1, 2], 'z': 9, 'y': 8}");
    CHECK_EQ(text("d = {1: 'a', 2: 'b'}\ne = d.copy()\ne.clear()\nstr(len(d)) + str(len(e)) + str(d.popitem()) + str(d)"), "20[2, 'b']{1: 'a'}");
    CHECK_EQ(text("counts = {}\nfor c in 'abca':\n    counts[c] = counts.get(c, 0) + 1\nstr(counts)"), "{'a': 2, 'b': 1, 'c': 1}");
    CHECK_EQ(text("a = {1, 2, 3}\nb = {3, 4}\nstr(sorted(a | b)) if False else str(sorted(a.union(b))) + str(sorted(a.intersection(b))) + str(sorted(a.difference(b))) + str(sorted(a.symmetric_difference(b))) + str(a.isdisjoint({9})) + str({1}.issubset(a)) + str(a.issuperset({1, 2}))"),
             "[1, 2, 3, 4][3][1, 2][1, 2, 4]TrueTrueTrue");
    CHECK_EQ(text("s = set()\ns.update([1, 2], [3])\nt = s.copy()\nt.add(4)\nstr(len(s)) + str(len(t)) + str(2 in s)"), "34True");
    CHECK_EQ(text("try:\n    {1}.remove(2)\nexcept KeyError:\n    r = 'KeyError'\nr"), "KeyError");
}

TEST(mathModule)
{
    CHECK_EQ(num("import math\nmath.floor(-1.5) + math.ceil(1.2) + math.trunc(-2.7) + math.fabs(-3) + math.sqrt(49)"), -2 + 2 - 2 + 3 + 7);
    CHECK_EQ(num("import math\nround(math.log(math.e) + math.log(8, 2) + math.log10(1000) + math.log2(4) + math.exp(0), 6)"), 1 + 3 + 3 + 2 + 1);
    CHECK_EQ(num("import math\nround(math.degrees(math.pi) + math.radians(180) - math.pi + math.hypot(3, 4) + math.pow(2, 3), 6)"), 180 + 5 + 8);
    CHECK_EQ(text("import math\nstr(math.isnan(math.nan)) + str(math.isinf(math.inf)) + str(math.isfinite(1.0)) + str(math.isclose(0.1 + 0.2, 0.3))"), "TrueTrueTrueTrue");
    CHECK_EQ(num("import math\nmath.gcd(48, 18) + math.lcm(4, 6) + math.factorial(5) + math.comb(5, 2) + math.prod([2, 3, 4]) + math.fsum([0.1] * 10)"), 6 + 12 + 120 + 10 + 24 + 1);
    CHECK_EQ(text("import math\ntry:\n    math.sqrt(-1)\nexcept ValueError as e:\n    r = str(e)\nr"), "math domain error");
}
