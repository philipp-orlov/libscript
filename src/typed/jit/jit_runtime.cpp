// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "script/internal/typed/jit_runtime.hpp"

#include <cmath>
#include <cstring>
#include <string>

#include "script/internal/stack_guard.hpp"
#include "script/internal/typed/jit.hpp"
#include "script/internal/typed/jit_call.hpp"
#include "script/internal/typed/runtime.hpp"
#include "script/internal/typed/types.hpp"
#include "script/internal/typed/vm.hpp"

namespace script {
namespace internal {
namespace typed {

namespace {

/// The tag a reference has, from the cell it points at.
Type tagOfCell(const GCObject* cell) noexcept
{
    if (!cell)
        return Type::Null;
    switch (cell->kind) {
        case CellKind::String: return Type::String;
        case CellKind::Array: return Type::Array;
        case CellKind::Delegate: return Type::Delegate;
        default: return Type::Object;
    }
}

/// A reference as a `Value`, holding a count of its own.
Value valueOfCell(GCObject* cell) noexcept
{
    return cell ? detail::Access::make(tagOfCell(cell), cell) : Value();
}

Value valueOfBits(std::int64_t tag, std::uint64_t payload) noexcept
{
    switch (tag) {
        case -1: return valueOfCell(reinterpret_cast<GCObject*>(payload));
        case std::int64_t(Type::Bool): return Value((payload & 0xff) != 0);
        case std::int64_t(Type::Char): return Value(char16_t(payload));
        case std::int64_t(Type::Int): return Value(std::int32_t(payload));
        case std::int64_t(Type::Long): return Value(std::int64_t(payload));
        case std::int64_t(Type::Float): {
            float f = 0;
            const auto bits = std::uint32_t(payload);
            std::memcpy(&f, &bits, sizeof f);
            return Value(f);
        }
        case std::int64_t(Type::Double): {
            double d = 0;
            std::memcpy(&d, &payload, sizeof d);
            return Value(d);
        }
        case std::int64_t(Type::String):
        case std::int64_t(Type::Object):
        case std::int64_t(Type::Array):
        case std::int64_t(Type::Delegate):
            return payload ? detail::Access::make(Type(tag), reinterpret_cast<GCObject*>(payload)) : Value();
        default: return Value();
    }
}

/// Take the cell out of a value, with the value's count.
GCObject* releaseCell(Value& v) noexcept
{
    GCObject* cell = detail::Access::ptr(v);
    if (cell)
        detail::incRef(cell);
    v = Value();
    return cell;
}

/// A boxed number as the static type it is being unboxed to.
std::uint64_t bitsOfBoxed(const Value& v, const TypeInfo* to) noexcept
{
    TypeKind kind = to ? to->kind : TypeKind::Int;
    if (kind == TypeKind::Enum && to->underlying)
        kind = to->underlying->kind;
    if (kind == TypeKind::Bool)
        return v.toBool() ? 1 : 0;
    if (kind == TypeKind::Long || kind == TypeKind::ULong)
        return std::uint64_t(v.toLong());
    if (kind == TypeKind::Float) {
        const float f = float(v.toDouble());
        std::uint32_t bits = 0;
        std::memcpy(&bits, &f, sizeof bits);
        return bits;
    }
    if (kind == TypeKind::Double) {
        const double d = v.toDouble();
        std::uint64_t bits = 0;
        std::memcpy(&bits, &d, sizeof bits);
        return bits;
    }
    return std::uint64_t(std::uint32_t(v.toInt()));
}

void recordCurrent(Runtime& rt) noexcept
{
    rt.nativePending = std::current_exception();
    rt.nativeError = std::uint32_t(ErrorCode::Exception);
}

/// Run `f`, which may throw; a throw becomes the runtime's pending error.
template <class F>
auto guarded(Runtime& rt, const void* pc, F f) noexcept -> decltype(f())
{
    rt.nativeTopPc = pc;
    try {
        return f();
    } catch (...) {
        recordCurrent(rt);
        return decltype(f())();
    }
}

}  // namespace

extern "C" {

GCObject* scriptJitNewInstance(Runtime* rt, const TypeInfo* type) noexcept
{
    // The common case in one frame: a type whose fresh bytes are known, no collection due and no limit to check.
    ContextImpl& ctx = rt->ctx;
    if (type && !type->instanceImage.empty() && !ctx.options.memoryLimit && ctx.heapBytes < ctx.nextGcBytes &&
        ctx.liveObjects + ctx.liveStrings < ctx.nextGcCells) {
        const std::size_t bytes = type->instanceImage.size() * 8;
        void* memory = nullptr;
        try {
            memory = ctx.heap.allocate(bytes);
        } catch (...) {
            memory = nullptr;  // the long way below reports it
        }
        if (memory) {
            auto* out = static_cast<std::uint64_t*>(memory);
            const std::uint64_t* image = type->instanceImage.data();
            for (std::size_t i = 0; i < bytes / 8; i += 2) {
                out[i] = image[i];
                out[i + 1] = image[i + 1];
            }
            auto* cell = static_cast<GCObject*>(memory);
            ctx.linkKnown(cell, std::uint32_t(bytes));
            return cell;
        }
    }
    return guarded(*rt, __builtin_return_address(0), [&]() -> GCObject* { return rt->newInstance(type); });
}

GCObject* scriptJitNewArray(Runtime* rt, const TypeInfo* element, std::int32_t length) noexcept
{
    return guarded(*rt, __builtin_return_address(0), [&]() -> GCObject* {
        if (length < 0)
            throwError(*rt, ErrorCode::ArgumentOutOfRange, "the array length cannot be negative");
        return rt->newArray(element, std::uint32_t(length));
    });
}

GCObject* scriptJitNewBox(Runtime* rt, std::int64_t tag, std::uint64_t payload) noexcept
{
    return guarded(*rt, __builtin_return_address(0),
                   [&]() -> GCObject* { return rt->newBox(valueOfBits(tag, payload)); });
}

GCObject* scriptJitClone(Runtime* rt, GCObject* source) noexcept
{
    return guarded(*rt, __builtin_return_address(0), [&]() -> GCObject* {
        Value v = cloneValue(*rt, valueOfCell(source));
        return releaseCell(v);
    });
}

GCObject* scriptJitNewDelegate(Runtime* rt, const TypeInfo* type, const MethodInfo* target, GCObject* receiver,
                               std::int64_t captures, GCObject* box0, GCObject* box1, GCObject* box2,
                               GCObject* box3) noexcept
{
    return guarded(*rt, __builtin_return_address(0), [&]() -> GCObject* {
        CSDelegate* cell = rt->newCell<CSDelegate>();
        Value handle = detail::Access::adopt(Type::Delegate, cell);
        cell->type = type;
        cell->method = target;
        cell->target = valueOfCell(receiver);
        GCObject* const boxes[] = {box0, box1, box2, box3};
        for (std::int64_t i = 0; i < captures; ++i)
            cell->captured.emplace_back(static_cast<Box*>(boxes[i]));
        return releaseCell(handle);
    });
}

void scriptJitStoreTagged(Value* slot, std::int64_t tag, std::uint64_t payload) noexcept
{
    if (tag >= std::int64_t(Type::String)) {
        auto* cell = reinterpret_cast<GCObject*>(payload);
        *slot = cell ? detail::Access::adopt(Type(tag), cell) : Value();
        return;
    }
    *slot = valueOfBits(tag, payload);
}

std::int64_t scriptJitTagOf(GCObject* cell) noexcept
{
    return std::int64_t(tagOfCell(cell));
}

void scriptJitCast(Runtime* rt, std::int64_t tag, std::uint64_t payload, const TypeInfo* to) noexcept
{
    guarded(*rt, __builtin_return_address(0), [&]() -> int {
        castTo(*rt, valueOfBits(tag, payload), to);
        return 0;
    });
}

std::int32_t scriptJitIsInstance(Runtime* rt, std::int64_t tag, std::uint64_t payload, const TypeInfo* to) noexcept
{
    return isInstanceOf(*rt, valueOfBits(tag, payload), to) ? 1 : 0;
}

std::int32_t scriptJitValueEquals(std::int64_t tagA, std::uint64_t a, std::int64_t tagB, std::uint64_t b) noexcept
{
    return valueEquals(valueOfBits(tagA, a), valueOfBits(tagB, b)) ? 1 : 0;
}

const MethodInfo* scriptJitResolveInterface(Runtime* rt, GCObject* receiver, const MethodInfo* declared) noexcept
{
    return guarded(*rt, __builtin_return_address(0),
                   [&]() -> const MethodInfo* { return resolveInterface(*rt, valueOfCell(receiver), declared); });
}

std::int32_t scriptJitStringEquals(GCObject* a, GCObject* b) noexcept
{
    if (a == b)
        return 1;
    if (!a || !b)
        return 0;
    return static_cast<StringCell*>(a)->text() == static_cast<StringCell*>(b)->text() ? 1 : 0;
}

double scriptJitFmod(double a, double b) noexcept
{
    return std::fmod(a, b);
}

float scriptJitFmodf(float a, float b) noexcept
{
    return std::fmod(a, b);
}

std::int64_t scriptJitAddOvf(Runtime* rt, std::int64_t a, std::int64_t b) noexcept
{
    std::int64_t out = 0;
    if (__builtin_add_overflow(a, b, &out))
        scriptJitRaise(rt, std::uint32_t(ErrorCode::Overflow), 0);
    return out;
}

std::int64_t scriptJitSubOvf(Runtime* rt, std::int64_t a, std::int64_t b) noexcept
{
    std::int64_t out = 0;
    if (__builtin_sub_overflow(a, b, &out))
        scriptJitRaise(rt, std::uint32_t(ErrorCode::Overflow), 0);
    return out;
}

std::int64_t scriptJitMulOvf(Runtime* rt, std::int64_t a, std::int64_t b) noexcept
{
    std::int64_t out = 0;
    if (__builtin_mul_overflow(a, b, &out))
        scriptJitRaise(rt, std::uint32_t(ErrorCode::Overflow), 0);
    return out;
}

GCObject* scriptJitConcat(Runtime* rt, std::int64_t count, std::int64_t tag0, std::uint64_t payload0, std::int64_t tag1,
                          std::uint64_t payload1, std::int64_t tag2, std::uint64_t payload2) noexcept
{
    return guarded(*rt, __builtin_return_address(0), [&]() -> GCObject* {
        Value parts[3] = {valueOfBits(tag0, payload0), count > 1 ? valueOfBits(tag1, payload1) : Value(),
                          count > 2 ? valueOfBits(tag2, payload2) : Value()};
        Value joined = concatStrings(*rt, parts, std::size_t(count));
        return releaseCell(joined);
    });
}

GCObject* scriptJitToString(Runtime* rt, std::int64_t tag, std::uint64_t payload) noexcept
{
    return guarded(*rt, __builtin_return_address(0), [&]() -> GCObject* {
        Value text = rt->str(toStringValue(*rt, valueOfBits(tag, payload)));
        return releaseCell(text);
    });
}

GCObject* scriptJitToStringUnsigned(Runtime* rt, std::int64_t tag, std::uint64_t payload) noexcept
{
    return guarded(*rt, __builtin_return_address(0), [&]() -> GCObject* {
        const std::string digits = tag == std::int64_t(Type::Long) ? std::to_string(payload)
                                                                   : std::to_string(std::uint32_t(payload));
        Value text = rt->str(digits.c_str());
        return releaseCell(text);
    });
}

namespace {

/// A value as native code receives it: the payload, with a count of its own if it is a reference.
std::uint64_t ownedBits(const Value& v) noexcept
{
    if (GCObject* cell = detail::Access::ptr(v)) {
        detail::incRef(cell);
        return reinterpret_cast<std::uint64_t>(cell);
    }
    return std::uint64_t(detail::Access::i8(v));
}

}  // namespace

std::int32_t scriptJitListCount(Runtime*, CSList* list) noexcept
{
    return std::int32_t(list->items.size());
}

std::uint64_t scriptJitUnbox(Runtime* rt, std::int64_t tag, std::uint64_t payload, const TypeInfo* to) noexcept
{
    return guarded(*rt, __builtin_return_address(0), [&]() -> std::uint64_t {
        const Value v = valueOfBits(tag, payload);
        if (!v.isNumber() && !v.isBool() && !v.isChar())
            throwError(*rt, ErrorCode::InvalidCast, "the boxed value is not a '" + rt->types->describe(to) + "'");
        return bitsOfBoxed(v, to);
    });
}

std::uint64_t scriptJitListGet(Runtime* rt, CSList* list, std::int32_t index) noexcept
{
    if (index < 0 || std::size_t(index) >= list->items.size()) {
        return guarded(*rt, __builtin_return_address(0), [&]() -> std::uint64_t {
            throwError(*rt, ErrorCode::ArgumentOutOfRange, "index is out of range");
        });
    }
    return ownedBits(list->items[std::size_t(index)]);
}

void scriptJitListSet(Runtime* rt, CSList* list, std::int32_t index, std::int64_t tag, std::uint64_t payload) noexcept
{
    if (index < 0 || std::size_t(index) >= list->items.size()) {
        guarded(*rt, __builtin_return_address(0), [&]() -> int {
            throwError(*rt, ErrorCode::ArgumentOutOfRange, "index is out of range");
        });
        return;
    }
    list->items[std::size_t(index)] = valueOfBits(tag, payload);
}

void scriptJitListAdd(Runtime* rt, CSList* list, std::int64_t tag, std::uint64_t payload) noexcept
{
    guarded(*rt, __builtin_return_address(0), [&]() -> int {
        list->add(valueOfBits(tag, payload));
        return 0;
    });
}

std::int32_t scriptJitDictCount(Runtime*, CSDict* dict) noexcept
{
    return std::int32_t(dict->liveCount);
}

std::uint64_t scriptJitDictGet(Runtime* rt, CSDict* dict, std::int64_t keyTag, std::uint64_t key) noexcept
{
    const Value k = valueOfBits(keyTag, key);
    CSDict::Entry* entry = dict->find(k);
    if (!entry) {
        return guarded(*rt, __builtin_return_address(0), [&]() -> std::uint64_t {
            throwError(*rt, ErrorCode::KeyNotFound, "the given key was not present in the dictionary");
        });
    }
    return ownedBits(entry->value);
}

void scriptJitDictSet(Runtime* rt, CSDict* dict, std::int64_t keyTag, std::uint64_t key, std::int64_t valueTag,
                      std::uint64_t value) noexcept
{
    guarded(*rt, __builtin_return_address(0), [&]() -> int {
        dict->put(valueOfBits(keyTag, key), valueOfBits(valueTag, value));
        return 0;
    });
}

std::int32_t scriptJitDictContains(Runtime*, CSDict* dict, std::int64_t keyTag, std::uint64_t key) noexcept
{
    return dict->find(valueOfBits(keyTag, key)) ? 1 : 0;
}

void scriptJitBuilderAppendChar(Runtime* rt, CSBuilder* builder, std::uint64_t ch) noexcept
{
    guarded(*rt, __builtin_return_address(0), [&]() -> int {
        const std::size_t before = builder->buf.capacity();
        builder->buf.push_back(Char(ch));
        builder->chargeIfGrown(builder->buf, before);
        return 0;
    });
}

void scriptJitBuilderAppendString(Runtime* rt, CSBuilder* builder, GCObject* text) noexcept
{
    guarded(*rt, __builtin_return_address(0), [&]() -> int {
        const std::size_t before = builder->buf.capacity();
        if (text)
            builder->buf += static_cast<StringCell*>(text)->text();
        builder->chargeIfGrown(builder->buf, before);
        return 0;
    });
}

void scriptJitThrow(Runtime* rt, GCObject* exception) noexcept
{
    guarded(*rt, __builtin_return_address(0), [&]() -> int {
        if (!exception)
            throwError(*rt, ErrorCode::NullReference, "the thrown value is null");
        throwValue(*rt, valueOfCell(exception));
    });
}

void scriptJitRaise(Runtime* rt, std::uint32_t code, std::int32_t index) noexcept
{
    guarded(*rt, __builtin_return_address(0), [&]() -> int {
        const ErrorCode raised = ErrorCode(code);
        const char* message = "execution was interrupted";
        switch (raised) {
            case ErrorCode::NullReference: message = "object reference not set to an instance of an object"; break;
            case ErrorCode::IndexOutOfRange: message = "the index is outside the bounds of the array"; break;
            case ErrorCode::DivideByZero: message = "attempted to divide by zero"; break;
            case ErrorCode::Overflow: message = "the arithmetic operation overflowed"; break;
            case ErrorCode::StackOverflow: message = "the call stack went too deep"; break;
            default: break;
        }
        if (raised == ErrorCode::IndexOutOfRange)
            throwError(*rt, raised, "index " + std::to_string(index) + " is outside the bounds of the array");
        throwError(*rt, raised, message);
    });
}

void scriptJitCleanup(GCObject** cells, std::uint64_t count) noexcept
{
    for (std::uint64_t i = 0; i < count; ++i)
        detail::decRef(cells[i]);
}

namespace {

/// The exception behind the pending error, if it is one a handler may see. Null when it is not.
bool pendingValue(Runtime& rt, Value& out) noexcept
{
    if (!rt.nativePending)
        return false;
    try {
        std::rethrow_exception(rt.nativePending);
    } catch (const Exception& raised) {
        const ErrorCode code = raised.code();
        if (code == ErrorCode::Interrupted || code == ErrorCode::StackOverflow || code == ErrorCode::OutOfMemory)
            return false;
        out = raised.value();
        return true;
    } catch (const Error& raised) {
        if (raised.code() == ErrorCode::Interrupted || raised.code() == ErrorCode::StackOverflow ||
            raised.code() == ErrorCode::OutOfMemory)
            return false;
        try {
            out = rt.makeException(raised.code(), fromUtf8(raised.what()));
            return true;
        } catch (...) {
            return false;
        }
    } catch (...) {
        return false;
    }
}

}  // namespace

GCObject* scriptJitCatch(Runtime* rt, const TypeInfo* type) noexcept
{
    Value exception;
    if (rt->nativeError == 0 || !pendingValue(*rt, exception))
        return nullptr;
    if (type && !isInstanceOf(*rt, exception, type))
        return nullptr;
    rt->nativeError = 0;
    rt->nativePending = nullptr;
    return releaseCell(exception);
}

std::int32_t scriptJitFinallyEnter(Runtime* rt) noexcept
{
    Value exception;
    if (rt->nativeError == 0 || !pendingValue(*rt, exception))
        return 0;
    rt->finallyPending.push_back(std::move(rt->nativePending));
    rt->nativePending = nullptr;
    rt->nativeError = 0;
    return std::int32_t(rt->finallyPending.size());
}

void scriptJitFinallyRethrow(Runtime* rt, std::int64_t continuation) noexcept
{
    const std::size_t number = std::size_t(-std::int32_t(continuation));
    if (number == 0 || number > rt->finallyPending.size())
        return;
    rt->nativePending = std::move(rt->finallyPending[number - 1]);
    rt->nativeError = std::uint32_t(ErrorCode::Exception);
    // Anything after it was put aside by a `finally` that an exception then left.
    rt->finallyPending.resize(number - 1);
}

}  // extern "C"

#if !defined(SCRIPT_ARCH_ARM64) && !(defined(SCRIPT_ARCH_X64) && !defined(_WIN32))
// No back end on this machine, so nothing calls these.
extern "C" {
void scriptJitLazyStub() {}
void scriptJitLazyDelegateStub() {}
void scriptJitPollStub() {}
void scriptJitEnter(Runtime*, void*, const std::uint64_t*, const double*, const std::uint64_t*, std::uint64_t,
                    const void*, std::uint64_t*, double*)
{
}
}
#endif

// ---------------------------------------------------------- marshalling ---

namespace {

#if defined(SCRIPT_ARCH_X64)
constexpr std::size_t kIntRegs = 6;
#else
constexpr std::size_t kIntRegs = 8;
#endif
constexpr std::size_t kFpRegs = 8;

/// Assigns arguments to registers and stack words in the native convention.
class Abi
{
public:
    /// True and the word's place when it goes in a register of its class.
    enum class Where
    {
        Int,
        Fp,
        Stack,
    };
    struct Place
    {
        Where where;
        std::size_t index;
    };
    Place next(bool fp) noexcept
    {
        if (fp) {
            if (fp_ < kFpRegs)
                return {Where::Fp, fp_++};
        } else if (int_ < kIntRegs) {
            return {Where::Int, int_++};
        }
        return {Where::Stack, stack_++};
    }
    std::size_t stackWords() const noexcept { return stack_; }

private:
    std::size_t int_ = 0;
    std::size_t fp_ = 0;
    std::size_t stack_ = 0;
};

TypeKind kindOf(const TypeInfo* type) noexcept
{
    if (!type)
        return TypeKind::Object;
    if (type->kind == TypeKind::Enum)
        return type->underlying ? type->underlying->kind : TypeKind::Int;
    return type->kind;
}

/// A value of this static type travels as a tag and a payload.
bool isTaggedKind(const TypeInfo* type) noexcept
{
    const TypeKind kind = kindOf(type);
    return kind == TypeKind::Object || kind == TypeKind::Interface;
}

bool isByRef(const ParamInfo& param) noexcept
{
    return param.mode == ParamMode::Ref || param.mode == ParamMode::Out;
}

/// A `Value` as the bits the native convention carries.
std::uint64_t bitsOfValue(const Value& v, const TypeInfo* type, bool byRef) noexcept
{
    if (byRef)
        return reinterpret_cast<std::uint64_t>(detail::Access::ptr(v));
    const TypeKind kind = kindOf(type);
    switch (kind) {
        case TypeKind::Bool: return v.toBool() ? 1 : 0;
        case TypeKind::SByte:
        case TypeKind::Byte:
        case TypeKind::Short:
        case TypeKind::UShort:
        case TypeKind::Int:
        case TypeKind::UInt:
        case TypeKind::Char: return std::uint64_t(std::uint32_t(v.toInt()));
        case TypeKind::Long:
        case TypeKind::ULong: return std::uint64_t(v.toLong());
        case TypeKind::Float: {
            const float f = float(v.toDouble());
            std::uint32_t bits = 0;
            std::memcpy(&bits, &f, sizeof bits);
            return bits;
        }
        case TypeKind::Double: {
            const double d = v.toDouble();
            std::uint64_t bits = 0;
            std::memcpy(&bits, &d, sizeof bits);
            return bits;
        }
        default: return reinterpret_cast<std::uint64_t>(detail::Access::ptr(v));
    }
}

/// The reverse. A reference gets a count of its own.
Value valueOfArg(std::uint64_t bits, const TypeInfo* type, bool byRef) noexcept
{
    if (byRef)
        return bits ? detail::Access::make(Type::Object, reinterpret_cast<GCObject*>(bits)) : Value();
    const TypeKind kind = kindOf(type);
    switch (kind) {
        case TypeKind::Bool: return valueOfBits(std::int64_t(Type::Bool), bits);
        case TypeKind::Char: return valueOfBits(std::int64_t(Type::Char), bits);
        case TypeKind::SByte:
        case TypeKind::Byte:
        case TypeKind::Short:
        case TypeKind::UShort:
        case TypeKind::Int:
        case TypeKind::UInt: return valueOfBits(std::int64_t(Type::Int), bits);
        case TypeKind::Long:
        case TypeKind::ULong: return valueOfBits(std::int64_t(Type::Long), bits);
        case TypeKind::Float: return valueOfBits(std::int64_t(Type::Float), bits);
        case TypeKind::Double: return valueOfBits(std::int64_t(Type::Double), bits);
        default: return valueOfCell(reinterpret_cast<GCObject*>(bits));
    }
}

bool isFpParam(const TypeInfo* type, bool byRef) noexcept
{
    if (byRef)
        return false;
    const TypeKind kind = kindOf(type);
    return kind == TypeKind::Float || kind == TypeKind::Double;
}

/// A returned value, in the place native code expects it.
void writeResult(const Value& result, const TypeInfo* type, bool constructorResult, std::uint64_t* ints, double* fps)
{
    // `ints` here is the two result words.
    if (constructorResult) {
        GCObject* cell = detail::Access::ptr(result);
        if (cell)
            detail::incRef(cell);
        ints[0] = reinterpret_cast<std::uint64_t>(cell);
        return;
    }
    if (!type || type->kind == TypeKind::Void)
        return;
    if (isTaggedKind(type)) {
        GCObject* cell = detail::Access::ptr(result);
        if (cell)
            detail::incRef(cell);
        ints[0] = cell ? reinterpret_cast<std::uint64_t>(cell) : std::uint64_t(detail::Access::i8(result));
        ints[1] = std::uint64_t(std::uint32_t(result.type()));
        return;
    }
    const std::uint64_t bits = bitsOfValue(result, type, false);
    if (isFpParam(type, false)) {
        std::memcpy(&fps[0], &bits, sizeof bits);
        return;
    }
    if (GCObject* cell = detail::Access::ptr(result)) {
        if (!isValueKind(kindOf(type)))
            detail::incRef(cell);  // the native caller owns what it gets back
    }
    ints[0] = bits;
}

}  // namespace

extern "C" void* scriptJitBridge(Runtime* rt, MethodInfo* method, std::uint64_t* saved, std::uint64_t* stackArgs,
                                 CSDelegate* delegate, void* frame) noexcept
{
    rt->nativeTopFrame = frame;
    rt->nativeTopPc = nullptr;
    std::uint64_t* ints = saved;
    auto* fps = reinterpret_cast<double*>(saved + kIntRegs);
    const auto word = [&](const Abi::Place& place) {
        return place.where == Abi::Where::Int  ? ints[place.index]
               : place.where == Abi::Where::Fp ? saved[kIntRegs + place.index]
                                               : stackArgs[place.index];
    };
    const auto readArg = [&](Abi& abi, const TypeInfo* type, bool byRef) -> Value {
        if (!byRef && isTaggedKind(type)) {
            const std::uint64_t tag = word(abi.next(false));
            const std::uint64_t payload = word(abi.next(false));
            return valueOfBits(std::int64_t(tag), payload);
        }
        return valueOfArg(word(abi.next(isFpParam(type, byRef))), type, byRef);
    };

    // A call through a delegate is described by the delegate type; a multicast one runs every link.
    if (delegate) {
        // A delegate made from a lambda may carry no delegate type with a signature; the target has the same one.
        const MethodInfo* signature = delegate->type && delegate->type->invoke ? delegate->type->invoke : delegate->method;
        if (!signature) {
            scriptJitRaise(rt, std::uint32_t(ErrorCode::InvalidOperation), 0);
            return nullptr;
        }
        // A delegate that is about to be run often earns its target a native entry.
        const MethodInfo* first = delegate->method;
        if (first && !delegate->next && !first->isNative() && rt->jit && rt->jit->enabled() &&
            !first->code.load(std::memory_order_acquire) &&
            first->hotness.load(std::memory_order_relaxed) != MethodInfo::kNeverCompile) {
            MethodInfo& mutableFirst = const_cast<MethodInfo&>(*first);
            rt->jit->onHot(*rt, mutableFirst);
            if (mutableFirst.delegateEntry != jitLazyDelegateEntry())
                return mutableFirst.delegateEntry;
        }
        try {
            Abi abi;
            const std::size_t argc = signature->params.size();
            ContextImpl::ArgBuffer buffer(rt->ctx);
            std::vector<Value>& args = buffer.args;
            for (std::size_t i = 0; i < argc; ++i) {
                const ParamInfo& param = signature->params[i];
                args.push_back(readArg(abi, param.type, isByRef(param)));
            }
            Value self = detail::Access::make(Type::Delegate, delegate);
            const Value result = invokeDelegate(*rt, self, args.data(), std::uint32_t(argc));
            writeResult(result, signature->returnType, false, saved + kIntRegs + kFpRegs, fps);
        } catch (...) {
            recordCurrent(*rt);
        }
        return nullptr;
    }

    // The first call from native code to a method with a body is a good moment to compile it.
    if (!method->isNative() && !method->isAbstract() && rt->jit && rt->jit->enabled() &&
        !method->code.load(std::memory_order_acquire) &&
        method->hotness.load(std::memory_order_relaxed) != MethodInfo::kNeverCompile) {
        rt->jit->onHot(*rt, *method);
        if (method->fastEntry != jitLazyEntry())
            return method->fastEntry;
    }

    try {
        Abi abi;
        ContextImpl::ArgBuffer buffer(rt->ctx);
        std::vector<Value>& args = buffer.args;
        const bool nativeConstructor =
            method->isConstructor() && method->owner && method->owner->nativeKind != NativeKind::None;
        if (!method->isStatic()) {
            // The receiver of a method of a primitive is that primitive.
            const Abi::Place place = abi.next(isFpParam(method->owner, false));
            const std::uint64_t self = place.where == Abi::Where::Fp ? saved[kIntRegs + place.index] : ints[place.index];
            args.push_back(isTaggedKind(method->owner) ? valueOfCell(reinterpret_cast<GCObject*>(self))
                                                       : valueOfArg(self, method->owner, false));
        }
        for (const ParamInfo& param : method->params)
            args.push_back(readArg(abi, param.type, isByRef(param)));
        // `invoke` may run an interpreter that grows `args` of a nested level, never this one.
        const Value result = invoke(*rt, method, args.data(), std::uint32_t(args.size()));
        writeResult(result, method->returnType, nativeConstructor, saved + kIntRegs + kFpRegs, fps);
    } catch (...) {
        recordCurrent(*rt);
    }
    return nullptr;
}

// ------------------------------------------------------- calling from C++ ---

namespace {

/// How far native frames may take the stack. Native frames are far smaller than the interpreter's, so the depth limit a
/// context sets is honoured as a budget of stack, not a count.
constexpr std::size_t kNativeBytesPerCall = 256;

void armNativeRun(Runtime& rt, const std::uintptr_t here) noexcept
{
    std::uintptr_t floor = nativeStackFloorCache;
    if (floor == 0)
        floor = nativeStackFloorLookup();
    const std::uintptr_t lowest = floor > 1 ? floor + kStackReserve : 0;
    const std::size_t depthLeft =
        rt.ctx.options.stackLimit > rt.ctx.callDepth ? rt.ctx.options.stackLimit - rt.ctx.callDepth : 0;
    const std::uintptr_t budget = std::uintptr_t(depthLeft) * kNativeBytesPerCall;
    const std::uintptr_t byDepth = here > budget ? here - budget : 0;
    rt.nativeStackLimit = lowest > byDepth ? lowest : byDepth;
    rt.pollFlag = rt.ctx.interrupt ? 1u : 0u;
}

/// Native code finished; raise what it left, if anything.
void raisePending(Runtime& rt)
{
    if (rt.nativeError == 0)
        return;
    const ErrorCode code = ErrorCode(rt.nativeError);
    rt.nativeError = 0;
    if (rt.nativePending) {
        std::exception_ptr pending = std::move(rt.nativePending);
        rt.nativePending = nullptr;
        std::rethrow_exception(pending);
    }
    throwError(rt, code, "execution was interrupted");
}

/// Marks a run of native frames in `Runtime::frames`.
struct NativeRun
{
    Runtime& rt;
    Frame& frame;
    bool pushed;
    std::uintptr_t savedLimit;
    std::size_t savedFinally;
    NativeRun(Runtime& runtime, Frame& marker, bool push)
        : rt(runtime), frame(marker), pushed(push), savedLimit(runtime.nativeStackLimit),
          savedFinally(runtime.finallyPending.size())
    {
        frame.nativeRun = true;
        frame.savedTopFrame = rt.nativeTopFrame;
        frame.savedTopPc = rt.nativeTopPc;
        rt.nativeTopFrame = nullptr;
        rt.nativeTopPc = nullptr;
        if (pushed) {
            ++rt.ctx.callDepth;
            rt.frames.push_back(&frame);
        }
    }
    ~NativeRun()
    {
        rt.finallyPending.resize(savedFinally);
        rt.nativeStackLimit = savedLimit;
        rt.nativeTopFrame = frame.savedTopFrame;
        rt.nativeTopPc = frame.savedTopPc;
        frame.nativeRun = false;
        if (pushed) {
            rt.frames.pop_back();
            --rt.ctx.callDepth;
        }
    }
};

}  // namespace

Value invokeNativeCode(Runtime& rt, const MethodInfo* method, NativeCode* code, Value* args, std::uint32_t argc)
{
    std::uint64_t ints[kIntRegs] = {};
    std::uint64_t fpBits[kFpRegs] = {};
    std::uint64_t stackArgs[32];
    Abi abi;
    std::uint32_t next = 0;

    const auto put = [&](bool fp, std::uint64_t bits) {
        const Abi::Place place = abi.next(fp);
        if (place.where == Abi::Where::Int)
            ints[place.index] = bits;
        else if (place.where == Abi::Where::Fp)
            fpBits[place.index] = bits;
        else if (place.index < 32)
            stackArgs[place.index] = bits;
    };
    if (!method->isStatic()) {
        put(isFpParam(method->owner, false), next < argc ? bitsOfValue(args[next], method->owner, false) : 0);
        ++next;
    }
    for (const ParamInfo& param : method->params) {
        const bool byRef = isByRef(param);
        if (!byRef && isTaggedKind(param.type)) {
            const Value none;
            const Value& v = next < argc ? args[next] : none;
            GCObject* cell = detail::Access::ptr(v);
            put(false, std::uint64_t(std::uint32_t(v.type())));
            put(false, cell ? reinterpret_cast<std::uint64_t>(cell) : std::uint64_t(detail::Access::i8(v)));
            ++next;
            continue;
        }
        put(isFpParam(param.type, byRef), next < argc ? bitsOfValue(args[next], param.type, byRef) : 0);
        ++next;
    }

    // A lambda reads its boxes through a delegate; a call from C++ comes with them in the slots the compiler set aside,
    // so a stand-in with the same layout does.
    void* boxes[16];
    const void* delegate = nullptr;
    const void* boxesData = boxes;
    if (!method->captureSlots.empty()) {
        for (std::size_t i = 0; i < method->captureSlots.size() && i < 16; ++i) {
            const std::uint32_t slot = method->captureSlots[i];
            boxes[i] = slot < argc ? detail::Access::ptr(args[slot]) : nullptr;
        }
        delegate = reinterpret_cast<const char*>(&boxesData) - kDelegateCapturedOffset;
    }

    Frame& marker = rt.acquireFrame(rt.ctx.callDepth);
    marker.method = method;
    NativeRun run(rt, marker, true);
    armNativeRun(rt, reinterpret_cast<std::uintptr_t>(__builtin_frame_address(0)));

    std::uint64_t intResults[2] = {0, 0};
    double fpResult = 0;
    scriptJitEnter(&rt, code->fastEntry, ints, reinterpret_cast<const double*>(fpBits), stackArgs, abi.stackWords(),
                   delegate, intResults, &fpResult);
    marker.method = nullptr;
    raisePending(rt);
    return nativeResultValue(method, intResults, fpResult);
}

Value nativeResultValue(const MethodInfo* method, const std::uint64_t* intResults, double fpResult)
{
    const std::uint64_t intResult = intResults[0];
    const TypeInfo* type = method->returnType;
    if (!type || type->kind == TypeKind::Void)
        return Value();
    if (isTaggedKind(type)) {
        const Type tag = Type(std::uint32_t(intResults[1]));
        if (tag >= Type::String)
            return intResult ? detail::Access::adopt(tag, reinterpret_cast<GCObject*>(intResult)) : Value();
        return valueOfBits(std::int64_t(tag), intResult);
    }
    if (isFpParam(type, false)) {
        std::uint64_t bits = 0;
        std::memcpy(&bits, &fpResult, sizeof bits);
        return valueOfArg(bits, type, false);
    }
    const TypeKind kind = kindOf(type);
    if (isValueKind(kind))
        return valueOfArg(intResult, type, false);
    auto* cell = reinterpret_cast<GCObject*>(intResult);
    return cell ? detail::Access::adopt(tagOfCell(cell), cell) : Value();
}

bool tryOnStackReplacement(Runtime& rt, Frame& frame, std::uint32_t pc, Value& result)
{
    MethodInfo& method = const_cast<MethodInfo&>(*frame.method);
    if (!rt.jit || !rt.jit->enabled() || !frame.stack.empty())
        return false;
    // Native code knows nothing of a `finally` the interpreter is in the middle of.
    if (!frame.pending.empty() || frame.unwinding || !frame.exception.isNull())
        return false;
    NativeCode* code = method.code.load(std::memory_order_acquire);
    if (!code && method.hotness.load(std::memory_order_relaxed) != MethodInfo::kNeverCompile)
        code = rt.jit->onHot(rt, method);
    if (!code || !code->fastEntry)
        return false;
    const NativeCode::Osr* entry = nullptr;
    for (const NativeCode::Osr& candidate : code->osr) {
        if (candidate.pc == pc)
            entry = &candidate;
    }
    if (!entry)
        return false;

    // Native code reads a picture of the frame, because the counts of what it will own move with the values and those
    // slots must read as empty from now on.
    const std::size_t pictureBytes = frame.slots.size() * sizeof(Value);
    void* picture = __builtin_alloca(pictureBytes);
    if (pictureBytes)
        std::memcpy(picture, static_cast<const void*>(frame.slots.data()), pictureBytes);
    for (std::uint32_t slot : entry->ownedSlots) {
        if (slot < frame.slots.size())
            detail::Access::retag(frame.slots[slot], Type::Null);
    }
    std::uint64_t ints[kIntRegs] = {reinterpret_cast<std::uint64_t>(picture)};
    std::uint64_t fpBits[kFpRegs] = {};

    NativeRun run(rt, frame, false);
    armNativeRun(rt, reinterpret_cast<std::uintptr_t>(__builtin_frame_address(0)));
    // From here on this frame's handlers are native code's business: what comes out is past all of them.
    frame.handledNatively = true;
    std::uint64_t intResults[2] = {0, 0};
    double fpResult = 0;
    scriptJitEnter(&rt, entry->entry, ints, reinterpret_cast<const double*>(fpBits), nullptr, 0, nullptr, intResults,
                   &fpResult);
    raisePending(rt);
    result = nativeResultValue(frame.method, intResults, fpResult);
    return true;
}

std::string describeNativeFrames(const Runtime& rt, void* topFrame, const void* topPc)
{
    std::string out;
    if (!rt.jit)
        return out;
    const auto describe = [&](const void* pc) -> bool {
        const NativeCode* code = rt.jit->findCode(pc);
        if (!code)
            return false;
        const std::uint32_t offset =
            std::uint32_t(static_cast<const std::uint8_t*>(pc) - static_cast<const std::uint8_t*>(code->codeStart));
        std::int32_t site = -1;
        for (const NativeCode::Site& candidate : code->sites) {
            if (candidate.offset == offset) {
                site = candidate.site;
                break;
            }
        }
        if (site < 0) {
            if (code->method)
                out += "   at " + code->method->qualifiedName() + "\n";
            return true;
        }
        for (; site >= 0; site = code->sourceSites[std::size_t(site)].parent) {
            const NativeCode::SourceSite& where = code->sourceSites[std::size_t(site)];
            const auto* method = static_cast<const MethodInfo*>(where.method);
            out += "   at " + method->qualifiedName();
            std::uint32_t line = 0;
            for (const LineEntry& entry : method->body.lines) {
                if (entry.offset > where.pc)
                    break;
                line = entry.line;
            }
            if (line) {
                const std::string file = method->assembly && method->assembly->sourceName ? *method->assembly->sourceName
                                                                                           : std::string("<assembly>");
                out += " (" + file + ":" + std::to_string(line) + ")";
            }
            out += "\n";
        }
        return true;
    };

    if (topPc)
        describe(topPc);
    // The frame chain: [fp] is the caller's frame, [fp + 8] the address returned to in it. It ends where native code was
    // entered from C++, whose return address is not in the code cache.
    for (auto* frame = static_cast<void**>(topFrame); frame;) {
        const void* returnTo = frame[1];
        if (!describe(returnTo))
            break;
        frame = static_cast<void**>(frame[0]);
    }
    return out;
}

}  // namespace typed
}  // namespace internal
}  // namespace script
