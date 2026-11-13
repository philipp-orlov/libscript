// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// A small x86-64 macro assembler for the vm core's baseline compiler: the instruction forms the templates use, labels with
// fix-ups, and nothing else. It writes into a byte vector; the caller copies the result into executable memory.
#pragma once

#include <cstdint>
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
    std::vector<std::int32_t> uses;  ///< offsets of 32-bit displacements waiting for it
    bool bound() const noexcept { return pos >= 0; }
};

class X64
{
public:
    std::vector<std::uint8_t> buf;

    std::int32_t here() const noexcept { return std::int32_t(buf.size()); }

    // ---- raw ----
    void b(std::uint8_t v) { buf.push_back(v); }
    void d32(std::uint32_t v)
    {
        for (int i = 0; i < 4; ++i)
            buf.push_back(std::uint8_t(v >> (8 * i)));
    }
    void d64(std::uint64_t v)
    {
        for (int i = 0; i < 8; ++i)
            buf.push_back(std::uint8_t(v >> (8 * i)));
    }
    void align(int n)
    {
        while (buf.size() % std::size_t(n))
            buf.push_back(0x90);
    }

    // ---- labels ----
    void bind(Label& l)
    {
        l.pos = here();
        for (std::int32_t at : l.uses)
            patch32(at, l.pos - (at + 4));
        l.uses.clear();
    }
    void patch32(std::int32_t at, std::int32_t v) { std::memcpy(&buf[std::size_t(at)], &v, 4); }
    void rel32To(Label& l)
    {
        if (l.bound()) {
            d32(std::uint32_t(l.pos - (here() + 4)));
        } else {
            l.uses.push_back(here());
            d32(0);
        }
    }
    void jmp(Label& l)
    {
        b(0xE9);
        rel32To(l);
    }
    void jcc(Cond c, Label& l)
    {
        b(0x0F);
        b(std::uint8_t(0x80 | c));
        rel32To(l);
    }
    void call(Label& l)
    {
        b(0xE8);
        rel32To(l);
    }

    // ---- encoding helpers ----
    void rex(bool w, int reg, int index, int base, bool force = false)
    {
        const std::uint8_t r = std::uint8_t(0x40 | (w ? 8 : 0) | ((reg & 8) ? 4 : 0) | ((index & 8) ? 2 : 0) | ((base & 8) ? 1 : 0));
        if (r != 0x40 || force)
            b(r);
    }
    void modrmReg(int reg, int rm) { b(std::uint8_t(0xC0 | ((reg & 7) << 3) | (rm & 7))); }
    void modrmMem(int reg, const Mem& m)
    {
        const int base = m.base & 7;
        const bool needSib = m.index >= 0 || base == 4;
        int mod;
        if (m.disp == 0 && base != 5)
            mod = 0;
        else if (m.disp >= -128 && m.disp <= 127)
            mod = 1;
        else
            mod = 2;
        b(std::uint8_t((mod << 6) | ((reg & 7) << 3) | (needSib ? 4 : base)));
        if (needSib) {
            int ss = 0;
            switch (m.scale) {
            case 2: ss = 1; break;
            case 4: ss = 2; break;
            case 8: ss = 3; break;
            default: ss = 0;
            }
            const int idx = m.index >= 0 ? (m.index & 7) : 4;
            b(std::uint8_t((ss << 6) | (idx << 3) | base));
        }
        if (mod == 1)
            b(std::uint8_t(m.disp));
        else if (mod == 2)
            d32(std::uint32_t(m.disp));
    }
    /// prefix, REX, one or two opcode bytes, modrm with a memory operand.
    void opMem(bool w, std::uint8_t op1, int reg, const Mem& m, int op2 = -1, int prefix = -1)
    {
        if (prefix >= 0)
            b(std::uint8_t(prefix));
        rex(w, reg, m.index >= 0 ? m.index : 0, m.base);
        b(op1);
        if (op2 >= 0)
            b(std::uint8_t(op2));
        modrmMem(reg, m);
    }
    void opReg(bool w, std::uint8_t op1, int reg, int rm, int op2 = -1, int prefix = -1, bool forceRex = false)
    {
        if (prefix >= 0)
            b(std::uint8_t(prefix));
        rex(w, reg, 0, rm, forceRex);
        b(op1);
        if (op2 >= 0)
            b(std::uint8_t(op2));
        modrmReg(reg, rm);
    }

    // ---- moves ----
    void mov(Reg d, Reg s) { opReg(true, 0x8B, d, s); }
    void mov32(Reg d, Reg s) { opReg(false, 0x8B, d, s); }
    void mov(Reg d, const Mem& m) { opMem(true, 0x8B, d, m); }
    void mov(const Mem& m, Reg s) { opMem(true, 0x89, s, m); }
    void mov32(Reg d, const Mem& m) { opMem(false, 0x8B, d, m); }
    void mov32(const Mem& m, Reg s) { opMem(false, 0x89, s, m); }
    void mov16(const Mem& m, Reg s) { opMem(false, 0x89, s, m, -1, 0x66); }
    void mov8(const Mem& m, Reg s)
    {
        rex(false, s, m.index >= 0 ? m.index : 0, m.base, s >= 4);
        b(0x88);
        modrmMem(s, m);
    }
    void movzx8(Reg d, const Mem& m) { opMem(false, 0x0F, d, m, 0xB6); }
    void movzx16(Reg d, const Mem& m) { opMem(false, 0x0F, d, m, 0xB7); }
    void movzx8(Reg d, Reg s)
    {
        rex(false, d, 0, s, s >= 4);
        b(0x0F);
        b(0xB6);
        modrmReg(d, s);
    }
    void movsx32(Reg d, Reg s) { opReg(true, 0x63, d, s); }
    /// Store a sign-extended 32-bit immediate as 64 bits.
    void mov(const Mem& m, std::int32_t imm)
    {
        opMem(true, 0xC7, 0, m);
        d32(std::uint32_t(imm));
    }
    void mov32(const Mem& m, std::int32_t imm)
    {
        opMem(false, 0xC7, 0, m);
        d32(std::uint32_t(imm));
    }
    void movImm(Reg d, std::uint64_t v)
    {
        if (v == 0) {
            opReg(false, 0x33, d, d);  // xor r32, r32
        } else if (v <= 0xFFFFFFFFull) {
            rex(false, 0, 0, d);
            b(std::uint8_t(0xB8 | (d & 7)));
            d32(std::uint32_t(v));
        } else if (std::int64_t(v) >= -2147483648ll && std::int64_t(v) < 0) {
            rex(true, 0, 0, d);
            b(0xC7);
            modrmReg(0, d);
            d32(std::uint32_t(v));
        } else {
            rex(true, 0, 0, d);
            b(std::uint8_t(0xB8 | (d & 7)));
            d64(v);
        }
    }
    void lea(Reg d, const Mem& m) { opMem(true, 0x8D, d, m); }

    // ---- ALU: op = 0 add, 1 or, 4 and, 5 sub, 6 xor, 7 cmp ----
    void alu(int op, Reg d, Reg s) { opReg(true, std::uint8_t(op * 8 + 3), d, s); }
    void alu32(int op, Reg d, Reg s) { opReg(false, std::uint8_t(op * 8 + 3), d, s); }
    void alu(int op, Reg d, const Mem& m) { opMem(true, std::uint8_t(op * 8 + 3), d, m); }
    void alu32(int op, Reg d, const Mem& m) { opMem(false, std::uint8_t(op * 8 + 3), d, m); }
    void alu(int op, const Mem& m, Reg s) { opMem(true, std::uint8_t(op * 8 + 1), s, m); }
    void alu32(int op, const Mem& m, Reg s) { opMem(false, std::uint8_t(op * 8 + 1), s, m); }
    void aluI(int op, Reg d, std::int32_t imm)
    {
        if (imm >= -128 && imm <= 127) {
            opReg(true, 0x83, op, d);
            b(std::uint8_t(imm));
        } else {
            opReg(true, 0x81, op, d);
            d32(std::uint32_t(imm));
        }
    }
    void aluI32(int op, Reg d, std::int32_t imm)
    {
        if (imm >= -128 && imm <= 127) {
            opReg(false, 0x83, op, d);
            b(std::uint8_t(imm));
        } else {
            opReg(false, 0x81, op, d);
            d32(std::uint32_t(imm));
        }
    }
    void aluI(int op, const Mem& m, std::int32_t imm)
    {
        if (imm >= -128 && imm <= 127) {
            opMem(true, 0x83, op, m);
            b(std::uint8_t(imm));
        } else {
            opMem(true, 0x81, op, m);
            d32(std::uint32_t(imm));
        }
    }
    void aluI32(int op, const Mem& m, std::int32_t imm)
    {
        if (imm >= -128 && imm <= 127) {
            opMem(false, 0x83, op, m);
            b(std::uint8_t(imm));
        } else {
            opMem(false, 0x81, op, m);
            d32(std::uint32_t(imm));
        }
    }
    void add(Reg d, Reg s) { alu(0, d, s); }
    void sub(Reg d, Reg s) { alu(5, d, s); }
    void and_(Reg d, Reg s) { alu(4, d, s); }
    void or_(Reg d, Reg s) { alu(1, d, s); }
    void xor_(Reg d, Reg s) { alu(6, d, s); }
    void cmp(Reg d, Reg s) { alu(7, d, s); }
    void cmp(Reg d, const Mem& m) { alu(7, d, m); }
    void cmp(const Mem& m, Reg s) { alu(7, m, s); }
    void cmpI(Reg d, std::int32_t imm) { aluI(7, d, imm); }
    void cmpI(const Mem& m, std::int32_t imm) { aluI(7, m, imm); }
    void cmpI32(const Mem& m, std::int32_t imm) { aluI32(7, m, imm); }
    void test(Reg a, Reg c) { opReg(true, 0x85, c, a); }
    void test32(Reg a, Reg c) { opReg(false, 0x85, c, a); }
    void testI(Reg a, std::int32_t imm)
    {
        opReg(true, 0xF7, 0, a);
        d32(std::uint32_t(imm));
    }
    void testI32(const Mem& m, std::uint32_t imm)
    {
        opMem(false, 0xF7, 0, m);
        d32(imm);
    }
    void cmpI8(const Mem& m, std::uint8_t imm)
    {
        opMem(false, 0x80, 7, m);
        b(imm);
    }
    void testI8(const Mem& m, std::uint8_t imm)
    {
        opMem(false, 0xF6, 0, m);
        b(imm);
    }
    void andI8(const Mem& m, std::uint8_t imm)
    {
        opMem(false, 0x80, 4, m);
        b(imm);
    }
    void imulI32(Reg d, Reg s, std::int32_t imm)
    {
        opReg(false, 0x69, d, s);
        d32(std::uint32_t(imm));
    }
    void imul32(Reg d, Reg s) { opReg(false, 0x0F, d, s, 0xAF); }
    void neg32(Reg r) { opReg(false, 0xF7, 3, r); }
    void not32(Reg r) { opReg(false, 0xF7, 2, r); }
    void inc32(const Mem& m) { opMem(false, 0xFF, 0, m); }
    void dec32(const Mem& m) { opMem(false, 0xFF, 1, m); }
    // shifts: kind 4 shl, 5 shr, 7 sar
    void shiftI(int kind, Reg r, int n)
    {
        opReg(true, 0xC1, kind, r);
        b(std::uint8_t(n));
    }
    void shiftI32(int kind, Reg r, int n)
    {
        opReg(false, 0xC1, kind, r);
        b(std::uint8_t(n));
    }
    void shiftCl32(int kind, Reg r) { opReg(false, 0xD3, kind, r); }
    void shiftCl(int kind, Reg r) { opReg(true, 0xD3, kind, r); }
    void cdq() { b(0x99); }
    void idiv32(Reg r) { opReg(false, 0xF7, 7, r); }
    void setcc(Cond c, Reg r)
    {
        rex(false, 0, 0, r, r >= 4);
        b(0x0F);
        b(std::uint8_t(0x90 | c));
        modrmReg(0, r);
    }
    void cmov(Cond c, Reg d, Reg s) { opReg(true, 0x0F, d, s, 0x40 | c); }

    // ---- control ----
    void jmp(Reg r) { opReg(false, 0xFF, 4, r); }
    void callReg(Reg r) { opReg(false, 0xFF, 2, r); }
    void callMem(const Mem& m) { opMem(false, 0xFF, 2, m); }
    void jmpMem(const Mem& m) { opMem(false, 0xFF, 4, m); }
    void ret() { b(0xC3); }
    void push(Reg r)
    {
        rex(false, 0, 0, r);
        b(std::uint8_t(0x50 | (r & 7)));
    }
    void pop(Reg r)
    {
        rex(false, 0, 0, r);
        b(std::uint8_t(0x58 | (r & 7)));
    }
    void int3() { b(0xCC); }
    /// A call to an absolute address through rax.
    void callAbs(const void* fn)
    {
        movImm(RAX, reinterpret_cast<std::uint64_t>(fn));
        callReg(RAX);
    }

    // ---- SSE2 scalar double; xmm registers are 0..15 ----
    void movqToX(int x, Reg r) { opReg(true, 0x0F, x, r, 0x6E, 0x66); }
    void movqFromX(Reg r, int x) { opReg(true, 0x0F, x, r, 0x7E, 0x66); }
    void sse(std::uint8_t op, int x, int y) { opReg(false, 0x0F, x, y, op, 0xF2); }
    void addsd(int x, int y) { sse(0x58, x, y); }
    void subsd(int x, int y) { sse(0x5C, x, y); }
    void mulsd(int x, int y) { sse(0x59, x, y); }
    void divsd(int x, int y) { sse(0x5E, x, y); }
    void ucomisd(int x, int y) { opReg(false, 0x0F, x, y, 0x2E, 0x66); }
    void cvtsi2sd32(int x, Reg r) { opReg(false, 0x0F, x, r, 0x2A, 0xF2); }
    void cvttsd2si32(Reg r, int x) { opReg(false, 0x0F, r, x, 0x2C, 0xF2); }
};

}  // namespace jit
}  // namespace vm
}  // namespace internal
}  // namespace script
