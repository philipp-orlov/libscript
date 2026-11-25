// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Part of baseline_x64.cpp: the lowered IR of a function the SSA tier compiles (lir.hpp), built after the representation
// plan (`ssaPlan`), so that `sval_` says which values are constants. Every instruction is one `Generic` node for now.

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
    for (std::uint32_t b = 0; b < nb; ++b) {
        const ssa::Block& bl = g.blocks[b];
        lir::Block& lb = f.blocks[b];
        lb.ssa = b;
        lb.preds = bl.preds;
        lb.succs = bl.succs;
        lb.loopHeader = bl.loopHeader;
        if (b == 0 || !bl.reached)
            continue;
        for (std::uint32_t p : bl.phis) {
            lir::Node n;
            n.op = lir::Op::Phi;
            n.type = lir::Type::Tagged;
            n.def = p;
            const std::uint32_t id = f.add(std::move(n));
            lb.nodes.push_back(id);
            f.valueNode[p] = id;
        }
        for (std::uint32_t i = bl.start; i < bl.end; i = opt::nextOf(*code_, i)) {
            const Ins& in = code_->code[i];
            if (in.opcode() == Op::Mov || in.opcode() == Op::Cache)
                continue;  // a second name for a value; the cache word of the instruction before
            const std::uint32_t def = g.defBase[i];
            if (def != ssa::kNone && sval_[def].isConst) {
                constNode(b, def);
                continue;
            }
            lir::Node n;
            n.op = lir::Op::Generic;
            n.type = def == ssa::kNone ? lir::Type::None : lir::Type::Tagged;
            n.at = i;
            n.def = def;
            n.reads = n.writes = lir::kWorld;
            n.mayExit = true;
            const std::uint32_t id = f.add(std::move(n));
            lb.nodes.push_back(id);
            if (def != ssa::kNone) {
                const std::size_t writes = ssa::effects(*code_, i).wr.size();
                for (std::size_t k = 0; k < writes; ++k)
                    f.valueNode[def + k] = id;
            }
        }
    }
    // The phis' inputs, now that every value has its node.
    for (std::uint32_t b = 1; b < nb; ++b)
        for (std::uint32_t p : g.blocks[b].phis)
            for (std::uint32_t in : g.values[p].in)
                f.nodes[f.valueNode[p]].args.push_back(in == ssa::kNone ? lir::kNone : f.valueNode[in]);
}
