// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// ArrayBuffer, the typed arrays and DataView. The buffers do not resize or detach.
#include <algorithm>
#include <cmath>
#include <cstring>

#include "script/internal/vm/typed.hpp"
#include "builtins_util.hpp"

namespace script {
namespace internal {
namespace vm {
namespace bi {

namespace {

/// Where `VM::typedCtors` keeps the ArrayBuffer and SharedArrayBuffer constructors, after the typed arrays'.
constexpr std::size_t kBufferSlot = 12, kSharedBufferSlot = 13;

const char* const kNames[] = {"Int8Array",  "Uint8Array",   "Uint8ClampedArray", "Int16Array",    "Uint16Array",   "Int32Array",
                              "Uint32Array", "Float32Array", "Float64Array",      "BigInt64Array", "BigUint64Array", "Float16Array"};

BufferState* bufferOf(JSValue v)
{
    if (isObject(v) && asObject(v)->cls() == ObjClass::ArrayBuffer)
        if (Rare* r = asObject(v)->rare())
            return static_cast<BufferState*>(r->ext);
    return nullptr;
}

JSTypedArray* asTyped(JSValue v)
{
    return isObject(v) && asObject(v)->cls() == ObjClass::TypedArray ? static_cast<JSTypedArray*>(asObject(v)) : nullptr;
}

/// Whether the typed array no longer fits its buffer, or the buffer is gone.
bool outOfBounds(const JSTypedArray* t)
{
    const BufferState* b = bufferOf(t->buffer);
    if (!b || b->detached)
        return true;
    if (t->tracking)
        return t->byteOffset > b->size;
    return std::size_t(t->byteOffset) + std::size_t(t->fixedLength) * kElemSize[std::size_t(t->kind)] > b->size;
}

/// ToIndex: false with a RangeError pending.
bool toIndex(VM& vm, JSValue v, const char* what, double* out)
{
    if (v.isUndefined()) {
        *out = 0;
        return true;
    }
    double d;
    if (!toNumber(vm, v, &d))
        return false;
    d = d != d ? 0 : std::trunc(d);
    if (d < 0 || d > 9007199254740991.0) {
        vm.throwError(ErrorCode::RangeError, std::string("Invalid ") + what);
        return false;
    }
    *out = d;
    return true;
}

double relative(double v, double length, double fallback, bool undefinedGivesFallback, bool isUndefined)
{
    if (isUndefined && undefinedGivesFallback)
        return fallback;
    v = v != v ? 0 : std::trunc(v);
    if (v < 0)
        return std::max(0.0, length + v);
    return std::min(v, length);
}

bool relativeArg(VM& vm, JSValue v, double length, double fallback, double* out)
{
    if (v.isUndefined()) {
        *out = fallback;
        return true;
    }
    double d;
    if (!toNumber(vm, v, &d))
        return false;
    *out = relative(d, length, fallback, false, false);
    return true;
}

/// A new typed array of `kind` with its own buffer.
JSTypedArray* makeTyped(VM& vm, ElemKind kind, JSObject* proto, std::uint32_t length)
{
    const std::size_t bytes = std::size_t(length) * kElemSize[std::size_t(kind)];
    JSObject* buffer = vm.newArrayBuffer(bytes);
    if (!buffer)
        return nullptr;
    Handle keep = Handle::adopt(toJSValue(buffer));
    return vm.newTypedArray(proto ? proto : vm.protos.typedArrays[std::size_t(kind)], kind, buffer, 0, length);
}

NATIVE(arrayBufferCtor)
{
    UNUSED();
    const bool shared = data != nullptr;
    if (vm.newTarget.isUndefined() || !isObject(self))
        return typeError(vm, shared ? "Constructor SharedArrayBuffer requires 'new'" : "Constructor ArrayBuffer requires 'new'");
    double length;
    if (!toIndex(vm, arg(args, argc, 0), "array buffer length", &length))
        return JSValue::exception();
    double maxLength = -1;
    if (isObject(arg(args, argc, 1))) {
        Key key = vm.internKey(std::string_view("maxByteLength"));
        JSValue m = getProperty(vm, args[1], key);
        detail::decRef(key);
        if (m.isException())
            return m;
        const bool present = !m.isUndefined();
        const bool ok = !present || toIndex(vm, m, "array buffer max length", &maxLength);
        release(m);
        if (!ok)
            return JSValue::exception();
        if (present && length > maxLength)
            return vm.throwError(ErrorCode::RangeError, "Invalid array buffer max length");
    }
    JSObject* o = vm.newArrayBuffer(std::size_t(length), maxLength < 0 ? 0 : std::size_t(maxLength), maxLength >= 0);
    if (!o)
        return JSValue::exception();
    o->setProto(vm, asObject(self)->proto(vm));
    static_cast<BufferState*>(o->rare()->ext)->shared = shared;
    return toJSValue(o);
}

/// The state of a buffer of the kind a method belongs to (`shared`), or null with a TypeError.
BufferState* bufferFor(VM& vm, JSValue self, bool shared, const char* what)
{
    BufferState* b = bufferOf(self);
    if (!b || b->shared != shared) {
        typeError(vm, std::string("Method ") + (shared ? "SharedArrayBuffer" : "ArrayBuffer") + ".prototype" + what +
                          " called on incompatible receiver");
        return nullptr;
    }
    return b;
}

NATIVE(arrayBufferGetter)
{
    UNUSED();
    const auto which = reinterpret_cast<std::intptr_t>(data);
    BufferState* b = bufferFor(vm, self, (which & 16) != 0, " getter");
    if (!b)
        return JSValue::exception();
    switch (which & 15) {
        case 0: return JSValue::boxNumber(double(b->size));
        case 1: return JSValue::boolean(b->resizable);
        case 3: return JSValue::boolean(b->detached);
        default: return JSValue::boxNumber(double(b->resizable ? b->capacity : b->size));
    }
}

NATIVE(arrayBufferResize)
{
    UNUSED();
    const bool shared = data != nullptr;  // grow
    BufferState* b = bufferFor(vm, self, shared, shared ? ".grow" : ".resize");
    if (!b)
        return JSValue::exception();
    if (!b->resizable)
        return typeError(vm, "Method ArrayBuffer.prototype.resize called on incompatible receiver");
    double length;
    if (!toIndex(vm, arg(args, argc, 0), "array buffer length", &length))
        return JSValue::exception();
    if (b->detached)
        return typeError(vm, "Cannot perform ArrayBuffer.prototype.resize on a detached ArrayBuffer");
    if (length > double(b->capacity) || (shared && length < double(b->size)))
        return vm.throwError(ErrorCode::RangeError, "ArrayBuffer.prototype.resize: Invalid length parameter");
    const std::size_t n = std::size_t(length);
    if (n < b->size)
        std::memset(b->data + n, 0, b->size - n);
    b->size = n;
    b->refreshViews();
    vm.epochMoved();  // optimized code may know the array of a global
    return JSValue::undefined();
}

/// transfer (data 0) and transferToFixedLength (1): the contents move to a new buffer and this one is detached.
NATIVE(arrayBufferTransfer)
{
    UNUSED();
    BufferState* b = bufferFor(vm, self, false, ".transfer");
    if (!b)
        return JSValue::exception();
    if (b->detached)
        return typeError(vm, "Cannot perform ArrayBuffer.prototype.transfer on a detached ArrayBuffer");
    double length = double(b->size);
    if (!arg(args, argc, 0).isUndefined() && !toIndex(vm, args[0], "array buffer length", &length))
        return JSValue::exception();
    const bool keepResizable = b->resizable && reinterpret_cast<std::intptr_t>(data) == 0;
    JSObject* o = vm.newArrayBuffer(std::size_t(length), keepResizable ? b->capacity : 0, keepResizable);
    if (!o)
        return JSValue::exception();
    auto* fresh = static_cast<BufferState*>(o->rare()->ext);
    std::memcpy(fresh->data, b->data, std::min(std::size_t(length), b->size));
    std::free(b->data);
    b->data = nullptr;
    b->size = 0;
    b->capacity = 0;
    b->detached = true;
    b->refreshViews();
    vm.epochMoved();  // optimized code may know the array of a global
    return toJSValue(o);
}

NATIVE(arrayBufferSlice)
{
    UNUSED();
    const bool shared = data != nullptr;
    BufferState* b = bufferFor(vm, self, shared, ".slice");
    if (!b)
        return JSValue::exception();
    const double size = double(b->size);
    double from, to;
    if (!relativeArg(vm, arg(args, argc, 0), size, 0, &from) || !relativeArg(vm, arg(args, argc, 1), size, size, &to))
        return JSValue::exception();
    const double n = std::max(to - from, 0.0);
    Handle keep = Handle::retained(self);
    Handle ctor = Handle::adopt(speciesConstructor(vm, self, vm.typedCtors[shared ? kSharedBufferSlot : kBufferSlot]));
    if (ctor.get().isException())
        return ctor.get();
    const JSValue lengthArg = JSValue::boxNumber(n);
    Handle made = Handle::adopt(vm.construct(ctor.get(), &lengthArg, 1));
    if (made.get().isException())
        return made.get();
    BufferState* nb = bufferOf(made.get());
    if (!nb || nb->shared != shared || nb->detached)
        return typeError(vm, "the species constructor did not make a usable buffer");
    if (made.get().bits == self.bits)
        return typeError(vm, "the species constructor returned the buffer being sliced");
    if (double(nb->size) < n)
        return typeError(vm, "the species constructor made a buffer that is too small");
    if (b->detached)
        return typeError(vm, "the buffer has been detached");
    if (from < double(b->size)) {
        const double count = std::min(n, double(b->size) - from);
        if (count > 0)
            std::memcpy(nb->data, b->data + std::size_t(from), std::size_t(count));
    }
    return made.take();
}

NATIVE(arrayBufferIsView)
{
    UNUSED();
    const JSValue v = arg(args, argc, 0);
    return JSValue::boolean(asTyped(v) || (isObject(v) && asObject(v)->cls() == ObjClass::DataView));
}

// ------------------------------------------------------------- typed arrays ---

NATIVE(typedCtor)
{
    UNUSED();
    const ElemKind kind = ElemKind(reinterpret_cast<std::intptr_t>(data) - 1);
    const std::uint32_t size = kElemSize[std::size_t(kind)];
    if (vm.newTarget.isUndefined() || !isObject(self))
        return typeError(vm, std::string("Constructor ") + kNames[std::size_t(kind)] + " requires 'new'");
    JSObject* proto = asObject(self)->proto(vm);
    const JSValue a0 = arg(args, argc, 0);
    if (!isObject(a0)) {
        double length;
        if (!toIndex(vm, a0, "typed array length", &length))
            return JSValue::exception();
        if (length * size > double(std::size_t(1) << 31))
            return vm.throwError(ErrorCode::RangeError, "Invalid typed array length: " + std::to_string(std::uint64_t(length)));
        JSTypedArray* t = makeTyped(vm, kind, proto, std::uint32_t(length));
        return t ? toJSValue(t) : JSValue::exception();
    }
    if (BufferState* b = bufferOf(a0)) {
        double offset, length = -1;
        if (!toIndex(vm, arg(args, argc, 1), "offset", &offset))
            return JSValue::exception();
        if (std::fmod(offset, size) != 0)
            return vm.throwError(ErrorCode::RangeError, std::string("start offset of ") + kNames[std::size_t(kind)] +
                                                              " should be a multiple of " + std::to_string(size));
        if (!arg(args, argc, 2).isUndefined() && !toIndex(vm, args[2], "typed array length", &length))
            return JSValue::exception();
        double count;
        const bool follows = length < 0 && b->resizable;
        if (length < 0) {
            if (!b->resizable && std::fmod(double(b->size), size) != 0)
                return vm.throwError(ErrorCode::RangeError, std::string("byte length of ") + kNames[std::size_t(kind)] +
                                                                  " should be a multiple of " + std::to_string(size));
            if (offset > double(b->size))
                return vm.throwError(ErrorCode::RangeError, "Start offset " + std::to_string(std::uint64_t(offset)) + " is outside the bounds of the buffer");
            count = (double(b->size) - offset) / size;
        } else {
            if (offset + length * size > double(b->size))
                return vm.throwError(ErrorCode::RangeError, "Invalid typed array length: " + std::to_string(std::uint64_t(length)));
            count = length;
        }
        JSTypedArray* made = vm.newTypedArray(proto, kind, asObject(a0), std::uint32_t(offset), std::uint32_t(count));
        made->tracking = follows;
        return toJSValue(made);
    }
    Vec<Handle> items;
    if (JSTypedArray* src = asTyped(a0)) {
        if (isBigKind(src->kind) != isBigKind(kind))
            return typeError(vm, "Cannot mix BigInt and other types, use explicit conversions");
        JSTypedArray* t = makeTyped(vm, kind, proto, src->length);
        if (!t)
            return JSValue::exception();
        Handle keep = Handle::adopt(toJSValue(t));
        for (std::uint32_t i = 0; i < src->length; ++i) {
            JSValue v = typedGet(vm, src, i);
            const bool ok = typedSet(vm, t, double(i), v);
            release(v);
            if (!ok)
                return JSValue::exception();
        }
        return keep.take();
    }
    JSValue method = getProperty(vm, a0, vm.atoms.symIterator);
    if (method.isException())
        return method;
    const bool iterable = isFunction(method);
    release(method);
    if (iterable) {
        if (!forEachIterated(vm, a0, [&](JSValue v) {
                items.push_back(Handle::retained(v));
                return Step::Continue;
            }))
            return JSValue::exception();
    } else {
        JSValue len = getProperty(vm, a0, vm.atoms.length);
        if (len.isException())
            return len;
        double n = 0;
        const bool ok = toNumber(vm, len, &n);
        release(len);
        if (!ok)
            return JSValue::exception();
        n = n != n ? 0 : std::max(0.0, std::trunc(n));
        if (n > double(std::size_t(1) << 31))
            return vm.throwError(ErrorCode::RangeError, "Invalid typed array length");
        for (std::uint32_t i = 0; i < std::uint32_t(n); ++i) {
            JSValue v = getIndex(vm, a0, i);
            if (v.isException())
                return v;
            items.push_back(Handle::adopt(v));
        }
    }
    JSTypedArray* t = makeTyped(vm, kind, proto, std::uint32_t(items.size()));
    if (!t)
        return JSValue::exception();
    Handle keep = Handle::adopt(toJSValue(t));
    for (std::size_t i = 0; i < items.size(); ++i)
        if (!typedSet(vm, t, double(i), items[i].get()))
            return JSValue::exception();
    return keep.take();
}

NATIVE(typedGetter)
{
    UNUSED();
    JSTypedArray* t = asTyped(self);
    const int which = int(reinterpret_cast<std::intptr_t>(data));  // 0 length, 1 byteLength, 2 byteOffset, 3 buffer, 4 tag
    if (which == 4) {
        if (!t)
            return JSValue::undefined();
        return makeString(vm, std::string_view(kNames[std::size_t(t->kind)]));
    }
    if (!t)
        return typeError(vm, "not a typed array");
    switch (which) {
        case 0: return JSValue::boxNumber(double(t->length));
        case 1: return JSValue::boxNumber(double(t->length) * kElemSize[std::size_t(t->kind)]);
        case 2: return JSValue::boxNumber(outOfBounds(t) ? 0.0 : double(t->byteOffset));
        default: retain(t->buffer); return t->buffer;
    }
}

// ----------------------------------------------------------------- DataView ---


/// The view as the buffer is now: false when it no longer fits (or the buffer is gone).
bool viewBounds(const ViewState* v, std::size_t* length)
{
    const BufferState* b = bufferOf(v->buffer);
    if (!b || b->detached)
        return false;
    if (v->tracking) {
        if (v->offset > b->size)
            return false;
        *length = b->size - v->offset;
        return true;
    }
    if (std::size_t(v->offset) + v->length > b->size)
        return false;
    *length = v->length;
    return true;
}

ViewState* viewOf(JSValue v)
{
    if (isObject(v) && asObject(v)->cls() == ObjClass::DataView)
        if (Rare* r = asObject(v)->rare())
            return static_cast<ViewState*>(r->ext);
    return nullptr;
}

NATIVE(dataViewCtor)
{
    UNUSED();
    if (vm.newTarget.isUndefined() || !isObject(self))
        return typeError(vm, "Constructor DataView requires 'new'");
    const JSValue buf = arg(args, argc, 0);
    BufferState* b = bufferOf(buf);
    if (!b)
        return typeError(vm, "First argument to DataView constructor must be an ArrayBuffer");
    double offset, length = -1;
    if (!toIndex(vm, arg(args, argc, 1), "DataView offset", &offset))
        return JSValue::exception();
    if (b->detached)
        return typeError(vm, "the buffer has been detached");
    if (offset > double(b->size))
        return vm.throwError(ErrorCode::RangeError, "Start offset " + std::to_string(std::uint64_t(offset)) + " is outside the bounds of the buffer");
    bool follows = false;
    if (arg(args, argc, 2).isUndefined()) {
        if (b->resizable)
            follows = true;
        length = double(b->size) - offset;
    } else {
        if (!toIndex(vm, args[2], "DataView length", &length))
            return JSValue::exception();
        if (offset + length > double(b->size))
            return vm.throwError(ErrorCode::RangeError, "Invalid DataView length " + std::to_string(std::uint64_t(length)));
    }
    auto* state = new ViewState();
    retain(buf);
    state->buffer = buf;
    state->offset = std::uint32_t(offset);
    state->length = std::uint32_t(length);
    state->tracking = follows;
    JSObject* o = asObject(self);
    o->h.cls = std::uint8_t(ObjClass::DataView);
    delete o->rareOf(vm).ext;
    o->rareOf(vm).ext = state;
    retain(self);
    return self;
}

}  // namespace

NATIVE(dataViewGetter)
{
    UNUSED();
    ViewState* v = viewOf(self);
    if (!v)
        return typeError(vm, "not a DataView");
    const auto which = reinterpret_cast<std::intptr_t>(data);
    if (which == 0) {
        retain(v->buffer);
        return v->buffer;
    }
    std::size_t length = 0;
    if (!viewBounds(v, &length))
        return typeError(vm, "the DataView is out of bounds");
    return JSValue::boxNumber(which == 1 ? double(length) : double(v->offset));
}

namespace {

/// Reverses `n` bytes in place: a DataView access in the other byte order.
void reverseBytes(std::uint8_t* bytes, std::size_t n) noexcept
{
    for (std::size_t lo = 0, hi = n; lo + 1 < hi; ++lo, --hi) {
        const std::uint8_t t = bytes[lo];
        bytes[lo] = bytes[hi - 1];
        bytes[hi - 1] = t;
    }
}

NATIVE(dataViewAccess)
{
    UNUSED();
    ViewState* v = viewOf(self);
    if (!v)
        return typeError(vm, "not a DataView");
    const auto bits = reinterpret_cast<std::intptr_t>(data);
    const ElemKind kind = ElemKind(bits & 255);
    const bool isSet = (bits >> 8) & 1;
    const std::uint32_t size = kElemSize[std::size_t(kind)];
    double index;
    if (!toIndex(vm, arg(args, argc, 0), "DataView offset", &index))
        return JSValue::exception();
    JSValue value = JSValue::undefined();
    double number = 0;
    std::uint64_t big = 0;
    if (isSet) {
        if (isBigKind(kind)) {
            JSValue prim = toPrimitive(vm, arg(args, argc, 1), false);
            if (prim.isException())
                return prim;
            if (!isBigInt(prim)) {
                release(prim);
                return typeError(vm, "Cannot convert " + describe(vm, arg(args, argc, 1)) + " to a BigInt");
            }
            big = bigintLow64(prim);
            release(prim);
        } else if (!toNumber(vm, arg(args, argc, 1), &number)) {
            return JSValue::exception();
        }
    }
    const bool little = toBoolean(arg(args, argc, isSet ? 2 : 1));
    std::size_t viewLength = 0;
    if (!viewBounds(v, &viewLength))
        return typeError(vm, "the DataView is out of bounds");
    if (index + size > double(viewLength))
        return vm.throwError(ErrorCode::RangeError, "Offset is outside the bounds of the DataView");
    BufferState* b = bufferOf(v->buffer);
    std::uint8_t* p = b->data + v->offset + std::size_t(index);
    std::uint8_t tmp[8];
    if (isSet) {
        if (isBigKind(kind))
            std::memcpy(tmp, &big, 8);
        else
            storeNumber(kind, tmp, number);
        if (!little)
            reverseBytes(tmp, size);
        std::memcpy(p, tmp, size);
        return JSValue::undefined();
    }
    std::memcpy(tmp, p, size);
    if (!little)
        reverseBytes(tmp, size);
    if (isBigKind(kind)) {
        std::int64_t raw;
        std::memcpy(&raw, tmp, 8);
        return bigintFromInt64(vm, raw, kind == ElemKind::BigUint64);
    }
    value = loadNumber(kind, tmp);
    return value;
}


// ---- Uint8Array and text: base64 and hex

const char kBase64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
const char kBase64Url[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

struct Base64Options
{
    bool url = false;
    int lastChunk = 0;  // 0 loose, 1 strict, 2 stop-before-partial
};

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


JSTypedArray* thisBytes(VM& vm, JSValue self)
{
    JSTypedArray* t = asTyped(self);
    if (!t || t->kind != ElemKind::Uint8) {
        typeError(vm, "not a Uint8Array");
        return nullptr;
    }
    return t;
}

/// The options of toBase64 and fromBase64; false with an exception pending.
bool base64Options(VM& vm, JSValue options, bool reading, Base64Options* out, bool* omitPadding)
{
    *omitPadding = false;
    if (options.isUndefined())
        return true;
    if (!isObject(options)) {
        typeError(vm, "the options are not an object");
        return false;
    }
    auto field = [&](const char* name, JSValue* v) {
        Key key = vm.internKey(std::string_view(name));
        *v = getProperty(vm, options, key);
        detail::decRef(key);
        return !v->isException();
    };
    JSValue alphabet;
    if (!field("alphabet", &alphabet))
        return false;
    Handle keepAlphabet = Handle::adopt(alphabet);
    if (!alphabet.isUndefined()) {
        if (!isString(alphabet)) {
            typeError(vm, "alphabet is not a string");
            return false;
        }
        const StringView a = asString(alphabet)->text();
        if (a == StringView(u"base64url")) {
            out->url = true;
        } else if (a != StringView(u"base64")) {
            typeError(vm, "alphabet is not one of the two there are");
            return false;
        }
    }
    if (reading) {
        JSValue last;
        if (!field("lastChunkHandling", &last))
            return false;
        Handle keepLast = Handle::adopt(last);
        if (!last.isUndefined()) {
            if (!isString(last)) {
                typeError(vm, "lastChunkHandling is not a string");
                return false;
            }
            const StringView l = asString(last)->text();
            if (l == StringView(u"strict"))
                out->lastChunk = 1;
            else if (l == StringView(u"stop-before-partial"))
                out->lastChunk = 2;
            else if (l != StringView(u"loose")) {
                typeError(vm, "lastChunkHandling is not one of the three there are");
                return false;
            }
        }
    } else {
        JSValue omit;
        if (!field("omitPadding", &omit))
            return false;
        *omitPadding = toBoolean(omit);
        release(omit);
    }
    return true;
}

NATIVE(bytesToBase64)
{
    UNUSED();
    JSTypedArray* t = thisBytes(vm, self);
    if (!t)
        return JSValue::exception();
    Base64Options options;
    bool omit;
    if (!base64Options(vm, arg(args, argc, 0), false, &options, &omit))
        return JSValue::exception();
    const char* table = options.url ? kBase64Url : kBase64;
    String out;
    std::size_t i = 0;
    const std::size_t n = t->length;
    const std::uint8_t* b = t->data;
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
        if (!omit)
            out += u"==";
    } else if (n - i == 2) {
        const std::uint32_t bits = (std::uint32_t(b[i]) << 16) | (std::uint32_t(b[i + 1]) << 8);
        out.push_back(Char(table[bits >> 18]));
        out.push_back(Char(table[(bits >> 12) & 63]));
        out.push_back(Char(table[(bits >> 6) & 63]));
        if (!omit)
            out.push_back(u'=');
    }
    return makeString(vm, StringView(out));
}

NATIVE(bytesToHex)
{
    UNUSED();
    JSTypedArray* t = thisBytes(vm, self);
    if (!t)
        return JSValue::exception();
    static const char kHex[] = "0123456789abcdef";
    String out;
    out.reserve(std::size_t(t->length) * 2);
    for (std::size_t i = 0; i < t->length; ++i) {
        out.push_back(Char(kHex[t->data[i] >> 4]));
        out.push_back(Char(kHex[t->data[i] & 15]));
    }
    return makeString(vm, StringView(out));
}

JSValue readWritten(VM& vm, std::size_t read, std::size_t written)
{
    JSObject* result = vm.newObject(vm.protos.object);
    value(vm, result, "read", JSValue::boxNumber(double(read)), attr::kDefault);
    value(vm, result, "written", JSValue::boxNumber(double(written)), attr::kDefault);
    return toJSValue(result);
}

/// fromBase64 (data 0), setFromBase64 (1).
NATIVE(bytesFromBase64)
{
    UNUSED();
    const bool into = data != nullptr;
    JSTypedArray* t = nullptr;
    if (into) {
        t = thisBytes(vm, self);
        if (!t)
            return JSValue::exception();
    }
    if (!isString(arg(args, argc, 0)))
        return typeError(vm, into ? "setFromBase64 takes a string" : "fromBase64 takes a string");
    Base64Options options;
    bool omit;
    if (!base64Options(vm, arg(args, argc, 1), true, &options, &omit))
        return JSValue::exception();
    const Decoded decoded = decodeBase64(asString(args[0])->text(), options, into ? t->length : std::size_t(-1));
    if (into) {
        const std::size_t written = std::min<std::size_t>(decoded.bytes.size(), t->length);
        if (written)
            std::memcpy(t->data, decoded.bytes.data(), written);
        if (decoded.bad)
            return vm.throwError(ErrorCode::SyntaxError, "not valid base64");
        return readWritten(vm, decoded.read, written);
    }
    if (decoded.bad)
        return vm.throwError(ErrorCode::SyntaxError, "not valid base64");
    JSTypedArray* out = makeTyped(vm, ElemKind::Uint8, nullptr, std::uint32_t(decoded.bytes.size()));
    if (!out)
        return JSValue::exception();
    if (!decoded.bytes.empty())
        std::memcpy(out->data, decoded.bytes.data(), decoded.bytes.size());
    return toJSValue(out);
}

/// fromHex (data 0), setFromHex (1).
NATIVE(bytesFromHex)
{
    UNUSED();
    const bool into = data != nullptr;
    JSTypedArray* t = nullptr;
    if (into) {
        t = thisBytes(vm, self);
        if (!t)
            return JSValue::exception();
    }
    if (!isString(arg(args, argc, 0)))
        return typeError(vm, into ? "setFromHex takes a string" : "fromHex takes a string");
    const StringView s = asString(args[0])->text();
    if (s.size() % 2)
        return vm.throwError(ErrorCode::SyntaxError, "an odd number of hex digits");
    const std::size_t room = into ? t->length : s.size() / 2;
    JSTypedArray* out = into ? t : makeTyped(vm, ElemKind::Uint8, nullptr, std::uint32_t(room));
    if (!out)
        return JSValue::exception();
    Handle keep = into ? Handle() : Handle::adopt(toJSValue(out));
    std::size_t written = 0, read = 0;
    while (read + 1 < s.size() && written < room) {
        const int a = hexDigit(s[read]), b = hexDigit(s[read + 1]);
        if (a < 0 || b < 0)
            return vm.throwError(ErrorCode::SyntaxError, "not a hex digit");
        out->data[written++] = std::uint8_t(a * 16 + b);
        read += 2;
    }
    if (into)
        return readWritten(vm, read, written);
    return keep.take();
}

}  // namespace

JSTypedArray* typedArrayOf(JSValue v) { return asTyped(v); }

bool typedArrayOutOfBounds(const JSTypedArray* t) { return outOfBounds(t); }

JSTypedArray* newTypedArrayOf(VM& vm, ElemKind kind, std::uint32_t length) { return makeTyped(vm, kind, nullptr, length); }

void installTypedArrays(VM& vm)
{
    JSObject* objectProto = vm.protos.object;

    // ArrayBuffer.
    JSObject* abProto = vm.newObject(objectProto);
    vm.setIntrinsic(&vm.protos.arrayBuffer, abProto);
    release(toJSValue(abProto));
    JSFunction* abCtor = constructor(vm, "ArrayBuffer", arrayBufferCtor, 1, abProto);
    vm.typedCtors[kBufferSlot] = toJSValue(abCtor);
    retain(vm.typedCtors[kBufferSlot]);
    method(vm, abCtor, "isView", arrayBufferIsView, 1);
    auto getter = [&](JSObject* target, const char* name, FastNative fn, void* data) {
        Key key = vm.internKey(std::string_view(name));
        Key fnName = vm.internKey(std::string_view(std::string("get ") + name));
        JSFunction* g = vm.newNativeFunction(vm.protos.function, fn, data, fnName, 0);
        Handle keep = Handle::adopt(toJSValue(g));
        defineAccessor(vm, target, key, toJSValue(g), JSValue::undefined(), attr::kConfigurable);
        detail::decRef(key);
        detail::decRef(fnName);
    };
    {
        JSObject* sabProto = vm.newObject(objectProto);
        vm.setIntrinsic(&vm.protos.sharedArrayBuffer, sabProto);
        release(toJSValue(sabProto));
        JSFunction* sabCtor = constructor(vm, "SharedArrayBuffer", arrayBufferCtor, 1, sabProto);
        sabCtor->rareOf(vm).nativeData = reinterpret_cast<void*>(1);
        vm.typedCtors[kSharedBufferSlot] = toJSValue(sabCtor);
        retain(vm.typedCtors[kSharedBufferSlot]);
        getter(sabProto, "byteLength", arrayBufferGetter, reinterpret_cast<void*>(16));
        getter(sabProto, "growable", arrayBufferGetter, reinterpret_cast<void*>(17));
        getter(sabProto, "maxByteLength", arrayBufferGetter, reinterpret_cast<void*>(18));
        method(vm, sabProto, "grow", arrayBufferResize, 1, reinterpret_cast<void*>(1));
        method(vm, sabProto, "slice", arrayBufferSlice, 2, reinterpret_cast<void*>(1));
        sabProto->putOwn(vm, vm.atoms.symToStringTag, makeString(vm, std::string_view("SharedArrayBuffer")), attr::kConfigurable);
    }
    getter(abProto, "byteLength", arrayBufferGetter, reinterpret_cast<void*>(0));
    getter(abProto, "resizable", arrayBufferGetter, reinterpret_cast<void*>(1));
    getter(abProto, "maxByteLength", arrayBufferGetter, reinterpret_cast<void*>(2));
    getter(abProto, "detached", arrayBufferGetter, reinterpret_cast<void*>(3));
    method(vm, abProto, "resize", arrayBufferResize, 1);
    method(vm, abProto, "transfer", arrayBufferTransfer, 0, reinterpret_cast<void*>(0));
    method(vm, abProto, "transferToFixedLength", arrayBufferTransfer, 0, reinterpret_cast<void*>(1));
    method(vm, abProto, "slice", arrayBufferSlice, 2);
    abProto->putOwn(vm, vm.atoms.symToStringTag, makeString(vm, std::string_view("ArrayBuffer")), attr::kConfigurable);

    // %TypedArray%.
    JSObject* taProto = vm.newObject(objectProto);
    JSFunction* taCtor = constructor(vm, "TypedArray", [](VM& vm2, JSValue, const JSValue*, std::uint32_t, void*) -> JSValue {
        return typeError(vm2, "Abstract class TypedArray not directly constructable");
    }, 0, taProto);
    {
        // Not a global.
        Key key = vm.internKey(std::string_view("TypedArray"));
        vm.global->deleteOwn(vm, key);
        detail::decRef(key);
    }
    getter(taProto, "length", typedGetter, reinterpret_cast<void*>(0));
    getter(taProto, "byteLength", typedGetter, reinterpret_cast<void*>(1));
    getter(taProto, "byteOffset", typedGetter, reinterpret_cast<void*>(2));
    getter(taProto, "buffer", typedGetter, reinterpret_cast<void*>(3));
    {
        Key fnName = vm.internKey(std::string_view("get [Symbol.toStringTag]"));
        JSFunction* g = vm.newNativeFunction(vm.protos.function, typedGetter, reinterpret_cast<void*>(4), fnName, 0);
        Handle keep = Handle::adopt(toJSValue(g));
        defineAccessor(vm, taProto, vm.atoms.symToStringTag, toJSValue(g), JSValue::undefined(), attr::kConfigurable);
        detail::decRef(fnName);
    }
    installTypedArrayMethods(vm, taProto, taCtor);
    for (int k = 0; k < int(ElemKind::Count); ++k) {
        JSObject* proto = vm.newObject(taProto);
        vm.protos.typedArrays[k] = proto;  // keeps the creation count
        retain(toJSValue(proto));
        JSFunction* ctor = constructor(vm, kNames[k], typedCtor, 3, proto);
        ctor->rareOf(vm).nativeData = reinterpret_cast<void*>(std::intptr_t(k + 1));
        ctor->setProto(vm, taCtor);
        vm.typedCtors[k] = toJSValue(ctor);
        retain(vm.typedCtors[k]);
        value(vm, ctor, "BYTES_PER_ELEMENT", JSValue::int32(std::int32_t(kElemSize[k])), 0);
        value(vm, proto, "BYTES_PER_ELEMENT", JSValue::int32(std::int32_t(kElemSize[k])), 0);
        release(toJSValue(proto));
    }
    release(toJSValue(taProto));

    // Uint8Array text.
    {
        JSObject* u8 = vm.protos.typedArrays[std::size_t(ElemKind::Uint8)];
        method(vm, u8, "toBase64", bytesToBase64, 0);
        method(vm, u8, "toHex", bytesToHex, 0);
        method(vm, u8, "setFromBase64", bytesFromBase64, 1, reinterpret_cast<void*>(1));
        method(vm, u8, "setFromHex", bytesFromHex, 1, reinterpret_cast<void*>(1));
        Key ck = vm.internKey(std::string_view("constructor"));
        JSValue* ctorSlot = u8->findOwn(vm, ck);
        detail::decRef(ck);
        if (ctorSlot && isObject(*ctorSlot)) {
            JSObject* u8ctor = asObject(*ctorSlot);
            method(vm, u8ctor, "fromBase64", bytesFromBase64, 1);
            method(vm, u8ctor, "fromHex", bytesFromHex, 1);
        }
    }

    // DataView.
    JSObject* dvProto = vm.newObject(objectProto);
    JSFunction* dvCtor = constructor(vm, "DataView", dataViewCtor, 1, dvProto);
    (void)dvCtor;
    getter(dvProto, "buffer", dataViewGetter, reinterpret_cast<void*>(0));
    getter(dvProto, "byteLength", dataViewGetter, reinterpret_cast<void*>(1));
    getter(dvProto, "byteOffset", dataViewGetter, reinterpret_cast<void*>(2));
    static const char* const viewNames[] = {"Int8", "Uint8", nullptr, "Int16", "Uint16", "Int32", "Uint32", "Float32", "Float64", "BigInt64", "BigUint64", "Float16"};
    for (int k = 0; k < int(ElemKind::Count); ++k) {
        if (!viewNames[k])
            continue;
        method(vm, dvProto, (std::string("get") + viewNames[k]).c_str(), dataViewAccess, 1, reinterpret_cast<void*>(std::intptr_t(k)));
        method(vm, dvProto, (std::string("set") + viewNames[k]).c_str(), dataViewAccess, 2, reinterpret_cast<void*>(std::intptr_t(k | 256)));
    }
    dvProto->putOwn(vm, vm.atoms.symToStringTag, makeString(vm, std::string_view("DataView")), attr::kConfigurable);
    release(toJSValue(dvProto));
}

}  // namespace bi

FastNative dataViewAccessNative() noexcept
{
    return bi::dataViewAccess;
}

FastNative dataViewGetterNative() noexcept
{
    return bi::dataViewGetter;
}

}  // namespace vm
}  // namespace internal
}  // namespace script
