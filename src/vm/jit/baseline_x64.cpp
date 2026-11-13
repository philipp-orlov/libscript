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
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>

#include "script/internal/stack_guard.hpp"
#include "script/internal/vm/bytecode.hpp"
#include "script/internal/vm/census.hpp"
#include "script/internal/vm/object.hpp"

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

using Stub = std::uint64_t (*)(VM*, Frame*, void*);

Stub entryStub()
{
    static Stub stub = [] {
        X64 a;
        a.push(RBP);
        a.push(RBX);
        a.push(R12);
        a.push(R13);
        a.push(R14);
        a.push(R15);
        a.aluI(5, RSP, 8);
        a.mov(R12, RDI);
        a.mov(R13, RSI);
        a.mov(RBX, mem(RSI, std::int32_t(offsetof(Frame, R))));
        a.movImm(R14, kNumberTag);
        a.movImm(R15, kNotCellMask);
        a.movImm(RBP, kDoubleOffset);
        a.jmp(RDX);
        CodeSpan span = allocateCode(a.buf.size());
        if (!span)
            return Stub(nullptr);
        std::memcpy(span.rw, a.buf.data(), a.buf.size());
        return reinterpret_cast<Stub>(span.rx);
    }();
    return stub;
}

/// Where compiled code goes on after a handler or a jump the interpreter took: the top frame's pc.
extern "C" void* scriptVmJitTarget(VM* vm)
{
    Frame* fr = vm->frameBase + vm->frameCount - 1;
    return fr->code->tier1->addressFor(std::uint32_t(fr->pc - fr->code->code.data()));
}

/// A C++ exception cannot cross compiled frames (they carry no unwind information): it is parked here, the compiled
/// code returns to `enterTier1` as it would for any result, and that throws it again.
thread_local std::exception_ptr pendingException;
constexpr std::uint64_t kStepThrow = 0x32;

extern "C" std::uint64_t scriptVmJitStep(VM* vm, std::uint32_t index)
{
    try {
        return stepInstruction(*vm, index);
    } catch (...) {
        pendingException = std::current_exception();
        return kStepThrow;
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
constexpr std::int32_t kFrameArgc = std::int32_t(offsetof(Frame, argc));
constexpr std::int32_t kFrameConstruct = std::int32_t(offsetof(Frame, construct));
constexpr std::int32_t kVmTicks = std::int32_t(offsetof(VM, ticks));
constexpr std::int32_t kVmFrameCount = std::int32_t(offsetof(VM, frameCount));
constexpr std::int32_t kArrayElems = std::int32_t(offsetof(JSArray, elems));
constexpr std::int32_t kArrayLength = std::int32_t(offsetof(JSArray, length));
#pragma GCC diagnostic pop
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

    Tier1Code* compile()
    {
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
        if (!span)
            return nullptr;
        std::memcpy(span.rw, a_.buf.data(), a_.buf.size());
        auto* t = new Tier1Code;
        t->rx = span.rx;
        t->bytes = span.bytes;
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
    Label step_, tail_, dispatch_, exit_, free_;
    struct Slow
    {
        std::uint32_t index;
        Label label;
    };
    std::deque<Slow> slows_;

    std::uint32_t length(std::uint32_t i) const noexcept
    {
        return (i + 1 < n_ && code_->code[i + 1].opcode() == Op::Cache) ? 2 : 1;
    }
    static Mem R(unsigned r) { return mem(RBX, std::int32_t(r) * 8); }
    /// A label that runs instruction `i` in the interpreter and then goes on after it.
    Label& slow(std::uint32_t i)
    {
        slows_.push_back(Slow{i, Label{}});
        return slows_.back().label;
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
        callStep(i);
        a_.cmpI(RAX, std::int32_t(kStepFall));
        a_.jcc(CNE, tail_);
    }
    /// For a taken backward jump: count down to the next look at the interrupt flag, in the interpreter when it is time.
    void tick(std::uint32_t i)
    {
        a_.mov32(RDX, mem(R12, kVmTicks));
        a_.aluI32(5, RDX, 1);
        a_.jcc(CE, slow(i));
        a_.mov32(mem(R12, kVmTicks), RDX);
    }
    /// Go to instruction `target` from the jump at `i`.
    void go(std::uint32_t i, std::uint32_t target)
    {
        if (target <= i)
            tick(i);
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
    void getProp(std::uint32_t i, const Ins& in);
    void putProp(std::uint32_t i, const Ins& in);
    void getElem(std::uint32_t i, const Ins& in);
    void putElem(std::uint32_t i, const Ins& in);
    void ret(std::uint32_t i, const Ins& in);
};

void Baseline::emitTails()
{
    // One stub per template that can give up: run the instruction in the interpreter, then go on after it.
    for (std::size_t k = 0; k < slows_.size(); ++k) {
        Slow& s = slows_[k];
        a_.bind(s.label);
        callStep(s.index);
        a_.cmpI(RAX, std::int32_t(kStepFall));
        a_.jcc(CNE, tail_);
        a_.jmp(at_[s.index + length(s.index)]);
    }
    a_.bind(step_);
    a_.movImm(RAX, reinterpret_cast<std::uint64_t>(&scriptVmJitStep));
    a_.jmp(RAX);
    a_.bind(free_);
    a_.movImm(RAX, reinterpret_cast<std::uint64_t>(&scriptVmJitFree));
    a_.jmp(RAX);
    a_.bind(tail_);
    a_.cmpI(RAX, std::int32_t(kStepJump));
    a_.jcc(CE, dispatch_);
    a_.bind(exit_);
    a_.aluI(0, RSP, 8);
    a_.pop(R15);
    a_.pop(R14);
    a_.pop(R13);
    a_.pop(R12);
    a_.pop(RBX);
    a_.pop(RBP);
    a_.ret();
    a_.bind(dispatch_);
    a_.mov(RDI, R12);
    a_.callAbs(reinterpret_cast<const void*>(&scriptVmJitTarget));
    a_.jmp(RAX);
}

void Baseline::emit(std::uint32_t i)
{
    const Ins& in = code_->code[i];
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
    case Op::Add: case Op::Sub: case Op::Mul: case Op::BitAnd: case Op::BitOr: case Op::BitXor:
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
        retain(RAX);
        write(in.a, RAX);
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
        retain(RAX);
        write(in.a, RAX);
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
    default:
        emitStep(i);
        return;
    }
}

void Baseline::arith(std::uint32_t i, const Ins& in)
{
    const Op op = in.opcode();
    Label& bad = slow(i);
    Label dbl, done;
    const bool numeric = op == Op::Add || op == Op::Sub || op == Op::Mul;
    a_.mov(RAX, R(in.b));
    a_.mov(RCX, R(in.c));
    if (op != Op::Div)
        needInts(RAX, RCX, numeric ? dbl : bad);
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
        emitStep(i);
        return;
    }
    Label& bad = slow(i);
    a_.mov(RAX, R(in.b));
    needInt(RAX, bad);
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

void Baseline::compare(std::uint32_t i, const Ins& in)
{
    const Relation rel = relationOf(in.opcode());
    Label& bad = slow(i);
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
        emitStep(i);
        return;
    }
    Label& bad = slow(i);
    Label done, lengthPath;
    a_.mov(RAX, R(in.b));
    needObject(RAX, bad);
    if (known) {
        a_.cmpI32(mem(RAX, 0), std::int32_t(ic->shape));
        a_.jcc(CNE, isLength ? lengthPath : bad);
        if (ic->holder) {
            a_.movImm(RCX, reinterpret_cast<std::uint64_t>(ic->validity));
            a_.cmpI32(mem(RCX, kValidityValid), 0);
            a_.jcc(CE, bad);
            a_.movImm(RAX, reinterpret_cast<std::uint64_t>(ic->holder));
            slotAddress(ic->index);
        } else {
            slotAddress(ic->index);
        }
        a_.mov(RAX, mem(RDX, 0));
        retain(RAX);
        write(in.a, RAX);
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
        a_.or_(RAX, R14);
        write(in.a, RAX);
    }
    a_.bind(done);
}

void Baseline::putProp(std::uint32_t i, const Ins& in)
{
    const std::uint16_t cacheId = code_->code[i + 1].a;
    const InlineCache* ic = cacheId != kNoCache ? &code_->caches[cacheId] : nullptr;
    if (!ic || ic->shape == 0 || ic->holder) {
        emitStep(i);
        return;
    }
    Label& bad = slow(i);
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
    Label& bad = slow(i);
    a_.mov(RAX, R(in.b));
    a_.mov(RCX, R(in.c));
    needInt(RCX, bad);
    a_.test32(RCX, RCX);
    a_.jcc(CS, bad);
    needArray(RAX, bad);
    a_.mov32(RCX, RCX);
    a_.alu32(7, RCX, mem(RAX, kArrayLength));
    a_.jcc(CAE, bad);
    a_.mov(RDX, mem(RAX, kArrayElems));
    a_.mov(RAX, mem(RDX, RCX, 8));
    a_.cmpI(RAX, std::int32_t(kHole));
    a_.jcc(CE, bad);
    retain(RAX);
    write(in.a, RAX);
}

void Baseline::putElem(std::uint32_t i, const Ins& in)
{
    Label& bad = slow(i);
    Label isNumber;
    a_.mov(RAX, R(in.a));
    a_.mov(RCX, R(in.b));
    needInt(RCX, bad);
    a_.test32(RCX, RCX);
    a_.jcc(CS, bad);
    needArray(RAX, bad);
    a_.testI8(mem(RAX, kHdrFlags), std::uint8_t(cellflags::kFrozen | cellflags::kNotExtensible));
    a_.jcc(CNE, bad);
    a_.mov32(RCX, RCX);
    a_.alu32(7, RCX, mem(RAX, kArrayLength));
    a_.jcc(CAE, bad);  // an append goes the long way
    a_.mov(RDX, mem(RAX, kArrayElems));
    a_.lea(RDX, mem(RDX, RCX, 8));
    a_.mov(RSI, R(in.c));
    retain(RSI);
    a_.test(RSI, R14);
    a_.jcc(CNE, isNumber);
    a_.andI8(mem(RAX, kHdrFlags), std::uint8_t(~cellflags::kAllNumbers));
    a_.bind(isNumber);
    a_.mov(RDI, mem(RDX, 0));
    a_.mov(mem(RDX, 0), RSI);
    release(RDI);
}

void Baseline::ret(std::uint32_t i, const Ins& in)
{
    Label& bad = slow(i);
    if (code_->constructor || (code_->flags & CodeBlock::kClassConstructor)) {
        a_.cmpI8(mem(R13, kFrameConstruct), 0);
        a_.jcc(CNE, bad);
    }
    a_.mov(RAX, R(in.a));
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

}  // namespace

bool tier1Available() noexcept
{
    return entryStub() != nullptr;
}

void compileTier1(VM& vm, CodeBlock* code)
{
    code->tier1Failed = true;
    if (code->flags & (CodeBlock::kGenerator | CodeBlock::kAsync | CodeBlock::kAsyncGen))
        return;
    if (code->code.size() > 20000 || !entryStub())
        return;
    Baseline compiler(vm, code);
    Tier1Code* t = compiler.compile();
    if (!t)
        return;
    code->tier1 = t;
    code->tier1Failed = false;
    SCRIPT_VM_CENSUS(Tier1Compiled);
}

void releaseTier1(Tier1Code* code) noexcept
{
    if (!code)
        return;
    CodeSpan span;
    span.rx = code->rx;
    span.bytes = code->bytes;
    releaseCode(span);
    delete code;
}

JSValue enterTier1(VM& vm, Tier1Code* code, std::uint32_t index)
{
    Frame* fr = vm.frameBase + vm.frameCount - 1;
    const std::uint64_t bits = entryStub()(&vm, fr, code->addressFor(index));
    if (bits == kStepThrow) [[unlikely]] {
        std::exception_ptr e = std::move(pendingException);
        pendingException = nullptr;
        std::rethrow_exception(e);
    }
    return JSValue::raw(bits);
}

#else

bool tier1Available() noexcept { return false; }
void compileTier1(VM&, CodeBlock* code) { code->tier1Failed = true; }
void releaseTier1(Tier1Code*) noexcept {}
JSValue enterTier1(VM&, Tier1Code*, std::uint32_t) { return JSValue::exception(); }

#endif

CodeBlock::~CodeBlock()
{
    if (tier1)
        releaseTier1(tier1);
}

}  // namespace vm
}  // namespace internal
}  // namespace script
