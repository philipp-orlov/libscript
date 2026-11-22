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
    if (def_) {
        // A deferred frame: the slots hold their values without counts.
        for (unsigned r : todo) {
            loadValue(name_[r], RAX);
            a_.mov(R(r), RAX);
            frame_[r] = name_[r];
        }
        return;
    }
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

/// What a handler over instruction `i` finds live is in its slots before the instruction can throw.
void Baseline::ssaAddHeld(std::uint32_t i, std::vector<unsigned>& regs) const
{
    for (std::uint16_t r : ssa_->held(i))
        if (std::find(regs.begin(), regs.end(), unsigned(r)) == regs.end())
            regs.push_back(r);
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
    std::vector<unsigned> reads(e.rd.begin(), e.rd.end());
    ssaAddHeld(i, reads);
    const std::vector<unsigned> set = ssaSyncSet(std::move(reads), keep, clobber);
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
#if defined(SCRIPT_JIT_ENV_OVERRIDE)
    if (std::getenv("SSA_GENERIC")) {
        std::string ic;
        if (i + 1 < n_ && code_->code[i + 1].opcode() == Op::Cache && code_->code[i + 1].a != kNoCache) {
            const InlineCache& c = code_->caches[code_->code[i + 1].a];
            char buf[96];
            std::snprintf(buf, sizeof buf, " ic shape %u next %u holder %d valid %d pad %u", c.shape, c.next, c.holder != nullptr,
                          c.validity ? int(c.validity->valid) : -1, unsigned(c.pad));
            ic = buf;
            if (in.opcode() == Op::GetProp && in.c < code_->atoms.size() && code_->atoms[in.c] &&
                isString(JSValue::cell(code_->atoms[in.c])))
                ic += " ." + toUtf8(static_cast<StringCell*>(code_->atoms[in.c])->text());
        }
        std::fprintf(stderr, "ssa generic %s @%u %s prof %d%s\n",
                     code_->name ? toUtf8(static_cast<StringCell*>(code_->name)->text()).c_str() : "anon", i,
                     opName(in.opcode()), i < code_->prof.size() ? int(code_->prof[i]) : -1, ic.c_str());
    }
#endif
    const opt::Effects e = ssa::effects(*code_, i);
    const unsigned nregs = code_->nregs;
    std::vector<std::uint8_t> clobber(nregs, 0);
    for (std::uint16_t r : e.wr)
        clobber[r] = 1;
    const Op callOp = in.opcode();
    const bool window = callOp == Op::Call || callOp == Op::New || callOp == Op::SuperCall || callOp == Op::CallSpread ||
                        callOp == Op::NewSpread || callOp == Op::SuperSpread;
    if (window)
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
    ssaAddHeld(i, reads);
    ssaSync(reads, keep, clobber);
    if (ssaFailed_)
        return;
    // A call covers the registers above its operands with the callee's frame: a slot known to hold no cell is
    // overwritten without a look (`windowPlain`). A register above the parameters that still holds its entry value holds
    // undefined.
    ssaPlainWin_.assign(nregs, 0);
    if (in.opcode() == Op::Call || in.opcode() == Op::New || in.opcode() == Op::SuperCall)
        for (unsigned r = in.b + 1u; r < nregs; ++r) {
            const std::uint32_t v = frame_[r];
            if (v == ssa::kNone)
                continue;
            const SVal& s = sval_[v];
            const bool plain = s.isConst ? !JSValue::raw(s.bits).isCell()
                                         : s.rep != kRepTagged || (vKnown_[v] & kVNotCell) ||
                                               (v == r && ssa_->values[v].kind == ssa::Value::Entry && r >= firstLocal());
            ssaPlainWin_[r] = plain || neverCell_[r];
        }
#if defined(SCRIPT_JIT_ENV_OVERRIDE)
    if (std::getenv("WINDBG") && (in.opcode() == Op::Call || in.opcode() == Op::New)) {
        std::fprintf(stderr, "win %s @%u b%u argc%u nregs%u:", code_->name ? toUtf8(static_cast<StringCell*>(code_->name)->text()).c_str() : "anon", i, unsigned(in.b), unsigned(in.c), nregs);
        for (unsigned r = in.b + 2u + in.c; r < nregs; ++r)
            std::fprintf(stderr, " r%u:%s", r, frame_[r] == ssa::kNone ? "?" : ssaPlainWin_[r] ? "p" : "c");
        std::fprintf(stderr, "\n");
    }
#endif
    if (CodeBlock* C = ssaDirectCallee(i)) {
        Label slow;
        ssaDirectCall(i, C, slow);
        a_.jmp(at_[i + length(i)]);
        a_.bind(slow);
    }
    emit(i);
    ssaPlainWin_.clear();
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
/// On a way into loop header `to` from before the loop: its checks (`loopGuards_`). One that fails leaves at the head,
/// where the frame is what it is here.
void Baseline::ssaLoopGuards(std::uint32_t from, std::uint32_t to)
{
    const LoopGuards& lg = loopGuards_[to];
    if (lg.ints.empty() && lg.arrays.empty() && lg.typed.empty() && lg.intParams.empty() && lg.globals.empty())
        return;
    const ssa::Block& tb = ssa_->blocks[to];
    const ssa::Bits live = liveBefore_;
    liveBefore_ = tb.liveIn;
    exitAt_ = -1;
    Label& bad = ssaExit(tb.start);
    exitAt_ = -1;
    liveBefore_ = live;
    for (std::uint32_t p : lg.ints)
        for (std::size_t k = 0; k < tb.preds.size(); ++k) {
            const std::uint32_t v = ssa_->values[p].in[k];
            if (tb.preds[k] != from || v == p || known(v, kVInt))
                continue;
            if (sval_[v].rep == kRepI32)
                continue;
            loadValue(v, RAX);
            needInt(RAX, bad);
            ssaKnow(v, kVInt | kVNotCell);
        }
    for (std::uint32_t i : lg.globals) {
        ssaGlobalRead(i, bad);
        storeTagged(ssa_->defBase[i], RAX);
    }
    for (std::uint32_t v : lg.intParams)
        if (!known(v, kVInt)) {
            loadValue(v, RAX);
            needInt(RAX, bad);
            ssaKnow(v, kVInt | kVNotCell);
        }
    for (std::uint32_t v : lg.arrays)
        if (!known(v, kVArray)) {
            loadValue(v, RAX);
            ssaNeedArray(v, RAX, ObjClass::Array, bad);
        }
    for (std::uint32_t v : lg.typed)
        if (!known(v, kVTyped)) {
            loadValue(v, RAX);
            ssaNeedArray(v, RAX, ObjClass::TypedArray, bad);
        }
}

/// Whether the code of block `to` comes right after that of `from`.
bool Baseline::ssaNextBlock(std::uint32_t from, std::uint32_t to) const
{
    for (std::uint32_t b = from + 1; b < to; ++b)
        if (ssa_->blocks[b].reached)
            return false;
    return to > from;
}

/// Whether the way from `from` to `to` is a jump and nothing else.
bool Baseline::ssaPlainEdge(std::uint32_t from, std::uint32_t to) const
{
    const ssa::Block& tb = ssa_->blocks[to];
    if (to <= from || tb.loopHeader || !loopCache_[to].empty())
        return false;
    std::size_t k = 0;
    while (k < tb.preds.size() && tb.preds[k] != from)
        ++k;
    for (std::uint32_t p : tb.phis) {
        const std::uint32_t src = ssa_->values[p].in[k];
        if (src == p || !hasLoc(p))
            continue;
        const SVal& a = sval_[src];
        const SVal& d = sval_[p];
        if (a.isConst || a.rep != d.rep || a.reg != d.reg || a.slot != d.slot)
            return false;
    }
    return true;
}

/// Such a way, its jump made by the caller: the block it leads to learns what holds here.
void Baseline::ssaTakePlainEdge(std::uint32_t to)
{
    edgeIn_[to].push_back(EdgeState{frame_, ssaFacts()});
}

/// `last`: the last code of block `from`, which may fall into the next one.
void Baseline::ssaEdge(std::uint32_t from, std::uint32_t to, bool last)
{
    const ssa::Block& tb = ssa_->blocks[to];
    const unsigned nregs = code_->nregs;
    if (to > from) {
        // What the loop's checks prove holds on this edge only: code emitted after it may be on another way.
        const std::vector<std::uint8_t> known = tb.loopHeader ? vKnown_ : std::vector<std::uint8_t>{};
        if (tb.loopHeader)
            ssaLoopGuards(from, to);
        ssaEdgeMoves(from, to);
        ssaLoadLoopCache(to);
        edgeIn_[to].push_back(EdgeState{frame_, ssaFacts()});
        if (tb.loopHeader)
            vKnown_ = known;
        if (!last || !ssaNextBlock(from, to))
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
    ssaAddHeld(ssa_->blocks[from].last, all);
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
    ssaAddHeld(i, reads);
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

Tier2Code* Baseline::compileSsa(ssa::Graph& g, Tier1Code* base)
{
    ssa_ = &g;
    base_ = base;
    ssaCtor_ = ssa::constructs(*code_) && vm_.deferred;
    if (n_ <= kPoolCode)
        a_.usePool();
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

    auto initValues = [&]() {
        // ---- what each value is: a constant, or a slot ----
        sval_.assign(g.values.size(), SVal{});
        for (unsigned r = firstLocal(); r < nregs; ++r) {
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
            case Op::GetGlobal: k = ssaGlobalConst(sv.at, &bits); break;
            case Op::GetProp: k = ssaMethodConst(sv.at, &bits); break;
            case Op::GetUpval: k = ssaUpvalConst(sv.at, &bits); break;
            case Op::GetBox: k = ssaUpvalConst(sv.at, &bits); break;
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
                           : !k.isCell()  ? kVNotCell
                           : !isObject(k) ? std::uint8_t(0)
                           : asObject(k)->cls() == ObjClass::Array      ? std::uint8_t(kVObj | kVArray)
                           : asObject(k)->cls() == ObjClass::TypedArray ? std::uint8_t(kVObj | kVTyped)
                                                                        : std::uint8_t(kVObj);
            }
        vKnown_ = vDef_;
        vShape_.assign(g.values.size(), 0);
        touched_.clear();
    };
    std::vector<std::vector<ssa::Bits>> liveAfter(nb);
    for (std::uint32_t b = 1; b < nb; ++b)
        if (g.blocks[b].reached)
            liveAfter[b] = g.liveAfter(*code_, b);
    elim_.assign(n_, 0);
    initValues();
    if (!ssaPlan(liveAfter))
        return fail();
    if (ssaOn_ >= 2) {
        const std::vector<std::uint8_t> before = native_;
        if (ssaLoadElim(g)) {
            // Planned again with the reads gone. What was done in place and is not now could change what they read: without.
            initValues();
            bool ok = ssaPlan(liveAfter);
            for (std::uint32_t i = 0; i < n_ && ok; ++i)
                ok = !before[i] || native_[i];
            if (!ok) {
                g = ssa::build(*code_);
                elim_.assign(n_, 0);
                initValues();
                if (!ssaPlan(liveAfter))
                    return fail();
            }
        }
    }

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
    a_.enter(RBX, R13, 8 + spill_);
    a_.mov(R13, RSI);
    a_.mov(RBX, RDI);
    for (unsigned r = 0; r < nregs; ++r)
        if (ssa::bitTest(g.blocks[1].liveIn, r) && hasLoc(r)) {
            if (sval_[r].reg >= 0) {
                a_.mov(Reg(sval_[r].reg), R(r));
                continue;
            }
            a_.mov(RAX, R(r));
            storeTagged(r, RAX);
        }
    {
        std::vector<std::uint32_t> st(nregs);
        for (unsigned r = 0; r < nregs; ++r)
            st[r] = r;  // the slots hold the entry values
        name_ = st;
        if (def_)
            for (unsigned r = firstLocal(); r < nregs; ++r)
                st[r] = ssa::kNone;  // anything
        frame_ = st;
        liveBefore_ = g.blocks[1].liveIn;
        ssaResetFacts();
        if (g.blocks[1].loopHeader)
            ssaLoopGuards(0, 1);
        edgeIn_[1].push_back(EdgeState{st, ssaFacts()});
        ssaEdgeMoves(0, 1);
        ssaLoadLoopCache(1);
    }
    for (std::uint32_t b = 1; b < nb && !ssaFailed_; ++b) {
        const ssa::Block& bl = g.blocks[b];
        if (!bl.reached)
            continue;
        // (The place of an epoch check is not this block's: other ways lead here.)
        if (!epochSites_.empty() && epochSites_.back().first == a_.here())
            a_.nop();
        a_.bind(blockLabel_[b]);
        if (!loopCache_[b].empty()) {
            // From here to the loop's last block, these values are in their loop registers.
            for (const auto& [v, r] : loopCache_[b]) {
                cacheSaved_.emplace_back(v, sval_[v].reg);
                sval_[v].reg = std::int8_t(r);
            }
            cacheEnd_ = loopLast_[b];
        }
        epochSeen_ = false;
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
            curIns_ = i;
            exitAt_ = -1;
            curLiveAfter_ = liveAfter[b][k];
            {
                const opt::Effects e = ssa::effects(*code_, i);
                liveBefore_ = liveAfter[b][k];
                for (std::uint16_t r : e.wr)
                    ssa::bitClear(liveBefore_, r);
                for (std::uint16_t r : e.rd)
                    ssa::bitSet(liveBefore_, r);
                for (std::uint16_t r : g.held(i))
                    ssa::bitSet(liveBefore_, r);
            }
            if (opt::jumpTarget(*code_, i) >= 0) {
                if (op == Op::Jmp)
                    ssaEdge(b, bl.succs[0], true);
                else if (!native_[i])
                    ssaStepJump(b, i);
                else if (!ssaBranch(b, i))
                    a_.jmp(ssaExit(i));
                ended = true;
                break;
            }
            if (elim_[i]) {
                name_[in.a] = g.defBase[i];
                continue;
            }
            switch (op) {
            case Op::GetProp:
                if (!native_[i]) {
                    const std::uint16_t c = i + 1 < n_ && code_->code[i + 1].opcode() == Op::Cache ? code_->code[i + 1].a
                                                                                                   : kNoCache;
                    if (ssaOn_ >= 2 && c != kNoCache && code_->caches[c].shape == 0 &&
                        !(i < code_->prof.size() && (code_->prof[i] & opt::kProfFailed)))
                        ssaImmature(i);
                    ssaGeneric(i, liveAfter[b][k]);
                } else if (ssaLengthSite(i)) {
                    ssaLength(i);
                } else if (const std::int32_t at = ssaStringProtoIndex(i); at >= 0) {
                    ssaStringProtoRead(i, std::uint32_t(at));
                } else if (!ssaGetProp(i))
                    a_.jmp(ssaExit(i));
                break;
            case Op::PutProp: case Op::InitProp:
                if (!native_[i])
                    ssaGeneric(i, liveAfter[b][k]);
                else if (ssaAddFast(i))
                    ssaPutAdd(i);
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
                if ((op == Op::GetUpval || op == Op::GetBox) && sval_[g.defBase[i]].isConst) {
                    ssaEpochCheck(i);
                    name_[in.a] = g.defBase[i];
                } else if (!native_[i])
                    ssaGeneric(i, liveAfter[b][k]);
                else if (!ssaSmall(i, liveAfter[b][k]))
                    a_.jmp(ssaExit(i));
                break;
            case Op::GetElem:
                if (!native_[i])
                    ssaGeneric(i, liveAfter[b][k]);
                else if (ssaPrivateSite(i))
                    ssaPrivateRead(i);
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
            case Op::Call:
                if (!native_[i]) {
                    ssaGeneric(i, liveAfter[b][k]);
                } else if (JSFunction* f = ssaMathCallee(i)) {
                    ssaMathCall(i, f);
                } else if (JSFunction* f = ssaCharCodeCallee(i)) {
                    ssaCharCode(i, f);
                } else {
                    ssaGeneric(i, liveAfter[b][k]);
                }
                break;
            case Op::CheckInit:
                if (!native_[i]) {
                    ssaGeneric(i, liveAfter[b][k]);
                } else {
                    // A binding read before its declaration ran: the interpreter throws.
                    const std::uint32_t v = name_[in.a];
                    if (sval_[v].isConst ? JSValue::raw(sval_[v].bits).bits == kUninit
                                         : sval_[v].rep == kRepTagged && !(vKnown_[v] & (kVInt | kVBool | kVObj))) {
                        Label& bad = ssaExit(i);
                        if (sval_[v].isConst) {
                            a_.jmp(bad);
                        } else {
                            loadValue(v, RAX);
                            a_.cmpI(RAX, std::int32_t(kUninit));
                            a_.jcc(CE, bad);
                        }
                    }
                }
                break;
            case Op::New:
                if (!native_[i])
                    ssaGeneric(i, liveAfter[b][k]);
                else if (!ssaNew(i, liveAfter[b][k]))
                    a_.jmp(ssaExit(i));
                break;
            case Op::AddI: case Op::SubI: case Op::Inc: case Op::Dec: case Op::BitAndI: case Op::BitOrI: case Op::BitXorI:
            case Op::ShlI: case Op::ShrI: case Op::Add: case Op::Sub: case Op::Mul: case Op::BitAnd: case Op::BitOr:
            case Op::BitXor: case Op::Lt: case Op::Le: case Op::Gt: case Op::Ge: case Op::Div: case Op::MulI: case Op::ModI:
            case Op::Mod: case Op::Neg: case Op::BitNot: case Op::Shl: case Op::Shr: case Op::UShr: case Op::UShrI:
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
            ssaEdge(b, bl.succs[0], true);
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
        // A slot the code takes to hold no cell, dead here: what the baseline code left there goes.
        for (unsigned r = 0; r < nregs; ++r)
            if (neverCell_[r] && !ssa::bitTest(bl.liveIn, r)) {
                if (def_) {
                    a_.mov(R(r), std::int32_t(kUndefined));
                    continue;
                }
                a_.mov(RDX, R(r));
                a_.mov(R(r), std::int32_t(kUndefined));
                release(RDX);
            }
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
                } else if (bits & (kVObj | kVArray | kVTyped)) {
                    needObject(RAX, fail);
                    if (bits & (kVArray | kVTyped)) {
                        a_.cmpI8(mem(RAX, kHdrCls), std::uint8_t(bits & kVArray ? ObjClass::Array : ObjClass::TypedArray));
                        a_.jcc(CNE, fail);
                    }
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
        // The globals read before the loops this head is in, and what is checked of them.
        for (std::uint32_t h = 1; h <= b; ++h) {
            if (loopGuards_[h].globals.empty())
                continue;
            std::uint32_t lastB = h;
            for (std::uint32_t q = h + 1; q < nb && g.blocks[q].start < g.blocks[h].loopEnd; ++q)
                if (g.blocks[q].reached)
                    lastB = q;
            if (b > lastB)
                continue;
            for (std::uint32_t gi : loopGuards_[h].globals) {
                const std::uint32_t v = ssa_->defBase[gi];
                ssaGlobalRead(gi, fail);
                storeTagged(v, RAX);
                if (std::find(loopGuards_[h].arrays.begin(), loopGuards_[h].arrays.end(), v) != loopGuards_[h].arrays.end()) {
                    needObject(RAX, fail);
                    a_.cmpI8(mem(RAX, kHdrCls), std::uint8_t(ObjClass::Array));
                    a_.jcc(CNE, fail);
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
    a_.finishPool();
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
    t->ctor = ssaCtor_;
    for (const auto& [at, bad] : epochSites_)
        t->epochSites.emplace_back(std::uint32_t(at), std::uint32_t(bad->pos));
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

bool Baseline::ssaGlobalConst(std::uint32_t i, std::uint64_t* bits)
{
    if (ssaOn_ < 2 || (i < code_->prof.size() && (code_->prof[i] & opt::kProfFailed)))
        return false;
    std::uint8_t attrs = 0;
    const JSValue* slot = vm_.global->findOwn(vm_, code_->atoms[code_->code[i].b], &attrs);
    if (!slot || (attrs & attr::kAccessor) || slot->isUndefined() || slot->isHole() || slot->isUninit() ||
        !vm_.watchGlobal(vm_.globalSlotIndex(slot)))
        return false;
    if (slot->isCell())
        holdValue(*slot);
    *bits = slot->bits;
    return true;
}

bool Baseline::ssaMethodConst(std::uint32_t i, std::uint64_t* bits)
{
    if (ssaOn_ < 2)
        return false;
    const std::vector<ReadEntry> es = ssaReadEntries(i);
    JSValue f = JSValue::undefined();
    for (const ReadEntry& e : es) {
        const Shape* s = liveShape(e.shape);
        if (!e.holder || e.absent || !s || !s->proto)
            return false;
        const JSValue v = *e.holder->slotAt(e.index);
        if (!isFunction(v) || (!f.isUndefined() && v != f))
            return false;
        f = v;
    }
    if (f.isUndefined())
        return false;
    for (const ReadEntry& e : es)
        if (!vm_.watchMethod(liveShape(e.shape)->proto, e.holder))
            return false;
    // (Watching moves a holder to another shape, not a property of it to another slot.)
    holdValue(f);
    *bits = f.bits;
    return true;
}

bool Baseline::ssaUpvalConst(std::uint32_t i, std::uint64_t* bits)
{
    const Ins& in = code_->code[i];
    if (ssaOn_ < 2 || (i < code_->prof.size() && (code_->prof[i] & opt::kProfFailed)))
        return false;
    Box* box;
    if (in.opcode() == Op::GetBox) {
        box = namedBox(i);
        if (!box)
            return false;
    } else {
        JSFunction* f = code_->only;
        if (code_->made != 1 || !f || f->code != code_ || in.b >= f->nUpvals)
            return false;
        box = f->upvals()[in.b];
    }
    const JSValue v = box->value;
    if ((box->h.aux & Box::kNotConstant) || (box->h.aux & Box::kWrites) == Box::kWrites || v.isUndefined() || v.isHole() || v.isUninit())
        return false;
    box->h.aux |= Box::kWatched;
    if (in.opcode() == Op::GetUpval)
        code_->singleAssumed = true;
    if (v.isCell())
        holdValue(v);
    *bits = v.bits;
    return true;
}

void Baseline::ssaEpochCheck(std::uint32_t i)
{
    if (epochSeen_)
        return;
    ssaEpochSite(ssaExit(i));
    epochSeen_ = true;
}

/// Nothing where code can be patched: once the epoch moves, a jump to `bad` is written over the instruction that comes next.
/// (Whatever way leads there has come past another such place since, or is as it is for `bad` too.)
void Baseline::ssaEpochSite(Label& bad)
{
    if (Asm::kPatches) {
        epochSites_.emplace_back(a_.here(), &bad);
        return;
    }
    a_.cmpI32(mem(R12, kVmGlobalEpoch), std::int32_t(vm_.globalEpoch));
    a_.jcc(CNE, bad);
}

JSTypedArray* Baseline::ssaKnownTyped(std::uint32_t v, ElemKind kind) const
{
    if (!sval_[v].isConst)
        return nullptr;
    const JSValue k = JSValue::raw(sval_[v].bits);
    if (!isObject(k) || asObject(k)->cls() != ObjClass::TypedArray)
        return nullptr;
    auto* t = static_cast<JSTypedArray*>(asObject(k));
    if (t->kind != kind || !t->data || !t->length || t->tracking || !isObject(t->buffer))
        return nullptr;
    const Rare* r = asObject(t->buffer)->rare();
    return r && r->ext && !static_cast<const BufferState*>(r->ext)->resizable ? t : nullptr;
}

bool Baseline::ssaLoadElim(ssa::Graph& g)
{
#if defined(SCRIPT_JIT_ENV_OVERRIDE)
    if (std::getenv("SSA_NOELIM"))
        return false;
    static int left = std::getenv("SSA_ELIM") ? std::atoi(std::getenv("SSA_ELIM")) : 0x7fffffff;
    if (left-- <= 0)
        return false;
#endif
    const std::uint32_t nb = std::uint32_t(g.blocks.size());
    // What is known to be in a slot: `obj` the value of the object (or a box, or none for an upvalue), `shape` the shape it
    // was found to have (1: a box's value, 0: an upvalue).
    struct Known
    {
        std::uint32_t obj, shape, index, value;
    };
    std::vector<std::vector<Known>> out(nb);
    std::vector<std::uint8_t> done(nb, 0);
    std::vector<std::uint32_t> alias(g.values.size());
    for (std::uint32_t v = 0; v < alias.size(); ++v)
        alias[v] = v;
    auto real = [&](std::uint32_t v) {
        while (v != ssa::kNone && alias[v] != v)
            v = alias[v];
        return v;
    };
    bool any = false;
    for (std::uint32_t b = 1; b < nb; ++b) {
        const ssa::Block& bl = g.blocks[b];
        if (!bl.reached)
            continue;
        std::vector<Known> k;
        bool first = true;
        if (!bl.loopHeader)
            for (std::uint32_t p : bl.preds) {
                if (p >= b || !done[p]) {
                    k.clear();
                    break;
                }
                if (first) {
                    k = out[p];
                } else {
                    for (std::size_t x = 0; x < k.size();) {
                        bool same = false;
                        for (const Known& o : out[p])
                            same = same || (o.obj == k[x].obj && o.shape == k[x].shape && o.index == k[x].index && o.value == k[x].value);
                        if (same) {
                            ++x;
                        } else {
                            k[x] = k.back();
                            k.pop_back();
                        }
                    }
                }
                first = false;
            }
        std::vector<std::uint32_t> names = bl.nameIn;
        for (std::uint32_t& v : names)
            v = real(v);
        auto find = [&](std::uint32_t obj, std::uint32_t shape, std::uint32_t index) {
            for (const Known& x : k)
                if (x.obj == obj && x.shape == shape && x.index == index)
                    return x.value;
            return ssa::kNone;
        };
        auto forget = [&](std::uint32_t shape, std::uint32_t index) {
            for (std::size_t x = 0; x < k.size();) {
                if (k[x].shape == shape && (shape <= 1 || k[x].index == index)) {
                    k[x] = k.back();
                    k.pop_back();
                } else {
                    ++x;
                }
            }
        };
        for (std::uint32_t i = bl.start; i < bl.end; i = opt::nextOf(*code_, i)) {
            const Ins& in = code_->code[i];
            const Op op = in.opcode();
            const opt::Effects e = ssa::effects(*code_, i);
            ElemKind kind;
            // Whatever could run other code, or write what is not known here: all of it is forgotten. (An element of a plain
            // array may be the runtime's to find.)
            const bool quiet = native_[i] && !((op == Op::GetElem || op == Op::PutElem) && !(op == Op::GetElem && ssaPrivateSite(i)) &&
                                               ssaElemPlan(i, op == Op::PutElem, &kind) == 1);
            // A value kept past the register it was read into has no owner of its own: an allocation may scan the frames, and a
            // store of a counted frame may free what held it.
            if (!quiet || op == Op::New || (!def_ && (op == Op::PutProp || op == Op::PutUpval || op == Op::PutBox || op == Op::PutElem)))
                k.clear();
            std::uint32_t found = ssa::kNone, obj = ssa::kNone, shape = 0, index = 0;
            bool read = false;
            if (native_[i] && op == Op::GetProp && !sval_[g.defBase[i]].isConst) {
                const std::vector<ReadEntry> es = ssaReadEntries(i);
                if (es.size() == 1 && !es[0].holder && !es[0].absent) {
                    read = true;
                    obj = names[in.b];
                    shape = es[0].shape;
                    index = es[0].index;
                }
            } else if (native_[i] && op == Op::GetUpval && !sval_[g.defBase[i]].isConst) {
                read = true;
                index = in.b;
            } else if (native_[i] && op == Op::GetBox && !sval_[g.defBase[i]].isConst) {
                read = true;
                obj = names[in.b];
                shape = 1;
            }
            if (read)
                found = find(obj, shape, index);
            if (found != ssa::kNone) {
#if defined(SCRIPT_JIT_ENV_OVERRIDE)
                if (std::getenv("SSA_ELIMDBG"))
                    std::fprintf(stderr, "elim %s @%u %s v%u := v%u\n", code_->name ? toUtf8(static_cast<StringCell*>(code_->name)->text()).c_str() : "anon", i, opName(op), g.defBase[i], found);
#endif
                alias[g.defBase[i]] = found;
                g.defBase[i] = found;
                elim_[i] = 1;
                any = true;
                names[in.a] = found;
                continue;
            }
            if (native_[i] && (op == Op::PutProp || op == Op::InitProp)) {
                const InlineCache& ic = code_->caches[code_->code[i + 1].a];
                const std::uint32_t o = names[in.a];
                if (ic.next == 0) {
                    forget(ic.shape, ic.index);
                    k.push_back({o, ic.shape, ic.index, names[in.c]});
                } else {
                    const std::size_t had = k.size();
                    for (std::size_t x = 0; x < had; ++x)
                        if (k[x].obj == o && k[x].shape == ic.shape)
                            k.push_back({o, ic.next, k[x].index, k[x].value});
                    k.push_back({o, ic.next, ic.index, names[in.c]});
                }
            } else if (op == Op::PutUpval || op == Op::PutBox) {
                // A box and an upvalue may be the same variable.
                forget(0, 0);
                forget(1, 0);
                if (native_[i] && op == Op::PutUpval)
                    k.push_back({ssa::kNone, 0, in.a, names[in.b]});
                else if (native_[i])
                    k.push_back({names[in.a], 1, 0, names[in.b]});
            }
            if (op == Op::Mov) {
                names[in.a] = names[in.b];
                continue;
            }
            for (std::size_t w = 0; w < e.wr.size(); ++w)
                names[e.wr[w]] = g.defBase[i] + std::uint32_t(w);
            if (read)
                k.push_back({obj, shape, index, g.defBase[i]});
            if (k.size() > 64)
                k.clear();
        }
        out[b] = std::move(k);
        done[b] = 1;
    }
    if (!any)
        return false;
    for (ssa::Block& bl : g.blocks) {
        for (std::uint32_t& v : bl.nameIn)
            v = real(v);
        for (std::uint32_t& v : bl.nameOut)
            v = real(v);
    }
    for (ssa::Value& sv : g.values)
        for (std::uint32_t& v : sv.in)
            v = real(v);
    return true;
}

void Baseline::ssaForgetShapes()
{
    epochSeen_ = false;
    for (std::uint32_t v : touched_)
        vShape_[v] = 0;
}

void Baseline::ssaResetFacts()
{
    epochSeen_ = false;
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

/// Only that it is a cell, as before a compare of its first word with a shape's id (`needCell`).
void Baseline::needCellV(std::uint32_t v, Reg r, Label& bad)
{
    if (!known(v, kVObj))
        needCell(r, bad);
}

/// The register that holds value `v` boxed: its own, or rax, loaded.
Reg Baseline::valueReg(std::uint32_t v)
{
    const SVal& s = sval_[v];
    if (!s.isConst && s.rep == kRepTagged && s.reg >= 0)
        return Reg(s.reg);
    loadValue(v, RAX);
    return RAX;
}

/// A register whose low half is the int32 `v` (known to be one): its own, or `tmp`, loaded.
Reg Baseline::i32Reg(std::uint32_t v, Reg tmp)
{
    const SVal& s = sval_[v];
    if (!s.isConst && s.rep != kRepF64 && s.reg >= 0)
        return Reg(s.reg);
    loadI32(v, tmp, nullptr);
    return tmp;
}

/// Where to make the boxed result of instruction `i`: its value's own register, or rax (then `ssaDef`).
Reg Baseline::defReg(std::uint32_t i) const
{
    const SVal& s = sval_[ssa_->defBase[i]];
    return s.rep == kRepTagged && s.reg >= 0 ? Reg(s.reg) : RAX;
}

/// As `ssaDef`, of a result made in `defReg`.
void Baseline::ssaDefIn(std::uint32_t i, Reg d, std::uint8_t bits)
{
    if (d == RAX) {
        ssaDef(i, bits);
        return;
    }
    const std::uint32_t v = ssa_->defBase[i];
    name_[code_->code[i].a] = v;
    vDef_[v] = bits;
    vKnown_[v] = bits;
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

/// Instruction `i` knew nothing when this code was made (an empty cache): after the code has run a while, it makes way
/// for code made with what the baseline code has learned since (exit kind 2: nothing is marked failed). Before `i`.
void Baseline::ssaImmature(std::uint32_t i)
{
    ssaExits_.push_back(SsaExit{i, name_, frame_, liveBefore_, Label{}});
    ssaExits_.back().kind = 2;
    a_.movImm(RAX, reinterpret_cast<std::uint64_t>(&code_->tier2Budget));
    a_.dec32(mem(RAX, 0));
    a_.jcc(CE, ssaExits_.back().label);
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
void Baseline::ssaCondEdges(std::uint32_t b, Cond taken, bool last)
{
    const ssa::Block& bl = ssa_->blocks[b];
    const std::uint32_t s0 = bl.succs[0], s1 = bl.succs[1];
    if (s0 != s1 && ssaPlainEdge(b, s1)) {
        a_.jcc(taken, blockLabel_[s1]);
        ssaTakePlainEdge(s1);
        ssaEdge(b, s0, last);
        return;
    }
    if (s0 != s1 && ssaPlainEdge(b, s0)) {
        a_.jcc(Cond(std::uint8_t(taken) ^ 1), blockLabel_[s0]);
        ssaTakePlainEdge(s0);
        ssaEdge(b, s1, last);
        return;
    }
    const std::vector<std::uint32_t> st = frame_;
    Label yes;
    a_.jcc(taken, yes);
    ssaEdge(b, s0);
    a_.bind(yes);
    frame_ = st;
    ssaEdge(b, s1, last);
}

namespace {
Cond invert(Cond c)
{
    return Cond(std::uint8_t(c) ^ 1);
}
}  // namespace

/// The two ways out of block `b`, whose jump is taken on equality (`eqTaken`) or on the opposite; `emit` is given where to go
/// for each.
template <class Emit>
void Baseline::ssaEqualEdges(std::uint32_t b, bool eqTaken, Emit emit)
{
    const ssa::Block& bl = ssa_->blocks[b];
    const std::uint32_t se = bl.succs[eqTaken ? 1 : 0], sn = bl.succs[eqTaken ? 0 : 1];
    const bool ed = se != sn && ssaPlainEdge(b, se), nd = se != sn && ssaPlainEdge(b, sn);
    Label eq, ne;
    emit(ed ? blockLabel_[se] : eq, nd ? blockLabel_[sn] : ne);
    if (ssaFailed_)
        return;
    const std::vector<std::uint32_t> state = frame_;
    if (ed) {
        ssaTakePlainEdge(se);
    } else {
        a_.bind(eq);
        ssaEdge(b, se, nd);
    }
    frame_ = state;
    if (nd) {
        ssaTakePlainEdge(sn);
    } else {
        a_.bind(ne);
        ssaEdge(b, sn, true);
    }
}

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
        a_.mov(RDX, valueReg(name_[in.a]));
        a_.aluI(4, RDX, -9);
        a_.cmpI(RDX, std::int32_t(kNull));
        ssaCondEdges(b, op == Op::JNullish ? CE : CNE);
        return true;
    case Op::JUndefined: case Op::JNotUndefined: case Op::JHole:
        a_.cmpI(valueReg(name_[in.a]), std::int32_t(op == Op::JHole ? kHole : kUndefined));
        ssaCondEdges(b, op == Op::JNotUndefined ? CNE : CE);
        return true;
    case Op::JTrue: case Op::JFalse: {
        const std::uint32_t v = name_[in.a];
        const Reg x = valueReg(v);
        const bool onTrue = op == Op::JTrue;
        if (known(v, kVBool)) {
            a_.cmpI(x, std::int32_t(kTrue));
            ssaCondEdges(b, onTrue ? CE : CNE);
            return true;
        }
        if (known(v, kVInt)) {
            a_.test32(x, x);
            ssaCondEdges(b, onTrue ? CNE : CE);
            return true;
        }
        if (failed)
            return false;
        Label& bad = ssaExit(i);
        const std::uint32_t st = bl.succs[onTrue ? 1 : 0], sf = bl.succs[onTrue ? 0 : 1];
        const bool td = st != sf && ssaPlainEdge(b, st), fd = st != sf && ssaPlainEdge(b, sf);
        Label t, f, notCell, notInt;
        Label& T = td ? blockLabel_[st] : t;
        Label& F = fd ? blockLabel_[sf] : f;
        if (!known(v, kVNotCell)) {
            // An object is true; a string or a double is left to the interpreter.
            a_.test(x, R15);
            a_.jcc(CNE, notCell);
            a_.cmpI8(mem(x, kHdrKind), kObjectKind);
            a_.jcc(CNE, bad);
            a_.jmp(T);
            a_.bind(notCell);
        }
        a_.cmpI(x, std::int32_t(kTrue));
        a_.jcc(CE, T);
        a_.cmp(x, R14);
        a_.jcc(CB, notInt);
        a_.test32(x, x);
        a_.jcc(CE, F);
        a_.jmp(T);
        a_.bind(notInt);
        a_.test(x, R14);
        a_.jcc(CNE, bad);
        // False, null or undefined.
        const std::vector<std::uint32_t> state = frame_;
        if (fd) {
            if (!ssaNextBlock(b, sf) || !td)
                a_.jmp(F);
            ssaTakePlainEdge(sf);
        } else {
            a_.bind(f);
            ssaEdge(b, sf, td);
        }
        frame_ = state;
        if (td) {
            ssaTakePlainEdge(st);
        } else {
            a_.bind(t);
            ssaEdge(b, st, true);
        }
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
        if (!pa && !pb && known(va, kVInt) && known(vb, kVInt)) {
            a_.alu32(7, i32Reg(va, RAX), i32Reg(vb, RCX));
            ssaCondEdges(b, eqTaken ? CE : CNE);
            return true;
        }
        if (pa || pb) {
            a_.cmpI(valueReg(pa ? vb : va), std::int32_t(sval_[pa ? va : vb].bits));
            ssaCondEdges(b, eqTaken ? CE : CNE);
            return true;
        }
        if (failed && loose)
            return false;
        Label& bad = ssaExit(i);
        if (loose && !ssa_->held(i).empty()) {
            std::vector<unsigned> regs;
            ssaAddHeld(i, regs);
            ssaSync(std::move(regs), liveBefore_, std::vector<std::uint8_t>(code_->nregs, 0));
            if (ssaFailed_)
                return true;
        }
        const bool ka = intConst(va), kb = intConst(vb);
        if (ka || kb) {
            const Reg x = valueReg(ka ? vb : va);
            const std::int32_t k = JSValue::raw(sval_[ka ? va : vb].bits).asInt32();
            ssaEqualEdges(b, eqTaken, [&](Label& eq, Label& ne) { ssaEqualsInt(i, x, k, loose, bad, eq, ne); });
            return true;
        }
        loadValue(va, RAX);
        loadValue(vb, RCX);
        ssaEqualEdges(b, eqTaken, [&](Label& eq, Label& ne) { ssaEqualsCore(i, loose, bad, eq, ne); });
        return true;
    }
    case Op::JStrictEqI: case Op::JStrictNeI: {
        const std::uint32_t v = name_[in.a];
        const std::int32_t imm = std::int32_t(std::int16_t(in.b));
        if (known(v, kVInt)) {
            // An int32 already: the low halves compared.
            a_.aluI32(7, i32Reg(v, RAX), imm);
            ssaCondEdges(b, op == Op::JStrictEqI ? CE : CNE);
            return true;
        }
        Label& bad = ssaExit(i);
        const Reg x = valueReg(v);
        ssaEqualEdges(b, op == Op::JStrictEqI, [&](Label& eq, Label& ne) { ssaEqualsInt(i, x, imm, false, bad, eq, ne); });
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
                    [&](Cond c, bool last) { ssaCondEdges(b, c, last); });
        return true;
    }
    case Op::JNotCallee: {
        // The register holds the function this frame runs (a call of itself, run in place by `selfInline`).
        loadValue(name_[in.a], RAX);
        a_.cmp(RAX, mem(R13, kFrameFn));
        ssaCondEdges(b, CNE);
        return true;
    }
    case Op::JNotCode: {
        // The register holds a function of the code whose uid the constant names (or the constant itself, a function), and a
        // `this` it can take as it is.
        const JSValue want = code_->consts[in.b];
        const std::uint64_t uid = want.isInt32() ? std::uint64_t(want.asInt32()) : 0;
        CodeBlock* callee = want.isInt32() ? vm_.codeByUid(uid) : static_cast<JSFunction*>(asObject(want))->code;
        const std::uint32_t vf = name_[in.a], vt = name_[in.a + 1u];
        const bool direct = bl.succs[0] != bl.succs[1] && ssaPlainEdge(b, bl.succs[1]);
        Label away;
        Label& no = direct ? blockLabel_[bl.succs[1]] : away;
        const JSValue kf = sval_[vf].isConst ? JSValue::raw(sval_[vf].bits) : JSValue::undefined();
        // A constant function of that code: nothing to test.
        const bool sure = want.isInt32() ? isFunction(kf) && callee && static_cast<JSFunction*>(asObject(kf))->code == callee
                                         : kf.bits == want.bits;
        if (!sure && !want.isInt32()) {
            loadValue(vf, RAX);
            a_.movImm(RDX, want.bits);
            a_.cmp(RAX, RDX);
            a_.jcc(CNE, no);
        } else if (!sure) {
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
        }
        if ((!callee || !(callee->flags & (CodeBlock::kStrict | CodeBlock::kArrow | CodeBlock::kNoThis))) &&
            !known(vt, kVObj)) {
            loadValue(vt, RAX);
            a_.test(RAX, R15);
            a_.jcc(CNE, no);
            a_.cmpI8(mem(RAX, kHdrKind), kObjectKind);
            a_.jcc(CNE, no);
        }
        if (direct) {
            ssaTakePlainEdge(bl.succs[1]);
            ssaEdge(b, bl.succs[0], true);
            return true;
        }
        const std::vector<std::uint32_t> st = frame_;
        ssaEdge(b, bl.succs[0]);
        a_.bind(no);
        frame_ = st;
        ssaEdge(b, bl.succs[1], true);
        return true;
    }
    default:
        return false;
    }
}

/// Empty when the site has found nothing, something of another kind, or has made code leave.
bool Baseline::ssaGetterRead(const PolyEntry& pe, std::uint32_t* index) const
{
    const Shape* s = liveShape(pe.shape);
    if (ssaOn_ < 2 || pe.kind != PolyEntry::ProtoAccessor || !s || !s->proto || !pe.holder || s->isDictionary())
        return false;
    const JSValue pair = *pe.holder->slotAt(pe.index);
    if (!pair.isCell() || isObject(pair) || !isVmCell(pair.asCell()) || kindOf(pair.asCell()) != Kind::GetterSetter)
        return false;
    const JSValue g = reinterpret_cast<const GetterSetter*>(pair.asCell())->getter;
    if (!isFunction(g))
        return false;
    auto* f = static_cast<JSFunction*>(asObject(g));
    const CodeBlock* c = f->code;
    if ((f->flags & JSFunction::kNative) || !c || (c->flags & (CodeBlock::kLazy | CodeBlock::kGenerator | CodeBlock::kAsync)) ||
        c->code.size() < 3)
        return false;
    Key key = nullptr;
    Box* box = nullptr;
    const Ins* in = c->code.data();
    if (in[0].opcode() == Op::GetProp && in[0].b == 0 && in[1].opcode() == Op::Cache && in[2].opcode() == Op::Ret && in[2].a == in[0].a) {
        key = c->atoms[in[0].c];
    } else if (in[0].opcode() == Op::GetUpval && in[0].a != 0 && in[1].opcode() == Op::GetElem && in[1].b == 0 && in[1].c == in[0].a &&
               in[2].opcode() == Op::Ret && in[2].a == in[1].a && (c->flags & CodeBlock::kPrivate) && in[0].b < f->nUpvals) {
        box = f->upvals()[in[0].b];
        if (!isSymbol(box->value) || (box->h.aux & Box::kNotConstant) || (box->h.aux & Box::kWrites) == Box::kWrites)
            return false;
        key = static_cast<Key>(box->value.asCell());
    }
    if (!key)
        return false;
    std::uint8_t attrs = 0;
    const std::int32_t at = const_cast<Shape*>(s)->indexOf(key, &attrs);
    if (at < 0 || (attrs & attr::kAccessor) || !vm_.watchMethod(s->proto, pe.holder))
        return false;
    if (box)
        box->h.aux |= Box::kWatched;
    *index = std::uint32_t(at);
    return true;
}

std::vector<Baseline::ReadEntry> Baseline::ssaReadEntries(std::uint32_t i) const
{
    std::vector<ReadEntry> es;
    if (i + 1 >= n_ || code_->code[i + 1].opcode() != Op::Cache)
        return es;
    const std::uint16_t cacheId = code_->code[i + 1].a;
    if (cacheId == kNoCache || (i < code_->prof.size() && (code_->prof[i] & opt::kProfFailed)))
        return es;
    const InlineCache& ic = code_->caches[cacheId];
    if (ic.next != 0 || (ic.holder && !(ic.validity && ic.validity->valid)) ||
        (ic.shape == 0 && code_->atoms[code_->code[i].c] == vm_.atoms.length))
        return es;
    if (ic.shape != 0)
        es.push_back({ic.shape, ic.index, ic.holder, ic.validity, false});
    if (ic.pad && ic.pad <= code_->poly.size())
        for (std::uint8_t k = 0; k < code_->poly[ic.pad - 1].n; ++k) {
            const PolyEntry& e = code_->poly[ic.pad - 1].e[k];
            if (e.kind == PolyEntry::OwnData) {
                es.push_back({e.shape, e.index, nullptr, nullptr, false});
            } else if (e.kind == PolyEntry::ProtoData && e.validity && e.validity->valid) {
                es.push_back({e.shape, e.index, e.holder, e.validity, false});
            } else if (e.kind == PolyEntry::Absent && (!e.validity || e.validity->valid)) {
                es.push_back({e.shape, 0, nullptr, e.validity, true});
            } else if (std::uint32_t index = 0; ssaGetterRead(e, &index)) {
                es.push_back({e.shape, index, nullptr, nullptr, false, true});
            } else {
                es.clear();
                return es;
            }
        }
    return es;
}

bool Baseline::ssaGetProp(std::uint32_t i)
{
    const Ins& in = code_->code[i];
    const std::vector<ReadEntry> es = ssaReadEntries(i);
    if (es.empty())
        return false;
    const std::uint32_t v = name_[in.b];
    if (const std::uint32_t d = ssa_->defBase[i]; sval_[d].isConst) {
        // A method that stays as it is: what is left is the test that the receiver still inherits it.
        Label& bad = ssaExit(i);
        bool shapeKnown = false;
        for (const ReadEntry& e : es)
            shapeKnown = shapeKnown || (vShape_[v] && vShape_[v] == e.shape);
        if (!shapeKnown) {
            const Reg o = valueReg(v);
            needCellV(v, o, bad);
            Label ok;
            for (std::size_t k = 0; k < es.size(); ++k) {
                a_.cmpI32(mem(o, 0), std::int32_t(es[k].shape));
                if (k + 1 < es.size())
                    a_.jcc(CE, ok);
                else
                    a_.jcc(CNE, bad);
            }
            a_.bind(ok);
            ssaKnow(v, kVObj, es.size() == 1 ? es[0].shape : 0);
        }
        ssaEpochCheck(i);
        name_[in.a] = d;
        return true;
    }
    Label& bad = ssaExit(i);
    const Reg o = valueReg(v), d = defReg(i);
    needCellV(v, o, bad);
    for (const ReadEntry& e : es)
        if (e.watched) {
            ssaEpochCheck(i);
            break;
        }
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
        for (std::size_t n = 0; n + 1 < emit.size(); ++n) {
            a_.cmpI32(mem(o, 0), std::int32_t(es[emit[n]].shape));
            a_.jcc(CE, at[emit[n]]);
        }
        a_.cmpI32(mem(o, 0), std::int32_t(es[emit.back()].shape));
        a_.jcc(CNE, bad);
        std::rotate(emit.begin(), emit.end() - 1, emit.end());  // the last test falls into its own
    }
    for (std::size_t n = 0; n < emit.size(); ++n) {
        const std::size_t k = emit[n];
        a_.bind(at[k]);
        const ReadEntry& e = es[k];
        if (e.holder || (e.absent && e.validity)) {
            hold(e.validity);
            a_.movImm(RCX, reinterpret_cast<std::uint64_t>(e.validity));
            a_.cmpI32(mem(RCX, kValidityValid), 0);
            a_.jcc(CE, bad);
        }
        if (e.absent) {
            a_.movImm(d, kUndefined);
        } else if (e.holder) {
            holderRead(e.holder, e.index);
            if (d != RAX)
                a_.mov(d, RAX);
        } else {
            a_.mov(d, slotMem(e.shape, e.index, o));
        }
        if (n + 1 < emit.size())
            a_.jmp(done);
    }
    a_.bind(done);
    ssaKnow(v, kVObj, emit.size() == 1 ? es[emit[0]].shape : 0);
    ssaDefIn(i, d, 0);
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
    const std::uint32_t x = name_[in.c];
    const bool number = !mayBeCell(x) && (sval_[x].rep != kRepTagged ||
                                          (sval_[x].isConst && JSValue::raw(sval_[x].bits).isNumber()));
    if (!number) {
        // A value that may be a cell (most likely over another): every live value gets an owner, the old one goes here.
        ssaSync({}, liveAfter, std::vector<std::uint8_t>(code_->nregs, 0));
        if (ssaFailed_)
            return true;
        Label& bad = ssaExit(i);
        const Reg o = valueReg(v);
        needCellV(v, o, bad);
        if (!vShape_[v]) {
            a_.cmpI32(mem(o, 0), std::int32_t(ic.shape));
            a_.jcc(CNE, bad);
            ssaKnow(v, kVObj, ic.shape);
        }
        const Mem slot = slotMem(ic.shape, ic.index, o);
        loadValue(x, RCX);
        if (mayBeCell(x))
            retain(RCX);
        a_.mov(RDI, slot);
        a_.mov(slot, RCX);
        release(RDI);
        return true;
    }
    const Reg o = valueReg(v);
    {
        Label& bad = ssaExit(i);
        needCellV(v, o, bad);
        if (!vShape_[v]) {
            a_.cmpI32(mem(o, 0), std::int32_t(ic.shape));
            a_.jcc(CNE, bad);
        }
    }
    // An old value that is a cell is let go of, which may free what a live value borrows: the runtime's store, out of
    // line, after every live value has an owner. Anything else (a number over a number) is stored here, with no sync.
    Label& cold = coldLabels_.emplace_back();
    Label& join = coldLabels_.emplace_back();
    const std::vector<std::uint32_t> frameBefore = frame_;
    ssaColdCall(i, &helperThunk<jitPutProp>, liveAfter, cold, join);
    if (ssaFailed_)
        return true;
    const std::vector<std::uint32_t> frameAfter = frame_;
    frame_ = frameBefore;
    const Mem slot = slotMem(ic.shape, ic.index, o);
    a_.mov(RDI, slot);
    a_.test(RDI, R15);
    a_.jcc(CE, cold);  // a cell
    loadValue(x, RCX);
    if (mayBeCell(x))
        retain(RCX);
    a_.mov(slot, RCX);
    a_.bind(join);
    frame_ = frameAfter;  // the runtime's store over a data property of that shape changes no shape
    ssaKnow(v, kVObj, ic.shape);
    return true;
}

/// Whether `new` instruction `i` can be made in place (`SsaNew`): its constructor, as the register's last global or upvalue
/// read found it, is a plain function whose body stores arguments or constants into new properties of `this` (each an
/// add its cache saw, into an inline slot) and returns undefined.
bool Baseline::ssaNewPlan(std::uint32_t i, SsaNew* out)
{
    const Ins& in = code_->code[i];
    if (ssaOn_ < 2 || in.opcode() != Op::New || (i < code_->prof.size() && (code_->prof[i] & opt::kProfFailed)))
        return false;
#if defined(SCRIPT_JIT_ENV_OVERRIDE)
    static const bool noNew = std::getenv("SSA_NONEW") != nullptr;
    if (noNew)
        return false;
#endif
    const JSValue known = guessBefore(i, in.b);
    if (!isFunction(known))
        return false;
    auto* f = static_cast<JSFunction*>(asObject(known));
    if ((f->flags & JSFunction::kNative) || !vm_.prototypeAt2(f))
        return false;
    CodeBlock* C = f->code;
    if (!C || !C->constructor ||
        (C->flags & (CodeBlock::kLazy | CodeBlock::kGenerator | CodeBlock::kAsync | CodeBlock::kAsyncGen |
                     CodeBlock::kNeedsArgs | CodeBlock::kDerived)))
        return false;
    // A base class's constructor. Its field initialiser is fixed when the class is made: undefined (the call of it jumped
    // over), or one that only defines fields as constants, which is taken in too.
    JSFunction* fieldFn = nullptr;
    if ((C->flags & CodeBlock::kClassConstructor) && f->rare() && isFunction(f->rare()->fieldInit)) {
        fieldFn = static_cast<JSFunction*>(asObject(f->rare()->fieldInit));
        if ((fieldFn->flags & JSFunction::kNative) || !fieldFn->code || (fieldFn->code->flags & CodeBlock::kLazy))
            return false;
    }
    const JSValue pv = *f->slotAt(2);
    if (!isObject(pv))
        return false;
    const Shape* root = vm_.rootShapeFor(asObject(pv), 3);
    SsaNew p;
    p.fn = known;
    p.proto = pv;
    p.fnShape = f->h.shape;
    p.aux = newSlotsOf(C, root);
    p.shape = vm_.rootShapeFor(asObject(pv), p.aux)->id;
    if (!canAllocObject(p.aux))
        return false;
    std::vector<std::int8_t> isConst(C->nregs, 0), isSelf(C->nregs, 0);
    std::vector<JSValue> constVal(C->nregs, JSValue::undefined());
    std::vector<JSValue*> ownOf(C->nregs, nullptr);
    std::vector<const void*> keyOf(C->nregs, nullptr);
    auto plainReg = [&](unsigned r) {
        isConst[r] = 0;
        isSelf[r] = 0;
        ownOf[r] = nullptr;
        keyOf[r] = nullptr;
    };
    // `this.#x = v` (`define`: the initialiser's `#x = v`) at the index the site found, the shape as it was.
    auto privateStore = [&](const CodeBlock* B, std::uint32_t j, const void* key, bool define, const SsaNew::Store& st) {
        if (!key || j >= B->privCaches.size())
            return false;
        const PrivateCache& pc = B->privCaches[j];
        if (pc.key != key || pc.shape != p.shape || pc.index >= p.aux || (define ? pc.next == 0 : pc.next != 0))
            return false;
        SsaNew::Store s = st;
        s.index = pc.index;
        p.stores.push_back(s);
        if (define)
            p.shape = pc.next;
        return true;
    };
    // The field initialiser's body, run on the object being made: private names read from its upvalues, constants
    // defined as fields of `this`, then `return undefined`.
    auto takeFields = [&]() {
        const CodeBlock* F = fieldFn->code;
        std::vector<const void*> fkey(F->nregs, nullptr);
        std::vector<std::int8_t> fconst(F->nregs, 0), fself(F->nregs, 0);
        std::vector<JSValue> fval(F->nregs, JSValue::undefined());
        std::vector<JSValue*> fown(F->nregs, nullptr);
        for (std::uint32_t j = 0; j < F->code.size(); j = opt::nextOf(*F, j)) {
            const Ins& fi = F->code[j];
            switch (fi.opcode()) {
            case Op::Nop:
                break;
            case Op::GetUpval: {
                if (fi.a == 0 || fi.b >= fieldFn->nUpvals)
                    return false;
                const Box* box = reinterpret_cast<Box*>(fieldFn->upvals()[fi.b]);
                if (!box->value.isCell())
                    return false;
                fconst[fi.a] = 0;
                fown[fi.a] = nullptr;
                fself[fi.a] = 0;
                fkey[fi.a] = nullptr;
                if (box->value.bits == known.bits) {
                    // The class itself (`color = Light.defaultColor`): the box checked to hold it.
                    fself[fi.a] = 1;
                    p.selfBoxes.push_back(box);
                    break;
                }
                fkey[fi.a] = box->value.asCell();
                p.keyBoxes.emplace_back(box, box->value.bits);
                break;
            }
            case Op::GetProp: {
                // A static field of the class: its slot in the class, read when stored (the class's shape is checked).
                if (fi.a == 0 || fi.b >= F->nregs || !fself[fi.b])
                    return false;
                std::uint8_t attrs = 0;
                JSValue* slot = f->findOwn(vm_, F->atoms[fi.c], &attrs);
                if (!slot || (attrs & attr::kAccessor))
                    return false;
                fconst[fi.a] = 0;
                fself[fi.a] = 0;
                fkey[fi.a] = nullptr;
                fown[fi.a] = slot;
                break;
            }
            case Op::LoadUndef: case Op::LoadNull: case Op::LoadTrue: case Op::LoadFalse: case Op::LoadInt:
                if (fi.a == 0)
                    return false;
                fconst[fi.a] = 1;
                fkey[fi.a] = nullptr;
                fself[fi.a] = 0;
                fown[fi.a] = nullptr;
                fval[fi.a] = fi.opcode() == Op::LoadUndef ? JSValue::undefined() : fi.opcode() == Op::LoadNull ? JSValue::null()
                           : fi.opcode() == Op::LoadInt ? JSValue::int32(fi.imm()) : JSValue::boolean(fi.opcode() == Op::LoadTrue);
                break;
            case Op::DefPrivate: case Op::DefPrivMethod: {
                // A constant, or (a private method) the function an upvalue holds, checked to hold it when the code runs.
                if (fi.a != 0 || fi.b >= F->nregs || fi.c >= F->nregs || !fkey[fi.b])
                    return false;
                JSValue v;
                if (fconst[fi.c])
                    v = fval[fi.c];
                else if (fi.opcode() == Op::DefPrivMethod && fkey[fi.c] &&
                         isFunction(JSValue::cell(reinterpret_cast<CellPtr>(const_cast<void*>(fkey[fi.c])))))
                    v = JSValue::cell(reinterpret_cast<CellPtr>(const_cast<void*>(fkey[fi.c])));
                else
                    return false;
                if (!privateStore(F, j, fkey[fi.b], true, SsaNew::Store{0, -1, v}))
                    return false;
                break;
            }
            case Op::InitProp: {
                // A public field: added as the site found it.
                if (fi.a != 0 || fi.c >= F->nregs || (!fconst[fi.c] && !fown[fi.c]) || j + 1 >= F->code.size() ||
                    F->code[j + 1].opcode() != Op::Cache)
                    return false;
                const std::uint16_t cacheId = F->code[j + 1].a;
                if (cacheId == kNoCache || cacheId >= F->caches.size())
                    return false;
                const InlineCache& ic = F->caches[cacheId];
                if (ic.shape != p.shape || ic.next == 0 || ic.index >= p.aux || ic.holder || ic.pad)
                    return false;
                if (ic.validity) {
                    if (!ic.validity->valid)
                        return false;
                    p.cells.push_back(ic.validity);
                }
                if (fown[fi.c])
                    p.stores.push_back(SsaNew::Store{ic.index, -2, JSValue::undefined(), fown[fi.c]});
                else
                    p.stores.push_back(SsaNew::Store{ic.index, -1, fval[fi.c]});
                p.shape = ic.next;
                break;
            }
            case Op::Ret:
                return fi.a < F->nregs && fconst[fi.a] && fval[fi.a].isUndefined();
            default:
                return false;
            }
        }
        return false;
    };
    std::int64_t fieldReg = -1, thisCopy = -1;
    bool fieldsTaken = false;
    // The value register `r` holds at a store: a constant, an argument (undefined past them), or a static field.
    auto valueOf = [&](unsigned r, SsaNew::Store* st) {
        if (ownOf[r])
            *st = SsaNew::Store{0, -2, JSValue::undefined(), ownOf[r]};
        else if (isConst[r])
            *st = SsaNew::Store{0, -1, constVal[r]};
        else if (r >= 1 && r <= C->nparams)
            *st = r - 1u < in.c ? SsaNew::Store{0, int(r) - 1, JSValue::undefined()} : SsaNew::Store{0, -1, JSValue::undefined()};
        else
            return false;
        return true;
    };
    std::int64_t jumpTo = -1;
    for (std::uint32_t j = 0; j < C->code.size();
         j = jumpTo >= 0 ? std::uint32_t(std::exchange(jumpTo, -1)) : opt::nextOf(*C, j)) {
        const Ins& ci = C->code[j];
        switch (ci.opcode()) {
        case Op::Nop:
            break;
        case Op::FieldInit:  // the field initialiser, or undefined
            if (!(C->flags & CodeBlock::kClassConstructor) || ci.a <= C->nparams)
                return false;
            plainReg(ci.a);
            if (fieldFn) {
                fieldReg = ci.a;
                break;
            }
            isConst[ci.a] = 1;
            constVal[ci.a] = JSValue::undefined();
            break;
        case Op::JUndefined: {
            const std::int64_t t = opt::jumpTarget(*C, j);
            if (fieldFn && std::int64_t(ci.a) == fieldReg)
                break;  // a function: on to its call
            if (ci.a >= C->nregs || !isConst[ci.a] || !constVal[ci.a].isUndefined() || t <= std::int64_t(j))
                return false;
            jumpTo = t;
            break;
        }
        case Op::Mov:
            // `this` into the window of the field initialiser's call.
            if (!fieldFn || ci.b != 0 || std::int64_t(ci.a) != fieldReg + 1)
                return false;
            thisCopy = ci.a;
            break;
        case Op::Call:
            if (!fieldFn || fieldsTaken || std::int64_t(ci.b) != fieldReg || ci.c != 0 || thisCopy != std::int64_t(ci.b) + 1 ||
                ci.a <= C->nparams || !takeFields())
                return false;
            fieldsTaken = true;
            fieldReg = thisCopy = -1;
            plainReg(ci.a);
            isConst[ci.a] = 1;  // its result: undefined
            constVal[ci.a] = JSValue::undefined();
            break;
        case Op::PutElem: {
            // `this.#x = v` over a field the initialiser defined.
            if (ci.a != 0 || ci.b >= C->nregs || ci.c >= C->nregs || !keyOf[ci.b])
                return false;
            SsaNew::Store st{};
            if (!valueOf(ci.c, &st) || !privateStore(C, j, keyOf[ci.b], false, st))
                return false;
            break;
        }
        case Op::GetUpval: {
            // The class itself (its own name in its methods): the box is checked to hold it when the code runs.
            if (ci.a <= C->nparams || ci.b >= f->nUpvals)
                return false;
            const Box* box = reinterpret_cast<Box*>(f->upvals()[ci.b]);
            plainReg(ci.a);
            if (box->value.bits != known.bits) {
                // A private name, for `this.#x = v`: the box checked to hold it.
                if (!(C->flags & CodeBlock::kPrivate) || !box->value.isCell())
                    return false;
                keyOf[ci.a] = box->value.asCell();
                p.keyBoxes.emplace_back(box, box->value.bits);
                break;
            }
            isSelf[ci.a] = 1;
            p.selfBoxes.push_back(box);
            break;
        }
        case Op::GetProp: {
            // A data property of the constructor itself, read when it is stored (its shape is checked).
            if (ci.a <= C->nparams || ci.b >= C->nregs || !isSelf[ci.b])
                return false;
            std::uint8_t attrs = 0;
            JSValue* slot = f->findOwn(vm_, C->atoms[ci.c], &attrs);
            if (!slot || (attrs & attr::kAccessor))
                return false;
            plainReg(ci.a);
            ownOf[ci.a] = slot;
            break;
        }
        case Op::LoadUndef: case Op::LoadNull: case Op::LoadTrue: case Op::LoadFalse: case Op::LoadInt:
            if (ci.a <= C->nparams)
                return false;
            plainReg(ci.a);
            isConst[ci.a] = 1;
            constVal[ci.a] = ci.opcode() == Op::LoadUndef ? JSValue::undefined() : ci.opcode() == Op::LoadNull ? JSValue::null()
                           : ci.opcode() == Op::LoadInt ? JSValue::int32(ci.imm()) : JSValue::boolean(ci.opcode() == Op::LoadTrue);
            break;
        case Op::PutProp: {
            if (ci.a != 0 || ci.c == 0 || ci.c >= C->nregs || j + 1 >= C->code.size() || C->code[j + 1].opcode() != Op::Cache)
                return false;
            const std::uint16_t cacheId = C->code[j + 1].a;
            if (cacheId == kNoCache)
                return false;
            const InlineCache& ic = C->caches[cacheId];
            // An add, or (after the field initialiser) a store over a field the object has.
            if (ic.shape != p.shape || ic.index >= p.aux || ic.holder || ic.pad || (ic.next == 0 && !fieldsTaken))
                return false;
            if (ic.validity) {
                if (!ic.validity->valid)
                    return false;
                p.cells.push_back(ic.validity);
            }
            SsaNew::Store st{};
            if (!valueOf(ci.c, &st))
                return false;
            st.index = ic.index;
            p.stores.push_back(st);
            if (ic.next)
                p.shape = ic.next;
            break;
        }
        case Op::Ret:
            if (ci.a >= C->nregs || !isConst[ci.a] || !constVal[ci.a].isUndefined())
                return false;
            if (out)
                *out = std::move(p);
            return true;
        default:
            return false;
        }
    }
    return false;
}

/// `new` made in place (`ssaNewPlan`): every check before the object is made. When the heap's current block has no
/// room or a collection is due, the same object is made by the runtime (`scriptVmSsaNewObject`), then filled here.
bool Baseline::ssaNew(std::uint32_t i, const ssa::Bits& liveAfter)
{
    SsaNew p;
    if (!ssaNewPlan(i, &p))
        return false;
    const Ins& in = code_->code[i];
    {
        Label& bad = ssaExit(i);
        holdValue(p.fn);
        holdValue(p.proto);
        loadValue(name_[in.b], RAX);
        a_.movImm(RCX, p.fn.bits);
        a_.cmp(RCX, RAX);
        a_.jcc(CNE, bad);
        a_.cmpI32(mem(RAX, 0), std::int32_t(p.fnShape));
        a_.jcc(CNE, bad);
        a_.mov(RDX, mem(RAX, kObjSlots));
        a_.movImm(RCX, p.proto.bits);
        a_.cmp(RCX, mem(RDX, 16));
        a_.jcc(CNE, bad);
        std::sort(p.cells.begin(), p.cells.end());
        p.cells.erase(std::unique(p.cells.begin(), p.cells.end()), p.cells.end());
        for (ValidityCell* c : p.cells) {
            hold(c);
            a_.movImm(RCX, reinterpret_cast<std::uint64_t>(c));
            a_.cmpI32(mem(RCX, kValidityValid), 0);
            a_.jcc(CE, bad);
        }
        for (const Box* box : p.selfBoxes) {
            a_.movImm(RDX, reinterpret_cast<std::uint64_t>(box));
            a_.movImm(RCX, p.fn.bits);
            a_.cmp(RCX, mem(RDX, kBoxValue));
            a_.jcc(CNE, bad);
        }
        for (const auto& [box, bits] : p.keyBoxes) {
            holdValue(JSValue::raw(bits));
            a_.movImm(RDX, reinterpret_cast<std::uint64_t>(box));
            a_.movImm(RCX, bits);
            a_.cmp(RCX, mem(RDX, kBoxValue));
            a_.jcc(CNE, bad);
        }
    }
    // The result's register lets go of what it held, and the runtime may collect: every live value, and every argument
    // stored, gets an owner first.
    {
        std::vector<std::uint8_t> clobber(code_->nregs, 0);
        clobber[in.a] = 1;
        ssa::Bits keep = liveAfter;
        ssa::bitClear(keep, in.a);
        std::vector<unsigned> args;
        for (const SsaNew::Store& st : p.stores)
            if (st.arg >= 0)
                args.push_back(in.b + 2u + unsigned(st.arg));
        ssaSync(args, keep, clobber);
        if (ssaFailed_)
            return true;
    }
    exitAt_ = -1;
    Label& failed = ssaExit(i);  // the runtime could not make it: the instruction runs again in the interpreter
    exitAt_ = -1;
    Label slowAlloc, allocated, after;
    allocObject(p.shape, p.aux, slowAlloc);
    a_.bind(allocated);
    for (const SsaNew::Store& st : p.stores) {
        const Mem slot = mem(RAX, kObjInline + std::int32_t(8 * st.index));
        if (st.own) {
            // The constructor's own slot: inline in it, or in its slot array (its shape is checked: the place holds).
            auto* fn = static_cast<JSFunction*>(asObject(p.fn));
            JSValue* inl = fn->inl();
            if (st.own >= inl && st.own < inl + fn->h.aux) {
                a_.movImm(RDX, reinterpret_cast<std::uint64_t>(st.own));
                a_.mov(RCX, mem(RDX, 0));
            } else {
                a_.movImm(RDX, reinterpret_cast<std::uint64_t>(&fn->slots));
                a_.mov(RDX, mem(RDX, 0));
                a_.mov(RCX, mem(RDX, std::int32_t(8 * (st.own - fn->slots))));
            }
            retain(RCX);
            a_.mov(slot, RCX);
            continue;
        }
        if (st.arg < 0) {
            const std::int64_t bits = std::int64_t(st.c.bits);
            if (st.c.isCell()) {
                // A function a private method names: held by the code, counted for the object.
                holdValue(st.c);
                a_.movImm(RCX, st.c.bits);
                a_.inc32(mem(RCX, kHdrRc));
                a_.mov(slot, RCX);
            } else if (bits >= INT32_MIN && bits <= INT32_MAX) {
                a_.mov(slot, std::int32_t(bits));
            } else {
                a_.movImm(RCX, st.c.bits);
                a_.mov(slot, RCX);
            }
            continue;
        }
        const std::uint32_t v = name_[in.b + 2u + unsigned(st.arg)];
        loadValue(v, RCX);
        if (mayBeCell(v))
            retain(RCX);
        a_.mov(slot, RCX);
    }
    // Into the result's slot, which owns it (a deferred one holds it without a count, as the table does).
    if (def_) {
        a_.mov(R(in.a), RAX);
    } else {
        a_.mov(RDI, R(in.a));
        a_.mov(R(in.a), RAX);
        release(RDI);
    }
    a_.mov(RAX, R(in.a));
    a_.jmp(after);
    a_.bind(slowAlloc);
    {
        const std::vector<int> keep = ssaLiveRegs();
        ssaSaveRegs(keep);
        a_.mov(RDI, R12);
        a_.movImm(RSI, p.shape);
        a_.movImm(RDX, p.aux);
        a_.movImm(RAX, reinterpret_cast<std::uint64_t>(&scriptVmSsaNewObject));
        a_.callC(RAX);
        ssaRestoreRegs(keep);
        a_.test(RAX, RAX);
        a_.jcc(CE, failed);
        a_.jmp(allocated);
    }
    a_.bind(after);
    ssaDef(i, kVObj);
    frame_[in.a] = ssa_->defBase[i];
    ssaKnow(ssa_->defBase[i], kVObj, p.shape);
    return true;
}

bool Baseline::ssaPrivateSite(std::uint32_t i) const
{
    return code_->code[i].opcode() == Op::GetElem && (code_->flags & CodeBlock::kPrivate) && i < code_->privCaches.size() &&
           code_->privCaches[i].key && code_->privCaches[i].shape;
}

void Baseline::ssaPrivateRead(std::uint32_t i)
{
    const Ins& in = code_->code[i];
    const PrivateCache& pc = code_->privCaches[i];
    const JSValue key = JSValue::cell(reinterpret_cast<CellPtr>(const_cast<void*>(pc.key)));
    holdValue(key);
    Label& bad = ssaExit(i);
    const std::uint32_t kv = name_[in.c];
    if (!sval_[kv].isConst || sval_[kv].bits != key.bits) {
        loadValue(kv, RCX);
        a_.movImm(RDX, key.bits);
        a_.cmp(RCX, RDX);
        a_.jcc(CNE, bad);
    }
    const std::uint32_t v = name_[in.b];
    const Reg o = valueReg(v), d = defReg(i);
    needCellV(v, o, bad);
    if (vShape_[v] != pc.shape) {
        a_.cmpI32(mem(o, 0), std::int32_t(pc.shape));
        a_.jcc(CNE, bad);
    }
    a_.mov(d, slotMem(pc.shape, pc.index, o));
    ssaKnow(v, kVObj, pc.shape);
    ssaDefIn(i, d, 0);
}

bool Baseline::ssaAddFast(std::uint32_t i) const
{
    const Op op = code_->code[i].opcode();
    if (ssaOn_ < 2 || (op != Op::PutProp && op != Op::InitProp) || i + 1 >= n_ || code_->code[i + 1].opcode() != Op::Cache)
        return false;
    const std::uint16_t cacheId = code_->code[i + 1].a;
    if (cacheId == kNoCache || (i < code_->prof.size() && (code_->prof[i] & opt::kProfFailed)))
        return false;
    const InlineCache& ic = code_->caches[cacheId];
    return ic.shape != 0 && ic.next != 0 && !ic.holder && !ic.pad && ic.index < 200 &&
           (op == Op::InitProp || !ic.validity || ic.validity->valid);
}

void Baseline::ssaPutAdd(std::uint32_t i)
{
    const Ins& in = code_->code[i];
    const InlineCache& ic = code_->caches[code_->code[i + 1].a];
    const std::uint32_t v = name_[in.a], x = name_[in.c];
    Label& bad = ssaExit(i);
    loadValue(v, RAX);
    needCellV(v, RAX, bad);
    if (vShape_[v] != ic.shape) {
        a_.cmpI32(mem(RAX, 0), std::int32_t(ic.shape));
        a_.jcc(CNE, bad);
    }
    if (in.opcode() == Op::PutProp) {
        // An add: no setter on the way (the cell), an object that may grow.
        if (ic.validity) {
            hold(ic.validity);
            a_.movImm(RCX, reinterpret_cast<std::uint64_t>(ic.validity));
            a_.cmpI32(mem(RCX, kValidityValid), 0);
            a_.jcc(CE, bad);
        }
        a_.testI8(mem(RAX, kHdrFlags), std::uint8_t(cellflags::kIsProto | cellflags::kNotExtensible));
        a_.jcc(CNE, bad);
    }
    addSlot(ic.shape, ic.index, bad);
    loadValue(x, RCX);
    if (mayBeCell(x))
        retain(RCX);
    a_.mov(mem(RDX, 0), RCX);
    a_.mov32(mem(RAX, 0), std::int32_t(ic.next));
    ssaKnow(v, kVObj, ic.next);
}

JSFunction* Baseline::ssaMathCallee(std::uint32_t i)
{
    const Ins& in = code_->code[i];
    if (ssaOn_ < 2 || in.opcode() != Op::Call || in.c != 1 || (i < code_->prof.size() && (code_->prof[i] & opt::kProfFailed)))
        return nullptr;
    // The instruction that wrote the callee's register last: a read of a property of a global object.
    for (std::uint32_t j = i; j-- > 0;) {
        if (code_->code[j].opcode() == Op::Cache)
            continue;
        const opt::Effects e = opt::effectsOf(*code_, j);
        if (std::find(e.wr.begin(), e.wr.end(), std::uint16_t(in.b)) == e.wr.end())
            continue;
        const Ins& r = code_->code[j];
        if (r.opcode() != Op::GetProp || r.a != in.b)
            return nullptr;
        const JSValue base = guessBefore(j, r.b);
        if (!isObject(base))
            return nullptr;
        std::uint8_t attrs = 0;
        JSValue* slot = asObject(base)->findOwn(vm_, code_->atoms[r.c], &attrs);
        if (!slot || (attrs & attr::kAccessor) || !isFunction(*slot))
            return nullptr;
        auto* f = static_cast<JSFunction*>(asObject(*slot));
        if (!(f->flags & JSFunction::kNative) || !f->rare() || f->rare()->native != mathUnaryNative())
            return nullptr;
        return f;
    }
    return nullptr;
}

/// The call of `ssaMathCallee`: the callee checked to be that function, the argument a number, the C function of its
/// data called with the live registers saved, the result boxed as the runtime boxes a number.
void Baseline::ssaMathCall(std::uint32_t i, JSFunction* f)
{
    const Ins& in = code_->code[i];
    holdValue(toJSValue(f));
    Label& bad = ssaExit(i);
    loadValue(name_[in.b], RAX);
    a_.movImm(RCX, toJSValue(f).bits);
    a_.cmp(RAX, RCX);
    a_.jcc(CNE, bad);
    loadF64(name_[in.b + 2u], 0, &bad);
    const std::vector<int> keep = ssaLiveRegs();
    ssaSaveRegs(keep);
    a_.movImm(RAX, reinterpret_cast<std::uint64_t>(f->rare()->nativeData));
    a_.callC(RAX);
    ssaRestoreRegs(keep);
    const std::uint32_t def = ssa_->defBase[i];
    if (sval_[def].rep == kRepF64) {
        ssaDefF64(i, 0);
    } else {
        boxNumber0();
        ssaDef(i, kVNotCell);
    }
}

bool Baseline::ssaLengthSite(std::uint32_t i) const
{
    const Ins& in = code_->code[i];
    return ssaOn_ >= 2 && in.opcode() == Op::GetProp && in.c < code_->atoms.size() && code_->atoms[in.c] == vm_.atoms.length &&
           ssaReadEntries(i).empty() && !(i < code_->prof.size() && (code_->prof[i] & opt::kProfFailed));
}

void Baseline::ssaLength(std::uint32_t i)
{
    const Ins& in = code_->code[i];
    const std::uint32_t v = name_[in.b];
    Label& bad = ssaExit(i);
    Label notString, done;
    loadValue(v, RAX);
    a_.test(RAX, R15);
    a_.jcc(CNE, bad);  // no cell
    a_.testI8(mem(RAX, 7), 0x80);
    a_.jcc(CNE, notString);
    a_.cmpI8(mem(RAX, kGcKind), std::uint8_t(detail::CellKind::String));
    a_.jcc(CNE, notString);
    a_.mov32(RAX, mem(RAX, kStrLength));
    a_.jmp(done);
    a_.bind(notString);
    needObject(RAX, bad);  // on this way only: nothing is known of the value after it
    a_.cmpI8(mem(RAX, kHdrCls), std::uint8_t(ObjClass::Array));
    a_.jcc(CNE, bad);
    a_.mov32(RAX, mem(RAX, kArrayLength));
    a_.test32(RAX, RAX);
    a_.jcc(CS, bad);  // past int32
    a_.bind(done);
    if (sval_[ssa_->defBase[i]].rep == kRepI32) {
        ssaDefI32(i);
    } else {
        a_.or_(RAX, R14);
        ssaDef(i, kVInt | kVNotCell);
    }
}

std::int32_t Baseline::ssaStringProtoIndex(std::uint32_t i) const
{
    const Ins& in = code_->code[i];
    if (ssaOn_ < 2 || in.opcode() != Op::GetProp || !ssaReadEntries(i).empty() || !vm_.protos.string ||
        (i < code_->prof.size() && (code_->prof[i] & opt::kProfFailed)) || code_->atoms[in.c] == vm_.atoms.length)
        return -1;
    JSObject* sp = vm_.protos.string;
    std::uint8_t attrs = 0;
    JSValue* slot = sp->findOwn(vm_, code_->atoms[in.c], &attrs);
    if (!slot || (attrs & attr::kAccessor) || !isFunction(*slot))
        return -1;
    JSValue* inl = sp->inl();
    if (slot >= inl && slot < inl + sp->h.aux)
        return std::int32_t(slot - inl);
    return sp->slots && slot >= sp->slots ? std::int32_t(sp->h.aux + (slot - sp->slots)) : -1;
}

/// `ssaStringProtoIndex`: the base checked to be a string and `String.prototype` to have the shape it had, its slot read.
void Baseline::ssaStringProtoRead(std::uint32_t i, std::uint32_t index)
{
    const Ins& in = code_->code[i];
    JSObject* sp = vm_.protos.string;
    holdValue(toJSValue(sp));
    Label& bad = ssaExit(i);
    loadValue(name_[in.b], RSI);
    needString(RSI, bad);
    a_.movImm(RAX, toJSValue(sp).bits);
    a_.cmpI32(mem(RAX, 0), std::int32_t(sp->h.shape));
    a_.jcc(CNE, bad);
    a_.mov(RAX, slotMem(sp->h.shape, index));
    ssaDef(i, kVObj);
}

JSFunction* Baseline::ssaCharCodeCallee(std::uint32_t i)
{
    const Ins& in = code_->code[i];
    if (ssaOn_ < 2 || in.opcode() != Op::Call || in.c != 1 || (i < code_->prof.size() && (code_->prof[i] & opt::kProfFailed)) ||
        !vm_.protos.string)
        return nullptr;
    for (std::uint32_t j = i; j-- > 0;) {
        if (code_->code[j].opcode() == Op::Cache)
            continue;
        const opt::Effects e = opt::effectsOf(*code_, j);
        if (std::find(e.wr.begin(), e.wr.end(), std::uint16_t(in.b)) == e.wr.end())
            continue;
        const Ins& r = code_->code[j];
        if (r.opcode() != Op::GetProp || r.a != in.b)
            return nullptr;
        std::uint8_t attrs = 0;
        JSValue* slot = vm_.protos.string->findOwn(vm_, code_->atoms[r.c], &attrs);
        if (!slot || (attrs & attr::kAccessor) || !isFunction(*slot))
            return nullptr;
        auto* f = static_cast<JSFunction*>(asObject(*slot));
        if (!(f->flags & JSFunction::kNative) || !f->rare() || f->rare()->native != stringCharNative() ||
            f->rare()->nativeData != reinterpret_cast<void*>(1))
            return nullptr;
        return f;
    }
    return nullptr;
}

/// `s.charCodeAt(i)` of `ssaCharCodeCallee`: the callee checked, a flat string and an index within it, the character
/// read here. Anything else leaves.
void Baseline::ssaCharCode(std::uint32_t i, JSFunction* f)
{
    const Ins& in = code_->code[i];
    holdValue(toJSValue(f));
    Label& bad = ssaExit(i);
    loadValue(name_[in.b], RAX);
    a_.movImm(RCX, toJSValue(f).bits);
    a_.cmp(RAX, RCX);
    a_.jcc(CNE, bad);
    loadValue(name_[in.b + 1u], RSI);
    needString(RSI, bad);
    loadI32(name_[in.b + 2u], RDX, &bad);
    a_.mov32(RDX, RDX);
    a_.alu32(7, RDX, mem(RSI, kStrLength));
    a_.jcc(CAE, bad);  // also a negative index
    stringChars(RSI, RCX, bad);
    a_.movzx16(RAX, mem(RCX, RDX, 2));
    if (sval_[ssa_->defBase[i]].rep == kRepI32) {
        ssaDefI32(i);
    } else {
        a_.or_(RAX, R14);
        ssaDef(i, kVInt | kVNotCell);
    }
}

CodeBlock* Baseline::ssaDirectCallee(std::uint32_t i) const
{
    const Ins& in = code_->code[i];
    if (ssaOn_ < 2 || in.opcode() != Op::Call || i >= code_->callProf.size() || !code_->callProf[i])
        return nullptr;
    CodeBlock* C = vm_.codeByUid(code_->callProf[i]);
    if (!C || C->inlinedFrom || in.c > C->nparams ||
        (C->flags & (CodeBlock::kLazy | CodeBlock::kGenerator | CodeBlock::kAsync | CodeBlock::kAsyncGen | CodeBlock::kNeedsArgs |
                     CodeBlock::kClassConstructor | CodeBlock::kProgram)))
        return nullptr;
    return C;
}

/// `Call` of a function of code `C` (the frame written: the callee, `this` and the arguments in their slots), its frame
/// pushed from constants; anything else goes to `slow`, the baseline call sequence. Comes back after the instruction.
void Baseline::ssaDirectCall(std::uint32_t i, CodeBlock* C, Label& slow)
{
    const Ins& in = code_->code[i];
    const unsigned base = in.b, argc = in.c;
    throws_.emplace_back(i, Label{});
    Label& exc = throws_.back().second;
    // The register holds a function of that code (compared by uid: a block that died leaves no stale match).
    const std::uint32_t vf = name_[base];
    const JSValue kf = vf != ssa::kNone && sval_[vf].isConst ? JSValue::raw(sval_[vf].bits) : JSValue::undefined();
    if (isFunction(kf) && static_cast<JSFunction*>(asObject(kf))->code == C) {
        // A constant one, of that code as long as it has not been given other code to run.
        a_.movImm(RAX, kf.bits);
        a_.movImm(RCX, reinterpret_cast<std::uint64_t>(C));
        a_.cmp(RCX, mem(RAX, kFnCode));
        a_.jcc(CNE, slow);
    } else {
        a_.mov(RAX, R(base));
        needObject(RAX, slow);
        a_.cmpI8(mem(RAX, kHdrCls), std::uint8_t(ObjClass::Function));
        a_.jcc(CNE, slow);
        a_.mov(RCX, mem(RAX, kFnCode));
        a_.test(RCX, RCX);
        a_.jcc(CE, slow);
        a_.movImm(RDX, C->uid);
        a_.cmp(RDX, mem(RCX, kCbUid));
        a_.jcc(CNE, slow);
    }
    a_.mov(RDX, mem(RCX, kCbTier1));
    a_.test(RDX, RDX);
    a_.jcc(CE, slow);
    a_.lea(R9, R(base + 1));
    // The frame and one past it filled: the stack is never filled closer than 8 to its end (`VM::fillStack`).
    a_.lea(R10, mem(R9, std::int32_t(8 * C->nregs + 8)));
    a_.alu(7, R10, mem(R12, kVmStackInit));
    a_.jcc(CA, slow);
    a_.lea(R11, mem(R13, std::int32_t(sizeof(Frame))));
    a_.alu(7, R11, mem(R12, kVmFrameLimit));
    a_.jcc(CAE, slow);
    a_.alu(7, RSP, mem(R12, kVmStackFloor));
    a_.jcc(CB, slow);
    a_.mov32(RDI, mem(R12, kVmTicks));
    a_.aluI32(5, RDI, 1);
    a_.jcc(CBE, slow);
    a_.mov32(mem(R12, kVmTicks), RDI);
    // Caller registers the callee's frame covers past the arguments: a plain one is overwritten, a cell goes the long way.
    const unsigned window = code_->nregs > base + 1 ? code_->nregs - (base + 1) : 0;
    // (A deferred frame has nothing to let go of there. Past the callee's registers, a call it makes may reach them.)
    // (A deferred frame: the parameters there is no argument for. The callee's code sees to its other registers.)
    if (def_)
        for (unsigned r = 1 + argc; r <= C->nparams; ++r)
            a_.mov(mem(R9, std::int32_t(8 * r)), std::int32_t(kUndefined));
    for (unsigned r = 1 + argc; !def_ && r < window; ++r) {
        const std::uint32_t at = frame_[base + 1 + r];
        if (at != ssa::kNone && sval_[at].isConst && sval_[at].bits == kUndefined)
            continue;  // as it is
        if (!def_ && !windowPlain(i, base + 1 + r)) {
            a_.mov(RDI, mem(R9, std::int32_t(8 * r)));
            a_.test(RDI, R15);
            a_.jcc(CE, slow);
        }
        a_.mov(mem(R9, std::int32_t(8 * r)), std::int32_t(kUndefined));
    }
    if (!(C->flags & (CodeBlock::kStrict | CodeBlock::kArrow | CodeBlock::kNoThis))) {
        // A sloppy function called with no `this` gets the global object.
        Label thisOk;
        a_.mov(RDI, R(base + 1));
        a_.aluI(4, RDI, -9);
        a_.cmpI(RDI, std::int32_t(kNull));
        a_.jcc(CNE, thisOk);
        a_.movImm(RDI, reinterpret_cast<std::uint64_t>(vm_.global));
        if (!def_)
            a_.inc32(mem(RDI, kHdrRc));
        a_.mov(R(base + 1), RDI);
        a_.bind(thisOk);
    }
    a_.movImm(RDI, reinterpret_cast<std::uint64_t>(&code_->code[i + 1]));
    a_.mov(mem(R13, kFramePc), RDI);
    static_assert(sizeof(Frame) == 64, "a frame is found by a shift");
    a_.lea(RDI, mem(R11, std::int32_t(sizeof(Frame))));
    a_.mov(mem(R12, kVmFrameTop), RDI);
    a_.mov(mem(R11, kFrameCode), RCX);
    a_.mov(mem(R11, kFrameFn), RAX);
    a_.mov(mem(R11, kFramePc), 0);
    a_.mov(mem(R11, kFrameR), R9);
    a_.movImm(RDI, (std::uint64_t(argc) << 32) | in.a);
    a_.mov(mem(R11, kFrameRetDst), RDI);
    a_.mov(mem(R11, kFrameEntry - 1), std::int32_t(0x100));
    bool reads = false;
    for (const Ins& ci : C->code)
        reads = reads || ci.opcode() == Op::NewTarget || ci.opcode() == Op::SetNewTarget || ci.opcode() == Op::SuperCall;
    if (reads) {
        a_.mov(mem(R11, kFrameNewTarget), std::int32_t(kUndefined));
        a_.mov(mem(R11, kFrameGen), std::int32_t(kUndefined));
    }
    a_.mov(RSI, R11);
    a_.mov(RDI, R9);
    a_.mov(RDX, mem(RDX, kTierEntry));
    a_.callReg(RDX);
    a_.test(RAX, RAX);
    a_.jcc(CE, exc);
    write(in.a, RAX);
}

bool Baseline::ssaArith(std::uint32_t i)
{
    const Ins& in = code_->code[i];
    const Op op = in.opcode();
    const bool compare = op == Op::Lt || op == Op::Le || op == Op::Gt || op == Op::Ge;
    if (!compare && !intArith_[i])
        return false;
    const bool shift = op == Op::Shl || op == Op::Shr || op == Op::UShr;
    const bool rr = op == Op::Add || op == Op::Sub || op == Op::Mul || op == Op::BitAnd || op == Op::BitOr ||
                    op == Op::BitXor || op == Op::Mod || shift || compare;
    const std::uint32_t va = name_[in.b];
    const std::uint32_t vb = rr ? name_[in.c] : ssa::kNone;
    Label& bad = ssaExit(i);
    if (compare) {
        // Into a boolean: rax 0 or 1, then false or true.
        Label done;
        ssaRelation(i, va, vb, 0, false, bad, [&](Cond c, bool) {
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
                         op == Op::BitOrI || op == Op::BitXorI || op == Op::ShlI || op == Op::ShrI || op == Op::UShrI ||
                         op == Op::BitNot || shift;
    // Made in the result's own register when it has one that holds no operand (an exit reads the operands), else in
    // rax; an int32 operand in a register is read where it is.
    auto regOf = [&](std::uint32_t v) {
        return v != ssa::kNone && !sval_[v].isConst && sval_[v].rep == kRepI32 && sval_[v].reg >= 0 ? sval_[v].reg : -1;
    };
    auto anyReg = [&](std::uint32_t v) { return v != ssa::kNone && !sval_[v].isConst ? sval_[v].reg : -1; };
    const std::uint32_t def = ssa_->defBase[i];
    const int rb = regOf(vb);
    // An operation that cannot fail once its operands are read (bitwise, or an add that may wrap) may also write over its
    // first operand's register, which no exit reads after it. (Not one that is converted there: the second may leave yet.)
    const bool noFailAfter = (bitwise && regOf(va) >= 0 && op != Op::UShr && !(op == Op::UShrI && !(in.c & 31))) ||
                             (wrapOk_[def] && (op == Op::Add || op == Op::Sub || op == Op::AddI ||
                                                          op == Op::SubI || op == Op::Inc || op == Op::Dec));
    Reg t = RAX;
    if (sval_[def].rep == kRepI32 && sval_[def].reg >= 0 && sval_[def].reg != anyReg(vb) &&
        (sval_[def].reg != anyReg(va) || noFailAfter))
        t = Reg(sval_[def].reg);
    if (bitwise)  // ToInt32 of each operand: a double truncates
        loadToInt32(va, t, bad);
    else
        loadI32(va, t, &bad);
    Reg b = RCX;
    if (rr) {
        if (rb >= 0 && !shift)
            b = Reg(rb);
        else if (bitwise)
            loadToInt32(vb, RCX, bad);  // (a shift's count is in rcx)
        else
            loadI32(vb, RCX, &bad);
    }
    const std::int32_t imm = op == Op::Inc ? 1 : op == Op::Dec ? -1 : std::int32_t(std::int16_t(in.c));
    const bool wraps = wrapOk_[def] != 0;  // every use truncates: no overflow check
    // `x | 0` and the like: the operand is the result.
    const bool identity = (imm == 0 && (op == Op::AddI || op == Op::SubI || op == Op::BitOrI || op == Op::BitXorI)) ||
                          ((imm & 31) == 0 && (op == Op::ShlI || op == Op::ShrI)) || (imm == -1 && op == Op::BitAndI);
    switch (identity ? Op::Nop : op) {
    case Op::Nop: break;
    case Op::AddI: case Op::Inc: case Op::Dec: a_.aluI32(0, t, imm); if (!wraps) a_.jcc(CO, bad); break;
    case Op::SubI: a_.aluI32(5, t, imm); if (!wraps) a_.jcc(CO, bad); break;
    case Op::BitAndI: a_.aluI32(4, t, imm); break;
    case Op::BitOrI: a_.aluI32(1, t, imm); break;
    case Op::BitXorI: a_.aluI32(6, t, imm); break;
    case Op::ShlI: a_.shiftI32(4, t, imm & 31); break;
    case Op::ShrI: a_.shiftI32(7, t, imm & 31); break;
    case Op::Add: a_.alu32(0, t, b); if (!wraps) a_.jcc(CO, bad); break;
    case Op::Sub: a_.alu32(5, t, b); if (!wraps) a_.jcc(CO, bad); break;
    case Op::BitAnd: a_.alu32(4, t, b); break;
    case Op::BitOr: a_.alu32(1, t, b); break;
    case Op::BitXor: a_.alu32(6, t, b); break;
    case Op::UShrI:
        a_.shiftI32(5, t, imm & 31);
        if ((imm & 31) == 0 && !nonNeg_[va]) {
            a_.test32(t, t);
            a_.jcc(CS, bad);
        }
        break;
    case Op::Shl: a_.shiftCl32(4, t); break;
    case Op::Shr: a_.shiftCl32(7, t); break;
    case Op::UShr:  // a result past int32 is a double
        a_.shiftCl32(5, t);
        a_.test32(t, t);
        a_.jcc(CS, bad);
        break;
    case Op::BitNot: a_.not32(t); break;
    case Op::Neg:
        // Zero is -0 then, and the least int32 has no opposite: the interpreter's.
        a_.test32(t, t);
        a_.jcc(CE, bad);
        a_.aluI32(7, t, INT32_MIN);
        a_.jcc(CE, bad);
        a_.neg32(t);
        break;
    case Op::Mod:
        if (!nonNeg_[va]) {
            a_.test32(t, t);
            a_.jcc(CS, bad);
        }
        a_.aluI32(7, b, 0);
        a_.jcc(CLE, bad);
        if (b != RCX)
            a_.mov32(RCX, b);
        a_.mov32(RAX, t);
        a_.cdq();
        a_.idiv32(RCX);
        a_.mov32(t, RDX);
        break;
    case Op::ModI: {
        // A dividend below zero: the interpreter's (its remainder may be -0). A power of two is a mask.
        if (!nonNeg_[va]) {
            a_.test32(t, t);
            a_.jcc(CS, bad);
        }
        if ((imm & (imm - 1)) == 0) {
            a_.aluI32(4, t, imm - 1);
        } else {
            // x / d for 0 <= x < 2^31 as (x * M) >> (32 + l), M = floor(2^(32+l) / d) + 1, l = ceil(log2 d): no divide.
            const unsigned l = 32u - unsigned(__builtin_clz(std::uint32_t(imm - 1)));
            const std::uint64_t magic = (std::uint64_t(1) << (32 + l)) / std::uint64_t(imm) + 1;
            a_.mov32(RDX, t);  // x, kept (t may be rax)
            a_.mov32(RAX, RDX);
            a_.movImm(RCX, magic);
            a_.mul64(RAX, RCX);
            a_.shiftI(5, RAX, int(32 + l));
            a_.imulI32(RCX, RAX, imm);
            a_.alu32(5, RDX, RCX);
            a_.mov32(t, RDX);
        }
        break;
    }
    case Op::MulI: {
        // An overflow, or a zero that may be a negative zero: the interpreter's.
        a_.imulI32(RDX, t, imm);
        a_.jcc(CO, bad);
        if (!(imm >= 0 && nonNeg_[va])) {
            Label nz;
            a_.test32(RDX, RDX);
            a_.jcc(CNE, nz);
            if (imm < 0) {
                a_.jmp(bad);
            } else {
                a_.test32(t, t);
                a_.jcc(CS, bad);
            }
            a_.bind(nz);
        }
        a_.mov32(t, RDX);
        break;
    }
    case Op::Mul: {
        // An overflow, or a zero that may be a negative zero (an operand below zero): the interpreter's.
        const bool noNegZero = nonNeg_[va] && vb != ssa::kNone && nonNeg_[vb];  // two not below zero: no -0
        if (!noNegZero)
            a_.mov32(RDX, t);
        a_.imul32(t, b);
        a_.jcc(CO, bad);
        if (!noNegZero) {
            Label nz;
            a_.test32(t, t);
            a_.jcc(CNE, nz);
            a_.alu32(1, RDX, b);
            a_.jcc(CS, bad);
            a_.bind(nz);
        }
        break;
    }
    default:
        break;
    }
    if (t != RAX) {
        name_[in.a] = def;
        return true;
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
    if (op == Op::Neg) {
        const std::uint32_t def = ssa_->defBase[i];
        const int x = sval_[def].reg >= 0 && sval_[def].rep == kRepF64 ? sval_[def].reg : 0;
        loadF64(name_[in.b], 1, &bad);
        a_.movImm(RDX, std::uint64_t(1) << 63);
        a_.movqToX(x, RDX);
        a_.xorps(x, 1);
        ssaDefF64(i, x);
        return true;
    }
    // In the result's own register when it has one that is not the second operand's; the second operand where it is.
    const std::uint32_t def = ssa_->defBase[i];
    const std::uint32_t vb = rr ? name_[in.c] : ssa::kNone;
    const bool inReg = rr && !sval_[vb].isConst && sval_[vb].rep == kRepF64 && sval_[vb].reg >= 0;
    int y = inReg ? sval_[vb].reg : 1;
    int x = sval_[def].reg >= 0 && sval_[def].rep == kRepF64 && sval_[def].reg != y ? sval_[def].reg : 0;
    if (inReg && x == 0 && sval_[def].rep == kRepF64 && sval_[def].reg == y &&
        (op == Op::Add || op == Op::Mul)) {
        // The result takes the second operand's register: the order does not matter, so do it there, with no move.
        const std::uint32_t va = name_[in.b];
        const bool aReg = !sval_[va].isConst && sval_[va].rep == kRepF64 && sval_[va].reg >= 0;
        const int z = aReg ? sval_[va].reg : 1;
        if (!aReg)
            loadF64(va, 1, &bad);
        if (op == Op::Add)
            a_.addsd(y, z);
        else
            a_.mulsd(y, z);
        ssaDefF64(i, y);
        return true;
    }
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
    if (elim_[i])
        return true;
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
    case Op::GetProp: return !ssaReadEntries(i).empty() || ssaLengthSite(i) || ssaStringProtoIndex(i) >= 0;
    case Op::New: return ssaNewPlan(i, nullptr);
    case Op::Call: return ssaMathCallee(i) != nullptr || ssaCharCodeCallee(i) != nullptr;
    case Op::PutProp: return ssaPutFast(i) || ssaAddFast(i);
    case Op::InitProp: return ssaAddFast(i);
    case Op::GetGlobal: return sval_[ssa_->defBase[i]].isConst || ssaGlobalFast(i);
    case Op::GetUpval: case Op::GetBox: case Op::PutUpval: case Op::PutBox:
        return true;
    case Op::CheckInit:
        return !failed;
    case Op::ToNumeric: case Op::Not: case Op::TypeOf:
        return !failed;
    case Op::GetElem: case Op::PutElem: {
        if (op == Op::GetElem && ssaPrivateSite(i))
            return !failed;
        ElemKind kind;
        return ssaElemPlan(i, op == Op::PutElem, &kind) != 0;
    }
    case Op::Eq: case Op::Ne: case Op::StrictEq: case Op::StrictNe:
        return equalsNative(name_[in.b], name_[in.c], op == Op::Eq || op == Op::Ne);
    case Op::AddI: case Op::SubI: case Op::Inc: case Op::Dec: case Op::BitAndI: case Op::BitOrI: case Op::BitXorI:
    case Op::ShlI: case Op::ShrI: case Op::Add: case Op::Sub: case Op::Mul: case Op::BitAnd: case Op::BitOr:
    case Op::BitXor: case Op::Div: case Op::MulI: case Op::Mod: case Op::Neg: case Op::BitNot: case Op::Shl: case Op::Shr:
    case Op::UShr: case Op::UShrI:
        return intArith_[i] || dblArith_[i];
    case Op::ModI:  // a positive divisor only
        return intArith_[i] && std::int16_t(in.c) > 0;
    case Op::Lt: case Op::Le: case Op::Gt: case Op::Ge:
        return !failed;
    case Op::JNullish: case Op::JNotNullish: case Op::JUndefined: case Op::JNotUndefined: case Op::JHole:
    case Op::JNotCode: case Op::JNotCallee:
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
    // An arithmetic site that has failed: the function's int32 profiles are not to be trusted (doubles came after them),
    // and each site failing in turn would cost an exit and a compile. Its other adds and products are then decided by
    // their operands (`byOperands` below), as sites with an empty profile are.
    bool arithFailed = false;
    for (std::uint32_t i = 0; i < n_; i = opt::nextOf(*code_, i)) {
        const Op op = code_->code[i].opcode();
        if ((op == Op::Add || op == Op::Sub || op == Op::Mul || op == Op::AddI || op == Op::SubI || op == Op::MulI) &&
            i < code_->prof.size() && (code_->prof[i] & opt::kProfFailed))
            arithFailed = true;
    }
    for (std::uint32_t i = 0; i < n_; i = opt::nextOf(*code_, i)) {
        const Op op = code_->code[i].opcode();
        const std::uint8_t prof = i < code_->prof.size() ? code_->prof[i] : 0;
        if (ssaOn_ < 2 || (prof & opt::kProfFailed))
            continue;
        if (arithFailed && prof == 1 && (op == Op::Add || op == Op::Sub || op == Op::Mul || op == Op::MulI))
            continue;
        switch (op) {
        case Op::AddI: case Op::SubI: case Op::Inc: case Op::Dec: case Op::BitAndI: case Op::BitOrI: case Op::BitXorI:
        case Op::ShlI: case Op::ShrI: case Op::MulI: case Op::Add: case Op::Sub: case Op::Mul: case Op::BitAnd:
        case Op::BitOr: case Op::BitXor: case Op::ModI: case Op::Mod: case Op::Neg: case Op::BitNot: case Op::Shl:
        case Op::Shr: case Op::UShr: case Op::UShrI:
            // A bitwise operation with nothing noted (0: the interpreter has not run it; the baseline code notes
            // nothing): its result is an int32 whatever the operands (a double truncates). A template there would make
            // every value across it live in a slot, in the hot code around it too.
            if (prof == 1 || (prof == 0 && (op == Op::BitAndI || op == Op::BitOrI || op == Op::BitXorI || op == Op::ShlI ||
                                            op == Op::ShrI || op == Op::BitAnd || op == Op::BitOr || op == Op::BitXor ||
                                            op == Op::BitNot || op == Op::Shl || op == Op::Shr ||
                                            (op == Op::UShrI && (code_->code[i].c & 31)))))
                intArith_[i] = 1;
            break;
        default:
            break;
        }
        switch (op) {
        case Op::Add: case Op::Sub: case Op::Mul: case Op::Div: case Op::AddI: case Op::SubI: case Op::MulI: case Op::Neg:
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
            elemRep[i] = kind == ElemKind::Float64 || kind == ElemKind::Float32 || kind == ElemKind::Uint32 ? 2 : 1;
        else if (plan == 1 && prof && !(prof & ~3))
            elemRep[i] = prof == 1 ? 1 : 2;  // a plain array the site found numbers in, only
    }
    // Arithmetic that nothing has noted (an empty profile: the interpreter never ran it, and the baseline code notes
    // nothing): what its operands are decides, below. All int32: int32 arithmetic, an exit on overflow; a double and
    // numbers: double arithmetic; anything else: the template.
    std::vector<std::vector<std::uint32_t>> operandsOf(nv);
    std::vector<std::uint8_t> byOperands(nv, 0);  // 1: an empty profile; 2: an int32 profile (a double operand widens it)
    std::vector<std::pair<std::uint32_t, std::uint32_t>> elemReads;  // (instruction, base value) of int32 array reads
    std::vector<std::uint8_t> baseMadeDoubles(nv, 0);
    if (ssaOn_ >= 2)
        for (std::uint32_t b = 1; b < nb; ++b) {
            const ssa::Block& bl = g.blocks[b];
            if (!bl.reached)
                continue;
            std::vector<std::uint32_t> names = bl.nameIn;
            for (std::uint32_t i = bl.start; i < bl.end; i = opt::nextOf(*code_, i)) {
                const Ins& in = code_->code[i];
                const Op op = in.opcode();
                const opt::Effects e = ssa::effects(*code_, i);
                const bool arith = op == Op::Add || op == Op::Sub || op == Op::Mul || op == Op::AddI || op == Op::SubI ||
                                   op == Op::MulI || op == Op::Inc || op == Op::Dec || op == Op::ModI || op == Op::Mod ||
                                   op == Op::Neg;
                if (op == Op::GetElem && names[in.b] != ssa::kNone) {
                    const std::uint8_t pe = i < code_->prof.size() ? code_->prof[i] : 0;
                    if (elemRep[i] == 1)
                        elemReads.emplace_back(i, names[in.b]);
                    else if ((pe & 2) && !(pe & ~3))
                        baseMadeDoubles[names[in.b]] = 1;
                }
                const std::uint8_t pf = i < code_->prof.size() ? code_->prof[i] : 0;
                const bool distrusted = arithFailed && pf == 1 &&
                                        (op == Op::Add || op == Op::Sub || op == Op::Mul || op == Op::MulI);
                if (arith && (pf == 0 || distrusted) && g.defBase[i] != ssa::kNone && !intArith_[i] && !dblArith_[i]) {
                    const std::uint32_t d = g.defBase[i];
                    byOperands[d] = 1;
                    for (std::uint16_t r : e.rd)
                        operandsOf[d].push_back(names[r]);
                } else if (arith && intArith_[i] && g.defBase[i] != ssa::kNone && op != Op::Inc && op != Op::Dec) {
                    // Profiled int32: a double among its operands (an element read that has made doubles since) makes it
                    // double arithmetic, which an int32 operand takes too; else each such site leaves in turn.
                    const std::uint32_t d = g.defBase[i];
                    byOperands[d] = 2;
                    for (std::uint16_t r : e.rd)
                        operandsOf[d].push_back(names[r]);
                }
                if (op == Op::Mov) {
                    names[in.a] = names[in.b];
                    continue;
                }
                for (std::size_t w = 0; w < e.wr.size(); ++w)
                    names[e.wr[w]] = g.defBase[i] + std::uint32_t(w);
            }
        }
    // A read of an array another read has found doubles in: a double too (each site learning it in turn would leave).
    for (const auto& [i, base] : elemReads)
        if (baseMadeDoubles[base])
            elemRep[i] = 2;
    // Representations: an instruction's by what it does; a phi's the least that holds every input.
    std::vector<std::uint8_t> cls(nv, 0);  // 0 none yet, 1 int32, 2 double, 3 boxed
    for (std::uint32_t v = 0; v < nv; ++v) {
        const ssa::Value& sv = g.values[v];
        if (sval_[v].isConst) {
            const JSValue k = JSValue::raw(sval_[v].bits);
            cls[v] = k.isInt32() ? 1 : k.isDouble() ? 2 : 3;
        } else if (byOperands[v] == 1) {
            cls[v] = 0;
        } else if (sv.kind == ssa::Value::Def) {
            cls[v] = intArith_[sv.at] ? 1 : dblArith_[sv.at] ? 2 : elemRep[sv.at];
            if (ssaOn_ >= 2 && cls[v] == 3 && (ssaLengthSite(sv.at) || ssaCharCodeCallee(sv.at)))
                cls[v] = 1;  // an int32 when it does not leave
        } else if (sv.kind == ssa::Value::Entry) {
            cls[v] = 3;
        }
    }
    // A loop head whose instruction has not failed may check what comes in from before the loop (`loopGuards_`).
    auto guarded = [&](std::uint32_t b) {
        const ssa::Block& bl = g.blocks[b];
        return ssaOn_ >= 2 && bl.loopHeader && !(bl.start < code_->prof.size() && (code_->prof[bl.start] & opt::kProfFailed));
    };
    // Whether forward input `in` of a phi can be checked to be an int32 on its way in (a boxed value that is not a constant).
    auto checkable = [&](std::uint32_t in) { return !sval_[in].isConst && (cls[in] == 0 || cls[in] == 1 || cls[in] == 3); };
    auto doubleOp = [](Op op) {
        return op == Op::Add || op == Op::Sub || op == Op::Mul || op == Op::AddI || op == Op::SubI || op == Op::MulI || op == Op::Neg;
    };
    for (bool changed = true; changed;) {
        changed = false;
        for (std::uint32_t v = 0; v < nv; ++v) {
            if (!byOperands[v])
                continue;
            std::uint8_t m = 0;
            for (std::uint32_t o : operandsOf[v]) {
                std::uint8_t oc = o == ssa::kNone ? std::uint8_t(3) : cls[o];
                if (byOperands[v] == 2 && oc != 2)
                    oc = 1;  // profiled int32: a boxed operand is taken as the profile says
                m = std::max(m, oc);
            }
            std::uint8_t c = std::max(cls[v], m);
            // Doubles only where the double path has the operation (not `ModI`, `Inc`, `Dec`): else the template.
            if (c == 2 && !doubleOp(code_->code[g.values[v].at].opcode()))
                c = 3;
            if (c != cls[v]) {
                cls[v] = c;
                changed = true;
            }
        }
        for (std::uint32_t b = 1; b < nb; ++b)
            for (std::uint32_t p : g.blocks[b].phis) {
                const ssa::Value& pv = g.values[p];
                std::uint8_t c = cls[p], back = 0;
                bool check = guarded(b);
                for (std::size_t k = 0; k < pv.in.size(); ++k) {
                    const std::uint32_t in = pv.in[k];
                    if (in == p)
                        continue;
                    if (g.blocks[b].preds[k] >= b)
                        back = std::max(back, cls[in]);
                    else
                        check = check && checkable(in);
                }
                // A loop's int32 that comes in boxed: an int32 phi, the input checked on its way in.
                if (check && back == 1)
                    c = std::max(c, back);
                else
                    for (std::uint32_t in : pv.in)
                        if (in != p)
                            c = std::max(c, cls[in]);
                if (c != cls[p]) {
                    cls[p] = c;
                    changed = true;
                }
            }
    }
    for (std::uint32_t v = 0; v < nv; ++v)
        if (byOperands[v]) {
            const std::uint32_t at = g.values[v].at;
            intArith_[at] = cls[v] == 1 || cls[v] == 0;
            dblArith_[at] = cls[v] == 2;
            if (cls[v] == 0)
                cls[v] = 1;
        }
    loopGuards_.assign(nb, {});
    for (std::uint32_t b = 1; b < nb; ++b) {
        if (!g.blocks[b].reached || !guarded(b))
            continue;
        const ssa::Block& bl = g.blocks[b];
        for (std::uint32_t p : bl.phis) {
            if (cls[p] != 1)
                continue;
            for (std::size_t k = 0; k < bl.preds.size(); ++k)
                if (bl.preds[k] < b && g.values[p].in[k] != p && cls[g.values[p].in[k]] != 1) {
                    loopGuards_[b].ints.push_back(p);
                    break;
                }
        }
        // A parameter the loop reads and does not write, whose profile has seen int32s only: checked once (a loop bound).
        for (unsigned r = 1; r <= code_->nparams && r < code_->nregs; ++r) {
            const std::uint32_t v = bl.nameIn[r];
            const std::size_t pp = n_ + (r - 1);
            if (v == r && !ssa::bitTest(g.loopWrites[b], r) && ssa::bitTest(bl.liveIn, r) && pp < code_->prof.size() &&
                code_->prof[pp] == 1 && sval_[v].rep == kRepTagged)
                loopGuards_[b].intParams.push_back(v);
        }
        // Element reads and writes in place, of a value the loop does not write: its kind checked once.
        for (std::uint32_t i = bl.start; i < bl.loopEnd; i = opt::nextOf(*code_, i)) {
            const Op op = code_->code[i].opcode();
            if (op != Op::GetElem && op != Op::PutElem)
                continue;
            ElemKind kind;
            const int plan = ssaElemPlan(i, op == Op::PutElem, &kind);
            const unsigned r = op == Op::GetElem ? code_->code[i].b : code_->code[i].a;
            const std::uint32_t v = bl.nameIn[r];
            if (!plan || v == ssa::kNone || ssa::bitTest(g.loopWrites[b], r) || sval_[v].isConst)
                continue;
            std::vector<std::uint32_t>& list = plan == 1 ? loopGuards_[b].arrays : loopGuards_[b].typed;
            if (std::find(list.begin(), list.end(), v) == list.end())
                list.push_back(v);
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
    // An int32 add or subtract whose every use truncates to int32 (a bitwise operation, or another such add) needs no
    // overflow check: ToInt32 of the exact sum is the wrapped one. A product is not exact past 2^53: not taken in.
    {
        auto isWrapOp = [](Op op) {
            return op == Op::Add || op == Op::Sub || op == Op::AddI || op == Op::SubI || op == Op::Inc || op == Op::Dec;
        };
        auto truncates = [](Op op) {
            switch (op) {
            case Op::BitAnd: case Op::BitOr: case Op::BitXor: case Op::Shl: case Op::Shr: case Op::UShr: case Op::BitAndI:
            case Op::BitOrI: case Op::BitXorI: case Op::ShlI: case Op::ShrI: case Op::UShrI:
                return true;
            default:
                return false;
            }
        };
        wrapOk_.assign(nv, 0);
        std::vector<std::uint8_t> badUse(nv, 0);
        std::vector<std::vector<std::uint32_t>> wrapUsers(nv);  // candidate defs that use the value
        for (std::uint32_t v = 0; v < nv; ++v) {
            const ssa::Value& sv = g.values[v];
            if (sv.kind == ssa::Value::Def && !sval_[v].isConst && sval_[v].rep == kRepI32 && intArith_[sv.at] &&
                isWrapOp(code_->code[sv.at].opcode()))
                wrapOk_[v] = 1;
            if (sv.kind == ssa::Value::Phi)
                for (std::uint32_t in : sv.in)
                    if (in != ssa::kNone)
                        badUse[in] = 1;
        }
        for (std::uint32_t b = 1; b < nb; ++b) {
            const ssa::Block& bl = g.blocks[b];
            if (!bl.reached)
                continue;
            std::vector<std::uint32_t> names = bl.nameIn;
            for (std::uint32_t i = bl.start; i < bl.end; i = opt::nextOf(*code_, i)) {
                const Ins& in = code_->code[i];
                const opt::Effects e = ssa::effects(*code_, i);
                const Op op = in.opcode();
                // The value a store into an integer typed array takes is truncated by the store.
                ElemKind kind = ElemKind::Float64;
                const bool intStore = op == Op::PutElem && ssaElemPlan(i, true, &kind) == 2 && kind != ElemKind::Float32 &&
                                      kind != ElemKind::Float64;
                for (std::uint16_t r : e.rd) {
                    const std::uint32_t v = names[r];
                    if (v == ssa::kNone)
                        continue;
                    if (truncates(op) || (intStore && r == in.c && in.a != in.c && in.b != in.c))
                        continue;
                    if (isWrapOp(op) && g.defBase[i] != ssa::kNone && wrapOk_[g.defBase[i]])
                        wrapUsers[v].push_back(g.defBase[i]);
                    else
                        badUse[v] = 1;
                }
                if (op == Op::Mov) {
                    names[in.a] = names[in.b];
                    continue;
                }
                for (std::size_t w = 0; w < e.wr.size(); ++w)
                    names[e.wr[w]] = g.defBase[i] + std::uint32_t(w);
            }
        }
        for (std::uint32_t v = 0; v < nv; ++v)
            if (badUse[v])
                wrapOk_[v] = 0;
        for (bool changed = true; changed;) {
            changed = false;
            for (std::uint32_t v = 0; v < nv; ++v)
                if (wrapOk_[v])
                    for (std::uint32_t u : wrapUsers[v])
                        if (!wrapOk_[u]) {
                            wrapOk_[v] = 0;
                            changed = true;
                            break;
                        }
        }
    }
    // Int32 values that are never below zero: a fixpoint from the most hopeful guess (every phi), as long as each
    // input of a phi and each operand that matters is one too. Overflow leaves, so a sum of two stays one.
    {
        nonNeg_.assign(nv, 0);
        std::vector<std::vector<std::uint32_t>> ops(nv);
        std::vector<std::uint8_t> kindOf(nv, 0);  // 1 always, 2 when every operand is, 3 when one operand is (`&`)
        for (std::uint32_t v = 0; v < nv; ++v)
            if (sval_[v].isConst) {
                const JSValue k = JSValue::raw(sval_[v].bits);
                nonNeg_[v] = k.isInt32() && k.asInt32() >= 0;
            } else if (g.values[v].kind == ssa::Value::Phi && sval_[v].rep == kRepI32) {
                nonNeg_[v] = 1;
                kindOf[v] = 2;
                for (std::uint32_t in : g.values[v].in)
                    if (in != v)
                        ops[v].push_back(in);
            }
        for (std::uint32_t b = 1; b < nb; ++b) {
            const ssa::Block& bl = g.blocks[b];
            if (!bl.reached)
                continue;
            std::vector<std::uint32_t> names = bl.nameIn;
            for (std::uint32_t i = bl.start; i < bl.end; i = opt::nextOf(*code_, i)) {
                const Ins& in = code_->code[i];
                const Op op = in.opcode();
                const opt::Effects e = ssa::effects(*code_, i);
                const std::uint32_t d = g.defBase[i];
                if (d != ssa::kNone && sval_[d].rep == kRepI32) {
                    const std::int32_t imm = std::int32_t(std::int16_t(in.c));
                    std::uint8_t k = 0;
                    if (op == Op::ModI || op == Op::Mod || op == Op::UShrI || op == Op::UShr || ssaLengthSite(i) || (op == Op::BitAndI && imm >= 0))
                        k = 1;
                    else if (((op == Op::Inc || (op == Op::AddI && imm >= 0) || op == Op::Add) && !wrapOk_[d]) ||
                             (op == Op::MulI && imm >= 0) || (op == Op::ShrI && (imm & 31) != 0) || op == Op::Mul)
                        k = 2;  // (an add that may wrap can turn negative)
                    else if (op == Op::BitAnd)
                        k = 3;
                    if (k) {
                        kindOf[d] = k;
                        nonNeg_[d] = 1;
                        for (std::uint16_t r : e.rd)
                            ops[d].push_back(names[r]);
                    }
                }
                if (op == Op::Mov) {
                    names[in.a] = names[in.b];
                    continue;
                }
                for (std::size_t w = 0; w < e.wr.size(); ++w)
                    names[e.wr[w]] = g.defBase[i] + std::uint32_t(w);
            }
        }
        for (bool changed = true; changed;) {
            changed = false;
            for (std::uint32_t v = 0; v < nv; ++v) {
                if (!nonNeg_[v] || kindOf[v] < 2)
                    continue;
                bool all = true, any = false;
                for (std::uint32_t o : ops[v]) {
                    const bool nn = o != ssa::kNone && nonNeg_[o];
                    all = all && nn;
                    any = any || nn;
                }
                if (kindOf[v] == 2 ? !all : !any) {
                    nonNeg_[v] = 0;
                    changed = true;
                }
            }
        }
        // Not run (no code, no value): not trusted.
        for (std::uint32_t v = 0; v < nv; ++v)
            if (g.values[v].kind == ssa::Value::Entry)
                nonNeg_[v] = 0;
    }
    // Registers whose slots never hold a cell: above the parameters, every value of them a number or a constant that is
    // no cell, and no call's argument (a callee may assign its parameters).
    neverCell_.assign(code_->nregs, 0);
    for (unsigned r = firstLocal(); r < code_->nregs; ++r)
        neverCell_[r] = 1;
    for (std::uint32_t v = g.nregs; v < nv; ++v) {
        const unsigned r = g.values[v].reg;
        if (r >= code_->nregs || !neverCell_[r])
            continue;
        const SVal& sv = sval_[v];
        if (sv.isConst ? JSValue::raw(sv.bits).isCell() : sv.rep == kRepTagged)
            neverCell_[r] = 0;
    }
    for (std::uint32_t i = 0; i < n_; i = opt::nextOf(*code_, i)) {
        const Ins& in = code_->code[i];
        const Op op = in.opcode();
        if (op == Op::Call || op == Op::New || op == Op::SuperCall || op == Op::CallSpread || op == Op::NewSpread)
            for (unsigned r = in.b + 1u; r <= in.b + 1u + in.c && r < code_->nregs; ++r)
                neverCell_[r] = 0;
        const opt::Effects e = ssa::effects(*code_, i);
        if (!e.ok)
            std::fill(neverCell_.begin(), neverCell_.end(), std::uint8_t(0));
    }
#if defined(SCRIPT_JIT_ENV_OVERRIDE)
    if (std::getenv("SSA_REPS"))
        for (std::uint32_t v = g.nregs; v < nv; ++v)
            if (!sval_[v].isConst && sval_[v].rep != kRepTagged) {
                const ssa::Value& sv = g.values[v];
                std::fprintf(stderr, "v%u r%u %s @%u %s\n", v, unsigned(sv.reg), sv.kind == ssa::Value::Phi ? "phi" : "def",
                             unsigned(sv.at), sval_[v].rep == kRepI32 ? "i32" : "f64");
            }
#endif
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
    usePos_.assign(n_, 0);
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
            usePos_[i] = use;
            const opt::Effects e = ssa::effects(*code_, i);
            for (std::uint16_t r : g.held(i))
                touch(name_[r], use);
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
    // A global read in a loop that clobbers nothing and writes no global, its register written by nothing else there:
    // read once on the way in (the outermost such loop), its value alive in all of the loop.
    ssaHoisted_.assign(n_, 0);
    for (std::uint32_t h = 1; h < nb && ssaOn_ >= 2; ++h) {
        const ssa::Block& hb = g.blocks[h];
        if (!hb.reached || !hb.loopHeader || !guarded(h))
            continue;
        std::uint32_t lastB = h;
        for (std::uint32_t b = h + 1; b < nb && g.blocks[b].start < hb.loopEnd; ++b)
            if (g.blocks[b].reached)
                lastB = b;
        bool clean = true;
        std::vector<unsigned> writers(code_->nregs, 0);
        for (std::uint32_t i = hb.start; i < hb.loopEnd && clean; i = opt::nextOf(*code_, i)) {
            if (!native_[i] || code_->code[i].opcode() == Op::PutGlobal)
                clean = false;
            const opt::Effects e = ssa::effects(*code_, i);
            for (std::uint16_t w : e.wr)
                if (w < code_->nregs)
                    ++writers[w];
        }
        if (!clean)
            continue;
        for (std::uint32_t i = hb.start; i < hb.loopEnd; i = opt::nextOf(*code_, i)) {
            const Ins& in = code_->code[i];
            if (in.opcode() != Op::GetGlobal || ssaHoisted_[i] || writers[in.a] != 1 || g.defBase[i] == ssa::kNone ||
                sval_[g.defBase[i]].isConst)
                continue;
            const std::uint32_t v = g.defBase[i];
            ssaHoisted_[i] = 1;
            loopGuards_[h].globals.push_back(i);
            first[v] = std::min(first[v], blockPos[h].first);
            last[v] = std::max(last[v], blockPos[lastB].second);
            // Elements read or written of it in place: an array, checked once.
            for (std::uint32_t j = hb.start; j < hb.loopEnd; j = opt::nextOf(*code_, j)) {
                const Op op = code_->code[j].opcode();
                ElemKind kind;
                if ((op == Op::GetElem || op == Op::PutElem) &&
                    (op == Op::GetElem ? code_->code[j].b : code_->code[j].a) == in.a &&
                    ssaElemPlan(j, op == Op::PutElem, &kind) == 1 &&
                    std::find(loopGuards_[h].arrays.begin(), loopGuards_[h].arrays.end(), v) == loopGuards_[h].arrays.end())
                    loopGuards_[h].arrays.push_back(v);
            }
        }
    }
    // Linear scan, one interval per value; a value across a clobber lives in a slot.
    std::vector<std::uint32_t> order;
    for (std::uint32_t v = 0; v < nv; ++v)
        if (last[v] >= 0)
            order.push_back(v);
    std::sort(order.begin(), order.end(), [&](std::uint32_t x, std::uint32_t y) { return first[x] < first[y]; });
    std::vector<int> gprs, xmms;
#if defined(SCRIPT_JIT_ENV_OVERRIDE)
    static const bool noRegs = std::getenv("SSA_NOREGS") != nullptr;
    static const bool noEvict = std::getenv("SSA_NOEVICT") != nullptr;
#else
    static const bool noRegs = false, noEvict = false;
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
        std::uint32_t v;
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
            active.push_back(Active{last[v], sval_[v].reg, x, v});
        } else if (!cross && !noRegs && !noEvict) {
            // No register free: the value of this kind that is needed longest gives up its own, when that is longer than
            // this one is (a short value in a slot costs a store and a load; a long one, a load where it is read).
            std::size_t far = active.size();
            for (std::size_t a = 0; a < active.size(); ++a)
                if (active[a].x == x && active[a].end > last[v] && (far == active.size() || active[a].end > active[far].end))
                    far = a;
            if (far < active.size()) {
                const std::uint32_t w = active[far].v;
                sval_[v].reg = std::int8_t(active[far].reg);
                sval_[w].reg = -1;
                sval_[w].slot = slots++;
                active[far] = Active{last[v], sval_[v].reg, x, v};
            } else {
                sval_[v].slot = slots++;
            }
        } else {
            sval_[v].slot = slots++;
        }
    }
    firstPos_ = first;
    lastPos_ = last;
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
    // A value kept for a later read of what it was read from (`ssaLoadElim`) is in no register's name meanwhile.
    for (std::uint32_t v = 0; v < firstPos_.size(); ++v)
        if (sval_[v].reg >= 0 && !sval_[v].isConst && firstPos_[v] <= usePos_[curIns_] && lastPos_[v] > usePos_[curIns_])
            out.push_back(sval_[v].rep == kRepF64 ? 100 + sval_[v].reg : sval_[v].reg);
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
    if (sval_[ssa_->defBase[i]].isConst) {
        ssaEpochCheck(i);
        name_[in.a] = ssa_->defBase[i];
        return true;
    }
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
    if (ssaHoisted_[i]) {
        name_[in.a] = ssa_->defBase[i];  // read on the way into the loop
        return true;
    }
    (void)index;
    Label& bad = ssaExit(i);
    ssaGlobalRead(i, bad);
    ssaDef(i, 0);
    return true;
}

void Baseline::ssaGlobalRead(std::uint32_t i, Label& bad)
{
    JSObject* g = vm_.global;
    std::uint8_t attrs = 0;
    JSValue* slot = g->findOwn(vm_, code_->atoms[code_->code[i].b], &attrs);
    JSValue* inlineBase = g->inl();
    const std::uint32_t index = slot >= inlineBase && slot < inlineBase + g->h.aux ? std::uint32_t(slot - inlineBase)
                                                                                 : std::uint32_t(slot - g->slots) + g->h.aux;
    ssaEpochSite(bad);
    globalSlot(index);
    a_.mov(RAX, mem(RDX, 0));
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
    if (loose && !ssa_->held(i).empty()) {
        std::vector<unsigned> regs;
        ssaAddHeld(i, regs);
        ssaSync(std::move(regs), liveBefore_, std::vector<std::uint8_t>(code_->nregs, 0));
        if (ssaFailed_)
            return true;
    }
    Label same, differ, done;
    const bool ka = intConst(va), kb = intConst(vb);
    if (ka || kb) {
        ssaEqualsInt(i, valueReg(ka ? vb : va), JSValue::raw(sval_[ka ? va : vb].bits).asInt32(), loose, bad, same, differ);
    } else {
        loadValue(va, RAX);
        loadValue(vb, RCX);
        ssaEqualsCore(i, loose, bad, same, differ);
    }
    a_.bind(same);
    a_.movImm(RAX, 1);
    a_.jmp(done);
    a_.bind(differ);
    a_.movImm(RAX, 0);
    a_.bind(done);
    a_.test32(RAX, RAX);
    finish(eq ? CNE : CE);
    return true;
}

/// A return: the result in its slot, then the baseline's way out, told which slots may hold something to clear.
void Baseline::ssaRet(std::uint32_t i)
{
    const Ins& in = code_->code[i];
    const unsigned nregs = code_->nregs;
    // The result into rax with one count for the caller: the count of a slot this frame clears on its way out that holds
    // it (the slot then holds undefined), else a new one.
    const std::uint32_t v = name_[in.a];
    if (ssaCtor_ && frame_[0] != name_[0]) {
        // What a construction gives back when the result is no object.
        loadValue(name_[0], RAX);
        a_.mov(R(0), RAX);
        frame_[0] = name_[0];
    }
    loadValue(v, RAX);
    int movedFrom = -1;
    if (mayBeCell(v) && !def_) {  // a deferred frame returns its result without a count
        for (unsigned r = firstLocal(); r < nregs && movedFrom < 0; ++r)
            if (frame_[r] == v) {
                a_.mov(R(r), std::int32_t(kUndefined));
                movedFrom = int(r);
            }
        if (movedFrom < 0)
            retain(RAX);
    }
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
    if (movedFrom >= 0)
        ssaPlain_[unsigned(movedFrom)] = 1;  // undefined now
    for (unsigned r = 0; r < nregs; ++r)
        ssaPlain_[r] = ssaPlain_[r] || neverCell_[r];
#if defined(SCRIPT_JIT_ENV_OVERRIDE)
    if (std::getenv("RETDBG")) {
        unsigned nd = 0, np = 0, unk = 0;
        for (unsigned r = 1u + code_->nparams; r < nregs; ++r) {
            nd += (dirty[r / 64] >> (r % 64)) & 1;
            np += ssaPlain_[r];
            unk += frame_[r] == ssa::kNone;
        }
        std::fprintf(stderr, "ret @%u nregs %u dirty %u plain %u unknown %u\n", i, nregs, nd, np, unk);
    }
#endif
    ret(i, in, true, &dirty);
    ssaPlain_.clear();
}

/// How element instruction `i` is done in place: 1 on a plain array, 2 on a typed array of `kind`, 0 not.
int Baseline::ssaElemPlan(std::uint32_t i, bool put, ElemKind* kind)
{
    if (ssaOn_ < 2 || (i < code_->prof.size() && (code_->prof[i] & opt::kProfFailed)) || (!put && ssaPrivateSite(i)))
        return 0;
    const std::uint8_t ep = i < code_->elemProf.size() ? code_->elemProf[i] : 0;
    // Nothing noted: tier 1 reads a plain array in place without a note (only its slow path notes), as tier 2 knows.
    if (ep == elemprof::kArray || ep == 0)
        return 1;
    if (typedSiteKind(i, put, kind)) {
        switch (*kind) {
        case ElemKind::Int8: case ElemKind::Uint8: case ElemKind::Int16: case ElemKind::Uint16: case ElemKind::Int32:
        case ElemKind::Uint32: case ElemKind::Float32: case ElemKind::Float64:
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
        // A boxed result: a hole, or past the end, of an array whose prototypes have no elements is undefined, here.
        const std::uint32_t def = ssa_->defBase[i];
        Label hole;
        Label& missing = sval_[def].rep == kRepTagged ? hole : cold;
        a_.alu32(7, RCX, mem(RAX, kArrayLength));  // unsigned: below zero is past the end
        a_.jcc(CAE, missing);
        a_.mov(RDX, mem(RAX, kArrayElems));
        a_.mov(RAX, mem(RDX, RCX, 8));
        a_.cmpI(RAX, std::int32_t(kHole));
        a_.jcc(CE, missing);
        if (sval_[def].rep == kRepTagged) {
            ssaDef(i, 0);
            a_.jmp(join);
            a_.bind(hole);
            loadValue(vb, RAX);
            if (plainHoleGuard(RDX, cold)) {
                a_.movImm(RAX, kUndefined);
                ssaDef(i, 0);
            } else {
                a_.jmp(cold);
            }
        } else if (sval_[def].rep == kRepI32) {
            needInt(RAX, bad);
            ssaDefI32(i);
        } else if (sval_[def].rep == kRepF64) {
            needNumber(RAX, bad);
            const int x = sval_[def].reg >= 0 ? sval_[def].reg : 0;
            loadNumber(x, RAX, RDX);
            ssaDefF64(i, x);
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
    if (const JSTypedArray* t = ssaKnownTyped(vb, kind)) {
        // The array of a global that stays as it is: where its elements are, and how many, is in the code.
        ssaEpochCheck(i);
        a_.aluI32(7, RCX, std::int32_t(t->length));
        a_.jcc(CAE, bad);
        a_.movImm(RSI, reinterpret_cast<std::uint64_t>(t->data));
    } else {
        loadValue(vb, RAX);
        ssaNeedArray(vb, RAX, ObjClass::TypedArray, bad);
        a_.cmpI8(mem(RAX, kTypedKind), std::uint8_t(kind));
        a_.jcc(CNE, bad);
        a_.alu32(7, RCX, mem(RAX, kTypedLength));
        a_.jcc(CAE, bad);
        a_.mov(RSI, mem(RAX, kTypedData));
    }
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
    case ElemKind::Uint32:  // past int32 it is a double, so always a double
        a_.mov32(RAX, mem(RSI, RCX, 4));
        a_.cvtsi2sd64(0, RAX);
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
        if (const JSTypedArray* t = ssaKnownTyped(vb, kind)) {
            ssaEpochCheck(i);
            a_.aluI32(7, RCX, std::int32_t(t->length));
            a_.jcc(CAE, bad);
            a_.movImm(RSI, reinterpret_cast<std::uint64_t>(t->data));
        } else {
            loadValue(vb, RAX);
            ssaNeedArray(vb, RAX, ObjClass::TypedArray, bad);
            a_.cmpI8(mem(RAX, kTypedKind), std::uint8_t(kind));
            a_.jcc(CNE, bad);
            a_.alu32(7, RCX, mem(RAX, kTypedLength));
            a_.jcc(CAE, bad);
            a_.mov(RSI, mem(RAX, kTypedData));
        }
        switch (kind) {
        case ElemKind::Float64: a_.movsdStore(mem(RSI, RCX, 8), 0); break;
        case ElemKind::Float32: a_.cvtsd2ss(0, 0); a_.movssStore(mem(RSI, RCX, 4), 0); break;
        case ElemKind::Int32: case ElemKind::Uint32: a_.mov32(mem(RSI, RCX, 4), RDI); break;
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
    Label append;
    a_.alu32(7, RCX, mem(RAX, kArrayLength));
    a_.jcc(CAE, append);
    a_.mov(RDX, mem(RAX, kArrayElems));
    a_.lea(RDX, mem(RDX, RCX, 8));
    // A hole of a plain array whose prototypes nothing can intercept a store with (`plainHoleGuard`): filled here.
    Label hole;
    Label overNumber;
    if (number) {
        // Below `packed` every element is a number: stored without a look at the old one (no load to wait for).
        a_.alu32(7, RCX, mem(RAX, kArrayPacked));
        a_.jcc(CB, overNumber);
        // A number over a number: nothing to count or let go of.
        a_.mov(RDI, mem(RDX, 0));
        a_.cmpI(RDI, std::int32_t(kHole));
        a_.jcc(CE, hole);
        a_.test(RDI, R14);
        a_.jcc(CE, bad);
    } else {
        a_.cmpI(mem(RDX, 0), std::int32_t(kHole));
        a_.jcc(CE, hole);
    }
    loadValue(vx, RSI);
    auto fillHole = [&]() {
        // Below `packed` there is no hole: the index is at or above it.
        a_.bind(hole);
        if (!plainHoleGuard(RDI, bad)) {
            a_.jmp(bad);
            return;
        }
        loadValue(vx, RSI);
        if (number) {
            packAt(RAX, RCX);
        } else {
            retain(RSI);
            Label isNumber, stored;
            a_.test(RSI, R14);
            a_.jcc(CNE, isNumber);
            a_.andI8(mem(RAX, kHdrFlags), std::uint8_t(~cellflags::kAllNumbers));
            a_.jmp(stored);
            a_.bind(isNumber);
            packAt(RAX, RCX);
            a_.bind(stored);
        }
        a_.mov(mem(RDX, 0), RSI);
    };
    auto appendHere = [&]() {
        a_.jmp(join);
        a_.bind(append);
        loadValue(vx, RSI);
        appendAt(number, bad);
    };
    if (number) {
        // A number over a number: `packed` (a lower bound) stays right as it is.
        a_.bind(overNumber);
        loadValue(vx, RSI);
        a_.mov(mem(RDX, 0), RSI);
        a_.jmp(join);
        fillHole();
        appendHere();
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
    a_.jmp(join);
    fillHole();
    appendHere();
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
            Label number;
            a_.bind(dbl);
            a_.test(dst, R14);
            a_.jcc(CNE, number);
            boolToInt32(dst, dst, bad);
            a_.jmp(done);
            a_.bind(number);
            a_.mov(RDX, dst);
            a_.sub(RDX, RBP);
            a_.movqToX(0, RDX);
        }
        if (Asm::hasToInt32()) {
            a_.toInt32(dst, 0);
            a_.bind(done);
            a_.mov32(dst, dst);
            return;
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
        then(rel.dc, true);
    };
    if (aI && bI) {
        const Reg ra = i32Reg(va, RAX);
        if (isImm)
            a_.aluI32(7, ra, imm);
        else if (sval_[vb].isConst)
            a_.aluI32(7, ra, JSValue::raw(sval_[vb].bits).asInt32());
        else
            a_.alu32(7, ra, i32Reg(vb, RCX));
        then(rel.ic, true);
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
        const Reg o = valueReg(other);
        a_.cmp(o, R14);
        a_.jcc(CB, dbl);
        const std::uint32_t known = aI ? va : vb;
        if (bI && sval_[vb].isConst) {
            a_.aluI32(7, o, JSValue::raw(sval_[vb].bits).asInt32());
        } else {
            const Reg k = i32Reg(known, RCX);
            a_.alu32(7, aI ? k : o, aI ? o : k);
        }
        then(rel.ic, false);
        a_.bind(dbl);
        frame_ = st;
        doubles();
        return;
    }
    const Reg ra = valueReg(va);
    if (isImm) {
        a_.cmp(ra, R14);
        a_.jcc(CB, dbl);
        a_.aluI32(7, ra, imm);
    } else {
        loadValue(vb, RCX);
        a_.mov(RDX, ra);
        a_.and_(RDX, RCX);
        a_.cmp(RDX, R14);
        a_.jcc(CB, dbl);
        a_.alu32(7, ra, RCX);
    }
    then(rel.ic, false);
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
        boxWrite(RAX);
        epochSeen_ = false;
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
/// `x` (a value's register, or rax) against the int32 constant `k`: to `eq` or `ne`; the general way only for what is no number
/// in a loose comparison.
void Baseline::ssaEqualsInt(std::uint32_t i, Reg x, std::int32_t k, bool loose, Label& bad, Label& eq, Label& ne)
{
    Label notInt;
    a_.cmp(x, R14);
    a_.jcc(CB, notInt);
    a_.aluI32(7, x, k);
    a_.jcc(CE, eq);
    a_.jmp(ne);
    a_.bind(notInt);
    if (loose) {
        // Null and undefined equal no number.
        a_.mov(RDX, x);
        a_.aluI(4, RDX, -9);
        a_.cmpI(RDX, std::int32_t(kNull));
        a_.jcc(CE, ne);
        if (x != RAX)
            a_.mov(RAX, x);
        a_.movImm(RCX, kNumberTag | std::uint32_t(k));
        ssaEqualsCore(i, true, bad, eq, ne);
        return;
    }
    a_.test(x, R14);
    a_.jcc(CE, ne);  // no number
    // A double: kept unboxed as `needNumber` would.
    loadNumber(0, x, RDX);
    a_.movImm(RDX, std::uint32_t(k));
    a_.cvtsi2sd32(1, RDX);
    a_.ucomisd(0, 1);
    Label nan;
    a_.jcc(CP, nan);
    a_.jcc(CE, eq);
    a_.bind(nan);
    a_.jmp(ne);
}

void Baseline::ssaEqualsCore(std::uint32_t i, bool loose, Label& bad, Label& eq, Label& ne)
{
    Label slow, differ, notCellA;
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
        // An object equals no other object and neither null nor undefined; a primitive has it converted.
        Label objectA;
        a_.cmpI8(mem(RAX, kHdrKind), kObjectKind);
        a_.jcc(CE, objectA);
        a_.test(RCX, R15);
        a_.jcc(CNE, slow);
        a_.cmpI8(mem(RCX, kHdrKind), kObjectKind);
        a_.jcc(CE, bad);
        a_.jmp(slow);
        a_.bind(objectA);
        Label cellB;
        a_.test(RCX, R15);
        a_.jcc(CE, cellB);
        a_.mov(RDX, RCX);
        a_.aluI(4, RDX, -9);
        a_.cmpI(RDX, std::int32_t(kNull));
        a_.jcc(CE, ne);
        a_.jmp(bad);
        a_.bind(cellB);
        a_.cmpI8(mem(RCX, kHdrKind), kObjectKind);
        a_.jcc(CE, ne);
        a_.jmp(bad);
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
        a_.test(RCX, R15);
        a_.jcc(CNE, slow);
        a_.cmpI8(mem(RCX, kHdrKind), kObjectKind);
        a_.jcc(CNE, slow);
        a_.mov(RDX, RAX);
        a_.aluI(4, RDX, -9);
        a_.cmpI(RDX, std::int32_t(kNull));
        a_.jcc(CE, ne);
        a_.jmp(bad);
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
    a_.test32(RAX, RAX);
    a_.jcc(CNE, eq);
    a_.jmp(ne);
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
