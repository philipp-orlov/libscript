// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "script/internal/dyn/bigint.hpp"
#include "script/internal/dyn/object.hpp"
#include "builtins_util.hpp"
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

JSValue bigintFromInt64(VM& vm, std::int64_t v, bool isUnsigned)
{
    try {
        dyn::bigint::Env env = bigEnv(vm);
        return fromValue(vm, isUnsigned ? dyn::bigint::fromUint64(env, std::uint64_t(v)) : dyn::bigint::fromInt64(env, v));
    } catch (const Error& e) {
        return vm.throwError(e.code(), e.what());
    }
}

std::uint64_t bigintLow64(JSValue v) { return dyn::bigint::toUint64(bigCell(v)); }

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
    // The empty string and those of one character below 256: made once, kept for the VM's life.
    if (text.size() <= 1 && (text.empty() || text[0] < 256) && !vm.tearingDown) {
        JSValue& cached = vm.smallStrings[text.empty() ? 256 : text[0]];
        if (cached.isUndefined())
            cached = fromValue(vm, vm.ctx.str(text));
        retain(cached);
        return cached;
    }
    return fromValue(vm, vm.ctx.str(text));
}

JSValue makeSubstring(VM& vm, JSValue whole, std::size_t start, std::size_t count)
{
    StringCell* cell = asString(whole);
    const std::size_t length = cell->length;
    if (count <= ContextImpl::kInternedLength || count < ContextImpl::kSliceLength || count * 4 < length)
        return makeString(vm, cell->text().substr(start, count));
    return fromValue(vm, vm.ctx.substring(cell, start, count));
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
    return toPrimitiveHint(vm, v, preferString ? 2 : 1);
}

JSValue toPrimitiveHint(VM& vm, JSValue v, int hint)
{
    if (!isObject(v)) {
        retain(v);
        return v;
    }
    // `v[Symbol.toPrimitive]`, when it has one, decides.
    JSValue exotic = getProperty(vm, v, vm.atoms.symToPrimitive);
    if (exotic.isException())
        return exotic;
    if (isFunction(exotic)) {
        JSValue name = makeString(vm, std::string_view(hint == 0 ? "default" : hint == 1 ? "number" : "string"));
        JSValue result = vm.call(exotic, v, &name, 1);
        release(name);
        release(exotic);
        if (result.isException())
            return result;
        if (isObject(result)) {
            release(result);
            return vm.throwError(ErrorCode::TypeError, "Cannot convert object to primitive value");
        }
        return result;
    }
    const bool present = !exotic.isNullish();
    release(exotic);
    if (present)
        return vm.throwError(ErrorCode::TypeError, "Symbol.toPrimitive is not a function");
    const bool preferString = hint == 2;
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
        return fromValue(vm, vm.ctx.fresh(StringView(p, std::size_t(end - p))));
    }
    if (v.isDouble())
        return fromValue(vm, vm.ctx.fresh(numberToString(v.asDouble())));
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

bool arrayIterationIntact(VM& vm, JSValue v)
{
    if (!isObject(v))
        return false;
    JSObject* a = asObject(v);
    if (a->cls() != ObjClass::Array || a->proto(vm) != vm.protos.array)
        return false;
    if (!vm.arrayRootShape)
        vm.arrayRootShape = vm.rootShapeFor(vm.protos.array, 0)->id;
    if (a->h.shape != vm.arrayRootShape && a->findOwn(vm, vm.atoms.symIterator))
        return false;
    JSValue* it = vm.protos.array->findOwn(vm, vm.atoms.symIterator);
    if (!it || *it != vm.origArrayValues)
        return false;
    JSValue* next = vm.protos.nativeIterator->findOwn(vm, vm.atoms.next);
    return next && *next == vm.origIteratorNext;
}

void wrapPrimitive(VM& vm, JSObject* o, ObjClass cls, JSValue v)
{
    auto* box = new PrimitiveBox();
    retain(v);
    box->value = v;
    Rare& rare = o->rareOf(vm);
    delete rare.ext;
    rare.ext = box;
    o->h.cls = std::uint8_t(cls);
}

bool isConstructor(JSValue v) noexcept
{
    if (!isFunction(v))
        return false;
    const auto* f = static_cast<const JSFunction*>(asObject(v));
    return (f->flags & JSFunction::kNative) ? (f->flags & JSFunction::kConstructor) != 0 : f->code->constructor;
}

JSValue toObject(VM& vm, JSValue v)
{
    if (isObject(v)) {
        retain(v);
        return v;
    }
    if (v.isNullish())
        return vm.throwError(ErrorCode::TypeError, "Cannot convert undefined or null to object");
    JSObject* proto;
    ObjClass cls;
    if (v.isNumber()) {
        proto = vm.protos.number;
        cls = ObjClass::Number;
    } else if (v.isBool()) {
        proto = vm.protos.boolean;
        cls = ObjClass::Boolean;
    } else if (isString(v)) {
        proto = vm.protos.string;
        cls = ObjClass::String;
    } else if (isSymbol(v)) {
        proto = vm.protos.symbol;
        cls = ObjClass::Symbol;
    } else {
        proto = vm.protos.bigint;
        cls = ObjClass::BigInt;
    }
    JSObject* o = vm.newObject(proto);
    Handle keep = Handle::adopt(toJSValue(o));
    wrapPrimitive(vm, o, cls, v);
    return keep.take();
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

JSValue VM::typeofName(unsigned which)
{
    static const char* const names[8] = {"undefined", "object", "boolean", "number", "string", "symbol", "bigint", "function"};
    if (tearingDown)
        return makeString(*this, std::string_view(names[which]));
    JSValue& cached = typeofNames[which];
    if (cached.bits == 0)
        cached = makeString(*this, std::string_view(names[which]));
    retain(cached);
    return cached;
}

JSValue typeOf(VM& vm, JSValue v)
{
    unsigned which;
    if (v.isNumber())
        which = 3;
    else if (v.isUndefined() || v.isHole() || v.isUninit())
        which = 0;
    else if (v.isNull())
        which = 1;
    else if (v.isBool())
        which = 2;
    else if (isString(v))
        which = 4;
    else if (isSymbol(v))
        which = 5;
    else if (isBigInt(v))
        which = 6;
    else if (isFunction(v))
        which = 7;
    else
        which = 1;
    return vm.typeofName(which);
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
        JSValue prim = toPrimitiveHint(vm, obj, 0);
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
    JSValue pa = toPrimitiveHint(vm, a, 0);
    if (pa.isException())
        return pa;
    JSValue pb = toPrimitiveHint(vm, b, 0);
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

namespace {

/// Where a string that more than one may hold starts to grow in a shared buffer: short of it, a copy costs less.
constexpr std::size_t kSharedLength = 256;

/// `s += tail` in a deferred frame, which cannot tell whether one register holds `s`: the characters go into a buffer
/// that only slices show, past the slice that ends where the buffer's written part does (`StringCell::kAppendable`).
/// The register gets a new slice; `s` stays what it was.
bool appendShared(VM& vm, JSValue& slot, StringCell* s, StringView tail, std::size_t length)
{
    if (length > 0xFFFFFFF0u)
        return false;
    StringCell* base = s->whole();
    StringCell* slice;
    if (base != s && (base->bits & StringCell::kAppendable) && s->text().data() == base->mutableChars() &&
        s->length == base->length && base->capacity() >= length) {
        std::memmove(base->mutableChars() + base->length, tail.data(), tail.size() * sizeof(Char));
        base->length = std::uint32_t(length);
        slice = vm.ctx.newCellSized<StringCell>(StringCell::kRopeBytes, base, base->mutableChars(), std::uint32_t(length));
    } else if (length >= kSharedLength) {
        vm.ctx.reserve(length * 2 * sizeof(Char));
        base = vm.ctx.newStringRoom(s->text(), tail, length * 2);
        base->bits |= StringCell::kAppendable;
        slice = vm.ctx.newCellSized<StringCell>(StringCell::kRopeBytes, base, base->mutableChars(), std::uint32_t(length));
        detail::decRef(base);  // the slice holds it
    } else {
        return false;
    }
    slot = JSValue::cell(slice);
    vm.releaseDeferred(slot);  // a register holds it without a count
    return true;
}

}  // namespace

bool appendInPlace(VM& vm, JSValue& slot, JSValue y, bool owned)
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
    if (vm.deferred) {
        if (!owned)
            return appendShared(vm, slot, s, tail, length);
        if (length > 0xFFFFFFF0u)
            return false;
        const bool sole = (s->bits & StringCell::kSole) && s->rc == 0 && !s->interned;
        if (sole && s->capacity() >= length) {
            std::memmove(s->mutableChars() + s->length, tail.data(), tail.size() * sizeof(Char));
            s->length = std::uint32_t(length);
            s->hash_ = 0;
            return true;
        }
        // A copy that is this register's own, where it stays so or is short; what is looked at between one piece and the
        // next grows in a shared buffer.
        if (!sole && length >= kSharedLength)
            return appendShared(vm, slot, s, tail, length);
        if (length < 32)
            return false;
        vm.ctx.reserve(length * 2 * sizeof(Char));
        StringCell* grown = vm.ctx.newStringRoom(s->text(), tail, length * 2);
        grown->bits |= StringCell::kSole;
        slot = JSValue::cell(grown);
        vm.releaseDeferred(slot);  // a register holds it without a count
        return true;
    }
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

double power(double x, double y) noexcept
{
    if (y == 2)
        return x * x;
    if (y != y)
        return std::nan("");
    if (y == 0)
        return 1;
    if ((x == 1 || x == -1) && std::isinf(y))
        return std::nan("");
    return std::pow(x, y);
}

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
        // ToNumeric of the left operand comes before anything is done with the right one.
        if (isSymbol(pa)) {
            release(pa);
            return vm.throwError(ErrorCode::TypeError, "Cannot convert a Symbol value to a number");
        }
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

static bool chainHasProxy(VM& vm, JSObject* o)
{
    for (; o; o = o->proto(vm))
        if (proxyOf(o))
            return true;
    return false;
}

/// OrdinaryHasInstance(b, a).
static int ordinaryHasInstance(VM& vm, JSValue b, JSValue a)
{
    if (!isFunction(b))
        return 0;
    // A bound function asks about what it is bound to.
    if (Rare* r = asObject(b)->rare(); r && isFunction(r->boundTarget))
        return instanceOf(vm, a, r->boundTarget);
    if (!isObject(a)) {
        // A value that is no object has no chain: false, which the cache keeps under shape 0 (valid as long as the
        // function's `prototype` and the `Symbol.hasInstance` it could inherit are as they were).
        if (vm.prototypeAt2(asObject(b)) && isObject(*asObject(b)->slotAt(2)) && !vm.hasInstanceSeen && !vm.proxyAsProto &&
            !chainHasProxy(vm, asObject(b))) {
            JSObject* want = asObject(*asObject(b)->slotAt(2));
            VM::InstanceEntry& e = vm.instanceCache[(std::uint32_t(reinterpret_cast<std::uintptr_t>(want) >> 4)) & (VM::kInstanceEntries - 1)];
            ValidityCell* cell = vm.validityOf(vm.protos.object);
            ++cell->refs;
            if (e.validity)
                vm.releaseValidity(e.validity);
            e.shape = 0;
            e.fnShape = asObject(b)->h.shape;
            e.want = want;
            e.validity = cell;
            e.result = 0;
        }
        return 0;
    }
    if (vm.prototypeAt2(asObject(b)) && isObject(*asObject(b)->slotAt(2))) {
        // `prototype` is a data property in its usual slot: the walk reads shapes only (a proxy anywhere takes the long way).
        JSObject* want = asObject(*asObject(b)->slotAt(2));
        JSObject* const start = asObject(a);
        VM::InstanceEntry& e = vm.instanceCache[(start->h.shape ^ std::uint32_t(reinterpret_cast<std::uintptr_t>(want) >> 4)) & (VM::kInstanceEntries - 1)];
        if (e.want == want && e.shape == start->h.shape && e.fnShape == asObject(b)->h.shape && e.validity && e.validity->valid &&
            !vm.hasInstanceSeen && !vm.proxyAsProto)
            return e.result;
        JSObject* o = start;
        if (!proxyOf(o)) {
            int found = 0;
            for (;;) {
                o = o->proto(vm);
                if (!o)
                    break;
                if (o == want) {
                    found = 1;
                    break;
                }
                if (proxyOf(o))
                    goto longWay;
            }
            // Remember it for the next object of this shape, while the chain above is as it is. Not once a proxy is a
            // prototype or a `Symbol.hasInstance` exists: what `b` inherits could then change without its shape.
            if (JSObject* p = start->proto(vm); p && !vm.hasInstanceSeen && !vm.proxyAsProto && !chainHasProxy(vm, asObject(b))) {
                ValidityCell* cell = vm.validityOf(p);
                ++cell->refs;
                if (e.validity)
                    vm.releaseValidity(e.validity);
                e.shape = start->h.shape;
                e.fnShape = asObject(b)->h.shape;
                e.want = want;
                e.validity = cell;
                e.result = found != 0;
            }
            return found;
        }
    longWay:;
    }
    JSValue proto = getProperty(vm, b, vm.atoms.prototype);
    if (proto.isException())
        return -1;
    if (!isObject(proto)) {
        release(proto);
        vm.throwError(ErrorCode::TypeError, "Function has non-object prototype in instanceof check");
        return -1;
    }
    int found = 0;
    Handle cur = Handle::retained(a);
    for (;;) {
        Handle next;
        if (bi::objGetPrototypeOf(vm, asObject(cur.get()), &next) < 0) {
            found = -1;
            break;
        }
        if (!isObject(next.get()))
            break;
        if (next.get() == proto) {
            found = 1;
            break;
        }
        cur = std::move(next);
    }
    release(proto);
    return found;
}

int instanceOf(VM& vm, JSValue a, JSValue b)
{
    if (!isObject(b)) {
        vm.throwError(ErrorCode::TypeError, "Right-hand side of 'instanceof' is not an object");
        return -1;
    }
    if (!vm.hasInstanceSeen && isFunction(b)) {
        // No object has a `Symbol.hasInstance`, so the lookup finds none unless a proxy's trap makes one up.
        bool proxied = false;
        for (JSObject* o = asObject(b); o && !proxied; o = o->proto(vm))
            proxied = proxyOf(o) != nullptr;
        if (!proxied)
            return ordinaryHasInstance(vm, b, a);
    }
    JSValue handler = getProperty(vm, b, vm.atoms.symHasInstance);
    if (handler.isException())
        return -1;
    if (!handler.isNullish()) {
        if (!isFunction(handler)) {
            release(handler);
            vm.throwError(ErrorCode::TypeError, "Symbol.hasInstance is not a function");
            return -1;
        }
        JSValue r = vm.call(handler, b, &a, 1);
        release(handler);
        if (r.isException())
            return -1;
        const bool result = toBoolean(r);
        release(r);
        return result;
    }
    release(handler);
    if (!isFunction(b)) {
        vm.throwError(ErrorCode::TypeError, "Right-hand side of 'instanceof' is not callable");
        return -1;
    }
    return ordinaryHasInstance(vm, b, a);
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
    if (!has && !vm.exception.isUndefined())
        return -1;
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
