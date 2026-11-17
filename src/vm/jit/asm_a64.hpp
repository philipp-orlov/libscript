// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The vm core's macro assembler for arm64. It has the interface of asm_x64.hpp, operation for operation, so the
// templates of both compilers are written once: a register name of that interface stands for a fixed arm64 register, an
// operation with a memory operand becomes a load, the operation and a store, and a conditional jump is encoded from the
// condition asked for and the kind of instruction that last set the flags.
//
// Registers. The argument registers of the two calling conventions line up, so a call into the runtime needs no moves
// before it; its result is moved after it (`callC`):
//   rdi x0   rsi x1   rdx x2   rcx x3   r8 x4   r9 x5   rax x8   r10 x9   r11 x10         (not kept by a call)
//   rbx x19  rbp x20  r12 x21  r13 x22  r14 x23  r15 x24                                  (kept by a call)
//   xmm0-7 v0-v7, xmm8-15 v16-v23 (none kept by a call, as the templates expect)
//   x14-x17 and v31 are the assembler's own, within one operation.
//
// The stack. The templates push single 8-byte values, which arm64 does not allow through sp (it must be 16-byte aligned
// whenever it is the base of an access). The stack pointer the templates see is x28, and sp is moved with it, so nothing
// below it is live when a signal arrives; sp itself is never the base of an access. A call keeps the caller's return
// address (x30) in the 8-byte slot where x86-64 keeps the callee's, so the layout and the alignment at every call are
// what the templates were written for, and returns are predicted as returns. Around a call into the runtime the address
// waits in x27 instead.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace script {
namespace internal {
namespace vm {
namespace jit {

enum Reg : std::uint8_t
{
    RAX = 0, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8, R9, R10, R11, R12, R13, R14, R15
};

enum Cond : std::uint8_t
{
    CO = 0, CNO = 1, CB = 2, CAE = 3, CE = 4, CNE = 5, CBE = 6, CA = 7, CS = 8, CNS = 9, CP = 10, CNP = 11, CL = 12, CGE = 13,
    CLE = 14, CG = 15
};

/// An address: `base + index * scale + disp`.
struct Mem
{
    Reg base;
    std::int32_t disp = 0;
    int index = -1;  ///< a Reg, or -1 for none
    std::uint8_t scale = 1;
};
inline Mem mem(Reg base, std::int32_t disp = 0) { return Mem{base, disp, -1, 1}; }
inline Mem mem(Reg base, Reg index, std::uint8_t scale, std::int32_t disp = 0) { return Mem{base, disp, index, scale}; }

struct Label
{
    std::int32_t pos = -1;           ///< where it is bound, or -1
    std::vector<std::int32_t> uses;  ///< offsets of the branch instructions waiting for it
    bool bound() const noexcept { return pos >= 0; }
};

class A64
{
public:
    std::vector<std::uint8_t> buf;
    /// A conditional branch reaches a megabyte. With this set, one to a label not yet bound is a branch over a jump.
    bool longBranches = longBranchDefault();
    /// What `longBranches` starts as for an assembler made on this thread, and whether one made on it found a branch
    /// out of reach: the caller compiles again with the first set.
    static bool& longBranchDefault() noexcept
    {
        static thread_local bool on = false;
        return on;
    }
    static bool& reachFailed() noexcept
    {
        static thread_local bool on = false;
        return on;
    }

    /// Whether the code cannot be used: a branch out of reach, or a jump on flags no instruction before it left.
    bool failed() const noexcept { return failed_; }
    bool outOfReach() const noexcept { return outOfReach_; }

private:
    // arm64 condition codes
    enum : int { EQ = 0, NE, HS, LO, MI, PL, VS, VC, HI, LS, GE, LT, GT, LE, kAlways = 14, kNever = 15 };
    /// What set the flags last.
    enum class Flags : std::uint8_t { None, Sub, Add, Logic, Mul, Fcmp };
    enum : int { kTmp = 14, kImm = 15, kAddr = 16, kVal = 17, kLink = 27, kStack = 28, kZr = 31, kVTmp = 31 };

    bool failed_ = false;
    bool outOfReach_ = false;
    Flags flags_ = Flags::None;
    // An instruction that leaves its result's zero and sign in the flags on x86-64 and sets none here: the test is made
    // only if the next thing emitted reads them.
    std::size_t pendingAt_ = ~std::size_t(0);
    int pendingReg_ = 0;
    bool pendingWide_ = false;
    int pendingMul_ = 0;  ///< 0: a test of pendingReg_; 1: a 32-bit product in x14; 2: the high half of a 64-bit one in x14

    std::size_t stored_ = ~std::size_t(0);  ///< where the last instruction, a store of a register to the stack, ends
    Reg storedReg_ = RAX;
    std::int32_t storedDisp_ = 0;
    bool storedWide_ = false;
    std::size_t zeroed_ = ~std::size_t(0);  ///< where the last instruction, a 32-bit write of `zeroedReg_`, ends
    Reg zeroedReg_ = RAX;

    static int X(Reg r) noexcept
    {
        static constexpr std::uint8_t map[16] = {8, 3, 2, 19, kStack, 20, 1, 0, 4, 5, 9, 10, 21, 22, 23, 24};
        return map[r];
    }
    static int V(int x) noexcept { return x < 8 ? x : x + 8; }

    void ins(std::uint32_t v)
    {
        const std::size_t at = buf.size();
        buf.resize(at + 4);
        std::memcpy(&buf[at], &v, 4);
    }
    void fail(const char* why)
    {
#if defined(SCRIPT_JIT_ENV_OVERRIDE)
        if (!failed_)
            std::fprintf(stderr, "libscript: arm64 assembler: %s at %zu\n", why, buf.size());
#else
        (void)why;
#endif
        failed_ = true;
    }

    // ---- immediates ----
    void imm(int rd, std::uint64_t v)
    {
        if (v <= 0xFFFFFFFFull) {
            const std::uint32_t lo = std::uint32_t(v) & 0xFFFF, hi = std::uint32_t(v >> 16);
            if (hi == 0) {
                ins(0x52800000u | (lo << 5) | std::uint32_t(rd));
            } else if (lo == 0) {
                ins(0x52A00000u | (hi << 5) | std::uint32_t(rd));
            } else if (hi == 0xFFFF) {
                ins(0x12800000u | ((~lo & 0xFFFF) << 5) | std::uint32_t(rd));  // movn w
            } else {
                ins(0x52800000u | (lo << 5) | std::uint32_t(rd));
                ins(0x72A00000u | (hi << 5) | std::uint32_t(rd));
            }
            return;
        }
        int zeros = 0, ones = 0;
        for (int i = 0; i < 4; ++i) {
            const std::uint32_t c = std::uint32_t(v >> (16 * i)) & 0xFFFF;
            zeros += c == 0;
            ones += c == 0xFFFF;
        }
        const bool inverted = ones > zeros;
        const std::uint32_t skip = inverted ? 0xFFFF : 0;
        bool first = true;
        for (std::uint32_t i = 0; i < 4; ++i) {
            const std::uint32_t c = std::uint32_t(v >> (16 * i)) & 0xFFFF;
            if (c == skip)
                continue;
            if (first && inverted)
                ins(0x92800000u | (i << 21) | ((~c & 0xFFFF) << 5) | std::uint32_t(rd));
            else if (first)
                ins(0xD2800000u | (i << 21) | (c << 5) | std::uint32_t(rd));
            else
                ins(0xF2800000u | (i << 21) | (c << 5) | std::uint32_t(rd));
            first = false;
        }
        if (first)  // all ones
            ins(0x92800000u | std::uint32_t(rd));
    }
    /// The N:immr:imms field (already in place, bits 22..10) of `v` as a logical immediate, if it is one.
    static bool logicalImm(std::uint64_t v, bool wide, std::uint32_t* field) noexcept
    {
        if (!wide) {
            v &= 0xFFFFFFFFull;
            v |= v << 32;
        }
        if (v == 0 || v == ~std::uint64_t(0))
            return false;
        unsigned size = 64;
        while (size > 2) {
            const unsigned half = size / 2;
            const std::uint64_t mask = (std::uint64_t(1) << half) - 1;
            if ((v & mask) != ((v >> half) & mask))
                break;
            size = half;
        }
        const std::uint64_t emask = size == 64 ? ~std::uint64_t(0) : (std::uint64_t(1) << size) - 1;
        const std::uint64_t elem = v & emask;
        const unsigned ones = unsigned(__builtin_popcountll(elem));
        const std::uint64_t run = ones == 64 ? ~std::uint64_t(0) : (std::uint64_t(1) << ones) - 1;
        for (unsigned r = 0; r < size; ++r) {
            const std::uint64_t rot = r == 0 ? elem : ((elem >> r) | (elem << (size - r))) & emask;
            if (rot != run)
                continue;
            const std::uint32_t immr = (size - r) % size;
            const std::uint32_t imms = ((~(size * 2 - 1)) & 0x3F) | (ones - 1);
            *field = (std::uint32_t(size == 64) << 22) | (immr << 16) | (imms << 10);
            return true;
        }
        return false;
    }

    // ---- addresses ----
    struct Ea
    {
        int base;
        int index;  ///< -1 for none
        int shift;
        std::int32_t disp;
    };
    static int log2Of(unsigned scale) noexcept { return scale == 8 ? 3 : scale == 4 ? 2 : scale == 2 ? 1 : 0; }
    /// `rd = rn + v`, setting no flags.
    void addImm(int rd, int rn, std::int64_t v)
    {
        if (v == 0) {
            if (rd != rn)
                ins(0xAA0003E0u | (std::uint32_t(rn) << 16) | std::uint32_t(rd));
            return;
        }
        const bool neg = v < 0;
        const std::uint64_t u = neg ? std::uint64_t(-v) : std::uint64_t(v);
        const std::uint32_t op = neg ? 0xD1000000u : 0x91000000u;
        if (u < (1u << 24)) {
            int from = rn;
            if (u & 0xFFF) {
                ins(op | (std::uint32_t(u & 0xFFF) << 10) | (std::uint32_t(from) << 5) | std::uint32_t(rd));
                from = rd;
            }
            if (u >> 12)
                ins(op | 0x400000u | (std::uint32_t(u >> 12) << 10) | (std::uint32_t(from) << 5) | std::uint32_t(rd));
            return;
        }
        imm(kImm, std::uint64_t(v));
        ins(0x8B000000u | (std::uint32_t(kImm) << 16) | (std::uint32_t(rn) << 5) | std::uint32_t(rd));
    }
    /// An address one access of `1 << size` bytes can use as it stands; what `m` needs beyond that is worked out into x16.
    Ea ea(const Mem& m, int size)
    {
        Ea a{X(m.base), m.index >= 0 ? X(Reg(m.index)) : -1, log2Of(m.scale), m.disp};
        if (a.index >= 0) {
            if (a.disp == 0 && (a.shift == 0 || a.shift == size))
                return a;
            ins(0x8B000000u | (std::uint32_t(a.index) << 16) | (std::uint32_t(a.shift) << 10) | (std::uint32_t(a.base) << 5) | kAddr);
            a = Ea{kAddr, -1, 0, m.disp};
        }
        const bool scaled = a.disp >= 0 && (a.disp & ((1 << size) - 1)) == 0 && (a.disp >> size) < 4096;
        if (scaled || (a.disp >= -256 && a.disp <= 255))
            return a;
        addImm(kAddr, a.base, a.disp);
        return Ea{kAddr, -1, 0, 0};
    }
    /// One load or store. `unscaled` is the instruction's LDUR/STUR form, from which the others follow.
    void access(std::uint32_t unscaled, int size, int rt, const Ea& a)
    {
        const std::uint32_t t = std::uint32_t(rt), b = std::uint32_t(a.base) << 5;
        if (a.index >= 0) {
            ins(unscaled | 0x00206800u | (a.shift ? 0x1000u : 0u) | (std::uint32_t(a.index) << 16) | b | t);
        } else if (a.disp >= 0 && (a.disp & ((1 << size) - 1)) == 0 && (a.disp >> size) < 4096) {
            ins(unscaled | 0x01000000u | (std::uint32_t(a.disp >> size) << 10) | b | t);
        } else {
            ins(unscaled | ((std::uint32_t(a.disp) & 0x1FF) << 12) | b | t);
        }
    }
    enum : std::uint32_t
    {
        kStrB = 0x38000000u, kLdrB = 0x38400000u, kLdrSB = 0x38C00000u, kStrH = 0x78000000u, kLdrH = 0x78400000u,
        kLdrSH = 0x78C00000u, kStrW = 0xB8000000u, kLdrW = 0xB8400000u, kStrX = 0xF8000000u, kLdrX = 0xF8400000u,
        kStrS = 0xBC000000u, kLdrS = 0xBC400000u, kStrD = 0xFC000000u, kLdrD = 0xFC400000u, kStrQ = 0x3C800000u
    };
    void access(std::uint32_t unscaled, int size, int rt, const Mem& m) { access(unscaled, size, rt, ea(m, size)); }

    // ---- flags ----
    void setFlags(Flags f) noexcept
    {
        flags_ = f;
        pendingAt_ = ~std::size_t(0);
    }
    /// The result in `reg` would have set zero and sign on x86-64: tested if the flags are read next.
    void pendingTest(int reg, bool wide) noexcept
    {
        flags_ = Flags::None;
        pendingAt_ = buf.size();
        pendingReg_ = reg;
        pendingWide_ = wide;
        pendingMul_ = 0;
    }
    void needFlags()
    {
        if (pendingAt_ != buf.size())
            return;
        pendingAt_ = ~std::size_t(0);
        if (pendingMul_ == 1) {
            ins(0xEB20C000u | (std::uint32_t(kTmp) << 16) | (std::uint32_t(kTmp) << 5) | kZr);  // cmp x14, w14, sxtw
            flags_ = Flags::Mul;
        } else if (pendingMul_ == 2) {
            // cmp x14, xreg, asr #63
            ins(0xEB800000u | (std::uint32_t(pendingReg_) << 16) | (63u << 10) | (std::uint32_t(kTmp) << 5) | kZr);
            flags_ = Flags::Mul;
        } else {
            const std::uint32_t r = std::uint32_t(pendingReg_);
            ins((pendingWide_ ? 0xEA000000u : 0x6A000000u) | (r << 16) | (r << 5) | kZr);  // tst r, r
            flags_ = Flags::Logic;
        }
    }
    /// The arm64 condition for `c` after the instruction that set the flags; kAlways and kNever for the ones it decides.
    int armCond(Cond c)
    {
        static constexpr std::int8_t sub[16] = {VS, VC, LO, HS, EQ, NE, LS, HI, MI, PL, -1, -1, LT, GE, LE, GT};
        static constexpr std::int8_t add[16] = {VS, VC, HS, LO, EQ, NE, -1, -1, MI, PL, -1, -1, LT, GE, LE, GT};
        static constexpr std::int8_t logic[16] = {kNever, kAlways, kNever, kAlways, EQ, NE, EQ, NE, MI, PL, -1, -1, LT, GE, LE, GT};
        static constexpr std::int8_t fcmp[16] = {-1, -1, LT, GE, -1, -1, LE, GT, -1, -1, VS, VC, -1, -1, -1, -1};
        int r = -1;
        switch (flags_) {
            case Flags::Sub: r = sub[c]; break;
            case Flags::Add: r = add[c]; break;
            case Flags::Logic: r = logic[c]; break;
            case Flags::Mul: r = c == CO ? NE : c == CNO ? EQ : -1; break;
            case Flags::Fcmp: r = fcmp[c]; break;
            case Flags::None: break;
        }
        if (r < 0) {
            fail("a condition the flags do not hold");
            return kNever;
        }
        return r;
    }

    void branch(std::uint32_t op, Label& l)
    {
        if (l.bound()) {
            ins(op | ((std::uint32_t(l.pos - here()) >> 2) & 0x03FFFFFFu));
        } else {
            l.uses.push_back(here());
            ins(op);
        }
    }
    void branchCond(int cond, Label& l)
    {
        if (cond == kNever)
            return;
        if (cond == kAlways) {
            branch(0x14000000u, l);
            return;
        }
        const bool near = l.bound() ? (here() - l.pos) < (1 << 20) - 8 : !longBranches;
        if (!near) {
            ins(0x54000040u | std::uint32_t(cond ^ 1));  // over the jump
            branch(0x14000000u, l);
        } else if (l.bound()) {
            ins(0x54000000u | (((std::uint32_t(l.pos - here()) >> 2) & 0x7FFFFu) << 5) | std::uint32_t(cond));
        } else {
            l.uses.push_back(here());
            ins(0x54000000u | std::uint32_t(cond));
        }
    }
    void noteStore(Reg s, const Mem& m, bool wide)
    {
        if (m.base == RSP && m.index < 0) {
            stored_ = buf.size();
            storedReg_ = s;
            storedDisp_ = m.disp;
            storedWide_ = wide;
        }
    }
    bool justStored(Reg d, const Mem& m, bool wide) const
    {
        return stored_ == buf.size() && m.base == RSP && m.index < 0 && m.disp == storedDisp_ && d == storedReg_ &&
               wide == storedWide_;
    }

    /// `rd = rn op rm` for the interface's ALU numbering (0 add, 1 or, 4 and, 5 sub, 6 xor, 7 cmp), with its flags.
    void aluRRR(int op, bool wide, int rd, int rn, int rm)
    {
        const std::uint32_t w = wide ? 0x80000000u : 0u;
        const std::uint32_t regs = (std::uint32_t(rm) << 16) | (std::uint32_t(rn) << 5);
        switch (op) {
            case 0:
                ins(w | 0x2B000000u | regs | std::uint32_t(rd));
                setFlags(Flags::Add);
                break;
            case 5:
                ins(w | 0x6B000000u | regs | std::uint32_t(rd));
                setFlags(Flags::Sub);
                break;
            case 7:
                ins(w | 0x6B000000u | regs | kZr);
                setFlags(Flags::Sub);
                break;
            case 4:
                ins(w | 0x6A000000u | regs | std::uint32_t(rd));
                setFlags(Flags::Logic);
                break;
            case 1:
                ins(w | 0x2A000000u | regs | std::uint32_t(rd));
                pendingTest(rd, wide);
                break;
            case 6:
                ins(w | 0x4A000000u | regs | std::uint32_t(rd));
                pendingTest(rd, wide);
                break;
            default: fail("an ALU operation the assembler does not have");
        }
    }
    /// `rd = rn op v`; `v` is what x86-64 would have sign-extended from 32 bits.
    void aluRRI(int op, bool wide, int rd, int rn, std::int32_t v)
    {
        const std::uint32_t w = wide ? 0x80000000u : 0u;
        const std::uint32_t d = std::uint32_t(op == 7 ? kZr : rd), n = std::uint32_t(rn) << 5;
        if (op == 0 || op == 5 || op == 7) {
            // An addition of a negative number is the subtraction of its magnitude, with the same flags as x86-64 reads
            // them (the carry of one is the complement of the other's borrow, which is how each kind is decoded).
            const bool adds = (op == 0) == (v >= 0);
            const std::uint32_t u = v >= 0 ? std::uint32_t(v) : std::uint32_t(-std::int64_t(v));
            if (u < 4096) {
                ins(w | (adds ? 0x31000000u : 0x71000000u) | (u << 10) | n | d);
            } else if ((u & 0xFFF) == 0 && (u >> 12) < 4096) {
                ins(w | (adds ? 0x31400000u : 0x71400000u) | ((u >> 12) << 10) | n | d);
            } else {
                imm(kImm, wide ? std::uint64_t(std::int64_t(v)) : std::uint64_t(std::uint32_t(v)));
                ins(w | (op == 0 ? 0x2B000000u : 0x6B000000u) | (std::uint32_t(kImm) << 16) | n | d);
            }
            setFlags(op == 0 ? Flags::Add : Flags::Sub);
            return;
        }
        const std::uint64_t bits = wide ? std::uint64_t(std::int64_t(v)) : std::uint64_t(std::uint32_t(v));
        std::uint32_t field = 0;
        if (logicalImm(bits, wide, &field)) {
            const std::uint32_t base = op == 4 ? 0x72000000u : op == 1 ? 0x32000000u : 0x52000000u;
            ins(w | base | field | n | d);
            if (op == 4)
                setFlags(Flags::Logic);
            else
                pendingTest(rd, wide);
            return;
        }
        imm(kImm, bits);
        aluRRR(op, wide, rd, rn, kImm);
    }
    void moveStack(bool down, std::int32_t bytes)
    {
        // sp goes down before anything is written below it and up after the last read.
        const std::uint32_t v = std::uint32_t(bytes);
        if (down) {
            addImm(31, 31, -std::int64_t(v));
            addImm(kStack, kStack, -std::int64_t(v));
        } else {
            addImm(kStack, kStack, v);
            addImm(31, 31, v);
        }
    }
    void pushX(int r)
    {
        ins(0xD10023FFu);                                                                 // sub sp, sp, #8
        ins(0xF8000C00u | (0x1F8u << 12) | (std::uint32_t(kStack) << 5) | std::uint32_t(r));  // str r, [x28, #-8]!
    }
    void popX(int r)
    {
        ins(0xF8400400u | (8u << 12) | (std::uint32_t(kStack) << 5) | std::uint32_t(r));  // ldr r, [x28], #8
        ins(0x910023FFu);                                                             // add sp, sp, #8
    }
    void fp3(std::uint32_t op, int d, int n, int m)
    {
        ins(op | (std::uint32_t(V(m)) << 16) | (std::uint32_t(V(n)) << 5) | std::uint32_t(V(d)));
    }

public:
    std::int32_t here() const noexcept { return std::int32_t(buf.size()); }

    void align(int n)
    {
        while (buf.size() % std::size_t(n))
            ins(0xD503201Fu);
    }

    // ---- labels ----
    /// A label only the code before it falls into: what the peephole knows of that code still holds.
    void bindFallthrough(Label& l)
    {
        const std::size_t stored = stored_, zeroed = zeroed_;
        bind(l);
        stored_ = stored;
        zeroed_ = zeroed;
    }
    void bind(Label& l)
    {
        stored_ = ~std::size_t(0);
        zeroed_ = ~std::size_t(0);
        l.pos = here();
        for (std::int32_t at : l.uses) {
            std::uint32_t v;
            std::memcpy(&v, &buf[std::size_t(at)], 4);
            const std::int32_t delta = (l.pos - at) >> 2;
            if ((v & 0x7C000000u) == 0x14000000u) {
                v |= std::uint32_t(delta) & 0x03FFFFFFu;
            } else {
                if (delta >= (1 << 18) || delta < -(1 << 18)) {
                    outOfReach_ = true;
                    failed_ = true;
                    reachFailed() = true;
                }
                v |= (std::uint32_t(delta) & 0x7FFFFu) << 5;
            }
            std::memcpy(&buf[std::size_t(at)], &v, 4);
        }
        l.uses.clear();
    }
    void jmp(Label& l) { branch(0x14000000u, l); }
    void jcc(Cond c, Label& l)
    {
        needFlags();
        if (flags_ == Flags::Fcmp && (c == CE || c == CNE)) {
            // x86-64 reads an unordered comparison as equal.
            if (c == CE) {
                branchCond(EQ, l);
                branchCond(VS, l);
            } else {
                Label skip;
                branchCond(VS, skip);
                branchCond(NE, l);
                bind(skip);
            }
            return;
        }
        branchCond(armCond(c), l);
    }
    void call(Label& l)
    {
        pushX(30);
        branch(0x94000000u, l);
        popX(30);
    }

    // ---- moves ----
    void mov(Reg d, Reg s) { ins(0xAA0003E0u | (std::uint32_t(X(s)) << 16) | std::uint32_t(X(d))); }
    void mov32(Reg d, Reg s)
    {
        // A register the instruction just before wrote as 32 bits is zero-extended already.
        if (d == s && zeroed_ == buf.size() && zeroedReg_ == d)
            return;
        ins(0x2A0003E0u | (std::uint32_t(X(s)) << 16) | std::uint32_t(X(d)));
        zeroed_ = buf.size();
        zeroedReg_ = d;
    }
    // A load of the stack slot the instruction just before stored from the same register reads what the register holds:
    // a 64-bit one is dropped, a 32-bit one only zero-extends. A label between ends this (`bind`).
    void mov(Reg d, const Mem& m)
    {
        if (justStored(d, m, true))
            return;
        access(kLdrX, 3, X(d), m);
    }
    void mov(const Mem& m, Reg s)
    {
        access(kStrX, 3, X(s), m);
        noteStore(s, m, true);
    }
    void mov32(Reg d, const Mem& m)
    {
        if (justStored(d, m, false)) {
            mov32(d, d);
            return;
        }
        access(kLdrW, 2, X(d), m);
        zeroed_ = buf.size();
        zeroedReg_ = d;
    }
    void mov32(const Mem& m, Reg s)
    {
        access(kStrW, 2, X(s), m);
        noteStore(s, m, false);
    }
    void mov16(const Mem& m, Reg s) { access(kStrH, 1, X(s), m); }
    void mov8(const Mem& m, Reg s) { access(kStrB, 0, X(s), m); }
    void movzx8(Reg d, const Mem& m) { access(kLdrB, 0, X(d), m); }
    void movzx16(Reg d, const Mem& m) { access(kLdrH, 1, X(d), m); }
    void movzx8(Reg d, Reg s) { ins(0x12001C00u | (std::uint32_t(X(s)) << 5) | std::uint32_t(X(d))); }
    void movsx32(Reg d, Reg s) { ins(0x93407C00u | (std::uint32_t(X(s)) << 5) | std::uint32_t(X(d))); }
    void movsx8(Reg d, const Mem& m) { access(kLdrSB, 0, X(d), m); }
    void movsx16(Reg d, const Mem& m) { access(kLdrSH, 1, X(d), m); }
    /// Store a sign-extended 32-bit immediate as 64 bits.
    void mov(const Mem& m, std::int32_t v)
    {
        const Ea a = ea(m, 3);
        if (v == 0) {
            access(kStrX, 3, kZr, a);
            return;
        }
        imm(kVal, std::uint64_t(std::int64_t(v)));
        access(kStrX, 3, kVal, a);
    }
    void mov32(const Mem& m, std::int32_t v)
    {
        const Ea a = ea(m, 2);
        if (v == 0) {
            access(kStrW, 2, kZr, a);
            return;
        }
        imm(kVal, std::uint64_t(std::uint32_t(v)));
        access(kStrW, 2, kVal, a);
    }
    void movImm(Reg d, std::uint64_t v) { imm(X(d), v); }
    void lea(Reg d, const Mem& m)
    {
        const int rd = X(d);
        if (m.index >= 0) {
            ins(0x8B000000u | (std::uint32_t(X(Reg(m.index))) << 16) | (std::uint32_t(log2Of(m.scale)) << 10) |
                (std::uint32_t(X(m.base)) << 5) | std::uint32_t(rd));
            addImm(rd, rd, m.disp);
        } else {
            addImm(rd, X(m.base), m.disp);
        }
    }

    // ---- ALU: op = 0 add, 1 or, 4 and, 5 sub, 6 xor, 7 cmp ----
    void alu(int op, Reg d, Reg s) { aluRRR(op, true, X(d), X(d), X(s)); }
    void alu32(int op, Reg d, Reg s) { aluRRR(op, false, X(d), X(d), X(s)); }
    void alu(int op, Reg d, const Mem& m)
    {
        access(kLdrX, 3, kVal, m);
        aluRRR(op, true, X(d), X(d), kVal);
    }
    void alu32(int op, Reg d, const Mem& m)
    {
        access(kLdrW, 2, kVal, m);
        aluRRR(op, false, X(d), X(d), kVal);
    }
    void alu(int op, const Mem& m, Reg s)
    {
        const Ea a = ea(m, 3);
        access(kLdrX, 3, kVal, a);
        aluRRR(op, true, kVal, kVal, X(s));
        if (op != 7)
            storeBack(kStrX, 3, a, true);
    }
    void alu32(int op, const Mem& m, Reg s)
    {
        const Ea a = ea(m, 2);
        access(kLdrW, 2, kVal, a);
        aluRRR(op, false, kVal, kVal, X(s));
        if (op != 7)
            storeBack(kStrW, 2, a, false);
    }
    void aluI(int op, Reg d, std::int32_t v)
    {
        if (d == RSP) {
            if (op != 0 && op != 5) {
                fail("an operation on the stack pointer the assembler does not have");
                return;
            }
            moveStack((op == 5) == (v >= 0), v >= 0 ? v : -v);
            flags_ = Flags::None;
            return;
        }
        aluRRI(op, true, X(d), X(d), v);
    }
    void aluI32(int op, Reg d, std::int32_t v) { aluRRI(op, false, X(d), X(d), v); }
    void aluI(int op, const Mem& m, std::int32_t v)
    {
        const Ea a = ea(m, 3);
        access(kLdrX, 3, kVal, a);
        aluRRI(op, true, kVal, kVal, v);
        if (op != 7)
            storeBack(kStrX, 3, a, true);
    }
    void aluI32(int op, const Mem& m, std::int32_t v)
    {
        const Ea a = ea(m, 2);
        access(kLdrW, 2, kVal, a);
        aluRRI(op, false, kVal, kVal, v);
        if (op != 7)
            storeBack(kStrW, 2, a, false);
    }

private:
    /// The store that ends an operation on memory: it changes no flag, and a test of the result still waits on x17.
    void storeBack(std::uint32_t op, int size, const Ea& a, bool wide)
    {
        const bool pending = pendingAt_ == buf.size();
        access(op, size, kVal, a);
        if (pending) {
            pendingAt_ = buf.size();
            pendingReg_ = kVal;
            pendingWide_ = wide;
        }
    }

public:
    void add(Reg d, Reg s) { alu(0, d, s); }
    void sub(Reg d, Reg s) { alu(5, d, s); }
    void and_(Reg d, Reg s) { alu(4, d, s); }
    void or_(Reg d, Reg s) { alu(1, d, s); }
    void xor_(Reg d, Reg s) { alu(6, d, s); }
    void cmp(Reg d, Reg s) { alu(7, d, s); }
    void cmp(Reg d, const Mem& m) { alu(7, d, m); }
    void cmp(const Mem& m, Reg s) { alu(7, m, s); }
    void cmpI(Reg d, std::int32_t v) { aluI(7, d, v); }
    void cmpI(const Mem& m, std::int32_t v) { aluI(7, m, v); }
    void cmpI32(const Mem& m, std::int32_t v) { aluI32(7, m, v); }
    void test(Reg a, Reg c)
    {
        ins(0xEA000000u | (std::uint32_t(X(c)) << 16) | (std::uint32_t(X(a)) << 5) | kZr);
        setFlags(Flags::Logic);
    }
    void test32(Reg a, Reg c)
    {
        ins(0x6A000000u | (std::uint32_t(X(c)) << 16) | (std::uint32_t(X(a)) << 5) | kZr);
        setFlags(Flags::Logic);
    }
    void testI(Reg a, std::int32_t v) { aluRRI(4, true, kZr, X(a), v); }
    void testI32(const Mem& m, std::uint32_t v)
    {
        access(kLdrW, 2, kVal, m);
        aluRRI(4, false, kZr, kVal, std::int32_t(v));
    }
    void cmpI16(const Mem& m, std::uint16_t v)
    {
        access(kLdrH, 1, kVal, m);
        aluRRI(7, false, kZr, kVal, std::int32_t(v));
    }
    void cmpI8(const Mem& m, std::uint8_t v)
    {
        access(kLdrB, 0, kVal, m);
        aluRRI(7, false, kZr, kVal, std::int32_t(v));
    }
    void testI8(const Mem& m, std::uint8_t v)
    {
        access(kLdrB, 0, kVal, m);
        aluRRI(4, false, kZr, kVal, std::int32_t(v));
    }
    void orI8(const Mem& m, std::uint8_t v)
    {
        const Ea a = ea(m, 0);
        access(kLdrB, 0, kVal, a);
        aluRRI(1, false, kVal, kVal, std::int32_t(v));
        access(kStrB, 0, kVal, a);
        flags_ = Flags::None;
    }
    void andI8(const Mem& m, std::uint8_t v)
    {
        const Ea a = ea(m, 0);
        access(kLdrB, 0, kVal, a);
        aluRRI(4, false, kVal, kVal, std::int32_t(v));
        access(kStrB, 0, kVal, a);
    }
    /// A 32-bit product, with the whole of it in x14 until the flags are asked for (overflow is all they can say).
    void imulI32(Reg d, Reg s, std::int32_t v)
    {
        imm(kImm, std::uint64_t(std::uint32_t(v)));
        ins(0x9B207C00u | (std::uint32_t(kImm) << 16) | (std::uint32_t(X(s)) << 5) | kTmp);  // smull x14, ws, w15
        ins(0x2A0003E0u | (std::uint32_t(kTmp) << 16) | std::uint32_t(X(d)));              // mov wd, w14
        pendingTest(kTmp, true);
        pendingMul_ = 1;
    }
    void imul32(Reg d, Reg s)
    {
        ins(0x9B207C00u | (std::uint32_t(X(s)) << 16) | (std::uint32_t(X(d)) << 5) | kTmp);
        ins(0x2A0003E0u | (std::uint32_t(kTmp) << 16) | std::uint32_t(X(d)));
        pendingTest(kTmp, true);
        pendingMul_ = 1;
    }
    void imul64(Reg d, Reg s)
    {
        ins(0x9B407C00u | (std::uint32_t(X(s)) << 16) | (std::uint32_t(X(d)) << 5) | kTmp);              // smulh x14
        ins(0x9B007C00u | (std::uint32_t(X(s)) << 16) | (std::uint32_t(X(d)) << 5) | std::uint32_t(X(d)));  // mul
        pendingTest(X(d), true);
        pendingMul_ = 2;
    }
    void neg32(Reg r)
    {
        ins(0x6B0003E0u | (std::uint32_t(X(r)) << 16) | std::uint32_t(X(r)));
        setFlags(Flags::Sub);
    }
    void not32(Reg r) { ins(0x2A2003E0u | (std::uint32_t(X(r)) << 16) | std::uint32_t(X(r))); }
    void inc32(const Mem& m)
    {
        const Ea a = ea(m, 2);
        access(kLdrW, 2, kVal, a);
        ins(0x31000400u | (std::uint32_t(kVal) << 5) | kVal);
        access(kStrW, 2, kVal, a);
        setFlags(Flags::Add);
    }
    void dec32(const Mem& m)
    {
        const Ea a = ea(m, 2);
        access(kLdrW, 2, kVal, a);
        ins(0x71000400u | (std::uint32_t(kVal) << 5) | kVal);
        access(kStrW, 2, kVal, a);
        setFlags(Flags::Sub);
    }
    // shifts: kind 4 shl, 5 shr, 7 sar
    void shiftI(int kind, Reg r, int n)
    {
        const std::uint32_t x = std::uint32_t(X(r)), s = std::uint32_t(n) & 63;
        if (kind == 4)
            ins(0xD3400000u | (((64 - s) & 63) << 16) | ((63 - s) << 10) | (x << 5) | x);
        else
            ins((kind == 5 ? 0xD3400000u : 0x93400000u) | (s << 16) | (63u << 10) | (x << 5) | x);
        pendingTest(int(x), true);
    }
    void shiftI32(int kind, Reg r, int n)
    {
        const std::uint32_t x = std::uint32_t(X(r)), s = std::uint32_t(n) & 31;
        if (kind == 4)
            ins(0x53000000u | (((32 - s) & 31) << 16) | ((31 - s) << 10) | (x << 5) | x);
        else
            ins((kind == 5 ? 0x53000000u : 0x13000000u) | (s << 16) | (31u << 10) | (x << 5) | x);
        pendingTest(int(x), false);
    }
    void shiftCl32(int kind, Reg r)
    {
        const std::uint32_t x = std::uint32_t(X(r));
        const std::uint32_t op = kind == 4 ? 0x1AC02000u : kind == 5 ? 0x1AC02400u : 0x1AC02800u;
        ins(op | (std::uint32_t(X(RCX)) << 16) | (x << 5) | x);
        pendingTest(int(x), false);
    }
    void shiftCl(int kind, Reg r)
    {
        const std::uint32_t x = std::uint32_t(X(r));
        const std::uint32_t op = kind == 4 ? 0x9AC02000u : kind == 5 ? 0x9AC02400u : 0x9AC02800u;
        ins(op | (std::uint32_t(X(RCX)) << 16) | (x << 5) | x);
        pendingTest(int(x), true);
    }
    void cdq() { ins(0x13000000u | (31u << 16) | (31u << 10) | (std::uint32_t(X(RAX)) << 5) | std::uint32_t(X(RDX))); }
    /// eax = edx:eax / r, edx the remainder; the templates make sure of the divisor first.
    void idiv32(Reg r)
    {
        const std::uint32_t a = std::uint32_t(X(RAX)), d = std::uint32_t(X(RDX)), x = std::uint32_t(X(r));
        ins(0x1AC00C00u | (x << 16) | (a << 5) | kTmp);              // sdiv w14, wa, wx
        ins(0x1B008000u | (x << 16) | (a << 10) | (std::uint32_t(kTmp) << 5) | d);  // msub wd, w14, wx, wa
        ins(0x2A0003E0u | (std::uint32_t(kTmp) << 16) | a);
        flags_ = Flags::None;
    }
    /// The low byte of `r` only, as x86-64 has it.
    void setcc(Cond c, Reg r)
    {
        needFlags();
        if (flags_ == Flags::Fcmp && (c == CE || c == CNE)) {
            ins(0x1A9F07E0u | (std::uint32_t((c == CE ? EQ : NE) ^ 1) << 12) | kVal);
            if (c == CE)
                ins(0x1A9F0400u | (std::uint32_t(kVal) << 5) | (std::uint32_t(VC) << 12) | kVal);  // csinc w17, w17, wzr, vc
            else
                ins(0x1A9F0000u | (std::uint32_t(kVal) << 5) | (std::uint32_t(VC) << 12) | kVal);  // csel w17, w17, wzr, vc
        } else {
            const int cond = armCond(c);
            if (cond == kAlways)
                ins(0x52800020u | kVal);
            else if (cond == kNever)
                ins(0x52800000u | kVal);
            else
                ins(0x1A9F07E0u | (std::uint32_t(cond ^ 1) << 12) | kVal);
        }
        ins(0xB3401C00u | (std::uint32_t(kVal) << 5) | std::uint32_t(X(r)));  // bfxil xr, x17, #0, #8
    }
    void cmov(Cond c, Reg d, Reg s)
    {
        needFlags();
        const int cond = armCond(c);
        if (cond == kNever)
            return;
        if (cond == kAlways) {
            mov(d, s);
            return;
        }
        ins(0x9A800000u | (std::uint32_t(X(d)) << 16) | (std::uint32_t(cond) << 12) | (std::uint32_t(X(s)) << 5) |
            std::uint32_t(X(d)));
    }

    // ---- control ----
    void jmp(Reg r) { ins(0xD61F0000u | (std::uint32_t(X(r)) << 5)); }
    /// A call of compiled code.
    void callReg(Reg r)
    {
        pushX(30);
        ins(0xD63F0000u | (std::uint32_t(X(r)) << 5));
        popX(30);
    }
    /// A call into the runtime: the arguments are where its convention wants them; the result goes to rax (and rdx, for a
    /// function that returns two words).
    void callC(Reg r) { callRuntime(X(r)); }
    /// A jump to a function of the runtime that returns to whoever called the code this ends. That code was entered by a
    /// call, which left the stack eight bytes off the alignment the runtime needs.
    void jmpC(Reg r)
    {
        moveStack(true, 8);
        callRuntime(X(r));
        moveStack(false, 8);
        ret();
    }
    void callAbs(const void* fn)
    {
        imm(kAddr, reinterpret_cast<std::uint64_t>(fn));
        callRuntime(kAddr);
    }
    void ret() { ins(0xD65F03C0u); }
    void push(Reg r) { pushX(X(r)); }
    void pop(Reg r) { popX(X(r)); }
    void int3() { ins(0xD4200000u); }
    /// What a function called from C++ does first and last: the registers the assembler itself relies on, and the stack as
    /// the templates expect it on entry (as if a return address had been pushed).
    void abiEnter()
    {
        ins(0xA9BF7BFDu);  // stp x29, x30, [sp, #-16]!
        ins(0xA9BF73FBu);  // stp x27, x28, [sp, #-16]!
        ins(0x910003FCu);  // mov x28, sp
        moveStack(true, 8);
    }
    void abiLeave()
    {
        ins(0xAA0803E0u);  // mov x0, x8: the result, where C++ looks for it
        moveStack(false, 8);
        ins(0xA8C173FBu);  // ldp x27, x28, [sp], #16
        ins(0xA8C17BFDu);  // ldp x29, x30, [sp], #16
    }

private:
    void callRuntime(int target)
    {
        ins(0xAA1E03FBu);                                 // mov x27, x30
        ins(0xD63F0000u | (std::uint32_t(target) << 5));  // blr
        ins(0xAA1B03FEu);                                 // mov x30, x27
        ins(0xAA0003E8u);                                 // mov x8, x0
        ins(0xAA0103E2u);                                 // mov x2, x1
        flags_ = Flags::None;
    }

public:
    // ---- scalar double; the interface's xmm registers are 0..15 ----
    void movqToX(int x, Reg r) { ins(0x9E670000u | (std::uint32_t(X(r)) << 5) | std::uint32_t(V(x))); }
    void movqFromX(Reg r, int x) { ins(0x9E660000u | (std::uint32_t(V(x)) << 5) | std::uint32_t(X(r))); }
    void addsd(int x, int y) { fp3(0x1E602800u, x, x, y); }
    void subsd(int x, int y) { fp3(0x1E603800u, x, x, y); }
    void mulsd(int x, int y) { fp3(0x1E600800u, x, x, y); }
    void divsd(int x, int y) { fp3(0x1E601800u, x, x, y); }
    void ucomisd(int x, int y)
    {
        ins(0x1E602000u | (std::uint32_t(V(y)) << 16) | (std::uint32_t(V(x)) << 5));
        setFlags(Flags::Fcmp);
    }
    void cvtsi2sd32(int x, Reg r) { ins(0x1E620000u | (std::uint32_t(X(r)) << 5) | std::uint32_t(V(x))); }
    void cvtsi2sd64(int x, Reg r) { ins(0x9E620000u | (std::uint32_t(X(r)) << 5) | std::uint32_t(V(x))); }
    /// Truncation as x86-64 does it: a value that does not fit, and a NaN, give the smallest integer.
    void cvttsd2si32(Reg r, int x)
    {
        const std::uint32_t v = std::uint32_t(V(x));
        ins(0x9E780000u | (v << 5) | kAddr);                                                    // fcvtzs x16, d
        ins(0x93407C00u | (std::uint32_t(kAddr) << 5) | kVal);                                  // sxtw x17, w16
        ins(0x1E602000u | (v << 16) | (v << 5));                                                // fcmp d, d
        ins(0xFA400000u | (std::uint32_t(kVal) << 16) | (std::uint32_t(VC) << 12) | (std::uint32_t(kAddr) << 5));  // ccmp x16, x17, #0, vc
        ins(0x52B00000u | kVal);                                                                // mov w17, #0x80000000
        ins(0x1A800000u | (std::uint32_t(kVal) << 16) | (std::uint32_t(EQ) << 12) | (std::uint32_t(kAddr) << 5) |
            std::uint32_t(X(r)));                                                               // csel wr, w16, w17, eq
        flags_ = Flags::None;
    }
    void cvttsd2si64(Reg r, int x)
    {
        const std::uint32_t v = std::uint32_t(V(x)), d = std::uint32_t(X(r));
        ins(0x9E780000u | (v << 5) | kAddr);                                                         // fcvtzs x16, d
        ins(0xD2F00000u | kVal);                                                                     // mov x17, #1 << 63
        ins(0xB100041Fu | (std::uint32_t(kAddr) << 5));                                              // cmn x16, #1
        ins(0x9A800000u | (std::uint32_t(kAddr) << 16) | (std::uint32_t(VS) << 12) | (std::uint32_t(kVal) << 5) | kAddr);  // csel x16, x17, x16, vs
        ins(0x1E602000u | (v << 16) | (v << 5));                                                     // fcmp d, d
        ins(0x9A800000u | (std::uint32_t(kAddr) << 16) | (std::uint32_t(VS) << 12) | (std::uint32_t(kVal) << 5) | d);      // csel xd, x17, x16, vs
        flags_ = Flags::None;
    }
    void xorps(int x, int y)
    {
        if (x == y)
            ins(0x2F00E400u | std::uint32_t(V(x)));  // movi d, #0
        else
            ins(0x6E201C00u | (std::uint32_t(V(y)) << 16) | (std::uint32_t(V(x)) << 5) | std::uint32_t(V(x)));
    }
    void movaps(int x, int y) { ins(0x4EA01C00u | (std::uint32_t(V(y)) << 16) | (std::uint32_t(V(y)) << 5) | std::uint32_t(V(x))); }
    void sqrtsd(int x, int y) { ins(0x1E61C000u | (std::uint32_t(V(y)) << 5) | std::uint32_t(V(x))); }
    /// `d = a op b` (op: 0x58 add, 0x5C sub, 0x59 mul, 0x5E div), and with 0x51 `d = sqrt(b)`.
    void vsd(std::uint8_t op, int d, int a, int bx)
    {
        switch (op) {
            case 0x58: fp3(0x1E602800u, d, a, bx); break;
            case 0x5C: fp3(0x1E603800u, d, a, bx); break;
            case 0x59: fp3(0x1E600800u, d, a, bx); break;
            case 0x5E: fp3(0x1E601800u, d, a, bx); break;
            case 0x51: sqrtsd(d, bx); break;
            default: fail("a double operation the assembler does not have");
        }
    }
    void cvtsd2ss(int x, int y) { ins(0x1E624000u | (std::uint32_t(V(y)) << 5) | std::uint32_t(V(x))); }
    void cvtss2sd(int x, int y) { ins(0x1E22C000u | (std::uint32_t(V(y)) << 5) | std::uint32_t(V(x))); }
    void movsdLoad(int x, const Mem& m) { access(kLdrD, 3, V(x), m); }
    void movsdStore(const Mem& m, int x) { access(kStrD, 3, V(x), m); }
    void movupsStore(const Mem& m, int x) { access(kStrQ, 4, V(x), m); }
    void punpcklqdq(int x, int y) { ins(0x6E180400u | (std::uint32_t(V(y)) << 5) | std::uint32_t(V(x))); }
    void movssLoad(int x, const Mem& m) { access(kLdrS, 2, V(x), m); }
    void movssStore(const Mem& m, int x) { access(kStrS, 2, V(x), m); }
};

using Asm = A64;

}  // namespace jit
}  // namespace vm
}  // namespace internal
}  // namespace script
