// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The x86-64 code generator (System V).
//
// Same shape as the arm64 back end, which is where the design is explained:
// the interpreter's frame is the calling convention, a slot's payload is
// written without touching its tag, the evaluation stack lives in the native
// frame at compile-time-constant offsets, and the supported subset is
// primitives only.
//
// The target-specific parts:
//
//   * System V passes the five arguments in rdi, rsi, rdx, ecx and r8. The
//     Microsoft convention is different enough that this back end reports
//     itself unavailable on Windows rather than generating something plausible
//     and wrong.
//   * rbx, r12, r13 and r14 are callee-saved and hold the frame, the context,
//     the evaluation stack and the result pointer.
//   * `idiv` faults on a zero divisor and on INT_MIN / -1, so both are tested
//     before it, the same way the interpreter checks them.
#include <cstdio>
#include <cstring>

#include "script/internal/typed/bytecode.hpp"
#include "script/internal/typed/runtime.hpp"
#include "script/internal/typed/jit.hpp"

namespace script {
namespace internal {
namespace typed {

namespace {

#if defined(SCRIPT_ARCH_X64) && !defined(_WIN32)

enum Reg : std::uint8_t
{
    RAX = 0, RCX, RDX, RBX, RSP, RBP, RSI, RDI,
    R8, R9, R10, R11, R12, R13, R14, R15,
};

enum Xmm : std::uint8_t
{
    XMM0 = 0, XMM1, XMM2, XMM3,
};

/// Condition codes, as the low nibble of the `setcc`/`jcc` opcodes.
enum Cc : std::uint8_t
{
    O = 0x0, NO = 0x1, B = 0x2, AE = 0x3, E = 0x4, NE = 0x5, BE = 0x6, A = 0x7,
    S = 0x8, NS = 0x9, P = 0xA, NP = 0xB, L = 0xC, GE = 0xD, LE = 0xE, G = 0xF,
};

constexpr Reg kFrame = RBX;
constexpr Reg kRt = R12;
constexpr Reg kStack = R13;
constexpr Reg kResult = R14;

class Assembler
{
public:
    void byte(std::uint8_t b) { code_.push_back(b); }
    void bytes(std::initializer_list<std::uint8_t> list)
    {
        for (std::uint8_t b : list)
            byte(b);
    }
    void dword(std::uint32_t v)
    {
        byte(std::uint8_t(v));
        byte(std::uint8_t(v >> 8));
        byte(std::uint8_t(v >> 16));
        byte(std::uint8_t(v >> 24));
    }
    void qword(std::uint64_t v)
    {
        dword(std::uint32_t(v));
        dword(std::uint32_t(v >> 32));
    }
    std::size_t here() const noexcept { return code_.size(); }
    const std::vector<std::uint8_t>& code() const noexcept { return code_; }

    void rex(bool w, std::uint8_t reg, std::uint8_t rm)
    {
        const std::uint8_t value =
            0x40u | (w ? 8u : 0u) | ((reg >= 8) ? 4u : 0u) | ((rm >= 8) ? 1u : 0u);
        if (value != 0x40u || w)
            byte(value);
    }
    /// ModRM with a 32-bit displacement, which keeps every instruction a fixed
    /// size and the branch patching simple.
    void memOperand(std::uint8_t reg, Reg base, std::uint32_t displacement)
    {
        byte(std::uint8_t(0x80u | ((reg & 7u) << 3) | (base & 7u)));
        if ((base & 7u) == RSP)
            byte(0x24);  // SIB: base = rsp, no index
        dword(displacement);
    }
    void regOperand(std::uint8_t reg, std::uint8_t rm)
    {
        byte(std::uint8_t(0xC0u | ((reg & 7u) << 3) | (rm & 7u)));
    }

    // ---- moves ----
    void movRegImm64(Reg d, std::uint64_t value)
    {
        rex(true, 0, d);
        byte(std::uint8_t(0xB8u + (d & 7u)));
        qword(value);
    }
    void movRegReg(Reg d, Reg s)
    {
        rex(true, s, d);
        byte(0x89);
        regOperand(s, d);
    }
    void movRegMem(Reg d, Reg base, std::uint32_t off)
    {
        rex(true, d, base);
        byte(0x8B);
        memOperand(d, base, off);
    }
    void movMemReg(Reg base, std::uint32_t off, Reg s)
    {
        rex(true, s, base);
        byte(0x89);
        memOperand(s, base, off);
    }
    void movRegMem32(Reg d, Reg base, std::uint32_t off)
    {
        rex(false, d, base);
        byte(0x8B);
        memOperand(d, base, off);
    }
    void movMemReg8(Reg base, std::uint32_t off, Reg s)
    {
        rex(false, s, base);
        byte(0x88);
        memOperand(s, base, off);
    }

    // ---- integer arithmetic ----
    void aluRegReg(std::uint8_t opcode, Reg d, Reg s, bool wide = true)
    {
        rex(wide, s, d);
        byte(opcode);
        regOperand(s, d);
    }
    void addRegReg(Reg d, Reg s, bool wide = true) { aluRegReg(0x01, d, s, wide); }
    void subRegReg(Reg d, Reg s, bool wide = true) { aluRegReg(0x29, d, s, wide); }
    void andRegReg(Reg d, Reg s, bool wide = true) { aluRegReg(0x21, d, s, wide); }
    void orRegReg(Reg d, Reg s, bool wide = true) { aluRegReg(0x09, d, s, wide); }
    void xorRegReg(Reg d, Reg s, bool wide = true) { aluRegReg(0x31, d, s, wide); }
    void cmpRegReg(Reg a, Reg b, bool wide = true) { aluRegReg(0x39, a, b, wide); }
    void imulRegReg(Reg d, Reg s, bool wide = true)
    {
        rex(wide, d, s);
        bytes({0x0F, 0xAF});
        regOperand(d, s);
    }
    void negReg(Reg d, bool wide = true)
    {
        rex(wide, 0, d);
        byte(0xF7);
        regOperand(3, d);
    }
    void notReg(Reg d, bool wide = true)
    {
        rex(wide, 0, d);
        byte(0xF7);
        regOperand(2, d);
    }
    void idivReg(Reg d, bool wide = true)
    {
        rex(wide, 0, d);
        byte(0xF7);
        regOperand(7, d);
    }
    void divReg(Reg d, bool wide = true)
    {
        rex(wide, 0, d);
        byte(0xF7);
        regOperand(6, d);
    }
    void cqo() { bytes({0x48, 0x99}); }
    void cdq() { byte(0x99); }
    void shiftByCl(std::uint8_t kind, Reg d, bool wide = true)
    {
        rex(wide, 0, d);
        byte(0xD3);
        regOperand(kind, d);  // 4 = shl, 5 = shr, 7 = sar
    }
    void cmpRegImm0(Reg d, bool wide = true)
    {
        rex(wide, 0, d);
        byte(0x83);
        regOperand(7, d);
        byte(0);
    }
    void setcc(Cc cc, Reg d)
    {
        rex(false, 0, d);
        bytes({0x0F, std::uint8_t(0x90u + cc)});
        regOperand(0, d);
    }
    void movzxByte(Reg d, Reg s)
    {
        rex(true, d, s);
        bytes({0x0F, 0xB6});
        regOperand(d, s);
    }
    void movsxByte(Reg d, Reg s)
    {
        rex(true, d, s);
        bytes({0x0F, 0xBE});
        regOperand(d, s);
    }
    void movsxWord(Reg d, Reg s)
    {
        rex(true, d, s);
        bytes({0x0F, 0xBF});
        regOperand(d, s);
    }
    void movzxWord(Reg d, Reg s)
    {
        rex(true, d, s);
        bytes({0x0F, 0xB7});
        regOperand(d, s);
    }
    void movsxd(Reg d, Reg s)
    {
        rex(true, d, s);
        byte(0x63);
        regOperand(d, s);
    }
    /// A 32-bit move zero-extends, which is the cheapest `uxtw`.
    void movzxDword(Reg d, Reg s)
    {
        rex(false, d, s);
        byte(0x8B);
        regOperand(d, s);
    }

    // ---- SSE2 ----
    void sseRegMem(std::uint8_t prefix, std::uint8_t opcode, Xmm d, Reg base, std::uint32_t off)
    {
        byte(prefix);
        rex(false, d, base);
        bytes({0x0F, opcode});
        memOperand(d, base, off);
    }
    void movsdLoad(Xmm d, Reg base, std::uint32_t off) { sseRegMem(0xF2, 0x10, d, base, off); }
    void movsdStore(Xmm s, Reg base, std::uint32_t off) { sseRegMem(0xF2, 0x11, s, base, off); }
    void movssLoad(Xmm d, Reg base, std::uint32_t off) { sseRegMem(0xF3, 0x10, d, base, off); }
    void movssStore(Xmm s, Reg base, std::uint32_t off) { sseRegMem(0xF3, 0x11, s, base, off); }
    void sseRegReg(std::uint8_t prefix, std::uint8_t opcode, Xmm d, Xmm s)
    {
        byte(prefix);
        rex(false, d, s);
        bytes({0x0F, opcode});
        regOperand(d, s);
    }
    void addsd(Xmm d, Xmm s) { sseRegReg(0xF2, 0x58, d, s); }
    void subsd(Xmm d, Xmm s) { sseRegReg(0xF2, 0x5C, d, s); }
    void mulsd(Xmm d, Xmm s) { sseRegReg(0xF2, 0x59, d, s); }
    void divsd(Xmm d, Xmm s) { sseRegReg(0xF2, 0x5E, d, s); }
    void addss(Xmm d, Xmm s) { sseRegReg(0xF3, 0x58, d, s); }
    void subss(Xmm d, Xmm s) { sseRegReg(0xF3, 0x5C, d, s); }
    void mulss(Xmm d, Xmm s) { sseRegReg(0xF3, 0x59, d, s); }
    void divss(Xmm d, Xmm s) { sseRegReg(0xF3, 0x5E, d, s); }
    void cvtsd2ss(Xmm d, Xmm s) { sseRegReg(0xF2, 0x5A, d, s); }
    void cvtss2sd(Xmm d, Xmm s) { sseRegReg(0xF3, 0x5A, d, s); }
    void ucomisd(Xmm a, Xmm b)
    {
        byte(0x66);
        rex(false, a, b);
        bytes({0x0F, 0x2E});
        regOperand(a, b);
    }
    void ucomiss(Xmm a, Xmm b)
    {
        rex(false, a, b);
        bytes({0x0F, 0x2E});
        regOperand(a, b);
    }
    void cvtsi2sd(Xmm d, Reg s, bool wide)
    {
        byte(0xF2);
        rex(wide, d, s);
        bytes({0x0F, 0x2A});
        regOperand(d, s);
    }
    void cvtsi2ss(Xmm d, Reg s, bool wide)
    {
        byte(0xF3);
        rex(wide, d, s);
        bytes({0x0F, 0x2A});
        regOperand(d, s);
    }
    void cvttsd2si(Reg d, Xmm s, bool wide)
    {
        byte(0xF2);
        rex(wide, d, s);
        bytes({0x0F, 0x2C});
        regOperand(d, s);
    }
    void cvttss2si(Reg d, Xmm s, bool wide)
    {
        byte(0xF3);
        rex(wide, d, s);
        bytes({0x0F, 0x2C});
        regOperand(d, s);
    }
    void movqToXmm(Xmm d, Reg s)
    {
        byte(0x66);
        rex(true, d, s);
        bytes({0x0F, 0x6E});
        regOperand(d, s);
    }
    void xorpd(Xmm d, Xmm s)
    {
        byte(0x66);
        rex(false, d, s);
        bytes({0x0F, 0x57});
        regOperand(d, s);
    }

    // ---- control ----
    std::size_t jmp()
    {
        byte(0xE9);
        const std::size_t at = here();
        dword(0);
        return at;
    }
    std::size_t jcc(Cc cc)
    {
        bytes({0x0F, std::uint8_t(0x80u + cc)});
        const std::size_t at = here();
        dword(0);
        return at;
    }
    void callReg(Reg r)
    {
        rex(false, 0, r);
        byte(0xFF);
        regOperand(2, r);
    }
    void ret() { byte(0xC3); }
    void push(Reg r)
    {
        if (r >= 8)
            byte(0x41);
        byte(std::uint8_t(0x50u + (r & 7u)));
    }
    void pop(Reg r)
    {
        if (r >= 8)
            byte(0x41);
        byte(std::uint8_t(0x58u + (r & 7u)));
    }
    void addRspImm(std::int32_t amount)
    {
        bytes({0x48, 0x81, 0xC4});
        dword(std::uint32_t(amount));
    }
    void subRspImm(std::int32_t amount)
    {
        bytes({0x48, 0x81, 0xEC});
        dword(std::uint32_t(amount));
    }

    /// A displacement is relative to the end of the instruction, and every
    /// branch here is the fixed-size rel32 form.
    void patch(std::size_t at, std::size_t target)
    {
        const std::int32_t delta = std::int32_t(target) - std::int32_t(at + 4);
        std::memcpy(&code_[at], &delta, sizeof delta);
    }

    void prologue(std::uint32_t stackBytes)
    {
        push(RBP);
        movRegReg(RBP, RSP);
        push(RBX);
        push(R12);
        push(R13);
        push(R14);
        movRegReg(kRt, RDI);
        movRegReg(kFrame, RDX);
        movRegReg(kResult, R8);
        // Five pushes plus the return address leave rsp 16-byte aligned once
        // the (16-aligned) stack area is subtracted.
        subRspImm(std::int32_t(stackBytes) + 8);
        movRegReg(kStack, RSP);
    }
    void epilogue(std::uint32_t stackBytes)
    {
        addRspImm(std::int32_t(stackBytes) + 8);
        pop(R14);
        pop(R13);
        pop(R12);
        pop(RBX);
        pop(RBP);
        ret();
    }

private:
    std::vector<std::uint8_t> code_;
};

class X64Compiler
{
public:
    X64Compiler(const MethodInfo& method, const BodyPlan& plan, std::uint32_t errorOffset)
        : m_(method), plan_(plan), errorOffset_(errorOffset)
    {}

    bool run()
    {
        const std::uint32_t stackBytes = ((plan_.maxDepth * 16u) + 15u) & ~15u;
        a_.prologue(stackBytes);

        const std::vector<std::uint8_t>& code = m_.body.code;
        offsets_.assign(code.size() + 1, 0);
        std::size_t pc = 0;
        while (pc < code.size()) {
            offsets_[pc] = a_.here();
            const Op op = Op(code[pc]);
            const std::int32_t depth = plan_.depth[pc];
            if (depth >= 0 &&
                !emit(op, &code[pc] + 1, std::uint32_t(depth), std::uint32_t(pc), plan_.stack[pc]))
                return false;
            pc += 1 + std::size_t(opArity(op)) * 4;
        }
        offsets_[code.size()] = a_.here();

        epilogueAt_ = a_.here();
        a_.epilogue(stackBytes);

        for (const Fixup& fixup : fixups_)
            a_.patch(fixup.at, offsets_[fixup.target]);
        for (std::size_t at : exits_)
            a_.patch(at, epilogueAt_);
        return true;
    }

    const std::vector<std::uint8_t>& code() const noexcept { return a_.code(); }

private:
    struct Fixup
    {
        std::size_t at;
        std::uint32_t target;
    };

    static std::uint32_t slotOffset(std::uint32_t index) noexcept { return index * 16 + 8; }
    static std::uint32_t stackOffset(std::uint32_t depth) noexcept { return depth * 16 + 8; }

    void loadStack(Reg r, std::uint32_t depth) { a_.movRegMem(r, kStack, stackOffset(depth)); }
    void storeStack(Reg r, std::uint32_t depth) { a_.movMemReg(kStack, stackOffset(depth), r); }
    void loadStackD(Xmm v, std::uint32_t depth) { a_.movsdLoad(v, kStack, stackOffset(depth)); }
    void storeStackD(Xmm v, std::uint32_t depth) { a_.movsdStore(v, kStack, stackOffset(depth)); }
    void loadStackS(Xmm v, std::uint32_t depth) { a_.movssLoad(v, kStack, stackOffset(depth)); }
    void storeStackS(Xmm v, std::uint32_t depth)
    {
        a_.xorRegReg(RAX, RAX);
        a_.movMemReg(kStack, stackOffset(depth), RAX);
        a_.movssStore(v, kStack, stackOffset(depth));
    }

    void raise(ErrorCode code)
    {
        a_.movRegImm64(RAX, std::uint64_t(std::uint32_t(code)));
        a_.rex(false, RAX, kRt);
        a_.byte(0x89);
        a_.memOperand(RAX, kRt, errorOffset_);
        exits_.push_back(a_.jmp());
    }

    void emitPoll()
    {
        a_.movRegReg(RDI, kRt);
        a_.movRegImm64(R11, std::uint64_t(reinterpret_cast<std::uintptr_t>(&scriptJitPoll)));
        a_.callReg(R11);
        a_.cmpRegImm0(RAX, false);
        const std::size_t keepGoing = a_.jcc(E);
        exits_.push_back(a_.jmp());
        a_.patch(keepGoing, a_.here());
    }

    bool emit(Op op, const std::uint8_t* operands, std::uint32_t depth, std::uint32_t pc,
              const std::vector<Type>& types)
    {
        const Type top = types.empty() ? Type::Int : types.back();
        switch (op) {
            case Op::Nop: return true;

            case Op::LdcI4:
                a_.movRegImm64(RAX, std::uint64_t(std::int64_t(std::int32_t(readU32(operands)))));
                storeStack(RAX, depth);
                return true;
            case Op::LdcI8:
            case Op::LdcR8: {
                const std::uint64_t bits =
                    std::uint64_t(readU32(operands)) | (std::uint64_t(readU32(operands + 4)) << 32);
                a_.movRegImm64(RAX, bits);
                storeStack(RAX, depth);
                return true;
            }
            case Op::LdcR4:
                a_.movRegImm64(RAX, std::uint64_t(readU32(operands)));
                storeStack(RAX, depth);
                return true;
            case Op::LdTrue:
            case Op::LdFalse:
                a_.movRegImm64(RAX, op == Op::LdTrue ? 1u : 0u);
                storeStack(RAX, depth);
                return true;
            case Op::Pop: return true;
            case Op::Dup:
                loadStack(RAX, depth - 1);
                storeStack(RAX, depth);
                return true;
            case Op::LdLoc:
                a_.movRegMem(RAX, kFrame, slotOffset(readU32(operands)));
                storeStack(RAX, depth);
                return true;
            case Op::StLoc:
                loadStack(RAX, depth - 1);
                a_.movMemReg(kFrame, slotOffset(readU32(operands)), RAX);
                return true;

            case Op::Br:
                if (readU32(operands) <= pc)
                    emitPoll();
                fixups_.push_back({a_.jmp(), readU32(operands)});
                return true;
            case Op::BrTrue:
            case Op::BrFalse:
                if (readU32(operands) <= pc)
                    emitPoll();
                loadStack(RAX, depth - 1);
                a_.cmpRegImm0(RAX);
                fixups_.push_back({a_.jcc(op == Op::BrTrue ? NE : E), readU32(operands)});
                return true;
            case Op::Ret:
                storeResult(depth - 1);
                exits_.push_back(a_.jmp());
                return true;
            case Op::RetVoid:
                exits_.push_back(a_.jmp());
                return true;

            case Op::ConvI1:
            case Op::ConvI2:
            case Op::ConvU1:
            case Op::ConvU2:
            case Op::ConvChar:
            case Op::ConvI4:
            case Op::ConvU4:
            case Op::ConvI8:
            case Op::ConvU8:
            case Op::ConvBool:
            case Op::ConvR4:
            case Op::ConvR8:
            case Op::ConvR8Un: return convert(op, depth, top);

            case Op::AddI4:
            case Op::SubI4:
            case Op::MulI4:
            case Op::AndI4:
            case Op::OrI4:
            case Op::XorI4:
            case Op::ShlI4:
            case Op::ShrI4:
            case Op::ShrU4: {
                loadStack(RAX, depth - 2);
                loadStack(RCX, depth - 1);
                switch (op) {
                    case Op::AddI4: a_.addRegReg(RAX, RCX, false); break;
                    case Op::SubI4: a_.subRegReg(RAX, RCX, false); break;
                    case Op::MulI4: a_.imulRegReg(RAX, RCX, false); break;
                    case Op::AndI4: a_.andRegReg(RAX, RCX, false); break;
                    case Op::OrI4: a_.orRegReg(RAX, RCX, false); break;
                    case Op::XorI4: a_.xorRegReg(RAX, RCX, false); break;
                    case Op::ShlI4: a_.shiftByCl(4, RAX, false); break;
                    case Op::ShrI4: a_.shiftByCl(7, RAX, false); break;
                    default: a_.shiftByCl(5, RAX, false); break;
                }
                a_.movsxd(RAX, RAX);
                storeStack(RAX, depth - 2);
                return true;
            }
            case Op::NegI4:
                loadStack(RAX, depth - 1);
                a_.negReg(RAX, false);
                a_.movsxd(RAX, RAX);
                storeStack(RAX, depth - 1);
                return true;
            case Op::NotI4:
                loadStack(RAX, depth - 1);
                a_.notReg(RAX, false);
                a_.movsxd(RAX, RAX);
                storeStack(RAX, depth - 1);
                return true;
            case Op::DivI4:
            case Op::DivU4:
            case Op::RemI4:
            case Op::RemU4:
                return divide(op, depth, false);

            case Op::AddI8:
            case Op::SubI8:
            case Op::MulI8:
            case Op::AndI8:
            case Op::OrI8:
            case Op::XorI8:
            case Op::ShlI8:
            case Op::ShrI8:
            case Op::ShrU8: {
                loadStack(RAX, depth - 2);
                loadStack(RCX, depth - 1);
                switch (op) {
                    case Op::AddI8: a_.addRegReg(RAX, RCX); break;
                    case Op::SubI8: a_.subRegReg(RAX, RCX); break;
                    case Op::MulI8: a_.imulRegReg(RAX, RCX); break;
                    case Op::AndI8: a_.andRegReg(RAX, RCX); break;
                    case Op::OrI8: a_.orRegReg(RAX, RCX); break;
                    case Op::XorI8: a_.xorRegReg(RAX, RCX); break;
                    case Op::ShlI8: a_.shiftByCl(4, RAX); break;
                    case Op::ShrI8: a_.shiftByCl(7, RAX); break;
                    default: a_.shiftByCl(5, RAX); break;
                }
                storeStack(RAX, depth - 2);
                return true;
            }
            case Op::NegI8:
                loadStack(RAX, depth - 1);
                a_.negReg(RAX);
                storeStack(RAX, depth - 1);
                return true;
            case Op::NotI8:
                loadStack(RAX, depth - 1);
                a_.notReg(RAX);
                storeStack(RAX, depth - 1);
                return true;
            case Op::DivI8:
            case Op::DivU8:
            case Op::RemI8:
            case Op::RemU8:
                return divide(op, depth, true);

            case Op::AddR8:
            case Op::SubR8:
            case Op::MulR8:
            case Op::DivR8:
                loadStackD(XMM0, depth - 2);
                loadStackD(XMM1, depth - 1);
                switch (op) {
                    case Op::AddR8: a_.addsd(XMM0, XMM1); break;
                    case Op::SubR8: a_.subsd(XMM0, XMM1); break;
                    case Op::MulR8: a_.mulsd(XMM0, XMM1); break;
                    default: a_.divsd(XMM0, XMM1); break;
                }
                storeStackD(XMM0, depth - 2);
                return true;
            case Op::NegR8:
                loadStackD(XMM0, depth - 1);
                a_.movRegImm64(RAX, 0x8000000000000000ull);
                a_.movqToXmm(XMM1, RAX);
                a_.xorpd(XMM0, XMM1);
                storeStackD(XMM0, depth - 1);
                return true;
            case Op::AddR4:
            case Op::SubR4:
            case Op::MulR4:
            case Op::DivR4:
                loadStackS(XMM0, depth - 2);
                loadStackS(XMM1, depth - 1);
                switch (op) {
                    case Op::AddR4: a_.addss(XMM0, XMM1); break;
                    case Op::SubR4: a_.subss(XMM0, XMM1); break;
                    case Op::MulR4: a_.mulss(XMM0, XMM1); break;
                    default: a_.divss(XMM0, XMM1); break;
                }
                storeStackS(XMM0, depth - 2);
                return true;
            case Op::NegR4:
                loadStackS(XMM0, depth - 1);
                a_.movRegImm64(RAX, 0x80000000ull);
                a_.movqToXmm(XMM1, RAX);
                a_.xorpd(XMM0, XMM1);
                storeStackS(XMM0, depth - 1);
                return true;

            case Op::LogicalNot:
                loadStack(RAX, depth - 1);
                a_.cmpRegImm0(RAX);
                a_.setcc(E, RAX);
                a_.movzxByte(RAX, RAX);
                storeStack(RAX, depth - 1);
                return true;
            default: break;
        }
        return emitCompare(op, depth, types);
    }

    bool divide(Op op, std::uint32_t depth, bool wide)
    {
        const bool signedOp = op == Op::DivI4 || op == Op::RemI4 || op == Op::DivI8 ||
                              op == Op::RemI8;
        const bool remainder = op == Op::RemI4 || op == Op::RemU4 || op == Op::RemI8 ||
                               op == Op::RemU8;
        loadStack(RAX, depth - 2);
        loadStack(RCX, depth - 1);

        a_.cmpRegImm0(RCX, wide);
        const std::size_t divisorOk = a_.jcc(NE);
        raise(ErrorCode::DivideByZero);
        a_.patch(divisorOk, a_.here());

        std::size_t skipOverflow = 0;
        if (signedOp) {
            // INT_MIN / -1 faults, and the language says it wraps.
            a_.movRegImm64(RDX, wide ? std::uint64_t(-1ll) : std::uint64_t(0xFFFFFFFFull));
            a_.cmpRegReg(RCX, RDX, wide);
            const std::size_t notMinusOne = a_.jcc(NE);
            if (remainder) {
                a_.xorRegReg(RAX, RAX);
            } else {
                a_.negReg(RAX, wide);
                if (!wide)
                    a_.movsxd(RAX, RAX);
            }
            storeStack(RAX, depth - 2);
            skipOverflow = a_.jmp();
            a_.patch(notMinusOne, a_.here());
        }

        if (signedOp) {
            if (wide)
                a_.cqo();
            else
                a_.cdq();
            a_.idivReg(RCX, wide);
        } else {
            a_.xorRegReg(RDX, RDX);
            a_.divReg(RCX, wide);
        }
        if (remainder)
            a_.movRegReg(RAX, RDX);
        if (!wide)
            a_.movsxd(RAX, RAX);
        storeStack(RAX, depth - 2);
        if (signedOp)
            a_.patch(skipOverflow, a_.here());
        return true;
    }

    bool convert(Op op, std::uint32_t depth, Type from)
    {
        const std::uint32_t at = depth - 1;
        switch (op) {
            case Op::ConvI1:
            case Op::ConvI2:
            case Op::ConvU1:
            case Op::ConvU2:
            case Op::ConvChar:
            case Op::ConvI4:
            case Op::ConvU4: {
                if (from == Type::Double) {
                    loadStackD(XMM0, at);
                    a_.cvttsd2si(RAX, XMM0, true);
                } else if (from == Type::Float) {
                    loadStackS(XMM0, at);
                    a_.cvttss2si(RAX, XMM0, true);
                } else {
                    loadStack(RAX, at);
                }
                switch (op) {
                    case Op::ConvI1: a_.movsxByte(RAX, RAX); break;
                    case Op::ConvI2: a_.movsxWord(RAX, RAX); break;
                    case Op::ConvU1: a_.movzxByte(RAX, RAX); break;
                    case Op::ConvU2:
                    case Op::ConvChar: a_.movzxWord(RAX, RAX); break;
                    case Op::ConvU4: a_.movzxDword(RAX, RAX); break;
                    default: a_.movsxd(RAX, RAX); break;
                }
                storeStack(RAX, at);
                return true;
            }
            case Op::ConvI8:
            case Op::ConvU8: {
                if (from == Type::Double) {
                    loadStackD(XMM0, at);
                    a_.cvttsd2si(RAX, XMM0, true);
                } else if (from == Type::Float) {
                    loadStackS(XMM0, at);
                    a_.cvttss2si(RAX, XMM0, true);
                } else if (from == Type::Long) {
                    return true;
                } else {
                    loadStack(RAX, at);
                    if (op == Op::ConvU8)
                        a_.movzxDword(RAX, RAX);
                    else
                        a_.movsxd(RAX, RAX);
                }
                storeStack(RAX, at);
                return true;
            }
            case Op::ConvBool:
                loadStack(RAX, at);
                a_.cmpRegImm0(RAX);
                a_.setcc(NE, RAX);
                a_.movzxByte(RAX, RAX);
                storeStack(RAX, at);
                return true;
            case Op::ConvR8:
            case Op::ConvR8Un: {
                if (from == Type::Double)
                    return true;
                if (from == Type::Float) {
                    loadStackS(XMM0, at);
                    a_.cvtss2sd(XMM0, XMM0);
                } else if (from == Type::Long) {
                    loadStack(RAX, at);
                    a_.cvtsi2sd(XMM0, RAX, true);
                } else {
                    loadStack(RAX, at);
                    a_.movsxd(RAX, RAX);
                    a_.cvtsi2sd(XMM0, RAX, true);
                }
                storeStackD(XMM0, at);
                return true;
            }
            case Op::ConvR4: {
                if (from == Type::Float)
                    return true;
                if (from == Type::Double) {
                    loadStackD(XMM0, at);
                    a_.cvtsd2ss(XMM0, XMM0);
                } else {
                    loadStack(RAX, at);
                    if (from != Type::Long)
                        a_.movsxd(RAX, RAX);
                    a_.cvtsi2ss(XMM0, RAX, true);
                }
                storeStackS(XMM0, at);
                return true;
            }
            default: return false;
        }
    }

    bool emitCompare(Op op, std::uint32_t depth, const std::vector<Type>& types)
    {
        Cc cc = E;
        bool wide = true;
        bool real = false;
        switch (op) {
            case Op::CeqI4: cc = E; wide = false; break;
            case Op::CneI4: cc = NE; wide = false; break;
            case Op::CltI4: cc = L; wide = false; break;
            case Op::CleI4: cc = LE; wide = false; break;
            case Op::CgtI4: cc = G; wide = false; break;
            case Op::CgeI4: cc = GE; wide = false; break;
            case Op::CltU4: cc = B; wide = false; break;
            case Op::CleU4: cc = BE; wide = false; break;
            case Op::CgtU4: cc = A; wide = false; break;
            case Op::CgeU4: cc = AE; wide = false; break;
            case Op::CeqI8: cc = E; break;
            case Op::CneI8: cc = NE; break;
            case Op::CltI8: cc = L; break;
            case Op::CleI8: cc = LE; break;
            case Op::CgtI8: cc = G; break;
            case Op::CgeI8: cc = GE; break;
            case Op::CltU8: cc = B; break;
            case Op::CleU8: cc = BE; break;
            case Op::CgtU8: cc = A; break;
            case Op::CgeU8: cc = AE; break;
            // `ucomis*` sets the unsigned flags, and an unordered result sets
            // all of them -- which is why every float comparison here uses one
            // of the unsigned conditions that answers false for NaN.
            case Op::CeqR8: cc = E; real = true; break;
            case Op::CneR8: cc = NE; real = true; break;
            case Op::CltR8: cc = B; real = true; break;
            case Op::CleR8: cc = BE; real = true; break;
            case Op::CgtR8: cc = A; real = true; break;
            case Op::CgeR8: cc = AE; real = true; break;
            default: return false;
        }

        if (real) {
            const Type operand = types.empty() ? Type::Double : types.back();
            if (operand == Type::Float) {
                loadStackS(XMM0, depth - 2);
                loadStackS(XMM1, depth - 1);
                a_.ucomiss(XMM0, XMM1);
            } else {
                loadStackD(XMM0, depth - 2);
                loadStackD(XMM1, depth - 1);
                a_.ucomisd(XMM0, XMM1);
            }
            // Unordered would otherwise report equal for `==`.
            if (op == Op::CeqR8) {
                a_.setcc(E, RAX);
                a_.setcc(NP, RCX);
                a_.andRegReg(RAX, RCX, false);
            } else if (op == Op::CneR8) {
                a_.setcc(NE, RAX);
                a_.setcc(P, RCX);
                a_.orRegReg(RAX, RCX, false);
            } else {
                a_.setcc(cc, RAX);
            }
        } else {
            loadStack(RAX, depth - 2);
            loadStack(RCX, depth - 1);
            a_.cmpRegReg(RAX, RCX, wide);
            a_.setcc(cc, RAX);
        }
        a_.movzxByte(RAX, RAX);
        storeStack(RAX, depth - 2);
        return true;
    }

    void storeResult(std::uint32_t depth)
    {
        loadStack(RAX, depth);
        a_.movMemReg(kResult, 8, RAX);
        a_.movRegImm64(RCX, std::uint64_t(std::uint32_t(runtimeTag(m_.returnType->kind))));
        a_.movMemReg8(kResult, 0, RCX);
    }

    const MethodInfo& m_;
    const BodyPlan& plan_;
    std::uint32_t errorOffset_;
    Assembler a_;
    std::vector<std::size_t> offsets_;
    std::vector<Fixup> fixups_;
    std::vector<std::size_t> exits_;
    std::size_t epilogueAt_ = 0;
};

class X64Backend : public JitBackend
{
public:
    X64Backend() : layout_(valueLayout()) {}

    const char* name() const noexcept override { return "x86-64"; }

    bool available() const noexcept override
    {
#if SCRIPT_ENABLE_JIT
        return layout_.isExpected();
#else
        return false;
#endif
    }

    bool supports(const MethodInfo& method, std::string* reason) const override
    {
        if (!available()) {
            if (reason)
                *reason = "the x86-64 back end is not enabled in this build";
            return false;
        }
        BodyPlan plan;
        return planBody(method, plan, reason);
    }

    NativeCode* compile(Runtime& rt, MethodInfo& method, std::vector<NativeCode*>&) override
    {
        BodyPlan plan;
        if (!planBody(method, plan, nullptr))
            return nullptr;

        const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(&rt);
        const std::uintptr_t field = reinterpret_cast<std::uintptr_t>(&rt.nativeError);
        const std::uint32_t errorOffset = std::uint32_t(field - base);

        X64Compiler compiler(method, plan, errorOffset);
        if (!compiler.run())
            return nullptr;

        const std::size_t bytes = compiler.code().size();
        void* block = cache_.reserve(bytes);
        if (!block)
            return nullptr;
        std::memcpy(block, compiler.code().data(), bytes);
        if (!cache_.commit(block, bytes)) {
            cache_.release(block, bytes);
            return nullptr;
        }

        auto* code = new NativeCode();
        code->entry = reinterpret_cast<NativeCode::Entry>(block);
        code->method = &method;
        code->codeStart = block;
        code->codeBytes = bytes;
        code->backend = "x86-64";
        return code;
    }

    void release(NativeCode* code) noexcept override
    {
        if (!code)
            return;
        cache_.release(code->codeStart, code->codeBytes);
        delete code;
    }

private:
    ValueLayout layout_;
};

#else  // the host is not System V x86-64

class X64Backend : public JitBackend
{
public:
    const char* name() const noexcept override { return "x86-64"; }
    bool available() const noexcept override { return false; }
    bool supports(const MethodInfo&, std::string* reason) const override
    {
        if (reason)
            *reason = "this build does not target System V x86-64";
        return false;
    }
    NativeCode* compile(Runtime&, MethodInfo&, std::vector<NativeCode*>&) override { return nullptr; }
    void release(NativeCode* code) noexcept override { delete code; }
};

#endif

}  // namespace

std::unique_ptr<JitBackend> createX64Backend()
{
    return std::make_unique<X64Backend>();
}

}  // namespace typed
}  // namespace internal
}  // namespace script
