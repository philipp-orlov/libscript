// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The optimizing compiler's half of `Baseline` (included from baseline_x64.cpp inside its anonymous namespace): code for the
// typed variables `opt::analyse` found, and the glue that keeps them and the frame in step. What a typed instruction does not
// cover is run by the baseline templates, with the variables it reads written back to the frame first.
//
// Two places hold a typed web's value: its variable (a register or a stack slot of this function's extra frame) and the
// frame register it is the value of. The variable is always right; the frame register is right when `dirty_` says so, and
// is brought up to date (boxed) before anything reads it through the frame: a baseline template, an exit, a handler.

Reg Baseline::useI32(std::int32_t w, Reg tmp)
{
    if (inReg(w))
        return gpr(w);
    a_.mov32(tmp, home(w));
    return tmp;
}

void Baseline::defI32(std::int32_t w, Reg src)
{
    if (inReg(w)) {
        if (gpr(w) != src)
            a_.mov32(gpr(w), src);
    } else {
        a_.mov32(home(w), src);
    }
    dirty_[std::size_t(w)] = 1;
}

/// The number of web `w` as a double in an xmm register: its own when it has one, else `xtmp`.
int Baseline::operandF64(std::int32_t w, int xtmp, Reg gtmp)
{
    if (isI32(w)) {
        const Reg r = useI32(w, gtmp);
        a_.cvtsi2sd32(xtmp, r);
        return xtmp;
    }
    if (inReg(w))
        return xmmOf(w);
    a_.movsdLoad(xtmp, home(w));
    return xtmp;
}

void Baseline::defF64(std::int32_t w, int x)
{
    if (inReg(w)) {
        if (xmmOf(w) != x)
            a_.movaps(xmmOf(w), x);
    } else {
        a_.movsdStore(home(w), x);
    }
    dirty_[std::size_t(w)] = 1;
}

void Baseline::setConstI32(std::int32_t w, std::int32_t v)
{
    if (inReg(w))
        a_.movImm(gpr(w), std::uint32_t(v));
    else
        a_.mov32(home(w), v);
    dirty_[std::size_t(w)] = 1;
}

void Baseline::spillToFrame(unsigned reg, std::int32_t w)
{
    if (isI32(w)) {
        const Reg r = useI32(w, RAX);
        a_.mov32(RAX, r);
        a_.or_(RAX, R14);
    } else {
        const int x = operandF64(w, 0, RAX);
        if (x != 0)
            a_.movaps(0, x);
        boxDouble0();
    }
    write(reg, RAX);
    dirty_[std::size_t(w)] = 0;
}

void Baseline::materializeReads(std::uint32_t i)
{
    const opt::Effects e = opt::effectsOf(*code_, i);
    const auto& uw = plan_->useWebs[i];
    for (std::size_t k = 0; k < e.rd.size(); ++k)
        if (uw[k] >= 0 && dirty_[std::size_t(uw[k])])
            spillToFrame(e.rd[k], uw[k]);
}

void Baseline::materializeLive(std::uint32_t i)
{
    for (const opt::LiveWeb& lw : plan_->liveBefore[i])
        if (dirty_[lw.web])
            spillToFrame(lw.reg, std::int32_t(lw.web));
}

/// RAX (a boxed value, not counted) into web `w`; `fail` when it is not a number of the web's kind.
void Baseline::unboxRax(std::int32_t w, Label& fail)
{
    if (isI32(w)) {
        needInt(RAX, fail);
        defI32(w, RAX);
    } else {
        needNumber(RAX, fail);
        loadNumber(0, RAX, RDX);
        defF64(w, 0);
    }
}

void Baseline::loadFromFrame(unsigned reg, std::int32_t w, Label& fail)
{
    a_.mov(RAX, R(reg));
    unboxRax(w, fail);
    dirty_[std::size_t(w)] = 0;
}

/// After an instruction that left its result in the frame: the typed variable it is the value of.
void Baseline::loadTypedAfter(std::uint32_t i)
{
    const std::int32_t w = plan_->defWeb[i];
    if (w < 0)
        return;
    const opt::Effects e = opt::effectsOf(*code_, i);
    loadFromFrame(e.wr[0], w, exitAfter(i));
}

void Baseline::afterGeneric(std::uint32_t i)
{
    if (!optimizing() || plan_->defWeb[i] < 0)
        return;
    delivered_ = true;
    loadTypedAfter(i);
}

void Baseline::savePool(std::uint32_t i)
{
    if (!optimizing())
        return;
    for (const opt::LiveWeb& lw : plan_->liveBefore[i]) {
        const std::int32_t w = std::int32_t(lw.web);
        if (!inReg(w))
            continue;
        if (isI32(w))
            a_.mov32(home(w), gpr(w));
        else
            a_.movsdStore(home(w), xmmOf(w));
    }
}

void Baseline::restorePool(std::uint32_t i)
{
    if (!optimizing())
        return;
    for (const opt::LiveWeb& lw : plan_->liveBefore[i]) {
        const std::int32_t w = std::int32_t(lw.web);
        if (!inReg(w))
            continue;
        if (isI32(w))
            a_.mov32(gpr(w), home(w));
        else
            a_.movsdLoad(xmmOf(w), home(w));
    }
}

Label& Baseline::exitLabel(std::uint32_t i, int kind)
{
    const std::uint64_t key = (std::uint64_t(i) << 1) | std::uint64_t(kind);
    auto it = exitIndex_.find(key);
    if (it != exitIndex_.end())
        return *it->second;
    exits_.push_back(ExitReq{i, kind, Label{}});
    exitIndex_[key] = &exits_.back().label;
    return exits_.back().label;
}

void Baseline::deliver(std::uint32_t i, unsigned dst)
{
    const std::int32_t w = optimizing() ? plan_->defWeb[i] : -1;
    if (w < 0) {
        retain(RAX);
        write(dst, RAX);
        return;
    }
    delivered_ = true;
    unboxRax(w, exitBefore(i));
}

void Baseline::deliverInt(std::uint32_t i, unsigned dst)
{
    const std::int32_t w = optimizing() ? plan_->defWeb[i] : -1;
    if (w < 0) {
        a_.or_(RAX, R14);
        write(dst, RAX);
        return;
    }
    delivered_ = true;
    if (isI32(w)) {
        defI32(w, RAX);
    } else {
        a_.cvtsi2sd32(0, RAX);
        defF64(w, 0);
    }
}

void Baseline::deliverDouble(std::uint32_t i, unsigned dst)
{
    const std::int32_t w = optimizing() ? plan_->defWeb[i] : -1;
    if (w < 0) {
        boxDouble0();
        write(dst, RAX);
        return;
    }
    delivered_ = true;
    if (!isI32(w)) {
        defF64(w, 0);
        return;
    }
    // A double for an int32 web: only if it is one.
    Label& bad = exitBefore(i);
    Label ok;
    a_.cvttsd2si32(RAX, 0);
    a_.cvtsi2sd32(1, RAX);
    a_.ucomisd(0, 1);
    a_.jcc(CNE, bad);
    a_.jcc(CP, bad);
    a_.test32(RAX, RAX);
    a_.jcc(CNE, ok);
    a_.movqFromX(RDX, 0);
    a_.test(RDX, RDX);
    a_.jcc(CS, bad);  // a negative zero
    a_.bind(ok);
    defI32(w, RAX);
}

/// The int32 a number web converts to (ToInt32 of a double that is in range), in `dst`.
void Baseline::int32OfWeb(std::int32_t w, Reg dst, std::uint32_t exitAt)
{
    if (isI32(w)) {
        const Reg r = useI32(w, dst);
        if (r != dst)
            a_.mov32(dst, r);
        return;
    }
    const int x = operandF64(w, 0, dst);
    a_.cvttsd2si32(dst, x);
    a_.aluI32(7, dst, INT32_MIN);
    a_.jcc(CE, exitBefore(exitAt));
}

namespace {

std::uint64_t doubleBits(double d)
{
    std::uint64_t b;
    std::memcpy(&b, &d, sizeof b);
    return b;
}

}  // namespace

bool Baseline::emitTyped(std::uint32_t i)
{
    const Ins& in = code_->code[i];
    const std::int32_t dw = plan_->defWeb[i];
    const auto& uw = plan_->useWebs[i];
    const Op op = in.opcode();
    auto allTyped = [&]() {
        for (std::int32_t w : uw)
            if (w < 0)
                return false;
        return true;
    };
    switch (op) {
    case Op::LoadInt:
        if (dw < 0)
            return false;
        if (isI32(dw)) {
            setConstI32(dw, in.imm());
        } else {
            a_.movImm(RAX, doubleBits(double(in.imm())));
            a_.movqToX(0, RAX);
            defF64(dw, 0);
        }
        return true;
    case Op::LoadK: {
        if (dw < 0)
            return false;
        const JSValue k = code_->consts[in.b];
        if (!k.isNumber())
            return false;
        if (isI32(dw)) {
            if (!k.isInt32())
                return false;
            setConstI32(dw, k.asInt32());
        } else {
            a_.movImm(RAX, doubleBits(k.asNumber()));
            a_.movqToX(0, RAX);
            defF64(dw, 0);
        }
        return true;
    }
    case Op::Mov:
        if (dw < 0 || !allTyped())
            return false;
        if (isI32(uw[0])) {
            const Reg r = useI32(uw[0], RAX);
            if (isI32(dw)) {
                defI32(dw, r);
            } else {
                a_.cvtsi2sd32(0, r);
                defF64(dw, 0);
            }
        } else {
            if (isI32(dw))
                return false;
            defF64(dw, operandF64(uw[0], 0, RAX));
        }
        return true;
    case Op::Add: case Op::Sub: case Op::Mul: case Op::Div: case Op::Mod: case Op::BitAnd: case Op::BitOr: case Op::BitXor:
    case Op::Shl: case Op::Shr: case Op::UShr:
    case Op::AddI: case Op::SubI: case Op::MulI: case Op::ModI: case Op::BitAndI: case Op::BitOrI: case Op::BitXorI:
    case Op::ShlI: case Op::ShrI: case Op::UShrI: case Op::Inc: case Op::Dec: case Op::Neg: {
        if (dw < 0 || !allTyped())
            return false;
        const bool imm = op == Op::AddI || op == Op::SubI || op == Op::MulI || op == Op::ModI || op == Op::BitAndI ||
                         op == Op::BitOrI || op == Op::BitXorI || op == Op::ShlI || op == Op::ShrI || op == Op::UShrI;
        const bool unary = op == Op::Inc || op == Op::Dec || op == Op::Neg;
        const std::int32_t k = imm ? std::int32_t(std::int16_t(in.c)) : 0;
        const bool bitwise = op == Op::BitAnd || op == Op::BitOr || op == Op::BitXor || op == Op::Shl || op == Op::Shr ||
                             op == Op::UShr || op == Op::BitAndI || op == Op::BitOrI || op == Op::BitXorI ||
                             op == Op::ShlI || op == Op::ShrI || op == Op::UShrI;
        Label& bad = exitBefore(i);
        if (bitwise) {
            int32OfWeb(uw[0], RAX, i);
            if (!imm)
                int32OfWeb(uw[1], RCX, i);
            switch (op) {
            case Op::BitAnd: a_.alu32(4, RAX, RCX); break;
            case Op::BitOr: a_.alu32(1, RAX, RCX); break;
            case Op::BitXor: a_.alu32(6, RAX, RCX); break;
            case Op::Shl: a_.shiftCl32(4, RAX); break;
            case Op::Shr: a_.shiftCl32(7, RAX); break;
            case Op::UShr: a_.shiftCl32(5, RAX); break;
            case Op::BitAndI: a_.aluI32(4, RAX, k); break;
            case Op::BitOrI:
                if (k != 0)
                    a_.aluI32(1, RAX, k);
                break;
            case Op::BitXorI:
                if (k != 0)
                    a_.aluI32(6, RAX, k);
                break;
            case Op::ShlI: a_.shiftI32(4, RAX, k & 31); break;
            case Op::ShrI: a_.shiftI32(7, RAX, k & 31); break;
            default: a_.shiftI32(5, RAX, k & 31); break;
            }
            if (op == Op::UShr || op == Op::UShrI) {
                if (isI32(dw)) {
                    a_.test32(RAX, RAX);
                    a_.jcc(CS, bad);
                } else {
                    a_.cvtsi2sd64(0, RAX);
                    defF64(dw, 0);
                    return true;
                }
            }
            if (!isI32(dw))
                return false;
            defI32(dw, RAX);
            return true;
        }
        if (isI32(dw)) {
            for (std::int32_t w : uw)
                if (!isI32(w))
                    return false;
            if (op == Op::Mod || op == Op::ModI) {
                if (op == Op::ModI && k <= 0)
                    return false;
                if (op == Op::ModI) {
                    // A non-negative dividend and a constant divisor: a mask, or a multiply by the reciprocal.
                    a_.mov32(RAX, useI32(uw[0], RAX));
                    a_.test32(RAX, RAX);
                    a_.jcc(CS, bad);
                    const std::uint32_t d = std::uint32_t(k);
                    if ((d & (d - 1)) == 0) {
                        a_.aluI32(4, RAX, std::int32_t(d - 1));
                        defI32(dw, RAX);
                        return true;
                    }
                    int l = 0;
                    while ((std::uint64_t(1) << l) < d)
                        ++l;
                    const std::uint64_t m = (std::uint64_t(1) << (32 + l)) / d + 1;
                    a_.movImm(RCX, m);
                    a_.imul64(RCX, RAX);
                    a_.shiftI(5, RCX, 32 + l);
                    a_.imulI32(RCX, RCX, std::int32_t(d));
                    a_.alu32(5, RAX, RCX);
                    defI32(dw, RAX);
                    return true;
                }
                a_.mov32(RCX, useI32(uw[1], RCX));
                a_.test32(RCX, RCX);
                a_.jcc(CLE, bad);
                a_.mov32(RAX, useI32(uw[0], RAX));
                a_.test32(RAX, RAX);
                a_.jcc(CS, bad);
                a_.cdq();
                a_.idiv32(RCX);
                defI32(dw, RDX);
                return true;
            }
            if (op == Op::Div) {
                const Reg c = useI32(uw[1], RCX);
                if (c != RCX)
                    a_.mov32(RCX, c);
                a_.test32(RCX, RCX);
                a_.jcc(CE, bad);
                a_.aluI32(7, RCX, -1);
                a_.jcc(CE, bad);
                a_.mov32(RAX, useI32(uw[0], RAX));
                a_.cdq();
                a_.idiv32(RCX);
                a_.test32(RDX, RDX);
                a_.jcc(CNE, bad);  // not exact
                Label ok;
                a_.test32(RAX, RAX);
                a_.jcc(CNE, ok);
                a_.test32(RCX, RCX);
                a_.jcc(CS, bad);  // a negative zero
                a_.bind(ok);
                defI32(dw, RAX);
                return true;
            }
            const Reg a = useI32(uw[0], RAX);
            a_.mov32(RAX, a);
            switch (op) {
            case Op::Add: a_.alu32(0, RAX, useI32(uw[1], RCX)); break;
            case Op::Sub: a_.alu32(5, RAX, useI32(uw[1], RCX)); break;
            case Op::AddI: a_.aluI32(0, RAX, k); break;
            case Op::SubI: a_.aluI32(5, RAX, k); break;
            case Op::Inc: a_.aluI32(0, RAX, 1); break;
            case Op::Dec: a_.aluI32(0, RAX, -1); break;
            case Op::Neg:
                a_.test32(RAX, RAX);
                a_.jcc(CE, bad);  // -0
                a_.aluI32(7, RAX, INT32_MIN);
                a_.jcc(CE, bad);
                a_.neg32(RAX);
                break;
            case Op::Mul:
            case Op::MulI: {
                if (op == Op::Mul)
                    a_.imul32(RAX, useI32(uw[1], RCX));
                else
                    a_.imulI32(RAX, RAX, k);
                a_.jcc(CO, bad);
                Label ok;
                a_.test32(RAX, RAX);
                a_.jcc(CNE, ok);
                // A zero that is not +0: a negative operand makes it -0.
                if (op == Op::Mul) {
                    a_.mov32(RDX, a);
                    a_.mov32(RCX, useI32(uw[1], RCX));
                    a_.alu32(1, RDX, RCX);
                    a_.jcc(CS, bad);
                } else if (k < 0) {
                    a_.jmp(bad);
                } else if (k == 0) {
                    a_.mov32(RDX, a);
                    a_.test32(RDX, RDX);
                    a_.jcc(CS, bad);
                }
                a_.bind(ok);
                break;
            }
            default:
                return false;
            }
            if ((op == Op::Add || op == Op::Sub || op == Op::AddI || op == Op::SubI || op == Op::Inc || op == Op::Dec) &&
                !plan_->wraps[i])
                a_.jcc(CO, bad);
            defI32(dw, RAX);
            return true;
        }
        // A double result.
        if (op == Op::Mod || op == Op::ModI)
            return false;
        const int x0 = operandF64(uw[0], 0, RAX);
        if (x0 != 0)
            a_.movaps(0, x0);
        if (op == Op::Neg) {
            a_.movImm(RAX, 0x8000000000000000ull);
            a_.movqToX(1, RAX);
            a_.xorps(0, 1);
            defF64(dw, 0);
            return true;
        }
        int x1;
        if (unary) {
            a_.movImm(RAX, doubleBits(op == Op::Inc ? 1.0 : -1.0));
            a_.movqToX(1, RAX);
            x1 = 1;
            a_.addsd(0, x1);
            defF64(dw, 0);
            return true;
        }
        if (imm) {
            a_.movImm(RAX, doubleBits(double(k)));
            a_.movqToX(1, RAX);
            x1 = 1;
        } else {
            x1 = operandF64(uw[1], 1, RCX);
        }
        switch (op) {
        case Op::Add: case Op::AddI: a_.addsd(0, x1); break;
        case Op::Sub: case Op::SubI: a_.subsd(0, x1); break;
        case Op::Mul: case Op::MulI: a_.mulsd(0, x1); break;
        case Op::Div: a_.divsd(0, x1); break;
        default: return false;
        }
        defF64(dw, 0);
        return true;
    }
    case Op::GetProp: {
        // A number read from a property the site has always found in the same place: straight into the variable.
        if (dw < 0 || uw.empty() || uw[0] >= 0)
            return false;
        const std::uint16_t cacheId = code_->code[i + 1].a;
        if (cacheId == kNoCache)
            return false;
        const InlineCache& ic = code_->caches[cacheId];
        if (ic.shape == 0 || ic.next != 0 || (ic.holder && !ic.validity))
            return false;
        Label& bad = exitBefore(i);
        if (ic.holder) {
            provenShape(in.b, ic.shape, bad);
            hold(ic.validity);
            a_.movImm(RCX, reinterpret_cast<std::uint64_t>(ic.validity));
            a_.cmpI32(mem(RCX, kValidityValid), 0);
            a_.jcc(CE, bad);
            a_.movImm(RAX, reinterpret_cast<std::uint64_t>(ic.holder));
            slotAddress(ic.index);
        } else {
            provenSlot(in.b, ic.shape, ic.index, ic.aux, bad);
        }
        a_.mov(RAX, mem(RDX, 0));
        unboxRax(dw, bad);
        return true;
    }
    case Op::GetElem: case Op::PutElem: {
        // An element of the one kind of indexed object the site has seen, at an int32 index: no boxing of the index, one
        // class test, the kind's own load or store.
        const bool put = op == Op::PutElem;
        if (uw.size() < (put ? 3u : 2u) || uw[0] >= 0 || uw[1] < 0 || !isI32(uw[1]))
            return false;
        if (i < code_->prof.size() && (code_->prof[i] & opt::kProfFailed))
            return false;
        using namespace elemprof;
        std::uint8_t ep = i < code_->elemProf.size() ? code_->elemProf[i] : 0;
        if (ep == 0)
            ep = kArray;  // not run yet: the usual case, and a guard that fails says so
        if (ep != kArray && ep != kFloat64 && ep != kInt32 && ep != kUint8)
            return false;
        const std::int32_t vw = put ? uw[2] : -1;
        if (put && ep != kArray && vw < 0)
            return false;
        Label& bad = exitBefore(i);
        Label done;
        a_.mov32(RCX, useI32(uw[1], RCX));
        a_.mov(RAX, R(put ? in.a : in.b));
        needObject(RAX, bad);
        if (ep == kArray) {
            a_.cmpI8(mem(RAX, kHdrCls), std::uint8_t(ObjClass::Array));
            a_.jcc(CNE, bad);
            if (put) {
                a_.testI8(mem(RAX, kHdrFlags), std::uint8_t(cellflags::kFrozen | cellflags::kNotExtensible));
                a_.jcc(CNE, bad);
            }
            a_.test32(RCX, RCX);
            a_.jcc(CS, bad);
            a_.alu32(7, RCX, mem(RAX, kArrayLength));
            a_.jcc(CAE, bad);
            a_.mov(RDX, mem(RAX, kArrayElems));
            if (!put) {
                a_.mov(RAX, mem(RDX, RCX, 8));
                a_.cmpI(RAX, std::int32_t(kHole));
                a_.jcc(CE, bad);
                deliver(i, in.a);
                return true;
            }
            a_.lea(RDX, mem(RDX, RCX, 8));
            // The value, boxed and counted, in RSI; a number leaves the array's all-numbers flag as it is.
            if (vw >= 0) {
                if (isI32(vw)) {
                    a_.mov32(RSI, useI32(vw, RSI));
                    a_.or_(RSI, R14);
                } else {
                    a_.mov(RDI, RAX);
                    const int x = operandF64(vw, 0, RSI);
                    if (x != 0)
                        a_.movaps(0, x);
                    boxDouble0();
                    a_.mov(RSI, RAX);
                    a_.mov(RAX, RDI);
                }
            } else {
                Label number;
                a_.mov(RSI, R(in.c));
                retain(RSI);
                a_.test(RSI, R14);
                a_.jcc(CNE, number);
                a_.andI8(mem(RAX, kHdrFlags), std::uint8_t(~cellflags::kAllNumbers));
                a_.bind(number);
            }
            Label hole;
            a_.mov(RDI, mem(RDX, 0));
            a_.cmpI(RDI, std::int32_t(kHole));
            a_.jcc(CE, hole);
            a_.mov(mem(RDX, 0), RSI);
            release(RDI);
            a_.jmp(done);
            // A hole of a plain array whose prototype chain is as it was: nothing can intercept the store. Otherwise the
            // value's count is given back and the instruction runs again in the interpreter.
            a_.bind(hole);
            Label undo;
            if (plainHoleGuard(RDI, undo)) {
                a_.mov(mem(RDX, 0), RSI);
                a_.jmp(done);
            }
            a_.bind(undo);
            if (vw < 0)
                release(RSI);
            a_.jmp(bad);
            a_.bind(done);
            return true;
        }
        a_.cmpI8(mem(RAX, kHdrCls), std::uint8_t(ObjClass::TypedArray));
        a_.jcc(CNE, bad);
        const ElemKind kind = ep == kFloat64 ? ElemKind::Float64 : ep == kInt32 ? ElemKind::Int32 : ElemKind::Uint8;
        a_.cmpI8(mem(RAX, kTypedKind), std::uint8_t(kind));
        a_.jcc(CNE, bad);
        a_.alu32(7, RCX, mem(RAX, kTypedLength));
        a_.jcc(CAE, put ? done : bad);  // a store to no element (past the end, or a negative index) does nothing
        a_.mov(RSI, mem(RAX, kTypedData));
        if (!put) {
            if (kind == ElemKind::Float64) {
                a_.movsdLoad(0, mem(RSI, RCX, 8));
                deliverDouble(i, in.a);
            } else {
                if (kind == ElemKind::Int32)
                    a_.mov32(RAX, mem(RSI, RCX, 4));
                else
                    a_.movzx8(RAX, mem(RSI, RCX, 1));
                deliverInt(i, in.a);
            }
            return true;
        }
        if (kind == ElemKind::Float64) {
            const int x = operandF64(vw, 0, RAX);
            a_.movsdStore(mem(RSI, RCX, 8), x);
        } else {
            int32OfWeb(vw, RAX, i);
            if (kind == ElemKind::Int32)
                a_.mov32(mem(RSI, RCX, 4), RAX);
            else
                a_.mov8(mem(RSI, RCX, 1), RAX);
        }
        a_.bind(done);
        return true;
    }
    case Op::PutProp: {
        // A number stored to an existing own data property: boxed in place, no trip through the frame.
        if (uw.size() < 2 || uw[0] >= 0 || uw[1] < 0)
            return false;
        const std::uint16_t cacheId = code_->code[i + 1].a;
        if (cacheId == kNoCache)
            return false;
        const InlineCache& ic = code_->caches[cacheId];
        if (ic.shape == 0 || ic.holder || ic.next != 0)
            return false;
        Label& bad = exitBefore(i);
        provenSlot(in.a, ic.shape, ic.index, ic.aux, bad);
        const std::int32_t vw = uw[1];
        if (isI32(vw)) {
            const Reg r = useI32(vw, RCX);
            a_.mov32(RCX, r);
            a_.or_(RCX, R14);
        } else {
            const int x = operandF64(vw, 0, RCX);
            if (x != 0)
                a_.movaps(0, x);
            boxDouble0();
            a_.mov(RCX, RAX);
        }
        a_.mov(RSI, mem(RDX, 0));
        a_.mov(mem(RDX, 0), RCX);
        release(RSI);
        return true;
    }
    case Op::Call: {
        // `Math.sqrt(x)` and the other one-argument `Math` functions on a number: the callee is guarded to be the function it
        // was when compiled, and the double function is run directly (`sqrt` as one instruction).
        if (in.c != 1 || dw < 0 || uw.size() < 3 || uw[2] < 0)
            return false;
        const JSValue callee = constantBefore(i, in.b);
        if (!isFunction(callee))
            return false;
        auto* fn = static_cast<JSFunction*>(asObject(callee));
        if (!(fn->flags & JSFunction::kNative) || !fn->rare() || fn->rare()->native != mathUnaryNative())
            return false;
        void* const target = fn->rare()->nativeData;
        holdValue(callee);
        Label& bad = exitBefore(i);
        a_.movImm(RCX, callee.bits);
        a_.cmp(RCX, R(in.b));
        a_.jcc(CNE, bad);
        const int x = operandF64(uw[2], 0, RAX);
        if (x != 0)
            a_.movaps(0, x);
        if (mathUnaryIsSqrt(target)) {
            a_.sqrtsd(0, 0);
        } else {
            savePool(i);
            a_.movImm(RAX, reinterpret_cast<std::uint64_t>(target));
            a_.callReg(RAX);
            restorePool(i);
        }
        if (isI32(dw)) {
            // An int32 web for the result: only if it is one; otherwise the number goes to the frame and the code leaves.
            Label ok, notInt, done;
            a_.cvttsd2si32(RAX, 0);
            a_.cvtsi2sd32(1, RAX);
            a_.ucomisd(0, 1);
            a_.jcc(CNE, notInt);
            a_.jcc(CP, notInt);
            a_.test32(RAX, RAX);
            a_.jcc(CNE, ok);
            a_.movqFromX(RDX, 0);
            a_.test(RDX, RDX);
            a_.jcc(CS, notInt);
            a_.bind(ok);
            defI32(dw, RAX);
            a_.jmp(done);
            a_.bind(notInt);
            boxDouble0();
            write(in.a, RAX);
            a_.jmp(exitAfter(i));
            a_.bind(done);
        } else {
            defF64(dw, 0);
        }
        return true;
    }
    case Op::JTrue: case Op::JFalse: case Op::JLt: case Op::JLe: case Op::JGt: case Op::JGe: case Op::JNotLt: case Op::JNotLe:
    case Op::JNotGt: case Op::JNotGe: case Op::JLtI: case Op::JLeI: case Op::JGtI: case Op::JGeI: case Op::JNotLtI:
    case Op::JNotLeI: case Op::JNotGtI: case Op::JNotGeI: case Op::JStrictEq: case Op::JStrictNe: case Op::JStrictEqI:
    case Op::JStrictNeI:
        if ((op == Op::JTrue || op == Op::JFalse) && !uw.empty() && uw[0] < 0) {
            // A value in the frame: booleans, int32s, undefined and null here; anything else in the interpreter, so the
            // variables need not be written back on the way.
            const std::uint32_t target = i + 1 + std::uint32_t(in.jump());
            Label& bad = exitBefore(i);
            Label taken, notTaken;
            const bool jt = op == Op::JTrue;
            a_.mov(RAX, R(in.a));
            a_.cmpI(RAX, std::int32_t(kTrue));
            a_.jcc(CE, jt ? taken : notTaken);
            a_.cmpI(RAX, std::int32_t(kFalse));
            a_.jcc(CE, jt ? notTaken : taken);
            a_.cmpI(RAX, std::int32_t(kUndefined));
            a_.jcc(CE, jt ? notTaken : taken);
            a_.cmpI(RAX, std::int32_t(kNull));
            a_.jcc(CE, jt ? notTaken : taken);
            needInt(RAX, bad);
            a_.test32(RAX, RAX);
            a_.jcc(jt ? CNE : CE, taken);
            a_.jmp(notTaken);
            a_.bind(taken);
            go(i, target);
            a_.bind(notTaken);
            return true;
        }
        if (!allTyped())
            return false;
        typedJump(i, in);
        return true;
    default:
        return false;
    }
}

void Baseline::typedJump(std::uint32_t i, const Ins& in)
{
    const Op op = in.opcode();
    const auto& uw = plan_->useWebs[i];
    if (op == Op::JTrue || op == Op::JFalse) {
        const std::uint32_t target = i + 1 + std::uint32_t(in.jump());
        if (isI32(uw[0])) {
            const Reg r = useI32(uw[0], RAX);
            a_.test32(r, r);
            goIf(op == Op::JTrue ? CNE : CE, i, target);
        } else {
            const int x = operandF64(uw[0], 0, RAX);
            a_.xorps(1, 1);
            a_.ucomisd(x, 1);
            if (op == Op::JTrue) {
                Label no;
                a_.jcc(CP, no);
                goIf(CNE, i, target);
                a_.bind(no);
            } else {
                Label yes, no;
                a_.jcc(CP, yes);
                a_.jcc(CNE, no);
                a_.bind(yes);
                go(i, target);
                a_.bind(no);
            }
        }
        return;
    }
    const bool imm = op == Op::JLtI || op == Op::JLeI || op == Op::JGtI || op == Op::JGeI || op == Op::JNotLtI ||
                     op == Op::JNotLeI || op == Op::JNotGtI || op == Op::JNotGeI || op == Op::JStrictEqI || op == Op::JStrictNeI;
    const std::uint32_t target = i + 1 + std::uint32_t(std::int32_t(in.jump16()));
    const std::int32_t k = imm ? std::int32_t(std::int16_t(in.b)) : 0;
    const bool strict = op == Op::JStrictEq || op == Op::JStrictNe || op == Op::JStrictEqI || op == Op::JStrictNeI;
    const Relation rel = relationOf(op);
    const bool bothInt = isI32(uw[0]) && (imm || isI32(uw[1]));
    if (bothInt) {
        const Reg a = useI32(uw[0], RAX);
        if (imm) {
            a_.aluI32(7, a, k);
        } else {
            const Reg b = useI32(uw[1], RCX);
            a_.alu32(7, a, b);
        }
        goIf(rel.ic, i, target);
        return;
    }
    const int xa = operandF64(uw[0], 0, RAX);
    int xb;
    if (imm) {
        a_.movImm(RCX, doubleBits(double(k)));
        a_.movqToX(1, RCX);
        xb = 1;
    } else {
        xb = operandF64(uw[1], 1, RCX);
    }
    if (strict) {
        a_.ucomisd(xa, xb);
        if (op == Op::JStrictEq || op == Op::JStrictEqI) {
            Label no;
            a_.jcc(CP, no);
            goIf(CE, i, target);
            a_.bind(no);
        } else {
            Label yes, no;
            a_.jcc(CP, yes);
            a_.jcc(CE, no);
            a_.bind(yes);
            go(i, target);
            a_.bind(no);
        }
        return;
    }
    if (rel.swap)
        a_.ucomisd(xb, xa);
    else
        a_.ucomisd(xa, xb);
    goIf(rel.dc, i, target);
}

void Baseline::emitOpt(std::uint32_t i)
{
    if (plan_->isLeader[i]) {
        std::fill(dirty_.begin(), dirty_.end(), 1);
        forgetFacts();
    }
    delivered_ = false;
    if (emitTyped(i)) {
        for (std::uint16_t r : opt::effectsOf(*code_, i).wr)
            if (r < factShape_.size()) {
                factShape_[r] = 0;
                factAux_[r] = -1;
            }
        return;
    }
    forgetFacts();
    materializeReads(i);
    if (opt::jumpTarget(*code_, i) >= 0)
        materializeLive(i);
    emit(i);
    if (plan_->defWeb[i] >= 0 && !delivered_)
        loadTypedAfter(i);
}

void Baseline::emitExits()
{
    for (ExitReq& e : exits_) {
        a_.bind(e.label);
        const bool after = e.kind == 1;
        const std::uint32_t at = after ? e.index + length(e.index) : e.index;
        const std::int32_t skip = after ? plan_->defWeb[e.index] : -1;
        if (at < n_)
            for (const opt::LiveWeb& lw : plan_->liveBefore[at])
                if (std::int32_t(lw.web) != skip)
                    spillToFrame(lw.reg, std::int32_t(lw.web));
        a_.aluI(0, RSP, spill_);
        a_.mov(RDI, R12);
        a_.movImm(RSI, e.index);
        a_.movImm(RDX, after ? 1 : 0);
        a_.movImm(RAX, reinterpret_cast<std::uint64_t>(&scriptVmOptExit));
        a_.callReg(RAX);
        Label notFall;
        a_.cmpI(RAX, std::int32_t(kStepFall));
        a_.jcc(CNE, notFall);
        a_.movImm(RAX, reinterpret_cast<std::uint64_t>(base_->addressFor(e.index + length(e.index))));
        a_.jmp(RAX);
        a_.bind(notFall);
        a_.jmp(tailBase_);
    }
}

void Baseline::emitOptTails()
{
    for (std::size_t k = 0; k < tickStubs_.size(); ++k) {
        auto& t = tickStubs_[k];
        a_.bind(t.second);
        savePool(t.first);
        a_.mov(RDI, R12);
        a_.call(tick_);
        a_.test32(RAX, RAX);
        a_.jcc(CNE, exception(t.first));
        restorePool(t.first);
        a_.jmp(tickBack_[k]);
    }
    emitExits();
    a_.bind(entryFail_);
    a_.aluI(0, RSP, spill_);
    a_.movImm(RAX, reinterpret_cast<std::uint64_t>(base_->addressFor(0)));
    a_.jmp(RAX);
}

Tier2Code* Baseline::compileOptimized()
{
    const std::uint32_t W = plan_->typedWebs;
    spill_ = std::int32_t((8 * W + 15) & ~15u);
    dirty_.assign(W, 1);
    factShape_.assign(code_->nregs + 1, 0u);
    factAux_.assign(code_->nregs + 1, -1);
    a_.push(RBX);
    a_.push(R13);
    a_.aluI(5, RSP, 8 + spill_);
    a_.mov(R13, RSI);
    a_.mov(RBX, mem(R13, kFrameR));
    for (const opt::LiveWeb& lw : plan_->liveBefore[0])
        loadFromFrame(lw.reg, std::int32_t(lw.web), entryFail_);
    for (std::uint32_t i = 0; i < n_;) {
        a_.bind(at_[i]);
        offsets_[i] = std::uint32_t(a_.here());
        emitOpt(i);
        i += length(i);
    }
    a_.bind(at_[n_]);
    a_.int3();
    std::vector<std::pair<std::uint32_t, std::uint32_t>> osr;
    for (std::uint32_t h : plan_->loopHeaders) {
        osr.emplace_back(h, std::uint32_t(a_.here()));
        Label fail;
        a_.aluI(5, RSP, spill_);
        for (const opt::LiveWeb& lw : plan_->liveBefore[h])
            loadFromFrame(lw.reg, std::int32_t(lw.web), fail);
        a_.jmp(at_[h]);
        a_.bind(fail);
        a_.aluI(0, RSP, spill_);
        a_.movImm(RAX, reinterpret_cast<std::uint64_t>(base_->addressFor(h)));
        a_.jmp(RAX);
    }
    emitTails();
    CodeSpan span = allocateCode(a_.buf.size());
    if (!span) {
        for (ValidityCell* v : validities_)
            vm_.releaseValidity(v);
        for (JSValue v : held_)
            vm::release(v);
        return nullptr;
    }
    std::memcpy(span.rw, a_.buf.data(), a_.buf.size());
#if defined(SCRIPT_JIT_ENV_OVERRIDE)
    if (const char* dumpFile = std::getenv("T2DUMP")) {
        if (std::FILE* f = std::fopen(dumpFile, "wb")) {
            std::fwrite(a_.buf.data(), 1, a_.buf.size(), f);
            std::fclose(f);
        }
    }
#endif
    auto* t = new Tier2Code;
    t->rx = span.rx;
    t->bytes = span.bytes;
    t->entry = t->rx;
    t->osr = std::move(osr);
    t->validities = std::move(validities_);
    t->held = std::move(held_);
    return t;
}

/// The baseline code's count of entries and back edges: when it runs out the optimizing tier is asked for.
void Baseline::emitCounter(std::uint32_t i, std::int64_t target)
{
    counterStubs_.push_back(CounterStub{i, target, Label{}, Label{}});
    CounterStub& c = counterStubs_.back();
    a_.movImm(RAX, reinterpret_cast<std::uint64_t>(&code_->tier2Counter));
    a_.dec32(mem(RAX, 0));
    a_.jcc(CLE, c.stub);
    a_.bind(c.back);
}
