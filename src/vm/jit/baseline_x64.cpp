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
#include <algorithm>
#include <exception>
#include <utility>

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

void emitPrologue(X64& a)
{
    a.push(RBX);
    a.push(R13);
    a.aluI(5, RSP, 8);
    a.mov(R13, RSI);
    a.mov(RBX, mem(R13, std::int32_t(offsetof(Frame, R))));
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
    Frame* fr = vm->frameBase + vm->frameCount - 1;
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
        Frame* fr = vm->frameBase + vm->frameCount - 1;
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
        Frame* fr = vm->frameBase + vm->frameCount - 1;
        CodeBlock* code = fr->code;
        if (index < code->prof.size())
            code->prof[index] |= opt::kProfFailed;
        SCRIPT_VM_CENSUS(Tier2Exit);
        if (++code->optExits > 40)
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
        Frame* fr = vm->frameBase + vm->frameCount - 1;
        CodeBlock* code = fr->code;
        if (!code->tier1 || code->tier2Tries >= 3) {
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
                code->tier2Tries = 3;
                return nullptr;
            }
            t->older = code->tier2;
            code->tier2 = t;
            code->tier1->entry = t->entry;
            code->optExits = 0;
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
    Frame* fr = vm.frameBase + vm.frameCount - 1;
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
        compileTier1(vm, c);
        if (!c->tier1)
            return {0, 0};
    }
    JSValue* newR = R + base + 1;
    if (vm.ticks <= 1 || vm.frameCount + 2 >= vm.frameCapacity || newR + c->nregs + 8 > vm.stackEnd || nativeStackLow())
        return {0, 0};
    fr->pc = ins + 1;
    --vm.ticks;
    if (isNew) {
        const JSValue pv = *f->slotAt(2);
        JSObject* made = vm.newObject(isObject(pv) ? asObject(pv) : vm.protos.object);
        const JSValue old = R[base + 1];
        R[base + 1] = toJSValue(made);
        release(old);
    }
    if (!(c->flags & (CodeBlock::kStrict | CodeBlock::kArrow)) && newR[0].isNullish()) {
        const JSValue old = newR[0];
        newR[0] = toJSValue(vm.global);
        retain(newR[0]);
        release(old);
    }
    const std::uint32_t keep = 1 + (argc < c->nparams ? argc : c->nparams);
    if (newR + c->nregs > vm.stackInit) [[unlikely]]
        vm.fillStack(newR + c->nregs);
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
    vm.frameBase[vm.frameCount++] =
        Frame{c, f, nullptr, newR, std::uint16_t(ins->a), argc, isNew, true, isNew ? callee : JSValue::undefined()};
    return {reinterpret_cast<std::uint64_t>(&vm.frameBase[vm.frameCount - 1]),
            reinterpret_cast<std::uint64_t>(c->tier1->entry)};
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
constexpr std::int32_t kVmStackInit = std::int32_t(offsetof(VM, stackInit));
constexpr std::int32_t kVmFrameCapacity = std::int32_t(offsetof(VM, frameCapacity));
constexpr std::int32_t kVmFrameBase = std::int32_t(offsetof(VM, frameBase));
constexpr std::int32_t kVmGlobalEpoch = std::int32_t(offsetof(VM, globalEpoch));
constexpr std::int32_t kVmStackFloor = std::int32_t(offsetof(VM, jitStackFloor));
constexpr std::int32_t kFnCode = std::int32_t(offsetof(JSFunction, code));
constexpr std::int32_t kFnFlags = std::int32_t(offsetof(JSFunction, flags));
constexpr std::int32_t kCbTier1 = std::int32_t(offsetof(CodeBlock, tier1));
constexpr std::int32_t kCbFlags = std::int32_t(offsetof(CodeBlock, flags));
constexpr std::int32_t kCbNparams = std::int32_t(offsetof(CodeBlock, nparams));
constexpr std::int32_t kCbNregs = std::int32_t(offsetof(CodeBlock, nregs));
constexpr std::int32_t kTierEntry = std::int32_t(offsetof(Tier1Code, entry));
constexpr std::int32_t kFrameArgc = std::int32_t(offsetof(Frame, argc));
constexpr std::int32_t kFrameConstruct = std::int32_t(offsetof(Frame, construct));
constexpr std::int32_t kVmTicks = std::int32_t(offsetof(VM, ticks));
constexpr std::int32_t kVmFrameCount = std::int32_t(offsetof(VM, frameCount));
constexpr std::int32_t kArrayElems = std::int32_t(offsetof(JSArray, elems));
constexpr std::int32_t kArrayLength = std::int32_t(offsetof(JSArray, length));
#pragma GCC diagnostic pop
constexpr std::int32_t kTypedData = std::int32_t(offsetof(JSTypedArray, data));
constexpr std::int32_t kTypedLength = std::int32_t(offsetof(JSTypedArray, length));
constexpr std::int32_t kTypedKind = std::int32_t(offsetof(JSTypedArray, kind));
constexpr std::int32_t kRareNative = std::int32_t(offsetof(Rare, native));
constexpr std::int32_t kRareData = std::int32_t(offsetof(Rare, nativeData));
constexpr std::int32_t kObjSlots = 16;
constexpr std::int32_t kObjInline = 24;
constexpr std::int32_t kBoxValue = 16;
constexpr std::int32_t kFnUpvals = std::int32_t(sizeof(JSFunction));
constexpr std::uint8_t kObjectKind = 0x80 | std::uint8_t(Kind::Object);
constexpr std::int32_t kHdrCls = 4, kHdrFlags = 5, kHdrAux = 6, kHdrKind = 7, kHdrRc = 8;
constexpr std::int32_t kValidityValid = 4;

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
    bool delivered_ = false;           ///< the template just emitted put a typed destination in its variable
    struct ExitReq
    {
        std::uint32_t index;
        int kind;  ///< 0: run the instruction in the interpreter; 1: go on after it (it has run)
        Label label;
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
    void unboxRax(std::int32_t w, Label& fail);
    void loadTypedAfter(std::uint32_t i);
    void savePool(std::uint32_t i);
    void restorePool(std::uint32_t i);
    Label& exitLabel(std::uint32_t i, int kind);
    Label& exitBefore(std::uint32_t i) { return exitLabel(i, 0); }
    Label& exitAfter(std::uint32_t i) { return exitLabel(i, 1); }
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
        if (oldIsNumber) {
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
        a_.cvttsd2si32(dst, 0);
        a_.aluI32(7, dst, INT32_MIN);
        a_.jcc(CE, bad);
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
    void boxDouble0()
    {
        Label ok;
        a_.movqFromX(RAX, 0);
        a_.ucomisd(0, 0);
        a_.jcc(CNP, ok);
        a_.movImm(RAX, kCanonicalNaN);
        a_.bind(ok);
        a_.add(RAX, RBP);
    }

    void callStep(std::uint32_t i)
    {
        a_.mov(RDI, R12);
        a_.movImm(RSI, i);
        a_.call(step_);
    }
    void emitStep(std::uint32_t i)
    {
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
        a_.mov32(RDX, mem(R12, kVmTicks));
        a_.aluI32(5, RDX, 1);
        if (optimizing()) {
            // The optimizing code may not leave for the interpreter here (its variables are in registers): a stub makes the
            // call that looks at the flag and comes back.
            tickStubs_.emplace_back(i, Label{});
            Label& stub = tickStubs_.back().second;
            a_.jcc(CE, stub);
            a_.mov32(mem(R12, kVmTicks), RDX);
            tickBack_.emplace_back(Label{});
            a_.bind(tickBack_.back());
            return;
        }
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
    void clearReg(unsigned r)
    {
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
    void putElem(std::uint32_t i, const Ins& in);
    void ret(std::uint32_t i, const Ins& in);
    void call(std::uint32_t i, const Ins& in);
    void getGlobal(std::uint32_t i, const Ins& in);
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
    case Op::JNotUndefined: case Op::JHole:
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
    case Op::InitProp:
        initProp(i, in);
        return;
    case Op::Closure:
        helperOp(i, &helperThunk<jitClosure>);
        return;
    case Op::NewBox:
        helperOp(i, &helperThunk<jitNewBox>);
        return;
    case Op::NewObject:
        helperOp(i, &helperThunk<jitNewObject>);
        return;
    case Op::NewArray:
        helperOp(i, &helperThunk<jitNewArray>);
        return;
    case Op::ArrayPush:
        helperOp(i, &helperThunk<jitArrayPush>);
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
        a_.imulI32(RAX, RAX, imm);
        a_.jcc(CO, bad);
        a_.test32(RAX, RAX);
        a_.jcc(CE, bad);
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
    const bool known = ic && ic->shape != 0 && (!ic->holder || ic->validity);
    const bool isLength = key == vm_.atoms.length;
    if (!known && !isLength) {
        helperOp(i, &helperThunk<jitGetProp>);
        return;
    }
    Label& bad = slow(i, &helperThunk<jitGetProp>);
    Label done, lengthPath;
    a_.mov(RAX, R(in.b));
    needObject(RAX, bad);
    if (known) {
        a_.cmpI32(mem(RAX, 0), std::int32_t(ic->shape));
        a_.jcc(CNE, isLength ? lengthPath : bad);
        if (ic->holder) {
            hold(ic->validity);
            a_.movImm(RCX, reinterpret_cast<std::uint64_t>(ic->validity));
            a_.cmpI32(mem(RCX, kValidityValid), 0);
            a_.jcc(CE, bad);
            a_.movImm(RAX, reinterpret_cast<std::uint64_t>(ic->holder));
            slotAddress(ic->index);
        } else {
            slotAddress(ic->index);
        }
        a_.mov(RAX, mem(RDX, 0));
        deliver(i, in.a);
        if (isLength)
            a_.jmp(done);
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
    a_.bind(done);
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
    a_.aluI32(7, RDX, int(E::Float64));
    a_.jcc(CE, f64);
    a_.aluI32(7, RDX, int(E::Int32));
    a_.jcc(CE, i32);
    a_.aluI32(7, RDX, int(E::Uint8));
    a_.jcc(CE, u8);
    a_.aluI32(7, RDX, int(E::Uint8Clamped));
    a_.jcc(CE, u8);
    a_.aluI32(7, RDX, int(E::Int8));
    a_.jcc(CE, s8);
    a_.aluI32(7, RDX, int(E::Int16));
    a_.jcc(CE, i16);
    a_.aluI32(7, RDX, int(E::Uint16));
    a_.jcc(CE, u16);
    a_.aluI32(7, RDX, int(E::Float32));
    a_.jcc(CE, f32);
    a_.jmp(bad);
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
    a_.jcc(CE, bad);  // a hole may stand for a property of the array's own
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
    a_.aluI32(7, RDX, int(E::Float64));
    a_.jcc(CE, f64);
    a_.aluI32(7, RDX, int(E::Int32));
    a_.jcc(CE, i32);
    a_.aluI32(7, RDX, int(E::Uint32));
    a_.jcc(CE, i32);
    a_.aluI32(7, RDX, int(E::Uint8));
    a_.jcc(CE, i8);
    a_.aluI32(7, RDX, int(E::Int8));
    a_.jcc(CE, i8);
    a_.aluI32(7, RDX, int(E::Int16));
    a_.jcc(CE, i16);
    a_.aluI32(7, RDX, int(E::Uint16));
    a_.jcc(CE, i16);
    a_.aluI32(7, RDX, int(E::Float32));
    a_.jcc(CE, f32);
    a_.jmp(bad);
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
    a_.movImm(RAX, reinterpret_cast<std::uint64_t>(g));
    slotAddress(index);
    a_.mov(RAX, mem(RDX, 0));
    deliver(i, in.a);
}

void Baseline::call(std::uint32_t i, const Ins& in)
{
    Label& bad = slow(i, nullptr, true);
    throws_.emplace_back(i, Label{});
    Label& exc = throws_.back().second;
    Label helper, enter, native, afterCall;
    savePool(i);
    const unsigned base = in.b, argc = in.c;
    const unsigned window = code_->nregs > base + 1 ? code_->nregs - (base + 1) : 0;
    const bool fast = in.opcode() == Op::Call && window <= 1 + argc + 6;
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
        a_.lea(R10, mem(R9, RSI, 8, 64));
        a_.alu(7, R10, mem(R12, kVmStackEnd));
        a_.jcc(CA, helper);
        a_.lea(R10, mem(R9, RSI, 8));
        a_.alu(7, R10, mem(R12, kVmStackInit));
        a_.jcc(CA, helper);
        a_.mov32(R10, mem(R12, kVmFrameCount));
        a_.aluI32(0, R10, 2);
        a_.alu32(7, R10, mem(R12, kVmFrameCapacity));
        a_.jcc(CAE, helper);
        a_.cmpI32(mem(R12, kVmTicks), 1);
        a_.jcc(CBE, helper);
        a_.alu(7, RSP, mem(R12, kVmStackFloor));
        a_.jcc(CB, helper);
        // What the frame sits on: caller registers above the arguments hold stale values; the call sequence of the
        // interpreter lets them go. Plain values are overwritten here; a cell takes the long way.
        for (unsigned r = 1 + argc; r < window; ++r) {
            Label skip;
            a_.aluI32(7, RSI, std::int32_t(r));
            a_.jcc(CBE, skip);
            a_.mov(RDI, mem(R9, std::int32_t(8 * r)));
            a_.test(RDI, R15);
            a_.jcc(CE, helper);
            a_.mov(mem(R9, std::int32_t(8 * r)), std::int32_t(kUndefined));
            a_.bind(skip);
        }
        {
            // A sloppy function called with no `this` gets the global object.
            Label thisOk;
            a_.testI32(mem(RCX, kCbFlags), CodeBlock::kStrict | CodeBlock::kArrow);
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
        a_.mov32(R10, mem(R12, kVmFrameCount));
        a_.mov(R11, mem(R12, kVmFrameBase));
        a_.imulI32(RDI, R10, std::int32_t(sizeof(Frame)));
        a_.add(R11, RDI);
        a_.inc32(mem(R12, kVmFrameCount));
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
        a_.mov(RDX, mem(RDX, kTierEntry));
        a_.jmp(enter);
    }
    a_.bind(helper);
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
    a_.mov(RDI, R12);
    a_.movImm(RSI, i);
    a_.call(call_);
    a_.test(RDX, RDX);
    a_.jcc(CE, bad);
    a_.cmpI(RDX, 1);
    a_.jcc(CE, native);
    a_.mov(RSI, RAX);
    a_.bind(enter);
    a_.callReg(RDX);
    a_.bind(native);
    a_.test(RAX, RAX);
    a_.jcc(CE, exc);
    write(in.a, RAX);
    a_.bind(afterCall);
    restorePool(i);
}

void Baseline::ret(std::uint32_t i, const Ins& in)
{
    (void)i;
    Label have;
    a_.mov(RAX, R(in.a));
    if (code_->constructor || (code_->flags & CodeBlock::kClassConstructor)) {
        // A constructor's result that is not an object is `this`.
        Label useThis;
        a_.cmpI8(mem(R13, kFrameConstruct), 0);
        a_.jcc(CE, have);
        a_.test(RAX, R15);
        a_.jcc(CNE, useThis);
        a_.cmpI8(mem(RAX, kHdrKind), kObjectKind);
        a_.jcc(CE, have);
        a_.bind(useThis);
        a_.mov(RAX, R(0));
    }
    a_.bind(have);
    retain(RAX);
    a_.mov(mem(RSP, 0), RAX);
    // Registers a call put its arguments in belong to the caller; those the call did not fill and everything above the
    // parameters are this frame's to clear.
    for (unsigned p = 1; p <= code_->nparams; ++p) {
        Label skip;
        a_.cmpI32(mem(R13, kFrameArgc), std::int32_t(p));
        a_.jcc(CAE, skip);
        clearReg(p);
        a_.bind(skip);
    }
    const unsigned first = 1 + code_->nparams;
    if (code_->nregs > first) {
        const unsigned count = code_->nregs - first;
        if (count <= 12) {
            for (unsigned r = first; r < code_->nregs; ++r)
                clearReg(r);
        } else {
            a_.lea(RDI, R(first));
            a_.movImm(RSI, count);
            a_.callAbs(reinterpret_cast<const void*>(&scriptVmJitClear));
        }
    }
    a_.dec32(mem(R12, kVmFrameCount));
    a_.mov(RAX, mem(RSP, 0));
    a_.jmp(exit_);
}

#include "opt_emit.inc"

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
    Baseline compiler(vm, code);
    return compiler.optimize(plan, code->tier1);
}

}  // namespace

void releaseTier2(Tier2Code* code, VM* vm) noexcept
{
    while (code) {
        if (vm)
            for (ValidityCell* v : code->validities)
                vm->releaseValidity(v);
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
    code->tier1Failed = false;
    code->tier2Counter = vm.tier2Threshold > 0 ? vm.tier2Threshold : 0x40000000;
    SCRIPT_VM_CENSUS(Tier1Compiled);
#if defined(SCRIPT_JIT_ENV_OVERRIDE)
    // SCRIPT_VM_PERFMAP=1: name the code for `perf report` in /tmp/perf-<pid>.map.
    static std::FILE* map = std::getenv("SCRIPT_VM_PERFMAP")
                                ? std::fopen(("/tmp/perf-" + std::to_string(getpid()) + ".map").c_str(), "w")
                                : nullptr;
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
    Frame* fr = vm.frameBase + vm.frameCount - 1;
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
