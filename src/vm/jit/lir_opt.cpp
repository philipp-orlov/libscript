// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The passes on the lowered IR (lir.hpp). They read and write `lir::Func` only.

#include "lir.hpp"

#include <map>
#include <tuple>

namespace script {
namespace internal {
namespace vm {
namespace lir {

void checkElim(Func& f)
{
    // A fact: a check (bounds of a plain array, bounds of a typed array, no hole) of one array and one index has passed.
    std::map<std::tuple<int, std::uint32_t, std::uint32_t>, std::uint32_t> ids;
    auto idOf = [&](int what, std::uint32_t arr, std::uint32_t index) {
        return ids.emplace(std::make_tuple(what, arr, index), std::uint32_t(ids.size())).first->second;
    };
    auto whatOf = [](const Node& n) { return n.op == Op::CheckHole ? 2 : n.imm ? 1 : 0; };
    bool any = false;
    for (Node& n : f.nodes) {
        n.gone = false;
        if (n.op == Op::CheckBounds || n.op == Op::CheckHole) {
            idOf(whatOf(n), n.args[0], n.args[1]);
            any = true;
        }
    }
    if (!any)
        return;
    const std::size_t words = (ids.size() + 63) / 64;
    using Set = std::vector<std::uint64_t>;
    auto has = [](const Set& s, std::uint32_t id) { return (s[id / 64] >> (id % 64)) & 1; };
    auto add = [](Set& s, std::uint32_t id) { s[id / 64] |= std::uint64_t(1) << (id % 64); };
    const std::uint32_t nb = std::uint32_t(f.blocks.size());
    // What holds at the end of each block: everything, until the block has been worked through (the greatest solution).
    std::vector<Set> out(nb, Set(words, ~std::uint64_t(0)));
    auto pass = [&](bool mark) {
        bool changed = false;
        for (std::uint32_t b = 0; b < nb; ++b) {
            Block& bl = f.blocks[b];
            if (!bl.reached)
                continue;
            Set cur(words, bl.entered ? 0 : ~std::uint64_t(0));
            if (!bl.entered)
                for (std::uint32_t p : bl.preds)
                    if (f.blocks[p].reached)
                        for (std::size_t w = 0; w < words; ++w)
                            cur[w] &= out[p][w];
            for (std::uint32_t id : bl.nodes) {
                Node& n = f.nodes[id];
                if (n.op == Op::CheckBounds || n.op == Op::CheckHole) {
                    const std::uint32_t fact = idOf(whatOf(n), n.args[0], n.args[1]);
                    if (has(cur, fact)) {
                        if (mark)
                            n.gone = true;
                    } else {
                        add(cur, fact);
                    }
                    continue;
                }
                if (n.op == Op::StoreElem && (n.imm & 2)) {
                    const auto bounds = ids.find(std::make_tuple(0, n.args[0], n.args[1]));
                    const auto hole = ids.find(std::make_tuple(2, n.args[0], n.args[1]));
                    if (bounds != ids.end() && hole != ids.end() && has(cur, bounds->second) && has(cur, hole->second)) {
                        if (mark) {
                            n.gone = true;
                            n.writes &= ~std::uint32_t(kWorld);
                        }
                        continue;
                    }
                }
                if (n.writes & kWorld)
                    cur.assign(words, 0);
            }
            if (cur != out[b]) {
                out[b] = std::move(cur);
                changed = true;
            }
        }
        return changed;
    };
    while (pass(false)) {
    }
    pass(true);
}

}  // namespace lir
}  // namespace vm
}  // namespace internal
}  // namespace script
