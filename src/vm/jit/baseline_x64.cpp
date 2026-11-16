// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The baseline compiler (x86-64 / Linux). Frame state is the interpreter's at every instruction boundary; see jit.hpp.
//
// Fixed registers while compiled code runs (callee-saved, so calls into the runtime keep them):
//   rbx  the frame's registers (R)     r12  the VM     r13  the Frame
//   r14  kNumberTag                    r15  kNotCellMask       rbp  kDoubleOffset
//
// Every instruction either has a template with the interpreter's fast path in machine code, whose failure cases jump to
// a stub that runs the whole instruction in the interpreter (`stepInstruction`), or is run that way from the start.
// A template changes nothing before it knows it will not fall back.
#include "script/internal/vm/jit.hpp"

#include <cstddef>
#include <cstdio>
#include <string>
#include <unistd.h>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <set>
#include <algorithm>
#include <array>
#include <exception>
#include <utility>
#include <initializer_list>
#include <tuple>

#include "script/internal/stack_guard.hpp"
#include "script/internal/vm/bytecode.hpp"
#include "script/internal/vm/census.hpp"
#include "script/internal/vm/convert.hpp"
#include "script/internal/vm/object.hpp"
#include "opt_analysis.hpp"

#if defined(__x86_64__) && defined(__linux__)
#include "asm_x64.hpp"
#include "code_region.hpp"
#define SCRIPT_VM_BASELINE 1
#endif

namespace script {
namespace internal {
namespace vm {

#if defined(SCRIPT_VM_BASELINE)

namespace {

using namespace jit;

/// run(vm, frame, label, entry): set the registers compiled code keeps fixed and call `entry` with the frame in rsi and
/// `label` in rdx. A function's own entry (its prologue) ignores `label`; `osrEnter` jumps to it.
using Stub = std::uint64_t (*)(VM*, Frame*, void*, void*);

struct Stubs
{
    Stub run = nullptr;
    void* osrEnter = nullptr;
};

/// A function's way in: the frame in rsi, its registers (the frame's `R`) in rdi, so that nothing waits on the frame's
/// store of them.
void emitPrologue(X64& a)
{
    a.push(RBX);
    a.push(R13);
    a.aluI(5, RSP, 8);
    a.mov(R13, RSI);
    a.mov(RBX, RDI);
}

const Stubs& stubs()
{
    static Stubs result = [] {
        Stubs st;
        X64 a;
        a.push(RBP);
        a.push(R12);
        a.push(R14);
        a.push(R15);
        a.aluI(5, RSP, 8);
        a.mov(R12, RDI);
        a.movImm(R14, kNumberTag);
        a.movImm(R15, kNotCellMask);
        a.movImm(RBP, kDoubleOffset);
        a.mov(RAX, RCX);
        a.mov(RDI, mem(RSI, std::int32_t(offsetof(Frame, R))));
        a.callReg(RAX);
        a.aluI(0, RSP, 8);
        a.pop(R15);
        a.pop(R14);
        a.pop(R12);
        a.pop(RBP);
        a.ret();
        a.align(16);
        const std::int32_t osr = a.here();
        emitPrologue(a);
        a.jmp(RDX);
        CodeSpan span = allocateCode(a.buf.size());
        if (!span)
            return st;
        std::memcpy(span.rw, a.buf.data(), a.buf.size());
        st.run = reinterpret_cast<Stub>(span.rx);
        st.osrEnter = span.rx + osr;
        return st;
    }();
    return result;
}

Stub entryStub()
{
    return stubs().run;
}

/// Where compiled code goes on after a handler or a jump the interpreter took: the top frame's pc.
extern "C" void* scriptVmJitTarget(VM* vm)
{
    Frame* fr = vm->frameTop - 1;
    return fr->code->tier1->addressFor(std::uint32_t(fr->pc - fr->code->code.data()));
}

/// A C++ exception cannot cross compiled frames (they carry no unwind information): it is parked here and the compiled
/// code leaves with the exception sentinel, as it does for a script exception; every frame that sees the sentinel
/// finds the parked exception and goes on leaving, and `enterTier1` throws it again.
thread_local std::exception_ptr pendingException;

#if defined(SCRIPT_JIT_ENV_OVERRIDE)
/// SCRIPT_VM_STEPS=1: at exit, how often compiled code ran each opcode in the interpreter.
std::uint64_t stepCounts[std::size_t(Op::Count)];
std::map<std::pair<CodeBlock*, std::uint32_t>, std::uint64_t> stepSites;
const bool stepCensus = [] {
    if (!std::getenv("SCRIPT_VM_STEPS"))
        return false;
    std::atexit([] {
        for (std::size_t k = 0; k < std::size_t(Op::Count); ++k)
            if (stepCounts[k])
                std::fprintf(stderr, "step %-20s %14llu\n", opName(Op(k)), static_cast<unsigned long long>(stepCounts[k]));
        for (auto& e : stepSites)
            if (e.second > 100000)
                std::fprintf(stderr, "site %s line %u %s: %llu\n", e.first.first->name ? "fn" : "anon",
                             e.first.first->lines.size() > e.first.second ? e.first.first->lines[e.first.second] : 0,
                             opName(Op(e.first.first->code[e.first.second].op)), static_cast<unsigned long long>(e.second));
    });
    return true;
}();
#endif

extern "C" std::uint64_t scriptVmJitStep(VM* vm, std::uint32_t index)
{
#if defined(SCRIPT_JIT_ENV_OVERRIDE)
    if (stepCensus) {
        Frame* fr = vm->frameTop - 1;
        ++stepCounts[fr->code->code[index].op];
        if (std::getenv("SCRIPT_VM_STEPS")[0] == '2')
            ++stepSites[{fr->code, index}];
    }
#endif
    try {
        return stepInstruction(*vm, index);
    } catch (...) {
        pendingException = std::current_exception();
        return 0;
    }
}

/// A call made from compiled code came back with the exception sentinel.
extern "C" std::uint64_t scriptVmJitThrow(VM* vm, std::uint32_t index)
{
    if (pendingException)
        return 0;
    try {
        return stepThrow(*vm, index);
    } catch (...) {
        pendingException = std::current_exception();
        return 0;
    }
}

/// A back edge in optimized code: the interpreter's interrupt check, without the jump. 1 when it threw.
extern "C" int scriptVmJitTick(VM* vm)
{
    vm->ticks = 4096;
    if (vm->ctx.interruptAsked()) {
        vm->throwError(ErrorCode::Interrupted, "execution was interrupted");
        return 1;
    }
    return 0;
}

/// Take optimized code out of use for new calls and entries: what it was told is no longer true. Frames already in it
/// finish there.
void retireTier2(VM& vm, CodeBlock* code) noexcept
{
    if (code->tier1 && code->tier2 && code->tier1->entry == code->tier2->entry)
        code->tier1->entry = code->tier1->rx;
    code->tier2Counter = vm.tier2Threshold > 0 ? vm.tier2Threshold : 0x40000000;
    if (code->tier2Tries < 255)
        ++code->tier2Tries;
}

/// A guard of optimized code failed at instruction `index` (kind 0: it has not run, kind 1: it has). Records that, so that
/// the next compile does not guess there, and (kind 0) runs it in the interpreter.
extern "C" std::uint64_t scriptVmOptExit(VM* vm, std::uint32_t index, int kind)
{
    try {
        Frame* fr = vm->frameTop - 1;
        CodeBlock* code = fr->code;
        if (kind == 2) {
            // Sites this code knew nothing of have run long enough: a new compile will know them (not a failure: no try
            // is used up).
            retireTier2(*vm, code);
            if (code->tier2Tries)
                --code->tier2Tries;
            return stepInstruction(*vm, index);
        }
        if (index < code->prof.size())
            code->prof[index] |= opt::kProfFailed;
#if defined(SCRIPT_JIT_ENV_OVERRIDE)
        if (std::getenv("T2EXITS"))
            std::fprintf(stderr, "tier2 exit %s @%u kind %d\n", code->name ? toUtf8(static_cast<StringCell*>(code->name)->text()).c_str() : "anon", index, kind);
#endif
        SCRIPT_VM_CENSUS(Tier2Exit);
        // A guard that keeps failing in one place: the code was made for something that no longer happens there.
        bool again = false;
        if (Tier2Code* t = code->tier2) {
            if (t->siteExits.size() < code->code.size())
                t->siteExits.resize(code->code.size(), 0);
            if (index < t->siteExits.size() && t->siteExits[index] < 255)
                again = ++t->siteExits[index] >= 3;
        }
        if (++code->optExits > 40 || again)
            retireTier2(*vm, code);
        else if (code->tier2Counter > 300)
            code->tier2Counter = 300;  // a loop that left optimized code goes back in soon
        if (kind == 1)
            return kStepFall;
        return stepInstruction(*vm, index);
    } catch (...) {
        pendingException = std::current_exception();
        return 0;
    }
}

Tier2Code* compileOptimizedCode(VM& vm, CodeBlock* code);

/// The baseline code's counter ran out at the function's entry (`header` is ~0u) or a loop's back edge: compile the
/// optimizing tier if it is not there, and return the way into it from the loop, if there is one.
extern "C" void* scriptVmJitTier2(VM* vm, std::uint32_t header)
{
    try {
        Frame* fr = vm->frameTop - 1;
        CodeBlock* code = fr->code;
        if (!code->tier1 || code->tier2Tries >= 5) {
            code->tier2Counter = 0x40000000;
            return nullptr;
        }
        const bool active = code->tier2 && code->tier1->entry == code->tier2->entry;
#if defined(SCRIPT_JIT_ENV_OVERRIDE)
        static int compiles = 0;
        if (!active && std::getenv("T2MAX") && compiles >= std::atoi(std::getenv("T2MAX"))) {
            code->tier2Counter = 0x40000000;
            return nullptr;
        }
        if (!active && std::getenv("T2ONLY")) {
            const std::string nm = code->name ? toUtf8(static_cast<StringCell*>(code->name)->text()) : std::string("anon");
            if (nm != std::getenv("T2ONLY")) {
                code->tier2Counter = 0x40000000;
                return nullptr;
            }
        }
#endif
        if (!active) {
            Tier2Code* t = compileOptimizedCode(*vm, code);
            if (!t) {
                code->tier2Counter = 0x40000000;
                code->tier2Tries = 5;
                return nullptr;
            }
            t->older = code->tier2;
            code->tier2 = t;
#if defined(SCRIPT_JIT_ENV_OVERRIDE)
            if (const char* dir = std::getenv("SCRIPT_VM_CODEDUMP")) {
                static int serial2 = 0;
                const std::string nm = code->name ? toUtf8(static_cast<StringCell*>(code->name)->text()) : std::string("anon");
                char at[32]; std::snprintf(at, sizeof at, "%p", static_cast<void*>(t->rx)); const std::string path = std::string(dir) + "/t2-" + std::to_string(++serial2) + "-" + nm + "@" + at + ".bin";
                if (std::FILE* f = std::fopen(path.c_str(), "wb")) {
                    std::fwrite(t->rx, 1, t->bytes, f);
                    std::fclose(f);
                }
            }
#endif
            code->tier1->entry = t->entry;
            code->optExits = 0;
            // Optimized code may have been made before parts of the function had run: it makes way after those have run
            // a while, a few times, each given longer.
            code->tier2Budget = code->tier2Matured < 4 ? 20000 << (2 * code->tier2Matured) : 0x7fffffff;
            if (code->tier2Matured < 4)
                ++code->tier2Matured;
            SCRIPT_VM_CENSUS(Tier2Compiled);
        }
        code->tier2Counter = 0x40000000;
        return header == 0xFFFFFFFFu ? nullptr : code->tier2->osrAddress(header);
    } catch (...) {
        return nullptr;
    }
}

using HelperFn = int (*)(VM*, std::uint32_t);

/// An interpreter helper (see jit.hpp) with C++ exceptions parked as in `scriptVmJitStep`.
template <int (*F)(VM&, std::uint32_t)>
int helperThunk(VM* vm, std::uint32_t index)
{
    try {
        return F(*vm, index);
    } catch (...) {
        pendingException = std::current_exception();
        return 0;
    }
}

struct CallResult
{
    std::uint64_t a, b;
};

/// A `Call` from compiled code. {0, 0}: not handled here, nothing changed; run the instruction in the interpreter.
/// {result, 1}: a native function ran; the result is owned, or 0 for an exception. {frame, code}: a script function,
/// its frame pushed and ready; enter `code` with it.
CallResult callPrepare(VM& vm, std::uint32_t index)
{
    Frame* fr = vm.frameTop - 1;
    CodeBlock* code = fr->code;
    const Ins* ins = code->code.data() + index;
    JSValue* R = fr->R;
    const std::uint32_t base = ins->b, argc = ins->c;
    const JSValue callee = R[base];
    const bool isNew = ins->opcode() == Op::New;
    if (!isFunction(callee))
        return {0, 0};
    auto* f = static_cast<JSFunction*>(asObject(callee));
    if (isNew && ((f->flags & JSFunction::kNative) || !f->code->constructor || f->h.shape != vm.fnProtoShape))
        return {0, 0};
    if (!(f->flags & JSFunction::kNative) && index < code->callProf.size())
        code->callProf[index] = f->code->uid;
    if (f->flags & JSFunction::kNative) {
        fr->pc = ins + 1;
        Rare* r = f->rare();
        vm.stackTop = R + code->nregs;
        const JSValue saved = vm.newTarget;
        vm.newTarget = JSValue::undefined();
        const JSValue res = r->native(vm, R[base + 1], &R[base + 2], argc, r->nativeData);
        vm.newTarget = saved;
        return {res.bits, 1};
    }
    CodeBlock* c = f->code;
    if (c->flags & (CodeBlock::kGenerator | CodeBlock::kAsync | CodeBlock::kAsyncGen | CodeBlock::kNeedsArgs) ||
        ((c->flags & CodeBlock::kClassConstructor) && !isNew))
        return {0, 0};
    if (!c->tier1) {
        if (c->tier1Failed || !vm.tier1Threshold || ++c->hotness < vm.tier1Threshold)
            return {0, 0};
        if (tierUpBlock(vm, f, c) != c)
            return {0, 0};
        compileTier1(vm, c);
        if (!c->tier1)
            return {0, 0};
    }
    JSValue* newR = R + base + 1;
    if (vm.ticks <= 1 || vm.frameTop >= vm.frameLimit || newR + c->nregs + 8 > vm.stackEnd || nativeStackLow())
        return {0, 0};
    fr->pc = ins + 1;
    --vm.ticks;
    if (isNew) {
        const JSValue pv = *f->slotAt(2);
        JSObject* made = vm.newObject(isObject(pv) ? asObject(pv) : vm.protos.object, thisSlotsOf(f->code));
        const JSValue old = R[base + 1];
        R[base + 1] = toJSValue(made);
        release(old);
    }
    if (!(c->flags & (CodeBlock::kStrict | CodeBlock::kArrow | CodeBlock::kNoThis)) && newR[0].isNullish()) {
        const JSValue old = newR[0];
        newR[0] = toJSValue(vm.global);
        retain(newR[0]);
        release(old);
    }
    const std::uint32_t keep = 1 + (argc < c->nparams ? argc : c->nparams);
    if (newR + c->nregs + 1 > vm.stackInit) [[unlikely]]
        vm.fillStack(newR + c->nregs + 1);  // one past: compiled code's test of the stack wants it
    {
        // What the frame sits on may be the caller's dead temporaries, or arguments the callee has no parameter for.
        JSValue* callerEnd = R + code->nregs;
        const std::uint32_t overlap =
            callerEnd > newR ? std::uint32_t(std::min<std::ptrdiff_t>(callerEnd - newR, c->nregs)) : 0;
        for (std::uint32_t i = keep; i < overlap; ++i) {
            const JSValue v = newR[i];
            newR[i] = JSValue::undefined();
            release(v);
        }
        for (std::uint32_t i = std::max(keep, overlap); i < 1 + argc && i < c->nregs; ++i) {
            const JSValue v = newR[i];
            newR[i] = JSValue::undefined();
            release(v);
        }
    }
    *vm.frameTop++ =
        Frame{c, f, nullptr, newR, std::uint16_t(ins->a), argc, isNew, true, isNew ? callee : JSValue::undefined()};
    return {reinterpret_cast<std::uint64_t>(vm.frameTop - 1),
            reinterpret_cast<std::uint64_t>(c->tier1->entry)};
}

/// `this` for `new f(...)` from compiled code: an object inheriting from `f.prototype` (an ordinary function: the property
/// is its third slot). Owned; 0 with the exception parked.
extern "C" std::uint64_t scriptVmJitNewThis(VM* vm, JSFunction* f)
{
    try {
        const JSValue pv = *f->slotAt(2);
        return toJSValue(vm->newObject(isObject(pv) ? asObject(pv) : vm->protos.object, thisSlotsOf(f->code))).bits;
    } catch (...) {
        pendingException = std::current_exception();
        return 0;
    }
}

/// `s += y` where the register holds the only count of a string with room for `y`'s characters: they are copied in
/// place. Allocates nothing, so it needs no frame state; false, with nothing done, for anything else.
extern "C" bool scriptVmJitAppendInPlace(JSValue* slot, std::uint64_t yBits) noexcept
{
    const JSValue y{yBits};
    if (!isString(*slot) || !isString(y) || *slot == y)
        return false;
    StringCell* s = asString(*slot);
    StringCell* t = asString(y);
    if (s->interned || s->rc != 1 || (s->bits & StringCell::kOutOfLine) || (t->bits & StringCell::kOutOfLine))
        return false;
    const std::size_t length = std::size_t(s->length) + t->length;
    if (s->capacity() < length)
        return false;
    // Mostly a character or a few: copied here rather than by the library.
    Char* to = s->mutableChars() + s->length;
    const Char* from = t->mutableChars();
    if (t->length <= 8) {
        for (std::uint32_t k = 0; k < t->length; ++k)
            to[k] = from[k];
    } else {
        std::memcpy(to, from, std::size_t(t->length) * sizeof(Char));
    }
    s->length = std::uint32_t(length);
    s->hash_ = 0;
    return true;
}

/// A native function called from compiled code, the frame's `pc` and the stack top already set. Owned result, or 0 with
/// the exception pending (a C++ one parked).
extern "C" std::uint64_t scriptVmJitNative(VM* vm, JSFunction* f, JSValue* thisAndArgs, std::uint32_t argc)
{
    try {
        Rare* r = f->rare();
        const JSValue saved = vm->newTarget;
        vm->newTarget = JSValue::undefined();
        const JSValue res = r->native(*vm, thisAndArgs[0], thisAndArgs + 1, argc, r->nativeData);
        vm->newTarget = saved;
        return res.bits;
    } catch (...) {
        pendingException = std::current_exception();
        return 0;
    }
}

extern "C" CallResult scriptVmJitCall(VM* vm, std::uint32_t index)
{
    try {
        return callPrepare(*vm, index);
    } catch (...) {
        pendingException = std::current_exception();
        return {0, 1};
    }
}

/// A cell whose count reached zero in compiled code.
extern "C" void scriptVmJitFree(CellPtr cell)
{
    detail::destroyUnreferenced(cell);
}

/// Let go of `count` registers from `first` and leave undefined in them.
extern "C" void scriptVmJitClear(JSValue* first, std::uint32_t count)
{
    for (std::uint32_t i = 0; i < count; ++i) {
        const JSValue v = first[i];
        first[i] = JSValue::undefined();
        release(v);
    }
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
constexpr std::int32_t kFrameFn = std::int32_t(offsetof(Frame, fn));
constexpr std::int32_t kFrameCode = std::int32_t(offsetof(Frame, code));
constexpr std::int32_t kFramePc = std::int32_t(offsetof(Frame, pc));
constexpr std::int32_t kFrameR = std::int32_t(offsetof(Frame, R));
constexpr std::int32_t kFrameRetDst = std::int32_t(offsetof(Frame, retDst));
constexpr std::int32_t kFrameEntry = std::int32_t(offsetof(Frame, entry));
constexpr std::int32_t kFrameNewTarget = std::int32_t(offsetof(Frame, newTarget));
constexpr std::int32_t kFrameGen = std::int32_t(offsetof(Frame, gen));
static_assert(offsetof(Frame, argc) == offsetof(Frame, retDst) + 4, "retDst and argc are stored together");
static_assert(offsetof(Frame, entry) == offsetof(Frame, construct) + 1, "construct and entry are stored together");
constexpr std::int32_t kVmStackEnd = std::int32_t(offsetof(VM, stackEnd));
constexpr std::int32_t kVmStackTop = std::int32_t(offsetof(VM, stackTop));
constexpr std::int32_t kVmStackInit = std::int32_t(offsetof(VM, stackInit));
constexpr std::int32_t kVmFrameCapacity = std::int32_t(offsetof(VM, frameCapacity));
constexpr std::int32_t kVmFrameBase = std::int32_t(offsetof(VM, frameBase));
constexpr std::int32_t kVmGlobalEpoch = std::int32_t(offsetof(VM, globalEpoch));
constexpr std::int32_t kVmStackFloor = std::int32_t(offsetof(VM, jitStackFloor));
constexpr std::int32_t kFnCode = std::int32_t(offsetof(JSFunction, code));
constexpr std::int32_t kFnFlags = std::int32_t(offsetof(JSFunction, flags));
constexpr std::int32_t kCbTier1 = std::int32_t(offsetof(CodeBlock, tier1));
constexpr std::int32_t kCbUid = std::int32_t(offsetof(CodeBlock, uid));
constexpr std::int32_t kCbConstructor = std::int32_t(offsetof(CodeBlock, constructor));
constexpr std::int32_t kCbThisSlots = std::int32_t(offsetof(CodeBlock, thisSlots));
constexpr std::int32_t kVmFnProtoShape = std::int32_t(offsetof(VM, fnProtoShape));
constexpr std::int32_t kCbFlags = std::int32_t(offsetof(CodeBlock, flags));
constexpr std::int32_t kCbNparams = std::int32_t(offsetof(CodeBlock, nparams));
constexpr std::int32_t kCbNregs = std::int32_t(offsetof(CodeBlock, nregs));
constexpr std::int32_t kTierEntry = std::int32_t(offsetof(Tier1Code, entry));
constexpr std::int32_t kFrameArgc = std::int32_t(offsetof(Frame, argc));
constexpr std::int32_t kFrameConstruct = std::int32_t(offsetof(Frame, construct));
static_assert(offsetof(Frame, entry) == offsetof(Frame, construct) + 1, "a frame push writes both flags in one store");
constexpr std::int32_t kVmTicks = std::int32_t(offsetof(VM, ticks));
constexpr std::int32_t kVmFrameTop = std::int32_t(offsetof(VM, frameTop));
constexpr std::int32_t kVmFrameLimit = std::int32_t(offsetof(VM, frameLimit));
constexpr std::int32_t kArrayElems = std::int32_t(offsetof(JSArray, elems));
constexpr std::int32_t kArrayCapacity = std::int32_t(offsetof(JSArray, capacity));
constexpr std::int32_t kArrayLength = std::int32_t(offsetof(JSArray, length));
#pragma GCC diagnostic pop
constexpr std::int32_t kTypedData = std::int32_t(offsetof(JSTypedArray, data));
constexpr std::int32_t kTypedLength = std::int32_t(offsetof(JSTypedArray, length));
constexpr std::int32_t kTypedKind = std::int32_t(offsetof(JSTypedArray, kind));
constexpr std::int32_t kRareNative = std::int32_t(offsetof(Rare, native));
constexpr std::int32_t kRareData = std::int32_t(offsetof(Rare, nativeData));
constexpr std::int32_t kRareHome = std::int32_t(offsetof(Rare, home));
constexpr std::int32_t kGcKind = std::int32_t(offsetof(detail::GCObject, kind));
constexpr std::int32_t kStrBits = std::int32_t(offsetof(StringCell, bits));
constexpr std::int32_t kStrLength = std::int32_t(offsetof(StringCell, length));
constexpr std::int32_t kStrChars = std::int32_t(sizeof(StringCell));
constexpr std::int32_t kRareChildRoot = std::int32_t(offsetof(Rare, childRoot));
constexpr std::int32_t kShapeId = std::int32_t(offsetof(Shape, id));
constexpr std::int32_t kGsGetter = std::int32_t(offsetof(GetterSetter, getter));
constexpr std::int32_t kSuperHome = std::int32_t(offsetof(SuperCache, home));
constexpr std::int32_t kSuperSlot = std::int32_t(offsetof(SuperCache, slot));
constexpr std::int32_t kSuperValidity = std::int32_t(offsetof(SuperCache, validity));
constexpr std::int32_t kObjSlots = 16;
constexpr std::int32_t kObjInline = 24;
constexpr std::int32_t kBoxValue = 16;
constexpr std::int32_t kFnUpvals = std::int32_t(sizeof(JSFunction));
constexpr std::uint8_t kObjectKind = 0x80 | std::uint8_t(Kind::Object);
constexpr std::int32_t kHdrCls = 4, kHdrFlags = 5, kHdrAux = 6, kHdrKind = 7, kHdrRc = 8;
constexpr std::int32_t kValidityValid = 4;

/// A value of an inlined function (opt_inline.inc): a typed variable of the caller, a register of the caller's frame, a
/// slot of the inline area, or a constant.
struct IVal
{
    enum Kind : std::uint8_t { I32, F64, Box, Const };
    Kind kind = Const;
    std::int32_t web = -1;
    std::int32_t frameReg = -1;
    std::int32_t slot = -1;
    JSValue c = JSValue::undefined();
    JSValue known = JSValue::undefined();  ///< Box: the function it was when compiled, when it is one
    std::uint8_t hint = 0;                 ///< Box: profBit bits of what it has held
    std::uint32_t epoch = 0;               ///< Box: side effects before it was read (a later one may have let it go)
};

class Baseline
{
public:
    Baseline(VM& vm, CodeBlock* code)
        : vm_(vm), code_(code), n_(std::uint32_t(code->code.size())), at_(n_ + 1), offsets_(n_ + 1, ~0u)
    {
    }

    /// The optimizing tier for the function this was made for, given the analysis of it; null when there is no room.
    Tier2Code* optimize(const opt::Plan& plan, Tier1Code* base)
    {
        plan_ = &plan;
        base_ = base;
        return compileOptimized();
    }

    Tier1Code* compile()
    {
        emitPrologue(a_);
        if (vm_.tier2Threshold > 0)
            emitCounter(0, -1);
        for (std::uint32_t i = 0; i < n_;) {
            a_.bind(at_[i]);
            offsets_[i] = std::uint32_t(a_.here());
            emit(i);
            i += length(i);
        }
        a_.bind(at_[n_]);
        a_.int3();  // the code never falls off its end
        emitTails();
        CodeSpan span = allocateCode(a_.buf.size());
        if (!span) {
            for (ValidityCell* v : validities_)
                vm_.releaseValidity(v);
            return nullptr;
        }
        std::memcpy(span.rw, a_.buf.data(), a_.buf.size());
        auto* t = new Tier1Code;
        t->rx = span.rx;
        t->bytes = span.bytes;
        t->entry = t->rx;
        t->validities = std::move(validities_);
        t->labels = std::move(offsets_);
        return t;
    }

private:
    VM& vm_;
    CodeBlock* code_;
    std::uint32_t n_;
    X64 a_;
    std::vector<Label> at_;
    std::vector<std::uint32_t> offsets_;
    std::vector<ValidityCell*> validities_;
    std::vector<JSValue> held_;  ///< values optimized code compares against, each counted (moved into its Tier2Code)
    /// Keep `v` alive for as long as the optimized code is: its address is in the code.
    void holdValue(JSValue v)
    {
        vm::retain(v);
        held_.push_back(v);
    }
    /// The value frame register `reg` will hold when instruction `i` runs, if straight-line code before it in the same block
    /// loads it from a global and (optionally) one property of that: read now, to be guarded at run time. Undefined if not.
    /// What register `reg` likely holds as instruction `i` begins: the global the nearest instruction before it that writes
    /// the register read, as it is now. Calls and joins between are passed over: only for code that checks the guess.
    JSValue guessBefore(std::uint32_t i, unsigned reg) const
    {
        for (std::uint32_t j = i; j-- > 0;) {
            if (code_->code[j].opcode() == Op::Cache)
                continue;
            const opt::Effects e = opt::effectsOf(*code_, j);
            bool writes = false;
            for (std::uint16_t w : e.wr)
                writes = writes || w == reg;
            if (!writes)
                continue;
            const Ins& in = code_->code[j];
            if (in.opcode() == Op::GetGlobal && in.a == reg && !(j < skipDef_.size() && skipDef_[j])) {
                std::uint8_t attrs = 0;
                JSValue* slot = vm_.global->findOwn(vm_, code_->atoms[in.b], &attrs);
                return slot && !(attrs & attr::kAccessor) ? *slot : JSValue::undefined();
            }
            return JSValue::undefined();
        }
        return JSValue::undefined();
    }
    JSValue constantBefore(std::uint32_t i, unsigned reg, int depth = 0)
    {
        if (depth > 2)
            return JSValue::undefined();
        for (std::uint32_t j = i; j-- > 0;) {
            if (code_->code[j].opcode() == Op::Cache)
                continue;
            const Ins& in = code_->code[j];
            const opt::Effects e = opt::effectsOf(*code_, j);
            bool writes = false;
            for (std::uint16_t w : e.wr)
                writes = writes || w == reg;
            if (!writes) {
                if (!e.ok || plan_->isLeader[j] || in.opcode() == Op::Call || in.opcode() == Op::New)
                    return JSValue::undefined();
                continue;
            }
            if (in.opcode() == Op::GetGlobal && in.a == reg) {
                std::uint8_t attrs = 0;
                JSValue* slot = vm_.global->findOwn(vm_, code_->atoms[in.b], &attrs);
                return slot && !(attrs & attr::kAccessor) ? *slot : JSValue::undefined();
            }
            if (in.opcode() == Op::GetProp && in.a == reg && in.b != reg) {
                const JSValue base = constantBefore(j, in.b, depth + 1);
                if (!isObject(base))
                    return JSValue::undefined();
                std::uint8_t attrs = 0;
                JSValue* slot = asObject(base)->findOwn(vm_, code_->atoms[in.c], &attrs);
                return slot && !(attrs & attr::kAccessor) ? *slot : JSValue::undefined();
            }
            return JSValue::undefined();
        }
        return JSValue::undefined();
    }
    /// Records in the element profile of instruction `i` that it has seen `bit` (elemprof), with `tmp` as scratch.
    void noteElem(std::uint32_t i, std::uint8_t bit, Reg tmp)
    {
        if (code_->elemProf.empty())
            code_->elemProf.assign(code_->code.size(), 0);
        a_.movImm(tmp, reinterpret_cast<std::uint64_t>(&code_->elemProf[i]));
        a_.orI8(mem(tmp, 0), bit);
    }
    /// For each element kind `kinds[k]`, a jump (on RDX equal to it) to a stub that notes `bits[k]` and goes on to `to[k]`.
    void kindDispatch(std::uint32_t i, std::initializer_list<std::tuple<ElemKind, std::uint8_t, Label*>> kinds, Label& none)
    {
        std::vector<Label> stubs(kinds.size());
        std::size_t k = 0;
        for (const auto& [kind, bit, to] : kinds) {
            a_.aluI32(7, RDX, int(kind));
            a_.jcc(CE, stubs[k++]);
        }
        a_.jmp(none);
        k = 0;
        for (const auto& [kind, bit, to] : kinds) {
            a_.bind(stubs[k++]);
            noteElem(i, bit, RDX);
            a_.jmp(*to);
        }
    }
    /// Guards (RAX the array) that a store into a hole of it cannot be intercepted (VM::holeStoreCell), with `tmp` as
    /// scratch; false, emitting nothing, when it can be now.
    bool plainHoleGuard(Reg tmp, Label& fail)
    {
        JSObject* const ap = vm_.protos.array;
        ValidityCell* const cell = vm_.holeStoreCell();
        if (!cell)
            return false;
        hold(cell);
        a_.cmpI32(mem(RAX, 0), std::int32_t(vm_.rootShapeFor(ap)->id));
        a_.jcc(CNE, fail);
        a_.movImm(tmp, reinterpret_cast<std::uint64_t>(cell));
        a_.cmpI32(mem(tmp, kValidityValid), 0);
        a_.jcc(CE, fail);
        if (ap->cls() == ObjClass::Array) {
            a_.movImm(tmp, reinterpret_cast<std::uint64_t>(ap));
            a_.cmpI32(mem(tmp, kArrayLength), 0);
            a_.jcc(CNE, fail);
        }
        return true;
    }
    /// A plain object of shape `shape` with `aux` inline slots, made here (as `VM::newObject` does, counters and all) when
    /// the heap's current block has room and no collection is due; else `slow`. The object, owned, in rax; clobbers rcx,
    /// rdx, rsi. False when it cannot be done here at all.
    /// Whether `allocObject` can make an object with `aux` inline slots here.
    bool canAllocObject(std::uint8_t aux) const
    {
        return mem::LocalHeap::kCanPopInline && !vm_.ctx.options.memoryLimit && JSObject::bytesFor(aux) <= 256;
    }
    bool allocObject(std::uint32_t shape, std::uint8_t aux, Label& slow, bool shapeInR8 = false)
    {
        if (!canAllocObject(aux))
            return false;
        CellHeader h{};
        h.shape = shape;
        h.cls = std::uint8_t(ObjClass::Plain);
        h.flags = cellflags::kYoung;
        h.aux = aux;
        h.kind = std::uint8_t(0x80 | std::uint8_t(Kind::Object));
        allocCellHere(JSObject::bytesFor(aux), h, slow, shapeInR8);
        a_.mov(mem(RAX, kObjSlots), 0);
        return true;
    }
    /// An empty array of `Array.prototype`, its elements in place, made as `VM::newArray` does; else `slow`. The array,
    /// owned, in rax; clobbers rcx, rdx, rsi. False when it cannot be done here at all.
    bool allocArray(Label& slow)
    {
        if (!mem::LocalHeap::kCanPopInline || vm_.ctx.options.memoryLimit)
            return false;
        CellHeader h{};
        h.shape = vm_.rootShapeFor(vm_.protos.array)->id;
        h.cls = std::uint8_t(ObjClass::Array);
        h.flags = cellflags::kYoung | cellflags::kAllNumbers;
        h.kind = std::uint8_t(0x80 | std::uint8_t(Kind::Object));
        allocCellHere(sizeof(JSArray), h, slow, false);
        static_assert(offsetof(JSArray, capacity) == offsetof(JSArray, length) + 4, "length and capacity in one store");
        a_.mov(mem(RAX, kObjSlots), 0);
        a_.lea(RDX, mem(RAX, std::int32_t(offsetof(JSArray, small))));
        a_.mov(mem(RAX, kArrayElems), RDX);
        a_.movImm(RDX, std::uint64_t(JSArray::kSmall) << 32);
        a_.mov(mem(RAX, kArrayLength), RDX);
        return true;
    }
    /// A cell of `bytes` with header `h` (its shape or-ed with r8 when `shapeInR8`) and a count of one, made here as
    /// `VM::allocCell` does, counters and all, when the heap's current block has room and no collection is due; else
    /// `slow`. The cell in rax; clobbers rcx, rdx, rsi.
    void allocCellHere(std::size_t bytes, const CellHeader& h, Label& slow, bool shapeInR8)
    {
        auto& ctx = vm_.ctx;
        auto at = [&](const void* field) {
            return std::int32_t(reinterpret_cast<const char*>(field) - reinterpret_cast<const char*>(&ctx));
        };
        auto atVm = [&](const void* field) {
            return std::int32_t(reinterpret_cast<const char*>(field) - reinterpret_cast<const char*>(&vm_));
        };
        // A collection due: the runtime's way.
        a_.movImm(RCX, reinterpret_cast<std::uint64_t>(&ctx));
        a_.mov(RAX, mem(RCX, at(&ctx.heapBytes)));
        a_.cmp(RAX, mem(RCX, at(&ctx.nextGcBytes)));
        a_.jcc(CAE, slow);
        a_.mov(RAX, mem(RCX, at(&ctx.liveObjects)));
        a_.alu(0, RAX, mem(RCX, at(&ctx.liveStrings)));
        a_.cmp(RAX, mem(RCX, at(&ctx.nextGcCells)));
        a_.jcc(CAE, slow);
        a_.mov(RAX, mem(R12, atVm(&vm_.liveCells)));
        a_.cmp(RAX, mem(R12, atVm(&vm_.cellBudget_)));
        a_.jcc(CAE, slow);
        // A slot of the current block: a free one, or one not carved yet.
        Label carve, got;
        a_.movImm(RDX, reinterpret_cast<std::uint64_t>(vm_.heap.currentBlockOf(bytes)));
        a_.mov(RDX, mem(RDX, 0));
        a_.mov(RAX, mem(RDX, std::int32_t(mem::LocalHeap::kBlockFreeOffset)));
        a_.test(RAX, RAX);
        a_.jcc(CE, carve);
        a_.mov(RSI, mem(RAX, 0));
        a_.mov(mem(RDX, std::int32_t(mem::LocalHeap::kBlockFreeOffset)), RSI);
        a_.jmp(got);
        a_.bind(carve);
        a_.mov(RAX, mem(RDX, std::int32_t(mem::LocalHeap::kBlockBumpOffset)));
        a_.cmp(RAX, mem(RDX, std::int32_t(mem::LocalHeap::kBlockEndOffset)));
        a_.jcc(CE, slow);
        a_.mov32(RSI, mem(RDX, std::int32_t(mem::LocalHeap::kBlockSlotBytesOffset)));
        a_.alu(0, mem(RDX, std::int32_t(mem::LocalHeap::kBlockBumpOffset)), RSI);
        a_.bind(got);
        a_.inc32(mem(RDX, std::int32_t(mem::LocalHeap::kBlockLiveOffset)));
        std::uint64_t word;
        std::memcpy(&word, &h, sizeof word);
        a_.movImm(RSI, word);
        if (shapeInR8)
            a_.or_(RSI, R8);
        a_.mov(mem(RAX, 0), RSI);
        a_.mov(mem(RAX, 8), 1);
        a_.aluI(0, mem(RCX, at(&ctx.heapBytes)), std::int32_t(bytes));
        a_.aluI(0, mem(RCX, at(&ctx.totalAllocated)), std::int32_t(bytes));
        a_.aluI(0, mem(RCX, at(&ctx.liveObjects)), 1);
        a_.aluI(0, mem(R12, atVm(&vm_.ownedBytes_)), std::int32_t(bytes));
        a_.aluI(0, mem(R12, atVm(&vm_.liveCells)), 1);
    }
    /// The value in `v` is a string (a cell of the core's own kind String), else `bad`.
    void needString(Reg v, Label& bad)
    {
        a_.test(v, R15);
        a_.jcc(CNE, bad);
        a_.testI8(mem(v, 7), 0x80);
        a_.jcc(CNE, bad);
        a_.cmpI8(mem(v, kGcKind), std::uint8_t(detail::CellKind::String));
        a_.jcc(CNE, bad);
    }
    /// The characters of the string in `s`, into `dst`; a rope not yet joined goes to `bad`.
    void stringChars(Reg s, Reg dst, Label& bad)
    {
        Label outOfLine, done;
        a_.testI8(mem(s, kStrBits), StringCell::kOutOfLine);
        a_.jcc(CNE, outOfLine);
        a_.lea(dst, mem(s, kStrChars));
        a_.jmp(done);
        a_.bind(outOfLine);
        a_.testI8(mem(s, kStrBits), StringCell::kRope);
        a_.jcc(CNE, bad);
        a_.mov(dst, mem(s, kStrChars));
        a_.bind(done);
    }
    /// The address of slot `index` of the global object, into rdx: its inline slots never change in number, so only a
    /// slot outside them needs a load (of where they are now).
    void globalSlot(std::uint32_t index)
    {
        JSObject* g = vm_.global;
        if (index < g->h.aux) {
            a_.movImm(RDX, reinterpret_cast<std::uint64_t>(&g->inl()[index]));
        } else {
            a_.movImm(RDX, reinterpret_cast<std::uint64_t>(&g->slots));
            a_.mov(RDX, mem(RDX, 0));
            a_.lea(RDX, mem(RDX, std::int32_t(8 * (index - g->h.aux))));
        }
    }
    /// The compiled code reads this cell: it must outlive the code, whatever the inline cache does next.
    void hold(ValidityCell* cell)
    {
        vm_.retainValidity(cell);
        validities_.push_back(cell);
    }
    Label step_, tail_, dispatch_, exit_, free_, call_, throw_, tier2_, tick_;
    std::deque<std::pair<std::uint32_t, Label>> throws_;
    struct Slow
    {
        std::uint32_t index;
        Label label;
        HelperFn helper;
        bool saved;  ///< the template has already saved the variables (and a C call since has clobbered the registers)
    };
    std::deque<Slow> slows_;

    // ---- the optimizing compiler's side (opt_emit.inc); `plan_` is null when this is the baseline compiler ----
    const opt::Plan* plan_ = nullptr;
    Tier1Code* base_ = nullptr;        ///< the baseline code a failed guard goes back to
    std::int32_t spill_ = 0;           ///< bytes of stack for the typed variables, above the baseline frame
    std::vector<std::uint8_t> dirty_;  ///< per typed web: the frame's register is behind the variable
    /// What straight-line optimized code has already proven about a frame register: it holds an object of this shape
    /// (0: nothing known) with this many inline slots (-1: not known). Forgotten at block heads, when the register is written,
    /// and at any instruction the optimizing compiler does not do itself.
    std::vector<std::uint32_t> factShape_;
    std::vector<std::int32_t> factAux_;
    /// Also: more inline slots than this (an object's count never changes), and one of these shapes (0 ends the list).
    std::vector<std::int32_t> factInl_;
    std::vector<std::array<std::uint32_t, 4>> factSet_;
    /// And: a function of this code (null: not known). That cannot change while the register is not written, so a loop
    /// that writes none of them has the ones checked before it all through it (`loopFacts_`).
    std::vector<const CodeBlock*> factCode_;
    struct LoopFact
    {
        std::uint32_t from, to;  ///< the loop: its header and its last back edge
        unsigned reg;
        const CodeBlock* code;
    };
    std::vector<LoopFact> loopFacts_;
    std::map<std::uint32_t, Label> preLoop_;  ///< a loop header with checks made before it: where they start
    std::uint32_t factAt_ = 0;               ///< the instruction being emitted
    /// Validity cells checked since the facts were last forgotten.
    std::set<const ValidityCell*> factValid_;
    /// Slots (register, property index) seen holding a number: only a typed store (a number) writes a slot without the
    /// facts being forgotten, so a store over one has nothing to let go of.
    /// What it holds: 1 a number, 2 an int32.
    std::map<std::pair<unsigned, std::uint32_t>, std::uint8_t> factNum_;
    struct LoopNum
    {
        std::uint32_t from, to;
        unsigned reg;
        std::uint32_t index;
        bool int32;  ///< every store of the loop to such a slot stores an int32
    };
    std::vector<LoopNum> loopNums_;
    /// A frame register's object is an array (1), and one that may be stored to: neither frozen nor closed to growth (2).
    std::vector<std::uint8_t> factArr_;
    struct LoopArr
    {
        std::uint32_t from, to;
        unsigned reg;
        std::uint8_t bits;
    };
    std::vector<LoopArr> loopArrs_;
    /// A global read at the head of a read chain, read once before the loop into a stack slot (and checked to be an array
    /// when the chain indexes it); a `Math` call whose function is checked once before the loop.
    struct LoopGlobal
    {
        std::uint32_t from, to, read;
        std::int32_t slot;  ///< rsp-relative
        bool array;
        std::uint32_t taker;  ///< the chain's link that reads it
    };
    std::vector<LoopGlobal> loopGlobals_;
    struct LoopMath
    {
        std::uint32_t from, to, call;
    };
    std::vector<LoopMath> loopMaths_;
    /// A number read from a slot of an object of known shape that nothing in the loop stores to: read and unboxed before the
    /// loop (as its variable's kind) into a stack slot.
    struct LoopRead
    {
        std::uint32_t from, to, read;
        std::int32_t slot;  ///< rsp-relative
        bool dup = false;   ///< another read of the loop fills the slot
    };
    std::vector<LoopRead> loopReads_;
    /// A method read from a prototype for an inlined call, in a loop that changes neither the receiver, nor its shape, nor
    /// the chain, nor that slot: read and compared once before the loop; inside, its window slot gets the function as a
    /// constant and the call needs no look at it.
    struct LoopWin
    {
        std::uint32_t from, to, read;
        JSValue known;
    };
    std::vector<LoopWin> loopWins_;
    const LoopWin* hoistedWin(std::uint32_t read) const
    {
        for (const LoopWin& w : loopWins_)
            if (w.read == read && w.from <= factAt_ && factAt_ <= w.to)
                return &w;
        return nullptr;
    }
    bool calleeChecked_ = false;  ///< the inlined call being compiled has its callee checked already (before its loop)
    const LoopRead* hoistedRead(std::uint32_t read) const
    {
        for (const LoopRead& r : loopReads_)
            if (r.read == read && r.from <= factAt_ && factAt_ <= r.to)
                return &r;
        return nullptr;
    }
    std::int32_t hoistBase_ = -1;  ///< rsp-relative: the slots of `loopReads_` and `loopGlobals_`
    std::uint32_t hoistSlots_ = 0;  ///< of those, taken so far (every loop's its own)
    const LoopGlobal* hoistedGlobal(std::uint32_t read) const
    {
        for (const LoopGlobal& g : loopGlobals_)
            if (g.read == read && g.from <= factAt_ && factAt_ <= g.to)
                return &g;
        return nullptr;
    }
    bool hoistedMath(std::uint32_t call) const
    {
        for (const LoopMath& m : loopMaths_)
            if (m.call == call && m.from <= factAt_ && factAt_ <= m.to)
                return true;
        return false;
    }
    /// The object in frame register `reg` (in RAX) is an array the way `bits` says, else `bad`; proved for the code after.
    void provenArray(unsigned reg, std::uint8_t bits, Label& bad)
    {
        if (reg < factArr_.size() && (factArr_[reg] & bits) == bits)
            return;
        needObject(RAX, bad);
        a_.cmpI8(mem(RAX, kHdrCls), std::uint8_t(ObjClass::Array));
        a_.jcc(CNE, bad);
        if (bits & 2) {
            a_.testI8(mem(RAX, kHdrFlags), std::uint8_t(cellflags::kFrozen | cellflags::kNotExtensible));
            a_.jcc(CNE, bad);
        }
        if (reg < factArr_.size())
            factArr_[reg] |= bits;
    }
    /// A loop that nothing in calls out of the optimized code (`sideDoors_`) keeps every object's shape and every chain as
    /// they were at its start: the shapes of the objects in registers it does not write, and the chains its reads rely on,
    /// are checked before it and known inside it. Should a way out turn up in its code after all, the function is compiled
    /// again without that (`hoistBroken_`, then `noHoist_`).
    struct LoopShape
    {
        std::uint32_t from, to;
        unsigned reg;
        std::uint32_t shape;
        std::int32_t aux;  ///< its inline slot count (-1: not checked)
    };
    std::vector<LoopShape> loopShapes_;
    struct LoopValid
    {
        std::uint32_t from, to;
        ValidityCell* cell;
    };
    std::vector<LoopValid> loopValids_;
    std::uint64_t sideDoors_ = 0;  ///< code emitted so far that may run script or call into the runtime
    std::map<std::uint32_t, std::pair<std::uint32_t, std::uint64_t>> hoistWatch_;  ///< header: its end, `sideDoors_` there
public:
    std::set<std::uint32_t> noHoist_;
    std::set<std::uint32_t> hoistBroken_;
private:
    void sideDoor() { ++sideDoors_; }
    /// `bad` unless validity cell `c` still holds; nothing to do when it was checked already.
    void checkValidity(ValidityCell* c, Label& bad, bool remember = true)
    {
        if (factValid_.count(c))
            return;
        hold(c);
        a_.movImm(RCX, reinterpret_cast<std::uint64_t>(c));
        a_.cmpI32(mem(RCX, kValidityValid), 0);
        a_.jcc(CE, bad);
        if (remember)
            factValid_.insert(c);
    }
    void planLoopFacts();
    void emitLoopGuards(std::uint32_t h, bool covered = false);
    bool encloses(std::uint32_t from, std::uint32_t to, std::uint32_t h) const;
    bool coveredShape(std::uint32_t h, const LoopShape& x) const;
    bool coveredMath(std::uint32_t h, const LoopMath& x) const;
    bool coveredArr(std::uint32_t h, const LoopArr& x) const;
    bool coveredNum(std::uint32_t h, const LoopNum& x) const;
    bool coveredValid(std::uint32_t h, const LoopValid& x) const;
    bool guardsCovered(std::uint32_t h) const;
    /// A loop header whose checks leave out what enclosing loops check: the way in for a jump from elsewhere (an entry
    /// from the interpreter, the return from an interrupt), which makes those checks first.
    std::map<std::uint32_t, Label> preLoopFull_;
    /// A frame register that a loop (with its checks before it, so that nothing in it runs script or changes an array) only
    /// sets to elements of arrays it does not write, and only reads as the object of a property: it holds them without a
    /// count, as the arrays keep them. It is let go of and set to undefined before the loop (it is dead there), and counted
    /// again on the way out of the loop, and on every way out to the interpreter.
    struct Borrow
    {
        std::uint32_t from, to;
        unsigned reg;
    };
    std::vector<Borrow> borrows_;
    void planBorrows();
    bool borrowed(std::uint32_t at, unsigned reg) const
    {
        for (const Borrow& b : borrows_)
            if (b.reg == reg && b.from <= at && at <= b.to)
                return true;
        return false;
    }
    /// The registers held without a count at instruction `at`.
    std::vector<unsigned> borrowedAt(std::uint32_t at) const
    {
        std::vector<unsigned> regs;
        for (const Borrow& b : borrows_)
            if (b.from <= at && at <= b.to)
                regs.push_back(b.reg);
        return regs;
    }
    void ownRegs(const std::vector<unsigned>& regs)
    {
        for (unsigned r : regs) {
            a_.mov(RAX, R(r));
            retain(RAX);
        }
    }
    Label& loopEntry(std::uint32_t h)
    {
        if (auto f = preLoopFull_.find(h); f != preLoopFull_.end())
            return f->second;
        return preLoop_.at(h);
    }
    void forgetFacts()
    {
        std::fill(factShape_.begin(), factShape_.end(), 0u);
        std::fill(factAux_.begin(), factAux_.end(), -1);
        std::fill(factInl_.begin(), factInl_.end(), 0);
        std::fill(factSet_.begin(), factSet_.end(), std::array<std::uint32_t, 4>{});
        std::fill(factCode_.begin(), factCode_.end(), nullptr);
        factValid_.clear();
        factNum_.clear();
        for (const LoopNum& ln : loopNums_)
            if (ln.from <= factAt_ && factAt_ <= ln.to)
                factNum_[{ln.reg, ln.index}] = ln.int32 ? 2 : 1;
        std::fill(factArr_.begin(), factArr_.end(), std::uint8_t(0));
        for (const LoopArr& la : loopArrs_)
            if (la.from <= factAt_ && factAt_ <= la.to && la.reg < factArr_.size())
                factArr_[la.reg] = la.bits;
        for (const LoopFact& lf : loopFacts_)
            if (lf.from <= factAt_ && factAt_ <= lf.to && lf.reg < factCode_.size())
                factCode_[lf.reg] = lf.code;
        for (const LoopShape& ls : loopShapes_)
            if (ls.from <= factAt_ && factAt_ <= ls.to && ls.reg < factShape_.size()) {
                factShape_[ls.reg] = ls.shape;
                factAux_[ls.reg] = ls.aux;
            }
        for (const LoopValid& lv : loopValids_)
            if (lv.from <= factAt_ && factAt_ <= lv.to)
                factValid_.insert(lv.cell);
    }
    void forgetFact(unsigned r)
    {
        if (r < factShape_.size()) {
            factShape_[r] = 0;
            factAux_[r] = -1;
            factInl_[r] = 0;
            factSet_[r] = {};
            factCode_[r] = nullptr;
            factArr_[r] = 0;
            for (auto it = factNum_.lower_bound({r, 0}); it != factNum_.end() && it->first.first == r;)
                it = factNum_.erase(it);
        }
    }
    /// Make the object in frame register `reg` (left in RAX) be of shape `shape`, else `bad`. Proved for the code after it.
    /// Rax holds frame register `raxObj_`'s object while nothing has been emitted since `raxObjAt_` but what is known to
    /// leave rax and the frame alone; a jump target ends it.
    int raxObj_ = -1;
    std::int32_t raxObjAt_ = -1;
    bool raxHoldsObj(unsigned reg) const { return raxObj_ == int(reg) && a_.here() == raxObjAt_; }
    void keepRax(unsigned reg)
    {
        raxObj_ = int(reg);
        raxObjAt_ = a_.here();
    }
    /// Code emitted from `start` left rax and the frame as they were.
    void passRax(std::int32_t start)
    {
        if (raxObjAt_ == start)
            raxObjAt_ = a_.here();
    }
    void provenShape(unsigned reg, std::uint32_t shape, Label& bad)
    {
        if (!raxHoldsObj(reg))
            a_.mov(RAX, R(reg));
        if (factShape_[reg] == shape)
            return;
        needObject(RAX, bad);
        a_.cmpI32(mem(RAX, 0), std::int32_t(shape));
        a_.jcc(CNE, bad);
        factShape_[reg] = shape;
        factAux_[reg] = -1;
    }
    /// The address of slot `index` of the object of shape `shape` in frame register `reg`, in RDX (the object in RAX), `bad`
    /// when it is another. `aux` is the inline slot count the cache saw, plus one (0: unknown): with it the address is fixed.
    /// As `provenSlot`, for a read: the slot as a memory operand (on rax or rdx) instead of its address.
    Mem provenSlotMem(unsigned reg, std::uint32_t shape, std::uint32_t index, std::uint8_t aux, Label& bad)
    {
        provenShape(reg, shape, bad);
        if (aux == 0) {
            slotAddress(index);
            return mem(RDX, 0);
        }
        const std::int32_t inl = std::int32_t(aux) - 1;
        if (factAux_[reg] != inl) {
            a_.cmpI8(mem(RAX, kHdrAux), std::uint8_t(inl));
            a_.jcc(CNE, bad);
            factAux_[reg] = inl;
        }
        if (std::int32_t(index) < inl)
            return mem(RAX, kObjInline + std::int32_t(8 * index));
        a_.mov(RDX, mem(RAX, kObjSlots));
        return mem(RDX, std::int32_t(8 * (std::int32_t(index) - inl)));
    }
    void provenSlot(unsigned reg, std::uint32_t shape, std::uint32_t index, std::uint8_t aux, Label& bad)
    {
        provenShape(reg, shape, bad);
        if (aux == 0) {
            slotAddress(index);
            return;
        }
        const std::int32_t inl = std::int32_t(aux) - 1;
        if (factAux_[reg] != inl) {
            a_.cmpI8(mem(RAX, kHdrAux), std::uint8_t(inl));
            a_.jcc(CNE, bad);
            factAux_[reg] = inl;
        }
        if (std::int32_t(index) < inl) {
            a_.lea(RDX, mem(RAX, kObjInline + std::int32_t(8 * index)));
        } else {
            a_.mov(RDX, mem(RAX, kObjSlots));
            a_.lea(RDX, mem(RDX, std::int32_t(8 * (std::int32_t(index) - inl))));
        }
    }
    bool delivered_ = false;           ///< the template just emitted put a typed destination in its variable
    /// The int32 web whose value rax still holds from the instruction just emitted (-1: none); `carried_` is it as the
    /// current instruction began, when nothing can have jumped in between.
    std::int32_t raxHolds_ = -1, carried_ = -1;
    struct ExitReq
    {
        std::uint32_t index;
        int kind;  ///< 0: run the instruction in the interpreter; 1: go on after it (it has run)
        Label label;
        std::vector<unsigned> own;  ///< registers held without a count where the exit is taken (`borrows_`)
    };
    std::deque<ExitReq> exits_;
    std::map<std::uint64_t, Label*> exitIndex_;
    std::deque<std::pair<std::uint32_t, Label>> osrEntries_;
    std::deque<std::pair<std::uint32_t, Label>> tickStubs_;
    std::deque<Label> tickBack_;
    Label tailBase_, exitBase_, dispBase_, freeAll_, entryFail_;
    struct CounterStub
    {
        std::uint32_t index;
        std::int64_t target;  ///< a loop header (the way into optimized code), or -1 for the function's entry
        Label stub;
        Label back;
    };
    std::deque<CounterStub> counterStubs_;
    bool optimizing() const noexcept { return plan_ != nullptr; }
    Tier2Code* compileOptimized();
    void emitOpt(std::uint32_t i);
    bool emitTyped(std::uint32_t i);
    // inlining (opt_inline.inc)
    struct InlineCtx
    {
        std::uint32_t at = 0;  ///< the caller's instruction: where a failed guard sends it
        bool emit = false;     ///< the second pass; the first only decides
        bool sideEffect = false;
        std::uint32_t epoch = 0;
        std::int32_t nextSlot = 0;
        Label dummy;
        Label* fail(Baseline* b);
    };
    Mem inlineSlot(std::int32_t s) const;
    bool inlinable(const CodeBlock* C) const;
    IVal callerValue(std::uint32_t i, unsigned reg, std::size_t k, std::uint8_t hint) const;
    void inlineBits(const IVal& v, Reg dst);
    void inlineI32(const IVal& v, Reg dst, Label* bad);
    void inlineF64(const IVal& v, int x, Reg tmp, Label* bad);
    void inlineBoxed(const IVal& v);
    bool inlineCall(std::uint32_t i, bool decideOnly = false);
    bool inlineGetter(std::uint32_t i);
    bool inlineNew(std::uint32_t i);
    bool inlineTop(std::uint32_t i, CodeBlock* C, const IVal& callee, JSValue known, const std::vector<IVal>& args,
                   unsigned dst, bool decideOnly);
    // direct calls (opt_inline.inc)
    struct DirectCall
    {
        JSValue fn;
        CodeBlock* code;
        std::uint32_t globalIndex;  ///< the global's slot index
        bool thisSkipped;
        std::uint32_t read;  ///< the `GetGlobal` of the callee
        bool thisNull;       ///< the skipped `this` is null (else undefined)
    };
    /// A `JNotCallee` of a block `selfInline` made, whose callee is the global read before the jump to it: the read is
    /// skipped (and the `this` after it), the guard checks the global's slot and the frame's function, and the call the
    /// long way reads the global then.
    struct SelfGuard
    {
        JSValue fn;
        std::uint32_t globalIndex;
        std::uint32_t read;    ///< the `GetGlobal`
        std::int64_t thisAt;   ///< the skipped `LoadUndef`/`LoadNull` of `this` (-1: not skipped)
    };
    std::map<std::uint32_t, SelfGuard> selfGuards_;
    /// For each instruction, whether every way to it passed a self guard with nothing since that may run script: the
    /// global still holds the running function (`selfFn_`), so a guard or a direct call of it need not look again.
    std::vector<std::uint8_t> selfChecked_;
    JSValue selfFn_ = JSValue::undefined();
    void planSelfChecked();
    /// Number reads of a property that an earlier read in the block made of the same object, with nothing between that
    /// may change it: the later read takes the earlier one's value from its home slot (`reuseRead_`: later -> earlier;
    /// `saveRead_`: earlier reads whose value goes to the home slot too).
    std::map<std::uint32_t, std::uint32_t> reuseRead_;
    std::set<std::uint32_t> saveRead_;
    void planReadReuse();
    void saveReadValue(std::uint32_t i);
    /// Whether the read at `i` is one `inlineGetter` would take (a getter's body in place).
    bool inlineGetterSite(std::uint32_t i) const
    {
        const std::uint16_t cacheId = code_->code[i + 1].a;
        if (cacheId == kNoCache || cacheId >= code_->caches.size())
            return true;
        const InlineCache& ic = code_->caches[cacheId];
        return ic.shape == 0;
    }
    void emitSelfGuard(std::uint32_t i);
    /// Whether the `Call` at `i` is the long way of a guarded call run in place, which this code never reaches.
    bool fallbackOf(std::uint32_t i) const
    {
        for (const auto& [g, sg] : selfGuards_)
            if (opt::jumpTarget(*code_, g) == std::int64_t(i))
                return true;
        return false;
    }
    std::vector<std::int32_t> exitRedirect_;  ///< an exit at an instruction runs from here instead (nothing between has effects)
    /// Inlined calls whose window registers are not written: what the inlined body reads for each (by call, then operand).
    std::map<std::uint32_t, std::map<std::size_t, IVal>> windowVal_;
    std::map<std::uint32_t, std::pair<std::int32_t, unsigned>> windowRead_;  ///< a GetProp of a window: its slot, its receiver
    JSValue windowMethod(std::uint32_t i, std::int32_t slot, const CodeBlock* C, std::int64_t* read = nullptr) const;
    std::int32_t windowBase_ = -1;
    void planWindows();
    std::map<std::uint32_t, std::pair<std::uint32_t, std::uint32_t>> mathWin_;  ///< Math call: (its GetGlobal, its GetProp)
    void planMathCalls();
    void emitReadInto(std::uint32_t i, bool fromRax, Label& bad);
    void emitWindowRead(std::uint32_t i);
    std::map<std::uint32_t, DirectCall> directCalls_;
    std::vector<std::uint8_t> skipDef_;  ///< instructions whose result a direct call keeps out of the frame
    std::map<std::uint32_t, std::vector<std::pair<unsigned, JSValue>>> pendingAt_;  ///< what an exit there writes first
    void planDirectCalls();
    // read chains (opt_inline.inc)
    std::vector<std::uint8_t> chainRole_;  ///< 1: the result goes to the chain slot; 2: the receiver comes from it
    std::vector<std::int32_t> chainHead_;  ///< for an instruction inside a chain: where a failed guard restarts
    std::int32_t chainSlot_ = -1;          ///< rsp-relative
    std::int32_t chainLeft_ = -1;  ///< where the code was when a chain link last left its value in rax
    /// Where on the stack the last link's value is kept (the chain's slot, or the slot of a global read before the loop);
    /// -1 when only rax has it (the next link comes right after).
    std::int32_t chainFrom_ = -1;
    void planChains();
    bool regDeadAfter(std::uint32_t i, unsigned r);
    bool chainable(std::uint32_t i, bool asConsumer);
    void emitChain(std::uint32_t i);
    void emitDirectCall(std::uint32_t i);
    void writePending(std::uint32_t point);
    bool inlineDeliver(InlineCtx& cx, const IVal& v, unsigned dst);
    bool inlineBody(InlineCtx& cx, CodeBlock* C, const IVal& callee, JSValue known, const std::vector<IVal>& args,
                    IVal& result, int depth);
    std::int32_t inlineBase_ = -1;
    /// Optimized code: registers that only ever hold numbers and other plain values while it runs, so clearing one is a
    /// store. Not those a call passes arguments in (the callee may assign its parameters) nor with exception handlers.
    std::vector<std::uint8_t> plainRegs_;
    /// For each instruction, the frame registers that may hold a cell as it begins (flow-sensitive; empty: never reached).
    std::vector<std::vector<bool>> cellAt_;
    std::set<std::uint32_t> movedOut_;  ///< `LoadUndef`s whose register a move already let go of
    bool emitMove(std::uint32_t i);
    void findCells();
    void planTame();
    std::vector<std::uint8_t> tame_;  ///< per web: its NaNs box without the test (`planTame`)
    bool tame(std::int32_t w) const { return w >= 0 && std::size_t(w) < tame_.size() && tame_[std::size_t(w)]; }
    /// Whether register `reg` cannot hold a cell as instruction `i` begins.
    bool plainAt(std::uint32_t i, unsigned reg) const
    {
        if (!optimizing() || i >= cellAt_.size() || cellAt_[i].empty() || reg >= cellAt_[i].size())
            return false;
        return !cellAt_[i][reg];
    }
    std::map<std::uint32_t, std::vector<unsigned>> windowCleanup_;  ///< a direct call's slow path lets these go after it
    void findPlainRegs();  ///< where the slots of inlined calls start in the optimized frame (rsp-relative)
    void emitExits();
    void emitOptTails();
    // variables
    const opt::Web& web(std::int32_t w) const { return plan_->webs[std::size_t(w)]; }
    Mem home(std::int32_t w) const { return mem(RSP, 8 + 8 * web(w).slot); }
    bool inReg(std::int32_t w) const { return web(w).loc.kind == opt::Loc::Reg; }
    Reg gpr(std::int32_t w) const { return Reg(web(w).loc.reg); }
    int xmmOf(std::int32_t w) const { return web(w).loc.reg; }
    bool isI32(std::int32_t w) const { return web(w).rep == opt::Rep::I32; }
    Reg useI32(std::int32_t w, Reg tmp);
    void defI32(std::int32_t w, Reg src);
    int operandF64(std::int32_t w, int xtmp, Reg gtmp);
    void defF64(std::int32_t w, int x);
    void setConstI32(std::int32_t w, std::int32_t v);
    void spillToFrame(unsigned reg, std::int32_t w);
    void materializeReads(std::uint32_t i);
    void materializeLive(std::uint32_t i);
    void loadFromFrame(unsigned reg, std::int32_t w, Label& fail);
    void unboxRax(std::int32_t w, Label& fail, bool number = false);
    void loadTypedAfter(std::uint32_t i);
    void savePool(std::uint32_t i);
    void restorePool(std::uint32_t i);
    bool liveAcross(std::uint32_t i, std::int32_t w) const;
    bool plainReadable(std::uint32_t i) const;
    Label& exitLabel(std::uint32_t i, int kind);
    Label& exitBefore(std::uint32_t i) { return exitLabel(i, 0); }
    Label& exitAfter(std::uint32_t i) { return exitLabel(i, 1); }
    /// A site that knew nothing when this code was made: after a while the code makes way for one made with what the
    /// baseline code has learned since (exit kind 2: the instruction runs in the interpreter, nothing is marked failed).
    void immature(std::uint32_t i)
    {
        if (!optimizing())
            return;
        a_.movImm(RAX, reinterpret_cast<std::uint64_t>(&code_->tier2Budget));
        a_.dec32(mem(RAX, 0));
        a_.jcc(CE, exitLabel(i, 2));
    }
    void deliver(std::uint32_t i, unsigned dst);
    void deliverInt(std::uint32_t i, unsigned dst);
    void deliverDouble(std::uint32_t i, unsigned dst);
    void afterGeneric(std::uint32_t i);
    void int32OfWeb(std::int32_t w, Reg dst, std::uint32_t exitAt);
    void typedJump(std::uint32_t i, const Ins& in);
    void emitCounter(std::uint32_t i, std::int64_t target);

    std::uint32_t length(std::uint32_t i) const noexcept
    {
        return (i + 1 < n_ && code_->code[i + 1].opcode() == Op::Cache) ? 2 : 1;
    }
    static Mem R(unsigned r) { return mem(RBX, std::int32_t(r) * 8); }
    /// A label that runs instruction `i` in the interpreter and then goes on after it.
    Label& slow(std::uint32_t i, HelperFn helper = nullptr, bool saved = false)
    {
        sideDoor();
        slows_.push_back(Slow{i, Label{}, helper, saved});
        return slows_.back().label;
    }
    Label& exception(std::uint32_t i)
    {
        throws_.emplace_back(i, Label{});
        return throws_.back().second;
    }
    /// Run the instruction's helper from here; no template.
    void helperOp(std::uint32_t i, HelperFn fn)
    {
        sideDoor();
        savePool(i);
        a_.mov(RDI, R12);
        a_.movImm(RSI, i);
        a_.movImm(RAX, reinterpret_cast<std::uint64_t>(fn));
        a_.callReg(RAX);
        a_.aluI32(7, RAX, 1);
        a_.jcc(CNE, exception(i));
        restorePool(i);
        afterGeneric(i);
    }

    // ---- counting ----
    /// One more count for the value in `r`, if it is a cell.
    void retain(Reg r)
    {
        Label done;
        a_.test(r, R15);
        a_.jcc(CNE, done);
        a_.inc32(mem(r, kHdrRc));
        a_.bind(done);
    }
    /// One count fewer for the value in `r`; the cell is destroyed at zero. Clobbers rdi and, through the call, every
    /// caller-saved register.
    void release(Reg r)
    {
        Label done;
        a_.test(r, R15);
        a_.jcc(CNE, done);
        a_.dec32(mem(r, kHdrRc));
        a_.jcc(CNE, done);
        if (r != RDI)
            a_.mov(RDI, r);
        a_.call(free_);
        a_.bind(done);
    }
    /// R[dst] = the value in `v`, whose count the caller has taken; the old value is let go. Clobbers rdx, rdi.
    void write(unsigned dst, Reg v, bool oldIsNumber = false)
    {
        if (oldIsNumber || (dst < plainRegs_.size() && plainRegs_[dst])) {
            a_.mov(R(dst), v);
            return;
        }
        a_.mov(RDX, R(dst));
        a_.mov(R(dst), v);
        release(RDX);
    }

    // ---- value tests; a failed test jumps to `bad` ----
    void needInt(Reg v, Label& bad)
    {
        a_.cmp(v, R14);
        a_.jcc(CB, bad);
    }
    /// Both `x` and `y` are int32 (clobbers rdx).
    void needInts(Reg x, Reg y, Label& bad)
    {
        a_.mov(RDX, x);
        a_.and_(RDX, y);
        a_.cmp(RDX, R14);
        a_.jcc(CB, bad);
    }
    void needObject(Reg v, Label& bad)
    {
        a_.test(v, R15);
        a_.jcc(CNE, bad);
        a_.cmpI8(mem(v, kHdrKind), kObjectKind);
        a_.jcc(CNE, bad);
    }
    void needArray(Reg v, Label& bad)
    {
        needObject(v, bad);
        a_.cmpI8(mem(v, kHdrCls), std::uint8_t(ObjClass::Array));
        a_.jcc(CNE, bad);
    }
    /// The address of slot `index` of the object in rax, into rdx. Clobbers rcx, rsi.
    void slotAddress(std::uint32_t index)
    {
        Label out, done;
        a_.movzx8(RCX, mem(RAX, kHdrAux));
        a_.aluI32(7, RCX, std::int32_t(index));
        a_.jcc(CBE, out);
        a_.lea(RDX, mem(RAX, kObjInline + std::int32_t(8 * index)));
        a_.jmp(done);
        a_.bind(out);
        a_.mov(RDX, mem(RAX, kObjSlots));
        a_.movImm(RSI, index);
        a_.alu32(5, RSI, RCX);
        a_.lea(RDX, mem(RDX, RSI, 8));
        a_.bind(done);
    }

    struct PolySeen
    {
        std::uint32_t shape, index;
        JSObject* holder;
        ValidityCell* validity;
        std::uint8_t aux;  ///< as `PolyEntry::aux`
    };
    /// The value in slot `index` of `holder` (a prototype, whose chain a checked validity cell vouches for), into rax. Its
    /// inline slots never move, and its others move only when it gains a property, which changes the chain.
    void holderRead(const JSObject* holder, std::uint32_t index)
    {
        if (index < holder->h.aux) {
            a_.movImm(RAX, reinterpret_cast<std::uint64_t>(holder) + std::uint64_t(kObjInline) + 8u * index);
            a_.mov(RAX, mem(RAX, 0));
        } else {
            a_.movImm(RAX, reinterpret_cast<std::uint64_t>(holder));
            a_.mov(RDX, mem(RAX, kObjSlots));
            a_.mov(RAX, mem(RDX, std::int32_t(8 * (index - holder->h.aux))));
        }
    }
    /// The access of one entry, on the object in rax (of its shape), into rax.
    void polyEntryRead(const PolySeen& s, Label& bad, int factReg)
    {
        if (s.holder) {
            checkValidity(s.validity, bad, factReg >= 0);
            holderRead(s.holder, s.index);
            return;
        }
        if (!s.aux) {
            slotAddress(s.index);
            a_.mov(RAX, mem(RDX, 0));
            return;
        }
        const std::int32_t inl = std::int32_t(s.aux) - 1;
        const bool knownAux = factReg >= 0 && factAux_[unsigned(factReg)] == inl;
        if (std::int32_t(s.index) < inl) {
            // An inline slot: any object with more inline slots than the index has it there.
            if (!knownAux && !(factReg >= 0 && std::int32_t(s.index) < factInl_[unsigned(factReg)])) {
                a_.cmpI8(mem(RAX, kHdrAux), std::uint8_t(s.index));
                a_.jcc(CBE, bad);
                if (factReg >= 0)
                    factInl_[unsigned(factReg)] = std::int32_t(s.index) + 1;
            }
            a_.mov(RAX, mem(RAX, kObjInline + std::int32_t(8 * s.index)));
            return;
        }
        if (!knownAux) {
            a_.cmpI8(mem(RAX, kHdrAux), std::uint8_t(inl));
            a_.jcc(CNE, bad);
            if (factReg >= 0)
                factAux_[unsigned(factReg)] = inl;
        }
        a_.mov(RDX, mem(RAX, kObjSlots));
        a_.mov(RAX, mem(RDX, std::int32_t(8 * (std::int32_t(s.index) - inl))));
    }
    /// The slot one of `seen` names, of the value in rax, into rax; `bad` when it is not an object of one of their shapes.
    /// Shapes reached the same way (an own slot at the same place) share one load. `factReg` is the frame register the
    /// value came from (-1: none), whose proven shape spares the checks and which learns the shape when there is one.
    /// Clobbers rcx, rdx, rsi.
    void polyRead(const PolySeen* seen, int count, Label& bad, int factReg = -1)
    {
        if (factReg >= 0 && factShape_[unsigned(factReg)] != 0) {
            for (int k = 0; k < count; ++k)
                if (seen[k].shape == factShape_[unsigned(factReg)]) {
                    polyEntryRead(seen[k], bad, factReg);
                    return;
                }
        }
        // How entry k reaches its slot: entries that agree on it share the code.
        auto key = [&](int k) -> std::uint64_t {
            const PolySeen& s = seen[k];
            if (s.holder)
                return ~std::uint64_t(k);
            if (s.aux && std::int32_t(s.index) < std::int32_t(s.aux) - 1)
                return std::uint64_t(s.index) << 2 | 1;
            return std::uint64_t(s.index) << 10 | std::uint64_t(s.aux) << 2 | 2;
        };
        if (factReg >= 0 && factSet_[unsigned(factReg)][0] != 0) {
            // One of a few shapes, proven: when all of them are read the same way, that is the read.
            const auto& set = factSet_[unsigned(factReg)];
            int common = -1;
            bool same = true;
            for (std::uint32_t sh : set) {
                if (!sh)
                    break;
                int at = -1;
                for (int k = 0; k < count; ++k)
                    if (seen[k].shape == sh)
                        at = k;
                if (at < 0 || (common >= 0 && key(at) != key(common)))
                    same = false;
                else if (common < 0)
                    common = at;
            }
            if (same && common >= 0 && !seen[common].holder) {
                polyEntryRead(seen[common], bad, factReg);
                return;
            }
        }
        needObject(RAX, bad);
        bool one = true;
        for (int k = 1; k < count; ++k)
            one = one && seen[k].shape == seen[0].shape;
        if (one) {
            a_.cmpI32(mem(RAX, 0), std::int32_t(seen[0].shape));
            a_.jcc(CNE, bad);
            if (factReg >= 0 && factShape_[unsigned(factReg)] != seen[0].shape) {
                factShape_[unsigned(factReg)] = seen[0].shape;
                factAux_[unsigned(factReg)] = -1;
            }
            polyEntryRead(seen[0], bad, factReg);
            return;
        }
        int group[5];
        Label labels[5];
        int groups = 0;
        int leader[5];
        for (int k = 0; k < count; ++k) {
            group[k] = -1;
            for (int g = 0; g < groups; ++g)
                if (key(leader[g]) == key(k))
                    group[k] = g;
            if (group[k] < 0) {
                leader[groups] = k;
                group[k] = groups++;
            }
        }
        // The last entry falls through to its group's code, which comes first.
        const int first = group[count - 1];
        for (int k = 0; k + 1 < count; ++k) {
            a_.cmpI32(mem(RAX, 0), std::int32_t(seen[k].shape));
            a_.jcc(CE, labels[group[k]]);
        }
        a_.cmpI32(mem(RAX, 0), std::int32_t(seen[count - 1].shape));
        a_.jcc(CNE, bad);
        if (factReg >= 0 && factShape_[unsigned(factReg)] == 0) {
            std::array<std::uint32_t, 4> set{};
            int n = 0;
            bool fits = true;
            for (int k = 0; k < count; ++k) {
                bool dup = false;
                for (int m = 0; m < n; ++m)
                    dup = dup || set[std::size_t(m)] == seen[k].shape;
                if (dup)
                    continue;
                if (n == 4)
                    fits = false;
                else
                    set[std::size_t(n++)] = seen[k].shape;
            }
            if (fits)
                factSet_[unsigned(factReg)] = set;
        }
        Label got;
        for (int n = 0; n < groups; ++n) {
            const int g = n == 0 ? first : (n <= first ? n - 1 : n);
            a_.bind(labels[g]);
            polyEntryRead(seen[leader[g]], bad, -1);
            if (n + 1 < groups)
                a_.jmp(got);
        }
        a_.bind(got);
    }

    /// Both `x` and `y` are numbers (int32 or double), else jump to `bad`.
    void needNumber(Reg v, Label& bad)
    {
        a_.test(v, R14);
        a_.jcc(CE, bad);
    }
    /// The number in `v` (known to be one) as a double in xmm`x`; `tmp` is clobbered.
    void loadNumber(int x, Reg v, Reg tmp)
    {
        Label dbl, done;
        a_.cmp(v, R14);
        a_.jcc(CB, dbl);
        a_.cvtsi2sd32(x, v);
        a_.jmp(done);
        a_.bind(dbl);
        a_.mov(tmp, v);
        a_.sub(tmp, RBP);
        a_.movqToX(x, tmp);
        a_.bind(done);
    }
    /// The number in `v` as a double in xmm`x`, else `bad` (null: known to be a number). A double falls straight through;
    /// an int32 is converted out of line. `tmp` (not `v`) is clobbered.
    struct ColdInt
    {
        Label entry, back;
        int x;
        Reg v;
    };
    std::deque<ColdInt> coldInts_;
    /// An int32 variable that overflowed when stepped in place: stepped back, then out by `bad`.
    struct ColdUndo
    {
        Label entry;
        Reg r;
        std::int32_t step;
        Label* bad;
    };
    std::deque<ColdUndo> coldUndos_;
    void unboxDouble(int x, Reg v, Reg tmp, Label* bad)
    {
        a_.mov(tmp, v);
        a_.sub(tmp, RBP);
        if (bad)
            a_.jcc(CB, *bad);  // below the first double: not a number
        a_.cmp(v, R14);
        coldInts_.emplace_back();
        ColdInt& c = coldInts_.back();
        c.x = x;
        c.v = v;
        a_.jcc(CAE, c.entry);
        a_.movqToX(x, tmp);
        a_.bind(c.back);
    }
    /// The int32 a number in `src` converts to, into `dst`: a plain int32, or a double that truncates without going out of
    /// range (NaN and the rest go to `bad`). Clobbers rdx and xmm0.
    void int32From(Reg dst, Reg src, Label& bad)
    {
        Label dbl, done;
        a_.cmp(src, R14);
        a_.jcc(CB, dbl);
        a_.mov32(dst, src);
        a_.jmp(done);
        a_.bind(dbl);
        a_.test(src, R14);
        a_.jcc(CE, bad);
        a_.mov(RDX, src);
        a_.sub(RDX, RBP);
        a_.movqToX(0, RDX);
        // ToInt32 of a double that fits 64 bits is the low half of its truncation; NaN, infinities and what does not fit give
        // the "indefinite" value and are left to the helper.
        a_.cvttsd2si64(dst, 0);
        a_.movImm(RDX, std::uint64_t(1) << 63);
        a_.cmp(dst, RDX);
        a_.jcc(CE, bad);
        a_.mov32(dst, dst);
        a_.bind(done);
    }
    /// xmm0 as the number `JSValue::boxNumber` makes of it, in rax: an int32 when it is one, else a double.
    void boxNumber0()
    {
        Label asDouble, isInt, done;
        a_.cvttsd2si32(RDX, 0);
        a_.cvtsi2sd32(1, RDX);
        a_.ucomisd(0, 1);
        a_.jcc(CNE, asDouble);
        a_.jcc(CP, asDouble);
        a_.test32(RDX, RDX);
        a_.jcc(CNE, isInt);
        a_.movqFromX(RAX, 0);  // a zero is an int unless it is a negative zero
        a_.test(RAX, RAX);
        a_.jcc(CS, asDouble);
        a_.bind(isInt);
        a_.mov32(RAX, RDX);
        a_.or_(RAX, R14);
        a_.jmp(done);
        a_.bind(asDouble);
        boxDouble0();
        a_.bind(done);
    }
    /// xmm0 as a boxed double in rax; every NaN becomes the canonical one.
    void boxDouble0() { boxDouble(0); }
    static bool hasAvx()
    {
        static const bool avx = __builtin_cpu_supports("avx");
        return avx;
    }
    /// xmm`x` as a boxed double in rax; every NaN becomes the canonical one.
    void boxDouble(int x) { boxDoubleTo(RAX, x); }
    void boxDoubleTo(Reg dst, int x, bool tame = false)
    {
        Label ok;
        a_.movqFromX(dst, x);
        if (tame) {
            a_.add(dst, RBP);
            return;
        }
        a_.ucomisd(x, x);
        a_.jcc(CNP, ok);
        a_.movImm(dst, kCanonicalNaN);
        a_.bind(ok);
        a_.add(dst, RBP);
    }

    void callStep(std::uint32_t i)
    {
        a_.mov(RDI, R12);
        a_.movImm(RSI, i);
        a_.call(step_);
    }
    void emitStep(std::uint32_t i)
    {
        sideDoor();
        savePool(i);
        callStep(i);
        a_.cmpI(RAX, std::int32_t(kStepFall));
        a_.jcc(CNE, tail_);
        restorePool(i);
        afterGeneric(i);
    }
    /// For a taken backward jump: count down to the next look at the interrupt flag, in the interpreter when it is time.
    void tick(std::uint32_t i)
    {
        if (optimizing()) {
            // The optimizing code may not leave for the interpreter here (its variables are in registers): a stub makes the
            // call that looks at the flag, sets the count again and comes back.
            tickStubs_.emplace_back(i, Label{});
            Label& stub = tickStubs_.back().second;
            a_.mov32(RDX, mem(R12, kVmTicks));
            a_.aluI32(5, RDX, 1);
            a_.jcc(CE, stub);
            a_.mov32(mem(R12, kVmTicks), RDX);
            tickBack_.emplace_back(Label{});
            a_.bind(tickBack_.back());
            return;
        }
        a_.mov32(RDX, mem(R12, kVmTicks));
        a_.aluI32(5, RDX, 1);
        a_.jcc(CE, slow(i));
        a_.mov32(mem(R12, kVmTicks), RDX);
    }
    /// Go to instruction `target` from the jump at `i`.
    void go(std::uint32_t i, std::uint32_t target)
    {
        if (target <= i) {
            tick(i);
            if (!optimizing() && vm_.tier2Threshold > 0)
                emitCounter(i, target);
        }
        a_.jmp(at_[target]);
    }
    /// Jump to `target` if `cond` holds.
    void goIf(Cond cond, std::uint32_t i, std::uint32_t target)
    {
        if (target > i) {
            a_.jcc(cond, at_[target]);
            return;
        }
        Label skip;
        a_.jcc(Cond(cond ^ 1), skip);
        go(i, target);
        a_.bind(skip);
    }
    bool windowPlain(std::uint32_t i, unsigned reg) const
    {
        return optimizing() && ((reg < plainRegs_.size() && plainRegs_[reg]) || plainAt(i, reg));
    }
    /// The caller registers past a call's arguments, `r9` the callee's registers and `rsi` its register count: those it
    /// covers made undefined, a cell in one taking the long way. A plain one (dead, never a cell) is written whether
    /// covered or not.
    void clearWindow(std::uint32_t i, unsigned base, unsigned argc, unsigned window, Label& helper)
    {
        for (unsigned r = 1 + argc; r < window; ++r) {
            if (windowPlain(i, base + 1 + r)) {
                a_.mov(mem(R9, std::int32_t(8 * r)), std::int32_t(kUndefined));
                continue;
            }
            Label skip;
            a_.aluI32(7, RSI, std::int32_t(r));
            a_.jcc(CBE, skip);
            a_.mov(RDI, mem(R9, std::int32_t(8 * r)));
            a_.test(RDI, R15);
            a_.jcc(CE, helper);
            a_.mov(mem(R9, std::int32_t(8 * r)), std::int32_t(kUndefined));
            a_.bind(skip);
        }
    }
    void clearReg(unsigned r)
    {
        if (r < plainRegs_.size() && plainRegs_[r]) {
            a_.mov(R(r), std::int32_t(kUndefined));  // never a cell here (`plainRegs_`)
            return;
        }
        a_.mov(RDX, R(r));
        a_.mov(R(r), std::int32_t(kUndefined));
        release(RDX);
    }

    void emit(std::uint32_t i);
    void emitTails();
    void arith(std::uint32_t i, const Ins& in);
    void arithImm(std::uint32_t i, const Ins& in);
    void compare(std::uint32_t i, const Ins& in);
    void jumps(std::uint32_t i, const Ins& in);
    void strictCore(Label& eq, Label& neq, Label& bad);
    void strictJump(std::uint32_t i, const Ins& in);
    void strictValue(std::uint32_t i, const Ins& in);
    void getProp(std::uint32_t i, const Ins& in);
    void putProp(std::uint32_t i, const Ins& in);
    void getElem(std::uint32_t i, const Ins& in);
    void getSuperProp(std::uint32_t i, const Ins& in);
    void getterCall(std::uint32_t i, const Ins& in, const PolyEntry& e);
    void putElem(std::uint32_t i, const Ins& in);
    void ret(std::uint32_t i, const Ins& in, bool numberInRax = false);
    /// R11 = the frame after the top one (R13): from the address, so that nothing waits on the top's last store.
    void topFrame() { a_.lea(R11, mem(R13, std::int32_t(sizeof(Frame)))); }
    void call(std::uint32_t i, const Ins& in);
    void getGlobal(std::uint32_t i, const Ins& in);
    void putGlobal(std::uint32_t i, const Ins& in);
    void initProp(std::uint32_t i, const Ins& in);
};

void Baseline::emitTails()
{
    // One stub per template that can give up: run the instruction in the interpreter, then go on after it.
    for (std::size_t k = 0; k < slows_.size(); ++k) {
        Slow& s = slows_[k];
        a_.bind(s.label);
        if (!s.saved)
            savePool(s.index);
        if (s.helper) {
            a_.mov(RDI, R12);
            a_.movImm(RSI, s.index);
            a_.movImm(RAX, reinterpret_cast<std::uint64_t>(s.helper));
            a_.callReg(RAX);
            a_.aluI32(7, RAX, 1);
            a_.jcc(CNE, exception(s.index));
        } else {
            callStep(s.index);
            a_.cmpI(RAX, std::int32_t(kStepFall));
            a_.jcc(CNE, tail_);
        }
        restorePool(s.index);
        afterGeneric(s.index);
        a_.jmp(at_[s.index + length(s.index)]);
    }
    if (optimizing())
        emitOptTails();
    for (ColdUndo& u : coldUndos_) {
        a_.bind(u.entry);
        a_.aluI32(5, u.r, u.step);
        a_.jmp(*u.bad);
    }
    for (ColdInt& c : coldInts_) {
        a_.bind(c.entry);
        a_.cvtsi2sd32(c.x, c.v);
        a_.jmp(c.back);
    }
    for (auto& t : throws_) {
        a_.bind(t.second);
        a_.mov(RDI, R12);
        a_.movImm(RSI, t.first);
        a_.call(throw_);
        a_.jmp(tail_);
    }
    for (CounterStub& c : counterStubs_) {
        a_.bind(c.stub);
        a_.mov(RDI, R12);
        a_.movImm(RSI, c.target >= 0 ? std::uint64_t(c.target) : std::uint64_t(0xFFFFFFFFu));
        a_.call(tier2_);
        if (c.target >= 0) {
            a_.test(RAX, RAX);
            a_.jcc(CE, c.back);
            a_.jmp(RAX);
        } else {
            a_.jmp(c.back);
        }
    }
    a_.bind(step_);
    a_.movImm(RAX, reinterpret_cast<std::uint64_t>(&scriptVmJitStep));
    a_.jmp(RAX);
    a_.bind(call_);
    a_.movImm(RAX, reinterpret_cast<std::uint64_t>(&scriptVmJitCall));
    a_.jmp(RAX);
    a_.bind(throw_);
    a_.movImm(RAX, reinterpret_cast<std::uint64_t>(&scriptVmJitThrow));
    a_.jmp(RAX);
    a_.bind(tier2_);
    a_.movImm(RAX, reinterpret_cast<std::uint64_t>(&scriptVmJitTier2));
    a_.jmp(RAX);
    a_.bind(tick_);
    a_.movImm(RAX, reinterpret_cast<std::uint64_t>(&scriptVmJitTick));
    a_.jmp(RAX);
    if (optimizing()) {
        // A cell let go of in optimized code: the variables in caller-saved registers survive the call.
        a_.bind(free_);
        a_.push(R8);
        a_.push(R9);
        a_.push(R10);
        a_.push(R11);
        a_.aluI(5, RSP, 120);
        for (int k = 0; k < 14; ++k)
            a_.movsdStore(mem(RSP, 8 * k), 2 + k);
        a_.movImm(RAX, reinterpret_cast<std::uint64_t>(&scriptVmJitFree));
        a_.callReg(RAX);
        for (int k = 0; k < 14; ++k)
            a_.movsdLoad(2 + k, mem(RSP, 8 * k));
        a_.aluI(0, RSP, 120);
        a_.pop(R11);
        a_.pop(R10);
        a_.pop(R9);
        a_.pop(R8);
        a_.ret();
        // Leaving for the interpreter or the caller: back to the baseline frame first.
        a_.bind(tail_);
        a_.aluI(0, RSP, spill_);
        a_.bind(tailBase_);
        a_.cmpI(RAX, std::int32_t(kStepJump));
        a_.jcc(CE, dispBase_);
        a_.bind(exitBase_);
        a_.aluI(0, RSP, 8);
        a_.pop(R13);
        a_.pop(RBX);
        a_.ret();
        a_.bind(dispBase_);
        a_.mov(RDI, R12);
        a_.callAbs(reinterpret_cast<const void*>(&scriptVmJitTarget));
        a_.jmp(RAX);
        a_.bind(exit_);
        a_.aluI(0, RSP, 8 + spill_);
        a_.pop(R13);
        a_.pop(RBX);
        a_.ret();
        return;
    }
    a_.bind(free_);
    a_.movImm(RAX, reinterpret_cast<std::uint64_t>(&scriptVmJitFree));
    a_.jmp(RAX);
    a_.bind(tail_);
    a_.cmpI(RAX, std::int32_t(kStepJump));
    a_.jcc(CE, dispatch_);
    a_.bind(exit_);
    a_.aluI(0, RSP, 8);
    a_.pop(R13);
    a_.pop(RBX);
    a_.ret();
    a_.bind(dispatch_);
    a_.mov(RDI, R12);
    a_.callAbs(reinterpret_cast<const void*>(&scriptVmJitTarget));
    a_.jmp(RAX);
}

void Baseline::emit(std::uint32_t i)
{
    const Ins& in = code_->code[i];
    if (const char* off = std::getenv("JT_OFF")) {
        if (std::strstr(off, opName(in.opcode()))) {
            emitStep(i);
            return;
        }
    }
    switch (in.opcode()) {
    case Op::Nop:
        return;
    case Op::Mov:
        a_.mov(RAX, R(in.b));
        retain(RAX);
        write(in.a, RAX);
        return;
    case Op::LoadK: {
        const JSValue k = code_->consts[in.b];
        a_.movImm(RAX, k.bits);
        if (k.isCell())
            a_.inc32(mem(RAX, kHdrRc));
        write(in.a, RAX);
        return;
    }
    case Op::LoadInt:
        a_.movImm(RAX, kNumberTag | std::uint32_t(std::int32_t(in.imm())));
        write(in.a, RAX);
        return;
    case Op::LoadUndef:
    case Op::LoadNull:
    case Op::LoadTrue:
    case Op::LoadFalse:
    case Op::LoadHole:
    case Op::LoadUninit: {
        const std::uint64_t v = in.opcode() == Op::LoadUndef ? kUndefined : in.opcode() == Op::LoadNull ? kNull
                                : in.opcode() == Op::LoadTrue ? kTrue : in.opcode() == Op::LoadFalse ? kFalse
                                : in.opcode() == Op::LoadHole ? kHole : kUninit;
        a_.movImm(RAX, v);
        write(in.a, RAX);
        return;
    }
    case Op::Add: case Op::Sub: case Op::Mul: case Op::Div: case Op::BitAnd: case Op::BitOr: case Op::BitXor:
    case Op::Shl: case Op::Shr: case Op::UShr:
        arith(i, in);
        return;
    case Op::AddI: case Op::SubI: case Op::MulI: case Op::ModI: case Op::BitAndI: case Op::BitOrI: case Op::BitXorI:
    case Op::ShlI: case Op::ShrI: case Op::UShrI: case Op::Inc: case Op::Dec:
        arithImm(i, in);
        return;
    case Op::Lt: case Op::Le: case Op::Gt: case Op::Ge: case Op::Eq: case Op::Ne: case Op::StrictEq: case Op::StrictNe:
        compare(i, in);
        return;
    case Op::Jmp: case Op::JTrue: case Op::JFalse: case Op::JNullish: case Op::JNotNullish: case Op::JUndefined:
    case Op::JNotUndefined: case Op::JHole: case Op::JNotCallee:
    case Op::JLt: case Op::JLe: case Op::JGt: case Op::JGe: case Op::JNotLt: case Op::JNotLe: case Op::JNotGt:
    case Op::JNotGe: case Op::JStrictNe: case Op::JStrictEq:
    case Op::JLtI: case Op::JLeI: case Op::JGtI: case Op::JGeI: case Op::JNotLtI: case Op::JNotLeI: case Op::JNotGtI:
    case Op::JNotGeI: case Op::JStrictNeI: case Op::JStrictEqI:
        jumps(i, in);
        return;
    case Op::GetBox:
        a_.mov(RAX, R(in.b));
        a_.mov(RAX, mem(RAX, kBoxValue));
        deliver(i, in.a);
        return;
    case Op::PutBox:
        a_.mov(RAX, R(in.a));
        a_.mov(RCX, R(in.b));
        retain(RCX);
        a_.mov(RSI, mem(RAX, kBoxValue));
        a_.mov(mem(RAX, kBoxValue), RCX);
        release(RSI);
        return;
    case Op::GetUpval:
        a_.mov(RAX, mem(R13, kFrameFn));
        a_.mov(RAX, mem(RAX, kFnUpvals + std::int32_t(8 * in.b)));
        a_.mov(RAX, mem(RAX, kBoxValue));
        deliver(i, in.a);
        return;
    case Op::PutUpval:
        a_.mov(RAX, mem(R13, kFrameFn));
        a_.mov(RAX, mem(RAX, kFnUpvals + std::int32_t(8 * in.a)));
        a_.mov(RCX, R(in.b));
        retain(RCX);
        a_.mov(RSI, mem(RAX, kBoxValue));
        a_.mov(mem(RAX, kBoxValue), RCX);
        release(RSI);
        return;
    case Op::GetProp:
        getProp(i, in);
        return;
    case Op::PutProp:
        putProp(i, in);
        return;
    case Op::GetElem:
        getElem(i, in);
        return;
    case Op::GetSuperProp:
        getSuperProp(i, in);
        return;
    case Op::PutElem:
        putElem(i, in);
        return;
    case Op::Ret:
        ret(i, in);
        return;
    case Op::Call:
    case Op::New:
        call(i, in);
        return;
    case Op::GetGlobal:
    case Op::GetGlobalOrUndef:
        getGlobal(i, in);
        return;
    case Op::PutGlobal:
        putGlobal(i, in);
        return;
    case Op::InitProp:
        initProp(i, in);
        return;
    case Op::Closure:
        helperOp(i, &helperThunk<jitClosure>);
        return;
    case Op::NewBox:
        helperOp(i, &helperThunk<jitNewBox>);
        return;
    case Op::NewObject: {
        Label& bad = slow(i, &helperThunk<jitNewObject>);
        if (!allocObject(vm_.rootShapeFor(vm_.protos.object)->id, std::uint8_t(in.b), bad)) {
            a_.jmp(bad);
            return;
        }
        write(in.a, RAX);
        return;
    }
    case Op::NewArray: {
        // A literal of a few elements: the array made here, its elements in place.
        if (in.b > JSArray::kSmall || !mem::LocalHeap::kCanPopInline || vm_.ctx.options.memoryLimit) {
            helperOp(i, &helperThunk<jitNewArray>);
            break;
        }
        allocArray(slow(i, &helperThunk<jitNewArray>));
        write(in.a, RAX);
        break;
    }
        return;
    case Op::ArrayPush: {
        // An element of a literal, while there is room for it.
        Label& sl = slow(i, &helperThunk<jitArrayPush>);
        Label counted, number;
        a_.mov(RAX, R(in.a));
        a_.mov32(RCX, mem(RAX, kArrayLength));
        a_.alu32(7, RCX, mem(RAX, kArrayCapacity));
        a_.jcc(CAE, sl);
        a_.mov(RDX, R(in.b));
        a_.test(RDX, R14);
        a_.jcc(CNE, number);
        a_.andI8(mem(RAX, kHdrFlags), std::uint8_t(~cellflags::kAllNumbers));
        retain(RDX);
        a_.bind(number);
        a_.mov(RSI, mem(RAX, kArrayElems));
        a_.mov(mem(RSI, RCX, 8), RDX);
        a_.inc32(mem(RAX, kArrayLength));
        break;
    }
        return;
    default:
        emitStep(i);
        return;
    }
}

void Baseline::arith(std::uint32_t i, const Ins& in)
{
    const Op op = in.opcode();
    Label& bad = slow(i, &helperThunk<jitBinary>);
    Label dbl, done;
    const bool numeric = op == Op::Add || op == Op::Sub || op == Op::Mul;
    const bool bitwise = !numeric && op != Op::Div;
    a_.mov(RAX, R(in.b));
    a_.mov(RCX, R(in.c));
    if (bitwise) {
        int32From(RAX, RAX, bad);
        int32From(RCX, RCX, bad);
    } else if (op != Op::Div) {
        needInts(RAX, RCX, dbl);
    }
    switch (op) {
    case Op::Add: a_.mov32(RDX, RAX); a_.alu32(0, RDX, RCX); a_.jcc(CO, dbl); a_.mov32(RAX, RDX); break;
    case Op::Sub: a_.mov32(RDX, RAX); a_.alu32(5, RDX, RCX); a_.jcc(CO, dbl); a_.mov32(RAX, RDX); break;
    case Op::Mul:
        a_.mov32(RDX, RAX);
        a_.imul32(RDX, RCX);
        a_.jcc(CO, dbl);
        a_.test32(RDX, RDX);  // a zero may be a negative zero: the double path knows
        a_.jcc(CE, dbl);
        a_.mov32(RAX, RDX);
        break;
    case Op::BitAnd: a_.alu32(4, RAX, RCX); break;
    case Op::BitOr: a_.alu32(1, RAX, RCX); break;
    case Op::BitXor: a_.alu32(6, RAX, RCX); break;
    case Op::Shl: a_.shiftCl32(4, RAX); break;
    case Op::Shr: a_.shiftCl32(7, RAX); break;
    case Op::UShr:  // a result past int32 is a double
        a_.shiftCl32(5, RAX);
        a_.test32(RAX, RAX);
        a_.jcc(CS, bad);
        break;
    case Op::Div:  // two integers may divide evenly, and that is an int32 again: the double path decides
        break;
    default:
        break;
    }
    if (op != Op::Div) {
        a_.or_(RAX, R14);
        write(in.a, RAX, in.a == in.b || in.a == in.c);
    }
    if (numeric || op == Op::Div) {
        if (op != Op::Div)
            a_.jmp(done);
        a_.bind(dbl);
        if (op == Op::Add && in.a == in.b) {
            // `s += t` into a string only this register holds, with room: the characters copied in place.
            Label notCell;
            a_.test(RAX, R15);
            a_.jcc(CNE, notCell);
            savePool(i);
            a_.lea(RDI, R(in.a));
            a_.mov(RSI, RCX);
            a_.callAbs(reinterpret_cast<const void*>(&scriptVmJitAppendInPlace));
            restorePool(i);
            a_.aluI32(4, RAX, 0xFF);  // a bool comes back in al alone
            a_.jcc(CE, bad);
            a_.jmp(done);
            a_.bind(notCell);
        }
        needNumber(RAX, bad);
        needNumber(RCX, bad);
        loadNumber(0, RAX, RDX);
        loadNumber(1, RCX, RDX);
        switch (op) {
        case Op::Add: a_.addsd(0, 1); break;
        case Op::Sub: a_.subsd(0, 1); break;
        case Op::Mul: a_.mulsd(0, 1); break;
        default: a_.divsd(0, 1); break;
        }
        Label asDouble;
        if (op == Op::Div) {
            a_.mov(RDX, RAX);
            a_.and_(RDX, RCX);
            a_.cmp(RDX, R14);
            a_.jcc(CB, asDouble);
            a_.cvttsd2si32(RDX, 0);
            a_.cvtsi2sd32(1, RDX);
            a_.ucomisd(0, 1);
            a_.jcc(CNE, asDouble);
            a_.jcc(CP, asDouble);
            a_.test32(RDX, RDX);
            a_.jcc(CE, asDouble);
            a_.mov32(RAX, RDX);
            a_.or_(RAX, R14);
            write(in.a, RAX, in.a == in.b || in.a == in.c);
            a_.jmp(done);
        }
        a_.bind(asDouble);
        boxDouble0();
        write(in.a, RAX, in.a == in.b || in.a == in.c);
    }
    a_.bind(done);
}

void Baseline::arithImm(std::uint32_t i, const Ins& in)
{
    const Op op = in.opcode();
    const bool isRR = op == Op::Inc || op == Op::Dec;
    const std::int32_t imm = isRR ? (op == Op::Inc ? 1 : -1) : std::int32_t(std::int16_t(in.c));
    if (op == Op::ModI && imm <= 0) {
        helperOp(i, &helperThunk<jitBinaryImm>);
        return;
    }
    Label& bad = slow(i, isRR ? nullptr : &helperThunk<jitBinaryImm>);
    const bool bitwise = op == Op::BitAndI || op == Op::BitOrI || op == Op::BitXorI || op == Op::ShlI || op == Op::ShrI ||
                         op == Op::UShrI;
    const bool dblOk = op == Op::AddI || op == Op::SubI || op == Op::MulI;
    Label dbl, done;
    a_.mov(RAX, R(in.b));
    if (bitwise)
        int32From(RAX, RAX, bad);
    else
        needInt(RAX, dblOk ? dbl : bad);
    switch (op) {
    case Op::AddI: case Op::Inc: case Op::Dec: a_.aluI32(0, RAX, imm); a_.jcc(CO, bad); break;
    case Op::SubI: a_.aluI32(5, RAX, imm); a_.jcc(CO, bad); break;
    case Op::MulI:
        // An overflow, or a zero that may be a negative zero, is the double path's: RAX still holds the operand.
        a_.imulI32(RDX, RAX, imm);
        a_.jcc(CO, dbl);
        a_.test32(RDX, RDX);
        a_.jcc(CE, dbl);
        a_.mov32(RAX, RDX);
        break;
    case Op::ModI:
        a_.test32(RAX, RAX);
        a_.jcc(CS, bad);
        if ((imm & (imm - 1)) == 0) {
            a_.aluI32(4, RAX, imm - 1);
        } else {
            a_.movImm(RCX, std::uint32_t(imm));
            a_.cdq();
            a_.idiv32(RCX);
            a_.mov32(RAX, RDX);
        }
        break;
    case Op::BitAndI: a_.aluI32(4, RAX, imm); break;
    case Op::BitOrI: a_.aluI32(1, RAX, imm); break;
    case Op::BitXorI: a_.aluI32(6, RAX, imm); break;
    case Op::ShlI: a_.shiftI32(4, RAX, imm & 31); break;
    case Op::ShrI: a_.shiftI32(7, RAX, imm & 31); break;
    default:  // UShrI
        a_.shiftI32(5, RAX, imm & 31);
        a_.test32(RAX, RAX);
        a_.jcc(CS, bad);
        break;
    }
    a_.or_(RAX, R14);
    write(in.a, RAX, in.a == in.b);
    if (dblOk) {
        // A double operand: the result is a double (the interpreter's number path).
        a_.jmp(done);
        a_.bind(dbl);
        needNumber(RAX, bad);
        loadNumber(0, RAX, RDX);
        a_.movImm(RCX, std::uint32_t(imm));
        a_.cvtsi2sd32(1, RCX);
        if (op == Op::AddI)
            a_.addsd(0, 1);
        else if (op == Op::SubI)
            a_.subsd(0, 1);
        else
            a_.mulsd(0, 1);
        boxDouble0();
        write(in.a, RAX, in.a == in.b);
    }
    a_.bind(done);
}

namespace {

/// How a relation reads off the flags: after `cmp` of two int32, condition `ic`; after `ucomisd` (with the operands
/// swapped when `swap`) condition `dc`, which is false when either is NaN, except in the negated forms, which are true.
struct Relation
{
    Cond ic;
    bool swap;
    Cond dc;
    bool doubles;
};

Relation relationOf(Op op)
{
    switch (op) {
    case Op::Lt: case Op::JLt: case Op::JLtI: return {CL, true, CA, true};
    case Op::Le: case Op::JLe: case Op::JLeI: return {CLE, true, CAE, true};
    case Op::Gt: case Op::JGt: case Op::JGtI: return {CG, false, CA, true};
    case Op::Ge: case Op::JGe: case Op::JGeI: return {CGE, false, CAE, true};
    case Op::JNotLt: case Op::JNotLtI: return {CGE, true, CBE, true};
    case Op::JNotLe: case Op::JNotLeI: return {CG, true, CB, true};
    case Op::JNotGt: case Op::JNotGtI: return {CLE, false, CBE, true};
    case Op::JNotGe: case Op::JNotGeI: return {CL, false, CB, true};
    case Op::Eq: case Op::StrictEq: case Op::JStrictEq: case Op::JStrictEqI: return {CE, false, CE, false};
    default: return {CNE, false, CNE, false};
    }
}

}  // namespace

/// rax === rcx: go to `eq` or `neq`, or to `bad` when only the interpreter can tell (strings, doubles).
void Baseline::strictCore(Label& eq, Label& neq, Label& bad)
{
    Label differ, aNumber;
    a_.cmp(RAX, RCX);
    a_.jcc(CNE, differ);
    a_.cmp(RAX, RBP);  // the same bits are equal, unless a double is not a number
    a_.jcc(CB, eq);
    a_.cmp(RAX, R14);
    a_.jcc(CAE, eq);
    a_.jmp(bad);
    a_.bind(differ);
    a_.cmp(RAX, RBP);
    a_.jcc(CAE, aNumber);
    a_.testI(RAX, 2);  // null, undefined and booleans are only equal to themselves
    a_.jcc(CNE, neq);
    a_.cmp(RCX, RBP);
    a_.jcc(CAE, neq);
    a_.testI(RCX, 2);
    a_.jcc(CNE, neq);
    a_.jmp(bad);  // two cells: strings and bigints can still be equal
    a_.bind(aNumber);
    a_.cmp(RCX, RBP);
    a_.jcc(CB, neq);
    a_.mov(RDX, RAX);
    a_.and_(RDX, RCX);
    a_.cmp(RDX, R14);
    a_.jcc(CAE, neq);  // two int32 that differ
    a_.jmp(bad);
}

void Baseline::strictJump(std::uint32_t i, const Ins& in)
{
    const Op op = in.opcode();
    const bool imm = op == Op::JStrictEqI || op == Op::JStrictNeI;
    const bool wantEqual = op == Op::JStrictEq || op == Op::JStrictEqI;
    const std::uint32_t target = i + 1 + std::uint32_t(std::int32_t(in.jump16()));
    if (!imm && optimizing() && i > 0 && !plan_->isLeader[i]) {
        // One side null, undefined, true or false loaded just before: the same value only as the same bits.
        const Ins& p = code_->code[i - 1];
        std::int64_t k = -1;
        switch (p.opcode()) {
        case Op::LoadNull: k = std::int64_t(kNull); break;
        case Op::LoadUndef: k = std::int64_t(kUndefined); break;
        case Op::LoadTrue: k = std::int64_t(kTrue); break;
        case Op::LoadFalse: k = std::int64_t(kFalse); break;
        default: break;
        }
        if (k >= 0 && (p.a == in.a) != (p.a == in.b)) {
            a_.mov(RAX, R(p.a == in.a ? in.b : in.a));
            a_.cmpI(RAX, std::int32_t(k));
            goIf(wantEqual ? CE : CNE, i, target);
            return;
        }
    }
    Label& bad = slow(i);
    Label eq, neq;
    a_.mov(RAX, R(in.a));
    if (imm) {
        Label isInt;
        a_.cmp(RAX, R14);
        a_.jcc(CAE, isInt);
        a_.cmp(RAX, RBP);
        a_.jcc(CAE, bad);  // a double
        a_.jmp(neq);
        a_.bind(isInt);
        a_.aluI32(7, RAX, std::int32_t(std::int16_t(in.b)));
        a_.jcc(CE, eq);
        a_.jmp(neq);
    } else {
        a_.mov(RCX, R(in.b));
        strictCore(eq, neq, bad);
    }
    a_.bind(wantEqual ? eq : neq);
    go(i, target);
    a_.bind(wantEqual ? neq : eq);
}

void Baseline::strictValue(std::uint32_t i, const Ins& in)
{
    const bool wantEqual = in.opcode() == Op::StrictEq;
    Label& bad = slow(i, &helperThunk<jitBinary>);
    Label eq, neq, store;
    a_.mov(RAX, R(in.b));
    a_.mov(RCX, R(in.c));
    strictCore(eq, neq, bad);
    a_.bind(eq);
    a_.movImm(RAX, wantEqual ? kTrue : kFalse);
    a_.jmp(store);
    a_.bind(neq);
    a_.movImm(RAX, wantEqual ? kFalse : kTrue);
    a_.bind(store);
    write(in.a, RAX);
}

void Baseline::compare(std::uint32_t i, const Ins& in)
{
    if (in.opcode() == Op::StrictEq || in.opcode() == Op::StrictNe) {
        strictValue(i, in);
        return;
    }
    const Relation rel = relationOf(in.opcode());
    Label& bad = slow(i, &helperThunk<jitBinary>);
    Label dbl, done;
    a_.mov(RAX, R(in.b));
    a_.mov(RCX, R(in.c));
    needInts(RAX, RCX, rel.doubles ? dbl : bad);
    a_.alu32(7, RAX, RCX);
    a_.setcc(rel.ic, RAX);
    a_.movzx8(RAX, RAX);
    a_.aluI32(0, RAX, 6);
    write(in.a, RAX, false);
    if (rel.doubles) {
        a_.jmp(done);
        a_.bind(dbl);
        needNumber(RAX, bad);
        needNumber(RCX, bad);
        loadNumber(0, RAX, RDX);
        loadNumber(1, RCX, RDX);
        if (rel.swap)
            a_.ucomisd(1, 0);
        else
            a_.ucomisd(0, 1);
        a_.setcc(rel.dc, RAX);
        a_.movzx8(RAX, RAX);
        a_.aluI32(0, RAX, 6);
        write(in.a, RAX, false);
    }
    a_.bind(done);
}

void Baseline::jumps(std::uint32_t i, const Ins& in)
{
    const Op op = in.opcode();
    if (op == Op::JStrictEq || op == Op::JStrictNe || op == Op::JStrictEqI || op == Op::JStrictNeI) {
        strictJump(i, in);
        return;
    }
    switch (op) {
    case Op::Jmp:
        go(i, i + 1 + std::uint32_t(in.jump()));
        return;
    case Op::JTrue:
    case Op::JFalse: {
        const std::uint32_t target = i + 1 + std::uint32_t(in.jump());
        Label& bad = slow(i);
        Label taken, notTaken;
        a_.mov(RAX, R(in.a));
        a_.cmpI(RAX, std::int32_t(kTrue));
        a_.jcc(CE, op == Op::JTrue ? taken : notTaken);
        a_.cmpI(RAX, std::int32_t(kFalse));
        a_.jcc(CE, op == Op::JTrue ? notTaken : taken);
        needInt(RAX, bad);
        a_.test32(RAX, RAX);
        a_.jcc(op == Op::JTrue ? CNE : CE, taken);
        a_.jmp(notTaken);
        a_.bind(taken);
        go(i, target);
        a_.bind(notTaken);
        return;
    }
    case Op::JNullish:
    case Op::JNotNullish:
    case Op::JUndefined:
    case Op::JNotUndefined:
    case Op::JHole: {
        const std::uint32_t target = i + 1 + std::uint32_t(in.jump());
        a_.mov(RAX, R(in.a));
        if (op == Op::JNullish || op == Op::JNotNullish) {
            a_.aluI(4, RAX, -9);  // clears bit 3: null and undefined become 2
            a_.cmpI(RAX, std::int32_t(kNull));
        } else {
            a_.cmpI(RAX, std::int32_t(op == Op::JHole ? kHole : kUndefined));
        }
        const bool wantEqual = op == Op::JNullish || op == Op::JUndefined || op == Op::JHole;
        goIf(wantEqual ? CE : CNE, i, target);
        return;
    }
    case Op::JNotCallee: {
        const std::uint32_t target = i + 1 + std::uint32_t(in.jump());
        a_.mov(RAX, R(in.a));
        a_.cmp(RAX, mem(R13, kFrameFn));
        goIf(CNE, i, target);
        return;
    }
    case Op::JLtI: case Op::JLeI: case Op::JGtI: case Op::JGeI: case Op::JNotLtI: case Op::JNotLeI: case Op::JNotGtI:
    case Op::JNotGeI: case Op::JStrictEqI: case Op::JStrictNeI: {
        const std::uint32_t target = i + 1 + std::uint32_t(std::int32_t(in.jump16()));
        const std::int32_t imm = std::int32_t(std::int16_t(in.b));
        const Relation rel = relationOf(op);
        Label& bad = slow(i);
        Label dbl, done;
        a_.mov(RAX, R(in.a));
        needInt(RAX, rel.doubles ? dbl : bad);
        a_.aluI32(7, RAX, imm);
        goIf(rel.ic, i, target);
        if (rel.doubles) {
            a_.jmp(done);
            a_.bind(dbl);
            needNumber(RAX, bad);
            loadNumber(0, RAX, RDX);
            a_.movImm(RCX, std::uint32_t(imm));
            a_.cvtsi2sd32(1, RCX);
            if (rel.swap)
                a_.ucomisd(1, 0);
            else
                a_.ucomisd(0, 1);
            goIf(rel.dc, i, target);
        }
        a_.bind(done);
        return;
    }
    default: {  // register against register
        const std::uint32_t target = i + 1 + std::uint32_t(std::int32_t(in.jump16()));
        const Relation rel = relationOf(op);
        Label& bad = slow(i);
        Label dbl, done;
        a_.mov(RAX, R(in.a));
        a_.mov(RCX, R(in.b));
        needInts(RAX, RCX, rel.doubles ? dbl : bad);
        a_.alu32(7, RAX, RCX);
        goIf(rel.ic, i, target);
        if (rel.doubles) {
            a_.jmp(done);
            a_.bind(dbl);
            needNumber(RAX, bad);
            needNumber(RCX, bad);
            loadNumber(0, RAX, RDX);
            loadNumber(1, RCX, RDX);
            if (rel.swap)
                a_.ucomisd(1, 0);
            else
                a_.ucomisd(0, 1);
            goIf(rel.dc, i, target);
        }
        a_.bind(done);
        return;
    }
    }
}

void Baseline::getProp(std::uint32_t i, const Ins& in)
{
    const Key key = code_->atoms[in.c];
    const std::uint16_t cacheId = code_->code[i + 1].a;
    const InlineCache* ic = cacheId != kNoCache ? &code_->caches[cacheId] : nullptr;
    const bool isLength = key == vm_.atoms.length;
    // The shapes the site has seen: its inline cache, then the data entries of its `PolyCache` (accessors go to the helper).
    struct Seen
    {
        std::uint32_t shape, index;
        JSObject* holder;
        ValidityCell* validity;
    };
    Seen seen[5];
    int count = 0;
    if (ic && ic->shape != 0 && (!ic->holder || ic->validity))
        seen[count++] = Seen{ic->shape, ic->index, ic->holder, ic->validity};
    if (ic && ic->pad && ic->pad <= code_->poly.size()) {
        const PolyCache& pc = code_->poly[ic->pad - 1];
        for (std::uint8_t k = 0; k < pc.n; ++k) {
            const PolyEntry& e = pc.e[k];
            if (e.kind == PolyEntry::OwnData || (e.kind == PolyEntry::ProtoData && e.validity))
                seen[count++] = Seen{e.shape, e.index, e.holder, e.validity};
        }
    }
    if (count == 0 && !isLength && ic && ic->shape == 0 && ic->pad && ic->pad <= code_->poly.size()) {
        // A getter the site always finds in the same place: called like a script function, without the runtime.
        const PolyCache& pc = code_->poly[ic->pad - 1];
        if (pc.n == 1 && (pc.e[0].kind == PolyEntry::OwnAccessor || (pc.e[0].kind == PolyEntry::ProtoAccessor && pc.e[0].validity))) {
            getterCall(i, in, pc.e[0]);
            return;
        }
    }
    // A string receiver: its length, or a data property of String.prototype while that chain is as it was.
    JSValue* protoSlot = nullptr;
    ValidityCell* protoCell = nullptr;
    double numeric;
    if (!isLength && vm_.protos.string && !canonicalNumericIndex(key, &numeric)) {
        std::uint8_t attrs = 0;
        JSObject* sp = vm_.protos.string;
        protoCell = vm_.validityOf(sp);
        JSValue* slot = sp->findOwn(vm_, key, &attrs);
        if (slot && !(attrs & attr::kAccessor) && protoCell->valid)
            protoSlot = slot;
    }
    const bool stringPath = isLength || protoSlot;
    if (count == 0 && !stringPath) {
        immature(i);
        helperOp(i, &helperThunk<jitGetProp>);
        return;
    }
    Label& bad = slow(i, &helperThunk<jitGetProp>);
    Label done, lengthPath, stringRecv;
    a_.mov(RAX, R(in.b));
    if (count == 0 && isLength) {
        // No shape seen: an array's length, or a string's.
        a_.test(RAX, R15);
        a_.jcc(CNE, bad);
        a_.cmpI8(mem(RAX, kHdrKind), kObjectKind);
        a_.jcc(CE, lengthPath);
        a_.jmp(stringRecv);
    } else if (count == 0) {
        a_.jmp(stringRecv);
    } else {
        a_.test(RAX, R15);
        a_.jcc(CNE, bad);
        a_.cmpI8(mem(RAX, kHdrKind), kObjectKind);
        a_.jcc(CNE, stringPath ? stringRecv : bad);
    }
    for (int k = 0; k < count; ++k) {
        Label next;
        const bool last = k == count - 1;
        // The last entry that does not match goes where a miss goes; a hit falls through to the end.
        a_.cmpI32(mem(RAX, 0), std::int32_t(seen[k].shape));
        a_.jcc(CNE, last ? (isLength ? lengthPath : bad) : next);
        if (seen[k].holder) {
            hold(seen[k].validity);
            a_.movImm(RCX, reinterpret_cast<std::uint64_t>(seen[k].validity));
            a_.cmpI32(mem(RCX, kValidityValid), 0);
            a_.jcc(CE, bad);
            a_.movImm(RAX, reinterpret_cast<std::uint64_t>(seen[k].holder));
        }
        slotAddress(seen[k].index);
        a_.mov(RAX, mem(RDX, 0));
        deliver(i, in.a);
        if (!last || isLength)
            a_.jmp(done);
        if (!last)
            a_.bind(next);
    }
    if (isLength) {
        a_.bind(lengthPath);
        a_.cmpI8(mem(RAX, kHdrCls), std::uint8_t(ObjClass::Array));
        a_.jcc(CNE, bad);
        a_.mov32(RAX, mem(RAX, kArrayLength));
        a_.test32(RAX, RAX);
        a_.jcc(CS, bad);
        deliverInt(i, in.a);
    }
    if (stringPath) {
        a_.jmp(done);
        a_.bind(stringRecv);
        needString(RAX, bad);
        if (isLength) {
            a_.mov32(RAX, mem(RAX, kStrLength));
            a_.test32(RAX, RAX);
            a_.jcc(CS, bad);
            deliverInt(i, in.a);
        } else {
            hold(protoCell);
            a_.movImm(RCX, reinterpret_cast<std::uint64_t>(protoCell));
            a_.cmpI32(mem(RCX, kValidityValid), 0);
            a_.jcc(CE, bad);
            a_.movImm(RDX, reinterpret_cast<std::uint64_t>(protoSlot));
            a_.mov(RAX, mem(RDX, 0));
            deliver(i, in.a);
        }
    }
    a_.bind(done);
}

void Baseline::getterCall(std::uint32_t i, const Ins& in, const PolyEntry& e)
{
    sideDoor();
    // The getter's frame goes above this function's registers: `this` (not counted: the receiver register holds it until
    // the result is written) and nothing else, the rest of the stack there being undefined already.
    Label& bad = slow(i, &helperThunk<jitGetProp>, true);
    throws_.emplace_back(i, Label{});
    Label& exc = throws_.back().second;
    const std::int32_t thisAt = std::int32_t(8 * code_->nregs);
    savePool(i);
    a_.mov(RAX, R(in.b));
    needObject(RAX, bad);
    a_.cmpI32(mem(RAX, 0), std::int32_t(e.shape));
    a_.jcc(CNE, bad);
    if (e.holder) {
        hold(e.validity);
        a_.movImm(RCX, reinterpret_cast<std::uint64_t>(e.validity));
        a_.cmpI32(mem(RCX, kValidityValid), 0);
        a_.jcc(CE, bad);
        a_.movImm(RAX, reinterpret_cast<std::uint64_t>(e.holder));
    }
    slotAddress(e.index);
    a_.mov(RAX, mem(RDX, 0));
    a_.mov(RAX, mem(RAX, kGsGetter));
    needObject(RAX, bad);
    a_.cmpI8(mem(RAX, kHdrCls), std::uint8_t(ObjClass::Function));
    a_.jcc(CNE, bad);
    a_.testI32(mem(RAX, kFnFlags), JSFunction::kNative);
    a_.jcc(CNE, bad);
    a_.mov(RCX, mem(RAX, kFnCode));
    a_.mov(RDX, mem(RCX, kCbTier1));
    a_.test(RDX, RDX);
    a_.jcc(CE, bad);
    a_.testI32(mem(RCX, kCbFlags), CodeBlock::kClassConstructor | CodeBlock::kGenerator | CodeBlock::kAsync |
                                       CodeBlock::kAsyncGen | CodeBlock::kNeedsArgs);
    a_.jcc(CNE, bad);
    a_.mov32(RSI, mem(RCX, kCbNregs));
    a_.lea(R9, mem(RBX, thisAt));
    // The frame and one past it filled: the stack is never filled closer than 8 to its end (`VM::fillStack`).
    a_.lea(R10, mem(R9, RSI, 8, 8));
    a_.alu(7, R10, mem(R12, kVmStackInit));
    a_.jcc(CA, bad);
    topFrame();
    a_.alu(7, R11, mem(R12, kVmFrameLimit));
    a_.jcc(CAE, bad);
    a_.cmpI32(mem(R12, kVmTicks), 1);
    a_.jcc(CBE, bad);
    a_.alu(7, RSP, mem(R12, kVmStackFloor));
    a_.jcc(CB, bad);
    a_.mov(RDI, R(in.b));
    a_.mov(mem(R9, 0), RDI);
    a_.dec32(mem(R12, kVmTicks));
    a_.movImm(RDI, reinterpret_cast<std::uint64_t>(&code_->code[i + length(i)]));
    a_.mov(mem(R13, kFramePc), RDI);
    topFrame();
    a_.lea(RDI, mem(R11, std::int32_t(sizeof(Frame))));
    a_.mov(mem(R12, kVmFrameTop), RDI);
    a_.mov(mem(R11, kFrameCode), RCX);
    a_.mov(mem(R11, kFrameFn), RAX);
    a_.mov(mem(R11, kFramePc), 0);
    a_.mov(mem(R11, kFrameR), R9);
    a_.movImm(RDI, in.a);
    a_.mov(mem(R11, kFrameRetDst), RDI);
    a_.mov(mem(R11, kFrameEntry - 1), std::int32_t(0x100));
    a_.mov(mem(R11, kFrameNewTarget), std::int32_t(kUndefined));
    a_.mov(mem(R11, kFrameGen), std::int32_t(kUndefined));
    a_.mov(RSI, R11);
    a_.mov(RDI, R9);
    a_.mov(RDX, mem(RDX, kTierEntry));
    a_.callReg(RDX);
    a_.mov(mem(RBX, thisAt), std::int32_t(kUndefined));
    a_.test(RAX, RAX);
    a_.jcc(CE, exc);
    write(in.a, RAX);
    restorePool(i);
}

void Baseline::putProp(std::uint32_t i, const Ins& in)
{
    const std::uint16_t cacheId = code_->code[i + 1].a;
    const InlineCache* ic = cacheId != kNoCache ? &code_->caches[cacheId] : nullptr;
    if (!ic || ic->shape == 0 || ic->holder) {
        helperOp(i, &helperThunk<jitPutProp>);
        return;
    }
    Label& bad = slow(i, &helperThunk<jitPutProp>);
    a_.mov(RAX, R(in.a));
    needObject(RAX, bad);
    a_.cmpI32(mem(RAX, 0), std::int32_t(ic->shape));
    a_.jcc(CNE, bad);
    if (ic->next == 0) {
        slotAddress(ic->index);
        a_.mov(RCX, R(in.c));
        retain(RCX);
        a_.mov(RSI, mem(RDX, 0));
        a_.mov(mem(RDX, 0), RCX);
        release(RSI);
        return;
    }
    if (ic->validity) {
        hold(ic->validity);
        a_.movImm(RCX, reinterpret_cast<std::uint64_t>(ic->validity));
        a_.cmpI32(mem(RCX, kValidityValid), 0);
        a_.jcc(CE, bad);
    }
    a_.testI8(mem(RAX, kHdrFlags), std::uint8_t(cellflags::kIsProto | cellflags::kNotExtensible));
    a_.jcc(CNE, bad);
    a_.mov(RCX, R(in.c));
    retain(RCX);
    a_.mov(mem(RAX, kObjInline + std::int32_t(8 * ic->index)), RCX);
    a_.mov32(mem(RAX, 0), std::int32_t(ic->next));
}

void Baseline::getSuperProp(std::uint32_t i, const Ins& in)
{
    // The slot the helper found last time, while the function's home object is the same and its chain unchanged.
    if (code_->superCaches.empty())
        code_->superCaches.assign(code_->code.size(), SuperCache{});
    const SuperCache* sc = &code_->superCaches[i];
    Label& bad = slow(i, &helperThunk<jitGetSuperProp>);
    a_.mov(RAX, mem(R13, kFrameFn));
    a_.test(RAX, RAX);
    a_.jcc(CE, bad);
    a_.testI8(mem(RAX, kHdrFlags), cellflags::kHasRare);
    a_.jcc(CE, bad);
    a_.mov(RAX, mem(RAX, kObjSlots));
    a_.mov(RAX, mem(RAX, -16));
    a_.mov(RAX, mem(RAX, kRareHome));
    a_.movImm(RCX, reinterpret_cast<std::uint64_t>(sc));
    a_.cmp(RAX, mem(RCX, kSuperHome));
    a_.jcc(CNE, bad);
    a_.mov(RDX, mem(RCX, kSuperValidity));
    a_.cmpI32(mem(RDX, kValidityValid), 0);
    a_.jcc(CE, bad);
    a_.mov(RDX, mem(RCX, kSuperSlot));
    a_.mov(RAX, mem(RDX, 0));
    deliver(i, in.a);
}

void Baseline::getElem(std::uint32_t i, const Ins& in)
{
    using E = ElemKind;
    Label& bad = slow(i, &helperThunk<jitGetElem>);
    Label typed, done;
    a_.mov(RAX, R(in.b));
    a_.mov(RCX, R(in.c));
    needInt(RCX, bad);
    a_.test32(RCX, RCX);
    a_.jcc(CS, bad);
    needObject(RAX, bad);
    a_.cmpI8(mem(RAX, kHdrCls), std::uint8_t(ObjClass::Array));
    a_.jcc(CNE, typed);
    a_.mov32(RCX, RCX);
    a_.alu32(7, RCX, mem(RAX, kArrayLength));
    a_.jcc(CAE, bad);
    a_.mov(RDX, mem(RAX, kArrayElems));
    a_.mov(RAX, mem(RDX, RCX, 8));
    a_.cmpI(RAX, std::int32_t(kHole));
    a_.jcc(CE, bad);
    noteElem(i, elemprof::kArray, RDX);
    deliver(i, in.a);
    a_.jmp(done);

    a_.bind(typed);
    a_.cmpI8(mem(RAX, kHdrCls), std::uint8_t(ObjClass::TypedArray));
    a_.jcc(CNE, bad);
    a_.mov32(RCX, RCX);
    a_.alu32(7, RCX, mem(RAX, kTypedLength));
    a_.jcc(CAE, bad);
    a_.movzx8(RDX, mem(RAX, kTypedKind));
    a_.mov(RSI, mem(RAX, kTypedData));
    Label f64, i32, u8, s8, i16, u16, f32;
    using namespace elemprof;
    kindDispatch(i, {{E::Float64, kFloat64, &f64}, {E::Int32, kInt32, &i32}, {E::Uint8, kUint8, &u8},
                     {E::Uint8Clamped, kOtherTyped, &u8}, {E::Int8, kOtherTyped, &s8}, {E::Int16, kOtherTyped, &i16},
                     {E::Uint16, kOtherTyped, &u16}, {E::Float32, kOtherTyped, &f32}}, bad);
    Label intResult;
    a_.bind(f64);
    a_.movsdLoad(0, mem(RSI, RCX, 8));
    Label box;
    a_.jmp(box);
    a_.bind(f32);
    a_.movssLoad(0, mem(RSI, RCX, 4));
    a_.cvtss2sd(0, 0);
    a_.bind(box);
    deliverDouble(i, in.a);
    a_.jmp(done);
    a_.bind(i32);
    a_.mov32(RAX, mem(RSI, RCX, 4));
    a_.jmp(intResult);
    a_.bind(u8);
    a_.movzx8(RAX, mem(RSI, RCX, 1));
    a_.jmp(intResult);
    a_.bind(s8);
    a_.movsx8(RAX, mem(RSI, RCX, 1));
    a_.jmp(intResult);
    a_.bind(i16);
    a_.movsx16(RAX, mem(RSI, RCX, 2));
    a_.jmp(intResult);
    a_.bind(u16);
    a_.movzx16(RAX, mem(RSI, RCX, 2));
    a_.bind(intResult);
    deliverInt(i, in.a);
    a_.bind(done);
}

void Baseline::putElem(std::uint32_t i, const Ins& in)
{
    using E = ElemKind;
    Label& bad = slow(i, &helperThunk<jitPutElem>);
    Label isNumber, typed, done;
    a_.mov(RAX, R(in.a));
    a_.mov(RCX, R(in.b));
    needInt(RCX, bad);
    a_.test32(RCX, RCX);
    a_.jcc(CS, bad);
    needObject(RAX, bad);
    a_.cmpI8(mem(RAX, kHdrCls), std::uint8_t(ObjClass::Array));
    a_.jcc(CNE, typed);
    a_.testI8(mem(RAX, kHdrFlags), std::uint8_t(cellflags::kFrozen | cellflags::kNotExtensible));
    a_.jcc(CNE, bad);
    a_.mov32(RCX, RCX);
    a_.alu32(7, RCX, mem(RAX, kArrayLength));
    a_.jcc(CAE, bad);  // an append goes the long way
    a_.mov(RDX, mem(RAX, kArrayElems));
    a_.lea(RDX, mem(RDX, RCX, 8));
    a_.mov(RDI, mem(RDX, 0));
    a_.cmpI(RDI, std::int32_t(kHole));
    Label hole;
    a_.jcc(CE, hole);
    noteElem(i, elemprof::kArray, RSI);
    a_.mov(RSI, R(in.c));
    retain(RSI);
    a_.test(RSI, R14);
    a_.jcc(CNE, isNumber);
    a_.andI8(mem(RAX, kHdrFlags), std::uint8_t(~cellflags::kAllNumbers));
    a_.bind(isNumber);
    a_.mov(RDI, mem(RDX, 0));
    a_.mov(mem(RDX, 0), RSI);
    release(RDI);
    a_.jmp(done);

    // A hole of a plain array (no properties of its own, the usual prototype chain, which has not changed since): nothing
    // can intercept the store.
    a_.bind(hole);
    {
        if (!plainHoleGuard(RDI, bad)) {
            a_.jmp(bad);
        } else {
            noteElem(i, elemprof::kArray, RSI);
            Label holeNumber;
            a_.mov(RSI, R(in.c));
            retain(RSI);
            a_.test(RSI, R14);
            a_.jcc(CNE, holeNumber);
            a_.andI8(mem(RAX, kHdrFlags), std::uint8_t(~cellflags::kAllNumbers));
            a_.bind(holeNumber);
            a_.mov(mem(RDX, 0), RSI);
            a_.jmp(done);
        }
    }

    // A typed array takes a number; one past its end is ignored.
    a_.bind(typed);
    a_.cmpI8(mem(RAX, kHdrCls), std::uint8_t(ObjClass::TypedArray));
    a_.jcc(CNE, bad);
    a_.mov(RDI, R(in.c));
    a_.test(RDI, R14);
    a_.jcc(CE, bad);
    a_.mov32(RCX, RCX);
    a_.alu32(7, RCX, mem(RAX, kTypedLength));
    a_.jcc(CAE, done);
    a_.movzx8(RDX, mem(RAX, kTypedKind));
    a_.mov(RSI, mem(RAX, kTypedData));
    Label f64, i32, i8, i16, f32;
    using namespace elemprof;
    kindDispatch(i, {{E::Float64, kFloat64, &f64}, {E::Int32, kInt32, &i32}, {E::Uint32, kOtherTyped, &i32},
                     {E::Uint8, kUint8, &i8}, {E::Int8, kOtherTyped, &i8}, {E::Int16, kOtherTyped, &i16},
                     {E::Uint16, kOtherTyped, &i16}, {E::Float32, kOtherTyped, &f32}}, bad);
    a_.bind(f64);
    loadNumber(0, RDI, RDX);
    a_.movsdStore(mem(RSI, RCX, 8), 0);
    a_.jmp(done);
    a_.bind(f32);
    loadNumber(0, RDI, RDX);
    a_.cvtsd2ss(0, 0);
    a_.movssStore(mem(RSI, RCX, 4), 0);
    a_.jmp(done);
    a_.bind(i32);
    int32From(RAX, RDI, bad);
    a_.mov32(mem(RSI, RCX, 4), RAX);
    a_.jmp(done);
    a_.bind(i8);
    int32From(RAX, RDI, bad);
    a_.mov8(mem(RSI, RCX, 1), RAX);
    a_.jmp(done);
    a_.bind(i16);
    int32From(RAX, RDI, bad);
    a_.mov16(mem(RSI, RCX, 2), RAX);
    a_.bind(done);
}

void Baseline::initProp(std::uint32_t i, const Ins& in)
{
    const std::uint16_t cacheId = code_->code[i + 1].a;
    const InlineCache* ic = cacheId != kNoCache ? &code_->caches[cacheId] : nullptr;
    if (!ic || ic->shape == 0 || ic->next == 0) {
        helperOp(i, &helperThunk<jitInitProp>);
        return;
    }
    Label& bad = slow(i, &helperThunk<jitInitProp>);
    a_.mov(RAX, R(in.a));
    a_.cmpI32(mem(RAX, 0), std::int32_t(ic->shape));
    a_.jcc(CNE, bad);
    a_.mov(RCX, R(in.c));
    retain(RCX);
    a_.mov(mem(RAX, kObjInline + std::int32_t(8 * ic->index)), RCX);
    a_.mov32(mem(RAX, 0), std::int32_t(ic->next));
}

void Baseline::getGlobal(std::uint32_t i, const Ins& in)
{
    // A data property the global object has now stays where it is until something deletes it or changes its attributes,
    // which the epoch counts.
    JSObject* g = vm_.global;
    std::uint8_t attrs = 0;
    JSValue* slot = g->findOwn(vm_, code_->atoms[in.b], &attrs);
    if (!slot || (attrs & attr::kAccessor)) {
        helperOp(i, &helperThunk<jitGetGlobal>);
        return;
    }
    std::uint32_t index;
    JSValue* inlineBase = g->inl();
    if (slot >= inlineBase && slot < inlineBase + g->h.aux)
        index = std::uint32_t(slot - inlineBase);
    else
        index = std::uint32_t(slot - g->slots) + g->h.aux;
    Label& bad = slow(i, &helperThunk<jitGetGlobal>);
    a_.cmpI32(mem(R12, kVmGlobalEpoch), std::int32_t(vm_.globalEpoch));
    a_.jcc(CNE, bad);
    globalSlot(index);
    a_.mov(RAX, mem(RDX, 0));
    deliver(i, in.a);
}

void Baseline::putGlobal(std::uint32_t i, const Ins& in)
{
    // As for a read: a writable data property of the global object stays where it is until the epoch moves. A name the global
    // object does not have yet, or one that cannot be written, is left to the interpreter.
    JSObject* g = vm_.global;
    std::uint8_t attrs = 0;
    JSValue* slot = g->findOwn(vm_, code_->atoms[in.a], &attrs);
    if (!slot || (attrs & attr::kAccessor) || !(attrs & attr::kWritable)) {
        emitStep(i);
        return;
    }
    std::uint32_t index;
    JSValue* inlineBase = g->inl();
    if (slot >= inlineBase && slot < inlineBase + g->h.aux)
        index = std::uint32_t(slot - inlineBase);
    else
        index = std::uint32_t(slot - g->slots) + g->h.aux;
    Label& bad = slow(i);
    a_.cmpI32(mem(R12, kVmGlobalEpoch), std::int32_t(vm_.globalEpoch));
    a_.jcc(CNE, bad);
    globalSlot(index);
    a_.mov(RCX, R(in.b));
    retain(RCX);
    a_.mov(RSI, mem(RDX, 0));
    a_.mov(mem(RDX, 0), RCX);
    release(RSI);
}

void Baseline::call(std::uint32_t i, const Ins& in)
{
    sideDoor();
    if (optimizing() && (i >= code_->callProf.size() || !code_->callProf[i]) && !isFunction(constantBefore(i, in.b)))
        immature(i);
    Label& bad = slow(i, nullptr, true);
    throws_.emplace_back(i, Label{});
    Label& exc = throws_.back().second;
    Label helper, enter, native, afterCall;
    savePool(i);
    const unsigned base = in.b, argc = in.c;
    const unsigned window = code_->nregs > base + 1 ? code_->nregs - (base + 1) : 0;
    // Caller registers past the arguments the callee's frame may cover: dead here (temporaries above a call's operands),
    // a plain one overwritten without a look, one that may hold a cell looked at.
    unsigned looked = 0;
    for (unsigned r = 1 + argc; r < window; ++r)
        looked += windowPlain(i, base + 1 + r) ? 0 : 1;
    const bool roomy = window <= 1 + argc + 6 || (looked <= 6 && window <= 1 + argc + 40);
    const bool fast = in.opcode() == Op::Call && roomy;
    if (fast) {
        // A script function that is compiled, takes at least as many parameters as there are arguments and needs nothing
        // the interpreter's call sequence does on the side: its frame is pushed here.
        a_.mov(RAX, R(base));
        needObject(RAX, helper);
        a_.cmpI8(mem(RAX, kHdrCls), std::uint8_t(ObjClass::Function));
        a_.jcc(CNE, helper);
        a_.testI32(mem(RAX, kFnFlags), JSFunction::kNative);
        a_.jcc(CNE, helper);
        a_.mov(RCX, mem(RAX, kFnCode));
        if (code_->callProf.empty())
            code_->callProf.assign(code_->code.size(), 0);
        a_.mov(RDI, mem(RCX, kCbUid));
        a_.movImm(RSI, reinterpret_cast<std::uint64_t>(&code_->callProf[i]));
        a_.mov(mem(RSI, 0), RDI);
        a_.mov(RDX, mem(RCX, kCbTier1));
        a_.test(RDX, RDX);
        a_.jcc(CE, helper);
        a_.testI32(mem(RCX, kCbFlags), CodeBlock::kClassConstructor | CodeBlock::kGenerator | CodeBlock::kAsync |
                                           CodeBlock::kAsyncGen | CodeBlock::kNeedsArgs);
        a_.jcc(CNE, helper);
        a_.movzx16(RSI, mem(RCX, kCbNparams));
        a_.aluI32(7, RSI, std::int32_t(argc));
        a_.jcc(CB, helper);
        a_.mov32(RSI, mem(RCX, kCbNregs));
        a_.lea(R9, R(base + 1));
        // The frame and one past it filled: the stack is never filled closer than 8 to its end (`VM::fillStack`).
        a_.lea(R10, mem(R9, RSI, 8, 8));
        a_.alu(7, R10, mem(R12, kVmStackInit));
        a_.jcc(CA, helper);
        topFrame();
        a_.alu(7, R11, mem(R12, kVmFrameLimit));
        a_.jcc(CAE, helper);
        a_.cmpI32(mem(R12, kVmTicks), 1);
        a_.jcc(CBE, helper);
        a_.alu(7, RSP, mem(R12, kVmStackFloor));
        a_.jcc(CB, helper);
        // What the frame sits on: caller registers above the arguments hold stale values; the call sequence of the
        // interpreter lets them go. Plain values are overwritten here; a cell takes the long way.
        clearWindow(i, base, argc, window, helper);
        {
            // A sloppy function called with no `this` gets the global object.
            Label thisOk;
            a_.testI32(mem(RCX, kCbFlags), CodeBlock::kStrict | CodeBlock::kArrow | CodeBlock::kNoThis);
            a_.jcc(CNE, thisOk);
            a_.mov(RDI, R(base + 1));
            a_.aluI(4, RDI, -9);
            a_.cmpI(RDI, std::int32_t(kNull));
            a_.jcc(CNE, thisOk);
            a_.movImm(RDI, reinterpret_cast<std::uint64_t>(vm_.global));
            a_.inc32(mem(RDI, kHdrRc));
            a_.mov(R(base + 1), RDI);
            a_.bind(thisOk);
        }
        a_.dec32(mem(R12, kVmTicks));
        a_.movImm(RDI, reinterpret_cast<std::uint64_t>(&code_->code[i + 1]));
        a_.mov(mem(R13, kFramePc), RDI);
        topFrame();
        a_.lea(RDI, mem(R11, std::int32_t(sizeof(Frame))));
        a_.mov(mem(R12, kVmFrameTop), RDI);
        a_.mov(mem(R11, kFrameCode), RCX);
        a_.mov(mem(R11, kFrameFn), RAX);
        a_.mov(mem(R11, kFramePc), 0);
        a_.mov(mem(R11, kFrameR), R9);
        a_.movImm(RDI, (std::uint64_t(argc) << 32) | in.a);
        a_.mov(mem(R11, kFrameRetDst), RDI);
        a_.mov(mem(R11, kFrameEntry - 1), std::int32_t(0x100));
        a_.mov(mem(R11, kFrameNewTarget), std::int32_t(kUndefined));
        a_.mov(mem(R11, kFrameGen), std::int32_t(kUndefined));
        a_.mov(RSI, R11);
        a_.mov(RDI, R9);
        a_.mov(RDX, mem(RDX, kTierEntry));
        a_.jmp(enter);
    }
    if (in.opcode() == Op::New && roomy) {
        // `new f(...)` of an ordinary compiled function: `this` made by a call into the runtime, the frame pushed here.
        a_.mov(RAX, R(base));
        needObject(RAX, helper);
        a_.cmpI8(mem(RAX, kHdrCls), std::uint8_t(ObjClass::Function));
        a_.jcc(CNE, helper);
        a_.testI32(mem(RAX, kFnFlags), JSFunction::kNative);
        a_.jcc(CNE, helper);
        a_.mov32(RSI, mem(R12, kVmFnProtoShape));
        a_.alu32(7, RSI, mem(RAX, 0));
        a_.jcc(CNE, helper);
        a_.mov(RCX, mem(RAX, kFnCode));
        a_.cmpI8(mem(RCX, kCbConstructor), 0);
        a_.jcc(CE, helper);
        a_.mov(RDX, mem(RCX, kCbTier1));
        a_.test(RDX, RDX);
        a_.jcc(CE, helper);
        a_.testI32(mem(RCX, kCbFlags), CodeBlock::kGenerator | CodeBlock::kAsync | CodeBlock::kAsyncGen |
                                           CodeBlock::kNeedsArgs);
        a_.jcc(CNE, helper);
        a_.movzx16(RSI, mem(RCX, kCbNparams));
        a_.aluI32(7, RSI, std::int32_t(argc));
        a_.jcc(CB, helper);
        a_.mov32(RSI, mem(RCX, kCbNregs));
        a_.lea(R9, R(base + 1));
        // The frame and one past it filled: the stack is never filled closer than 8 to its end (`VM::fillStack`).
        a_.lea(R10, mem(R9, RSI, 8, 8));
        a_.alu(7, R10, mem(R12, kVmStackInit));
        a_.jcc(CA, helper);
        topFrame();
        a_.alu(7, R11, mem(R12, kVmFrameLimit));
        a_.jcc(CAE, helper);
        a_.cmpI32(mem(R12, kVmTicks), 1);
        a_.jcc(CBE, helper);
        a_.alu(7, RSP, mem(R12, kVmStackFloor));
        a_.jcc(CB, helper);
        clearWindow(i, base, argc, window, helper);
        a_.movImm(RDI, reinterpret_cast<std::uint64_t>(&code_->code[i + 1]));
        a_.mov(mem(R13, kFramePc), RDI);
        {
            // `this` made here when `f.prototype` is an object that is already some shape tree's prototype.
            Label viaRuntime, made;
            a_.mov(RSI, mem(RAX, kObjSlots));
            a_.test(RSI, RSI);
            a_.jcc(CE, viaRuntime);
            a_.mov(RSI, mem(RSI, 16));
            needObject(RSI, viaRuntime);
            a_.testI8(mem(RSI, kHdrFlags), cellflags::kHasRare);
            a_.jcc(CE, viaRuntime);
            a_.mov(RSI, mem(RSI, kObjSlots));
            a_.mov(RSI, mem(RSI, -16));
            a_.mov(RSI, mem(RSI, kRareChildRoot));
            a_.test(RSI, RSI);
            a_.jcc(CE, viaRuntime);
            a_.mov32(R8, mem(RSI, kShapeId));
            Label big;
            a_.cmpI8(mem(RCX, kCbThisSlots), 3);
            a_.jcc(CNE, big);
            if (allocObject(0, 3, viaRuntime, true))
                a_.jmp(made);
            else
                a_.jmp(viaRuntime);
            a_.bind(big);
            if (allocObject(0, 8, viaRuntime, true))
                a_.jmp(made);
            a_.bind(viaRuntime);
            a_.mov(RAX, R(base));
            a_.mov(RDI, R12);
            a_.mov(RSI, RAX);
            a_.movImm(RAX, reinterpret_cast<std::uint64_t>(&scriptVmJitNewThis));
            a_.callReg(RAX);
            a_.test(RAX, RAX);
            a_.jcc(CE, exc);
            a_.bind(made);
        }
        a_.mov(RDI, R(base + 1));
        a_.mov(R(base + 1), RAX);
        release(RDI);
        a_.mov(RAX, R(base));
        a_.mov(RCX, mem(RAX, kFnCode));
        a_.mov(RDX, mem(RCX, kCbTier1));
        a_.lea(R9, R(base + 1));
        a_.dec32(mem(R12, kVmTicks));
        topFrame();
        a_.lea(RDI, mem(R11, std::int32_t(sizeof(Frame))));
        a_.mov(mem(R12, kVmFrameTop), RDI);
        a_.mov(mem(R11, kFrameCode), RCX);
        a_.mov(mem(R11, kFrameFn), RAX);
        a_.mov(mem(R11, kFramePc), 0);
        a_.mov(mem(R11, kFrameR), R9);
        a_.movImm(RDI, (std::uint64_t(argc) << 32) | in.a);
        a_.mov(mem(R11, kFrameRetDst), RDI);
        a_.mov(mem(R11, kFrameEntry - 1), std::int32_t(0x101));
        a_.mov(mem(R11, kFrameNewTarget), RAX);
        a_.mov(mem(R11, kFrameGen), std::int32_t(kUndefined));
        a_.mov(RSI, R11);
        a_.mov(RDI, R9);
        a_.mov(RDX, mem(RDX, kTierEntry));
        a_.jmp(enter);
    }
    a_.bind(helper);
    if (in.opcode() == Op::Call && argc <= 1) {
        // `a.push(v)` with room for it, `a.pop()` of an element that is there: done here on a plain array.
        Label generic;
        a_.mov(RAX, R(base));
        needObject(RAX, generic);
        a_.cmpI8(mem(RAX, kHdrCls), std::uint8_t(ObjClass::Function));
        a_.jcc(CNE, generic);
        a_.testI32(mem(RAX, kFnFlags), JSFunction::kNative);
        a_.jcc(CE, generic);
        a_.mov(RCX, mem(RAX, kObjSlots));
        a_.mov(RCX, mem(RCX, -16));
        a_.movImm(RDX, reinterpret_cast<std::uint64_t>(argc == 1 ? arrayPushNative() : arrayPopNative()));
        a_.cmp(RDX, mem(RCX, kRareNative));
        a_.jcc(CNE, generic);
        a_.mov(RSI, R(base + 1));
        needObject(RSI, generic);
        a_.cmpI8(mem(RSI, kHdrCls), std::uint8_t(ObjClass::Array));
        a_.jcc(CNE, generic);
        a_.testI8(mem(RSI, kHdrFlags), std::uint8_t(cellflags::kFrozen | cellflags::kNotExtensible));
        a_.jcc(CNE, generic);
        a_.mov32(RCX, mem(RSI, kArrayLength));
        a_.mov(RDX, mem(RSI, kArrayElems));
        if (argc == 1) {
            a_.alu32(7, RCX, mem(RSI, kArrayCapacity));
            a_.jcc(CAE, generic);
            a_.aluI32(7, RCX, 0x7ffffffe);
            a_.jcc(CA, generic);
            Label number;
            a_.mov(RAX, R(base + 2));
            retain(RAX);
            a_.test(RAX, R14);
            a_.jcc(CNE, number);
            a_.andI8(mem(RSI, kHdrFlags), std::uint8_t(~cellflags::kAllNumbers));
            a_.bind(number);
            a_.mov(mem(RDX, RCX, 8), RAX);
            a_.aluI32(0, RCX, 1);
            a_.mov32(mem(RSI, kArrayLength), RCX);
            a_.mov32(RAX, RCX);
            a_.or_(RAX, R14);
        } else {
            a_.test32(RCX, RCX);
            a_.jcc(CE, generic);
            a_.aluI32(5, RCX, 1);
            a_.mov(RAX, mem(RDX, RCX, 8));
            a_.cmpI(RAX, std::int32_t(kHole));
            a_.jcc(CE, generic);
            a_.mov32(mem(RSI, kArrayLength), RCX);
        }
        write(in.a, RAX);
        a_.jmp(afterCall);
        a_.bind(generic);
    }
    if (in.opcode() == Op::Call && argc == 1) {
        // `s.charCodeAt(i)`: the character read here.
        Label generic;
        a_.mov(RAX, R(base));
        needObject(RAX, generic);
        a_.cmpI8(mem(RAX, kHdrCls), std::uint8_t(ObjClass::Function));
        a_.jcc(CNE, generic);
        a_.testI32(mem(RAX, kFnFlags), JSFunction::kNative);
        a_.jcc(CE, generic);
        a_.mov(RCX, mem(RAX, kObjSlots));
        a_.mov(RCX, mem(RCX, -16));
        a_.movImm(RDX, reinterpret_cast<std::uint64_t>(stringCharNative()));
        a_.cmp(RDX, mem(RCX, kRareNative));
        a_.jcc(CNE, generic);
        a_.cmpI(mem(RCX, kRareData), 1);
        a_.jcc(CNE, generic);
        a_.mov(RSI, R(base + 1));
        needString(RSI, generic);
        a_.mov(RDX, R(base + 2));
        needInt(RDX, generic);
        a_.mov32(RDX, RDX);
        a_.alu32(7, RDX, mem(RSI, kStrLength));
        a_.jcc(CAE, generic);  // also a negative index
        stringChars(RSI, RCX, generic);
        a_.movzx16(RAX, mem(RCX, RDX, 2));
        a_.or_(RAX, R14);
        write(in.a, RAX, false);
        a_.jmp(afterCall);
        a_.bind(generic);
    }
    if (in.opcode() == Op::Call && argc == 1) {
        // `String.fromCharCode(c)` of a character below 256: the string the VM keeps for it.
        Label generic;
        a_.mov(RAX, R(base));
        needObject(RAX, generic);
        a_.cmpI8(mem(RAX, kHdrCls), std::uint8_t(ObjClass::Function));
        a_.jcc(CNE, generic);
        a_.testI32(mem(RAX, kFnFlags), JSFunction::kNative);
        a_.jcc(CE, generic);
        a_.mov(RCX, mem(RAX, kObjSlots));
        a_.mov(RCX, mem(RCX, -16));
        a_.movImm(RDX, reinterpret_cast<std::uint64_t>(stringFromCharCodeNative()));
        a_.cmp(RDX, mem(RCX, kRareNative));
        a_.jcc(CNE, generic);
        a_.mov(RDX, R(base + 2));
        needInt(RDX, generic);
        a_.aluI32(4, RDX, 0xFFFF);
        a_.aluI32(7, RDX, 255);
        a_.jcc(CA, generic);
        a_.movImm(RCX, reinterpret_cast<std::uint64_t>(&vm_.smallStrings[0]));
        a_.mov(RAX, mem(RCX, RDX, 8));
        a_.cmpI(RAX, std::int32_t(kUndefined));
        a_.jcc(CE, generic);
        retain(RAX);
        write(in.a, RAX);
        a_.jmp(afterCall);
        a_.bind(generic);
    }
    if (in.opcode() == Op::Call && argc == 1) {
        // `Math.floor(x)` and its kind: a native whose data is a function of one double, called without the interpreter.
        Label generic, notInt;
        a_.mov(RAX, R(base));
        needObject(RAX, generic);
        a_.cmpI8(mem(RAX, kHdrCls), std::uint8_t(ObjClass::Function));
        a_.jcc(CNE, generic);
        a_.testI32(mem(RAX, kFnFlags), JSFunction::kNative);
        a_.jcc(CE, generic);
        a_.mov(RCX, mem(RAX, kObjSlots));
        a_.mov(RCX, mem(RCX, -16));
        a_.movImm(RDX, reinterpret_cast<std::uint64_t>(mathUnaryNative()));
        a_.cmp(RDX, mem(RCX, kRareNative));
        a_.jcc(CNE, generic);
        a_.mov(RSI, R(base + 2));
        needNumber(RSI, generic);
        a_.mov(RAX, mem(RCX, kRareData));
        loadNumber(0, RSI, RDX);
        a_.callReg(RAX);
        boxNumber0();
        write(in.a, RAX);
        a_.jmp(afterCall);
        a_.bind(generic);
    }
    if (in.opcode() == Op::Call) {
        // Any other native: called directly.
        Label notNative;
        a_.mov(RAX, R(base));
        needObject(RAX, notNative);
        a_.cmpI8(mem(RAX, kHdrCls), std::uint8_t(ObjClass::Function));
        a_.jcc(CNE, notNative);
        a_.testI32(mem(RAX, kFnFlags), JSFunction::kNative);
        a_.jcc(CE, notNative);
        a_.testI8(mem(RAX, kHdrFlags), cellflags::kHasRare);
        a_.jcc(CE, notNative);
        a_.movImm(RDI, reinterpret_cast<std::uint64_t>(&code_->code[i + 1]));
        a_.mov(mem(R13, kFramePc), RDI);
        a_.lea(RDI, R(code_->nregs));
        a_.mov(mem(R12, kVmStackTop), RDI);
        a_.mov(RDI, R12);
        a_.mov(RSI, RAX);
        a_.lea(RDX, R(base + 1));
        a_.movImm(RCX, argc);
        a_.movImm(RAX, reinterpret_cast<std::uint64_t>(&scriptVmJitNative));
        a_.callReg(RAX);
        a_.jmp(native);
        a_.bind(notNative);
    }
    a_.mov(RDI, R12);
    a_.movImm(RSI, i);
    a_.call(call_);
    a_.test(RDX, RDX);
    a_.jcc(CE, bad);
    a_.cmpI(RDX, 1);
    a_.jcc(CE, native);
    a_.mov(RSI, RAX);
    a_.mov(RDI, mem(RAX, kFrameR));
    a_.bind(enter);
    a_.callReg(RDX);
    a_.bind(native);
    a_.test(RAX, RAX);
    a_.jcc(CE, exc);
    write(in.a, RAX);
    a_.bind(afterCall);
    restorePool(i);
}

void Baseline::ret(std::uint32_t i, const Ins& in, bool numberInRax)
{
    // `numberInRax`: the result is a number the optimized code has boxed in rax already, which needs no count.
    const unsigned first = 1 + code_->nparams;
    // A result in a register this frame clears on its way out goes with that register's count (optimized code).
    const bool taken = optimizing() && !numberInRax && in.a >= first && in.a < code_->nregs;
    Label have;
    if (!numberInRax)
        a_.mov(RAX, R(in.a));
    if (taken && (code_->constructor || (code_->flags & CodeBlock::kClassConstructor))) {
        // A constructor's result that is not an object is `this`; the register's value is then let go.
        Label useThis, counted;
        a_.cmpI8(mem(R13, kFrameConstruct), 0);
        a_.jcc(CE, counted);
        a_.test(RAX, R15);
        a_.jcc(CNE, useThis);
        a_.cmpI8(mem(RAX, kHdrKind), kObjectKind);
        a_.jcc(CE, counted);
        a_.bind(useThis);
        a_.mov(RDX, R(in.a));
        a_.mov(R(in.a), std::int32_t(kUndefined));
        release(RDX);
        a_.mov(RAX, R(0));
        retain(RAX);
        a_.bind(counted);
    } else if (taken) {
    } else if (code_->constructor || (code_->flags & CodeBlock::kClassConstructor)) {
        // A constructor's result that is not an object is `this`.
        Label useThis;
        a_.cmpI8(mem(R13, kFrameConstruct), 0);
        a_.jcc(CE, have);
        if (!numberInRax) {
            a_.test(RAX, R15);
            a_.jcc(CNE, useThis);
            a_.cmpI8(mem(RAX, kHdrKind), kObjectKind);
            a_.jcc(CE, have);
        }
        a_.bind(useThis);
        a_.mov(RAX, R(0));
        if (numberInRax) {
            Label counted;
            retain(RAX);
            a_.jmp(counted);
            a_.bind(have);
            a_.bind(counted);
        }
    }
    if (!numberInRax && !taken) {
        a_.bind(have);
        retain(RAX);
    }
    // Registers a call put its arguments in belong to the caller; those the call did not fill and everything above the
    // parameters are this frame's to clear. When none can hold a cell, nothing here calls out and rax stays.
    auto isPlain = [&](unsigned r) {
        return (taken && r == in.a) || (r < plainRegs_.size() && plainRegs_[r]) || plainAt(i, r);
    };
    unsigned cells = 0;
    for (unsigned r = first; r < code_->nregs; ++r)
        cells += isPlain(r) ? 0 : 1;
    // Inline: a dozen registers, or a few more when most need no look (two at a time).
    const bool inlineClear = code_->nregs <= first || code_->nregs - first <= 12 ||
                             (code_->nregs - first <= 40 && cells <= 8);
    bool plain = inlineClear;
    for (unsigned r = 1; r < code_->nregs && plain; ++r)
        plain = isPlain(r);
    if (!plain)
        a_.mov(mem(RSP, 0), RAX);
    for (unsigned p = 1; p <= code_->nparams; ++p) {
        Label skip;
        a_.cmpI32(mem(R13, kFrameArgc), std::int32_t(p));
        a_.jcc(CAE, skip);
        clearReg(p);
        a_.bind(skip);
    }
    if (code_->nregs > first) {
        const unsigned count = code_->nregs - first;
        if (inlineClear) {
            // Two registers that never hold a cell at a time, from xmm0 (two undefineds); a release calls out (xmm0 goes).
            bool pair = false;
            for (unsigned r = first; r < code_->nregs;) {
                if (r + 1 < code_->nregs && isPlain(r) && isPlain(r + 1)) {
                    if (!pair) {
                        a_.movImm(RDX, kUndefined);
                        a_.movqToX(0, RDX);
                        a_.punpcklqdq(0, 0);
                        pair = true;
                    }
                    a_.movupsStore(R(r), 0);
                    r += 2;
                } else {
                    pair = pair && isPlain(r);
                    if (isPlain(r))
                        a_.mov(R(r), std::int32_t(kUndefined));
                    else
                        clearReg(r);
                    r += 1;
                }
            }
        } else {
            if (taken)
                a_.mov(R(in.a), std::int32_t(kUndefined));  // its count went with the result
            a_.lea(RDI, R(first));
            a_.movImm(RSI, count);
            a_.callAbs(reinterpret_cast<const void*>(&scriptVmJitClear));
        }
    }
    // This frame is one past the top once it is gone.
    a_.mov(mem(R12, kVmFrameTop), R13);
    if (!plain)
        a_.mov(RAX, mem(RSP, 0));
    // The way out in place (as at exit_).
    a_.aluI(0, RSP, optimizing() ? 8 + spill_ : 8);
    a_.pop(R13);
    a_.pop(RBX);
    a_.ret();
}

#include "opt_emit.inc"
#include "opt_inline.inc"

Tier2Code* compileOptimizedCode(VM& vm, CodeBlock* code)
{
    if (!code->tier1)
        return nullptr;
#if defined(SCRIPT_JIT_ENV_OVERRIDE)
    if (const char* f = std::getenv("T2FAIL")) {
        // Testing: treat these instructions as having failed a guard.
        for (const char* q = f; *q;) {
            const unsigned long v = std::strtoul(q, nullptr, 10);
            if (v < code->code.size())
                code->prof[v] |= opt::kProfFailed;
            while (*q && *q != ',')
                ++q;
            if (*q == ',')
                ++q;
        }
    }
#endif
    opt::Plan plan = opt::analyse(*code);
    if (!plan.ok)
        return nullptr;
#if defined(SCRIPT_JIT_ENV_OVERRIDE)
    if (std::getenv("T2DBG")) {
        std::fprintf(stderr, "tier2 compile: %s n=%u tries=%d webs=%zu prof:", code->name ? toUtf8(static_cast<StringCell*>(code->name)->text()).c_str() : "anon",
                     plan.n, int(code->tier2Tries), plan.webs.size());
        for (std::uint32_t i = 0; i < plan.n && i < 60; ++i)
            std::fprintf(stderr, " %u", unsigned(code->prof[i]));
        std::fprintf(stderr, "\n");
        for (std::size_t w = 0; w < plan.webs.size(); ++w)
            std::fprintf(stderr, "  web %zu reg %u rep %d loc %s%d [%u,%u]\n", w, plan.webs[w].reg, int(plan.webs[w].rep),
                         plan.webs[w].loc.kind == opt::Loc::Reg ? "r" : "s", int(plan.webs[w].loc.reg), plan.webs[w].first, plan.webs[w].last);
        std::fprintf(stderr, "%s\n", dump(*code).c_str());
    }
#endif
    // A loop whose shapes were taken as known turned out to call out: again, without that for it.
    std::set<std::uint32_t> noHoist;
    for (int attempt = 0; attempt < 4; ++attempt) {
        Baseline compiler(vm, code);
        compiler.noHoist_ = noHoist;
        Tier2Code* t = compiler.optimize(plan, code->tier1);
        if (compiler.hoistBroken_.empty())
            return t;
        noHoist.insert(compiler.hoistBroken_.begin(), compiler.hoistBroken_.end());
#if defined(SCRIPT_JIT_ENV_OVERRIDE)
        if (std::getenv("T2HOIST"))
            std::fprintf(stderr, "hoist: a loop calls out; compiled again\n");
#endif
    }
    return nullptr;
}

}  // namespace

void releaseTier2(Tier2Code* code, VM* vm) noexcept
{
    while (code) {
        if (vm) {
            for (ValidityCell* v : code->validities)
                vm->releaseValidity(v);
            if (!vm->tearingDown)
                for (JSValue v : code->held)
                    release(v);
        }
        CodeSpan span;
        span.rx = code->rx;
        span.bytes = code->bytes;
        releaseCode(span);
        Tier2Code* older = code->older;
        delete code;
        code = older;
    }
}

bool tier1Available() noexcept
{
    return entryStub() != nullptr;
}

void compileTier1(VM& vm, CodeBlock* code)
{
    code->tier1Failed = true;
    if (code->flags & (CodeBlock::kGenerator | CodeBlock::kAsync | CodeBlock::kAsyncGen))
        return;
    const std::size_t limit = vm.ctx.options.jit.codeCacheLimit;
    if (code->code.size() > 20000 || !entryStub() || (limit && codeInUse() > limit))
        return;
    Baseline compiler(vm, code);
    Tier1Code* t = compiler.compile();
    if (!t)
        return;
    code->tier1 = t;
    thisSlotsOf(code);
    {
        bool reads = false;
        for (std::uint32_t j = 0; j < code->code.size() && !reads; j = opt::nextOf(*code, j)) {
            const opt::Effects e = opt::effectsOf(*code, j);
            reads = !e.ok;
            for (std::uint16_t r : e.rd)
                reads = reads || r == 0;
        }
        if (!reads)
            code->flags |= CodeBlock::kNoThis;
    }
    code->tier1Failed = false;
    code->tier2Counter = vm.tier2Threshold > 0 ? vm.tier2Threshold : 0x40000000;
    SCRIPT_VM_CENSUS(Tier1Compiled);
#if defined(SCRIPT_JIT_ENV_OVERRIDE)
    // SCRIPT_VM_PERFMAP=1: name the code for `perf report` in /tmp/perf-<pid>.map.
    static std::FILE* map = std::getenv("SCRIPT_VM_PERFMAP")
                                ? std::fopen(("/tmp/perf-" + std::to_string(getpid()) + ".map").c_str(), "w")
                                : nullptr;
    if (const char* dir = std::getenv("SCRIPT_VM_CODEDUMP")) {
        // Raw code for `objdump -D -b binary -mi386:x86-64 --adjust-vma=<rx>`.
        static int serial = 0;
        const std::string nm = code->name ? toUtf8(static_cast<StringCell*>(code->name)->text()) : std::string("anon");
        const std::string path = std::string(dir) + "/t1-" + std::to_string(++serial) + "-" + nm + ".bin";
        if (std::FILE* f = std::fopen(path.c_str(), "wb")) {
            std::fwrite(t->rx, 1, t->bytes, f);
            std::fclose(f);
        }
    }
    if (map) {
        std::fprintf(map, "%llx %zx js:%s:%u\n", static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(t->rx)),
                     t->bytes, code->name ? toUtf8(static_cast<StringCell*>(code->name)->text()).c_str() : "anon",
                     code->lines.empty() ? 0u : code->lines[0]);
        std::fflush(map);
    }
#endif
}

void releaseTier1(Tier1Code* code, VM* vm) noexcept
{
    if (!code)
        return;
    if (vm)
        for (ValidityCell* v : code->validities)
            vm->releaseValidity(v);
    CodeSpan span;
    span.rx = code->rx;
    span.bytes = code->bytes;
    releaseCode(span);
    delete code;
}

JSValue enterTier1(VM& vm, Tier1Code* code, std::uint32_t index)
{
    Frame* fr = vm.frameTop - 1;
    {
        std::uintptr_t floor = nativeStackFloorCache;
        if (floor == 0)
            floor = nativeStackFloorLookup();
        vm.jitStackFloor = floor == 1 ? 0 : floor + kStackReserve;
    }
    const Stubs& st = stubs();
    const std::uint64_t bits = index == 0 ? st.run(&vm, fr, nullptr, code->entry)
                                          : st.run(&vm, fr, code->addressFor(index), st.osrEnter);
    if (bits == 0 && pendingException) [[unlikely]] {
        std::exception_ptr e = std::move(pendingException);
        pendingException = nullptr;
        std::rethrow_exception(e);
    }
    return JSValue::raw(bits);
}

#else

bool tier1Available() noexcept { return false; }
void compileTier1(VM&, CodeBlock* code) { code->tier1Failed = true; }
void releaseTier1(Tier1Code*, VM*) noexcept {}
void releaseTier2(Tier2Code*, VM*) noexcept {}
JSValue enterTier1(VM&, Tier1Code*, std::uint32_t) { return JSValue::exception(); }

#endif

CodeBlock::~CodeBlock()
{
    if (tier2)
        releaseTier2(tier2, nullptr);
    if (tier1)
        releaseTier1(tier1, nullptr);
}

}  // namespace vm
}  // namespace internal
}  // namespace script
