// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Python: the front end's registration, and the runtime support the lowered
// program relies on. Source goes through the Python lexer and parser into
// the ECMAScript syntax tree and from there into the dynamic engine's
// compiler. The first Python compile in a context installs `__py`: a handful
// of native helpers (indexing, `len`, truth tests, output) and a prelude
// written in JavaScript that defines the built-in functions, the exception
// classes, the Python spellings of the list, string, dict and set methods,
// and the `math` and `random` modules.
#include <cmath>

#include "script/internal/dyn/builtins.hpp"
#include "script/internal/dyn/compiler.hpp"
#include "script/internal/dyn/parser.hpp"
#include "script/internal/dyn/runtime.hpp"
#include "script/internal/dyn/vm.hpp"
#include "script/internal/language.hpp"
#include "script/internal/python/parser.hpp"

namespace script {
namespace internal {
namespace dyn {

Value runProgramUnit(ContextImpl& ctx, CompiledUnit& unit, const std::vector<Value>& args);

namespace {

// ------------------------------------------------------------ natives ---

/// Raise a Python exception class defined by the prelude, so `except
/// IndexError` catches what `__py.getitem` raises.
[[noreturn]] void raise(Runtime& rt, const char* className, const std::string& message)
{
    Value global = detail::Access::make(Type::Object, rt.globalObject.get());
    Value ctor = getProperty(rt, global, rt.intern(className));
    if (!ctor.isFunction())
        throwError(rt, ErrorCode::TypeError, message);
    Value error = constructValue(rt, ctor, {rt.str(fromUtf8(message))});
    throw Exception(ErrorCode::Exception, message, error, rt.captureStack());
}

DynMap* asCollection(const Value& v, ObjClass cls) noexcept
{
    DynObject* obj = asObject(v);
    return obj && obj->cls == cls ? static_cast<DynMap*>(obj) : nullptr;
}

bool hasMethod(Runtime& rt, const Value& v, const char* name)
{
    return asObject(v) && hasProperty(rt, v, rt.intern(name));
}

/// A list or string index: an integer, negative counting from the end.
bool resolveIndex(Runtime& rt, const Value& index, std::size_t length, std::size_t& out)
{
    if (!index.isNumber())
        raise(rt, "TypeError", "indices must be integers, not " + std::string(typeOfName(index)));
    const double d = numberOf(index);
    if (d != std::floor(d))
        raise(rt, "TypeError", "indices must be integers, not float");
    double i = d < 0 ? d + double(length) : d;
    if (i < 0 || i >= double(length))
        return false;
    out = std::size_t(i);
    return true;
}

Value getitem(CallInfo& ci)
{
    Runtime& rt = rtOf(ci);
    const Value& base = ci.arg(0);
    const Value& index = ci.arg(1);
    if (DynArray* a = asArray(base)) {
        std::size_t i = 0;
        if (!resolveIndex(rt, index, a->size(), i))
            raise(rt, "IndexError", "list index out of range");
        return a->at(i);
    }
    if (StringCell* s = asString(base)) {
        std::size_t i = 0;
        if (!resolveIndex(rt, index, s->text().size(), i))
            raise(rt, "IndexError", "string index out of range");
        return rt.str(s->text().substr(i, 1));
    }
    if (DynMap* m = asCollection(base, ObjClass::Map)) {
        const std::int64_t at = m->find(index);
        if (at < 0)
            raise(rt, "KeyError", toUtf8(inspectValue(rt, index, 0)));
        return m->entries[std::size_t(at)].value;
    }
    if (hasMethod(rt, base, "__getitem__"))
        return invokeMethod(rt, base, rt.intern("__getitem__"), {index});
    if (base.isNullish())
        raise(rt, "TypeError", "'NoneType' object is not subscriptable");
    // A host object or a plain JavaScript object: ordinary property access.
    return getProperty(rt, base, toPropertyKey(rt, index).get());
}

Value setitem(CallInfo& ci)
{
    Runtime& rt = rtOf(ci);
    const Value& base = ci.arg(0);
    const Value& index = ci.arg(1);
    const Value& value = ci.arg(2);
    if (DynArray* a = asArray(base)) {
        std::size_t i = 0;
        if (!resolveIndex(rt, index, a->size(), i))
            raise(rt, "IndexError", "list assignment index out of range");
        a->put(i, value);
        return value;
    }
    if (DynMap* m = asCollection(base, ObjClass::Map)) {
        m->insert(index, value);
        return value;
    }
    if (base.isString())
        raise(rt, "TypeError", "'str' object does not support item assignment");
    if (hasMethod(rt, base, "__setitem__")) {
        invokeMethod(rt, base, rt.intern("__setitem__"), {index, value});
        return value;
    }
    if (base.isNullish())
        raise(rt, "TypeError", "'NoneType' object does not support item assignment");
    setProperty(rt, base, toPropertyKey(rt, index).get(), value);
    return value;
}

Value len(CallInfo& ci)
{
    Runtime& rt = rtOf(ci);
    const Value& v = ci.arg(0);
    if (DynArray* a = asArray(v))
        return Value(double(a->size()));
    if (StringCell* s = asString(v))
        return Value(double(s->text().size()));
    if (DynMap* m = asCollection(v, ObjClass::Map))
        return Value(double(m->liveCount));
    if (DynMap* m = asCollection(v, ObjClass::Set))
        return Value(double(m->liveCount));
    if (hasMethod(rt, v, "__len__"))
        return invokeMethod(rt, v, rt.intern("__len__"), {});
    if (asObject(v) && hasProperty(rt, v, rt.atom.length.get()))
        return getProperty(rt, v, rt.atom.length.get());
    raise(rt, "TypeError",
          std::string("object of type '") + typeOfName(v) + "' has no len()");
}

/// Python truth: None, False, zero, NaN, the empty string and the empty
/// container are false; everything else is true.
Value truthy(CallInfo& ci)
{
    Runtime& rt = rtOf(ci);
    const Value& v = ci.arg(0);
    if (DynArray* a = asArray(v))
        return Value(!a->empty());
    if (DynMap* m = asCollection(v, ObjClass::Map))
        return Value(m->liveCount != 0);
    if (DynMap* m = asCollection(v, ObjClass::Set))
        return Value(m->liveCount != 0);
    if (asObject(v)) {
        if (hasMethod(rt, v, "__bool__"))
            return Value(toBoolean(invokeMethod(rt, v, rt.intern("__bool__"), {})));
        if (hasMethod(rt, v, "__len__"))
            return Value(numberOf(invokeMethod(rt, v, rt.intern("__len__"), {})) != 0);
        return Value(true);
    }
    return Value(toBoolean(v));
}

Value write(CallInfo& ci)
{
    Runtime& rt = rtOf(ci);
    rt.ctx.write(toStringValue(rt, ci.arg(0)), false);
    return Value::undefined();
}

Value input(CallInfo& ci)
{
    Runtime& rt = rtOf(ci);
    if (!ci.arg(0).isUndefined())
        rt.ctx.write(toStringValue(rt, ci.arg(0)), false);
    std::string line;
    if (!rt.ctx.readLine || !rt.ctx.readLine(line))
        raise(rt, "EOFError", "EOF when reading a line");
    return rt.str(fromUtf8(line));
}

/// `__py.new(f, args...)`: construct when `f` is a class, call otherwise.
/// Used for a capitalised name the parser could not resolve statically.
Value construct(CallInfo& ci)
{
    Runtime& rt = rtOf(ci);
    const Value& callee = ci.arg(0);
    std::vector<Value> args(ci.args().begin() + (ci.size() ? 1 : 0), ci.args().end());
    DynFunction* fn = asFunction(callee);
    if (!fn)
        raise(rt, "TypeError", "'" + std::string(typeOfName(callee)) + "' object is not callable");
    if (fn->isClassConstructor)
        return constructValue(rt, callee, args);
    return callValue(rt, callee, Value::undefined(), args);
}

// ------------------------------------------------------------ prelude ---

/// Everything Python code can reach by name that is not a keyword. Written
/// against the engine's own JavaScript, so it has no dependencies.
const char* const kPrelude = R"PY(
var __name__ = "__main__";
(function () {
    var py = __py;
    var def = function (proto, name, fn) {
        Object.defineProperty(proto, name, { value: fn, writable: true, configurable: true });
    };
    var isList = Array.isArray;
    var isStr = function (x) { return typeof x === 'string'; };
    var isNum = function (x) { return typeof x === 'number'; };
    var isDict = function (x) { return x instanceof Map; };
    var isSet = function (x) { return x instanceof Set; };
    var isInt = function (x) { return typeof x === 'number' && Number.isInteger(x); };

    // ---- keyword arguments ----
    class KW { constructor(o) { this.o = o; } }
    py.KW = KW;
    py.kw = function (o) { return new KW(o); };
    var takeKw = function (args) {
        if (args.length > 0 && args[args.length - 1] instanceof KW)
            return args.pop().o;
        return {};
    };
    var kwOr = function (kw, name, fallback) { return name in kw ? kw[name] : fallback; };

    // ---- exceptions ----
    var Exception = Error;
    var BaseException = Error;
    class ArithmeticError extends Error {}
    class LookupError extends Error {}
    class ValueError extends Error {}
    class KeyError extends LookupError {}
    class IndexError extends LookupError {}
    class ZeroDivisionError extends ArithmeticError {}
    class OverflowError extends ArithmeticError {}
    class RuntimeError extends Error {}
    class RecursionError extends RuntimeError {}
    class NotImplementedError extends RuntimeError {}
    class AssertionError extends Error {}
    class AttributeError extends Error {}
    class NameError extends Error {}
    class StopIteration extends Error {}
    class EOFError extends Error {}
    for (var cls of [ArithmeticError, LookupError, ValueError, KeyError, IndexError,
                     ZeroDivisionError, OverflowError, RuntimeError, RecursionError,
                     NotImplementedError, AssertionError, AttributeError, NameError,
                     StopIteration, EOFError])
        cls.prototype.name = cls.name;
    globalThis.Exception = Exception; globalThis.BaseException = BaseException;
    globalThis.ArithmeticError = ArithmeticError; globalThis.LookupError = LookupError;
    globalThis.ValueError = ValueError; globalThis.KeyError = KeyError;
    globalThis.IndexError = IndexError; globalThis.ZeroDivisionError = ZeroDivisionError;
    globalThis.OverflowError = OverflowError; globalThis.RuntimeError = RuntimeError;
    globalThis.RecursionError = RecursionError;
    globalThis.NotImplementedError = NotImplementedError;
    globalThis.AssertionError = AssertionError; globalThis.AttributeError = AttributeError;
    globalThis.NameError = NameError; globalThis.StopIteration = StopIteration;
    globalThis.EOFError = EOFError;

    // ---- type names and conversions ----
    var typeName = function (x) {
        if (x === null || x === undefined) return 'NoneType';
        if (typeof x === 'boolean') return 'bool';
        if (isNum(x)) return Number.isInteger(x) ? 'int' : 'float';
        if (isStr(x)) return 'str';
        if (isList(x)) return 'list';
        if (isDict(x)) return 'dict';
        if (isSet(x)) return 'set';
        if (typeof x === 'function') return 'function';
        if (x.constructor && x.constructor.name) return x.constructor.name;
        return 'object';
    };
    py.typeName = typeName;

    var reprString = function (s) {
        var quote = s.indexOf("'") >= 0 && s.indexOf('"') < 0 ? '"' : "'";
        var out = quote;
        for (var i = 0; i < s.length; i++) {
            var c = s[i];
            if (c === '\\') out += '\\\\';
            else if (c === '\n') out += '\\n';
            else if (c === '\t') out += '\\t';
            else if (c === '\r') out += '\\r';
            else if (c === quote) out += '\\' + quote;
            else out += c;
        }
        return out + quote;
    };
    var numStr = function (x) {
        if (x !== x) return 'nan';
        if (x === Infinity) return 'inf';
        if (x === -Infinity) return '-inf';
        return String(x);
    };
    var str = function (x) {
        if (x === null || x === undefined) return 'None';
        if (x === true) return 'True';
        if (x === false) return 'False';
        if (isStr(x)) return x;
        if (isNum(x)) return numStr(x);
        if (isList(x)) return '[' + x.map(repr).join(', ') + ']';
        if (isDict(x)) {
            var parts = [];
            x.forEach(function (v, k) { parts.push(repr(k) + ': ' + repr(v)); });
            return '{' + parts.join(', ') + '}';
        }
        if (isSet(x)) {
            if (x.size === 0) return 'set()';
            var items = [];
            x.forEach(function (v) { items.push(repr(v)); });
            return '{' + items.join(', ') + '}';
        }
        if (typeof x.__str__ === 'function') return x.__str__();
        if (typeof x.__repr__ === 'function') return x.__repr__();
        if (x instanceof Error) return x.message;
        if (typeof x === 'function') return '<function ' + (x.name || '<lambda>') + '>';
        if (x instanceof KW) return str(x.o);
        return '<' + typeName(x) + ' object>';
    };
    var repr = function (x) {
        if (isStr(x)) return reprString(x);
        if (x !== null && x !== undefined && typeof x === 'object') {
            if (typeof x.__repr__ === 'function') return x.__repr__();
            if (x instanceof Error) return typeName(x) + '(' + reprString(x.message) + ')';
        }
        return str(x);
    };
    py.str = str;
    py.repr = repr;

    // ---- operators ----
    var eq = function (a, b) {
        if (a === b) return true;
        if (a === null || a === undefined || b === null || b === undefined) return a == b;
        if (typeof a !== 'object' || typeof b !== 'object') return false;
        if (isList(a) && isList(b)) {
            if (a.length !== b.length) return false;
            for (var i = 0; i < a.length; i++) if (!eq(a[i], b[i])) return false;
            return true;
        }
        if (isDict(a) && isDict(b)) {
            if (a.size !== b.size) return false;
            for (var [k, v] of a) if (!b.has(k) || !eq(v, b.get(k))) return false;
            return true;
        }
        if (isSet(a) && isSet(b)) {
            if (a.size !== b.size) return false;
            for (var e of a) if (!b.has(e)) return false;
            return true;
        }
        if (typeof a.__eq__ === 'function') return !!a.__eq__(b);
        return false;
    };
    py.eq = eq;
    py.contains = function (c, x) {
        if (isStr(c)) {
            if (!isStr(x)) throw new TypeError("'in <string>' requires string as left operand, not " + typeName(x));
            return c.includes(x);
        }
        if (isList(c)) {
            for (var i = 0; i < c.length; i++) if (eq(c[i], x)) return true;
            return false;
        }
        if (isDict(c) || isSet(c)) return c.has(x);
        if (c !== null && c !== undefined && typeof c.__contains__ === 'function') return !!c.__contains__(x);
        if (c !== null && c !== undefined && typeof c[Symbol.iterator] === 'function') {
            for (var e of c) if (eq(e, x)) return true;
            return false;
        }
        throw new TypeError("argument of type '" + typeName(c) + "' is not iterable");
    };
    py.iter = function (x) {
        if (isDict(x)) return Array.from(x.keys());
        if (x === null || x === undefined) throw new TypeError("'NoneType' object is not iterable");
        return x;
    };
    var toIterable = function (x) {
        if (isDict(x)) return Array.from(x.keys());
        if (x === null || x === undefined || typeof x[Symbol.iterator] !== 'function')
            throw new TypeError("'" + typeName(x) + "' object is not iterable");
        return x;
    };
    var toList = function (x) { return isList(x) ? x : Array.from(toIterable(x)); };
    py.add = function (a, b) {
        if (isList(a) && isList(b)) return a.concat(b);
        return a + b;
    };
    py.mul = function (a, b) {
        if (isStr(a) && isNum(b)) return b > 0 ? a.repeat(b) : '';
        if (isNum(a) && isStr(b)) return a > 0 ? b.repeat(a) : '';
        if (isList(a) && isNum(b)) {
            // Append in place: `out = out.concat(a)` copies everything built so far on every pass, which makes
            // `[0] * n` quadratic in n.
            var out = [];
            for (var i = 0; i < b; i++) for (var j = 0; j < a.length; j++) out.push(a[j]);
            return out;
        }
        if (isNum(a) && isList(b)) return py.mul(b, a);
        return a * b;
    };
    py.mod = function (a, b) {
        if (isStr(a)) return percentFormat(a, b);
        if (b === 0) throw new ZeroDivisionError('integer division or modulo by zero');
        var r = a % b;
        return r !== 0 && (r < 0) !== (b < 0) ? r + b : r;
    };
    var cmp = function (a, b) {
        if (isNum(a) && isNum(b)) return a < b ? -1 : a > b ? 1 : 0;
        if (isStr(a) && isStr(b)) return a < b ? -1 : a > b ? 1 : 0;
        if (isList(a) && isList(b)) {
            var n = Math.min(a.length, b.length);
            for (var i = 0; i < n; i++) { var c = cmp(a[i], b[i]); if (c !== 0) return c; }
            return a.length - b.length;
        }
        if (typeof a === 'boolean' && typeof b === 'boolean') return (a ? 1 : 0) - (b ? 1 : 0);
        if (a !== null && a !== undefined && typeof a.__lt__ === 'function')
            return a.__lt__(b) ? -1 : (b !== null && b !== undefined && typeof b.__lt__ === 'function' && b.__lt__(a)) ? 1 : 0;
        throw new TypeError("'<' not supported between instances of '" + typeName(a) + "' and '" + typeName(b) + "'");
    };
    py.cmp = cmp;

    // ---- slicing ----
    var normalizeSlice = function (n, lo, hi, step) {
        if (step === undefined || step === null) step = 1;
        if (step === 0) throw new ValueError('slice step cannot be zero');
        var start, stop;
        if (step > 0) {
            start = lo === undefined || lo === null ? 0 : lo < 0 ? Math.max(lo + n, 0) : Math.min(lo, n);
            stop = hi === undefined || hi === null ? n : hi < 0 ? Math.max(hi + n, 0) : Math.min(hi, n);
        } else {
            start = lo === undefined || lo === null ? n - 1 : lo < 0 ? Math.max(lo + n, -1) : Math.min(lo, n - 1);
            stop = hi === undefined || hi === null ? -1 : hi < 0 ? Math.max(hi + n, -1) : Math.min(hi, n - 1);
        }
        return [start, stop, step];
    };
    py.slice = function (x, lo, hi, step) {
        if (!isList(x) && !isStr(x)) {
            if (x !== null && x !== undefined && typeof x.__getitem__ === 'function')
                return x.__getitem__([lo, hi, step]);
            throw new TypeError("'" + typeName(x) + "' object is not subscriptable");
        }
        var r = normalizeSlice(x.length, lo, hi, step), i = r[0], stop = r[1], s = r[2];
        if (s === 1) return x.slice(i, Math.max(stop, i));
        var out = [];
        if (s > 0) for (; i < stop; i += s) out.push(x[i]);
        else for (; i > stop; i += s) out.push(x[i]);
        return isStr(x) ? out.join('') : out;
    };
    py.setslice = function (x, lo, hi, step, v) {
        if (!isList(x)) throw new TypeError("'" + typeName(x) + "' object does not support slice assignment");
        if (step !== undefined && step !== null && step !== 1) throw new ValueError('extended slice assignment is not supported');
        var r = normalizeSlice(x.length, lo, hi, 1);
        x.splice(r[0], Math.max(r[1] - r[0], 0), ...toList(v));
        return v;
    };
    py.delitem = function (x, i) {
        if (isList(x)) {
            if (!isInt(i)) throw new TypeError('list indices must be integers');
            var j = i < 0 ? i + x.length : i;
            if (j < 0 || j >= x.length) throw new IndexError('list assignment index out of range');
            x.splice(j, 1);
            return;
        }
        if (isDict(x)) {
            if (!x.delete(i)) throw new KeyError(repr(i));
            return;
        }
        if (x !== null && x !== undefined && typeof x.__delitem__ === 'function') { x.__delitem__(i); return; }
        throw new TypeError("'" + typeName(x) + "' object does not support item deletion");
    };
    py.delslice = function (x, lo, hi, step) {
        if (!isList(x)) throw new TypeError("'" + typeName(x) + "' object does not support slice deletion");
        var r = normalizeSlice(x.length, lo, hi, step);
        if (r[2] === 1) { x.splice(r[0], Math.max(r[1] - r[0], 0)); return; }
        var keep = [];
        var drop = new Set(py.slice(Array.from(x.keys()), lo, hi, step));
        for (var i = 0; i < x.length; i++) if (!drop.has(i)) keep.push(x[i]);
        x.length = 0;
        x.push(...keep);
    };

    // ---- formatting ----
    var groupThousands = function (digits, sep) {
        var out = '';
        for (var i = 0; i < digits.length; i++) {
            if (i > 0 && (digits.length - i) % 3 === 0) out += sep;
            out += digits[i];
        }
        return out;
    };
    var fixExponent = function (s) {
        // JavaScript writes 1e+2 where Python writes 1e+02.
        var at = s.indexOf('e');
        if (at < 0) return s;
        var mant = s.slice(0, at), exp = s.slice(at + 1);
        var sign = exp[0] === '-' ? '-' : '+';
        var digits = exp.replace('+', '').replace('-', '');
        if (digits.length < 2) digits = '0' + digits;
        return mant + 'e' + sign + digits;
    };
    var pad = function (s, width, fill, align, numeric) {
        if (s.length >= width) return s;
        var n = width - s.length;
        if (!align) align = numeric ? '>' : '<';
        if (align === '<') return s + fill.repeat(n);
        if (align === '>') return fill.repeat(n) + s;
        if (align === '^') { var left = Math.floor(n / 2); return fill.repeat(left) + s + fill.repeat(n - left); }
        // '=': padding goes after the sign.
        var signLen = s[0] === '-' || s[0] === '+' || s[0] === ' ' ? 1 : 0;
        return s.slice(0, signLen) + fill.repeat(n) + s.slice(signLen);
    };
    var formatNumber = function (v, type, precision, alt) {
        var a = Math.abs(v);
        switch (type) {
            case 'f': case 'F': return a.toFixed(precision < 0 ? 6 : precision);
            case 'e': case 'E': {
                var s = fixExponent(a.toExponential(precision < 0 ? 6 : precision));
                return type === 'E' ? s.toUpperCase() : s;
            }
            case 'g': case 'G': {
                var p = precision < 0 ? 6 : precision === 0 ? 1 : precision;
                if (a === 0) return '0';
                var exp = Math.floor(Math.log10(a));
                var s2;
                if (exp < -4 || exp >= p) {
                    s2 = fixExponent(a.toExponential(p - 1));
                    if (!alt) {
                        var ePos = s2.indexOf('e'), mant = s2.slice(0, ePos), rest = s2.slice(ePos);
                        if (mant.indexOf('.') >= 0) {
                            while (mant[mant.length - 1] === '0') mant = mant.slice(0, -1);
                            if (mant[mant.length - 1] === '.') mant = mant.slice(0, -1);
                        }
                        s2 = mant + rest;
                    }
                } else {
                    s2 = a.toFixed(Math.max(p - 1 - exp, 0));
                    if (!alt && s2.indexOf('.') >= 0) {
                        while (s2[s2.length - 1] === '0') s2 = s2.slice(0, -1);
                        if (s2[s2.length - 1] === '.') s2 = s2.slice(0, -1);
                    }
                }
                return type === 'G' ? s2.toUpperCase() : s2;
            }
            case '%': return (a * 100).toFixed(precision < 0 ? 6 : precision) + '%';
            case 'd': case 'n': return String(Math.trunc(a));
            case 'x': return (alt ? '0x' : '') + Math.trunc(a).toString(16);
            case 'X': return (alt ? '0X' : '') + Math.trunc(a).toString(16).toUpperCase();
            case 'o': return (alt ? '0o' : '') + Math.trunc(a).toString(8);
            case 'b': return (alt ? '0b' : '') + Math.trunc(a).toString(2);
            case 'c': return String.fromCharCode(Math.trunc(a));
            default:
                if (precision >= 0) return formatNumber(v, 'g', precision, alt);
                return numStr(a);
        }
    };
    py.format = function (v, spec) {
        if (v !== null && v !== undefined && typeof v.__format__ === 'function') return v.__format__(spec);
        var i = 0, fill = ' ', align = '', sign = '-', alt = false, zero = false, width = 0, group = '', precision = -1, type = '';
        if (spec.length >= 2 && '<>^='.indexOf(spec[1]) >= 0) { fill = spec[0]; align = spec[1]; i = 2; }
        else if (spec.length >= 1 && '<>^='.indexOf(spec[0]) >= 0) { align = spec[0]; i = 1; }
        if (i < spec.length && '+- '.indexOf(spec[i]) >= 0) sign = spec[i++];
        if (spec[i] === '#') { alt = true; i++; }
        if (spec[i] === '0') { zero = true; i++; }
        while (i < spec.length && spec[i] >= '0' && spec[i] <= '9') width = width * 10 + (spec.charCodeAt(i++) - 48);
        if (spec[i] === ',' || spec[i] === '_') group = spec[i++];
        if (spec[i] === '.') {
            i++;
            precision = 0;
            while (i < spec.length && spec[i] >= '0' && spec[i] <= '9') precision = precision * 10 + (spec.charCodeAt(i++) - 48);
        }
        if (i < spec.length) type = spec[i++];
        if (i < spec.length) throw new ValueError('Invalid format specifier');
        var s;
        if (isNum(v) || typeof v === 'boolean') {
            var n = typeof v === 'boolean' ? (v ? 1 : 0) : v;
            if (type === 's') { s = str(v); }
            else {
                var body = formatNumber(n, type, precision, alt);
                if (group) {
                    var dot = body.indexOf('.'), e = body.indexOf('e');
                    var end = dot >= 0 ? dot : e >= 0 ? e : body.length;
                    body = groupThousands(body.slice(0, end), group) + body.slice(end);
                }
                var neg = n < 0 || (n === 0 && 1 / n < 0);
                var signStr = neg ? '-' : sign === '+' ? '+' : sign === ' ' ? ' ' : '';
                if (zero && !align) { align = '='; fill = '0'; }
                s = signStr + body;
            }
            return pad(s, width, fill, align, true);
        }
        if (type && type !== 's') throw new ValueError("Unknown format code '" + type + "' for object of type '" + typeName(v) + "'");
        s = str(v);
        if (precision >= 0) s = s.slice(0, precision);
        return pad(s, width, fill, align, false);
    };
    var percentFormat = function (fmt, args) {
        var specs = 0;
        for (var k = 0; k < fmt.length; k++) {
            if (fmt[k] !== '%') continue;
            if (fmt[k + 1] === '%') { k++; continue; }
            specs++;
        }
        if (!isList(args) || specs !== args.length) args = [args];
        var out = '', next = 0;
        for (var i = 0; i < fmt.length; i++) {
            var c = fmt[i];
            if (c !== '%') { out += c; continue; }
            i++;
            if (fmt[i] === '%') { out += '%'; continue; }
            var flags = '';
            while ('-+ 0#'.indexOf(fmt[i]) >= 0) flags += fmt[i++];
            var width = '';
            while (fmt[i] >= '0' && fmt[i] <= '9') width += fmt[i++];
            var precision = '';
            if (fmt[i] === '.') { i++; precision = '.'; while (fmt[i] >= '0' && fmt[i] <= '9') precision += fmt[i++]; if (precision === '.') precision = '.0'; }
            var type = fmt[i];
            if (next >= args.length) throw new TypeError('not enough arguments for format string');
            var v = args[next++];
            var spec = '';
            if (flags.indexOf('-') >= 0) spec += '<';
            if (flags.indexOf('+') >= 0) spec += '+'; else if (flags.indexOf(' ') >= 0) spec += ' ';
            if (flags.indexOf('#') >= 0) spec += '#';
            if (flags.indexOf('0') >= 0 && flags.indexOf('-') < 0) spec += '0';
            spec += width;
            if (type === 's') { out += py.format(str(v), spec + precision + 's'); continue; }
            if (type === 'r') { out += py.format(repr(v), spec + precision + 's'); continue; }
            if (type === 'i' || type === 'u') type = 'd';
            if (type === 'd' && !isNum(v)) throw new TypeError('%d format: a real number is required, not ' + typeName(v));
            out += py.format(v, spec + (type === 'd' ? '' : precision) + type);
        }
        if (next < args.length) throw new TypeError('not all arguments converted during string formatting');
        return out;
    };

    // ---- reading a method without calling it: `f = obj.method` ----
    py.bindRead = function (obj, name, value) {
        // A class's own functions, an instance's own callable attribute and the class itself are plain values.
        if (typeof obj === 'function' || name === 'constructor' ||
            Object.prototype.hasOwnProperty.call(Object(obj), name))
            return value;
        var dispatched = py['m_' + name];
        if (dispatched && (value !== undefined || isList(obj) || isStr(obj)))
            return function (...args) { return dispatched(obj, ...args); };
        return value === undefined ? value : value.bind(obj);
    };

    // ---- method helpers: names whose meaning differs between the languages ----
    var pySort = function (list, args) {
        var kw = takeKw(args);
        var key = kwOr(kw, 'key', args.length > 0 ? args[0] : null);
        var reverse = !!kwOr(kw, 'reverse', args.length > 1 ? args[1] : false);
        var keyed = list.map(function (v, i) { return [key ? key(v) : v, i, v]; });
        keyed.sort(function (a, b) { var c = cmp(a[0], b[0]); return c !== 0 ? (reverse ? -c : c) : a[1] - b[1]; });
        for (var i = 0; i < list.length; i++) list[i] = keyed[i][2];
        return list;
    };
    py.m_sort = function (obj, ...args) {
        if (isList(obj)) { pySort(obj, args); return undefined; }
        return obj.sort(...args);
    };
    py.m_join = function (obj, ...args) {
        if (isStr(obj)) {
            var parts = toList(args[0]);
            for (var i = 0; i < parts.length; i++)
                if (!isStr(parts[i])) throw new TypeError('sequence item ' + i + ': expected str instance, ' + typeName(parts[i]) + ' found');
            return parts.join(obj);
        }
        return obj.join(...args);
    };
    py.m_split = function (obj, ...args) {
        if (!isStr(obj)) return obj.split(...args);
        var kw = takeKw(args);
        var sep = kwOr(kw, 'sep', args.length > 0 ? args[0] : null);
        var maxsplit = kwOr(kw, 'maxsplit', args.length > 1 ? args[1] : -1);
        if (sep === null || sep === undefined) {
            var words = [], word = '', splits = 0;
            for (var i = 0; i < obj.length; i++) {
                var c = obj[i];
                var space = c === ' ' || c === '\t' || c === '\n' || c === '\r' || c === '\f' || c === '\v';
                if (space) {
                    if (word) { words.push(word); word = ''; splits++; }
                    if (maxsplit >= 0 && splits >= maxsplit) {
                        var rest = obj.slice(i).trimStart();
                        if (rest) words.push(rest);
                        return words;
                    }
                } else word += c;
            }
            if (word) words.push(word);
            return words;
        }
        if (sep === '') throw new ValueError('empty separator');
        if (maxsplit < 0) return obj.split(sep);
        var out = [], at = 0, count = 0;
        while (count < maxsplit) {
            var found = obj.indexOf(sep, at);
            if (found < 0) break;
            out.push(obj.slice(at, found));
            at = found + sep.length;
            count++;
        }
        out.push(obj.slice(at));
        return out;
    };
    py.m_replace = function (obj, ...args) {
        if (!isStr(obj)) return obj.replace(...args);
        var count = args.length > 2 ? args[2] : -1;
        if (count < 0) return args[0] === '' ? args[1] + obj.split('').join(args[1]) + args[1] : obj.replaceAll(args[0], args[1]);
        var out = obj;
        for (var i = 0; i < count; i++) {
            var at = out.indexOf(args[0]);
            if (at < 0) break;
            out = out.slice(0, at) + args[1] + out.slice(at + args[0].length);
        }
        return out;
    };
    py.m_pop = function (obj, ...args) {
        if (isList(obj)) {
            if (obj.length === 0) throw new IndexError('pop from empty list');
            if (args.length === 0) return obj.pop();
            var i = args[0] < 0 ? args[0] + obj.length : args[0];
            if (i < 0 || i >= obj.length) throw new IndexError('pop index out of range');
            return obj.splice(i, 1)[0];
        }
        if (isDict(obj)) {
            if (obj.has(args[0])) { var v = obj.get(args[0]); obj.delete(args[0]); return v; }
            if (args.length > 1) return args[1];
            throw new KeyError(repr(args[0]));
        }
        if (isSet(obj)) {
            for (var e of obj) { obj.delete(e); return e; }
            throw new KeyError('pop from an empty set');
        }
        return obj.pop(...args);
    };
    py.m_keys = function (obj, ...args) { return isDict(obj) ? Array.from(obj.keys()) : obj.keys(...args); };
    py.m_values = function (obj, ...args) { return isDict(obj) ? Array.from(obj.values()) : obj.values(...args); };

    // ---- list methods ----
    var AP = Array.prototype;
    def(AP, 'append', function (v) { this.push(v); });
    def(AP, 'extend', function (it) { this.push(...toList(it)); });
    def(AP, 'insert', function (i, v) {
        if (i < 0) i = Math.max(i + this.length, 0);
        this.splice(Math.min(i, this.length), 0, v);
    });
    def(AP, 'remove', function (v) {
        for (var i = 0; i < this.length; i++) if (eq(this[i], v)) { this.splice(i, 1); return; }
        throw new ValueError('list.remove(x): x not in list');
    });
    def(AP, 'clear', function () { this.length = 0; });
    def(AP, 'index', function (v, start, stop) {
        var lo = start === undefined ? 0 : start < 0 ? Math.max(start + this.length, 0) : start;
        var hi = stop === undefined ? this.length : stop < 0 ? stop + this.length : Math.min(stop, this.length);
        for (var i = lo; i < hi; i++) if (eq(this[i], v)) return i;
        throw new ValueError(repr(v) + ' is not in list');
    });
    def(AP, 'count', function (v) { var n = 0; for (var i = 0; i < this.length; i++) if (eq(this[i], v)) n++; return n; });
    def(AP, 'copy', function () { return this.slice(); });

    // ---- string methods ----
    var SP = String.prototype;
    var isSpaceChar = function (c) { return c === ' ' || c === '\t' || c === '\n' || c === '\r' || c === '\f' || c === '\v'; };
    var stripWith = function (s, chars, left, right) {
        var isStrip = chars === undefined || chars === null ? isSpaceChar : function (c) { return chars.indexOf(c) >= 0; };
        var i = 0, j = s.length;
        if (left) while (i < j && isStrip(s[i])) i++;
        if (right) while (j > i && isStrip(s[j - 1])) j--;
        return s.slice(i, j);
    };
    def(SP, 'upper', function () { return this.toUpperCase(); });
    def(SP, 'lower', function () { return this.toLowerCase(); });
    def(SP, 'strip', function (chars) { return stripWith(this, chars, true, true); });
    def(SP, 'lstrip', function (chars) { return stripWith(this, chars, true, false); });
    def(SP, 'rstrip', function (chars) { return stripWith(this, chars, false, true); });
    def(SP, 'startswith', function (p, start) {
        if (isList(p)) { for (var i = 0; i < p.length; i++) if (this.startsWith(p[i], start)) return true; return false; }
        return this.startsWith(p, start);
    });
    def(SP, 'endswith', function (p) {
        if (isList(p)) { for (var i = 0; i < p.length; i++) if (this.endsWith(p[i])) return true; return false; }
        return this.endsWith(p);
    });
    def(SP, 'find', function (sub, start) { return this.indexOf(sub, start === undefined ? 0 : start < 0 ? start + this.length : start); });
    def(SP, 'rfind', function (sub) { return this.lastIndexOf(sub); });
    def(SP, 'index', function (sub, start) {
        var at = this.indexOf(sub, start === undefined ? 0 : start);
        if (at < 0) throw new ValueError('substring not found');
        return at;
    });
    def(SP, 'rindex', function (sub) {
        var at = this.lastIndexOf(sub);
        if (at < 0) throw new ValueError('substring not found');
        return at;
    });
    def(SP, 'count', function (sub) {
        if (sub === '') return this.length + 1;
        var n = 0, at = 0;
        for (;;) { var f = this.indexOf(sub, at); if (f < 0) return n; n++; at = f + sub.length; }
    });
    var everyChar = function (s, test) { if (s.length === 0) return false; for (var i = 0; i < s.length; i++) if (!test(s[i])) return false; return true; };
    var isDigitChar = function (c) { return c >= '0' && c <= '9'; };
    var isAlphaChar = function (c) { return c.toLowerCase() !== c.toUpperCase() || (c > '\x7f' && !isSpaceChar(c)); };
    def(SP, 'isdigit', function () { return everyChar(this, isDigitChar); });
    def(SP, 'isnumeric', function () { return everyChar(this, isDigitChar); });
    def(SP, 'isdecimal', function () { return everyChar(this, isDigitChar); });
    def(SP, 'isalpha', function () { return everyChar(this, isAlphaChar); });
    def(SP, 'isalnum', function () { return everyChar(this, function (c) { return isDigitChar(c) || isAlphaChar(c); }); });
    def(SP, 'isspace', function () { return everyChar(this, isSpaceChar); });
    def(SP, 'isupper', function () { return this !== this.toLowerCase() && this === this.toUpperCase(); });
    def(SP, 'islower', function () { return this !== this.toUpperCase() && this === this.toLowerCase(); });
    def(SP, 'capitalize', function () { return this.length ? this[0].toUpperCase() + this.slice(1).toLowerCase() : ''; });
    def(SP, 'title', function () {
        var out = '', startWord = true;
        for (var i = 0; i < this.length; i++) {
            var c = this[i];
            if (isAlphaChar(c)) { out += startWord ? c.toUpperCase() : c.toLowerCase(); startWord = false; }
            else { out += c; startWord = true; }
        }
        return out;
    });
    def(SP, 'swapcase', function () {
        var out = '';
        for (var i = 0; i < this.length; i++) { var c = this[i]; var u = c.toUpperCase(); out += c === u ? c.toLowerCase() : u; }
        return out;
    });
    def(SP, 'center', function (width, fill) { return pad(String(this), width, fill === undefined ? ' ' : fill, '^', false); });
    def(SP, 'ljust', function (width, fill) { return this.padEnd(width, fill === undefined ? ' ' : fill); });
    def(SP, 'rjust', function (width, fill) { return this.padStart(width, fill === undefined ? ' ' : fill); });
    def(SP, 'zfill', function (width) {
        var s = String(this);
        var sign = s[0] === '-' || s[0] === '+' ? s[0] : '';
        return sign + s.slice(sign.length).padStart(width - sign.length, '0');
    });
    def(SP, 'splitlines', function (keepends) {
        var out = [], line = '';
        for (var i = 0; i < this.length; i++) {
            var c = this[i];
            if (c === '\n' || c === '\r') {
                if (c === '\r' && this[i + 1] === '\n') { if (keepends) line += '\r'; i++; c = '\n'; }
                out.push(keepends ? line + c : line);
                line = '';
            } else line += c;
        }
        if (line) out.push(line);
        return out;
    });
    def(SP, 'partition', function (sep) {
        var at = this.indexOf(sep);
        return at < 0 ? [String(this), '', ''] : [this.slice(0, at), sep, this.slice(at + sep.length)];
    });
    def(SP, 'rpartition', function (sep) {
        var at = this.lastIndexOf(sep);
        return at < 0 ? ['', '', String(this)] : [this.slice(0, at), sep, this.slice(at + sep.length)];
    });
    def(SP, 'rsplit', function (...args) { return py.m_split(String(this), ...args); });
    def(SP, 'format', function (...args) {
        var kw = takeKw(args);
        var s = String(this), out = '', auto = 0;
        for (var i = 0; i < s.length; i++) {
            var c = s[i];
            if (c === '{' && s[i + 1] === '{') { out += '{'; i++; continue; }
            if (c === '}' && s[i + 1] === '}') { out += '}'; i++; continue; }
            if (c !== '{') { out += c; continue; }
            var end = s.indexOf('}', i);
            if (end < 0) throw new ValueError("Single '{' encountered in format string");
            var field = s.slice(i + 1, end), spec = '', conv = '';
            var colon = field.indexOf(':');
            if (colon >= 0) { spec = field.slice(colon + 1); field = field.slice(0, colon); }
            var bang = field.indexOf('!');
            if (bang >= 0) { conv = field.slice(bang + 1); field = field.slice(0, bang); }
            var dot = field.indexOf('.'), bracket = field.indexOf('[');
            var head = field, tail = '';
            var cut = dot >= 0 && (bracket < 0 || dot < bracket) ? dot : bracket;
            if (cut >= 0) { head = field.slice(0, cut); tail = field.slice(cut); }
            var v;
            if (head === '') v = args[auto++];
            else if (isDigitChar(head[0])) v = args[Number(head)];
            else if (head in kw) v = kw[head];
            else throw new KeyError(repr(head));
            while (tail) {
                if (tail[0] === '.') {
                    var stop = tail.indexOf('.', 1), stop2 = tail.indexOf('[', 1);
                    var e = stop < 0 ? (stop2 < 0 ? tail.length : stop2) : (stop2 < 0 ? stop : Math.min(stop, stop2));
                    v = v[tail.slice(1, e)];
                    tail = tail.slice(e);
                } else {
                    var close = tail.indexOf(']');
                    var key = tail.slice(1, close);
                    v = __py.getitem(v, isDigitChar(key[0]) ? Number(key) : key);
                    tail = tail.slice(close + 1);
                }
            }
            if (conv === 'r') v = repr(v);
            else if (conv === 's') v = str(v);
            out += spec ? py.format(v, spec) : str(v);
            i = end;
        }
        return out;
    });
    def(SP, 'encode', function () { return String(this); });

    // ---- dict methods ----
    var MP = Map.prototype;
    var mapGet = MP.get;
    def(MP, 'get', function (k, fallback) { return this.has(k) ? mapGet.call(this, k) : fallback === undefined ? null : fallback; });
    def(MP, 'items', function () { return Array.from(this.entries()); });
    def(MP, 'update', function (other, ...rest) {
        var kw = takeKw(rest);
        if (other instanceof KW) { kw = other.o; other = undefined; }
        if (other !== undefined && other !== null) {
            if (isDict(other)) other.forEach((v, k) => { this.set(k, v); });
            else for (var pair of other) this.set(pair[0], pair[1]);
        }
        for (var name in kw) this.set(name, kw[name]);
    });
    def(MP, 'setdefault', function (k, v) {
        if (this.has(k)) return mapGet.call(this, k);
        this.set(k, v === undefined ? null : v);
        return v === undefined ? null : v;
    });
    def(MP, 'copy', function () { return new Map(this); });
    def(MP, 'popitem', function () {
        var last;
        for (var pair of this) last = pair;
        if (!last) throw new KeyError('popitem(): dictionary is empty');
        this.delete(last[0]);
        return last;
    });

    // ---- set methods ----
    var TP = Set.prototype;
    def(TP, 'remove', function (v) { if (!this.delete(v)) throw new KeyError(repr(v)); });
    def(TP, 'discard', function (v) { this.delete(v); });
    def(TP, 'update', function (...others) { for (var o of others) for (var v of toIterable(o)) this.add(v); });
    def(TP, 'copy', function () { return new Set(this); });
    def(TP, 'union', function (...others) { var s = new Set(this); for (var o of others) for (var v of toIterable(o)) s.add(v); return s; });
    def(TP, 'intersection', function (other) { var s = new Set(); var o = new Set(toIterable(other)); for (var v of this) if (o.has(v)) s.add(v); return s; });
    def(TP, 'difference', function (other) { var s = new Set(); var o = new Set(toIterable(other)); for (var v of this) if (!o.has(v)) s.add(v); return s; });
    def(TP, 'symmetric_difference', function (other) {
        var s = new Set(); var o = new Set(toIterable(other));
        for (var v of this) if (!o.has(v)) s.add(v);
        for (var w of o) if (!this.has(w)) s.add(w);
        return s;
    });
    def(TP, 'issubset', function (other) { var o = new Set(toIterable(other)); for (var v of this) if (!o.has(v)) return false; return true; });
    def(TP, 'issuperset', function (other) { for (var v of toIterable(other)) if (!this.has(v)) return false; return true; });
    def(TP, 'isdisjoint', function (other) { for (var v of toIterable(other)) if (this.has(v)) return false; return true; });

    // ---- built-in functions ----
    var G = globalThis;
    G.print = function (...args) {
        var kw = takeKw(args);
        var sep = kwOr(kw, 'sep', ' '), end = kwOr(kw, 'end', '\n');
        py.write(args.map(str).join(sep === null ? ' ' : sep) + (end === null ? '\n' : end));
    };
    G.len = py.len;
    G.input = py.input;
    G.repr = repr;
    G.range = function (a, b, c) {
        var start = b === undefined ? 0 : a, stop = b === undefined ? a : b, step = c === undefined ? 1 : c;
        if (step === 0) throw new ValueError('range() arg 3 must not be zero');
        var out = [];
        if (step > 0) for (var i = start; i < stop; i += step) out.push(i);
        else for (var j = start; j > stop; j += step) out.push(j);
        return out;
    };
    G.str = function (x) { return arguments.length === 0 ? '' : str(x); };
    G.int = function (x, base) {
        if (arguments.length === 0) return 0;
        if (x instanceof KW) { base = x.o.base; x = arguments[0]; }
        if (isStr(x)) {
            var s = x.trim().split('_').join('');
            if (base === undefined) {
                var sign = s[0] === '-' ? -1 : 1;
                var digits = s[0] === '-' || s[0] === '+' ? s.slice(1) : s;
                if (!everyChar(digits, isDigitChar)) throw new ValueError("invalid literal for int() with base 10: " + repr(x));
                return sign * Number(digits);
            }
            var n = parseInt(s, base);
            if (n !== n) throw new ValueError('invalid literal for int() with base ' + base + ': ' + repr(x));
            return n;
        }
        if (typeof x === 'boolean') return x ? 1 : 0;
        if (isNum(x)) {
            if (x !== x || x === Infinity || x === -Infinity) throw new ValueError('cannot convert float ' + numStr(x) + ' to integer');
            return Math.trunc(x);
        }
        if (x !== null && x !== undefined && typeof x.__int__ === 'function') return x.__int__();
        throw new TypeError("int() argument must be a string or a number, not '" + typeName(x) + "'");
    };
    G.float = function (x) {
        if (arguments.length === 0) return 0;
        if (isStr(x)) {
            var s = x.trim().toLowerCase();
            if (s === 'inf' || s === '+inf' || s === 'infinity') return Infinity;
            if (s === '-inf' || s === '-infinity') return -Infinity;
            if (s === 'nan') return NaN;
            var n = s === '' ? NaN : Number(s);
            if (n !== n) throw new ValueError('could not convert string to float: ' + repr(x));
            return n;
        }
        if (typeof x === 'boolean') return x ? 1 : 0;
        if (isNum(x)) return x;
        if (x !== null && x !== undefined && typeof x.__float__ === 'function') return x.__float__();
        throw new TypeError("float() argument must be a string or a number, not '" + typeName(x) + "'");
    };
    G.bool = function (x) { return arguments.length === 0 ? false : py.truthy(x); };
    G.list = function (x) { return arguments.length === 0 ? [] : Array.from(toIterable(x)); };
    G.tuple = G.list;
    G.dict = function (x, ...rest) {
        var kw = takeKw(rest);
        if (x instanceof KW) { kw = x.o; x = undefined; }
        var m;
        if (x === undefined || x === null) m = new Map();
        else if (isDict(x)) m = new Map(x);
        else { m = new Map(); for (var pair of toIterable(x)) m.set(pair[0], pair[1]); }
        for (var name in kw) m.set(name, kw[name]);
        return m;
    };
    G.set = function (x) { return arguments.length === 0 ? new Set() : new Set(toIterable(x)); };
    G.frozenset = G.set;
    G.abs = function (x) { if (x !== null && x !== undefined && typeof x.__abs__ === 'function') return x.__abs__(); return Math.abs(x); };
    G.round = function (x, n) {
        if (n === undefined || n === null) {
            var r = Math.round(x);
            // Half to even, as Python rounds.
            if (Math.abs(x - Math.trunc(x)) === 0.5 && r % 2 !== 0) r -= 1;
            return r;
        }
        var scale = Math.pow(10, n);
        return Math.round(x * scale) / scale;
    };
    G.divmod = function (a, b) { return [Math.floor(a / b), py.mod(a, b)]; };
    G.pow = function (a, b, m) { return m === undefined ? Math.pow(a, b) : py.mod(Math.pow(a, b), m); };
    var minmax = function (args, sign, name) {
        var kw = takeKw(args);
        var key = kwOr(kw, 'key', null);
        var items = args.length === 1 ? toList(args[0]) : args;
        if (items.length === 0) {
            if ('default' in kw) return kw['default'];
            throw new ValueError(name + '() arg is an empty sequence');
        }
        var best = items[0], bestKey = key ? key(best) : best;
        for (var i = 1; i < items.length; i++) {
            var k = key ? key(items[i]) : items[i];
            if (cmp(k, bestKey) * sign > 0) { best = items[i]; bestKey = k; }
        }
        return best;
    };
    G.min = function (...args) { return minmax(args, -1, 'min'); };
    G.max = function (...args) { return minmax(args, 1, 'max'); };
    G.sum = function (it, start) {
        var total = start === undefined ? 0 : start;
        for (var v of toIterable(it)) total = isList(total) && isList(v) ? total.concat(v) : total + v;
        return total;
    };
    G.sorted = function (it, ...rest) { return pySort(toList(it).slice(), rest); };
    G.reversed = function (it) { return toList(it).slice().reverse(); };
    G.enumerate = function (it, start) {
        if (start instanceof KW) start = start.o.start;
        var i = start === undefined ? 0 : start, out = [];
        for (var v of toIterable(it)) out.push([i++, v]);
        return out;
    };
    G.zip = function (...its) {
        var lists = its.map(toList), out = [];
        if (lists.length === 0) return out;
        var n = Math.min(...lists.map(function (l) { return l.length; }));
        for (var i = 0; i < n; i++) out.push(lists.map(function (l) { return l[i]; }));
        return out;
    };
    G.map = function (f, ...its) {
        if (its.length === 1) return toList(its[0]).map(function (v) { return f(v); });
        return G.zip(...its).map(function (args) { return f(...args); });
    };
    G.filter = function (f, it) { return toList(it).filter(f === null ? py.truthy : function (v) { return py.truthy(f(v)); }); };
    G.any = function (it) { for (var v of toIterable(it)) if (py.truthy(v)) return true; return false; };
    G.all = function (it) { for (var v of toIterable(it)) if (!py.truthy(v)) return false; return true; };
    G.chr = function (n) { return String.fromCodePoint(n); };
    G.ord = function (s) {
        if (!isStr(s) || s.length === 0 || s.length > 2) throw new TypeError('ord() expected a character, but string of length ' + (isStr(s) ? s.length : 0) + ' found');
        return s.codePointAt(0);
    };
    G.hex = function (n) { return (n < 0 ? '-0x' : '0x') + Math.abs(Math.trunc(n)).toString(16); };
    G.oct = function (n) { return (n < 0 ? '-0o' : '0o') + Math.abs(Math.trunc(n)).toString(8); };
    G.bin = function (n) { return (n < 0 ? '-0b' : '0b') + Math.abs(Math.trunc(n)).toString(2); };
    G.callable = function (x) { return typeof x === 'function'; };
    G.hasattr = function (o, name) { return o !== null && o !== undefined && (name in Object(o)); };
    G.getattr = function (o, name, fallback) {
        if (o !== null && o !== undefined && name in Object(o)) return o[name];
        if (arguments.length > 2) return fallback;
        throw new AttributeError("'" + typeName(o) + "' object has no attribute '" + name + "'");
    };
    G.setattr = function (o, name, v) { o[name] = v; };
    G.delattr = function (o, name) { delete o[name]; };
    G.id = function (o) { return 0; };
    G.iter = function (x) { return toIterable(x)[Symbol.iterator](); };
    G.next = function (it, fallback) {
        var r = it.next();
        if (!r.done) return r.value;
        if (arguments.length > 1) return fallback;
        throw new StopIteration('');
    };
    G.isinstance = function (x, t) {
        if (isList(t)) { for (var i = 0; i < t.length; i++) if (G.isinstance(x, t[i])) return true; return false; }
        if (t === G.int) return isInt(x) && typeof x !== 'boolean';
        if (t === G.float) return isNum(x);
        if (t === G.str) return isStr(x);
        if (t === G.bool) return typeof x === 'boolean';
        if (t === G.list || t === G.tuple) return isList(x);
        if (t === G.dict) return isDict(x);
        if (t === G.set || t === G.frozenset) return isSet(x);
        if (t === G.object) return x !== null && x !== undefined;
        if (typeof t !== 'function') throw new TypeError('isinstance() arg 2 must be a type or tuple of types');
        return x instanceof t;
    };
    py.isinstance = G.isinstance;
    G.issubclass = function (a, b) {
        if (a === b) return true;
        for (var p = Object.getPrototypeOf(a.prototype); p; p = Object.getPrototypeOf(p)) if (p === b.prototype) return true;
        return false;
    };
    G.type = function (x) {
        if (arguments.length !== 1) throw new TypeError('type() with three arguments is not supported');
        if (x === null || x === undefined) return G.NoneType;
        if (typeof x === 'boolean') return G.bool;
        if (isNum(x)) return Number.isInteger(x) ? G.int : G.float;
        if (isStr(x)) return G.str;
        if (isList(x)) return G.list;
        if (isDict(x)) return G.dict;
        if (isSet(x)) return G.set;
        return x.constructor;
    };
    G.NoneType = function () { return null; };
    G.object = class object {};
    G.int.__name__ = 'int'; G.float.__name__ = 'float'; G.str.__name__ = 'str'; G.bool.__name__ = 'bool';
    G.list.__name__ = 'list'; G.dict.__name__ = 'dict'; G.set.__name__ = 'set'; G.NoneType.__name__ = 'NoneType';

    // ---- modules ----
    var gcd = function (a, b) { a = Math.abs(a); b = Math.abs(b); while (b) { var t = a % b; a = b; b = t; } return a; };
    G.math = {
        pi: Math.PI, e: Math.E, tau: 2 * Math.PI, inf: Infinity, nan: NaN,
        sqrt: function (x) { if (x < 0) throw new ValueError('math domain error'); return Math.sqrt(x); },
        floor: Math.floor, ceil: Math.ceil, trunc: Math.trunc, fabs: Math.abs,
        sin: Math.sin, cos: Math.cos, tan: Math.tan, asin: Math.asin, acos: Math.acos, atan: Math.atan,
        atan2: Math.atan2, sinh: Math.sinh, cosh: Math.cosh, tanh: Math.tanh,
        exp: Math.exp, pow: Math.pow, hypot: Math.hypot,
        log: function (x, base) { if (x <= 0) throw new ValueError('math domain error'); return base === undefined ? Math.log(x) : Math.log(x) / Math.log(base); },
        log2: Math.log2, log10: Math.log10,
        degrees: function (r) { return r * 180 / Math.PI; }, radians: function (d) { return d * Math.PI / 180; },
        isnan: function (x) { return x !== x; }, isinf: function (x) { return x === Infinity || x === -Infinity; },
        isfinite: function (x) { return Number.isFinite(x); },
        isclose: function (a, b, ...rest) { var kw = takeKw(rest); var rel = kwOr(kw, 'rel_tol', 1e-9), abs = kwOr(kw, 'abs_tol', 0); return Math.abs(a - b) <= Math.max(rel * Math.max(Math.abs(a), Math.abs(b)), abs); },
        gcd: gcd,
        lcm: function (a, b) { return a === 0 || b === 0 ? 0 : Math.abs(a * b) / gcd(a, b); },
        factorial: function (n) { if (n < 0 || !isInt(n)) throw new ValueError('factorial() not defined for negative values'); var r = 1; for (var i = 2; i <= n; i++) r *= i; return r; },
        fsum: function (it) { var s = 0; for (var v of toIterable(it)) s += v; return s; },
        prod: function (it) { var p = 1; for (var v of toIterable(it)) p *= v; return p; },
        comb: function (n, k) { if (k < 0 || k > n) return 0; var r = 1; for (var i = 1; i <= k; i++) r = r * (n - k + i) / i; return Math.round(r); },
        copysign: function (a, b) { return (b < 0 || (b === 0 && 1 / b < 0)) ? -Math.abs(a) : Math.abs(a); },
        fmod: function (a, b) { return a % b; },
        modf: function (x) { var i = Math.trunc(x); return [x - i, i]; },
    };
    G.random = {
        random: Math.random,
        seed: function () {},
        randint: function (a, b) { return a + Math.floor(Math.random() * (b - a + 1)); },
        randrange: function (a, b, step) { if (b === undefined) { b = a; a = 0; } step = step === undefined ? 1 : step; return a + step * Math.floor(Math.random() * Math.ceil((b - a) / step)); },
        uniform: function (a, b) { return a + Math.random() * (b - a); },
        choice: function (seq) { var l = toList(seq); if (l.length === 0) throw new IndexError('Cannot choose from an empty sequence'); return l[Math.floor(Math.random() * l.length)]; },
        shuffle: function (l) { for (var i = l.length - 1; i > 0; i--) { var j = Math.floor(Math.random() * (i + 1)); var t = l[i]; l[i] = l[j]; l[j] = t; } },
        sample: function (pop, k) { var l = toList(pop).slice(); G.random.shuffle(l); return l.slice(0, k); },
    };
    G.sys = {
        argv: typeof argv !== 'undefined' ? argv : [],
        maxsize: Number.MAX_SAFE_INTEGER,
        version: '3 (libscript)',
        exit: function (code) { throw new (class SystemExit extends Error {})(code === undefined ? '' : str(code)); },
    };
    G.time = {
        time: function () { return Date.now() / 1000; },
        perf_counter: function () { return Date.now() / 1000; },
        monotonic: function () { return Date.now() / 1000; },
        sleep: function () {},
    };
})();
)PY";

void installPrelude(Runtime& rt)
{
    DynObject* global = rt.globalObject.get();
    if (global->findOwn(rt.intern("__py")))
        return;
    DynObject* py = rt.newObject(rt.objectProto.get());
    Value pyValue = detail::Access::adopt(Type::Object, py);
    rt.method(py, "getitem", &getitem, 2);
    rt.method(py, "setitem", &setitem, 3);
    rt.method(py, "len", &len, 1);
    rt.method(py, "truthy", &truthy, 1);
    rt.method(py, "write", &write, 1);
    rt.method(py, "input", &input, 0);
    rt.method(py, "new", &construct, 1);
    rt.constant(global, "__py", pyValue);
    if (!global->findOwn(rt.intern("globalThis")))
        rt.constant(global, "globalThis", detail::Access::make(Type::Object, global));

    Parser parser(kPrelude, "<python>", /*typescript=*/false);
    NodePtr program = parser.parseProgram();
    runProgram(rt, compileProgram(rt, *program, "<python>"));
}

std::shared_ptr<CompiledUnit> compilePython(ContextImpl& ctx, std::string_view source,
                                            std::string_view name, std::vector<Diagnostic>&)
{
    Runtime& rt = ctx.dyn();
    rt.syncHostClasses();
    installPrelude(rt);
    python::Parser parser(source, std::string(name));
    NodePtr program = parser.parseModule();
    auto unit = std::make_shared<Program>();
    unit->language = Language::Python;
    unit->name = std::string(name);
    unit->code = compileProgram(rt, *program, std::string(name));
    rt.adoptProgram(unit);
    return unit;
}

}  // namespace

const LanguageInfo& pythonLanguage() noexcept
{
    static const char* const extensions[] = {".py", nullptr};
    static const LanguageInfo info = [] {
        LanguageInfo l;
        l.id = Language::Python;
        l.name = "python";
        l.extensions = extensions;
        l.engine = EngineKind::Dynamic;
        l.compile = &compilePython;
        l.run = &runProgramUnit;
        return l;
    }();
    return info;
}

}  // namespace dyn
}  // namespace internal
}  // namespace script
