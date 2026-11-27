// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The compiler of native matchers (arm64).
//
// Fixed registers while a matcher runs:
//   rbx  the input        rbp  its length      r12  the position
//   r13  the registers    r14  the stack's top r15  where the stack is nearly full     r21  the context     r22  the code
//
// The stack is the matcher's (entries of two or three words, a tag in the last), so the instructions with no template here
// are done by `regex::step`. What an entry names is an index: of an instruction to go on at, or (a greedy loop's) of the
// code that gives one unit back. `table` says where the code for it is.
//
// The gain over the matcher is in what is not pushed. A choice whose first alternative cannot begin with the next unit is
// not made, and one whose second cannot leaves nothing to come back to; a greedy loop that is followed by something its
// own units cannot begin keeps no way back either.
#include "regexp_jit.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <unistd.h>

#include "script/internal/dyn/regexp_program.hpp"

#if defined(__aarch64__) && defined(__linux__)
#include "asm_a64.hpp"
#include "code_region.hpp"
#define SCRIPT_VM_REGEXP_JIT 1
#endif

namespace script {
namespace internal {
namespace vm {
namespace jit {

namespace regex = dyn::regex;

#if defined(SCRIPT_VM_REGEXP_JIT)

namespace {

using namespace dyn::regex;

constexpr std::uint32_t kUndo = 0x80000000u, kGreedy = 0x40000000u, kGreedyCodes = 0x20000000u;
constexpr std::uint32_t kIndexMask = 0x1fffffffu;
/// Room every push may count on past the limit, in words.
constexpr std::size_t kRoom = 16;
constexpr std::uint32_t kHot = 12, kNoNative = 0x40000000u;

struct Ctx
{
    const char16_t* s;
    std::uint32_t length;
    std::uint32_t at;
    std::int32_t* regs;
    std::uint32_t* stack;
    std::uint32_t* limit;
    // What a call out finds and leaves.
    std::uint32_t pos;
    std::uint32_t pad;
    std::uint32_t* sp;
    std::uint64_t spill[3];
    const Program* program;
    Matcher* matcher;
};

void setStack(Ctx* c) noexcept
{
    c->stack = c->matcher->stack.data();
    c->limit = c->stack + c->matcher->stack.size() - kRoom;
}

/// Twice the room: false when there is no memory for it.
extern "C" bool scriptRegexGrow(Ctx* c) noexcept
{
    const std::ptrdiff_t used = c->sp - c->stack;
    try {
        c->matcher->stack.resize(c->matcher->stack.size() * 2);
    } catch (...) {
        return false;
    }
    setStack(c);
    c->sp = c->stack + used;
    return true;
}

extern "C" bool scriptRegexStep(Ctx* c, std::uint32_t pc) noexcept
{
    StepState st{c->program, c->matcher, c->s, std::int32_t(c->length), std::int32_t(c->pos), c->regs, c->stack,
                 std::size_t(c->sp - c->stack)};
    const bool ok = step(st, pc);
    c->pos = std::uint32_t(st.pos);
    c->sp = c->stack + st.sp;
    return ok;
}

extern "C" bool scriptRegexHas(const CodeSet* set, std::uint32_t unit) noexcept { return set->has(unit); }

extern "C" std::uint32_t scriptRegexFind(Ctx* c, std::uint32_t at, std::uint32_t unit) noexcept
{
    return std::uint32_t(findUnit(c->s, std::int32_t(at), std::int32_t(c->length), char16_t(unit)));
}

/// What can come first at an instruction: units below 256 by the bit, the others together.
struct First
{
    std::uint32_t low[8] = {};
    bool high = false;
    bool open = false;  ///< anything, or nothing at all
    /// When every unit above 255 that can come first is the lead surrogate `lead` of a code point: the trail units
    /// (less 0xDC00) that can follow it. `mixed`: not so (another unit above 255, or code points of several leads).
    std::uint32_t lead = 0;
    bool mixed = false;
    std::uint32_t trail[32] = {};

    void add(std::uint32_t unit)
    {
        if (unit < 256) {
            low[unit >> 5] |= 1u << (unit & 31);
        } else {
            high = true;
            mixed = true;
        }
    }
    void addPoints(char32_t lo, char32_t hi)
    {
        if (lo < 0x10000 || hi - lo >= 1024) {
            mixed = true;
            return;
        }
        for (char32_t c = lo; c <= hi && !mixed; ++c) {
            const std::uint32_t l = 0xD800 + ((c - 0x10000) >> 10), t = (c - 0x10000) & 0x3FF;
            if (lead && lead != l)
                mixed = true;
            lead = l;
            trail[t >> 5] |= 1u << (t & 31);
        }
    }
    void add(const CodeSet& set)
    {
        for (int k = 0; k < 8; ++k)
            low[k] |= set.low[k];
        high = high || !set.high.empty();
        for (const auto& [lo, hi] : set.high)
            addPoints(lo, hi);
    }
    void add(const First& o)
    {
        for (int k = 0; k < 8; ++k)
            low[k] |= o.low[k];
        high = high || o.high;
        open = open || o.open;
        mixed = mixed || o.mixed || (lead && o.lead && lead != o.lead);
        if (!lead)
            lead = o.lead;
        for (int k = 0; k < 32; ++k)
            trail[k] |= o.trail[k];
    }
    /// Units above 255 begin only code points of one lead, which the trail tells apart.
    bool astral() const { return high && !mixed && lead; }
    bool hasTrail(std::uint32_t t) const { return (trail[t >> 5] >> (t & 31) & 1) != 0; }
    bool has(std::uint32_t unit) const { return unit < 256 ? (low[unit >> 5] >> (unit & 31) & 1) != 0 : high; }
    /// `this` and `o` hold no unit in common.
    bool apartFrom(const First& o) const
    {
        if (open || o.open || (high && o.high))
            return false;
        for (int k = 0; k < 8; ++k)
            if (low[k] & o.low[k])
                return false;
        return true;
    }
};

struct Code : NativeMatcher
{
    CodeSpan span;
    std::uint32_t (*run)(Ctx*) = nullptr;
    std::vector<std::uint64_t> table;  ///< for an index: where its code is
    std::deque<std::array<std::uint8_t, 256>> bytes;
    std::deque<std::array<std::uint8_t, 1024>> trails;  ///< by the trail unit less 0xDC00, as `bytes` by the unit
    ~Code() override
    {
        if (span)
            releaseCode(span);
    }
};

class Compiler
{
public:
    explicit Compiler(const Program& program)
        : p_(program), code_(program.code), n_(std::uint32_t(program.code.size())), at_(n_ + 1), target_(n_ + 1, 0)
    {
    }

    Code* compile()
    {
        if (n_ > 6000)
            return nullptr;
        out_ = std::make_unique<Code>();
        out_->table.assign(2 * std::size_t(n_) + 1, 0);
        for (std::uint32_t pc = 0; pc < n_; pc += size(pc))
            markTargets(pc);
        planClean();
        prologue();
        for (std::uint32_t pc = 0; pc < n_; pc += size(pc)) {
            if (target_[pc])
                known_.clear();
            if (clean_[pc])
                known_.assign(1, Entry{false, n_});  // only the entry of the next place to begin
            a_.bind(at_[pc]);
            offsets_.emplace_back(pc, a_.here());
            emit(pc);
        }
        tails();
        if (a_.failed() || a_.outOfReach())
            return nullptr;
        for (const auto& [index, offset] : offsets_)
            out_->table[index] = std::uint64_t(offset);
        out_->span = allocateCode(a_.buf.size());
        if (!out_->span)
            return nullptr;
        for (std::uint64_t& t : out_->table)
            t += reinterpret_cast<std::uint64_t>(out_->span.rx);
        commitCode(out_->span, a_.buf.data(), a_.buf.size());
#if defined(SCRIPT_JIT_ENV_OVERRIDE)
        if (std::getenv("SCRIPT_VM_PERFMAP"))
            if (std::FILE* map = std::fopen(("/tmp/perf-" + std::to_string(getpid()) + ".map").c_str(), "a")) {
                static std::atomic<int> serial{0};
                std::fprintf(map, "%llx %zx regexp:%d:ops%u\n", static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(out_->span.rx)),
                             a_.buf.size(), ++serial, n_);
                std::fclose(map);
            }
#endif
        out_->run = reinterpret_cast<std::uint32_t (*)(Ctx*)>(out_->span.rx);
        return out_.release();
    }

private:
    struct Entry
    {
        bool undo;
        std::uint32_t what;  ///< the register to put back, or the instruction to go on at
    };
    struct Stub
    {
        Label in;
        std::vector<Entry> entries;  ///< top last; the last but none is the choice
    };
    struct Later
    {
        Label in;
        std::function<void()> emit;
    };

    std::uint32_t size(std::uint32_t pc) const
    {
        switch (code_[pc]) {
            case OpMatch: case OpLineStart: case OpLineStartM: case OpLineEnd: case OpLineEndM: return 1;
            case OpLit2: case OpLit2Back: case OpStr: case OpClear: case OpWord: case OpNegBegin: return 3;
            case OpGreedy: case OpGreedyU: case OpCheckCounted: return 4;
            case OpLoopEnter: return 6;
            default: return 2;
        }
    }
    void markTargets(std::uint32_t pc)
    {
        switch (code_[pc]) {
            case OpSplit: case OpJump: target_[code_[pc + 1]] = 1; break;
            case OpSplitLazy: target_[code_[pc + 1]] = 1; target_[pc + 2] = 1; break;
            case OpLoopEnter: target_[code_[pc + 5]] = 1; target_[pc + 6] = 1; break;
            case OpNegBegin: target_[code_[pc + 2]] = 1; break;
            case OpGreedy: case OpGreedyU: target_[pc + 4] = 1; break;
            default: break;
        }
    }

    // ---- what every match has ----
    /// Whether every match has `unit` (then `unit2`, when a string gives it one) at `lo` to `hi` units from where it
    /// begins: the program begins with pieces of a fixed width (a unit, a string, a capture, an assertion, or a Split of
    /// two such ways that meet), then a literal.
    bool unitAhead(std::uint32_t* lo, std::uint32_t* hi, std::uint32_t* unit, std::uint32_t* unit2) const
    {
        // The width of the way from `pc` to where it meets `join` (a Jump to it, or arriving at it); false if not fixed.
        const auto width = [&](std::uint32_t pc, std::uint32_t* join, std::uint32_t* w) {
            *w = 0;
            for (int k = 0; k < 16 && pc < n_; ++k) {
                if (*join != ~0u && pc == *join)
                    return true;
                switch (code_[pc]) {
                    case OpSave: case OpLineStart: case OpLineStartM: case OpLineEnd: case OpLineEndM: case OpWord: break;
                    case OpLit: case OpLit2: case OpSet: ++*w; break;
                    case OpStr: *w += code_[pc + 2]; break;
                    case OpJump:
                        if (*join != ~0u && code_[pc + 1] != *join)
                            return false;
                        *join = code_[pc + 1];
                        return true;
                    default: return false;
                }
                pc += size(pc);
            }
            return false;
        };
        std::uint32_t pc = 0;
        *lo = *hi = 0;
        for (int k = 0; k < 16 && pc < n_ && *hi <= 8; ++k) {
            switch (code_[pc]) {
                case OpLit: *unit = code_[pc + 1]; return *hi - *lo <= 3;
                case OpStr:
                    *unit = p_.literals[code_[pc + 1]];
                    if (code_[pc + 2] >= 2)
                        *unit2 = p_.literals[code_[pc + 1] + 1];
                    return *hi - *lo <= 3;
                case OpSave: case OpLineStart: case OpLineStartM: case OpLineEnd: case OpLineEndM: case OpWord: break;
                case OpLit2: case OpSet: ++*lo; ++*hi; break;
                case OpSplit: {
                    std::uint32_t join = ~0u, a = 0, b = 0;
                    if (!width(pc + 2, &join, &a) || join == ~0u || !width(code_[pc + 1], &join, &b))
                        return false;
                    *lo += std::min(a, b);
                    *hi += std::max(a, b);
                    pc = join;
                    continue;
                }
                default: return false;
            }
            pc += size(pc);
        }
        return false;
    }

    // ---- where nothing is pushed yet ----
    /// `clean_`: the places that every way in reaches with nothing pushed since the attempt began (only the entry of the
    /// next place to begin on the stack). A capture there has nothing to undo; the registers so written (`noUndo_`) are set
    /// back where the next attempt begins. Only for programs of these instructions.
    void planClean()
    {
        clean_.assign(n_ + 1, 0);
        for (std::uint32_t pc = 0; pc < n_; pc += size(pc)) {
            switch (code_[pc]) {
                case OpMatch: case OpLit: case OpLit2: case OpStr: case OpSet: case OpGreedy: case OpSplit: case OpSplitLazy:
                case OpJump: case OpSave: case OpLineStart: case OpLineStartM: case OpLineEnd: case OpLineEndM: case OpWord:
                    break;
                default:
                    return;
            }
        }
        // 2: not reached yet; 1: clean on every way found so far; 0: not.
        std::vector<std::uint8_t> st(n_ + 1, 2);
        st[0] = 1;
        bool changed = true;
        const auto into = [&](std::uint32_t to, std::uint8_t v) {
            if (to <= n_ && v < st[to]) {
                st[to] = v;
                changed = true;
            }
        };
        while (changed) {
            changed = false;
            for (std::uint32_t pc = 0; pc < n_; pc += size(pc)) {
                if (st[pc] == 2)
                    continue;
                const std::uint8_t v = st[pc];
                switch (code_[pc]) {
                    case OpMatch: break;
                    case OpJump: into(code_[pc + 1], v); break;
                    case OpSplit: into(pc + 2, 0); into(code_[pc + 1], v); break;  // the other way: once the choice is off
                    case OpSplitLazy: into(code_[pc + 1], 0); into(pc + 2, v); break;
                    case OpGreedy: {
                        // As `greedy`: an entry to give back from is pushed unless what follows cannot take what it took.
                        const bool pushes = !(endsAt(pc + 4) || firstOf(pc + 4).apartFrom(ofSet(p_.sets[code_[pc + 1]])));
                        into(pc + 4, pushes ? 0 : v);
                        break;
                    }
                    default: into(pc + size(pc), v); break;
                }
            }
        }
        for (std::uint32_t pc = 0; pc < n_; pc += size(pc)) {
            clean_[pc] = st[pc] == 1;
            if (clean_[pc] && code_[pc] == OpSave &&
                std::find(noUndo_.begin(), noUndo_.end(), code_[pc + 1]) == noUndo_.end())
                noUndo_.push_back(code_[pc + 1]);
        }
    }

    // ---- what can come first ----
    First firstOf(std::uint32_t pc)
    {
        First f;
        std::vector<std::uint8_t> seen(n_ + 1, 0);
        unsigned budget = 400;
        firstInto(pc, f, seen, budget);
        return f;
    }
    void firstInto(std::uint32_t pc, First& f, std::vector<std::uint8_t>& seen, unsigned& budget)
    {
        while (!f.open) {
            if (!budget--) {
                f.open = true;
                return;
            }
            if (seen[pc])
                return;  // what it can begin with is in already, or is being added
            seen[pc] = 1;
            switch (code_[pc]) {
                case OpLit: f.add(code_[pc + 1]); return;
                case OpLit2: f.add(code_[pc + 1]); f.add(code_[pc + 2]); return;
                case OpStr: f.add(p_.literals[code_[pc + 1]]); return;
                case OpSet: case OpSetU: f.add(p_.sets[code_[pc + 1]]); return;
                case OpGreedy: case OpGreedyU:
                    f.add(p_.sets[code_[pc + 1]]);
                    if (code_[pc + 2] > 0)
                        return;
                    pc += 4;
                    break;
                case OpSplit: case OpSplitLazy:
                    firstInto(code_[pc + 1], f, seen, budget);
                    pc += 2;
                    break;
                case OpJump: pc = code_[pc + 1]; break;
                case OpLoopEnter:
                    // It goes on or it is done: how often it has run is not known here.
                    firstInto(code_[pc + 5], f, seen, budget);
                    pc += 6;
                    break;
                case OpSave: case OpClear: case OpMark: case OpCheckProgress: case OpCheckCounted: case OpLoopInit: case OpLoopNext:
                case OpLineStart: case OpLineStartM: case OpWord: case OpNegBegin:
                    if (code_[pc] == OpNegBegin) {
                        pc = code_[pc + 2];
                        break;
                    }
                    pc += size(pc);
                    break;
                default: f.open = true; return;
            }
        }
    }
    static First ofSet(const CodeSet& set)
    {
        First f;
        f.add(set);
        return f;
    }

    // ---- failing ----
    /// Where to go when an instruction fails, with the stack as it is here.
    Label& fail()
    {
        if (known_.empty() || known_.front().undo)
            return fail_;
        stubs_.push_back(Stub{Label{}, known_});
        return stubs_.back().in;
    }
    void pushed(bool undo, std::uint32_t what)
    {
        if (!undo)
            known_.clear();  // what is under it does not matter
        if (undo && known_.empty())
            return;
        if (known_.size() >= 6) {
            known_.clear();
            return;
        }
        known_.push_back(Entry{undo, what});
    }

    // ---- pieces ----
    static Mem ctx(std::size_t offset) { return mem(R21, std::int32_t(offset)); }
    void room()
    {
        Later& l = later();
        a_.cmp(R14, R15);
        a_.jcc(CA, l.in);
        auto after = std::make_shared<Label>();
        a_.bind(*after);
        l.emit = [this, after] {
            saveState();
            a_.mov(ctx(offsetof(Ctx, spill)), RSI);
            a_.mov(ctx(offsetof(Ctx, spill) + 8), RDX);
            a_.mov(ctx(offsetof(Ctx, spill) + 16), RAX);
            a_.mov(RDI, R21);
            a_.callAbs(reinterpret_cast<const void*>(&scriptRegexGrow));
            a_.aluI32(4, RAX, 0xFF);
            a_.jcc(CE, noMemory_);
            loadState();
            a_.mov(RSI, ctx(offsetof(Ctx, spill)));
            a_.mov(RDX, ctx(offsetof(Ctx, spill) + 8));
            a_.mov(RAX, ctx(offsetof(Ctx, spill) + 16));
            a_.mov(R15, ctx(offsetof(Ctx, limit)));
            a_.jmp(*after);
        };
    }
    void saveState()
    {
        a_.mov32(ctx(offsetof(Ctx, pos)), R12);
        a_.mov(ctx(offsetof(Ctx, sp)), R14);
    }
    void loadState()
    {
        a_.mov32(R12, ctx(offsetof(Ctx, pos)));
        a_.mov(R14, ctx(offsetof(Ctx, sp)));
    }
    void push2(Reg value, std::uint32_t tag)
    {
        a_.mov32(mem(R14, 0), value);
        a_.movImm(RCX, tag);
        a_.mov32(mem(R14, 4), RCX);
        a_.aluI(0, R14, 8);
    }
    void choice(std::uint32_t pc)
    {
        room();
        push2(R12, pc);
        pushed(false, pc);
    }
    /// regs[r] = `value`, which is none of rcx and rdx.
    void setRegister(std::uint32_t r, Reg value)
    {
        room();
        a_.mov32(RDX, mem(R13, std::int32_t(r * 4)));
        push2(RDX, r | kUndo);
        a_.mov32(mem(R13, std::int32_t(r * 4)), value);
        pushed(true, r);
    }
    Later& later()
    {
        laters_.emplace_back();
        return laters_.back();
    }
    /// The unit at the position, into rax; none left goes to `none`.
    void needUnit(Label& none)
    {
        a_.alu32(7, R12, RBP);
        a_.jcc(CAE, none);
        a_.movzx16(RAX, mem(RBX, R12, 2));
    }
    const std::uint8_t* bytesOf(const First& f, const First* g = nullptr)
    {
        out_->bytes.emplace_back();
        auto& t = out_->bytes.back();
        for (std::uint32_t c = 0; c < 256; ++c)
            t[c] = std::uint8_t((f.has(c) ? 1 : 0) | (g && g->has(c) ? 2 : 0));
        return t.data();
    }
    const std::uint8_t* trailsOf(const First& f, const First& g)
    {
        out_->trails.emplace_back();
        auto& t = out_->trails.back();
        for (std::uint32_t c = 0; c < 1024; ++c)
            t[c] = std::uint8_t((f.hasTrail(c) ? 1 : 0) | (g.hasTrail(c) ? 2 : 0));
        return t.data();
    }
    static Ranges rangesOf(const CodeSet& set)
    {
        Ranges r;
        for (std::uint32_t c = 0; c < 256; ++c) {
            if (!(set.low[c >> 5] >> (c & 31) & 1))
                continue;
            if (!r.empty() && r.back().second + 1 == c)
                r.back().second = c;
            else
                r.emplace_back(c, c);
        }
        for (const auto& [lo, hi] : set.high) {
            if (lo > 0xffff)
                continue;
            const char32_t top = std::min<char32_t>(hi, 0xffff);
            if (!r.empty() && r.back().second + 1 == lo)
                r.back().second = top;
            else
                r.emplace_back(lo, top);
        }
        return r;
    }
    static Ranges without(const Ranges& r)
    {
        Ranges out;
        char32_t at = 0;
        for (const auto& [lo, hi] : r) {
            if (lo > at)
                out.emplace_back(at, lo - 1);
            at = hi + 1;
        }
        if (at <= 0xffff)
            out.emplace_back(at, 0xffff);
        return out;
    }
    /// The unit in rax is in the range: jumps to `to` (or, not `in`, when it is not). Uses rcx.
    void inRange(const std::pair<char32_t, char32_t>& r, bool in, Label& to)
    {
        if (r.first == r.second) {
            a_.aluI32(7, RAX, std::int32_t(r.first));
            a_.jcc(in ? CE : CNE, to);
            return;
        }
        if (r.first == 0) {
            a_.aluI32(7, RAX, std::int32_t(r.second));
            a_.jcc(in ? CBE : CA, to);
            return;
        }
        a_.op32I(5, RCX, RAX, std::int32_t(r.first));
        a_.aluI32(7, RCX, std::int32_t(r.second - r.first));
        a_.jcc(in ? CBE : CA, to);
    }
    /// Jumps to `out` when the unit in rax is not in the set. Uses rcx; keeps the rest.
    void needIn(std::uint32_t index, Label& out)
    {
        const CodeSet& set = p_.sets[index];
        const Ranges r = rangesOf(set);
        if (r.empty()) {
            a_.jmp(out);
            return;
        }
        if (r.size() == 1) {
            if (r[0].first == 0 && r[0].second == 0xffff)
                return;
            inRange(r[0], false, out);
            return;
        }
        const Ranges not_ = without(r);
        if (not_.size() <= 2) {
            for (const auto& x : not_)
                inRange(x, true, out);
            return;
        }
        if (r.size() == 2) {
            Label in;
            inRange(r[0], true, in);
            inRange(r[1], false, out);
            a_.bind(in);
            return;
        }
        auto in = std::make_shared<Label>();
        a_.aluI32(7, RAX, 256);
        if (set.high.empty()) {
            a_.jcc(CAE, out);
        } else {
            Later& l = later();
            a_.jcc(CAE, l.in);
            Label* o = &out;
            Ranges high;
            for (const auto& x : r)
                if (x.second >= 256)
                    high.push_back(x);
            l.emit = [this, in, o, high, &set] {
                if (high.size() <= 6) {
                    for (const auto& x : high)
                        inRange(x, true, *in);
                    a_.jmp(*o);
                    return;
                }
                callHas(set);
                a_.jcc(CNE, *in);
                a_.jmp(*o);
            };
        }
        a_.movImm(RCX, reinterpret_cast<std::uint64_t>(bytesOf(ofSet(set))));
        a_.movzx8(RCX, mem(RCX, RAX, 1));
        a_.test32(RCX, RCX);
        a_.jcc(CE, out);
        a_.bind(*in);
    }
    /// Past the code point at the position, whose first unit is in rax, if the set has it; else to `out`. Uses rcx and rdi.
    void codePoint(std::uint32_t index, Label& out)
    {
        auto done = std::make_shared<Label>();
        auto single = std::make_shared<Label>();
        Later& l = later();
        a_.op32I(5, RCX, RAX, 0xd800);
        a_.aluI32(7, RCX, 0x3ff);
        a_.jcc(CBE, l.in);
        a_.bind(*single);
        needIn(index, out);
        a_.aluI32(0, R12, 1);
        a_.bind(*done);
        Label* o = &out;
        l.emit = [this, index, done, single, o] {
            a_.op32I(0, RDI, R12, 1);
            a_.alu32(7, RDI, RBP);
            a_.jcc(CAE, *single);
            a_.movzx16(RDI, mem(RBX, R12, 2, 2));
            a_.op32I(5, RDI, RDI, 0xdc00);
            a_.aluI32(7, RDI, 0x3ff);
            a_.jcc(CA, *single);
            a_.shiftI32(4, RCX, 10);
            a_.alu32(0, RCX, RDI);
            a_.op32I(0, RAX, RCX, 0x10000);
            Label in;
            Ranges above;
            const CodeSet& set = p_.sets[index];
            for (const auto& [lo, hi] : set.high)
                if (hi > 0xffff)
                    above.emplace_back(std::max<char32_t>(lo, 0x10000), hi);
            if (above.size() > 8) {
                callHas(set);
                a_.jcc(CE, *o);
            } else {
                for (const auto& x : above) {
                    if (x.first == 0x10000 && x.second == 0x10ffff)
                        a_.jmp(in);
                    else
                        inRange(x, true, in);
                }
                a_.jmp(*o);
            }
            a_.bind(in);
            a_.aluI32(0, R12, 2);
            a_.jmp(*done);
        };
    }
    /// Whether the set has what is in rax, in the flags: not zero when it does. Keeps rsi and rdx.
    void callHas(const CodeSet& set)
    {
        a_.mov(ctx(offsetof(Ctx, spill)), RSI);
        a_.mov(ctx(offsetof(Ctx, spill) + 8), RDX);
        a_.movImm(RDI, reinterpret_cast<std::uint64_t>(&set));
        a_.mov(RSI, RAX);
        a_.callAbs(reinterpret_cast<const void*>(&scriptRegexHas));
        a_.mov(RSI, ctx(offsetof(Ctx, spill)));
        a_.mov(RDX, ctx(offsetof(Ctx, spill) + 8));
        a_.aluI32(4, RAX, 0xFF);
    }
    void callStep(std::uint32_t pc)
    {
        room();
        Label& no = fail();
        saveState();
        a_.mov(RDI, R21);
        a_.movImm(RSI, pc);
        a_.callAbs(reinterpret_cast<const void*>(&scriptRegexStep));
        loadState();
        a_.aluI32(4, RAX, 0xFF);
        a_.jcc(CE, no);
    }

    void prologue()
    {
        a_.abiEnter();
        a_.push(RBP);
        a_.push(RBX);
        a_.push(R12);
        a_.push(R13);
        a_.push(R14);
        a_.push(R15);
        a_.aluI(5, RSP, 8);
        a_.mov(R21, RDI);
        a_.mov(RBX, ctx(offsetof(Ctx, s)));
        a_.mov32(RBP, ctx(offsetof(Ctx, length)));
        a_.mov(R13, ctx(offsetof(Ctx, regs)));
        a_.mov(R14, ctx(offsetof(Ctx, stack)));
        a_.mov(R15, ctx(offsetof(Ctx, limit)));
        a_.movImm(R22, reinterpret_cast<std::uint64_t>(out_->table.data()));
        // At the bottom of the stack: the next place to begin.
        a_.mov32(mem(R14, 0), 0);
        a_.movImm(RCX, n_);
        a_.mov32(mem(R14, 4), RCX);
        a_.aluI(0, R14, 8);
        a_.mov32(R12, ctx(offsetof(Ctx, at)));
        const bool pairs = p_.flags & (kUnicode | kUnicodeSets);
        const bool once = pairs || (p_.flags & kSticky) || (p_.anchored && !(p_.flags & kMultiline));
        Label begin, search;
        a_.jmp(begin);
        a_.bind(at_[n_]);
        offsets_.emplace_back(n_, a_.here());
        // Nothing from where it began: everything pushed since is off again, and every register as it was (those written
        // with nothing to undo, here).
        for (std::uint32_t r : noUndo_)
            a_.mov32(mem(R13, std::int32_t(r * 4)), -1);
        a_.mov32(R12, ctx(offsetof(Ctx, at)));
        if (once) {
            a_.jmp(none_);
        } else {
            a_.alu32(7, R12, RBP);
            a_.jcc(CAE, none_);
            a_.aluI32(0, R12, 1);
            a_.mov32(mem(R14, 0), 0);  // the same entry again
            a_.aluI(0, R14, 8);
        }
        a_.bind(begin);
        std::uint32_t lo = 0, hi = 0, unit = 0, unit2 = ~0u;
        if (!once && p_.firstUnit < 0 && unitAhead(&lo, &hi, &unit, &unit2) && hi > 0) {
            // A unit every match has at `lo` to `hi` units from its start, when none is known to begin it (that is found
            // faster below): places without it there are passed over.
            Label next, scan, found;
            a_.jmp(scan);
            a_.bind(next);
            a_.aluI32(0, R12, 1);
            a_.bind(scan);
            a_.op32I(0, RAX, R12, std::int32_t(lo));
            a_.alu32(7, RAX, RBP);
            a_.jcc(CAE, none_);
            for (std::uint32_t d = lo; d <= hi; ++d) {
                Label other;
                a_.op32I(0, RAX, R12, std::int32_t(d));
                a_.alu32(7, RAX, RBP);
                a_.jcc(CAE, next);
                a_.movzx16(RCX, mem(RBX, RAX, 2));
                a_.aluI32(7, RCX, std::int32_t(unit));
                if (unit2 == ~0u) {
                    a_.jcc(CE, found);
                    continue;
                }
                // And the unit after it, when the literal has one.
                a_.jcc(CNE, other);
                a_.aluI32(0, RAX, 1);
                a_.alu32(7, RAX, RBP);
                a_.jcc(CAE, other);
                a_.movzx16(RCX, mem(RBX, RAX, 2));
                a_.aluI32(7, RCX, std::int32_t(unit2));
                a_.jcc(CE, found);
                a_.bind(other);
            }
            a_.jmp(next);
            a_.bind(found);
        } else if (!once && p_.firstSet >= 0) {
            if (p_.firstUnit >= 0) {
                Label found;
                needUnit(none_);
                a_.aluI32(7, RAX, p_.firstUnit);
                a_.jcc(CE, found);
                a_.mov(RDI, R21);
                a_.mov(RSI, R12);
                a_.movImm(RDX, std::uint64_t(p_.firstUnit));
                a_.callAbs(reinterpret_cast<const void*>(&scriptRegexFind));
                a_.mov32(R12, RAX);
                a_.alu32(7, R12, RBP);
                a_.jcc(CAE, none_);
                a_.bind(found);
            } else {
                Label& next = newLabel();
                a_.jmp(search);
                a_.bind(next);
                a_.aluI32(0, R12, 1);
                a_.bind(search);
                needUnit(none_);
                needIn(std::uint32_t(p_.firstSet), next);
            }
        }
        a_.mov32(ctx(offsetof(Ctx, at)), R12);
    }

    /// Goes to `to` unless what the next unit says lets the branch begin. Nothing when it says nothing.
    void splitOn(std::uint32_t on, std::uint32_t off)
    {
        if (code_[on] == OpLineStart) {
            // `^` (not multiline) past the start: that branch fails at once, so the other is taken with nothing pushed.
            a_.test32(R12, R12);
            a_.jcc(CNE, at_[off]);
        }
        const First a = firstOf(on), b = firstOf(off);
        if (a.open && b.open) {
            choice(off);
            return;
        }
        Label noChoice;
        a_.alu32(7, R12, RBP);
        a_.jcc(CAE, a.open ? noChoice : at_[off]);  // at the end: only an open one can go on
        a_.movzx16(RAX, mem(RBX, R12, 2));
        const std::uint8_t* t = bytesOf(a, &b);
        Label push;
        a_.aluI32(7, RAX, 256);
        const bool aHigh = a.open || a.high, bHigh = b.open || b.high;
        if (!a.open && !b.open && a.astral() && b.astral() && a.lead == b.lead) {
            // Code points of one lead on both ways: the trail after it says which can begin, as the table does below 256.
            Label high, test, none;
            a_.jcc(CAE, high);
            a_.movImm(RCX, reinterpret_cast<std::uint64_t>(t));
            a_.movzx8(RCX, mem(RCX, RAX, 1));
            a_.bind(test);
            a_.testI(RCX, 1);
            a_.jcc(CE, at_[off]);
            a_.testI(RCX, 2);
            a_.jcc(CE, noChoice);
            a_.jmp(push);
            a_.bind(high);
            a_.aluI32(7, RAX, std::int32_t(a.lead));
            a_.jcc(CNE, none);
            a_.op32I(0, RDX, R12, 1);
            a_.alu32(7, RDX, RBP);
            a_.jcc(CAE, none);
            a_.movzx16(RCX, mem(RBX, RDX, 2));
            a_.op32I(5, RCX, RCX, 0xDC00);
            a_.aluI32(7, RCX, 0x3FF);
            a_.jcc(CA, none);
            a_.movImm(RDX, reinterpret_cast<std::uint64_t>(trailsOf(a, b)));
            a_.movzx8(RCX, mem(RDX, RCX, 1));
            a_.jmp(test);
            a_.bind(none);
            a_.movImm(RCX, 0);
            a_.jmp(test);
            a_.bind(push);
            known_.clear();
            choice(off);
            a_.bind(noChoice);
            known_.clear();
            return;
        }
        if (aHigh && bHigh)
            a_.jcc(CAE, push);
        else if (aHigh)
            a_.jcc(CAE, noChoice);
        else
            a_.jcc(CAE, at_[off]);
        a_.movImm(RCX, reinterpret_cast<std::uint64_t>(t));
        a_.movzx8(RCX, mem(RCX, RAX, 1));
        if (!a.open) {
            a_.testI(RCX, 1);
            a_.jcc(CE, at_[off]);
        }
        if (!b.open) {
            a_.testI(RCX, 2);
            a_.jcc(CE, noChoice);
        }
        a_.bind(push);
        known_.clear();
        choice(off);
        a_.bind(noChoice);
        if (!b.open || a.open)
            known_.clear();  // not always pushed
    }

    void emit(std::uint32_t pc)
    {
        switch (code_[pc]) {
            case OpMatch:
                a_.mov32(RAX, ctx(offsetof(Ctx, at)));
                a_.mov32(mem(R13, 0), RAX);
                a_.mov32(mem(R13, 4), R12);
                a_.movImm(RAX, 1);
                a_.jmp(leave_);
                known_.clear();
                return;
            case OpLit: {
                Label& no = fail();
                needUnit(no);
                a_.aluI32(7, RAX, std::int32_t(code_[pc + 1]));
                a_.jcc(CNE, no);
                a_.aluI32(0, R12, 1);
                return;
            }
            case OpLit2: {
                Label& no = fail();
                Label yes;
                needUnit(no);
                a_.aluI32(7, RAX, std::int32_t(code_[pc + 1]));
                a_.jcc(CE, yes);
                a_.aluI32(7, RAX, std::int32_t(code_[pc + 2]));
                a_.jcc(CNE, no);
                a_.bind(yes);
                a_.aluI32(0, R12, 1);
                return;
            }
            case OpStr: {
                const std::uint32_t length = code_[pc + 2];
                const char16_t* text = p_.literals.data() + code_[pc + 1];
                Label& no = fail();
                a_.op32I(0, RAX, R12, std::int32_t(length));
                a_.alu32(7, RAX, RBP);
                a_.jcc(CA, no);
                a_.lea(RDX, mem(RBX, R12, 2));
                std::uint32_t k = 0;
                for (; k + 4 <= length; k += 4) {
                    std::uint64_t v;
                    std::memcpy(&v, text + k, 8);
                    a_.mov(RCX, mem(RDX, std::int32_t(2 * k)));
                    a_.movImm(RSI, v);
                    a_.cmp(RCX, RSI);
                    a_.jcc(CNE, no);
                }
                for (; k + 2 <= length; k += 2) {
                    std::uint32_t v;
                    std::memcpy(&v, text + k, 4);
                    a_.mov32(RCX, mem(RDX, std::int32_t(2 * k)));
                    a_.movImm(RSI, v);
                    a_.alu32(7, RCX, RSI);
                    a_.jcc(CNE, no);
                }
                if (k < length) {
                    a_.movzx16(RCX, mem(RDX, std::int32_t(2 * k)));
                    a_.aluI32(7, RCX, std::int32_t(text[k]));
                    a_.jcc(CNE, no);
                }
                a_.mov32(R12, RAX);
                return;
            }
            case OpSet: {
                Label& no = fail();
                needUnit(no);
                needIn(code_[pc + 1], no);
                a_.aluI32(0, R12, 1);
                return;
            }
            case OpGreedy: greedy(pc); return;
            case OpSplit: splitOn(pc + 2, code_[pc + 1]); return;
            case OpSplitLazy:
                splitOn(code_[pc + 1], pc + 2);
                a_.jmp(at_[code_[pc + 1]]);
                known_.clear();
                return;
            case OpJump:
                a_.jmp(at_[code_[pc + 1]]);
                known_.clear();
                return;
            case OpSave: case OpMark:
                if (clean_[pc]) {
                    a_.mov32(mem(R13, std::int32_t(code_[pc + 1] * 4)), R12);
                    return;
                }
                setRegister(code_[pc + 1], R12);
                return;
            case OpClear:
                for (std::uint32_t r = code_[pc + 1]; r < code_[pc + 2]; ++r) {
                    Label unset;
                    room();
                    a_.mov32(RDX, mem(R13, std::int32_t(r * 4)));
                    a_.aluI32(7, RDX, -1);
                    a_.jcc(CE, unset);
                    push2(RDX, r | kUndo);
                    a_.mov32(mem(R13, std::int32_t(r * 4)), -1);
                    a_.bind(unset);
                }
                if (code_[pc + 1] < code_[pc + 2])
                    known_.clear();
                return;
            case OpCheckProgress:
                a_.mov32(RAX, mem(R13, std::int32_t(code_[pc + 1] * 4)));
                a_.alu32(7, RAX, R12);
                a_.jcc(CE, fail());
                return;
            case OpCheckCounted: {
                Label fine;
                a_.mov32(RAX, mem(R13, std::int32_t(code_[pc + 1] * 4)));
                a_.alu32(7, RAX, R12);
                a_.jcc(CNE, fine);
                a_.mov32(RAX, mem(R13, std::int32_t(code_[pc + 2] * 4)));
                a_.movImm(RCX, code_[pc + 3]);
                a_.alu32(7, RAX, RCX);
                a_.jcc(CAE, fail());
                a_.bind(fine);
                return;
            }
            case OpLoopInit:
                a_.movImm(RAX, 0);
                setRegister(code_[pc + 1], RAX);
                return;
            case OpLoopEnter: {
                const std::uint32_t min = code_[pc + 2], max = code_[pc + 3], exit = code_[pc + 5];
                Label on, choose;
                a_.mov32(RAX, mem(R13, std::int32_t(code_[pc + 1] * 4)));
                if (min > 0) {
                    a_.movImm(RCX, min);
                    a_.alu32(7, RAX, RCX);
                    a_.jcc(CB, on);
                }
                if (max != kUnbounded) {
                    a_.movImm(RCX, max);
                    a_.alu32(7, RAX, RCX);
                    a_.jcc(CAE, at_[exit]);
                }
                known_.clear();
                if (code_[pc + 4]) {
                    splitOn(pc + 6, exit);
                } else {
                    splitOn(exit, pc + 6);
                    a_.jmp(at_[exit]);
                }
                a_.bind(on);
                known_.clear();
                return;
            }
            case OpLoopNext:
                a_.mov32(RAX, mem(R13, std::int32_t(code_[pc + 1] * 4)));
                a_.aluI32(0, RAX, 1);
                setRegister(code_[pc + 1], RAX);
                return;
            case OpLineStart:
                a_.test32(R12, R12);
                a_.jcc(CNE, fail());
                return;
            case OpLineEnd:
                a_.alu32(7, R12, RBP);
                a_.jcc(CNE, fail());
                return;
            case OpLineStartM: case OpLineEndM: {
                Label fine;
                const bool start = code_[pc] == OpLineStartM;
                Label& no = fail();
                if (start)
                    a_.test32(R12, R12);
                else
                    a_.alu32(7, R12, RBP);
                a_.jcc(CE, fine);
                a_.movzx16(RAX, mem(RBX, R12, 2, start ? -2 : 0));
                a_.aluI32(7, RAX, 10);
                a_.jcc(CE, fine);
                a_.aluI32(7, RAX, 13);
                a_.jcc(CE, fine);
                a_.op32I(5, RCX, RAX, 0x2028);
                a_.aluI32(7, RCX, 1);
                a_.jcc(CA, no);
                a_.bind(fine);
                return;
            }
            case OpWord: {
                if (code_[pc + 2]) {
                    callStep(pc);
                    return;
                }
                First word;
                for (std::uint32_t c = '0'; c <= 'z'; ++c)
                    if (c <= '9' || (c >= 'A' && c <= 'Z') || c == '_' || c >= 'a')
                        word.add(c);
                if (!wordBytes_)
                    wordBytes_ = bytesOf(word);
                Label& no = fail();
                a_.movImm(RDX, reinterpret_cast<std::uint64_t>(wordBytes_));
                for (int side = 0; side < 2; ++side) {
                    const Reg d = side ? RSI : RDI;
                    Label none;
                    a_.movImm(d, 0);
                    if (side) {
                        a_.alu32(7, R12, RBP);
                        a_.jcc(CAE, none);
                        a_.movzx16(RAX, mem(RBX, R12, 2));
                    } else {
                        a_.test32(R12, R12);
                        a_.jcc(CE, none);
                        a_.movzx16(RAX, mem(RBX, R12, 2, -2));
                    }
                    a_.aluI32(7, RAX, 256);
                    a_.jcc(CAE, none);
                    a_.movzx8(d, mem(RDX, RAX, 1));
                    a_.bind(none);
                }
                a_.alu32(7, RDI, RSI);
                a_.jcc(code_[pc + 1] ? CNE : CE, no);
                return;
            }
            case OpLookBegin: {
                const std::uint32_t r = code_[pc + 1];
                setRegister(r + 1, R12);
                height();
                setRegister(r, RAX);
                return;
            }
            case OpNegBegin: {
                const std::uint32_t r = code_[pc + 1];
                room();
                height();
                setRegister(r, RAX);
                choice(code_[pc + 2]);
                return;
            }
            case OpNegEnd:
                saveState();
                a_.mov(RDI, R21);
                a_.movImm(RSI, pc);
                a_.callAbs(reinterpret_cast<const void*>(&scriptRegexStep));
                loadState();
                a_.jmp(fail_);
                known_.clear();
                return;
            case OpLookEnd:
                known_.clear();
                callStep(pc);
                known_.clear();
                return;
            case OpGreedyU: greedyCodes(pc); return;
            case OpSetU: {
                Label& no = fail();
                needUnit(no);
                codePoint(code_[pc + 1], no);
                return;
            }
            default:
                callStep(pc);
                return;
        }
    }
    /// The stack's height after one more push, into rax: what `regex::step` takes an end of a lookaround back to.
    void height()
    {
        a_.mov(RAX, R14);
        a_.alu(5, RAX, ctx(offsetof(Ctx, stack)));
        a_.shiftI(5, RAX, 2);
        a_.aluI32(0, RAX, 2);
    }

    void greedy(std::uint32_t pc)
    {
        const std::uint32_t set = code_[pc + 1], min = code_[pc + 2], max = code_[pc + 3];
        Label& no = fail();
        Label loop;
        Label& end = newLabel();
        a_.mov32(RSI, R12);  // where it began
        if (max == kUnbounded) {
            a_.mov32(RDX, RBP);
        } else {
            Label less;
            a_.movImm(RCX, max);
            a_.mov32(RDX, RBP);
            a_.alu32(5, RDX, R12);
            a_.alu32(7, RDX, RCX);
            a_.jcc(CBE, less);
            a_.mov32(RDX, RCX);
            a_.bind(less);
            a_.alu32(0, RDX, R12);
        }
        a_.bind(loop);
        a_.alu32(7, R12, RDX);
        a_.jcc(CAE, end);
        a_.movzx16(RAX, mem(RBX, R12, 2));
        needIn(set, end);
        a_.aluI32(0, R12, 1);
        a_.jmp(loop);
        a_.bind(end);
        if (min > 0) {
            a_.op32I(0, RSI, RSI, std::int32_t(min));
            a_.alu32(7, R12, RSI);
            a_.jcc(CB, no);
        }
        // rsi: as far back as it may give.
        const First next = firstOf(pc + 4);
        if (endsAt(pc + 4) || next.apartFrom(ofSet(p_.sets[set]))) {
            known_.clear();
            return;
        }
        Label nothing;
        a_.alu32(7, R12, RSI);
        a_.jcc(CBE, nothing);
        room();
        a_.mov32(mem(R14, 0), RSI);
        a_.mov32(mem(R14, 4), R12);
        a_.movImm(RCX, (n_ + 1 + pc) | kGreedy);
        a_.mov32(mem(R14, 8), RCX);
        a_.aluI(0, R14, 12);
        a_.bind(nothing);
        known_.clear();
        // Give one back, and more while what is left cannot go on.
        Later& l = later();
        l.emit = [this, pc, next] {
            offsets_.emplace_back(n_ + 1 + pc, a_.here());
            a_.mov32(R12, mem(R14, -8));
            a_.mov32(RSI, mem(R14, -12));
            Label again, take, last;
            a_.bind(again);
            a_.aluI32(5, R12, 1);
            a_.alu32(7, R12, RSI);
            a_.jcc(CBE, last);
            if (!next.open) {
                a_.movzx16(RAX, mem(RBX, R12, 2));
                a_.aluI32(7, RAX, 256);
                if (next.high)
                    a_.jcc(CAE, take);
                else
                    a_.jcc(CAE, again);
                a_.movImm(RCX, reinterpret_cast<std::uint64_t>(bytesOf(next)));
                a_.movzx8(RCX, mem(RCX, RAX, 1));
                a_.test32(RCX, RCX);
                a_.jcc(CE, again);
            }
            a_.bind(take);
            a_.mov32(mem(R14, -8), R12);
            a_.jmp(at_[pc + 4]);
            a_.bind(last);
            a_.aluI(5, R14, 12);
            a_.jmp(at_[pc + 4]);
        };
    }
    /// A greedy loop by code points. Its entry is given back from by `fail_`.
    void greedyCodes(std::uint32_t pc)
    {
        const std::uint32_t set = code_[pc + 1], min = code_[pc + 2], max = code_[pc + 3];
        Label& no = fail();
        Label loop, skip;
        Label& end = newLabel();
        a_.mov32(RSI, R12);    // as far back as it may give
        a_.movImm(RDX, 0);     // how many
        a_.bind(loop);
        if (max != kUnbounded) {
            a_.movImm(RCX, max);
            a_.alu32(7, RDX, RCX);
            a_.jcc(CAE, end);
        }
        needUnit(end);
        codePoint(set, end);
        a_.aluI32(0, RDX, 1);
        if (min > 0) {
            a_.movImm(RCX, min);
            a_.alu32(7, RDX, RCX);
            a_.jcc(CNE, loop);
            a_.mov32(RSI, R12);
        }
        a_.jmp(loop);
        a_.bind(end);
        a_.movImm(RCX, min);
        if (min > 0) {
            a_.alu32(7, RDX, RCX);
            a_.jcc(CB, no);
        }
        a_.alu32(7, RDX, RCX);
        a_.jcc(CBE, skip);
        const First next = firstOf(pc + 4);
        if (!endsAt(pc + 4) && !next.apartFrom(ofSet(p_.sets[set]))) {
            room();
            a_.mov32(mem(R14, 0), RSI);
            a_.mov32(mem(R14, 4), R12);
            a_.movImm(RCX, (pc + 4) | kGreedyCodes);
            a_.mov32(mem(R14, 8), RCX);
            a_.aluI(0, R14, 12);
        }
        a_.bind(skip);
        known_.clear();
    }
    /// The instruction at `pc` is the end of the match, whatever came before.
    bool endsAt(std::uint32_t pc) const
    {
        for (int k = 0; k < 8; ++k) {
            if (code_[pc] == OpMatch)
                return true;
            if (code_[pc] == OpJump)
                pc = code_[pc + 1];
            else if (code_[pc] == OpSave)
                pc += 2;
            else
                return false;
        }
        return false;
    }

    void tails()
    {
        for (std::size_t k = 0; k < laters_.size(); ++k) {
            a_.bind(laters_[k].in);
            laters_[k].emit();
        }
        for (Stub& s : stubs_) {
            a_.bind(s.in);
            std::int32_t down = 0;
            for (std::size_t k = s.entries.size(); k-- > 1;) {
                a_.mov32(RCX, mem(R14, down - 8));
                a_.mov32(mem(R13, std::int32_t(s.entries[k].what * 4)), RCX);
                down -= 8;
            }
            a_.mov32(R12, mem(R14, down - 8));
            a_.aluI(5, R14, 8 - down);
            a_.jmp(at_[s.entries[0].what]);
        }
        Label undo;
        a_.bind(fail_);
        a_.mov32(RAX, mem(R14, -4));
        a_.test32(RAX, RAX);
        a_.jcc(CS, undo);
        Label three, codes;
        a_.testI(RAX, std::int32_t(kGreedy | kGreedyCodes));
        a_.jcc(CNE, three);
        a_.mov32(R12, mem(R14, -8));
        a_.aluI(5, R14, 8);
        a_.mov(RAX, mem(R22, RAX, 8));
        a_.jmp(RAX);
        a_.bind(three);
        a_.testI(RAX, std::int32_t(kGreedyCodes));
        a_.jcc(CNE, codes);
        a_.aluI32(4, RAX, std::int32_t(kIndexMask));
        a_.mov(RAX, mem(R22, RAX, 8));
        a_.jmp(RAX);
        a_.bind(codes);
        {
            // One code point back.
            Label whole, last;
            a_.aluI32(4, RAX, std::int32_t(kIndexMask));
            a_.mov32(RSI, mem(R14, -12));
            a_.mov32(R12, mem(R14, -8));
            a_.aluI32(5, R12, 1);
            a_.alu32(7, R12, RSI);
            a_.jcc(CBE, whole);
            a_.movzx16(RCX, mem(RBX, R12, 2));
            a_.op32I(5, RCX, RCX, 0xdc00);
            a_.aluI32(7, RCX, 0x3ff);
            a_.jcc(CA, whole);
            a_.movzx16(RCX, mem(RBX, R12, 2, -2));
            a_.op32I(5, RCX, RCX, 0xd800);
            a_.aluI32(7, RCX, 0x3ff);
            a_.jcc(CA, whole);
            a_.aluI32(5, R12, 1);
            a_.bind(whole);
            a_.mov(RAX, mem(R22, RAX, 8));
            a_.alu32(7, R12, RSI);
            a_.jcc(CE, last);
            a_.mov32(mem(R14, -8), R12);
            a_.jmp(RAX);
            a_.bind(last);
            a_.aluI(5, R14, 12);
            a_.jmp(RAX);
        }
        a_.bind(undo);
        a_.movImm(RCX, kIndexMask);
        a_.alu32(4, RAX, RCX);
        a_.mov32(RCX, mem(R14, -8));
        a_.mov32(mem(R13, RAX, 4), RCX);
        a_.aluI(5, R14, 8);
        a_.jmp(fail_);
        a_.bind(noMemory_);
        a_.movImm(RAX, 2);
        a_.jmp(leave_);
        a_.bind(none_);
        a_.movImm(RAX, 0);
        a_.bind(leave_);
        a_.aluI(0, RSP, 8);
        a_.pop(R15);
        a_.pop(R14);
        a_.pop(R13);
        a_.pop(R12);
        a_.pop(RBX);
        a_.pop(RBP);
        a_.abiLeave();
        a_.ret();
    }
    Label& newLabel()
    {
        labels_.emplace_back();
        return labels_.back();
    }

    const Program& p_;
    const std::vector<std::uint32_t>& code_;
    const std::uint32_t n_;
    A64 a_;
    std::unique_ptr<Code> out_;
    std::vector<Label> at_;
    std::vector<std::uint8_t> target_;
    std::vector<std::pair<std::uint32_t, std::int32_t>> offsets_;
    std::vector<Entry> known_;  ///< the top of the stack, as far as it is known: a choice, then what is to be undone
    std::vector<std::uint8_t> clean_;   ///< see `planClean`
    std::vector<std::uint32_t> noUndo_;
    std::deque<Stub> stubs_;
    std::deque<Later> laters_;
    const std::uint8_t* wordBytes_ = nullptr;
    std::deque<Label> labels_;  ///< those that code emitted later jumps to
    Label fail_, none_, leave_, noMemory_;
};

std::mutex compileLock;

const Code* nativeOf(const Program& program, std::size_t ahead)
{
    if (NativeMatcher* m = program.native.load(std::memory_order_acquire))
        return static_cast<const Code*>(m);
    std::uint32_t heat = program.heat.load(std::memory_order_relaxed);
    if (heat >= kNoNative)
        return nullptr;
    heat += 1 + std::uint32_t(std::min<std::size_t>(ahead / 256, kHot));
    program.heat.store(heat, std::memory_order_relaxed);
    if (heat < kHot)
        return nullptr;
    std::lock_guard<std::mutex> hold(compileLock);
    if (NativeMatcher* m = program.native.load(std::memory_order_acquire))
        return static_cast<const Code*>(m);
    Code* made = Compiler(program).compile();
    if (!made) {
        program.heat.store(kNoNative, std::memory_order_relaxed);
        return nullptr;
    }
    program.native.store(made, std::memory_order_release);
    return made;
}

}  // namespace

namespace {

[[gnu::noinline]] bool regexExecLong(const Program& program, std::u16string_view input, std::size_t start, Matcher& matcher);

}  // namespace

bool regexExec(const Program& program, std::u16string_view input, std::size_t start, Matcher& matcher)
{
    // The usual call in a few instructions: a program that has its native code, no surrogate pairs to step over, the
    // matcher's stack and registers already as large as the program needs.
    if (NativeMatcher* m = program.native.load(std::memory_order_acquire);
        m && !matcher.poll && !(program.flags & (kUnicode | kUnicodeSets)) && matcher.stack.size() >= 256 &&
        matcher.registers.size() >= program.registers) [[likely]] {
        std::int32_t* const regs = matcher.registers.data();
        for (std::uint32_t i = 0; i < program.registers; ++i)
            regs[i] = -1;
        Ctx c;
        c.s = input.data();
        c.length = std::uint32_t(input.size());
        c.at = std::uint32_t(start);
        c.regs = regs;
        c.pos = 0;
        c.pad = 0;
        c.sp = nullptr;
        c.spill[0] = c.spill[1] = c.spill[2] = 0;
        c.program = &program;
        c.matcher = &matcher;
        setStack(&c);
        const std::uint32_t r = static_cast<const Code*>(m)->run(&c);
        if (r > 1) [[unlikely]]
            throw std::bad_alloc();
        return r != 0;
    }
    return regexExecLong(program, input, start, matcher);
}

namespace {

bool regexExecLong(const Program& program, std::u16string_view input, std::size_t start, Matcher& matcher)
{
    const Code* native = matcher.poll ? nullptr : nativeOf(program, input.size() - std::min(start, input.size()));
    if (!native)
        return exec(program, input, start, matcher);
    const char16_t* const s = input.data();
    const std::uint32_t length = std::uint32_t(input.size());
    if (matcher.stack.size() < 256)
        matcher.stack.resize(256);
    matcher.registers.assign(program.registers, -1);
    Ctx c{};
    c.s = s;
    c.length = length;
    c.at = std::uint32_t(start);
    c.regs = matcher.registers.data();
    c.program = &program;
    c.matcher = &matcher;
    setStack(&c);
    const bool pairs = program.flags & (kUnicode | kUnicodeSets);
    if (!pairs) {
        const std::uint32_t r = native->run(&c);
        if (r > 1)
            throw std::bad_alloc();
        return r != 0;
    }
    // Where a match begins is a code point: the search is here, and the code tries one place.
    const auto high = [&](std::uint32_t i) { return i < length && s[i] >= 0xd800 && s[i] <= 0xdbff; };
    const auto low = [&](std::uint32_t i) { return i < length && s[i] >= 0xdc00 && s[i] <= 0xdfff; };
    std::uint32_t at = std::uint32_t(start);
    if (at > 0 && low(at) && high(at - 1))
        --at;
    const bool once = (program.flags & kSticky) != 0 || (program.anchored && !(program.flags & kMultiline));
    const CodeSet* first = !once && program.firstSet >= 0 ? &program.sets[std::size_t(program.firstSet)] : nullptr;
    for (;;) {
        if (first) {
            while (at < length && !first->has(s[at]))
                at += high(at) && low(at + 1) ? 2 : 1;
            if (at >= length)
                return false;
        }
        c.at = at;
        const std::uint32_t r = native->run(&c);
        if (r > 1)
            throw std::bad_alloc();
        if (r)
            return true;
        if (once || at >= length)
            return false;
        at += high(at) && low(at + 1) ? 2 : 1;
    }
}

}  // namespace

#else

bool regexExec(const regex::Program& program, std::u16string_view input, std::size_t start, regex::Matcher& matcher)
{
    return regex::exec(program, input, start, matcher);
}

#endif

}  // namespace jit
}  // namespace vm
}  // namespace internal
}  // namespace script
