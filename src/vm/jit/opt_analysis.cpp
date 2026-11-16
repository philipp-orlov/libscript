// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "opt_analysis.hpp"

#include <algorithm>
#include <numeric>

namespace script {
namespace internal {
namespace vm {
namespace opt {

std::uint32_t nextOf(const CodeBlock& code, std::uint32_t i)
{
    return i + ((i + 1 < code.code.size() && code.code[i + 1].opcode() == Op::Cache) ? 2 : 1);
}

bool isConditionalJump(Op op)
{
    switch (op) {
    case Op::JTrue: case Op::JFalse: case Op::JNullish: case Op::JNotNullish: case Op::JUndefined: case Op::JNotUndefined:
    case Op::JHole: case Op::JLt: case Op::JLe: case Op::JGt: case Op::JGe: case Op::JNotLt: case Op::JNotLe: case Op::JNotGt:
    case Op::JNotGe: case Op::JStrictNe: case Op::JStrictEq: case Op::JLtI: case Op::JLeI: case Op::JGtI: case Op::JGeI:
    case Op::JNotLtI: case Op::JNotLeI: case Op::JNotGtI: case Op::JNotGeI: case Op::JStrictNeI: case Op::JStrictEqI:
        return true;
    default:
        return false;
    }
}

std::int64_t jumpTarget(const CodeBlock& code, std::uint32_t i)
{
    const Ins& in = code.code[i];
    switch (in.opcode()) {
    case Op::Jmp: case Op::JTrue: case Op::JFalse: case Op::JNullish: case Op::JNotNullish: case Op::JUndefined:
    case Op::JNotUndefined: case Op::JHole:
        return std::int64_t(i) + 1 + in.jump();
    case Op::JLt: case Op::JLe: case Op::JGt: case Op::JGe: case Op::JNotLt: case Op::JNotLe: case Op::JNotGt: case Op::JNotGe:
    case Op::JStrictNe: case Op::JStrictEq: case Op::JLtI: case Op::JLeI: case Op::JGtI: case Op::JGeI: case Op::JNotLtI:
    case Op::JNotLeI: case Op::JNotGtI: case Op::JNotGeI: case Op::JStrictNeI: case Op::JStrictEqI:
        return std::int64_t(i) + 1 + in.jump16();
    default:
        return -1;
    }
}

Effects effectsOf(const CodeBlock& code, std::uint32_t i)
{
    const Ins& in = code.code[i];
    Effects e;
    auto rd = [&](unsigned r) { e.rd.push_back(std::uint16_t(r)); };
    auto wr = [&](unsigned r) { e.wr.push_back(std::uint16_t(r)); };
    switch (in.opcode()) {
    case Op::Nop: case Op::Jmp: case Op::ThrowError:
        break;
    case Op::Mov:
        rd(in.b); wr(in.a);
        break;
    case Op::LoadK: case Op::LoadInt: case Op::LoadUndef: case Op::LoadNull: case Op::LoadTrue: case Op::LoadFalse:
    case Op::LoadHole: case Op::LoadUninit: case Op::GetGlobal: case Op::GetGlobalOrUndef: case Op::GetUpval:
    case Op::LoadCallee: case Op::NewObject: case Op::NewArray: case Op::NewRegExp:
        wr(in.a);
        break;
    case Op::CheckInit:
        rd(in.a);
        break;
    case Op::PutGlobal: case Op::DefGlobal:
        rd(in.b);
        break;
    case Op::NewBox: case Op::CopyBox:
        rd(in.a); wr(in.a);
        break;
    case Op::GetBox:
        rd(in.b); wr(in.a);
        break;
    case Op::PutBox:
        rd(in.a); rd(in.b);
        break;
    case Op::PutUpval:
        rd(in.b);
        break;
    case Op::Add: case Op::Sub: case Op::Mul: case Op::Div: case Op::Mod: case Op::Exp: case Op::BitAnd: case Op::BitOr:
    case Op::BitXor: case Op::Shl: case Op::Shr: case Op::UShr: case Op::Lt: case Op::Le: case Op::Gt: case Op::Ge:
    case Op::Eq: case Op::Ne: case Op::StrictEq: case Op::StrictNe: case Op::In: case Op::InstanceOf: case Op::GetElem:
    case Op::DelElem:
        rd(in.b); rd(in.c); wr(in.a);
        break;
    case Op::AddI: case Op::SubI: case Op::MulI: case Op::ModI: case Op::BitAndI: case Op::BitOrI: case Op::BitXorI:
    case Op::ShlI: case Op::ShrI: case Op::UShrI: case Op::Neg: case Op::ToNumber: case Op::ToNumeric: case Op::ToString:
    case Op::Not: case Op::BitNot: case Op::TypeOf: case Op::Inc: case Op::Dec: case Op::GetProp: case Op::DelProp:
    case Op::KeysOf: case Op::ToKey:
        rd(in.b); wr(in.a);
        break;
    case Op::GetSuperProp:
        rd(in.c); wr(in.a);
        break;
    case Op::JTrue: case Op::JFalse: case Op::JNullish: case Op::JNotNullish: case Op::JUndefined: case Op::JNotUndefined:
    case Op::JHole: case Op::JLtI: case Op::JLeI: case Op::JGtI: case Op::JGeI: case Op::JNotLtI: case Op::JNotLeI:
    case Op::JNotGtI: case Op::JNotGeI: case Op::JStrictNeI: case Op::JStrictEqI: case Op::Ret: case Op::Throw:
        rd(in.a);
        break;
    case Op::JLt: case Op::JLe: case Op::JGt: case Op::JGe: case Op::JNotLt: case Op::JNotLe: case Op::JNotGt: case Op::JNotGe:
    case Op::JStrictNe: case Op::JStrictEq:
        rd(in.a); rd(in.b);
        break;
    case Op::PutProp: case Op::InitProp: case Op::InitGetter: case Op::InitSetter:
        rd(in.a); rd(in.c);
        break;
    case Op::PutElem: case Op::InitElem: case Op::InitGetterElem: case Op::InitSetterElem:
        rd(in.a); rd(in.b); rd(in.c);
        break;
    case Op::ArrayPush: case Op::SetProto:
        rd(in.a); rd(in.b);
        break;
    case Op::Closure: {
        wr(in.a);
        const CodeBlock* child = code.children[in.b];
        for (const UpvalDesc& d : child->upvals)
            if (d.fromParentLocal)
                rd(d.index);
        break;
    }
    case Op::Call: case Op::New:
        for (unsigned r = in.b; r < in.b + 2u + in.c; ++r)
            rd(r);
        wr(in.a);
        if (in.opcode() == Op::New)
            wr(in.b + 1u);
        break;
    case Op::IterInit:
        rd(in.b); wr(in.a); wr(in.a + 1u);
        break;
    case Op::IterNext:
        rd(in.b); rd(in.b + 1u); wr(in.a); wr(in.b + 1u);
        break;
    case Op::IterClose:
        rd(in.a); rd(in.a + 1u); wr(in.a + 1u);
        break;
    default:
        e.ok = false;
        break;
    }
    return e;
}

namespace {

using Bits = std::vector<std::uint64_t>;

inline bool bitTest(const Bits& b, std::size_t i) { return (b[i >> 6] >> (i & 63)) & 1; }
inline void bitSet(Bits& b, std::size_t i) { b[i >> 6] |= std::uint64_t(1) << (i & 63); }
inline void bitClear(Bits& b, std::size_t i) { b[i >> 6] &= ~(std::uint64_t(1) << (i & 63)); }
inline bool orInto(Bits& dst, const Bits& src)
{
    bool changed = false;
    for (std::size_t k = 0; k < dst.size(); ++k) {
        const std::uint64_t v = dst[k] | src[k];
        if (v != dst[k]) {
            dst[k] = v;
            changed = true;
        }
    }
    return changed;
}

struct UnionFind
{
    std::vector<std::uint32_t> parent;
    explicit UnionFind(std::size_t n) : parent(n)
    {
        std::iota(parent.begin(), parent.end(), 0u);
    }
    std::uint32_t find(std::uint32_t x)
    {
        while (parent[x] != x) {
            parent[x] = parent[parent[x]];
            x = parent[x];
        }
        return x;
    }
    void join(std::uint32_t a, std::uint32_t b)
    {
        a = find(a);
        b = find(b);
        if (a != b)
            parent[std::max(a, b)] = std::min(a, b);
    }
};

Rep joinRep(Rep a, Rep b)
{
    return std::uint8_t(a) > std::uint8_t(b) ? a : b;
}

}  // namespace

Plan analyse(const CodeBlock& code)
{
    Plan plan;
    const std::uint32_t n = std::uint32_t(code.code.size());
    const std::uint32_t nregs = code.nregs;
    plan.n = n;
    if (n == 0 || n > 6000 || nregs == 0 || nregs > 600 || (code.flags & (CodeBlock::kGenerator | CodeBlock::kAsync | CodeBlock::kAsyncGen)))
        return plan;

    // ---- instructions, effects, successors ----
    std::vector<std::uint8_t> isIns(n, 0);
    std::vector<Effects> eff(n);
    for (std::uint32_t i = 0; i < n; i = nextOf(code, i)) {
        isIns[i] = 1;
        eff[i] = effectsOf(code, i);
        if (!eff[i].ok)
            return plan;
        for (std::uint16_t r : eff[i].rd)
            if (r >= nregs)
                return plan;
        for (std::uint16_t r : eff[i].wr)
            if (r >= nregs)
                return plan;
    }
    plan.isLeader.assign(n, 0);
    plan.isLoopHeader.assign(n, 0);
    plan.isLeader[0] = 1;
    std::vector<std::vector<std::uint32_t>> succ(n), pred(n);
    auto addEdge = [&](std::uint32_t from, std::uint32_t to) {
        if (to >= n || !isIns[to])
            return;
        succ[from].push_back(to);
        pred[to].push_back(from);
    };
    for (std::uint32_t i = 0; i < n; i = nextOf(code, i)) {
        const Op op = code.code[i].opcode();
        const std::uint32_t nx = nextOf(code, i);
        const std::int64_t t = jumpTarget(code, i);
        if (t >= 0) {
            if (t >= std::int64_t(n) || !isIns[std::size_t(t)])
                return plan;
            addEdge(i, std::uint32_t(t));
            plan.isLeader[std::size_t(t)] = 1;
            if (std::uint32_t(t) <= i) {
                plan.isLoopHeader[std::size_t(t)] = 1;
            }
            if (isConditionalJump(op))
                addEdge(i, nx);
            if (nx < n)
                plan.isLeader[nx] = 1;
        } else if (op != Op::Ret && op != Op::Throw && op != Op::ThrowError) {
            addEdge(i, nx);
        } else if (nx < n) {
            plan.isLeader[nx] = 1;
        }
    }
    // A jump to the code's own end is a return: there is no instruction there.
    for (const Handler& h : code.handlers) {
        if (h.target >= n || !isIns[h.target])
            return plan;
        plan.isLeader[h.target] = 1;
        for (std::uint32_t i = h.start; i < h.end && i < n; i = nextOf(code, i))
            addEdge(i, h.target);
    }
    for (std::uint32_t i = 0; i < n; i = nextOf(code, i))
        if (plan.isLoopHeader[i])
            plan.loopHeaders.push_back(i);

    // ---- definitions ----
    // One id per (instruction, register written), one per register for its value at entry, one per handler for the thrown
    // value it receives.
    struct Def
    {
        std::uint16_t reg;
        std::uint32_t ins;  // instruction, or ~0u for entry
        bool handler;
    };
    std::vector<Def> defs;
    std::vector<std::vector<std::uint32_t>> defsOfReg(nregs);
    std::vector<std::vector<std::uint32_t>> insDefs(n);  // def ids an instruction creates
    auto newDef = [&](std::uint16_t reg, std::uint32_t ins, bool handler) {
        const std::uint32_t id = std::uint32_t(defs.size());
        defs.push_back({reg, ins, handler});
        defsOfReg[reg].push_back(id);
        return id;
    };
    std::vector<std::uint32_t> entryDef(nregs);
    for (std::uint16_t r = 0; r < nregs; ++r)
        entryDef[r] = newDef(r, ~0u, false);
    for (std::uint32_t i = 0; i < n; i = nextOf(code, i)) {
        for (std::uint16_t r : eff[i].wr)
            insDefs[i].push_back(newDef(r, i, false));
    }
    std::vector<std::uint32_t> handlerDef(code.handlers.size());
    for (std::size_t h = 0; h < code.handlers.size(); ++h) {
        handlerDef[h] = newDef(code.handlers[h].reg, code.handlers[h].target, true);
        insDefs[code.handlers[h].target].push_back(handlerDef[h]);
    }
    const std::size_t D = defs.size();
    const std::size_t W = (D + 63) / 64;

    // ---- reaching definitions, instruction by instruction ----
    std::vector<Bits> in(n, Bits(W, 0)), out(n, Bits(W, 0));
    for (std::uint16_t r = 0; r < nregs; ++r)
        bitSet(in[0], entryDef[r]);
    std::vector<std::uint32_t> work;
    std::vector<std::uint8_t> queued(n, 0);
    for (std::uint32_t i = 0; i < n; i = nextOf(code, i)) {
        work.push_back(i);
        queued[i] = 1;
    }
    std::reverse(work.begin(), work.end());
    auto transfer = [&](std::uint32_t i, Bits& result) {
        result = in[i];
        for (std::uint32_t id : insDefs[i])
            for (std::uint32_t other : defsOfReg[defs[id].reg])
                bitClear(result, other);
        for (std::uint32_t id : insDefs[i])
            bitSet(result, id);
    };
    while (!work.empty()) {
        const std::uint32_t i = work.back();
        work.pop_back();
        queued[i] = 0;
        Bits result;
        transfer(i, result);
        out[i] = result;
        // An exception can leave the instruction before or after it has written: both states reach a handler.
        for (std::uint32_t s : succ[i]) {
            bool changed = orInto(in[s], out[i]);
            for (const Handler& h : code.handlers)
                if (h.target == s && i >= h.start && i < h.end)
                    changed |= orInto(in[s], in[i]);
            if (changed && !queued[s]) {
                queued[s] = 1;
                work.push_back(s);
            }
        }
    }

    // ---- webs ----
    UnionFind uf(D);
    std::vector<std::vector<std::uint32_t>> useDef(n);  // a def id for each read, parallel to eff[i].rd
    for (std::uint32_t i = 0; i < n; i = nextOf(code, i)) {
        for (std::uint16_t r : eff[i].rd) {
            std::uint32_t first = ~0u;
            for (std::uint32_t id : defsOfReg[r]) {
                if (!bitTest(in[i], id))
                    continue;
                if (first == ~0u)
                    first = id;
                else
                    uf.join(first, id);
            }
            if (first == ~0u)
                first = entryDef[r];
            useDef[i].push_back(first);
        }
    }
    // A write that is also a read of the same register (NewBox, IterNext) is one value's life; handled by the above.

    // ---- liveness (registers), backward ----
    std::vector<Bits> liveIn(n, Bits((nregs + 63) / 64, 0));
    {
        std::vector<std::uint32_t> wl;
        std::vector<std::uint8_t> q(n, 0);
        for (std::uint32_t i = 0; i < n; i = nextOf(code, i)) {
            wl.push_back(i);
            q[i] = 1;
        }
        while (!wl.empty()) {
            const std::uint32_t i = wl.back();
            wl.pop_back();
            q[i] = 0;
            Bits live((nregs + 63) / 64, 0);
            for (std::uint32_t s : succ[i])
                orInto(live, liveIn[s]);
            for (std::uint16_t r : eff[i].wr)
                bitClear(live, r);
            for (const Handler& h : code.handlers)
                if (h.target == i)
                    bitClear(live, h.reg);
            for (std::uint16_t r : eff[i].rd)
                bitSet(live, r);
            if (live != liveIn[i]) {
                liveIn[i] = live;
                for (std::uint32_t p : pred[i])
                    if (!q[p]) {
                        q[p] = 1;
                        wl.push_back(p);
                    }
            }
        }
    }

    // ---- what each web holds ----
    // A web is named by the least def id of its class.
    std::vector<Rep> rep(D, Rep::None);
    auto webOf = [&](std::uint32_t def) { return uf.find(def); };
    auto raise = [&](std::uint32_t def, Rep r) {
        const std::uint32_t w = webOf(def);
        const Rep joined = joinRep(rep[w], r);
        if (joined != rep[w]) {
            rep[w] = joined;
            return true;
        }
        return false;
    };
    // Webs that must stay in the frame: those that meet a handler, and those that are live when one starts.
    for (std::size_t h = 0; h < code.handlers.size(); ++h)
        raise(handlerDef[h], Rep::Mem);
    for (const Handler& h : code.handlers) {
        for (std::uint16_t r = 0; r < nregs; ++r) {
            if (!bitTest(liveIn[h.target], r))
                continue;
            for (std::uint32_t id : defsOfReg[r])
                if (bitTest(in[h.target], id))
                    raise(id, Rep::Mem);
        }
    }
    // What arrives in the frame: `this` and anything not a parameter is unknown; a parameter is what calls passed.
    auto argRep = [&](std::uint32_t r) -> Rep {
        if (r < 1 || r > code.nparams || code.prof.size() < n + code.nparams)
            return Rep::Mem;
        const std::uint8_t p = code.prof[n + (r - 1)];
        if (p == 1)
            return Rep::I32;
        if (p == 2 || p == 3)
            return Rep::F64;
        return Rep::Mem;
    };
    for (std::uint16_t r = 0; r < nregs; ++r)
        raise(entryDef[r], argRep(r));

    // ---- int32 truncation ----
    // A web whose every use converts it with ToInt32 (a bitwise operator), or is an add or subtract whose own result is such a
    // web, may be computed modulo 2^32: the exact sum of int32s is far below 2^53, so wrapping changes nothing ToInt32 sees.
    auto truncating = [](Op op) {
        switch (op) {
        case Op::BitAnd: case Op::BitOr: case Op::BitXor: case Op::Shl: case Op::Shr: case Op::UShr: case Op::BitAndI:
        case Op::BitOrI: case Op::BitXorI: case Op::ShlI: case Op::ShrI: case Op::UShrI: case Op::BitNot:
            return true;
        default:
            return false;
        }
    };
    auto additive = [](Op op) {
        return op == Op::Add || op == Op::Sub || op == Op::AddI || op == Op::SubI || op == Op::Inc || op == Op::Dec;
    };
    std::vector<std::uint8_t> truncOnly(D, 0), used(D, 0);
    for (std::uint32_t i = 0; i < n; i = nextOf(code, i))
        for (std::uint32_t d : useDef[i])
            used[webOf(d)] = 1;
    for (std::uint32_t id = 0; id < D; ++id)
        truncOnly[id] = used[id];
    for (bool changed = true; changed;) {
        changed = false;
        for (std::uint32_t i = 0; i < n; i = nextOf(code, i)) {
            const Op op = code.code[i].opcode();
            for (std::uint32_t d : useDef[i]) {
                const std::uint32_t w = webOf(d);
                if (!truncOnly[w])
                    continue;
                bool ok = truncating(op);
                if (!ok && additive(op) && !insDefs[i].empty())
                    ok = truncOnly[webOf(insDefs[i][0])] != 0;
                if (!ok) {
                    truncOnly[w] = 0;
                    changed = true;
                }
            }
        }
    }
    // A product by a constant wraps too: its input is never truncated-only through it (a product needs the whole value), so
    // the input is an exact int32, and the exact product (below 2^47) agrees with the wrapped one in its low 32 bits.
    auto wrapping = [&](std::uint32_t i) {
        const Op op = code.code[i].opcode();
        return (additive(op) || op == Op::MulI) && !insDefs[i].empty() && truncOnly[webOf(insDefs[i][0])];
    };

    auto useRep = [&](std::uint32_t i, std::size_t k) { return rep[webOf(useDef[i][k])]; };
    auto profOf = [&](std::uint32_t i) { return i < code.prof.size() ? code.prof[i] : std::uint8_t(0); };
    auto fromProfile = [&](std::uint32_t i) -> Rep {
        const std::uint8_t p = profOf(i);
        if ((p & kProfFailed) || p == 0)
            return Rep::Mem;
        if (p == 1)
            return Rep::I32;
        if (p == 2 || p == 3)
            return Rep::F64;
        return Rep::Mem;
    };
    // The type of what instruction `i` writes to its first register, given what it reads.
    auto defType = [&](std::uint32_t i) -> Rep {
        const Ins& ins = code.code[i];
        const std::uint8_t p = profOf(i);
        auto two = [&]() -> Rep {  // both operands numbers? the join of their types, None while unknown
            const Rep a = useRep(i, 0), b = useRep(i, 1);
            if (a == Rep::None || b == Rep::None)
                return Rep::None;
            if (a == Rep::Mem || b == Rep::Mem)
                return Rep::Mem;
            return joinRep(a, b);
        };
        auto one = [&]() -> Rep {
            const Rep a = useRep(i, 0);
            return a;
        };
        if (wrapping(i)) {
            // Overflow does not matter here: the result is int32 whenever the operands are.
            const Op op = ins.opcode();
            const bool binary = op == Op::Add || op == Op::Sub;
            const Rep a = useRep(i, 0), b = binary ? useRep(i, 1) : Rep::I32;
            if (a == Rep::None || b == Rep::None)
                return Rep::None;
            if (a == Rep::I32 && b == Rep::I32)
                return Rep::I32;
        }
        if (p & kProfFailed) {
            // A guard failed here. Where what it made can be said by what the interpreter then saw (arithmetic that
            // overflowed made a double), the profile has it; elsewhere the instruction goes back to the frame.
            switch (ins.opcode()) {
            case Op::LoadInt: case Op::LoadK: case Op::Mov: case Op::Add: case Op::Sub: case Op::Mul: case Op::Div:
            case Op::AddI: case Op::SubI: case Op::MulI: case Op::Inc: case Op::Dec: case Op::Neg:
                break;
            default:
                return Rep::Mem;
            }
        }
        switch (ins.opcode()) {
        case Op::LoadInt:
            return Rep::I32;
        case Op::LoadK: {
            const JSValue k = code.consts[ins.b];
            return k.isInt32() ? Rep::I32 : k.isDouble() ? Rep::F64 : Rep::Mem;
        }
        case Op::Mov:
            return one();
        case Op::Add: case Op::Sub: case Op::Mul: {
            const Rep t = two();
            if (t != Rep::I32)
                return t;
            return (p & 2) ? Rep::F64 : Rep::I32;
        }
        case Op::Div: {
            const Rep t = two();
            if (t != Rep::I32)
                return t;
            return (p & 2) ? Rep::F64 : Rep::I32;
        }
        case Op::Mod: {
            const Rep t = two();
            return t == Rep::I32 ? Rep::I32 : t == Rep::None ? Rep::None : Rep::Mem;
        }
        case Op::BitAnd: case Op::BitOr: case Op::BitXor: case Op::Shl: case Op::Shr: {
            const Rep t = two();
            return t == Rep::None ? Rep::None : t == Rep::Mem ? Rep::Mem : Rep::I32;
        }
        case Op::UShr: {
            const Rep t = two();
            if (t == Rep::None || t == Rep::Mem)
                return t;
            return (p & 2) ? Rep::F64 : Rep::I32;
        }
        case Op::AddI: case Op::SubI: case Op::MulI: case Op::Inc: case Op::Dec: case Op::Neg: {
            const Rep t = one();
            if (t != Rep::I32)
                return t;
            return (p & 2) ? Rep::F64 : Rep::I32;
        }
        case Op::ModI: {
            const Rep t = one();
            return t == Rep::I32 ? Rep::I32 : t == Rep::None ? Rep::None : Rep::Mem;
        }
        case Op::BitAndI: case Op::BitOrI: case Op::BitXorI: case Op::ShlI: case Op::ShrI: {
            const Rep t = one();
            return t == Rep::None ? Rep::None : t == Rep::Mem ? Rep::Mem : Rep::I32;
        }
        case Op::UShrI: {
            const Rep t = one();
            if (t == Rep::None || t == Rep::Mem)
                return t;
            return (p & 2) ? Rep::F64 : Rep::I32;
        }
        case Op::GetProp: case Op::GetElem: case Op::GetBox: case Op::GetUpval: case Op::GetGlobal: case Op::GetGlobalOrUndef:
        case Op::Call:
            return fromProfile(i);
        default:
            return Rep::Mem;
        }
    };
    for (bool changed = true; changed;) {
        changed = false;
        for (std::uint32_t i = 0; i < n; i = nextOf(code, i)) {
            for (std::size_t k = 0; k < eff[i].wr.size(); ++k) {
                const Rep t = k == 0 ? defType(i) : Rep::Mem;
                if (t == Rep::None)
                    continue;
                if (raise(insDefs[i][k], t))
                    changed = true;
            }
        }
    }
    // A web with an untyped def, or never given one, stays in the frame.
    for (std::uint32_t id = 0; id < D; ++id)
        if (webOf(id) == id && rep[id] == Rep::None)
            rep[id] = Rep::Mem;

    // ---- the plan: typed webs, who uses them, what is live ----
    std::vector<std::int32_t> webIndex(D, -1);
    for (std::uint32_t id = 0; id < D; ++id) {
        if (webOf(id) != id || (rep[id] != Rep::I32 && rep[id] != Rep::F64))
            continue;
        webIndex[id] = std::int32_t(plan.webs.size());
        Web w;
        w.reg = defs[id].reg;
        w.rep = rep[id];
        plan.webs.push_back(w);
    }
    auto typedWeb = [&](std::uint32_t def) -> std::int32_t { return webIndex[webOf(def)]; };
    plan.defWeb.assign(n, -1);
    plan.wraps.assign(n, 0);
    for (std::uint32_t i = 0; i < n; i = nextOf(code, i)) {
        if (!wrapping(i) || rep[webOf(insDefs[i][0])] != Rep::I32)
            continue;
        bool allInt = true;
        for (std::uint32_t d : useDef[i])
            allInt = allInt && rep[webOf(d)] == Rep::I32;
        plan.wraps[i] = allInt;
    }
    plan.useWebs.assign(n, {});
    plan.liveBefore.assign(n, {});
    // Loop depth, for weights.
    std::vector<std::uint32_t> depth(n, 0);
    for (std::uint32_t i = 0; i < n; i = nextOf(code, i)) {
        const std::int64_t t = jumpTarget(code, i);
        if (t >= 0 && std::uint32_t(t) <= i)
            for (std::uint32_t k = std::uint32_t(t); k <= i; ++k)
                ++depth[k];
    }
    auto touch = [&](std::int32_t w, std::uint32_t i) {
        if (w < 0)
            return;
        Web& web = plan.webs[std::size_t(w)];
        web.first = std::min(web.first, i);
        web.last = std::max(web.last, i);
        std::uint32_t wgt = 1;
        for (std::uint32_t d = 0; d < std::min<std::uint32_t>(depth[i], 4); ++d)
            wgt *= 8;
        web.weight += wgt;
    };
    for (std::uint32_t i = 0; i < n; i = nextOf(code, i)) {
        for (std::size_t k = 0; k < eff[i].rd.size(); ++k) {
            const std::int32_t w = typedWeb(useDef[i][k]);
            plan.useWebs[i].push_back(w);
            touch(w, i);
            if (w >= 0 && defs[useDef[i][k]].ins == ~0u) {
                plan.webs[std::size_t(w)].hasEntryDef = true;
                plan.webs[std::size_t(w)].first = 0;
            }
        }
        if (!eff[i].wr.empty()) {
            const std::int32_t w = typedWeb(insDefs[i][0]);
            plan.defWeb[i] = w;
            touch(w, i);
        }
        // What is live here and typed.
        for (std::uint16_t r = 0; r < nregs; ++r) {
            if (!bitTest(liveIn[i], r))
                continue;
            for (std::uint32_t id : defsOfReg[r]) {
                if (!bitTest(in[i], id))
                    continue;
                const std::int32_t w = typedWeb(id);
                if (w >= 0) {
                    plan.liveBefore[i].push_back({r, std::uint32_t(w)});
                    if (defs[id].ins == ~0u)
                        plan.webs[std::size_t(w)].hasEntryDef = true;
                }
                break;
            }
        }
    }
    // Webs that live across a loop's back edge live for all of it.
    for (bool grew = true; grew;) {
        grew = false;
        for (std::uint32_t h : plan.loopHeaders) {
            // The back edges to h.
            for (std::uint32_t j : pred[h]) {
                if (j < h)
                    continue;
                for (const LiveWeb& lw : plan.liveBefore[h]) {
                    Web& w = plan.webs[lw.web];
                    if (w.first > j || w.last < h)
                        continue;
                    if (w.first > h) {
                        w.first = h;
                        grew = true;
                    }
                    if (w.last < j) {
                        w.last = j;
                        grew = true;
                    }
                }
            }
        }
    }
    for (std::uint32_t k = 0; k < plan.webs.size(); ++k) {
        plan.webs[k].slot = std::int32_t(k);
        if (plan.webs[k].hasEntryDef)
            plan.entryWebs.push_back(k);
        if (plan.webs[k].first == ~0u)
            plan.webs[k].first = 0;
    }
    plan.stackSlots = std::uint32_t(plan.webs.size());
    plan.typedWebs = std::uint32_t(plan.webs.size());

    // ---- registers: the heaviest webs first, a register whose other webs do not overlap ----
    std::vector<std::uint32_t> order(plan.webs.size());
    std::iota(order.begin(), order.end(), 0u);
    std::stable_sort(order.begin(), order.end(), [&](std::uint32_t a, std::uint32_t b) {
        return plan.webs[a].weight > plan.webs[b].weight;
    });
    struct Interval
    {
        std::uint32_t first, last;
    };
    std::vector<Interval> gpr[16], xmm[16];
    constexpr std::uint8_t kGprPool[] = {8, 9, 10, 11};
    for (std::uint32_t wi : order) {
        Web& w = plan.webs[wi];
        std::vector<Interval>* regs = w.rep == Rep::I32 ? gpr : xmm;
        auto fits = [&](std::uint8_t r) {
            for (const Interval& iv : regs[r])
                if (!(iv.last < w.first || iv.first > w.last))
                    return false;
            return true;
        };
        bool placed = false;
        if (w.rep == Rep::I32) {
            for (std::uint8_t r : kGprPool)
                if (fits(r)) {
                    regs[r].push_back({w.first, w.last});
                    w.loc = {Loc::Reg, r};
                    placed = true;
                    break;
                }
        } else {
            for (std::uint8_t r = 2; r < 16; ++r)
                if (fits(r)) {
                    regs[r].push_back({w.first, w.last});
                    w.loc = {Loc::Reg, r};
                    placed = true;
                    break;
                }
        }
        if (!placed)
            w.loc = {Loc::Stack, 0};
    }
    plan.ok = true;
    return plan;
}

}  // namespace opt
}  // namespace vm
}  // namespace internal
}  // namespace script
