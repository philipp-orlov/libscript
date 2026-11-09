// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// What native code for the dynamic languages calls, and how it is entered.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "script/internal/dyn/builtins.hpp"
#include "script/internal/dyn/numconv.hpp"
#include "script/internal/dyn/jit.hpp"
#include "script/internal/dyn/jit_helpers.hpp"
#include "script/internal/dyn/runtime.hpp"
#include "script/internal/dyn/vm.hpp"
#include "script/internal/stack_guard.hpp"
#include "script/internal/typed/jit_runtime.hpp"

namespace script {
namespace internal {
namespace dyn {

extern "C" NativePair scriptJitCall5(Runtime* rt, const void* entry, std::uint64_t a0, std::uint64_t a1, std::uint64_t a2,
                                     std::uint64_t a3, std::uint64_t a4);

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
        if (!rt->ctx.interruptAsked())
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
    return numberPow(a, b);
}

double scriptDynSin(double d) noexcept { return std::sin(d); }
double scriptDynCos(double d) noexcept { return std::cos(d); }
double scriptDynTan(double d) noexcept { return std::tan(d); }
double scriptDynAtan(double d) noexcept { return std::atan(d); }
double scriptDynExp(double d) noexcept { return std::exp(d); }
double scriptDynLog(double d) noexcept { return std::log(d); }
double scriptDynRound(double d) noexcept { return numberRound(d); }
double scriptDynMax(double a, double b) noexcept { return numberMax(a, b); }
double scriptDynMin(double a, double b) noexcept { return numberMin(a, b); }
double scriptDynAtan2(double a, double b) noexcept { return std::atan2(a, b); }

double scriptDynParseInt(double d) noexcept
{
    return parseIntOfNumber(d);
}

NativePair scriptDynMapGet(Runtime* rt, GCObject* map, std::int64_t tag, std::uint64_t payload) noexcept
{
    return guarded(*rt, SCRIPT_HERE, [&]() -> NativePair {
        auto* m = static_cast<DynMap*>(map);
        const std::int64_t at = m->find(valueOf(tag, payload));
        return take(at < 0 ? Value::undefined() : m->entries[std::size_t(at)].value);
    });
}

NativePair scriptDynMapHas(Runtime* rt, GCObject* map, std::int64_t tag, std::uint64_t payload) noexcept
{
    return guarded(*rt, SCRIPT_HERE, [&]() -> NativePair {
        return take(Value(static_cast<DynMap*>(map)->find(valueOf(tag, payload)) >= 0));
    });
}

NativePair scriptDynMapSet(Runtime* rt, GCObject* map, std::int64_t tag, std::uint64_t payload, std::int64_t valueTag,
                           std::uint64_t value) noexcept
{
    return guarded(*rt, SCRIPT_HERE, [&]() -> NativePair {
        auto* m = static_cast<DynMap*>(map);
        m->insert(valueOf(tag, payload), valueOf(valueTag, value));
        return take(detail::Access::make(Type::Object, m));
    });
}

NativePair scriptDynArrayPush(Runtime* rt, GCObject* array, std::int64_t tag, std::uint64_t payload) noexcept
{
    return guarded(*rt, SCRIPT_HERE, [&]() -> NativePair {
        auto* a = static_cast<DynArray*>(array);
        a->append(valueOf(tag, payload));
        return take(Value(double(a->size())));
    });
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

std::int32_t scriptDynIterIsPlain(Runtime* rt, std::int64_t tag, std::uint64_t payload) noexcept
{
    DynObject* object = asObject(valueOf(tag, payload));
    if (!object || object->cls != ObjClass::Array || object->proto.get() != rt->arrayProto.get() || object->findOwn(rt->sym.iterator.get()))
        return 0;
    const Prop* iterate = rt->arrayProto->findOwn(rt->sym.iterator.get());
    if (!iterate || detail::Access::ptr(iterate->value) != rt->intrinsics[Runtime::ArrayValues].get())
        return 0;
    const Prop* next = rt->arrayIteratorProto->findOwn(rt->atom.next.get());
    if (!next || detail::Access::ptr(next->value) != rt->intrinsics[Runtime::ArrayIteratorNext].get())
        return 0;
    GCObject* name = rt->intern("return");
    return !rt->arrayIteratorProto->findOwn(name) && !rt->iteratorProto->findOwn(name) && !rt->objectProto->findOwn(name);
}

std::int32_t scriptDynStrictEquals(std::int64_t ta, std::uint64_t a, std::int64_t tb, std::uint64_t b) noexcept
{
    return strictEquals(valueOf(ta, a), valueOf(tb, b)) ? 1 : 0;
}

NativePair scriptDynTypeOf(Runtime* rt, std::int64_t tag, std::uint64_t payload) noexcept
{
    return guarded(*rt, SCRIPT_HERE, [&]() -> NativePair { return take(rt->typeOfValue(valueOf(tag, payload))); });
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

NativePair scriptDynResume(Runtime* rt, const FunctionCode* code, DynFunction* fn, std::int64_t thisTag,
                           std::uint64_t thisPayload, std::uint8_t* header) noexcept
{
    std::uint64_t where = 0;
    std::memcpy(&where, header, sizeof where);
    const auto pc = std::uint32_t(where);
    const auto stackCount = std::uint32_t(where >> 32);
    const FunctionCode* outer = nullptr;
    std::memcpy(&outer, header + 8, sizeof outer);
    if (!outer)
        outer = code;
    std::uint8_t* values = header + 16;
    // Code that keeps bailing is thrown away; what it is compiled to next assumes what the interpreter learns meanwhile,
    // and after a few rounds of that, nothing. The bail is the instruction's, in whichever function it is, but the code
    // to throw away is that of the function it was compiled into.
    constexpr std::uint32_t kBailLimit = 8;
    if (std::find(code->jitBailedAt.begin(), code->jitBailedAt.end(), pc) == code->jitBailedAt.end())
        code->jitBailedAt.push_back(pc);
#if defined(SCRIPT_JIT_ENV_OVERRIDE)
    if (const char* log = std::getenv("SCRIPT_JIT_BAILS")) {
        if (std::FILE* out = std::fopen(log, "a")) {
            std::fprintf(out, "%s %u %s\n", code->name.c_str(), pc, opName(Op(code->code[pc])));
            std::fclose(out);
        }
    }
#endif
    if (++outer->jitBails > kBailLimit && outer->jitEntry) {
        outer->jitEntry = nullptr;
        outer->jitCallEntry = nullptr;
        outer->jitCode = nullptr;
        // It was hot a moment ago, and what it just learnt is no more use the longer it waits.
        outer->jitHotness = outer->jitThreshold(rt->ctx.options.jit.threshold) - std::min<std::uint32_t>(outer->jitThreshold(rt->ctx.options.jit.threshold), 16);
        outer->jitBails = 0;
        if (outer->jitGeneration < 255)
            ++outer->jitGeneration;
    }
    struct State
    {
        std::uint8_t* values;
        std::uint32_t locals;
        std::uint32_t stack;
    } state{values, code->numLocals, stackCount};
    const auto fill = [](Frame& frame, void* raw) {
        const State& in = *static_cast<const State*>(raw);
        const auto at = [&in](std::uint32_t i) -> Value {
            const std::uint8_t* slot = in.values + std::size_t(i) * 16;
            std::uint64_t payload = 0;
            std::memcpy(&payload, slot + 8, sizeof payload);
            const Type type = Type(slot[0]);
            if (type >= Type::String && slot[1])
                return detail::Access::adopt(type, reinterpret_cast<GCObject*>(payload));
            return valueOf(std::int64_t(type), payload);
        };
        for (std::uint32_t i = 0; i < in.locals; ++i)
            frame.locals[i] = at(i);
        for (std::uint32_t i = 0; i < in.locals; ++i) {
            const std::uint8_t* slot = in.values + std::size_t(in.locals + i) * 16;
            std::uint64_t payload = 0;
            std::memcpy(&payload, slot + 8, sizeof payload);
            frame.cells[i].reset(reinterpret_cast<Box*>(payload), slot[1] != 0);
        }
        for (std::uint32_t i = 0; i < in.stack; ++i)
            frame.stack.push_back(at(2 * in.locals + i));
    };
    return guarded(*rt, SCRIPT_HERE, [&]() -> NativePair {
        return take(resumeInterpreter(*rt, fn, *code, valueOf(thisTag, thisPayload), pc, fill, &state));
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
        Value element;
        if (getElementByNumber(*rt, valueOf(tag, payload), valueOf(keyTag, key), element))
            return keep(*rt, element);
        Ref<GCObject> k = toPropertyKey(*rt, valueOf(keyTag, key));
        return keep(*rt, getProperty(*rt, valueOf(tag, payload), k.get()));
    });
}

void scriptDynSetElem(Runtime* rt, std::int64_t tag, std::uint64_t payload, std::int64_t keyTag, std::uint64_t key,
                      std::int64_t valueTag, std::uint64_t value) noexcept
{
    guarded(*rt, SCRIPT_HERE, [&]() -> int {
        if (setElementByNumber(*rt, valueOf(tag, payload), valueOf(keyTag, key), valueOf(valueTag, value)))
            return 0;
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

NativePair scriptDynBail(Runtime* rt, const DeoptInfo* info, const char* frame) noexcept
{
    const std::uint64_t* saved = rt->bailRegs;
    const auto word = [&](std::int32_t at) -> std::uint64_t {
        const std::int32_t place = info->places[std::size_t(at)];
        if (place >= 0)
            return saved[std::size_t(place) + (info->placeIsFp[std::size_t(at)] ? 32 : 0)];
        std::uint64_t out = 0;
        std::memcpy(&out, frame + info->spillOffset + std::size_t(-place - 1) * 8, sizeof out);
        return out;
    };
    // The picture of the frame `scriptDynResume` reads: pc and depth, whose code, then 16 bytes a value.
    const std::size_t count = info->values.size();
    auto* buffer = static_cast<std::uint8_t*>(__builtin_alloca(16 + count * 16));
    std::memset(buffer, 0, 16 + count * 16);
    const std::uint64_t where = std::uint64_t(info->pc) | (std::uint64_t(info->stack) << 32);
    std::memcpy(buffer, &where, sizeof where);
    std::memcpy(buffer + 8, &info->outer, sizeof info->outer);
    for (std::size_t i = 0; i < count; ++i) {
        const DeoptValue& v = info->values[i];
        std::uint8_t* slot = buffer + 16 + i * 16;
        slot[0] = v.tagAt >= 0 ? std::uint8_t(word(v.tagAt)) : v.tag;
        slot[1] = v.owned;
        std::uint64_t payload = v.bits;
        if (v.form == DeoptValue::Arguments) {
            payload = reinterpret_cast<std::uint64_t>(scriptDynArguments(rt, reinterpret_cast<const Value*>(word(v.payloadAt)), word(std::int32_t(v.bits))));
        } else if (v.form != DeoptValue::Constant) {
            payload = word(v.payloadAt);
            if (v.form == DeoptValue::Int32 || v.form == DeoptValue::Int64) {
                const double d = v.form == DeoptValue::Int32 ? double(std::int32_t(payload)) : double(std::int64_t(payload));
                std::memcpy(&payload, &d, sizeof payload);
            }
        }
        std::memcpy(slot + 8, &payload, sizeof payload);
    }
    return scriptDynResume(rt, info->code, reinterpret_cast<DynFunction*>(word(info->fnAt)), std::int64_t(std::uint8_t(word(info->thisTagAt))),
                           word(info->thisAt), buffer);
}

NativePair scriptDynEntryMiss(Runtime* rt, const FunctionCode* code, DynFunction* fn, std::int64_t thisTag,
                              std::uint64_t self, const Value* args, std::uint64_t argc) noexcept
{
    return guarded(*rt, SCRIPT_HERE, [&]() -> NativePair {
        const std::uint32_t arity = std::uint32_t(std::min(std::max(code->arity, 0), 32));
        for (std::uint32_t i = 0; i < arity; ++i) {
            if (i >= argc || !args[i].isDouble()) {
                code->argsNotNumber |= 1u << i;
                continue;
            }
            const double d = detail::Access::r8(args[i]);
            if (!(d >= -2147483648.0 && d <= 2147483647.0) || double(std::int32_t(d)) != d || (d == 0 && std::signbit(d)))
                code->argsNotInt |= 1u << i;
        }
        // What the code assumed is known now to be wrong, and the next compile knows better. The profile only ever
        // grows, so this happens at most twice for each parameter.
        code->jitEntry = nullptr;
        code->jitCallEntry = nullptr;
        code->jitCode = nullptr;
        code->jitHotness = code->jitThreshold(rt->ctx.options.jit.threshold) - std::min<std::uint32_t>(code->jitThreshold(rt->ctx.options.jit.threshold), 16);
        ContextImpl::ArgBuffer buffer(rt->ctx);
        buffer.args.assign(args, args + argc);
        return take(callValue(*rt, detail::Access::make(Type::Object, fn), valueOf(thisTag, self), buffer.args));
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

GCObject* scriptDynNewObject(Runtime* rt, std::uint64_t room) noexcept
{
    return guarded(*rt, SCRIPT_HERE, [&]() -> GCObject* { return rt->newObjectSized(rt->objectProto.get(), std::uint32_t(room)); });
}

GCObject* scriptDynNewOf(Runtime* rt, GCObject* proto, std::uint64_t room) noexcept
{
    return guarded(*rt, SCRIPT_HERE, [&]() -> GCObject* { return rt->newObjectSized(static_cast<DynObject*>(proto), std::uint32_t(room)); });
}

GCObject* scriptDynNewThis(Runtime* rt, GCObject* cell) noexcept
{
    auto* fn = static_cast<DynFunction*>(cell);
    // A prototype still to make is made by the long way, which can allocate.
    if (fn->fnKind != FnKind::Bytecode || fn->isArrow || fn->prototypePending)
        return nullptr;
    const Prop* prototype = fn->findOwn(rt->atom.prototype.get());
    if (!prototype || fn->accessorOf(*prototype))
        return nullptr;
    DynObject* proto = asObject(prototype->value);
    if (!proto)
        proto = rt->objectProto.get();
    return guarded(*rt, SCRIPT_HERE, [&]() -> GCObject* {
        Shape* root = rt->rootShapeFor(proto);
        return rt->newObjectSized(proto, root ? std::min<std::uint32_t>(root->expect, 32) : 0);
    });
}

GCObject* scriptDynNewArray(Runtime* rt, std::uint64_t reserve) noexcept
{
    return guarded(*rt, SCRIPT_HERE, [&]() -> GCObject* { return rt->newArray(reserve); });
}

GCObject* scriptDynNewRegExp(Runtime* rt, GCObject* templateObject) noexcept
{
    return guarded(*rt, SCRIPT_HERE, [&]() -> GCObject* { return newRegExpFromTemplate(*rt, static_cast<DynObject*>(templateObject)); });
}

void scriptDynArrayAppend(Runtime* rt, GCObject* array, std::int64_t tag, std::uint64_t payload) noexcept
{
    guarded(*rt, SCRIPT_HERE, [&]() -> int {
        static_cast<DynArray*>(array)->append(valueOf(tag, payload));
        return 0;
    });
}

void scriptDynDefField(Runtime* rt, GCObject* object, GCObject* key, std::int64_t tag, std::uint64_t payload, PropertySite* site) noexcept
{
    guarded(*rt, SCRIPT_HERE, [&]() -> int {
        defineFieldAt(*rt, site, static_cast<DynObject*>(object), key, valueOf(tag, payload));
        return 0;
    });
}

/// Where a `catch` in native code begins: what was thrown, taken from where the helper that raised left it. Anything but
/// an exception of the program's stays pending, for the caller.
NativePair scriptDynCatch(Runtime* rt) noexcept
{
    std::exception_ptr pending = std::move(rt->nativePending);
    rt->nativePending = nullptr;
    try {
        if (pending)
            std::rethrow_exception(pending);
    } catch (Exception& e) {
        rt->nativeError = 0;
        return take(e.value());
    } catch (...) {
        rt->nativePending = std::current_exception();
        return take(Value::undefined());
    }
    rt->nativeError = 0;
    return take(Value::undefined());
}

/// `break` out of a `for-of`, or `return` through one: the iterator's `return()`, if it has one.
void scriptDynIterCloseAt(Runtime* rt, std::int64_t tag, std::uint64_t payload) noexcept
{
    guarded(*rt, SCRIPT_HERE, [&]() -> int {
        const Value iterator = valueOf(tag, payload);
        const Value fn = getProperty(*rt, iterator, rt->intern("return"));
        if (!fn.isNullish() && !callValue(*rt, fn, iterator, {}).isObject())
            throwError(*rt, ErrorCode::TypeError, "the iterator's return() did not give an object");
        return 0;
    });
}

/// An exception leaving a `for-of`: the iterator closed, whatever closing it throws, and the exception raised again.
void scriptDynIterCloseThrow(Runtime* rt, std::int64_t iterTag, std::uint64_t iter, std::int64_t thrownTag, std::uint64_t thrown) noexcept
{
    guarded(*rt, SCRIPT_HERE, [&]() -> int {
        try {
            iteratorClose(*rt, valueOf(iterTag, iter));
        } catch (const Exception&) {
            // The reason for leaving stands.
        }
        throwValue(*rt, valueOf(thrownTag, thrown));
    });
}

/// Where an exception enters a `finally` in native code: it is put aside, to be raised again where the block ends, and
/// its number among those put aside comes back. Anything that is no exception of the program's skips the block, as in
/// the interpreter: 0, with it still pending.
std::uint64_t scriptDynFinallyEnter(Runtime* rt) noexcept
{
    std::exception_ptr pending = std::move(rt->nativePending);
    rt->nativePending = nullptr;
    try {
        if (pending)
            std::rethrow_exception(pending);
    } catch (Exception&) {
        rt->finallyPending.push_back(std::current_exception());
        rt->nativeError = 0;
        return rt->finallyPending.size();
    } catch (...) {
        rt->nativePending = std::current_exception();
        return 0;
    }
    return 0;
}

/// The end of a `finally` that an exception entered: number `n` raised again. Any put aside after it belong to blocks
/// that were left another way.
void scriptDynFinallyRethrow(Runtime* rt, std::uint64_t number) noexcept
{
    if (number == 0 || number > rt->finallyPending.size())
        return;
    rt->nativePending = std::move(rt->finallyPending[number - 1]);
    rt->nativeError = std::uint32_t(ErrorCode::Exception);
    rt->finallyPending.resize(number - 1);
}

/// `f(...args)` and `new F(...args)`: the arguments arrive as the array the spread made.
NativePair scriptDynCallSpread(Runtime* rt, std::int64_t calleeTag, std::uint64_t callee, std::int64_t selfTag, std::uint64_t self,
                               GCObject* array, std::uint64_t construct) noexcept
{
    return guarded(*rt, SCRIPT_HERE, [&]() -> NativePair {
        const auto* items = static_cast<const DynArray*>(array);
        ContextImpl::ArgBuffer buffer(rt->ctx);
        buffer.args.reserve(items->size());
        for (std::size_t i = 0; i < items->size(); ++i)
            buffer.args.push_back(items->at(i));
        if (construct)
            return take(constructValue(*rt, valueOf(calleeTag, callee), buffer.args));
        return take(callValue(*rt, valueOf(calleeTag, callee), valueOf(selfTag, self), buffer.args));
    });
}

/// `[...src]` in an array literal: what iterating `src` gives, onto the array being made.
void scriptDynArraySpread(Runtime* rt, GCObject* array, std::int64_t tag, std::uint64_t payload) noexcept
{
    guarded(*rt, SCRIPT_HERE, [&]() -> int {
        const Vec<Value> items = iterableToVector(*rt, valueOf(tag, payload));
        static_cast<DynArray*>(array)->appendAll(items.data(), items.size());
        return 0;
    });
}

/// `[a, ...rest] = it`: what is left of the iterator, as an array.
NativePair scriptDynIterRest(Runtime* rt, std::int64_t tag, std::uint64_t payload) noexcept
{
    return guarded(*rt, SCRIPT_HERE, [&]() -> NativePair {
        DynArray* rest = rt->newArray();
        Value out = detail::Access::adopt(Type::Object, rest);
        const Value iterator = valueOf(tag, payload);
        Value item = Value::undefined();
        while (iteratorStep(*rt, iterator, item))
            rest->append(item);
        return take(std::move(out));
    });
}

/// `{...src}` in an object literal: `src`'s own enumerable properties, onto the object being made.
void scriptDynObjectSpread(Runtime* rt, GCObject* object, std::int64_t tag, std::uint64_t payload) noexcept
{
    guarded(*rt, SCRIPT_HERE, [&]() -> int {
        objectSpread(*rt, static_cast<DynObject*>(object), valueOf(tag, payload));
        return 0;
    });
}

/// `#x = v` in a class body, or a private method: a member the object must not have yet.
void scriptDynDefPrivate(Runtime* rt, GCObject* object, GCObject* name, std::int64_t tag, std::uint64_t payload, std::uint64_t method,
                         PropertySite* site) noexcept
{
    guarded(*rt, SCRIPT_HERE, [&]() -> int {
        auto* o = static_cast<DynObject*>(object);
        if (!method) {
            definePrivateAt(*rt, site, o, name, valueOf(tag, payload));
            return 0;
        }
        if (o->findOwn(name))
            throwError(*rt, ErrorCode::TypeError, "this object has that private member already");
        o->putOwn(name, valueOf(tag, payload), std::uint8_t(0));
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

GCObject* scriptDynArgsArray(Runtime* rt, const Value* args, std::uint64_t argc, std::uint64_t from) noexcept
{
    return guarded(*rt, SCRIPT_HERE, [&]() -> GCObject* {
        DynArray* out = rt->newArray(argc > from ? std::size_t(argc - from) : 0);
        Value handle = detail::Access::adopt(Type::Object, out);
        for (std::uint64_t i = from; i < argc; ++i)
            out->append(args[i]);
        detail::incRef(out);
        return out;
    });
}

GCObject* scriptDynArguments(Runtime* rt, const Value* args, std::uint64_t argc) noexcept
{
    return guarded(*rt, SCRIPT_HERE, [&]() -> GCObject* { return newArgumentsObject(*rt, args, std::size_t(argc)); });
}

std::int32_t scriptDynInstanceOf(Runtime* rt, std::int64_t ta, std::uint64_t a, std::int64_t tb, std::uint64_t b) noexcept
{
    return guarded(*rt, SCRIPT_HERE, [&]() -> std::int32_t { return instanceOf(*rt, valueOf(ta, a), valueOf(tb, b)) ? 1 : 0; });
}

std::int32_t scriptDynIn(Runtime* rt, std::int64_t ta, std::uint64_t a, std::int64_t tb, std::uint64_t b) noexcept
{
    return guarded(*rt, SCRIPT_HERE, [&]() -> std::int32_t {
        Ref<GCObject> key = toPropertyKey(*rt, valueOf(ta, a));
        return hasProperty(*rt, valueOf(tb, b), key.get()) ? 1 : 0;
    });
}

std::int32_t scriptDynDeleteProp(Runtime* rt, std::int64_t tag, std::uint64_t payload, GCObject* key) noexcept
{
    return guarded(*rt, SCRIPT_HERE, [&]() -> std::int32_t { return deleteProperty(*rt, valueOf(tag, payload), key) ? 1 : 0; });
}

std::int32_t scriptDynDeleteElem(Runtime* rt, std::int64_t tag, std::uint64_t payload, std::int64_t keyTag, std::uint64_t key) noexcept
{
    return guarded(*rt, SCRIPT_HERE, [&]() -> std::int32_t {
        Ref<GCObject> k = toPropertyKey(*rt, valueOf(keyTag, key));
        return deleteProperty(*rt, valueOf(tag, payload), k.get()) ? 1 : 0;
    });
}

void scriptDynDefGlobal(Runtime* rt, GCObject* key, std::uint64_t attrs) noexcept
{
    guarded(*rt, SCRIPT_HERE, [&]() -> int {
        if (!rt->globalObject->findOwn(key))
            rt->globalObject->putOwn(key, Value::undefined(), std::uint8_t(attrs));
        return 0;
    });
}

NativePair scriptDynTypeofGlobal(Runtime* rt, GCObject* key) noexcept
{
    return guarded(*rt, SCRIPT_HERE, [&]() -> NativePair {
        const Value g = detail::Access::make(Type::Object, rt->globalObject.get());
        if (!hasProperty(*rt, g, key))
            return take(rt->str("undefined"));
        return take(rt->typeOfValue(getProperty(*rt, g, key)));
    });
}

void scriptDynDefMember(Runtime* rt, std::int64_t mode, GCObject* object, GCObject* key, std::int64_t tag, std::uint64_t payload) noexcept
{
    guarded(*rt, SCRIPT_HERE, [&]() -> int {
        defineMemberForNative(*rt, int(mode), static_cast<DynObject*>(object), key, valueOf(tag, payload));
        return 0;
    });
}

void scriptDynDefElem(Runtime* rt, std::int64_t mode, GCObject* object, std::int64_t keyTag, std::uint64_t key, std::int64_t tag,
                      std::uint64_t payload) noexcept
{
    guarded(*rt, SCRIPT_HERE, [&]() -> int {
        Ref<GCObject> k = toPropertyKey(*rt, valueOf(keyTag, key));
        defineMemberForNative(*rt, mode == 0 ? 3 : 0, static_cast<DynObject*>(object), k.get(), valueOf(tag, payload));
        return 0;
    });
}

void scriptDynClassSetup(Runtime* rt, std::int64_t hasSuper, std::int64_t superTag, std::uint64_t super, std::int64_t ctorTag,
                         std::uint64_t ctor) noexcept
{
    guarded(*rt, SCRIPT_HERE, [&]() -> int {
        classSetupForNative(*rt, hasSuper != 0, valueOf(superTag, super), valueOf(ctorTag, ctor));
        return 0;
    });
}

GCObject* scriptDynForInKeys(Runtime* rt, std::int64_t tag, std::uint64_t payload) noexcept
{
    return guarded(*rt, SCRIPT_HERE, [&]() -> GCObject* { return forInKeysForNative(*rt, valueOf(tag, payload)); });
}

NativePair scriptDynForInNext(Runtime* rt, GCObject* keys, std::int64_t indexTag, std::uint64_t index) noexcept
{
    (void)rt;
    auto* array = static_cast<DynArray*>(keys);
    const std::size_t at = std::size_t(numberOf(valueOf(indexTag, index)));
    if (at >= array->size())
        return {0, std::uint64_t(kForInDone)};
    return take(array->at(at));
}

NativePair scriptDynGetSuper(Runtime* rt, GCObject* fn, std::int64_t thisTag, std::uint64_t self, GCObject* key) noexcept
{
    return guarded(*rt, SCRIPT_HERE, [&]() -> NativePair {
        DynObject* home = static_cast<DynFunction*>(fn)->homeObject.get();
        DynObject* base = home ? home->proto.get() : nullptr;
        if (!base)
            throwError(*rt, ErrorCode::TypeError, "'super' has no base object");
        return take(getPropertyOfObject(*rt, base, key, valueOf(thisTag, self)));
    });
}

NativePair scriptDynAppendIfLocal(Runtime* rt, std::int64_t ta, std::uint64_t a, std::int64_t tb, std::uint64_t b, std::int64_t lt,
                                  std::uint64_t local) noexcept
{
    return guarded(*rt, SCRIPT_HERE, [&]() -> NativePair {
        // The local, the copy native code holds and the one made here are the counts there are when nothing else sees the
        // string.
        if (ta == std::int64_t(Type::String) && lt == ta && local == a)
            return take(appendForLocal(*rt, valueOf(ta, a), valueOf(tb, b), 3));
        return take(add(*rt, valueOf(ta, a), valueOf(tb, b)));
    });
}

NativePair scriptDynAppendIfCell(Runtime* rt, std::int64_t ta, std::uint64_t a, std::int64_t tb, std::uint64_t b, GCObject* cell) noexcept
{
    return guarded(*rt, SCRIPT_HERE, [&]() -> NativePair {
        const Box* box = static_cast<const Box*>(cell);
        // The same three: the cell's, native code's copy and this one.
        if (ta == std::int64_t(Type::String) && box && detail::Access::ptr(box->value) == reinterpret_cast<GCObject*>(a))
            return take(appendForLocal(*rt, valueOf(ta, a), valueOf(tb, b), 3));
        return take(add(*rt, valueOf(ta, a), valueOf(tb, b)));
    });
}

NativePair scriptDynAppendLocal(Runtime* rt, std::int64_t ta, std::uint64_t a, std::int64_t tb, std::uint64_t b) noexcept
{
    return guarded(*rt, SCRIPT_HERE, [&]() -> NativePair { return take(appendForLocal(*rt, valueOf(ta, a), valueOf(tb, b), 2)); });
}

NativePair scriptDynFromCharCode(Runtime* rt, std::int64_t tag, std::uint64_t payload) noexcept
{
    return guarded(*rt, SCRIPT_HERE, [&]() -> NativePair {
        const Char c = Char(toUint32(toNumber(*rt, valueOf(tag, payload))) & 0xffff);
        if (c < 128) {
            Ref<GCObject>& cached = rt->asciiChars[c];
            if (!cached)
                cached.reset(rt->ctx.intern(StringView(&c, 1)));
            return take(detail::Access::make(Type::String, cached.get()));
        }
        return take(rt->str(StringView(&c, 1)));
    });
}

NativePair scriptDynToObject(Runtime* rt, std::int64_t tag, std::uint64_t payload) noexcept
{
    return guarded(*rt, SCRIPT_HERE, [&]() -> NativePair { return take(toObject(*rt, valueOf(tag, payload))); });
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
    const NativePair out = scriptJitCall5(&rt, code.jitEntry, reinterpret_cast<std::uint64_t>(fn), std::uint64_t(thisValue.type()),
                                          payloadOf(thisValue), reinterpret_cast<std::uint64_t>(args), argc);
    const std::uint64_t results[2] = {out.payload, out.tag};
    raisePending(rt);
    return resultValue(results);
}

struct CallbackLoop::Session
{
    Frame& marker;
    NativeRun run;
    explicit Session(Runtime& rt) : marker(rt.acquireFrame(rt.frames.size())), run(rt, marker, true)
    {
        armNativeRun(rt, reinterpret_cast<std::uintptr_t>(__builtin_frame_address(0)));
    }
};

CallbackLoop::CallbackLoop(Runtime& rt, const Value& callee) : rt_(rt), callee_(callee)
{
    DynFunction* fn = asFunction(callee);
    if (fn && fn->fnKind == FnKind::Bytecode && !fn->isClassConstructor && rt.ctx.options.jit.mode != JitOptions::Mode::Off)
        fn_ = fn;
}

CallbackLoop::~CallbackLoop()
{
    delete session_;
}

Value CallbackLoop::slow(const Value& thisValue, std::initializer_list<Value> args)
{
    ContextImpl::ArgBuffer buffer(rt_.ctx);
    buffer.args.assign(args.begin(), args.end());
    return callValue(rt_, callee_, thisValue, buffer.args);
}

Value CallbackLoop::operator()(const Value& thisValue, std::initializer_list<Value> args)
{
    const void* entry = fn_ ? fn_->code->jitCallEntry : nullptr;
    if (!entry)
        return slow(thisValue, args);
    return enter(entry, std::int64_t(thisValue.type()), payloadOf(thisValue), args.begin(), args.size());
}

Value CallbackLoop::ofTwoNumbers(double a, double b)
{
    const void* entry = fn_ ? fn_->code->jitCallEntry : nullptr;
    if (!entry)
        return slow(Value::undefined(), {Value(a), Value(b)});
    const Value args[2] = {Value(a), Value(b)};
    return enter(entry, std::int64_t(Type::Undefined), 0, args, 2);
}

double CallbackLoop::numberOfTwoNumbers(double a, double b)
{
    const void* entry = fn_ ? fn_->code->jitCallEntry : nullptr;
    if (!entry || rt_.ctx.callDepth >= rt_.ctx.options.stackLimit)
        return toNumber(rt_, ofTwoNumbers(a, b));
    if (!session_)
        session_ = new Session(rt_);
    // Two values as native code reads them, a tag word and a payload each: numbers need no more than that.
    std::uint64_t args[4] = {std::uint64_t(Type::Double), 0, std::uint64_t(Type::Double), 0};
    std::memcpy(&args[1], &a, 8);
    std::memcpy(&args[3], &b, 8);
    ++rt_.ctx.callDepth;
    rt_.nativeTopFrame = nullptr;
    rt_.nativeTopPc = nullptr;
    const NativePair out = scriptJitCall5(&rt_, entry, reinterpret_cast<std::uint64_t>(fn_), std::uint64_t(Type::Undefined), 0,
                                          reinterpret_cast<std::uint64_t>(args), 2);
    --rt_.ctx.callDepth;
    if (!rt_.nativeKeepAlive.isUndefined())
        rt_.nativeKeepAlive = Value::undefined();
    if (rt_.nativeError != 0)
        raisePending(rt_);
    if (std::uint32_t(out.tag) == std::uint32_t(Type::Double)) {
        double d;
        std::memcpy(&d, &out.payload, 8);
        return d;
    }
    const std::uint64_t results[2] = {out.payload, out.tag};
    return toNumber(rt_, resultValue(results));
}

Value CallbackLoop::withArgs(const Value& thisValue, const Value* args, std::size_t count)
{
    const void* entry = fn_ ? fn_->code->jitCallEntry : nullptr;
    if (!entry) {
        ContextImpl::ArgBuffer buffer(rt_.ctx);
        buffer.args.assign(args, args + count);
        return callValue(rt_, callee_, thisValue, buffer.args);
    }
    return enter(entry, std::int64_t(thisValue.type()), payloadOf(thisValue), args, count);
}

Value CallbackLoop::enter(const void* entry, std::int64_t thisTag, std::uint64_t thisPayload, const Value* args, std::size_t count)
{
    if (!session_)
        session_ = new Session(rt_);
    struct Depth
    {
        ContextImpl& ctx;
        explicit Depth(Runtime& rt) : ctx(rt.ctx)
        {
            if (++ctx.callDepth > ctx.options.stackLimit) {
                --ctx.callDepth;
                throwError(rt, ErrorCode::StackOverflow, "maximum call depth exceeded");
            }
        }
        ~Depth() { --ctx.callDepth; }
    } depth(rt_);
    // Each call starts with no native frames published, as a fresh run would.
    rt_.nativeTopFrame = nullptr;
    rt_.nativeTopPc = nullptr;
    const NativePair out = scriptJitCall5(&rt_, entry, reinterpret_cast<std::uint64_t>(fn_), std::uint64_t(thisTag), thisPayload,
                                          reinterpret_cast<std::uint64_t>(args), count);
    const std::uint64_t results[2] = {out.payload, out.tag};
    if (!rt_.nativeKeepAlive.isUndefined())
        rt_.nativeKeepAlive = Value::undefined();
    // Before making a value of them: after a throw what came back is nothing.
    if (rt_.nativeError != 0)
        raisePending(rt_);
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
    // Parameters or locals the code takes to be numbers that are not, which the interpreter can have made so after a bail:
    // this code is no good for the call, nor for the next. What it is compiled to next knows where the bail was.
    const auto discard = [&code]() {
        code.jitEntry = nullptr;
        code.jitCallEntry = nullptr;
        code.jitCode = nullptr;
        code.jitHotness = 0;
        return false;
    };
    if (code.jitThisIsObject) {
        const Value& self = frame.thisValue;
        const bool nullish = code.sloppyThis && self.isNullish();
        if (!nullish && !asObject(self)) {
            code.jitBailedAt.push_back(0);
            return discard();
        }
    }
    const std::uint32_t params = std::uint32_t(std::max(code.arity, 0));
    for (std::uint32_t slot : code.jitNumberLocals) {
        // Not yet written is fine, for a local: nothing reads one of these before a number is stored. A parameter has been.
        const Type type = slot < frame.locals.size() ? frame.locals[slot].type() : Type::Undefined;
        if (type != Type::Double && (type != Type::Undefined || slot < params)) {
            if (slot < params && slot < 32)
                code.argsNotNumber |= 1u << slot;
            return discard();
        }
    }
    for (std::uint32_t slot : code.jitIntLocals) {
        if (slot >= frame.locals.size() || !frame.locals[slot].isDouble())
            continue;
        const double d = detail::Access::r8(frame.locals[slot]);
        if (!(d >= -2147483648.0 && d <= 2147483647.0) || double(std::int32_t(d)) != d || (d == 0 && std::signbit(d))) {
            if (slot < params && slot < 32)
                code.argsNotInt |= 1u << slot;
            return discard();
        }
    }

    // Native code reads a picture of the frame: the counts of what it will own move with the values, and those slots must
    // read as empty here from now on.
    const std::size_t valueBytes = frame.locals.size() * sizeof(Value);
    void* values = __builtin_alloca(valueBytes + 16);
    std::memcpy(values, static_cast<const void*>(frame.locals.data()), valueBytes);
    // An int32 local starts from its whole number, not from the bits of the double the interpreter holds it in.
    for (std::uint32_t slot : code.jitIntLocals) {
        if (slot >= frame.locals.size())
            continue;
        const std::int64_t whole = frame.locals[slot].isDouble() ? std::int32_t(detail::Access::r8(frame.locals[slot])) : 0;
        std::memcpy(static_cast<std::uint8_t*>(values) + std::size_t(slot) * 16 + 8, &whole, sizeof whole);
    }
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
    // Native code for non-strict code takes a nullish `this` to have been made the global object on entry.
    const Value self = code.sloppyThis && frame.thisValue.isNullish()
                           ? detail::Access::make(Type::Object, rt.globalObject.get())
                           : frame.thisValue;
    std::uint64_t ints[8] = {reinterpret_cast<std::uint64_t>(values), reinterpret_cast<std::uint64_t>(cells),
                             reinterpret_cast<std::uint64_t>(frame.fn), std::uint64_t(self.type()), payloadOf(self)};
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

namespace {

/// The code `fn` was built into goes in place for `code`: false when there is none, and then none is tried again.
bool installNative(Runtime& rt, const FunctionCode& code, const typed::IrFunction& fn, typed::NativeCode* compiled)
{
    if (!compiled) {
        code.jitRefused = true;
        if (rt.ctx.options.jit.dumpCode)
            rt.ctx.write(fromUtf8("jit: the code generator refused " + code.name + ": " + rt.jitBackend->functionRefusal() + "\n"), false);
        return false;
    }
    // Where each bail's values ended up, now that that is decided.
    for (const typed::IrCall& call : fn.calls) {
        if (!call.readsPlaces)
            continue;
        auto* info = static_cast<DeoptInfo*>(call.readsPlaces);
        const std::size_t first = std::size_t(call.abiArgs);
        info->places.resize(call.args.size() - first);
        info->placeIsFp.resize(call.args.size() - first);
        for (std::size_t i = first; i < call.args.size(); ++i) {
            info->places[i - first] = fn.location[std::size_t(call.args[i])];
            info->placeIsFp[i - first] = fn.vregIsFp[std::size_t(call.args[i])];
        }
        info->spillOffset = (fn.frameAreaBytes + 7u) & ~7u;
    }
    code.jitCode = compiled;
    rt.jitCodes.push_back(compiled);
    ++rt.jitCompiled;
    if (rt.ctx.options.jit.dumpCode)
        rt.ctx.write(fromUtf8("jit: " + code.name + " (" + std::to_string(compiled->codeBytes) + " bytes) at " +
                              std::to_string(reinterpret_cast<std::uintptr_t>(compiled->codeStart)) + " line " +
                              std::to_string(code.lineAt(0)) + " inlined " + std::to_string(fn.inlinedMethods) + "\n" +
                              rt.jitBackend->dump(compiled)),
                     false);
    if (!code.isClassConstructor)
        code.jitCallEntry = compiled->fastEntry;
    code.jitEntry = compiled->fastEntry;
    return true;
}

#if SCRIPT_ENABLE_JIT_THREAD
/// IR instructions below which a compile is quick enough to wait for: the call that made the function hot gets its code.
constexpr std::size_t kBackgroundIr = 3000;

bool backgroundWanted(const Runtime& rt, const typed::IrFunction& fn)
{
    const JitOptions& options = rt.ctx.options.jit;
    if (!options.background || options.mode == JitOptions::Mode::Eager || options.dumpCode)
        return false;
#if defined(SCRIPT_JIT_ENV_OVERRIDE)
    if (const char* v = std::getenv("SCRIPT_JIT_BACKGROUND"))
        return v[0] != '0' && fn.code.size() >= std::size_t(std::atol(v));
#endif
    return fn.code.size() >= kBackgroundIr;
}
#endif

}  // namespace

#if SCRIPT_ENABLE_JIT_THREAD
JitWorker::JitWorker(const typed::JitBackend& backend) : backend_(backend), thread_([this] { run(); }) {}

JitWorker::~JitWorker()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
        queue_.clear();
    }
    wake_.notify_one();
    thread_.join();
}

void JitWorker::submit(std::shared_ptr<JitJob> job)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push_back(std::move(job));
    }
    wake_.notify_one();
}

void JitWorker::run()
{
    for (;;) {
        std::shared_ptr<JitJob> job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [this] { return stop_ || !queue_.empty(); });
            if (stop_)
                return;
            job = std::move(queue_.front());
            queue_.pop_front();
        }
        // The function this was for may be gone already, with only this left holding the job.
        if (job.use_count() > 1)
            job->ok = backend_.buildFunction(job->fn, job->config, job->built);
        job->done.store(true, std::memory_order_release);
    }
}
#endif

bool compileNative(Runtime& rt, const FunctionCode& code)
{
    if (code.jitEntry)
        return true;
#if SCRIPT_ENABLE_JIT_THREAD
    if (code.jitJob) {
        if (!code.jitJob->done.load(std::memory_order_acquire))
            return false;
        const std::shared_ptr<JitJob> job = std::move(code.jitJob);
        code.jitJob = nullptr;
        if (!job->ok) {
            code.jitRefused = true;
            if (rt.ctx.options.jit.dumpCode)
                rt.ctx.write(fromUtf8("jit: the code generator refused " + code.name + ": " + job->built.refusal + "\n"), false);
            return false;
        }
        return installNative(rt, code, job->fn, rt.jitBackend->install(job->built));
    }
#endif
    if (code.jitRefused || !rt.jitBackend || !rt.jitBackend->available() ||
        rt.ctx.options.jit.mode == JitOptions::Mode::Off)
        return false;
#if SCRIPT_ENABLE_JIT_THREAD
    auto job = std::make_shared<JitJob>();
    typed::IrFunction& fn = job->fn;
#else
    typed::IrFunction fn;
#endif
    std::string why;
#if defined(SCRIPT_JIT_ENV_OVERRIDE)
    if (const char* skip = std::getenv("SCRIPT_JIT_SKIP"); skip && (std::string(",") + skip + ",").find("," + code.name + ",") != std::string::npos) {
        code.jitRefused = true;
        return false;
    }
    const auto started = std::chrono::steady_clock::now();
#endif
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
    config.savedRegs = offset(rt.bailRegs[0]);
    config.raise = reinterpret_cast<const void*>(&scriptDynRaise);
    config.poll = reinterpret_cast<const void*>(&scriptDynPoll);
#if defined(SCRIPT_JIT_ENV_OVERRIDE)
    if (std::getenv("SCRIPT_JIT_SIZES"))
        std::fprintf(stderr, "compile %s (%zu bytecode, %zu ir, gen %u)\n", code.name.c_str(), code.code.size(), fn.code.size(), unsigned(code.jitGeneration));
    const auto lowered = std::chrono::steady_clock::now();
#endif
#if SCRIPT_ENABLE_JIT_THREAD
    if (backgroundWanted(rt, fn)) {
        job->config = config;
        if (!rt.jitWorker)
            rt.jitWorker = std::make_unique<JitWorker>(*rt.jitBackend);
        code.jitJob = job;
        rt.jitWorker->submit(std::move(job));
        return false;
    }
#endif
    typed::NativeCode* compiled = rt.jitBackend->compileFunction(fn, config);
#if defined(SCRIPT_JIT_ENV_OVERRIDE)
    if (std::getenv("SCRIPT_JIT_SIZES")) {
        const auto ms = [](auto d) { return std::chrono::duration<double, std::milli>(d).count(); };
        std::fprintf(stderr, "compiled %s: lower %.1f ms, back end %.1f ms\n", code.name.c_str(), ms(lowered - started),
                     ms(std::chrono::steady_clock::now() - lowered));
    }
#endif
    return installNative(rt, code, fn, compiled);
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
        // A site in code inlined into a caller names the caller's call as its parent: one line for each.
        while (site >= 0 && std::size_t(site) < code->sourceSites.size() && shown < 16) {
            const typed::NativeCode::SourceSite& where = code->sourceSites[std::size_t(site)];
            const auto* function = static_cast<const FunctionCode*>(where.method);
            out += "    at ";
            out += function->name.empty() ? "<anonymous>" : function->name;
            out += " (";
            out += function->sourceName ? *function->sourceName : std::string("<unknown>");
            out += ":" + std::to_string(function->lineAt(where.pc)) + ")\n";
            ++shown;
            site = where.parent;
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
