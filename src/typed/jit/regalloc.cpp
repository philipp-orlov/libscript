// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Linear scan register allocation.
//
// Each virtual register gets one location for its whole life: a machine register, or a spill slot. That is cruder than
// splitting intervals, and the point is that it is enough: the values worth a register are the ones in loops, and the
// spill choice below is by how often a value is touched, weighted by loop depth, rather than by how far off its next use
// is.
#include <algorithm>
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

struct Interval
{
    std::int32_t vreg = -1;
    std::int32_t start = 0x7fffffff;
    std::int32_t end = -1;
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
                starts[i + 1] = 1;
                break;
            default: break;
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
        block.use = block.def = block.liveIn = block.liveOut = BitSet(vregs);
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

    // ---- liveness ----
    for (Block& block : blocks) {
        for (std::int32_t i = block.first; i <= block.last; ++i) {
            const IrInst& in = fn.code[std::size_t(i)];
            visitOperands(
                fn, in,
                [&](std::int32_t v) {
                    if (!block.def.test(std::size_t(v)))
                        block.use.set(std::size_t(v));
                },
                [](std::int32_t) {});
            if (const IrExit* exit = exitOf(in)) {
                const auto read = [&](std::int32_t v) {
                    if (!block.def.test(std::size_t(v)))
                        block.use.set(std::size_t(v));
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
            visitOperands(fn, in, [](std::int32_t) {}, [&](std::int32_t v) { block.def.set(std::size_t(v)); });
        }
    }
    for (bool changed = true; changed;) {
        changed = false;
        for (std::size_t b = blocks.size(); b-- > 0;) {
            Block& block = blocks[b];
            for (std::int32_t s : block.successors)
                changed = block.liveOut.merge(blocks[std::size_t(s)].liveIn) || changed;
            changed = block.liveIn.merge(block.use) || changed;
            changed = block.liveIn.mergeMinus(block.liveOut, block.def) || changed;
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

    // ---- intervals ----
    // Position 2i is where instruction i reads, 2i + 1 where it writes, so a register whose last read is at i can be
    // the one instruction i writes.
    std::vector<Interval> intervals(vregs);
    for (std::size_t v = 0; v < vregs; ++v)
        intervals[v].vreg = std::int32_t(v);
    const auto extend = [&](std::int32_t v, std::int32_t pos) {
        Interval& iv = intervals[std::size_t(v)];
        iv.start = std::min(iv.start, pos);
        iv.end = std::max(iv.end, pos);
    };
    std::vector<std::int32_t> callPositions;
    static const double kDepthWeight[] = {1, 8, 64, 512, 4096, 32768, 262144};
    for (const Block& block : blocks) {
        block.liveIn.forEach([&](std::size_t v) { extend(std::int32_t(v), block.first * 2); });
        block.liveOut.forEach([&](std::size_t v) { extend(std::int32_t(v), block.last * 2 + 2); });
        for (std::int32_t i = block.first; i <= block.last; ++i) {
            const IrInst& in = fn.code[std::size_t(i)];
            const double weight = kDepthWeight[depth[std::size_t(i)]];
            visitOperands(
                fn, in,
                [&](std::int32_t v) {
                    extend(v, i * 2);
                    intervals[std::size_t(v)].weight += weight;
                },
                [&](std::int32_t v) {
                    extend(v, i * 2 + 1);
                    intervals[std::size_t(v)].weight += weight;
                });
            if (const IrExit* exit = exitOf(in)) {
                for (std::int32_t v : exit->cleanup)
                    extend(v, i * 2 + 1 + (in.op == IrOp::Call ? 1 : 0));
                for (const auto& tagged : exit->cleanupTagged) {
                    extend(tagged.first, i * 2 + 1 + (in.op == IrOp::Call ? 1 : 0));
                    extend(tagged.second, i * 2 + 1 + (in.op == IrOp::Call ? 1 : 0));
                }
                if (exit->indexVreg >= 0)
                    extend(exit->indexVreg, i * 2 + 1);
                for (const IrExit::WriteBack& back : exit->writeBacks) {
                    extend(back.base, i * 2 + 1);
                    extend(back.vreg, i * 2 + 1);
                }
            }
            if (in.op == IrOp::Call)
                callPositions.push_back(i * 2);
            // Raising on the way to a handler in this method calls the runtime, and the handler carries on after it.
            else if (in.op == IrOp::ExitCmp && fn.exits[std::size_t(in.target)].handlerLabel >= 0)
                callPositions.push_back(i * 2);
        }
    }
    fn.makesCalls = false;
    for (const IrInst& in : fn.code)
        fn.makesCalls = fn.makesCalls || in.op == IrOp::Call;
    // What arrives at entry is live from before the first instruction.
    for (std::int32_t p : fn.params)
        extend(p, -1);
    for (std::int32_t c : fn.captures)
        extend(c, -1);
    for (std::int32_t z : fn.osrZero)
        extend(z, -1);
    // Entering at a loop head writes only what is live there; anything else may share a register with something that is.
    for (IrOsrEntry& entry : fn.osrEntries) {
        const std::int32_t at = labelAt[std::size_t(entry.label)];
        const BitSet& live = blocks[std::size_t(blockOfInst[std::size_t(at)])].liveIn;
        entry.slots.erase(std::remove_if(entry.slots.begin(), entry.slots.end(),
                                         [&](const IrOsrEntry::Slot& s) {
                                             return !live.test(std::size_t(s.vreg)) &&
                                                    (s.tagVreg < 0 || !live.test(std::size_t(s.tagVreg)));
                                         }),
                          entry.slots.end());
    }

    // A value that has to survive a call needs a callee-saved register or a slot.
    for (Interval& iv : intervals) {
        if (iv.end < 0)
            continue;
        const auto it = std::upper_bound(callPositions.begin(), callPositions.end(), iv.start);
        iv.crossesCall = it != callPositions.end() && *it < iv.end;
    }
    // An incoming parameter gets its own register's value handed over at entry with no move if nothing stands in the
    // way; the same goes for an argument computed right before the call that takes it.
    {
        std::int32_t nextInt = 0, nextFp = 0;
        for (std::int32_t p : fn.params) {
            if (fn.vregIsFp[std::size_t(p)]) {
                if (nextFp < 8)
                    intervals[std::size_t(p)].hint = nextFp;
                ++nextFp;
            } else {
                if (nextInt < 8)
                    intervals[std::size_t(p)].hint = nextInt;
                ++nextInt;
            }
        }
        for (const IrCall& call : fn.calls) {
            std::int32_t intArg = 0, fpArg = 0;
            for (std::int32_t arg : call.args) {
                if (fn.vregIsFp[std::size_t(arg)]) {
                    if (fpArg < 8 && intervals[std::size_t(arg)].hint < 0)
                        intervals[std::size_t(arg)].hint = fpArg;
                    ++fpArg;
                } else {
                    if (intArg < 8 && intervals[std::size_t(arg)].hint < 0)
                        intervals[std::size_t(arg)].hint = intArg;
                    ++intArg;
                }
            }
            if (call.result >= 0 && intervals[std::size_t(call.result)].hint < 0)
                intervals[std::size_t(call.result)].hint = 0;
        }
    }

    std::vector<Interval*> order;
    for (Interval& iv : intervals) {
        if (iv.end >= 0)
            order.push_back(&iv);
    }
    std::sort(order.begin(), order.end(), [](const Interval* a, const Interval* b) { return a->start < b->start; });

    // ---- the scan ----
    fn.location.assign(vregs, IrFunction::kUnused);
    struct Active
    {
        Interval* iv;
        std::int32_t reg;
    };
    std::vector<Active> activeInt, activeFp;
    std::uint64_t freeInt = 0, freeFp = 0;
    std::uint64_t calleeMaskInt = 0, calleeMaskFp = 0;
    for (std::int32_t r : regs.intCallerSaved)
        freeInt |= std::uint64_t(1) << r;
    for (std::int32_t r : regs.intCalleeSaved) {
        freeInt |= std::uint64_t(1) << r;
        calleeMaskInt |= std::uint64_t(1) << r;
    }
    for (std::int32_t r : regs.fpCallerSaved)
        freeFp |= std::uint64_t(1) << r;
    for (std::int32_t r : regs.fpCalleeSaved) {
        freeFp |= std::uint64_t(1) << r;
        calleeMaskFp |= std::uint64_t(1) << r;
    }

    std::uint32_t spillSlots = 0;
    std::vector<std::uint32_t> freeSlots;

    for (Interval* iv : order) {
        const bool fp = fn.vregIsFp[std::size_t(iv->vreg)] != 0;
        std::vector<Active>& active = fp ? activeFp : activeInt;
        std::uint64_t& free = fp ? freeFp : freeInt;
        const std::uint64_t calleeMask = fp ? calleeMaskFp : calleeMaskInt;
        const std::vector<std::int32_t>& callerOrder = fp ? regs.fpCallerSaved : regs.intCallerSaved;
        const std::vector<std::int32_t>& calleeOrder = fp ? regs.fpCalleeSaved : regs.intCalleeSaved;

        for (std::size_t i = 0; i < active.size();) {
            // An interval that ends where this one starts gives its register up: its last read comes first.
            if (active[i].iv->end <= iv->start) {
                free |= std::uint64_t(1) << active[i].reg;
                active[i] = active.back();
                active.pop_back();
            } else {
                ++i;
            }
        }

        std::int32_t chosen = -1;
        const auto isFree = [&](std::int32_t r) { return (free >> r) & 1u; };
        if (!iv->crossesCall && iv->hint >= 0 && isFree(iv->hint))
            chosen = iv->hint;
        if (chosen < 0 && !iv->crossesCall) {
            for (std::int32_t r : callerOrder) {
                if (isFree(r)) {
                    chosen = r;
                    break;
                }
            }
        }
        if (chosen < 0) {
            for (std::int32_t r : calleeOrder) {
                if (isFree(r)) {
                    chosen = r;
                    break;
                }
            }
        }
        if (chosen < 0) {
            // Nothing free. Whoever is touched least per unit of length goes to memory: this one, or an active one whose
            // register this interval is allowed to take.
            const auto density = [](const Interval* i) { return i->weight / double(i->end - i->start + 1); };
            Active* victim = nullptr;
            for (Active& a : active) {
                if (iv->crossesCall && !((calleeMask >> a.reg) & 1u))
                    continue;
                if (!victim || density(a.iv) < density(victim->iv))
                    victim = &a;
            }
            if (victim && density(victim->iv) < density(iv)) {
                chosen = victim->reg;
                fn.location[std::size_t(victim->iv->vreg)] = -std::int32_t(spillSlots++) - 1;
                *victim = active.back();
                active.pop_back();
                free |= std::uint64_t(1) << chosen;
            }
        }
        if (chosen < 0) {
            fn.location[std::size_t(iv->vreg)] = -std::int32_t(spillSlots++) - 1;
            continue;
        }
        free &= ~(std::uint64_t(1) << chosen);
        fn.location[std::size_t(iv->vreg)] = chosen;
        active.push_back({iv, chosen});
        (fp ? fn.usedFpRegs : fn.usedIntRegs) |= std::uint64_t(1) << chosen;
    }
    fn.spillSlots = spillSlots;

    // What a release's cold path has to keep: registers of values live past it. Callee-saved ones keep themselves.
    std::uint64_t callerInt = 0, callerFp = 0;
    for (std::int32_t r : regs.intCallerSaved)
        callerInt |= std::uint64_t(1) << r;
    for (std::int32_t r : regs.fpCallerSaved)
        callerFp |= std::uint64_t(1) << r;
    fn.liveAtRelease.clear();
    for (std::size_t i = 0; i < count; ++i) {
        IrInst& in = fn.code[i];
        if (in.op != IrOp::DecRef && in.op != IrOp::DecRefTagged)
            continue;
        IrFunction::LiveRegs live;
        const std::int32_t pos = std::int32_t(i) * 2;
        for (const Interval* iv : order) {
            const std::int32_t loc = fn.location[std::size_t(iv->vreg)];
            if (loc < 0 || iv->start > pos || iv->end <= pos)
                continue;
            if (fn.vregIsFp[std::size_t(iv->vreg)])
                live.fps |= (std::uint64_t(1) << loc) & callerFp;
            else
                live.ints |= (std::uint64_t(1) << loc) & callerInt;
        }
        in.target = std::int32_t(fn.liveAtRelease.size());
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
        "storetagidx", "incref", "decref", "increft", "decreft", "tagof", "poll", "call", "ret", "trap", "osr"};
    std::string out;
    char line[160];
    for (std::size_t i = 0; i < fn.code.size(); ++i) {
        const IrInst& in = fn.code[i];
        std::snprintf(line, sizeof line, "  %4zu %-10s w%d c%d d=%d a=%d b=%d c=%d t=%d imm=%lld%s\n", i,
                      kNames[std::size_t(in.op)], in.width, int(in.cond), in.d, in.a, in.b, in.c, in.target,
                      (long long)in.imm, in.hasImm ? " #" : "");
        out += line;
    }
    return out;
}

}  // namespace typed
}  // namespace internal
}  // namespace script
