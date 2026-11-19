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
            const opt::Effects e = ssa::effects(*code_, i);
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
        ssaResetFacts();
        std::size_t k = 0;
        bool ended = false;
        for (std::uint32_t i = bl.start; i < bl.end && !ssaFailed_; i = opt::nextOf(*code_, i), ++k) {
            const Ins& in = code_->code[i];
            const Op op = in.opcode();
            offsets_[i] = std::uint32_t(a_.here());
            exitAt_ = -1;
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
                else if (ssaOn_ < 2 || !ssaBranch(b, i))
                    ssaStepJump(b, i);
                ended = true;
                break;
            }
            switch (op) {
            case Op::GetProp:
                if (ssaOn_ < 2 || !ssaGetProp(i))
                    ssaGeneric(i, liveAfter[b][k]);
                break;
            case Op::PutProp:
                if (ssaOn_ < 2 || !ssaPutProp(i, liveAfter[b][k]))
                    ssaGeneric(i, liveAfter[b][k]);
                break;
            case Op::AddI: case Op::SubI: case Op::Inc: case Op::Dec: case Op::BitAndI: case Op::BitOrI: case Op::BitXorI:
            case Op::ShlI: case Op::ShrI: case Op::Add: case Op::Sub: case Op::Mul: case Op::BitAnd: case Op::BitOr:
            case Op::BitXor: case Op::Lt: case Op::Le: case Op::Gt: case Op::Ge:
                if (ssaOn_ < 2 || !ssaArith(i))
                    ssaGeneric(i, liveAfter[b][k]);
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
        a_.movImm(RDX, 0);
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
    if (sval_[v].slot >= 0)
        a_.mov(slotOf(v), RAX);
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
        if (failed)
            return false;
        Label& bad = ssaExit(i);
        Label bits;
        // Two int32s, or two objects: equal when their bits are.
        a_.mov(RDX, RAX);
        a_.and_(RDX, RCX);
        a_.cmp(RDX, R14);
        a_.jcc(CAE, bits);
        a_.test(RAX, R15);
        a_.jcc(CNE, bad);
        a_.test(RCX, R15);
        a_.jcc(CNE, bad);
        a_.cmpI8(mem(RAX, kHdrKind), kObjectKind);
        a_.jcc(CNE, bad);
        a_.cmpI8(mem(RCX, kHdrKind), kObjectKind);
        a_.jcc(CNE, bad);
        a_.bind(bits);
        a_.cmp(RAX, RCX);
        ssaCondEdges(b, eqTaken ? CE : CNE);
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
        loadValue(va, RAX);
        needIntV(va, RAX, bad);
        if (imm) {
            a_.aluI32(7, RAX, std::int32_t(std::int16_t(in.b)));
        } else {
            loadValue(vb, RCX);
            needIntV(vb, RCX, bad);
            a_.alu32(7, RAX, RCX);
        }
        Cond c;
        switch (op) {
        case Op::JLt: case Op::JLtI: c = CL; break;
        case Op::JLe: case Op::JLeI: c = CLE; break;
        case Op::JGt: case Op::JGtI: c = CG; break;
        case Op::JGe: case Op::JGeI: c = CGE; break;
        case Op::JNotLt: case Op::JNotLtI: c = CGE; break;
        case Op::JNotLe: case Op::JNotLeI: c = CG; break;
        case Op::JNotGt: case Op::JNotGtI: c = CLE; break;
        default: c = CL; break;  // JNotGe
        }
        ssaCondEdges(b, c);
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
    if (i + 1 >= n_ || code_->code[i + 1].opcode() != Op::Cache)
        return false;
    const std::uint16_t cacheId = code_->code[i + 1].a;
    if (cacheId == kNoCache || (i < code_->prof.size() && (code_->prof[i] & opt::kProfFailed)))
        return false;
    const InlineCache& ic = code_->caches[cacheId];
    if (ic.shape == 0 || ic.next != 0 || ic.holder || ic.pad)
        return false;
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
    const std::uint8_t prof = i < code_->prof.size() ? code_->prof[i] : 0;
    const bool compare = op == Op::Lt || op == Op::Le || op == Op::Gt || op == Op::Ge;
    // Only where the site has made int32s (or booleans) and nothing else, and has not failed.
    if ((prof & opt::kProfFailed) || (!compare && prof != 1))
        return false;
    const bool rr = op == Op::Add || op == Op::Sub || op == Op::Mul || op == Op::BitAnd || op == Op::BitOr ||
                    op == Op::BitXor || compare;
    const std::uint32_t va = name_[in.b];
    const std::uint32_t vb = rr ? name_[in.c] : ssa::kNone;
    Label& bad = ssaExit(i);
    loadValue(va, RAX);
    needIntV(va, RAX, bad);
    if (rr) {
        loadValue(vb, RCX);
        needIntV(vb, RCX, bad);
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
    default: {
        Cond c = CL;
        switch (op) {
        case Op::Lt: c = CL; break;
        case Op::Le: c = CLE; break;
        case Op::Gt: c = CG; break;
        default: c = CGE; break;
        }
        a_.alu32(7, RAX, RCX);
        a_.setcc(c, RAX);
        a_.movzx8(RAX, RAX);
        a_.aluI32(1, RAX, std::int32_t(kFalse));
        ssaDef(i, kVBool | kVNotCell);
        return true;
    }
    }
    a_.or_(RAX, R14);
    ssaDef(i, kVInt | kVNotCell);
    return true;
}
