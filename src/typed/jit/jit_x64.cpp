// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The x86-64 code generator: IR in, machine code out, for the System V convention.
//
// The native calling convention is the C one for arguments: integers and references in rdi, rsi, rdx, rcx, r8, r9, floats
// in xmm0-xmm7, the rest in stack words, with `this` first. On top of that:
//
//   * a result is in rax or xmm0, and the tag of an `object` in rdx;
//   * r15 holds the runtime for a whole run of native code;
//   * r10 holds the delegate at a call through one, r11 the method at a call through a method's entry;
//   * rax, rcx, rdx, r10 and r11 are never allocated: the instruction set wants the first three for division and shifts, so
//     this file uses them as scratch, and xmm14 and xmm15 likewise;
//   * rbp chains the frames, so that a stack trace can walk them.
//
// A method that raises records the error in the runtime and returns; its caller checks after the call. Nothing unwinds.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

#include "script/internal/typed/bytecode.hpp"
#include "script/internal/typed/jit.hpp"
#include "script/internal/typed/jit_ir.hpp"
#include "script/internal/typed/jit_runtime.hpp"
#include "script/internal/typed/runtime.hpp"
#include "script/internal/typed/vm.hpp"

namespace script {
namespace internal {
namespace typed {

namespace {

#if defined(SCRIPT_ARCH_X64) && !defined(_WIN32)

enum Reg : std::uint32_t
{
    RAX = 0, RCX, RDX, RBX, RSP, RBP, RSI, RDI,
    R8, R9, R10, R11, R12, R13, R14, R15,
};
using Xmm = std::uint32_t;

constexpr Reg kRt = R15;
constexpr Reg kDelegateReg = R10;
constexpr Reg kMethodReg = R11;
constexpr Xmm kFpScratchA = 14, kFpScratchB = 15;

constexpr Reg kIntArgs[] = {RDI, RSI, RDX, RCX, R8, R9};
constexpr std::uint32_t kIntArgCount = 6;

/// Condition codes as the instruction set numbers them.
enum Cc : std::uint32_t
{
    O = 0, NO = 1, B = 2, AE = 3, E = 4, NE = 5, BE = 6, A = 7, S = 8, NS = 9, P = 10, NP = 11, L = 12, GE = 13, LE = 14, G = 15,
};

class Assembler
{
public:
    void byte(std::uint32_t b) { code_.push_back(std::uint8_t(b)); }
    void dword(std::uint32_t v)
    {
        for (int i = 0; i < 4; ++i)
            byte((v >> (i * 8)) & 0xFF);
    }
    void qword(std::uint64_t v)
    {
        for (int i = 0; i < 8; ++i)
            byte(std::uint32_t((v >> (i * 8)) & 0xFF));
    }
    std::size_t here() const noexcept { return code_.size(); }
    std::vector<std::uint8_t>& code() noexcept { return code_; }
    const std::vector<std::uint8_t>& code() const noexcept { return code_; }

    /// REX, with W, R, X and B from the register numbers.
    void rex(bool wide, std::uint32_t reg, std::uint32_t index, std::uint32_t base, bool force = false)
    {
        const std::uint32_t value = 0x40 | (wide ? 8u : 0u) | ((reg >> 3) << 2) | ((index >> 3) << 1) | (base >> 3);
        if (value != 0x40 || force)
            byte(value);
    }
    void modReg(std::uint32_t reg, std::uint32_t rm) { byte(0xC0 | ((reg & 7) << 3) | (rm & 7)); }
    /// [base + disp]
    void modMem(std::uint32_t reg, std::uint32_t base, std::int32_t disp)
    {
        const std::uint32_t r = (reg & 7) << 3;
        const std::uint32_t b = base & 7;
        const bool sib = b == 4;
        if (disp == 0 && b != 5) {
            byte(r | (sib ? 4 : b));
            if (sib)
                byte(0x24);
        } else if (disp >= -128 && disp < 128) {
            byte(0x40 | r | (sib ? 4 : b));
            if (sib)
                byte(0x24);
            byte(std::uint32_t(disp) & 0xFF);
        } else {
            byte(0x80 | r | (sib ? 4 : b));
            if (sib)
                byte(0x24);
            dword(std::uint32_t(disp));
        }
    }
    /// [base + index << scale]
    void modIndexed(std::uint32_t reg, std::uint32_t base, std::uint32_t index, std::uint32_t scale)
    {
        const std::uint32_t r = (reg & 7) << 3;
        const bool needsDisp = (base & 7) == 5;
        byte((needsDisp ? 0x40 : 0x00) | r | 4);
        byte((scale << 6) | ((index & 7) << 3) | (base & 7));
        if (needsDisp)
            byte(0);
    }

    // ---- moves ----
    void movRR(Reg d, Reg s, bool wide = true)
    {
        rex(wide, s, 0, d);
        byte(0x89);
        modReg(s, d);
    }
    void movImm(Reg d, std::uint64_t v)
    {
        if (v == 0) {
            rex(false, d, 0, d);
            byte(0x31);
            modReg(d, d);
        } else if (v <= 0xFFFFFFFFu) {
            rex(false, 0, 0, d);
            byte(0xB8 | (d & 7));
            dword(std::uint32_t(v));
        } else if (std::int64_t(v) >= -2147483648ll && std::int64_t(v) < 0) {
            rex(true, 0, 0, d);
            byte(0xC7);
            modReg(0, d);
            dword(std::uint32_t(v));
        } else {
            rex(true, 0, 0, d);
            byte(0xB8 | (d & 7));
            qword(v);
        }
    }
    /// Keeps the flags, unlike the `xor` a zero makes in `movImm`.
    void movImmKeepFlags(Reg d, std::uint32_t v)
    {
        rex(false, 0, 0, d);
        byte(0xB8 | (d & 7));
        dword(v);
    }
    void load(Reg d, Reg base, std::int32_t disp, std::uint32_t bytes, bool isSigned)
    {
        switch (bytes) {
            case 1:
                rex(false, d, 0, base);
                byte(0x0F);
                byte(isSigned ? 0xBE : 0xB6);
                break;
            case 2:
                rex(false, d, 0, base);
                byte(0x0F);
                byte(isSigned ? 0xBF : 0xB7);
                break;
            case 4:
                rex(false, d, 0, base);
                byte(0x8B);
                break;
            default:
                rex(true, d, 0, base);
                byte(0x8B);
                break;
        }
        modMem(d, base, disp);
    }
    void store(Reg s, Reg base, std::int32_t disp, std::uint32_t bytes)
    {
        switch (bytes) {
            case 1:
                rex(false, s, 0, base, s >= 4);
                byte(0x88);
                break;
            case 2:
                byte(0x66);
                rex(false, s, 0, base);
                byte(0x89);
                break;
            case 4:
                rex(false, s, 0, base);
                byte(0x89);
                break;
            default:
                rex(true, s, 0, base);
                byte(0x89);
                break;
        }
        modMem(s, base, disp);
    }
    void loadIdx(Reg d, Reg base, Reg index, std::uint32_t scale, std::uint32_t bytes, bool isSigned)
    {
        switch (bytes) {
            case 1:
                rex(false, d, index, base);
                byte(0x0F);
                byte(isSigned ? 0xBE : 0xB6);
                break;
            case 2:
                rex(false, d, index, base);
                byte(0x0F);
                byte(isSigned ? 0xBF : 0xB7);
                break;
            case 4:
                rex(false, d, index, base);
                byte(0x8B);
                break;
            default:
                rex(true, d, index, base);
                byte(0x8B);
                break;
        }
        modIndexed(d, base, index, scale);
    }
    void storeIdx(Reg s, Reg base, Reg index, std::uint32_t scale, std::uint32_t bytes)
    {
        switch (bytes) {
            case 1:
                rex(false, s, index, base, s >= 4);
                byte(0x88);
                break;
            case 2:
                byte(0x66);
                rex(false, s, index, base);
                byte(0x89);
                break;
            case 4:
                rex(false, s, index, base);
                byte(0x89);
                break;
            default:
                rex(true, s, index, base);
                byte(0x89);
                break;
        }
        modIndexed(s, base, index, scale);
    }

    // ---- integer ----
    /// `op d, s`, for the "op r/m, r" forms: add 01, or 09, and 21, sub 29, xor 31, cmp 39, test 85.
    void alu(std::uint32_t opcode, Reg d, Reg s, bool wide)
    {
        rex(wide, s, 0, d);
        byte(opcode);
        modReg(s, d);
    }
    /// `op d, imm32`: /0 add, /1 or, /4 and, /5 sub, /6 xor, /7 cmp.
    void aluImm(std::uint32_t sub, Reg d, std::int32_t imm, bool wide)
    {
        rex(wide, 0, 0, d);
        if (imm >= -128 && imm < 128) {
            byte(0x83);
            modReg(sub, d);
            byte(std::uint32_t(imm) & 0xFF);
        } else {
            byte(0x81);
            modReg(sub, d);
            dword(std::uint32_t(imm));
        }
    }
    void imul(Reg d, Reg s, bool wide)
    {
        rex(wide, d, 0, s);
        byte(0x0F);
        byte(0xAF);
        modReg(d, s);
    }
    /// /3 neg, /2 not, /7 idiv, /6 div
    void unary(std::uint32_t sub, Reg d, bool wide)
    {
        rex(wide, 0, 0, d);
        byte(0xF7);
        modReg(sub, d);
    }
    void signExtendRax(bool wide)
    {
        if (wide)
            byte(0x48);
        byte(0x99);  // cqo, or cdq
    }
    /// /4 shl, /5 shr, /7 sar, by cl.
    void shiftCl(std::uint32_t sub, Reg d, bool wide)
    {
        rex(wide, 0, 0, d);
        byte(0xD3);
        modReg(sub, d);
    }
    void shiftImm(std::uint32_t sub, Reg d, std::uint32_t count, bool wide)
    {
        rex(wide, 0, 0, d);
        byte(0xC1);
        modReg(sub, d);
        byte(count);
    }
    void movsxByte(Reg d, Reg s)
    {
        rex(true, d, 0, s);
        byte(0x0F);
        byte(0xBE);
        modReg(d, s);
    }
    void movsxWord(Reg d, Reg s)
    {
        rex(true, d, 0, s);
        byte(0x0F);
        byte(0xBF);
        modReg(d, s);
    }
    void movsxd(Reg d, Reg s)
    {
        rex(true, d, 0, s);
        byte(0x63);
        modReg(d, s);
    }
    void movzxByte(Reg d, Reg s)
    {
        rex(false, d, 0, s, s >= 4);
        byte(0x0F);
        byte(0xB6);
        modReg(d, s);
    }
    void movzxWord(Reg d, Reg s)
    {
        rex(false, d, 0, s);
        byte(0x0F);
        byte(0xB7);
        modReg(d, s);
    }
    void setcc(Cc cc, Reg d)
    {
        rex(false, 0, 0, d, d >= 4);
        byte(0x0F);
        byte(0x90 | cc);
        modReg(0, d);
    }
    void cmov(Cc cc, Reg d, Reg s, bool wide)
    {
        rex(wide, d, 0, s);
        byte(0x0F);
        byte(0x40 | cc);
        modReg(d, s);
    }
    /// add/sub dword [base + disp], 1: the sub sets ZF when a count reaches zero.
    void memAdjust(std::uint32_t sub, Reg base, std::int32_t disp)
    {
        rex(false, 0, 0, base);
        byte(0x83);
        modMem(sub, base, disp);
        byte(1);
    }
    void lea(Reg d, Reg base, std::int32_t disp)
    {
        rex(true, d, 0, base);
        byte(0x8D);
        modMem(d, base, disp);
    }
    void cmpMem32Zero(Reg base, std::int32_t disp)
    {
        rex(false, 0, 0, base);
        byte(0x83);
        modMem(7, base, disp);
        byte(0);
    }
    void cmpMem64Zero(Reg base, std::int32_t disp)
    {
        rex(true, 0, 0, base);
        byte(0x83);
        modMem(7, base, disp);
        byte(0);
    }
    /// cmp rsp, [base + disp]
    void cmpRspMem(Reg base, std::int32_t disp)
    {
        rex(true, RSP, 0, base);
        byte(0x3B);
        modMem(RSP, base, disp);
    }

    // ---- floating point ----
    /// `prefix 0F op`, xmm from xmm.
    void sse(std::uint32_t prefix, std::uint32_t op, Xmm d, Xmm s)
    {
        if (prefix)
            byte(prefix);
        rex(false, d, 0, s);
        byte(0x0F);
        byte(op);
        modReg(d, s);
    }
    void movsdRR(Xmm d, Xmm s) { sse(0xF2, 0x10, d, s); }
    void loadSd(Xmm d, Reg base, std::int32_t disp, std::uint32_t bytes)
    {
        byte(bytes == 4 ? 0xF3 : 0xF2);
        rex(false, d, 0, base);
        byte(0x0F);
        byte(0x10);
        modMem(d, base, disp);
    }
    void storeSd(Xmm s, Reg base, std::int32_t disp, std::uint32_t bytes)
    {
        byte(bytes == 4 ? 0xF3 : 0xF2);
        rex(false, s, 0, base);
        byte(0x0F);
        byte(0x11);
        modMem(s, base, disp);
    }
    void loadSdIdx(Xmm d, Reg base, Reg index, std::uint32_t scale, std::uint32_t bytes)
    {
        byte(bytes == 4 ? 0xF3 : 0xF2);
        rex(false, d, index, base);
        byte(0x0F);
        byte(0x10);
        modIndexed(d, base, index, scale);
    }
    void storeSdIdx(Xmm s, Reg base, Reg index, std::uint32_t scale, std::uint32_t bytes)
    {
        byte(bytes == 4 ? 0xF3 : 0xF2);
        rex(false, s, index, base);
        byte(0x0F);
        byte(0x11);
        modIndexed(s, base, index, scale);
    }
    /// movq xmm, r64
    void movqXR(Xmm d, Reg s)
    {
        byte(0x66);
        rex(true, d, 0, s);
        byte(0x0F);
        byte(0x6E);
        modReg(d, s);
    }
    /// movq r64, xmm
    void movqRX(Reg d, Xmm s)
    {
        byte(0x66);
        rex(true, s, 0, d);
        byte(0x0F);
        byte(0x7E);
        modReg(s, d);
    }
    void movdXR(Xmm d, Reg s)
    {
        byte(0x66);
        rex(false, d, 0, s);
        byte(0x0F);
        byte(0x6E);
        modReg(d, s);
    }
    void movdRX(Reg d, Xmm s)
    {
        byte(0x66);
        rex(false, s, 0, d);
        byte(0x0F);
        byte(0x7E);
        modReg(s, d);
    }
    /// cvtsi2sd / cvtsi2ss from a general register.
    void intToFp(Xmm d, Reg s, bool isDouble, bool wideSource)
    {
        byte(isDouble ? 0xF2 : 0xF3);
        rex(wideSource, d, 0, s);
        byte(0x0F);
        byte(0x2A);
        modReg(d, s);
    }
    /// cvttsd2si / cvttss2si to a general register.
    void fpToInt(Reg d, Xmm s, bool isDouble)
    {
        byte(isDouble ? 0xF2 : 0xF3);
        rex(true, d, 0, s);
        byte(0x0F);
        byte(0x2C);
        modReg(d, s);
    }
    /// ucomisd or ucomiss
    void ucomis(Xmm a, Xmm b, bool isDouble)
    {
        if (isDouble)
            byte(0x66);
        rex(false, a, 0, b);
        byte(0x0F);
        byte(0x2E);
        modReg(a, b);
    }
    /// roundsd d, s, mode (SSE4.1)
    void roundsd(Xmm d, Xmm s, std::uint32_t mode)
    {
        byte(0x66);
        rex(false, d, 0, s);
        byte(0x0F);
        byte(0x3A);
        byte(0x0B);
        modReg(d, s);
        byte(mode);
    }

    // ---- control ----
    std::size_t jmp()
    {
        byte(0xE9);
        dword(0);
        return here() - 4;
    }
    std::size_t jcc(Cc cc)
    {
        byte(0x0F);
        byte(0x80 | cc);
        dword(0);
        return here() - 4;
    }
    std::size_t call()
    {
        byte(0xE8);
        dword(0);
        return here() - 4;
    }
    void callReg(Reg r)
    {
        rex(false, 0, 0, r);
        byte(0xFF);
        modReg(2, r);
    }
    void callMem(Reg base, std::int32_t disp)
    {
        rex(false, 0, 0, base);
        byte(0xFF);
        modMem(2, base, disp);
    }
    void ret() { byte(0xC3); }
    void push(Reg r)
    {
        rex(false, 0, 0, r);
        byte(0x50 | (r & 7));
    }
    void pop(Reg r)
    {
        rex(false, 0, 0, r);
        byte(0x58 | (r & 7));
    }
    void int3() { byte(0xCC); }

    /// Point the rel32 at `at` to `target`, both offsets into the code.
    void patch(std::size_t at, std::size_t target)
    {
        const std::int32_t delta = std::int32_t(std::int64_t(target) - std::int64_t(at + 4));
        for (int i = 0; i < 4; ++i)
            code_[at + std::size_t(i)] = std::uint8_t((std::uint32_t(delta) >> (i * 8)) & 0xFF);
    }

private:
    std::vector<std::uint8_t> code_;
};

Cc ccOf(IrCond c) noexcept
{
    switch (c) {
        case IrCond::Eq: return E;
        case IrCond::Ne: return NE;
        case IrCond::Lt: return L;
        case IrCond::Le: return LE;
        case IrCond::Gt: return G;
        case IrCond::Ge: return GE;
        case IrCond::Lo: return B;
        case IrCond::Ls: return BE;
        case IrCond::Hi: return A;
        case IrCond::Hs: return AE;
        default: return E;
    }
}

struct CallFixup
{
    std::size_t at;
    const MethodInfo* target;
};


class Emitter
{
public:
    Emitter(const IrFunction& fn, const NativeConfig& config, const std::unordered_map<const MethodInfo*, std::size_t>& session)
        : fn_(fn), config_(config), session_(session)
    {}

    bool run()
    {
        layoutFrame();
        if (frameBytes_ > 32000)
            return false;
        labels_.assign(std::size_t(fn_.labelCount) + 1, std::size_t(-1));
        emitPrologue(true);
        emitParameters();
        // The hot code first, then what the IR marked cold, so that the hot code stays together.
        bool cold = false;
        for (const IrInst& in : fn_.code) {
            if (in.op == IrOp::ColdBegin || in.op == IrOp::ColdEnd)
                cold = in.op == IrOp::ColdBegin;
            else if (!cold)
                emit(in);
        }
        a_.int3();
        cold = false;
        for (const IrInst& in : fn_.code) {
            if (in.op == IrOp::ColdBegin || in.op == IrOp::ColdEnd)
                cold = in.op == IrOp::ColdBegin;
            else if (cold)
                emit(in);
        }
        emitCold();
        emitOsrEntries();
        if (saverWanted_) {
            labels_[std::size_t(fn_.labelCount)] = a_.here();
            const std::int32_t base = config_.savedRegs;
            for (std::uint32_t r = 0; r < 16; ++r)
                a_.store(Reg(r), kRt, base + std::int32_t(r * 8), 8);
            for (std::uint32_t r = 0; r < 16; ++r)
                a_.storeSd(Xmm(r), kRt, base + std::int32_t((32 + r) * 8), 8);
            a_.ret();
        }
        for (const LabelFixup& fixup : labelFixups_) {
            if (labels_[std::size_t(fixup.label)] == std::size_t(-1))
                return false;
            a_.patch(fixup.at, labels_[std::size_t(fixup.label)]);
        }
        for (const LabelFixup& fixup : exitFixups_)
            a_.patch(fixup.at, exitPads_[std::size_t(fixup.label)]);
        return true;
    }

    std::vector<std::uint8_t>& code() noexcept { return a_.code(); }
    const std::vector<CallFixup>& callFixups() const noexcept { return callFixups_; }
    const std::vector<NativeCode::Site>& sites() const noexcept { return sites_; }
    struct OsrOut
    {
        std::uint32_t pc;
        std::size_t at;
        std::vector<std::uint32_t> owned;
        std::vector<std::uint32_t> ownedPointers;
    };
    const std::vector<OsrOut>& osr() const noexcept { return osr_; }

private:
    struct LabelFixup
    {
        std::size_t at;
        std::int32_t label;
    };
    struct ColdDecRef
    {
        std::size_t branch;
        Reg cell;
        std::size_t back;
        std::int32_t site;
    };
    struct ColdPoll
    {
        std::size_t branch;
        std::int32_t exit;
        std::size_t back;
    };
    struct ColdFpToInt
    {
        std::size_t branch;
        Reg d;
        Xmm source;
        bool isDouble;
        std::size_t back;
    };

    void layoutFrame()
    {
        std::uint32_t outgoing = fn_.maxCleanup;
        bool hasDecOrPoll = false;
        for (const IrInst& in : fn_.code) {
            if (in.op == IrOp::DecRef || in.op == IrOp::DecRefTagged || in.op == IrOp::Poll)
                hasDecOrPoll = true;
        }
        for (const IrCall& call : fn_.calls)
            outgoing = std::max(outgoing, stackWords(call.passed()));
        areaBase_ = outgoing * 8;
        spillBase_ = areaBase_ + ((fn_.frameAreaBytes + 7u) & ~7u);
        coldSaveBase_ = spillBase_ + fn_.spillSlots * 8;
        std::uint32_t at = coldSaveBase_ + fn_.coldSaveWords * 8;
        saveBase_ = at;
        for (Reg r : {RBX, R12, R13, R14}) {
            if ((fn_.usedIntRegs >> r) & 1u)
                savedInt_.push_back(r);
        }
        at += std::uint32_t(savedInt_.size()) * 8;
        // After `push rbp` the stack is aligned, so what follows keeps it so.
        frameBytes_ = (at + 15u) & ~15u;
        hasFrame_ = fn_.makesCalls || hasDecOrPoll || at > 0 || stackWords(fn_.params) > 0;
        for (const IrExit& exit : fn_.exits)
            hasFrame_ = hasFrame_ || exit.handlerLabel >= 0;
    }

    std::uint32_t stackWords(const std::vector<std::int32_t>& values) const
    {
        std::uint32_t ints = 0, fps = 0, stack = 0;
        for (std::int32_t v : values) {
            if (fn_.vregIsFp[std::size_t(v)]) {
                if (fps < 8)
                    ++fps;
                else
                    ++stack;
            } else {
                if (ints < kIntArgCount)
                    ++ints;
                else
                    ++stack;
            }
        }
        return stack;
    }

    void emitPrologue(bool checkStack)
    {
        if (!hasFrame_)
            return;
        if (checkStack && fn_.makesCalls) {
            a_.cmpRspMem(kRt, config_.stackLimit);
            overflowBranch_ = a_.jcc(BE);
            needsOverflowPad_ = true;
        }
        a_.push(RBP);
        a_.movRR(RBP, RSP);
        if (frameBytes_)
            a_.aluImm(5, RSP, std::int32_t(frameBytes_), true);
        std::int32_t at = std::int32_t(saveBase_);
        for (Reg r : savedInt_) {
            a_.store(r, RSP, at, 8);
            at += 8;
        }
    }

    void emitEpilogue()
    {
        if (!hasFrame_) {
            a_.ret();
            return;
        }
        std::int32_t at = std::int32_t(saveBase_);
        for (Reg r : savedInt_) {
            a_.load(r, RSP, at, 8, false);
            at += 8;
        }
        a_.movRR(RSP, RBP);
        a_.pop(RBP);
        a_.ret();
    }

    // ---- places ----
    struct Place
    {
        bool isMem = false;
        std::uint32_t reg = 0;
        std::int32_t offset = 0;
    };
    std::int32_t spillOffset(std::int32_t location) const noexcept
    {
        return std::int32_t(spillBase_) + (-location - 1) * 8;
    }
    Place placeOf(std::int32_t vreg) const noexcept
    {
        const std::int32_t loc = fn_.location[std::size_t(vreg)];
        Place p;
        if (loc >= 0) {
            p.reg = std::uint32_t(loc);
        } else {
            p.isMem = true;
            p.reg = RSP;
            p.offset = spillOffset(loc);
        }
        return p;
    }
    bool isUnused(std::int32_t vreg) const noexcept { return fn_.location[std::size_t(vreg)] == IrFunction::kUnused; }
    bool isFp(std::int32_t vreg) const noexcept { return fn_.vregIsFp[std::size_t(vreg)] != 0; }

    Reg use(std::int32_t v, Reg scratch)
    {
        const std::int32_t loc = fn_.location[std::size_t(v)];
        if (loc >= 0)
            return Reg(loc);
        a_.load(scratch, RSP, spillOffset(loc), 8, false);
        return scratch;
    }
    Xmm useFp(std::int32_t v, Xmm scratch)
    {
        const std::int32_t loc = fn_.location[std::size_t(v)];
        if (loc >= 0)
            return Xmm(loc);
        a_.loadSd(scratch, RSP, spillOffset(loc), 8);
        return scratch;
    }
    Reg def(std::int32_t v, Reg scratch) const
    {
        const std::int32_t loc = fn_.location[std::size_t(v)];
        return loc >= 0 && loc != IrFunction::kUnused ? Reg(loc) : scratch;
    }
    Xmm defFp(std::int32_t v, Xmm scratch) const
    {
        const std::int32_t loc = fn_.location[std::size_t(v)];
        return loc >= 0 && loc != IrFunction::kUnused ? Xmm(loc) : scratch;
    }
    void commit(std::int32_t v, Reg r)
    {
        const std::int32_t loc = fn_.location[std::size_t(v)];
        if (loc < 0)
            a_.store(r, RSP, spillOffset(loc), 8);
    }
    void commitFp(std::int32_t v, Xmm r)
    {
        const std::int32_t loc = fn_.location[std::size_t(v)];
        if (loc < 0)
            a_.storeSd(r, RSP, spillOffset(loc), 8);
    }

    struct Move
    {
        Place from;
        Place to;
    };

    void parallelMoves(std::vector<Move> moves, bool fp)
    {
        const auto movRegs = [&](std::uint32_t d, std::uint32_t s) {
            if (fp)
                a_.movsdRR(d, s);
            else
                a_.movRR(Reg(d), Reg(s));
        };
        const auto load = [&](std::uint32_t d, const Place& m) {
            if (fp)
                a_.loadSd(d, Reg(m.reg), m.offset, 8);
            else
                a_.load(Reg(d), Reg(m.reg), m.offset, 8, false);
        };
        const auto store = [&](std::uint32_t s, const Place& m) {
            if (fp)
                a_.storeSd(s, Reg(m.reg), m.offset, 8);
            else
                a_.store(Reg(s), Reg(m.reg), m.offset, 8);
        };
        const std::uint32_t scratch = fp ? kFpScratchB : std::uint32_t(RAX);
        for (const Move& m : moves) {
            if (!m.to.isMem)
                continue;
            if (m.from.isMem) {
                load(scratch, m.from);
                store(scratch, m.to);
            } else {
                store(m.from.reg, m.to);
            }
        }
        std::vector<Move> pending;
        for (const Move& m : moves) {
            if (!m.to.isMem && !m.from.isMem && m.from.reg != m.to.reg)
                pending.push_back(m);
        }
        while (!pending.empty()) {
            bool progressed = false;
            for (std::size_t i = 0; i < pending.size();) {
                bool blocked = false;
                for (std::size_t j = 0; j < pending.size(); ++j) {
                    if (j != i && pending[j].from.reg == pending[i].to.reg)
                        blocked = true;
                }
                if (blocked) {
                    ++i;
                    continue;
                }
                movRegs(pending[i].to.reg, pending[i].from.reg);
                pending[i] = pending.back();
                pending.pop_back();
                progressed = true;
            }
            if (progressed)
                continue;
            const std::uint32_t victim = pending[0].from.reg;
            movRegs(scratch, victim);
            for (Move& m : pending) {
                if (m.from.reg == victim)
                    m.from.reg = scratch;
            }
        }
        for (const Move& m : moves) {
            if (!m.to.isMem && m.from.isMem)
                load(m.to.reg, m.from);
        }
    }

    void emitParameters()
    {
        std::vector<Move> ints, fps;
        std::uint32_t nextInt = 0, nextFp = 0, stack = 0;
        for (std::int32_t p : fn_.params) {
            const bool fp = isFp(p);
            Move m;
            if (fp ? nextFp < 8 : nextInt < kIntArgCount) {
                m.from.reg = fp ? nextFp++ : std::uint32_t(kIntArgs[nextInt++]);
            } else {
                m.from.isMem = true;
                m.from.reg = RBP;
                m.from.offset = std::int32_t(16 + 8 * stack++);
            }
            if (isUnused(p))
                continue;
            m.to = placeOf(p);
            (fp ? fps : ints).push_back(m);
        }
        parallelMoves(std::move(ints), false);
        parallelMoves(std::move(fps), true);
        if (!fn_.captures.empty()) {
            a_.load(RAX, kDelegateReg, std::int32_t(kDelegateCapturedOffset), 8, false);
            for (std::size_t i = 0; i < fn_.captures.size(); ++i) {
                const std::int32_t box = fn_.captures[i];
                if (isUnused(box))
                    continue;
                const Reg d = def(box, RCX);
                a_.load(d, RAX, std::int32_t(i * 8), 8, false);
                commit(box, d);
            }
        }
    }

    // ---- exits ----
    void branchToExit(std::size_t at, std::int32_t exit)
    {
        exitFixups_.push_back({at, exit});
        usedExits_.resize(std::max(usedExits_.size(), std::size_t(exit) + 1), 0);
        usedExits_[std::size_t(exit)] = 1;
    }
    void branchToLabel(std::size_t at, std::int32_t label) { labelFixups_.push_back({at, label}); }
    void callAbsolute(const void* target)
    {
        a_.movImm(RAX, reinterpret_cast<std::uint64_t>(target));
        a_.callReg(RAX);
    }
    void noteSite(std::int32_t site) { sites_.push_back({std::uint32_t(a_.here()), site}); }

    void raiseHere(std::uint32_t code, std::int32_t site)
    {
        a_.store(RBP, kRt, config_.topFrame, 8);
        a_.movRR(RDI, kRt);
        a_.movImm(RSI, code);
        callAbsolute(config_.raise);
        noteSite(site);
    }

    void emitReleaseCall(const ColdDecRef& cold)
    {
        const IrFunction::LiveRegs& live = fn_.liveAtRelease[std::size_t(cold.site)];
        std::vector<std::uint32_t> ints, fps;
        for (std::uint32_t r = 0; r < 16; ++r) {
            if ((live.ints >> r) & 1u)
                ints.push_back(r);
        }
        for (std::uint32_t r = 0; r < 14; ++r) {
            if ((live.fps >> r) & 1u)
                fps.push_back(r);
        }
        const std::int32_t bytes = std::int32_t((ints.size() + fps.size() + 1) & ~std::size_t(1)) * 8;
        a_.movRR(RCX, cold.cell);
        if (bytes)
            a_.aluImm(5, RSP, bytes, true);
        std::int32_t at = 0;
        for (std::uint32_t r : ints) {
            a_.store(Reg(r), RSP, at, 8);
            at += 8;
        }
        for (std::uint32_t r : fps) {
            a_.storeSd(r, RSP, at, 8);
            at += 8;
        }
        a_.movRR(RDI, RCX);
        callAbsolute(reinterpret_cast<const void*>(&detail::destroyUnreferenced));
        at = 0;
        for (std::uint32_t r : ints) {
            a_.load(Reg(r), RSP, at, 8, false);
            at += 8;
        }
        for (std::uint32_t r : fps) {
            a_.loadSd(r, RSP, at, 8);
            at += 8;
        }
        if (bytes)
            a_.aluImm(0, RSP, bytes, true);
    }

    void emitCold()
    {
        for (const ColdDecRef& cold : coldDecRefs_) {
            a_.patch(cold.branch, a_.here());
            emitReleaseCall(cold);
            a_.patch(a_.jmp(), cold.back);
        }
        for (const ColdPoll& cold : coldPolls_) {
            a_.patch(cold.branch, a_.here());
            a_.movImm(R11, reinterpret_cast<std::uint64_t>(config_.poll));
            a_.movImm(RAX, reinterpret_cast<std::uint64_t>(&scriptJitPollStub));
            a_.callReg(RAX);
            a_.alu(0x85, RAX, RAX, true);
            branchToExit(a_.jcc(NE), cold.exit);
            a_.patch(a_.jmp(), cold.back);
        }
        for (const ColdFpToInt& cold : coldFpToInts_) {
            // The instruction said "out of range". The interpreter says: NaN and infinities 0, finite values saturate.
            a_.patch(cold.branch, a_.here());
            if (cold.isDouble) {
                a_.movqRX(RAX, cold.source);
            } else {
                a_.movdRX(RAX, cold.source);
                a_.shiftImm(4, RAX, 32, true);
            }
            a_.movRR(RCX, RAX);
            a_.shiftImm(4, RCX, 1, true);
            a_.movImm(RDX, cold.isDouble ? 0xFFE0000000000000ull : 0xFF00000000000000ull);
            a_.alu(0x39, RCX, RDX, true);
            const std::size_t finite = a_.jcc(B);
            a_.movImm(cold.d, 0);
            const std::size_t done = a_.jmp();
            a_.patch(finite, a_.here());
            a_.movImm(RCX, 0x7FFFFFFFFFFFFFFFull);
            a_.movImm(RDX, 0x8000000000000000ull);
            a_.alu(0x85, RAX, RAX, true);
            a_.cmov(S, RCX, RDX, true);
            a_.movRR(cold.d, RCX);
            a_.patch(done, a_.here());
            a_.patch(a_.jmp(), cold.back);
        }
        exitPads_.assign(fn_.exits.size(), 0);
        for (std::size_t i = 0; i < fn_.exits.size(); ++i) {
            if (i < usedExits_.size() && usedExits_[i])
                emitExitPad(i);
        }
        if (needsOverflowPad_) {
            a_.patch(overflowBranch_, a_.here());
            emitFramelessRaise(std::uint32_t(ErrorCode::StackOverflow));
        }
    }

    void emitFramelessRaise(std::uint32_t code)
    {
        a_.push(RBP);
        a_.movRR(RBP, RSP);
        a_.store(RBP, kRt, config_.topFrame, 8);
        a_.movRR(RDI, kRt);
        a_.movImm(RSI, code);
        callAbsolute(config_.raise);
        noteSite(-1);
        a_.pop(RBP);
        a_.ret();
    }

    void emitExitPad(std::size_t index)
    {
        const IrExit& exit = fn_.exits[index];
        exitPads_[index] = a_.here();
        if (!hasFrame_) {
            if (exit.indexVreg >= 0)
                a_.movRR(RDX, use(exit.indexVreg, RDX), false);
            emitFramelessRaise(exit.code);
            return;
        }
        for (const IrExit::WriteBack& back : exit.writeBacks) {
            const Reg base = use(back.base, RAX);
            a_.alu(0x85, base, base, true);
            const std::size_t skip = a_.jcc(E);
            if (isFp(back.vreg)) {
                const Xmm v = useFp(back.vreg, kFpScratchA);
                a_.storeSd(v, base, std::int32_t(back.offset), back.width);
            } else {
                const Reg v = use(back.vreg, RCX);
                a_.store(v, base, std::int32_t(back.offset), back.width);
            }
            a_.patch(skip, a_.here());
        }
        std::int32_t parked = 0;
        for (std::int32_t v : exit.cleanup) {
            if (isUnused(v))
                continue;
            a_.store(use(v, RAX), RSP, parked * 8, 8);
            ++parked;
        }
        for (const auto& tagged : exit.cleanupTagged) {
            if (isUnused(tagged.first) || isUnused(tagged.second))
                continue;
            const Reg t = use(tagged.second, RCX);
            const Reg r = use(tagged.first, RAX);
            if (r != RAX)
                a_.movRR(RAX, r);
            a_.movImm(RDX, 0);
            a_.aluImm(7, t, std::int32_t(Type::String), false);
            a_.cmov(B, RAX, RDX, true);
            a_.store(RAX, RSP, parked * 8, 8);
            ++parked;
        }
        if (exit.indexVreg >= 0 && exit.code)
            a_.movRR(RDX, use(exit.indexVreg, RDX), false);
        if (exit.code)
            raiseHere(exit.code, exit.site);
        if (parked) {
            a_.movRR(RDI, RSP);
            a_.movImm(RSI, std::uint64_t(parked));
            callAbsolute(reinterpret_cast<const void*>(&scriptJitCleanup));
        }
        if (exit.handlerLabel >= 0) {
            branchToLabel(a_.jmp(), exit.handlerLabel);
            return;
        }
        emitEpilogue();
    }

    void emitOsrEntries()
    {
        for (const IrOsrEntry& entry : fn_.osrEntries) {
            OsrOut out;
            out.pc = entry.bytecodePc;
            out.at = a_.here();
            emitPrologue(false);
            {
                // What comes after the two tables, all at once, as a method's parameters do.
                std::vector<Move> args;
                std::uint32_t next = 2;
                for (std::int32_t v : fn_.osrArguments) {
                    if (!isUnused(v)) {
                        Move m;
                        m.from.reg = kIntArgs[next];
                        m.to = placeOf(v);
                        args.push_back(m);
                    }
                    ++next;
                }
                a_.movRR(R10, RDI);
                a_.movRR(R11, RSI);
                parallelMoves(std::move(args), false);
            }
            for (const IrOsrEntry::Slot& slot : entry.pointers) {
                if (isUnused(slot.vreg))
                    continue;
                const Reg d = def(slot.vreg, RCX);
                a_.load(d, R11, std::int32_t(slot.slot) * 8, 8, false);
                commit(slot.vreg, d);
                if (slot.owned)
                    out.ownedPointers.push_back(slot.slot);
            }
            a_.movRR(RAX, R10);
            for (std::int32_t zero : fn_.osrZero) {
                if (isUnused(zero))
                    continue;
                const Reg d = def(zero, RCX);
                a_.movImm(d, 0);
                commit(zero, d);
            }
            for (const IrOsrEntry::Slot& slot : entry.slots) {
                if (isUnused(slot.vreg))
                    continue;
                const std::int32_t offset = std::int32_t(slot.slot) * 16 + 8;
                if (isFp(slot.vreg)) {
                    const Xmm d = defFp(slot.vreg, kFpScratchA);
                    a_.loadSd(d, RAX, offset, slot.bytes);
                    commitFp(slot.vreg, d);
                } else {
                    const Reg d = def(slot.vreg, RCX);
                    a_.load(d, RAX, offset, slot.bytes, false);
                    commit(slot.vreg, d);
                }
                if (slot.tagVreg >= 0 && !isUnused(slot.tagVreg)) {
                    const Reg d = def(slot.tagVreg, RCX);
                    a_.load(d, RAX, std::int32_t(slot.slot) * 16, 1, false);
                    commit(slot.tagVreg, d);
                }
                if (slot.owned)
                    out.owned.push_back(slot.slot);
            }
            branchToLabel(a_.jmp(), entry.label);
            osr_.push_back(std::move(out));
        }
    }

    // ---- comparisons ----
    static bool fitsImm32(std::int64_t v) noexcept { return v >= -2147483648ll && v <= 2147483647ll; }

    void compare(const IrInst& in)
    {
        const bool wide = in.width == 64;
        const Reg a = use(in.a, RAX);
        if (in.hasImm) {
            if (in.imm == 0) {
                a_.alu(0x85, a, a, wide);
            } else if (fitsImm32(in.imm)) {
                a_.aluImm(7, a, std::int32_t(in.imm), wide);
            } else {
                a_.movImm(RCX, std::uint64_t(in.imm));
                a_.alu(0x39, a, RCX, wide);
            }
            return;
        }
        const Reg b = use(in.b, RCX);
        a_.alu(0x39, a, b, wide);
    }

    /// Float comparison, for a condition that a single flag test answers. `FEq` and `FNe` are handled by the caller.
    Cc compareFp(const IrInst& in)
    {
        const Xmm a = useFp(in.a, kFpScratchA);
        const Xmm b = useFp(in.b, kFpScratchB);
        const bool isDouble = in.width == 64;
        switch (in.cond) {
            // Unordered sets CF, so "above" and "above or equal" are false for NaN: the less-thans compare the other way.
            case IrCond::FLt: a_.ucomis(b, a, isDouble); return A;
            case IrCond::FLe: a_.ucomis(b, a, isDouble); return AE;
            case IrCond::FGt: a_.ucomis(a, b, isDouble); return A;
            case IrCond::FGe: a_.ucomis(a, b, isDouble); return AE;
            default: a_.ucomis(a, b, isDouble); return E;
        }
    }

    // ---- instructions ----
    /// d = a; then op on d, keeping in mind that d may be what b is.
    void arithmetic(const IrInst& in)
    {
        const bool wide = in.width == 64;
        const Reg d = def(in.d, RAX);
        // Loaded into `d` unless `d` is where the other operand is.
        const bool dIsB = !in.hasImm && in.b >= 0 && fn_.location[std::size_t(in.b)] == std::int32_t(d);
        const Reg a = use(in.a, dIsB ? RAX : d);
        if (in.hasImm) {
            if (in.op == IrOp::Shl || in.op == IrOp::Shr || in.op == IrOp::Sar) {
                if (d != a)
                    a_.movRR(d, a, wide);
                const std::uint32_t sub = in.op == IrOp::Shl ? 4 : in.op == IrOp::Shr ? 5 : 7;
                a_.shiftImm(sub, d, std::uint32_t(in.imm) & (wide ? 63u : 31u), wide);
                commit(in.d, d);
                return;
            }
            std::int32_t sub = -1;
            switch (in.op) {
                case IrOp::Add: sub = 0; break;
                case IrOp::Or: sub = 1; break;
                case IrOp::And: sub = 4; break;
                case IrOp::Sub: sub = 5; break;
                case IrOp::Xor: sub = 6; break;
                default: break;
            }
            if (sub >= 0 && fitsImm32(in.imm)) {
                if (d != a)
                    a_.movRR(d, a, wide);
                a_.aluImm(std::uint32_t(sub), d, std::int32_t(in.imm), wide);
                commit(in.d, d);
                return;
            }
            a_.movImm(RCX, std::uint64_t(in.imm));
            binary(in.op, d, a, RCX, wide);
            commit(in.d, d);
            return;
        }
        const Reg b = use(in.b, RCX);
        binary(in.op, d, a, b, wide);
        commit(in.d, d);
    }

    void binary(IrOp op, Reg d, Reg a, Reg b, bool wide)
    {
        if (op == IrOp::Shl || op == IrOp::Shr || op == IrOp::Sar) {
            if (b != RCX)
                a_.movRR(RCX, b);
            Reg out = d;
            if (d == RCX)
                out = RAX;
            if (out != a)
                a_.movRR(out, a, wide);
            a_.shiftCl(op == IrOp::Shl ? 4 : op == IrOp::Shr ? 5 : 7, out, wide);
            if (out != d)
                a_.movRR(d, out, wide);
            return;
        }
        std::uint32_t opcode = 0;
        bool commutes = true;
        switch (op) {
            case IrOp::Add: opcode = 0x01; break;
            case IrOp::Or: opcode = 0x09; break;
            case IrOp::And: opcode = 0x21; break;
            case IrOp::Xor: opcode = 0x31; break;
            case IrOp::Sub: opcode = 0x29; commutes = false; break;
            default: break;  // Mul
        }
        if (d == b && d != a) {
            if (commutes) {
                std::swap(a, b);
            } else {
                a_.movRR(RAX, a, wide);
                a_.alu(opcode, RAX, b, wide);
                a_.movRR(d, RAX, wide);
                return;
            }
        }
        if (d != a)
            a_.movRR(d, a, wide);
        if (op == IrOp::Mul)
            a_.imul(d, b, wide);
        else
            a_.alu(opcode, d, b, wide);
    }

    void divideOrRemainder(const IrInst& in)
    {
        const bool wide = in.width == 64;
        const bool isSigned = in.op == IrOp::SDiv || in.op == IrOp::SRem;
        const bool isRem = in.op == IrOp::SRem || in.op == IrOp::URem;
        Reg b = R10;
        if (in.hasImm)
            a_.movImm(R10, std::uint64_t(in.imm));
        else
            b = use(in.b, R10);
        const Reg a = use(in.a, RAX);
        if (a != RAX)
            a_.movRR(RAX, a, wide);
        std::size_t notMinusOne = 0, done = 0;
        if (isSigned) {
            // The smallest integer divided by -1 traps here; the language says it wraps.
            a_.aluImm(7, b, -1, wide);
            notMinusOne = a_.jcc(NE);
            if (isRem)
                a_.movImm(RDX, 0);
            else
                a_.unary(3, RAX, wide);
            done = a_.jmp();
            a_.patch(notMinusOne, a_.here());
            a_.signExtendRax(wide);
            a_.unary(7, b, wide);
        } else {
            a_.movImm(RDX, 0);
            a_.unary(6, b, wide);
        }
        if (isSigned)
            a_.patch(done, a_.here());
        const Reg d = def(in.d, RAX);
        const Reg result = isRem ? RDX : RAX;
        if (d != result)
            a_.movRR(d, result);
        commit(in.d, d);
    }

    void memory(const IrInst& in)
    {
        const std::uint32_t bytes = in.width;
        switch (in.op) {
            case IrOp::Load: {
                const Reg base = use(in.a, RAX);
                if (isFp(in.d)) {
                    const Xmm d = defFp(in.d, kFpScratchA);
                    a_.loadSd(d, base, std::int32_t(in.imm), bytes);
                    commitFp(in.d, d);
                } else {
                    const Reg d = def(in.d, RCX);
                    a_.load(d, base, std::int32_t(in.imm), bytes, in.isSigned);
                    commit(in.d, d);
                }
                return;
            }
            case IrOp::Store: {
                const Reg base = use(in.a, RAX);
                if (isFp(in.c)) {
                    a_.storeSd(useFp(in.c, kFpScratchA), base, std::int32_t(in.imm), bytes);
                } else {
                    a_.store(use(in.c, RCX), base, std::int32_t(in.imm), bytes);
                }
                return;
            }
            case IrOp::LoadIdx: {
                Reg base = use(in.a, RAX);
                const Reg index = use(in.b, RCX);
                if (in.imm) {
                    a_.movRR(RDX, base);
                    a_.aluImm(0, RDX, std::int32_t(in.imm), true);
                    base = RDX;
                }
                if (isFp(in.d)) {
                    const Xmm d = defFp(in.d, kFpScratchA);
                    a_.loadSdIdx(d, base, index, in.scale, bytes);
                    commitFp(in.d, d);
                } else {
                    const Reg d = def(in.d, R10);
                    a_.loadIdx(d, base, index, in.scale, bytes, in.isSigned);
                    commit(in.d, d);
                }
                return;
            }
            case IrOp::StoreIdx: {
                Reg base = use(in.a, RAX);
                const Reg index = use(in.b, RCX);
                if (in.imm) {
                    a_.movRR(RDX, base);
                    a_.aluImm(0, RDX, std::int32_t(in.imm), true);
                    base = RDX;
                }
                if (isFp(in.c))
                    a_.storeSdIdx(useFp(in.c, kFpScratchA), base, index, in.scale, bytes);
                else
                    a_.storeIdx(use(in.c, R10), base, index, in.scale, bytes);
                return;
            }
            default: return;
        }
    }

    void saveLive(const IrFunction::LiveRegs& live, bool restore)
    {
        std::int32_t at = std::int32_t(coldSaveBase_);
        for (std::uint32_t r = 0; r < 16; ++r) {
            if (!((live.ints >> r) & 1u))
                continue;
            if (restore)
                a_.load(Reg(r), RSP, at, 8, false);
            else
                a_.store(Reg(r), RSP, at, 8);
            at += 8;
        }
        for (std::uint32_t r = 0; r < 14; ++r) {
            if (!((live.fps >> r) & 1u))
                continue;
            if (restore)
                a_.loadSd(r, RSP, at, 8);
            else
                a_.storeSd(r, RSP, at, 8);
            at += 8;
        }
    }

    void emitCall(const IrCall& call)
    {
        if (call.publishFrame && (call.kind == IrCallKind::Helper || call.kind == IrCallKind::Indirect))
            a_.store(RBP, kRt, config_.topFrame, 8);
        if (call.cold)
            saveLive(fn_.liveAtRelease[std::size_t(call.liveIndex)], false);

        std::vector<Move> ints, fps;
        std::uint32_t nextInt = 0, nextFp = 0, stack = 0;
        for (std::int32_t arg : call.passed()) {
            const bool fp = isFp(arg);
            Move m;
            m.from = placeOf(arg);
            if (fp ? nextFp < 8 : nextInt < kIntArgCount) {
                m.to.reg = fp ? nextFp++ : std::uint32_t(kIntArgs[nextInt++]);
            } else {
                m.to.isMem = true;
                m.to.reg = RSP;
                m.to.offset = std::int32_t(8 * stack++);
            }
            (fp ? fps : ints).push_back(m);
        }
        if (call.delegate >= 0) {
            Move m;
            m.from = placeOf(call.delegate);
            m.to.reg = call.kind == IrCallKind::Delegate ? std::uint32_t(kDelegateReg) : std::uint32_t(kMethodReg);
            ints.push_back(m);
        }
        parallelMoves(std::move(ints), false);
        parallelMoves(std::move(fps), true);

        switch (call.kind) {
            case IrCallKind::Helper:
                callAbsolute(call.helper);
                break;
            case IrCallKind::Direct:
                if (session_.count(call.target) || call.target->code.load(std::memory_order_acquire)) {
                    callFixups_.push_back({a_.call(), call.target});
                } else {
                    a_.movImm(kMethodReg, reinterpret_cast<std::uint64_t>(call.target));
                    a_.callMem(kMethodReg, std::int32_t(kMethodFastEntryOffset));
                }
                break;
            case IrCallKind::Virtual:
                a_.load(RAX, RDI, std::int32_t(kInstanceTypeOffset), 8, false);
                a_.load(RAX, RAX, std::int32_t(kTypeVtableOffset), 8, false);
                a_.load(kMethodReg, RAX, call.target->vtableSlot * 8, 8, false);
                a_.callMem(kMethodReg, std::int32_t(kMethodFastEntryOffset));
                break;
            case IrCallKind::Interface:
                a_.callMem(kMethodReg, std::int32_t(kMethodFastEntryOffset));
                break;
            case IrCallKind::Indirect:
                a_.callReg(kMethodReg);
                break;
            case IrCallKind::Delegate: {
                a_.cmpMem64Zero(kDelegateReg, std::int32_t(kDelegateNextOffset));
                const std::size_t multicast = a_.jcc(NE);
                a_.load(kMethodReg, kDelegateReg, std::int32_t(kDelegateMethodOffset), 8, false);
                a_.callMem(kMethodReg, std::int32_t(kMethodDelegateEntryOffset));
                const std::size_t go = a_.jmp();
                a_.patch(multicast, a_.here());
                a_.movImm(RAX, reinterpret_cast<std::uint64_t>(&scriptJitLazyDelegateStub));
                a_.callReg(RAX);
                a_.patch(go, a_.here());
                // The return address the frame walk sees is either of the two; both stand for this site.
                if (call.site >= 0)
                    sites_.push_back({std::uint32_t(go + 4), call.site});
                break;
            }
        }
        if (call.site >= 0)
            noteSite(call.site);
        if (call.cold) {
            // rax and rdx are never allocated, so the results are safe there; xmm0 may be.
            a_.movsdRR(kFpScratchA, 0);
            saveLive(fn_.liveAtRelease[std::size_t(call.liveIndex)], true);
            if (call.exit >= 0) {
                a_.cmpMem32Zero(kRt, config_.nativeError);
                branchToExit(a_.jcc(NE), call.exit);
            }
            if (call.resultTag >= 0 && !isUnused(call.resultTag)) {
                const Reg d = def(call.resultTag, RDX);
                if (d != RDX)
                    a_.movRR(d, RDX);
                commit(call.resultTag, d);
            }
            if (call.result >= 0 && !isUnused(call.result)) {
                if (isFp(call.result)) {
                    const Xmm d = defFp(call.result, kFpScratchA);
                    if (d != kFpScratchA)
                        a_.movsdRR(d, kFpScratchA);
                    commitFp(call.result, d);
                } else {
                    const Reg d = def(call.result, RAX);
                    if (d != RAX)
                        a_.movRR(d, RAX);
                    commit(call.result, d);
                }
            }
            return;
        }
        if (call.exit >= 0) {
            a_.cmpMem32Zero(kRt, config_.nativeError);
            branchToExit(a_.jcc(NE), call.exit);
        }
        if (call.resultTag >= 0) {
            std::vector<Move> results;
            if (!isUnused(call.result)) {
                Move m;
                m.from.reg = RAX;
                m.to = placeOf(call.result);
                results.push_back(m);
            }
            if (!isUnused(call.resultTag)) {
                Move m;
                m.from.reg = RDX;
                m.to = placeOf(call.resultTag);
                results.push_back(m);
            }
            parallelMoves(std::move(results), false);
        } else if (call.result >= 0 && !isUnused(call.result)) {
            const Place to = placeOf(call.result);
            if (isFp(call.result)) {
                if (to.isMem)
                    a_.storeSd(0, RSP, to.offset, 8);
                else if (to.reg != 0)
                    a_.movsdRR(to.reg, 0);
            } else if (to.isMem) {
                a_.store(RAX, RSP, to.offset, 8);
            } else {
                a_.movRR(Reg(to.reg), RAX);
            }
        }
    }

    void loadFpConstant(Xmm d, std::uint64_t bits, bool isDouble)
    {
        if (bits == 0) {
            a_.sse(0x66, 0x57, d, d);  // xorpd
            return;
        }
        a_.movImm(RAX, bits);
        if (isDouble)
            a_.movqXR(d, RAX);
        else
            a_.movdXR(d, RAX);
    }

    void emit(const IrInst& in)
    {
        switch (in.op) {
            case IrOp::Label:
                labels_[std::size_t(in.imm)] = a_.here();
                return;
            case IrOp::Mov:
                if (isUnused(in.d))
                    return;
                if (isFp(in.d)) {
                    const Xmm d = defFp(in.d, kFpScratchA);
                    const Xmm a = useFp(in.a, d);
                    if (d != a)
                        a_.movsdRR(d, a);
                    commitFp(in.d, d);
                } else {
                    const Reg d = def(in.d, RAX);
                    const Reg a = use(in.a, d);
                    if (d != a)
                        a_.movRR(d, a);
                    commit(in.d, d);
                }
                return;
            case IrOp::MovImm: {
                if (isUnused(in.d))
                    return;
                const Reg d = def(in.d, RAX);
                a_.movImm(d, std::uint64_t(in.imm));
                commit(in.d, d);
                return;
            }
            case IrOp::MovRuntime: {
                if (isUnused(in.d))
                    return;
                const Reg d = def(in.d, RAX);
                a_.movRR(d, kRt);
                commit(in.d, d);
                return;
            }
            case IrOp::FMovImm: {
                if (isUnused(in.d))
                    return;
                const Xmm d = defFp(in.d, kFpScratchA);
                loadFpConstant(d, in.width == 32 ? std::uint64_t(std::uint32_t(in.imm)) : std::uint64_t(in.imm),
                               in.width == 64);
                commitFp(in.d, d);
                return;
            }
            case IrOp::BitsToFp: {
                const Xmm d = defFp(in.d, kFpScratchA);
                a_.movqXR(d, use(in.a, RAX));
                commitFp(in.d, d);
                return;
            }
            case IrOp::FpToBits: {
                const Reg d = def(in.d, RAX);
                const Xmm a = useFp(in.a, kFpScratchA);
                if (in.width == 32)
                    a_.movdRX(d, a);
                else
                    a_.movqRX(d, a);
                commit(in.d, d);
                return;
            }
            case IrOp::Add:
            case IrOp::Sub:
            case IrOp::Mul:
            case IrOp::And:
            case IrOp::Or:
            case IrOp::Xor:
            case IrOp::Shl:
            case IrOp::Sar:
            case IrOp::Shr: arithmetic(in); return;
            case IrOp::SDiv:
            case IrOp::UDiv:
            case IrOp::SRem:
            case IrOp::URem: divideOrRemainder(in); return;
            case IrOp::Neg:
            case IrOp::Not: {
                const Reg d = def(in.d, RAX);
                const Reg a = use(in.a, d);
                if (d != a)
                    a_.movRR(d, a, in.width == 64);
                a_.unary(in.op == IrOp::Neg ? 3 : 2, d, in.width == 64);
                commit(in.d, d);
                return;
            }
            case IrOp::Sext8:
            case IrOp::Sext16:
            case IrOp::Sext32:
            case IrOp::Zext8:
            case IrOp::Zext16:
            case IrOp::Zext32: {
                const Reg d = def(in.d, RAX);
                const Reg a = use(in.a, RCX);
                switch (in.op) {
                    case IrOp::Sext8: a_.movsxByte(d, a); break;
                    case IrOp::Sext16: a_.movsxWord(d, a); break;
                    case IrOp::Sext32: a_.movsxd(d, a); break;
                    case IrOp::Zext8: a_.movzxByte(d, a); break;
                    case IrOp::Zext16: a_.movzxWord(d, a); break;
                    default: a_.movRR(d, a, false); break;
                }
                commit(in.d, d);
                return;
            }
            case IrOp::FAdd:
            case IrOp::FSub:
            case IrOp::FMul:
            case IrOp::FDiv: {
                const std::uint32_t prefix = in.width == 64 ? 0xF2 : 0xF3;
                std::uint32_t opcode = 0x58;
                bool commutes = true;
                if (in.op == IrOp::FMul) {
                    opcode = 0x59;
                } else if (in.op == IrOp::FSub) {
                    opcode = 0x5C;
                    commutes = false;
                } else if (in.op == IrOp::FDiv) {
                    opcode = 0x5E;
                    commutes = false;
                }
                Xmm a = useFp(in.a, kFpScratchA);
                Xmm b = useFp(in.b, kFpScratchB);
                const Xmm d = defFp(in.d, kFpScratchA);
                if (d == b && d != a) {
                    if (commutes) {
                        std::swap(a, b);
                    } else {
                        a_.movsdRR(kFpScratchB, b);
                        b = kFpScratchB;
                    }
                }
                if (d != a)
                    a_.movsdRR(d, a);
                a_.sse(prefix, opcode, d, b);
                commitFp(in.d, d);
                return;
            }
            case IrOp::FNeg:
            case IrOp::FAbs: {
                const Xmm a = useFp(in.a, kFpScratchA);
                const Xmm d = defFp(in.d, kFpScratchA);
                if (d != a)
                    a_.movsdRR(d, a);
                const bool isDouble = in.width == 64;
                std::uint64_t mask = isDouble ? 0x8000000000000000ull : 0x80000000ull;
                if (in.op == IrOp::FAbs)
                    mask = isDouble ? 0x7FFFFFFFFFFFFFFFull : 0x7FFFFFFFull;
                loadFpConstant(kFpScratchB, mask, isDouble);
                a_.sse(0x66, in.op == IrOp::FNeg ? 0x57 : 0x54, d, kFpScratchB);
                commitFp(in.d, d);
                return;
            }
            case IrOp::FSqrt: {
                const Xmm a = useFp(in.a, kFpScratchA);
                const Xmm d = defFp(in.d, kFpScratchA);
                a_.sse(0xF2, 0x51, d, a);
                commitFp(in.d, d);
                return;
            }
            case IrOp::FFloor:
            case IrOp::FCeil:
            case IrOp::FTrunc:
            case IrOp::FRoundEven: {
                const Xmm a = useFp(in.a, kFpScratchA);
                const Xmm d = defFp(in.d, kFpScratchA);
                a_.roundsd(d, a, in.op == IrOp::FFloor ? 9 : in.op == IrOp::FCeil ? 10 : in.op == IrOp::FTrunc ? 11 : 8);
                commitFp(in.d, d);
                return;
            }
            case IrOp::FConvert: {
                const Xmm a = useFp(in.a, kFpScratchA);
                const Xmm d = defFp(in.d, kFpScratchA);
                a_.sse(in.width == 64 ? 0xF3 : 0xF2, 0x5A, d, a);
                commitFp(in.d, d);
                return;
            }
            case IrOp::IntToFp: {
                const Reg a = use(in.a, RAX);
                const Xmm d = defFp(in.d, kFpScratchA);
                const bool isDouble = in.width == 64;
                if (!in.isUnsigned) {
                    a_.intToFp(d, a, isDouble, in.srcWidth == 64);
                } else if (in.srcWidth == 32) {
                    // A uint fits a signed 64-bit register.
                    a_.movRR(RCX, a, false);
                    a_.intToFp(d, RCX, isDouble, true);
                } else {
                    a_.alu(0x85, a, a, true);
                    const std::size_t negative = a_.jcc(S);
                    a_.intToFp(d, a, isDouble, true);
                    const std::size_t done = a_.jmp();
                    a_.patch(negative, a_.here());
                    // Half, keeping the lowest bit for rounding, then doubled.
                    a_.movRR(RCX, a);
                    a_.movRR(RDX, a);
                    a_.shiftImm(5, RCX, 1, true);
                    a_.aluImm(4, RDX, 1, true);
                    a_.alu(0x09, RCX, RDX, true);
                    a_.intToFp(d, RCX, isDouble, true);
                    a_.sse(isDouble ? 0xF2 : 0xF3, 0x58, d, d);
                    a_.patch(done, a_.here());
                }
                commitFp(in.d, d);
                return;
            }
            case IrOp::FpToInt: {
                const Xmm a = useFp(in.a, kFpScratchA);
                const Reg d = def(in.d, R10);
                const bool isDouble = in.srcWidth == 64;
                a_.fpToInt(d, a, isDouble);
                a_.movImm(RAX, 0x8000000000000000ull);
                a_.alu(0x39, d, RAX, true);
                const std::size_t cold = a_.jcc(E);
                coldFpToInts_.push_back({cold, d, a, isDouble, a_.here()});
                commit(in.d, d);
                return;
            }
            case IrOp::Cset: {
                compare(in);
                a_.setcc(ccOf(in.cond), RAX);
                const Reg d = def(in.d, RAX);
                a_.movzxByte(d, RAX);
                commit(in.d, d);
                return;
            }
            case IrOp::FCset: {
                const Reg d = def(in.d, RAX);
                if (in.cond == IrCond::FEq || in.cond == IrCond::FNe) {
                    compareFp(in);
                    a_.setcc(in.cond == IrCond::FEq ? NP : P, RCX);
                    a_.setcc(in.cond == IrCond::FEq ? E : NE, RAX);
                    if (in.cond == IrCond::FEq)
                        a_.alu(0x21, RAX, RCX, false);
                    else
                        a_.alu(0x09, RAX, RCX, false);
                } else {
                    const Cc cc = compareFp(in);
                    a_.setcc(cc, RAX);
                }
                a_.movzxByte(d, RAX);
                commit(in.d, d);
                return;
            }
            case IrOp::Br:
                branchToLabel(a_.jmp(), std::int32_t(in.imm));
                return;
            case IrOp::BrCmp:
                compare(in);
                branchToLabel(a_.jcc(ccOf(in.cond)), in.target);
                return;
            case IrOp::BrFCmp:
                if (in.cond == IrCond::FEq) {
                    compareFp(in);
                    const std::size_t unordered = a_.jcc(P);
                    branchToLabel(a_.jcc(E), in.target);
                    a_.patch(unordered, a_.here());
                } else if (in.cond == IrCond::FNe) {
                    compareFp(in);
                    branchToLabel(a_.jcc(P), in.target);
                    branchToLabel(a_.jcc(NE), in.target);
                } else {
                    const Cc cc = compareFp(in);
                    branchToLabel(a_.jcc(cc), in.target);
                }
                return;
            case IrOp::ExitCmp:
                compare(in);
                branchToExit(a_.jcc(ccOf(in.cond)), in.target);
                return;
            case IrOp::Load:
            case IrOp::LoadIdx:
            case IrOp::Store:
            case IrOp::StoreIdx: memory(in); return;
            case IrOp::StoreTag: {
                const Reg base = use(in.a, RDX);
                a_.movImm(RAX, std::uint64_t(std::uint32_t(in.tag)));
                if (in.c >= 0) {
                    const Reg c = use(in.c, R10);
                    a_.movImm(RCX, std::uint64_t(std::uint32_t(Type::Null)));
                    a_.alu(0x85, c, c, true);
                    a_.cmov(E, RAX, RCX, false);
                }
                a_.store(RAX, base, std::int32_t(in.imm), 1);
                return;
            }
            case IrOp::IncRef:
            case IrOp::DecRef:
            case IrOp::IncRefTagged:
            case IrOp::DecRefTagged: {
                const bool tagged = in.op == IrOp::IncRefTagged || in.op == IrOp::DecRefTagged;
                const bool isDec = in.op == IrOp::DecRef || in.op == IrOp::DecRefTagged;
                std::size_t notCell = 0;
                if (tagged) {
                    const Reg t = use(in.b, RAX);
                    a_.aluImm(7, t, std::int32_t(Type::String), false);
                    notCell = a_.jcc(B);
                }
                const Reg a = use(in.a, RCX);
                std::size_t skip = 0;
                if (in.isNullable) {
                    a_.alu(0x85, a, a, true);
                    skip = a_.jcc(E);
                }
                a_.memAdjust(isDec ? 5 : 0, a, std::int32_t(kCellRefCountOffset));
                if (isDec) {
                    const std::size_t cold = a_.jcc(E);
                    coldDecRefs_.push_back({cold, a, a_.here(), in.target});
                }
                if (in.isNullable)
                    a_.patch(skip, a_.here());
                if (tagged)
                    a_.patch(notCell, a_.here());
                return;
            }
            case IrOp::TagOfNullable: {
                const Reg d = def(in.d, RAX);
                const Reg a = use(in.a, RCX);
                a_.movImm(RDX, std::uint64_t(std::uint32_t(Type::Null)));
                a_.movImmKeepFlags(R10, std::uint32_t(in.tag));
                a_.alu(0x85, a, a, true);
                a_.cmov(E, R10, RDX, false);
                a_.movRR(d, R10, false);
                commit(in.d, d);
                return;
            }
            case IrOp::Poll: {
                a_.cmpMem32Zero(kRt, config_.pollFlag);
                const std::size_t cold = a_.jcc(NE);
                coldPolls_.push_back({cold, in.target, a_.here()});
                return;
            }
            case IrOp::Call: emitCall(fn_.calls[std::size_t(in.imm)]); return;
            case IrOp::Ret:
                if (in.b >= 0) {
                    std::vector<Move> results;
                    Move payload;
                    payload.from = placeOf(in.a);
                    payload.to.reg = RAX;
                    results.push_back(payload);
                    Move tag;
                    tag.from = placeOf(in.b);
                    tag.to.reg = RDX;
                    results.push_back(tag);
                    parallelMoves(std::move(results), false);
                } else if (in.a >= 0) {
                    if (isFp(in.a)) {
                        const Xmm r = useFp(in.a, 0);
                        if (r != 0)
                            a_.movsdRR(0, r);
                    } else {
                        const Reg r = use(in.a, RAX);
                        if (r != RAX)
                            a_.movRR(RAX, r);
                    }
                }
                emitEpilogue();
                return;
            case IrOp::Trap: a_.int3(); return;
            case IrOp::SaveRegs:
                saverWanted_ = true;
                branchToLabel(a_.call(), std::int32_t(fn_.labelCount));
                return;
            case IrOp::FrameAddr: {
                const Reg d = def(in.d, RAX);
                a_.lea(d, RSP, std::int32_t(areaBase_) + std::int32_t(in.imm));
                commit(in.d, d);
                return;
            }
            case IrOp::FpToIntRaw: {
                const Xmm a = useFp(in.a, kFpScratchA);
                const Reg d = def(in.d, RAX);
                a_.fpToInt(d, a, true);
                commit(in.d, d);
                return;
            }
            default: return;
        }
    }

    bool saverWanted_ = false;
    const IrFunction& fn_;
    NativeConfig config_;
    const std::unordered_map<const MethodInfo*, std::size_t>& session_;
    Assembler a_;
    std::vector<std::size_t> labels_;
    std::vector<LabelFixup> labelFixups_;
    std::vector<LabelFixup> exitFixups_;
    std::vector<CallFixup> callFixups_;
    std::vector<NativeCode::Site> sites_;
    std::vector<OsrOut> osr_;
    std::vector<ColdDecRef> coldDecRefs_;
    std::vector<ColdPoll> coldPolls_;
    std::vector<ColdFpToInt> coldFpToInts_;
    std::vector<std::uint8_t> usedExits_;
    std::vector<std::size_t> exitPads_;
    std::vector<Reg> savedInt_;
    std::uint32_t areaBase_ = 0;
    std::uint32_t spillBase_ = 0;
    std::uint32_t coldSaveBase_ = 0;
    std::uint32_t saveBase_ = 0;
    std::uint32_t frameBytes_ = 0;
    bool hasFrame_ = false;
    bool needsOverflowPad_ = false;
    std::size_t overflowBranch_ = 0;
};

template <class T>
std::int32_t offsetInRuntime(const Runtime& rt, const T& field) noexcept
{
    return std::int32_t(reinterpret_cast<std::uintptr_t>(&field) - reinterpret_cast<std::uintptr_t>(&rt));
}

class X64Backend : public JitBackend
{
public:
    X64Backend() : layout_(valueLayout())
    {
        // rax, rcx, rdx, r10, r11 and r15 are spoken for: see the top of the file.
        regs_.intCallerSaved = {RSI, RDI, R8, R9};
        regs_.intCalleeSaved = {RBX, R12, R13, R14};
        regs_.fpCallerSaved = {8, 9, 10, 11, 12, 13, 7, 6, 5, 4, 3, 2, 1, 0};
        regs_.intArgs = {RDI, RSI, -1, -1, R8, R9};
        regs_.fpArgs = {0, 1, 2, 3, 4, 5, 6, 7};
        regs_.intResult = -1;
    }

    const char* name() const noexcept override { return "x86-64"; }

    bool available() const noexcept override
    {
#if SCRIPT_ENABLE_JIT
        return layout_.isExpected() && __builtin_cpu_supports("sse4.1");
#else
        return false;
#endif
    }

    bool supports(const MethodInfo&, std::string* reason) const override
    {
        if (available())
            return true;
        if (reason)
            *reason = "the x86-64 back end is not enabled in this build, or the processor has no SSE4.1";
        return false;
    }

    const std::string& lastRefusal() const noexcept override { return refusal_; }

    NativeCode* compile(Runtime& rt, MethodInfo& root, std::vector<NativeCode*>& others) override
    {
        struct Unit
        {
            MethodInfo* method;
            IrFunction fn;
            std::unique_ptr<Emitter> emitter;
            std::size_t at = 0;
            std::size_t bytes = 0;
        };
        std::vector<std::unique_ptr<Unit>> units;
        std::unordered_map<const MethodInfo*, std::size_t> session;

        std::vector<MethodInfo*> work{&root};
        while (!work.empty()) {
            MethodInfo* method = work.back();
            work.pop_back();
            if (session.count(method) || method->code.load(std::memory_order_acquire))
                continue;
            if (method->isNative() || method->isAbstract() || method->body.code.empty())
                continue;
            if (method->hotness.load(std::memory_order_relaxed) == MethodInfo::kNeverCompile)
                continue;
            auto unit = std::make_unique<Unit>();
            unit->method = method;
            std::string why;
            if (!lowerMethod(rt, *method, unit->fn, true, &why)) {
                method->hotness.store(MethodInfo::kNeverCompile, std::memory_order_relaxed);
                if (rt.ctx.options.jit.dumpCode)
                    rt.ctx.write(fromUtf8("jit: skipping " + method->qualifiedName() + ": " + why + "\n"), false);
                if (method == &root) {
                    refusal_ = why;
                    return nullptr;
                }
                continue;
            }
            for (const IrCall& call : unit->fn.calls) {
                if (call.kind == IrCallKind::Direct && call.target)
                    work.push_back(const_cast<MethodInfo*>(call.target));
            }
            session.emplace(method, units.size());
            units.push_back(std::move(unit));
        }
        if (units.empty())
            return nullptr;

        NativeConfig config;
        config.nativeError = offsetInRuntime(rt, rt.nativeError);
        config.pollFlag = offsetInRuntime(rt, rt.pollFlag);
        config.stackLimit = offsetInRuntime(rt, rt.nativeStackLimit);
        config.topFrame = offsetInRuntime(rt, rt.nativeTopFrame);
        config.raise = reinterpret_cast<const void*>(&scriptJitRaise);
        config.poll = reinterpret_cast<const void*>(&scriptJitPoll);

        std::size_t total = 0;
        for (auto& unit : units) {
            optimizeLoops(unit->fn);
            allocateRegisters(unit->fn, regs_);
            unit->emitter = std::make_unique<Emitter>(unit->fn, config, session);
            if (!unit->emitter->run()) {
                refusal_ = "the method is too big for the code generator";
                unit->method->hotness.store(MethodInfo::kNeverCompile, std::memory_order_relaxed);
                return nullptr;
            }
            unit->at = total;
            unit->bytes = unit->emitter->code().size();
            total += (unit->bytes + 15u) & ~std::size_t(15u);
        }

        void* block = cache_.reserve(total);
        if (!block)
            return nullptr;
        auto* base = static_cast<std::uint8_t*>(block);

        for (auto& unit : units) {
            std::vector<std::uint8_t>& code = unit->emitter->code();
            for (const CallFixup& fixup : unit->emitter->callFixups()) {
                const std::uint8_t* target = nullptr;
                const auto it = session.find(fixup.target);
                if (it != session.end()) {
                    target = base + units[it->second]->at;
                } else if (NativeCode* compiled = fixup.target->code.load(std::memory_order_acquire)) {
                    target = static_cast<const std::uint8_t*>(compiled->fastEntry);
                }
                const std::uint8_t* next = base + unit->at + fixup.at + 4;
                const std::int64_t delta = target ? target - next : (std::int64_t(1) << 40);
                if (delta < -2147483648ll || delta > 2147483647ll) {
                    cache_.release(block, total);
                    return nullptr;
                }
                const std::uint32_t rel = std::uint32_t(std::int32_t(delta));
                std::memcpy(&code[fixup.at], &rel, sizeof rel);
            }
            std::memcpy(base + unit->at, code.data(), unit->bytes);
        }
        if (!cache_.commit(block, total)) {
            cache_.release(block, total);
            return nullptr;
        }

        NativeCode* rootCode = nullptr;
        for (auto& unit : units) {
            auto* code = new NativeCode();
            code->method = unit->method;
            code->codeStart = base + unit->at;
            code->codeBytes = unit->bytes;
            code->backend = "x86-64";
            code->fastEntry = code->codeStart;
            for (const Emitter::OsrOut& osr : unit->emitter->osr()) {
                NativeCode::Osr out;
                out.pc = osr.pc;
                out.entry = base + unit->at + osr.at;
                out.ownedSlots = osr.owned;
                code->osr.push_back(std::move(out));
            }
            code->sites = unit->emitter->sites();
            for (const IrSite& site : unit->fn.sites)
                code->sourceSites.push_back({site.method, site.pc, site.parent});
            unit->method->fastEntry = code->fastEntry;
            if (unit->method->isStatic())
                unit->method->delegateEntry = code->fastEntry;
            if (unit->method == &root) {
                rootCode = code;
            } else {
                others.push_back(code);
                unit->method->code.store(code, std::memory_order_release);
            }
        }
        return rootCode;
    }

    bool buildFunction(IrFunction& fn, const NativeConfig& config, BuiltCode& out) const override
    {
        optimizeLoops(fn);
        allocateRegisters(fn, regs_);
        static const std::unordered_map<const MethodInfo*, std::size_t> kNoSession;
        Emitter emitter(fn, config, kNoSession);
        if (!emitter.run()) {
            out.refusal = "the code generator refused";
            return false;
        }
        out.bytes = emitter.code();
        for (const Emitter::OsrOut& osr : emitter.osr())
            out.osr.push_back({osr.pc, osr.at, osr.owned, osr.ownedPointers});
        out.sites = emitter.sites();
        for (const IrSite& site : fn.sites)
            out.sourceSites.push_back({site.method, site.pc, site.parent});
        return true;
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
    RegisterSet regs_;
    std::string refusal_;
};

#else  // not x86-64, or not its System V convention

class X64Backend : public JitBackend
{
public:
    const char* name() const noexcept override { return "x86-64"; }
    bool available() const noexcept override { return false; }
    bool supports(const MethodInfo&, std::string* reason) const override
    {
        if (reason)
            *reason = "this build does not target x86-64 with the System V convention";
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
