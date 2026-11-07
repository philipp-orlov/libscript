// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "script/internal/dyn/typedarray.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "script/internal/dyn/bigint.hpp"
#include "script/internal/dyn/builtins.hpp"
#include "script/internal/dyn/numconv.hpp"
#include "script/internal/dyn/vm.hpp"

namespace script {
namespace internal {
namespace dyn {

/// What every builtin here shares.
struct TypedState
{
    Ref<DynFunction> constructors[int(ElementKind::Count)];
    Ref<DynObject> prototypes[int(ElementKind::Count)];
    Ref<DynFunction> arrayBufferCtor, sharedBufferCtor, abstractCtor;
    Ref<DynObject> arrayBufferProto, sharedBufferProto, dataViewProto, abstractProto;
};

namespace {

constexpr std::size_t kMaxBufferBytes = std::size_t(1) << 33;
constexpr double kMaxSafe = 9007199254740991.0;

struct KindInfo
{
    const char* name;
    std::uint8_t size;
};
const KindInfo kKinds[] = {
    {"Int8Array", 1},    {"Uint8Array", 1},    {"Uint8ClampedArray", 1}, {"Int16Array", 2},    {"Uint16Array", 2},    {"Int32Array", 4},
    {"Uint32Array", 4},  {"Float16Array", 2},  {"Float32Array", 4},      {"Float64Array", 8},  {"BigInt64Array", 8},  {"BigUint64Array", 8},
};

std::size_t sizeOf(ElementKind kind) noexcept { return kKinds[int(kind)].size; }
bool isBigKind(ElementKind kind) noexcept { return kind == ElementKind::BigInt64 || kind == ElementKind::BigUint64; }

Value objectValue(DynObject* o) { return detail::Access::make(Type::Object, o); }

TypedState& stateOf(Runtime& rt) { return *rt.typed; }

double toIndex(Runtime& rt, const Value& v)
{
    if (v.isUndefined())
        return 0;
    const double d = toNumber(rt, v);
    const double whole = std::isnan(d) ? 0 : std::trunc(d);
    if (whole < 0 || whole > kMaxSafe)
        throwError(rt, ErrorCode::RangeError, "not a valid index");
    return whole == 0 ? 0 : whole;
}

double halfToDouble(std::uint16_t h) noexcept
{
    const int sign = h >> 15;
    const int exponent = (h >> 10) & 31;
    const int fraction = h & 1023;
    double r;
    if (exponent == 31)
        r = fraction ? std::nan("") : HUGE_VAL;
    else if (exponent == 0)
        r = std::ldexp(double(fraction), -24);
    else
        r = std::ldexp(double(fraction + 1024), exponent - 25);
    return sign ? -r : r;
}

std::uint16_t doubleToHalf(double d) noexcept
{
    const std::uint16_t sign = std::signbit(d) ? 0x8000 : 0;
    if (std::isnan(d))
        return 0x7e00;
    const double m = std::fabs(d);
    if (std::isinf(m) || m >= 65520.0)
        return sign | 0x7c00;
    if (m < std::ldexp(1.0, -14)) {
        // Subnormal: a whole number of 2^-24.
        const double scaled = std::nearbyint(std::ldexp(m, 24));
        return std::uint16_t(sign | std::uint16_t(scaled));
    }
    int exponent = 0;
    const double f = std::frexp(m, &exponent);  // m = f * 2^exponent, f in [0.5, 1)
    const double scaled = std::nearbyint(std::ldexp(f, 11));  // 1024 to 2048
    std::uint32_t bits = std::uint32_t(scaled);
    if (bits == 2048) {
        bits = 1024;
        ++exponent;
    }
    return std::uint16_t(sign | std::uint16_t((exponent + 14) << 10) | std::uint16_t(bits - 1024));
}

std::uint8_t toClamped(double d) noexcept
{
    if (!(d > 0))
        return 0;
    if (d >= 255)
        return 255;
    return std::uint8_t(std::nearbyint(d));  // ties to even, the default rounding mode
}

template <class T>
T readAs(const std::uint8_t* p) noexcept
{
    T v;
    std::memcpy(&v, p, sizeof(T));
    return v;
}
template <class T>
void writeAs(std::uint8_t* p, T v) noexcept
{
    std::memcpy(p, &v, sizeof(T));
}

Value readElement(Runtime& rt, ElementKind kind, const std::uint8_t* p)
{
    switch (kind) {
        case ElementKind::Int8: return Value(double(readAs<std::int8_t>(p)));
        case ElementKind::Uint8:
        case ElementKind::Uint8Clamped: return Value(double(*p));
        case ElementKind::Int16: return Value(double(readAs<std::int16_t>(p)));
        case ElementKind::Uint16: return Value(double(readAs<std::uint16_t>(p)));
        case ElementKind::Int32: return Value(double(readAs<std::int32_t>(p)));
        case ElementKind::Uint32: return Value(double(readAs<std::uint32_t>(p)));
        case ElementKind::Float16: return Value(halfToDouble(readAs<std::uint16_t>(p)));
        case ElementKind::Float32: return Value(double(readAs<float>(p)));
        case ElementKind::Float64: return Value(readAs<double>(p));
        case ElementKind::BigInt64: return bigint::fromInt64(rt, readAs<std::int64_t>(p));
        case ElementKind::BigUint64: return bigint::fromUint64(rt, readAs<std::uint64_t>(p));
        default: return Value::undefined();
    }
}

/// A value already converted to what this kind holds: a number, or a bigint for the two that take them.
void writeElement(ElementKind kind, std::uint8_t* p, const Value& v) noexcept
{
    switch (kind) {
        case ElementKind::Int8:
        case ElementKind::Uint8: *p = std::uint8_t(toInt32(numberOf(v))); break;
        case ElementKind::Uint8Clamped: *p = toClamped(numberOf(v)); break;
        case ElementKind::Int16:
        case ElementKind::Uint16: writeAs<std::uint16_t>(p, std::uint16_t(toInt32(numberOf(v)))); break;
        case ElementKind::Int32:
        case ElementKind::Uint32: writeAs<std::uint32_t>(p, std::uint32_t(toInt32(numberOf(v)))); break;
        case ElementKind::Float16: writeAs<std::uint16_t>(p, doubleToHalf(numberOf(v))); break;
        case ElementKind::Float32: writeAs<float>(p, float(numberOf(v))); break;
        case ElementKind::Float64: writeAs<double>(p, numberOf(v)); break;
        case ElementKind::BigInt64:
        case ElementKind::BigUint64: writeAs<std::uint64_t>(p, bigint::toUint64(bigint::cellOf(v))); break;
        default: break;
    }
}

Value convertFor(Runtime& rt, ElementKind kind, const Value& v)
{
    if (isBigKind(kind))
        return toBigInt(rt, v);
    return v.isDouble() ? v : Value(toNumber(rt, v));
}

DynArrayBuffer* newBuffer(Runtime& rt, double bytes, DynObject* proto, bool shared, bool resizable, double maxBytes)
{
    const double room = resizable ? maxBytes : bytes;
    if (room > double(kMaxBufferBytes))
        throwError(rt, ErrorCode::RangeError, "an ArrayBuffer this large cannot be allocated");
    rt.ctx.reserve(std::size_t(bytes));
    auto* buffer = rt.newCell<DynArrayBuffer>();
    Value keep = detail::Access::adopt(Type::Object, buffer);
    TypedState& state = stateOf(rt);
    buffer->cls = shared ? ObjClass::SharedArrayBuffer : ObjClass::ArrayBuffer;
    buffer->setProto(proto ? proto : shared ? state.sharedBufferProto.get() : state.arrayBufferProto.get());
    // Room for all it may grow to now, which the system hands out as it is touched, so that the data never moves.
    buffer->data = static_cast<std::uint8_t*>(std::calloc(std::max<std::size_t>(std::size_t(room), 1), 1));
    if (!buffer->data)
        throwError(rt, ErrorCode::RangeError, "an ArrayBuffer this large cannot be allocated");
    buffer->byteLength = std::size_t(bytes);
    buffer->maxByteLength = std::size_t(room);
    buffer->resizable = resizable;
    buffer->shared = shared;
    rt.ctx.charge(buffer->byteLength);
    detail::incRef(buffer);
    return buffer;
}

DynArrayBuffer* thisBuffer(CallInfo& ci, bool shared, const char* what)
{
    DynObject* o = asObject(ci.thisValue());
    if (!o || o->cls != (shared ? ObjClass::SharedArrayBuffer : ObjClass::ArrayBuffer))
        ci.fail(ErrorCode::TypeError, std::string(what) + " called on something of another kind");
    return static_cast<DynArrayBuffer*>(o);
}

DynTypedArray* asTypedArray(const Value& v) noexcept
{
    DynObject* o = asObject(v);
    return o && o->cls == ObjClass::TypedArray ? static_cast<DynTypedArray*>(o) : nullptr;
}

/// ValidateTypedArray: `this` as a view that is usable right now.
DynTypedArray* validate(Runtime& rt, const Value& v)
{
    DynTypedArray* ta = asTypedArray(v);
    if (!ta)
        throwError(rt, ErrorCode::TypeError, "not a typed array");
    if (ta->outOfBounds || ta->buffer->detached)
        throwError(rt, ErrorCode::TypeError, "the typed array's buffer is detached or too short for it");
    return ta;
}

DynTypedArray* newView(Runtime& rt, ElementKind kind, DynArrayBuffer* buffer, std::size_t offset, std::size_t length, bool tracks, DynObject* proto)
{
    auto* ta = rt.newCell<DynTypedArray>();
    ta->cls = ObjClass::TypedArray;
    ta->setProto(proto ? proto : stateOf(rt).prototypes[int(kind)].get());
    ta->element = kind;
    ta->buffer.reset(buffer);
    ta->byteOffset = offset;
    ta->fixedLength = length;
    ta->tracksLength = tracks;
    buffer->views.push_back(ta);
    ta->refresh();
    return ta;
}

/// A new array of this kind and length over a new buffer.
Value allocateArray(Runtime& rt, ElementKind kind, double length, DynObject* proto = nullptr)
{
    const double bytes = length * double(sizeOf(kind));
    if (bytes > double(kMaxBufferBytes))
        throwError(rt, ErrorCode::RangeError, "a typed array this long cannot be allocated");
    DynArrayBuffer* buffer = newBuffer(rt, bytes, nullptr, false, false, 0);
    Value keepBuffer = detail::Access::adopt(Type::Object, buffer);
    return detail::Access::adopt(Type::Object, newView(rt, kind, buffer, 0, std::size_t(length), false, proto));
}

Value makeFromValues(Runtime& rt, ElementKind kind, const Vec<Value>& values)
{
    Value out = allocateArray(rt, kind, double(values.size()));
    DynTypedArray* ta = asTypedArray(out);
    for (std::size_t i = 0; i < values.size(); ++i)
        typedArraySet(rt, ta, double(i), values[i]);
    return out;
}

/// TypedArrayCreateFromCtor, checking what came back is a usable view, long enough.
Value createFromCtor(Runtime& rt, const Value& ctor, const std::vector<Value>& args)
{
    Value made = constructValue(rt, ctor, args);
    DynTypedArray* ta = validate(rt, made);
    if (args.size() == 1 && args[0].isNumber() && double(ta->length) < numberOf(args[0]))
        throwError(rt, ErrorCode::TypeError, "the constructor returned a typed array too short");
    return made;
}

/// TypedArraySpeciesCreate.
Value speciesCreate(Runtime& rt, DynTypedArray* exemplar, const std::vector<Value>& args)
{
    TypedState& state = stateOf(rt);
    const Value fallback = objectValue(state.constructors[int(exemplar->element)].get());
    const Value ctor = speciesConstructor(rt, objectValue(exemplar), fallback);
    Value made = createFromCtor(rt, ctor, args);
    if (isBigKind(asTypedArray(made)->element) != isBigKind(exemplar->element))
        throwError(rt, ErrorCode::TypeError, "a BigInt array and a number array are not interchangeable");
    return made;
}

void copyBetween(Runtime& rt, DynTypedArray* to, std::size_t toIndex, DynTypedArray* from, std::size_t fromIndex, std::size_t count)
{
    if (to->element == from->element) {
        const std::size_t size = sizeOf(to->element);
        count = std::min({count, to->length > toIndex ? to->length - toIndex : 0, from->length > fromIndex ? from->length - fromIndex : 0});
        if (count)
            std::memmove(to->elements + toIndex * size, from->elements + fromIndex * size, count * size);
        return;
    }
    // They may share a buffer: read everything first.
    Vec<Value> values;
    values.reserve(count);
    for (std::size_t i = 0; i < count; ++i)
        values.push_back(fromIndex + i < from->length ? typedArrayGet(rt, from, fromIndex + i) : Value::undefined());
    for (std::size_t i = 0; i < count; ++i) {
        if (values[i].isUndefined())
            continue;
        const Value converted = convertFor(rt, to->element, values[i]);
        if (toIndex + i < to->length)
            writeElement(to->element, to->elements + (toIndex + i) * sizeOf(to->element), converted);
    }
}

/// `new X(...)` of any of the concrete kinds.
Value constructTyped(CallInfo& ci, ElementKind kind)
{
    Runtime& rt = rtOf(ci);
    if (!ci.isConstructorCall())
        ci.fail(ErrorCode::TypeError, std::string(kKinds[int(kind)].name) + " must be called with 'new'");
    const Value first = ci.arg(0);
    DynObject* object = asObject(first);
    if (!object)
        return allocateArray(rt, kind, toIndex(rt, first));

    if (object->cls == ObjClass::ArrayBuffer || object->cls == ObjClass::SharedArrayBuffer) {
        auto* buffer = static_cast<DynArrayBuffer*>(object);
        const std::size_t size = sizeOf(kind);
        const double offset = toIndex(rt, ci.arg(1));
        if (std::fmod(offset, double(size)) != 0)
            ci.fail(ErrorCode::RangeError, "the offset is not a multiple of the element size");
        double newLength = 0;
        const bool hasLength = !ci.arg(2).isUndefined();
        if (hasLength)
            newLength = toIndex(rt, ci.arg(2));
        if (buffer->detached)
            ci.fail(ErrorCode::TypeError, "the buffer is detached");
        const double bufferBytes = double(buffer->byteLength);
        if (!hasLength && buffer->resizable) {
            if (offset > bufferBytes)
                ci.fail(ErrorCode::RangeError, "the offset is past the end of the buffer");
            return detail::Access::adopt(Type::Object, newView(rt, kind, buffer, std::size_t(offset), 0, true, nullptr));
        }
        double bytes;
        if (!hasLength) {
            if (std::fmod(bufferBytes, double(size)) != 0)
                ci.fail(ErrorCode::RangeError, "the buffer is not a multiple of the element size");
            bytes = bufferBytes - offset;
            if (bytes < 0)
                ci.fail(ErrorCode::RangeError, "the offset is past the end of the buffer");
        } else {
            bytes = newLength * double(size);
            if (offset + bytes > bufferBytes)
                ci.fail(ErrorCode::RangeError, "the view does not fit in the buffer");
        }
        return detail::Access::adopt(Type::Object, newView(rt, kind, buffer, std::size_t(offset), std::size_t(bytes) / size, false, nullptr));
    }

    if (DynTypedArray* source = asTypedArray(first)) {
        if (source->outOfBounds || source->buffer->detached)
            ci.fail(ErrorCode::TypeError, "the source's buffer is detached");
        if (isBigKind(source->element) != isBigKind(kind))
            ci.fail(ErrorCode::TypeError, "a BigInt array and a number array cannot be converted");
        Value out = allocateArray(rt, kind, double(source->length));
        copyBetween(rt, asTypedArray(out), 0, source, 0, source->length);
        return out;
    }

    const Value usingIterator = getProperty(rt, first, rt.sym.iterator.get());
    if (!usingIterator.isNullish()) {
        if (!asFunction(usingIterator))
            ci.fail(ErrorCode::TypeError, "Symbol.iterator is not a function");
        const Vec<Value> values = iterableToVector(rt, first);
        return makeFromValues(rt, kind, values);
    }
    const double length = lengthOfLike(rt, first);
    Value out = allocateArray(rt, kind, length);
    DynTypedArray* ta = asTypedArray(out);
    for (double i = 0; i < length; ++i) {
        checkLongLoop(rt, std::size_t(i), 0);
        typedArraySet(rt, ta, i, getProperty(rt, first, rt.intern(numberToString(i))));
    }
    return out;
}

/// Relative index, as `slice` and friends take them.
double relative(Runtime& rt, const Value& v, double length, double missing)
{
    if (v.isUndefined())
        return missing;
    const double d = toNumber(rt, v);
    const double whole = std::isnan(d) ? 0 : std::trunc(d) + 0.0;  // -0 is 0
    if (whole < 0)
        return std::max(length + whole, 0.0);
    return std::min(whole, length);
}

void requireCallable(CallInfo& ci, const Value& f)
{
    if (!asFunction(f))
        ci.fail(ErrorCode::TypeError, "the callback is not a function");
}

bool sameValueZeroNumber(const Value& a, const Value& b) { return sameValueZero(a, b); }

/// The order `sort` puts numbers in with no comparator.
bool numberLess(double a, double b) noexcept
{
    if (std::isnan(a))
        return false;
    if (std::isnan(b))
        return true;
    if (a < b)
        return true;
    if (a > b)
        return false;
    return a == 0 && b == 0 && std::signbit(a) && !std::signbit(b);
}

Vec<Value> sortedValues(Runtime& rt, DynTypedArray* ta, const Value& comparator)
{
    Vec<Value> values;
    values.reserve(ta->length);
    for (std::size_t i = 0; i < ta->length; ++i)
        values.push_back(typedArrayGet(rt, ta, i));
    if (!comparator.isUndefined()) {
        std::stable_sort(values.begin(), values.end(), [&](const Value& a, const Value& b) {
            const double order = toNumber(rt, callValue(rt, comparator, Value::undefined(), {a, b}));
            return order < 0;
        });
    } else if (isBigKind(ta->element)) {
        std::stable_sort(values.begin(), values.end(), [](const Value& a, const Value& b) { return bigint::compare(bigint::cellOf(a), bigint::cellOf(b)) < 0; });
    } else {
        std::stable_sort(values.begin(), values.end(), [](const Value& a, const Value& b) { return numberLess(numberOf(a), numberOf(b)); });
    }
    return values;
}

void installTypedArrayPrototype(Runtime& rt, DynObject* proto)
{
    const auto self = [](CallInfo& ci) { return validate(rtOf(ci), ci.thisValue()); };

    rt.getter(proto, "buffer", [](CallInfo& ci) -> Value {
        DynTypedArray* ta = asTypedArray(ci.thisValue());
        if (!ta)
            ci.fail(ErrorCode::TypeError, "not a typed array");
        return objectValue(ta->buffer.get());
    });
    rt.getter(proto, "byteLength", [](CallInfo& ci) -> Value {
        DynTypedArray* ta = asTypedArray(ci.thisValue());
        if (!ta)
            ci.fail(ErrorCode::TypeError, "not a typed array");
        return Value(double(ta->length * sizeOf(ta->element)));
    });
    rt.getter(proto, "byteOffset", [](CallInfo& ci) -> Value {
        DynTypedArray* ta = asTypedArray(ci.thisValue());
        if (!ta)
            ci.fail(ErrorCode::TypeError, "not a typed array");
        return Value(ta->outOfBounds || ta->buffer->detached ? 0.0 : double(ta->byteOffset));
    });
    rt.getter(proto, "length", [](CallInfo& ci) -> Value {
        DynTypedArray* ta = asTypedArray(ci.thisValue());
        if (!ta)
            ci.fail(ErrorCode::TypeError, "not a typed array");
        return Value(double(ta->length));
    });
    proto->putOwn(rt.sym.toStringTag.get(), rt.makeFunction(ascii("get [Symbol.toStringTag]"), [](CallInfo& ci) -> Value {
        DynTypedArray* ta = asTypedArray(ci.thisValue());
        if (!ta)
            return Value::undefined();
        return rtOf(ci).str(kKinds[int(ta->element)].name);
    }, 0), PropConfigurable | PropAccessor);

    rt.method(proto, "at", [self](CallInfo& ci) -> Value {
        Runtime& rt2 = rtOf(ci);
        DynTypedArray* ta = self(ci);
        const double length = double(ta->length);
        const double d = toNumber(rt2, ci.arg(0));
        const double whole = std::isnan(d) ? 0 : std::trunc(d);
        const double k = whole >= 0 ? whole : length + whole;
        if (k < 0 || k >= double(ta->length))
            return Value::undefined();
        return typedArrayGet(rt2, ta, std::size_t(k));
    }, 1);

    rt.method(proto, "copyWithin", [self](CallInfo& ci) -> Value {
        Runtime& rt2 = rtOf(ci);
        DynTypedArray* ta = self(ci);
        const double length = double(ta->length);
        const double to = relative(rt2, ci.arg(0), length, 0);
        const double from = relative(rt2, ci.arg(1), length, 0);
        const double end = relative(rt2, ci.arg(2), length, length);
        double count = std::min(end - from, length - to);
        if (count > 0) {
            validate(rt2, ci.thisValue());
            const double now = double(ta->length);
            count = std::min({count, now - from, now - to});
            if (count > 0) {
                const std::size_t size = sizeOf(ta->element);
                std::memmove(ta->elements + std::size_t(to) * size, ta->elements + std::size_t(from) * size, std::size_t(count) * size);
            }
        }
        return ci.thisValue();
    }, 2);

    const auto iterate = [](int mode) {
        return [mode](CallInfo& ci) -> Value {
            Runtime& rt2 = rtOf(ci);
            validate(rt2, ci.thisValue());
            return makeArrayIterator(rt2, ci.thisValue(), mode);
        };
    };
    rt.method(proto, "keys", iterate(1), 0);
    rt.method(proto, "entries", iterate(2), 0);
    rt.method(proto, "values", iterate(0), 0);
    proto->putOwn(rt.sym.iterator.get(), proto->findOwn(rt.intern("values"))->value, PropWritable | PropConfigurable);

    rt.method(proto, "fill", [self](CallInfo& ci) -> Value {
        Runtime& rt2 = rtOf(ci);
        DynTypedArray* ta = self(ci);
        const double length = double(ta->length);
        const Value value = convertFor(rt2, ta->element, ci.arg(0));
        const double start = relative(rt2, ci.arg(1), length, 0);
        double end = relative(rt2, ci.arg(2), length, length);
        validate(rt2, ci.thisValue());
        end = std::min(end, double(ta->length));
        const std::size_t size = sizeOf(ta->element);
        for (double i = start; i < end; ++i)
            writeElement(ta->element, ta->elements + std::size_t(i) * size, value);
        return ci.thisValue();
    }, 1);

    /// Calls `fn(value, index, this)` over the elements as they were counted at the start: one shrunk meanwhile gives undefined.
    const auto loopCalls = [self](CallInfo& ci, auto&& each) {
        Runtime& rt2 = rtOf(ci);
        DynTypedArray* ta = self(ci);
        requireCallable(ci, ci.arg(0));
        const std::size_t length = ta->length;
        for (std::size_t i = 0; i < length; ++i) {
            const Value item = i < ta->length ? typedArrayGet(rt2, ta, i) : Value::undefined();
            const Value answer = callValue(rt2, ci.arg(0), ci.arg(1), {item, Value(double(i)), ci.thisValue()});
            if (each(i, item, answer))
                break;
        }
    };

    rt.method(proto, "every", [loopCalls](CallInfo& ci) -> Value {
        bool all = true;
        loopCalls(ci, [&](std::size_t, const Value&, const Value& answer) { all = toBoolean(answer); return !all; });
        return Value(all);
    }, 1);
    rt.method(proto, "some", [loopCalls](CallInfo& ci) -> Value {
        bool any = false;
        loopCalls(ci, [&](std::size_t, const Value&, const Value& answer) { any = toBoolean(answer); return any; });
        return Value(any);
    }, 1);
    rt.method(proto, "forEach", [loopCalls](CallInfo& ci) -> Value {
        loopCalls(ci, [](std::size_t, const Value&, const Value&) { return false; });
        return Value::undefined();
    }, 1);
    rt.method(proto, "find", [loopCalls](CallInfo& ci) -> Value {
        Value found = Value::undefined();
        loopCalls(ci, [&](std::size_t, const Value& item, const Value& answer) {
            if (!toBoolean(answer))
                return false;
            found = item;
            return true;
        });
        return found;
    }, 1);
    rt.method(proto, "findIndex", [loopCalls](CallInfo& ci) -> Value {
        double found = -1;
        loopCalls(ci, [&](std::size_t i, const Value&, const Value& answer) {
            if (!toBoolean(answer))
                return false;
            found = double(i);
            return true;
        });
        return Value(found);
    }, 1);
    const auto findBack = [self](CallInfo& ci, bool wantIndex) -> Value {
        Runtime& rt2 = rtOf(ci);
        DynTypedArray* ta = self(ci);
        requireCallable(ci, ci.arg(0));
        for (std::size_t i = ta->length; i-- > 0;) {
            const Value item = i < ta->length ? typedArrayGet(rt2, ta, i) : Value::undefined();
            if (toBoolean(callValue(rt2, ci.arg(0), ci.arg(1), {item, Value(double(i)), ci.thisValue()})))
                return wantIndex ? Value(double(i)) : item;
        }
        return wantIndex ? Value(-1.0) : Value::undefined();
    };
    rt.method(proto, "findLast", [findBack](CallInfo& ci) -> Value { return findBack(ci, false); }, 1);
    rt.method(proto, "findLastIndex", [findBack](CallInfo& ci) -> Value { return findBack(ci, true); }, 1);

    rt.method(proto, "filter", [loopCalls](CallInfo& ci) -> Value {
        Runtime& rt2 = rtOf(ci);
        Vec<Value> kept;
        loopCalls(ci, [&](std::size_t, const Value& item, const Value& answer) {
            if (toBoolean(answer))
                kept.push_back(item);
            return false;
        });
        DynTypedArray* ta = asTypedArray(ci.thisValue());
        Value out = speciesCreate(rt2, ta, {Value(double(kept.size()))});
        DynTypedArray* made = asTypedArray(out);
        for (std::size_t i = 0; i < kept.size(); ++i)
            typedArraySet(rt2, made, double(i), kept[i]);
        return out;
    }, 1);
    rt.method(proto, "map", [self](CallInfo& ci) -> Value {
        Runtime& rt2 = rtOf(ci);
        DynTypedArray* ta = self(ci);
        requireCallable(ci, ci.arg(0));
        const std::size_t length = ta->length;
        Value out = speciesCreate(rt2, ta, {Value(double(length))});
        DynTypedArray* made = asTypedArray(out);
        for (std::size_t i = 0; i < length; ++i) {
            const Value item = i < ta->length ? typedArrayGet(rt2, ta, i) : Value::undefined();
            const Value mapped = callValue(rt2, ci.arg(0), ci.arg(1), {item, Value(double(i)), ci.thisValue()});
            typedArraySet(rt2, made, double(i), mapped);
        }
        return out;
    }, 1);

    const auto reduce = [self](CallInfo& ci, bool right) -> Value {
        Runtime& rt2 = rtOf(ci);
        DynTypedArray* ta = self(ci);
        requireCallable(ci, ci.arg(0));
        const std::size_t length = ta->length;
        std::size_t k = 0;
        Value acc;
        if (ci.size() >= 2) {
            acc = ci.arg(1);
        } else {
            if (length == 0)
                ci.fail(ErrorCode::TypeError, "reduce of an empty array with no initial value");
            acc = typedArrayGet(rt2, ta, right ? length - 1 : 0);
            k = 1;
        }
        for (; k < length; ++k) {
            const std::size_t at = right ? length - 1 - k : k;
            const Value item = at < ta->length ? typedArrayGet(rt2, ta, at) : Value::undefined();
            acc = callValue(rt2, ci.arg(0), Value::undefined(), {acc, item, Value(double(at)), ci.thisValue()});
        }
        return acc;
    };
    rt.method(proto, "reduce", [reduce](CallInfo& ci) -> Value { return reduce(ci, false); }, 1);
    rt.method(proto, "reduceRight", [reduce](CallInfo& ci) -> Value { return reduce(ci, true); }, 1);

    rt.method(proto, "includes", [self](CallInfo& ci) -> Value {
        Runtime& rt2 = rtOf(ci);
        DynTypedArray* ta = self(ci);
        const double length = double(ta->length);
        if (length == 0)
            return Value(false);
        double k = relative(rt2, ci.arg(1), length, 0);
        for (; k < length; ++k) {
            const Value item = k < double(ta->length) ? typedArrayGet(rt2, ta, std::size_t(k)) : Value::undefined();
            if (sameValueZeroNumber(item, ci.arg(0)))
                return Value(true);
        }
        return Value(false);
    }, 1);
    rt.method(proto, "indexOf", [self](CallInfo& ci) -> Value {
        Runtime& rt2 = rtOf(ci);
        DynTypedArray* ta = self(ci);
        const double length = double(ta->length);
        if (length == 0)
            return Value(-1.0);
        double k = relative(rt2, ci.arg(1), length, 0);
        const double end = std::min(length, double(ta->length));
        for (; k < end; ++k) {
            if (strictEquals(typedArrayGet(rt2, ta, std::size_t(k)), ci.arg(0)))
                return Value(k);
        }
        return Value(-1.0);
    }, 1);
    rt.method(proto, "lastIndexOf", [self](CallInfo& ci) -> Value {
        Runtime& rt2 = rtOf(ci);
        DynTypedArray* ta = self(ci);
        const double length = double(ta->length);
        if (length == 0)
            return Value(-1.0);
        double k = length - 1;
        if (ci.size() > 1) {
            const double d = toNumber(rt2, ci.arg(1));
            const double whole = std::isnan(d) ? 0 : std::trunc(d);
            k = whole >= 0 ? std::min(whole, length - 1) : length + whole;
        }
        for (; k >= 0; --k) {
            if (k < double(ta->length) && strictEquals(typedArrayGet(rt2, ta, std::size_t(k)), ci.arg(0)))
                return Value(k);
        }
        return Value(-1.0);
    }, 1);

    rt.method(proto, "join", [self](CallInfo& ci) -> Value {
        Runtime& rt2 = rtOf(ci);
        DynTypedArray* ta = self(ci);
        const std::size_t length = ta->length;
        const String separator = ci.arg(0).isUndefined() ? ascii(",") : toStringValue(rt2, ci.arg(0));
        String out;
        for (std::size_t i = 0; i < length; ++i) {
            checkLongLoop(rt2, i, out.size() * sizeof(Char));
            if (i)
                out += separator;
            if (i < ta->length)
                out += toStringValue(rt2, typedArrayGet(rt2, ta, i));
        }
        return rt2.str(out);
    }, 1);
    rt.method(proto, "toLocaleString", [self](CallInfo& ci) -> Value {
        Runtime& rt2 = rtOf(ci);
        DynTypedArray* ta = self(ci);
        const std::size_t length = ta->length;
        String out;
        for (std::size_t i = 0; i < length; ++i) {
            if (i)
                out.push_back(u',');
            const Value item = i < ta->length ? typedArrayGet(rt2, ta, i) : Value::undefined();
            if (!item.isNullish())
                out += toStringValue(rt2, invokeMethod(rt2, item, rt2.intern("toLocaleString"), {}));
        }
        return rt2.str(out);
    }, 0);

    rt.method(proto, "reverse", [self](CallInfo& ci) -> Value {
        Runtime& rt2 = rtOf(ci);
        DynTypedArray* ta = self(ci);
        (void)rt2;
        const std::size_t size = sizeOf(ta->element);
        std::uint8_t swap[8];
        for (std::size_t lower = 0, upper = ta->length; lower + 1 < upper; ++lower) {
            --upper;
            std::uint8_t* a = ta->elements + lower * size;
            std::uint8_t* b = ta->elements + upper * size;
            std::memcpy(swap, a, size);
            std::memcpy(a, b, size);
            std::memcpy(b, swap, size);
            if (lower + 1 >= upper)
                break;
        }
        return ci.thisValue();
    }, 0);
    rt.method(proto, "toReversed", [self](CallInfo& ci) -> Value {
        Runtime& rt2 = rtOf(ci);
        DynTypedArray* ta = self(ci);
        const std::size_t length = ta->length;
        Value out = allocateArray(rt2, ta->element, double(length));
        DynTypedArray* made = asTypedArray(out);
        const std::size_t size = sizeOf(ta->element);
        for (std::size_t i = 0; i < length; ++i)
            std::memcpy(made->elements + i * size, ta->elements + (length - 1 - i) * size, size);
        return out;
    }, 0);

    rt.method(proto, "set", [](CallInfo& ci) -> Value {
        Runtime& rt2 = rtOf(ci);
        DynTypedArray* ta = asTypedArray(ci.thisValue());
        if (!ta)
            ci.fail(ErrorCode::TypeError, "not a typed array");
        const double raw = toNumber(rt2, ci.arg(1));
        const double offset = std::isnan(raw) ? 0 : std::trunc(raw);
        if (offset < 0)
            ci.fail(ErrorCode::RangeError, "the offset is negative");
        if (ta->outOfBounds || ta->buffer->detached)
            ci.fail(ErrorCode::TypeError, "the typed array's buffer is detached");
        const double targetLength = double(ta->length);
        if (DynTypedArray* from = asTypedArray(ci.arg(0))) {
            if (from->outOfBounds || from->buffer->detached)
                ci.fail(ErrorCode::TypeError, "the source's buffer is detached");
            if (isBigKind(from->element) != isBigKind(ta->element))
                ci.fail(ErrorCode::TypeError, "a BigInt array and a number array cannot be mixed");
            if (double(from->length) + offset > targetLength)
                ci.fail(ErrorCode::RangeError, "the source does not fit");
            copyBetween(rt2, ta, std::size_t(offset), from, 0, from->length);
            return Value::undefined();
        }
        const Value source = toObject(rt2, ci.arg(0));
        const double length = lengthOfLike(rt2, source);
        if (length + offset > targetLength)
            ci.fail(ErrorCode::RangeError, "the source does not fit");
        for (double i = 0; i < length; ++i) {
            checkLongLoop(rt2, std::size_t(i), 0);
            const Value item = getProperty(rt2, source, rt2.intern(numberToString(i)));
            typedArraySet(rt2, ta, offset + i, item);
        }
        return Value::undefined();
    }, 1);

    rt.method(proto, "slice", [self](CallInfo& ci) -> Value {
        Runtime& rt2 = rtOf(ci);
        DynTypedArray* ta = self(ci);
        const double length = double(ta->length);
        const double start = relative(rt2, ci.arg(0), length, 0);
        const double end = relative(rt2, ci.arg(1), length, length);
        const double count = std::max(end - start, 0.0);
        Value out = speciesCreate(rt2, ta, {Value(count)});
        if (count == 0)
            return out;
        DynTypedArray* made = asTypedArray(out);
        validate(rt2, ci.thisValue());
        const double stop = std::min(end, double(ta->length));
        if (made->element == ta->element) {
            const std::size_t size = sizeOf(ta->element);
            std::uint8_t* to = made->elements;
            const std::uint8_t* from = ta->elements + std::size_t(start) * size;
            const double bytes = std::min((stop - start) * double(size), double(made->length * size));
            // Byte by byte, as the language has it, when the two overlap.
            for (std::size_t i = 0; bytes > 0 && i < std::size_t(bytes); ++i)
                to[i] = from[i];
            return out;
        }
        double n = 0;
        for (double k = start; k < stop; ++k, ++n)
            typedArraySet(rt2, made, n, typedArrayGet(rt2, ta, std::size_t(k)));
        return out;
    }, 2);

    rt.method(proto, "sort", [](CallInfo& ci) -> Value {
        Runtime& rt2 = rtOf(ci);
        if (!ci.arg(0).isUndefined() && !asFunction(ci.arg(0)))
            ci.fail(ErrorCode::TypeError, "the comparator is not a function");
        DynTypedArray* ta = validate(rt2, ci.thisValue());
        const Vec<Value> values = sortedValues(rt2, ta, ci.arg(0));
        const std::size_t size = sizeOf(ta->element);
        for (std::size_t i = 0; i < values.size() && i < ta->length; ++i)
            writeElement(ta->element, ta->elements + i * size, values[i]);
        return ci.thisValue();
    }, 1);
    rt.method(proto, "toSorted", [](CallInfo& ci) -> Value {
        Runtime& rt2 = rtOf(ci);
        if (!ci.arg(0).isUndefined() && !asFunction(ci.arg(0)))
            ci.fail(ErrorCode::TypeError, "the comparator is not a function");
        DynTypedArray* ta = validate(rt2, ci.thisValue());
        Value out = allocateArray(rt2, ta->element, double(ta->length));
        DynTypedArray* made = asTypedArray(out);
        const Vec<Value> values = sortedValues(rt2, ta, ci.arg(0));
        for (std::size_t i = 0; i < values.size(); ++i)
            writeElement(made->element, made->elements + i * sizeOf(made->element), values[i]);
        return out;
    }, 1);

    rt.method(proto, "subarray", [](CallInfo& ci) -> Value {
        Runtime& rt2 = rtOf(ci);
        DynTypedArray* ta = asTypedArray(ci.thisValue());
        if (!ta)
            ci.fail(ErrorCode::TypeError, "not a typed array");
        const double length = double(ta->length);
        const double begin = relative(rt2, ci.arg(0), length, 0);
        const double beginBytes = double(ta->byteOffset) + begin * double(sizeOf(ta->element));
        const Value bufferValue = objectValue(ta->buffer.get());
        if (ta->tracksLength && ci.arg(1).isUndefined())
            return speciesCreate(rt2, ta, {bufferValue, Value(beginBytes)});
        const double end = relative(rt2, ci.arg(1), length, length);
        return speciesCreate(rt2, ta, {bufferValue, Value(beginBytes), Value(std::max(end - begin, 0.0))});
    }, 2);

    rt.method(proto, "with", [self](CallInfo& ci) -> Value {
        Runtime& rt2 = rtOf(ci);
        DynTypedArray* ta = self(ci);
        const double length = double(ta->length);
        const double d = toNumber(rt2, ci.arg(0));
        const double whole = std::isnan(d) ? 0 : std::trunc(d);
        const double at = whole >= 0 ? whole : length + whole;
        const Value value = convertFor(rt2, ta->element, ci.arg(1));
        if (!(at >= 0 && at < double(ta->length)))
            ci.fail(ErrorCode::RangeError, "the index is out of range");
        Value out = allocateArray(rt2, ta->element, length);
        DynTypedArray* made = asTypedArray(out);
        const std::size_t size = sizeOf(ta->element);
        for (std::size_t i = 0; i < std::size_t(length); ++i) {
            if (double(i) == at)
                writeElement(made->element, made->elements + i * size, value);
            else if (i < ta->length)
                std::memcpy(made->elements + i * size, ta->elements + i * size, size);
        }
        return out;
    }, 2);

    proto->putOwn(rt.atom.toString.get(), rt.arrayProto->findOwn(rt.atom.toString.get())->value, PropWritable | PropConfigurable);
}

void installAbstract(Runtime& rt)
{
    TypedState& state = stateOf(rt);
    DynObject* proto = rt.newObject(rt.objectProto.get());
    state.abstractProto.reset(proto, true);
    Value ctorValue = rt.makeFunction(ascii("TypedArray"), [](CallInfo& ci) -> Value {
        ci.fail(ErrorCode::TypeError, "TypedArray is abstract: it cannot be called or constructed");
    }, 0);
    DynFunction* ctor = asFunction(ctorValue);
    ctor->isNativeConstructor = true;
    ctor->putOwn(rt.atom.prototype.get(), objectValue(proto), 0);
    proto->putOwn(rt.atom.constructor.get(), ctorValue, PropWritable | PropConfigurable);
    state.abstractCtor.reset(ctor);
    ctor->putOwn(rt.sym.species.get(), rt.makeFunction(ascii("get [Symbol.species]"), [](CallInfo& ci) { return ci.thisValue(); }, 0), PropConfigurable | PropAccessor);

    rt.method(ctor, "of", [](CallInfo& ci) -> Value {
        Runtime& rt2 = rtOf(ci);
        DynFunction* c = asFunction(ci.thisValue());
        if (!c || !isConstructor(rt2, c))
            ci.fail(ErrorCode::TypeError, "this is not a constructor");
        Value out = createFromCtor(rt2, ci.thisValue(), {Value(double(ci.size()))});
        for (std::size_t i = 0; i < ci.size(); ++i)
            setProperty(rt2, out, rt2.intern(numberToString(double(i))), ci.arg(i));
        return out;
    }, 0);
    rt.method(ctor, "from", [](CallInfo& ci) -> Value {
        Runtime& rt2 = rtOf(ci);
        DynFunction* c = asFunction(ci.thisValue());
        if (!c || !isConstructor(rt2, c))
            ci.fail(ErrorCode::TypeError, "this is not a constructor");
        const Value mapper = ci.arg(1);
        if (!mapper.isUndefined() && !asFunction(mapper))
            ci.fail(ErrorCode::TypeError, "the mapping function is not a function");
        const Value source = ci.arg(0);
        const Value usingIterator = getProperty(rt2, source, rt2.sym.iterator.get());
        Vec<Value> items;
        bool fromIterator = false;
        if (!usingIterator.isNullish()) {
            if (!asFunction(usingIterator))
                ci.fail(ErrorCode::TypeError, "Symbol.iterator is not a function");
            items = iterableToVector(rt2, source);
            fromIterator = true;
        }
        const Value object = fromIterator ? Value::undefined() : toObject(rt2, source);
        const double length = fromIterator ? double(items.size()) : lengthOfLike(rt2, object);
        Value out = createFromCtor(rt2, ci.thisValue(), {Value(length)});
        for (double k = 0; k < length; ++k) {
            checkLongLoop(rt2, std::size_t(k), 0);
            GCObject* key = rt2.intern(numberToString(k));
            Value item = fromIterator ? items[std::size_t(k)] : getProperty(rt2, object, key);
            if (!mapper.isUndefined())
                item = callValue(rt2, mapper, ci.arg(2), {item, Value(k)});
            setProperty(rt2, out, key, item);
        }
        return out;
    }, 1);

    installTypedArrayPrototype(rt, proto);

    for (int i = 0; i < int(ElementKind::Count); ++i) {
        const auto kind = ElementKind(i);
        DynObject* p = rt.newObject(proto);
        state.prototypes[i].reset(p, true);
        DynFunction* c = rt.makeConstructor(kKinds[i].name, [kind](CallInfo& ci) { return constructTyped(ci, kind); }, 3, p);
        c->setProto(ctor);
        state.constructors[i].reset(c);
        c->putOwn(rt.intern("BYTES_PER_ELEMENT"), Value(double(kKinds[i].size)), 0);
        p->putOwn(rt.intern("BYTES_PER_ELEMENT"), Value(double(kKinds[i].size)), 0);
    }
}

// ---- ArrayBuffer

void installBuffers(Runtime& rt)
{
    TypedState& state = stateOf(rt);
    const auto install = [&](bool shared) {
        const char* name = shared ? "SharedArrayBuffer" : "ArrayBuffer";
        DynObject* proto = rt.newObject(rt.objectProto.get());
        (shared ? state.sharedBufferProto : state.arrayBufferProto).reset(proto, true);
        DynFunction* ctor = rt.makeConstructor(name, [shared](CallInfo& ci) -> Value {
            Runtime& rt2 = rtOf(ci);
            if (!ci.isConstructorCall())
                ci.fail(ErrorCode::TypeError, std::string(shared ? "SharedArrayBuffer" : "ArrayBuffer") + " must be called with 'new'");
            const double length = toIndex(rt2, ci.arg(0));
            bool resizable = false;
            double maxBytes = 0;
            if (ci.arg(1).isObject()) {
                const Value option = getProperty(rt2, ci.arg(1), rt2.intern("maxByteLength"));
                if (!option.isUndefined()) {
                    maxBytes = toIndex(rt2, option);
                    resizable = true;
                    if (length > maxBytes)
                        ci.fail(ErrorCode::RangeError, "the length is past the maximum");
                }
            }
            if (length > double(kMaxBufferBytes))
                ci.fail(ErrorCode::RangeError, "an ArrayBuffer this large cannot be allocated");
            // The prototype is read only once the lengths are known to be acceptable.
            return detail::Access::adopt(Type::Object, newBuffer(rt2, length, nullptr, shared, resizable, maxBytes));
        }, 1, proto);
        (shared ? state.sharedBufferCtor : state.arrayBufferCtor).reset(ctor);
        ctor->putOwn(rt.sym.species.get(), rt.makeFunction(ascii("get [Symbol.species]"), [](CallInfo& ci) { return ci.thisValue(); }, 0), PropConfigurable | PropAccessor);
        proto->putOwn(rt.sym.toStringTag.get(), rt.str(name), PropConfigurable);

        rt.getter(proto, "byteLength", [shared](CallInfo& ci) -> Value {
            DynArrayBuffer* b = thisBuffer(ci, shared, "byteLength");
            return Value(double(b->byteLength));
        });
        rt.getter(proto, "maxByteLength", [shared](CallInfo& ci) -> Value {
            DynArrayBuffer* b = thisBuffer(ci, shared, "maxByteLength");
            return Value(double(b->detached ? 0 : b->resizable ? b->maxByteLength : b->byteLength));
        });
        rt.getter(proto, shared ? "growable" : "resizable", [shared](CallInfo& ci) -> Value {
            return Value(thisBuffer(ci, shared, "resizable")->resizable);
        });

        rt.method(proto, "slice", [shared](CallInfo& ci) -> Value {
            Runtime& rt2 = rtOf(ci);
            DynArrayBuffer* b = thisBuffer(ci, shared, "slice");
            if (b->detached)
                ci.fail(ErrorCode::TypeError, "the buffer is detached");
            const double length = double(b->byteLength);
            const double first = relative(rt2, ci.arg(0), length, 0);
            const double final = relative(rt2, ci.arg(1), length, length);
            const double newLength = std::max(final - first, 0.0);
            TypedState& st = stateOf(rt2);
            const Value ctor2 = speciesConstructor(rt2, ci.thisValue(), objectValue(shared ? st.sharedBufferCtor.get() : st.arrayBufferCtor.get()));
            Value out = constructValue(rt2, ctor2, {Value(newLength)});
            DynObject* outObject = asObject(out);
            if (!outObject || outObject->cls != (shared ? ObjClass::SharedArrayBuffer : ObjClass::ArrayBuffer))
                ci.fail(ErrorCode::TypeError, "the species constructor made something else");
            auto* made = static_cast<DynArrayBuffer*>(outObject);
            if (made->detached)
                ci.fail(ErrorCode::TypeError, "the new buffer is detached");
            if (made == b)
                ci.fail(ErrorCode::TypeError, "the species constructor returned the same buffer");
            if (double(made->byteLength) < newLength)
                ci.fail(ErrorCode::TypeError, "the species constructor made too short a buffer");
            if (b->detached)
                ci.fail(ErrorCode::TypeError, "the buffer was detached meanwhile");
            const double now = double(b->byteLength);
            if (first < now) {
                const double count = std::min(newLength, now - first);
                std::memcpy(made->data, b->data + std::size_t(first), std::size_t(count));
            }
            return out;
        }, 2);
    };
    install(false);
    install(true);

    DynObject* proto = state.arrayBufferProto.get();
    rt.method(state.arrayBufferCtor.get(), "isView", [](CallInfo& ci) -> Value {
        DynObject* o = asObject(ci.arg(0));
        return Value(o && (o->cls == ObjClass::TypedArray || o->cls == ObjClass::DataView));
    }, 1);
    rt.getter(proto, "detached", [](CallInfo& ci) -> Value { return Value(thisBuffer(ci, false, "detached")->detached); });
    rt.method(proto, "resize", [](CallInfo& ci) -> Value {
        Runtime& rt2 = rtOf(ci);
        DynArrayBuffer* b = thisBuffer(ci, false, "resize");
        if (!b->resizable)
            ci.fail(ErrorCode::TypeError, "the buffer is not resizable");
        const double wanted = toIndex(rt2, ci.arg(0));
        if (b->detached)
            ci.fail(ErrorCode::TypeError, "the buffer is detached");
        if (wanted > double(b->maxByteLength))
            ci.fail(ErrorCode::RangeError, "past the maximum length");
        const std::size_t size = std::size_t(wanted);
        if (size < b->byteLength)
            std::memset(b->data + size, 0, b->byteLength - size);
        b->byteLength = size;
        for (DynTypedArray* view : b->views)
            view->refresh();
        return Value::undefined();
    }, 1);
    const auto transfer = [](CallInfo& ci, bool keepResizable) -> Value {
        Runtime& rt2 = rtOf(ci);
        DynArrayBuffer* b = thisBuffer(ci, false, "transfer");
        const double wanted = ci.arg(0).isUndefined() ? double(b->byteLength) : toIndex(rt2, ci.arg(0));
        if (b->detached)
            ci.fail(ErrorCode::TypeError, "the buffer is detached");
        const bool resizable = keepResizable && b->resizable;
        const double maxBytes = resizable ? double(b->maxByteLength) : 0;
        if (resizable && wanted > maxBytes)
            ci.fail(ErrorCode::RangeError, "past the maximum length");
        DynArrayBuffer* made = newBuffer(rt2, wanted, nullptr, false, resizable, maxBytes);
        Value out = detail::Access::adopt(Type::Object, made);
        std::memcpy(made->data, b->data, std::min(std::size_t(wanted), b->byteLength));
        detachBuffer(rt2, b);
        return out;
    };
    rt.method(proto, "transfer", [transfer](CallInfo& ci) { return transfer(ci, true); }, 0);
    rt.method(proto, "transferToFixedLength", [transfer](CallInfo& ci) { return transfer(ci, false); }, 0);

    DynObject* sharedProto = state.sharedBufferProto.get();
    rt.method(sharedProto, "grow", [](CallInfo& ci) -> Value {
        Runtime& rt2 = rtOf(ci);
        DynArrayBuffer* b = thisBuffer(ci, true, "grow");
        if (!b->resizable)
            ci.fail(ErrorCode::TypeError, "the buffer is not growable");
        const double wanted = toIndex(rt2, ci.arg(0));
        if (wanted > double(b->maxByteLength) || wanted < double(b->byteLength))
            ci.fail(ErrorCode::RangeError, "a buffer can only grow, up to its maximum");
        b->byteLength = std::size_t(wanted);
        for (DynTypedArray* view : b->views)
            view->refresh();
        return Value::undefined();
    }, 1);
}

// ---- DataView

bool viewOutOfBounds(const DynDataView* v) noexcept
{
    const DynArrayBuffer* b = v->buffer.get();
    if (b->detached)
        return true;
    if (v->byteOffset > b->byteLength)
        return true;
    return !v->tracksLength && v->byteOffset + v->fixedLength > b->byteLength;
}

std::size_t viewLength(const DynDataView* v) noexcept
{
    return v->tracksLength ? v->buffer->byteLength - v->byteOffset : v->fixedLength;
}

DynDataView* thisDataView(CallInfo& ci)
{
    DynObject* o = asObject(ci.thisValue());
    if (!o || o->cls != ObjClass::DataView)
        ci.fail(ErrorCode::TypeError, "not a DataView");
    return static_cast<DynDataView*>(o);
}

void installDataView(Runtime& rt)
{
    DynObject* proto = rt.newObject(rt.objectProto.get());
    stateOf(rt).dataViewProto.reset(proto, true);
    rt.makeConstructor("DataView", [](CallInfo& ci) -> Value {
        Runtime& rt2 = rtOf(ci);
        if (!ci.isConstructorCall())
            ci.fail(ErrorCode::TypeError, "DataView must be called with 'new'");
        DynObject* o = asObject(ci.arg(0));
        if (!o || (o->cls != ObjClass::ArrayBuffer && o->cls != ObjClass::SharedArrayBuffer))
            ci.fail(ErrorCode::TypeError, "DataView needs an ArrayBuffer");
        auto* buffer = static_cast<DynArrayBuffer*>(o);
        const double offset = toIndex(rt2, ci.arg(1));
        if (buffer->detached)
            ci.fail(ErrorCode::TypeError, "the buffer is detached");
        const double bufferLength = double(buffer->byteLength);
        if (offset > bufferLength)
            ci.fail(ErrorCode::RangeError, "the offset is past the end of the buffer");
        const bool hasLength = !ci.arg(2).isUndefined();
        const bool tracks = !hasLength && buffer->resizable;
        double length = bufferLength - offset;
        if (hasLength) {
            length = toIndex(rt2, ci.arg(2));
            if (offset + length > bufferLength)
                ci.fail(ErrorCode::RangeError, "the view does not fit in the buffer");
        }
        auto* view = rt2.newCell<DynDataView>();
        Value out = detail::Access::adopt(Type::Object, view);
        view->cls = ObjClass::DataView;
        view->setProto(stateOf(rt2).dataViewProto.get());
        view->buffer.reset(buffer);
        view->byteOffset = std::size_t(offset);
        view->fixedLength = std::size_t(length);
        view->tracksLength = tracks;
        return out;
    }, 1, proto);
    proto->putOwn(rt.sym.toStringTag.get(), rt.str("DataView"), PropConfigurable);
    rt.getter(proto, "buffer", [](CallInfo& ci) -> Value { return objectValue(thisDataView(ci)->buffer.get()); });
    rt.getter(proto, "byteLength", [](CallInfo& ci) -> Value {
        DynDataView* v = thisDataView(ci);
        if (viewOutOfBounds(v))
            ci.fail(ErrorCode::TypeError, "the view's buffer is detached or too short for it");
        return Value(double(viewLength(v)));
    });
    rt.getter(proto, "byteOffset", [](CallInfo& ci) -> Value {
        DynDataView* v = thisDataView(ci);
        if (viewOutOfBounds(v))
            ci.fail(ErrorCode::TypeError, "the view's buffer is detached or too short for it");
        return Value(double(v->byteOffset));
    });

    static const struct { const char* get; const char* set; ElementKind kind; } kAccessors[] = {
        {"getInt8", "setInt8", ElementKind::Int8},          {"getUint8", "setUint8", ElementKind::Uint8},
        {"getInt16", "setInt16", ElementKind::Int16},       {"getUint16", "setUint16", ElementKind::Uint16},
        {"getInt32", "setInt32", ElementKind::Int32},       {"getUint32", "setUint32", ElementKind::Uint32},
        {"getFloat16", "setFloat16", ElementKind::Float16}, {"getFloat32", "setFloat32", ElementKind::Float32},
        {"getFloat64", "setFloat64", ElementKind::Float64}, {"getBigInt64", "setBigInt64", ElementKind::BigInt64},
        {"getBigUint64", "setBigUint64", ElementKind::BigUint64},
    };
    for (const auto& accessor : kAccessors) {
        const ElementKind kind = accessor.kind;
        rt.method(proto, accessor.get, [kind](CallInfo& ci) -> Value {
            Runtime& rt2 = rtOf(ci);
            DynDataView* v = thisDataView(ci);
            const double index = toIndex(rt2, ci.arg(0));
            const bool little = toBoolean(ci.arg(1));
            if (viewOutOfBounds(v))
                ci.fail(ErrorCode::TypeError, "the view's buffer is detached or too short for it");
            const std::size_t size = sizeOf(kind);
            if (index + double(size) > double(viewLength(v)))
                ci.fail(ErrorCode::RangeError, "past the end of the view");
            std::uint8_t bytes[8];
            std::memcpy(bytes, v->buffer->data + v->byteOffset + std::size_t(index), size);
            if (!little)
                std::reverse(bytes, bytes + size);
            return readElement(rt2, kind, bytes);
        }, 1);
        rt.method(proto, accessor.set, [kind](CallInfo& ci) -> Value {
            Runtime& rt2 = rtOf(ci);
            DynDataView* v = thisDataView(ci);
            const double index = toIndex(rt2, ci.arg(0));
            const Value value = convertFor(rt2, kind, ci.arg(1));
            const bool little = toBoolean(ci.arg(2));
            if (viewOutOfBounds(v))
                ci.fail(ErrorCode::TypeError, "the view's buffer is detached or too short for it");
            const std::size_t size = sizeOf(kind);
            if (index + double(size) > double(viewLength(v)))
                ci.fail(ErrorCode::RangeError, "past the end of the view");
            std::uint8_t bytes[8];
            writeElement(kind, bytes, value);
            if (!little)
                std::reverse(bytes, bytes + size);
            std::memcpy(v->buffer->data + v->byteOffset + std::size_t(index), bytes, size);
            return Value::undefined();
        }, 2);
    }
}

// ---- Atomics: one thread of execution, so every operation is atomic already

DynTypedArray* atomicArray(CallInfo& ci, bool waitable)
{
    Runtime& rt = rtOf(ci);
    DynTypedArray* ta = validate(rt, ci.arg(0));
    const ElementKind k = ta->element;
    const bool integer = k == ElementKind::Int8 || k == ElementKind::Uint8 || k == ElementKind::Int16 || k == ElementKind::Uint16 || k == ElementKind::Int32 ||
                         k == ElementKind::Uint32 || k == ElementKind::BigInt64 || k == ElementKind::BigUint64;
    if (!integer || (waitable && k != ElementKind::Int32 && k != ElementKind::BigInt64))
        ci.fail(ErrorCode::TypeError, "Atomics does not work on this kind of array");
    return ta;
}

std::size_t atomicIndex(CallInfo& ci, DynTypedArray* ta)
{
    const double i = toIndex(rtOf(ci), ci.arg(1));
    if (i >= double(ta->length))
        ci.fail(ErrorCode::RangeError, "the index is out of range");
    return std::size_t(i);
}

void installAtomics(Runtime& rt)
{
    DynObject* atomics = rt.newObject(rt.objectProto.get());
    Value atomicsValue = detail::Access::adopt(Type::Object, atomics);
    rt.globalObject->putOwn(rt.intern("Atomics"), atomicsValue, PropWritable | PropConfigurable);
    atomics->putOwn(rt.sym.toStringTag.get(), rt.str("Atomics"), PropConfigurable);

    const auto modify = [](CallInfo& ci, std::uint64_t (*op)(std::uint64_t, std::uint64_t)) -> Value {
        Runtime& rt2 = rtOf(ci);
        DynTypedArray* ta = atomicArray(ci, false);
        const std::size_t i = atomicIndex(ci, ta);
        Value operand;
        if (isBigKind(ta->element)) {
            operand = toBigInt(rt2, ci.arg(2));
        } else {
            const double d = toNumber(rt2, ci.arg(2));
            operand = Value(std::isnan(d) ? 0.0 : std::trunc(d));
        }
        validate(rt2, ci.arg(0));
        if (i >= ta->length)
            ci.fail(ErrorCode::RangeError, "the index is out of range");
        std::uint8_t* p = ta->elements + i * sizeOf(ta->element);
        const Value before = readElement(rt2, ta->element, p);
        // Work in the widest kind; what goes back is cut to the element's size.
        const std::uint64_t a = isBigKind(ta->element) ? bigint::toUint64(bigint::cellOf(before)) : std::uint64_t(std::int64_t(numberOf(before)));
        const std::uint64_t b = isBigKind(ta->element) ? bigint::toUint64(bigint::cellOf(operand)) : std::uint64_t(std::int64_t(std::fmod(numberOf(operand), 18446744073709551616.0)));
        const std::uint64_t r = op(a, b);
        switch (sizeOf(ta->element)) {
            case 1: *p = std::uint8_t(r); break;
            case 2: writeAs<std::uint16_t>(p, std::uint16_t(r)); break;
            case 4: writeAs<std::uint32_t>(p, std::uint32_t(r)); break;
            default: writeAs<std::uint64_t>(p, r); break;
        }
        return before;
    };
    rt.method(atomics, "add", [modify](CallInfo& ci) { return modify(ci, [](std::uint64_t a, std::uint64_t b) { return a + b; }); }, 3);
    rt.method(atomics, "sub", [modify](CallInfo& ci) { return modify(ci, [](std::uint64_t a, std::uint64_t b) { return a - b; }); }, 3);
    rt.method(atomics, "and", [modify](CallInfo& ci) { return modify(ci, [](std::uint64_t a, std::uint64_t b) { return a & b; }); }, 3);
    rt.method(atomics, "or", [modify](CallInfo& ci) { return modify(ci, [](std::uint64_t a, std::uint64_t b) { return a | b; }); }, 3);
    rt.method(atomics, "xor", [modify](CallInfo& ci) { return modify(ci, [](std::uint64_t a, std::uint64_t b) { return a ^ b; }); }, 3);
    rt.method(atomics, "exchange", [modify](CallInfo& ci) { return modify(ci, [](std::uint64_t, std::uint64_t b) { return b; }); }, 3);

    rt.method(atomics, "load", [](CallInfo& ci) -> Value {
        Runtime& rt2 = rtOf(ci);
        DynTypedArray* ta = atomicArray(ci, false);
        const std::size_t i = atomicIndex(ci, ta);
        validate(rt2, ci.arg(0));
        if (i >= ta->length)
            ci.fail(ErrorCode::RangeError, "the index is out of range");
        return typedArrayGet(rt2, ta, i);
    }, 2);
    rt.method(atomics, "store", [](CallInfo& ci) -> Value {
        Runtime& rt2 = rtOf(ci);
        DynTypedArray* ta = atomicArray(ci, false);
        const std::size_t i = atomicIndex(ci, ta);
        Value result;
        if (isBigKind(ta->element)) {
            result = toBigInt(rt2, ci.arg(2));
        } else {
            const double d = toNumber(rt2, ci.arg(2));
            const double whole = std::isnan(d) ? 0 : std::trunc(d);
            result = Value(whole == 0 ? 0.0 : whole);
        }
        validate(rt2, ci.arg(0));
        if (i >= ta->length)
            ci.fail(ErrorCode::RangeError, "the index is out of range");
        writeElement(ta->element, ta->elements + i * sizeOf(ta->element), result);
        return result;
    }, 3);
    rt.method(atomics, "compareExchange", [](CallInfo& ci) -> Value {
        Runtime& rt2 = rtOf(ci);
        DynTypedArray* ta = atomicArray(ci, false);
        const std::size_t i = atomicIndex(ci, ta);
        Value expected, replacement;
        if (isBigKind(ta->element)) {
            expected = toBigInt(rt2, ci.arg(2));
            replacement = toBigInt(rt2, ci.arg(3));
        } else {
            expected = Value(toNumber(rt2, ci.arg(2)));
            replacement = Value(toNumber(rt2, ci.arg(3)));
        }
        validate(rt2, ci.arg(0));
        if (i >= ta->length)
            ci.fail(ErrorCode::RangeError, "the index is out of range");
        std::uint8_t* p = ta->elements + i * sizeOf(ta->element);
        const Value before = readElement(rt2, ta->element, p);
        std::uint8_t wanted[8];
        writeElement(ta->element, wanted, expected);
        if (std::memcmp(wanted, p, sizeOf(ta->element)) == 0)
            writeElement(ta->element, p, replacement);
        return before;
    }, 4);
    rt.method(atomics, "pause", [](CallInfo& ci) -> Value {
        const Value n = ci.arg(0);
        if (!n.isUndefined() && (!n.isNumber() || numberOf(n) != std::trunc(numberOf(n)) || !std::isfinite(numberOf(n))))
            ci.fail(ErrorCode::TypeError, "Atomics.pause takes an integer");
        return Value::undefined();
    }, 0);
    rt.method(atomics, "isLockFree", [](CallInfo& ci) -> Value {
        const double n = toNumber(rtOf(ci), ci.arg(0));
        return Value(n == 1 || n == 2 || n == 4 || n == 8);
    }, 1);
    rt.method(atomics, "notify", [](CallInfo& ci) -> Value {
        Runtime& rt2 = rtOf(ci);
        DynTypedArray* ta = atomicArray(ci, true);
        atomicIndex(ci, ta);
        if (!ci.arg(2).isUndefined()) {
            const double d = toNumber(rt2, ci.arg(2));
            (void)d;
        }
        // Nothing can be waiting: there is one thread.
        return Value(0.0);
    }, 3);
    rt.method(atomics, "wait", [](CallInfo& ci) -> Value {
        Runtime& rt2 = rtOf(ci);
        DynTypedArray* ta = atomicArray(ci, true);
        if (ta->buffer->cls != ObjClass::SharedArrayBuffer)
            ci.fail(ErrorCode::TypeError, "Atomics.wait needs a shared buffer");
        const std::size_t i = atomicIndex(ci, ta);
        Value expected;
        if (isBigKind(ta->element))
            expected = toBigInt(rt2, ci.arg(2));
        else
            expected = Value(double(toInt32(toNumber(rt2, ci.arg(2)))));
        const double timeout = toNumber(rt2, ci.arg(3));
        (void)timeout;
        std::uint8_t wanted[8];
        writeElement(ta->element, wanted, expected);
        if (std::memcmp(wanted, ta->elements + i * sizeOf(ta->element), sizeOf(ta->element)) != 0)
            return rt2.str("not-equal");
        // Nothing could wake it, so all it can do is run out of time; not waiting comes to the same answer.
        return rt2.str("timed-out");
    }, 4);
}

// ---- Uint8Array and text

const char kBase64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
const char kBase64Url[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

DynTypedArray* thisBytes(CallInfo& ci)
{
    DynTypedArray* ta = asTypedArray(ci.thisValue());
    if (!ta || ta->element != ElementKind::Uint8)
        ci.fail(ErrorCode::TypeError, "not a Uint8Array");
    return ta;
}

struct Base64Options
{
    bool url = false;
    int lastChunk = 0;  // 0 loose, 1 strict, 2 stop-before-partial
};

Base64Options base64Options(CallInfo& ci, const Value& options, bool reading)
{
    Runtime& rt = rtOf(ci);
    Base64Options out;
    if (options.isUndefined())
        return out;
    if (!options.isObject())
        ci.fail(ErrorCode::TypeError, "the options are not an object");
    const Value alphabet = getProperty(rt, options, rt.intern("alphabet"));
    if (!alphabet.isUndefined()) {
        if (!alphabet.isString())
            ci.fail(ErrorCode::TypeError, "alphabet is not a string");
        const StringView a = asString(alphabet)->text();
        if (a == StringView(u"base64url"))
            out.url = true;
        else if (a != StringView(u"base64"))
            ci.fail(ErrorCode::TypeError, "alphabet is not one of the two there are");
    }
    if (reading) {
        const Value last = getProperty(rt, options, rt.intern("lastChunkHandling"));
        if (!last.isUndefined()) {
            if (!last.isString())
                ci.fail(ErrorCode::TypeError, "lastChunkHandling is not a string");
            const StringView l = asString(last)->text();
            if (l == StringView(u"strict"))
                out.lastChunk = 1;
            else if (l == StringView(u"stop-before-partial"))
                out.lastChunk = 2;
            else if (l != StringView(u"loose"))
                ci.fail(ErrorCode::TypeError, "lastChunkHandling is not one of the three there are");
        }
    }
    return out;
}

/// FromBase64: what it decodes, how far it read, and whether what it read was acceptable.
struct Decoded
{
    std::vector<std::uint8_t> bytes;
    std::size_t read = 0;
    bool bad = false;
};

Decoded decodeBase64(StringView s, const Base64Options& options, std::size_t maxLength)
{
    Decoded out;
    if (maxLength == 0)
        return out;
    const auto isWhite = [](Char c) { return c == 9 || c == 10 || c == 12 || c == 13 || c == 32; };
    const auto value = [&](Char c) -> int {
        if (c >= 'A' && c <= 'Z')
            return c - 'A';
        if (c >= 'a' && c <= 'z')
            return c - 'a' + 26;
        if (c >= '0' && c <= '9')
            return c - '0' + 52;
        if (c == (options.url ? '-' : '+'))
            return 62;
        if (c == (options.url ? '_' : '/'))
            return 63;
        return -1;
    };
    std::size_t i = 0;
    int chunk[4];
    int have = 0;
    const auto flush = [&](int count) {
        std::uint32_t bits = 0;
        for (int k = 0; k < 4; ++k)
            bits = (bits << 6) | std::uint32_t(k < count ? chunk[k] : 0);
        out.bytes.push_back(std::uint8_t(bits >> 16));
        if (count > 2)
            out.bytes.push_back(std::uint8_t(bits >> 8));
        if (count > 3)
            out.bytes.push_back(std::uint8_t(bits));
    };
    for (;;) {
        while (i < s.size() && isWhite(s[i]))
            ++i;
        if (i >= s.size()) {
            if (have > 0) {
                if (options.lastChunk == 2)
                    return out;
                if (options.lastChunk == 1 || have == 1) {
                    out.bad = true;
                    return out;
                }
                flush(have);
            }
            out.read = s.size();
            return out;
        }
        const Char c = s[i++];
        if (c == '=') {
            if (have < 2) {
                out.bad = true;
                return out;
            }
            while (i < s.size() && isWhite(s[i]))
                ++i;
            if (have == 2) {
                if (i >= s.size()) {
                    if (options.lastChunk == 2) {
                        out.read = out.read;
                        return out;
                    }
                    out.bad = true;
                    return out;
                }
                if (s[i] != '=') {
                    out.bad = true;
                    return out;
                }
                ++i;
                while (i < s.size() && isWhite(s[i]))
                    ++i;
            }
            if (i < s.size()) {
                out.bad = true;
                return out;
            }
            if (options.lastChunk == 1) {
                const std::uint32_t extra = have == 2 ? std::uint32_t(chunk[1]) & 15 : std::uint32_t(chunk[2]) & 3;
                if (extra) {
                    out.bad = true;
                    return out;
                }
            }
            flush(have);
            out.read = s.size();
            return out;
        }
        const int v = value(c);
        if (v < 0) {
            out.bad = true;
            return out;
        }
        const std::size_t remaining = maxLength - out.bytes.size();
        if (remaining == 1 && options.lastChunk == 0 + 0 && have == 2 && false)
            return out;
        chunk[have++] = v;
        if (have == 4) {
            if (remaining < 3)
                return out;
            flush(4);
            have = 0;
            out.read = i;
            if (out.bytes.size() == maxLength)
                return out;
        } else if (have == 2 && remaining == 1) {
            // One byte more is all there is room for, and the next character may or may not belong to it: leave it.
        }
    }
}

int hexDigit(Char c) noexcept
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

void installBytesAndText(Runtime& rt)
{
    TypedState& state = stateOf(rt);
    DynFunction* ctor = state.constructors[int(ElementKind::Uint8)].get();
    DynObject* proto = state.prototypes[int(ElementKind::Uint8)].get();

    rt.method(proto, "toBase64", [](CallInfo& ci) -> Value {
        Runtime& rt2 = rtOf(ci);
        DynTypedArray* ta = thisBytes(ci);
        const Base64Options options = base64Options(ci, ci.arg(0), false);
        const Value omit = ci.arg(0).isUndefined() ? Value::undefined() : getProperty(rt2, ci.arg(0), rt2.intern("omitPadding"));
        const bool pad = !toBoolean(omit);
        validate(rt2, ci.thisValue());
        const char* table = options.url ? kBase64Url : kBase64;
        String out;
        std::size_t i = 0;
        const std::size_t n = ta->length;
        const std::uint8_t* b = ta->elements;
        for (; i + 3 <= n; i += 3) {
            const std::uint32_t bits = (std::uint32_t(b[i]) << 16) | (std::uint32_t(b[i + 1]) << 8) | b[i + 2];
            out.push_back(Char(table[bits >> 18]));
            out.push_back(Char(table[(bits >> 12) & 63]));
            out.push_back(Char(table[(bits >> 6) & 63]));
            out.push_back(Char(table[bits & 63]));
        }
        if (n - i == 1) {
            const std::uint32_t bits = std::uint32_t(b[i]) << 16;
            out.push_back(Char(table[bits >> 18]));
            out.push_back(Char(table[(bits >> 12) & 63]));
            if (pad)
                out += ascii("==");
        } else if (n - i == 2) {
            const std::uint32_t bits = (std::uint32_t(b[i]) << 16) | (std::uint32_t(b[i + 1]) << 8);
            out.push_back(Char(table[bits >> 18]));
            out.push_back(Char(table[(bits >> 12) & 63]));
            out.push_back(Char(table[(bits >> 6) & 63]));
            if (pad)
                out.push_back(u'=');
        }
        return rt2.str(out);
    }, 0);
    rt.method(proto, "toHex", [](CallInfo& ci) -> Value {
        Runtime& rt2 = rtOf(ci);
        DynTypedArray* ta = thisBytes(ci);
        validate(rt2, ci.thisValue());
        static const char kHex[] = "0123456789abcdef";
        String out;
        out.reserve(ta->length * 2);
        for (std::size_t i = 0; i < ta->length; ++i) {
            out.push_back(Char(kHex[ta->elements[i] >> 4]));
            out.push_back(Char(kHex[ta->elements[i] & 15]));
        }
        return rt2.str(out);
    }, 0);

    rt.method(ctor, "fromBase64", [](CallInfo& ci) -> Value {
        Runtime& rt2 = rtOf(ci);
        if (!ci.arg(0).isString())
            ci.fail(ErrorCode::TypeError, "fromBase64 takes a string");
        const Base64Options options = base64Options(ci, ci.arg(1), true);
        const Decoded decoded = decodeBase64(asString(ci.arg(0))->text(), options, std::size_t(-1));
        if (decoded.bad)
            ci.fail(ErrorCode::SyntaxError, "not valid base64");
        Value out = allocateArray(rt2, ElementKind::Uint8, double(decoded.bytes.size()));
        if (!decoded.bytes.empty())
            std::memcpy(asTypedArray(out)->elements, decoded.bytes.data(), decoded.bytes.size());
        return out;
    }, 1);
    rt.method(proto, "setFromBase64", [](CallInfo& ci) -> Value {
        Runtime& rt2 = rtOf(ci);
        DynTypedArray* ta = thisBytes(ci);
        if (!ci.arg(0).isString())
            ci.fail(ErrorCode::TypeError, "setFromBase64 takes a string");
        const Base64Options options = base64Options(ci, ci.arg(1), true);
        validate(rt2, ci.thisValue());
        const Decoded decoded = decodeBase64(asString(ci.arg(0))->text(), options, ta->length);
        if (decoded.bad) {
            if (!decoded.bytes.empty())
                std::memcpy(ta->elements, decoded.bytes.data(), std::min(decoded.bytes.size(), ta->length));
            ci.fail(ErrorCode::SyntaxError, "not valid base64");
        }
        if (!decoded.bytes.empty())
            std::memcpy(ta->elements, decoded.bytes.data(), std::min(decoded.bytes.size(), ta->length));
        DynObject* result = rt2.newObject();
        Value out = detail::Access::adopt(Type::Object, result);
        result->putOwn(rt2.intern("read"), Value(double(decoded.read)));
        result->putOwn(rt2.intern("written"), Value(double(std::min(decoded.bytes.size(), ta->length))));
        return out;
    }, 1);
    rt.method(ctor, "fromHex", [](CallInfo& ci) -> Value {
        Runtime& rt2 = rtOf(ci);
        if (!ci.arg(0).isString())
            ci.fail(ErrorCode::TypeError, "fromHex takes a string");
        const StringView s = asString(ci.arg(0))->text();
        if (s.size() % 2)
            ci.fail(ErrorCode::SyntaxError, "an odd number of hex digits");
        Value out = allocateArray(rt2, ElementKind::Uint8, double(s.size() / 2));
        DynTypedArray* ta = asTypedArray(out);
        for (std::size_t i = 0; i < s.size(); i += 2) {
            const int a = hexDigit(s[i]);
            const int b = hexDigit(s[i + 1]);
            if (a < 0 || b < 0)
                ci.fail(ErrorCode::SyntaxError, "not a hex digit");
            ta->elements[i / 2] = std::uint8_t(a * 16 + b);
        }
        return out;
    }, 1);
    rt.method(proto, "setFromHex", [](CallInfo& ci) -> Value {
        Runtime& rt2 = rtOf(ci);
        DynTypedArray* ta = thisBytes(ci);
        if (!ci.arg(0).isString())
            ci.fail(ErrorCode::TypeError, "setFromHex takes a string");
        validate(rt2, ci.thisValue());
        const StringView s = asString(ci.arg(0))->text();
        if (s.size() % 2)
            ci.fail(ErrorCode::SyntaxError, "an odd number of hex digits");
        std::size_t written = 0;
        std::size_t read = 0;
        bool bad = false;
        while (read + 1 < s.size() && written < ta->length) {
            const int a = hexDigit(s[read]);
            const int b = hexDigit(s[read + 1]);
            if (a < 0 || b < 0) {
                bad = true;
                break;
            }
            ta->elements[written++] = std::uint8_t(a * 16 + b);
            read += 2;
        }
        if (bad)
            ci.fail(ErrorCode::SyntaxError, "not a hex digit");
        DynObject* result = rt2.newObject();
        Value out = detail::Access::adopt(Type::Object, result);
        result->putOwn(rt2.intern("read"), Value(double(read)));
        result->putOwn(rt2.intern("written"), Value(double(written)));
        return out;
    }, 1);
}

}  // namespace

// ---- the cells

DynArrayBuffer::~DynArrayBuffer() { std::free(data); }
std::size_t DynArrayBuffer::heapSize() const { return sizeof(*this) + byteLength; }

void DynArrayBuffer::clearReferences()
{
    DynObject::clearReferences();
    views.clear();
}

DynTypedArray::~DynTypedArray()
{
    if (buffer) {
        auto& views = buffer->views;
        auto it = std::find(views.begin(), views.end(), this);
        if (it != views.end())
            views.erase(it);
    }
}

void DynTypedArray::children(GcList& out) const
{
    DynObject::children(out);
    if (buffer)
        out.push_back(buffer.get());
}

void DynTypedArray::clearReferences()
{
    DynObject::clearReferences();
    if (buffer) {
        auto& views = buffer->views;
        auto it = std::find(views.begin(), views.end(), this);
        if (it != views.end())
            views.erase(it);
    }
    buffer.reset();
    elements = nullptr;
    length = 0;
}

void DynTypedArray::refresh() noexcept
{
    const DynArrayBuffer* b = buffer.get();
    const std::size_t size = sizeOf(element);
    outOfBounds = false;
    if (b->detached) {
        elements = nullptr;
        length = 0;
        outOfBounds = true;
        return;
    }
    std::size_t count = fixedLength;
    if (tracksLength) {
        if (byteOffset > b->byteLength) {
            outOfBounds = true;
            count = 0;
        } else {
            count = (b->byteLength - byteOffset) / size;
        }
    } else if (byteOffset + fixedLength * size > b->byteLength) {
        outOfBounds = true;
        count = 0;
    }
    length = count;
    elements = outOfBounds ? nullptr : b->data + byteOffset;
}

void DynDataView::children(GcList& out) const
{
    DynObject::children(out);
    if (buffer)
        out.push_back(buffer.get());
}

void DynDataView::clearReferences()
{
    DynObject::clearReferences();
    buffer.reset();
}

bool numericKey(const GCObject* key, double& index)
{
    if (key->kind != CellKind::String)
        return false;
    const StringView text = static_cast<const StringCell*>(key)->text();
    if (text.empty())
        return false;
    const Char c = text[0];
    if (!(c >= '0' && c <= '9') && c != '-' && c != 'I' && c != 'N')
        return false;
    std::uint32_t small = 0;
    if (arrayIndexOf(text, small)) {
        index = small;
        return true;
    }
    if (text == StringView(u"-0")) {
        index = -1;
        return true;
    }
    const double n = stringToNumber(text);
    if (numberToString(n) != text)
        return false;
    index = (n >= 0 && n == std::floor(n) && !std::isinf(n)) ? n : -1;
    return true;
}

Value typedArrayGet(Runtime& rt, const DynTypedArray* array, std::size_t index)
{
    return readElement(rt, array->element, array->elements + index * sizeOf(array->element));
}

void typedArraySet(Runtime& rt, DynTypedArray* array, double index, const Value& v)
{
    const Value converted = convertFor(rt, array->element, v);
    if (typedArrayHas(array, index))
        writeElement(array->element, array->elements + std::size_t(index) * sizeOf(array->element), converted);
}

void detachBuffer(Runtime& rt, DynArrayBuffer* buffer)
{
    if (buffer->shared)
        throwError(rt, ErrorCode::TypeError, "a shared buffer cannot be detached");
    std::free(buffer->data);
    buffer->data = nullptr;
    buffer->byteLength = 0;
    buffer->maxByteLength = 0;
    buffer->detached = true;
    for (DynTypedArray* view : buffer->views)
        view->refresh();
}

void installTypedArrays(Runtime& rt)
{
    rt.typed = std::make_shared<TypedState>();
    installBuffers(rt);
    installAbstract(rt);
    installDataView(rt);
    installAtomics(rt);
    installBytesAndText(rt);
}

}  // namespace dyn
}  // namespace internal
}  // namespace script
