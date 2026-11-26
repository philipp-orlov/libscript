// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Part of baseline_x64.cpp: the lowered IR of a function the SSA tier compiles (lir.hpp), built after the representation
// plan (`ssaPlan`), so that `sval_` says which values are constants and `native_` which instructions are done in place. An element access
// in place is lowered; every other instruction is one `Generic` node for now.

/// The LIR of graph `g`: one block per SSA block, a node per value made (constants, entry values, phis, instructions).
void Baseline::lirBuild(const ssa::Graph& g, lir::Func& f)
{
    const std::uint32_t nb = std::uint32_t(g.blocks.size());
    const unsigned nregs = code_->nregs;
    f = lir::Func{};
    f.blocks.resize(nb);
    f.valueNode.assign(g.values.size(), lir::kNone);
    auto constNode = [&](std::uint32_t b, std::uint32_t v) {
        lir::Node n;
        n.op = lir::Op::Const;
        n.type = lir::Type::Tagged;
        n.imm = std::int64_t(sval_[v].bits);
        n.def = v;
        const std::uint32_t id = f.add(std::move(n));
        f.blocks[b].nodes.push_back(id);
        f.valueNode[v] = id;
    };
    // The way in: the entry values (a register no instruction has written yet is a constant: undefined).
    for (unsigned r = 0; r < nregs; ++r) {
        if (sval_[r].isConst) {
            constNode(0, r);
            continue;
        }
        lir::Node n;
        n.op = lir::Op::Entry;
        n.type = lir::Type::Tagged;
        n.imm = r;
        n.def = r;
        const std::uint32_t id = f.add(std::move(n));
        f.blocks[0].nodes.push_back(id);
        f.valueNode[r] = id;
    }
    f.blocks[0].reached = f.blocks[0].entered = true;
    auto simple = [&](lir::Op op, std::uint32_t i, std::vector<std::uint32_t> args, std::int64_t imm, bool exits) {
        lir::Node n;
        n.op = op;
        n.at = i;
        n.imm = imm;
        n.args = std::move(args);
        n.mayExit = exits;
        return n;
    };
    for (std::uint32_t b = 0; b < nb; ++b) {
        const ssa::Block& bl = g.blocks[b];
        lir::Block& lb = f.blocks[b];
        lb.ssa = b;
        lb.preds = bl.preds;
        lb.succs = bl.succs;
        lb.loopHeader = bl.loopHeader;
        if (b == 0 || !bl.reached)
            continue;
        lb.reached = true;
        lb.entered = bl.loopHeader && !bl.noEntry;  // the baseline code comes in there
        for (std::uint32_t p : bl.phis) {
            lir::Node n;
            n.op = lir::Op::Phi;
            n.type = lir::Type::Tagged;
            n.def = p;
            const std::uint32_t id = f.add(std::move(n));
            lb.nodes.push_back(id);
            f.valueNode[p] = id;
        }
        std::vector<std::uint32_t> names = bl.nameIn;
        auto nodeOf = [&](unsigned r) { return names[r] == ssa::kNone ? lir::kNone : f.valueNode[names[r]]; };
        for (std::uint32_t i = bl.start; i < bl.end; i = opt::nextOf(*code_, i)) {
            const Ins& in = code_->code[i];
            const Op op = in.opcode();
            if (op == Op::Cache)
                continue;  // the cache word of the instruction before
            if (op == Op::Mov) {
                names[in.a] = names[in.b];  // a second name for a value
                continue;
            }
            const std::uint32_t def = g.defBase[i];
            const opt::Effects e = ssa::effects(*code_, i);
            auto defined = [&](std::uint32_t id) {
                for (std::size_t w = 0; w < e.wr.size(); ++w) {
                    names[e.wr[w]] = def + std::uint32_t(w);
                    if (id != lir::kNone)
                        f.valueNode[def + w] = id;
                }
            };
            if (def != ssa::kNone && sval_[def].isConst) {
                constNode(b, def);
                defined(lir::kNone);
                continue;
            }
            if (op == Op::ToNumeric && native_[i] && nodeOf(in.b) != lir::kNone && !sval_[names[in.b]].isConst &&
                (sval_[names[in.b]].rep == kRepI32 || sval_[names[in.b]].rep == kRepF64)) {
                defined(nodeOf(in.b));  // of a number: that number
                continue;
            }
            ElemKind kind;
            const bool put = op == Op::PutElem;
            const int plan = (put || (op == Op::GetElem && !ssaPrivateSite(i))) && native_[i] && !elim_[i] ? ssaElemPlan(i, put, &kind) : 0;
            const std::uint32_t arr = plan ? nodeOf(put ? in.a : in.b) : lir::kNone;
            const std::uint32_t index = plan ? nodeOf(put ? in.b : in.c) : lir::kNone;
            if (plan && arr != lir::kNone && index != lir::kNone && (plan == 2 || put || ssaElemQuiet(i))) {
                // An element access in place: its checks, then the load or the store.
                const bool typed = plan == 2;
                const bool checked = typed ? f.nodes[index].op != lir::Op::Const : !put && sval_[def].rep != kRepTagged;
                if (checked)
                    lb.nodes.push_back(f.add(simple(lir::Op::CheckBounds, i, {arr, index}, typed, true)));
                if (checked && !typed)
                    lb.nodes.push_back(f.add(simple(lir::Op::CheckHole, i, {arr, index}, 0, true)));
                lir::Node n = put ? simple(lir::Op::StoreElem, i, {arr, index, nodeOf(in.c)}, typed ? 1 : ssaQuietStore(i, names[in.c]) ? 2 : 0, true)
                                  : simple(lir::Op::LoadElem, i, {arr, index}, typed, true);
                if (put && !typed)
                    n.writes = lir::kWorld;  // the runtime's, unless the pass finds its element (`checkElim`)
                if (!put) {
                    n.type = lir::Type::Tagged;
                    n.def = def;
                }
                const std::uint32_t id = f.add(std::move(n));
                lb.nodes.push_back(id);
                defined(put ? lir::kNone : id);
                continue;
            }
            lir::Node n;
            n.op = lir::Op::Generic;
            n.type = def == ssa::kNone ? lir::Type::None : lir::Type::Tagged;
            n.at = i;
            n.def = def;
            // In place it runs no other code and takes no element away, but for a new `length`. (An allocation and an
            // iterator step are taken as the load elimination takes them.)
            const bool quiet = native_[i] && op != Op::GetElem && op != Op::PutElem && op != Op::New && op != Op::IterInit &&
                               op != Op::IterNext && op != Op::IterClose &&
                               !((op == Op::PutProp || op == Op::InitProp) && code_->atoms[in.b] == vm_.atoms.length);
            n.reads = lir::kWorld;
            n.writes = quiet ? lir::kNoRegion : lir::kWorld;
            n.mayExit = true;
            const std::uint32_t id = f.add(std::move(n));
            lb.nodes.push_back(id);
            defined(def == ssa::kNone ? lir::kNone : id);
        }
    }
    // The phis' inputs, now that every value has its node.
    for (std::uint32_t b = 1; b < nb; ++b)
        for (std::uint32_t p : g.blocks[b].phis)
            for (std::uint32_t in : g.values[p].in)
                f.nodes[f.valueNode[p]].args.push_back(in == ssa::kNone ? lir::kNone : f.valueNode[in]);
}

/// Whether the store at `i` of value `vx` into a plain array can go without the runtime when its element is known to be
/// there: the place has met arrays of int32s only and the value is an int32, or arrays of doubles only and the value is
/// a number; and no such store has left the code there before.
bool Baseline::ssaQuietStore(std::uint32_t i, std::uint32_t vx) const
{
    if (vx == ssa::kNone || !ssaElemQuiet(i))
        return false;
    const SVal& s = sval_[vx];
    const JSValue k = JSValue::raw(s.bits);
    switch (ssaElemKinds(i)) {
    case elemprof::kIntArray: return s.isConst ? k.isInt32() : s.rep == kRepI32;
    case elemprof::kNumArray: return s.isConst ? k.isInt32() || (k.isDouble() && k.asDouble() == k.asDouble()) : s.rep != kRepTagged;
    default: return false;
    }
}

/// The LIR of `g` and its passes: what the emitter reads of them (`lirGone_`, `lirChecked_`).
void Baseline::lirPlan(const ssa::Graph& g)
{
    lirGone_.assign(n_, 0);
    lirChecked_.assign(n_, 0);
#if defined(SCRIPT_JIT_ENV_OVERRIDE)
    static const bool off = [] { const char* l = std::getenv("SSA_LIR"); return l && std::strstr(l, "off"); }();
    if (off)
        return;
#endif
    // Only a function with a store that can go without the runtime gains for now (`ssaQuietStore`).
    bool any = false;
    for (std::uint32_t i = 0; i < n_ && !any; i += length(i)) {
        ElemKind kind;
        any = code_->code[i].opcode() == Op::PutElem && native_[i] && ssaElemQuiet(i) && ssaElemPlan(i, true, &kind) == 1 &&
              (ssaElemKinds(i) == elemprof::kIntArray || ssaElemKinds(i) == elemprof::kNumArray);
    }
    if (!any)
        return;
    lir::Func f;
    lirBuild(g, f);
    lir::checkElim(f);
    for (const lir::Node& n : f.nodes) {
        if (n.op == lir::Op::StoreElem ? !(n.imm & 1) : n.op == lir::Op::CheckBounds && n.imm)
            lirGone_[n.at] = lirGone_[n.at] || n.gone;
        else if (n.op == lir::Op::CheckBounds && !n.gone)
            lirChecked_[n.at] = 1;
    }
#if defined(SCRIPT_JIT_ENV_OVERRIDE)
    if (const char* l = std::getenv("SSA_LIR"); l && std::strstr(l, "dump"))
        std::fprintf(stderr, "lir %s\n%s", mapName(code_).c_str(), lir::dump(f).c_str());
#endif
}
