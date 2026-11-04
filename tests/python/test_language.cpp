// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Language semantics: scoping, classes, control flow, exceptions, and the
// places where Python and the engine's JavaScript disagree.

#include <memory>
#include <vector>

#include "lang.hpp"
#include "tests/test_util.hpp"

namespace {

/// A Value belongs to its Context, so the contexts the tests make are kept
/// alive until the process ends.
py::Value run(const char* source)
{
    static std::vector<std::unique_ptr<py::Context>> contexts;
    contexts.push_back(std::make_unique<py::Context>());
    return contexts.back()->eval(source, "<test>");
}

std::string output(const char* source)
{
    py::Context ctx;
    std::string out;
    ctx.setPrintHandler([&out](std::string_view text, bool) { out += text; });
    ctx.eval(source, "<test>");
    return out;
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

TEST(arithmeticFollowsPython)
{
    CHECK_EQ(num("1 + 2 * 3"), 7.0);
    CHECK_EQ(num("2 ** 10"), 1024.0);
    CHECK_EQ(num("-2 ** 2"), -4.0);
    CHECK_EQ(num("2 ** -1"), 0.5);
    CHECK_EQ(num("7 // 2"), 3.0);
    CHECK_EQ(num("-7 // 2"), -4.0);
    CHECK_EQ(num("7 / 2"), 3.5);
    CHECK_EQ(num("-7 % 3"), 2.0);
    CHECK_EQ(num("7 % -3"), -2.0);
    CHECK_EQ(num("n = 3\n-7 % n"), 2.0);
    CHECK_EQ(num("7.5 % 2"), 1.5);
    CHECK_EQ(num("0xff & 0x0f | 1 << 4"), 31.0);
    CHECK_EQ(num("~0"), -1.0);
    CHECK_EQ(num("x = 5\nx += 3\nx *= 2\nx //= 3\nx"), 5.0);
    CHECK_EQ(num("x = 10\nx %= 4\nx **= 2\nx"), 4.0);
    CHECK_EQ(num("1_000 + 0b11 + 0o10 + 0x10"), 1027.0);
}

TEST(truthIsPythonTruth)
{
    CHECK_EQ(text("'y' if [] else 'n'"), "n");
    CHECK_EQ(text("'y' if [0] else 'n'"), "y");
    CHECK_EQ(text("'y' if {} else 'n'"), "n");
    CHECK_EQ(text("'y' if '' else 'n'"), "n");
    CHECK_EQ(text("'y' if None else 'n'"), "n");
    CHECK_EQ(text("'y' if 0.0 else 'n'"), "n");
    CHECK_EQ(text("'y' if not [] else 'n'"), "y");
    CHECK_EQ(text("[] or 'empty'"), "empty");
    CHECK_EQ(text("'a' and 'b'"), "b");
    CHECK_EQ(text("str(0 or None)"), "None");
    CHECK_EQ(num("n = 0\nwhile [1, 2, 3][n:]:\n    n += 1\nn"), 3.0);
    CHECK_EQ(text("str(bool([]))"), "False");
}

TEST(comparisonsChainAndCompareByValue)
{
    CHECK_EQ(text("str(1 < 2 < 3)"), "True");
    CHECK_EQ(text("str(1 < 3 < 2)"), "False");
    CHECK_EQ(text("str([1, 2] == [1, 2])"), "True");
    CHECK_EQ(text("str([1, 2] != [1, 2])"), "False");
    CHECK_EQ(text("str((1, 'a') == (1, 'a'))"), "True");
    CHECK_EQ(text("str({'a': 1} == {'a': 1})"), "True");
    CHECK_EQ(text("str({1, 2} == {2, 1})"), "True");
    CHECK_EQ(text("a = [1]\nb = a\nstr(a is b and a is not [1])"), "True");
    CHECK_EQ(text("x = None\nstr(x is None and x == None)"), "True");
    CHECK_EQ(text("str(3 in [1, 2, 3] and 4 not in [1, 2, 3])"), "True");
    CHECK_EQ(text("str('bc' in 'abcd' and 'k' in {'k': 1} and 2 in {1, 2})"), "True");
    CHECK_EQ(text("str([1, 2] in [[1, 2], [3]])"), "True");
}

TEST(stringsAndFStrings)
{
    CHECK_EQ(text("'a' + 'b' * 3"), "abbb");
    CHECK_EQ(text("'ab' 'cd'"), "abcd");
    CHECK_EQ(text("'''multi\nline'''"), "multi\nline");
    CHECK_EQ(text("r'\\n'"), "\\n");
    CHECK_EQ(text("'tab\\there \\x41\\u00e9 \\\\'"), "tab\there A\u00e9 \\");
    CHECK_EQ(text("name = 'World'\nf'Hello, {name}!'"), "Hello, World!");
    CHECK_EQ(text("x = 3.14159\nf'{x:.2f}|{42:>5}|{7:03d}|{255:x}|{0.5:.0%}'"), "3.14|   42|007|ff|50%");
    CHECK_EQ(text("v = [1, 'a']\nf'{v} {v!r} {None} {True}'"), "[1, 'a'] [1, 'a'] None True");
    CHECK_EQ(text("f'{{literal}} {1 + 1}'"), "{literal} 2");
    CHECK_EQ(text("f'{\"quoted\"}'"), "quoted");
    CHECK_EQ(text("w = 8\nf'{\"hi\":^{w}}|{3.14159:.{w - 6}f}'"), "   hi   |3.14");
    CHECK_EQ(text("'%d-%s-%.1f-%5d|%-3d|%x' % (1, 'two', 3.0, 4, 5, 255)"), "1-two-3.0-    4|5  |ff");
    CHECK_EQ(text("'%s' % 'single'"), "single");
    CHECK_EQ(text("'%s' % [1, 2]"), "[1, 2]");
    CHECK_EQ(text("'{} and {}'.format(1, 'b')"), "1 and b");
    CHECK_EQ(text("'{1}{0}{name}'.format('a', 'b', name='c')"), "bac");
    CHECK_EQ(text("'{:>4}|{:.1f}'.format(1, 2.25)"), "   1|2.2");
}

TEST(strConversions)
{
    CHECK_EQ(text("str(None) + str(True) + str(False)"), "NoneTrueFalse");
    CHECK_EQ(text("str([1, 'a', None, [2]])"), "[1, 'a', None, [2]]");
    CHECK_EQ(text("str({'a': 1, 2: 'b'})"), "{'a': 1, 2: 'b'}");
    CHECK_EQ(text("str({1, 2})"), "{1, 2}");
    CHECK_EQ(text("str(set())"), "set()");
    CHECK_EQ(text("str((1, 2))"), "[1, 2]");
    CHECK_EQ(text("repr('it\\'s')"), "\"it's\"");
    CHECK_EQ(text("repr('a\\nb')"), "'a\\nb'");
    CHECK_EQ(text("str(1e21) + ' ' + str(float('inf')) + ' ' + str(-0.5)"), "1e+21 inf -0.5");
}

TEST(functionsAndScoping)
{
    CHECK_EQ(num("def add(a, b=10):\n    return a + b\nadd(1) + add(1, 2)"), 14.0);
    CHECK_EQ(num("def f(*args):\n    return len(args)\nf() + f(1, 2, 3)"), 3.0);
    CHECK_EQ(num("def f(a, b, c=3):\n    return a * 100 + b * 10 + c\nf(1, c=9, b=2)"), 129.0);
    CHECK_EQ(num("def f(a, b, c=3):\n    return a * 100 + b * 10 + c\nargs = [1, 2]\nf(*args)"), 123.0);
    CHECK_EQ(num("x = 1\ndef f():\n    x = 2\n    return x\nf() + x"), 3.0);
    CHECK_EQ(num("x = 1\ndef f():\n    global x\n    x = 2\nf()\nx"), 2.0);
    CHECK_EQ(num("def outer():\n    n = 0\n    def inc():\n        nonlocal n\n        n += 1\n        return n\n    inc()\n    return inc()\nouter()"), 2.0);
    CHECK_EQ(num("def counter():\n    c = [0]\n    def bump():\n        c[0] += 1\n        return c[0]\n    return bump\nb = counter()\nb()\nb()"), 2.0);
    // Late binding, as in Python: every lambda sees the final value.
    CHECK_EQ(num("fs = [lambda: i for i in range(3)]\nfs[0]() + fs[2]()"), 4.0);
    CHECK_EQ(num("fs = []\nfor i in range(3):\n    fs.append(lambda: i)\nfs[0]()"), 2.0);
    CHECK_EQ(num("def fact(n):\n    return 1 if n <= 1 else n * fact(n - 1)\nfact(10)"), 3628800.0);
    CHECK_EQ(num("def later():\n    return helper()\ndef helper():\n    return 7\nlater()"), 7.0);
    CHECK_EQ(text("def f():\n    pass\nstr(f())"), "None");
    CHECK_EQ(num("sq = lambda x: x * x\nsq(9)"), 81.0);
    CHECK_EQ(num("def apply(f, v):\n    return f(v)\napply(lambda v: v + 1, 1)"), 2.0);
    CHECK_EQ(text("def f(a, b):\n    return a\nstr(f(b=1, a=None))"), "None");
}

TEST(classesAreClasses)
{
    const char* shapes =
        "class Shape:\n"
        "    sides = 0\n"
        "    def __init__(self, name):\n"
        "        self.name = name\n"
        "    def describe(self):\n"
        "        return f'{self.name} with {self.sides} sides'\n"
        "    def area(self):\n"
        "        return 0\n"
        "class Square(Shape):\n"
        "    sides = 4\n"
        "    def __init__(self, side):\n"
        "        super().__init__('square')\n"
        "        self.side = side\n"
        "    def area(self):\n"
        "        return self.side ** 2\n"
        "    def describe(self):\n"
        "        return super().describe() + f', area {self.area()}'\n"
        "class Tri(Shape):\n"
        "    sides = 3\n"
        "    def __init__(self):\n"
        "        Shape.__init__(self, 'tri')\n";
    CHECK_EQ(text((std::string(shapes) + "Square(3).describe()").c_str()), "square with 4 sides, area 9");
    CHECK_EQ(text((std::string(shapes) + "Tri().describe()").c_str()), "tri with 3 sides");
    CHECK_EQ(text((std::string(shapes) + "str(isinstance(Square(1), Shape)) + str(isinstance(Tri(), Square))").c_str()), "TrueFalse");
    CHECK_EQ(num((std::string(shapes) + "Shape.sides + Square.sides + Square(1).sides").c_str()), 8.0);
    CHECK_EQ(num((std::string(shapes) + "shapes = [Square(2), Tri()]\nsum(s.area() for s in shapes)").c_str()), 4.0);
    CHECK_EQ(text("class P:\n    def __init__(self, x):\n        self.x = x\n    def __str__(self):\n        return f'P({self.x})'\n    def __repr__(self):\n        return 'repr'\n    def __eq__(self, o):\n        return self.x == o.x\n    def __len__(self):\n        return self.x\n    def __getitem__(self, i):\n        return self.x * i\n    def __contains__(self, v):\n        return v == self.x\n"
                  "p = P(3)\nf'{p} {str(p)} {p!r} {p == P(3)} {p != P(4)} {len(p)} {p[2]} {3 in p} {bool(P(0))}'"),
             "P(3) P(3) repr True True 3 6 True False");
    CHECK_EQ(num("class C:\n    def __init__(self):\n        self.n = 0\n    @property\n    def double(self):\n        return self.n * 2\n    @staticmethod\n    def make(n):\n        c = C()\n        c.n = n\n        return c\n    @classmethod\n    def name(cls):\n        return cls.tag\nC.tag = 'tag'\nC.make(21).double"), 42.0);
    CHECK_EQ(text("class C:\n    tag = 'x'\n    @classmethod\n    def name(cls):\n        return cls.tag\nC.name()"), "x");
    CHECK_EQ(num("class Node:\n    def __init__(self, v, nxt=None):\n        self.v = v\n        self.next = nxt\nn = Node(1, Node(2, Node(3)))\ntotal = 0\nwhile n is not None:\n    total += n.v\n    n = n.next\ntotal"), 6.0);
    CHECK_EQ(text("class Base:\n    def __init__(self):\n        self.tag = 'base'\nclass Derived(Base):\n    def __init__(self):\n        self.extra = 1\nd = Derived()\nd.tag + str(d.extra)"), "base1");
    CHECK_EQ(text("class E(Exception):\n    pass\ntry:\n    raise E('custom')\nexcept E as e:\n    r = 'got ' + str(e)\nr"), "got custom");
    CHECK_EQ(num("class Bag:\n    def __init__(self):\n        self.items = [1, 2, 3]\n    def __iter__(self):\n        return iter(self.items)\nsum(x for x in Bag())"), 6.0);
}

TEST(controlFlow)
{
    CHECK_EQ(text("x = 5\nif x < 3:\n    r = 'small'\nelif x < 10:\n    r = 'medium'\nelse:\n    r = 'large'\nr"), "medium");
    CHECK_EQ(num("total = 0\nfor i in range(10):\n    if i % 2:\n        continue\n    if i > 6:\n        break\n    total += i\ntotal"), 12.0);
    CHECK_EQ(num("total = 0\nfor i in range(2, 20, 5):\n    total += i\ntotal"), 2.0 + 7 + 12 + 17);
    CHECK_EQ(num("total = 0\nfor i in range(5, 0, -2):\n    total += i\ntotal"), 9.0);
    CHECK_EQ(num("xs = [1, 2, 3]\nfor i in range(len(xs)):\n    xs.append(0)\nlen(xs)"), 6.0);
    CHECK_EQ(num("n = 10\nsteps = 0\nwhile n != 1:\n    n = n // 2 if n % 2 == 0 else 3 * n + 1\n    steps += 1\nsteps"), 6.0);
    CHECK_EQ(text("out = []\nfor k, v in {'a': 1, 'b': 2}.items():\n    out.append(k + str(v))\n''.join(out)"), "a1b2");
    CHECK_EQ(text("out = ''\nfor c in 'abc':\n    out = c + out\nout"), "cba");
    CHECK_EQ(text("out = []\nfor k in {'x': 1, 'y': 2}:\n    out.append(k)\n''.join(out)"), "xy");
    CHECK_EQ(num("for i, v in enumerate([10, 20], 1):\n    last = i * v\nlast"), 40.0);
    CHECK_EQ(num("i = 0\nfor i in range(3):\n    pass\ni"), 2.0);
    CHECK_EQ(text("a, b = 1, 2\na, b = b, a\nf'{a}{b}'"), "21");
    CHECK_EQ(text("(a, b), c = (1, 2), 3\nf'{a}{b}{c}'"), "123");
    CHECK_EQ(text("first, *rest = 'abc'\nfirst + ''.join(rest)"), "abc");
    CHECK_EQ(num("x = y = z = 4\nx + y + z"), 12.0);
    CHECK_EQ(text("x = 1; y = 2; z = x + y\nstr(z)"), "3");
}

TEST(exceptions)
{
    CHECK_EQ(text("try:\n    [][0]\nexcept IndexError as e:\n    r = str(e)\nr"), "list index out of range");
    CHECK_EQ(text("try:\n    {}['k']\nexcept KeyError as e:\n    r = str(e)\nr"), "k");
    CHECK_EQ(text("try:\n    int('x')\nexcept (KeyError, ValueError) as e:\n    r = type(e).__name__ if False else 'value'\nr"), "value");
    CHECK_EQ(text("try:\n    raise ValueError('v')\nexcept KeyError:\n    r = 'key'\nexcept ValueError:\n    r = 'value'\nr"), "value");
    CHECK_EQ(text("try:\n    raise ValueError('v')\nexcept Exception as e:\n    r = 'any ' + str(e)\nr"), "any v");
    CHECK_EQ(text("log = []\ntry:\n    log.append('try')\n    raise RuntimeError('x')\nexcept RuntimeError:\n    log.append('except')\nfinally:\n    log.append('finally')\n','.join(log)"), "try,except,finally");
    CHECK_EQ(text("def f():\n    try:\n        return 'from try'\n    finally:\n        pass\nf()"), "from try");
    CHECK_EQ(text("try:\n    try:\n        raise ValueError('inner')\n    except ValueError:\n        raise\nexcept ValueError as e:\n    r = 'outer ' + str(e)\nr"), "outer inner");
    CHECK_EQ(text("try:\n    raise IndexError\nexcept IndexError:\n    r = 'bare class'\nr"), "bare class");
    CHECK_EQ(text("try:\n    assert 1 == 2, 'nope'\nexcept AssertionError as e:\n    r = str(e)\nr"), "nope");
    CHECK_EQ(text("try:\n    None.x\nexcept Exception:\n    r = 'engine errors are exceptions too'\nr"), "engine errors are exceptions too");
    CHECK_EQ(text("try:\n    undefined_name\nexcept Exception as e:\n    r = 'caught'\nr"), "caught");
    CHECK_EQ(text("try:\n    1 % 0\nexcept ZeroDivisionError:\n    r = 'zero'\nr"), "zero");

    py::Context ctx;
    try {
        ctx.eval("def f():\n    raise ValueError('escaped')\nf()", "prog.py");
        CHECK(false);
    } catch (const py::Exception& e) {
        CHECK(std::string(e.what()).find("escaped") != std::string::npos);
        CHECK(e.stack().find("prog.py:2") != std::string::npos);
    }
}

TEST(collections)
{
    CHECK_EQ(text("xs = [3, 1, 2]\nxs.append(4)\nxs.insert(0, 0)\nxs.extend([5, 6])\nxs.remove(1)\nstr(xs) + str(xs.pop()) + str(xs.pop(0)) + str(xs.index(2)) + str(xs.count(3))"), "[0, 3, 2, 4, 5, 6]6011");
    CHECK_EQ(text("xs = [3, 1, 2]\nxs.sort()\nys = sorted(xs, reverse=True)\nxs.reverse()\nstr(xs) + str(ys) + str(sorted([10, 9, 1]))"), "[3, 2, 1][3, 2, 1][1, 9, 10]");
    CHECK_EQ(text("xs = [[1, 'b'], [2, 'a']]\nxs.sort(key=lambda p: p[1])\nstr(xs)"), "[[2, 'a'], [1, 'b']]");
    CHECK_EQ(text("xs = list(range(10))\nstr(xs[2:5]) + str(xs[-2:]) + str(xs[::3]) + str(xs[::-1][:2]) + str(xs[7:2:-2])"), "[2, 3, 4][8, 9][0, 3, 6, 9][9, 8][7, 5, 3]");
    CHECK_EQ(text("xs = [1, 2, 3, 4]\nxs[1:3] = ['a']\ndel xs[0]\nstr(xs)"), "['a', 4]");
    CHECK_EQ(text("xs = [0] * 3\nxs[-1] = 9\nstr(xs) + str([1, 2] + [3])"), "[0, 0, 9][1, 2, 3]");
    CHECK_EQ(text("ys = [1, 2]\nzs = ys.copy()\nzs.append(3)\nstr(len(ys)) + str(len(zs))"), "23");
    CHECK_EQ(text("d = {'a': 1}\nd['b'] = 2\nd.update({'c': 3})\nstr(d) + str(d.get('z')) + str(d.get('z', 0)) + str(list(d.keys())) + str(list(d.values())) + str(d.items())"),
             "{'a': 1, 'b': 2, 'c': 3}None0['a', 'b', 'c'][1, 2, 3][['a', 1], ['b', 2], ['c', 3]]");
    CHECK_EQ(text("d = {1: 'one'}\nd[2] = 'two'\ndel d[1]\nstr(d) + str(2 in d) + str(1 in d) + str(len(d)) + str(d.pop(2)) + str(d.pop(3, 'dflt')) + str(d.setdefault('k', []))"), "{2: 'two'}TrueFalse1twodflt[]");
    CHECK_EQ(text("d = dict(a=1, b=2)\ne = dict([('x', 1)])\nstr(d) + str(e) + str(dict(d) == d)"), "{'a': 1, 'b': 2}{'x': 1}True");
    CHECK_EQ(text("s = {1, 2}\ns.add(3)\ns.discard(9)\ns.remove(1)\nstr(sorted(s)) + str(len(s)) + str(s.union({7}) == {2, 3, 7}) + str(s.issubset({2, 3, 4}))"), "[2, 3]2TrueTrue");
    CHECK_EQ(text("s = set('hello')\nstr(sorted(s))"), "['e', 'h', 'l', 'o']");
    CHECK_EQ(text("t = (1, 2, 3)\nstr(t[0] + t[-1]) + str(len(t)) + str(t[1:])"), "43[2, 3]");
    CHECK_EQ(text("str([x * 2 for x in range(4) if x % 2]) + str({x: x ** 2 for x in range(3)}) + str(sorted({c for c in 'aab'}))"), "[2, 6]{0: 0, 1: 1, 2: 4}['a', 'b']");
    CHECK_EQ(text("str([[i * j for j in range(3)] for i in range(2)]) + str([p for row in [[1, 2], [3]] for p in row])"), "[[0, 0, 0], [0, 1, 2]][1, 2, 3]");
    CHECK_EQ(text("m = [[0] * 2 for _ in range(2)]\nm[0][1] = 5\nm[1][0] += 3\nstr(m)"), "[[0, 5], [3, 0]]");
    CHECK_EQ(text("n = [1, 2, 3]\nn[0], n[2] = n[2], n[0]\nstr(n)"), "[3, 2, 1]");
}

TEST(printAndCompletionValue)
{
    CHECK_EQ(output("print('a', 1, None, [1, 'b'], sep='|')"), "a|1|None|[1, 'b']\n");
    CHECK_EQ(output("print('no newline', end='')\nprint()"), "no newline\n");
    CHECK_EQ(output("print(3.0, 2.5, True, 10 / 5)"), "3 2.5 True 2\n");
    CHECK_EQ(output("for i in range(3):\n    print(i, end=' ')"), "0 1 2 ");
    CHECK_EQ(run("x = 6\nx * 7").toInt(), 42);
    CHECK(run("x = 1").isNullish());
    CHECK_EQ(run("def f(): return [1, 2]\nf()").length(), 2u);
}

TEST(stateSurvivesBetweenEvaluations)
{
    py::Context ctx;
    ctx.eval("counter = 0\ndef bump():\n    global counter\n    counter += 1\n    return counter");
    ctx.eval("class Acc:\n    def __init__(self):\n        self.total = 0\n    def add(self, n):\n        self.total += n\n        return self");
    ctx.eval("bump()");
    CHECK_EQ(ctx.eval("bump()").toInt(), 2);
    CHECK_EQ(ctx.eval("Acc().add(3).add(4).total").toInt(), 7);
    CHECK_EQ(ctx.eval("counter").toInt(), 2);
    // A global assigned from inside a function only.
    ctx.eval("def setit():\n    global late\n    late = 'set'\nsetit()");
    CHECK_EQ(ctx.eval("late").toString(), std::string("set"));
}

TEST(modules)
{
    CHECK_EQ(num("import math\nmath.floor(math.sqrt(17)) + math.ceil(0.1)"), 5.0);
    CHECK_EQ(num("from math import pi, sqrt\nsqrt(pi * pi)") > 3.14 ? 1.0 : 0.0, 1.0);
    CHECK_EQ(num("import math as m\nm.gcd(12, 18) + m.factorial(4)"), 30.0);
    CHECK_EQ(text("import random\nrandom.seed(1)\nv = random.randint(1, 3)\nstr(1 <= v <= 3) + str(random.choice(['x']))"), "Truex");
    CHECK_EQ(text("import sys\nstr(len(sys.argv) >= 0)"), "True");
}
