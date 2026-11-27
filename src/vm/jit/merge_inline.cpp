// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Calls of other functions run in place. A function that has turned hot gets a copy of its block in which a `Call` whose
// callee the baseline code saw (the uid in `callProf`) is followed by that callee's own instructions, behind a guard
// (`JNotCode`) that the register holds a function of that code; when it does not, the call is made as before. The callee's
// registers sit where its frame would have put them (`this` right after the callee's register, then the arguments), its
// constants and names are added to the copy's, and what the baseline code learned about each of its instructions comes
// along, so the optimizing compiler sees one body with the profile of both. The interpreter and both compilers see an
// ordinary block.

#include "opt_analysis.hpp"
#include "script/internal/vm/bytecode.hpp"
#include "script/internal/vm/census.hpp"
#include "script/internal/vm/jit.hpp"
#include "script/internal/vm/object.hpp"
#include "script/internal/vm/vm.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <vector>

namespace script {
namespace internal {
namespace vm {

namespace {

constexpr std::uint32_t kMergeMaxCallee = 80;
constexpr std::uint32_t kMergeMaxAdded = 600;
constexpr unsigned kMergeMaxSites = 8;
constexpr unsigned kMergeMaxLevels = 3;  ///< with the SSA tier: calls in functions run in place, run in place
constexpr std::size_t kMergeMaxTotal = 1500;
constexpr std::uint32_t kMergeMaxLooping = 200;
constexpr std::uint32_t kMergeTinyCallee = 12;    ///< with the SSA tier: a callee this small is taken in at more sites
constexpr unsigned kMergeMaxTinySites = 64;
constexpr std::uint32_t kMergeMaxTinyAdded = 2000;
constexpr std::size_t kMergeMaxTotalSsa = 8000;
constexpr std::uint32_t kMergeLarge = 1000;

/// The instructions a callee run in place may hold: none looks at the frame beyond its registers (the closures and the home
/// object are the callee's own, which the copy has no way to name; `needsSelf` has those that take the function itself).
bool mergeOp(Op op)
{
    switch (op) {
    case Op::Nop: case Op::Cache: case Op::Mov: case Op::LoadK: case Op::LoadInt: case Op::LoadUndef: case Op::LoadNull:
    case Op::LoadTrue: case Op::LoadFalse: case Op::LoadHole: case Op::LoadUninit: case Op::CheckInit: case Op::GetGlobal:
    case Op::GetGlobalOrUndef: case Op::PutGlobal:
    case Op::Add: case Op::Sub: case Op::Mul: case Op::Div: case Op::Mod: case Op::Exp: case Op::BitAnd: case Op::BitOr:
    case Op::BitXor: case Op::Shl: case Op::Shr: case Op::UShr: case Op::Lt: case Op::Le: case Op::Gt: case Op::Ge:
    case Op::Eq: case Op::Ne: case Op::StrictEq: case Op::StrictNe: case Op::In: case Op::InstanceOf:
    case Op::AddI: case Op::SubI: case Op::MulI: case Op::ModI: case Op::BitAndI: case Op::BitOrI: case Op::BitXorI:
    case Op::ShlI: case Op::ShrI: case Op::UShrI: case Op::Neg: case Op::ToNumber: case Op::ToNumeric: case Op::ToString:
    case Op::ToConcat: case Op::StrCat:
    case Op::Not: case Op::BitNot: case Op::TypeOf: case Op::Inc: case Op::Dec: case Op::Append: case Op::Unshare:
    case Op::Jmp: case Op::JTrue: case Op::JFalse: case Op::JNullish: case Op::JNotNullish: case Op::JUndefined:
    case Op::JNotUndefined: case Op::JLt: case Op::JLe: case Op::JGt: case Op::JGe: case Op::JNotLt: case Op::JNotLe:
    case Op::JNotGt: case Op::JNotGe: case Op::JStrictNe: case Op::JStrictEq: case Op::JEq: case Op::JNe: case Op::JLtI: case Op::JLeI: case Op::JGtI:
    case Op::JGeI: case Op::JNotLtI: case Op::JNotLeI: case Op::JNotGtI: case Op::JNotGeI: case Op::JStrictNeI:
    case Op::JStrictEqI: case Op::JNotCode:
    case Op::GetProp: case Op::PutProp: case Op::GetElem: case Op::PutElem: case Op::NewObject: case Op::NewArray:
    case Op::ArrayPush: case Op::InitProp: case Op::InitElem: case Op::Call: case Op::New: case Op::Ret: case Op::Throw:
        return true;
    default:
        return false;
    }
}

/// What takes the function itself: its upvalues' boxes and the function are constants of the copy, whose guard then tests
/// for that function. Only where there is one function of the code.
bool needsSelf(Op op)
{
    return op == Op::GetUpval || op == Op::PutUpval || op == Op::LoadCallee;
}

/// Whether the function `C` may be run in place of a call with `argc` arguments.
bool mergeable(const CodeBlock* C, unsigned argc)
{
    const std::uint32_t n = std::uint32_t(C->code.size());
    if (!n || n > kMergeMaxCallee || !C->handlers.empty() || !C->evalScopes.empty() || C->lines.size() != n ||
        C->prof.empty() || argc > C->nparams || C->uid > 0x7fffffffu || C->inlinedFrom)
        return false;
    if (C->flags & (CodeBlock::kGenerator | CodeBlock::kAsync | CodeBlock::kAsyncGen | CodeBlock::kNeedsArgs | CodeBlock::kArgCount |
                    CodeBlock::kClassConstructor | CodeBlock::kProgram | CodeBlock::kDerived | CodeBlock::kLazy))
        return false;
    for (std::uint32_t j = 0; j < n; j = opt::nextOf(*C, j)) {
        const Op op = C->code[j].opcode();
        if (needsSelf(op) ? C->made != 1 || !C->only || C->only->code != C : !mergeOp(op))
            return false;
        if (!opt::effectsOf(*C, j).ok)
            return false;
        const std::int64_t t = opt::jumpTarget(*C, j);
        // (Without the SSA tier a loop is no small method: its own optimized code runs it better than a copy in each caller.)
        if (t > std::int64_t(n) || (t >= 0 && t <= std::int64_t(j) && ssaTierMode() < 2))
            return false;
    }
    return C->code[n - 1].opcode() == Op::Ret || C->code[n - 1].opcode() == Op::Throw;
}

/// The functions the call at `at` has called, as far as that is few: as their blocks are now.
std::vector<CodeBlock*> calleesAt(VM& vm, const CodeBlock& c, std::uint32_t at)
{
    std::vector<CodeBlock*> out;
    auto add = [&](std::uint64_t uid) {
        CodeBlock* C = uid ? vm.codeByUid(uid) : nullptr;
        // The one function of a block that runs another by now.
        while (C && C->mergedInto && C->made == 1)
            C = vm.codeByUid(C->mergedInto);
        if (C && std::find(out.begin(), out.end(), C) == out.end())
            out.push_back(C);
    };
    if (ssaTierMode() >= 2 && at < c.callTargetAt.size() && c.callTargetAt[at]) {
        const CodeBlock::CallTargets& t = c.callTargets[c.callTargetAt[at] - 1];
        if (t.more)
            return out;
        for (std::uint64_t uid : t.uid)
            add(uid);
        if (!out.empty())
            return out;
    }
    add(c.callProf[at]);
    return out;
}

/// A copy of `c` with its calls of small functions run in place; with `deeper`, the calls in what `c` itself already runs in
/// place. Null when there are none.
CodeBlock* mergeOnce(VM& vm, CodeBlock* c, std::uint32_t loop, bool deeper)
{
    const std::uint32_t n = std::uint32_t(c->code.size());
    if (!n || c->callProf.size() != n || c->prof.empty() || c->lines.size() != n || !c->evalScopes.empty() ||
        c->inlinedFrom || c->mergedFrom || c->inlineSites.empty() == deeper)
        return nullptr;
    if (c->flags & (CodeBlock::kGenerator | CodeBlock::kAsync | CodeBlock::kAsyncGen | CodeBlock::kNeedsArgs | CodeBlock::kArgCount |
                    CodeBlock::kClassConstructor | CodeBlock::kProgram | CodeBlock::kDerived | CodeBlock::kLazy))
        return nullptr;

    // The calls to take in: their callee, and what it adds.
    struct Site
    {
        std::uint32_t at;
        CodeBlock* callee;
    };
    std::vector<Site> sites;
    std::uint32_t added = 0, addedTiny = 0;
    unsigned normal = 0;
    // With the SSA tier, which inlines nothing itself, a tiny callee (`uint` in compiled C) is taken in at many more sites.
    const bool ssaTier = ssaTierMode() >= 2;
    auto tiny = [&](const CodeBlock* C) { return ssaTier && C->code.size() <= kMergeTinyCallee; };
    for (std::uint32_t j = 0; j < n; j = opt::nextOf(*c, j)) {
        const Ins& in = c->code[j];
        if (in.opcode() != Op::Call || !c->callProf[j] || in.a > in.b)
            continue;
        for (CodeBlock* C : calleesAt(vm, *c, j)) {
            if (C == c || !mergeable(C, in.c))
                continue;
            if (deeper) {
                // In a function run in place, and none it is in is that function (whose block may be another by now).
                auto same = [&](const CodeBlock* b) { return b == C || (b->source == C->source && b->srcStart == C->srcStart && b->srcEnd == C->srcEnd); };
                bool within = false, again = same(c);
                for (const InlineSite& s : c->inlineSites) {
                    again = again || s.end == j;  // the long way of one run in place
                    if (j >= s.start && j < s.end) {
                        within = true;
                        again = again || (s.callee && same(s.callee));
                    }
                }
                if (!within || again)
                    continue;
            }
            const std::uint32_t cost = std::uint32_t(C->code.size()) + C->nregs + 4;
            if (tiny(C)) {
                if (sites.size() >= kMergeMaxTinySites || addedTiny + cost > kMergeMaxTinyAdded)
                    continue;
                addedTiny += cost;
                sites.push_back(Site{j, C});
                continue;
            }
            if (normal >= kMergeMaxSites || added + cost > kMergeMaxAdded)
                continue;
            added += cost;
            ++normal;
            sites.push_back(Site{j, C});
        }
    }
    // A large function is not copied, compiled again and warmed up again for little.
    if (sites.empty() || (n > kMergeLarge && (added + addedTiny) < n / 16))
        return nullptr;
    if (loop != ~0u) {
        // A large function that loops is copied and compiled again for little: only small ones, or (with the SSA tier)
        // only its calls of tiny callees.
        if (n > kMergeMaxLooping) {
            if (!ssaTier)
                return nullptr;
            sites.erase(std::remove_if(sites.begin(), sites.end(), [&](const Site& s) { return !tiny(s.callee); }),
                        sites.end());
            if (sites.empty())
                return nullptr;
        }
        // The loop runs from its header to its last back edge.
        std::int64_t end = -1;
        for (std::uint32_t j = 0; j < n; j = opt::nextOf(*c, j))
            if (opt::jumpTarget(*c, j) == std::int64_t(loop))
                end = std::max<std::int64_t>(end, j);
        bool inLoop = false;
        for (const Site& s : sites)
            inLoop = inLoop || (s.at >= loop && std::int64_t(s.at) <= end);
        if (!inLoop)
            return nullptr;
    }

    CodeBlock* d = vm.newCodeBlock(c->name, c->length, c->constructor);
    d->flags = c->flags;
    // A function that names `#x` keeps a `PrivateCache` for each instruction: the copy has those of its instructions,
    // from the block each came from (a cache holds no count).
    bool hasPriv = (c->flags & CodeBlock::kPrivate) != 0;
    for (const Site& s : sites)
        hasPriv = hasPriv || (s.callee->flags & CodeBlock::kPrivate) != 0;
    if (hasPriv)
        d->flags |= CodeBlock::kPrivate;
    d->nparams = c->nparams;
    d->nregs = c->nregs;
    d->thisSlots = c->thisSlots;
    d->file = c->file;
    d->source = c->source;
    d->srcStart = c->srcStart;
    d->srcEnd = c->srcEnd;
    d->upvals = c->upvals;
    d->evalHome = c->evalHome;
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

    const bool hasElemProf = c->elemProf.size() == n, hasElemKinds = c->elemKinds.size() == n,
               hasGlobalRecs = c->globalRecs.size() == n, hasElemSite = c->elemSite.size() == n;
    // The side tables are kept one entry to an instruction as the code grows.
    auto putSide = [&](const CodeBlock& from, std::uint32_t at, bool call) {
        d->prof.push_back(at < from.prof.size() ? from.prof[at] : std::uint8_t(0));
        d->callProf.push_back(at < from.callProf.size() ? from.callProf[at] : 0);
        std::uint32_t targets = 0;
        if (call) {
            const bool had = at < from.callTargetAt.size() && from.callTargetAt[at];
            d->callTargets.push_back(had ? from.callTargets[from.callTargetAt[at] - 1] : CodeBlock::CallTargets{});
            targets = std::uint32_t(d->callTargets.size());
        }
        d->callTargetAt.push_back(targets);
        if (hasElemProf)
            d->elemProf.push_back(at < from.elemProf.size() ? from.elemProf[at] : std::uint8_t(0));
        if (hasElemKinds)
            d->elemKinds.push_back(at < from.elemKinds.size() ? from.elemKinds[at] : std::uint8_t(0));
        if (hasGlobalRecs)
            d->globalRecs.push_back(at < from.globalRecs.size() ? from.globalRecs[at] : CodeBlock::GlobalRec{});
        std::uint32_t site = 0;
        if (at < from.elemSite.size() && from.elemSite[at]) {
            const std::uint32_t first = from.elemSite[at] - 1;
            site = std::uint32_t(d->elemCaches.size()) + 1;
            for (std::uint32_t w = 0; w < ElemCache::kWays; ++w) {
                ElemCache e = from.elemCaches[first + w];
                if (e.validity)
                    ++e.validity->refs;
                if (e.key)
                    detail::incRef(e.key);
                d->elemCaches.push_back(e);
            }
        }
        if (hasElemSite)
            d->elemSite.push_back(site);
        if (hasPriv)
            d->privCaches.push_back(at < from.privCaches.size() ? from.privCaches[at] : PrivateCache{});
    };
    auto putCache = [&](const CodeBlock& from, std::uint16_t id) -> std::uint16_t {
        if (id == kNoCache || id >= from.caches.size())
            return kNoCache;
        InlineCache ic = from.caches[id];
        if (ic.validity)
            ++ic.validity->refs;
        if (ic.pad && ic.pad <= from.poly.size()) {
            PolyCache pc = from.poly[ic.pad - 1];
            for (std::uint8_t k = 0; k < pc.n; ++k)
                if (pc.e[k].validity)
                    ++pc.e[k].validity->refs;
            d->poly.push_back(pc);
            ic.pad = std::uint16_t(d->poly.size());
        } else {
            ic.pad = 0;
        }
        d->caches.push_back(ic);
        return std::uint16_t(d->caches.size() - 1);
    };
    auto put = [&](Ins in, std::uint32_t line, const CodeBlock& from, std::uint32_t at) {
        d->code.push_back(in);
        d->lines.push_back(line);
        putSide(from, at, in.opcode() == Op::Call);
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
    bool tooFar = false;
    auto fits16 = [&](std::uint32_t from, std::uint32_t to) {
        const std::int64_t delta = std::int64_t(to) - std::int64_t(from) - 1;
        return delta >= -32768 && delta <= 32767;
    };
    std::map<std::pair<const CodeBlock*, std::uint32_t>, std::uint32_t> constOf;
    auto constFor = [&](const CodeBlock& from, std::uint32_t k) {
        auto it = constOf.find({&from, k});
        if (it != constOf.end())
            return it->second;
        retain(from.consts[k]);
        d->consts.push_back(from.consts[k]);
        return constOf[{&from, k}] = std::uint32_t(d->consts.size() - 1);
    };
    std::map<std::pair<const CodeBlock*, std::uint32_t>, std::uint32_t> atomOf;
    auto atomFor = [&](const CodeBlock& from, std::uint32_t k) {
        auto it = atomOf.find({&from, k});
        if (it != atomOf.end())
            return it->second;
        detail::incRef(from.atoms[k]);
        d->atoms.push_back(from.atoms[k]);
        return atomOf[{&from, k}] = std::uint32_t(d->atoms.size() - 1);
    };

    // The caller's instruction `k` (and its cache word) as it was, its registers, names and constants its own.
    std::vector<std::uint32_t> moved(n + 1, 0);
    std::vector<std::pair<std::uint32_t, std::uint32_t>> ownJumps;  // (new index, old target)
    for (std::uint32_t k = 0; k < n;) {
        moved[k] = std::uint32_t(d->code.size());
        const Ins in = c->code[k];
        const std::uint32_t len = opt::nextOf(*c, k) - k;
        auto site = std::find_if(sites.begin(), sites.end(), [&](const Site& s) { return s.at == k; });
        if (site != sites.end()) {
            const unsigned inner = in.b + 1u, argc = in.c;
            const std::uint32_t line = c->lines[k];
            std::vector<std::uint32_t> outs;
            // The caller tests the result right after the call (`if (o.m())`): each return gets its own copy of the test,
            // so no value has to join the returns first. A return that a test of the same register jumps to (`a || b`)
            // gets copies that know the answer and go straight to where the caller's test would.
            const std::uint32_t testAt = k + len;
            const bool tested = ssaTier && testAt < n &&
                                (c->code[testAt].opcode() == Op::JTrue || c->code[testAt].opcode() == Op::JFalse) &&
                                c->code[testAt].a == in.a;
            const std::uint32_t testTaken = tested ? std::uint32_t(opt::jumpTarget(*c, testAt)) : 0;
            const std::uint32_t testNext = tested ? opt::nextOf(*c, testAt) : 0;
            for (; site != sites.end() && site->at == k; ++site) {
                CodeBlock* C = site->callee;
                d->nregs = std::max<std::uint32_t>(d->nregs, inner + C->nregs + 1);
                if (inner + C->nregs + 1 > 0xFFF0)
                    tooFar = true;
                SCRIPT_VM_CENSUS(MergedSites);
                std::vector<bool> reached;
                const std::vector<bool> needInit = readBeforeWritten(*C, reached);
                const std::uint32_t guard = put(Ins::make(Op::JNotCode, in.b, std::uint16_t(0), 0), line, *c, k);
                {
                    JSValue what = JSValue::int32(std::int32_t(C->uid));
                    for (std::uint32_t m = 0; m < C->code.size(); m = opt::nextOf(*C, m))
                        if (reached[m] && needsSelf(C->code[m].opcode()))
                            what = toJSValue(C->only);
                    retain(what);
                    d->consts.push_back(what);
                    d->code[guard].b = std::uint16_t(d->consts.size() - 1);
                    if (d->consts.size() > 0xFFFF)
                        tooFar = true;
                }
                for (unsigned p = argc; p < C->nparams; ++p)
                    put(Ins::make(Op::LoadUndef, std::uint16_t(inner + 1 + p)), line, *c, k);
                for (unsigned r = 1u + C->nparams; r < C->nregs; ++r)
                    if (needInit[r])
                        put(Ins::make(Op::LoadUndef, std::uint16_t(inner + r)), line, *c, k);
                const std::uint32_t body = std::uint32_t(d->code.size());
                const std::uint32_t cn = std::uint32_t(C->code.size());
                std::vector<std::uint32_t> cmoved(cn + 1, 0);
                std::vector<std::pair<std::uint32_t, std::uint32_t>> calleeJumps;
                // Per instruction of the callee: a test (1 `JTrue`, 2 `JFalse`) of a register that jumps to the return of
                // that register; with 4, the register holds a boolean there (a compare right before made it, and nothing
                // else jumps to the test). `known`: where the copies of a return are, by the test (its kind and register).
                std::vector<std::uint8_t> toRet(cn, 0);
                std::vector<std::uint32_t> retOf(cn, 0);
                std::map<std::pair<std::uint32_t, unsigned>, std::uint32_t> known;
                std::vector<std::pair<std::uint32_t, std::pair<std::uint32_t, unsigned>>> knownJumps;
                if (tested) {
                    std::vector<bool> target(cn + 1, false);
                    for (std::uint32_t m = 0; m < cn; m = opt::nextOf(*C, m))
                        if (const std::int64_t t = opt::jumpTarget(*C, m); t >= 0)
                            target[std::size_t(t)] = true;
                    std::uint32_t prev = cn;
                    for (std::uint32_t m = 0; m < cn; prev = m, m = opt::nextOf(*C, m)) {
                        const Ins& j = C->code[m];
                        const std::int64_t t = opt::jumpTarget(*C, m);
                        if (!reached[m] || (j.opcode() != Op::JTrue && j.opcode() != Op::JFalse) || t < 0 || t >= cn)
                            continue;
                        // The return, or a move into the register it returns.
                        std::uint32_t ret = std::uint32_t(t);
                        std::uint16_t reg = j.a;
                        if (C->code[ret].opcode() == Op::Mov && C->code[ret].b == reg && ret + 1 < cn) {
                            reg = C->code[ret].a;
                            ++ret;
                        }
                        if (C->code[ret].opcode() != Op::Ret || C->code[ret].a != reg)
                            continue;
                        retOf[m] = ret;
                        toRet[m] = j.opcode() == Op::JTrue ? 1 : 2;
                        if (prev < cn && !target[m] && C->code[prev].a == j.a) {
                            switch (C->code[prev].opcode()) {
                            case Op::Lt: case Op::Le: case Op::Gt: case Op::Ge: case Op::Eq: case Op::Ne: case Op::StrictEq:
                            case Op::StrictNe: case Op::Not: case Op::In: case Op::InstanceOf:
                                toRet[m] |= 4;
                                break;
                            default:
                                break;
                            }
                        }
                    }
                }
                for (std::uint32_t m = 0; m < cn; ++m) {
                    cmoved[m] = std::uint32_t(d->code.size());
                    Ins ci = C->code[m];
                    const Op op = ci.opcode();
                    const bool live = op == Op::Cache ? m > 0 && reached[m - 1] : reached[m];
                    if (!live) {
                        put(Ins::make(Op::Nop), C->lines[m], *C, m);
                        continue;
                    }
                    if (op == Op::Ret) {
                        // The value to the call's destination, the registers let go as the frame of a call would have on its
                        // way out, then on after the call.
                        put(Ins::make(Op::Mov, in.a, std::uint16_t(ci.a + inner)), C->lines[m], *C, m);
                        for (unsigned r = 0; r < C->nregs; ++r)
                            put(Ins::make(Op::LoadUndef, std::uint16_t(inner + r)), C->lines[m], *C, m);
                        if (!tested) {
                            outs.push_back(put(Ins::make(Op::Jmp), C->lines[m], *C, m));
                            continue;
                        }
                        ownJumps.emplace_back(put(c->code[testAt], c->lines[testAt], *c, testAt), testTaken);
                        ownJumps.emplace_back(put(Ins::make(Op::Jmp), C->lines[m], *C, m), testNext);
                        for (std::uint32_t j = 0; j < cn; j = opt::nextOf(*C, j)) {
                            const unsigned what = unsigned(toRet[j]) | unsigned(C->code[j].a) << 8;
                            if (!toRet[j] || retOf[j] != m || known.count({m, what}))
                                continue;
                            // The register is true-like (`JTrue` jumped) or false-like here.
                            const bool truthy = (toRet[j] & 3) == 1;
                            known[{m, what}] = std::uint32_t(d->code.size());
                            if (toRet[j] & 4)
                                put(Ins::make(truthy ? Op::LoadTrue : Op::LoadFalse, in.a), C->lines[m], *C, m);
                            else
                                put(Ins::make(Op::Mov, in.a, std::uint16_t(C->code[j].a + inner)), C->lines[m], *C, m);
                            for (unsigned r = 0; r < C->nregs; ++r)
                                put(Ins::make(Op::LoadUndef, std::uint16_t(inner + r)), C->lines[m], *C, m);
                            const bool taken = truthy == (c->code[testAt].opcode() == Op::JTrue);
                            ownJumps.emplace_back(put(Ins::make(Op::Jmp), C->lines[m], *C, m), taken ? testTaken : testNext);
                        }
                        continue;
                    }
                    if (needsSelf(op)) {
                        const bool store = op == Op::PutUpval;
                        const std::uint32_t n = store ? ci.a : ci.b;
                        const std::uint16_t reg = std::uint16_t(inner + (store ? C->nregs : ci.a));
                        const JSValue k = op == Op::LoadCallee ? toJSValue(C->only)
                                                               : JSValue::cell(reinterpret_cast<CellPtr>(C->only->upvals()[n]));
                        retain(k);
                        d->consts.push_back(k);
                        Ins load = Ins::make(Op::LoadK, reg);
                        load.b = std::uint16_t((d->consts.size() - 1) & 0xFFFF);
                        load.c = std::uint16_t((d->consts.size() - 1) >> 16);
                        put(load, C->lines[m], *C, m);
                        if (op == Op::GetUpval)
                            put(Ins::make(Op::GetBox, reg, reg), C->lines[m], *C, m);
                        if (store) {
                            put(Ins::make(Op::PutBox, reg, std::uint16_t(ci.b + inner)), C->lines[m], *C, m);
                            put(Ins::make(Op::LoadUndef, reg), C->lines[m], *C, m);
                        }
                        if (d->consts.size() > 0xFFFFFFu)
                            tooFar = true;
                        continue;
                    }
                    if (op == Op::Cache) {
                        ci.a = putCache(*C, ci.a);
                    } else {
                        const char* kinds = opKinds(op);
                        std::uint16_t* fields[3] = {&ci.a, &ci.b, &ci.c};
                        for (int f = 0; f < 3; ++f) {
                            if (kinds[f] == 'R') {
                                *fields[f] = std::uint16_t(*fields[f] + inner);
                            } else if (kinds[f] == 'A') {
                                *fields[f] = std::uint16_t(atomFor(*C, *fields[f]));
                            } else if (kinds[f] == 'K') {
                                if (op == Op::LoadK) {
                                    const std::uint32_t idx = constFor(*C, ci.kIndex());
                                    ci.b = std::uint16_t(idx & 0xFFFF);
                                    ci.c = std::uint16_t(idx >> 16);
                                } else {
                                    *fields[f] = std::uint16_t(constFor(*C, *fields[f]));
                                }
                            }
                        }
                        if (d->atoms.size() > 0xFFFF || d->consts.size() > 0xFFFFFFu)
                            tooFar = true;
                    }
                    const std::uint32_t at = put(ci, C->lines[m], *C, m);
                    const std::int64_t t = op == Op::Cache ? -1 : opt::jumpTarget(*C, m);
                    if (t >= 0 && toRet[m])
                        knownJumps.emplace_back(at, std::make_pair(retOf[m], unsigned(toRet[m]) | unsigned(C->code[m].a) << 8));
                    else if (t >= 0)
                        calleeJumps.emplace_back(at, std::uint32_t(t));
                }
                cmoved[cn] = std::uint32_t(d->code.size());
                for (const auto& [at, t] : calleeJumps) {
                    const char* kinds = opKinds(d->code[at].opcode());
                    if (kinds[1] != '-' && kinds[1] != 'J' && kinds[2] == 'J' && !fits16(at, cmoved[t]))
                        tooFar = true;
                    else
                        jumpTo(at, cmoved[t]);
                }
                for (const auto& [at, t] : knownJumps)
                    jumpTo(at, known.at(t));
                d->inlineSites.push_back(InlineSite{body, std::uint32_t(d->code.size()), guard, C});
                ++C->refs;
                // A callee that is itself a merged copy brings the calls it runs in place.
                for (const InlineSite& s : C->inlineSites) {
                    d->inlineSites.push_back(InlineSite{cmoved[s.start], cmoved[s.end], cmoved[s.site], s.callee});
                    if (s.callee)
                        ++s.callee->refs;
                }
                // The guard's miss goes to the next one's, or to the call the long way, which falls through to what follows.
                if (!fits16(guard, std::uint32_t(d->code.size())))
                    tooFar = true;
                else
                    jumpTo(guard, std::uint32_t(d->code.size()));
            }
            // The call and its cache word, as they were.
            for (std::uint32_t m = 0; m < len; ++m) {
                Ins x = c->code[k + m];
                if (x.opcode() == Op::Cache)
                    x.a = putCache(*c, x.a);
                put(x, c->lines[k + m], *c, k + m);
            }
            for (std::uint32_t at : outs)
                jumpTo(at, std::uint32_t(d->code.size()));
            k += len;
            continue;
        }
        for (std::uint32_t m = 0; m < len; ++m) {
            Ins x = c->code[k + m];
            if (x.opcode() == Op::Cache)
                x.a = putCache(*c, x.a);
            const std::uint32_t at = put(x, c->lines[k + m], *c, k + m);
            const std::int64_t t = x.opcode() == Op::Cache ? -1 : opt::jumpTarget(*c, k + m);
            if (t >= 0)
                ownJumps.emplace_back(at, std::uint32_t(t));
        }
        k += len;
    }
    moved[n] = std::uint32_t(d->code.size());
    for (const auto& [at, t] : ownJumps) {
        const char* kinds = opKinds(d->code[at].opcode());
        if (kinds[1] != '-' && kinds[1] != 'J' && kinds[2] == 'J' && !fits16(at, moved[t]))
            tooFar = true;
        else
            jumpTo(at, moved[t]);
    }
    for (const Handler& h : c->handlers) {
        Handler g = h;
        g.start = moved[h.start];
        g.end = moved[h.end];
        g.target = moved[h.target];
        d->handlers.push_back(g);
    }
    // The parameters' feedback, past the instructions'.
    for (std::uint32_t p = 0; p < c->nparams + 2u; ++p)
        d->prof.push_back(n + p < c->prof.size() ? c->prof[n + p] : std::uint8_t(0));
    if (tooFar || d->code.size() > (ssaTier ? kMergeMaxTotalSsa : kMergeMaxTotal)) {
        vm.releaseCode(d);
        return nullptr;
    }
    for (const InlineSite& s : c->inlineSites) {
        d->inlineSites.push_back(InlineSite{moved[s.start], moved[s.end], moved[s.site], s.callee});
        if (s.callee)
            ++s.callee->refs;
    }
#if defined(SCRIPT_JIT_ENV_OVERRIDE)
    if (std::getenv("T2INL")) {
        auto nm = [](const CodeBlock* b) { return b->name ? toUtf8(static_cast<StringCell*>(b->name)->text()) : std::string("anon"); };
        std::fprintf(stderr, "merge %s line %u: %zu -> %zu instructions, sites:", nm(c).c_str(),
                     c->lines.empty() ? 0u : unsigned(c->lines[0]), c->code.size(), d->code.size());
        for (const Site& s : sites)
            std::fprintf(stderr, " %s:%u@%u", nm(s.callee).c_str(), s.callee->lines.empty() ? 0u : unsigned(s.callee->lines[0]), s.at);
        std::fprintf(stderr, "\n");
    }
#endif
    return d;
}

}  // namespace

CodeBlock* mergeInline(VM& vm, CodeBlock* c, std::uint32_t loop)
{
    if (c->mergeTried)
        return nullptr;
    c->mergeTried = true;
    CodeBlock* d = mergeOnce(vm, c, loop, false);
    for (unsigned level = 1; d && level < kMergeMaxLevels && ssaTierMode() >= 2; ++level) {
        CodeBlock* e = mergeOnce(vm, d, ~0u, true);
        if (!e)
            break;
        vm.releaseCode(d);
        d = e;
    }
    if (d)
        d->mergedFrom = c;
    return d;
}

}  // namespace vm
}  // namespace internal
}  // namespace script
