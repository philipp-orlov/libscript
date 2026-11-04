// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The arm64 code generator.
//
// The shape of the generated code, and why it is this shape:
//
//   * The frame the interpreter builds is the calling convention. `Value*
//     frame` arrives in x2 and stays in x19, so `LdLoc n` is a load from
//     `[x19, #n*16 + 8]` -- no indirection, no bounds check, no dispatch.
//   * A slot's static type never changes, so generated code writes only the
//     payload half of a slot and leaves the tag the caller wrote. That is what
//     takes `Value` construction out of the inner loop.
//   * The evaluation stack lives in the native frame, and its depth and the
//     type of every entry are compile-time constants (`planBody`), so a stack
//     slot is another constant offset and a conversion is one instruction
//     rather than a tag test.
//   * The supported subset is primitives only: no allocation, no reference
//     counting, no calls. That is what makes the generated code safe without
//     unwind tables or GC maps, and it covers the numeric kernels where the
//     interpreter's overhead actually shows.
//
// Widening it is a matter of adding cases: the runtime helpers in vm.hpp are
// ordinary functions, and `emitPoll` shows the call sequence.
#include <cstdio>
#include <cstring>

#include "script/internal/typed/bytecode.hpp"
#include "script/internal/typed/runtime.hpp"
#include "script/internal/typed/jit.hpp"

namespace script {
namespace internal {
namespace typed {

namespace {

#if defined(SCRIPT_ARCH_ARM64)

// ----------------------------------------------------------- encodings ---

enum Reg : std::uint32_t
{
    X0 = 0, X1, X2, X3, X4, X5, X6, X7,
    X8, X9, X10, X11, X12, X13, X14, X15,
    X16, X17, X18, X19, X20, X21, X22, X23,
    X24, X25, X26, X27, X28, X29, X30, XZR = 31,
};

enum Cond : std::uint32_t
{
    EQ = 0, NE = 1, HS = 2, LO = 3, MI = 4, PL = 5, VS = 6, VC = 7,
    HI = 8, LS = 9, GE = 10, LT = 11, GT = 12, LE = 13,
};

/// The fixed registers; x0-x15 are scratch.
constexpr Reg kFrame = X19;   ///< Value* frame
constexpr Reg kRt = X20;      ///< Runtime*
constexpr Reg kStack = X21;   ///< Value* evaluation stack
constexpr Reg kResult = X22;  ///< Value* result

class Assembler
{
public:
    void word(std::uint32_t instruction) { code_.push_back(instruction); }
    std::size_t here() const noexcept { return code_.size(); }
    const std::vector<std::uint32_t>& code() const noexcept { return code_; }

    // ---- moves ----
    void movReg(Reg d, Reg m) { word(0xAA0003E0u | (m << 16) | d); }
    void movImm(Reg d, std::uint64_t value)
    {
        word(0xD2800000u | (std::uint32_t(value & 0xFFFFu) << 5) | d);
        for (std::uint32_t shift = 1; shift < 4; ++shift) {
            const std::uint32_t half = std::uint32_t((value >> (shift * 16)) & 0xFFFFu);
            if (half)
                word(0xF2800000u | (shift << 21) | (half << 5) | d);
        }
    }

    // ---- memory ----
    void ldrX(Reg t, Reg n, std::uint32_t off) { word(0xF9400000u | ((off / 8) << 10) | (n << 5) | t); }
    void strX(Reg t, Reg n, std::uint32_t off) { word(0xF9000000u | ((off / 8) << 10) | (n << 5) | t); }
    void ldrW(Reg t, Reg n, std::uint32_t off) { word(0xB9400000u | ((off / 4) << 10) | (n << 5) | t); }
    void strW(Reg t, Reg n, std::uint32_t off) { word(0xB9000000u | ((off / 4) << 10) | (n << 5) | t); }
    void strB(Reg t, Reg n, std::uint32_t off) { word(0x39000000u | (off << 10) | (n << 5) | t); }
    void ldrD(std::uint32_t t, Reg n, std::uint32_t off) { word(0xFD400000u | ((off / 8) << 10) | (n << 5) | t); }
    void strD(std::uint32_t t, Reg n, std::uint32_t off) { word(0xFD000000u | ((off / 8) << 10) | (n << 5) | t); }
    void ldrS(std::uint32_t t, Reg n, std::uint32_t off) { word(0xBD400000u | ((off / 4) << 10) | (n << 5) | t); }
    void strS(std::uint32_t t, Reg n, std::uint32_t off) { word(0xBD000000u | ((off / 4) << 10) | (n << 5) | t); }

    // ---- integer arithmetic ----
    void addX(Reg d, Reg n, Reg m) { word(0x8B000000u | (m << 16) | (n << 5) | d); }
    void subX(Reg d, Reg n, Reg m) { word(0xCB000000u | (m << 16) | (n << 5) | d); }
    void mulX(Reg d, Reg n, Reg m) { word(0x9B007C00u | (m << 16) | (n << 5) | d); }
    void sdivX(Reg d, Reg n, Reg m) { word(0x9AC00C00u | (m << 16) | (n << 5) | d); }
    void udivX(Reg d, Reg n, Reg m) { word(0x9AC00800u | (m << 16) | (n << 5) | d); }
    void msubX(Reg d, Reg n, Reg m, Reg a) { word(0x9B008000u | (m << 16) | (a << 10) | (n << 5) | d); }
    void addW(Reg d, Reg n, Reg m) { word(0x0B000000u | (m << 16) | (n << 5) | d); }
    void subW(Reg d, Reg n, Reg m) { word(0x4B000000u | (m << 16) | (n << 5) | d); }
    void mulW(Reg d, Reg n, Reg m) { word(0x1B007C00u | (m << 16) | (n << 5) | d); }
    void sdivW(Reg d, Reg n, Reg m) { word(0x1AC00C00u | (m << 16) | (n << 5) | d); }
    void udivW(Reg d, Reg n, Reg m) { word(0x1AC00800u | (m << 16) | (n << 5) | d); }
    void msubW(Reg d, Reg n, Reg m, Reg a) { word(0x1B008000u | (m << 16) | (a << 10) | (n << 5) | d); }
    void negX(Reg d, Reg m) { word(0xCB0003E0u | (m << 16) | d); }
    void negW(Reg d, Reg m) { word(0x4B0003E0u | (m << 16) | d); }

    // ---- bitwise and shifts ----
    void andX(Reg d, Reg n, Reg m) { word(0x8A000000u | (m << 16) | (n << 5) | d); }
    void orrX(Reg d, Reg n, Reg m) { word(0xAA000000u | (m << 16) | (n << 5) | d); }
    void eorX(Reg d, Reg n, Reg m) { word(0xCA000000u | (m << 16) | (n << 5) | d); }
    void mvnX(Reg d, Reg m) { word(0xAA2003E0u | (m << 16) | d); }
    void andW(Reg d, Reg n, Reg m) { word(0x0A000000u | (m << 16) | (n << 5) | d); }
    void orrW(Reg d, Reg n, Reg m) { word(0x2A000000u | (m << 16) | (n << 5) | d); }
    void eorW(Reg d, Reg n, Reg m) { word(0x4A000000u | (m << 16) | (n << 5) | d); }
    void mvnW(Reg d, Reg m) { word(0x2A2003E0u | (m << 16) | d); }
    void lslX(Reg d, Reg n, Reg m) { word(0x9AC02000u | (m << 16) | (n << 5) | d); }
    void lsrX(Reg d, Reg n, Reg m) { word(0x9AC02400u | (m << 16) | (n << 5) | d); }
    void asrX(Reg d, Reg n, Reg m) { word(0x9AC02800u | (m << 16) | (n << 5) | d); }
    void lslW(Reg d, Reg n, Reg m) { word(0x1AC02000u | (m << 16) | (n << 5) | d); }
    void lsrW(Reg d, Reg n, Reg m) { word(0x1AC02400u | (m << 16) | (n << 5) | d); }
    void asrW(Reg d, Reg n, Reg m) { word(0x1AC02800u | (m << 16) | (n << 5) | d); }

    // ---- extension ----
    void sxtb(Reg d, Reg n) { word(0x93401C00u | (n << 5) | d); }
    void sxth(Reg d, Reg n) { word(0x93403C00u | (n << 5) | d); }
    void sxtw(Reg d, Reg n) { word(0x93407C00u | (n << 5) | d); }
    void uxtb(Reg d, Reg n) { word(0x53001C00u | (n << 5) | d); }
    void uxth(Reg d, Reg n) { word(0x53003C00u | (n << 5) | d); }
    void uxtw(Reg d, Reg n) { word(0x2A0003E0u | (n << 16) | d); }

    // ---- comparison ----
    void cmpX(Reg n, Reg m) { word(0xEB00001Fu | (m << 16) | (n << 5)); }
    void cmpW(Reg n, Reg m) { word(0x6B00001Fu | (m << 16) | (n << 5)); }
    void cmpImmX(Reg n, std::uint32_t imm12) { word(0xF100001Fu | (imm12 << 10) | (n << 5)); }
    void csetW(Reg d, Cond cond) { word(0x1A9F07E0u | ((cond ^ 1u) << 12) | d); }

    // ---- floating point ----
    void faddD(std::uint32_t d, std::uint32_t n, std::uint32_t m) { word(0x1E602800u | (m << 16) | (n << 5) | d); }
    void fsubD(std::uint32_t d, std::uint32_t n, std::uint32_t m) { word(0x1E603800u | (m << 16) | (n << 5) | d); }
    void fmulD(std::uint32_t d, std::uint32_t n, std::uint32_t m) { word(0x1E600800u | (m << 16) | (n << 5) | d); }
    void fdivD(std::uint32_t d, std::uint32_t n, std::uint32_t m) { word(0x1E601800u | (m << 16) | (n << 5) | d); }
    void fnegD(std::uint32_t d, std::uint32_t n) { word(0x1E614000u | (n << 5) | d); }
    void faddS(std::uint32_t d, std::uint32_t n, std::uint32_t m) { word(0x1E202800u | (m << 16) | (n << 5) | d); }
    void fsubS(std::uint32_t d, std::uint32_t n, std::uint32_t m) { word(0x1E203800u | (m << 16) | (n << 5) | d); }
    void fmulS(std::uint32_t d, std::uint32_t n, std::uint32_t m) { word(0x1E200800u | (m << 16) | (n << 5) | d); }
    void fdivS(std::uint32_t d, std::uint32_t n, std::uint32_t m) { word(0x1E201800u | (m << 16) | (n << 5) | d); }
    void fnegS(std::uint32_t d, std::uint32_t n) { word(0x1E214000u | (n << 5) | d); }
    void fcmpD(std::uint32_t n, std::uint32_t m) { word(0x1E602000u | (m << 16) | (n << 5)); }
    void fcmpS(std::uint32_t n, std::uint32_t m) { word(0x1E202000u | (m << 16) | (n << 5)); }
    void scvtfDX(std::uint32_t d, Reg n) { word(0x9E620000u | (n << 5) | d); }
    void scvtfDW(std::uint32_t d, Reg n) { word(0x1E620000u | (n << 5) | d); }
    void ucvtfDX(std::uint32_t d, Reg n) { word(0x9E630000u | (n << 5) | d); }
    void ucvtfDW(std::uint32_t d, Reg n) { word(0x1E630000u | (n << 5) | d); }
    void scvtfSX(std::uint32_t d, Reg n) { word(0x9E220000u | (n << 5) | d); }
    void scvtfSW(std::uint32_t d, Reg n) { word(0x1E220000u | (n << 5) | d); }
    void fcvtzsXD(Reg d, std::uint32_t n) { word(0x9E780000u | (n << 5) | d); }
    void fcvtzuXD(Reg d, std::uint32_t n) { word(0x9E790000u | (n << 5) | d); }
    void fcvtzsXS(Reg d, std::uint32_t n) { word(0x9E380000u | (n << 5) | d); }
    void fmovXD(Reg d, std::uint32_t n) { word(0x9E660000u | (n << 5) | d); }  // fmov xd, dn
    void fmovWS(Reg d, std::uint32_t n) { word(0x1E260000u | (n << 5) | d); }  // fmov wd, sn
    void lsl1X(Reg d, Reg n) { word(0xD37FF800u | (n << 5) | d); }             // lsl xd, xn, #1
    void lsl1W(Reg d, Reg n) { word(0x531F7800u | (n << 5) | d); }             // lsl wd, wn, #1
    void cselX(Reg d, Reg n, Reg m, Cond cond) { word(0x9A800000u | (m << 16) | (cond << 12) | (n << 5) | d); }
    void fcvtSD(std::uint32_t d, std::uint32_t n) { word(0x1E624000u | (n << 5) | d); }
    void fcvtDS(std::uint32_t d, std::uint32_t n) { word(0x1E22C000u | (n << 5) | d); }

    // ---- control ----
    std::size_t b() { word(0x14000000u); return here() - 1; }
    std::size_t bcond(Cond cond) { word(0x54000000u | cond); return here() - 1; }
    std::size_t cbzW(Reg t) { word(0x34000000u | t); return here() - 1; }
    std::size_t cbnzW(Reg t) { word(0x35000000u | t); return here() - 1; }
    std::size_t cbnzX(Reg t) { word(0xB5000000u | t); return here() - 1; }
    void blr(Reg n) { word(0xD63F0000u | (n << 5)); }
    void ret() { word(0xD65F03C0u); }

    void patch(std::size_t at, std::size_t target)
    {
        const std::int32_t delta = std::int32_t(target) - std::int32_t(at);
        std::uint32_t& instruction = code_[at];
        if ((instruction & 0xFC000000u) == 0x14000000u)
            instruction = 0x14000000u | (std::uint32_t(delta) & 0x03FFFFFFu);
        else
            instruction = (instruction & 0xFF00001Fu) | ((std::uint32_t(delta) & 0x7FFFFu) << 5);
    }

    void prologue(std::uint32_t stackBytes)
    {
        word(0xA9BF7BFDu);  // stp x29, x30, [sp, #-16]!
        word(0x910003FDu);  // mov x29, sp
        word(0xA9BF53F3u);  // stp x19, x20, [sp, #-16]!
        word(0xA9BF5BF5u);  // stp x21, x22, [sp, #-16]!
        movReg(kRt, X0);
        movReg(kFrame, X2);
        movReg(kResult, X4);
        if (stackBytes)
            word(0xD1000000u | (stackBytes << 10) | (31u << 5) | 31u);  // sub sp, sp, #n
        word(0x910003F5u);                                              // mov x21, sp
    }
    void epilogue(std::uint32_t stackBytes)
    {
        if (stackBytes)
            word(0x91000000u | (stackBytes << 10) | (31u << 5) | 31u);  // add sp, sp, #n
        word(0xA8C15BF5u);                                              // ldp x21, x22, [sp], #16
        word(0xA8C153F3u);                                              // ldp x19, x20, [sp], #16
        word(0xA8C17BFDu);                                              // ldp x29, x30, [sp], #16
        ret();
    }

private:
    std::vector<std::uint32_t> code_;
};

// -------------------------------------------------------------- codegen ---

class Arm64Compiler
{
public:
    Arm64Compiler(const MethodInfo& method, const BodyPlan& plan, std::uint32_t errorOffset)
        : m_(method), plan_(plan), errorOffset_(errorOffset)
    {}

    bool run()
    {
        const std::uint32_t stackBytes = ((plan_.maxDepth * 16u) + 15u) & ~15u;
        if (stackBytes > 4064 || m_.frameSize() * 16u > 32000u)
            return false;
        a_.prologue(stackBytes);

        const std::vector<std::uint8_t>& code = m_.body.code;
        offsets_.assign(code.size() + 1, 0);
        std::size_t pc = 0;
        while (pc < code.size()) {
            offsets_[pc] = a_.here();
            const Op op = Op(code[pc]);
            const std::int32_t depth = plan_.depth[pc];
            // Unreachable instructions -- the odd stranded one after a return
            // -- get no code, only a label.
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

    const std::vector<std::uint32_t>& code() const noexcept { return a_.code(); }

private:
    struct Fixup
    {
        std::size_t at;
        std::uint32_t target;
    };

    static std::uint32_t slotOffset(std::uint32_t index) noexcept { return index * 16 + 8; }
    static std::uint32_t stackOffset(std::uint32_t depth) noexcept { return depth * 16 + 8; }

    void loadStackX(Reg r, std::uint32_t depth) { a_.ldrX(r, kStack, stackOffset(depth)); }
    void storeStackX(Reg r, std::uint32_t depth) { a_.strX(r, kStack, stackOffset(depth)); }
    void loadStackW(Reg r, std::uint32_t depth) { a_.ldrW(r, kStack, stackOffset(depth)); }
    void loadStackD(std::uint32_t v, std::uint32_t depth) { a_.ldrD(v, kStack, stackOffset(depth)); }
    void storeStackD(std::uint32_t v, std::uint32_t depth) { a_.strD(v, kStack, stackOffset(depth)); }
    void loadStackS(std::uint32_t v, std::uint32_t depth) { a_.ldrS(v, kStack, stackOffset(depth)); }
    void storeStackS(std::uint32_t v, std::uint32_t depth)
    {
        // Clear the slot first, so the unused half of a float never carries
        // whatever the native stack happened to hold.
        a_.strX(XZR, kStack, stackOffset(depth));
        a_.strS(v, kStack, stackOffset(depth));
    }

    /// Raise from generated code: record the code and jump to the epilogue.
    void raise(ErrorCode code)
    {
        a_.movImm(X9, std::uint64_t(std::uint32_t(code)));
        a_.strW(X9, kRt, errorOffset_);
        exits_.push_back(a_.b());
    }

    /// After `fcvtzs`/`fcvtzu` of the floating-point register `fp` into `result`: make an infinity convert to 0, as the
    /// interpreter does (its `toInt64` maps NaN and both infinities to 0 and saturates only finite values), instead of
    /// the instruction's saturated value. Without this `(int)(1e300 * 1e300)` was 0 interpreted and -1 native, so a
    /// method gave a different answer once it got hot. Branch-free: the sign bit is shifted out and what is left is
    /// compared with the bits of an infinity.
    void zeroIfInfinity(Reg result, std::uint32_t fp, bool single)
    {
        if (single) {
            a_.fmovWS(X10, fp);
            a_.lsl1W(X10, X10);
            a_.movImm(X11, 0xFF000000u);
            a_.cmpW(X10, X11);
        } else {
            a_.fmovXD(X10, fp);
            a_.lsl1X(X10, X10);
            a_.movImm(X11, 0xFFE0000000000000ull);
            a_.cmpX(X10, X11);
        }
        a_.cselX(result, XZR, result, EQ);
    }

    /// The interrupt check at a loop back edge, matching the interpreter's
    /// tick. Everything live is in memory, so the call clobbers nothing.
    void emitPoll()
    {
        a_.movReg(X0, kRt);
        a_.movImm(X9, std::uint64_t(reinterpret_cast<std::uintptr_t>(&scriptJitPoll)));
        a_.blr(X9);
        const std::size_t keepGoing = a_.cbzW(X0);
        exits_.push_back(a_.b());
        a_.patch(keepGoing, a_.here());
    }

    bool emit(Op op, const std::uint8_t* operands, std::uint32_t depth, std::uint32_t pc,
              const std::vector<Type>& types)
    {
        const Type top = types.empty() ? Type::Int : types.back();
        switch (op) {
            case Op::Nop: return true;

            case Op::LdcI4:
                a_.movImm(X9, std::uint64_t(std::int64_t(std::int32_t(readU32(operands)))));
                storeStackX(X9, depth);
                return true;
            case Op::LdcI8:
            case Op::LdcR8: {
                const std::uint64_t bits =
                    std::uint64_t(readU32(operands)) | (std::uint64_t(readU32(operands + 4)) << 32);
                a_.movImm(X9, bits);
                storeStackX(X9, depth);
                return true;
            }
            case Op::LdcR4:
                a_.movImm(X9, std::uint64_t(readU32(operands)));
                storeStackX(X9, depth);
                return true;
            case Op::LdTrue:
            case Op::LdFalse:
                a_.movImm(X9, op == Op::LdTrue ? 1u : 0u);
                storeStackX(X9, depth);
                return true;
            case Op::Pop: return true;
            case Op::Dup:
                loadStackX(X9, depth - 1);
                storeStackX(X9, depth);
                return true;
            case Op::LdLoc:
                a_.ldrX(X9, kFrame, slotOffset(readU32(operands)));
                storeStackX(X9, depth);
                return true;
            case Op::StLoc:
                // Payload only: the tag the caller wrote is still right,
                // because a slot's static type does not change.
                loadStackX(X9, depth - 1);
                a_.strX(X9, kFrame, slotOffset(readU32(operands)));
                return true;

            case Op::Br:
                if (readU32(operands) <= pc)
                    emitPoll();
                fixups_.push_back({a_.b(), readU32(operands)});
                return true;
            case Op::BrTrue:
            case Op::BrFalse:
                if (readU32(operands) <= pc)
                    emitPoll();
                loadStackX(X9, depth - 1);
                a_.cmpImmX(X9, 0);
                fixups_.push_back({a_.bcond(op == Op::BrTrue ? NE : EQ), readU32(operands)});
                return true;
            case Op::Ret:
                storeResult(depth - 1);
                exits_.push_back(a_.b());
                return true;
            case Op::RetVoid:
                exits_.push_back(a_.b());
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
            case Op::ShrU4:
                loadStackW(X9, depth - 2);
                loadStackW(X10, depth - 1);
                switch (op) {
                    case Op::AddI4: a_.addW(X9, X9, X10); break;
                    case Op::SubI4: a_.subW(X9, X9, X10); break;
                    case Op::MulI4: a_.mulW(X9, X9, X10); break;
                    case Op::AndI4: a_.andW(X9, X9, X10); break;
                    case Op::OrI4: a_.orrW(X9, X9, X10); break;
                    case Op::XorI4: a_.eorW(X9, X9, X10); break;
                    case Op::ShlI4: a_.lslW(X9, X9, X10); break;
                    case Op::ShrI4: a_.asrW(X9, X9, X10); break;
                    default: a_.lsrW(X9, X9, X10); break;
                }
                a_.sxtw(X9, X9);
                storeStackX(X9, depth - 2);
                return true;
            case Op::NegI4:
                loadStackW(X9, depth - 1);
                a_.negW(X9, X9);
                a_.sxtw(X9, X9);
                storeStackX(X9, depth - 1);
                return true;
            case Op::NotI4:
                loadStackW(X9, depth - 1);
                a_.mvnW(X9, X9);
                a_.sxtw(X9, X9);
                storeStackX(X9, depth - 1);
                return true;
            case Op::DivI4:
            case Op::DivU4:
            case Op::RemI4:
            case Op::RemU4: {
                loadStackW(X9, depth - 2);
                loadStackW(X10, depth - 1);
                const std::size_t ok = a_.cbnzW(X10);
                raise(ErrorCode::DivideByZero);
                a_.patch(ok, a_.here());
                if (op == Op::DivI4 || op == Op::RemI4)
                    a_.sdivW(X11, X9, X10);
                else
                    a_.udivW(X11, X9, X10);
                if (op == Op::RemI4 || op == Op::RemU4)
                    a_.msubW(X9, X11, X10, X9);
                else
                    a_.movReg(X9, X11);
                a_.sxtw(X9, X9);
                storeStackX(X9, depth - 2);
                return true;
            }

            case Op::AddI8:
            case Op::SubI8:
            case Op::MulI8:
            case Op::AndI8:
            case Op::OrI8:
            case Op::XorI8:
            case Op::ShlI8:
            case Op::ShrI8:
            case Op::ShrU8:
                loadStackX(X9, depth - 2);
                loadStackX(X10, depth - 1);
                switch (op) {
                    case Op::AddI8: a_.addX(X9, X9, X10); break;
                    case Op::SubI8: a_.subX(X9, X9, X10); break;
                    case Op::MulI8: a_.mulX(X9, X9, X10); break;
                    case Op::AndI8: a_.andX(X9, X9, X10); break;
                    case Op::OrI8: a_.orrX(X9, X9, X10); break;
                    case Op::XorI8: a_.eorX(X9, X9, X10); break;
                    case Op::ShlI8: a_.lslX(X9, X9, X10); break;
                    case Op::ShrI8: a_.asrX(X9, X9, X10); break;
                    default: a_.lsrX(X9, X9, X10); break;
                }
                storeStackX(X9, depth - 2);
                return true;
            case Op::NegI8:
                loadStackX(X9, depth - 1);
                a_.negX(X9, X9);
                storeStackX(X9, depth - 1);
                return true;
            case Op::NotI8:
                loadStackX(X9, depth - 1);
                a_.mvnX(X9, X9);
                storeStackX(X9, depth - 1);
                return true;
            case Op::DivI8:
            case Op::DivU8:
            case Op::RemI8:
            case Op::RemU8: {
                loadStackX(X9, depth - 2);
                loadStackX(X10, depth - 1);
                const std::size_t ok = a_.cbnzX(X10);
                raise(ErrorCode::DivideByZero);
                a_.patch(ok, a_.here());
                if (op == Op::DivI8 || op == Op::RemI8)
                    a_.sdivX(X11, X9, X10);
                else
                    a_.udivX(X11, X9, X10);
                if (op == Op::RemI8 || op == Op::RemU8)
                    a_.msubX(X9, X11, X10, X9);
                else
                    a_.movReg(X9, X11);
                storeStackX(X9, depth - 2);
                return true;
            }

            case Op::AddR8:
            case Op::SubR8:
            case Op::MulR8:
            case Op::DivR8:
                loadStackD(0, depth - 2);
                loadStackD(1, depth - 1);
                switch (op) {
                    case Op::AddR8: a_.faddD(0, 0, 1); break;
                    case Op::SubR8: a_.fsubD(0, 0, 1); break;
                    case Op::MulR8: a_.fmulD(0, 0, 1); break;
                    default: a_.fdivD(0, 0, 1); break;
                }
                storeStackD(0, depth - 2);
                return true;
            case Op::NegR8:
                loadStackD(0, depth - 1);
                a_.fnegD(0, 0);
                storeStackD(0, depth - 1);
                return true;
            case Op::AddR4:
            case Op::SubR4:
            case Op::MulR4:
            case Op::DivR4:
                loadStackS(0, depth - 2);
                loadStackS(1, depth - 1);
                switch (op) {
                    case Op::AddR4: a_.faddS(0, 0, 1); break;
                    case Op::SubR4: a_.fsubS(0, 0, 1); break;
                    case Op::MulR4: a_.fmulS(0, 0, 1); break;
                    default: a_.fdivS(0, 0, 1); break;
                }
                storeStackS(0, depth - 2);
                return true;
            case Op::NegR4:
                loadStackS(0, depth - 1);
                a_.fnegS(0, 0);
                storeStackS(0, depth - 1);
                return true;

            case Op::LogicalNot:
                loadStackX(X9, depth - 1);
                a_.cmpImmX(X9, 0);
                a_.csetW(X9, EQ);
                storeStackX(X9, depth - 1);
                return true;
            default: break;
        }
        return emitCompare(op, depth, types);
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
                // Narrowing to an int-sized result; a real source goes through
                // the float unit first.
                if (from == Type::Double) {
                    loadStackD(0, at);
                    a_.fcvtzsXD(X9, 0);
                    zeroIfInfinity(X9, 0, false);
                } else if (from == Type::Float) {
                    loadStackS(0, at);
                    a_.fcvtzsXS(X9, 0);
                    zeroIfInfinity(X9, 0, true);
                } else {
                    loadStackX(X9, at);
                }
                switch (op) {
                    case Op::ConvI1: a_.sxtb(X9, X9); break;
                    case Op::ConvI2: a_.sxth(X9, X9); break;
                    case Op::ConvU1: a_.uxtb(X9, X9); break;
                    case Op::ConvU2:
                    case Op::ConvChar: a_.uxth(X9, X9); break;
                    case Op::ConvU4: a_.uxtw(X9, X9); break;
                    default: a_.sxtw(X9, X9); break;
                }
                storeStackX(X9, at);
                return true;
            }
            case Op::ConvI8:
            case Op::ConvU8: {
                if (from == Type::Double) {
                    loadStackD(0, at);
                    if (op == Op::ConvU8)
                        a_.fcvtzuXD(X9, 0);
                    else
                        a_.fcvtzsXD(X9, 0);
                    zeroIfInfinity(X9, 0, false);
                } else if (from == Type::Float) {
                    loadStackS(0, at);
                    a_.fcvtzsXS(X9, 0);
                    zeroIfInfinity(X9, 0, true);
                } else if (from == Type::Long) {
                    return true;  // already the right width
                } else {
                    loadStackX(X9, at);
                    if (op == Op::ConvU8)
                        a_.uxtw(X9, X9);
                    else
                        a_.sxtw(X9, X9);
                }
                storeStackX(X9, at);
                return true;
            }
            case Op::ConvBool:
                loadStackX(X9, at);
                a_.cmpImmX(X9, 0);
                a_.csetW(X9, NE);
                storeStackX(X9, at);
                return true;
            case Op::ConvR8:
            case Op::ConvR8Un: {
                if (from == Type::Double)
                    return true;
                if (from == Type::Float) {
                    loadStackS(0, at);
                    a_.fcvtDS(0, 0);
                } else if (from == Type::Long) {
                    loadStackX(X9, at);
                    if (op == Op::ConvR8Un)
                        a_.ucvtfDX(0, X9);
                    else
                        a_.scvtfDX(0, X9);
                } else {
                    loadStackW(X9, at);
                    if (op == Op::ConvR8Un)
                        a_.ucvtfDW(0, X9);
                    else
                        a_.scvtfDW(0, X9);
                }
                storeStackD(0, at);
                return true;
            }
            case Op::ConvR4: {
                if (from == Type::Float)
                    return true;
                if (from == Type::Double) {
                    loadStackD(0, at);
                    a_.fcvtSD(0, 0);
                } else if (from == Type::Long) {
                    loadStackX(X9, at);
                    a_.scvtfSX(0, X9);
                } else {
                    loadStackW(X9, at);
                    a_.scvtfSW(0, X9);
                }
                storeStackS(0, at);
                return true;
            }
            default: return false;
        }
    }

    bool emitCompare(Op op, std::uint32_t depth, const std::vector<Type>& types)
    {
        Cond cond = EQ;
        bool wide = true;
        bool real = false;
        switch (op) {
            case Op::CeqI4: cond = EQ; wide = false; break;
            case Op::CneI4: cond = NE; wide = false; break;
            case Op::CltI4: cond = LT; wide = false; break;
            case Op::CleI4: cond = LE; wide = false; break;
            case Op::CgtI4: cond = GT; wide = false; break;
            case Op::CgeI4: cond = GE; wide = false; break;
            case Op::CltU4: cond = LO; wide = false; break;
            case Op::CleU4: cond = LS; wide = false; break;
            case Op::CgtU4: cond = HI; wide = false; break;
            case Op::CgeU4: cond = HS; wide = false; break;
            case Op::CeqI8: cond = EQ; break;
            case Op::CneI8: cond = NE; break;
            case Op::CltI8: cond = LT; break;
            case Op::CleI8: cond = LE; break;
            case Op::CgtI8: cond = GT; break;
            case Op::CgeI8: cond = GE; break;
            case Op::CltU8: cond = LO; break;
            case Op::CleU8: cond = LS; break;
            case Op::CgtU8: cond = HI; break;
            case Op::CgeU8: cond = HS; break;
            // These are the conditions that answer false for NaN, which is
            // what the language asks for.
            case Op::CeqR8: cond = EQ; real = true; break;
            case Op::CneR8: cond = NE; real = true; break;
            case Op::CltR8: cond = MI; real = true; break;
            case Op::CleR8: cond = LS; real = true; break;
            case Op::CgtR8: cond = GT; real = true; break;
            case Op::CgeR8: cond = GE; real = true; break;
            default: return false;
        }

        if (real) {
            // The float comparisons are spelled R8 whatever the operands are,
            // so their type decides the instruction.
            const Type operand = types.empty() ? Type::Double : types.back();
            if (operand == Type::Float) {
                loadStackS(0, depth - 2);
                loadStackS(1, depth - 1);
                a_.fcmpS(0, 1);
            } else {
                loadStackD(0, depth - 2);
                loadStackD(1, depth - 1);
                a_.fcmpD(0, 1);
            }
        } else if (wide) {
            loadStackX(X9, depth - 2);
            loadStackX(X10, depth - 1);
            a_.cmpX(X9, X10);
        } else {
            loadStackW(X9, depth - 2);
            loadStackW(X10, depth - 1);
            a_.cmpW(X9, X10);
        }
        a_.csetW(X9, cond);
        storeStackX(X9, depth - 2);
        return true;
    }

    /// `*result = stack[depth]`, tag included: this one leaves the frame.
    void storeResult(std::uint32_t depth)
    {
        loadStackX(X9, depth);
        a_.strX(X9, kResult, 8);
        a_.movImm(X10, std::uint64_t(std::uint32_t(runtimeTag(m_.returnType->kind))));
        a_.strB(X10, kResult, 0);
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

class Arm64Backend : public JitBackend
{
public:
    Arm64Backend() : layout_(valueLayout()) {}

    const char* name() const noexcept override { return "arm64"; }

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
                *reason = "the arm64 back end is not enabled in this build";
            return false;
        }
        BodyPlan plan;
        return planBody(method, plan, reason);
    }

    NativeCode* compile(Runtime& rt, MethodInfo& method) override
    {
        BodyPlan plan;
        if (!planBody(method, plan, nullptr))
            return nullptr;

        // Where `nativeError` sits, measured rather than assumed: Runtime
        // is not a standard-layout type.
        const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(&rt);
        const std::uintptr_t field = reinterpret_cast<std::uintptr_t>(&rt.nativeError);
        const std::uint32_t errorOffset = std::uint32_t(field - base);
        if (errorOffset > 16380 || (errorOffset & 3u) != 0)
            return nullptr;

        Arm64Compiler compiler(method, plan, errorOffset);
        if (!compiler.run())
            return nullptr;

        const std::size_t bytes = compiler.code().size() * sizeof(std::uint32_t);
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
        code->backend = "arm64";
        return code;
    }

    void release(NativeCode* code) noexcept override
    {
        if (!code)
            return;
        cache_.release(code->codeStart, code->codeBytes);
        delete code;
    }

    std::string dump(const NativeCode* code) const override
    {
        if (!code || !code->codeStart)
            return "<no code>";
        std::string out;
        char buf[48];
        const auto* words = static_cast<const std::uint32_t*>(code->codeStart);
        for (std::size_t i = 0; i < code->codeBytes / 4; ++i) {
            std::snprintf(buf, sizeof buf, "  %4zu: %08x\n", i * 4, words[i]);
            out += buf;
        }
        return out;
    }

private:
    ValueLayout layout_;
};

#else  // the host is not arm64

class Arm64Backend : public JitBackend
{
public:
    const char* name() const noexcept override { return "arm64"; }
    bool available() const noexcept override { return false; }
    bool supports(const MethodInfo&, std::string* reason) const override
    {
        if (reason)
            *reason = "this build does not target arm64";
        return false;
    }
    NativeCode* compile(Runtime&, MethodInfo&) override { return nullptr; }
    void release(NativeCode* code) noexcept override { delete code; }
};

#endif

}  // namespace

std::unique_ptr<JitBackend> createArm64Backend()
{
    return std::make_unique<Arm64Backend>();
}

}  // namespace typed
}  // namespace internal
}  // namespace script
