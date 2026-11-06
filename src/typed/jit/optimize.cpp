// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Loop optimizations over the IR: what does not change inside a loop moves out of it.
//
// Innermost loops only, and only loops with no call in them, which is where all of this pays and where it can be argued
// about without an alias analysis:
//
//   * a field of an object the loop does not reassign lives in a register for the length of the loop: read once before it
//     and, if the loop writes it, written back on every way out. The only places another reference could see it are
//     another base at the same offset, and that rules the field out; nothing else in a call-free loop can look;
//   * a floating point constant is materialized once before the loop.
//
// A loop head an on-stack replacement enters is handled by moving the entry to the new preheader.
#include <algorithm>
#include <map>
#include <set>
#include <vector>

#include "script/internal/typed/jit_ir.hpp"

namespace script {
namespace internal {
namespace typed {

namespace {

struct Loop
{
    std::int32_t head;  ///< index of the label
    std::int32_t last;  ///< index of the last back branch
    std::int32_t label;
};

std::int64_t labelTargetOf(const IrInst& in) noexcept
{
    if (in.op == IrOp::Br)
        return in.imm;
    if (in.op == IrOp::BrCmp || in.op == IrOp::BrFCmp)
        return in.target;
    return -1;
}

struct Promoted
{
    std::int32_t base;
    std::int64_t offset;
    std::uint8_t width;
    bool isFp;
    bool written;
    std::int32_t vreg;
};

class LoopOptimizer
{
public:
    explicit LoopOptimizer(IrFunction& fn) : fn_(fn) {}

    void run()
    {
        // One at a time, last first: rewriting a loop moves everything after it, and leaves what is before it alone.
        std::vector<Loop> loops = innermostLoops();
        std::sort(loops.begin(), loops.end(), [](const Loop& a, const Loop& b) { return a.head > b.head; });
        for (const Loop& loop : loops)
            optimize(loop);
    }

private:
    std::vector<Loop> innermostLoops() const
    {
        std::vector<std::int32_t> labelAt(std::size_t(fn_.labelCount), -1);
        for (std::size_t i = 0; i < fn_.code.size(); ++i) {
            if (fn_.code[i].op == IrOp::Label)
                labelAt[std::size_t(fn_.code[i].imm)] = std::int32_t(i);
        }
        std::map<std::int64_t, Loop> byLabel;
        for (std::size_t i = 0; i < fn_.code.size(); ++i) {
            const std::int64_t label = labelTargetOf(fn_.code[i]);
            if (label < 0 || labelAt[std::size_t(label)] < 0 || labelAt[std::size_t(label)] > std::int32_t(i))
                continue;
            Loop loop{labelAt[std::size_t(label)], std::int32_t(i), std::int32_t(label)};
            const auto it = byLabel.find(label);
            if (it == byLabel.end())
                byLabel.emplace(label, loop);
            else
                it->second.last = std::max(it->second.last, loop.last);
        }
        std::vector<Loop> out;
        for (const auto& candidate : byLabel) {
            bool innermost = true;
            for (const auto& other : byLabel) {
                if (&other == &candidate)
                    continue;
                if (other.second.head > candidate.second.head && other.second.last <= candidate.second.last)
                    innermost = false;
            }
            if (innermost)
                out.push_back(candidate.second);
        }
        return out;
    }

    void optimize(const Loop& loop)
    {
        // What the loop is made of.
        std::vector<std::uint8_t> definedInside(fn_.vregIsFp.size(), 0);
        std::vector<std::uint32_t> defCount(fn_.vregIsFp.size(), 0);
        for (const IrInst& in : fn_.code) {
            if (in.d >= 0)
                ++defCount[std::size_t(in.d)];
            if (in.op == IrOp::Call) {
                const std::int32_t r = fn_.calls[std::size_t(in.imm)].result;
                if (r >= 0)
                    ++defCount[std::size_t(r)];
                const std::int32_t t = fn_.calls[std::size_t(in.imm)].resultTag;
                if (t >= 0)
                    ++defCount[std::size_t(t)];
            }
        }
        std::vector<std::uint8_t> labelInside(std::size_t(fn_.labelCount), 0);
        for (std::int32_t i = loop.head; i <= loop.last; ++i) {
            const IrInst& in = fn_.code[std::size_t(i)];
            if (in.op == IrOp::Call || in.op == IrOp::DecRef || in.op == IrOp::DecRefTagged || in.op == IrOp::Trap)
                return;
            if (in.d >= 0)
                definedInside[std::size_t(in.d)] = 1;
            if (in.op == IrOp::Label)
                labelInside[std::size_t(in.imm)] = 1;
        }
        // Nothing may jump into the middle: the preheader would not be on its way.
        for (std::int32_t i = 0; i < std::int32_t(fn_.code.size()); ++i) {
            if (i >= loop.head && i <= loop.last)
                continue;
            const std::int64_t label = labelTargetOf(fn_.code[std::size_t(i)]);
            if (label >= 0 && labelInside[std::size_t(label)] && label != loop.label)
                return;
        }

        // ---- which fields ----
        std::map<std::pair<std::int32_t, std::int64_t>, Promoted> fields;
        std::map<std::int64_t, std::vector<std::int32_t>> basesAtOffset;
        std::vector<std::pair<std::int32_t, std::int64_t>> rejected;
        for (std::int32_t i = loop.head; i <= loop.last; ++i) {
            const IrInst& in = fn_.code[std::size_t(i)];
            if (in.op != IrOp::Load && in.op != IrOp::Store)
                continue;
            // A byte load is a tag: this is a field of a type that needs one, written by a helper this loop has none of,
            // and which a promotion of the payload alone would tear from it.
            const auto key = std::make_pair(in.a, in.imm);
            std::vector<std::int32_t>& bases = basesAtOffset[in.imm];
            if (std::find(bases.begin(), bases.end(), in.a) == bases.end())
                bases.push_back(in.a);
            const std::int32_t value = in.op == IrOp::Load ? in.d : in.c;
            const bool isFp = fn_.vregIsFp[std::size_t(value)] != 0;
            if (definedInside[std::size_t(in.a)] || (in.width != 4 && in.width != 8) || in.isSigned) {
                rejected.push_back(key);
                continue;
            }
            auto it = fields.find(key);
            if (it == fields.end()) {
                Promoted field{in.a, in.imm, in.width, isFp, in.op == IrOp::Store, -1};
                fields.emplace(key, field);
            } else {
                it->second.written = it->second.written || in.op == IrOp::Store;
                if (it->second.width != in.width || it->second.isFp != isFp)
                    rejected.push_back(key);
            }
        }
        for (const auto& key : rejected)
            fields.erase(key);
        // A tag store says this is a reference field being written, which is not for this pass; and two bases that are
        // the same object would each see a stale copy once either writes.
        for (std::int32_t i = loop.head; i <= loop.last; ++i) {
            const IrInst& in = fn_.code[std::size_t(i)];
            if (in.op == IrOp::StoreTag)
                fields.erase(std::make_pair(in.a, in.imm + 8));
        }
        for (auto it = fields.begin(); it != fields.end();) {
            const std::vector<std::int32_t>& bases = basesAtOffset[it->second.offset];
            bool alone = bases.size() == 1;
            if (!alone) {
                alone = true;
                for (std::int32_t base : bases) {
                    const auto other = fields.find(std::make_pair(base, it->second.offset));
                    const bool writes = other != fields.end() && other->second.written;
                    if (writes || it->second.written)
                        alone = false;
                }
                // A write through a base that was rejected counts too.
                for (std::int32_t i = loop.head; i <= loop.last && alone; ++i) {
                    const IrInst& in = fn_.code[std::size_t(i)];
                    if (in.op == IrOp::Store && in.imm == it->second.offset)
                        alone = false;
                }
            }
            if (alone)
                ++it;
            else
                it = fields.erase(it);
        }
        for (auto& entry : fields)
            entry.second.vreg = fn_.newVreg(entry.second.isFp);

        // ---- constants ----
        std::vector<IrInst> hoisted;
        for (std::int32_t i = loop.head; i <= loop.last; ++i) {
            IrInst& in = fn_.code[std::size_t(i)];
            if (in.op != IrOp::FMovImm || defCount[std::size_t(in.d)] != 1)
                continue;
            const auto same = std::find_if(hoisted.begin(), hoisted.end(), [&](const IrInst& other) {
                return other.imm == in.imm && other.width == in.width;
            });
            if (same != hoisted.end()) {
                // The same constant again is a copy of the first, which copy propagation then removes.
                const std::int32_t d = in.d;
                const std::int32_t first = same->d;
                in = IrInst();
                in.op = IrOp::Mov;
                in.d = d;
                in.a = first;
                continue;
            }
            hoisted.push_back(in);
            in.op = IrOp::Label;  // replaced below
            in.imm = -1;
        }
        if (fields.empty() && hoisted.empty())
            return;

        // ---- the rewrite ----
        std::vector<IrInst> out;
        out.reserve(fn_.code.size() + fields.size() * 4 + 8);
        for (std::int32_t i = 0; i < loop.head; ++i)
            out.push_back(fn_.code[std::size_t(i)]);

        // A preheader of its own, which an on-stack replacement enters too.
        const std::int32_t preheader = fn_.labelCount++;
        pushLabel(out, preheader);
        for (const IrInst& in : hoisted)
            out.push_back(in);
        const std::vector<const Promoted*> list = sorted(fields);
        emitGuarded(out, list, /*load=*/true);
        out.push_back(fn_.code[std::size_t(loop.head)]);

        // Ways out write back. An exit is copied: outside this loop it must not touch these registers.
        std::map<std::int32_t, std::int32_t> exitCopies;
        std::map<std::int64_t, std::int32_t> edgeLabels;
        std::vector<IrInst> tail;
        const bool anyWritten = std::any_of(list.begin(), list.end(), [](const Promoted* p) { return p->written; });

        const auto exitFor = [&](std::int32_t exit) {
            if (!anyWritten)
                return exit;
            const auto it = exitCopies.find(exit);
            if (it != exitCopies.end())
                return it->second;
            IrExit copy = fn_.exits[std::size_t(exit)];
            for (const Promoted* p : list) {
                if (p->written)
                    copy.writeBacks.push_back({p->base, p->offset, p->vreg, p->width});
            }
            fn_.exits.push_back(std::move(copy));
            const std::int32_t index = std::int32_t(fn_.exits.size()) - 1;
            exitCopies.emplace(exit, index);
            return index;
        };

        for (std::int32_t i = loop.head + 1; i <= loop.last; ++i) {
            IrInst in = fn_.code[std::size_t(i)];
            if (in.op == IrOp::Label && in.imm == -1)
                continue;
            if (in.op == IrOp::Load || in.op == IrOp::Store) {
                const auto it = fields.find(std::make_pair(in.a, in.imm));
                if (it != fields.end()) {
                    IrInst mov;
                    mov.op = IrOp::Mov;
                    if (in.op == IrOp::Load) {
                        mov.d = in.d;
                        mov.a = it->second.vreg;
                    } else {
                        mov.d = it->second.vreg;
                        mov.a = in.c;
                    }
                    out.push_back(mov);
                    continue;
                }
            }
            if (in.op == IrOp::ExitCmp || in.op == IrOp::Poll)
                in.target = exitFor(in.target);
            const std::int64_t label = labelTargetOf(in);
            if (label >= 0 && !labelInside[std::size_t(label)] && anyWritten) {
                // Leaving by a branch: through a little block that writes back.
                auto it = edgeLabels.find(label);
                if (it == edgeLabels.end()) {
                    const std::int32_t edge = fn_.labelCount++;
                    it = edgeLabels.emplace(label, edge).first;
                    pushLabel(tail, edge);
                    emitGuarded(tail, list, /*load=*/false);
                    IrInst br;
                    br.op = IrOp::Br;
                    br.imm = label;
                    tail.push_back(br);
                }
                if (in.op == IrOp::Br)
                    in.imm = it->second;
                else
                    in.target = it->second;
            }
            out.push_back(in);
        }
        // Falling out of the bottom, past a conditional back branch.
        if (anyWritten && fn_.code[std::size_t(loop.last)].op != IrOp::Br)
            emitGuarded(out, list, /*load=*/false);
        for (std::size_t i = std::size_t(loop.last) + 1; i < fn_.code.size(); ++i)
            out.push_back(fn_.code[i]);
        out.insert(out.end(), tail.begin(), tail.end());
        fn_.code.swap(out);

        for (IrOsrEntry& entry : fn_.osrEntries) {
            if (entry.label == loop.label)
                entry.label = preheader;
        }
        // Whatever reached the loop by a branch from before it comes through the preheader.
        for (std::size_t i = 0; i < fn_.code.size(); ++i) {
            IrInst& in = fn_.code[i];
            if (labelTargetOf(in) != loop.label)
                continue;
            if (in.op == IrOp::Label)
                continue;
            if (std::int32_t(i) < loop.head) {
                if (in.op == IrOp::Br)
                    in.imm = preheader;
                else
                    in.target = preheader;
            }
        }
    }

    static std::vector<const Promoted*> sorted(const std::map<std::pair<std::int32_t, std::int64_t>, Promoted>& fields)
    {
        std::vector<const Promoted*> out;
        for (const auto& entry : fields)
            out.push_back(&entry.second);
        return out;
    }

    static void pushLabel(std::vector<IrInst>& out, std::int32_t label)
    {
        IrInst in;
        in.op = IrOp::Label;
        in.imm = label;
        out.push_back(in);
    }

    /// Loads or stores, per base and only when the base is not null: the loop's own null check still decides whether
    /// anything raises, and when.
    void emitGuarded(std::vector<IrInst>& out, const std::vector<const Promoted*>& list, bool load)
    {
        std::size_t i = 0;
        while (i < list.size()) {
            const std::int32_t base = list[i]->base;
            bool any = false;
            for (std::size_t j = i; j < list.size() && list[j]->base == base; ++j)
                any = any || load || list[j]->written;
            std::size_t j = i;
            if (any) {
                const std::int32_t skip = fn_.labelCount++;
                IrInst guard;
                guard.op = IrOp::BrCmp;
                guard.a = base;
                guard.cond = IrCond::Eq;
                guard.width = 64;
                guard.hasImm = true;
                guard.imm = 0;
                guard.target = skip;
                out.push_back(guard);
                for (; j < list.size() && list[j]->base == base; ++j) {
                    if (!load && !list[j]->written)
                        continue;
                    IrInst mem;
                    mem.op = load ? IrOp::Load : IrOp::Store;
                    mem.a = base;
                    mem.imm = list[j]->offset;
                    mem.width = list[j]->width;
                    if (load)
                        mem.d = list[j]->vreg;
                    else
                        mem.c = list[j]->vreg;
                    out.push_back(mem);
                }
                pushLabel(out, skip);
            } else {
                while (j < list.size() && list[j]->base == base)
                    ++j;
            }
            i = j;
        }
    }

    IrFunction& fn_;
};

/// Within a basic block, the same computation twice is one computation and a copy; the same check twice is one check. What
/// makes a load the same is that nothing between could have written there: a store at that offset, or a call.
class CommonSubexpressions
{
public:
    explicit CommonSubexpressions(IrFunction& fn) : fn_(fn) {}

    void run()
    {
        std::vector<IrInst> out;
        out.reserve(fn_.code.size());
        for (const IrInst& original : fn_.code) {
            IrInst in = original;
            switch (in.op) {
                case IrOp::Label:
                    known_.clear();
                    break;
                case IrOp::ColdBegin:
                    // The hot path goes from before the cold block straight to after it, so what it knew it knows again.
                    saved_.push_back(known_);
                    break;
                case IrOp::ColdEnd:
                    if (!saved_.empty()) {
                        known_ = std::move(saved_.back());
                        saved_.pop_back();
                    }
                    break;
                case IrOp::Call:
                    // A call may have written anything, so no load survives; a constant or a sum does.
                    dropMemory();
                    killDefs(in);
                    break;
                case IrOp::Store:
                    dropLoadsAt(in.imm, in.alias);
                    dropIndexed();
                    break;
                case IrOp::StoreIdx:
                    dropIndexed();
                    break;
                case IrOp::StoreTag:
                    dropLoadsAt(in.imm, in.alias);
                    break;
                case IrOp::ExitCmp:
                    if (isCheck(in)) {
                        if (findCheck(in))
                            continue;
                        known_.push_back(in);
                    }
                    break;
                default:
                    if (isPure(in)) {
                        const IrInst* same = findSame(in);
                        if (same && same->d != in.d) {
                            const std::int32_t from = same->d;
                            IrInst copy;
                            copy.op = IrOp::Mov;
                            copy.d = in.d;
                            copy.a = from;
                            killDefs(in);
                            out.push_back(copy);
                            continue;
                        }
                        const bool selfReferencing = in.d == in.a || in.d == in.b;
                        killDefs(in);
                        if (!selfReferencing)
                            known_.push_back(in);
                        out.push_back(in);
                        continue;
                    }
                    killDefs(in);
                    break;
            }
            out.push_back(in);
        }
        fn_.code.swap(out);
    }

private:
    static bool isLoad(const IrInst& in) noexcept { return in.op == IrOp::Load || in.op == IrOp::LoadIdx; }
    static bool isCheck(const IrInst& in) noexcept
    {
        // A comparison that always goes the same way is how a method leaves on purpose.
        return in.a != in.b;
    }
    static bool isPure(const IrInst& in) noexcept
    {
        switch (in.op) {
            case IrOp::MovImm:
            case IrOp::MovRuntime:
            case IrOp::FMovImm:
            case IrOp::Add:
            case IrOp::Sub:
            case IrOp::Mul:
            case IrOp::And:
            case IrOp::Or:
            case IrOp::Xor:
            case IrOp::Shl:
            case IrOp::Sar:
            case IrOp::Shr:
            case IrOp::Sext8:
            case IrOp::Sext16:
            case IrOp::Sext32:
            case IrOp::Zext8:
            case IrOp::Zext16:
            case IrOp::Zext32:
            case IrOp::Load:
            case IrOp::LoadIdx:
            case IrOp::Cset: return true;
            default: return false;
        }
    }
    static bool sameInst(const IrInst& a, const IrInst& b) noexcept
    {
        return a.op == b.op && a.a == b.a && a.b == b.b && a.c == b.c && a.imm == b.imm && a.width == b.width &&
               a.scale == b.scale && a.hasImm == b.hasImm && a.isSigned == b.isSigned && a.cond == b.cond;
    }
    const IrInst* findSame(const IrInst& in) const noexcept
    {
        for (const IrInst& k : known_) {
            if (sameInst(k, in) && k.op != IrOp::ExitCmp && fn_.vregIsFp[std::size_t(k.d)] == fn_.vregIsFp[std::size_t(in.d)])
                return &k;
        }
        return nullptr;
    }
    bool findCheck(const IrInst& in) const noexcept
    {
        for (const IrInst& k : known_) {
            if (k.op == IrOp::ExitCmp && sameInst(k, in))
                return true;
        }
        return false;
    }
    void dropWhere(bool (*test)(const IrInst&, std::int64_t), std::int64_t arg)
    {
        known_.erase(std::remove_if(known_.begin(), known_.end(), [&](const IrInst& k) { return test(k, arg); }), known_.end());
    }
    void dropMemory()
    {
        dropWhere([](const IrInst& k, std::int64_t) { return isLoad(k); }, 0);
    }
    void dropIndexed()
    {
        dropWhere([](const IrInst& k, std::int64_t) { return k.op == IrOp::LoadIdx; }, 0);
    }
    void dropLoadsAt(std::int64_t offset, std::uint8_t alias)
    {
        known_.erase(std::remove_if(known_.begin(), known_.end(),
                                    [&](const IrInst& k) {
                                        return k.op == IrOp::Load && k.imm == offset &&
                                               (k.alias == 0 || alias == 0 || k.alias == alias);
                                    }),
                     known_.end());
    }
    void kill(std::int32_t v)
    {
        known_.erase(std::remove_if(known_.begin(), known_.end(),
                                    [&](const IrInst& k) { return k.d == v || k.a == v || k.b == v || k.c == v; }),
                     known_.end());
    }
    void killDefs(const IrInst& in)
    {
        if (in.d >= 0)
            kill(in.d);
        if (in.op == IrOp::Call) {
            const IrCall& call = fn_.calls[std::size_t(in.imm)];
            if (call.result >= 0)
                kill(call.result);
            if (call.resultTag >= 0)
                kill(call.resultTag);
        }
    }

    IrFunction& fn_;
    std::vector<IrInst> known_;
    std::vector<std::vector<IrInst>> saved_;
};

/// Removes copies: `t = a op b; x = t` becomes `x = a op b`, and `t = x; ... use t` uses `x`, wherever `t` is a temporary
/// with that one definition and that one use and nothing changes `x` between.
class CopyPropagation
{
public:
    explicit CopyPropagation(IrFunction& fn) : fn_(fn) {}

    void run()
    {
        count();
        std::vector<IrInst> out;
        out.reserve(fn_.code.size());
        for (std::size_t i = 0; i < fn_.code.size(); ++i) {
            IrInst in = fn_.code[i];
            // `t = a < b; if (t != 0) goto x`
            if (in.op == IrOp::BrCmp && in.hasImm && in.imm == 0 && !out.empty() && out.back().d == in.a && isTemp(in.a) &&
                (in.cond == IrCond::Eq || in.cond == IrCond::Ne)) {
                const IrInst& test = out.back();
                if (test.op == IrOp::Cset) {
                    IrInst fused = test;
                    fused.op = IrOp::BrCmp;
                    fused.d = -1;
                    fused.cond = in.cond == IrCond::Ne ? test.cond : invert(test.cond);
                    fused.target = in.target;
                    out.back() = fused;
                    continue;
                }
                if (test.op == IrOp::FCset && in.cond == IrCond::Ne) {
                    IrInst fused = test;
                    fused.op = IrOp::BrFCmp;
                    fused.d = -1;
                    fused.target = in.target;
                    out.back() = fused;
                    continue;
                }
            }
            // `t = ...; x = t`
            if (in.op == IrOp::Mov && !out.empty() && isTemp(in.a) && out.back().d == in.a && out.back().op != IrOp::Call &&
                fn_.vregIsFp[std::size_t(in.a)] == fn_.vregIsFp[std::size_t(in.d)] && isPlainDef(out.back())) {
                out.back().d = in.d;
                continue;
            }
            // `t = x; ... use t`, within the block, with no write to `x` or `t` on the way.
            if (in.op == IrOp::Mov && isTemp(in.d) && forwardCopy(i, in.d, in.a))
                continue;
            out.push_back(in);
        }
        fn_.code.swap(out);
        renameCopies();
    }

private:
    /// `t = x` where each is written exactly once: `x` is already there wherever `t` is used, so `t` becomes `x`.
    void renameCopies()
    {
        count();
        std::vector<std::int32_t> rename(fn_.vregIsFp.size(), -1);
        std::vector<std::int32_t> defAt(fn_.vregIsFp.size(), -1);
        for (std::size_t i = 0; i < fn_.code.size(); ++i) {
            if (fn_.code[i].d >= 0 && fn_.code[i].op != IrOp::Call)
                defAt[std::size_t(fn_.code[i].d)] = std::int32_t(i);
        }
        const auto reads = [&](const IrInst& in, std::int32_t v) {
            if (in.op == IrOp::Call) {
                const IrCall& call = fn_.calls[std::size_t(in.imm)];
                return call.delegate == v || std::find(call.args.begin(), call.args.end(), v) != call.args.end();
            }
            return in.a == v || in.b == v || in.c == v;
        };
        bool any = false;
        for (std::size_t i = 0; i < fn_.code.size(); ++i) {
            const IrInst& in = fn_.code[i];
            if (in.op != IrOp::Mov || in.d == in.a || defs_[std::size_t(in.d)] != 1 || defs_[std::size_t(in.a)] != 1 ||
                pinned_[std::size_t(in.d)] || pinned_[std::size_t(in.a)] ||
                fn_.vregIsFp[std::size_t(in.d)] != fn_.vregIsFp[std::size_t(in.a)])
                continue;
            // `x` is written before the copy, and nothing between reads `t`, which there still holds the `x` of a previous
            // time round a loop.
            const std::int32_t from = defAt[std::size_t(in.a)];
            if (from < 0 || std::size_t(from) >= i)
                continue;
            bool clean = true;
            for (std::size_t j = std::size_t(from) + 1; j < i && clean; ++j)
                clean = !reads(fn_.code[j], in.d);
            if (!clean)
                continue;
            rename[std::size_t(in.d)] = in.a;
            any = true;
        }
        if (!any)
            return;
        const auto to = [&](std::int32_t v) {
            while (v >= 0 && rename[std::size_t(v)] >= 0)
                v = rename[std::size_t(v)];
            return v;
        };
        std::vector<IrInst> out;
        out.reserve(fn_.code.size());
        for (IrInst in : fn_.code) {
            if (in.op == IrOp::Mov && in.d >= 0 && rename[std::size_t(in.d)] >= 0)
                continue;
            if (in.op == IrOp::Call) {
                IrCall& call = fn_.calls[std::size_t(in.imm)];
                for (std::int32_t& v : call.args)
                    v = to(v);
                call.delegate = to(call.delegate);
            } else {
                in.a = to(in.a);
                in.b = to(in.b);
                in.c = to(in.c);
            }
            out.push_back(in);
        }
        fn_.code.swap(out);
    }

    bool isTemp(std::int32_t v) const noexcept
    {
        return defs_[std::size_t(v)] == 1 && uses_[std::size_t(v)] == 1 && !pinned_[std::size_t(v)];
    }
    static bool isPlainDef(const IrInst& in) noexcept
    {
        switch (in.op) {
            case IrOp::Label:
            case IrOp::Call:
            case IrOp::Br:
            case IrOp::BrCmp:
            case IrOp::BrFCmp:
            case IrOp::ColdBegin:
            case IrOp::ColdEnd:
            case IrOp::Ret: return false;
            default: return true;
        }
    }

    void count()
    {
        defs_.assign(fn_.vregIsFp.size(), 0);
        uses_.assign(fn_.vregIsFp.size(), 0);
        pinned_.assign(fn_.vregIsFp.size(), 0);
        for (std::int32_t v : fn_.params)
            pinned_[std::size_t(v)] = 1;
        for (std::int32_t v : fn_.captures)
            pinned_[std::size_t(v)] = 1;
        for (std::int32_t v : fn_.osrArguments)
            pinned_[std::size_t(v)] = 1;
        for (const IrOsrEntry& entry : fn_.osrEntries) {
            for (const IrOsrEntry::Slot& slot : entry.slots) {
                pinned_[std::size_t(slot.vreg)] = 1;
                if (slot.tagVreg >= 0)
                    pinned_[std::size_t(slot.tagVreg)] = 1;
            }
            for (const IrOsrEntry::Slot& slot : entry.pointers)
                pinned_[std::size_t(slot.vreg)] = 1;
        }
        for (const IrExit& exit : fn_.exits) {
            for (std::int32_t v : exit.cleanup)
                pinned_[std::size_t(v)] = 1;
            for (const auto& tagged : exit.cleanupTagged) {
                pinned_[std::size_t(tagged.first)] = 1;
                pinned_[std::size_t(tagged.second)] = 1;
            }
            if (exit.indexVreg >= 0)
                pinned_[std::size_t(exit.indexVreg)] = 1;
            for (const IrExit::WriteBack& back : exit.writeBacks) {
                pinned_[std::size_t(back.base)] = 1;
                pinned_[std::size_t(back.vreg)] = 1;
            }
        }
        for (const IrInst& in : fn_.code) {
            if (in.op == IrOp::Call) {
                const IrCall& call = fn_.calls[std::size_t(in.imm)];
                for (std::int32_t v : call.args)
                    ++uses_[std::size_t(v)];
                if (call.delegate >= 0)
                    ++uses_[std::size_t(call.delegate)];
                if (call.result >= 0)
                    ++defs_[std::size_t(call.result)];
                if (call.resultTag >= 0)
                    ++defs_[std::size_t(call.resultTag)];
                continue;
            }
            if (in.a >= 0)
                ++uses_[std::size_t(in.a)];
            if (in.b >= 0)
                ++uses_[std::size_t(in.b)];
            if (in.c >= 0)
                ++uses_[std::size_t(in.c)];
            if (in.d >= 0)
                ++defs_[std::size_t(in.d)];
        }
    }

    bool forwardCopy(std::size_t at, std::int32_t temp, std::int32_t source)
    {
        for (std::size_t j = at + 1; j < fn_.code.size(); ++j) {
            IrInst& next = fn_.code[j];
            if (next.op == IrOp::Label || next.op == IrOp::Br || next.op == IrOp::BrCmp || next.op == IrOp::BrFCmp ||
                next.op == IrOp::Ret || next.op == IrOp::Call || next.op == IrOp::ColdBegin || next.op == IrOp::ColdEnd)
                return false;
            bool used = false;
            if (next.a == temp) {
                next.a = source;
                used = true;
            }
            if (next.b == temp) {
                next.b = source;
                used = true;
            }
            if (next.c == temp) {
                next.c = source;
                used = true;
            }
            if (used)
                return true;
            if (next.d == source)
                return false;
        }
        return false;
    }

    IrFunction& fn_;
    std::vector<std::uint32_t> defs_, uses_;
    std::vector<std::uint8_t> pinned_;
};

/// `head: if (!c) goto out; body; goto head` becomes `if (!c) goto out; body: ...; if (c) goto body; goto out`: one branch
/// taken per iteration instead of two. A null check at the very start of the body runs once, between the two tests: what
/// it tests does not change in the loop.
class LoopRotation
{
public:
    explicit LoopRotation(IrFunction& fn) : fn_(fn) {}

    void run()
    {
        for (bool again = true; again;) {
            again = false;
            for (std::size_t i = 0; i + 1 < fn_.code.size(); ++i) {
                if (fn_.code[i].op == IrOp::Label && !done_.count(fn_.code[i].imm) && rotate(i)) {
                    again = true;
                    break;
                }
            }
        }
    }

private:
    bool rotate(std::size_t head)
    {
        const std::int64_t label = fn_.code[head].imm;
        done_.insert(label);
        const IrInst test = fn_.code[head + 1];
        if (test.op != IrOp::BrCmp)
            return false;
        // The back branch: the last plain branch to this label, with the test's operands unchanged meaning at that point
        // because the test reads them afresh.
        std::size_t back = 0;
        std::uint32_t backs = 0;
        for (std::size_t i = head + 2; i < fn_.code.size(); ++i) {
            const IrInst& in = fn_.code[i];
            if ((in.op == IrOp::Br && in.imm == label) ||
                ((in.op == IrOp::BrCmp || in.op == IrOp::BrFCmp) && in.target == label)) {
                ++backs;
                if (in.op == IrOp::Br)
                    back = i;
            }
        }
        if (backs != 1 || back == 0)
            return false;

        const std::int32_t body = fn_.labelCount++;
        IrInst rotated = test;
        rotated.cond = invert(test.cond);
        rotated.target = body;
        IrInst leave;
        leave.op = IrOp::Br;
        leave.imm = test.target;

        std::vector<std::uint8_t> definedInside(fn_.vregIsFp.size(), 0);
        for (std::size_t i = head; i <= back; ++i) {
            if (fn_.code[i].d >= 0)
                definedInside[std::size_t(fn_.code[i].d)] = 1;
            if (fn_.code[i].op == IrOp::Call && fn_.calls[std::size_t(fn_.code[i].imm)].result >= 0)
                definedInside[std::size_t(fn_.calls[std::size_t(fn_.code[i].imm)].result)] = 1;
            if (fn_.code[i].op == IrOp::Call && fn_.calls[std::size_t(fn_.code[i].imm)].resultTag >= 0)
                definedInside[std::size_t(fn_.calls[std::size_t(fn_.code[i].imm)].resultTag)] = 1;
        }

        std::vector<IrInst> out(fn_.code.begin(), fn_.code.begin() + std::ptrdiff_t(head + 2));
        std::size_t next = head + 2;
        // Null checks of what the loop does not change, ahead of the body.
        while (next < back) {
            const IrInst& in = fn_.code[next];
            if (in.op != IrOp::ExitCmp || !in.hasImm || in.imm != 0 || in.cond != IrCond::Eq || in.width != 64 ||
                definedInside[std::size_t(in.a)] || !fn_.exits[std::size_t(in.target)].writeBacks.empty())
                break;
            out.push_back(in);
            ++next;
        }
        IrInst bodyLabel;
        bodyLabel.op = IrOp::Label;
        bodyLabel.imm = body;
        done_.insert(body);
        out.push_back(bodyLabel);
        out.insert(out.end(), fn_.code.begin() + std::ptrdiff_t(next), fn_.code.begin() + std::ptrdiff_t(back));
        out.push_back(rotated);
        out.push_back(leave);
        out.insert(out.end(), fn_.code.begin() + std::ptrdiff_t(back + 1), fn_.code.end());
        fn_.code.swap(out);
        return true;
    }

    IrFunction& fn_;
    std::set<std::int64_t> done_;
};

}  // namespace

void optimizeLoops(IrFunction& fn)
{
    CommonSubexpressions(fn).run();
    LoopOptimizer(fn).run();
    LoopRotation(fn).run();
    CopyPropagation(fn).run();
}

}  // namespace typed
}  // namespace internal
}  // namespace script
