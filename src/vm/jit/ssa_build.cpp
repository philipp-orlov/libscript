// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "ssa.hpp"

#include <algorithm>

#include "opt_analysis.hpp"

namespace script {
namespace internal {
namespace vm {
namespace ssa {

namespace {

bool isTerminator(Op op)
{
    return op == Op::Ret || op == Op::Throw || op == Op::ThrowError;
}

/// The first register a call's frame covers in the caller: the callee's frame starts at the call's `this`.
std::int32_t windowBase(const Ins& in)
{
    if (in.opcode() == Op::Call || in.opcode() == Op::New)
        return std::int32_t(in.b) + 1;
    return -1;
}

}  // namespace

opt::Effects effects(const CodeBlock& code, std::uint32_t i)
{
    opt::Effects e = opt::effectsOf(code, i);
    if (code.code[i].opcode() == Op::JNotCode)
        e.rd.push_back(std::uint16_t(code.code[i].a + 1u));
    return e;
}

bool supported(const CodeBlock& code, std::uint32_t i)
{
    const Op op = code.code[i].opcode();
    if (op == Op::EnvGet || op == Op::EnvHas)
        return false;  // names of a direct eval's scope: the frame is read by name
    return opt::effectsOf(code, i).ok;
}

std::vector<Bits> Graph::liveAfter(const CodeBlock& code, std::uint32_t b) const
{
    const Block& bl = blocks[b];
    std::vector<std::uint32_t> ins;
    for (std::uint32_t i = bl.start; i < bl.end; i = opt::nextOf(code, i))
        ins.push_back(i);
    std::vector<Bits> out(ins.size());
    Bits live = bl.liveOut;
    for (std::size_t k = ins.size(); k-- > 0;) {
        out[k] = live;
        const opt::Effects e = effects(code, ins[k]);
        for (std::uint16_t r : e.wr)
            bitClear(live, r);
        for (std::uint16_t r : e.rd)
            bitSet(live, r);
    }
    return out;
}

Graph build(const CodeBlock& code)
{
    Graph g;
    const std::uint32_t n = std::uint32_t(code.code.size());
    const std::uint32_t nregs = code.nregs;
    g.n = n;
    g.nregs = nregs;
    if (n == 0 || n > 6000 || nregs == 0 || nregs > 600) {
        g.why = "size";
        return g;
    }
    if (code.flags & (CodeBlock::kGenerator | CodeBlock::kAsync | CodeBlock::kAsyncGen | CodeBlock::kNeedsArgs)) {
        g.why = "kind";
        return g;
    }
    if (!code.handlers.empty()) {
        g.why = "handlers";
        return g;
    }
    const std::size_t words = (nregs + 63) / 64;

    // ---- instructions and their effects ----
    g.isIns.assign(n, 0);
    std::vector<opt::Effects> eff(n);
    for (std::uint32_t i = 0; i < n; i = opt::nextOf(code, i)) {
        g.isIns[i] = 1;
        if (!supported(code, i)) {
            g.why = "op";
            return g;
        }
        eff[i] = effects(code, i);
        for (std::uint16_t r : eff[i].rd)
            if (r >= nregs) {
                g.why = "reg";
                return g;
            }
        for (std::uint16_t r : eff[i].wr)
            if (r >= nregs) {
                g.why = "reg";
                return g;
            }
    }

    // ---- blocks ----
    std::vector<std::uint8_t> leader(n + 1, 0);
    leader[0] = 1;
    for (std::uint32_t i = 0; i < n; i = opt::nextOf(code, i)) {
        const std::uint32_t nx = opt::nextOf(code, i);
        const std::int64_t t = opt::jumpTarget(code, i);
        if (t >= 0) {
            if (t >= std::int64_t(n) || !g.isIns[std::size_t(t)]) {
                g.why = "jump";
                return g;
            }
            leader[std::size_t(t)] = 1;
            if (nx <= n)
                leader[nx] = 1;
        } else if (isTerminator(code.code[i].opcode()) && nx <= n) {
            leader[nx] = 1;
        }
    }
    g.blockOf.assign(n, kNone);
    // Block 0 has no instructions: the way in, so that the first instruction may be a loop's head.
    g.blocks.emplace_back();
    for (std::uint32_t i = 0; i < n; i = opt::nextOf(code, i)) {
        if (leader[i]) {
            Block b;
            b.start = i;
            g.blocks.push_back(std::move(b));
        }
        g.blockOf[i] = std::uint32_t(g.blocks.size() - 1);
        g.blocks.back().last = i;
        g.blocks.back().end = opt::nextOf(code, i);
    }
    const std::uint32_t nb = std::uint32_t(g.blocks.size());
    g.blocks[0].succs.push_back(1);
    for (std::uint32_t b = 1; b < nb; ++b) {
        Block& bl = g.blocks[b];
        const std::uint32_t i = bl.last;
        const Op op = code.code[i].opcode();
        const std::int64_t t = opt::jumpTarget(code, i);
        auto edge = [&](std::uint32_t to) {
            bl.succs.push_back(to);
        };
        if (t >= 0) {
            if (opt::isConditionalJump(op)) {
                if (bl.end >= n) {
                    g.why = "falls off";
                    return g;
                }
                edge(g.blockOf[bl.end]);
            }
            edge(g.blockOf[std::size_t(t)]);
        } else if (!isTerminator(op)) {
            if (bl.end >= n) {
                g.why = "falls off";
                return g;
            }
            edge(g.blockOf[bl.end]);
        }
    }
    // Reached from the entry; only edges from reached blocks count.
    {
        std::vector<std::uint32_t> work{0};
        g.blocks[0].reached = true;
        while (!work.empty()) {
            const std::uint32_t b = work.back();
            work.pop_back();
            for (std::uint32_t s : g.blocks[b].succs)
                if (!g.blocks[s].reached) {
                    g.blocks[s].reached = true;
                    work.push_back(s);
                }
        }
    }
    for (std::uint32_t b = 0; b < nb; ++b) {
        if (!g.blocks[b].reached)
            continue;
        for (std::uint32_t s : g.blocks[b].succs) {
            Block& sb = g.blocks[s];
            sb.preds.push_back(b);
            if (s <= b) {
                sb.loopHeader = true;
                sb.loopEnd = std::max(sb.loopEnd, g.blocks[b].end);
            }
        }
    }

    // ---- liveness ----
    for (Block& bl : g.blocks) {
        bl.liveIn.assign(words, 0);
        bl.liveOut.assign(words, 0);
    }
    std::vector<Bits> use(nb, Bits(words, 0)), def(nb, Bits(words, 0));
    for (std::uint32_t b = 0; b < nb; ++b) {
        for (std::uint32_t i = g.blocks[b].start; i < g.blocks[b].end; i = opt::nextOf(code, i)) {
            for (std::uint16_t r : eff[i].rd)
                if (!bitTest(def[b], r))
                    bitSet(use[b], r);
            for (std::uint16_t r : eff[i].wr)
                bitSet(def[b], r);
        }
    }
    for (bool changed = true; changed;) {
        changed = false;
        for (std::uint32_t b = nb; b-- > 0;) {
            Block& bl = g.blocks[b];
            if (!bl.reached)
                continue;
            Bits out(words, 0);
            for (std::uint32_t s : bl.succs)
                for (std::size_t k = 0; k < words; ++k)
                    out[k] |= g.blocks[s].liveIn[k];
            Bits in(words, 0);
            for (std::size_t k = 0; k < words; ++k)
                in[k] = use[b][k] | (out[k] & ~def[b][k]);
            if (in != bl.liveIn || out != bl.liveOut) {
                bl.liveIn = std::move(in);
                bl.liveOut = std::move(out);
                changed = true;
            }
        }
    }

    // ---- what each loop writes ----
    g.loopWrites.assign(nb, Bits());
    for (std::uint32_t h = 0; h < nb; ++h) {
        Block& hb = g.blocks[h];
        if (!hb.loopHeader)
            continue;
        Bits w(words, 0);
        for (std::uint32_t i = hb.start; i < hb.loopEnd; i = opt::nextOf(code, i)) {
            for (std::uint16_t r : eff[i].wr)
                bitSet(w, r);
            const std::int32_t base = windowBase(code.code[i]);
            for (std::int32_t r = base; base >= 0 && r < std::int32_t(nregs); ++r)
                bitSet(w, std::size_t(r));
        }
        g.loopWrites[h] = std::move(w);
    }

    // ---- values ----
    for (std::uint32_t r = 0; r < nregs; ++r) {
        Value v;
        v.kind = Value::Entry;
        v.reg = std::uint16_t(r);
        g.values.push_back(v);
    }
    g.defBase.assign(n, kNone);
    for (std::uint32_t i = 0; i < n; i = opt::nextOf(code, i)) {
        if (eff[i].wr.empty())
            continue;
        g.defBase[i] = std::uint32_t(g.values.size());
        for (std::uint16_t r : eff[i].wr) {
            Value v;
            v.kind = Value::Def;
            v.reg = r;
            v.at = i;
            g.values.push_back(v);
        }
    }
    auto newPhi = [&](std::uint32_t b, unsigned r) {
        Value v;
        v.kind = Value::Phi;
        v.reg = std::uint16_t(r);
        v.at = b;
        g.values.push_back(v);
        const std::uint32_t id = std::uint32_t(g.values.size() - 1);
        g.blocks[b].phis.push_back(id);
        return id;
    };
    for (std::uint32_t b = 0; b < nb; ++b) {
        Block& bl = g.blocks[b];
        if (!bl.reached)
            continue;
        bl.nameIn.assign(nregs, kNone);
        if (b == 0) {
            for (std::uint32_t r = 0; r < nregs; ++r)
                bl.nameIn[r] = r;
            bl.nameOut = bl.nameIn;
            continue;
        } else {
            for (std::uint32_t r = 0; r < nregs; ++r) {
                if (!bitTest(bl.liveIn, r))
                    continue;
                if (bl.loopHeader && bitTest(g.loopWrites[b], r)) {
                    bl.nameIn[r] = newPhi(b, r);
                    continue;
                }
                std::uint32_t one = kNone;
                bool many = false;
                for (std::uint32_t p : bl.preds) {
                    if (p >= b)
                        continue;  // a back edge: the loop does not write r, so it brings what the header has
                    const std::uint32_t v = g.blocks[p].nameOut[r];
                    if (one == kNone)
                        one = v;
                    else if (v != one)
                        many = true;
                }
                if (one == kNone) {
                    g.why = "no value";
                    return g;
                }
                bl.nameIn[r] = many ? newPhi(b, r) : one;
            }
        }
        bl.nameOut = bl.nameIn;
        for (std::uint32_t i = bl.start; i < bl.end; i = opt::nextOf(code, i)) {
            if (code.code[i].opcode() == Op::Mov) {
                // A copy is a second name for the value, not a value of its own.
                bl.nameOut[code.code[i].a] = bl.nameOut[code.code[i].b];
                continue;
            }
            for (std::size_t k = 0; k < eff[i].wr.size(); ++k)
                bl.nameOut[eff[i].wr[k]] = g.defBase[i] + std::uint32_t(k);
        }
    }
    // Phi inputs, now that every block's end is known.
    for (std::uint32_t b = 0; b < nb; ++b) {
        Block& bl = g.blocks[b];
        if (!bl.reached)
            continue;
        for (std::uint32_t v : bl.phis) {
            Value& pv = g.values[v];
            for (std::uint32_t p : bl.preds) {
                const std::uint32_t in = g.blocks[p].nameOut[pv.reg];
                if (in == kNone) {
                    g.why = "no phi input";
                    return g;
                }
                pv.in.push_back(in);
            }
        }
    }
    // Phis of one value only (besides themselves) are that value.
    std::vector<std::uint32_t> repl(g.values.size());
    for (std::uint32_t v = 0; v < repl.size(); ++v)
        repl[v] = v;
    auto find = [&](std::uint32_t v) {
        while (v != kNone && repl[v] != v)
            v = repl[v];
        return v;
    };
    for (bool changed = true; changed;) {
        changed = false;
        for (std::uint32_t b = 0; b < nb; ++b)
            for (std::uint32_t v : g.blocks[b].phis) {
                if (repl[v] != v)
                    continue;
                std::uint32_t one = kNone;
                bool many = false;
                for (std::uint32_t in : g.values[v].in) {
                    const std::uint32_t u = find(in);
                    if (u == v)
                        continue;
                    if (one == kNone)
                        one = u;
                    else if (u != one)
                        many = true;
                }
                if (!many && one != kNone) {
                    repl[v] = one;
                    changed = true;
                }
            }
    }
    for (Block& bl : g.blocks) {
        if (!bl.reached)
            continue;
        for (std::uint32_t& v : bl.nameIn)
            v = find(v);
        for (std::uint32_t& v : bl.nameOut)
            v = find(v);
        std::vector<std::uint32_t> kept;
        for (std::uint32_t v : bl.phis)
            if (repl[v] == v) {
                for (std::uint32_t& in : g.values[v].in)
                    in = find(in);
                kept.push_back(v);
            }
        bl.phis = std::move(kept);
    }
    g.ok = true;
    return g;
}

}  // namespace ssa
}  // namespace vm
}  // namespace internal
}  // namespace script
