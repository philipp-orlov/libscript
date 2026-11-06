// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// What native code for the dynamic languages calls, and how it is entered.
#include <cmath>
#include <cstring>
#include <string>

#include "script/internal/dyn/jit.hpp"
#include "script/internal/dyn/jit_helpers.hpp"
#include "script/internal/dyn/runtime.hpp"
#include "script/internal/dyn/vm.hpp"
#include "script/internal/stack_guard.hpp"
#include "script/internal/typed/jit_runtime.hpp"

namespace script {
namespace internal {
namespace dyn {

namespace {

/// A value from its two registers, holding a count of its own.
inline Value valueOf(std::int64_t tag, std::uint64_t payload) noexcept
{
    const Type type = Type(tag);
    switch (type) {
        case Type::Undefined: return Value::undefined();
        case Type::Null: return Value();
        case Type::Bool: return Value((payload & 0xff) != 0);
        case Type::Double: {
            double d = 0;
            std::memcpy(&d, &payload, sizeof d);
            return Value(d);
        }
        case Type::Char: return Value(char16_t(payload));
        case Type::Int: return Value(std::int32_t(payload));
        case Type::Long: return Value(std::int64_t(payload));
        case Type::Float: {
            float f = 0;
            const auto bits = std::uint32_t(payload);
            std::memcpy(&f, &bits, sizeof f);
            return Value(f);
        }
        default: return detail::Access::make(type, reinterpret_cast<GCObject*>(payload));
    }
}

/// Take a value apart, the count going with it.
inline NativePair take(Value v) noexcept
{
    NativePair out;
    out.tag = std::uint64_t(v.type());
    if (GCObject* cell = detail::Access::ptr(v)) {
        detail::incRef(cell);
        out.payload = reinterpret_cast<std::uint64_t>(cell);
    } else {
        out.payload = std::uint64_t(detail::Access::i8(v));
    }
    return out;
}

/// Hand back a value that stays alive until the next one handed back this way: for a slow path whose fast path only borrows.
inline NativePair keep(Runtime& rt, Value v) noexcept
{
    rt.nativeKeepAlive = std::move(v);
    NativePair out;
    out.tag = std::uint64_t(rt.nativeKeepAlive.type());
    GCObject* cell = detail::Access::ptr(rt.nativeKeepAlive);
    out.payload = cell ? reinterpret_cast<std::uint64_t>(cell) : std::uint64_t(detail::Access::i8(rt.nativeKeepAlive));
    return out;
}

void recordCurrent(Runtime& rt) noexcept
{
    rt.nativePending = std::current_exception();
    rt.nativeError = std::uint32_t(ErrorCode::Exception);
}

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

#define SCRIPT_HERE __builtin_return_address(0)

}  // namespace

extern "C" {

void scriptDynRaise(Runtime* rt, std::uint32_t code, std::int32_t) noexcept
{
    guarded(*rt, SCRIPT_HERE, [&]() -> int {
        throwError(*rt, ErrorCode(code), code == std::uint32_t(ErrorCode::StackOverflow) ? "maximum call depth exceeded"
                                                                                         : "execution was interrupted");
    });
}

int scriptDynPoll(Runtime* rt) noexcept
{
    if (!rt->ctx.interrupt)
        return 0;
    constexpr std::uint32_t kPollInterval = 1024;
    if (rt->ctx.interruptCountdown > 0) {
        --rt->ctx.interruptCountdown;
        return 0;
    }
    rt->ctx.interruptCountdown = kPollInterval - 1;
    try {
        if (!rt->ctx.interrupt())
            return 0;
    } catch (...) {
    }
    rt->nativeError = std::uint32_t(ErrorCode::Interrupted);
    return 1;
}

double scriptDynToNumber(Runtime* rt, std::int64_t tag, std::uint64_t payload) noexcept
{
    return guarded(*rt, SCRIPT_HERE, [&]() -> double { return toNumber(*rt, valueOf(tag, payload)); });
}

std::int32_t scriptDynToInt32(double d) noexcept
{
    return toInt32(d);
}

std::int32_t scriptDynToBoolean(std::int64_t tag, std::uint64_t payload) noexcept
{
    return toBoolean(valueOf(tag, payload)) ? 1 : 0;
}

double scriptDynFmod(double a, double b) noexcept
{
    return std::fmod(a, b);
}

double scriptDynPow(double a, double b) noexcept
{
    return std::pow(a, b);
}

NativePair scriptDynAdd(Runtime* rt, std::int64_t ta, std::uint64_t a, std::int64_t tb, std::uint64_t b) noexcept
{
    return guarded(*rt, SCRIPT_HERE, [&]() -> NativePair {
        const Value x = valueOf(ta, a), y = valueOf(tb, b);
        if (x.isNumber() && y.isNumber())
            return take(Value(numberOf(x) + numberOf(y)));
        return take(add(*rt, x, y));
    });
}

std::int32_t scriptDynCompare(Runtime* rt, std::int64_t op, std::int64_t ta, std::uint64_t a, std::int64_t tb,
                              std::uint64_t b) noexcept
{
    return guarded(*rt, SCRIPT_HERE, [&]() -> std::int32_t {
        const int r = compare(*rt, valueOf(ta, a), valueOf(tb, b));
        if (r == 2)
            return 0;
        switch (Op(op)) {
            case Op::Lt: return r < 0;
            case Op::Le: return r <= 0;
            case Op::Gt: return r > 0;
            default: return r >= 0;
        }
    });
}

std::int32_t scriptDynLooseEquals(Runtime* rt, std::int64_t ta, std::uint64_t a, std::int64_t tb, std::uint64_t b) noexcept
{
    return guarded(*rt, SCRIPT_HERE, [&]() -> std::int32_t { return looseEquals(*rt, valueOf(ta, a), valueOf(tb, b)); });
}

std::int32_t scriptDynStrictEquals(std::int64_t ta, std::uint64_t a, std::int64_t tb, std::uint64_t b) noexcept
{
    return strictEquals(valueOf(ta, a), valueOf(tb, b)) ? 1 : 0;
}

NativePair scriptDynTypeOf(Runtime* rt, std::int64_t tag, std::uint64_t payload) noexcept
{
    return guarded(*rt, SCRIPT_HERE, [&]() -> NativePair { return take(rt->str(typeOfName(valueOf(tag, payload)))); });
}

NativePair scriptDynToStringOp(Runtime* rt, std::int64_t tag, std::uint64_t payload) noexcept
{
    return guarded(*rt, SCRIPT_HERE,
                   [&]() -> NativePair { return take(rt->str(toStringValue(*rt, valueOf(tag, payload)))); });
}

NativePair scriptDynToPropertyKeyOp(Runtime* rt, std::int64_t tag, std::uint64_t payload) noexcept
{
    return guarded(*rt, SCRIPT_HERE, [&]() -> NativePair {
        Ref<GCObject> key = toPropertyKey(*rt, valueOf(tag, payload));
        return take(detail::Access::make(key->kind == CellKind::Symbol ? Type::Symbol : Type::String, key.get()));
    });
}

NativePair scriptDynGetProp(Runtime* rt, const FunctionCode* code, std::int64_t tag, std::uint64_t payload,
                            std::uint32_t operand) noexcept
{
    return guarded(*rt, SCRIPT_HERE, [&]() -> NativePair {
        return keep(*rt, getPropertyForNative(*rt, *code, valueOf(tag, payload), operand));
    });
}

void scriptDynSetProp(Runtime* rt, const FunctionCode* code, std::int64_t tag, std::uint64_t payload, std::uint32_t operand,
                      std::int64_t valueTag, std::uint64_t value) noexcept
{
    guarded(*rt, SCRIPT_HERE, [&]() -> int {
        setPropertyForNative(*rt, *code, valueOf(tag, payload), operand, valueOf(valueTag, value));
        return 0;
    });
}

NativePair scriptDynGetElem(Runtime* rt, std::int64_t tag, std::uint64_t payload, std::int64_t keyTag,
                            std::uint64_t key) noexcept
{
    return guarded(*rt, SCRIPT_HERE, [&]() -> NativePair {
        Ref<GCObject> k = toPropertyKey(*rt, valueOf(keyTag, key));
        return keep(*rt, getProperty(*rt, valueOf(tag, payload), k.get()));
    });
}

void scriptDynSetElem(Runtime* rt, std::int64_t tag, std::uint64_t payload, std::int64_t keyTag, std::uint64_t key,
                      std::int64_t valueTag, std::uint64_t value) noexcept
{
    guarded(*rt, SCRIPT_HERE, [&]() -> int {
        Ref<GCObject> k = toPropertyKey(*rt, valueOf(keyTag, key));
        setProperty(*rt, valueOf(tag, payload), k.get(), valueOf(valueTag, value));
        return 0;
    });
}

NativePair scriptDynGetGlobal(Runtime* rt, GCObject* key) noexcept
{
    return guarded(*rt, SCRIPT_HERE, [&]() -> NativePair { return keep(*rt, getGlobalForNative(*rt, key)); });
}

void scriptDynSetGlobal(Runtime* rt, GCObject* key, std::int64_t tag, std::uint64_t payload) noexcept
{
    guarded(*rt, SCRIPT_HERE, [&]() -> int {
        setGlobalForNative(*rt, key, valueOf(tag, payload));
        return 0;
    });
}

NativePair scriptDynCall(Runtime* rt, std::int64_t fnTag, std::uint64_t fn, std::int64_t thisTag, std::uint64_t self,
                         const Value* args, std::uint64_t argc) noexcept
{
    return guarded(*rt, SCRIPT_HERE, [&]() -> NativePair {
        ContextImpl::ArgBuffer buffer(rt->ctx);
        buffer.args.assign(args, args + argc);
        return take(callValue(*rt, valueOf(fnTag, fn), valueOf(thisTag, self), buffer.args));
    });
}

NativePair scriptDynNew(Runtime* rt, std::int64_t fnTag, std::uint64_t fn, const Value* args, std::uint64_t argc) noexcept
{
    return guarded(*rt, SCRIPT_HERE, [&]() -> NativePair {
        ContextImpl::ArgBuffer buffer(rt->ctx);
        buffer.args.assign(args, args + argc);
        return take(constructValue(*rt, valueOf(fnTag, fn), buffer.args));
    });
}

GCObject* scriptDynNewObject(Runtime* rt) noexcept
{
    return guarded(*rt, SCRIPT_HERE, [&]() -> GCObject* { return rt->newObject(rt->objectProto.get()); });
}

GCObject* scriptDynNewArray(Runtime* rt, std::uint64_t reserve) noexcept
{
    return guarded(*rt, SCRIPT_HERE, [&]() -> GCObject* { return rt->newArray(reserve); });
}

void scriptDynArrayAppend(Runtime* rt, GCObject* array, std::int64_t tag, std::uint64_t payload) noexcept
{
    guarded(*rt, SCRIPT_HERE, [&]() -> int {
        static_cast<DynArray*>(array)->append(valueOf(tag, payload));
        return 0;
    });
}

void scriptDynDefField(Runtime* rt, GCObject* object, GCObject* key, std::int64_t tag, std::uint64_t payload) noexcept
{
    guarded(*rt, SCRIPT_HERE, [&]() -> int {
        static_cast<DynObject*>(object)->putOwn(key, valueOf(tag, payload));
        return 0;
    });
}

GCObject* scriptDynNewBox(Runtime* rt, std::int64_t tag, std::uint64_t payload) noexcept
{
    return guarded(*rt, SCRIPT_HERE, [&]() -> GCObject* {
        auto* box = rt->newCell<Box>();
        box->value = valueOf(tag, payload);
        return box;
    });
}

void scriptDynStoreBox(GCObject* box, std::int64_t tag, std::uint64_t payload) noexcept
{
    static_cast<Box*>(box)->value = valueOf(tag, payload);
}

NativePair scriptDynClosure(Runtime* rt, const std::shared_ptr<FunctionCode>* code, DynFunction* parent,
                            Box* const* boxes) noexcept
{
    return guarded(*rt, SCRIPT_HERE,
                   [&]() -> NativePair { return take(makeClosureForNative(*rt, *code, parent, boxes)); });
}

void scriptDynThrow(Runtime* rt, std::int64_t tag, std::uint64_t payload) noexcept
{
    guarded(*rt, SCRIPT_HERE, [&]() -> int {
        throwValueForNative(*rt, valueOf(tag, payload));
    });
}

NativePair scriptDynGetIterator(Runtime* rt, std::int64_t tag, std::uint64_t payload) noexcept
{
    return guarded(*rt, SCRIPT_HERE, [&]() -> NativePair { return take(getIterator(*rt, valueOf(tag, payload))); });
}

/// One step: 1 and the item in `out`, or 0 at the end. `out` gets a count, as an owned value.
std::int32_t scriptDynIterNext(Runtime* rt, std::int64_t tag, std::uint64_t payload, NativePair* out) noexcept
{
    return guarded(*rt, SCRIPT_HERE, [&]() -> std::int32_t {
        Value item = Value::undefined();
        if (!iteratorStep(*rt, valueOf(tag, payload), item))
            return 0;
        *out = take(std::move(item));
        return 1;
    });
}

void scriptDynIterClose(Runtime* rt, std::int64_t tag, std::uint64_t payload) noexcept
{
    guarded(*rt, SCRIPT_HERE, [&]() -> int {
        iteratorClose(*rt, valueOf(tag, payload));
        return 0;
    });
}

NativePair scriptDynGlobalThis(Runtime* rt) noexcept
{
    return take(detail::Access::make(Type::Object, rt->globalObject.get()));
}

}  // extern "C"

// ---------------------------------------------------------------- calling ---

namespace {

/// How much stack a level of `stackLimit` buys native frames.
constexpr std::size_t kNativeBytesPerCall = 1024;

void armNativeRun(Runtime& rt, std::uintptr_t here) noexcept
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

struct NativeRun
{
    Runtime& rt;
    Frame& frame;
    bool pushed;
    std::uintptr_t savedLimit;
    NativeRun(Runtime& runtime, Frame& marker, bool push)
        : rt(runtime), frame(marker), pushed(push), savedLimit(runtime.nativeStackLimit)
    {
        frame.nativeRun = true;
        frame.savedTopFrame = rt.nativeTopFrame;
        frame.savedTopPc = rt.nativeTopPc;
        rt.nativeTopFrame = nullptr;
        rt.nativeTopPc = nullptr;
        if (pushed)
            rt.frames.push_back(&frame);
    }
    ~NativeRun()
    {
        // Nothing past this point borrows it: a caller's reads hold counts across a call.
        rt.nativeKeepAlive = Value::undefined();
        rt.nativeStackLimit = savedLimit;
        rt.nativeTopFrame = frame.savedTopFrame;
        rt.nativeTopPc = frame.savedTopPc;
        frame.nativeRun = false;
        if (pushed)
            rt.frames.pop_back();
    }
};

inline Value resultValue(const std::uint64_t* results) noexcept
{
    const Type type = Type(std::uint32_t(results[1]));
    if (type >= Type::String)
        return detail::Access::adopt(type, reinterpret_cast<GCObject*>(results[0]));
    return valueOf(std::int64_t(type), results[0]);
}

inline std::uint64_t payloadOf(const Value& v) noexcept
{
    GCObject* cell = detail::Access::ptr(v);
    return cell ? reinterpret_cast<std::uint64_t>(cell) : std::uint64_t(detail::Access::i8(v));
}

}  // namespace

Value enterNative(Runtime& rt, DynFunction* fn, const FunctionCode& code, const Value& thisValue, const Value* args,
                  std::size_t argc)
{
    Frame& marker = rt.acquireFrame(rt.frames.size());
    NativeRun run(rt, marker, true);
    armNativeRun(rt, reinterpret_cast<std::uintptr_t>(__builtin_frame_address(0)));
    std::uint64_t ints[8] = {reinterpret_cast<std::uint64_t>(fn), std::uint64_t(thisValue.type()), payloadOf(thisValue),
                             reinterpret_cast<std::uint64_t>(args), argc};
    const double fps[8] = {};
    std::uint64_t results[2] = {0, 0};
    double fpResult = 0;
    typed::scriptJitEnter(reinterpret_cast<typed::Runtime*>(&rt), code.jitEntry, ints, fps, nullptr, 0, nullptr, results,
                          &fpResult);
    raisePending(rt);
    return resultValue(results);
}

bool enterNativeAtLoop(Runtime& rt, Frame& frame, std::uint32_t pc, Value& result)
{
    const FunctionCode& code = *frame.code;
    if (!frame.stack.empty() || !compileNative(rt, code) || !code.jitCode)
        return false;
    const typed::NativeCode::Osr* entry = nullptr;
    for (const typed::NativeCode::Osr& candidate : code.jitCode->osr) {
        if (candidate.pc == pc)
            entry = &candidate;
    }
    if (!entry)
        return false;

    // Native code reads a picture of the frame: the counts of what it will own move with the values, and those slots must
    // read as empty here from now on.
    const std::size_t valueBytes = frame.locals.size() * sizeof(Value);
    void* values = __builtin_alloca(valueBytes + 16);
    std::memcpy(values, static_cast<const void*>(frame.locals.data()), valueBytes);
    const std::size_t cellBytes = frame.cells.size() * sizeof(void*);
    void* cells = __builtin_alloca(cellBytes + 16);
    std::memcpy(cells, static_cast<const void*>(frame.cells.data()), cellBytes);
    for (std::uint32_t slot : entry->ownedSlots) {
        if (slot < frame.locals.size())
            detail::Access::retag(frame.locals[slot], Type::Undefined);
    }
    for (std::uint32_t slot : entry->ownedPointers) {
        if (slot < frame.cells.size()) {
            // Gives the count up without releasing it.
            std::memset(static_cast<void*>(&frame.cells[slot]), 0, sizeof(void*));
        }
    }
    std::uint64_t ints[8] = {reinterpret_cast<std::uint64_t>(values), reinterpret_cast<std::uint64_t>(cells),
                             reinterpret_cast<std::uint64_t>(frame.fn), std::uint64_t(frame.thisValue.type()),
                             payloadOf(frame.thisValue)};
    const double fps[8] = {};
    NativeRun run(rt, frame, false);
    armNativeRun(rt, reinterpret_cast<std::uintptr_t>(__builtin_frame_address(0)));
    std::uint64_t results[2] = {0, 0};
    double fpResult = 0;
    typed::scriptJitEnter(reinterpret_cast<typed::Runtime*>(&rt), entry->entry, ints, fps, nullptr, 0, nullptr, results,
                          &fpResult);
    raisePending(rt);
    result = resultValue(results);
    return true;
}

bool compileNative(Runtime& rt, const FunctionCode& code)
{
    if (code.jitEntry)
        return true;
    if (code.jitRefused || !rt.jitBackend || !rt.jitBackend->available() ||
        rt.ctx.options.jit.mode == JitOptions::Mode::Off)
        return false;
    typed::IrFunction fn;
    std::string why;
    if (!lowerFunction(rt, code, fn, &why)) {
        code.jitRefused = true;
        if (rt.ctx.options.jit.dumpCode)
            rt.ctx.write(fromUtf8("jit: skipping " + code.name + ": " + why + "\n"), false);
        return false;
    }
    typed::NativeConfig config;
    const auto offset = [&rt](const auto& field) {
        return std::int32_t(reinterpret_cast<std::uintptr_t>(&field) - reinterpret_cast<std::uintptr_t>(&rt));
    };
    config.nativeError = offset(rt.nativeError);
    config.pollFlag = offset(rt.pollFlag);
    config.stackLimit = offset(rt.nativeStackLimit);
    config.topFrame = offset(rt.nativeTopFrame);
    config.raise = reinterpret_cast<const void*>(&scriptDynRaise);
    config.poll = reinterpret_cast<const void*>(&scriptDynPoll);
    typed::NativeCode* compiled = rt.jitBackend->compileFunction(fn, config);
    if (!compiled) {
        code.jitRefused = true;
        return false;
    }
    code.jitCode = compiled;
    rt.jitCodes.push_back(compiled);
    ++rt.jitCompiled;
    if (rt.ctx.options.jit.dumpCode)
        rt.ctx.write(fromUtf8("jit: " + code.name + " (" + std::to_string(compiled->codeBytes) + " bytes)\n" +
                              rt.jitBackend->dump(compiled)),
                     false);
    if (!code.isClassConstructor)
        code.jitCallEntry = compiled->fastEntry;
    code.jitEntry = compiled->fastEntry;
    return true;
}

std::string describeNativeFrames(const Runtime& rt, void* topFrame, const void* topPc, int& shown)
{
    std::string out;
    const auto find = [&rt](const void* pc) -> const typed::NativeCode* {
        const auto* at = static_cast<const std::uint8_t*>(pc);
        for (const typed::NativeCode* code : rt.jitCodes) {
            const auto* start = static_cast<const std::uint8_t*>(code->codeStart);
            if (at >= start && at < start + code->codeBytes)
                return code;
        }
        return nullptr;
    };
    const auto describe = [&](const void* pc) -> bool {
        const typed::NativeCode* code = find(pc);
        if (!code)
            return false;
        const std::uint32_t offset =
            std::uint32_t(static_cast<const std::uint8_t*>(pc) - static_cast<const std::uint8_t*>(code->codeStart));
        std::int32_t site = -1;
        for (const typed::NativeCode::Site& candidate : code->sites) {
            if (candidate.offset == offset)
                site = candidate.site;
        }
        if (site < 0 && !code->sourceSites.empty())
            site = 0;
        if (site >= 0 && shown < 16) {
            const typed::NativeCode::SourceSite& where = code->sourceSites[std::size_t(site)];
            const auto* function = static_cast<const FunctionCode*>(where.method);
            out += "    at ";
            out += function->name.empty() ? "<anonymous>" : function->name;
            out += " (";
            out += function->sourceName ? *function->sourceName : std::string("<unknown>");
            out += ":" + std::to_string(function->lineAt(where.pc)) + ")\n";
            ++shown;
        }
        return true;
    };
    if (topPc)
        describe(topPc);
    for (auto* frame = static_cast<void**>(topFrame); frame;) {
        if (!describe(frame[1]))
            break;
        frame = static_cast<void**>(frame[0]);
    }
    return out;
}

}  // namespace dyn
}  // namespace internal
}  // namespace script
