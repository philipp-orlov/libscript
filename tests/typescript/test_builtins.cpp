// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The standard library surface the TypeScript engine ships.

#include <limits>

#include "lang.hpp"
#include "tests/test_util.hpp"

namespace {

std::string run(const char* source)
{
    ts::Context ctx;
    return ctx.eval(source, "<test>").toString();
}

}  // namespace

TEST(objectStatics)
{
    CHECK_EQ(run("Object.keys({ a: 1, b: 2 }).join(',')"), std::string("a,b"));
    CHECK_EQ(run("Object.values({ a: 1, b: 2 }).join(',')"), std::string("1,2"));
    CHECK_EQ(run("JSON.stringify(Object.entries({ a: 1 }))"), std::string("[[\"a\",1]]"));
    CHECK_EQ(run("JSON.stringify(Object.assign({}, { a: 1 }, { b: 2 }))"),
             std::string("{\"a\":1,\"b\":2}"));
    CHECK_EQ(run("JSON.stringify(Object.fromEntries([['a', 1]]))"), std::string("{\"a\":1}"));
    CHECK_EQ(run("const o = Object.freeze({ a: 1 }); o.a = 2; `${o.a}${Object.isFrozen(o)}`"),
             std::string("1true"));
    CHECK_EQ(run("({ a: 1 }).hasOwnProperty('a') + ' ' + ({}).hasOwnProperty('a')"),
             std::string("true false"));
    CHECK_EQ(run("const o: any = {}; Object.defineProperty(o, 'x', { get: () => 7 }); `${o.x}`"),
             std::string("7"));
}

TEST(arrayMethods)
{
    CHECK_EQ(run("[3,1,2].sort().join('')"), std::string("123"));
    CHECK_EQ(run("[3,1,2].sort((a, b) => b - a).join('')"), std::string("321"));
    CHECK_EQ(run("[1,2,3].map(x => x * 2).filter(x => x > 2).join(',')"), std::string("4,6"));
    CHECK_EQ(run("[1,2,3,4].reduce((a, b) => a + b)"), std::string("10"));
    CHECK_EQ(run("[1,2,3].reduceRight((a, b) => a + '' + b)"), std::string("321"));
    CHECK_EQ(run("[1,2,3].find(x => x > 1) + ',' + [1,2,3].findIndex(x => x > 1)"),
             std::string("2,1"));
    CHECK_EQ(run("[1,2,3].some(x => x > 2) + ' ' + [1,2,3].every(x => x > 0)"),
             std::string("true true"));
    CHECK_EQ(run("[1,[2,[3]]].flat(2).join('')"), std::string("123"));
    CHECK_EQ(run("[1,2,3].slice(1).join('') + [1,2,3].at(-1)"), std::string("233"));
    CHECK_EQ(run("const a = [1,2,3,4]; a.splice(1, 2, 9); a.join(',')"), std::string("1,9,4"));
    CHECK_EQ(run("[1,2,3].includes(2) + ' ' + [1,2,3].indexOf(9)"), std::string("true -1"));
    CHECK_EQ(run("Array.from({ length: 3 }, (_, i) => i).join('')"), std::string("012"));
    CHECK_EQ(run("Array.isArray([]) + ' ' + Array.isArray({})"), std::string("true false"));
    CHECK_EQ(run("[...[1,2].entries()].map(e => e.join(':')).join(',')"), std::string("0:1,1:2"));
    CHECK_EQ(run("const a = [1,2,3]; a.length = 1; a.join('')"), std::string("1"));
}

TEST(stringMethods)
{
    CHECK_EQ(run("'Hello'.toUpperCase() + 'X'.toLowerCase()"), std::string("HELLOx"));
    CHECK_EQ(run("'  pad  '.trim() + '|' + 'a'.padStart(3, '-')"), std::string("pad|--a"));
    CHECK_EQ(run("'a,b,c'.split(',').join('-')"), std::string("a-b-c"));
    CHECK_EQ(run("'abc'.slice(1, -1) + 'abc'.substring(1) + 'abc'.charAt(0)"), std::string("bbca"));
    CHECK_EQ(run("'abcabc'.replace('b', 'X') + '|' + 'abcabc'.replaceAll('b', 'X')"),
             std::string("aXcabc|aXcaXc"));
    CHECK_EQ(run("'ab'.repeat(3) + 'ab'.includes('b') + 'ab'.startsWith('a')"),
             std::string("ababab"
                         "truetrue"));
    CHECK_EQ(run("'abc'.indexOf('c') + ',' + 'abc'.length + ',' + 'abc'[1]"), std::string("2,3,b"));
    CHECK_EQ(run("String.fromCharCode(104, 105)"), std::string("hi"));
    CHECK_EQ(run("'héllo'.length"), std::string("5"));
}

TEST(numberAndMath)
{
    CHECK_EQ(run("(1234.5678).toFixed(2)"), std::string("1234.57"));
    CHECK_EQ(run("(0.000001234).toExponential(2)"), std::string("1.23e-6"));
    CHECK_EQ(run("Number.isInteger(3) + ' ' + Number.isInteger(3.5)"), std::string("true false"));
    CHECK_EQ(run("parseInt('42px') + parseFloat('3.5rem')"), std::string("45.5"));
    CHECK_EQ(run("Math.max(1, 5, 3) + Math.min(1, 5, 3) + Math.abs(-2)"), std::string("8"));
    CHECK_EQ(run("Math.round(2.5) + ',' + Math.round(-2.5) + ',' + Math.floor(-2.5)"),
             std::string("3,-2,-3"));
    CHECK_EQ(run("Math.sqrt(16) + Math.pow(2, 10)"), std::string("1028"));
    CHECK_EQ(run("const r = Math.random(); r >= 0 && r < 1"), std::string("true"));
}

TEST(jsonRoundTrips)
{
    CHECK_EQ(run("JSON.stringify({ a: [1, 'two', null, true], b: { c: 3 } })"),
             std::string("{\"a\":[1,\"two\",null,true],\"b\":{\"c\":3}}"));
    CHECK_EQ(run("JSON.parse('{\"a\":[1,2]}').a[1]"), std::string("2"));
    CHECK_EQ(run("JSON.stringify({ a: 1 }, null, 2)"), std::string("{\n  \"a\": 1\n}"));
    CHECK_EQ(run("JSON.stringify({ a: undefined, b: () => 1, c: 1 })"), std::string("{\"c\":1}"));
    CHECK_EQ(run("JSON.stringify('a\\nb')"), std::string("\"a\\nb\""));
}

TEST(mapAndSet)
{
    CHECK_EQ(run("const m = new Map([['a', 1]]); m.set('b', 2); "
                 "`${m.size}${m.get('a')}${m.has('c')}`"),
             std::string("21false"));
    CHECK_EQ(run("const m = new Map(); const k = {}; m.set(k, 'v'); m.get(k)"), std::string("v"));
    CHECK_EQ(run("[...new Set([1, 1, 2, 3])].join('')"), std::string("123"));
    CHECK_EQ(run("const s = new Set([1, 2]); s.delete(1); `${s.size}${s.has(2)}`"),
             std::string("1true"));
    CHECK_EQ(run("const m = new Map([['a', 1], ['b', 2]]); "
                 "[...m].map(([k, v]) => k + v).join(',')"),
             std::string("a1,b2"));
}

TEST(symbolsAndIterators)
{
    CHECK_EQ(run("typeof Symbol('x')"), std::string("symbol"));
    CHECK_EQ(run("Symbol.for('k') === Symbol.for('k')"), std::string("true"));
    CHECK_EQ(run("Symbol('x') === Symbol('x')"), std::string("false"));
    CHECK_EQ(run(R"(
        const range = { from: 1, to: 3, [Symbol.iterator]() {
            let n = this.from, last = this.to;
            return { next: () => n <= last ? { value: n++, done: false } : { done: true } };
        } };
        [...range].join('')
    )"),
             std::string("123"));
}

TEST(errorsHaveNamesAndStacks)
{
    CHECK_EQ(run("new TypeError('x').name"), std::string("TypeError"));
    CHECK_EQ(run("String(new Error('boom'))"), std::string("Error: boom"));
    CHECK_EQ(run("new RangeError('x') instanceof Error"), std::string("true"));
    // `declare` is how a program promises the host provides a name. The host
    // does not, so the reference is a run-time ReferenceError, not a type error.
    CHECK_EQ(run("declare const missing: number;"
                 "try { missing } catch (e) { e.name }"),
             std::string("ReferenceError"));
    ts::Context ctx;
    CHECK(!ctx.eval("function f() { return new Error('e'); } f().stack").toString().empty());
}

TEST(functionPrototypeMethods)
{
    CHECK_EQ(run("function f(a, b) { return this.n + a + b; } f.call({ n: 1 }, 2, 3)"),
             std::string("6"));
    CHECK_EQ(run("function f(a, b) { return this.n + a + b; } f.apply({ n: 1 }, [2, 3])"),
             std::string("6"));
    CHECK_EQ(run("function f(a, b) { return this.n + a + b; } f.bind({ n: 1 }, 2)(3)"),
             std::string("6"));
}

TEST(promisesResolveThroughTheJobQueue)
{
    ts::Context ctx;
    ctx.eval("var log = ''");
    ctx.eval(
        "Promise.resolve(1).then(v => { log += 'a' + v; return v + 1; })"
        "  .then(v => { log += 'b' + v; })");
    CHECK_EQ(ctx.eval("log").toString(), std::string(""));
    ctx.runJobs();
    CHECK_EQ(ctx.eval("log").toString(), std::string("a1b2"));

    ctx.eval("log = ''; Promise.reject(new Error('x')).catch(e => { log += e.message; })");
    ctx.runJobs();
    CHECK_EQ(ctx.eval("log").toString(), std::string("x"));

    ctx.eval("log = ''; Promise.all([Promise.resolve(1), 2]).then(v => { log += v.join(','); })");
    ctx.runJobs();
    CHECK_EQ(ctx.eval("log").toString(), std::string("1,2"));
}

TEST(consoleGoesThroughThePrintHandler)
{
    ts::Context ctx;
    std::string captured;
    ctx.setPrintHandler(
        [&captured](std::string_view line, bool) { captured += std::string(line); });
    ctx.eval("console.log('n =', 42, [1, 2], { a: 1 })");
    CHECK_EQ(captured, std::string("n = 42 [ 1, 2 ] { a: 1 }\n"));
}
