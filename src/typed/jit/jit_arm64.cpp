// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The arm64 code generator: IR in, machine code out.
//
// The native calling convention, which only generated code and the stubs below use:
//
//   * integer and reference arguments in x0-x7, floats in d0-d7, the rest in stack words, in declaration order with `this`
//     first; a result in x0 or d0;
//   * x20 holds the runtime, for a whole run of native code;
//   * x19 and x21-x28, d8-d15 are callee-saved as in the C convention, which is what lets helpers be plain C functions;
//   * x14 holds the delegate at a call through one, and x15 the method at a call through a method's entry; x16 and x17
//     and d30 and d31 are this file's scratch registers;
//   * x29 chains the frames, so that a stack trace can walk them.
//
// A method that raises records the error in the runtime and returns; its caller checks after the call. Nothing unwinds.
#include <algorithm>
#include <map>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

#include "script/internal/typed/bytecode.hpp"
#include "script/internal/typed/runtime.hpp"
#include "script/internal/typed/jit.hpp"
#include "script/internal/typed/jit_ir.hpp"
#include "script/internal/typed/jit_runtime.hpp"
#include "script/internal/typed/vm.hpp"

namespace script {
namespace internal {
namespace typed {

namespace {

#if defined(SCRIPT_ARCH_ARM64)
#if defined(__linux__)
#include <asm/hwcap.h>
#include <sys/auxv.h>
#endif

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

constexpr Reg kRt = X20;          ///< Runtime*, pinned for the whole of a run of native code
constexpr Reg kDelegateReg = X14; ///< at a call through a delegate: the delegate
constexpr Reg kMethodReg = X15;   ///< at a call through a method's entry: the method
constexpr std::uint32_t kFpScratchA = 30, kFpScratchB = 31;

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
    void fjcvtzs(Reg d, std::uint32_t n) { word(0x1E7E0000u | (n << 5) | d); }
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
        if ((instruction & 0xFC000000u) == 0x14000000u) {
            if (delta < -(1 << 25) || delta >= (1 << 25))
                rangeLost_ = true;
            instruction = 0x14000000u | (std::uint32_t(delta) & 0x03FFFFFFu);
        } else {
            // What did not reach is noted, and the whole function is tried again with long branches.
            if (delta < -(1 << 18) || delta >= (1 << 18))
                rangeLost_ = true;
            instruction = (instruction & 0xFF00001Fu) | ((std::uint32_t(delta) & 0x7FFFFu) << 5);
        }
    }
    bool rangeLost() const noexcept { return rangeLost_; }
    bool rangeLost_ = false;

    // ---- what the optimizing tier adds ----
    void loadMem(Reg t, Reg n, std::uint32_t off, std::uint32_t bytes, bool isSigned)
    {
        switch (bytes) {
            case 1: word((isSigned ? 0x39C00000u : 0x39400000u) | (off << 10) | (n << 5) | t); return;
            case 2: word((isSigned ? 0x79C00000u : 0x79400000u) | ((off / 2) << 10) | (n << 5) | t); return;
            case 4: ldrW(t, n, off); return;
            default: ldrX(t, n, off); return;
        }
    }
    void storeMem(Reg t, Reg n, std::uint32_t off, std::uint32_t bytes)
    {
        switch (bytes) {
            case 1: strB(t, n, off); return;
            case 2: word(0x79000000u | ((off / 2) << 10) | (n << 5) | t); return;
            case 4: strW(t, n, off); return;
            default: strX(t, n, off); return;
        }
    }
    void loadIdx(Reg t, Reg n, Reg m, std::uint32_t bytes, bool isSigned, bool shifted)
    {
        std::uint32_t base = 0;
        switch (bytes) {
            case 1: base = isSigned ? 0x38E06800u : 0x38606800u; break;
            case 2: base = isSigned ? 0x78E06800u : 0x78606800u; break;
            case 4: base = 0xB8606800u; break;
            default: base = 0xF8606800u; break;
        }
        word(base | (shifted ? 0x1000u : 0u) | (m << 16) | (n << 5) | t);
    }
    void storeIdx(Reg t, Reg n, Reg m, std::uint32_t bytes, bool shifted)
    {
        std::uint32_t base = 0;
        switch (bytes) {
            case 1: base = 0x38206800u; break;
            case 2: base = 0x78206800u; break;
            case 4: base = 0xB8206800u; break;
            default: base = 0xF8206800u; break;
        }
        word(base | (shifted ? 0x1000u : 0u) | (m << 16) | (n << 5) | t);
    }
    void fpLoadIdx(std::uint32_t t, Reg n, Reg m, std::uint32_t bytes, bool shifted)
    {
        word((bytes == 4 ? 0xBC606800u : 0xFC606800u) | (shifted ? 0x1000u : 0u) | (m << 16) | (n << 5) | t);
    }
    void fpStoreIdx(std::uint32_t t, Reg n, Reg m, std::uint32_t bytes, bool shifted)
    {
        word((bytes == 4 ? 0xBC206800u : 0xFC206800u) | (shifted ? 0x1000u : 0u) | (m << 16) | (n << 5) | t);
    }
    void addImm(Reg d, Reg n, std::uint32_t imm, bool wide) { word((wide ? 0x91000000u : 0x11000000u) | (imm << 10) | (n << 5) | d); }
    void subImm(Reg d, Reg n, std::uint32_t imm, bool wide) { word((wide ? 0xD1000000u : 0x51000000u) | (imm << 10) | (n << 5) | d); }
    /// The same of up to 24 bits, as a shifted part and the rest: a frame larger than the 12 bits one takes.
    void addLarge(Reg d, Reg n, std::uint32_t imm)
    {
        if (imm >> 12) {
            word(0x91400000u | ((imm >> 12) << 10) | (n << 5) | d);
            n = d;
            imm &= 0xfffu;
            if (!imm)
                return;
        }
        addImm(d, n, imm, true);
    }
    void subLarge(Reg d, Reg n, std::uint32_t imm)
    {
        if (imm >> 12) {
            word(0xD1400000u | ((imm >> 12) << 10) | (n << 5) | d);
            n = d;
            imm &= 0xfffu;
            if (!imm)
                return;
        }
        subImm(d, n, imm, true);
    }
    void subsImm(Reg d, Reg n, std::uint32_t imm, bool wide) { word((wide ? 0xF1000000u : 0x71000000u) | (imm << 10) | (n << 5) | d); }
    void cmpImm(Reg n, std::uint32_t imm, bool wide) { subsImm(XZR, n, imm, wide); }
    void cmnImm(Reg n, std::uint32_t imm, bool wide) { word((wide ? 0xB100001Fu : 0x3100001Fu) | (imm << 10) | (n << 5)); }
    void lslImm(Reg d, Reg n, std::uint32_t s, bool wide)
    {
        if (wide)
            word(0xD3400000u | (((64 - s) & 63) << 16) | ((63 - s) << 10) | (n << 5) | d);
        else
            word(0x53000000u | (((32 - s) & 31) << 16) | ((31 - s) << 10) | (n << 5) | d);
    }
    void lsrImm(Reg d, Reg n, std::uint32_t s, bool wide) { word((wide ? 0xD340FC00u : 0x53007C00u) | (s << 16) | (n << 5) | d); }
    void asrImm(Reg d, Reg n, std::uint32_t s, bool wide) { word((wide ? 0x9340FC00u : 0x13007C00u) | (s << 16) | (n << 5) | d); }
    /// A logical instruction with an encoded bitmask immediate. `opc`: 0 and, 1 orr, 2 eor.
    void logicalImm(std::uint32_t opc, Reg d, Reg n, std::uint32_t encoded, bool wide)
    {
        const std::uint32_t base = (wide ? 0x92000000u : 0x12000000u) | (opc << 29);
        word(base | (encoded << 10) | (n << 5) | d);
    }
    void cselW(Reg d, Reg n, Reg m, Cond cond) { word(0x1A800000u | (m << 16) | (cond << 12) | (n << 5) | d); }
    void cmpSp(Reg m) { word(0xEB2063FFu | (m << 16)); }
    void movW(Reg d, Reg m) { word(0x2A0003E0u | (m << 16) | d); }
    void movSp(Reg d, Reg n) { addImm(d, n, 0, true); }
    void fmovD(std::uint32_t d, std::uint32_t n) { word(0x1E604000u | (n << 5) | d); }
    void fmovDX(std::uint32_t d, Reg n) { word(0x9E670000u | (n << 5) | d); }
    void fmovSW(std::uint32_t d, Reg n) { word(0x1E270000u | (n << 5) | d); }
    void fsqrtD(std::uint32_t d, std::uint32_t n) { word(0x1E61C000u | (n << 5) | d); }
    void fabsD(std::uint32_t d, std::uint32_t n) { word(0x1E60C000u | (n << 5) | d); }
    void frintmD(std::uint32_t d, std::uint32_t n) { word(0x1E654000u | (n << 5) | d); }
    void frintpD(std::uint32_t d, std::uint32_t n) { word(0x1E64C000u | (n << 5) | d); }
    void frintzD(std::uint32_t d, std::uint32_t n) { word(0x1E65C000u | (n << 5) | d); }
    void frintnD(std::uint32_t d, std::uint32_t n) { word(0x1E644000u | (n << 5) | d); }
    void ucvtfSW(std::uint32_t d, Reg n) { word(0x1E230000u | (n << 5) | d); }
    void ucvtfSX(std::uint32_t d, Reg n) { word(0x9E230000u | (n << 5) | d); }
    void stpX(Reg a, Reg b, Reg n, std::int32_t off) { word(0xA9000000u | ((std::uint32_t(off / 8) & 0x7Fu) << 15) | (b << 10) | (n << 5) | a); }
    void ldpX(Reg a, Reg b, Reg n, std::int32_t off) { word(0xA9400000u | ((std::uint32_t(off / 8) & 0x7Fu) << 15) | (b << 10) | (n << 5) | a); }
    std::size_t cbzX(Reg t) { word(0xB4000000u | t); return here() - 1; }
    std::size_t bl() { word(0x94000000u); return here() - 1; }
    void brk() { word(0xD4200000u); }
    bool patchChecked(std::size_t at, std::size_t target)
    {
        const std::int64_t delta = std::int64_t(target) - std::int64_t(at);
        const std::uint32_t top = code_[at] & 0xFC000000u;
        if (top == 0x14000000u || top == 0x94000000u) {
            if (delta < -(std::int64_t(1) << 25) || delta >= (std::int64_t(1) << 25))
                return false;
            code_[at] = top | (std::uint32_t(delta) & 0x03FFFFFFu);
            return true;
        }
        if (delta < -(std::int64_t(1) << 18) || delta >= (std::int64_t(1) << 18))
            return false;
        code_[at] = (code_[at] & 0xFF00001Fu) | ((std::uint32_t(delta) & 0x7FFFFu) << 5);
        return true;
    }
    std::vector<std::uint32_t>& mutableCode() noexcept { return code_; }

private:
    std::vector<std::uint32_t> code_;
};

// -------------------------------------------------------------- codegen ---

/// The encoding of `value` as an arm64 logical immediate, when it has one: N, immr, imms packed as the instruction wants
/// them. Returns -1 otherwise.
std::int32_t encodeLogicalImm(std::uint64_t value, bool wide) noexcept
{
    if (!wide)
        value = std::uint64_t(std::uint32_t(value)) | (std::uint64_t(std::uint32_t(value)) << 32);
    if (value == 0 || value == ~std::uint64_t(0))
        return -1;
    // The smallest repeating element.
    unsigned size = 64;
    while (size > 2) {
        const unsigned half = size / 2;
        const std::uint64_t mask = (std::uint64_t(1) << half) - 1;
        if ((value & mask) != ((value >> half) & mask))
            break;
        size = half;
    }
    const std::uint64_t mask = size == 64 ? ~std::uint64_t(0) : (std::uint64_t(1) << size) - 1;
    const std::uint64_t element = value & mask;
    // It must be one run of ones, rotated.
    unsigned ones = unsigned(__builtin_popcountll(element));
    unsigned rotation = 0;
    for (; rotation < size; ++rotation) {
        const std::uint64_t rotated =
            rotation == 0 ? element : ((element >> rotation) | (element << (size - rotation))) & mask;
        if (rotated == (ones == 64 ? mask : (std::uint64_t(1) << ones) - 1))
            break;
    }
    if (rotation == size)
        return -1;
    const std::uint32_t immr = (size - rotation) & (size - 1);
    std::uint32_t imms = (ones - 1) | (~(size * 2 - 1) & 0x3F);
    const std::uint32_t n = size == 64 ? 1 : 0;
    if (!wide && n)
        return -1;
    return std::int32_t((n << 12) | (immr << 6) | imms);
}

/// The eight bits `fmov` can carry for this constant, or -1. Most literals in real code fit: 0.5, 1, 2, 4, 10.
std::int32_t fmovImm8(std::uint64_t bits, bool isDouble) noexcept
{
    if (isDouble) {
        if (bits & 0xFFFFFFFFFFFFull)
            return -1;
        const std::uint32_t exp = std::uint32_t((bits >> 54) & 0x1FF);
        if (exp != 0x100 && exp != 0xFF)
            return -1;
        return std::int32_t(((bits >> 56) & 0x80) | ((bits >> 48) & 0x7F));
    }
    const std::uint32_t f = std::uint32_t(bits);
    if (f & 0x7FFFF)
        return -1;
    const std::uint32_t exp = (f >> 25) & 0x3F;
    if (exp != 0x20 && exp != 0x1F)
        return -1;
    return std::int32_t(((f >> 24) & 0x80) | ((f >> 19) & 0x7F));
}

Cond condOf(IrCond c) noexcept
{
    switch (c) {
        case IrCond::Eq:
        case IrCond::FEq: return EQ;
        case IrCond::Ne:
        case IrCond::FNe: return NE;
        case IrCond::Lt: return LT;
        case IrCond::Le: return LE;
        case IrCond::Gt:
        case IrCond::FGt: return GT;
        case IrCond::Ge:
        case IrCond::FGe: return GE;
        case IrCond::Lo: return LO;
        case IrCond::Ls:
        case IrCond::FLe: return LS;
        case IrCond::Hi: return HI;
        case IrCond::Hs: return HS;
        case IrCond::FLt: return MI;
    }
    return EQ;
}

/// A call whose target is known at link time.
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
        // A spill slot is reached with a scaled 12-bit offset: 32 KB.
        if (frameBytes_ > 32000) {
            why_ = "frame of " + std::to_string(frameBytes_) + " bytes";
            return false;
        }
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
        const std::size_t hotWords = a_.here();
        // Falling off the end only happens after a raise, which never comes back.
        a_.brk();
        cold = false;
        for (const IrInst& in : fn_.code) {
            if (in.op == IrOp::ColdBegin || in.op == IrOp::ColdEnd)
                cold = in.op == IrOp::ColdBegin;
            else if (cold)
                emit(in);
        }
        const std::size_t coldWords = a_.here();
        emitCold();
        const std::size_t padWords = a_.here();
        emitOsrEntries();
#if defined(SCRIPT_JIT_ENV_OVERRIDE)
        if (std::getenv("SCRIPT_JIT_SIZES") && a_.here() > 50000)
            std::fprintf(stderr, "sizes: hot %zu cold %zu pads %zu osr %zu words, %zu exits, %zu osr entries\n", hotWords, coldWords - hotWords, padWords - coldWords,
                         a_.here() - padWords, fn_.exits.size(), fn_.osrEntries.size());
#else
        (void)hotWords;
        (void)coldWords;
        (void)padWords;
#endif
        if (saverWanted_) {
            labels_[std::size_t(fn_.labelCount)] = a_.here();
            a_.addImm(X16, kRt, std::uint32_t(config_.savedRegs), true);
            for (std::uint32_t r = 0; r < 30; r += 2)
                a_.stpX(Reg(r), Reg(r + 1), X16, std::int32_t(r * 8));
            for (std::uint32_t r = 0; r < 32; ++r)
                a_.strD(r, X16, (32 + r) * 8);
            a_.ret();
        }
        for (const LabelFixup& fixup : labelFixups_) {
            if (labels_[std::size_t(fixup.label)] == std::size_t(-1)) {
                why_ = "a label never placed";
                return false;
            }
            if (!a_.patchChecked(fixup.at, labels_[std::size_t(fixup.label)])) {
                why_ = "label branch out of range: word " + std::to_string(a_.code()[fixup.at]) + " at " + std::to_string(fixup.at) + " to " + std::to_string(labels_[std::size_t(fixup.label)]) + (longBranches_ ? " long" : "");
                outOfRange_ = true;
                return false;
            }
        }
        for (const LabelFixup& fixup : exitFixups_) {
            if (!a_.patchChecked(fixup.at, exitPads_[std::size_t(fixup.label)])) {
                why_ = "exit branch out of range: word " + std::to_string(a_.code()[fixup.at]) + (longBranches_ ? " long" : "");
                outOfRange_ = true;
                return false;
            }
        }
        if (a_.rangeLost()) {
            why_ = std::string("an out-of-line branch out of range") + (longBranches_ ? " long" : "");
            outOfRange_ = true;
            return false;
        }
        return true;
    }

    const std::vector<std::uint32_t>& code() const noexcept { return a_.code(); }
    std::vector<std::uint32_t>& mutableCode() noexcept { return a_.mutableCode(); }
    const std::vector<CallFixup>& callFixups() const noexcept { return callFixups_; }
    const std::vector<NativeCode::Site>& sites() const noexcept { return sites_; }
    /// By bytecode pc of the loop head, where in this code to enter.
    struct OsrOut
    {
        std::uint32_t pc;
        std::size_t at;
        std::vector<std::uint32_t> owned;
        std::vector<std::uint32_t> ownedPointers;
    };
    const std::vector<OsrOut>& osr() const noexcept { return osr_; }
    /// Where a delegate call of an instance lambda comes in, or -1.
    std::int64_t delegateThunk() const noexcept { return delegateThunk_; }

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
        std::int32_t site;  ///< index of the instruction, for what is live there
    };
    struct ColdPoll
    {
        std::size_t branch;
        std::int32_t exit;
        std::size_t back;
    };

    // ---- frame ----
    /// sp + 0: outgoing stack words, and where an exit parks what it releases. Then the spill slots, the saved registers,
    /// and at the top x29 and x30.
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
        for (std::uint32_t r = 19; r <= 28; ++r) {
            if ((fn_.usedIntRegs >> r) & 1u)
                savedInt_.push_back(Reg(r));
        }
        for (std::uint32_t r = 8; r <= 15; ++r) {
            if ((fn_.usedFpRegs >> r) & 1u)
                savedFp_.push_back(r);
        }
        at += std::uint32_t(savedInt_.size() + savedFp_.size()) * 8;
        at += 16;  // x29, x30
        frameBytes_ = (at + 15u) & ~15u;
        hasFrame_ = fn_.makesCalls || hasDecOrPoll || at > 16 || stackWords(fn_.params) > 0;
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
                if (ints < 8)
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
            a_.ldrX(X16, kRt, std::uint32_t(config_.stackLimit));
            a_.cmpSp(X16);
            overflowBranch_ = lengthen(a_.bcond(LS));
            needsOverflowPad_ = true;
        }
        a_.subLarge(Reg(31), Reg(31), frameBytes_);
        // `stp` reaches only 504 bytes.
        if (frameBytes_ - 16 <= 504) {
            a_.stpX(X29, X30, Reg(31), std::int32_t(frameBytes_ - 16));
        } else {
            a_.strX(X29, Reg(31), frameBytes_ - 16);
            a_.strX(X30, Reg(31), frameBytes_ - 8);
        }
        a_.addLarge(X29, Reg(31), frameBytes_ - 16);
        std::uint32_t at = saveBase_;
        for (std::size_t i = 0; i < savedInt_.size(); ++i) {
            if (i + 1 < savedInt_.size() && at <= 496) {
                a_.stpX(savedInt_[i], savedInt_[i + 1], Reg(31), std::int32_t(at));
                ++i;
                at += 16;
            } else {
                a_.strX(savedInt_[i], Reg(31), at);
                at += 8;
            }
        }
        for (std::uint32_t r : savedFp_) {
            a_.strD(r, Reg(31), at);
            at += 8;
        }
    }

    void emitEpilogue()
    {
        if (!hasFrame_) {
            a_.ret();
            return;
        }
        std::uint32_t at = saveBase_;
        for (std::size_t i = 0; i < savedInt_.size(); ++i) {
            if (i + 1 < savedInt_.size() && at <= 496) {
                a_.ldpX(savedInt_[i], savedInt_[i + 1], Reg(31), std::int32_t(at));
                ++i;
                at += 16;
            } else {
                a_.ldrX(savedInt_[i], Reg(31), at);
                at += 8;
            }
        }
        for (std::uint32_t r : savedFp_) {
            a_.ldrD(r, Reg(31), at);
            at += 8;
        }
        if (frameBytes_ - 16 <= 504) {
            a_.ldpX(X29, X30, Reg(31), std::int32_t(frameBytes_ - 16));
        } else {
            a_.ldrX(X29, Reg(31), frameBytes_ - 16);
            a_.ldrX(X30, Reg(31), frameBytes_ - 8);
        }
        a_.addLarge(Reg(31), Reg(31), frameBytes_);
        a_.ret();
    }

    // ---- moves ----
    struct Place
    {
        bool isMem = false;
        std::uint32_t reg = 0;  ///< the register, or the base of a memory place
        std::uint32_t offset = 0;
    };

    std::uint32_t spillOffset(std::int32_t location) const noexcept
    {
        return spillBase_ + std::uint32_t(-location - 1) * 8;
    }
    Place placeOf(std::int32_t vreg) const noexcept
    {
        const std::int32_t loc = fn_.location[std::size_t(vreg)];
        Place p;
        if (loc >= 0) {
            p.reg = std::uint32_t(loc);
        } else {
            p.isMem = true;
            p.reg = 31;
            p.offset = spillOffset(loc);
        }
        return p;
    }
    bool isUnused(std::int32_t vreg) const noexcept { return fn_.location[std::size_t(vreg)] == IrFunction::kUnused; }

    /// The register holding `v`, loading a spilled one into `scratch`.
    Reg use(std::int32_t v, Reg scratch)
    {
        const std::int32_t loc = fn_.location[std::size_t(v)];
        if (loc >= 0)
            return Reg(loc);
        a_.ldrX(scratch, Reg(31), spillOffset(loc));
        return scratch;
    }
    std::uint32_t useFp(std::int32_t v, std::uint32_t scratch)
    {
        const std::int32_t loc = fn_.location[std::size_t(v)];
        if (loc >= 0)
            return std::uint32_t(loc);
        a_.ldrD(scratch, Reg(31), spillOffset(loc));
        return scratch;
    }
    /// The register to compute `v` into; `commit` writes a spilled one back.
    Reg def(std::int32_t v, Reg scratch) const
    {
        const std::int32_t loc = fn_.location[std::size_t(v)];
        return loc >= 0 && loc != IrFunction::kUnused ? Reg(loc) : scratch;
    }
    std::uint32_t defFp(std::int32_t v, std::uint32_t scratch) const
    {
        const std::int32_t loc = fn_.location[std::size_t(v)];
        return loc >= 0 && loc != IrFunction::kUnused ? std::uint32_t(loc) : scratch;
    }
    void commit(std::int32_t v, Reg r)
    {
        const std::int32_t loc = fn_.location[std::size_t(v)];
        if (loc < 0)
            a_.strX(r, Reg(31), spillOffset(loc));
    }
    void commitFp(std::int32_t v, std::uint32_t r)
    {
        const std::int32_t loc = fn_.location[std::size_t(v)];
        if (loc < 0)
            a_.strD(r, Reg(31), spillOffset(loc));
    }

    struct Move
    {
        Place from;
        Place to;
    };

    /// Moves that happen at once, as at a call or a method's entry.
    void parallelMoves(std::vector<Move> moves, bool fp)
    {
        const auto movRegs = [&](std::uint32_t d, std::uint32_t s) {
            if (fp)
                a_.fmovD(d, s);
            else
                a_.movReg(Reg(d), Reg(s));
        };
        const auto load = [&](std::uint32_t d, const Place& m) {
            if (fp)
                a_.ldrD(d, Reg(m.reg), m.offset);
            else
                a_.ldrX(Reg(d), Reg(m.reg), m.offset);
        };
        const auto store = [&](std::uint32_t s, const Place& m) {
            if (fp)
                a_.strD(s, Reg(m.reg), m.offset);
            else
                a_.strX(Reg(s), Reg(m.reg), m.offset);
        };
        const std::uint32_t scratch = fp ? kFpScratchB : std::uint32_t(X16);

        // Memory first: it overlaps nothing, and reading registers now is reading them before they change.
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
        // Register to register: one whose destination nobody still reads can go; what is left are cycles.
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
            const bool isFp = fn_.vregIsFp[std::size_t(p)] != 0;
            Move m;
            std::uint32_t& counter = isFp ? nextFp : nextInt;
            if (counter < 8) {
                m.from.reg = counter++;
            } else {
                m.from.isMem = true;
                m.from.reg = X29;
                m.from.offset = 16 + 8 * stack++;
            }
            if (isUnused(p))
                continue;
            m.to = placeOf(p);
            (isFp ? fps : ints).push_back(m);
        }
        parallelMoves(std::move(ints), false);
        parallelMoves(std::move(fps), true);
        if (!fn_.captures.empty())
            emitCaptures();
    }

    void emitCaptures()
    {
        a_.ldrX(X16, kDelegateReg, std::uint32_t(kDelegateCapturedOffset));
        for (std::size_t i = 0; i < fn_.captures.size(); ++i) {
            const std::int32_t box = fn_.captures[i];
            if (isUnused(box))
                continue;
            const Reg d = def(box, X17);
            a_.ldrX(d, X16, std::uint32_t(i * 8));
            commit(box, d);
        }
    }

    void moveImm(Reg d, std::uint64_t value)
    {
        // Whichever of movz and movn takes fewer instructions.
        int zeros = 0, ones = 0;
        for (int shift = 0; shift < 64; shift += 16) {
            const std::uint32_t half = std::uint32_t((value >> shift) & 0xFFFFu);
            zeros += half == 0;
            ones += half == 0xFFFFu;
        }
        const bool inverted = ones > zeros;
        bool first = true;
        for (std::uint32_t shift = 0; shift < 4; ++shift) {
            const std::uint32_t half = std::uint32_t((value >> (shift * 16)) & 0xFFFFu);
            if (half == (inverted ? 0xFFFFu : 0u))
                continue;
            if (first) {
                const std::uint32_t imm = inverted ? (~half & 0xFFFFu) : half;
                a_.word((inverted ? 0x92800000u : 0xD2800000u) | (shift << 21) | (imm << 5) | d);
                first = false;
            } else {
                a_.word(0xF2800000u | (shift << 21) | (half << 5) | d);
            }
        }
        if (first)
            a_.word((inverted ? 0x92800000u : 0xD2800000u) | d);
    }

    /// An address with a displacement a load or store can encode, adding it into `scratch` when it cannot.
    Reg displaced(Reg base, std::int64_t& offset, std::uint32_t bytes, Reg scratch)
    {
        if (offset >= 0 && offset % bytes == 0 && offset / bytes < 4096)
            return base;
        moveImm(scratch, std::uint64_t(offset));
        a_.addX(scratch, base, scratch);
        offset = 0;
        return scratch;
    }

    // ---- exits ----
    /// In a function too big for a conditional branch to reach across, one just made becomes its inverse over an
    /// unconditional one, which reaches 128 MB: what is to be patched is then that.
    std::size_t lengthen(std::size_t at)
    {
        if (!longBranches_ || at + 1 != a_.here())
            return at;
        std::vector<std::uint32_t>& code = a_.mutableCode();
        std::uint32_t& op = code[at];
        const std::uint32_t top = op & 0xFC000000u;
        if (top == 0x14000000u || top == 0x94000000u)
            return at;  // b and bl reach far already
        if ((op & 0xFF000010u) == 0x54000000u) {
            if ((op & 0xFu) >= 14)
                return at;
            op = (op & 0xFF00001Fu) ^ 1u;  // the other condition
            op |= 2u << 5;
        } else if ((op & 0x7E000000u) == 0x34000000u) {
            op = ((op & 0xFF00001Fu) ^ 0x01000000u) | (2u << 5);  // cbz and cbnz
        } else if ((op & 0x7E000000u) == 0x36000000u) {
            op = ((op & 0xFFF8001Fu) ^ 0x01000000u) | (2u << 5);  // tbz and tbnz
        } else {
            return at;
        }
        return a_.b();
    }

    void branchToExit(std::size_t at, std::int32_t exit)
    {
        at = lengthen(at);
        exitFixups_.push_back({at, exit});
        usedExits_.resize(std::max(usedExits_.size(), std::size_t(exit) + 1), 0);
        usedExits_[std::size_t(exit)] = 1;
    }

    void branchToLabel(std::size_t at, std::int32_t label) { labelFixups_.push_back({lengthen(at), label}); }

    void callAbsolute(const void* target)
    {
        moveImm(X16, reinterpret_cast<std::uint64_t>(target));
        a_.blr(X16);
    }

    void noteSite(std::int32_t site)
    {
        sites_.push_back({std::uint32_t(a_.here() * 4), site});
    }

    /// Raise from a place with a frame already set up.
    void raiseHere(std::uint32_t code, std::int32_t site)
    {
        a_.strX(X29, kRt, std::uint32_t(config_.topFrame));
        a_.movReg(X0, kRt);
        moveImm(X1, code);
        callAbsolute(config_.raise);
        noteSite(site);
    }

    void emitCold()
    {
        for (const ColdDecRef& cold : coldDecRefs_) {
            a_.patch(cold.branch, a_.here());
            emitReleaseCall(cold);
            a_.patch(a_.b(), cold.back);
        }
        for (const ColdPoll& cold : coldPolls_) {
            a_.patch(cold.branch, a_.here());
            moveImm(X16, reinterpret_cast<std::uint64_t>(config_.poll));
            moveImm(X17, reinterpret_cast<std::uint64_t>(&scriptJitPollStub));
            a_.blr(X17);
            branchToExit(a_.cbnzX(X16), cold.exit);
            a_.patch(a_.b(), cold.back);
        }
        exitPads_.assign(fn_.exits.size(), 0);
        // An exit that raises nothing of its own -- a helper raised -- is the same code wherever what it releases is the
        // same, since a value is in one place for its whole life: one pad serves all of them.
        std::map<std::vector<std::int64_t>, std::size_t> shared;
        for (std::size_t i = 0; i < fn_.exits.size(); ++i) {
            if (i >= usedExits_.size() || !usedExits_[i])
                continue;
            const IrExit& exit = fn_.exits[i];
            if (exit.code == 0 && hasFrame_) {
                std::vector<std::int64_t> key;
                key.push_back(exit.handlerLabel);
                key.push_back(std::int64_t(exit.writeBacks.size()));
                for (const IrExit::WriteBack& back : exit.writeBacks) {
                    key.push_back(back.base);
                    key.push_back(back.offset);
                    key.push_back(back.vreg);
                    key.push_back(back.width);
                }
                key.push_back(-1);
                for (std::int32_t v : exit.cleanup)
                    key.push_back(isUnused(v) ? -2 : v);
                key.push_back(-1);
                for (const auto& tagged : exit.cleanupTagged) {
                    key.push_back(isUnused(tagged.first) || isUnused(tagged.second) ? -2 : tagged.first);
                    key.push_back(tagged.second);
                }
                auto found = shared.find(key);
                if (found != shared.end()) {
                    exitPads_[i] = found->second;
                    continue;
                }
                emitExitPad(i);
                shared.emplace(std::move(key), exitPads_[i]);
                continue;
            }
            emitExitPad(i);
        }
        if (needsOverflowPad_) {
            a_.patch(overflowBranch_, a_.here());
            emitFramelessRaise(std::uint32_t(ErrorCode::StackOverflow));
        }
    }

    /// Destroy a cell from a cold path, keeping exactly the caller-saved registers that are live here.
    void emitReleaseCall(const ColdDecRef& cold)
    {
        const IrFunction::LiveRegs& live = fn_.liveAtRelease[std::size_t(cold.site)];
        std::vector<std::uint32_t> ints, fps;
        for (std::uint32_t r = 0; r < 16; ++r) {
            if ((live.ints >> r) & 1u)
                ints.push_back(r);
        }
        for (std::uint32_t r = 0; r < 30; ++r) {
            if ((live.fps >> r) & 1u)
                fps.push_back(r);
        }
        const std::uint32_t bytes = std::uint32_t((ints.size() + fps.size() + 1) & ~std::size_t(1)) * 8;
        if (cold.cell != X0)
            a_.movReg(X16, cold.cell);
        if (bytes)
            a_.subImm(Reg(31), Reg(31), bytes, true);
        std::uint32_t at = 0;
        for (std::uint32_t r : ints) {
            a_.strX(Reg(r), Reg(31), at);
            at += 8;
        }
        for (std::uint32_t r : fps) {
            a_.strD(r, Reg(31), at);
            at += 8;
        }
        if (cold.cell != X0)
            a_.movReg(X0, X16);
        callAbsolute(reinterpret_cast<const void*>(&detail::destroyUnreferenced));
        at = 0;
        for (std::uint32_t r : ints) {
            a_.ldrX(Reg(r), Reg(31), at);
            at += 8;
        }
        for (std::uint32_t r : fps) {
            a_.ldrD(r, Reg(31), at);
            at += 8;
        }
        if (bytes)
            a_.addImm(Reg(31), Reg(31), bytes, true);
    }

    void emitFramelessRaise(std::uint32_t code)
    {
        a_.word(0xA9BF7BFDu);  // stp x29, x30, [sp, #-16]!
        a_.word(0x910003FDu);  // mov x29, sp
        a_.strX(X29, kRt, std::uint32_t(config_.topFrame));
        a_.movReg(X0, kRt);
        moveImm(X1, code);
        callAbsolute(config_.raise);
        noteSite(-1);
        a_.word(0xA8C17BFDu);  // ldp x29, x30, [sp], #16
        a_.ret();
    }

    void emitExitPad(std::size_t index)
    {
        const IrExit& exit = fn_.exits[index];
        exitPads_[index] = a_.here();
        if (!hasFrame_) {
            // A leaf with nothing to release and no frame of its own.
            emitFramelessRaise(exit.code);
            return;
        }
        for (const IrExit::WriteBack& back : exit.writeBacks) {
            const Reg base = use(back.base, X16);
            const std::size_t skip = a_.cbzX(base);
            std::int64_t offset = back.offset;
            const Reg address = displaced(base, offset, back.width, X16);
            if (fn_.vregIsFp[std::size_t(back.vreg)]) {
                const std::uint32_t v = useFp(back.vreg, kFpScratchA);
                if (back.width == 4)
                    a_.strS(v, address, std::uint32_t(offset));
                else
                    a_.strD(v, address, std::uint32_t(offset));
            } else {
                const Reg v = use(back.vreg, X17);
                a_.storeMem(v, address, std::uint32_t(offset), back.width);
            }
            a_.patch(skip, a_.here());
        }
        // Parked first: raising clobbers what is in a caller-saved register.
        std::uint32_t parked = 0;
        for (std::int32_t v : exit.cleanup) {
            if (isUnused(v))
                continue;
            const Reg r = use(v, X16);
            a_.strX(r, Reg(31), parked * 8);
            ++parked;
        }
        for (const auto& tagged : exit.cleanupTagged) {
            if (isUnused(tagged.first) || isUnused(tagged.second))
                continue;
            // Only a tag that says reference makes the payload a cell.
            const Reg t = use(tagged.second, X17);
            const Reg r = use(tagged.first, X16);
            a_.cmpImm(t, std::uint32_t(Type::String), false);
            a_.word(0x9A800000u | (XZR << 16) | (std::uint32_t(HS) << 12) | (r << 5) | X16);  // csel x16, r, xzr, hs
            a_.strX(X16, Reg(31), parked * 8);
            ++parked;
        }
        if (exit.indexVreg >= 0 && exit.code)
            a_.movW(X2, use(exit.indexVreg, X2));
        if (exit.code)
            raiseHere(exit.code, exit.site);
        if (parked) {
            a_.movSp(X0, Reg(31));
            moveImm(X1, parked);
            callAbsolute(reinterpret_cast<const void*>(&scriptJitCleanup));
        }
        if (exit.handlerLabel >= 0) {
            branchToLabel(a_.b(), exit.handlerLabel);
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
            // Enter with the frame made as if from the top; the interpreter's frame is in x0.
            const bool forcedFrame = !hasFrame_;
            if (forcedFrame) {
                // A leaf loop has no frame, and needs none here either.
            } else {
                emitPrologue(false);
            }
            {
                // What comes after the two tables, all at once, as a method's parameters do.
                std::vector<Move> args;
                std::uint32_t next = 2;
                for (std::int32_t v : fn_.osrArguments) {
                    if (!isUnused(v)) {
                        Move m;
                        m.from.reg = next;
                        m.to = placeOf(v);
                        args.push_back(m);
                    }
                    ++next;
                }
                // The tables stay reachable through scratch registers while that happens.
                a_.movReg(X17, X0);
                a_.movReg(X15, X1);
                parallelMoves(std::move(args), false);
            }
            for (const IrOsrEntry::Slot& slot : entry.pointers) {
                if (isUnused(slot.vreg))
                    continue;
                const Reg d = def(slot.vreg, X16);
                a_.ldrX(d, X15, slot.slot * 8);
                commit(slot.vreg, d);
                if (slot.owned)
                    out.ownedPointers.push_back(slot.slot);
            }
            for (std::int32_t zero : fn_.osrZero) {
                if (isUnused(zero))
                    continue;
                const Reg d = def(zero, X16);
                moveImm(d, 0);
                commit(zero, d);
            }
            for (const IrOsrEntry::Slot& slot : entry.slots) {
                if (isUnused(slot.vreg))
                    continue;
                const std::uint32_t offset = slot.slot * 16 + 8;
                if (fn_.vregIsFp[std::size_t(slot.vreg)]) {
                    const std::uint32_t d = defFp(slot.vreg, kFpScratchA);
                    if (slot.bytes == 4)
                        a_.ldrS(d, X17, offset);
                    else
                        a_.ldrD(d, X17, offset);
                    commitFp(slot.vreg, d);
                } else {
                    const Reg d = def(slot.vreg, X16);
                    if (slot.bytes == 4)
                        a_.ldrW(d, X17, offset);
                    else
                        a_.ldrX(d, X17, offset);
                    commit(slot.vreg, d);
                }
                if (slot.tagVreg >= 0 && !isUnused(slot.tagVreg)) {
                    const Reg d = def(slot.tagVreg, X16);
                    a_.loadMem(d, X17, slot.slot * 16, 1, false);
                    commit(slot.tagVreg, d);
                }
                if (slot.owned)
                    out.owned.push_back(slot.slot);
            }
            branchToLabel(a_.b(), entry.label);
            osr_.push_back(std::move(out));
        }
    }

    // ---- comparisons ----
    void compareImm(Reg a, std::int64_t imm, bool wide)
    {
        if (imm >= 0 && imm < 4096) {
            a_.cmpImm(a, std::uint32_t(imm), wide);
        } else if (imm < 0 && imm > -4096) {
            a_.cmnImm(a, std::uint32_t(-imm), wide);
        } else {
            moveImm(X17, std::uint64_t(imm));
            if (wide)
                a_.cmpX(a, X17);
            else
                a_.cmpW(a, X17);
        }
    }

    void compare(const IrInst& in)
    {
        const bool wide = in.width == 64;
        const Reg a = use(in.a, X16);
        if (in.hasImm) {
            compareImm(a, in.imm, wide);
        } else {
            const Reg b = use(in.b, X17);
            if (wide)
                a_.cmpX(a, b);
            else
                a_.cmpW(a, b);
        }
    }

    void compareFp(const IrInst& in)
    {
        const std::uint32_t a = useFp(in.a, kFpScratchA);
        const std::uint32_t b = useFp(in.b, kFpScratchB);
        if (in.width == 32)
            a_.fcmpS(a, b);
        else
            a_.fcmpD(a, b);
    }

    /// True when a comparison with zero can be `cbz` or `cbnz`.
    static bool isZeroTest(const IrInst& in) noexcept
    {
        return in.hasImm && in.imm == 0 && (in.cond == IrCond::Eq || in.cond == IrCond::Ne);
    }

    std::size_t zeroBranch(const IrInst& in)
    {
        const Reg a = use(in.a, X16);
        if (in.width == 64)
            return in.cond == IrCond::Eq ? a_.cbzX(a) : a_.cbnzX(a);
        return in.cond == IrCond::Eq ? a_.cbzW(a) : a_.cbnzW(a);
    }

    // ---- instructions ----
    void arithmetic(const IrInst& in)
    {
        const bool wide = in.width == 64;
        const Reg d = def(in.d, X17);
        const Reg a = use(in.a, X16);
        if (in.hasImm) {
            const std::int64_t imm = in.imm;
            switch (in.op) {
                case IrOp::Add:
                case IrOp::Sub: {
                    const bool add = (in.op == IrOp::Add) == (imm >= 0);
                    const std::uint64_t magnitude = imm >= 0 ? std::uint64_t(imm) : std::uint64_t(0) - std::uint64_t(imm);
                    if (magnitude < 4096) {
                        if (add)
                            a_.addImm(d, a, std::uint32_t(magnitude), wide);
                        else
                            a_.subImm(d, a, std::uint32_t(magnitude), wide);
                        commit(in.d, d);
                        return;
                    }
                    break;
                }
                case IrOp::And:
                case IrOp::Or:
                case IrOp::Xor: {
                    const std::int32_t encoded = encodeLogicalImm(std::uint64_t(imm), wide);
                    if (encoded >= 0) {
                        a_.logicalImm(in.op == IrOp::And ? 0u : in.op == IrOp::Or ? 1u : 2u, d, a,
                                      std::uint32_t(encoded), wide);
                        commit(in.d, d);
                        return;
                    }
                    break;
                }
                case IrOp::Shl:
                    a_.lslImm(d, a, std::uint32_t(imm) & (wide ? 63u : 31u), wide);
                    commit(in.d, d);
                    return;
                case IrOp::Shr:
                    a_.lsrImm(d, a, std::uint32_t(imm) & (wide ? 63u : 31u), wide);
                    commit(in.d, d);
                    return;
                case IrOp::Sar:
                    a_.asrImm(d, a, std::uint32_t(imm) & (wide ? 63u : 31u), wide);
                    commit(in.d, d);
                    return;
                default: break;
            }
            moveImm(X15, std::uint64_t(imm));
            threeReg(in.op, d, a, X15, wide);
            commit(in.d, d);
            return;
        }
        const Reg b = use(in.b, X15);
        threeReg(in.op, d, a, b, wide);
        commit(in.d, d);
    }

    void threeReg(IrOp op, Reg d, Reg a, Reg b, bool wide)
    {
        switch (op) {
            case IrOp::Add: wide ? a_.addX(d, a, b) : a_.addW(d, a, b); return;
            case IrOp::Sub: wide ? a_.subX(d, a, b) : a_.subW(d, a, b); return;
            case IrOp::Mul: wide ? a_.mulX(d, a, b) : a_.mulW(d, a, b); return;
            case IrOp::And: wide ? a_.andX(d, a, b) : a_.andW(d, a, b); return;
            case IrOp::Or: wide ? a_.orrX(d, a, b) : a_.orrW(d, a, b); return;
            case IrOp::Xor: wide ? a_.eorX(d, a, b) : a_.eorW(d, a, b); return;
            case IrOp::Shl: wide ? a_.lslX(d, a, b) : a_.lslW(d, a, b); return;
            case IrOp::Shr: wide ? a_.lsrX(d, a, b) : a_.lsrW(d, a, b); return;
            case IrOp::Sar: wide ? a_.asrX(d, a, b) : a_.asrW(d, a, b); return;
            case IrOp::SDiv: wide ? a_.sdivX(d, a, b) : a_.sdivW(d, a, b); return;
            case IrOp::UDiv: wide ? a_.udivX(d, a, b) : a_.udivW(d, a, b); return;
            default: return;
        }
    }

    /// A signed 32-bit division by a constant of at least 2, as a multiplication: Hacker's Delight, figure 10-1.
    void divideByConstant(Reg q, Reg a, std::int32_t divisor)
    {
        const std::uint32_t two31 = 0x80000000u;
        const std::uint32_t ad = std::uint32_t(divisor);
        const std::uint32_t anc = two31 - 1 - two31 % ad;
        int p = 31;
        std::uint32_t q1 = two31 / anc, r1 = two31 - q1 * anc;
        std::uint32_t q2 = two31 / ad, r2 = two31 - q2 * ad;
        std::uint32_t delta = 0;
        do {
            ++p;
            q1 *= 2;
            r1 *= 2;
            if (r1 >= anc) {
                ++q1;
                r1 -= anc;
            }
            q2 *= 2;
            r2 *= 2;
            if (r2 >= ad) {
                ++q2;
                r2 -= ad;
            }
            delta = ad - r2;
        } while (q1 < delta || (q1 == delta && r1 == 0));
        const std::int32_t magic = std::int32_t(q2 + 1);
        const std::uint32_t shift = std::uint32_t(p - 32);

        moveImm(X17, std::uint64_t(std::uint32_t(magic)));
        a_.word(0x9B207C00u | (X17 << 16) | (a << 5) | q);  // smull xq, wa, w17
        a_.asrImm(q, q, 32, true);
        if (magic < 0)
            a_.addW(q, q, a);
        if (shift)
            a_.asrImm(q, q, shift, false);
        a_.word(0x0B400000u | (q << 16) | (31u << 10) | (q << 5) | q);  // add wq, wq, wq, lsr #31
    }

    void divideOrRemainder(const IrInst& in)
    {
        const bool wide = in.width == 64;
        const bool isSigned = in.op == IrOp::SDiv || in.op == IrOp::SRem;
        const bool isRem = in.op == IrOp::SRem || in.op == IrOp::URem;
        if (in.hasImm && !wide && isSigned && in.imm >= 2 && in.imm <= 0x7FFFFFFF) {
            const Reg a = use(in.a, X15);
            const Reg d = def(in.d, X14);
            divideByConstant(X16, a, std::int32_t(in.imm));
            if (isRem) {
                moveImm(X17, std::uint64_t(in.imm));
                a_.msubW(d, X16, X17, a);
            } else {
                a_.movW(d, X16);
            }
            commit(in.d, d);
            return;
        }
        const Reg a = use(in.a, X15);
        Reg b = X14;
        if (in.hasImm)
            moveImm(X14, std::uint64_t(in.imm));
        else
            b = use(in.b, X14);
        const Reg d = def(in.d, X17);
        if (!isRem) {
            threeReg(isSigned ? IrOp::SDiv : IrOp::UDiv, d, a, b, wide);
        } else {
            threeReg(isSigned ? IrOp::SDiv : IrOp::UDiv, X16, a, b, wide);
            if (wide)
                a_.msubX(d, X16, b, a);
            else
                a_.msubW(d, X16, b, a);
        }
        commit(in.d, d);
    }

    void memory(const IrInst& in)
    {
        const std::uint32_t bytes = in.width;
        switch (in.op) {
            case IrOp::Load: {
                std::int64_t offset = in.imm;
                const Reg base = displaced(use(in.a, X16), offset, bytes, X17);
                if (fn_.vregIsFp[std::size_t(in.d)]) {
                    const std::uint32_t d = defFp(in.d, kFpScratchA);
                    if (bytes == 4)
                        a_.ldrS(d, base, std::uint32_t(offset));
                    else
                        a_.ldrD(d, base, std::uint32_t(offset));
                    commitFp(in.d, d);
                } else {
                    const Reg d = def(in.d, X15);
                    a_.loadMem(d, base, std::uint32_t(offset), bytes, in.isSigned);
                    commit(in.d, d);
                }
                return;
            }
            case IrOp::Store: {
                std::int64_t offset = in.imm;
                const Reg base = displaced(use(in.a, X16), offset, bytes, X17);
                if (fn_.vregIsFp[std::size_t(in.c)]) {
                    const std::uint32_t v = useFp(in.c, kFpScratchA);
                    if (bytes == 4)
                        a_.strS(v, base, std::uint32_t(offset));
                    else
                        a_.strD(v, base, std::uint32_t(offset));
                } else {
                    const Reg v = use(in.c, X15);
                    a_.storeMem(v, base, std::uint32_t(offset), bytes);
                }
                return;
            }
            case IrOp::LoadIdx: {
                Reg base = use(in.a, X16);
                const Reg index = use(in.b, X17);
                if (in.imm) {
                    a_.addImm(X16, base, std::uint32_t(in.imm), true);
                    base = X16;
                }
                const bool shifted = in.scale != 0;
                if (fn_.vregIsFp[std::size_t(in.d)]) {
                    const std::uint32_t d = defFp(in.d, kFpScratchA);
                    a_.fpLoadIdx(d, base, index, bytes, shifted);
                    commitFp(in.d, d);
                } else {
                    const Reg d = def(in.d, X15);
                    a_.loadIdx(d, base, index, bytes, in.isSigned, shifted);
                    commit(in.d, d);
                }
                return;
            }
            case IrOp::StoreIdx: {
                Reg base = use(in.a, X16);
                const Reg index = use(in.b, X17);
                if (in.imm) {
                    a_.addImm(X16, base, std::uint32_t(in.imm), true);
                    base = X16;
                }
                const bool shifted = in.scale != 0;
                if (fn_.vregIsFp[std::size_t(in.c)]) {
                    const std::uint32_t v = useFp(in.c, kFpScratchA);
                    a_.fpStoreIdx(v, base, index, bytes, shifted);
                } else {
                    const Reg v = use(in.c, X15);
                    a_.storeIdx(v, base, index, bytes, shifted);
                }
                return;
            }
            default: return;
        }
    }

    /// A cold call keeps the live caller-saved registers itself, in words of the frame set aside for it.
    void saveLive(const IrFunction::LiveRegs& live, bool restore)
    {
        std::uint32_t at = coldSaveBase_;
        for (std::uint32_t r = 0; r < 16; ++r) {
            if (!((live.ints >> r) & 1u))
                continue;
            if (restore)
                a_.ldrX(Reg(r), Reg(31), at);
            else
                a_.strX(Reg(r), Reg(31), at);
            at += 8;
        }
        for (std::uint32_t r = 0; r < 30; ++r) {
            if (!((live.fps >> r) & 1u))
                continue;
            if (restore)
                a_.ldrD(r, Reg(31), at);
            else
                a_.strD(r, Reg(31), at);
            at += 8;
        }
    }

    void emitCall(const IrCall& call)
    {
        if (call.publishFrame && (call.kind == IrCallKind::Helper || call.kind == IrCallKind::Indirect))
            a_.strX(X29, kRt, std::uint32_t(config_.topFrame));
        if (call.cold)
            saveLive(fn_.liveAtRelease[std::size_t(call.liveIndex)], false);

        // Arguments go where the convention says, all at once.
        std::vector<Move> ints, fps;
        std::uint32_t nextInt = 0, nextFp = 0, stack = 0;
        for (std::int32_t arg : call.passed()) {
            const bool isFp = fn_.vregIsFp[std::size_t(arg)] != 0;
            Move m;
            m.from = placeOf(arg);
            std::uint32_t& counter = isFp ? nextFp : nextInt;
            if (counter < 8) {
                m.to.reg = counter++;
            } else {
                m.to.isMem = true;
                m.to.reg = 31;
                m.to.offset = 8 * stack++;
            }
            (isFp ? fps : ints).push_back(m);
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
                    callFixups_.push_back({a_.bl(), call.target});
                } else {
                    moveImm(kMethodReg, reinterpret_cast<std::uint64_t>(call.target));
                    a_.ldrX(X16, kMethodReg, std::uint32_t(kMethodFastEntryOffset));
                    a_.blr(X16);
                }
                break;
            case IrCallKind::Virtual:
                a_.ldrX(X16, X0, std::uint32_t(kInstanceTypeOffset));
                a_.ldrX(X16, X16, std::uint32_t(kTypeVtableOffset));
                a_.ldrX(kMethodReg, X16, std::uint32_t(call.target->vtableSlot) * 8);
                a_.ldrX(X16, kMethodReg, std::uint32_t(kMethodFastEntryOffset));
                a_.blr(X16);
                break;
            case IrCallKind::Interface:
                a_.ldrX(X16, kMethodReg, std::uint32_t(kMethodFastEntryOffset));
                a_.blr(X16);
                break;
            case IrCallKind::Indirect:
                a_.blr(kMethodReg);
                break;
            case IrCallKind::Delegate: {
                a_.ldrX(X16, kDelegateReg, std::uint32_t(kDelegateNextOffset));
                const std::size_t multicast = a_.cbnzX(X16);
                a_.ldrX(kMethodReg, kDelegateReg, std::uint32_t(kDelegateMethodOffset));
                a_.ldrX(X16, kMethodReg, std::uint32_t(kMethodDelegateEntryOffset));
                const std::size_t go = a_.b();
                a_.patch(multicast, a_.here());
                moveImm(X16, reinterpret_cast<std::uint64_t>(&scriptJitLazyDelegateStub));
                a_.patch(go, a_.here());
                a_.blr(X16);
                break;
            }
        }
        if (call.site >= 0)
            noteSite(call.site);
        if (call.cold) {
            // The results wait in scratch registers while the live ones come back, which may well include x0.
            a_.movReg(X14, X0);
            a_.movReg(X15, X1);
            a_.fmovD(kFpScratchA, 0);
            saveLive(fn_.liveAtRelease[std::size_t(call.liveIndex)], true);
            if (call.exit >= 0) {
                a_.ldrW(X16, kRt, std::uint32_t(config_.nativeError));
                branchToExit(a_.cbnzW(X16), call.exit);
            }
            if (call.resultTag >= 0 && !isUnused(call.resultTag)) {
                const Reg d = def(call.resultTag, X15);
                if (d != X15)
                    a_.movReg(d, X15);
                commit(call.resultTag, d);
            }
            if (call.result >= 0 && !isUnused(call.result)) {
                if (fn_.vregIsFp[std::size_t(call.result)]) {
                    const std::uint32_t d = defFp(call.result, kFpScratchA);
                    if (d != kFpScratchA)
                        a_.fmovD(d, kFpScratchA);
                    commitFp(call.result, d);
                } else {
                    const Reg d = def(call.result, X14);
                    if (d != X14)
                        a_.movReg(d, X14);
                    commit(call.result, d);
                }
            }
            return;
        }
        if (call.exit >= 0) {
            a_.ldrW(X16, kRt, std::uint32_t(config_.nativeError));
            branchToExit(a_.cbnzW(X16), call.exit);
        }
        if (call.resultTag >= 0) {
            std::vector<Move> results;
            if (!isUnused(call.result)) {
                Move m;
                m.from.reg = X0;
                m.to = placeOf(call.result);
                results.push_back(m);
            }
            if (!isUnused(call.resultTag)) {
                Move m;
                m.from.reg = X1;
                m.to = placeOf(call.resultTag);
                results.push_back(m);
            }
            parallelMoves(std::move(results), false);
        } else if (call.result >= 0 && !isUnused(call.result)) {
            if (fn_.vregIsFp[std::size_t(call.result)]) {
                const Place to = placeOf(call.result);
                if (to.isMem)
                    a_.strD(0, Reg(31), to.offset);
                else if (to.reg != 0)
                    a_.fmovD(to.reg, 0);
            } else {
                const Place to = placeOf(call.result);
                if (to.isMem)
                    a_.strX(X0, Reg(31), to.offset);
                else if (to.reg != 0)
                    a_.movReg(Reg(to.reg), X0);
            }
        }
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
                if (fn_.vregIsFp[std::size_t(in.d)]) {
                    const std::uint32_t d = defFp(in.d, kFpScratchA);
                    const std::uint32_t a = useFp(in.a, d);
                    if (d != a)
                        a_.fmovD(d, a);
                    commitFp(in.d, d);
                } else {
                    const Reg d = def(in.d, X16);
                    const Reg a = use(in.a, d);
                    if (d != a)
                        a_.movReg(d, a);
                    commit(in.d, d);
                }
                return;
            case IrOp::MovImm: {
                if (isUnused(in.d))
                    return;
                const Reg d = def(in.d, X16);
                moveImm(d, std::uint64_t(in.imm));
                commit(in.d, d);
                return;
            }
            case IrOp::MovRuntime: {
                if (isUnused(in.d))
                    return;
                const Reg d = def(in.d, X16);
                a_.movReg(d, kRt);
                commit(in.d, d);
                return;
            }
            case IrOp::FMovImm: {
                if (isUnused(in.d))
                    return;
                const std::uint32_t d = defFp(in.d, kFpScratchA);
                const std::int32_t small = fmovImm8(std::uint64_t(in.imm), in.width == 64);
                if (in.imm == 0) {
                    a_.fmovDX(d, XZR);
                } else if (small >= 0) {
                    a_.word((in.width == 64 ? 0x1E601000u : 0x1E201000u) | (std::uint32_t(small) << 13) | d);
                } else if (in.width == 32) {
                    moveImm(X16, std::uint64_t(std::uint32_t(in.imm)));
                    a_.fmovSW(d, X16);
                } else {
                    moveImm(X16, std::uint64_t(in.imm));
                    a_.fmovDX(d, X16);
                }
                commitFp(in.d, d);
                return;
            }
            case IrOp::BitsToFp: {
                const std::uint32_t d = defFp(in.d, kFpScratchA);
                a_.fmovDX(d, use(in.a, X16));
                commitFp(in.d, d);
                return;
            }
            case IrOp::FpToBits: {
                const Reg d = def(in.d, X16);
                const std::uint32_t a = useFp(in.a, kFpScratchA);
                if (in.width == 32)
                    a_.fmovWS(d, a);
                else
                    a_.fmovXD(d, a);
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
            case IrOp::Neg: {
                const Reg d = def(in.d, X16);
                const Reg a = use(in.a, X17);
                in.width == 64 ? a_.negX(d, a) : a_.negW(d, a);
                commit(in.d, d);
                return;
            }
            case IrOp::Not: {
                const Reg d = def(in.d, X16);
                const Reg a = use(in.a, X17);
                in.width == 64 ? a_.mvnX(d, a) : a_.mvnW(d, a);
                commit(in.d, d);
                return;
            }
            case IrOp::Sext8:
            case IrOp::Sext16:
            case IrOp::Sext32:
            case IrOp::Zext8:
            case IrOp::Zext16:
            case IrOp::Zext32: {
                const Reg d = def(in.d, X16);
                const Reg a = use(in.a, X17);
                switch (in.op) {
                    case IrOp::Sext8: a_.sxtb(d, a); break;
                    case IrOp::Sext16: a_.sxth(d, a); break;
                    case IrOp::Sext32: a_.sxtw(d, a); break;
                    case IrOp::Zext8: a_.uxtb(d, a); break;
                    case IrOp::Zext16: a_.uxth(d, a); break;
                    default: a_.uxtw(d, a); break;
                }
                commit(in.d, d);
                return;
            }
            case IrOp::FAdd:
            case IrOp::FSub:
            case IrOp::FMul:
            case IrOp::FDiv: {
                const std::uint32_t a = useFp(in.a, kFpScratchA);
                const std::uint32_t b = useFp(in.b, kFpScratchB);
                const std::uint32_t d = defFp(in.d, kFpScratchA);
                const bool isDouble = in.width == 64;
                switch (in.op) {
                    case IrOp::FAdd: isDouble ? a_.faddD(d, a, b) : a_.faddS(d, a, b); break;
                    case IrOp::FSub: isDouble ? a_.fsubD(d, a, b) : a_.fsubS(d, a, b); break;
                    case IrOp::FMul: isDouble ? a_.fmulD(d, a, b) : a_.fmulS(d, a, b); break;
                    default: isDouble ? a_.fdivD(d, a, b) : a_.fdivS(d, a, b); break;
                }
                commitFp(in.d, d);
                return;
            }
            case IrOp::FNeg:
            case IrOp::FSqrt:
            case IrOp::FAbs:
            case IrOp::FFloor:
            case IrOp::FCeil:
            case IrOp::FTrunc:
            case IrOp::FRoundEven: {
                const std::uint32_t a = useFp(in.a, kFpScratchA);
                const std::uint32_t d = defFp(in.d, kFpScratchA);
                switch (in.op) {
                    case IrOp::FNeg: in.width == 64 ? a_.fnegD(d, a) : a_.fnegS(d, a); break;
                    case IrOp::FSqrt: a_.fsqrtD(d, a); break;
                    case IrOp::FAbs: a_.fabsD(d, a); break;
                    case IrOp::FFloor: a_.frintmD(d, a); break;
                    case IrOp::FCeil: a_.frintpD(d, a); break;
                    case IrOp::FTrunc: a_.frintzD(d, a); break;
                    default: a_.frintnD(d, a); break;
                }
                commitFp(in.d, d);
                return;
            }
            case IrOp::FConvert: {
                const std::uint32_t a = useFp(in.a, kFpScratchA);
                const std::uint32_t d = defFp(in.d, kFpScratchA);
                if (in.width == 64)
                    a_.fcvtDS(d, a);
                else
                    a_.fcvtSD(d, a);
                commitFp(in.d, d);
                return;
            }
            case IrOp::IntToFp: {
                const Reg a = use(in.a, X16);
                const std::uint32_t d = defFp(in.d, kFpScratchA);
                const bool wideSource = in.srcWidth == 64;
                if (in.width == 64) {
                    if (in.isUnsigned)
                        wideSource ? a_.ucvtfDX(d, a) : a_.ucvtfDW(d, a);
                    else
                        wideSource ? a_.scvtfDX(d, a) : a_.scvtfDW(d, a);
                } else if (in.isUnsigned) {
                    wideSource ? a_.ucvtfSX(d, a) : a_.ucvtfSW(d, a);
                } else {
                    wideSource ? a_.scvtfSX(d, a) : a_.scvtfSW(d, a);
                }
                commitFp(in.d, d);
                return;
            }
            case IrOp::FpToInt: {
                // What the interpreter does: NaN and both infinities give zero, finite values saturate.
                const std::uint32_t a = useFp(in.a, kFpScratchA);
                const Reg d = def(in.d, X15);
                if (in.srcWidth == 64) {
                    if (in.isUnsigned)
                        a_.fcvtzuXD(d, a);
                    else
                        a_.fcvtzsXD(d, a);
                    a_.fmovXD(X16, a);
                    a_.lsl1X(X16, X16);
                    moveImm(X17, 0xFFE0000000000000ull);
                    a_.cmpX(X16, X17);
                } else {
                    a_.fcvtzsXS(d, a);
                    a_.fmovWS(X16, a);
                    a_.lsl1W(X16, X16);
                    moveImm(X17, 0xFF000000u);
                    a_.cmpW(X16, X17);
                }
                a_.cselX(d, XZR, d, EQ);
                commit(in.d, d);
                return;
            }
            case IrOp::Cset: {
                compare(in);
                const Reg d = def(in.d, X16);
                a_.csetW(d, condOf(in.cond));
                commit(in.d, d);
                return;
            }
            case IrOp::FCset: {
                compareFp(in);
                const Reg d = def(in.d, X16);
                a_.csetW(d, condOf(in.cond));
                commit(in.d, d);
                return;
            }
            case IrOp::Br:
                branchToLabel(a_.b(), std::int32_t(in.imm));
                return;
            case IrOp::BrCmp:
                if (isZeroTest(in)) {
                    branchToLabel(zeroBranch(in), in.target);
                    return;
                }
                compare(in);
                branchToLabel(a_.bcond(condOf(in.cond)), in.target);
                return;
            case IrOp::BrFCmp:
                compareFp(in);
                branchToLabel(a_.bcond(condOf(in.cond)), in.target);
                return;
            case IrOp::ExitCmp:
                if (isZeroTest(in)) {
                    branchToExit(zeroBranch(in), in.target);
                    return;
                }
                compare(in);
                branchToExit(a_.bcond(condOf(in.cond)), in.target);
                return;
            case IrOp::Load:
            case IrOp::LoadIdx:
            case IrOp::Store:
            case IrOp::StoreIdx: memory(in); return;
            case IrOp::StoreTag: {
                const Reg base = use(in.a, X15);
                moveImm(X16, std::uint64_t(std::uint32_t(in.tag)));
                if (in.c >= 0) {
                    // Whether the reference is null decides between the tag and Null: `csinc` makes the 1 that Null is.
                    static_assert(std::uint32_t(Type::Null) == 1, "csinc below");
                    const Reg c = use(in.c, X14);
                    a_.cmpImm(c, 0, true);
                    a_.word(0x1A9F0400u | (X16 << 5) | (std::uint32_t(NE) << 12) | X16);  // csinc w16, w16, wzr, ne
                }
                a_.strB(X16, base, std::uint32_t(in.imm));
                return;
            }
            case IrOp::IncRef: {
                const Reg a = use(in.a, X17);
                std::size_t skip = 0;
                if (in.isNullable)
                    skip = a_.cbzX(a);
                a_.ldrW(X16, a, std::uint32_t(kCellRefCountOffset));
                a_.addImm(X16, X16, 1, false);
                a_.strW(X16, a, std::uint32_t(kCellRefCountOffset));
                if (in.isNullable)
                    a_.patch(skip, a_.here());
                return;
            }
            case IrOp::DecRef: {
                const Reg a = use(in.a, X17);
                std::size_t skip = 0;
                if (in.isNullable)
                    skip = a_.cbzX(a);
                a_.ldrW(X16, a, std::uint32_t(kCellRefCountOffset));
                a_.subsImm(X16, X16, 1, false);
                a_.strW(X16, a, std::uint32_t(kCellRefCountOffset));
                const std::size_t cold = lengthen(a_.bcond(EQ));
                // A spilled cell was loaded into x17, which the cold path overwrites only after copying it.
                coldDecRefs_.push_back({cold, a, a_.here(), std::int32_t(in.target)});
                if (in.isNullable)
                    a_.patch(skip, a_.here());
                return;
            }
            case IrOp::IncRefTagged: {
                const Reg t = use(in.b, X16);
                a_.cmpImm(t, std::uint32_t(Type::String), false);
                const std::size_t notCell = a_.bcond(LO);
                const Reg a = use(in.a, X17);
                std::size_t skip = 0;
                if (in.isNullable)
                    skip = a_.cbzX(a);
                a_.ldrW(X16, a, std::uint32_t(kCellRefCountOffset));
                a_.addImm(X16, X16, 1, false);
                a_.strW(X16, a, std::uint32_t(kCellRefCountOffset));
                if (in.isNullable)
                    a_.patch(skip, a_.here());
                a_.patch(notCell, a_.here());
                return;
            }
            case IrOp::DecRefTagged: {
                const Reg t = use(in.b, X16);
                a_.cmpImm(t, std::uint32_t(Type::String), false);
                const std::size_t notCell = a_.bcond(LO);
                const Reg a = use(in.a, X17);
                std::size_t skip = 0;
                if (in.isNullable)
                    skip = a_.cbzX(a);
                a_.ldrW(X16, a, std::uint32_t(kCellRefCountOffset));
                a_.subsImm(X16, X16, 1, false);
                a_.strW(X16, a, std::uint32_t(kCellRefCountOffset));
                const std::size_t cold = lengthen(a_.bcond(EQ));
                coldDecRefs_.push_back({cold, a, a_.here(), std::int32_t(in.target)});
                if (in.isNullable)
                    a_.patch(skip, a_.here());
                a_.patch(notCell, a_.here());
                return;
            }
            case IrOp::TagOfNullable: {
                const Reg d = def(in.d, X16);
                const Reg a = use(in.a, X17);
                moveImm(X15, std::uint64_t(std::uint32_t(in.tag)));
                a_.cmpImm(a, 0, true);
                static_assert(std::uint32_t(Type::Null) == 1, "csinc below");
                a_.word(0x1A9F0400u | (X15 << 5) | (std::uint32_t(NE) << 12) | d);  // csinc wd, w15, wzr, ne
                commit(in.d, d);
                return;
            }
            case IrOp::Poll: {
                a_.ldrW(X16, kRt, std::uint32_t(config_.pollFlag));
                const std::size_t cold = lengthen(a_.cbnzW(X16));
                coldPolls_.push_back({cold, in.target, a_.here()});
                return;
            }
            case IrOp::Call: emitCall(fn_.calls[std::size_t(in.imm)]); return;
            case IrOp::Ret:
                if (in.b >= 0) {
                    std::vector<Move> results;
                    Move payload;
                    payload.from = placeOf(in.a);
                    payload.to.reg = X0;
                    results.push_back(payload);
                    Move tag;
                    tag.from = placeOf(in.b);
                    tag.to.reg = X1;
                    results.push_back(tag);
                    parallelMoves(std::move(results), false);
                } else if (in.a >= 0) {
                    if (fn_.vregIsFp[std::size_t(in.a)]) {
                        const std::uint32_t r = useFp(in.a, 0);
                        if (r != 0)
                            a_.fmovD(0, r);
                    } else {
                        const Reg r = use(in.a, X0);
                        if (r != X0)
                            a_.movReg(X0, r);
                    }
                }
                emitEpilogue();
                return;
            case IrOp::Trap: a_.brk(); return;
            case IrOp::FrameAddr: {
                const Reg d = def(in.d, X16);
                a_.addLarge(d, Reg(31), areaBase_ + std::uint32_t(in.imm));
                commit(in.d, d);
                return;
            }
            case IrOp::SaveRegs:
                // A branch and link to the one place that does it: x30 is in the frame already.
                saverWanted_ = true;
                a_.word(0x94000000u);
                labelFixups_.push_back({a_.here() - 1, std::int32_t(fn_.labelCount)});
                return;
            case IrOp::FpToInt32Js: {
                const std::uint32_t a = useFp(in.a, kFpScratchA);
                const Reg d = def(in.d, X16);
                a_.fjcvtzs(d, a);
                commit(in.d, d);
                return;
            }
            case IrOp::FpToIntRaw: {
                const std::uint32_t a = useFp(in.a, kFpScratchA);
                const Reg d = def(in.d, X16);
                a_.fcvtzsXD(d, a);
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
    std::vector<std::uint8_t> usedExits_;
    std::vector<std::size_t> exitPads_;
    std::vector<Reg> savedInt_;
    std::vector<std::uint32_t> savedFp_;
    std::uint32_t areaBase_ = 0;
    std::uint32_t spillBase_ = 0;
    std::uint32_t coldSaveBase_ = 0;
    std::uint32_t saveBase_ = 0;
    std::uint32_t frameBytes_ = 0;
    bool longBranches_ = false;
    bool outOfRange_ = false;

public:
    void useLongBranches() noexcept { longBranches_ = true; }
    bool outOfRange() const noexcept { return outOfRange_; }
    std::string why_ = "out of branch range";

private:
    bool hasFrame_ = false;
    bool needsOverflowPad_ = false;
    std::size_t overflowBranch_ = 0;
    std::int64_t delegateThunk_ = -1;
};

template <class T>
std::int32_t offsetInRuntime(const Runtime& rt, const T& field) noexcept
{
    return std::int32_t(reinterpret_cast<std::uintptr_t>(&field) - reinterpret_cast<std::uintptr_t>(&rt));
}

class Arm64Backend : public JitBackend
{
public:
    Arm64Backend() : layout_(valueLayout())
    {
        regs_.intCallerSaved = {9, 10, 11, 12, 13, 8, 7, 6, 5, 4, 3, 2, 1, 0};
        regs_.intCalleeSaved = {19, 21, 22, 23, 24, 25, 26, 27, 28};
        regs_.fpCallerSaved = {16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 7, 6, 5, 4, 3, 2, 1, 0};
        regs_.fpCalleeSaved = {8, 9, 10, 11, 12, 13, 14, 15};
        regs_.intArgs = {0, 1, 2, 3, 4, 5, 6, 7};
        regs_.fpArgs = {0, 1, 2, 3, 4, 5, 6, 7};
        regs_.intResult = 0;
    }

    const char* name() const noexcept override { return "arm64"; }

    bool hasJsToInt32() const noexcept override
    {
#if defined(__linux__)
        static const bool has = (getauxval(AT_HWCAP) & HWCAP_JSCVT) != 0;
        return has;
#else
        return false;
#endif
    }
    bool available() const noexcept override
    {
#if SCRIPT_ENABLE_JIT
        return layout_.isExpected();
#else
        return false;
#endif
    }

    bool supports(const MethodInfo&, std::string* reason) const override
    {
        if (available())
            return true;
        if (reason)
            *reason = "the arm64 back end is not enabled in this build";
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

        // The method and everything it calls directly, so those calls can be plain branches.
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
#if defined(SCRIPT_JIT_ENV_OVERRIDE)
                if (const char* log = std::getenv("SCRIPT_JIT_LOG")) {
                    if (std::FILE* out = std::fopen(log, "a")) {
                        std::fprintf(out, "%s\n", why.c_str());
                        std::fclose(out);
                    }
                }
#endif
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
        if (config.nativeError > 16380 || config.pollFlag > 16380 || config.stackLimit > 32000 || config.topFrame > 32000)
            return nullptr;

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
            unit->bytes = unit->emitter->code().size() * sizeof(std::uint32_t);
            // The thunk a delegate comes in by, for a lambda that has a receiver.
            total += (unit->bytes + 15u) & ~std::size_t(15u);
        }

        void* block = cache_.reserve(total);
        if (!block)
            return nullptr;
        auto* base = static_cast<std::uint8_t*>(block);

        // Link: every call between compiled methods is a direct branch.
        for (auto& unit : units) {
            std::vector<std::uint32_t>& code = unit->emitter->mutableCode();
            for (const CallFixup& fixup : unit->emitter->callFixups()) {
                const std::uint8_t* target = nullptr;
                const auto it = session.find(fixup.target);
                if (it != session.end()) {
                    target = base + units[it->second]->at;
                } else if (NativeCode* compiled = fixup.target->code.load(std::memory_order_acquire)) {
                    target = static_cast<const std::uint8_t*>(compiled->fastEntry);
                }
                const std::uint8_t* from = base + unit->at + fixup.at * 4;
                const std::int64_t delta = target ? (target - from) / 4 : (std::int64_t(1) << 40);
                if (delta < -(std::int64_t(1) << 25) || delta >= (std::int64_t(1) << 25)) {
                    cache_.release(block, total);
                    return nullptr;
                }
                code[fixup.at] = 0x94000000u | (std::uint32_t(delta) & 0x03FFFFFFu);
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
            code->backend = "arm64";
            code->fastEntry = code->codeStart;
            for (const Emitter::OsrOut& osr : unit->emitter->osr()) {
                NativeCode::Osr out;
                out.pc = osr.pc;
                out.entry = base + unit->at + osr.at * 4;
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

    NativeCode* compileFunction(IrFunction& fn, const NativeConfig& config) override
    {
#if defined(SCRIPT_JIT_ENV_OVERRIDE)
        if (const char* path = std::getenv("SCRIPT_JIT_IR_BEFORE")) {
            if (std::FILE* out = std::fopen(path, "a")) {
                const std::string text = dumpIr(fn);
                std::fwrite(text.data(), 1, text.size(), out);
                std::fclose(out);
            }
        }
#endif
        optimizeLoops(fn);
        allocateRegisters(fn, regs_);
#if defined(SCRIPT_JIT_ENV_OVERRIDE)
        if (const char* path = std::getenv("SCRIPT_JIT_IR")) {
            if (std::FILE* out = std::fopen(path, "a")) {
                const std::string text = dumpIr(fn);
                std::fwrite(text.data(), 1, text.size(), out);
                std::fclose(out);
            }
        }
#endif
        static const std::unordered_map<const MethodInfo*, std::size_t> kNoSession;
        Emitter first(fn, config, kNoSession);
        std::unique_ptr<Emitter> again;
        Emitter* chosen = &first;
        if (!first.run()) {
            refusal_ = first.why_;
            if (!first.outOfRange())
                return nullptr;
            again = std::make_unique<Emitter>(fn, config, kNoSession);
            again->useLongBranches();
            if (!again->run()) {
                refusal_ = again->why_;
                return nullptr;
            }
            chosen = again.get();
        }
        Emitter& emitter = *chosen;
        const std::size_t bytes = emitter.code().size() * sizeof(std::uint32_t);
        void* block = cache_.reserve(bytes);
        if (!block) {
            refusal_ = "the code cache has no room for " + std::to_string(bytes) + " bytes";
            return nullptr;
        }
        std::memcpy(block, emitter.code().data(), bytes);
        if (!cache_.commit(block, bytes)) {
            cache_.release(block, bytes);
            return nullptr;
        }
        auto* code = new NativeCode();
        code->codeStart = block;
        code->codeBytes = bytes;
        code->backend = "arm64";
        code->fastEntry = block;
        for (const Emitter::OsrOut& osr : emitter.osr()) {
            NativeCode::Osr out;
            out.pc = osr.pc;
            out.entry = static_cast<std::uint8_t*>(block) + osr.at * 4;
            out.ownedSlots = osr.owned;
            out.ownedPointers = osr.ownedPointers;
            code->osr.push_back(std::move(out));
        }
        code->sites = emitter.sites();
        for (const IrSite& site : fn.sites)
            code->sourceSites.push_back({site.method, site.pc, site.parent});
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
    RegisterSet regs_;
    std::string refusal_;
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
    NativeCode* compile(Runtime&, MethodInfo&, std::vector<NativeCode*>&) override { return nullptr; }
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
