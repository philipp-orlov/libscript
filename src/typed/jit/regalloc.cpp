// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Register allocation by priority over live ranges.
//
// Each virtual register gets one location for its whole life: a machine register, or a spill slot. Its life is the set of
// positions where it is live, holes included, so two values share a register wherever their lives do not overlap. Values
// are placed most-touched first, uses weighted by loop depth and next to nothing in cold code, so what a loop works on
// gets a register and what only a bail block reads takes what is left.
#include <algorithm>
#include <functional>
#include <queue>
#include <set>
#include <cstdio>
#include <string>

#include "script/internal/typed/jit_ir.hpp"

namespace script {
namespace internal {
namespace typed {

namespace {

/// Calls `use` for every virtual register an instruction reads, and `def` for the one it writes.
template <class Use, class Def>
void visitOperands(const IrFunction& fn, const IrInst& in, Use use, Def def)
{
    if (in.op == IrOp::Call) {
        const IrCall& call = fn.calls[std::size_t(in.imm)];
        for (std::int32_t arg : call.args)
            use(arg);
        if (call.delegate >= 0)
            use(call.delegate);
        if (call.result >= 0)
            def(call.result);
        if (call.resultTag >= 0)
            def(call.resultTag);
        return;
    }
    if (in.a >= 0)
        use(in.a);
    if (in.b >= 0)
        use(in.b);
    if (in.c >= 0)
        use(in.c);
    if (in.d >= 0)
        def(in.d);
}

struct Range
{
    std::int32_t from;
    std::int32_t to;  ///< inclusive
    bool operator<(const Range& o) const noexcept { return from < o.from || (from == o.from && to < o.to); }
};

struct Interval
{
    std::int32_t vreg = -1;
    std::int32_t start = 0x7fffffff;
    std::int32_t end = -1;
    std::vector<Range> ranges;
    double weight = 0;
    bool crossesCall = false;
    std::int32_t hint = -1;  ///< a machine register it would like to be in
};

class BitSet
{
public:
    explicit BitSet(std::size_t bits = 0) : words_((bits + 63) / 64, 0) {}
    bool test(std::size_t i) const noexcept { return (words_[i / 64] >> (i % 64)) & 1u; }
    void set(std::size_t i) noexcept { words_[i / 64] |= std::uint64_t(1) << (i % 64); }
    void clear(std::size_t i) noexcept { words_[i / 64] &= ~(std::uint64_t(1) << (i % 64)); }
    /// this |= other & ~minus; true if anything changed.
    bool mergeMinus(const BitSet& other, const BitSet& minus) noexcept
    {
        bool changed = false;
        for (std::size_t i = 0; i < words_.size(); ++i) {
            const std::uint64_t next = words_[i] | (other.words_[i] & ~minus.words_[i]);
            changed = changed || next != words_[i];
            words_[i] = next;
        }
        return changed;
    }
    bool merge(const BitSet& other) noexcept
    {
        bool changed = false;
        for (std::size_t i = 0; i < words_.size(); ++i) {
            const std::uint64_t next = words_[i] | other.words_[i];
            changed = changed || next != words_[i];
            words_[i] = next;
        }
        return changed;
    }
    template <class F>
    void forEach(F f) const
    {
        for (std::size_t i = 0; i < words_.size(); ++i) {
            std::uint64_t w = words_[i];
            while (w) {
                f(i * 64 + std::size_t(__builtin_ctzll(w)));
                w &= w - 1;
            }
        }
    }

private:
    std::vector<std::uint64_t> words_;
};

struct Block
{
    std::int32_t first = 0;
    std::int32_t last = 0;  ///< inclusive
    std::vector<std::int32_t> successors;
    BitSet use, def, liveIn, liveOut;
};

}  // namespace

void allocateRegisters(IrFunction& fn, const RegisterSet& regs)
{
    const std::size_t count = fn.code.size();
    const std::size_t vregs = fn.vregIsFp.size();
    // A use in cold code counts for this much of one in hot code.
    constexpr double kColdWeight = 0.01;

    // ---- basic blocks ----
    std::vector<std::int32_t> labelAt(std::size_t(fn.labelCount), -1);
    std::vector<std::uint8_t> starts(count + 1, 0);
    starts[0] = 1;
    for (std::size_t i = 0; i < count; ++i) {
        const IrInst& in = fn.code[i];
        switch (in.op) {
            case IrOp::Label:
                labelAt[std::size_t(in.imm)] = std::int32_t(i);
                starts[i] = 1;
                break;
            case IrOp::Br:
            case IrOp::BrCmp:
            case IrOp::BrFCmp:
            case IrOp::Ret:
            case IrOp::ColdBegin:
                starts[i + 1] = 1;
                break;
            case IrOp::ColdEnd:
                starts[i] = 1;
                break;
            default: break;
        }
    }
    // The hot path goes from before a cold region straight to after it: that is where the region is not laid out.
    std::vector<std::int32_t> coldEndOf(count, -1);
    {
        std::vector<std::size_t> open;
        for (std::size_t i = 0; i < count; ++i) {
            if (fn.code[i].op == IrOp::ColdBegin) {
                open.push_back(i);
            } else if (fn.code[i].op == IrOp::ColdEnd && !open.empty()) {
                coldEndOf[open.back()] = std::int32_t(i);
                open.pop_back();
            }
        }
    }
    std::vector<Block> blocks;
    std::vector<std::int32_t> blockOfInst(count + 1, 0);
    for (std::size_t i = 0; i < count;) {
        Block block;
        block.first = std::int32_t(i);
        std::size_t j = i + 1;
        while (j < count && !starts[j])
            ++j;
        block.last = std::int32_t(j - 1);
        for (std::size_t k = i; k < j; ++k)
            blockOfInst[k] = std::int32_t(blocks.size());
        blocks.push_back(std::move(block));
        i = j;
    }
    for (std::size_t b = 0; b < blocks.size(); ++b) {
        Block& block = blocks[b];
        const IrInst& last = fn.code[std::size_t(block.last)];
        const auto toLabel = [&](std::int64_t label) {
            block.successors.push_back(blockOfInst[std::size_t(labelAt[std::size_t(label)])]);
        };
        if (last.op == IrOp::Br) {
            toLabel(last.imm);
        } else if (last.op == IrOp::BrCmp || last.op == IrOp::BrFCmp) {
            toLabel(last.target);
            if (b + 1 < blocks.size())
                block.successors.push_back(std::int32_t(b + 1));
        } else if (last.op == IrOp::ColdBegin && coldEndOf[std::size_t(block.last)] >= 0) {
            block.successors.push_back(blockOfInst[std::size_t(coldEndOf[std::size_t(block.last)])]);
        } else if (last.op != IrOp::Ret && b + 1 < blocks.size()) {
            block.successors.push_back(std::int32_t(b + 1));
        }
        // An instruction that can raise may end up at a handler.
        for (std::int32_t i = block.first; i <= block.last; ++i) {
            const IrInst& in = fn.code[std::size_t(i)];
            std::int32_t e = -1;
            if (in.op == IrOp::Call)
                e = fn.calls[std::size_t(in.imm)].exit;
            else if (in.op == IrOp::ExitCmp || in.op == IrOp::Poll)
                e = in.target;
            if (e >= 0 && fn.exits[std::size_t(e)].handlerLabel >= 0)
                toLabel(fn.exits[std::size_t(e)].handlerLabel);
        }
    }

    // An exit reads what it has to release, after the instruction that takes it.
    const auto exitOf = [&](const IrInst& in) -> const IrExit* {
        if (in.op == IrOp::Call) {
            const std::int32_t e = fn.calls[std::size_t(in.imm)].exit;
            return e >= 0 ? &fn.exits[std::size_t(e)] : nullptr;
        }
        if (in.op == IrOp::ExitCmp || in.op == IrOp::Poll)
            return &fn.exits[std::size_t(in.target)];
        return nullptr;
    };

    // ---- which values cross blocks ----
    // A value seen in one block only, and written there before it is read, never is live into or out of one: the sets
    // below are only over the others, which in a long function are a small part of all.
    std::vector<std::int32_t> globalOf(vregs, -1);
    std::vector<std::int32_t> globalVreg;
    {
        std::vector<std::int32_t> home(vregs, -1);
        std::vector<std::int32_t> definedIn(vregs, -1);
        std::vector<std::uint8_t> crosses(vregs, 0);
        for (std::size_t b = 0; b < blocks.size(); ++b) {
            const std::int32_t bi = std::int32_t(b);
            const auto see = [&](std::int32_t v) {
                if (home[std::size_t(v)] < 0)
                    home[std::size_t(v)] = bi;
                else if (home[std::size_t(v)] != bi)
                    crosses[std::size_t(v)] = 1;
            };
            const auto read = [&](std::int32_t v) {
                if (v < 0)
                    return;
                see(v);
                if (definedIn[std::size_t(v)] != bi)
                    crosses[std::size_t(v)] = 1;
            };
            for (std::int32_t i = blocks[b].first; i <= blocks[b].last; ++i) {
                const IrInst& in = fn.code[std::size_t(i)];
                visitOperands(fn, in, read, [](std::int32_t) {});
                if (const IrExit* exit = exitOf(in)) {
                    for (std::int32_t v : exit->cleanup)
                        read(v);
                    for (const auto& tagged : exit->cleanupTagged) {
                        read(tagged.first);
                        read(tagged.second);
                    }
                    read(exit->indexVreg);
                    for (const IrExit::WriteBack& back : exit->writeBacks) {
                        read(back.base);
                        read(back.vreg);
                    }
                }
                visitOperands(fn, in, [](std::int32_t) {}, [&](std::int32_t v) {
                    see(v);
                    definedIn[std::size_t(v)] = bi;
                });
            }
        }
        // What arrives from outside is live from before the first block.
        const auto fromOutside = [&](std::int32_t v) {
            if (v >= 0)
                crosses[std::size_t(v)] = 1;
        };
        for (std::int32_t p : fn.params)
            fromOutside(p);
        for (std::int32_t c : fn.captures)
            fromOutside(c);
        for (std::int32_t z : fn.osrZero)
            fromOutside(z);
        for (std::int32_t z : fn.osrArguments)
            fromOutside(z);
        for (const IrOsrEntry& entry : fn.osrEntries) {
            for (const IrOsrEntry::Slot& slot : entry.slots) {
                fromOutside(slot.vreg);
                fromOutside(slot.tagVreg);
            }
            for (const IrOsrEntry::Slot& slot : entry.pointers)
                fromOutside(slot.vreg);
        }
        for (std::size_t v = 0; v < vregs; ++v) {
            if (crosses[v]) {
                globalOf[v] = std::int32_t(globalVreg.size());
                globalVreg.push_back(std::int32_t(v));
            }
        }
    }
    for (Block& block : blocks)
        block.use = block.def = block.liveIn = block.liveOut = BitSet(globalVreg.size());

    // ---- liveness ----
    for (Block& block : blocks) {
        for (std::int32_t i = block.first; i <= block.last; ++i) {
            const IrInst& in = fn.code[std::size_t(i)];
            visitOperands(
                fn, in,
                [&](std::int32_t v) {
                    const std::int32_t g = globalOf[std::size_t(v)];
                    if (g >= 0 && !block.def.test(std::size_t(g)))
                        block.use.set(std::size_t(g));
                },
                [](std::int32_t) {});
            if (const IrExit* exit = exitOf(in)) {
                const auto read = [&](std::int32_t v) {
                    const std::int32_t g = v >= 0 ? globalOf[std::size_t(v)] : -1;
                    if (g >= 0 && !block.def.test(std::size_t(g)))
                        block.use.set(std::size_t(g));
                };
                for (std::int32_t v : exit->cleanup)
                    read(v);
                for (const auto& tagged : exit->cleanupTagged) {
                    read(tagged.first);
                    read(tagged.second);
                }
                if (exit->indexVreg >= 0)
                    read(exit->indexVreg);
                for (const IrExit::WriteBack& back : exit->writeBacks) {
                    read(back.base);
                    read(back.vreg);
                }
            }
            visitOperands(fn, in, [](std::int32_t) {}, [&](std::int32_t v) {
                if (globalOf[std::size_t(v)] >= 0)
                    block.def.set(std::size_t(globalOf[std::size_t(v)]));
            });
        }
    }
    // A worklist: a block is looked at again only when what is live into one of its successors grew. Sweeping all of them
    // until nothing changes costs a pass over every block's sets each time, which a function of many blocks and many
    // values cannot afford.
    {
        std::vector<std::vector<std::int32_t>> preds(blocks.size());
        for (std::size_t b = 0; b < blocks.size(); ++b) {
            for (std::int32_t s : blocks[b].successors)
                preds[std::size_t(s)].push_back(std::int32_t(b));
        }
        std::vector<std::int32_t> work;
        std::vector<std::uint8_t> queued(blocks.size(), 1);
        work.reserve(blocks.size());
        for (std::size_t b = 0; b < blocks.size(); ++b)
            work.push_back(std::int32_t(b));  // popped from the back: the last block first
        while (!work.empty()) {
            const std::int32_t b = work.back();
            work.pop_back();
            queued[std::size_t(b)] = 0;
            Block& block = blocks[std::size_t(b)];
            for (std::int32_t s : block.successors)
                block.liveOut.merge(blocks[std::size_t(s)].liveIn);
            bool grew = block.liveIn.merge(block.use);
            grew = block.liveIn.mergeMinus(block.liveOut, block.def) || grew;
            if (!grew)
                continue;
            for (std::int32_t p : preds[std::size_t(b)]) {
                if (!queued[std::size_t(p)]) {
                    queued[std::size_t(p)] = 1;
                    work.push_back(p);
                }
            }
        }
    }

    // ---- loop depth, for the spill weights ----
    std::vector<std::uint8_t> depth(count + 1, 0);
    for (std::size_t i = 0; i < count; ++i) {
        const IrInst& in = fn.code[i];
        std::int64_t label = -1;
        if (in.op == IrOp::Br)
            label = in.imm;
        else if (in.op == IrOp::BrCmp || in.op == IrOp::BrFCmp)
            label = in.target;
        if (label < 0)
            continue;
        const std::size_t to = std::size_t(labelAt[std::size_t(label)]);
        if (to <= i) {
            for (std::size_t k = to; k <= i; ++k) {
                if (depth[k] < 6)
                    ++depth[k];
            }
        }
    }

    // ---- live ranges ----
    // Position 2i is where instruction i reads, 2i + 1 where it writes. A value's life is a list of closed ranges of
    // positions, with holes where it is dead: the result of one arm of a conditional is not live across a call in the
    // other. Ranges are built a block at a time, backwards from what is live out of it.
    static const double kDepthWeight[] = {1, 8, 64, 512, 4096, 32768, 262144};
    // Code between ColdBegin and ColdEnd runs about never: a use there costs next to nothing when it reads memory.
    std::vector<std::uint8_t> cold(count + 1, 0);
    {
        int nest = 0;
        for (std::size_t i = 0; i < count; ++i) {
            if (fn.code[i].op == IrOp::ColdBegin)
                ++nest;
            cold[i] = nest > 0;
            if (fn.code[i].op == IrOp::ColdEnd && nest > 0)
                --nest;
        }
    }
    std::vector<Interval> intervals(vregs);
    for (std::size_t v = 0; v < vregs; ++v)
        intervals[v].vreg = std::int32_t(v);
    std::vector<std::int32_t> callPositions;
    // Where the range being built for each value ends, walking a block backwards; -1 when it is not live there.
    std::vector<std::int32_t> endOf(vregs, -1);
    for (std::size_t bi = blocks.size(); bi-- > 0;) {
        const Block& block = blocks[bi];
        const std::int32_t from = block.first * 2;
        const std::int32_t to = block.last * 2 + 1;
        block.liveOut.forEach([&](std::size_t g) { endOf[std::size_t(globalVreg[g])] = to; });
        const auto use = [&](std::int32_t v, std::int32_t pos) {
            if (endOf[std::size_t(v)] < 0)
                endOf[std::size_t(v)] = pos;
        };
        const auto def = [&](std::int32_t v, std::int32_t pos) {
            const std::int32_t end = endOf[std::size_t(v)];
            intervals[std::size_t(v)].ranges.push_back({pos, end < pos ? pos : end});
            endOf[std::size_t(v)] = -1;
        };
        for (std::int32_t i = block.last; i >= block.first; --i) {
            const IrInst& in = fn.code[std::size_t(i)];
            const double weight = kDepthWeight[depth[std::size_t(i)]] * (cold[std::size_t(i)] ? kColdWeight : 1.0);
            visitOperands(fn, in, [](std::int32_t) {}, [&](std::int32_t v) {
                def(v, i * 2 + 1);
                intervals[std::size_t(v)].weight += weight;
            });
            if (const IrExit* exit = exitOf(in)) {
                const std::int32_t after = i * 2 + 1 + (in.op == IrOp::Call ? 1 : 0);
                for (std::int32_t v : exit->cleanup)
                    use(v, after);
                for (const auto& tagged : exit->cleanupTagged) {
                    use(tagged.first, after);
                    use(tagged.second, after);
                }
                if (exit->indexVreg >= 0)
                    use(exit->indexVreg, i * 2 + 1);
                for (const IrExit::WriteBack& back : exit->writeBacks) {
                    use(back.base, i * 2 + 1);
                    use(back.vreg, i * 2 + 1);
                }
            }
            visitOperands(fn, in, [&](std::int32_t v) {
                use(v, i * 2);
                intervals[std::size_t(v)].weight += weight;
            }, [](std::int32_t) {});
            if (in.op == IrOp::Call && !fn.calls[std::size_t(in.imm)].cold)
                callPositions.push_back(i * 2);
            // Raising on the way to a handler in this method calls the runtime, and the handler carries on after it.
            else if (in.op == IrOp::ExitCmp && fn.exits[std::size_t(in.target)].handlerLabel >= 0)
                callPositions.push_back(i * 2);
        }
        // What is still open is what is live into the block.
        block.liveIn.forEach([&](std::size_t g) {
            const std::size_t v = std::size_t(globalVreg[g]);
            if (endOf[v] >= 0)
                intervals[v].ranges.push_back({from, endOf[v]});
            endOf[v] = -1;
        });
    }
    std::sort(callPositions.begin(), callPositions.end());
    fn.makesCalls = false;
    for (const IrInst& in : fn.code)
        fn.makesCalls = fn.makesCalls || in.op == IrOp::Call;
    // What arrives at entry is live from before the first instruction.
    const auto fromEntry = [&](std::int32_t v) { intervals[std::size_t(v)].ranges.push_back({-1, 0}); };
    for (std::int32_t p : fn.params)
        fromEntry(p);
    for (std::int32_t c : fn.captures)
        fromEntry(c);
    for (std::int32_t z : fn.osrZero)
        fromEntry(z);
    for (std::int32_t z : fn.osrArguments)
        fromEntry(z);
    // Sorted and merged: touching ranges are one.
    for (Interval& iv : intervals) {
        if (iv.ranges.empty())
            continue;
        std::sort(iv.ranges.begin(), iv.ranges.end());
        std::vector<Range> merged;
        for (const Range& r : iv.ranges) {
            if (!merged.empty() && r.from <= merged.back().to + 1)
                merged.back().to = std::max(merged.back().to, r.to);
            else
                merged.push_back(r);
        }
        iv.ranges.swap(merged);
        iv.start = iv.ranges.front().from;
        iv.end = iv.ranges.back().to;
    }
    // Entering at a loop head writes only what is live there; anything else may share a register with something that is.
    for (IrOsrEntry& entry : fn.osrEntries) {
        const std::int32_t at = labelAt[std::size_t(entry.label)];
        const BitSet& liveIn = blocks[std::size_t(blockOfInst[std::size_t(at)])].liveIn;
        const auto isLive = [&](std::int32_t v) {
            return v >= 0 && globalOf[std::size_t(v)] >= 0 && liveIn.test(std::size_t(globalOf[std::size_t(v)]));
        };
        entry.pointers.erase(std::remove_if(entry.pointers.begin(), entry.pointers.end(),
                                            [&](const IrOsrEntry::Slot& s) { return !isLive(s.vreg); }),
                             entry.pointers.end());
        entry.slots.erase(std::remove_if(entry.slots.begin(), entry.slots.end(),
                                         [&](const IrOsrEntry::Slot& s) {
                                             return !isLive(s.vreg) && (s.tagVreg < 0 || !isLive(s.tagVreg));
                                         }),
                          entry.slots.end());
    }

    // A value that has to survive a call needs a callee-saved register or a slot: one live both where the call reads and
    // where it writes.
    const auto liveAcross = [](const Interval& iv, std::int32_t pos) {
        for (const Range& r : iv.ranges) {
            if (r.from <= pos && r.to >= pos + 1)
                return true;
        }
        return false;
    };
    for (Interval& iv : intervals) {
        for (const Range& r : iv.ranges) {
            auto it = std::lower_bound(callPositions.begin(), callPositions.end(), r.from);
            if (it != callPositions.end() && *it + 1 <= r.to) {
                iv.crossesCall = true;
                break;
            }
        }
    }
    // An incoming parameter gets its own register's value handed over at entry with no move if nothing stands in the
    // way; the same goes for an argument computed right before the call that takes it.
    {
        const auto argReg = [](const std::vector<std::int32_t>& regsOf, std::int32_t index) {
            return std::size_t(index) < regsOf.size() ? regsOf[std::size_t(index)] : -1;
        };
        std::int32_t nextInt = 0, nextFp = 0;
        for (std::int32_t p : fn.params) {
            if (fn.vregIsFp[std::size_t(p)])
                intervals[std::size_t(p)].hint = argReg(regs.fpArgs, nextFp++);
            else
                intervals[std::size_t(p)].hint = argReg(regs.intArgs, nextInt++);
        }
        for (const IrCall& call : fn.calls) {
            std::int32_t intArg = 0, fpArg = 0;
            for (std::int32_t arg : call.passed()) {
                Interval& iv = intervals[std::size_t(arg)];
                if (fn.vregIsFp[std::size_t(arg)]) {
                    const std::int32_t r = argReg(regs.fpArgs, fpArg++);
                    if (iv.hint < 0)
                        iv.hint = r;
                } else {
                    const std::int32_t r = argReg(regs.intArgs, intArg++);
                    if (iv.hint < 0)
                        iv.hint = r;
                }
            }
            if (call.result >= 0 && intervals[std::size_t(call.result)].hint < 0 && !fn.vregIsFp[std::size_t(call.result)])
                intervals[std::size_t(call.result)].hint = regs.intResult;
            if (call.result >= 0 && intervals[std::size_t(call.result)].hint < 0 && fn.vregIsFp[std::size_t(call.result)])
                intervals[std::size_t(call.result)].hint = 0;
        }
    }

    // Values a copy joins: where one of them has a register the other fits in, the copy becomes nothing.
    std::vector<std::vector<std::int32_t>> partners(vregs);
    for (const IrInst& in : fn.code) {
        if (in.op != IrOp::Mov || in.d < 0 || in.a < 0 || in.d == in.a || fn.vregIsFp[std::size_t(in.d)] != fn.vregIsFp[std::size_t(in.a)])
            continue;
        partners[std::size_t(in.d)].push_back(in.a);
        partners[std::size_t(in.a)].push_back(in.d);
    }

    // ---- assignment, most valuable first ----
    // Each value is placed whole: in the first register none of whose values overlaps it, or in memory. Going by how much
    // a value is touched, the ones a loop reads get registers before ones that only a bail looks at, and those then fit
    // in around them or spill at little cost.
    std::vector<Interval*> order;
    for (Interval& iv : intervals) {
        if (!iv.ranges.empty())
            order.push_back(&iv);
    }
    std::stable_sort(order.begin(), order.end(), [](const Interval* a, const Interval* b) { return a->weight > b->weight; });

    fn.location.assign(vregs, IrFunction::kUnused);
    // What each machine register holds, as disjoint ranges in order: a tree, since a long function puts tens of
    // thousands in each, in no particular order.
    std::vector<std::set<Range>> busyInt(64), busyFp(64);
    const auto fits = [](const std::set<Range>& busy, const Interval& iv) {
        for (const Range& r : iv.ranges) {
            // The ranges either side of where r.from would go: the one after must start beyond r.to, the one before end
            // short of r.from.
            auto it = busy.upper_bound(Range{r.from, 0x7fffffff});
            if (it != busy.end() && it->from <= r.to)
                return false;
            if (it != busy.begin() && std::prev(it)->to >= r.from)
                return false;
        }
        return true;
    };
    const auto occupy = [](std::set<Range>& busy, const Interval& iv) {
        for (const Range& r : iv.ranges)
            busy.insert(r);
    };
    std::vector<Interval*> spilled;
    for (Interval* iv : order) {
        const bool fp = fn.vregIsFp[std::size_t(iv->vreg)] != 0;
        std::vector<std::set<Range>>& busy = fp ? busyFp : busyInt;
        const std::vector<std::int32_t>& callerOrder = fp ? regs.fpCallerSaved : regs.intCallerSaved;
        const std::vector<std::int32_t>& calleeOrder = fp ? regs.fpCalleeSaved : regs.intCalleeSaved;
        std::int32_t chosen = -1;
        const auto isCallee = [&](std::int32_t r) {
            return std::find(calleeOrder.begin(), calleeOrder.end(), r) != calleeOrder.end();
        };
        for (std::int32_t other : partners[std::size_t(iv->vreg)]) {
            const std::int32_t r = fn.location[std::size_t(other)];
            if (r >= 0 && r != IrFunction::kUnused && (!iv->crossesCall || isCallee(r)) && fits(busy[std::size_t(r)], *iv)) {
                chosen = r;
                break;
            }
        }
        if (chosen < 0 && iv->hint >= 0 && (!iv->crossesCall || isCallee(iv->hint)) && fits(busy[std::size_t(iv->hint)], *iv))
            chosen = iv->hint;
        if (chosen < 0 && !iv->crossesCall) {
            for (std::int32_t r : callerOrder) {
                if (fits(busy[std::size_t(r)], *iv)) {
                    chosen = r;
                    break;
                }
            }
        }
        if (chosen < 0) {
            for (std::int32_t r : calleeOrder) {
                if (fits(busy[std::size_t(r)], *iv)) {
                    chosen = r;
                    break;
                }
            }
        }
        if (chosen < 0) {
            // A slot is found for it below; meanwhile it is in memory, which is all a partner asks.
            fn.location[std::size_t(iv->vreg)] = -1;
            spilled.push_back(iv);
            continue;
        }
        occupy(busy[std::size_t(chosen)], *iv);
        fn.location[std::size_t(iv->vreg)] = chosen;
        (fp ? fn.usedFpRegs : fn.usedIntRegs) |= std::uint64_t(1) << chosen;
    }
    // Slots, shared by what does not overlap from first use to last: in order of where each begins, a slot is free again
    // once what had it has ended. Holes in a life are not used, which a long function cannot afford the search for.
    std::sort(spilled.begin(), spilled.end(), [](const Interval* a, const Interval* b) { return a->start < b->start; });
    using Ending = std::pair<std::int32_t, std::uint32_t>;  // (end, slot)
    std::priority_queue<Ending, std::vector<Ending>, std::greater<Ending>> ending;
    std::vector<std::uint32_t> freeSlots;
    std::uint32_t slots = 0;
    for (Interval* iv : spilled) {
        while (!ending.empty() && ending.top().first < iv->start) {
            freeSlots.push_back(ending.top().second);
            ending.pop();
        }
        std::uint32_t slot;
        if (!freeSlots.empty()) {
            slot = freeSlots.back();
            freeSlots.pop_back();
        } else {
            slot = slots++;
        }
        ending.emplace(iv->end, slot);
        fn.location[std::size_t(iv->vreg)] = -std::int32_t(slot) - 1;
    }
    fn.spillSlots = slots;

    // What a release's cold path has to keep: registers of values live past it. Callee-saved ones keep themselves.
    std::uint64_t callerInt = 0, callerFp = 0;
    for (std::int32_t r : regs.intCallerSaved)
        callerInt |= std::uint64_t(1) << r;
    for (std::int32_t r : regs.fpCallerSaved)
        callerFp |= std::uint64_t(1) << r;
    fn.liveAtRelease.clear();
    // Only values in caller-saved registers count, and only while they are live: going through the code in order, those are
    // few at any point, however many values a long function has.
    std::vector<const Interval*> waiting;
    for (const Interval* iv : order) {
        const std::int32_t loc = fn.location[std::size_t(iv->vreg)];
        if (loc >= 0 && ((std::uint64_t(1) << loc) & (fn.vregIsFp[std::size_t(iv->vreg)] ? callerFp : callerInt)))
            waiting.push_back(iv);
    }
    std::sort(waiting.begin(), waiting.end(), [](const Interval* a, const Interval* b) { return a->start < b->start; });
    std::size_t nextWaiting = 0;
    std::vector<const Interval*> active;
    for (std::size_t i = 0; i < count; ++i) {
        IrInst& in = fn.code[i];
        const bool isColdCall = in.op == IrOp::Call && fn.calls[std::size_t(in.imm)].cold;
        if (in.op != IrOp::DecRef && in.op != IrOp::DecRefTagged && !isColdCall)
            continue;
        IrFunction::LiveRegs live;
        const std::int32_t pos = std::int32_t(i) * 2;
        while (nextWaiting < waiting.size() && waiting[nextWaiting]->start <= pos)
            active.push_back(waiting[nextWaiting++]);
        active.erase(std::remove_if(active.begin(), active.end(), [&](const Interval* iv) { return iv->end <= pos; }), active.end());
        for (const Interval* iv : active) {
            const std::int32_t loc = fn.location[std::size_t(iv->vreg)];
            if (!liveAcross(*iv, pos))
                continue;
            if (fn.vregIsFp[std::size_t(iv->vreg)])
                live.fps |= (std::uint64_t(1) << loc) & callerFp;
            else
                live.ints |= (std::uint64_t(1) << loc) & callerInt;
        }
        if (isColdCall) {
            fn.calls[std::size_t(in.imm)].liveIndex = std::int32_t(fn.liveAtRelease.size());
            const std::uint32_t words =
                std::uint32_t(__builtin_popcountll(live.ints)) + std::uint32_t(__builtin_popcountll(live.fps));
            fn.coldSaveWords = std::max(fn.coldSaveWords, words);
        } else {
            in.target = std::int32_t(fn.liveAtRelease.size());
        }
        fn.liveAtRelease.push_back(live);
    }
}

std::string dumpIr(const IrFunction& fn)
{
    static const char* const kNames[] = {
        "label", "mov", "movi", "movrt", "fmovi", "bits2fp", "fp2bits", "add", "sub", "mul", "and", "or", "xor", "shl", "sar",
        "shr", "sdiv", "udiv", "srem", "urem", "neg", "not", "sext8", "sext16", "sext32", "zext8", "zext16", "zext32",
        "fadd", "fsub", "fmul", "fdiv", "fneg", "fsqrt", "fabs", "ffloor", "fceil", "ftrunc", "fround", "fconv", "i2f",
        "f2i", "cset", "fcset", "br", "brcmp", "brfcmp", "exitcmp", "load", "loadidx", "store", "storeidx", "storetag",
        "storetagidx", "incref", "decref", "increft", "decreft", "tagof", "poll", "call", "ret", "trap", "osr", "frameaddr", "fp2intraw", "saveregs", "fp2int32js", "coldbegin", "coldend"};
    std::string out;
    char line[160];
    for (std::size_t i = 0; i < fn.code.size(); ++i) {
        const IrInst& in = fn.code[i];
        std::snprintf(line, sizeof line, "  %4zu %-10s w%d c%d d=%d a=%d b=%d c=%d t=%d imm=%lld%s\n", i,
                      kNames[std::size_t(in.op)], in.width, int(in.cond), in.d, in.a, in.b, in.c, in.target,
                      (long long)in.imm, in.hasImm ? " #" : "");
        out += line;
        if (in.op == IrOp::Call) {
            const IrCall& call = fn.calls[std::size_t(in.imm)];
            out += "         args";
            for (std::int32_t a : call.args)
                out += " " + std::to_string(a);
            out += " -> " + std::to_string(call.result) + "," + std::to_string(call.resultTag) + (call.cold ? " cold\n" : "\n");
        }
    }
    return out;
}

}  // namespace typed
}  // namespace internal
}  // namespace script
