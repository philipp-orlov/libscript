// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Loop optimizations over the IR: what does not change inside a loop moves out of it.
//
// Innermost loops only, and only loops with no call in them that comes back, which is where all of this pays and where it
// can be argued about without an alias analysis:
//
//   * a field of an object the loop does not reassign lives in a register for the length of the loop: read once before it
//     and, if the loop writes it, written back on every way out, a bail to the interpreter included, and before a release
//     in cold code (read again after it). The only places another reference could see it are another base at the same
//     offset, and that rules the field out; nothing else in such a loop can look;
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
    bool hot;  ///< isFp is what the hot path uses
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
        // Cold code may leave for good through a call (the dynamic engine's bail to the interpreter, which carries on from
        // memory), or release something (which may run a host's finalizer): the fields go back to memory first, and after
        // a release they are read again. Anything else that calls out is not for this pass.
        int nest = 0;
        for (std::int32_t i = loop.head; i <= loop.last; ++i) {
            const IrInst& in = fn_.code[std::size_t(i)];
            if (in.op == IrOp::ColdBegin)
                ++nest;
            else if (in.op == IrOp::ColdEnd)
                --nest;
            if (in.op == IrOp::Trap)
                return;
            if (in.op == IrOp::Call && (nest == 0 || !leavesFor(i)))
                return;
            if ((in.op == IrOp::DecRef || in.op == IrOp::DecRefTagged) && nest == 0)
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
        nest = 0;
        for (std::int32_t i = loop.head; i <= loop.last; ++i) {
            const IrInst& in = fn_.code[std::size_t(i)];
            if (in.op == IrOp::ColdBegin)
                ++nest;
            else if (in.op == IrOp::ColdEnd)
                --nest;
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
            // The register is of the kind the hot path uses; elsewhere the bits are moved across.
            auto it = fields.find(key);
            if (it == fields.end()) {
                Promoted field{in.a, in.imm, in.width, isFp, in.op == IrOp::Store, -1, nest == 0};
                fields.emplace(key, field);
            } else {
                it->second.written = it->second.written || in.op == IrOp::Store;
                if (it->second.width != in.width || (it->second.isFp != isFp && in.width != 8))
                    rejected.push_back(key);
                if (!it->second.hot && nest == 0) {
                    it->second.isFp = isFp;
                    it->second.hot = true;
                }
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
                    mov.width = 64;
                    if (in.op == IrOp::Load) {
                        mov.d = in.d;
                        mov.a = it->second.vreg;
                    } else {
                        mov.d = it->second.vreg;
                        mov.a = in.c;
                    }
                    const bool fromFp = fn_.vregIsFp[std::size_t(mov.a)] != 0;
                    const bool toFp = fn_.vregIsFp[std::size_t(mov.d)] != 0;
                    mov.op = fromFp == toFp ? IrOp::Mov : toFp ? IrOp::BitsToFp : IrOp::FpToBits;
                    out.push_back(mov);
                    continue;
                }
            }
            const bool release = in.op == IrOp::DecRef || in.op == IrOp::DecRefTagged;
            const bool leaving = (in.op == IrOp::Call && leavesFor(i)) ||
                                 (in.op == IrOp::Ret && fn_.code[std::size_t(i - 1)].op != IrOp::Call);
            if ((release || leaving) && anyWritten)
                emitGuarded(out, list, /*load=*/false);
            if (release) {
                out.push_back(in);
                emitGuarded(out, list, /*load=*/true);
                continue;
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

    /// A call that is the last thing before the method returns.
    bool leavesFor(std::int32_t at) const
    {
        return fn_.code[std::size_t(at)].op == IrOp::Call && std::size_t(at) + 1 < fn_.code.size() &&
               fn_.code[std::size_t(at) + 1].op == IrOp::Ret;
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

/// A branch to where execution would go anyway, past cold blocks that are laid out elsewhere, and then labels nothing
/// goes to: what follows them has one way in, so what was known before them is still known. An inlined function that
/// returns at its end is most of these.
class Straighten
{
public:
    explicit Straighten(IrFunction& fn) : fn_(fn) {}

    void run()
    {
        std::vector<IrInst>& code = fn_.code;
        std::vector<std::uint32_t> uses(std::size_t(fn_.labelCount), 0);
        const auto use = [&](std::int64_t label) {
            if (label >= 0 && std::size_t(label) < uses.size())
                ++uses[std::size_t(label)];
        };
        for (const IrInst& in : code)
            use(labelTargetOf(in));
        for (const IrExit& exit : fn_.exits)
            use(exit.handlerLabel);
        for (const IrOsrEntry& entry : fn_.osrEntries)
            use(entry.label);
        std::vector<std::uint8_t> removed(code.size(), 0);
        // What follows a branch or a return with no label between is never reached.
        for (std::size_t i = 0; i + 1 < code.size(); ++i) {
            if (code[i].op != IrOp::Br && code[i].op != IrOp::Ret)
                continue;
            for (std::size_t j = i + 1; j < code.size(); ++j) {
                const IrOp op = code[j].op;
                if (op == IrOp::Label || op == IrOp::ColdBegin || op == IrOp::ColdEnd || op == IrOp::OsrEntry ||
                    op == IrOp::Trap)
                    break;
                removed[j] = 1;
                const std::int64_t label = labelTargetOf(code[j]);
                if (label >= 0)
                    --uses[std::size_t(label)];
                i = j;
            }
        }
        int cold = 0;
        for (std::size_t i = 0; i < code.size(); ++i) {
            if (code[i].op == IrOp::ColdBegin)
                ++cold;
            else if (code[i].op == IrOp::ColdEnd)
                --cold;
            if (cold || code[i].op != IrOp::Br || removed[i])
                continue;
            std::size_t next = i + 1;
            int skipping = 0;
            for (; next < code.size(); ++next) {
                const IrOp op = code[next].op;
                if (removed[next])
                    continue;
                if (op == IrOp::ColdBegin)
                    ++skipping;
                else if (op == IrOp::ColdEnd)
                    --skipping;
                else if (!skipping && !(op == IrOp::Label && code[next].imm != code[i].imm))
                    break;
            }
            if (next < code.size() && code[next].op == IrOp::Label && code[next].imm == code[i].imm) {
                removed[i] = 1;
                --uses[std::size_t(code[i].imm)];
            }
        }
        std::vector<IrInst> out;
        out.reserve(code.size());
        for (std::size_t i = 0; i < code.size(); ++i) {
            if (removed[i] || (code[i].op == IrOp::Label && uses[std::size_t(code[i].imm)] == 0))
                continue;
            out.push_back(code[i]);
        }
        code.swap(out);
    }

private:
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
        findQuietLoops();
        findSingleEntries();
        for (std::int32_t position = 0; position < std::int32_t(fn_.code.size()); ++position) {
            while (!floors_.empty() && floors_.back().last < position)
                floors_.pop_back();
            IrInst in = fn_.code[std::size_t(position)];
            // What a register is a copy of makes two reads of the same place look the same.
            if (in.op != IrOp::Call && in.op != IrOp::Label && in.op != IrOp::Br) {
                in.a = originalOf(in.a);
                in.b = originalOf(in.b);
                in.c = originalOf(in.c);
            }
            switch (in.op) {
                case IrOp::Label:
                    // A join of a cold block that leaves everything as the hot path knows it says so.
                    if (const auto only = atBranch_.find(in.imm); only != atBranch_.end() && !fallsInto(position)) {
                        // Reached by one branch from above and nothing else: what held there holds here.
                        known_ = std::move(only->second);
                        atBranch_.erase(only);
                    } else if (in.alias == 0) {
                        const auto loop = quietLoops_.find(in.imm);
                        if (loop != quietLoops_.end()) {
                            keepThrough(loop->second);
                            floors_.push_back({loop->second.last, known_});
                        } else if (!floors_.empty()) {
                            // A join in such a loop: what holds all the way round it holds here too.
                            known_ = floors_.back().facts;
                        } else {
                            known_.clear();
                        }
                    }
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
                    if (in.width == 8 || in.width == 1)
                        known_.push_back(in);
                    break;
                case IrOp::BrCmp: {
                    if (singleEntry_.count(in.target))
                        atBranch_[in.target] = known_;
                    bool taken = false;
                    if (decided(in, taken)) {
                        if (!taken)
                            continue;
                        IrInst br;
                        br.op = IrOp::Br;
                        br.imm = in.target;
                        out.push_back(br);
                        continue;
                    }
                    // The same question, not taken a moment ago, wherever this one would have gone.
                    if (passedAlready(in))
                        continue;
                    known_.push_back(in);
                    break;
                }
                case IrOp::Mov:
                    killDefs(in);
                    if (in.d != in.a)
                        known_.push_back(in);
                    break;
                case IrOp::StoreIdx:
                    dropIndexed();
                    break;
                case IrOp::IncRefTagged:
                case IrOp::DecRefTagged: {
                    // A tag known to be no cell's: nothing to count.
                    std::int64_t tag = 0;
                    if (tagOf(in.b, tag) && std::uint32_t(tag) < std::uint32_t(Type::String))
                        continue;
                    break;
                }
                case IrOp::IncRef:
                case IrOp::DecRef: {
                    std::int64_t value = 0;
                    if (in.isNullable && constantOf(in.a, value) && value == 0)
                        continue;
                    break;
                }
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
                    if (in.op == IrOp::Load && !in.isSigned) {
                        // What was just stored there, instead of asking memory.
                        if (const IrInst* stored = findStore(in)) {
                            IrInst copy;
                            const bool fromFp = fn_.vregIsFp[std::size_t(stored->c)] != 0;
                            const bool toFp = fn_.vregIsFp[std::size_t(in.d)] != 0;
                            copy.op = fromFp == toFp ? IrOp::Mov : toFp ? IrOp::BitsToFp : IrOp::FpToBits;
                            copy.d = in.d;
                            copy.a = stored->c;
                            copy.width = 64;
                            killDefs(in);
                            if (copy.op == IrOp::Mov && copy.d != copy.a)
                                known_.push_back(copy);
                            out.push_back(copy);
                            continue;
                        }
                    }
                    if (isPure(in)) {
                        const IrInst* same = findSame(in);
                        if (same && same->d != in.d) {
                            const std::int32_t from = same->d;
                            IrInst copy;
                            copy.op = IrOp::Mov;
                            copy.d = in.d;
                            copy.a = from;
                            killDefs(in);
                            known_.push_back(copy);
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
    /// Labels after the one conditional branch that goes to them, and that nothing else enters.
    void findSingleEntries()
    {
        std::vector<std::int32_t> refs(std::size_t(fn_.labelCount), 0);
        std::vector<std::int32_t> from(std::size_t(fn_.labelCount), -1);
        for (std::size_t i = 0; i < fn_.code.size(); ++i) {
            const std::int64_t to = labelTargetOf(fn_.code[i]);
            if (to < 0)
                continue;
            ++refs[std::size_t(to)];
            from[std::size_t(to)] = fn_.code[i].op == IrOp::BrCmp ? std::int32_t(i) : -1;
        }
        for (const IrExit& exit : fn_.exits) {
            if (exit.handlerLabel >= 0)
                refs[std::size_t(exit.handlerLabel)] += 2;
        }
        for (const IrOsrEntry& entry : fn_.osrEntries)
            refs[std::size_t(entry.label)] += 2;
        for (std::size_t i = 0; i < fn_.code.size(); ++i) {
            const IrInst& in = fn_.code[i];
            if (in.op == IrOp::Label && refs[std::size_t(in.imm)] == 1 && from[std::size_t(in.imm)] >= 0 &&
                from[std::size_t(in.imm)] < std::int32_t(i))
                singleEntry_.insert(in.imm);
        }
    }

    /// Whether the instruction before a label carries on into it.
    bool fallsInto(std::int32_t position) const
    {
        if (position == 0)
            return true;
        const IrOp op = fn_.code[std::size_t(position - 1)].op;
        return op != IrOp::ColdBegin && op != IrOp::Br && op != IrOp::Ret && op != IrOp::Trap;
    }

    /// What a loop can change, for a loop that nothing else enters and that runs nothing it does not see. Facts about what
    /// is not in here hold all the way round it.
    struct LoopWrites
    {
        std::int32_t last = 0;
        std::set<std::int32_t> regs;
        std::set<std::int64_t> headerOffsets;  ///< where it stores into headers
        std::set<std::int64_t> valueOffsets;   ///< where it stores into values
        /// Of those, where every store is of one byte and one constant: a tag the loop only ever sets to that.
        std::map<std::int64_t, std::int64_t> constantBytes;
        std::set<std::int64_t> variedBytes;
        bool memory = false;                   ///< it stores with no more said about where
    };
    struct Floor
    {
        std::int32_t last;
        std::vector<IrInst> facts;
    };

    /// A loop head that is reached from above and from its own back branches only, whose body makes no call that comes
    /// back, and whose cold blocks leave things as they were or do not return. The lowering says which those are by giving
    /// the join a nonzero alias.
    void findQuietLoops()
    {
        // Registers with one definition, a constant.
        std::map<std::int32_t, std::uint32_t> defs;
        for (const IrInst& in : fn_.code) {
            if (in.d >= 0)
                ++defs[in.d];
            if (in.op == IrOp::Call) {
                const IrCall& call = fn_.calls[std::size_t(in.imm)];
                if (call.result >= 0)
                    ++defs[call.result];
                if (call.resultTag >= 0)
                    ++defs[call.resultTag];
            }
        }
        for (std::int32_t v : fn_.params)
            ++defs[v];
        for (const IrInst& in : fn_.code) {
            if (in.op == IrOp::MovImm && defs[in.d] == 1)
                constants_.emplace(in.d, in.imm);
        }
        std::vector<std::int32_t> labelAt(std::size_t(fn_.labelCount), -1);
        for (std::size_t i = 0; i < fn_.code.size(); ++i) {
            if (fn_.code[i].op == IrOp::Label)
                labelAt[std::size_t(fn_.code[i].imm)] = std::int32_t(i);
        }
        std::map<std::int64_t, std::int32_t> lastBack;
        for (std::size_t i = 0; i < fn_.code.size(); ++i) {
            const std::int64_t label = labelTargetOf(fn_.code[i]);
            if (label >= 0 && labelAt[std::size_t(label)] >= 0 && labelAt[std::size_t(label)] <= std::int32_t(i))
                lastBack[label] = std::int32_t(i);
        }
        for (const auto& [label, last] : lastBack) {
            const std::int32_t head = labelAt[std::size_t(label)];
            // A long loop is unlikely to go without a call, and this looks at everything in it.
            if (last - head > 4000)
                continue;
            LoopWrites writes;
            writes.last = last;
            if (loopIsQuiet(head, last, label, labelAt, writes))
                quietLoops_.emplace(label, std::move(writes));
        }
    }

    bool loopIsQuiet(std::int32_t head, std::int32_t last, std::int64_t label, const std::vector<std::int32_t>& labelAt, LoopWrites& writes) const
    {
        const auto define = [&](std::int32_t v) {
            if (v >= 0)
                writes.regs.insert(v);
        };
        int cold = 0;
        std::vector<std::uint8_t> callInCold(1, 0);
        for (std::int32_t i = head; i <= last; ++i) {
            const IrInst& in = fn_.code[std::size_t(i)];
            // Nothing from outside may land in the middle, other than what is cold and comes back from a cold call.
            if (in.op == IrOp::ColdBegin) {
                ++cold;
                callInCold.push_back(0);
                continue;
            }
            if (in.op == IrOp::ColdEnd) {
                if (cold)
                    --cold;
                callInCold.pop_back();
                continue;
            }
            define(in.d);
            if (in.op == IrOp::Call) {
                const IrCall& call = fn_.calls[std::size_t(in.imm)];
                if (!cold || !call.cold)
                    return false;
                callInCold.back() = 1;
                define(call.result);
                define(call.resultTag);
                continue;
            }
            const std::int64_t to = labelTargetOf(in);
            if (to >= 0 && cold) {
                // A cold block that carries on in the hot path, having called something, may have changed anything, unless
                // the place it comes back to says otherwise.
                const std::int32_t at = labelAt[std::size_t(to)];
                if (callInCold.back() && at >= 0 && fn_.code[std::size_t(at)].alias == 0)
                    return false;
            }
            // A cold block's stores too: one that carries on in the hot path has done them by then.
            if (in.op == IrOp::Store || in.op == IrOp::StoreTag || in.op == IrOp::StoreIdx) {
                if (in.op == IrOp::Store && in.alias == 1)
                    writes.headerOffsets.insert(in.imm);
                else if (in.op == IrOp::Store && in.alias == 2) {
                    writes.valueOffsets.insert(in.imm);
                    const auto constant = constants_.find(in.c);
                    if (in.width != 1 || constant == constants_.end()) {
                        writes.variedBytes.insert(in.imm);
                    } else {
                        const auto [at, added] = writes.constantBytes.emplace(in.imm, constant->second);
                        if (!added && at->second != constant->second)
                            writes.variedBytes.insert(in.imm);
                    }
                } else {
                    writes.memory = true;
                }
            }
        }
        // Branches from before or after the loop into it.
        std::set<std::int64_t> inside;
        for (std::int32_t i = head; i <= last; ++i) {
            if (fn_.code[std::size_t(i)].op == IrOp::Label)
                inside.insert(fn_.code[std::size_t(i)].imm);
        }
        for (std::int32_t i = 0; i < std::int32_t(fn_.code.size()); ++i) {
            if (i >= head && i <= last)
                continue;
            const std::int64_t to = labelTargetOf(fn_.code[std::size_t(i)]);
            if (to >= 0 && inside.count(to))
                return false;
        }
        for (const IrExit& exit : fn_.exits) {
            if (exit.handlerLabel >= 0 && inside.count(exit.handlerLabel))
                return false;
        }
        for (const IrOsrEntry& entry : fn_.osrEntries) {
            if (inside.count(entry.label))
                return false;
        }
        (void)label;
        return true;
    }

    /// Forget at a loop head what the loop may make untrue.
    void keepThrough(const LoopWrites& writes)
    {
        // A byte known to equal the one constant the loop ever stores there stays known: a store makes it that again.
        std::set<std::int32_t> stays;
        for (const IrInst& k : known_) {
            if (k.op != IrOp::Load || k.width != 1 || k.alias != 2 || writes.variedBytes.count(k.imm))
                continue;
            const auto constant = writes.constantBytes.find(k.imm);
            if (constant == writes.constantBytes.end())
                continue;
            for (const IrInst& test : known_) {
                if (test.op == IrOp::BrCmp && test.a == k.d && test.hasImm && test.cond == IrCond::Ne &&
                    test.imm == constant->second)
                    stays.insert(k.d);
            }
        }
        known_.erase(std::remove_if(known_.begin(), known_.end(),
                                    [&](const IrInst& k) {
                                        if (writes.regs.count(k.d) || writes.regs.count(k.a) || writes.regs.count(k.b) || writes.regs.count(k.c))
                                            return true;
                                        if (k.op == IrOp::LoadIdx || k.op == IrOp::Store)
                                            return true;
                                        if (k.op == IrOp::Load) {
                                            if (writes.memory)
                                                return true;
                                            if (k.alias == 1)
                                                return writes.headerOffsets.count(k.imm) != 0;
                                            if (k.alias == 2 && stays.count(k.d))
                                                return false;
                                            return k.alias != 2 || writes.valueOffsets.count(k.imm) != 0;
                                        }
                                        return false;
                                    }),
                     known_.end());
    }

    /// The register this one is a known copy of, if it is.
    std::map<std::int32_t, std::int64_t> constants_;
    std::set<std::int64_t> singleEntry_;
    std::map<std::int64_t, std::vector<IrInst>> atBranch_;

    std::int32_t originalOf(std::int32_t v) const noexcept
    {
        if (v < 0)
            return v;
        for (auto k = known_.rbegin(); k != known_.rend(); ++k) {
            if (k->d != v)
                continue;
            return k->op == IrOp::Mov ? k->a : v;
        }
        return v;
    }
    bool passedAlready(const IrInst& in) const noexcept
    {
        for (const IrInst& k : known_) {
            if (k.op == IrOp::BrCmp && k.a == in.a && k.b == in.b && k.cond == in.cond && k.width == in.width && k.hasImm == in.hasImm &&
                (!in.hasImm || k.imm == in.imm))
                return true;
        }
        return false;
    }

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
            case IrOp::BitsToFp:
            case IrOp::FpToBits:
            case IrOp::IntToFp:
            case IrOp::FpToIntRaw:
            case IrOp::FpToInt32Js:
            case IrOp::FAdd:
            case IrOp::FSub:
            case IrOp::FMul:
            case IrOp::Load:
            case IrOp::LoadIdx:
            case IrOp::Cset: return true;
            default: return false;
        }
    }
    static bool sameInst(const IrInst& a, const IrInst& b) noexcept
    {
        return a.op == b.op && a.a == b.a && a.b == b.b && a.c == b.c && a.imm == b.imm && a.width == b.width &&
               a.scale == b.scale && a.hasImm == b.hasImm && a.isSigned == b.isSigned && a.cond == b.cond &&
               a.srcWidth == b.srcWidth && a.isUnsigned == b.isUnsigned;
    }
    const IrInst* findSame(const IrInst& in) const noexcept
    {
        for (const IrInst& k : known_) {
            if (sameInst(k, in) && k.op != IrOp::ExitCmp && fn_.vregIsFp[std::size_t(k.d)] == fn_.vregIsFp[std::size_t(in.d)])
                return &k;
        }
        return nullptr;
    }
    const IrInst* findStore(const IrInst& in) const noexcept
    {
        for (const IrInst& k : known_) {
            if (k.op == IrOp::Store && k.a == in.a && k.imm == in.imm && k.width == in.width)
                return &k;
        }
        return nullptr;
    }
    /// The constant a register holds, when that is known here.
    /// A constant, or what a test passed a moment ago says the low 32 bits are: enough for a tag.
    bool tagOf(std::int32_t v, std::int64_t& value) const noexcept
    {
        if (constantOf(v, value))
            return true;
        for (const IrInst& k : known_) {
            if (k.op == IrOp::BrCmp && k.a == v && k.hasImm && k.cond == IrCond::Ne && (k.width == 32 || k.width == 64)) {
                value = k.imm;
                return true;
            }
        }
        return false;
    }

    bool constantOf(std::int32_t v, std::int64_t& value) const noexcept
    {
        for (int hops = 0; hops < 4; ++hops) {
            bool again = false;
            for (auto k = known_.rbegin(); k != known_.rend(); ++k) {
                if (k->d != v)
                    continue;
                if (k->op == IrOp::MovImm) {
                    value = k->imm;
                    return true;
                }
                if (k->op == IrOp::Mov) {
                    v = k->a;
                    again = true;
                }
                break;
            }
            if (!again)
                return false;
        }
        return false;
    }
    /// A comparison of a known constant with an immediate, which goes the same way every time.
    bool decided(const IrInst& in, bool& taken) const noexcept
    {
        std::int64_t a = 0;
        if (!in.hasImm || !constantOf(in.a, a))
            return false;
        std::int64_t b = in.imm;
        if (in.width == 32) {
            a = std::int32_t(a);
            b = std::int32_t(b);
        } else if (in.width != 64) {
            return false;
        }
        const auto ua = std::uint64_t(in.width == 32 ? std::uint32_t(a) : std::uint64_t(a));
        const auto ub = std::uint64_t(in.width == 32 ? std::uint32_t(b) : std::uint64_t(b));
        switch (in.cond) {
            case IrCond::Eq: taken = a == b; return true;
            case IrCond::Ne: taken = a != b; return true;
            case IrCond::Lt: taken = a < b; return true;
            case IrCond::Le: taken = a <= b; return true;
            case IrCond::Gt: taken = a > b; return true;
            case IrCond::Ge: taken = a >= b; return true;
            case IrCond::Lo: taken = ua < ub; return true;
            case IrCond::Ls: taken = ua <= ub; return true;
            case IrCond::Hi: taken = ua > ub; return true;
            case IrCond::Hs: taken = ua >= ub; return true;
            default: return false;
        }
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
        dropWhere([](const IrInst& k, std::int64_t) { return isLoad(k) || k.op == IrOp::Store; }, 0);
    }
    void dropIndexed()
    {
        dropWhere([](const IrInst& k, std::int64_t) { return k.op == IrOp::LoadIdx; }, 0);
    }
    void dropLoadsAt(std::int64_t offset, std::uint8_t alias)
    {
        known_.erase(std::remove_if(known_.begin(), known_.end(),
                                    [&](const IrInst& k) {
                                        return (k.op == IrOp::Load || k.op == IrOp::Store) && k.imm == offset &&
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
    std::map<std::int64_t, LoopWrites> quietLoops_;
    std::vector<Floor> floors_;
};

/// A constant, or a computation from what the loop does not change, is worked out once ahead of the loop. Calls do not
/// matter to these: they read no memory. Cold code keeps its own, not to take up a register all through the loop for it.
class LoopInvariants
{
public:
    explicit LoopInvariants(IrFunction& fn) : fn_(fn) {}

    void run()
    {
        std::vector<std::uint32_t> defCount(fn_.vregIsFp.size(), 0);
        for (const IrInst& in : fn_.code) {
            if (in.d >= 0)
                ++defCount[std::size_t(in.d)];
            if (in.op == IrOp::Call) {
                const IrCall& call = fn_.calls[std::size_t(in.imm)];
                if (call.result >= 0)
                    ++defCount[std::size_t(call.result)];
                if (call.resultTag >= 0)
                    ++defCount[std::size_t(call.resultTag)];
            }
        }
        for (std::int32_t v : fn_.params)
            ++defCount[std::size_t(v)];
        std::vector<std::int32_t> labelAt(std::size_t(fn_.labelCount), -1);
        for (std::size_t i = 0; i < fn_.code.size(); ++i) {
            if (fn_.code[i].op == IrOp::Label)
                labelAt[std::size_t(fn_.code[i].imm)] = std::int32_t(i);
        }
        // Each label branched back to, with its last such branch; last first, as rewriting moves what follows.
        std::map<std::int64_t, std::int32_t> lastBack;
        for (std::size_t i = 0; i < fn_.code.size(); ++i) {
            const std::int64_t label = labelTargetOf(fn_.code[i]);
            if (label >= 0 && labelAt[std::size_t(label)] >= 0 && labelAt[std::size_t(label)] <= std::int32_t(i))
                lastBack[label] = std::int32_t(i);
        }
        std::vector<std::pair<std::int32_t, std::int64_t>> loops;
        for (const auto& back : lastBack)
            loops.emplace_back(labelAt[std::size_t(back.first)], back.first);
        std::sort(loops.begin(), loops.end());
        // Innermost first would be better for nesting; going outermost last, what an outer loop can take moves again.
        for (auto it = loops.rbegin(); it != loops.rend(); ++it) {
            std::int32_t head = -1;
            std::int32_t last = -1;
            for (std::size_t i = 0; i < fn_.code.size(); ++i) {
                if (fn_.code[i].op == IrOp::Label && fn_.code[i].imm == it->second)
                    head = std::int32_t(i);
                if (labelTargetOf(fn_.code[i]) == it->second && head >= 0 && std::int32_t(i) >= head)
                    last = std::int32_t(i);
            }
            if (head >= 0 && last > head)
                hoist(head, last, it->second, defCount);
        }
    }

private:
    static bool movable(const IrInst& in) noexcept
    {
        switch (in.op) {
            case IrOp::MovImm:
            case IrOp::FMovImm:
            case IrOp::MovRuntime:
            case IrOp::BitsToFp:
            case IrOp::IntToFp:
            case IrOp::Sext32:
                return true;
            default: return false;
        }
    }

    void hoist(std::int32_t head, std::int32_t last, std::int64_t label, const std::vector<std::uint32_t>& defCount)
    {
        // Nothing but the loop's own may branch into its middle, or skip what is moved ahead of it.
        std::vector<std::uint8_t> labelInside(std::size_t(fn_.labelCount), 0);
        std::vector<std::uint8_t> definedInside(fn_.vregIsFp.size(), 0);
        // Across a real call only so many registers last, and a constant read from the frame is no better than one made
        // where it is needed. Nor are a few registers going to serve a loop of this size.
        if (last - head > 2000)
            return;
        for (std::int32_t i = head; i <= last; ++i) {
            const IrInst& in = fn_.code[std::size_t(i)];
            if (in.op == IrOp::Label)
                labelInside[std::size_t(in.imm)] = 1;
            if (in.d >= 0)
                definedInside[std::size_t(in.d)] = 1;
            if (in.op == IrOp::Call) {
                const IrCall& call = fn_.calls[std::size_t(in.imm)];
                if (!call.cold)
                    return;
                if (call.result >= 0)
                    definedInside[std::size_t(call.result)] = 1;
                if (call.resultTag >= 0)
                    definedInside[std::size_t(call.resultTag)] = 1;
            }
        }
        for (std::int32_t i = 0; i < std::int32_t(fn_.code.size()); ++i) {
            if (i >= head && i <= last)
                continue;
            const std::int64_t to = labelTargetOf(fn_.code[std::size_t(i)]);
            if (to >= 0 && labelInside[std::size_t(to)] && to != label)
                return;
        }
        for (const IrExit& exit : fn_.exits) {
            if (exit.handlerLabel >= 0 && labelInside[std::size_t(exit.handlerLabel)])
                return;
        }
        for (const IrOsrEntry& entry : fn_.osrEntries) {
            if (labelInside[std::size_t(entry.label)] && entry.label != label)
                return;
        }
        std::vector<IrInst> moved;
        std::vector<std::uint8_t> gone(std::size_t(last - head) + 1, 0);
        int nest = 0;
        for (std::int32_t i = head; i <= last; ++i) {
            const IrInst& in = fn_.code[std::size_t(i)];
            if (in.op == IrOp::ColdBegin)
                ++nest;
            else if (in.op == IrOp::ColdEnd)
                --nest;
            if (nest > 0 || !movable(in) || defCount[std::size_t(in.d)] != 1)
                continue;
            if ((in.a >= 0 && definedInside[std::size_t(in.a)]) || (in.b >= 0 && definedInside[std::size_t(in.b)]))
                continue;
            moved.push_back(in);
            definedInside[std::size_t(in.d)] = 0;
            gone[std::size_t(i - head)] = 1;
        }
        if (moved.empty())
            return;
        std::vector<IrInst> out;
        out.reserve(fn_.code.size() + 1);
        out.insert(out.end(), fn_.code.begin(), fn_.code.begin() + head);
        const std::int32_t preheader = fn_.labelCount++;
        IrInst mark;
        mark.op = IrOp::Label;
        mark.imm = preheader;
        out.push_back(mark);
        out.insert(out.end(), moved.begin(), moved.end());
        for (std::int32_t i = head; i < std::int32_t(fn_.code.size()); ++i) {
            if (i <= last && gone[std::size_t(i - head)])
                continue;
            out.push_back(fn_.code[std::size_t(i)]);
        }
        fn_.code.swap(out);
        for (IrOsrEntry& entry : fn_.osrEntries) {
            if (entry.label == label)
                entry.label = preheader;
        }
        // What comes to the loop from before it comes through the moved code.
        for (std::size_t i = 0; i < std::size_t(head); ++i) {
            IrInst& in = fn_.code[i];
            if (labelTargetOf(in) != label)
                continue;
            if (in.op == IrOp::Br)
                in.imm = preheader;
            else
                in.target = preheader;
        }
    }

    IrFunction& fn_;
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
            // `t = bits of x; ... [m] = t`: the store takes the floating point register as it is.
            if (in.op == IrOp::FpToBits && in.width == 64 && isTemp(in.d) && forwardBitsToStore(i, in.d, in.a))
                continue;
            // `t = [m]; ... x = from bits of t`: the load puts it in the floating point register to begin with.
            if (in.op == IrOp::BitsToFp && isTemp(in.a) && defs_[std::size_t(in.d)] == 1 && !pinned_[std::size_t(in.d)] &&
                loadAsFp(out, in.a, in.d))
                continue;
            out.push_back(in);
        }
        fn_.code.swap(out);
        renameCopies();
        for (bool changed = true; changed;)
            changed = removeDead();
    }

private:
    /// What computes a value nothing reads, and has no other effect, goes.
    bool removeDead()
    {
        count();
        const auto removable = [](const IrInst& in) {
            switch (in.op) {
                case IrOp::Mov:
                case IrOp::MovImm:
                case IrOp::MovRuntime:
                case IrOp::FMovImm:
                case IrOp::BitsToFp:
                case IrOp::FpToBits:
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
                case IrOp::FAdd:
                case IrOp::FSub:
                case IrOp::FMul:
                case IrOp::FDiv:
                case IrOp::IntToFp:
                case IrOp::FpToIntRaw:
                case IrOp::FpToInt32Js:
                case IrOp::Cset:
                case IrOp::FCset:
                case IrOp::Load:
                case IrOp::LoadIdx:
                case IrOp::FrameAddr: return true;
                default: return false;
            }
        };
        std::vector<IrInst> out;
        out.reserve(fn_.code.size());
        bool any = false;
        for (const IrInst& in : fn_.code) {
            if (in.d >= 0 && uses_[std::size_t(in.d)] == 0 && !pinned_[std::size_t(in.d)] && removable(in)) {
                any = true;
                continue;
            }
            out.push_back(in);
        }
        if (any)
            fn_.code.swap(out);
        return any;
    }

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

    bool forwardBitsToStore(std::size_t at, std::int32_t temp, std::int32_t source)
    {
        for (std::size_t j = at + 1; j < fn_.code.size(); ++j) {
            IrInst& next = fn_.code[j];
            if (next.op == IrOp::Label || next.op == IrOp::Br || next.op == IrOp::BrCmp || next.op == IrOp::BrFCmp ||
                next.op == IrOp::Ret || next.op == IrOp::Call || next.op == IrOp::ColdBegin || next.op == IrOp::ColdEnd)
                return false;
            if (next.a == temp || next.b == temp)
                return false;
            if (next.c == temp) {
                if ((next.op != IrOp::Store && next.op != IrOp::StoreIdx) || next.width != 8)
                    return false;
                next.c = source;
                return true;
            }
            if (next.d == source)
                return false;
        }
        return false;
    }

    bool loadAsFp(std::vector<IrInst>& out, std::int32_t temp, std::int32_t fp)
    {
        for (std::size_t j = out.size(); j-- > 0;) {
            IrInst& def = out[j];
            if (def.d != temp)
                continue;
            if ((def.op != IrOp::Load && def.op != IrOp::LoadIdx) || def.width != 8 || def.isSigned)
                return false;
            def.d = fp;
            return true;
        }
        return false;
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

/// A number goes through memory as a double: `x = load; f = bits of x` loads `f` itself, and `y = bits of f; store y`
/// stores `f`. Moving bits between the two register files is slower than the load, and a value stored and read back
/// round a loop pays for it twice an iteration.
class FpMemory
{
public:
    explicit FpMemory(IrFunction& fn) : fn_(fn) {}

    void run()
    {
        std::vector<std::pair<Known, Known>> saved;
        for (IrInst& in : fn_.code) {
            switch (in.op) {
                case IrOp::Label: known_ = Known(); continue;
                case IrOp::ColdBegin: saved.emplace_back(known_, Known()); continue;
                case IrOp::ColdEnd:
                    if (!saved.empty()) {
                        known_ = saved.back().first;
                        saved.pop_back();
                    }
                    continue;
                case IrOp::BitsToFp: {
                    const auto it = known_.loads.find(in.a);
                    if (it != known_.loads.end() && in.d != in.a) {
                        const std::int32_t d = in.d;
                        in = it->second;
                        in.d = d;
                    }
                    break;
                }
                case IrOp::Store:
                    if (in.width == 8 && !fn_.vregIsFp[std::size_t(in.c)]) {
                        const auto it = known_.bits.find(in.c);
                        if (it != known_.bits.end())
                            in.c = it->second;
                    }
                    known_.loads.clear();
                    continue;
                default: break;
            }
            if (in.op == IrOp::StoreIdx || in.op == IrOp::StoreTag || in.op == IrOp::StoreTagIdx || in.op == IrOp::Call)
                known_.loads.clear();
            forget(in);
            if (in.op == IrOp::Load && in.width == 8 && !in.isSigned && !fn_.vregIsFp[std::size_t(in.d)] && in.a != in.d)
                known_.loads[in.d] = in;
            if (in.op == IrOp::FpToBits && in.width == 64 && in.d != in.a)
                known_.bits[in.d] = in.a;
        }
    }

private:
    struct Known
    {
        std::map<std::int32_t, IrInst> loads;      ///< an integer register, and the load it holds the result of
        std::map<std::int32_t, std::int32_t> bits;  ///< an integer register, and the double it holds the bits of
    };

    /// A register written makes whatever was known of it, or through it, stale.
    void forget(const IrInst& in)
    {
        std::vector<std::int32_t> written;
        if (in.d >= 0)
            written.push_back(in.d);
        if (in.op == IrOp::Call) {
            const IrCall& call = fn_.calls[std::size_t(in.imm)];
            written.push_back(call.result);
            written.push_back(call.resultTag);
        }
        for (std::int32_t v : written) {
            if (v < 0)
                continue;
            known_.loads.erase(v);
            known_.bits.erase(v);
            for (auto it = known_.loads.begin(); it != known_.loads.end();)
                it = it->second.a == v ? known_.loads.erase(it) : std::next(it);
            for (auto it = known_.bits.begin(); it != known_.bits.end();)
                it = it->second == v ? known_.bits.erase(it) : std::next(it);
        }
    }

    IrFunction& fn_;
    Known known_;
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
    Straighten(fn).run();
    CommonSubexpressions(fn).run();
    LoopOptimizer(fn).run();
    LoopInvariants(fn).run();
    LoopRotation(fn).run();
    CopyPropagation(fn).run();
    FpMemory(fn).run();
}

}  // namespace typed
}  // namespace internal
}  // namespace script
