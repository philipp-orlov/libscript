// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The SSA tier's code generator: a third mode of `Baseline` (included from baseline_x64.cpp inside its anonymous
// namespace). See the design in the progress notes and in ssa.hpp.
//
// Each frame register owns one count of what its slot physically holds, as in the interpreter, but this code writes a
// register's slot only when something reads it there: a baseline template, a call, an exit. While it emits, `name_[r]` is
// the SSA value register r holds and `frame_[r]` what its slot holds (kNone: not known). A value is safe when it cannot be
// a cell or some slot holds it; any other value is borrowed and stays valid only until something may let go of a cell.
// Before such an event `ssaSync` writes the slots that keep every live value safe.

/// Value `v`, boxed, into `r` (a double is boxed through xmm1).
void Baseline::loadValue(std::uint32_t v, Reg r)
{
    const SVal& s = sval_[v];
    if (s.isConst) {
        a_.movImm(r, s.bits);
        return;
    }
    switch (s.rep) {
    case kRepI32:
        if (s.reg >= 0)
            a_.mov32(r, Reg(s.reg));
        else
            a_.mov32(r, slotOf(v));
        a_.or_(r, R14);
        return;
    case kRepF64:
        if (s.reg >= 0) {
            boxDoubleTo(r, s.reg);
        } else {
            a_.movsdLoad(1, slotOf(v));
            boxDoubleTo(r, 1);
        }
        return;
    default:
        if (s.reg >= 0)
            a_.mov(r, Reg(s.reg));
        else
            a_.mov(r, slotOf(v));
        return;
    }
}

/// A boxed value in `src` is `v`'s (whose representation is boxed).
void Baseline::storeTagged(std::uint32_t v, Reg src)
{
    const SVal& s = sval_[v];
    if (s.reg >= 0)
        a_.mov(Reg(s.reg), src);
    else if (s.slot >= 0)
        a_.mov(slotOf(v), src);
}

/// Value `v` as an int32 in the low half of `dst`, else `bad` (null: it is known to be one).
void Baseline::loadI32(std::uint32_t v, Reg dst, Label* bad)
{
    const SVal& s = sval_[v];
    if (s.isConst) {
        a_.movImm(dst, s.bits);
        if (!JSValue::raw(s.bits).isInt32())
            a_.jmp(*bad);
        return;
    }
    switch (s.rep) {
    case kRepI32:
        if (s.reg >= 0)
            a_.mov32(dst, Reg(s.reg));
        else
            a_.mov32(dst, slotOf(v));
        return;
    case kRepF64:
        a_.jmp(*bad);
        return;
    default:
        if (s.reg >= 0)
            a_.mov(dst, Reg(s.reg));
        else
            a_.mov(dst, slotOf(v));
        if (!known(v, kVInt)) {
            needInt(dst, *bad);
            ssaKnow(v, kVInt | kVNotCell);
        }
        return;
    }
}

/// Value `v` as a double in xmm`x`, else `bad`. Clobbers rdx, rsi.
void Baseline::loadF64(std::uint32_t v, int x, Label* bad)
{
    const SVal& s = sval_[v];
    if (s.isConst) {
        const JSValue k = JSValue::raw(s.bits);
        if (k.isInt32()) {
            a_.movImm(RDX, std::uint32_t(k.asInt32()));
            a_.cvtsi2sd32(x, RDX);
        } else if (k.isDouble()) {
            a_.movImm(RDX, s.bits - kDoubleOffset);
            a_.movqToX(x, RDX);
        } else {
            a_.jmp(*bad);
        }
        return;
    }
    switch (s.rep) {
    case kRepI32:
        if (s.reg >= 0) {
            a_.cvtsi2sd32(x, Reg(s.reg));
        } else {
            a_.mov32(RDX, slotOf(v));
            a_.cvtsi2sd32(x, RDX);
        }
        return;
    case kRepF64:
        if (s.reg >= 0) {
            if (s.reg != x)
                a_.movaps(x, s.reg);
        } else {
            a_.movsdLoad(x, slotOf(v));
        }
        return;
    default:
        if (s.reg >= 0)
            a_.mov(RDX, Reg(s.reg));
        else
            a_.mov(RDX, slotOf(v));
        if (known(v, kVInt)) {
            a_.cvtsi2sd32(x, RDX);
            return;
        }
        needNumber(RDX, *bad);
        loadNumber(x, RDX, RSI);
        return;
    }
}

/// Write the slots of `regs` (with the value each register holds), and of every other live register whose value would
/// otherwise have no owner once the slots of `clobber` (1 per register) are overwritten by what comes next. New values are
/// counted first, the old contents let go of last, so nothing live is freed in between.
std::vector<unsigned> Baseline::ssaSyncSet(std::vector<unsigned> regs, const ssa::Bits& live,
                                           const std::vector<std::uint8_t>& clobber)
{
    const unsigned nregs = code_->nregs;
    std::vector<std::uint8_t> in(nregs, 0);
    std::vector<unsigned> set;
    auto add = [&](unsigned r) {
        if (in[r] || name_[r] == ssa::kNone)
            return;
        if (clobber[r] && !std::count(regs.begin(), regs.end(), r)) {
            // A live value whose only home is overwritten by the event itself: not modelled.
            ssaFailed_ = true;
            ssaWhy_ = "live across its own slot's overwrite";
            return;
        }
        in[r] = 1;
        set.push_back(r);
    };
    for (unsigned r : regs)
        add(r);
    for (bool changed = true; changed && !ssaFailed_;) {
        changed = false;
        // What the slots hold after the event.
        std::vector<std::uint32_t> owned;
        for (unsigned r = 0; r < nregs; ++r) {
            if (clobber[r])
                continue;  // written by the event (a register it reads is written for it first, and owns nothing after)
            const std::uint32_t v = in[r] ? name_[r] : frame_[r];
            if (v != ssa::kNone)
                owned.push_back(v);
        }
        std::sort(owned.begin(), owned.end());
        for (unsigned r = 0; r < nregs; ++r) {
            if (in[r] || !ssa::bitTest(live, r))
                continue;
            const std::uint32_t v = name_[r];
            if (v == ssa::kNone || !mayBeCell(v) || std::binary_search(owned.begin(), owned.end(), v))
                continue;
            if (clobber[r]) {
                ssaFailed_ = true;
                ssaWhy_ = "live in a call's window";
                return set;
            }
            add(r);
            changed = true;
        }
    }
    return set;
}

/// Write the slots of `set` (each with the value its register holds): new values counted first, the old contents let go
/// of last, so nothing live is freed in between.
void Baseline::ssaSyncEmit(const std::vector<unsigned>& set)
{
    std::vector<unsigned> todo;
    for (unsigned r : set)
        if (frame_[r] != name_[r])
            todo.push_back(r);
    for (unsigned r : todo)
        if (mayBeCell(name_[r])) {
            loadValue(name_[r], RAX);
            retain(RAX);
        }
    for (unsigned r : todo) {
        loadValue(name_[r], RAX);
        const bool old = frame_[r] == ssa::kNone || mayBeCell(frame_[r]);
        if (old)
            a_.mov(RDX, R(r));
        a_.mov(R(r), RAX);
        if (old)
            release(RDX);
        frame_[r] = name_[r];
    }
}

/// Write the slots of `regs`, and of every other live register whose value would otherwise have no owner once the slots
/// of `clobber` (1 per register) are overwritten by what comes next.
void Baseline::ssaSync(std::vector<unsigned> regs, const ssa::Bits& live, const std::vector<std::uint8_t>& clobber)
{
    const std::vector<unsigned> set = ssaSyncSet(std::move(regs), live, clobber);
    if (!ssaFailed_)
        ssaSyncEmit(set);
}

/// The long way of an instruction done in place: out of line, the frame as the helper wants it (what it reads, every
/// live value owned), the registers kept, the helper called; back at `join`. The slots it writes are "not known" after
/// the instruction on both ways, which is true of either. `dst`: the register the helper writes (ssa::kNone: none),
/// whose value (boxed) is taken into the instruction's value.
void Baseline::ssaColdCall(std::uint32_t i, HelperFn helper, const ssa::Bits& liveAfter, Label& entry, Label& join)
{
    const opt::Effects e = ssa::effects(*code_, i);
    std::vector<std::uint8_t> clobber(code_->nregs, 0);
    for (std::uint16_t r : e.wr)
        clobber[r] = 1;
    ssa::Bits keep = liveAfter;
    for (std::uint16_t r : e.wr)
        ssa::bitClear(keep, r);
    const std::vector<unsigned> set = ssaSyncSet(std::vector<unsigned>(e.rd.begin(), e.rd.end()), keep, clobber);
    if (ssaFailed_)
        return;
    const std::uint32_t def = e.wr.empty() ? ssa::kNone : ssa_->defBase[i];
    const unsigned dst = e.wr.empty() ? 0 : e.wr[0];
    ColdPath c{i, helper, name_, frame_, set, def, dst, &entry, &join, liveAfter};
    coldPaths_.push_back(std::move(c));
    // After the instruction, either way: those slots hold what the way taken left there.
    for (unsigned r : set)
        if (frame_[r] != name_[r])
            frame_[r] = ssa::kNone;
    for (std::uint16_t r : e.wr)
        frame_[r] = ssa::kNone;
}

void Baseline::emitColdPaths()
{
    for (ColdPath& c : coldPaths_) {
        a_.bind(*c.entry);
        name_ = c.name;
        frame_ = c.frame;
        ssaSyncEmit(c.set);
        ssaSaveRegs();
        a_.mov(RDI, R12);
        a_.movImm(RSI, c.i);
        a_.movImm(RAX, reinterpret_cast<std::uint64_t>(c.helper));
        a_.callC(RAX);
        ssaRestoreRegs();
        a_.aluI32(7, RAX, 1);
        a_.jcc(CNE, exception(c.i));
        if (c.def != ssa::kNone && hasLoc(c.def)) {
            a_.mov(RAX, R(c.dst));
            const std::uint8_t rep = sval_[c.def].rep;
            if (rep == kRepTagged) {
                storeTagged(c.def, RAX);
            } else {
                // Not of the kind the code takes it to be: leave after the instruction (its result is in its slot).
                name_[c.dst] = c.def;
                frame_[c.dst] = c.def;
                ssaExits_.push_back(SsaExit{c.i, name_, frame_, c.liveAfter, Label{}, 1});
                Label& after = ssaExits_.back().label;
                const SVal& d = sval_[c.def];
                if (rep == kRepI32) {
                    needInt(RAX, after);
                    if (d.reg >= 0)
                        a_.mov32(Reg(d.reg), RAX);
                    else
                        a_.mov32(slotOf(c.def), RAX);
                } else {
                    needNumber(RAX, after);
                    loadNumber(0, RAX, RDX);
                    if (d.reg >= 0)
                        a_.movaps(d.reg, 0);
                    else
                        a_.movsdStore(slotOf(c.def), 0);
                }
            }
        }
        a_.jmp(*c.join);
    }
}

/// An instruction run by its baseline template, on the frame: the registers it reads are written first, then every live
/// value it could let go of is given an owner; after it, the registers it wrote hold new values the frame owns.
void Baseline::ssaGeneric(std::uint32_t i, const ssa::Bits& liveAfter)
{
    const Ins& in = code_->code[i];
    const opt::Effects e = ssa::effects(*code_, i);
    const unsigned nregs = code_->nregs;
    std::vector<std::uint8_t> clobber(nregs, 0);
    for (std::uint16_t r : e.wr)
        clobber[r] = 1;
    if (in.opcode() == Op::Call || in.opcode() == Op::New)
        for (unsigned r = in.b + 1u; r < nregs; ++r)
            clobber[r] = 1;
    // Live after it and not written by it: these keep their values across.
    ssa::Bits keep = liveAfter;
    for (std::uint16_t r : e.wr)
        ssa::bitClear(keep, r);
    std::vector<unsigned> reads(e.rd.begin(), e.rd.end());
#if defined(SCRIPT_JIT_ENV_OVERRIDE)
    static const bool syncAll = std::getenv("SSA_SYNCALL") != nullptr;
    if (syncAll)
        for (unsigned r = 0; r < nregs; ++r)
            if (ssa::bitTest(keep, r) && !clobber[r])
                reads.push_back(r);
#endif
    ssaSync(reads, keep, clobber);
    if (ssaFailed_)
        return;
    emit(i);
    a_.bind(at_[i + length(i)]);  // where the template's long way comes back
    ssaForgetShapes();
    for (unsigned r = 0; r < nregs; ++r)
        if (clobber[r])
            frame_[r] = ssa::kNone;
    if (ssa_->defBase[i] != ssa::kNone)
        for (std::size_t k = 0; k < e.wr.size(); ++k) {
            const unsigned r = e.wr[k];
            const std::uint32_t v = ssa_->defBase[i] + std::uint32_t(k);
            name_[r] = v;
            frame_[r] = v;
            if (hasLoc(v)) {
                a_.mov(RAX, R(r));
                storeTagged(v, RAX);
            }
        }
}

/// The phi moves of edge `from` -> `to`: each phi of `to` gets its input from `from` (converted to the phi's
/// representation), all read before any is written. Moves of one representation between registers and slots are put in
/// an order that reads every source before its place is written, a cycle broken through a scratch register; a move that
/// converts or loads a constant goes last, unless that would write a place another move still reads (then everything goes
/// through the stack).
void Baseline::ssaEdgeMoves(std::uint32_t from, std::uint32_t to)
{
    const ssa::Block& tb = ssa_->blocks[to];
    std::size_t k = 0;
    while (k < tb.preds.size() && tb.preds[k] != from)
        ++k;
    struct Move
    {
        std::uint32_t src, dst;  // values
    };
    std::vector<Move> plain, complex;
    for (std::uint32_t p : tb.phis) {
        const std::uint32_t src = ssa_->values[p].in[k];
        if (src == p || !hasLoc(p))
            continue;
        const SVal& a = sval_[src];
        const SVal& d = sval_[p];
        if (!a.isConst && a.rep == d.rep && a.reg == d.reg && a.slot == d.slot)
            continue;
        if (!a.isConst && a.rep == d.rep)
            plain.push_back(Move{src, p});
        else
            complex.push_back(Move{src, p});
    }
    if (plain.empty() && complex.empty())
        return;
    // A place: a register (class by representation) or a slot.
    auto place = [&](std::uint32_t v) {
        const SVal& s = sval_[v];
        if (s.reg >= 0)
            return std::int64_t(s.rep == kRepF64 ? 1000 + s.reg : s.reg);
        return std::int64_t(100000 + s.slot);
    };
    bool viaStack = false;
    for (const Move& c : complex) {
        for (const Move& m : plain)
            if (place(m.src) == place(c.dst) || (!sval_[c.src].isConst && place(c.src) == place(m.dst)))
                viaStack = true;
        for (const Move& o : complex)
            if (&o != &c && !sval_[o.src].isConst && place(o.src) == place(c.dst))
                viaStack = true;
    }
    auto fetch = [&](std::uint32_t src, std::uint32_t p) {
        switch (sval_[p].rep) {
        case kRepI32:
            loadI32(src, RAX, nullptr);
            a_.mov32(RAX, RAX);
            break;
        case kRepF64:
            loadF64(src, 0, nullptr);
            a_.movqFromX(RAX, 0);
            break;
        default:
            loadValue(src, RAX);
            break;
        }
    };
    auto put = [&](std::uint32_t p) {
        const SVal& d = sval_[p];
        if (d.rep == kRepF64) {
            if (d.reg >= 0)
                a_.movqToX(d.reg, RAX);
            else
                a_.mov(slotOf(p), RAX);
        } else if (d.reg >= 0) {
            a_.mov(Reg(d.reg), RAX);
        } else {
            a_.mov(slotOf(p), RAX);
        }
    };
    if (viaStack) {
        std::vector<Move> all = plain;
        all.insert(all.end(), complex.begin(), complex.end());
        const std::int32_t room = std::int32_t((8 * all.size() + 15) & ~std::size_t(15));
        a_.aluI(5, RSP, room);
        moveRoom_ = room;
        for (std::size_t m = 0; m < all.size(); ++m) {
            fetch(all[m].src, all[m].dst);
            a_.mov(mem(RSP, std::int32_t(8 * m)), RAX);
        }
        for (std::size_t m = 0; m < all.size(); ++m) {
            a_.mov(RAX, mem(RSP, std::int32_t(8 * m)));
            put(all[m].dst);
        }
        moveRoom_ = 0;
        a_.aluI(0, RSP, room);
        return;
    }
    // One plain move: from a place to a place of the same representation. `tmpFrom`: the source is the scratch.
    auto move = [&](std::uint32_t src, std::uint32_t dst, bool tmpFrom) {
        const SVal& a = sval_[src];
        const SVal& d = sval_[dst];
        if (d.rep == kRepF64) {
            const int x = tmpFrom ? 1 : (a.reg >= 0 ? a.reg : -1);
            if (x < 0) {
                if (d.reg >= 0)
                    a_.movsdLoad(d.reg, slotOf(src));
                else {
                    a_.mov(RAX, slotOf(src));
                    a_.mov(slotOf(dst), RAX);
                }
            } else if (d.reg >= 0) {
                if (d.reg != x)
                    a_.movaps(d.reg, x);
            } else {
                a_.movsdStore(slotOf(dst), x);
            }
            return;
        }
        Reg r;
        if (tmpFrom)
            r = RCX;
        else if (a.reg >= 0)
            r = Reg(a.reg);
        else {
            a_.mov(RAX, slotOf(src));
            r = RAX;
        }
        if (d.reg >= 0)
            a_.mov(Reg(d.reg), r);
        else
            a_.mov(slotOf(dst), r);
    };
    std::vector<Move> pending = plain;
    std::vector<std::uint8_t> fromTmp(pending.size(), 0);  // the move's source is in the scratch of its class
    while (!pending.empty()) {
        bool progress = false;
        for (std::size_t m = 0; m < pending.size(); ++m) {
            const std::int64_t d = place(pending[m].dst);
            bool read = false;
            for (std::size_t o = 0; o < pending.size() && !read; ++o)
                read = o != m && !fromTmp[o] && place(pending[o].src) == d;
            if (read)
                continue;
            move(pending[m].src, pending[m].dst, fromTmp[m] != 0);
            pending.erase(pending.begin() + std::ptrdiff_t(m));
            fromTmp.erase(fromTmp.begin() + std::ptrdiff_t(m));
            progress = true;
            break;
        }
        if (progress)
            continue;
        // Every place written is still read: a cycle. The first destination's contents go to the scratch, and the move
        // that reads them reads the scratch.
        const std::uint32_t dv = pending[0].dst;
        const SVal& d = sval_[dv];
        const std::int64_t dp = place(dv);
        if (d.rep == kRepF64) {
            if (d.reg >= 0)
                a_.movaps(1, d.reg);
            else
                a_.movsdLoad(1, slotOf(dv));
        } else if (d.reg >= 0) {
            a_.mov(RCX, Reg(d.reg));
        } else {
            a_.mov(RCX, slotOf(dv));
        }
        for (std::size_t o = 0; o < pending.size(); ++o)
            if (!fromTmp[o] && place(pending[o].src) == dp)
                fromTmp[o] = 1;
    }
    for (const Move& c : complex) {
        fetch(c.src, c.dst);
        put(c.dst);
    }
}

/// Leave block `from` for block `to`: a back edge looks at the interrupt flag, puts in the slots the loop's head takes as
/// given, and moves the phis; a forward edge moves the phis and leaves its frame state for the join.
void Baseline::ssaEdge(std::uint32_t from, std::uint32_t to)
{
    const ssa::Block& tb = ssa_->blocks[to];
    const unsigned nregs = code_->nregs;
    if (to > from) {
        ssaEdgeMoves(from, to);
        ssaLoadLoopCache(to);
        edgeIn_[to].push_back(EdgeState{frame_, ssaFacts()});
        a_.jmp(blockLabel_[to]);
        return;
    }
    // A back edge. The names here are the loop's: a register the head takes as in its slot is one the loop does not write.
    const std::vector<std::uint32_t> before = frame_;
    auto finish = [&]() {
        std::vector<unsigned> repair;
        for (unsigned r = 0; r < nregs; ++r)
            if (headerFrame_[to][r] != ssa::kNone && frame_[r] != headerFrame_[to][r])
                repair.push_back(r);
        if (!repair.empty())
            ssaSync(repair, tb.liveIn, std::vector<std::uint8_t>(nregs, 0));
        ssaEdgeMoves(from, to);
        a_.jmp(blockLabel_[to]);
    };
    Label tick;
    a_.mov(RDX, mem(R12, kVmInterruptSet));
    a_.cmpI32(mem(RDX, 0), 0);
    a_.jcc(CNE, tick);
    finish();
    // The host has an interrupt handler: count down to the next look at it.
    a_.bind(tick);
    frame_ = before;
    Label due;
    a_.mov32(RDX, mem(R12, kVmTicks));
    a_.aluI32(5, RDX, 1);
    a_.jcc(CE, due);
    a_.mov32(mem(R12, kVmTicks), RDX);
    finish();
    // It may run code: every live value goes to its slot first.
    a_.bind(due);
    frame_ = before;
    std::vector<unsigned> all;
    for (unsigned r = 0; r < nregs; ++r)
        if (ssa::bitTest(tb.liveIn, r))
            all.push_back(r);
    ssaSync(all, tb.liveIn, std::vector<std::uint8_t>(nregs, 0));
    ssaSaveRegs();
    a_.mov(RDI, R12);
    a_.call(tick_);
    ssaRestoreRegs();
    a_.test32(RAX, RAX);
    a_.jcc(CNE, exception(ssa_->blocks[from].last));
    finish();
}

/// A conditional jump the interpreter decides (one step of it): what the jump reads in the frame, every live value safe.
void Baseline::ssaStepJump(std::uint32_t b, std::uint32_t i)
{
    const ssa::Block& bl = ssa_->blocks[b];
    const opt::Effects e = ssa::effects(*code_, i);
    std::vector<unsigned> reads(e.rd.begin(), e.rd.end());
    ssaSync(reads, bl.liveOut, std::vector<std::uint8_t>(code_->nregs, 0));
    if (ssaFailed_)
        return;
    callStep(i);
    const std::vector<std::uint32_t> after = frame_;
    Label notTaken, fall;
    a_.cmpI(RAX, std::int32_t(kStepJump));
    a_.jcc(CNE, notTaken);
    ssaEdge(b, bl.succs[1]);
    a_.bind(notTaken);
    frame_ = after;
    a_.cmpI(RAX, std::int32_t(kStepFall));
    a_.jcc(CNE, tail_);
    ssaEdge(b, bl.succs[0]);
}

/// The frame state at the start of block `b`, from the edges into it.
void Baseline::ssaEnterBlock(std::uint32_t b)
{
    const ssa::Block& bl = ssa_->blocks[b];
    const unsigned nregs = code_->nregs;
    name_ = bl.nameIn;
    std::vector<std::uint32_t> st(nregs, ssa::kNone);
    const std::vector<EdgeState>& ins = edgeIn_[b];
    if (!ins.empty()) {
        for (unsigned r = 0; r < nregs; ++r) {
            std::uint32_t v = ins[0].frame[r];
            for (const EdgeState& es : ins)
                if (es.frame[r] != v)
                    v = ssa::kNone;
            st[r] = v;
        }
        // A phi whose every input is in the register's slot on its edge: the slot holds the phi.
        std::vector<std::uint32_t> fwd;
        for (std::size_t k = 0; k < bl.preds.size(); ++k)
            if (bl.preds[k] < b)
                fwd.push_back(std::uint32_t(k));
        if (fwd.size() == ins.size() && !bl.loopHeader)
            for (std::uint32_t p : bl.phis) {
                const ssa::Value& pv = ssa_->values[p];
                bool all = true;
                for (std::size_t e = 0; e < ins.size() && all; ++e)
                    all = ins[e].frame[pv.reg] == pv.in[fwd[e]];
                if (all)
                    st[pv.reg] = p;
            }
    }
    if (bl.loopHeader) {
        for (unsigned r = 0; r < nregs; ++r)
            if (ssa::bitTest(ssa_->loopWrites[b], r) || bl.nameIn[r] == ssa::kNone || st[r] != bl.nameIn[r])
                st[r] = ssa::kNone;
        headerFrame_[b] = st;
    }
    frame_ = std::move(st);
}

Tier2Code* Baseline::compileSsa(const ssa::Graph& g, Tier1Code* base)
{
    ssa_ = &g;
    base_ = base;
    auto fail = [&]() -> Tier2Code* {
        for (ValidityCell* v : validities_)
            vm_.releaseValidity(v);
        for (JSValue v : held_)
            vm::release(v);
        validities_.clear();
        held_.clear();
        return nullptr;
    };
    const unsigned nregs = code_->nregs;
    const std::uint32_t nb = std::uint32_t(g.blocks.size());

    // ---- what each value is: a constant, or a slot ----
    sval_.assign(g.values.size(), SVal{});
    for (unsigned r = 1 + code_->nparams; r < nregs; ++r) {
        sval_[r].isConst = true;  // a register no instruction has written yet is undefined
        sval_[r].bits = kUndefined;
    }
    for (std::uint32_t v = nregs; v < g.values.size(); ++v) {
        const ssa::Value& sv = g.values[v];
        if (sv.kind != ssa::Value::Def)
            continue;
        const Ins& in = code_->code[sv.at];
        std::uint64_t bits = 0;
        bool k = true;
        switch (in.opcode()) {
        case Op::LoadUndef: bits = kUndefined; break;
        case Op::LoadNull: bits = kNull; break;
        case Op::LoadTrue: bits = kTrue; break;
        case Op::LoadFalse: bits = kFalse; break;
        case Op::LoadHole: bits = kHole; break;
        case Op::LoadUninit: bits = kUninit; break;
        case Op::LoadInt: bits = kNumberTag | std::uint32_t(std::int32_t(in.imm())); break;
        case Op::LoadK: bits = code_->consts[in.kIndex()].bits; break;  // the block holds it as long as this code
        default: k = false; break;
        }
        sval_[v].isConst = k;
        sval_[v].bits = bits;
    }
    vDef_.assign(g.values.size(), 0);
    for (std::uint32_t v = 0; v < g.values.size(); ++v)
        if (sval_[v].isConst) {
            const JSValue k = JSValue::raw(sval_[v].bits);
            vDef_[v] = k.isInt32() ? std::uint8_t(kVInt | kVNotCell) : k.isBool() ? std::uint8_t(kVBool | kVNotCell)
                       : !k.isCell() ? kVNotCell : std::uint8_t(0);
        }
    vKnown_ = vDef_;
    vShape_.assign(g.values.size(), 0);
    touched_.clear();
    std::vector<std::vector<ssa::Bits>> liveAfter(nb);
    for (std::uint32_t b = 1; b < nb; ++b)
        if (g.blocks[b].reached)
            liveAfter[b] = g.liveAfter(*code_, b);
    if (!ssaPlan(liveAfter))
        return fail();

#if defined(SCRIPT_JIT_ENV_OVERRIDE)
    if (std::getenv("SSA_DUMP")) {
        for (std::uint32_t b = 0; b < nb; ++b) {
            const ssa::Block& bl = g.blocks[b];
            if (!bl.reached)
                continue;
            std::fprintf(stderr, "B%u [%u,%u)%s preds", b, bl.start, bl.end, bl.loopHeader ? " loop" : "");
            for (std::uint32_t p : bl.preds)
                std::fprintf(stderr, " B%u", p);
            std::fprintf(stderr, " succs");
            for (std::uint32_t p : bl.succs)
                std::fprintf(stderr, " B%u", p);
            std::fprintf(stderr, "\n  in:");
            for (unsigned r = 0; r < nregs; ++r)
                if (bl.nameIn[r] != ssa::kNone)
                    std::fprintf(stderr, " r%u=v%u", r, bl.nameIn[r]);
            std::fprintf(stderr, "\n");
            for (std::uint32_t p : bl.phis) {
                std::fprintf(stderr, "  v%u = phi r%u slot %d:", p, unsigned(g.values[p].reg), sval_[p].slot);
                for (std::uint32_t in : g.values[p].in)
                    std::fprintf(stderr, " v%u", in);
                std::fprintf(stderr, "\n");
            }
        }
        for (std::uint32_t v = 0; v < g.values.size(); ++v)
            if (sval_[v].slot >= 0)
                std::fprintf(stderr, "  v%u kind %d r%u at %u slot %d\n", v, int(g.values[v].kind), unsigned(g.values[v].reg),
                             g.values[v].at, sval_[v].slot);
    }
#endif
    // ---- code ----
    blockLabel_.assign(nb, Label{});
    edgeIn_.assign(nb, {});
    headerFrame_.assign(nb, {});
    headerFacts_.assign(nb, {});
    a_.push(RBX);
    a_.push(R13);
    a_.aluI(5, RSP, 8 + spill_);
    a_.mov(R13, RSI);
    a_.mov(RBX, RDI);
    for (unsigned r = 0; r < nregs; ++r)
        if (ssa::bitTest(g.blocks[1].liveIn, r) && hasLoc(r)) {
            a_.mov(RAX, R(r));
            storeTagged(r, RAX);
        }
    {
        std::vector<std::uint32_t> st(nregs);
        for (unsigned r = 0; r < nregs; ++r)
            st[r] = r;  // the slots hold the entry values
        edgeIn_[1].push_back(EdgeState{st, {}});
        ssaEdgeMoves(0, 1);
        ssaLoadLoopCache(1);
    }
    for (std::uint32_t b = 1; b < nb && !ssaFailed_; ++b) {
        const ssa::Block& bl = g.blocks[b];
        if (!bl.reached)
            continue;
        a_.bind(blockLabel_[b]);
        if (!loopCache_[b].empty()) {
            // From here to the loop's last block, these values are in their loop registers.
            for (const auto& [v, r] : loopCache_[b]) {
                cacheSaved_.emplace_back(v, sval_[v].reg);
                sval_[v].reg = std::int8_t(r);
            }
            cacheEnd_ = loopLast_[b];
        }
        ssaEnterBlock(b);
        ssaResetFacts();
        {
            // What every edge in knows (a loop's head: not the shapes, which its body may change).
            const std::vector<EdgeState>& ins = edgeIn_[b];
            if (!ins.empty())
                for (const Fact& f : ins[0].facts) {
                    std::uint8_t bits = f.bits;
                    std::uint32_t shape = bl.loopHeader ? 0 : f.shape;
                    for (std::size_t e = 1; e < ins.size() && (bits || shape); ++e) {
                        auto it = std::lower_bound(ins[e].facts.begin(), ins[e].facts.end(), f.v,
                                                   [](const Fact& x, std::uint32_t v) { return x.v < v; });
                        if (it == ins[e].facts.end() || it->v != f.v) {
                            bits = 0;
                            shape = 0;
                        } else {
                            bits &= it->bits;
                            if (it->shape != shape)
                                shape = 0;
                        }
                    }
                    if (bits || shape)
                        ssaKnow(f.v, bits, shape);
                }
            if (bl.loopHeader)
                headerFacts_[b] = ssaFacts();
        }
        std::size_t k = 0;
        bool ended = false;
        for (std::uint32_t i = bl.start; i < bl.end && !ssaFailed_; i = opt::nextOf(*code_, i), ++k) {
            const Ins& in = code_->code[i];
            const Op op = in.opcode();
            offsets_[i] = std::uint32_t(a_.here());
            exitAt_ = -1;
            curLiveAfter_ = liveAfter[b][k];
            {
                const opt::Effects e = ssa::effects(*code_, i);
                liveBefore_ = liveAfter[b][k];
                for (std::uint16_t r : e.wr)
                    ssa::bitClear(liveBefore_, r);
                for (std::uint16_t r : e.rd)
                    ssa::bitSet(liveBefore_, r);
            }
            if (opt::jumpTarget(*code_, i) >= 0) {
                if (op == Op::Jmp)
                    ssaEdge(b, bl.succs[0]);
                else if (!native_[i])
                    ssaStepJump(b, i);
                else if (!ssaBranch(b, i))
                    a_.jmp(ssaExit(i));
                ended = true;
                break;
            }
            switch (op) {
            case Op::GetProp:
                if (!native_[i])
                    ssaGeneric(i, liveAfter[b][k]);
                else if (!ssaGetProp(i))
                    a_.jmp(ssaExit(i));
                break;
            case Op::PutProp:
                if (!native_[i])
                    ssaGeneric(i, liveAfter[b][k]);
                else if (!ssaPutProp(i, liveAfter[b][k]))
                    a_.jmp(ssaExit(i));
                break;
            case Op::TypeOf:
                if (!native_[i])
                    ssaGeneric(i, liveAfter[b][k]);
                else
                    ssaTypeOf(i);
                break;
            case Op::GetUpval: case Op::GetBox: case Op::PutUpval: case Op::PutBox: case Op::ToNumeric: case Op::Not:
                if (!native_[i])
                    ssaGeneric(i, liveAfter[b][k]);
                else if (!ssaSmall(i, liveAfter[b][k]))
                    a_.jmp(ssaExit(i));
                break;
            case Op::GetElem:
                if (!native_[i])
                    ssaGeneric(i, liveAfter[b][k]);
                else if (!ssaGetElem(i))
                    a_.jmp(ssaExit(i));
                break;
            case Op::PutElem:
                if (!native_[i])
                    ssaGeneric(i, liveAfter[b][k]);
                else if (!ssaPutElem(i, liveAfter[b][k]))
                    a_.jmp(ssaExit(i));
                break;
            case Op::GetGlobal:
                if (!native_[i])
                    ssaGeneric(i, liveAfter[b][k]);
                else if (!ssaGetGlobal(i))
                    a_.jmp(ssaExit(i));
                break;
            case Op::Eq: case Op::Ne: case Op::StrictEq: case Op::StrictNe:
                if (!native_[i])
                    ssaGeneric(i, liveAfter[b][k]);
                else if (!ssaEquals(i))
                    a_.jmp(ssaExit(i));
                break;
            case Op::Ret:
                ssaRet(i);
                ended = true;
                break;
            case Op::AddI: case Op::SubI: case Op::Inc: case Op::Dec: case Op::BitAndI: case Op::BitOrI: case Op::BitXorI:
            case Op::ShlI: case Op::ShrI: case Op::Add: case Op::Sub: case Op::Mul: case Op::BitAnd: case Op::BitOr:
            case Op::BitXor: case Op::Lt: case Op::Le: case Op::Gt: case Op::Ge: case Op::Div: case Op::MulI:
                if (!native_[i])
                    ssaGeneric(i, liveAfter[b][k]);
                else if (!(dblArith_[i] ? ssaArithF64(i) : ssaArith(i)))
                    a_.jmp(ssaExit(i));
                break;
            case Op::Nop:
                break;
            case Op::Mov:
                name_[in.a] = name_[in.b];
                break;
            case Op::LoadUndef: case Op::LoadNull: case Op::LoadTrue: case Op::LoadFalse: case Op::LoadHole:
            case Op::LoadUninit: case Op::LoadInt: case Op::LoadK:
                name_[in.a] = g.defBase[i];
                break;
            default:
                ssaGeneric(i, liveAfter[b][k]);
                if (op == Op::Ret || op == Op::Throw || op == Op::ThrowError)
                    ended = true;
                break;
            }
        }
        if (!ended && !ssaFailed_)
            ssaEdge(b, bl.succs[0]);
        if (b == cacheEnd_ && !cacheSaved_.empty()) {
            for (const auto& [v, r] : cacheSaved_)
                sval_[v].reg = r;
            cacheSaved_.clear();
            cacheEnd_ = 0;
        }
    }
    if (ssaFailed_)
        return fail();
    a_.int3();

    // ---- ways in from the baseline code at loop heads: the frame is all written there ----
    std::vector<std::pair<std::uint32_t, std::uint32_t>> osr;
    for (std::uint32_t b = 1; b < nb; ++b) {
        const ssa::Block& bl = g.blocks[b];
        if (!bl.reached || !bl.loopHeader)
            continue;
        osr.emplace_back(bl.start, std::uint32_t(a_.here()));
        a_.aluI(5, RSP, spill_);
        std::vector<std::uint8_t> done(g.values.size(), 0);
        Label fail;
        for (unsigned r = 0; r < nregs; ++r) {
            const std::uint32_t v = bl.nameIn[r];
            if (v == ssa::kNone || done[v])
                continue;
            done[v] = 1;
            a_.mov(RAX, R(r));
            if (hasLoc(v)) {
                const SVal& sv = sval_[v];
                if (sv.rep == kRepI32) {
                    needInt(RAX, fail);
                    if (sv.reg >= 0)
                        a_.mov32(Reg(sv.reg), RAX);
                    else
                        a_.mov32(slotOf(v), RAX);
                } else if (sv.rep == kRepF64) {
                    needNumber(RAX, fail);
                    loadNumber(0, RAX, RDX);
                    if (sv.reg >= 0)
                        a_.movaps(sv.reg, 0);
                    else
                        a_.movsdStore(slotOf(v), 0);
                } else {
                    storeTagged(v, RAX);
                }
            }
            // What the loop's code takes as known of it.
            for (const Fact& f : headerFacts_[b]) {
                if (f.v != v)
                    continue;
                const std::uint8_t bits = std::uint8_t(f.bits & ~vDef_[v]);
                if (bits & kVInt) {
                    needInt(RAX, fail);
                } else if (bits & kVBool) {
                    a_.mov(RDX, RAX);
                    a_.aluI(4, RDX, -2);
                    a_.cmpI(RDX, std::int32_t(kFalse));
                    a_.jcc(CNE, fail);
                } else if (bits & kVObj) {
                    needObject(RAX, fail);
                } else if (bits & kVNotCell) {
                    Label ok;
                    a_.test(RAX, R15);
                    a_.jcc(CNE, ok);
                    a_.test(RAX, RAX);
                    a_.jcc(CNE, fail);
                    a_.bind(ok);
                }
            }
        }
        // The registers of the loop with a cache this head is in (its own, or one around it).
        for (std::uint32_t h = 1; h <= b; ++h)
            if (!loopCache_[h].empty() && b <= loopLast_[h])
                ssaLoadLoopCache(h);
        a_.jmp(blockLabel_[b]);
        a_.bind(fail);
        a_.aluI(0, RSP, spill_);
        a_.mov(RDI, R12);
        a_.movImm(RAX, reinterpret_cast<std::uint64_t>(&scriptVmOptEntryFail));
        a_.callC(RAX);
        a_.movImm(RAX, reinterpret_cast<std::uint64_t>(base_->addressFor(bl.start)));
        a_.jmp(RAX);
    }
    emitTails();
    CodeSpan span = a_.failed() ? CodeSpan{} : allocateCode(a_.buf.size());
    if (!span)
        return fail();
    commitCode(span, a_.buf.data(), a_.buf.size());
    if (const char* mapFile = std::getenv("T2MAP")) {
        // One line per instruction: where its code starts (as the optimizing compiler writes them).
        if (std::FILE* f = std::fopen(mapFile, "a")) {
            for (std::uint32_t j = 0; j < n_; j += length(j))
                if (offsets_[j] != ~0u)
                    std::fprintf(f, "%llx %s %u %s %u\n", static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(span.rx)),
                                 "fn", unsigned(code_->lines.empty() ? 0 : code_->lines[0]), opName(code_->code[j].opcode()),
                                 unsigned(offsets_[j]));
            std::fclose(f);
        }
    }
    auto* t = new Tier2Code;
    t->rx = span.rx;
    t->bytes = span.bytes;
    t->entry = t->rx;
    t->osr = std::move(osr);
    t->validities = std::move(validities_);
    t->held = std::move(held_);
#if defined(SCRIPT_JIT_ENV_OVERRIDE)
    perfMapNote(t->rx, t->bytes, code_, "js3");
#endif
    return t;
}

void Baseline::emitSsaTails()
{
    emitColdPaths();
    for (SsaExit& e : ssaExits_) {
        a_.bind(e.label);
        name_ = e.name;
        frame_ = e.frame;
        std::vector<unsigned> all;
        for (unsigned r = 0; r < code_->nregs; ++r)
            if (ssa::bitTest(e.live, r))
                all.push_back(r);
        ssaSync(all, e.live, std::vector<std::uint8_t>(code_->nregs, 0));
        a_.aluI(0, RSP, spill_);
        a_.mov(RDI, R12);
        a_.movImm(RSI, e.i);
        a_.movImm(RDX, std::uint64_t(e.kind));
        a_.movImm(RAX, reinterpret_cast<std::uint64_t>(&scriptVmOptExit));
        a_.callC(RAX);
        Label notFall;
        a_.cmpI(RAX, std::int32_t(kStepFall));
        a_.jcc(CNE, notFall);
        a_.movImm(RAX, reinterpret_cast<std::uint64_t>(base_->addressFor(e.i + length(e.i))));
        a_.jmp(RAX);
        a_.bind(notFall);
        a_.jmp(tailBase_);
    }
}

// ---- M2: what is known of values, exits, and the instructions done in place ----

void Baseline::ssaKnow(std::uint32_t v, std::uint8_t bits, std::uint32_t shape)
{
    vKnown_[v] |= bits;
    if (shape)
        vShape_[v] = shape;
    touched_.push_back(v);
}

std::vector<Baseline::Fact> Baseline::ssaFacts()
{
    std::vector<std::uint32_t> vs = touched_;
    std::sort(vs.begin(), vs.end());
    vs.erase(std::unique(vs.begin(), vs.end()), vs.end());
    std::vector<Fact> out;
    for (std::uint32_t v : vs)
        if (vKnown_[v] != vDef_[v] || vShape_[v])
            out.push_back(Fact{v, vKnown_[v], vShape_[v]});
    return out;
}

void Baseline::ssaForgetShapes()
{
    for (std::uint32_t v : touched_)
        vShape_[v] = 0;
}

void Baseline::ssaResetFacts()
{
    for (std::uint32_t v : touched_) {
        vKnown_[v] = vDef_[v];
        vShape_[v] = 0;
    }
    touched_.clear();
}

void Baseline::needIntV(std::uint32_t v, Reg r, Label& bad)
{
    if (known(v, kVInt))
        return;
    needInt(r, bad);
    ssaKnow(v, kVInt | kVNotCell);
}

void Baseline::needObjV(std::uint32_t v, Reg r, Label& bad)
{
    if (known(v, kVObj))
        return;
    needObject(r, bad);
    ssaKnow(v, kVObj);
}

/// The way out before instruction `i`, as things are now: every live register written to its slot, then the instruction
/// run in the interpreter (`scriptVmOptExit`) and the baseline code taken from there.
Label& Baseline::ssaExit(std::uint32_t i)
{
    if (exitAt_ == std::int64_t(i))
        return *exitLabel_;
    ssaExits_.push_back(SsaExit{i, name_, frame_, liveBefore_, Label{}});
    exitAt_ = i;
    exitLabel_ = &ssaExits_.back().label;
    return *exitLabel_;
}

/// The instruction's result, in rax, is the value it defines (`bits`: what that is known to be).
void Baseline::ssaDef(std::uint32_t i, std::uint8_t bits)
{
    const std::uint32_t v = ssa_->defBase[i];
    name_[code_->code[i].a] = v;
    vDef_[v] = bits;
    vKnown_[v] = bits;
    storeTagged(v, RAX);
}

/// The instruction's result is the int32 in the low half of rax.
void Baseline::ssaDefI32(std::uint32_t i)
{
    const std::uint32_t v = ssa_->defBase[i];
    name_[code_->code[i].a] = v;
    const SVal& s = sval_[v];
    if (s.reg >= 0)
        a_.mov32(Reg(s.reg), RAX);
    else if (s.slot >= 0)
        a_.mov32(slotOf(v), RAX);
}

/// The instruction's result is the double in xmm`x`.
void Baseline::ssaDefF64(std::uint32_t i, int x)
{
    const std::uint32_t v = ssa_->defBase[i];
    name_[code_->code[i].a] = v;
    const SVal& s = sval_[v];
    if (s.reg >= 0) {
        if (s.reg != x)
            a_.movaps(s.reg, x);
    } else if (s.slot >= 0) {
        a_.movsdStore(slotOf(v), x);
    }
}

/// The flags say whether the jump of block `b` is taken: both ways out.
void Baseline::ssaCondEdges(std::uint32_t b, Cond taken)
{
    const ssa::Block& bl = ssa_->blocks[b];
    const std::vector<std::uint32_t> st = frame_;
    Label yes;
    a_.jcc(taken, yes);
    ssaEdge(b, bl.succs[0]);
    a_.bind(yes);
    frame_ = st;
    ssaEdge(b, bl.succs[1]);
}

namespace {
Cond invert(Cond c)
{
    return Cond(std::uint8_t(c) ^ 1);
}
}  // namespace

bool Baseline::ssaBranch(std::uint32_t b, std::uint32_t i)
{
    const Ins& in = code_->code[i];
    const Op op = in.opcode();
    const ssa::Block& bl = ssa_->blocks[b];
    const bool failed = i < code_->prof.size() && (code_->prof[i] & opt::kProfFailed);
    auto plainConst = [&](std::uint32_t v) {
        // A constant that is no number and no cell: strict (and loose, against another such) equality is equality of bits.
        if (!sval_[v].isConst)
            return false;
        const JSValue k = JSValue::raw(sval_[v].bits);
        return !k.isNumber() && !k.isCell();
    };
    switch (op) {
    case Op::JNullish: case Op::JNotNullish:
        loadValue(name_[in.a], RAX);
        a_.aluI(4, RAX, -9);
        a_.cmpI(RAX, std::int32_t(kNull));
        ssaCondEdges(b, op == Op::JNullish ? CE : CNE);
        return true;
    case Op::JUndefined: case Op::JNotUndefined: case Op::JHole:
        loadValue(name_[in.a], RAX);
        a_.cmpI(RAX, std::int32_t(op == Op::JHole ? kHole : kUndefined));
        ssaCondEdges(b, op == Op::JNotUndefined ? CNE : CE);
        return true;
    case Op::JTrue: case Op::JFalse: {
        const std::uint32_t v = name_[in.a];
        loadValue(v, RAX);
        const bool onTrue = op == Op::JTrue;
        if (known(v, kVBool)) {
            a_.cmpI(RAX, std::int32_t(kTrue));
            ssaCondEdges(b, onTrue ? CE : CNE);
            return true;
        }
        if (failed)
            return false;
        Label& bad = ssaExit(i);
        Label t, f, notInt;
        a_.cmpI(RAX, std::int32_t(kTrue));
        a_.jcc(CE, t);
        a_.cmpI(RAX, std::int32_t(kFalse));
        a_.jcc(CE, f);
        a_.cmp(RAX, R14);
        a_.jcc(CB, notInt);
        a_.test32(RAX, RAX);
        a_.jcc(CNE, t);
        a_.jmp(f);
        a_.bind(notInt);
        a_.mov(RDX, RAX);
        a_.aluI(4, RDX, -9);
        a_.cmpI(RDX, std::int32_t(kNull));
        a_.jcc(CE, f);
        needObject(RAX, bad);  // an object is true; a string or a double is left to the interpreter
        a_.jmp(t);
        const std::vector<std::uint32_t> st = frame_;
        a_.bind(f);
        ssaEdge(b, bl.succs[onTrue ? 0 : 1]);
        a_.bind(t);
        frame_ = st;
        ssaEdge(b, bl.succs[onTrue ? 1 : 0]);
        return true;
    }
    case Op::JStrictEq: case Op::JStrictNe: case Op::JEq: case Op::JNe: {
        const std::uint32_t va = name_[in.a], vb = name_[in.b];
        const bool eqTaken = op == Op::JStrictEq || op == Op::JEq;
        const bool loose = op == Op::JEq || op == Op::JNe;
        const bool pa = plainConst(va), pb = plainConst(vb);
        if (loose && (pa || pb)) {
            // Loose against null or undefined is a test for either; against a boolean it converts: not here.
            const JSValue k = JSValue::raw(sval_[pa ? va : vb].bits);
            if (!k.isNullish())
                return false;
            loadValue(pa ? vb : va, RAX);
            a_.aluI(4, RAX, -9);
            a_.cmpI(RAX, std::int32_t(kNull));
            ssaCondEdges(b, eqTaken ? CE : CNE);
            return true;
        }
        loadValue(va, RAX);
        loadValue(vb, RCX);
        if (pa || pb || (known(va, kVInt) && known(vb, kVInt))) {
            a_.cmp(RAX, RCX);
            ssaCondEdges(b, eqTaken ? CE : CNE);
            return true;
        }
        if (failed && loose)
            return false;
        Label& bad = ssaExit(i);
        ssaEqualsCore(i, loose, bad);
        a_.test32(RAX, RAX);
        ssaCondEdges(b, eqTaken ? CNE : CE);
        return true;
    }
    case Op::JStrictEqI: case Op::JStrictNeI: {
        const std::uint32_t v = name_[in.a];
        const std::int32_t imm = std::int32_t(std::int16_t(in.b));
        if (failed && !known(v, kVInt))
            return false;
        loadValue(v, RAX);
        if (!known(v, kVInt)) {
            // Not a number: not equal. A double: the interpreter's.
            Label& bad = ssaExit(i);
            Label isInt;
            a_.cmp(RAX, R14);
            a_.jcc(CAE, isInt);
            a_.test(RAX, R14);
            a_.jcc(CNE, bad);
            a_.movImm(RAX, kUndefined);  // equal to no int32
            a_.bind(isInt);
        }
        a_.movImm(RCX, kNumberTag | std::uint32_t(imm));
        a_.cmp(RAX, RCX);
        ssaCondEdges(b, op == Op::JStrictEqI ? CE : CNE);
        return true;
    }
    case Op::JLt: case Op::JLe: case Op::JGt: case Op::JGe: case Op::JNotLt: case Op::JNotLe: case Op::JNotGt:
    case Op::JNotGe: case Op::JLtI: case Op::JLeI: case Op::JGtI: case Op::JGeI: case Op::JNotLtI: case Op::JNotLeI:
    case Op::JNotGtI: case Op::JNotGeI: {
        const bool imm = op >= Op::JLtI && op <= Op::JNotGeI;
        const std::uint32_t va = name_[in.a], vb = imm ? ssa::kNone : name_[in.b];
        if (failed && !(known(va, kVInt) && (imm || known(vb, kVInt))))
            return false;
        Label& bad = ssaExit(i);
        ssaRelation(i, va, vb, imm ? std::int32_t(std::int16_t(in.b)) : 0, imm, bad,
                    [&](Cond c) { ssaCondEdges(b, c); });
        return true;
    }
    case Op::JNotCode: {
        // The register holds a function of the code whose uid the constant names, and a `this` it can take as it is.
        const std::uint64_t uid = std::uint64_t(code_->consts[in.b].asInt32());
        CodeBlock* callee = vm_.codeByUid(uid);
        const std::uint32_t vf = name_[in.a], vt = name_[in.a + 1u];
        Label no, yes;
        loadValue(vf, RAX);
        if (!known(vf, kVObj)) {
            a_.test(RAX, R15);
            a_.jcc(CNE, no);
            a_.cmpI8(mem(RAX, kHdrKind), kObjectKind);
            a_.jcc(CNE, no);
        }
        a_.cmpI8(mem(RAX, kHdrCls), std::uint8_t(ObjClass::Function));
        a_.jcc(CNE, no);
        a_.mov(RCX, mem(RAX, kFnCode));
        if (callee) {
            a_.movImm(RDX, reinterpret_cast<std::uint64_t>(callee));
            a_.cmp(RDX, RCX);
        } else {
            a_.test(RCX, RCX);
            a_.jcc(CE, no);
            a_.movImm(RDX, uid);
            a_.cmp(RDX, mem(RCX, kCbUid));
        }
        a_.jcc(CNE, no);
        if ((!callee || !(callee->flags & (CodeBlock::kStrict | CodeBlock::kArrow | CodeBlock::kNoThis))) &&
            !known(vt, kVObj)) {
            loadValue(vt, RAX);
            a_.test(RAX, R15);
            a_.jcc(CNE, no);
            a_.cmpI8(mem(RAX, kHdrKind), kObjectKind);
            a_.jcc(CNE, no);
        }
        const std::vector<std::uint32_t> st = frame_;
        ssaEdge(b, bl.succs[0]);
        a_.bind(no);
        frame_ = st;
        ssaEdge(b, bl.succs[1]);
        (void)yes;
        return true;
    }
    default:
        return false;
    }
}

bool Baseline::ssaGetProp(std::uint32_t i)
{
    const Ins& in = code_->code[i];
    if (!plainReadable(i))
        return false;
    const InlineCache& ic = code_->caches[code_->code[i + 1].a];
    struct Entry
    {
        std::uint32_t shape, index;
        JSObject* holder;
        ValidityCell* validity;
    };
    std::vector<Entry> es{{ic.shape, ic.index, ic.holder, ic.validity}};
    if (ic.pad && ic.pad <= code_->poly.size())
        for (std::uint8_t k = 0; k < code_->poly[ic.pad - 1].n; ++k) {
            const PolyEntry& e = code_->poly[ic.pad - 1].e[k];
            es.push_back({e.shape, e.index, e.kind == PolyEntry::ProtoData ? e.holder : nullptr, e.validity});
        }
    const std::uint32_t v = name_[in.b];
    Label& bad = ssaExit(i);
    loadValue(v, RAX);
    needObjV(v, RAX, bad);
    std::vector<std::size_t> emit;
    if (vShape_[v]) {
        for (std::size_t k = 0; k < es.size() && emit.empty(); ++k)
            if (es[k].shape == vShape_[v])
                emit.push_back(k);
        if (emit.empty())
            return false;  // known to be of a shape the site has not seen
    } else {
        for (std::size_t k = 0; k < es.size(); ++k)
            emit.push_back(k);
    }
    std::vector<Label> at(es.size());
    Label done;
    if (!vShape_[v]) {
        for (std::size_t k : emit) {
            a_.cmpI32(mem(RAX, 0), std::int32_t(es[k].shape));
            a_.jcc(CE, at[k]);
        }
        a_.jmp(bad);
    }
    for (std::size_t k : emit) {
        a_.bind(at[k]);
        const Entry& e = es[k];
        if (e.holder) {
            hold(e.validity);
            a_.movImm(RCX, reinterpret_cast<std::uint64_t>(e.validity));
            a_.cmpI32(mem(RCX, kValidityValid), 0);
            a_.jcc(CE, bad);
            holderRead(e.holder, e.index);
        } else {
            slotAddress(e.index);
            a_.mov(RAX, mem(RDX, 0));
        }
        a_.jmp(done);
    }
    a_.bind(done);
    if (emit.size() == 1)
        ssaKnow(v, kVObj, es[emit[0]].shape);
    ssaDef(i, 0);
    return true;
}

bool Baseline::ssaPutProp(std::uint32_t i, const ssa::Bits& liveAfter)
{
    const Ins& in = code_->code[i];
    if (!ssaPutFast(i))
        return false;
    const InlineCache& ic = code_->caches[code_->code[i + 1].a];
    const std::uint32_t v = name_[in.a];
    if (vShape_[v] && vShape_[v] != ic.shape)
        return false;
    // The old value is let go of: every live value gets an owner first.
    ssaSync({}, liveAfter, std::vector<std::uint8_t>(code_->nregs, 0));
    if (ssaFailed_)
        return true;
    const std::uint32_t x = name_[in.c];
    Label& bad = ssaExit(i);
    loadValue(v, RAX);
    needObjV(v, RAX, bad);
    if (!vShape_[v]) {
        a_.cmpI32(mem(RAX, 0), std::int32_t(ic.shape));
        a_.jcc(CNE, bad);
        ssaKnow(v, kVObj, ic.shape);
    }
    slotAddress(ic.index);
    loadValue(x, RCX);
    if (mayBeCell(x))
        retain(RCX);
    a_.mov(RDI, mem(RDX, 0));
    a_.mov(mem(RDX, 0), RCX);
    release(RDI);
    return true;
}

bool Baseline::ssaArith(std::uint32_t i)
{
    const Ins& in = code_->code[i];
    const Op op = in.opcode();
    const bool compare = op == Op::Lt || op == Op::Le || op == Op::Gt || op == Op::Ge;
    if (!compare && !intArith_[i])
        return false;
    const bool rr = op == Op::Add || op == Op::Sub || op == Op::Mul || op == Op::BitAnd || op == Op::BitOr ||
                    op == Op::BitXor || compare;
    const std::uint32_t va = name_[in.b];
    const std::uint32_t vb = rr ? name_[in.c] : ssa::kNone;
    Label& bad = ssaExit(i);
    if (compare) {
        // Into a boolean: rax 0 or 1, then false or true.
        Label done;
        ssaRelation(i, va, vb, 0, false, bad, [&](Cond c) {
            a_.setcc(c, RAX);
            a_.movzx8(RAX, RAX);
            a_.jmp(done);
        });
        a_.bind(done);
        a_.aluI32(1, RAX, std::int32_t(kFalse));
        ssaDef(i, kVBool | kVNotCell);
        return true;
    }
    const bool bitwise = op == Op::BitAnd || op == Op::BitOr || op == Op::BitXor || op == Op::BitAndI ||
                         op == Op::BitOrI || op == Op::BitXorI || op == Op::ShlI || op == Op::ShrI;
    if (bitwise) {
        // ToInt32 of each operand: a double truncates.
        loadToInt32(va, RAX, bad);
        if (rr) {
            a_.mov(RDI, RAX);
            loadToInt32(vb, RCX, bad);
            a_.mov(RAX, RDI);
        }
    } else {
        loadI32(va, RAX, &bad);
        if (rr)
            loadI32(vb, RCX, &bad);
    }
    const std::int32_t imm = op == Op::Inc ? 1 : op == Op::Dec ? -1 : std::int32_t(std::int16_t(in.c));
    switch (op) {
    case Op::AddI: case Op::Inc: case Op::Dec: a_.aluI32(0, RAX, imm); a_.jcc(CO, bad); break;
    case Op::SubI: a_.aluI32(5, RAX, imm); a_.jcc(CO, bad); break;
    case Op::BitAndI: a_.aluI32(4, RAX, imm); break;
    case Op::BitOrI: a_.aluI32(1, RAX, imm); break;
    case Op::BitXorI: a_.aluI32(6, RAX, imm); break;
    case Op::ShlI: a_.shiftI32(4, RAX, imm & 31); break;
    case Op::ShrI: a_.shiftI32(7, RAX, imm & 31); break;
    case Op::Add: a_.alu32(0, RAX, RCX); a_.jcc(CO, bad); break;
    case Op::Sub: a_.alu32(5, RAX, RCX); a_.jcc(CO, bad); break;
    case Op::BitAnd: a_.alu32(4, RAX, RCX); break;
    case Op::BitOr: a_.alu32(1, RAX, RCX); break;
    case Op::BitXor: a_.alu32(6, RAX, RCX); break;
    case Op::MulI: {
        // An overflow, or a zero that may be a negative zero: the interpreter's.
        a_.imulI32(RDX, RAX, imm);
        a_.jcc(CO, bad);
        Label nz;
        a_.test32(RDX, RDX);
        a_.jcc(CNE, nz);
        if (imm < 0) {
            a_.jmp(bad);
        } else {
            a_.test32(RAX, RAX);
            a_.jcc(CS, bad);
        }
        a_.bind(nz);
        a_.mov32(RAX, RDX);
        break;
    }
    case Op::Mul: {
        // An overflow, or a zero that may be a negative zero (an operand below zero): the interpreter's.
        a_.mov32(RDX, RAX);
        a_.imul32(RAX, RCX);
        a_.jcc(CO, bad);
        Label nz;
        a_.test32(RAX, RAX);
        a_.jcc(CNE, nz);
        a_.alu32(1, RDX, RCX);
        a_.jcc(CS, bad);
        a_.bind(nz);
        break;
    }
    default:
        break;
    }
    ssaDefI32(i);
    return true;
}

/// Arithmetic on numbers where the site has made doubles (and maybe int32s), nothing else: in xmm0.
bool Baseline::ssaArithF64(std::uint32_t i)
{
    const Ins& in = code_->code[i];
    const Op op = in.opcode();
    const bool rr = op == Op::Add || op == Op::Sub || op == Op::Mul || op == Op::Div;
    Label& bad = ssaExit(i);
    // In the result's own register when it has one that is not the second operand's; the second operand where it is.
    const std::uint32_t def = ssa_->defBase[i];
    const std::uint32_t vb = rr ? name_[in.c] : ssa::kNone;
    const bool inReg = rr && !sval_[vb].isConst && sval_[vb].rep == kRepF64 && sval_[vb].reg >= 0;
    const int y = inReg ? sval_[vb].reg : 1;
    int x = sval_[def].reg >= 0 && sval_[def].rep == kRepF64 && sval_[def].reg != y ? sval_[def].reg : 0;
    if (rr && !inReg)
        loadF64(vb, 1, &bad);
    loadF64(name_[in.b], x, &bad);
    if (!rr) {
        a_.movImm(RDX, std::uint32_t(std::int32_t(std::int16_t(in.c))));
        a_.cvtsi2sd32(1, RDX);
    }
    switch (op) {
    case Op::Add: case Op::AddI: a_.addsd(x, y); break;
    case Op::Sub: case Op::SubI: a_.subsd(x, y); break;
    case Op::Mul: case Op::MulI: a_.mulsd(x, y); break;
    default: a_.divsd(x, y); break;
    }
    ssaDefF64(i, x);
    return true;
}

bool Baseline::ssaPutFast(std::uint32_t i) const
{
    if (i + 1 >= n_ || code_->code[i + 1].opcode() != Op::Cache)
        return false;
    const std::uint16_t cacheId = code_->code[i + 1].a;
    if (cacheId == kNoCache || (i < code_->prof.size() && (code_->prof[i] & opt::kProfFailed)))
        return false;
    const InlineCache& ic = code_->caches[cacheId];
    return ic.shape != 0 && ic.next == 0 && !ic.holder && !ic.pad;
}

bool Baseline::ssaGlobalFast(std::uint32_t i) const
{
    const Ins& in = code_->code[i];
    if (i < code_->prof.size() && (code_->prof[i] & opt::kProfFailed) && code_->tier2Tries >= 2)
        return false;
    std::uint8_t attrs = 0;
    const JSValue* slot = vm_.global->findOwn(vm_, code_->atoms[in.b], &attrs);
    return slot && !(attrs & attr::kAccessor);
}

/// Whether instruction `i` is done in place (no template, no step: nothing it does clobbers a register), as the code
/// generator will decide it knowing at least what is known now. Called with `name_` and `vKnown_` as they are before it.
bool Baseline::ssaNative(std::uint32_t i)
{
    const Ins& in = code_->code[i];
    const Op op = in.opcode();
    switch (op) {
    case Op::Nop: case Op::Mov: case Op::LoadUndef: case Op::LoadNull: case Op::LoadTrue: case Op::LoadFalse:
    case Op::LoadHole: case Op::LoadUninit: case Op::LoadInt: case Op::LoadK: case Op::Jmp: case Op::Ret:
        return true;
    default:
        break;
    }
    if (ssaOn_ < 2)
        return false;
    const bool failed = i < code_->prof.size() && (code_->prof[i] & opt::kProfFailed);
    auto plainConst = [&](std::uint32_t v) {
        if (!sval_[v].isConst)
            return false;
        const JSValue k = JSValue::raw(sval_[v].bits);
        return !k.isNumber() && !k.isCell();
    };
    auto equalsNative = [&](std::uint32_t va, std::uint32_t vb, bool loose) {
        const bool pa = plainConst(va), pb = plainConst(vb);
        if (loose && (pa || pb))
            return JSValue::raw(sval_[pa ? va : vb].bits).isNullish();
        if (pa || pb || (known(va, kVInt) && known(vb, kVInt)) || !loose)
            return true;
        return !failed;
    };
    switch (op) {
    case Op::GetProp: return plainReadable(i);
    case Op::PutProp: return ssaPutFast(i);
    case Op::GetGlobal: return ssaGlobalFast(i);
    case Op::GetUpval: case Op::GetBox: case Op::PutUpval: case Op::PutBox:
        return true;
    case Op::ToNumeric: case Op::Not: case Op::TypeOf:
        return !failed;
    case Op::GetElem: case Op::PutElem: {
        ElemKind kind;
        return ssaElemPlan(i, op == Op::PutElem, &kind) != 0;
    }
    case Op::Eq: case Op::Ne: case Op::StrictEq: case Op::StrictNe:
        return equalsNative(name_[in.b], name_[in.c], op == Op::Eq || op == Op::Ne);
    case Op::AddI: case Op::SubI: case Op::Inc: case Op::Dec: case Op::BitAndI: case Op::BitOrI: case Op::BitXorI:
    case Op::ShlI: case Op::ShrI: case Op::Add: case Op::Sub: case Op::Mul: case Op::BitAnd: case Op::BitOr:
    case Op::BitXor: case Op::Div: case Op::MulI:
        return intArith_[i] || dblArith_[i];
    case Op::Lt: case Op::Le: case Op::Gt: case Op::Ge:
        return !failed;
    case Op::JNullish: case Op::JNotNullish: case Op::JUndefined: case Op::JNotUndefined: case Op::JHole:
    case Op::JNotCode:
        return true;
    case Op::JTrue: case Op::JFalse:
        return known(name_[in.a], kVBool) || !failed;
    case Op::JStrictEq: case Op::JStrictNe: case Op::JEq: case Op::JNe:
        return equalsNative(name_[in.a], name_[in.b], op == Op::JEq || op == Op::JNe);
    case Op::JStrictEqI: case Op::JStrictNeI:
        return !failed || known(name_[in.a], kVInt);
    case Op::JLt: case Op::JLe: case Op::JGt: case Op::JGe: case Op::JNotLt: case Op::JNotLe: case Op::JNotGt:
    case Op::JNotGe:
        return !failed || (known(name_[in.a], kVInt) && known(name_[in.b], kVInt));
    case Op::JLtI: case Op::JLeI: case Op::JGtI: case Op::JGeI: case Op::JNotLtI: case Op::JNotLeI: case Op::JNotGtI:
    case Op::JNotGeI:
        return !failed || known(name_[in.a], kVInt);
    default:
        return false;
    }
}

/// Before code: each value's representation, which instructions are done in place, and where each value lives (a
/// register for its whole life when nothing between its definition and its last use clobbers registers, else a slot).
bool Baseline::ssaPlan(const std::vector<std::vector<ssa::Bits>>& liveAfter)
{
    const ssa::Graph& g = *ssa_;
    const std::uint32_t nb = std::uint32_t(g.blocks.size());
    const std::uint32_t nv = std::uint32_t(g.values.size());
    native_.assign(n_, 0);
    intArith_.assign(n_, 0);
    dblArith_.assign(n_, 0);
    for (std::uint32_t i = 0; i < n_; i = opt::nextOf(*code_, i)) {
        const Op op = code_->code[i].opcode();
        const std::uint8_t prof = i < code_->prof.size() ? code_->prof[i] : 0;
        if (ssaOn_ < 2 || (prof & opt::kProfFailed))
            continue;
        switch (op) {
        case Op::AddI: case Op::SubI: case Op::Inc: case Op::Dec: case Op::BitAndI: case Op::BitOrI: case Op::BitXorI:
        case Op::ShlI: case Op::ShrI: case Op::MulI: case Op::Add: case Op::Sub: case Op::Mul: case Op::BitAnd:
        case Op::BitOr: case Op::BitXor:
            if (prof == 1)
                intArith_[i] = 1;
            break;
        default:
            break;
        }
        switch (op) {
        case Op::Add: case Op::Sub: case Op::Mul: case Op::Div: case Op::AddI: case Op::SubI: case Op::MulI:
            if ((prof & 2) && !(prof & 8))
                dblArith_[i] = 1;
            break;
        default:
            break;
        }
    }
    // An element read of a typed array makes a number of its kind.
    std::vector<std::uint8_t> elemRep(n_, 3);
    for (std::uint32_t i = 0; i < n_; i = opt::nextOf(*code_, i)) {
        ElemKind kind;
        if (code_->code[i].opcode() != Op::GetElem)
            continue;
        const int plan = ssaElemPlan(i, false, &kind);
        const std::uint8_t prof = i < code_->prof.size() ? code_->prof[i] : 0;
        if (plan == 2)
            elemRep[i] = kind == ElemKind::Float64 || kind == ElemKind::Float32 ? 2 : 1;
        else if (plan == 1 && prof && !(prof & ~3))
            elemRep[i] = prof == 1 ? 1 : 2;  // a plain array the site found numbers in, only
    }
    // Representations: an instruction's by what it does; a phi's the least that holds every input.
    std::vector<std::uint8_t> cls(nv, 0);  // 0 none yet, 1 int32, 2 double, 3 boxed
    for (std::uint32_t v = 0; v < nv; ++v) {
        const ssa::Value& sv = g.values[v];
        if (sval_[v].isConst) {
            const JSValue k = JSValue::raw(sval_[v].bits);
            cls[v] = k.isInt32() ? 1 : k.isDouble() ? 2 : 3;
        } else if (sv.kind == ssa::Value::Def) {
            cls[v] = intArith_[sv.at] ? 1 : dblArith_[sv.at] ? 2 : elemRep[sv.at];
        } else if (sv.kind == ssa::Value::Entry) {
            cls[v] = 3;
        }
    }
    for (bool changed = true; changed;) {
        changed = false;
        for (std::uint32_t b = 1; b < nb; ++b)
            for (std::uint32_t p : g.blocks[b].phis) {
                std::uint8_t c = cls[p];
                for (std::uint32_t in : g.values[p].in)
                    if (in != p)
                        c = std::max(c, cls[in]);
                if (c != cls[p]) {
                    cls[p] = c;
                    changed = true;
                }
            }
    }
    for (std::uint32_t v = 0; v < nv; ++v) {
        if (sval_[v].isConst)
            continue;
        sval_[v].rep = cls[v] == 1 ? kRepI32 : cls[v] == 2 ? kRepF64 : kRepTagged;
        if (sval_[v].rep == kRepI32)
            vDef_[v] = kVInt | kVNotCell;
        else if (sval_[v].rep == kRepF64)
            vDef_[v] = kVNotCell;
    }
    vKnown_ = vDef_;
    // In place or not, with names as the code will have them; then where each value is first and last needed.
    std::vector<std::int32_t> first(nv, INT32_MAX), last(nv, -1);
    std::vector<std::int32_t> clob;
    auto touch = [&](std::uint32_t v, std::int32_t at) {
        if (v == ssa::kNone || sval_[v].isConst)
            return;
        first[v] = std::min(first[v], at);
        last[v] = std::max(last[v], at);
    };
    std::int32_t pos = 0;
    std::vector<std::pair<std::int32_t, std::int32_t>> blockPos(nb, {-1, -1});
    std::vector<std::vector<std::int32_t>> uses(nv);
    for (std::uint32_t b = 1; b < nb; ++b) {
        const ssa::Block& bl = g.blocks[b];
        if (!bl.reached)
            continue;
        const std::int32_t start = pos++;
        blockPos[b].first = start;
        if (b == 1)
            // The way in (block 0) ends here: what it gives block 1's phis is needed until then.
            for (std::uint32_t p : bl.phis)
                for (std::size_t q = 0; q < bl.preds.size(); ++q)
                    if (bl.preds[q] == 0)
                        touch(g.values[p].in[q], start);
        for (unsigned r = 0; r < code_->nregs; ++r)
            if (ssa::bitTest(bl.liveIn, r))
                touch(bl.nameIn[r], start);
        for (std::uint32_t p : bl.phis)
            touch(p, start);
        name_ = bl.nameIn;
        std::size_t k = 0;
        for (std::uint32_t i = bl.start; i < bl.end; i = opt::nextOf(*code_, i), ++k) {
            const Ins& in = code_->code[i];
            const std::int32_t use = pos++, def = pos++;
            const opt::Effects e = ssa::effects(*code_, i);
            for (std::uint16_t r : e.rd)
            {
                touch(name_[r], use);
                if (name_[r] != ssa::kNone && !sval_[name_[r]].isConst)
                    uses[name_[r]].push_back(use);
            }
            native_[i] = ssaNative(i);
            if (!native_[i])
                clob.push_back(use);
            if (in.opcode() == Op::Mov) {
                name_[in.a] = name_[in.b];
                continue;
            }
            for (std::size_t w = 0; w < e.wr.size(); ++w) {
                const std::uint32_t v = g.defBase[i] + std::uint32_t(w);
                name_[e.wr[w]] = v;
                if (ssa::bitTest(liveAfter[b][k], e.wr[w]))
                    touch(v, def);
            }
            const Op op = in.opcode();
            if (native_[i] && (op == Op::Lt || op == Op::Le || op == Op::Gt || op == Op::Ge || op == Op::Eq ||
                               op == Op::Ne || op == Op::StrictEq || op == Op::StrictNe)) {
                vDef_[g.defBase[i]] = kVBool | kVNotCell;
                vKnown_[g.defBase[i]] = kVBool | kVNotCell;
            }
        }
        const std::int32_t end = pos++;
        blockPos[b].second = end;
        for (unsigned r = 0; r < code_->nregs; ++r)
            if (ssa::bitTest(bl.liveOut, r))
                touch(name_[r], end);
    }
    std::sort(clob.begin(), clob.end());
    // Linear scan, one interval per value; a value across a clobber lives in a slot.
    std::vector<std::uint32_t> order;
    for (std::uint32_t v = 0; v < nv; ++v)
        if (last[v] >= 0)
            order.push_back(v);
    std::sort(order.begin(), order.end(), [&](std::uint32_t x, std::uint32_t y) { return first[x] < first[y]; });
    std::vector<int> gprs, xmms;
#if defined(SCRIPT_JIT_ENV_OVERRIDE)
    static const bool noRegs = std::getenv("SSA_NOREGS") != nullptr;
#else
    static const bool noRegs = false;
#endif
    if (!noRegs) {
        for (int r : {int(R20), int(R19), int(R18), int(R17), int(R16), int(R11), int(R10), int(R9), int(R8)})
            if (r < 16 || r < 16 + Asm::kExtraGpr - 2)  // not R21, R22: optimized code elsewhere keeps values there
                gprs.push_back(r);
        for (int x = 2 + 14 + Asm::kExtraXmm; x-- > 2;)
            xmms.push_back(x);
    }
    struct Active
    {
        std::int32_t end;
        int reg;
        bool x;
    };
    std::vector<Active> active;
    std::int32_t slots = 0;
    for (std::uint32_t v : order) {
        for (std::size_t a = 0; a < active.size();) {
            if (active[a].end < first[v]) {
                (active[a].x ? xmms : gprs).push_back(active[a].reg);
                active[a] = active.back();
                active.pop_back();
            } else {
                ++a;
            }
        }
        const auto c = std::upper_bound(clob.begin(), clob.end(), first[v]);
        const bool cross = c != clob.end() && *c < last[v];
        const bool x = sval_[v].rep == kRepF64;
        std::vector<int>& pool = x ? xmms : gprs;
        if (!cross && !pool.empty()) {
            sval_[v].reg = std::int8_t(pool.back());
            pool.pop_back();
            active.push_back(Active{last[v], sval_[v].reg, x});
        } else {
            sval_[v].slot = slots++;
        }
    }
    if (slots > 2048) {
        ssaWhy_ = "slots";
        return false;
    }
    spill_ = std::int32_t((8 * slots + 15) & ~15);
    // Loops that clobber no register: values from before them that live in slots, read inside, get a register for the
    // loop, loaded where it is entered (values never change, so the slot stays right and nothing is written back).
    loopCache_.assign(nb, {});
    loopLast_.assign(nb, 0);
    std::int32_t cachedUntil = -1;
    for (std::uint32_t h = 1; h < nb && !noRegs; ++h) {
        const ssa::Block& hb = g.blocks[h];
        if (!hb.reached || !hb.loopHeader || blockPos[h].first <= cachedUntil)
            continue;
        std::uint32_t lastB = h;
        for (std::uint32_t b = h + 1; b < nb && g.blocks[b].start < hb.loopEnd; ++b)
            if (g.blocks[b].reached)
                lastB = b;
        const std::int32_t from = blockPos[h].first, to = blockPos[lastB].second;
        // Clean, and entered at its head only.
        const auto c = std::lower_bound(clob.begin(), clob.end(), from);
        if (c != clob.end() && *c <= to)
            continue;
        bool closed = true;
        for (std::uint32_t b = h + 1; b <= lastB && closed; ++b)
            if (g.blocks[b].reached)
                for (std::uint32_t p : g.blocks[b].preds)
                    closed = closed && p >= h && p <= lastB;
        if (!closed)
            continue;
        std::vector<std::uint8_t> busy(64, 0);  // registers some value has inside the loop (gprs 0..31, xmms 32..)
        for (std::uint32_t v = 0; v < nv; ++v)
            if (sval_[v].reg >= 0 && last[v] >= from && first[v] <= to)
                busy[(sval_[v].rep == kRepF64 ? 32 : 0) + sval_[v].reg] = 1;
        std::vector<int> freeG, freeX;
        for (int r : {int(R8), int(R9), int(R10), int(R11), int(R16), int(R17), int(R18), int(R19), int(R20)})
            if ((r < 16 || r < 16 + Asm::kExtraGpr - 2) && !busy[r])
                freeG.push_back(r);
        for (int x = 2; x < 2 + 14 + Asm::kExtraXmm; ++x)
            if (!busy[32 + x])
                freeX.push_back(x);
        std::vector<std::pair<std::size_t, std::uint32_t>> cands;  // (reads inside, value)
        for (std::uint32_t v = 0; v < nv; ++v) {
            if (sval_[v].slot < 0 || sval_[v].isConst || first[v] >= from || last[v] < from)
                continue;
            std::size_t n = 0;
            for (std::int32_t u : uses[v])
                n += u >= from && u <= to ? 1 : 0;
            if (n)
                cands.emplace_back(n, v);
        }
        std::sort(cands.begin(), cands.end(), [](const auto& x, const auto& y) { return x.first > y.first; });
        for (const auto& [n, v] : cands) {
            std::vector<int>& pool = sval_[v].rep == kRepF64 ? freeX : freeG;
            if (pool.empty())
                continue;
            loopCache_[h].emplace_back(v, pool.back());
            pool.pop_back();
        }
        if (!loopCache_[h].empty()) {
            loopLast_[h] = lastB;
            cachedUntil = to;
        }
    }
    return true;
}

/// Load the registers loop `h` keeps values in from their slots (on a way into it from outside).
void Baseline::ssaLoadLoopCache(std::uint32_t h)
{
    for (const auto& [v, r] : loopCache_[h]) {
        if (sval_[v].rep == kRepF64)
            a_.movsdLoad(r, mem(RSP, moveRoom_ + 8 + 8 * sval_[v].slot));
        else
            a_.mov(Reg(r), mem(RSP, moveRoom_ + 8 + 8 * sval_[v].slot));
    }
}

/// The registers that hold values live here (before or after the instruction being emitted): a gpr as its number, an
/// xmm as 100 plus its number.
std::vector<int> Baseline::ssaLiveRegs() const
{
    std::vector<int> out;
    for (unsigned r = 0; r < code_->nregs; ++r) {
        if (!(ssa::bitTest(liveBefore_, r) || ssa::bitTest(curLiveAfter_, r)))
            continue;
        const std::uint32_t v = name_[r];
        if (v == ssa::kNone || sval_[v].isConst || sval_[v].reg < 0)
            continue;
        out.push_back(sval_[v].rep == kRepF64 ? 100 + sval_[v].reg : sval_[v].reg);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

void Baseline::ssaSaveRegs(const std::vector<int>& regs)
{
    if (regs.empty())
        return;
    const std::int32_t room = std::int32_t((8 * regs.size() + 15) & ~std::size_t(15));
    a_.aluI(5, RSP, room);
    for (std::size_t k = 0; k < regs.size(); ++k) {
        if (regs[k] >= 100)
            a_.movsdStore(mem(RSP, std::int32_t(8 * k)), regs[k] - 100);
        else
            a_.mov(mem(RSP, std::int32_t(8 * k)), Reg(regs[k]));
    }
}

void Baseline::ssaRestoreRegs(const std::vector<int>& regs)
{
    if (regs.empty())
        return;
    const std::int32_t room = std::int32_t((8 * regs.size() + 15) & ~std::size_t(15));
    for (std::size_t k = 0; k < regs.size(); ++k) {
        if (regs[k] >= 100)
            a_.movsdLoad(regs[k] - 100, mem(RSP, std::int32_t(8 * k)));
        else
            a_.mov(Reg(regs[k]), mem(RSP, std::int32_t(8 * k)));
    }
    a_.aluI(0, RSP, room);
}

void Baseline::ssaSaveRegs()
{
    const int gprs = 4 + Asm::kExtraGpr, xmms = 14 + Asm::kExtraXmm;
    const std::int32_t room = std::int32_t((8 * (gprs + xmms) + 15) & ~15);
    a_.aluI(5, RSP, room);
    for (int k = 0; k < gprs; ++k)
        a_.mov(mem(RSP, 8 * k), Reg(k < 4 ? R8 + k : 16 + (k - 4)));
    for (int k = 0; k < xmms; ++k)
        a_.movsdStore(mem(RSP, 8 * (gprs + k)), 2 + k);
}

void Baseline::ssaRestoreRegs()
{
    const int gprs = 4 + Asm::kExtraGpr, xmms = 14 + Asm::kExtraXmm;
    const std::int32_t room = std::int32_t((8 * (gprs + xmms) + 15) & ~15);
    for (int k = 0; k < gprs; ++k)
        a_.mov(Reg(k < 4 ? R8 + k : 16 + (k - 4)), mem(RSP, 8 * k));
    for (int k = 0; k < xmms; ++k)
        a_.movsdLoad(2 + k, mem(RSP, 8 * (gprs + k)));
    a_.aluI(0, RSP, room);
}

bool Baseline::ssaGetGlobal(std::uint32_t i)
{
    // A data property the global object has now stays where it is until something deletes it or changes its attributes,
    // which the epoch counts. The value read is the global object's: borrowed.
    const Ins& in = code_->code[i];
    if (!ssaGlobalFast(i))
        return false;
    JSObject* g = vm_.global;
    std::uint8_t attrs = 0;
    JSValue* slot = g->findOwn(vm_, code_->atoms[in.b], &attrs);
    if (!slot || (attrs & attr::kAccessor))
        return false;
    std::uint32_t index;
    JSValue* inlineBase = g->inl();
    if (slot >= inlineBase && slot < inlineBase + g->h.aux)
        index = std::uint32_t(slot - inlineBase);
    else
        index = std::uint32_t(slot - g->slots) + g->h.aux;
    Label& bad = ssaExit(i);
    a_.cmpI32(mem(R12, kVmGlobalEpoch), std::int32_t(vm_.globalEpoch));
    a_.jcc(CNE, bad);
    globalSlot(index);
    a_.mov(RAX, mem(RDX, 0));
    ssaDef(i, 0);
    return true;
}

/// `==`, `!=`, `===`, `!==` into a boolean, where they compare bits: against null or undefined (loose), against a constant
/// that is no number and no cell, two int32s, two objects; anything else leaves.
bool Baseline::ssaEquals(std::uint32_t i)
{
    const Ins& in = code_->code[i];
    const Op op = in.opcode();
    const std::uint32_t va = name_[in.b], vb = name_[in.c];
    const bool eq = op == Op::Eq || op == Op::StrictEq;
    const bool loose = op == Op::Eq || op == Op::Ne;
    const bool failed = i < code_->prof.size() && (code_->prof[i] & opt::kProfFailed);
    auto plainConst = [&](std::uint32_t v) {
        if (!sval_[v].isConst)
            return false;
        const JSValue k = JSValue::raw(sval_[v].bits);
        return !k.isNumber() && !k.isCell();
    };
    const bool pa = plainConst(va), pb = plainConst(vb);
    auto finish = [&](Cond c) {
        a_.setcc(c, RAX);
        a_.movzx8(RAX, RAX);
        a_.aluI32(1, RAX, std::int32_t(kFalse));
        ssaDef(i, kVBool | kVNotCell);
    };
    if (loose && (pa || pb)) {
        const JSValue k = JSValue::raw(sval_[pa ? va : vb].bits);
        if (!k.isNullish())
            return false;
        loadValue(pa ? vb : va, RAX);
        a_.aluI(4, RAX, -9);
        a_.cmpI(RAX, std::int32_t(kNull));
        finish(eq ? CE : CNE);
        return true;
    }
    if (pa || pb || (known(va, kVInt) && known(vb, kVInt))) {
        loadValue(va, RAX);
        loadValue(vb, RCX);
        a_.cmp(RAX, RCX);
        finish(eq ? CE : CNE);
        return true;
    }
    if (failed && loose)
        return false;
    Label& bad = ssaExit(i);
    loadValue(va, RAX);
    loadValue(vb, RCX);
    ssaEqualsCore(i, loose, bad);
    a_.test32(RAX, RAX);
    finish(eq ? CNE : CE);
    return true;
}

/// A return: the result in its slot, then the baseline's way out, told which slots may hold something to clear.
void Baseline::ssaRet(std::uint32_t i)
{
    const Ins& in = code_->code[i];
    const unsigned nregs = code_->nregs;
    ssaSync({in.a}, ssa::Bits((nregs + 63) / 64, 0), std::vector<std::uint8_t>(nregs, 0));
    std::vector<std::uint64_t> dirty((nregs + 63) / 64, 0);
    ssaPlain_.assign(nregs, 0);
    for (unsigned r = 0; r < nregs; ++r) {
        const std::uint32_t v = frame_[r];
        const bool undef = v != ssa::kNone && sval_[v].isConst && sval_[v].bits == kUndefined;
        if (!undef)
            dirty[r / 64] |= std::uint64_t(1) << (r % 64);
        if (v != ssa::kNone && !mayBeCell(v) && r != in.a)
            ssaPlain_[r] = 1;
    }
    dirty[in.a / 64] |= std::uint64_t(1) << (in.a % 64);
    ret(i, in, false, &dirty);
    ssaPlain_.clear();
}

/// How element instruction `i` is done in place: 1 on a plain array, 2 on a typed array of `kind`, 0 not.
int Baseline::ssaElemPlan(std::uint32_t i, bool put, ElemKind* kind)
{
    if (ssaOn_ < 2 || (i < code_->prof.size() && (code_->prof[i] & opt::kProfFailed)))
        return 0;
    const std::uint8_t ep = i < code_->elemProf.size() ? code_->elemProf[i] : 0;
    if (ep == elemprof::kArray)
        return 1;
    if (typedSiteKind(i, put, kind)) {
        switch (*kind) {
        case ElemKind::Int8: case ElemKind::Uint8: case ElemKind::Int16: case ElemKind::Uint16: case ElemKind::Int32:
        case ElemKind::Float32: case ElemKind::Float64:
            return 2;
        case ElemKind::Uint8Clamped:
            return put ? 0 : 2;
        default:
            return 0;
        }
    }
    return 0;
}

/// Value `v` in `r` is an object of class `cls` (an array or a typed array), else `bad`.
void Baseline::ssaNeedArray(std::uint32_t v, Reg r, ObjClass cls, Label& bad)
{
    const std::uint8_t fact = cls == ObjClass::Array ? kVArray : kVTyped;
    if (known(v, fact))
        return;
    needObjV(v, r, bad);
    a_.cmpI8(mem(r, kHdrCls), std::uint8_t(cls));
    a_.jcc(CNE, bad);
    ssaKnow(v, std::uint8_t(kVObj | fact));
}

bool Baseline::ssaGetElem(std::uint32_t i)
{
    const Ins& in = code_->code[i];
    ElemKind kind = ElemKind::Int8;
    const int plan = ssaElemPlan(i, false, &kind);
    if (!plan)
        return false;
    const std::uint32_t vb = name_[in.b], vi = name_[in.c];
    if (plan == 1) {
        // Anything but an element the array has: the runtime's, out of line.
        Label& cold = coldLabels_.emplace_back();
        Label& join = coldLabels_.emplace_back();
        const std::vector<std::uint32_t> frameBefore = frame_;
        ssaColdCall(i, &helperThunk<jitGetElem>, curLiveAfter_, cold, join);
        if (ssaFailed_)
            return true;
        const std::vector<std::uint32_t> frameAfter = frame_;
        frame_ = frameBefore;
        // Not an int32 index or not an array: leave (what is known after holds on both ways back to `join`).
        Label& bad = ssaExit(i);
        loadI32(vi, RCX, &bad);
        a_.mov32(RCX, RCX);
        loadValue(vb, RAX);
        ssaNeedArray(vb, RAX, ObjClass::Array, bad);
        a_.alu32(7, RCX, mem(RAX, kArrayLength));  // unsigned: below zero is past the end
        a_.jcc(CAE, cold);
        a_.mov(RDX, mem(RAX, kArrayElems));
        a_.mov(RAX, mem(RDX, RCX, 8));
        a_.cmpI(RAX, std::int32_t(kHole));
        a_.jcc(CE, cold);
        const std::uint32_t def = ssa_->defBase[i];
        if (sval_[def].rep == kRepI32) {
            needInt(RAX, bad);
            ssaDefI32(i);
        } else if (sval_[def].rep == kRepF64) {
            needNumber(RAX, bad);
            loadNumber(0, RAX, RDX);
            ssaDefF64(i, 0);
        } else {
            ssaDef(i, 0);
        }
        a_.bind(join);
        frame_ = frameAfter;
        ssaForgetShapes();
        return true;
    }
    Label& bad = ssaExit(i);
    loadI32(vi, RCX, &bad);
    a_.mov32(RCX, RCX);
    loadValue(vb, RAX);
    ssaNeedArray(vb, RAX, ObjClass::TypedArray, bad);
    a_.cmpI8(mem(RAX, kTypedKind), std::uint8_t(kind));
    a_.jcc(CNE, bad);
    a_.alu32(7, RCX, mem(RAX, kTypedLength));
    a_.jcc(CAE, bad);
    a_.mov(RSI, mem(RAX, kTypedData));
    switch (kind) {
    case ElemKind::Float64:
        a_.movsdLoad(0, mem(RSI, RCX, 8));
        ssaDefF64(i, 0);
        return true;
    case ElemKind::Float32:
        a_.movssLoad(0, mem(RSI, RCX, 4));
        a_.cvtss2sd(0, 0);
        ssaDefF64(i, 0);
        return true;
    case ElemKind::Int32: a_.mov32(RAX, mem(RSI, RCX, 4)); break;
    case ElemKind::Int16: a_.movsx16(RAX, mem(RSI, RCX, 2)); break;
    case ElemKind::Uint16: a_.movzx16(RAX, mem(RSI, RCX, 2)); break;
    case ElemKind::Int8: a_.movsx8(RAX, mem(RSI, RCX, 1)); break;
    default: a_.movzx8(RAX, mem(RSI, RCX, 1)); break;  // Uint8, Uint8Clamped
    }
    ssaDefI32(i);
    return true;
}

bool Baseline::ssaPutElem(std::uint32_t i, const ssa::Bits& liveAfter)
{
    const Ins& in = code_->code[i];
    ElemKind kind = ElemKind::Int8;
    const int plan = ssaElemPlan(i, true, &kind);
    if (!plan)
        return false;
    const std::uint32_t vb = name_[in.a], vi = name_[in.b], vx = name_[in.c];
    if (plan == 2) {
        Label& bad = ssaExit(i);
        const bool flt = kind == ElemKind::Float64 || kind == ElemKind::Float32;
        if (flt)
            loadF64(vx, 0, &bad);
        else
            loadI32(vx, RDI, &bad);
        loadI32(vi, RCX, &bad);
        a_.mov32(RCX, RCX);
        loadValue(vb, RAX);
        ssaNeedArray(vb, RAX, ObjClass::TypedArray, bad);
        a_.cmpI8(mem(RAX, kTypedKind), std::uint8_t(kind));
        a_.jcc(CNE, bad);
        a_.alu32(7, RCX, mem(RAX, kTypedLength));
        a_.jcc(CAE, bad);
        a_.mov(RSI, mem(RAX, kTypedData));
        switch (kind) {
        case ElemKind::Float64: a_.movsdStore(mem(RSI, RCX, 8), 0); break;
        case ElemKind::Float32: a_.cvtsd2ss(0, 0); a_.movssStore(mem(RSI, RCX, 4), 0); break;
        case ElemKind::Int32: a_.mov32(mem(RSI, RCX, 4), RDI); break;
        case ElemKind::Int16: case ElemKind::Uint16: a_.mov16(mem(RSI, RCX, 2), RDI); break;
        default: a_.mov8(mem(RSI, RCX, 1), RDI); break;
        }
        return true;
    }
    // A plain array, an element it has (no append, no hole). A number over a number (the array holds numbers only) lets
    // nothing go; anything else may, so every live value gets an owner first.
    const bool number = known(vx, kVInt) || sval_[vx].rep == kRepF64 ||
                        (sval_[vx].isConst && JSValue::raw(sval_[vx].bits).isNumber());
    if (!number) {
        ssaSync({}, liveAfter, std::vector<std::uint8_t>(code_->nregs, 0));
        if (ssaFailed_)
            return true;
    }
    // Anything but a store over an element the array has (an append, a hole, another object): the runtime's, out of line.
    Label& bad = coldLabels_.emplace_back();
    Label& join = coldLabels_.emplace_back();
    const std::vector<std::uint32_t> frameBefore = frame_;
    ssaColdCall(i, &helperThunk<jitPutElem>, liveAfter, bad, join);
    if (ssaFailed_)
        return true;
    const std::vector<std::uint32_t> frameAfter = frame_;
    frame_ = frameBefore;
    struct Done
    {
        Baseline* self;
        Label* join;
        std::vector<std::uint32_t> after;
        ~Done()
        {
            self->a_.bind(*join);
            self->frame_ = after;
            self->ssaForgetShapes();
        }
    } done{this, &join, frameAfter};
    {
        Label& out = ssaExit(i);
        loadI32(vi, RCX, &out);
        a_.mov32(RCX, RCX);
        loadValue(vb, RAX);
        ssaNeedArray(vb, RAX, ObjClass::Array, out);
    }
    a_.testI8(mem(RAX, kHdrFlags), std::uint8_t(cellflags::kFrozen | cellflags::kNotExtensible));
    a_.jcc(CNE, bad);
    a_.alu32(7, RCX, mem(RAX, kArrayLength));
    a_.jcc(CAE, bad);
    a_.mov(RDX, mem(RAX, kArrayElems));
    a_.lea(RDX, mem(RDX, RCX, 8));
    if (number) {
        // A number over a number: nothing to count or let go of.
        a_.mov(RDI, mem(RDX, 0));
        a_.test(RDI, R14);
        a_.jcc(CE, bad);
    } else {
        a_.cmpI(mem(RDX, 0), std::int32_t(kHole));
        a_.jcc(CE, bad);
    }
    loadValue(vx, RSI);
    if (number) {
        packAt(RAX, RCX);
        a_.mov(mem(RDX, 0), RSI);
        return true;
    }
    retain(RSI);
    Label isNumber, stored;
    a_.test(RSI, R14);
    a_.jcc(CNE, isNumber);
    a_.andI8(mem(RAX, kHdrFlags), std::uint8_t(~cellflags::kAllNumbers));
    unpackAt(RAX, RCX);
    a_.jmp(stored);
    a_.bind(isNumber);
    packAt(RAX, RCX);
    a_.bind(stored);
    a_.mov(RDI, mem(RDX, 0));
    a_.mov(mem(RDX, 0), RSI);
    release(RDI);
    return true;
}

/// Value `v` converted by ToInt32 into the low half of `dst` (a double truncates), else `bad`. Clobbers rdx, xmm0.
void Baseline::loadToInt32(std::uint32_t v, Reg dst, Label& bad)
{
    const SVal& s = sval_[v];
    if (s.isConst) {
        const JSValue k = JSValue::raw(s.bits);
        if (k.isInt32()) {
            a_.movImm(dst, std::uint32_t(k.asInt32()));
        } else if (k.isDouble()) {
            const double d = k.asDouble();
            std::int32_t r = 0;
            if (d == d && d > -9.2e18 && d < 9.2e18)
                r = std::int32_t(std::uint32_t(std::uint64_t(std::int64_t(d))));
            else
                a_.jmp(bad);
            a_.movImm(dst, std::uint32_t(r));
        } else {
            a_.jmp(bad);
        }
        return;
    }
    if (s.rep == kRepF64 || (s.rep == kRepTagged && !known(v, kVInt))) {
        Label done;
        if (s.rep == kRepF64) {
            loadF64(v, 0, nullptr);
        } else {
            Label dbl;
            loadValue(v, dst);
            a_.cmp(dst, R14);
            a_.jcc(CB, dbl);
            a_.jmp(done);
            a_.bind(dbl);
            a_.test(dst, R14);
            a_.jcc(CE, bad);
            a_.mov(RDX, dst);
            a_.sub(RDX, RBP);
            a_.movqToX(0, RDX);
        }
        // The low half of the truncation, for a double that fits 64 bits; else the interpreter's.
        a_.cvttsd2si64(dst, 0);
        a_.movImm(RDX, std::uint64_t(1) << 63);
        a_.cmp(dst, RDX);
        a_.jcc(CE, bad);
        a_.bind(done);
        a_.mov32(dst, dst);
        return;
    }
    loadI32(v, dst, nullptr);
}

/// A relation of `va` and `vb` (or the int32 `imm`): `then(c)` is called with the flags set and `c` the condition that is
/// true when it holds; for int32s, for doubles, or (values whose kind is not known) both, at run time. Not numbers: `bad`.
template <typename Then>
void Baseline::ssaRelation(std::uint32_t i, std::uint32_t va, std::uint32_t vb, std::int32_t imm, bool isImm,
                           Label& bad, Then then)
{
    const Relation rel = relationOf(code_->code[i].opcode());
    auto isDouble = [&](std::uint32_t v) {
        return sval_[v].isConst ? JSValue::raw(sval_[v].bits).isDouble() : sval_[v].rep == kRepF64;
    };
    const bool aI = known(va, kVInt), bI = isImm || known(vb, kVInt);
    auto doubles = [&]() {
        loadF64(va, 0, &bad);
        if (isImm) {
            a_.movImm(RDX, std::uint32_t(imm));
            a_.cvtsi2sd32(1, RDX);
        } else {
            loadF64(vb, 1, &bad);
        }
        if (rel.swap)
            a_.ucomisd(1, 0);
        else
            a_.ucomisd(0, 1);
        then(rel.dc);
    };
    if (aI && bI) {
        loadI32(va, RAX, nullptr);
        if (isImm) {
            a_.aluI32(7, RAX, imm);
        } else {
            loadI32(vb, RCX, nullptr);
            a_.alu32(7, RAX, RCX);
        }
        then(rel.ic);
        return;
    }
    if (isDouble(va) || (!isImm && isDouble(vb))) {
        doubles();
        return;
    }
    // Not known: two int32s compare as such, anything else as doubles.
    const std::vector<std::uint32_t> st = frame_;
    Label dbl;
    if (!isImm && (aI || bI)) {
        // One is an int32 already: only the other is looked at.
        const std::uint32_t other = aI ? vb : va;
        loadValue(other, aI ? RCX : RAX);
        a_.cmp(aI ? RCX : RAX, R14);
        a_.jcc(CB, dbl);
        loadI32(aI ? va : vb, aI ? RAX : RCX, nullptr);
        a_.alu32(7, RAX, RCX);
        then(rel.ic);
        a_.bind(dbl);
        frame_ = st;
        doubles();
        return;
    }
    loadValue(va, RAX);
    if (isImm) {
        a_.cmp(RAX, R14);
        a_.jcc(CB, dbl);
        a_.aluI32(7, RAX, imm);
    } else {
        loadValue(vb, RCX);
        a_.mov(RDX, RAX);
        a_.and_(RDX, RCX);
        a_.cmp(RDX, R14);
        a_.jcc(CB, dbl);
        a_.alu32(7, RAX, RCX);
    }
    then(rel.ic);
    a_.bind(dbl);
    frame_ = st;
    doubles();
}

/// Upvalues and boxes (borrowed reads; a store gives every live value an owner first, as the old value is let go of),
/// `ToNumeric` of a number, `!` of what has a plain truth.
bool Baseline::ssaSmall(std::uint32_t i, const ssa::Bits& liveAfter)
{
    const Ins& in = code_->code[i];
    switch (in.opcode()) {
    case Op::GetUpval:
        a_.mov(RAX, mem(R13, kFrameFn));
        a_.mov(RAX, mem(RAX, kFnUpvals + std::int32_t(8 * in.b)));
        a_.mov(RAX, mem(RAX, kBoxValue));
        ssaDef(i, 0);
        return true;
    case Op::GetBox:
        loadValue(name_[in.b], RAX);
        a_.mov(RAX, mem(RAX, kBoxValue));
        ssaDef(i, 0);
        return true;
    case Op::PutUpval: case Op::PutBox: {
        const bool upval = in.opcode() == Op::PutUpval;
        ssaSync({}, liveAfter, std::vector<std::uint8_t>(code_->nregs, 0));
        if (ssaFailed_)
            return true;
        if (upval) {
            a_.mov(RAX, mem(R13, kFrameFn));
            a_.mov(RAX, mem(RAX, kFnUpvals + std::int32_t(8 * in.a)));
        } else {
            loadValue(name_[in.a], RAX);
        }
        loadValue(name_[in.b], RCX);
        if (mayBeCell(name_[in.b]))
            retain(RCX);
        a_.mov(RSI, mem(RAX, kBoxValue));
        a_.mov(mem(RAX, kBoxValue), RCX);
        release(RSI);
        return true;
    }
    case Op::ToNumeric: {
        const std::uint32_t v = name_[in.b];
        const SVal& sv = sval_[v];
        Label& bad = ssaExit(i);
        loadValue(v, RAX);
        if (!known(v, kVInt) && (sv.isConst || sv.rep == kRepTagged)) {
            a_.cmp(RAX, RBP);
            a_.jcc(CB, bad);
        }
        ssaDef(i, kVNotCell);
        return true;
    }
    case Op::Not: {
        const std::uint32_t v = name_[in.b];
        Label& bad = ssaExit(i);
        loadValue(v, RAX);
        Label t, f, done, notInt;
        if (known(v, kVBool)) {
            a_.aluI32(6, RAX, 1);  // true <-> false
            ssaDef(i, kVBool | kVNotCell);
            return true;
        }
        a_.cmpI(RAX, std::int32_t(kTrue));
        a_.jcc(CE, t);
        a_.cmpI(RAX, std::int32_t(kFalse));
        a_.jcc(CE, f);
        a_.cmp(RAX, R14);
        a_.jcc(CB, notInt);
        a_.test32(RAX, RAX);
        a_.jcc(CNE, t);
        a_.jmp(f);
        a_.bind(notInt);
        a_.mov(RDX, RAX);
        a_.aluI(4, RDX, -9);
        a_.cmpI(RDX, std::int32_t(kNull));
        a_.jcc(CE, f);
        needObject(RAX, bad);
        a_.bind(t);
        a_.movImm(RAX, kFalse);
        a_.jmp(done);
        a_.bind(f);
        a_.movImm(RAX, kTrue);
        a_.bind(done);
        ssaDef(i, kVBool | kVNotCell);
        return true;
    }
    default:
        return false;
    }
}

}  // namespace

extern "C" std::uint64_t scriptVmSsaStrictEquals(std::uint64_t a, std::uint64_t b)
{
    return strictEquals(JSValue::raw(a), JSValue::raw(b)) ? 1 : 0;
}

/// 1 or 0 for `a == b` (neither an object), 2 with an exception pending.
extern "C" std::uint64_t scriptVmSsaLooseEquals(VM* vm, std::uint64_t a, std::uint64_t b)
{
    try {
        bool ok = true;
        const bool r = looseEquals(*vm, JSValue::raw(a), JSValue::raw(b), &ok);
        return !ok ? 2 : r ? 1 : 0;
    } catch (...) {
        pendingException = std::current_exception();
        return 2;
    }
}

namespace {

/// rax and rcx (boxed): rax becomes 1 when they are equal (`==` when `loose`, else `===`), else 0. Bits, int32s and
/// objects are decided here; the rest by the runtime, the registers kept (it runs no script: a loose compare with an
/// object leaves through `bad` first).
void Baseline::ssaEqualsCore(std::uint32_t i, bool loose, Label& bad)
{
    Label eq, ne, slow, differ, notCellA, done;
    a_.cmp(RAX, RCX);
    a_.jcc(CNE, differ);
    // The same bits: equal, unless a double (a NaN is not itself).
    a_.cmp(RAX, RBP);
    a_.jcc(CB, eq);
    a_.cmp(RAX, R14);
    a_.jcc(CAE, eq);
    a_.jmp(slow);
    a_.bind(differ);
    a_.mov(RDX, RAX);
    a_.and_(RDX, RCX);
    a_.cmp(RDX, R14);
    a_.jcc(CAE, ne);  // two int32s
    a_.test(RAX, R15);
    a_.jcc(CNE, notCellA);
    // A is a cell.
    if (loose) {
        a_.cmpI8(mem(RAX, kHdrKind), kObjectKind);
        a_.jcc(CE, bad);
        Label bCell;
        a_.test(RCX, R15);
        a_.jcc(CE, bCell);
        a_.jmp(slow);
        a_.bind(bCell);
        a_.cmpI8(mem(RCX, kHdrKind), kObjectKind);
        a_.jcc(CE, bad);
        a_.jmp(slow);
    } else {
        a_.cmpI8(mem(RAX, kHdrKind), kObjectKind);
        a_.jcc(CE, ne);
        a_.test(RCX, R15);
        a_.jcc(CNE, ne);
        a_.cmpI8(mem(RCX, kHdrKind), kObjectKind);
        a_.jcc(CE, ne);
        a_.jmp(slow);  // two strings, two bigints
    }
    a_.bind(notCellA);
    if (loose) {
        Label bCell;
        a_.test(RCX, R15);
        a_.jcc(CNE, slow);
        a_.cmpI8(mem(RCX, kHdrKind), kObjectKind);
        a_.jcc(CE, bad);
        a_.jmp(slow);
    } else {
        // A is no cell: equal only to a number of the same value (an int32 and a double).
        a_.test(RAX, R14);
        a_.jcc(CE, ne);
        a_.test(RCX, R14);
        a_.jcc(CE, ne);
        a_.jmp(slow);
    }
    a_.bind(slow);
    if (!loose) {
        // Two strings that are both in the string table are equal only as the same cell; of other lengths, not at all.
        Label call;
        a_.test(RAX, R15);
        a_.jcc(CNE, call);
        a_.test(RCX, R15);
        a_.jcc(CNE, call);
        a_.cmpI8(mem(RAX, kGcKind), std::uint8_t(detail::CellKind::String));
        a_.jcc(CNE, call);
        a_.cmpI8(mem(RCX, kGcKind), std::uint8_t(detail::CellKind::String));
        a_.jcc(CNE, call);
        a_.mov32(RDX, mem(RAX, kStrLength));
        a_.alu32(7, RDX, mem(RCX, kStrLength));
        a_.jcc(CNE, ne);
        a_.movzx8(RDX, mem(RAX, kStrInterned));
        a_.movzx8(RSI, mem(RCX, kStrInterned));
        a_.and_(RDX, RSI);
        a_.test32(RDX, RDX);
        a_.jcc(CNE, ne);
        a_.bind(call);
    }
    a_.mov(RSI, RAX);
    a_.mov(RDX, RCX);
    const std::vector<int> keep = ssaLiveRegs();
    ssaSaveRegs(keep);
    if (loose) {
        a_.mov(RDI, R12);
        a_.movImm(RAX, reinterpret_cast<std::uint64_t>(&scriptVmSsaLooseEquals));
    } else {
        a_.mov(RDI, RSI);
        a_.mov(RSI, RDX);
        a_.movImm(RAX, reinterpret_cast<std::uint64_t>(&scriptVmSsaStrictEquals));
    }
    a_.callC(RAX);
    ssaRestoreRegs(keep);
    if (loose) {
        a_.cmpI(RAX, 2);
        a_.jcc(CE, exception(i));
    }
    a_.jmp(done);
    a_.bind(eq);
    a_.movImm(RAX, 1);
    a_.jmp(done);
    a_.bind(ne);
    a_.movImm(RAX, 0);
    a_.bind(done);
}

/// `typeof v`: one of the names the VM keeps (for its life, so the value is held by nothing here); a bigint or a symbol
/// leaves.
bool Baseline::ssaTypeOf(std::uint32_t i)
{
    const Ins& in = code_->code[i];
    std::uint64_t names[8];
    for (unsigned k = 0; k < 8; ++k) {
        const JSValue n = vm_.typeofName(k);
        names[k] = n.bits;
        vm::release(n);
    }
    Label& bad = ssaExit(i);
    Label number, undef, object, boolean, string, function, cell, done, notObject;
    loadValue(name_[in.b], RAX);
    a_.cmp(RAX, RBP);
    a_.jcc(CAE, number);
    a_.test(RAX, R15);
    a_.jcc(CE, cell);
    a_.cmpI(RAX, std::int32_t(kNull));
    a_.jcc(CE, object);
    a_.mov(RDX, RAX);
    a_.aluI(4, RDX, -2);
    a_.cmpI(RDX, std::int32_t(kFalse));
    a_.jcc(CE, boolean);
    a_.jmp(undef);
    a_.bind(cell);
    a_.cmpI8(mem(RAX, kHdrKind), kObjectKind);
    a_.jcc(CNE, notObject);
    a_.cmpI8(mem(RAX, kHdrCls), std::uint8_t(ObjClass::Function));
    a_.jcc(CE, function);
    a_.jmp(object);
    a_.bind(notObject);
    a_.testI8(mem(RAX, 7), 0x80);
    a_.jcc(CNE, bad);
    a_.cmpI8(mem(RAX, kGcKind), std::uint8_t(detail::CellKind::String));
    a_.jcc(CNE, bad);
    a_.bind(string);
    a_.movImm(RAX, names[4]);
    a_.jmp(done);
    const std::pair<Label*, unsigned> cases[] = {{&undef, 0}, {&object, 1}, {&boolean, 2}, {&number, 3}, {&function, 7}};
    for (const auto& [l, k] : cases) {
        a_.bind(*l);
        a_.movImm(RAX, names[k]);
        a_.jmp(done);
    }
    a_.bind(done);
    ssaDef(i, 0);
    return true;
}
