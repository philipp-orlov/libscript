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

void Baseline::loadValue(std::uint32_t v, Reg r)
{
    if (sval_[v].isConst)
        a_.movImm(r, sval_[v].bits);
    else
        a_.mov(r, slotOf(v));
}

/// Write the slots of `regs` (with the value each register holds), and of every other live register whose value would
/// otherwise have no owner once the slots of `clobber` (1 per register) are overwritten by what comes next. New values are
/// counted first, the old contents let go of last, so nothing live is freed in between.
void Baseline::ssaSync(std::vector<unsigned> regs, const ssa::Bits& live, const std::vector<std::uint8_t>& clobber)
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
                return;
            }
            add(r);
            changed = true;
        }
    }
    // Already there: nothing to write.
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

/// An instruction run by its baseline template, on the frame: the registers it reads are written first, then every live
/// value it could let go of is given an owner; after it, the registers it wrote hold new values the frame owns.
void Baseline::ssaGeneric(std::uint32_t i, const ssa::Bits& liveAfter)
{
    const Ins& in = code_->code[i];
    const opt::Effects e = opt::effectsOf(*code_, i);
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
    for (unsigned r = 0; r < nregs; ++r)
        if (clobber[r])
            frame_[r] = ssa::kNone;
    if (ssa_->defBase[i] != ssa::kNone)
        for (std::size_t k = 0; k < e.wr.size(); ++k) {
            const unsigned r = e.wr[k];
            const std::uint32_t v = ssa_->defBase[i] + std::uint32_t(k);
            name_[r] = v;
            frame_[r] = v;
            if (sval_[v].slot >= 0) {
                a_.mov(RAX, R(r));
                a_.mov(slotOf(v), RAX);
            }
        }
}

/// The phi moves of edge `from` -> `to`: each phi of `to` gets its input from `from`, all read before any is written.
void Baseline::ssaEdgeMoves(std::uint32_t from, std::uint32_t to)
{
    const ssa::Block& tb = ssa_->blocks[to];
    std::size_t k = 0;
    while (k < tb.preds.size() && tb.preds[k] != from)
        ++k;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> moves;  // (source value, phi)
    bool readsPhi = false;
    for (std::uint32_t p : tb.phis) {
        const std::uint32_t src = ssa_->values[p].in[k];
        if (src == p)
            continue;
        moves.emplace_back(src, p);
        readsPhi = readsPhi || (ssa_->values[src].kind == ssa::Value::Phi && ssa_->values[src].at == to);
    }
    if (!readsPhi) {
        for (const auto& [src, p] : moves) {
            loadValue(src, RAX);
            a_.mov(slotOf(p), RAX);
        }
        return;
    }
    // A phi that reads another phi of the same block: through the stack, all read first.
    const std::int32_t room = std::int32_t((8 * moves.size() + 15) & ~std::size_t(15));
    a_.aluI(5, RSP, room);
    for (std::size_t m = 0; m < moves.size(); ++m) {
        const std::uint32_t src = moves[m].first;
        if (sval_[src].isConst)
            a_.movImm(RAX, sval_[src].bits);
        else
            a_.mov(RAX, mem(RSP, room + 8 + 8 * sval_[src].slot));
        a_.mov(mem(RSP, std::int32_t(8 * m)), RAX);
    }
    for (std::size_t m = 0; m < moves.size(); ++m) {
        a_.mov(RAX, mem(RSP, std::int32_t(8 * m)));
        a_.mov(mem(RSP, room + 8 + 8 * sval_[moves[m].second].slot), RAX);
    }
    a_.aluI(0, RSP, room);
}

/// Leave block `from` for block `to`: a back edge looks at the interrupt flag, puts in the slots the loop's head takes as
/// given, and moves the phis; a forward edge moves the phis and leaves its frame state for the join.
void Baseline::ssaEdge(std::uint32_t from, std::uint32_t to)
{
    const ssa::Block& tb = ssa_->blocks[to];
    const unsigned nregs = code_->nregs;
    if (to > from) {
        ssaEdgeMoves(from, to);
        edgeIn_[to].push_back(EdgeState{frame_});
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
    a_.mov(RDI, R12);
    a_.call(tick_);
    a_.test32(RAX, RAX);
    a_.jcc(CNE, exception(ssa_->blocks[from].last));
    finish();
}

/// A conditional jump the interpreter decides (one step of it): what the jump reads in the frame, every live value safe.
void Baseline::ssaStepJump(std::uint32_t b, std::uint32_t i)
{
    const ssa::Block& bl = ssa_->blocks[b];
    const opt::Effects e = opt::effectsOf(*code_, i);
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
    // A slot for every value something may read: those named at a block's start, phis, and what instructions leave in
    // live registers.
    std::vector<std::uint8_t> used(g.values.size(), 0);
    std::vector<std::vector<ssa::Bits>> liveAfter(nb);
    for (std::uint32_t b = 1; b < nb; ++b) {
        const ssa::Block& bl = g.blocks[b];
        if (!bl.reached)
            continue;
        for (std::uint32_t v : bl.nameIn)
            if (v != ssa::kNone)
                used[v] = 1;
        for (std::uint32_t p : bl.phis) {
            used[p] = 1;
            for (std::uint32_t in : g.values[p].in)
                used[in] = 1;
        }
        liveAfter[b] = g.liveAfter(*code_, b);
        std::size_t k = 0;
        for (std::uint32_t i = bl.start; i < bl.end; i = opt::nextOf(*code_, i), ++k) {
            if (g.defBase[i] == ssa::kNone || code_->code[i].opcode() == Op::Mov)
                continue;
            const opt::Effects e = opt::effectsOf(*code_, i);
            for (std::size_t w = 0; w < e.wr.size(); ++w)
                if (ssa::bitTest(liveAfter[b][k], e.wr[w]))
                    used[g.defBase[i] + w] = 1;
        }
    }
    std::int32_t slots = 0;
    for (std::uint32_t v = 0; v < g.values.size(); ++v)
        if (used[v] && !sval_[v].isConst)
            sval_[v].slot = slots++;
    if (slots > 1024) {
        ssaWhy_ = "slots";
        return nullptr;
    }
    spill_ = std::int32_t((8 * slots + 15) & ~15);

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
    a_.push(RBX);
    a_.push(R13);
    a_.aluI(5, RSP, 8 + spill_);
    a_.mov(R13, RSI);
    a_.mov(RBX, RDI);
    for (unsigned r = 0; r < nregs; ++r)
        if (ssa::bitTest(g.blocks[1].liveIn, r) && sval_[r].slot >= 0) {
            a_.mov(RAX, R(r));
            a_.mov(slotOf(r), RAX);
        }
    {
        std::vector<std::uint32_t> st(nregs);
        for (unsigned r = 0; r < nregs; ++r)
            st[r] = r;  // the slots hold the entry values
        edgeIn_[1].push_back(EdgeState{st});
        ssaEdgeMoves(0, 1);
    }
    for (std::uint32_t b = 1; b < nb && !ssaFailed_; ++b) {
        const ssa::Block& bl = g.blocks[b];
        if (!bl.reached)
            continue;
        a_.bind(blockLabel_[b]);
        ssaEnterBlock(b);
        std::size_t k = 0;
        bool ended = false;
        for (std::uint32_t i = bl.start; i < bl.end && !ssaFailed_; i = opt::nextOf(*code_, i), ++k) {
            const Ins& in = code_->code[i];
            const Op op = in.opcode();
            offsets_[i] = std::uint32_t(a_.here());
            if (opt::jumpTarget(*code_, i) >= 0) {
                if (op == Op::Jmp)
                    ssaEdge(b, bl.succs[0]);
                else
                    ssaStepJump(b, i);
                ended = true;
                break;
            }
            switch (op) {
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
        for (unsigned r = 0; r < nregs; ++r) {
            const std::uint32_t v = bl.nameIn[r];
            if (v == ssa::kNone || sval_[v].slot < 0 || done[v])
                continue;
            done[v] = 1;
            a_.mov(RAX, R(r));
            a_.mov(slotOf(v), RAX);
        }
        a_.jmp(blockLabel_[b]);
    }
    emitTails();
    CodeSpan span = a_.failed() ? CodeSpan{} : allocateCode(a_.buf.size());
    if (!span)
        return fail();
    commitCode(span, a_.buf.data(), a_.buf.size());
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
}
