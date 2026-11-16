// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Recursion run in place one level deep. A small function that calls itself by its global name (`fib(n - 1)`) gets a copy of
// its block in which each such `Call` jumps to a stub at the end: when the callee is the running function itself, the stub
// runs the function's own instructions with their registers moved to where the call's frame would have put them, and its
// `Ret` hands the value back to the call's destination; otherwise the stub makes the call as before. Constants, names,
// children and upvalues are the function's own, so the copied instructions mean in place what they mean in a frame of
// their own. The interpreter, the baseline and the optimizing compilers see an ordinary block; the optimizing compiler
// makes one body of both levels, which halves the frames a recursion pushes.

#include "opt_analysis.hpp"
#include "script/internal/vm/bytecode.hpp"
#include "script/internal/vm/jit.hpp"
#include "script/internal/vm/object.hpp"
#include "script/internal/vm/vm.hpp"

#include <algorithm>
#include <vector>

namespace script {
namespace internal {
namespace vm {

namespace {

constexpr std::uint32_t kSelfInlineMaxCode = 64;
constexpr unsigned kSelfInlineMaxSites = 4;
constexpr std::size_t kSelfInlineMaxTotal = 1024;

/// The instructions a body run in place may hold: none looks at the frame beyond its registers and its function.
bool inPlaceOp(Op op)
{
    switch (op) {
    case Op::Nop: case Op::Cache: case Op::Mov: case Op::LoadK: case Op::LoadInt: case Op::LoadUndef: case Op::LoadNull:
    case Op::LoadTrue: case Op::LoadFalse: case Op::LoadHole: case Op::LoadUninit: case Op::CheckInit: case Op::GetGlobal:
    case Op::GetGlobalOrUndef: case Op::PutGlobal: case Op::GetUpval: case Op::PutUpval:
    case Op::Add: case Op::Sub: case Op::Mul: case Op::Div: case Op::Mod: case Op::Exp: case Op::BitAnd: case Op::BitOr:
    case Op::BitXor: case Op::Shl: case Op::Shr: case Op::UShr: case Op::Lt: case Op::Le: case Op::Gt: case Op::Ge:
    case Op::Eq: case Op::Ne: case Op::StrictEq: case Op::StrictNe: case Op::In: case Op::InstanceOf:
    case Op::AddI: case Op::SubI: case Op::MulI: case Op::ModI: case Op::BitAndI: case Op::BitOrI: case Op::BitXorI:
    case Op::ShlI: case Op::ShrI: case Op::UShrI: case Op::Neg: case Op::ToNumber: case Op::ToNumeric: case Op::ToString:
    case Op::Not: case Op::BitNot: case Op::TypeOf: case Op::Inc: case Op::Dec:
    case Op::Jmp: case Op::JTrue: case Op::JFalse: case Op::JNullish: case Op::JNotNullish: case Op::JUndefined:
    case Op::JNotUndefined: case Op::JLt: case Op::JLe: case Op::JGt: case Op::JGe: case Op::JNotLt: case Op::JNotLe:
    case Op::JNotGt: case Op::JNotGe: case Op::JStrictNe: case Op::JStrictEq: case Op::JLtI: case Op::JLeI: case Op::JGtI:
    case Op::JGeI: case Op::JNotLtI: case Op::JNotLeI: case Op::JNotGtI: case Op::JNotGeI: case Op::JStrictNeI:
    case Op::JStrictEqI:
    case Op::GetProp: case Op::PutProp: case Op::GetElem: case Op::PutElem: case Op::NewObject: case Op::NewArray:
    case Op::ArrayPush: case Op::InitProp: case Op::InitElem: case Op::Call: case Op::New: case Op::Ret: case Op::Throw:
    case Op::ThrowError:
        return true;
    default:
        return false;
    }
}

/// The registers of `c` some path from the entry reads before writing, past `this` and the parameters; `reached` gets which
/// instructions a path from the entry reaches.
std::vector<bool> readBeforeWritten(const CodeBlock& c, std::vector<bool>& reached)
{
    const std::uint32_t n = std::uint32_t(c.code.size());
    const unsigned nregs = c.nregs;
    std::vector<bool> need(nregs, false);
    // For each instruction, the registers written on every path to it (empty: not reached yet).
    std::vector<std::vector<bool>> written(n);
    std::vector<std::uint32_t> work{0};
    written[0].assign(nregs, false);
    for (unsigned r = 0; r <= c.nparams && r < nregs; ++r)
        written[0][r] = true;
    auto flow = [&](std::uint32_t to, const std::vector<bool>& w) {
        if (to >= n)
            return;
        if (written[to].empty()) {
            written[to] = w;
            work.push_back(to);
            return;
        }
        bool changed = false;
        for (unsigned r = 0; r < nregs; ++r)
            if (written[to][r] && !w[r]) {
                written[to][r] = false;
                changed = true;
            }
        if (changed)
            work.push_back(to);
    };
    while (!work.empty()) {
        const std::uint32_t j = work.back();
        work.pop_back();
        std::vector<bool> w = written[j];
        const opt::Effects e = opt::effectsOf(c, j);
        for (std::uint16_t r : e.rd)
            if (r < nregs && !w[r])
                need[r] = true;
        for (std::uint16_t r : e.wr)
            if (r < nregs)
                w[r] = true;
        const Op op = c.code[j].opcode();
        const std::int64_t t = opt::jumpTarget(c, j);
        if (t >= 0)
            flow(std::uint32_t(t), w);
        if (op != Op::Jmp && op != Op::Ret && op != Op::Throw && op != Op::ThrowError)
            flow(opt::nextOf(c, j), w);
    }
    reached.assign(n, false);
    for (std::uint32_t j = 0; j < n; ++j)
        reached[j] = !written[j].empty();
    return need;
}

}  // namespace

CodeBlock* selfInline(VM& vm, CodeBlock* c)
{
    const int depth = vm.selfInlining;
    if (c->selfInlined)
        return c->selfInlined;
    if (c->selfInlineTried || c->inlinedFrom)
        return nullptr;
    c->selfInlineTried = true;
    const std::uint32_t n = std::uint32_t(c->code.size());
    if (!c->name || n == 0 || n > kSelfInlineMaxCode || !c->handlers.empty() || !c->evalScopes.empty() ||
        c->lines.size() != n)
        return nullptr;
    if (c->flags & (CodeBlock::kGenerator | CodeBlock::kAsync | CodeBlock::kAsyncGen | CodeBlock::kNeedsArgs |
                    CodeBlock::kClassConstructor | CodeBlock::kProgram | CodeBlock::kDerived))
        return nullptr;
    std::vector<bool> target(n + 1, false);
    bool readsThis = false;
    for (std::uint32_t j = 0; j < n; j = opt::nextOf(*c, j)) {
        if (!inPlaceOp(c->code[j].opcode()))
            return nullptr;
        const opt::Effects e = opt::effectsOf(*c, j);
        if (!e.ok)
            return nullptr;
        for (std::uint16_t r : e.rd)
            readsThis = readsThis || r == 0;
        const std::int64_t t = opt::jumpTarget(*c, j);
        if (t >= 0) {
            if (t > std::int64_t(n))
                return nullptr;
            target[std::size_t(t)] = true;
        }
    }
    // A sloppy function's `this` is the global object when the call passes none: only a frame of its own makes it so.
    if (readsThis && !(c->flags & CodeBlock::kStrict))
        return nullptr;

    // The calls of the function by its own global name, the name read by the instructions just before with no way in
    // between.
    std::vector<std::uint32_t> sites;
    for (std::uint32_t j = 0; j < n; j = opt::nextOf(*c, j)) {
        const Ins& in = c->code[j];
        if (in.opcode() != Op::Call || target[j])
            continue;
        for (std::int64_t k = std::int64_t(j) - 1; k >= 0; --k) {
            const Ins& d = c->code[std::size_t(k)];
            if (d.opcode() == Op::Cache)
                continue;
            const opt::Effects e = opt::effectsOf(*c, std::uint32_t(k));
            if (std::find(e.wr.begin(), e.wr.end(), in.b) != e.wr.end()) {
                if (d.opcode() == Op::GetGlobal && d.b < c->atoms.size() && c->atoms[d.b] == c->name)
                    sites.push_back(j);
                break;
            }
            if (target[std::size_t(k)] || opt::jumpTarget(*c, std::uint32_t(k)) >= 0 || d.opcode() == Op::Ret)
                break;
        }
        if (sites.size() > kSelfInlineMaxSites)
            return nullptr;
    }
    if (sites.empty())
        return nullptr;
    std::vector<bool> reached;
    const std::vector<bool> needInit = readBeforeWritten(*c, reached);

    CodeBlock* d = vm.newCodeBlock(c->name, c->length, c->constructor);
    d->flags = c->flags;
    d->nparams = c->nparams;
    d->nregs = c->nregs;
    for (JSValue v : c->consts) {
        retain(v);
        d->consts.push_back(v);
    }
    for (Key k : c->atoms) {
        detail::incRef(k);
        d->atoms.push_back(k);
    }
    for (CodeBlock* child : c->children) {
        ++child->refs;
        d->children.push_back(child);
    }
    d->upvals = c->upvals;
    d->evalHome = c->evalHome;
    d->caches.resize(c->caches.size());
    d->file = c->file;
    d->source = c->source;
    d->srcStart = c->srcStart;
    d->srcEnd = c->srcEnd;

    auto put = [&](Ins in, std::uint32_t line) {
        d->code.push_back(in);
        d->lines.push_back(line);
        return std::uint32_t(d->code.size() - 1);
    };
    auto jumpTo = [&](std::uint32_t from, std::uint32_t to) {
        Ins& in = d->code[from];
        const char* kinds = opKinds(in.opcode());
        if (kinds[1] != '-' && kinds[1] != 'J' && kinds[2] == 'J')  // two operands: 16 bits in c
            in.c = std::uint16_t(std::int16_t(std::int32_t(to) - std::int32_t(from) - 1));
        else
            in.setJump(std::int32_t(to) - std::int32_t(from) - 1);
    };
    // Each call site in place: the guard, the body, its returns, then the call the long way, which falls through to what
    // follows. Every jump goes forward. Jumps of a copy's own instructions are moved to where their targets went. A body
    // in place has its own calls of the function in place in turn, down to `depth` levels.
    bool tooFar = false;
    // The function's instructions with registers moved up by `off`; at a level below the top a return goes to the
    // instruction after the copy with the value in `dst` (its jumps are left in `outs`).
    auto emitCopy = [&](auto& self, int level, unsigned off, unsigned dst, std::vector<std::uint32_t>& outs) -> void {
        std::vector<std::uint32_t> moved(n + 1, 0);
        std::vector<std::pair<std::uint32_t, std::uint32_t>> ownJumps;  // (new index, old target)
        for (std::uint32_t k = 0; k < n; ++k) {
            moved[k] = std::uint32_t(d->code.size());
            Ins in = c->code[k];
            const Op op = in.opcode();
            const bool live = op == Op::Cache ? k > 0 && reached[k - 1] : reached[k];
            if (level > 0 && !live) {
                // Never runs (`return undefined` after a last `return`): nothing of it may seem to flow anywhere.
                put(Ins::make(Op::Nop), c->lines[k]);
                continue;
            }
            const bool site = level < depth && std::find(sites.begin(), sites.end(), k) != sites.end();
            if (site) {
                const unsigned inner = off + in.b + 1u, argc = in.c;
                const std::uint32_t line = c->lines[k];
                const std::uint32_t guard = put(Ins::make(Op::JNotCallee, std::uint16_t(off + in.b)), line);
                for (unsigned p = argc; p < c->nparams; ++p)
                    put(Ins::make(Op::LoadUndef, std::uint16_t(inner + 1 + p)), line);
                for (unsigned r = 1u + c->nparams; r < c->nregs; ++r)
                    if (needInit[r])
                        put(Ins::make(Op::LoadUndef, std::uint16_t(inner + r)), line);
                const std::uint32_t body = std::uint32_t(d->code.size());
                std::vector<std::uint32_t> back;
                self(self, level + 1, inner, off + in.a, back);
                d->inlineSites.push_back(InlineSite{body, std::uint32_t(d->code.size()), guard});
                jumpTo(guard, std::uint32_t(d->code.size()));
                Ins call = in;
                call.a = std::uint16_t(call.a + off);
                call.b = std::uint16_t(call.b + off);
                put(call, line);
                for (std::uint32_t at : back)
                    jumpTo(at, std::uint32_t(d->code.size()));
                d->nregs = std::max<std::uint32_t>(d->nregs, inner + c->nregs);
                if (inner + c->nregs > 0xFFF0)
                    tooFar = true;
                continue;
            }
            if (level > 0 && op == Op::Ret) {
                // The value to the call's destination, the body's registers let go (as the frame of a call would have on
                // its way out), then on after the call.
                put(Ins::make(Op::Mov, std::uint16_t(dst), std::uint16_t(in.a + off)), c->lines[k]);
                for (unsigned r = 0; r < c->nregs; ++r)
                    put(Ins::make(Op::LoadUndef, std::uint16_t(off + r)), c->lines[k]);
                outs.push_back(put(Ins::make(Op::Jmp), c->lines[k]));
                continue;
            }
            if (op == Op::Cache) {
                if (level > 0 && in.a != kNoCache) {
                    in.a = std::uint16_t(d->caches.size());
                    d->caches.emplace_back();
                }
            } else if (off) {
                const char* kinds = opKinds(op);
                std::uint16_t* fields[3] = {&in.a, &in.b, &in.c};
                for (int f = 0; f < 3 && kinds[f]; ++f)
                    if (kinds[f] == 'R')
                        *fields[f] = std::uint16_t(*fields[f] + off);
            }
            const std::uint32_t at = put(in, c->lines[k]);
            const std::int64_t t = op == Op::Cache ? -1 : opt::jumpTarget(*c, k);
            if (t >= 0)
                ownJumps.emplace_back(at, std::uint32_t(t));
        }
        moved[n] = std::uint32_t(d->code.size());
        for (const auto& [at, t] : ownJumps) {
            const std::int64_t delta = std::int64_t(moved[t]) - std::int64_t(at) - 1;
            const char* kinds = opKinds(d->code[at].opcode());
            if (kinds[1] != '-' && kinds[1] != 'J' && kinds[2] == 'J' && (delta < -32768 || delta > 32767))
                tooFar = true;
            else
                jumpTo(at, moved[t]);
        }
    };
    std::vector<std::uint32_t> none;
    emitCopy(emitCopy, 0, 0, 0, none);
    if (tooFar || d->code.size() > kSelfInlineMaxTotal) {
        vm.releaseCode(d);
        return nullptr;
    }
    d->inlinedFrom = c;
    ++c->refs;
    c->selfInlined = d;
    // No count of its own: each function that takes it holds one (`tierUpBlock`).
    --d->refs;
    return d;
}

CodeBlock* tierUpBlock(VM& vm, JSFunction* f, CodeBlock* c)
{
    if (!vm.selfInlining)
        return c;
    CodeBlock* d = selfInline(vm, c);
    if (!d)
        return c;
    ++d->refs;
    f->code = d;
    vm.releaseCode(c);
    return d;
}

}  // namespace vm
}  // namespace internal
}  // namespace script
