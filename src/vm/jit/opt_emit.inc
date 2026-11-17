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
    raxHolds_ = src == RAX ? w : -1;
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
        boxDoubleTo(RAX, operandF64(w, 0, RAX), tame(w));
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
void Baseline::unboxRax(std::int32_t w, Label& fail, bool number)
{
    if (isI32(w)) {
        needInt(RAX, fail);
        defI32(w, RAX);
    } else if (inReg(w)) {
        unboxDouble(xmmOf(w), RAX, RDX, number ? nullptr : &fail);
        dirty_[std::size_t(w)] = 1;
    } else {
        unboxDouble(0, RAX, RDX, number ? nullptr : &fail);
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

/// Whether web `w`, live before instruction `i`, is still wanted after it: what a call out at `i` must keep. With
/// handlers in the function every one is kept (one may catch at `i` and want them).
bool Baseline::liveAcross(std::uint32_t i, std::int32_t w) const
{
    const std::uint32_t next = i + length(i);
    if (!code_->handlers.empty() || next >= n_ || opt::jumpTarget(*code_, i) >= 0)
        return true;  // (after a jump, what is wanted is what its target wants)
    for (const opt::LiveWeb& lw : plan_->liveBefore[next])
        if (std::int32_t(lw.web) == w)
            return true;
    return false;
}

void Baseline::savePool(std::uint32_t i, bool all)
{
    if (!optimizing())
        return;
    for (const opt::LiveWeb& lw : plan_->liveBefore[i]) {
        const std::int32_t w = std::int32_t(lw.web);
        if (!inReg(w) || (!all && !liveAcross(i, w)))
            continue;
        if (isI32(w))
            a_.mov32(home(w), gpr(w));
        else
            a_.movsdStore(home(w), xmmOf(w));
    }
}

void Baseline::restorePool(std::uint32_t i, bool all)
{
    if (!optimizing())
        return;
    for (const opt::LiveWeb& lw : plan_->liveBefore[i]) {
        const std::int32_t w = std::int32_t(lw.web);
        if (!inReg(w) || (!all && !liveAcross(i, w)))
            continue;
        if (isI32(w))
            a_.mov32(gpr(w), home(w));
        else
            a_.movsdLoad(xmmOf(w), home(w));
    }
}

Label& Baseline::exitLabel(std::uint32_t i, int kind)
{
    // Inside a read chain every exit runs the chain again from its start: nothing in it has an effect.
    if (optimizing() && i < chainHead_.size() && chainHead_[i] >= 0 && std::uint32_t(chainHead_[i]) != i) {
        i = std::uint32_t(chainHead_[i]);
        kind = 0;
    }
    if (optimizing() && i < exitRedirect_.size() && exitRedirect_[i] >= 0) {
        i = std::uint32_t(exitRedirect_[i]);
        kind = 0;
    }
    // Exits taken where registers are held without a count are their own: the count is taken first.
    std::uint64_t ctx = 0;
    std::vector<unsigned> own;
    std::vector<std::size_t> promos;
    if (optimizing()) {
        own.assign(fnBorrow_.begin(), fnBorrow_.end());  // empty unless `borrowInfo_`
        for (std::size_t k = 0; k < borrows_.size(); ++k)
            if (borrows_[k].from <= factAt_ && factAt_ <= borrows_[k].to) {
                ctx |= std::uint64_t(1) << (k % 10);
                own.push_back(borrows_[k].reg);
            }
    }
    if (optimizing() && !loopPromos_.empty()) {
        for (std::size_t k : promosAt(factAt_))
            if (std::int64_t(loopPromos_[k].from) != emittingGuards_) {
                promos.push_back(k);
                ctx |= std::uint64_t(1) << (10 + k % 10);
            }
    }
    const std::uint64_t key = (ctx << 40) | (std::uint64_t(i) << 2) | std::uint64_t(kind);
    auto it = exitIndex_.find(key);
    if (it != exitIndex_.end())
        return *it->second;
    exits_.push_back(ExitReq{i, kind, Label{}, own, promos});
    exitIndex_[key] = &exits_.back().label;
    return exits_.back().label;
}

void Baseline::deliver(std::uint32_t i, unsigned dst)
{
    const std::int32_t w = optimizing() ? plan_->defWeb[i] : -1;
    if (w < 0) {
        if (!fnBorrowed(dst))
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

/// Slots of the optimized frame for the values of inlined calls.
constexpr std::uint32_t kInlineSlots = 32;
/// Slots for method reads that feed an inlined call.
constexpr std::uint32_t kWindowSlots = 4;
constexpr std::uint32_t kHoistSlots = 16;

std::uint64_t doubleBits(double d)
{
    std::uint64_t b;
    std::memcpy(&b, &d, sizeof b);
    return b;
}

}  // namespace

/// Whether the read at `i` finds data properties only, where the site has found them: compiled code does it without the
/// runtime, and leaves when the object is another.
bool Baseline::plainReadable(std::uint32_t i) const
{
    if (i + 1 >= n_ || code_->code[i + 1].opcode() != Op::Cache)
        return false;
    const std::uint16_t cacheId = code_->code[i + 1].a;
    if (cacheId == kNoCache || (i < code_->prof.size() && (code_->prof[i] & opt::kProfFailed)))
        return false;
    const InlineCache& ic = code_->caches[cacheId];
    if (ic.shape == 0 || ic.next != 0 || (ic.holder && !(ic.validity && ic.validity->valid)))
        return false;
    if (ic.pad && ic.pad <= code_->poly.size())
        for (std::uint8_t k = 0; k < code_->poly[ic.pad - 1].n; ++k) {
            const PolyEntry& e = code_->poly[ic.pad - 1].e[k];
            if (!(e.kind == PolyEntry::OwnData || (e.kind == PolyEntry::ProtoData && e.validity && e.validity->valid)))
                return false;
        }
    return true;
}

bool Baseline::emitTyped(std::uint32_t i)
{
    const Ins& in = code_->code[i];
    const std::int32_t dw = plan_->defWeb[i];
    const auto& uw = plan_->useWebs[i];
    const Op op = in.opcode();
    const std::int32_t start = a_.here();
    auto allTyped = [&]() {
        for (std::int32_t w : uw)
            if (w < 0)
                return false;
        return true;
    };
    switch (op) {
    case Op::Ret: {
        // A number: boxed straight into rax, not through the frame, and counted by no one.
        if (uw.empty() || uw[0] < 0)
            return false;
        const std::int32_t w = uw[0];
        if (isI32(w)) {
            const Reg r = useI32(w, RAX);
            a_.mov32(RAX, r);
            a_.or_(RAX, R14);
        } else {
            boxDoubleTo(RAX, operandF64(w, 0, RAX), tame(w));
        }
        if (!dirtyIn_.empty() && i < dirtyIn_.size()) {
            std::vector<std::uint64_t> dirty = dirtyIn_[i];
            dirty[in.a / 64] |= std::uint64_t(1) << (in.a % 64);
            ret(i, in, true, &dirty);
        } else {
            ret(i, in, true);
        }
        return true;
    }
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
        if (web(uw[0]).slot == web(dw).slot && isI32(uw[0]) == isI32(dw)) {
            // Joined by the allocator: one place for both.
            dirty_[std::size_t(dw)] = 1;
            if (carried_ == uw[0])
                raxHolds_ = dw;
            return true;
        }
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
        if (Asm::kThreeOperand && isI32(dw) && isI32(uw[0]) && (imm || unary || isI32(uw[1])) && op != Op::UShr &&
            op != Op::UShrI && op != Op::Mod && op != Op::ModI && op != Op::Div) {
            // Straight from the operands' registers into the result's. An instruction that may leave before it is done
            // (an overflow) must leave its operands as they were: when the result is one of them, the way below, through
            // rax, is taken (but for the counter stepped in place, which undoes its step).
            const bool same = uw[0] == dw || (!imm && !unary && uw[1] == dw);
            const bool checked = !bitwise && !plan_->wraps[i];
            const bool counter = (op == Op::Inc || op == Op::Dec || op == Op::AddI || op == Op::SubI) && uw[0] == dw && inReg(dw);
            if (!(same && checked) && !counter && !(op == Op::MulI && k == 0)) {
                const Reg ra = useI32(uw[0], RAX);
                const Reg rb = imm || unary ? RCX : useI32(uw[1], RCX);
                const Reg rd = inReg(dw) ? gpr(dw) : RAX;
                switch (op) {
                case Op::BitAnd: a_.op32(4, rd, ra, rb); break;
                case Op::BitOr: a_.op32(1, rd, ra, rb); break;
                case Op::BitXor: a_.op32(6, rd, ra, rb); break;
                case Op::Shl: a_.shift32(4, rd, ra, rb); break;
                case Op::Shr: a_.shift32(7, rd, ra, rb); break;
                case Op::BitAndI: a_.op32I(4, rd, ra, k); break;
                case Op::BitOrI:
                    if (k != 0)
                        a_.op32I(1, rd, ra, k);
                    else if (rd != ra)
                        a_.mov32(rd, ra);
                    break;
                case Op::BitXorI:
                    if (k != 0)
                        a_.op32I(6, rd, ra, k);
                    else if (rd != ra)
                        a_.mov32(rd, ra);
                    break;
                case Op::ShlI: a_.shift32I(4, rd, ra, k & 31); break;
                case Op::ShrI: a_.shift32I(7, rd, ra, k & 31); break;
                case Op::Add: a_.op32(0, rd, ra, rb); break;
                case Op::Sub: a_.op32(5, rd, ra, rb); break;
                case Op::AddI: a_.op32I(0, rd, ra, k); break;
                case Op::SubI: a_.op32I(5, rd, ra, k); break;
                case Op::Inc: a_.op32I(0, rd, ra, 1); break;
                case Op::Dec: a_.op32I(0, rd, ra, -1); break;
                case Op::Neg:
                    a_.test32(ra, ra);
                    a_.jcc(CE, bad);  // -0
                    a_.aluI32(7, ra, INT32_MIN);
                    a_.jcc(CE, bad);
                    a_.neg32r(rd, ra);
                    break;
                case Op::Mul:
                case Op::MulI: {
                    Reg rk = rb;
                    if (op == Op::MulI) {
                        a_.movImm(RCX, std::uint32_t(k));
                        rk = RCX;
                    }
                    a_.mul32(rd, ra, rk);
                    if (!checked)
                        break;
                    a_.jcc(CO, bad);
                    Label ok;
                    a_.test32(rd, rd);
                    a_.jcc(CNE, ok);
                    // A zero that is not +0: a negative operand makes it -0.
                    if (op == Op::Mul) {
                        a_.mov32(RDX, useI32(uw[0], RDX));
                        a_.alu32(1, RDX, useI32(uw[1], RCX));
                        a_.jcc(CS, bad);
                    } else if (k < 0) {
                        a_.jmp(bad);
                    }
                    a_.bind(ok);
                    break;
                }
                default: break;
                }
                if (checked && op != Op::Mul && op != Op::MulI && op != Op::Neg)
                    a_.jcc(CO, bad);
                if (rd == RAX) {
                    defI32(dw, RAX);
                } else {
                    dirty_[std::size_t(dw)] = 1;
                    raxHolds_ = -1;
                }
                return true;
            }
        }
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
            if ((op == Op::Inc || op == Op::Dec || op == Op::AddI || op == Op::SubI) && uw[0] == dw && inReg(dw) &&
                !plan_->wraps[i]) {
                // A variable counted in place: an overflow takes the step back out of line before it leaves.
                const std::int32_t step = op == Op::Inc ? 1 : op == Op::Dec ? -1 : op == Op::AddI ? k : -k;
                const Reg r = gpr(dw);
                a_.aluI32(0, r, step);
                if (step == 1 && i < noOverflow_.size() && noOverflow_[i]) {
                    // Below an int32 just before: one more is one still.
                    dirty_[std::size_t(dw)] = 1;
                    raxHolds_ = -1;
                    return true;
                }
                coldUndos_.emplace_back();
                ColdUndo& u = coldUndos_.back();
                u.r = r;
                u.step = step;
                u.bad = &bad;
                a_.jcc(CO, u.entry);
                dirty_[std::size_t(dw)] = 1;
                raxHolds_ = -1;
                return true;
            }
            if (uw[0] != carried_) {
                const Reg a = useI32(uw[0], RAX);
                a_.mov32(RAX, a);
            }
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
                if (plan_->wraps[i])
                    break;  // only its low 32 bits matter, and -0 is 0 there
                a_.jcc(CO, bad);
                Label ok;
                a_.test32(RAX, RAX);
                a_.jcc(CNE, ok);
                // A zero that is not +0: a negative operand makes it -0.
                // The operands from their variables (rax holds the product).
                if (op == Op::Mul) {
                    a_.mov32(RDX, useI32(uw[0], RDX));
                    a_.mov32(RCX, useI32(uw[1], RCX));
                    a_.alu32(1, RDX, RCX);
                    a_.jcc(CS, bad);
                } else if (k < 0) {
                    a_.jmp(bad);
                } else if (k == 0) {
                    a_.mov32(RDX, useI32(uw[0], RDX));
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
        if (op == Op::Neg || unary) {
            if (x0 != 0)
                a_.movaps(0, x0);
        }
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
        auto apply = [&](int d, int src) {
            switch (op) {
            case Op::Add: case Op::AddI: a_.addsd(d, src); break;
            case Op::Sub: case Op::SubI: a_.subsd(d, src); break;
            case Op::Mul: case Op::MulI: a_.mulsd(d, src); break;
            case Op::Div: a_.divsd(d, src); break;
            default: break;
            }
        };
        if (op != Op::Add && op != Op::AddI && op != Op::Sub && op != Op::SubI && op != Op::Mul && op != Op::MulI &&
            op != Op::Div)
            return false;
        const std::uint8_t vop = op == Op::Add || op == Op::AddI ? 0x58 : op == Op::Sub || op == Op::SubI ? 0x5C
                                 : op == Op::Mul || op == Op::MulI ? 0x59 : 0x5E;
        // Three operands with AVX: `d = a op b` in one instruction.
        if (inReg(dw) && hasAvx()) {
            a_.vsd(vop, xmmOf(dw), x0, x1);
            dirty_[std::size_t(dw)] = 1;
            if (!imm && !isI32(uw[0]) && !isI32(uw[1]))
                passRax(start);
            return true;
        }
        // Straight into the variable's own register when it has one: `d = a op b` as `d = a; d op= b`.
        if (inReg(dw) && xmmOf(dw) != x1) {
            const int d = xmmOf(dw);
            const int a = x0 != 0 ? x0 : 0;
            if (d != a)
                a_.movaps(d, a);
            apply(d, x1);
            dirty_[std::size_t(dw)] = 1;
            return true;
        }
        if (inReg(dw) && xmmOf(dw) == x1 && (op == Op::Add || op == Op::AddI || op == Op::Mul || op == Op::MulI)) {
            apply(x1, x0 != 0 ? x0 : 0);
            dirty_[std::size_t(dw)] = 1;
            return true;
        }
        if (hasAvx()) {
            a_.vsd(vop, 0, x0, x1);
        } else {
            if (x0 != 0)
                a_.movaps(0, x0);
            apply(0, x1);
        }
        defF64(dw, 0);
        return true;
    }
    case Op::GetProp: {
        if (inlineGetter(i))
            return true;
        if (uw.empty() || uw[0] >= 0)
            return false;
        if (dw < 0) {
            // Any other value from a data property found where the site has found it: guarded, counted, into the frame. A
            // property found elsewhere leaves the code; nothing here runs script.
            if (!plainReadable(i))
                return false;
            const InlineCache& ic = code_->caches[code_->code[i + 1].a];
            Label& bad = exitBefore(i);
            const bool failed = i < code_->prof.size() && (code_->prof[i] & opt::kProfFailed);
            if (ic.pad && ic.pad <= code_->poly.size() && code_->poly[ic.pad - 1].n) {
                const PolyCache& pc = code_->poly[ic.pad - 1];
                PolySeen seen[5];
                int count = 0;
                seen[count++] = PolySeen{ic.shape, ic.index, ic.holder, ic.validity, failed ? std::uint8_t(0) : ic.aux};
                for (std::uint8_t k = 0; k < pc.n; ++k)
                    seen[count++] = PolySeen{pc.e[k].shape, pc.e[k].index, pc.e[k].holder, pc.e[k].validity,
                                             failed ? std::uint8_t(0) : pc.e[k].aux};
                a_.mov(RAX, R(in.b));
                polyRead(seen, count, bad, int(in.b));
            } else if (ic.holder) {
                provenShape(in.b, ic.shape, bad);
                checkValidity(ic.validity, bad);
                holderRead(ic.holder, ic.index);
            } else {
                a_.mov(RAX, provenSlotMem(in.b, ic.shape, ic.index, failed ? 0 : ic.aux, bad));
            }
            if (!fnBorrowed(in.a))
                retain(RAX);
            write(in.a, RAX);
            return true;
        }
        if (const LoopRead* lr = hoistedRead(i)) {
            // Read before the loop, which stores to it nowhere: from its slot on the stack.
            if (isI32(dw)) {
                const Reg r = inReg(dw) ? gpr(dw) : RAX;
                a_.mov32(r, mem(RSP, lr->slot));
                defI32(dw, r);
            } else {
                const int x = inReg(dw) ? xmmOf(dw) : 0;
                a_.movsdLoad(x, mem(RSP, lr->slot));
                defF64(dw, x);
                passRax(start);
            }
            return true;
        }
        // A number read from a property the site has always found in the same place: straight into the variable.
        const std::uint16_t cacheId = code_->code[i + 1].a;
        if (cacheId == kNoCache)
            return false;
        const InlineCache& ic = code_->caches[cacheId];
        if (ic.shape == 0 || ic.next != 0 || (ic.holder && !ic.validity))
            return false;
        if (ic.pad && ic.pad <= code_->poly.size() && code_->poly[ic.pad - 1].n) {
            // Several shapes: a test for each, the slot found at run time.
            const PolyCache& pc = code_->poly[ic.pad - 1];
            PolySeen seen[5];
            int count = 0;
            seen[count++] = PolySeen{ic.shape, ic.index, ic.holder, ic.validity, ic.aux};
            for (std::uint8_t k = 0; k < pc.n; ++k) {
                const PolyEntry& e = pc.e[k];
                if (e.kind == PolyEntry::OwnData || (e.kind == PolyEntry::ProtoData && e.validity))
                    seen[count++] = PolySeen{e.shape, e.index, e.holder, e.validity, e.aux};
                else
                    return false;
            }
            if (i < code_->prof.size() && (code_->prof[i] & opt::kProfFailed))
                for (int k = 0; k < count; ++k)
                    seen[k].aux = 0;
            Label& bad = exitBefore(i);
            a_.mov(RAX, R(in.b));
            polyRead(seen, count, bad, int(in.b));
            unboxRax(dw, bad);
            return true;
        }
        Label& bad = exitBefore(i);
        if (ic.holder) {
            provenShape(in.b, ic.shape, bad);
            checkValidity(ic.validity, bad);
            holderRead(ic.holder, ic.index);
            unboxRax(dw, bad);
            return true;
        } else {
            // A site whose guard failed before sees objects of this shape with other inline slot counts: the address is
            // worked out each time.
            const bool failed = i < code_->prof.size() && (code_->prof[i] & opt::kProfFailed);
            const std::uint8_t aux = failed ? 0 : ic.aux;
            const Mem m = provenSlotMem(in.b, ic.shape, ic.index, aux, bad);
            std::uint8_t& known = factNum_[{in.b, ic.index}];
            if (!isI32(dw) && aux != 0 && in.a != in.b) {
                // A double through rcx: rax keeps the object for the next read or store of it.
                a_.mov(RCX, m);
                const int x = inReg(dw) ? xmmOf(dw) : 0;
                unboxDoubleInPlace(x, RCX, known ? nullptr : &bad);
                if (inReg(dw))
                    dirty_[std::size_t(dw)] = 1;
                else
                    defF64(dw, 0);
                known = std::max<std::uint8_t>(known, 1);
                keepRax(in.b);
                return true;
            }
            a_.mov(RAX, m);
            if (known == 2 && isI32(dw))
                defI32(dw, RAX);  // an int32 there, seen or stored
            else
                unboxRax(dw, bad, known != 0);
            known = isI32(dw) ? std::uint8_t(2) : std::max<std::uint8_t>(known, 1);
            return true;
        }
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
        const bool notRun = ep == 0;
        if (ep == 0)
            ep = kArray;  // not run yet: the usual case, and a guard that fails says so
        if (ep != kArray && ep != kFloat64 && ep != kInt32 && ep != kUint8)
            return false;
        const std::int32_t vw = put ? uw[2] : -1;
        if (put && ep != kArray && vw < 0)
            return false;
        Label& bad = exitBefore(i);
        Label done;
        // A read that had not run when this code was made says nothing of what it yields: once it has run a while the
        // function is compiled again, and what the read gives can live in a register.
        if (notRun && !put && dw < 0)
            immature(i);
        a_.mov32(RCX, useI32(uw[1], RCX));
        a_.mov(RAX, R(put ? in.a : in.b));
        if (ep != kArray)
            needObject(RAX, bad);
        if (ep == kArray) {
            provenArray(put ? in.a : in.b, put ? 3 : 1, bad);
            // Unsigned: a negative index is past any length (an array has fewer than 2^31 elements).
            if (put && vw >= 0) {
                // A number over one of the array's packed numbers: nothing to look at there, nothing to let go of.
                Label notPacked;
                a_.alu32(7, RCX, mem(RAX, kArrayPacked));
                a_.jcc(CAE, notPacked);
                a_.mov(RDX, mem(RAX, kArrayElems));
                if (isI32(vw)) {
                    a_.mov32(RSI, useI32(vw, RSI));
                    a_.or_(RSI, R14);
                } else {
                    boxDoubleTo(RSI, operandF64(vw, 0, RSI), tame(vw));
                }
                a_.mov(mem(RDX, RCX, 8), RSI);
                a_.jmp(done);
                a_.bind(notPacked);
            }
            a_.alu32(7, RCX, mem(RAX, kArrayLength));
            a_.jcc(CAE, bad);
            a_.mov(RDX, mem(RAX, kArrayElems));
            if (!put) {
                a_.mov(RAX, mem(RDX, RCX, 8));
                a_.cmpI(RAX, std::int32_t(kHole));
                a_.jcc(CE, bad);
                if (auto pg = promoGuards_.find(i); pg != promoGuards_.end()) {
                    std::set<unsigned> seen;
                    for (std::size_t k : pg->second) {
                        // The object a promoted slot belongs to: the code leaves (writing the slot back).
                        if (!seen.insert(loopPromos_[k].reg).second)
                            continue;
                        a_.cmp(RAX, R(loopPromos_[k].reg));
                        a_.jcc(CE, bad);
                    }
                }
                if (plan_->defWeb[i] < 0 && borrowed(i, in.a)) {
                    a_.mov(R(in.a), RAX);  // held by the array: no count, and the old value had none
                    keepRax(in.a);
                    return true;
                }
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
                unpackAt(RAX, RCX);
                a_.bind(number);
            }
            Label hole;
            a_.mov(RDI, mem(RDX, 0));
            a_.cmpI(RDI, std::int32_t(kHole));
            a_.jcc(CE, hole);
            a_.mov(mem(RDX, 0), RSI);
            if (vw >= 0)
                packAt(RAX, RCX);
            release(RDI);
            a_.jmp(done);
            // A hole of a plain array whose prototype chain is as it was: nothing can intercept the store. Otherwise the
            // value's count is given back and the instruction runs again in the interpreter.
            a_.bind(hole);
            Label undo;
            if (plainHoleGuard(RDI, undo)) {
                a_.mov(mem(RDX, 0), RSI);
                if (vw >= 0)
                    packAt(RAX, RCX);
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
        const bool failed = i < code_->prof.size() && (code_->prof[i] & opt::kProfFailed);
        // The value boxed in rdi first (finding the slot uses rax, rcx, rdx and rsi), then stored into the slot.
        const std::int32_t vw = uw[1];
        const std::int32_t boxAt = a_.here();
        if (isI32(vw)) {
            const Reg r = useI32(vw, RDI);
            a_.mov32(RDI, r);
            a_.or_(RDI, R14);
        } else {
            boxDoubleTo(RDI, operandF64(vw, 0, RDI), tame(vw));
        }
        passRax(boxAt);  // boxing goes through rdi only
        const Mem slot = provenSlotMem(in.a, ic.shape, ic.index, failed ? 0 : ic.aux, bad);
        if (factNum_.count({in.a, ic.index})) {
            a_.mov(slot, RDI);  // over a number
            if (!failed && ic.aux != 0)
                keepRax(in.a);
        } else {
            a_.mov(RSI, slot);
            a_.mov(slot, RDI);
            release(RSI);
        }
        if (!isI32(vw))
            for (auto& [numSlot, kind] : factNum_)
                if (numSlot.second == ic.index && kind == 2)
                    kind = 1;  // the same object may be under another name
        factNum_[{in.a, ic.index}] = isI32(vw) ? 2 : 1;
        return true;
    }
    case Op::New:
        return inlineNew(i);
    case Op::Call: {
        if (inlineCall(i))
            return true;
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
        if (hoistedMath(i)) {
            // Checked before the loop, which changes neither.
        } else if (auto mw = mathWin_.find(i); mw != mathWin_.end()) {
            // `Math` and its method read here: their registers were never written.
            emitReadInto(mw->second.first, false, bad);
            emitReadInto(mw->second.second, true, bad);
            a_.movImm(RCX, callee.bits);
            a_.cmp(RCX, RAX);
            a_.jcc(CNE, bad);
        } else {
            a_.movImm(RCX, callee.bits);
            a_.cmp(RCX, R(in.b));
            a_.jcc(CNE, bad);
        }
        const int x = operandF64(uw[2], 0, RAX);
        if (mathUnaryIsSqrt(target) && !isI32(dw) && inReg(dw)) {
            // One instruction, from the argument's register into the result's.
            // VEX with the argument as the merged operand: no false wait on what the result register held before.
            if (hasAvx())
                a_.vsd(0x51, xmmOf(dw), x, x);
            else
                a_.sqrtsd(xmmOf(dw), x);
            dirty_[std::size_t(dw)] = 1;
            return true;
        }
        if (x != 0)
            a_.movaps(0, x);
        if (mathUnaryIsSqrt(target)) {
            a_.sqrtsd(0, 0);
        } else {
            savePool(i);
            a_.movImm(RAX, reinterpret_cast<std::uint64_t>(target));
            a_.callC(RAX);
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

void Baseline::planBoundedSteps()
{
    noOverflow_.assign(n_ + 1, 0);
    if (!code_->handlers.empty())
        return;
    const std::size_t words = (code_->nregs + 63) / 64;
    const std::vector<std::uint64_t> top(words, ~std::uint64_t(0));
    std::vector<std::vector<std::uint64_t>> in(n_ + 1, top);
    std::vector<std::uint8_t> reached(n_ + 1, 0);
    in[0].assign(words, 0);
    reached[0] = 1;
    auto i32 = [&](std::uint32_t j, std::size_t k) {
        const auto& uw = plan_->useWebs[j];
        return k < uw.size() && uw[k] >= 0 && isI32(uw[k]);
    };
    // For a compare of int32s, the register found strictly below an int32 (or at most a 16-bit constant) on the way it jumps
    // (taken) and on the way it falls through: -1 for none.
    auto bounds = [&](std::uint32_t j, bool taken) -> int {
        const Ins& x = code_->code[j];
        const Op op = x.opcode();
        const bool both = i32(j, 0) && i32(j, 1);
        switch (op) {
        case Op::JLt: case Op::JNotGe: return both && taken ? int(x.a) : -1;
        case Op::JGe: case Op::JNotLt: return both && !taken ? int(x.a) : -1;
        case Op::JGt: case Op::JNotLe: return both && taken ? int(x.b) : -1;
        case Op::JLe: case Op::JNotGt: return both && !taken ? int(x.b) : -1;
        case Op::JLtI: case Op::JLeI: case Op::JNotGeI: case Op::JNotGtI: return i32(j, 0) && taken ? int(x.a) : -1;
        case Op::JGeI: case Op::JGtI: case Op::JNotLtI: case Op::JNotLeI: return i32(j, 0) && !taken ? int(x.a) : -1;
        default: return -1;
        }
    };
    for (bool changed = true; changed;) {
        changed = false;
        for (std::uint32_t j = 0; j < n_; j += length(j)) {
            if (!reached[j])
                continue;
            std::vector<std::uint64_t> out = in[j];
            const opt::Effects e = opt::effectsOf(*code_, j);
            if (!e.ok)
                out.assign(words, 0);
            for (std::uint16_t r : e.wr)
                if (r < code_->nregs)
                    out[r / 64] &= ~(std::uint64_t(1) << (r % 64));
            auto flow = [&](std::uint32_t to, int bound) {
                if (to > n_)
                    return;
                std::vector<std::uint64_t> st = out;
                if (bound >= 0 && unsigned(bound) < code_->nregs)
                    st[unsigned(bound) / 64] |= std::uint64_t(1) << (unsigned(bound) % 64);
                if (!reached[to]) {
                    reached[to] = 1;
                    in[to] = st;
                    changed = true;
                    return;
                }
                for (std::size_t w = 0; w < words; ++w)
                    if (in[to][w] & ~st[w]) {
                        in[to][w] &= st[w];
                        changed = true;
                    }
            };
            const Op op = code_->code[j].opcode();
            const std::int64_t t = opt::jumpTarget(*code_, j);
            if (t >= 0)
                flow(std::uint32_t(t), bounds(j, true));
            if (op != Op::Jmp && op != Op::Ret && op != Op::Throw && op != Op::ThrowError)
                flow(j + length(j), t >= 0 ? bounds(j, false) : -1);
        }
    }
    for (std::uint32_t j = 0; j < n_; j += length(j)) {
        const Ins& x = code_->code[j];
        const bool one = x.opcode() == Op::Inc || (x.opcode() == Op::AddI && std::int16_t(x.c) == 1);
        if (one && reached[j] && x.b < code_->nregs && (in[j][x.b / 64] >> (x.b % 64) & 1))
            noOverflow_[j] = 1;
    }
}

void Baseline::planSinglePreds()
{
    singlePred_.assign(n_ + 1, -1);
    factSnaps_.clear();
    if (!code_->handlers.empty())
        return;
    std::vector<std::uint32_t> preds(n_ + 1, 0);
    std::vector<std::int64_t> from(n_ + 1, -1);
    std::uint32_t prev = ~0u;
    for (std::uint32_t j = 0; j < n_; j += length(j)) {
        if (prev != ~0u) {
            const Op op = code_->code[prev].opcode();
            if (op != Op::Jmp && op != Op::Ret && op != Op::Throw && op != Op::ThrowError)
                ++preds[j];  // falls in
        } else {
            ++preds[j];  // the way in
        }
        const std::int64_t t = opt::jumpTarget(*code_, j);
        if (t >= 0 && std::uint64_t(t) <= n_) {
            ++preds[std::size_t(t)];
            from[std::size_t(t)] = t > std::int64_t(j) ? std::int64_t(j) : -2;
        }
        prev = j;
    }
    for (std::uint32_t j = 0; j < n_; j += length(j))
        if (plan_->isLeader[j] && preds[j] == 1 && from[j] >= 0 && code_->code[std::size_t(from[j])].opcode() != Op::JNotCallee)
            singlePred_[j] = from[j];
}

void Baseline::emitOpt(std::uint32_t i)
{
    factAt_ = i;
    if (plan_->isLeader[i]) {
        std::fill(dirty_.begin(), dirty_.end(), 1);
        if (auto sn = factSnaps_.find(i); sn != factSnaps_.end()) {
            // Reached from one jump only: what was known there holds.
            FactSnap& f = sn->second;
            factShape_ = std::move(f.shape);
            factAux_ = std::move(f.aux);
            factInl_ = std::move(f.inl);
            factSet_ = std::move(f.set);
            factCode_ = std::move(f.code);
            factValid_ = std::move(f.valid);
            factNum_ = std::move(f.num);
            factArr_ = std::move(f.arr);
            factSnaps_.erase(sn);
        } else {
            forgetFacts();
        }
        raxObj_ = -1;
    }

    delivered_ = false;
    carried_ = plan_->isLeader[i] ? -1 : raxHolds_;
    raxHolds_ = -1;
    if (code_->code[i].opcode() == Op::Call && fallbackOf(i))
        return;  // the long way of a call run in place: its guard leaves for the interpreter instead
    if (code_->code[i].opcode() == Op::Jmp) {
        // A jump over nothing that makes code (padding, the long ways of calls run in place) is no jump.
        const std::int64_t t = opt::jumpTarget(*code_, i);
        bool empty = t > std::int64_t(i);
        for (std::uint32_t j = i + length(i); empty && std::int64_t(j) < t; j += length(j))
            empty = code_->code[j].opcode() == Op::Nop || (code_->code[j].opcode() == Op::Call && fallbackOf(j));
        if (empty) {
            materializeReads(i);
            return;
        }
    }
#if defined(SCRIPT_JIT_ENV_OVERRIDE)
    if (code_->code[i].opcode() == Op::LoadUndef && std::getenv("T2UNDEF"))
        std::fprintf(stderr, "undef @%u r%u plain %d dead %d\n", i, unsigned(code_->code[i].a),
                     int(plainAt(i, code_->code[i].a)), int(regDeadAfter(i, code_->code[i].a)));
#endif
    if (code_->code[i].opcode() == Op::LoadUndef && (movedOut_.count(i) ||
        (plainAt(i, code_->code[i].a) && regDeadAfter(i, code_->code[i].a)))) {
        // Over no cell, and nothing reads it: the frame's way out writes it anyway. Or a move took what it held.
        forgetFact(code_->code[i].a);
        return;
    }

    if (skipDef_.size() > i && skipDef_[i]) {
        // Nothing runs; the register it would have written no longer holds what was proven about it.
        for (std::uint16_t r : opt::effectsOf(*code_, i).wr)
            forgetFact(r);
        return;
    }
    if (directCalls_.count(i)) {
        if (borrowInfo_ && directCalls_.at(i).code == code_) {
            // A call of itself that comes back here ran nothing else (`scriptVmBorrowGate`) and changed no object it did
            // not make: what is known of this frame's registers below the callee's holds.
            const Ins& ci = code_->code[i];
            for (unsigned r = ci.b; r < code_->nregs; ++r)
                forgetFact(r);
            forgetFact(ci.a);
        } else {
            forgetFacts();
        }
        emitDirectCall(i);
        return;
    }
    if (selfGuards_.count(i)) {
        emitSelfGuard(i);
        return;
    }
    if (i < chainRole_.size() && chainRole_[i]) {
        forgetFacts();
        emitChain(i);
        return;
    }
    if (windowRead_.count(i)) {
        // A pure read into a window slot: what is proven about the frame's registers stays.
        emitWindowRead(i);
        for (std::uint16_t r : opt::effectsOf(*code_, i).wr)
            forgetFact(r);
        return;
    }
    if (auto pa = promoAccess_.find(i); pa != promoAccess_.end()) {
        // A promoted slot: the double in its stack slot.
        const LoopPromo& lp = loopPromos_[pa->second];
        const Ins& in = code_->code[i];
        const std::int32_t start = a_.here();
        if (in.opcode() == Op::GetProp) {
            const std::int32_t dw = plan_->defWeb[i];
            const int x = inReg(dw) ? xmmOf(dw) : 0;
            a_.movsdLoad(x, mem(RSP, lp.slot));
            defF64(dw, x);
        } else {
            const std::int32_t vw = plan_->useWebs[i][1];
            a_.movsdStore(mem(RSP, lp.slot), operandF64(vw, 0, RCX));
        }
        passRax(start);
        for (std::uint16_t r : opt::effectsOf(*code_, i).wr)
            forgetFact(r);
        return;
    }
    if (auto ru = reuseRead_.find(i); ru != reuseRead_.end()) {
        // The value an earlier read of the same property of the same object left in its home slot.
        // A read hoisted before the loop left it in its loop slot instead. Straight into the variable's register; rax stays.
        const std::int32_t from = plan_->defWeb[ru->second], dw = plan_->defWeb[i];
        const LoopRead* lr = hoistedRead(ru->second);
        const Mem src = lr ? mem(RSP, lr->slot) : home(from);
        const std::int32_t start = a_.here();
        if (isI32(dw)) {
            const Reg r = inReg(dw) ? gpr(dw) : RCX;
            a_.mov32(r, src);
            defI32(dw, r);
        } else {
            const int x = inReg(dw) ? xmmOf(dw) : 0;
            a_.movsdLoad(x, src);
            defF64(dw, x);
        }
        if (raxObj_ != int(code_->code[i].a))
            passRax(start);
        for (std::uint16_t r : opt::effectsOf(*code_, i).wr)
            forgetFact(r);
        return;
    }
    if (emitTyped(i)) {
        for (std::uint16_t r : opt::effectsOf(*code_, i).wr)
            forgetFact(r);
        saveReadValue(i);
        return;
    }
    if (code_->code[i].opcode() == Op::LoadUndef && plainAt(i, code_->code[i].a)) {
        forgetFact(code_->code[i].a);
        a_.mov(R(code_->code[i].a), std::int32_t(kUndefined));  // over no cell
        return;
    }
    if (emitMove(i))
        return;
    if (emitReturnCopy(i))
        return;
    forgetFacts();
    materializeReads(i);
    // A jump that may run the interpreter (a test of an object, a tick) leaves the frame as the interpreter wants it. A
    // forward `Jmp` runs nothing, and where it goes every variable's frame copy counts as stale.
    // A strict test against null, undefined or a boolean loaded just before compares bits and runs nothing either.
    if (const std::int64_t t = opt::jumpTarget(*code_, i);
        t >= 0 && !(code_->code[i].opcode() == Op::Jmp && t > std::int64_t(i)) && !(t > std::int64_t(i) && bitTestAt(i)))
        materializeLive(i);
    emit(i);
    if (plan_->defWeb[i] >= 0 && !delivered_)
        loadTypedAfter(i);
    saveReadValue(i);
}

void Baseline::planTestReads()
{
    testReads_.clear();
    quietDefs_.clear();
    for (std::uint32_t k = 3; k < n_; k += length(k)) {
        const Ins& in = code_->code[k];
        if ((in.opcode() != Op::JStrictEq && in.opcode() != Op::JStrictNe) || plan_->isLeader[k])
            continue;
        const std::uint32_t j = k - 1;
        const Op cop = code_->code[j].opcode();
        if ((cop != Op::LoadNull && cop != Op::LoadUndef && cop != Op::LoadTrue && cop != Op::LoadFalse) ||
            plan_->isLeader[j] || j < 2)
            continue;
        const unsigned y = code_->code[j].a;
        if ((y == in.a) == (y == in.b))
            continue;
        const unsigned x = y == in.a ? in.b : in.a;
        const std::uint32_t i = j - 2;
        const Ins& g = code_->code[i];
        if (g.opcode() != Op::GetProp || code_->code[i + 1].opcode() != Op::Cache || g.a != x || g.b == x || g.b == y ||
            code_->code[i + 1].a == kNoCache || plan_->defWeb[i] >= 0 || plan_->defWeb[j] >= 0)
            continue;
        if ((i < skipDef_.size() && skipDef_[i]) || (i < chainRole_.size() && chainRole_[i]) || windowRead_.count(i) ||
            (j < skipDef_.size() && skipDef_[j]) || reuseRead_.count(i) || (i < code_->prof.size() && (code_->prof[i] & opt::kProfFailed)))
            continue;
        const auto& uw = plan_->useWebs[i];
        if (uw.empty() || uw[0] >= 0)
            continue;
        const InlineCache& ic = code_->caches[code_->code[i + 1].a];
        if (ic.shape == 0 || ic.holder || ic.next != 0 || ic.aux == 0 ||
            (ic.pad && ic.pad <= code_->poly.size() && code_->poly[ic.pad - 1].n))
            continue;
        if (!regDeadAfter(k, x) || !regDeadAfter(k, y))
            continue;
        testReads_[k] = i;
        quietDefs_.insert(i);
        quietDefs_.insert(j);
        skipDef_[i] = 1;
        skipDef_[j] = 1;
        exitRedirect_[j] = std::int32_t(i);
        exitRedirect_[k] = std::int32_t(i);
    }
}

/// Which registers each instruction may find written since the frame began (with something other than undefined).
void Baseline::planDirty()
{
    dirtyIn_.clear();
    if (!code_->handlers.empty())
        return;
    const std::size_t words = (code_->nregs + 63) / 64;
    std::vector<std::vector<std::uint64_t>> in(n_ + 1, std::vector<std::uint64_t>(words, 0));
    std::vector<opt::Effects> eff(n_);
    std::vector<std::int32_t> kill(n_, -1);
    for (std::uint32_t j = 0; j < n_; j += length(j)) {
        eff[j] = opt::effectsOf(*code_, j);
        if (!eff[j].ok)
            return;
        if (quietDefs_.count(j) || newWinDefs_.count(j))
            eff[j].wr.clear();  // writes nothing (`planTestReads`; a `new`'s long way clears what it writes)
        // Undefined, or left out and so leaving the register as it was: no new value either way. Undefined when it is
        // made, or a move took the value (`emitMove`, `inlineNew`): as the frame found it, nothing to clear on the way out.
        if (code_->code[j].opcode() == Op::LoadUndef) {
            const unsigned r = code_->code[j].a;
            eff[j].wr.clear();
            if (!(j < skipDef_.size() && skipDef_[j]) && !(plainAt(j, r) && regDeadAfter(j, r)))
                kill[j] = std::int32_t(r);
        }
    }
    // The global read before a self guard is written only on its way out to the interpreter.
    for (const auto& [g, sg] : selfGuards_)
        if (sg.read < n_)
            eff[sg.read].wr.clear();
    // A typed variable's register is written in the frame only when something reads it there (a generic instruction, a
    // call's arguments) or a jump that may run the interpreter writes the frame back. Where neither can happen, the
    // instructions that always make it in place write no register.
    auto allTypedUses = [&](std::uint32_t j) {
        for (std::int32_t w : plan_->useWebs[j])
            if (w < 0)
                return false;
        return true;
    };
    auto inPlace = [&](std::uint32_t j) {
        switch (code_->code[j].opcode()) {
        case Op::Mov: case Op::Add: case Op::Sub: case Op::AddI: case Op::SubI: case Op::Inc: case Op::Dec:
        case Op::LoadInt:
            return plan_->defWeb[j] >= 0 && isI32(plan_->defWeb[j]) && allTypedUses(j);
        default:
            return false;
        }
    };
    bool genericJump = false;
    std::vector<std::uint8_t> frameRead(code_->nregs + 1, 0);
    for (std::uint32_t j = 0; j < n_; j += length(j)) {
        const Op op = code_->code[j].opcode();
        const bool cond = opt::isConditionalJump(op) && op != Op::JNotCallee;
        if (cond && !allTypedUses(j))
            genericJump = true;
        const bool typedReader = inPlace(j) || (cond && allTypedUses(j)) || (op == Op::Ret && allTypedUses(j)) ||
                                 (op == Op::Call && fallbackOf(j));  // never reached here
        if (!typedReader)
            for (std::uint16_t r : eff[j].rd)
                if (r < frameRead.size())
                    frameRead[r] = 1;
    }
    if (!genericJump)
        for (std::uint32_t j = 0; j < n_; j += length(j))
            if (inPlace(j) && !eff[j].wr.empty() && eff[j].wr[0] < frameRead.size() && !frameRead[eff[j].wr[0]])
                eff[j].wr.clear();
    for (bool changed = true; changed;) {
        changed = false;
        for (std::uint32_t j = 0; j < n_; j += length(j)) {
            std::vector<std::uint64_t> out = in[j];
            if (kill[j] >= 0 && unsigned(kill[j]) < code_->nregs)
                out[unsigned(kill[j]) / 64] &= ~(std::uint64_t(1) << (unsigned(kill[j]) % 64));
            for (std::uint16_t r : eff[j].wr)
                if (r < code_->nregs)
                    out[r / 64] |= std::uint64_t(1) << (r % 64);
            auto flow = [&](std::uint32_t to) {
                if (to > n_)
                    return;
                for (std::size_t w = 0; w < words; ++w)
                    if (out[w] & ~in[to][w]) {
                        in[to][w] |= out[w];
                        changed = true;
                    }
            };
            const Op op = code_->code[j].opcode();
            const std::int64_t t = opt::jumpTarget(*code_, j);
            if (t >= 0)
                flow(std::uint32_t(t));
            if (op != Op::Jmp && op != Op::Ret && op != Op::Throw && op != Op::ThrowError)
                flow(j + length(j));
        }
    }
    dirtyIn_ = std::move(in);
#if defined(SCRIPT_JIT_ENV_OVERRIDE)
    if (std::getenv("T2DIRTY"))
        for (std::uint32_t j = 0; j < n_; j += length(j))
            if (code_->code[j].opcode() == Op::Ret) {
                std::fprintf(stderr, "ret @%u dirty:", j);
                for (unsigned r = 0; r < code_->nregs; ++r)
                    if (dirtyIn_[j][r / 64] >> (r % 64) & 1)
                        std::fprintf(stderr, " r%u", r);
                std::fprintf(stderr, " (genericJump %d)\n", int(genericJump));
            }
#endif
}

/// A forward `Jmp` to a `Ret` (or to `Mov a, b; Ret a`): the return made here, clearing only what this path wrote.
bool Baseline::emitReturnCopy(std::uint32_t i)
{
    const Ins& in = code_->code[i];
    if (in.opcode() != Op::Jmp || dirtyIn_.empty())
        return false;
    const std::int64_t t = opt::jumpTarget(*code_, i);
    if (t <= std::int64_t(i) || std::uint32_t(t) + 1 >= n_)
        return false;
    const std::uint32_t at = std::uint32_t(t);
    const Ins& ti = code_->code[at];
    unsigned src;
    std::int32_t w;
    if (ti.opcode() == Op::Ret) {
        src = ti.a;
        w = plan_->useWebs[at].empty() ? -1 : plan_->useWebs[at][0];
    } else if (ti.opcode() == Op::Mov && code_->code[at + 1].opcode() == Op::Ret && code_->code[at + 1].a == ti.a &&
               !plan_->isLeader[at + 1]) {
        src = ti.b;
        w = plan_->useWebs[at].empty() ? -1 : plan_->useWebs[at][0];
    } else {
        return false;
    }
    if (src == 0 || (w < 0 && plan_->defWeb[at] >= 0))
        return false;
    std::vector<std::uint64_t> dirty = dirtyIn_[i];
    dirty[src / 64] |= std::uint64_t(1) << (src % 64);
    Ins r = code_->code[at + (ti.opcode() == Op::Ret ? 0 : 1)];
    r.a = std::uint16_t(src);
    materializeReads(i);
    if (w >= 0) {
        if (isI32(w)) {
            const Reg v = useI32(w, RAX);
            a_.mov32(RAX, v);
            a_.or_(RAX, R14);
        } else {
            boxDoubleTo(RAX, operandF64(w, 0, RAX), tame(w));
        }
        ret(i, r, true, &dirty);
    } else {
        ret(i, r, false, &dirty);
    }
    return true;
}

void Baseline::saveReadValue(std::uint32_t i)
{
    if (!saveRead_.count(i) || hoistedRead(i))
        return;  // a hoisted read's value stays in its loop slot
    const std::int32_t w = plan_->defWeb[i];
    if (!inReg(w))
        return;  // there already
    if (isI32(w))
        a_.mov32(home(w), gpr(w));
    else
        a_.movsdStore(home(w), xmmOf(w));
}

void Baseline::emitExits()
{
    for (ExitReq& e : exits_) {
        a_.bind(e.label);
        writeBack(e.promos);
        ownRegs(e.own);
        const bool after = e.kind == 1;
        const std::uint32_t at = after ? e.index + length(e.index) : e.index;
        const std::int32_t skip = after ? plan_->defWeb[e.index] : -1;
        if (at < n_)
            for (const opt::LiveWeb& lw : plan_->liveBefore[at])
                if (std::int32_t(lw.web) != skip)
                    spillToFrame(lw.reg, std::int32_t(lw.web));
        writePending(at);
        for (const auto& [d, n] : calleeReads_)
            if (d < at && at <= n) {
                // The constructor's global read this code left out, as it would have been (nothing else ran since).
                a_.mov(RDI, R12);
                a_.movImm(RSI, d);
                a_.movImm(RAX, reinterpret_cast<std::uint64_t>(&helperThunk<jitGetGlobal>));
                a_.callC(RAX);
                a_.aluI32(7, RAX, 1);
                a_.jcc(CNE, exception(e.index));
            }
        a_.aluI(0, RSP, spill_);
        if (borrowInfo_ && !e.preOwned)
            borrowGate(false);  // this frame's own were counted above
        a_.mov(RDI, R12);
        a_.movImm(RSI, e.index);
        a_.movImm(RDX, std::uint64_t(e.kind));
        a_.movImm(RAX, reinterpret_cast<std::uint64_t>(&scriptVmOptExit));
        a_.callC(RAX);
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
        writeBack(promosAt(t.first));  // what the interrupt may see; the loop's way in loads it again
        savePool(t.first);
        a_.mov(RDI, R12);
        a_.call(tick_);
        a_.test32(RAX, RAX);
        if (const std::vector<unsigned> own = borrowedAt(t.first); !own.empty()) {
            Label ok;
            a_.jcc(CE, ok);
            ownRegs(own);
            a_.jmp(exception(t.first));
            a_.bind(ok);
        } else {
            a_.jcc(CNE, exception(t.first));
        }
        restorePool(t.first);
        // What the interrupt ran may have changed what a loop takes as known: through its checks again.
        const std::int64_t target = opt::jumpTarget(*code_, t.first);
        auto pre = target >= 0 ? preLoop_.find(std::uint32_t(target)) : preLoop_.end();
        if (pre != preLoop_.end() && target <= std::int64_t(t.first)) {
            a_.jmp(loopEntry(std::uint32_t(target)));
        } else {
            a_.jmp(tickBack_[k]);
        }
    }
    emitExits();
    a_.bind(entryFail_);
    a_.aluI(0, RSP, spill_);
    a_.movImm(RAX, reinterpret_cast<std::uint64_t>(base_->addressFor(0)));
    a_.jmp(RAX);
}

void Baseline::findPlainRegs()
{
    plainRegs_.assign(code_->nregs, 1);
    windowCleanup_.clear();
    if (!plainRegs_.empty())
        plainRegs_[0] = 0;
    if (!code_->handlers.empty() || (code_->flags & (CodeBlock::kGenerator | CodeBlock::kAsync))) {
        plainRegs_.assign(code_->nregs, 0);
        return;
    }
    for (std::uint32_t i = 0; i < n_; i += length(i)) {
        const Ins& in = code_->code[i];
        const Op op = in.opcode();
        const opt::Effects e = opt::effectsOf(*code_, i);
        // An instruction this code skips writes nothing here (where its value is written, the code is leaving, or a
        // direct call's long way lets it go after the call).
        if (i < skipDef_.size() && skipDef_[i])
            continue;
        if (op == Op::Call && i > 0 && fallbackOf(i))
            continue;  // the long way of a call run in place: this code leaves before it
        const bool constant = op == Op::LoadUndef || op == Op::LoadNull || op == Op::LoadTrue || op == Op::LoadFalse ||
                              op == Op::LoadInt || op == Op::LoadHole || op == Op::LoadUninit ||
                              (op == Op::LoadK && !code_->consts[in.b].isCell());
        for (std::size_t k = 0; k < e.wr.size(); ++k) {
            const bool typed = k == 0 && plan_->defWeb[i] >= 0;
            if (!typed && !constant && e.wr[k] < plainRegs_.size())
                plainRegs_[e.wr[k]] = 0;
        }
        if (op == Op::Call || op == Op::New) {
            // The callee may assign its parameters (or `this`), which are these registers: unless it is a known one that
            // does not, and they are dead after the call (its slow path, which may reach another, lets them go).
            bool keep = false;
            auto dc = directCalls_.find(i);
            if (dc != directCalls_.end()) {
                const CodeBlock* C = dc->second.code;
                keep = true;
                for (std::uint32_t j = 0; j < C->code.size() && keep; j = opt::nextOf(*C, j)) {
                    const opt::Effects ce = opt::effectsOf(*C, j);
                    keep = ce.ok;
                    for (std::uint16_t w : ce.wr)
                        keep = keep && w > C->nparams;
                }
                for (unsigned r = in.b + 1u; r <= in.b + 1u + in.c && keep; ++r)
                    keep = regDeadAfter(i, r);
            }
            for (unsigned r = in.b + 1u; r <= in.b + 1u + in.c && r < plainRegs_.size(); ++r) {
                if (!keep)
                    plainRegs_[r] = 0;
                else
                    windowCleanup_[i].push_back(r);
            }
            if (dc != directCalls_.end())
                windowCleanup_[i].push_back(in.b);  // the long way reads the callee into it
        }
    }
}

/// `Mov a, b` of a boxed value with `LoadUndef b` later in the block and nothing between that touches `b`: the value moves
/// with its count, and `b` is let go of at once (the `LoadUndef` then does nothing).
bool Baseline::emitMove(std::uint32_t i)
{
    const Ins& in = code_->code[i];
    if (in.opcode() != Op::Mov || in.a == in.b || plan_->defWeb[i] >= 0 || fnBorrowed(in.a) || fnBorrowed(in.b))
        return false;
    const auto& uw = plan_->useWebs[i];
    if (!uw.empty() && uw[0] >= 0)
        return false;
    std::int64_t clear = -1;
    for (std::uint32_t j = i + length(i); j < n_ && !plan_->isLeader[j]; j += length(j)) {
        const Ins& x = code_->code[j];
        if (x.opcode() == Op::LoadUndef && x.a == in.b) {
            clear = j;
            break;
        }
        // The frame's way out clears `b`: what it held goes with the result.
        if (x.opcode() == Op::Ret && x.a != in.b && in.b > code_->nparams) {
            forgetFacts();
            a_.mov(RAX, R(in.b));
            a_.mov(R(in.b), std::int32_t(kUndefined));
            write(in.a, RAX);
            return true;
        }
        const opt::Effects e = opt::effectsOf(*code_, j);
        bool touches = !e.ok;
        for (std::uint16_t r : e.rd)
            touches = touches || r == in.b;
        for (std::uint16_t r : e.wr)
            touches = touches || r == in.b;
        if (touches || opt::jumpTarget(*code_, j) >= 0)
            return false;
    }
    if (clear < 0)
        return false;
    forgetFacts();
    a_.mov(RAX, R(in.b));
    a_.mov(R(in.b), std::int32_t(kUndefined));
    write(in.a, RAX);
    movedOut_.insert(std::uint32_t(clear));
    return true;
}

void Baseline::planReadReuse()
{
    reuseRead_.clear();
    saveRead_.clear();
    if (!code_->handlers.empty())
        return;
    struct Seen
    {
        unsigned obj;
        std::uint16_t atom;
        std::uint32_t at;
    };
    std::vector<Seen> seen;
    auto typedUses = [&](std::uint32_t j) {
        for (std::int32_t w : plan_->useWebs[j])
            if (w < 0)
                return false;
        return true;
    };
    // A store of a number into a property where the site has always found it: nothing but that slot changes.
    auto numberStore = [&](std::uint32_t j) {
        const auto& uw = plan_->useWebs[j];
        if (uw.size() < 2 || uw[0] >= 0 || uw[1] < 0)
            return false;
        const std::uint16_t cacheId = code_->code[j + 1].a;
        if (cacheId == kNoCache)
            return false;
        const InlineCache& ic = code_->caches[cacheId];
        return ic.shape != 0 && !ic.holder && ic.next == 0;
    };
    for (std::uint32_t j = 0; j < n_; j += length(j)) {
        if (plan_->isLeader[j])
            seen.clear();
        const Ins& in = code_->code[j];
        const Op op = in.opcode();
        const bool skipped = j < skipDef_.size() && skipDef_[j];
        if (skipped)
            continue;
        // A read in a chain or into a window keeps its value elsewhere: neither end of a reuse.
        const bool elsewhere = (j < chainRole_.size() && chainRole_[j]) || windowRead_.count(j);
        if (op == Op::GetProp && !elsewhere && plan_->defWeb[j] >= 0 && !plan_->useWebs[j].empty() &&
            plan_->useWebs[j][0] < 0 && plainReadable(j) && !inlineGetterSite(j)) {
            bool found = false;
            for (const Seen& x : seen)
                if (x.obj == in.b && x.atom == in.c &&
                    isI32(plan_->defWeb[x.at]) == isI32(plan_->defWeb[j])) {
                    reuseRead_[j] = x.at;
                    saveRead_.insert(x.at);
                    found = true;
                    break;
                }
            if (!found && in.a != in.b)
                seen.push_back(Seen{in.b, std::uint16_t(in.c), j});
        } else if (op == Op::PutProp && numberStore(j)) {
            seen.erase(std::remove_if(seen.begin(), seen.end(), [&](const Seen& x) { return x.atom == in.b; }),
                       seen.end());
        } else {
            bool quiet = false;
            switch (op) {
            case Op::Nop: case Op::Jmp: case Op::LoadUndef: case Op::LoadNull: case Op::LoadInt: case Op::LoadK:
            case Op::LoadTrue: case Op::LoadFalse: case Op::LoadHole: case Op::LoadUninit: case Op::Mov:
                quiet = true;
                break;
            case Op::GetProp:
                quiet = plan_->useWebs[j].size() >= 1 && plan_->useWebs[j][0] < 0 && plainReadable(j) &&
                        !inlineGetterSite(j);
                break;
            case Op::Add: case Op::Sub: case Op::Mul: case Op::Div: case Op::BitAnd: case Op::BitOr: case Op::BitXor:
            case Op::Shl: case Op::Shr: case Op::AddI: case Op::SubI: case Op::MulI: case Op::BitAndI: case Op::BitOrI:
            case Op::BitXorI: case Op::ShlI: case Op::ShrI: case Op::Inc: case Op::Dec: case Op::Neg:
                quiet = plan_->defWeb[j] >= 0 && typedUses(j);
                break;
            default:
                quiet = opt::isConditionalJump(op) && typedUses(j);
                break;
            }
            if (!quiet) {
                seen.clear();
                continue;
            }
        }
        // A register written here is no longer the object the reads were of.
        for (std::uint16_t r : opt::effectsOf(*code_, j).wr)
            seen.erase(std::remove_if(seen.begin(), seen.end(), [&](const Seen& x) { return x.obj == r; }), seen.end());
    }
}

/// The double webs whose NaNs can only be the ones arithmetic makes from boxed numbers: the canonical NaN, or the
/// processor's default (0xFFF8...), as they came or with the sign flipped. Boxed by adding the offset, neither runs into the
/// tags, so such a web is boxed without the test for NaN. Not so a double straight from a typed array or from a native,
/// which can be any NaN.
void Baseline::planTame()
{
    tame_.assign(plan_->webs.size(), 1);
    auto from = [&](std::uint32_t i) {
        for (std::int32_t w : plan_->useWebs[i])
            if (w >= 0 && !isI32(w) && !tame_[std::size_t(w)])
                return false;
        return true;
    };
    for (bool changed = true; changed;) {
        changed = false;
        for (std::uint32_t i = 0; i < n_; i += length(i)) {
            const std::int32_t w = plan_->defWeb[i];
            if (w < 0 || isI32(w) || !tame_[std::size_t(w)])
                continue;
            const Ins& in = code_->code[i];
            bool t = false;
            switch (in.opcode()) {
            case Op::Mov: case Op::Add: case Op::Sub: case Op::Mul: case Op::Div: case Op::Mod: case Op::AddI: case Op::SubI:
            case Op::MulI: case Op::ModI: case Op::Inc: case Op::Dec: case Op::Neg: case Op::LoadK: case Op::LoadInt:
            case Op::GetProp: case Op::GetGlobal: case Op::GetUpval: case Op::GetBox: case Op::ToNumber: case Op::ToNumeric:
            case Op::BitAnd: case Op::BitOr: case Op::BitXor: case Op::Shl: case Op::Shr: case Op::UShr: case Op::BitAndI:
            case Op::BitOrI: case Op::BitXorI: case Op::ShlI: case Op::ShrI: case Op::UShrI:
                t = from(i);
                break;
            case Op::GetElem: {
                const std::uint8_t ep = i < code_->elemProf.size() ? code_->elemProf[i] : 0;
                t = (ep == 0 || ep == elemprof::kArray) && from(i);
                break;
            }
            case Op::Call: {
                // Math.sqrt: one instruction, whose NaN is the default one or its operand's.
                const JSValue callee = constantBefore(i, in.b);
                if (in.c == 1 && isFunction(callee)) {
                    auto* fn = static_cast<JSFunction*>(asObject(callee));
                    t = (fn->flags & JSFunction::kNative) && fn->rare() && fn->rare()->native == mathUnaryNative() &&
                        mathUnaryIsSqrt(fn->rare()->nativeData) && from(i);
                }
                break;
            }
            default:
                break;
            }
            if (!t) {
                tame_[std::size_t(w)] = 0;
                changed = true;
            }
        }
    }
}

void Baseline::findCells()
{
    struct Dump
    {
        Baseline* b;
        ~Dump()
        {
#if defined(SCRIPT_JIT_ENV_OVERRIDE)
            if (!std::getenv("T2CELLS"))
                return;
            for (std::uint32_t i = 0; i < b->n_; ++i) {
                if (b->cellAt_[i].empty())
                    continue;
                std::fprintf(stderr, "cells @%u:", i);
                for (unsigned r = 0; r < b->cellAt_[i].size(); ++r)
                    if (b->cellAt_[i][r])
                        std::fprintf(stderr, " r%u", r);
                std::fprintf(stderr, "\n");
            }
#endif
        }
    } dump{this};
    const unsigned R = code_->nregs;
    cellAt_.assign(n_ + 1, {});
    if (!code_->handlers.empty() || (code_->flags & (CodeBlock::kGenerator | CodeBlock::kAsync)))
        return;
    std::vector<bool> all(R, true);
    std::vector<std::uint32_t> work;
    auto flow = [&](std::uint32_t to, const std::vector<bool>& s) {
        if (to >= n_)
            return;
        std::vector<bool>& at = cellAt_[to];
        if (at.empty()) {
            at = s;
            work.push_back(to);
            return;
        }
        bool changed = false;
        for (unsigned r = 0; r < R; ++r)
            if (s[r] && !at[r]) {
                at[r] = true;
                changed = true;
            }
        if (changed)
            work.push_back(to);
    };
    {
        std::vector<bool> entry(R, false);
        for (unsigned r = 0; r <= code_->nparams && r < R; ++r)
            entry[r] = true;
        flow(0, entry);
    }
    while (!work.empty()) {
        const std::uint32_t i = work.back();
        work.pop_back();
        std::vector<bool> s = cellAt_[i];
        const Ins& in = code_->code[i];
        const Op op = in.opcode();
        if (!(i < skipDef_.size() && skipDef_[i])) {
            const opt::Effects e = opt::effectsOf(*code_, i);
            if (!e.ok) {
                s = all;
            } else {
                const bool constant = op == Op::LoadUndef || op == Op::LoadNull || op == Op::LoadTrue ||
                                      op == Op::LoadFalse || op == Op::LoadInt || op == Op::LoadHole ||
                                      op == Op::LoadUninit || (op == Op::LoadK && !code_->consts[in.b].isCell());
                const bool fromPlain = op == Op::Mov && in.b < R && !s[in.b];
                // A typed result stays in its variable: the frame register keeps what it held.
                for (std::size_t k = 0; k < e.wr.size(); ++k)
                    if (e.wr[k] < R && !(k == 0 && plan_->defWeb[i] >= 0))
                        s[e.wr[k]] = !(constant || fromPlain);
                bool keepsParams = false;
                if (auto dc = directCalls_.find(i); dc != directCalls_.end()) {
                    const CodeBlock* C = dc->second.code;
                    keepsParams = true;
                    for (std::uint32_t j = 0; j < C->code.size() && keepsParams; j = opt::nextOf(*C, j)) {
                        const opt::Effects ce = opt::effectsOf(*C, j);
                        keepsParams = ce.ok;
                        for (std::uint16_t w : ce.wr)
                            keepsParams = keepsParams && w > C->nparams;
                    }
                }
                if ((op == Op::Call || op == Op::New || op == Op::CallSpread || op == Op::NewSpread) && !keepsParams)
                    for (unsigned r = in.b + 1u; r <= in.b + 1u + in.c && r < R; ++r)
                        s[r] = true;  // the callee's parameters, which it may assign
            }
        }
        // The long way of a call run in place: this code leaves before it.
        if (op == Op::Call && fallbackOf(i))
            continue;
        const std::int64_t t = opt::jumpTarget(*code_, i);
        // A loop's header may be entered from the interpreter (OSR), with anything in the registers.
        if (t >= 0 && !(op == Op::JNotCallee && selfGuards_.count(i)))
            flow(std::uint32_t(t), std::uint32_t(t) <= i ? all : s);
        if (op != Op::Jmp && op != Op::Ret && op != Op::Throw && op != Op::ThrowError)
            flow(i + length(i), s);
    }
}

Tier2Code* Baseline::compileOptimized()
{
    const std::uint32_t W = plan_->typedWebs;
    bool calls = false;
    for (std::uint32_t j = 0; j < n_; j += length(j))
        calls = calls || code_->code[j].opcode() == Op::Call || code_->code[j].opcode() == Op::New;
    inlineBase_ = calls ? std::int32_t(8 + 8 * W) : -1;
    chainSlot_ = std::int32_t(8 + 8 * W + (calls ? 8 * kInlineSlots : 0));
    windowBase_ = chainSlot_ + 8;
    hoistBase_ = windowBase_ + std::int32_t(8 * kWindowSlots);
    spill_ = std::int32_t((8 * W + (calls ? 8 * kInlineSlots : 0) + 8 + 8 * kWindowSlots + 8 * kHoistSlots + 15) & ~15u);
    dirty_.assign(W, 1);
    planDirectCalls();
    planWindows();
    planMathCalls();
    planTame();
    findPlainRegs();
    findCells();
    planSelfChecked();
    planReadReuse();
    movedOut_.clear();
    planChains();
    planTestReads();
    factShape_.assign(code_->nregs + 1, 0u);
    factAux_.assign(code_->nregs + 1, -1);
    factInl_.assign(code_->nregs + 1, 0);
    factSet_.assign(code_->nregs + 1, std::array<std::uint32_t, 4>{});
    factCode_.assign(code_->nregs + 1, nullptr);
    factArr_.assign(code_->nregs + 1, 0);
    planLoopFacts();
    preLoopFull_.clear();
    for (const auto& [h, label] : preLoop_)
        if (guardsCovered(h))
            preLoopFull_[h];
    planBorrows();
    planFnBorrows();
    planSinglePreds();
    planBoundedSteps();
    planDirty();
#if defined(SCRIPT_JIT_ENV_OVERRIDE)
    if (std::getenv("T2CODE"))
        std::fprintf(stderr, "== code %u regs\n%s", unsigned(code_->nregs), dump(*code_).c_str());
#endif
    delete borrowInfo_;
    borrowInfo_ = nullptr;
    if (gated_) {
        borrowInfo_ = new BorrowInfo;
        borrowInfo_->regs.assign(fnBorrow_.begin(), fnBorrow_.end());
    }
    a_.bind(selfEntry_);
    a_.push(RBX);
    a_.push(R13);
    a_.aluI(5, RSP, 8 + spill_);
    a_.mov(R13, RSI);
    a_.mov(RBX, RDI);  // the frame's registers, as every way in passes them
    for (const opt::LiveWeb& lw : plan_->liveBefore[0])
        loadFromFrame(lw.reg, std::int32_t(lw.web), entryFail_);
    if (fnAtEntry_) {
        // Self guards compare the global only: a frame of another function of this code takes the baseline code.
        holdValue(selfFn_);
        a_.movImm(RAX, selfFn_.bits);
        a_.cmp(RAX, mem(R13, kFrameFn));
        a_.jcc(CNE, entryFail_);

    }
    for (std::uint32_t i = 0; i < n_;) {
        for (const Borrow& b : borrows_)
            if (b.from == i) {
                // Dead here: let go of, so that inside the loop it never holds a counted value.
                a_.mov(RAX, R(b.reg));
                a_.movImm(RDX, kUndefined);
                a_.mov(R(b.reg), RDX);
                release(RAX);
            }
        if (preLoop_.count(i))
            emitLoopGuards(i);
        if (!plan_->isLeader[i] && !slowNext_.count(i) && !preLoop_.count(i))
            a_.bindFallthrough(at_[i]);  // nothing jumps here
        else
            a_.bind(at_[i]);
        offsets_[i] = std::uint32_t(a_.here());
        emitOpt(i);
        for (const Borrow& b : borrows_)
            if (b.to == i) {
                // Out of the loop (its last instruction does not jump back): counted again.
                a_.mov(RAX, R(b.reg));
                retain(RAX);
            }
        for (std::size_t k = 0; k < loopPromos_.size(); ++k)
            if (loopPromos_[k].to == i)
                writeBack({k});
        for (const auto& [h, w] : hoistWatch_)
            if (w.first == i && w.second != sideDoors_)
                hoistBroken_.insert(h);
        i += length(i);
    }
    a_.bind(at_[n_]);
    a_.int3();
    std::vector<std::pair<std::uint32_t, std::uint32_t>> osr;
    for (std::uint32_t h : plan_->loopHeaders) {
        osr.emplace_back(h, std::uint32_t(a_.here()));
        Label fail;
        a_.aluI(5, RSP, spill_);
        // The baseline code may have left a cell where this code keeps only plain values: then it goes on there.
        for (unsigned r = 1; r < plainRegs_.size(); ++r) {
            if (!plainRegs_[r])
                continue;
            a_.mov(RAX, R(r));
            a_.test(RAX, R15);
            a_.jcc(CE, fail);
        }
        // Not into a loop inside one that holds registers without a count (they hold counted values here).
        bool nested = false;
        for (const Borrow& b : borrows_)
            nested = nested || (b.from < h && h <= b.to);
        for (const LoopPromo& lp : loopPromos_)
            nested = nested || (lp.from < h && h <= lp.to);
        if (nested)
            a_.jmp(fail);
        for (const opt::LiveWeb& lw : plan_->liveBefore[h])
            loadFromFrame(lw.reg, std::int32_t(lw.web), fail);
        for (const Borrow& b : borrows_)
            if (b.from == h) {
                a_.mov(RAX, R(b.reg));
                a_.movImm(RDX, kUndefined);
                a_.mov(R(b.reg), RDX);
                release(RAX);
            }
        if (preLoop_.count(h))
            a_.jmp(loopEntry(h));  // through the checks the loop's code relies on
        else
            a_.jmp(at_[h]);
        a_.bind(fail);
        a_.aluI(0, RSP, spill_);
        a_.movImm(RAX, reinterpret_cast<std::uint64_t>(base_->addressFor(h)));
        a_.jmp(RAX);
    }
    for (auto& [h, full] : preLoopFull_) {
        a_.bind(full);
        factAt_ = h;
        emitLoopGuards(h, true);
        a_.jmp(preLoop_.at(h));
    }
    emitTails();
    CodeSpan span = hoistBroken_.empty() && !a_.failed() ? allocateCode(a_.buf.size()) : CodeSpan{};
    if (!span) {
        for (ValidityCell* v : validities_)
            vm_.releaseValidity(v);
        for (JSValue v : held_)
            vm::release(v);
        delete borrowInfo_;
        borrowInfo_ = nullptr;
        return nullptr;
    }
    commitCode(span, a_.buf.data(), a_.buf.size());
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
    t->borrow = borrowInfo_;
    borrowInfo_ = nullptr;
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
