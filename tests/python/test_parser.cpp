// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// What the grammar accepts and, just as importantly, what it refuses.

#include "lang.hpp"
#include "tests/test_util.hpp"

namespace {

void accepts(const char* source)
{
    py::Context ctx;
    ctx.compile(source, "<accepts>");
}

void rejects(const char* source, const char* fragment = nullptr)
{
    py::Context ctx;
    try {
        ctx.compile(source, "<rejects>");
    } catch (const py::Error& e) {
        if (e.code() != py::ErrorCode::SyntaxError)
            ::tst::fail(__FILE__, __LINE__, std::string("wrong error from: ") + source + "\n  " + e.what());
        if (fragment && std::string(e.what()).find(fragment) == std::string::npos)
            ::tst::fail(__FILE__, __LINE__,
                        std::string("message '") + e.what() + "' does not mention '" + fragment + "'");
        return;
    }
    ::tst::fail(__FILE__, __LINE__, std::string("expected a syntax error from: ") + source);
}

}  // namespace

TEST(literalsAndOperators)
{
    accepts("1 + 2 * 3 ** 4 % 5 // 6 - -7");
    accepts("0x1f + 0b1010 + 0o17 + 1_000_000 + 1e-7 + .5 + 5.");
    accepts("'a' + \"b\" + '''c''' + \"\"\"d\"\"\" + r'\\n' + f'{1}' + 'x' 'y'");
    accepts("a < b <= c > d >= e == f != g is h is not i in j not in k");
    accepts("~a & b | c ^ d << e >> f");
    accepts("not a and b or c");
    accepts("x if a else y");
    accepts("lambda: 0; lambda a, b=1, *c: a");
    accepts("a[1]; a[1:2]; a[:]; a[::2]; a[-1]; a[1:-1:2]");
    accepts("(); (1,); (1, 2); [1, 2,]; {1: 2}; {1, 2}; {}");
    accepts("[x for x in y]; {k: v for k, v in y if k}; {x for x in y}; (x for x in y)");
    accepts("f(); f(1); f(a, *b); f(a, k=1); f(x for x in y)");
    accepts("a.b.c(1)[2].d");
    accepts("x = y = 1; a, b = 1, 2; a, *b = c; [a, b] = c; a.b = 1; a[0] = 1; a[1:2] = b");
    accepts("x += 1; x -= 1; x *= 1; x /= 1; x //= 1; x %= 1; x **= 1; x &= 1; x |= 1; x ^= 1; x <<= 1; x >>= 1");
    accepts("x: int = 5\ny: str");
}

TEST(statements)
{
    accepts("if a:\n    pass\nelif b:\n    pass\nelse:\n    pass");
    accepts("while a:\n    break\n    continue");
    accepts("for i in range(3):\n    pass\nfor k, v in d.items(): pass");
    accepts("try:\n    pass\nexcept ValueError as e:\n    pass\nexcept (KeyError, IndexError):\n    pass\nexcept:\n    raise\nfinally:\n    pass");
    accepts("def f(a, b=2, *rest):\n    '''doc'''\n    return a\n");
    accepts("def f(a: int, b: str = 'x') -> int:\n    return 1");
    accepts("class A:\n    pass\nclass B(A):\n    x = 1\n    def __init__(self):\n        super().__init__()\n    @property\n    def p(self): return 1\n    @staticmethod\n    def s(): return 2\n    @classmethod\n    def c(cls): return 3");
    accepts("global g\nnonlocal_ = 1\ndel a, b[0], c.d\nassert x, 'msg'\nraise ValueError('x') from None");
    accepts("import math\nimport random as r\nfrom math import sqrt, pi as PI\nfrom math import (floor, ceil)");
    accepts("x = 1; y = 2;\n");
    accepts("if x: y = 1; z = 2\n");
    accepts("# comment only\n\n   \n");
    accepts("a = (1 +\n     2)\nb = [\n  1,\n  2,\n]\nc = 1 + \\\n    2");
}

TEST(indentation)
{
    accepts("if a:\n\tpass\n");
    accepts("if a:\n    if b:\n        pass\n    pass\npass");
    rejects("if a:\npass", "indented block");
    rejects("  x = 1", "unexpected indent");
    rejects("if a:\n    x = 1\n  y = 2", "unindent");
}

TEST(unsupportedConstructsAreRefusedWithAReason)
{
    rejects("def g():\n    yield 1", "yield");
    rejects("with open('f') as f:\n    pass", "with");
    rejects("async def f():\n    pass", "async");
    rejects("def f(**kw):\n    pass", "kwargs");
    rejects("def f(a, *, b):\n    pass", "keyword-only");
    rejects("f(**d)", "'**'");
    rejects("class C(A, B):\n    pass", "multiple inheritance");
    rejects("import os", "not available");
    rejects("from os import path", "not available");
    rejects("from math import *", "import *");
    rejects("@decorate\ndef f():\n    pass", "decorator");
    rejects("for x in y:\n    pass\nelse:\n    pass", "'else'");
    rejects("try:\n    pass\nexcept:\n    pass\nelse:\n    pass", "'else'");
    rejects("if (n := 1):\n    pass", ":=");
    rejects("a @ b", "'@'");
    rejects("x = 1j", "complex");
    rejects("x = b'bytes'", "bytes");
    rejects("a[1, 2]", "multi-dimensional");
    rejects("raise", "bare 'raise'");
    rejects("class C:\n    if x:\n        pass", "class body");
    rejects("x = (1", "expected");
    rejects("x = 'unterminated", "unterminated");
    rejects("x = f'{}'", "empty expression");
    rejects("1 = x", "cannot assign");
    rejects("f(a=1, 2)", "positional argument");
}

TEST(errorsCarryTheLine)
{
    py::Context ctx;
    try {
        ctx.compile("x = 1\ny = 2\nz = (3\n", "prog.py");
    } catch (const py::Error& e) {
        CHECK(std::string(e.what()).find("prog.py:3") != std::string::npos ||
              std::string(e.what()).find("prog.py:4") != std::string::npos);
        return;
    }
    CHECK(false);
}
