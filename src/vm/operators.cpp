// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "script/internal/dyn/bigint.hpp"
#include "script/internal/dyn/object.hpp"
#include "script/internal/vm/operators.hpp"

#include <cmath>
#include <cstring>

#include "script/internal/dyn/numconv.hpp"
#include "script/internal/vm/convert.hpp"

namespace script {
namespace internal {
namespace vm {

namespace {

dyn::bigint::Env bigEnv(VM& vm)
{
    return dyn::bigint::Env{vm.ctx,
                            [](void*, ErrorCode code, const char* message) { throw Error(code, message); }, &vm,
                            [](void*, std::size_t) {}};
}

dyn::BigIntCell* bigCell(JSValue v) noexcept { return static_cast<dyn::BigIntCell*>(v.asCell()); }

}  // namespace

JSValue bigintBinary(VM& vm, Op op, JSValue a, JSValue b)
{
    using dyn::bigint::BinaryOp;
    BinaryOp which;
    switch (op) {
        case Op::Add: which = BinaryOp::Add; break;
        case Op::Sub: which = BinaryOp::Sub; break;
        case Op::Mul: which = BinaryOp::Mul; break;
        case Op::Div: which = BinaryOp::Div; break;
        case Op::Mod: which = BinaryOp::Mod; break;
        case Op::Exp: which = BinaryOp::Pow; break;
        case Op::BitAnd: which = BinaryOp::And; break;
        case Op::BitOr: which = BinaryOp::Or; break;
        case Op::BitXor: which = BinaryOp::Xor; break;
        case Op::Shl: which = BinaryOp::Shl; break;
        case Op::Shr: which = BinaryOp::Shr; break;
        default: return vm.throwError(ErrorCode::TypeError, "BigInts have no unsigned right shift, use >> instead");
    }
    try {
        dyn::bigint::Env env = bigEnv(vm);
        return fromValue(vm, dyn::bigint::binary(env, which, bigCell(a), bigCell(b)));
    } catch (const Error& e) {
        return vm.throwError(e.code(), e.what());
    }
}

JSValue bigintUnary(VM& vm, int kind, JSValue a)
{
    try {
        dyn::bigint::Env env = bigEnv(vm);
        switch (kind) {
            case 0: return fromValue(vm, dyn::bigint::negate(env, bigCell(a)));
            case 1: return fromValue(vm, dyn::bigint::bitNot(env, bigCell(a)));
            case 2: return fromValue(vm, dyn::bigint::increment(env, bigCell(a), 1));
            default: return fromValue(vm, dyn::bigint::increment(env, bigCell(a), -1));
        }
    } catch (const Error& e) {
        return vm.throwError(e.code(), e.what());
    }
}

JSValue bigintFromText(VM& vm, StringView text, bool* ok)
{
    try {
        dyn::bigint::Env env = bigEnv(vm);
        Value v;
        *ok = dyn::bigint::parseString(env, text, v);
        return *ok ? fromValue(vm, v) : JSValue::undefined();
    } catch (const Error& e) {
        *ok = false;
        return vm.throwError(e.code(), e.what());
    }
}

JSValue bigintFromDouble(VM& vm, double d)
{
    try {
        dyn::bigint::Env env = bigEnv(vm);
        return fromValue(vm, dyn::bigint::fromDouble(env, d));
    } catch (const Error& e) {
        return vm.throwError(e.code(), e.what());
    }
}

JSValue bigintToString(VM& vm, JSValue v, int radix)
{
    return makeString(vm, StringView(dyn::bigint::toString(bigCell(v), radix)));
}

JSValue bigintAsN(VM& vm, std::uint64_t bits, JSValue a, bool isSigned)
{
    try {
        dyn::bigint::Env env = bigEnv(vm);
        return fromValue(vm, dyn::bigint::asIntN(env, bits, bigCell(a), isSigned));
    } catch (const Error& e) {
        return vm.throwError(e.code(), e.what());
    }
}

double bigintToDouble(JSValue v) { return dyn::bigint::toDouble(bigCell(v)); }

bool bigintIsZero(JSValue v) noexcept { return bigCell(v)->digits.empty(); }

/// -1, 0 or 1, or 2 when they cannot be ordered (a NaN, a string that is no integer). `a` is a BigInt.
int bigintCompareWith(VM& vm, JSValue a, JSValue other, bool* ok)
{
    *ok = true;
    if (isBigInt(other))
        return dyn::bigint::compare(bigCell(a), bigCell(other));
    if (isString(other)) {
        bool parsed;
        JSValue b = bigintFromText(vm, asString(other)->text(), &parsed);
        if (!parsed) {
            if (b.isException()) {
                *ok = false;
                vm.clearException();
            }
            return 2;
        }
        const int c = dyn::bigint::compare(bigCell(a), bigCell(b));
        release(b);
        return c;
    }
    double d;
    if (!toNumber(vm, other, &d)) {
        *ok = false;
        return 2;
    }
    return dyn::bigint::compareDouble(bigCell(a), d);
}



JSValue makeString(VM& vm, StringView text)
{
    return fromValue(vm, vm.ctx.str(text));
}

JSValue makeString(VM& vm, std::string_view utf8)
{
    return makeString(vm, fromUtf8(utf8));
}

bool toBoolean(JSValue v) noexcept
{
    if (v.isInt32())
        return v.asInt32() != 0;
    if (v.isBool())
        return v.asBool();
    if (v.isDouble()) {
        const double d = v.asDouble();
        return d == d && d != 0;
    }
    if (v.isNullish() || v.isHole() || v.isUninit())
        return false;
    if (v.isCell()) {
        if (!isVmCell(v.asCell()) && v.asCell()->kind == detail::CellKind::String)
            return static_cast<StringCell*>(v.asCell())->length != 0;
        if (!isVmCell(v.asCell()) && v.asCell()->kind == detail::CellKind::BigInt)
            return !bigintIsZero(v);
        return true;
    }
    return false;
}

JSValue toPrimitive(VM& vm, JSValue v, bool preferString)
{
    if (!isObject(v)) {
        retain(v);
        return v;
    }
    const char* first = preferString ? "toString" : "valueOf";
    const char* second = preferString ? "valueOf" : "toString";
    for (const char* name : {first, second}) {
        Key key = vm.internKey(std::string_view(name));
        JSValue method = getProperty(vm, v, key);
        release(JSValue::cell(key));
        if (method.isException())
            return method;
        if (isFunction(method)) {
            JSValue result = vm.call(method, v, nullptr, 0);
            release(method);
            if (result.isException())
                return result;
            if (!isObject(result))
                return result;
            release(result);
        } else {
            release(method);
        }
    }
    return vm.throwError(ErrorCode::TypeError, "Cannot convert object to primitive value");
}

bool toNumber(VM& vm, JSValue v, double* out)
{
    if (v.isInt32()) {
        *out = v.asInt32();
        return true;
    }
    if (v.isDouble()) {
        *out = v.asDouble();
        return true;
    }
    if (v.isUndefined()) {
        *out = std::nan("");
        return true;
    }
    if (v.isNull()) {
        *out = 0;
        return true;
    }
    if (v.isBool()) {
        *out = v.asBool() ? 1 : 0;
        return true;
    }
    if (isString(v)) {
        *out = dyn::stringToNumber(asString(v)->text());
        return true;
    }
    if (isSymbol(v)) {
        vm.throwError(ErrorCode::TypeError, "Cannot convert a Symbol value to a number");
        return false;
    }
    if (isBigInt(v)) {
        vm.throwError(ErrorCode::TypeError, "Cannot convert a BigInt value to a number");
        return false;
    }
    if (isObject(v)) {
        JSValue prim = toPrimitive(vm, v, false);
        if (prim.isException())
            return false;
        const bool ok = toNumber(vm, prim, out);
        release(prim);
        return ok;
    }
    *out = std::nan("");
    return true;
}

JSValue toNumeric(VM& vm, JSValue v)
{
    if (v.isNumber())
        return v;
    JSValue prim = v;
    if (isObject(v)) {
        prim = toPrimitive(vm, v, false);
        if (prim.isException())
            return prim;
    }
    if (isBigInt(prim)) {
        retain(prim);
        if (prim != v)
            release(prim);
        return prim;
    }
    double d;
    const bool ok = toNumber(vm, prim, &d);
    if (prim != v)
        release(prim);
    if (!ok)
        return JSValue::exception();
    return JSValue::boxNumber(d);
}

JSValue toStringValue(VM& vm, JSValue v)
{
    if (isString(v)) {
        retain(v);
        return v;
    }
    if (v.isInt32()) {
        char16_t buffer[16];
        char16_t* end = buffer + 16;
        char16_t* p = end;
        std::int64_t n = v.asInt32();
        const bool negative = n < 0;
        if (negative)
            n = -n;
        do {
            *--p = char16_t(u'0' + n % 10);
            n /= 10;
        } while (n);
        if (negative)
            *--p = u'-';
        return makeString(vm, StringView(p, std::size_t(end - p)));
    }
    if (v.isDouble())
        return makeString(vm, numberToString(v.asDouble()));
    if (v.isUndefined())
        return makeString(vm, std::string_view("undefined"));
    if (v.isNull())
        return makeString(vm, std::string_view("null"));
    if (v.isBool())
        return makeString(vm, std::string_view(v.asBool() ? "true" : "false"));
    if (isSymbol(v))
        return vm.throwError(ErrorCode::TypeError, "Cannot convert a Symbol value to a string");
    if (isBigInt(v))
        return bigintToString(vm, v, 10);
    if (isObject(v)) {
        JSValue prim = toPrimitive(vm, v, true);
        if (prim.isException())
            return prim;
        JSValue out = toStringValue(vm, prim);
        release(prim);
        return out;
    }
    return makeString(vm, std::string_view("undefined"));
}

JSValue toObject(VM& vm, JSValue v)
{
    if (isObject(v)) {
        retain(v);
        return v;
    }
    if (v.isNullish())
        return vm.throwError(ErrorCode::TypeError, "Cannot convert undefined or null to object");
    // Wrappers for primitives are not made by this core yet: a primitive stands in for its own wrapper, since every use
    // the interpreter has for one is a property read, and that goes through the prototype.
    retain(v);
    return v;
}

std::int32_t toInt32(double d) noexcept
{
    if (d >= -2147483648.0 && d <= 2147483647.0)
        return std::int32_t(d);
    if (!(d == d) || std::isinf(d))
        return 0;
    const double m = std::fmod(std::trunc(d), 4294967296.0);
    return std::int32_t(std::uint32_t(std::int64_t(m < 0 ? m + 4294967296.0 : m)));
}

JSValue typeOf(VM& vm, JSValue v)
{
    const char* name;
    if (v.isNumber())
        name = "number";
    else if (v.isUndefined() || v.isHole() || v.isUninit())
        name = "undefined";
    else if (v.isNull())
        name = "object";
    else if (v.isBool())
        name = "boolean";
    else if (isString(v))
        name = "string";
    else if (isSymbol(v))
        name = "symbol";
    else if (isBigInt(v))
        name = "bigint";
    else if (isFunction(v))
        name = "function";
    else
        name = "object";
    return makeString(vm, std::string_view(name));
}

// --------------------------------------------------------------- equality ---

bool strictEquals(JSValue a, JSValue b) noexcept
{
    if (a.isNumber() && b.isNumber())
        return a.isInt32() && b.isInt32() ? a.bits == b.bits : a.asNumber() == b.asNumber();
    if (a == b)
        return true;
    if (isString(a) && isString(b))
        return stringsEqual(a, b);
    if (isBigInt(a) && isBigInt(b))
        return dyn::bigint::equal(bigCell(a), bigCell(b));
    return false;
}

bool sameValueZero(JSValue a, JSValue b) noexcept
{
    if (a.isNumber() && b.isNumber()) {
        const double x = a.asNumber(), y = b.asNumber();
        return x == y || (x != x && y != y);
    }
    return strictEquals(a, b);
}

bool looseEquals(VM& vm, JSValue a, JSValue b, bool* ok)
{
    *ok = true;
    if (a.isNumber() && b.isNumber())
        return a.asNumber() == b.asNumber();
    if (a.isNullish() && b.isNullish())
        return true;
    if (a.isNullish() || b.isNullish())
        return false;
    const bool aObj = isObject(a), bObj = isObject(b);
    if (aObj && bObj)
        return a == b;
    if (isString(a) && isString(b))
        return stringsEqual(a, b);
    if (a.isBool() && b.isBool())
        return a == b;
    if (aObj != bObj) {
        // An object against a primitive: the object becomes a primitive.
        JSValue obj = aObj ? a : b;
        JSValue prim = toPrimitive(vm, obj, false);
        if (prim.isException()) {
            *ok = false;
            return false;
        }
        const bool r = aObj ? looseEquals(vm, prim, b, ok) : looseEquals(vm, a, prim, ok);
        release(prim);
        return r;
    }
    if (isSymbol(a) || isSymbol(b))
        return a == b;
    if (isBigInt(a) || isBigInt(b)) {
        const JSValue big = isBigInt(a) ? a : b, other = isBigInt(a) ? b : a;
        bool fine;
        const int c = bigintCompareWith(vm, big, other, &fine);
        if (!fine)
            *ok = false;
        return fine && c == 0;
    }
    double x, y;
    if (!toNumber(vm, a, &x) || !toNumber(vm, b, &y)) {
        *ok = false;
        return false;
    }
    return x == y;
}

// ------------------------------------------------------------- arithmetic ---

JSValue add(VM& vm, JSValue a, JSValue b)
{
    if (a.isInt32() && b.isInt32()) {
        std::int32_t r;
        if (!__builtin_add_overflow(a.asInt32(), b.asInt32(), &r))
            return JSValue::int32(r);
        return JSValue::boxDouble(double(a.asInt32()) + double(b.asInt32()));
    }
    if (a.isNumber() && b.isNumber())
        return JSValue::boxNumber(a.asNumber() + b.asNumber());
    if (isString(a) && isString(b))
        return concatStrings(vm, a, b);
    JSValue pa = toPrimitive(vm, a, false);
    if (pa.isException())
        return pa;
    JSValue pb = toPrimitive(vm, b, false);
    if (pb.isException()) {
        release(pa);
        return pb;
    }
    JSValue result;
    if (isString(pa) || isString(pb)) {
        JSValue sa = toStringValue(vm, pa);
        JSValue sb = sa.isException() ? sa : toStringValue(vm, pb);
        if (sa.isException() || sb.isException()) {
            if (!sa.isException())
                release(sa);
            result = JSValue::exception();
        } else {
            result = concatStrings(vm, sa, sb);
            release(sa);
            release(sb);
        }
    } else if (isBigInt(pa) || isBigInt(pb)) {
        if (isBigInt(pa) && isBigInt(pb))
            result = bigintBinary(vm, Op::Add, pa, pb);
        else
            result = vm.throwError(ErrorCode::TypeError, "Cannot mix BigInt and other types, use explicit conversions");
    } else {
        double x, y;
        if (toNumber(vm, pa, &x) && toNumber(vm, pb, &y))
            result = JSValue::boxNumber(x + y);
        else
            result = JSValue::exception();
    }
    release(pa);
    release(pb);
    return result;
}

bool appendInPlace(VM& vm, JSValue& slot, JSValue y)
{
    if (slot == y)
        return false;
    StringCell* s = asString(slot);
    String digits;
    StringView tail;
    if (isString(y)) {
        tail = asString(y)->text();
    } else if (y.isNumber()) {
        digits = numberToString(y.asNumber());
        tail = digits;
    } else {
        return false;
    }
    const std::size_t length = std::size_t(s->length) + tail.size();
    if (length > 0xFFFFFFF0u || (s->bits & StringCell::kOutOfLine))
        return false;
    // Only this register holds the cell, so nothing sees the old text.
    if (!s->interned && s->rc == 1 && s->capacity() >= length) {
        std::memmove(s->mutableChars() + s->length, tail.data(), tail.size() * sizeof(Char));
        s->length = std::uint32_t(length);
        s->hash_ = 0;
        return true;
    }
    if (length >= 32) {
        vm.ctx.reserve(length * 2 * sizeof(Char));
        StringCell* grown = vm.ctx.newStringRoom(s->text(), tail, length * 2);
        const JSValue old = slot;
        slot = JSValue::cell(grown);
        release(old);
        return true;
    }
    return false;
}

namespace {

double power(double x, double y) noexcept
{
    if (y != y)
        return std::nan("");
    if (y == 0)
        return 1;
    if ((x == 1 || x == -1) && std::isinf(y))
        return std::nan("");
    return std::pow(x, y);
}

}  // namespace

JSValue arithmetic(VM& vm, Op op, JSValue a, JSValue b)
{
    if (a.isInt32() && b.isInt32()) {
        const std::int32_t x = a.asInt32(), y = b.asInt32();
        std::int32_t r;
        switch (op) {
            case Op::Sub:
                if (!__builtin_sub_overflow(x, y, &r))
                    return JSValue::int32(r);
                break;
            case Op::Mul:
                if (!__builtin_mul_overflow(x, y, &r) && (r != 0 || (x >= 0 && y >= 0)))
                    return JSValue::int32(r);
                break;
            case Op::Mod:
                if (x >= 0 && y > 0)
                    return JSValue::int32(x % y);
                break;
            case Op::BitAnd: return JSValue::int32(x & y);
            case Op::BitOr: return JSValue::int32(x | y);
            case Op::BitXor: return JSValue::int32(x ^ y);
            case Op::Shl: return JSValue::int32(std::int32_t(std::uint32_t(x) << (y & 31)));
            case Op::Shr: return JSValue::int32(x >> (y & 31));
            case Op::UShr: return JSValue::boxNumber(double(std::uint32_t(x) >> (y & 31)));
            default: break;
        }
    }
    if ((a.isCell() || b.isCell()) && (isBigInt(a) || isBigInt(b) || isObject(a) || isObject(b))) {
        JSValue pa = toPrimitive(vm, a, false);
        if (pa.isException())
            return pa;
        JSValue pb = toPrimitive(vm, b, false);
        if (pb.isException()) {
            release(pa);
            return pb;
        }
        if (isBigInt(pa) || isBigInt(pb)) {
            JSValue r = isBigInt(pa) && isBigInt(pb)
                            ? bigintBinary(vm, op, pa, pb)
                            : vm.throwError(ErrorCode::TypeError, "Cannot mix BigInt and other types, use explicit conversions");
            release(pa);
            release(pb);
            return r;
        }
        JSValue r = arithmetic(vm, op, pa, pb);
        release(pa);
        release(pb);
        return r;
    }
    double x, y;
    if (!toNumber(vm, a, &x) || !toNumber(vm, b, &y))
        return JSValue::exception();
    switch (op) {
        case Op::Sub: return JSValue::boxNumber(x - y);
        case Op::Mul: return JSValue::boxNumber(x * y);
        case Op::Div: return JSValue::boxNumber(x / y);
        case Op::Mod: return JSValue::boxNumber(std::fmod(x, y));
        case Op::Exp: return JSValue::boxNumber(power(x, y));
        case Op::BitAnd: return JSValue::int32(toInt32(x) & toInt32(y));
        case Op::BitOr: return JSValue::int32(toInt32(x) | toInt32(y));
        case Op::BitXor: return JSValue::int32(toInt32(x) ^ toInt32(y));
        case Op::Shl: return JSValue::int32(std::int32_t(std::uint32_t(toInt32(x)) << (toUint32(y) & 31)));
        case Op::Shr: return JSValue::int32(toInt32(x) >> (toUint32(y) & 31));
        case Op::UShr: return JSValue::boxNumber(double(toUint32(x) >> (toUint32(y) & 31)));
        default: return JSValue::exception();
    }
}

int relational(VM& vm, Op op, JSValue a, JSValue b)
{
    if (a.isInt32() && b.isInt32()) {
        const std::int32_t x = a.asInt32(), y = b.asInt32();
        switch (op) {
            case Op::Lt: return x < y;
            case Op::Le: return x <= y;
            case Op::Gt: return x > y;
            default: return x >= y;
        }
    }
    JSValue pa = toPrimitive(vm, a, false);
    if (pa.isException())
        return -1;
    JSValue pb = toPrimitive(vm, b, false);
    if (pb.isException()) {
        release(pa);
        return -1;
    }
    int result;
    if (isString(pa) && isString(pb)) {
        const int c = compareStrings(pa, pb);
        result = op == Op::Lt ? c < 0 : op == Op::Le ? c <= 0 : op == Op::Gt ? c > 0 : c >= 0;
    } else if (isBigInt(pa) || isBigInt(pb)) {
        bool fine;
        int c;
        if (isBigInt(pa)) {
            c = bigintCompareWith(vm, pa, pb, &fine);
        } else {
            c = bigintCompareWith(vm, pb, pa, &fine);
            if (c != 2)
                c = -c;
        }
        if (!fine)
            result = -1;
        else if (c == 2)
            result = 0;
        else
            result = op == Op::Lt ? c < 0 : op == Op::Le ? c <= 0 : op == Op::Gt ? c > 0 : c >= 0;
    } else {
        double x, y;
        if (!toNumber(vm, pa, &x) || !toNumber(vm, pb, &y)) {
            result = -1;
        } else {
            result = op == Op::Lt ? x < y : op == Op::Le ? x <= y : op == Op::Gt ? x > y : x >= y;
        }
    }
    release(pa);
    release(pb);
    return result;
}

int instanceOf(VM& vm, JSValue a, JSValue b)
{
    if (!isFunction(b)) {
        vm.throwError(ErrorCode::TypeError, "Right-hand side of 'instanceof' is not callable");
        return -1;
    }
    if (!isObject(a))
        return 0;
    JSValue proto = getProperty(vm, b, vm.atoms.prototype);
    if (proto.isException())
        return -1;
    if (!isObject(proto)) {
        release(proto);
        vm.throwError(ErrorCode::TypeError, "Function has non-object prototype in instanceof check");
        return -1;
    }
    int found = 0;
    for (JSObject* p = asObject(a)->proto(vm); p; p = p->proto(vm)) {
        if (toJSValue(p) == proto) {
            found = 1;
            break;
        }
    }
    release(proto);
    return found;
}

int inOperator(VM& vm, JSValue a, JSValue b)
{
    if (!isObject(b)) {
        vm.throwError(ErrorCode::TypeError, "Cannot use 'in' operator to search for a key in a non-object");
        return -1;
    }
    if (isSymbol(a) && static_cast<dyn::SymbolCell*>(a.asCell())->isPrivate)
        return asObject(b)->findOwn(vm, a.asCell()) != nullptr;
    JSValue prim = toPrimitive(vm, a, true);
    if (prim.isException())
        return -1;
    Key key = toPropertyKey(vm, prim);
    release(prim);
    if (!key)
        return -1;
    const bool has = hasProperty(vm, b, key);
    release(JSValue::cell(key));
    return has;
}

std::string describe(VM& vm, JSValue v)
{
    if (v.isUndefined())
        return "undefined";
    if (v.isNull())
        return "null";
    if (isObject(v))
        return isFunction(v) ? "function" : "[object Object]";
    JSValue s = toStringValue(vm, v);
    if (s.isException()) {
        vm.clearException();
        return "?";
    }
    std::string out = toUtf8(asString(s)->text());
    release(s);
    return out;
}

}  // namespace vm
}  // namespace internal
}  // namespace script
