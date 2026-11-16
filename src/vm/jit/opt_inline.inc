// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Inlining for the optimizing compiler (included from baseline_x64.cpp after opt_emit.inc, inside `Baseline`'s namespace).
//
// A `Call`, or a property read that finds a getter, whose callee is a small straight-line function (property reads,
// arithmetic, upvalues, `super.x`, calls of the same kind, a return) runs the callee's instructions in place: no frame,
// the arguments read where the caller keeps them, the result delivered as a typed value. The callee's values live in slots
// of the optimized frame. Every guard of an inlined body comes before its first side effect, so a guard that fails leaves
// the caller as it was before the instruction, which then runs again in the interpreter.

namespace {

constexpr std::uint32_t kInlineMaxCode = 48;
constexpr int kInlineDepth = 2;

bool readsThis(const CodeBlock& c)
{
    for (std::uint32_t j = 0; j < c.code.size(); j = opt::nextOf(c, j))
        for (std::uint16_t r : opt::effectsOf(c, j).rd)
            if (r == 0)
                return true;
    return false;
}

std::string codeName(const CodeBlock* c)
{
    return c->name ? toUtf8(static_cast<StringCell*>(c->name)->text()) : std::string("anon");
}

}  // namespace

Mem Baseline::inlineSlot(std::int32_t s) const
{
    return mem(RSP, inlineBase_ + 8 * s);
}

bool Baseline::inlinable(const CodeBlock* C) const
{
    if (!C || C == code_ || C->prof.size() < C->code.size() + C->nparams || C->code.size() > kInlineMaxCode)
        return false;
    if (C->flags & (CodeBlock::kGenerator | CodeBlock::kAsync | CodeBlock::kAsyncGen | CodeBlock::kNeedsArgs |
                    CodeBlock::kClassConstructor))
        return false;
    if ((C->flags & CodeBlock::kArrow) && readsThis(*C))
        return false;
    return true;
}

/// The value `v` boxed (not counted) in rax. Clobbers rdx, xmm0.
void Baseline::inlineBoxed(const IVal& v)
{
    switch (v.kind) {
    case IVal::I32:
        inlineI32(v, RAX, nullptr);
        a_.or_(RAX, R14);
        break;
    case IVal::F64:
        inlineF64(v, 0, RAX, nullptr);
        boxDouble0();
        break;
    case IVal::Const:
        a_.movImm(RAX, v.c.bits);
        break;
    case IVal::Box:
        inlineBits(v, RAX);
        break;
    }
}

void Baseline::inlineBits(const IVal& v, Reg dst)
{
    if (v.kind == IVal::Const)
        a_.movImm(dst, v.c.bits);
    else if (v.frameReg >= 0)
        a_.mov(dst, R(unsigned(v.frameReg)));
    else if (v.slot <= -2)
        a_.mov(dst, mem(RSP, windowBase_ + 8 * (-2 - v.slot)));  // a window slot
    else
        a_.mov(dst, inlineSlot(v.slot));
}

void Baseline::inlineI32(const IVal& v, Reg dst, Label* bad)
{
    switch (v.kind) {
    case IVal::I32:
        if (v.web >= 0) {
            const Reg r = useI32(v.web, dst);
            if (r != dst)
                a_.mov32(dst, r);
        } else {
            a_.mov32(dst, inlineSlot(v.slot));
        }
        break;
    case IVal::Const:
        a_.movImm(dst, std::uint32_t(v.c.asInt32()));
        break;
    case IVal::Box:
        inlineBits(v, dst);
        needInt(dst, *bad);
        a_.mov32(dst, dst);
        break;
    default:
        break;
    }
}

/// `tmp` and rdx are clobbered.
void Baseline::inlineF64(const IVal& v, int x, Reg tmp, Label* bad)
{
    switch (v.kind) {
    case IVal::I32:
        if (v.web >= 0) {
            a_.cvtsi2sd32(x, useI32(v.web, tmp));
        } else {
            a_.mov32(tmp, inlineSlot(v.slot));
            a_.cvtsi2sd32(x, tmp);
        }
        break;
    case IVal::F64:
        if (v.web >= 0) {
            const int r = operandF64(v.web, x, tmp);
            if (r != x)
                a_.movaps(x, r);
        } else {
            a_.movsdLoad(x, inlineSlot(v.slot));
        }
        break;
    case IVal::Const:
        a_.movImm(tmp, doubleBits(v.c.asNumber()));
        a_.movqToX(x, tmp);
        break;
    case IVal::Box:
        inlineBits(v, tmp);
        needNumber(tmp, *bad);
        loadNumber(x, tmp, RDX);
        break;
    }
}

IVal Baseline::callerValue(std::uint32_t i, unsigned reg, std::size_t k, std::uint8_t hint) const
{
    if (auto w = windowVal_.find(i); w != windowVal_.end()) {
        if (auto o = w->second.find(k); o != w->second.end()) {
            IVal v = o->second;
            if (v.kind == IVal::Box && !v.hint)
                v.hint = hint;
            return v;
        }
    }
    const auto& uw = plan_->useWebs[i];
    IVal v;
    if (k < uw.size() && uw[k] >= 0) {
        v.kind = isI32(uw[k]) ? IVal::I32 : IVal::F64;
        v.web = uw[k];
    } else {
        v.kind = IVal::Box;
        v.frameReg = std::int32_t(reg);
        v.hint = hint;
    }
    return v;
}

bool Baseline::inlineCall(std::uint32_t i, bool decideOnly)
{
    const Ins& in = code_->code[i];
    if (in.opcode() != Op::Call || inlineBase_ < 0)
        return false;
    if (i < code_->prof.size() && (code_->prof[i] & opt::kProfFailed))
        return false;
    const unsigned base = in.b, argc = in.c;
    const auto& uw = plan_->useWebs[i];
    if (uw.size() < 2u + argc || uw[0] >= 0)
        return false;
    CodeBlock* C = nullptr;
    JSValue known = constantBefore(i, base);
    if (isFunction(known)) {
        auto* f = static_cast<JSFunction*>(asObject(known));
        if (f->flags & JSFunction::kNative)
            return false;
        C = f->code;
    } else {
        known = JSValue::undefined();
        if (i < code_->callProf.size() && code_->callProf[i])
            C = vm_.codeByUid(code_->callProf[i]);
    }
    if (!inlinable(C))
        return false;
    IVal callee = callerValue(i, base, 0, 8);
    if (callee.kind != IVal::Box && callee.kind != IVal::Const)
        return false;
    std::vector<IVal> args;
    args.push_back(callerValue(i, base + 1, 1, 8));
    for (unsigned k = 0; k < argc; ++k)
        args.push_back(callerValue(i, base + 2 + k, 2 + k, k < C->nparams ? C->prof[C->code.size() + k] : 8));
    return inlineTop(i, C, callee, known, args, in.a, decideOnly);
}

bool Baseline::inlineGetter(std::uint32_t i)
{
    // A read that has always found the same getter: the getter's body in place, the receiver its `this`.
    const Ins& in = code_->code[i];
    if (inlineBase_ < 0 || (i < code_->prof.size() && (code_->prof[i] & opt::kProfFailed)))
        return false;
    const auto& uw = plan_->useWebs[i];
    if (uw.empty() || uw[0] >= 0)
        return false;
    const std::uint16_t cacheId = code_->code[i + 1].a;
    if (cacheId == kNoCache)
        return false;
    const InlineCache& ic = code_->caches[cacheId];
    if (ic.shape != 0 || !ic.pad || ic.pad > code_->poly.size())
        return false;
    const PolyCache& pc = code_->poly[ic.pad - 1];
    if (pc.n != 1)
        return false;
    const PolyEntry& e = pc.e[0];
    if (e.kind != PolyEntry::OwnAccessor && !(e.kind == PolyEntry::ProtoAccessor && e.validity && e.validity->valid))
        return false;
    if (e.kind == PolyEntry::OwnAccessor)
        return false;  // the getter would have to be read from each receiver at compile time
    const JSValue slot = *e.holder->slotAt(e.index);
    if (!slot.isCell())
        return false;
    const auto* gs = reinterpret_cast<const GetterSetter*>(slot.asCell());
    if (!isFunction(gs->getter))
        return false;
    auto* f = static_cast<JSFunction*>(asObject(gs->getter));
    if ((f->flags & JSFunction::kNative) || !inlinable(f->code))
        return false;
    IVal callee;
    callee.kind = IVal::Const;
    callee.c = gs->getter;
    std::vector<IVal> args;
    IVal recv;
    recv.kind = IVal::Box;
    recv.frameReg = std::int32_t(in.b);
    recv.hint = 8;
    args.push_back(recv);
    for (int pass = 0; pass < 2; ++pass) {
        InlineCtx cx;
        cx.at = i;
        cx.emit = pass == 1;
        if (cx.emit) {
            // The receiver has the shape, the prototype chain is as it was, and the getter is the one compiled in.
            Label& bad = exitBefore(i);
            a_.mov(RAX, R(in.b));
            needObject(RAX, bad);
            a_.cmpI32(mem(RAX, 0), std::int32_t(e.shape));
            a_.jcc(CNE, bad);
            hold(e.validity);
            a_.movImm(RCX, reinterpret_cast<std::uint64_t>(e.validity));
            a_.cmpI32(mem(RCX, kValidityValid), 0);
            a_.jcc(CE, bad);
            a_.movImm(RAX, reinterpret_cast<std::uint64_t>(e.holder));
            slotAddress(e.index);
            a_.mov(RAX, mem(RDX, 0));
            holdValue(gs->getter);
            a_.movImm(RCX, gs->getter.bits);
            a_.cmp(RCX, mem(RAX, kGsGetter));
            a_.jcc(CNE, bad);
        }
        IVal result;
        const bool ok = inlineBody(cx, f->code, callee, gs->getter, args, result, 0);
#if defined(SCRIPT_JIT_ENV_OVERRIDE)
        if (!cx.emit && std::getenv("T2INL"))
            std::fprintf(stderr, "inline getter @%u of %s: %s\n", i, codeName(f->code).c_str(), ok ? "yes" : "no");
#endif
        if (!ok || !inlineDeliver(cx, result, in.a))
            return false;
    }
    return true;
}

bool Baseline::inlineTop(std::uint32_t i, CodeBlock* C, const IVal& callee, JSValue known, const std::vector<IVal>& args,
                         unsigned dst, bool decideOnly)
{
    // The first pass decides everything and emits nothing; the second follows it.
    for (int pass = 0; pass < (decideOnly ? 1 : 2); ++pass) {
        InlineCtx cx;
        cx.at = i;
        cx.emit = pass == 1;
        IVal result;
        const bool ok = inlineBody(cx, C, callee, known, args, result, 0) && inlineDeliver(cx, result, dst);
#if defined(SCRIPT_JIT_ENV_OVERRIDE)
        if (!cx.emit && std::getenv("T2INL"))
            std::fprintf(stderr, "inline @%u of %s: %s\n", i, codeName(C).c_str(), ok ? "yes" : "no");
#endif
        if (!ok)
            return false;
    }
    return true;
}

/// The result of an inlined body to the caller's register `dst` (its typed variable, or the frame).
bool Baseline::inlineDeliver(InlineCtx& cx, const IVal& v, unsigned dst)
{
    const bool emit = cx.emit;
    if (v.kind == IVal::Box && v.epoch != cx.epoch)
        return false;
    const std::int32_t dw = plan_->defWeb[cx.at];
    auto isNum = [](const IVal& x) {
        return x.kind == IVal::I32 || x.kind == IVal::F64 || (x.kind == IVal::Const && x.c.isNumber()) ||
               (x.kind == IVal::Box && x.hint && !(x.hint & ~3));
    };
    if (dw >= 0 && isI32(dw)) {
        if (v.kind == IVal::Const) {
            if (!v.c.isInt32())
                return false;
            if (emit)
                setConstI32(dw, v.c.asInt32());
        } else if (v.kind == IVal::I32) {
            if (emit) {
                inlineI32(v, RAX, nullptr);
                defI32(dw, RAX);
            }
        } else {
            Label* bad = cx.fail(this);
            if (!bad)
                return false;
            if (emit) {
                if (v.kind == IVal::Box) {
                    inlineI32(v, RAX, bad);
                } else {
                    // A double for an int32 variable: only if it is one.
                    Label ok;
                    inlineF64(v, 0, RAX, nullptr);
                    a_.cvttsd2si32(RAX, 0);
                    a_.cvtsi2sd32(1, RAX);
                    a_.ucomisd(0, 1);
                    a_.jcc(CNE, *bad);
                    a_.jcc(CP, *bad);
                    a_.test32(RAX, RAX);
                    a_.jcc(CNE, ok);
                    a_.movqFromX(RDX, 0);
                    a_.test(RDX, RDX);
                    a_.jcc(CS, *bad);
                    a_.bind(ok);
                }
                defI32(dw, RAX);
            }
        }
        return true;
    }
    if (dw >= 0) {
        if (!isNum(v))
            return false;
        Label* bad = v.kind == IVal::Box ? cx.fail(this) : &cx.dummy;
        if (!bad)
            return false;
        if (emit) {
            inlineF64(v, 0, RAX, bad);
            defF64(dw, 0);
        }
        return true;
    }
    if (emit) {
        inlineBoxed(v);
        if (v.kind == IVal::Box || (v.kind == IVal::Const && v.c.isCell()))
            retain(RAX);
        write(dst, RAX);
    }
    return true;
}

Label* Baseline::InlineCtx::fail(Baseline* b)
{
    if (sideEffect)
        return nullptr;
    return emit ? &b->exitBefore(at) : &dummy;
}

bool Baseline::inlineBody(InlineCtx& cx, CodeBlock* C, const IVal& callee, JSValue known, const std::vector<IVal>& args,
                          IVal& result, int depth)
{
    const bool emit = cx.emit;
    auto newSlot = [&]() -> std::int32_t { return cx.nextSlot < std::int32_t(kInlineSlots) ? cx.nextSlot++ : -1; };
    auto isInt = [](const IVal& v) {
        return v.kind == IVal::I32 || (v.kind == IVal::Const && v.c.isInt32()) || (v.kind == IVal::Box && v.hint == 1);
    };
    auto isNum = [](const IVal& v) {
        return v.kind == IVal::I32 || v.kind == IVal::F64 || (v.kind == IVal::Const && v.c.isNumber()) ||
               (v.kind == IVal::Box && v.hint && !(v.hint & ~3));
    };
    auto stale = [&](const IVal& v) { return v.kind == IVal::Box && v.epoch != cx.epoch; };
    // A value read from an object or a box (rax), into a new slot as its profile says. False when it cannot be.
    auto takeRead = [&](std::uint8_t hint, Label* bad, IVal& out) {
        hint &= 0x0F;
        if (!hint)
            return false;
        out = IVal{};
        out.slot = newSlot();
        if (out.slot < 0)
            return false;
        if (hint == 1) {
            if (!bad)
                return false;
            out.kind = IVal::I32;
            if (emit) {
                needInt(RAX, *bad);
                a_.mov32(inlineSlot(out.slot), RAX);
            }
        } else if (!(hint & ~3)) {
            if (!bad)
                return false;
            out.kind = IVal::F64;
            if (emit) {
                needNumber(RAX, *bad);
                loadNumber(0, RAX, RDX);
                a_.movsdStore(inlineSlot(out.slot), 0);
            }
        } else {
            out.kind = IVal::Box;
            out.hint = hint;
            out.epoch = cx.epoch;
            if (emit)
                a_.mov(inlineSlot(out.slot), RAX);
        }
        return true;
    };
    auto loadCallee = [&]() {
        if (emit)
            inlineBits(callee, RAX);
    };
    if (stale(callee))
        return false;

    // ---- the callee is the one compiled in, and `this` is what it takes ----
    const bool sloppyThis = !(C->flags & (CodeBlock::kStrict | CodeBlock::kArrow)) && readsThis(*C);
    {
        Label* bad = cx.fail(this);
        if (!bad)
            return false;
        if (sloppyThis && (args.empty() || args[0].kind != IVal::Box || stale(args[0])))
            return false;
        if (emit) {
            if (!known.isUndefined()) {
                if (callee.kind != IVal::Const) {
                    holdValue(known);
                    inlineBits(callee, RAX);
                    a_.movImm(RCX, known.bits);
                    a_.cmp(RCX, RAX);
                    a_.jcc(CNE, *bad);
                }
            } else {
                inlineBits(callee, RAX);
                needObject(RAX, *bad);
                a_.cmpI8(mem(RAX, kHdrCls), std::uint8_t(ObjClass::Function));
                a_.jcc(CNE, *bad);
                a_.mov(RCX, mem(RAX, kFnCode));
                a_.test(RCX, RCX);
                a_.jcc(CE, *bad);
                a_.movImm(RDX, C->uid);
                a_.cmp(RDX, mem(RCX, kCbUid));
                a_.jcc(CNE, *bad);
            }
            if (sloppyThis) {
                inlineBits(args[0], RAX);
                needObject(RAX, *bad);
            }
        }
    }

    std::vector<IVal> regs(C->nregs);
    for (std::size_t k = 0; k < args.size() && k < 1u + C->nparams && k < regs.size(); ++k)
        regs[k] = args[k];
    std::vector<std::int8_t> upKnown(C->upvals.size() + 1, -1);
    std::vector<IVal> upVal(C->upvals.size() + 1);
    for (std::uint32_t j = 0; j < C->code.size(); j = opt::nextOf(*C, j)) {
        const Ins& ci = C->code[j];
        const Op op = ci.opcode();
        const std::uint8_t p = C->prof[j];
        switch (op) {
        case Op::LoadInt:
            regs[ci.a] = IVal{};
            regs[ci.a].c = JSValue::int32(ci.imm());
            break;
        case Op::LoadK:
            regs[ci.a] = IVal{};
            regs[ci.a].c = C->consts[ci.b];
            break;
        case Op::LoadUndef: case Op::LoadNull: case Op::LoadTrue: case Op::LoadFalse:
            regs[ci.a] = IVal{};
            regs[ci.a].c = op == Op::LoadUndef ? JSValue::undefined() : op == Op::LoadNull ? JSValue::null()
                         : JSValue::boolean(op == Op::LoadTrue);
            break;
        case Op::Mov:
            regs[ci.a] = regs[ci.b];
            break;
        case Op::GetProp: {
            const IVal recv = regs[ci.b];
            if (recv.kind != IVal::Box || stale(recv))
                return false;
            const std::uint16_t cacheId = C->code[j + 1].a;
            if (cacheId == kNoCache)
                return false;
            const InlineCache& ic = C->caches[cacheId];
            struct Seen
            {
                std::uint32_t shape, index;
                JSObject* holder;
                ValidityCell* validity;
            };
            Seen seen[5];
            int count = 0;
            if (ic.shape != 0 && ic.next == 0 && (!ic.holder || ic.validity))
                seen[count++] = Seen{ic.shape, ic.index, ic.holder, ic.validity};
            if (ic.pad && ic.pad <= C->poly.size()) {
                const PolyCache& pc = C->poly[ic.pad - 1];
                for (std::uint8_t k = 0; k < pc.n; ++k) {
                    const PolyEntry& e = pc.e[k];
                    if (e.kind == PolyEntry::OwnData || (e.kind == PolyEntry::ProtoData && e.validity))
                        seen[count++] = Seen{e.shape, e.index, e.holder, e.validity};
                    else
                        return false;
                }
            }
            Label* bad = cx.fail(this);
            if (!count || !bad)
                return false;
            if (emit) {
                Label got;
                inlineBits(recv, RAX);
                needObject(RAX, *bad);
                for (int k = 0; k < count; ++k) {
                    Label next;
                    a_.cmpI32(mem(RAX, 0), std::int32_t(seen[k].shape));
                    a_.jcc(CNE, k + 1 < count ? next : *bad);
                    if (seen[k].holder) {
                        hold(seen[k].validity);
                        a_.movImm(RCX, reinterpret_cast<std::uint64_t>(seen[k].validity));
                        a_.cmpI32(mem(RCX, kValidityValid), 0);
                        a_.jcc(CE, *bad);
                        a_.movImm(RAX, reinterpret_cast<std::uint64_t>(seen[k].holder));
                    }
                    slotAddress(seen[k].index);
                    a_.jmp(got);
                    if (k + 1 < count)
                        a_.bind(next);
                }
                a_.bind(got);
                a_.mov(RAX, mem(RDX, 0));
            }
            IVal out;
            if (!takeRead(p, bad, out))
                return false;
            regs[ci.a] = out;
            break;
        }
        case Op::GetSuperProp: {
            // The slot the baseline code found, while this function's home object and its chain are as they were.
            if (j >= C->superCaches.size())
                return false;
            const SuperCache& sc = C->superCaches[j];
            Label* bad = cx.fail(this);
            if (!sc.home || !sc.validity || !sc.validity->valid || !bad)
                return false;
            if (emit) {
                loadCallee();
                a_.testI8(mem(RAX, kHdrFlags), cellflags::kHasRare);
                a_.jcc(CE, *bad);
                a_.mov(RAX, mem(RAX, kObjSlots));
                a_.mov(RAX, mem(RAX, -16));
                a_.movImm(RCX, reinterpret_cast<std::uint64_t>(sc.home));
                a_.cmp(RCX, mem(RAX, kRareHome));
                a_.jcc(CNE, *bad);
                hold(sc.validity);
                a_.movImm(RCX, reinterpret_cast<std::uint64_t>(sc.validity));
                a_.cmpI32(mem(RCX, kValidityValid), 0);
                a_.jcc(CE, *bad);
                a_.movImm(RCX, reinterpret_cast<std::uint64_t>(sc.slot));
                a_.mov(RAX, mem(RCX, 0));
            }
            IVal out;
            if (!takeRead(p ? p : 8, bad, out))
                return false;
            if (out.kind == IVal::Box && isFunction(*sc.slot))
                out.known = *sc.slot;
            regs[ci.a] = out;
            break;
        }
        case Op::Call: {
            // A call in the body: inlined in its turn, or the body is not.
            if (depth + 1 >= kInlineDepth)
                return false;
            const IVal fv = regs[ci.b];
            CodeBlock* D = nullptr;
            JSValue dk = JSValue::undefined();
            if (fv.kind == IVal::Const && isFunction(fv.c)) {
                dk = fv.c;
            } else if (fv.kind == IVal::Box && isFunction(fv.known)) {
                dk = fv.known;
            }
            if (isFunction(dk)) {
                auto* g = static_cast<JSFunction*>(asObject(dk));
                if (g->flags & JSFunction::kNative)
                    return false;
                D = g->code;
            } else if (fv.kind == IVal::Box && j < C->callProf.size() && C->callProf[j]) {
                D = vm_.codeByUid(C->callProf[j]);
            }
            if (!inlinable(D) || D == C)
                return false;
            std::vector<IVal> inner;
            for (unsigned k = 0; k < 1u + ci.c; ++k)
                inner.push_back(regs[ci.b + 1 + k]);
            for (unsigned k = 0; k < ci.c && k < D->nparams; ++k)
                if (inner[1 + k].kind == IVal::Box && !inner[1 + k].hint)
                    inner[1 + k].hint = D->prof[D->code.size() + k];
            IVal out;
            if (!inlineBody(cx, D, fv, dk, inner, out, depth + 1))
                return false;
            regs[ci.a] = out;
            break;
        }
        case Op::GetUpval: {
            if (ci.b < upKnown.size() && upKnown[ci.b] >= 0) {
                regs[ci.a] = upVal[ci.b];
                break;
            }
            Label* bad = cx.fail(this);
            if (emit) {
                loadCallee();
                a_.mov(RAX, mem(RAX, kFnUpvals + std::int32_t(8 * ci.b)));
                a_.mov(RAX, mem(RAX, kBoxValue));
            }
            IVal out;
            if (!takeRead(p, bad, out))
                return false;
            regs[ci.a] = out;
            break;
        }
        case Op::PutUpval: {
            if (ci.a >= upKnown.size())
                return false;
            const IVal v = regs[ci.b];
            if (stale(v))
                return false;
            if (emit) {
                inlineBoxed(v);
                a_.mov(RCX, RAX);
                if (v.kind == IVal::Box || (v.kind == IVal::Const && v.c.isCell()))
                    retain(RCX);
                loadCallee();
                a_.mov(RAX, mem(RAX, kFnUpvals + std::int32_t(8 * ci.a)));
                a_.mov(RSI, mem(RAX, kBoxValue));
                a_.mov(mem(RAX, kBoxValue), RCX);
                release(RSI);
            }
            cx.sideEffect = true;
            ++cx.epoch;
            upKnown[ci.a] = 1;
            upVal[ci.a] = v;
            if (v.kind == IVal::Box)
                upVal[ci.a].epoch = cx.epoch;
            break;
        }
        case Op::Add: case Op::Sub: case Op::Mul: case Op::Div: case Op::BitAnd: case Op::BitOr: case Op::BitXor:
        case Op::Shl: case Op::Shr: case Op::AddI: case Op::SubI: case Op::MulI: case Op::BitAndI: case Op::BitOrI:
        case Op::BitXorI: case Op::ShlI: case Op::ShrI: case Op::UShrI: case Op::Inc: case Op::Dec: {
            const bool imm = op == Op::AddI || op == Op::SubI || op == Op::MulI || op == Op::BitAndI || op == Op::BitOrI ||
                             op == Op::BitXorI || op == Op::ShlI || op == Op::ShrI || op == Op::UShrI;
            const bool unary = op == Op::Inc || op == Op::Dec;
            const bool bitwise = op == Op::BitAnd || op == Op::BitOr || op == Op::BitXor || op == Op::Shl ||
                                 op == Op::Shr || op == Op::BitAndI || op == Op::BitOrI || op == Op::BitXorI ||
                                 op == Op::ShlI || op == Op::ShrI || op == Op::UShrI;
            const std::int32_t k = imm ? std::int32_t(std::int16_t(ci.c)) : 0;
            const IVal A = regs[ci.b];
            IVal B;
            if (imm || unary)
                B.c = JSValue::int32(unary ? 1 : k);
            else
                B = regs[ci.c];
            if (stale(A) || stale(B))
                return false;
            if (op == Op::UShrI && (k & 31) == 0)
                return false;
            IVal out;
            out.slot = newSlot();
            if (out.slot < 0)
                return false;
            const bool ints = isInt(A) && isInt(B);
            const bool boxed = A.kind == IVal::Box || B.kind == IVal::Box;
            if (bitwise) {
                if (!ints)
                    return false;
                Label* bad = boxed ? cx.fail(this) : &cx.dummy;
                if (!bad)
                    return false;
                if (emit) {
                    inlineI32(A, RAX, bad);
                    if (!imm)
                        inlineI32(B, RCX, bad);
                    switch (op) {
                    case Op::BitAnd: a_.alu32(4, RAX, RCX); break;
                    case Op::BitOr: a_.alu32(1, RAX, RCX); break;
                    case Op::BitXor: a_.alu32(6, RAX, RCX); break;
                    case Op::Shl: a_.shiftCl32(4, RAX); break;
                    case Op::Shr: a_.shiftCl32(7, RAX); break;
                    case Op::BitAndI: a_.aluI32(4, RAX, k); break;
                    case Op::BitOrI: a_.aluI32(1, RAX, k); break;
                    case Op::BitXorI: a_.aluI32(6, RAX, k); break;
                    case Op::ShlI: a_.shiftI32(4, RAX, k & 31); break;
                    case Op::ShrI: a_.shiftI32(7, RAX, k & 31); break;
                    case Op::UShrI: a_.shiftI32(5, RAX, k & 31); break;
                    default: break;
                    }
                    a_.mov32(inlineSlot(out.slot), RAX);
                }
                out.kind = IVal::I32;
                regs[ci.a] = out;
                break;
            }
            if (!isNum(A) || !isNum(B))
                return false;
            if (ints && op != Op::Div && !(p & 2)) {
                // Overflow, or a product that is a negative zero, leaves the int32s.
                Label* bad = cx.fail(this);
                if (!bad)
                    return false;
                if (emit) {
                    inlineI32(A, RAX, bad);
                    if (!imm && !unary)
                        inlineI32(B, RCX, bad);
                    switch (op) {
                    case Op::Add: a_.alu32(0, RAX, RCX); break;
                    case Op::Sub: a_.alu32(5, RAX, RCX); break;
                    case Op::AddI: a_.aluI32(0, RAX, k); break;
                    case Op::SubI: a_.aluI32(5, RAX, k); break;
                    case Op::Inc: a_.aluI32(0, RAX, 1); break;
                    case Op::Dec: a_.aluI32(0, RAX, -1); break;
                    case Op::Mul: case Op::MulI: {
                        if (op == Op::MulI)
                            a_.movImm(RCX, std::uint32_t(k));
                        a_.mov32(RSI, RAX);
                        a_.alu32(1, RSI, RCX);
                        a_.imul32(RAX, RCX);
                        a_.jcc(CO, *bad);
                        Label nonzero;
                        a_.test32(RAX, RAX);
                        a_.jcc(CNE, nonzero);
                        a_.test32(RSI, RSI);
                        a_.jcc(CS, *bad);
                        a_.bind(nonzero);
                        break;
                    }
                    default: break;
                    }
                    if (op != Op::Mul && op != Op::MulI)
                        a_.jcc(CO, *bad);
                    a_.mov32(inlineSlot(out.slot), RAX);
                }
                out.kind = IVal::I32;
                regs[ci.a] = out;
                break;
            }
            Label* bad = boxed ? cx.fail(this) : &cx.dummy;
            if (!bad)
                return false;
            if (emit) {
                inlineF64(A, 0, RAX, bad);
                inlineF64(B, 1, RCX, bad);
                switch (op) {
                case Op::Add: case Op::AddI: case Op::Inc: a_.addsd(0, 1); break;
                case Op::Sub: case Op::SubI: case Op::Dec: a_.subsd(0, 1); break;
                case Op::Mul: case Op::MulI: a_.mulsd(0, 1); break;
                case Op::Div: a_.divsd(0, 1); break;
                default: break;
                }
                a_.movsdStore(inlineSlot(out.slot), 0);
            }
            out.kind = IVal::F64;
            regs[ci.a] = out;
            break;
        }
        case Op::Ret:
            result = regs[ci.a];
            return !stale(result);
        default:
            return false;
        }
    }
    return false;
}

// ---- direct calls ----
//
// `f(x)` where `f` is a global function: the `GetGlobal` that reads it and the `LoadUndef` of `this` (when the callee never
// reads it) write nothing; the call checks that the global still holds the function and pushes its frame from constants.
// A slow path, or an exit between those instructions and the call, writes the registers first.

void Baseline::planDirectCalls()
{
    directCalls_.clear();
    pendingAt_.clear();
    skipDef_.assign(n_ + 1, 0);
    exitRedirect_.assign(n_ + 1, -1);
    if (!code_->handlers.empty())
        return;
    std::vector<std::uint32_t> starts;
    for (std::uint32_t j = 0; j < n_; j += length(j))
        starts.push_back(j);
    auto reads = [&](std::uint32_t j, unsigned r) {
        for (std::uint16_t x : opt::effectsOf(*code_, j).rd)
            if (x == r)
                return true;
        return false;
    };
    auto writes = [&](std::uint32_t j, unsigned r) {
        for (std::uint16_t x : opt::effectsOf(*code_, j).wr)
            if (x == r)
                return true;
        return false;
    };
    // Instructions that may sit between the read of the global and the call: nothing that runs script or writes globals.
    auto quiet = [&](std::uint32_t j) {
        const Op op = code_->code[j].opcode();
        switch (op) {
        case Op::LoadUndef: case Op::LoadNull: case Op::LoadInt: case Op::LoadK: case Op::LoadTrue: case Op::LoadFalse:
        case Op::Mov:
            return true;
        case Op::Add: case Op::Sub: case Op::Mul: case Op::BitAnd: case Op::BitOr: case Op::BitXor: case Op::Shl:
        case Op::Shr: case Op::AddI: case Op::SubI: case Op::MulI: case Op::BitAndI: case Op::BitOrI: case Op::BitXorI:
        case Op::ShlI: case Op::ShrI: case Op::Inc: case Op::Dec: {
            if (plan_->defWeb[j] < 0)
                return false;
            for (std::int32_t w : plan_->useWebs[j])
                if (w < 0)
                    return false;
            return true;
        }
        default:
            return false;
        }
    };
    auto deadAfter = [&](std::size_t k, unsigned r) { return regDeadAfter(starts[k], r); };
    for (std::size_t k = 0; k < starts.size(); ++k) {
        const std::uint32_t i = starts[k];
        const Ins& in = code_->code[i];
        if (in.opcode() != Op::Call || (i < code_->prof.size() && (code_->prof[i] & opt::kProfFailed)))
            continue;
        const unsigned base = in.b, argc = in.c;
        const JSValue f = constantBefore(i, base);
        if (!isFunction(f))
            continue;
        auto* fn = static_cast<JSFunction*>(asObject(f));
        if (fn->flags & JSFunction::kNative)
            continue;
        CodeBlock* C = fn->code;
        if (!C || argc > C->nparams ||
            (C->flags & (CodeBlock::kGenerator | CodeBlock::kAsync | CodeBlock::kAsyncGen | CodeBlock::kNeedsArgs |
                         CodeBlock::kClassConstructor)))
            continue;
        if (inlineCall(i, true))
            continue;
        // The read of the callee, in this block, with only quiet instructions after it.
        std::int64_t d = -1, t = -1;
        bool ok = true;
        for (std::size_t m = k; m-- > 0;) {
            const std::uint32_t j = starts[m];
            if (writes(j, base)) {
                if (code_->code[j].opcode() == Op::GetGlobal && plan_->defWeb[j] < 0)
                    d = j;
                break;
            }
            if (reads(j, base) || !quiet(j) || plan_->isLeader[j]) {
                ok = false;
                break;
            }
            if (t < 0 && writes(j, base + 1)) {
                const Op op = code_->code[j].opcode();
                if (op == Op::LoadUndef || op == Op::LoadNull)
                    t = j;
            }
        }
        if (!ok || d < 0 || !deadAfter(k, base))
            continue;
        // The global's slot.
        JSObject* g = vm_.global;
        std::uint8_t attrs = 0;
        JSValue* slot = g->findOwn(vm_, code_->atoms[code_->code[std::size_t(d)].b], &attrs);
        if (!slot || (attrs & attr::kAccessor) || *slot != f)
            continue;
        JSValue* inl = g->inl();
        const std::uint32_t index = slot >= inl && slot < inl + g->h.aux ? std::uint32_t(slot - inl)
                                                                          : std::uint32_t(slot - g->slots) + g->h.aux;
        bool thisSkipped = false;
        if (t >= 0 && (C->flags & CodeBlock::kNoThis) && deadAfter(k, base + 1)) {
            // Nothing else between the `this` and the call reads it.
            bool readBetween = false;
            for (std::size_t m = 0; m < k; ++m)
                if (starts[m] > std::uint32_t(t) && reads(starts[m], base + 1))
                    readBetween = true;
            thisSkipped = !readBetween;
        }
        holdValue(f);
        directCalls_[i] = DirectCall{f, C, index, thisSkipped, std::uint32_t(d),
                                     t >= 0 && code_->code[std::size_t(t)].opcode() == Op::LoadNull};
        skipDef_[std::size_t(d)] = 1;
        if (thisSkipped)
            skipDef_[std::size_t(t)] = 1;
        // An exit between the read of the global and the call runs again from the read: the global may have changed
        // since this code was made, and what lies between has no effects.
        for (std::size_t m = 0; m < k; ++m)
            if (starts[m] > std::uint32_t(d))
                exitRedirect_[starts[m]] = std::int32_t(d);
    }
}

void Baseline::writePending(std::uint32_t point)
{
    auto it = pendingAt_.find(point);
    if (it == pendingAt_.end())
        return;
    for (const auto& [reg, v] : it->second) {
        a_.movImm(RAX, v.bits);
        if (v.isCell())
            a_.inc32(mem(RAX, kHdrRc));
        write(reg, RAX);
    }
}

void Baseline::emitDirectCall(std::uint32_t i)
{
    const Ins& in = code_->code[i];
    const DirectCall& dc = directCalls_.at(i);
    CodeBlock* C = dc.code;
    const unsigned base = in.b, argc = in.c;
    materializeReads(i);
    savePool(i);
    Label slowPath, done;
    throws_.emplace_back(i, Label{});
    Label& exc = throws_.back().second;
    // The global still holds the function.
    a_.cmpI32(mem(R12, kVmGlobalEpoch), std::int32_t(vm_.globalEpoch));
    a_.jcc(CNE, slowPath);
    globalSlot(dc.globalIndex);
    a_.movImm(RAX, dc.fn.bits);
    a_.cmp(RAX, mem(RDX, 0));
    a_.jcc(CNE, slowPath);
    // Its baseline code (once made, it stays while the block lives), room for its frame (stack, initialised stack, a
    // frame, native stack) and a tick.
    a_.movImm(RCX, reinterpret_cast<std::uint64_t>(C));
    a_.mov(RDX, mem(RCX, kCbTier1));
    if (!C->tier1) {
        a_.test(RDX, RDX);
        a_.jcc(CE, slowPath);
    }
    a_.lea(R9, R(base + 1));
    a_.lea(R10, mem(R9, std::int32_t(8 * C->nregs + 64)));
    a_.alu(7, R10, mem(R12, kVmStackEnd));
    a_.jcc(CA, slowPath);
    a_.lea(R10, mem(R9, std::int32_t(8 * C->nregs)));
    a_.alu(7, R10, mem(R12, kVmStackInit));
    a_.jcc(CA, slowPath);
    // The new frame follows this one (the top one): its index from the address, so nothing waits on the count's last
    // store.
    a_.lea(R11, mem(R13, std::int32_t(sizeof(Frame))));
    a_.mov(R10, R11);
    a_.alu(5, R10, mem(R12, kVmFrameBase));
    a_.shiftI(5, R10, 6);
    a_.lea(RDI, mem(R10, 2));
    a_.alu32(7, RDI, mem(R12, kVmFrameCapacity));
    a_.jcc(CAE, slowPath);
    a_.alu(7, RSP, mem(R12, kVmStackFloor));
    a_.jcc(CB, slowPath);
    a_.cmpI32(mem(R12, kVmTicks), 1);
    a_.jcc(CBE, slowPath);
    a_.dec32(mem(R12, kVmTicks));
    // Caller registers the callee's frame covers past the arguments: plain values are overwritten, a cell goes the long way.
    const unsigned window = code_->nregs > base + 1 ? code_->nregs - (base + 1) : 0;
    for (unsigned r = 1 + argc; r < window && r < C->nregs; ++r) {
        const unsigned reg = base + 1 + r;
        if (!(reg < plainRegs_.size() && plainRegs_[reg])) {
            a_.mov(RDI, mem(R9, std::int32_t(8 * r)));
            a_.test(RDI, R15);
            a_.jcc(CE, slowPath);
        }
        a_.mov(mem(R9, std::int32_t(8 * r)), std::int32_t(kUndefined));
    }
    if (!dc.thisSkipped && !(C->flags & (CodeBlock::kStrict | CodeBlock::kArrow | CodeBlock::kNoThis))) {
        Label thisOk;
        a_.mov(RDI, R(base + 1));
        a_.aluI(4, RDI, -9);
        a_.cmpI(RDI, std::int32_t(kNull));
        a_.jcc(CNE, thisOk);
        a_.movImm(RDI, reinterpret_cast<std::uint64_t>(vm_.global));
        a_.inc32(mem(RDI, kHdrRc));
        a_.mov(R(base + 1), RDI);
        a_.bind(thisOk);
    }
    a_.movImm(RDI, reinterpret_cast<std::uint64_t>(&code_->code[i + 1]));
    a_.mov(mem(R13, kFramePc), RDI);
    static_assert(sizeof(Frame) == 64, "a frame is found by a shift");
    a_.lea(RDI, mem(R10, 1));
    a_.mov32(mem(R12, kVmFrameCount), RDI);
    a_.mov(mem(R11, kFrameCode), RCX);
    a_.mov(mem(R11, kFrameFn), RAX);
    a_.mov(mem(R11, kFramePc), 0);
    a_.mov(mem(R11, kFrameR), R9);
    a_.movImm(RDI, (std::uint64_t(argc) << 32) | in.a);
    a_.mov(mem(R11, kFrameRetDst), RDI);
    a_.mov(mem(R11, kFrameEntry - 1), std::int32_t(0x100));
    a_.mov(mem(R11, kFrameNewTarget), std::int32_t(kUndefined));
    a_.mov(mem(R11, kFrameGen), std::int32_t(kUndefined));
    a_.mov(RSI, R11);
    a_.mov(RDX, mem(RDX, kTierEntry));
    a_.callReg(RDX);
    a_.test(RAX, RAX);
    a_.jcc(CE, exc);
    write(in.a, RAX);
    restorePool(i);
    a_.jmp(done);
    // The long way: the registers written as the instructions would have (the global read now), then the baseline call
    // sequence.
    a_.bind(slowPath);
    restorePool(i);
    savePool(i);
    a_.mov(RDI, R12);
    a_.movImm(RSI, dc.read);
    a_.movImm(RAX, reinterpret_cast<std::uint64_t>(&helperThunk<jitGetGlobal>));
    a_.callReg(RAX);
    a_.aluI32(7, RAX, 1);
    a_.jcc(CNE, exception(i));
    restorePool(i);
    if (dc.thisSkipped) {
        a_.movImm(RAX, dc.thisNull ? kNull : kUndefined);
        write(base + 1, RAX);
    }
    call(i, in);
    if (auto wc = windowCleanup_.find(i); wc != windowCleanup_.end()) {
        // What another callee may have left in its parameters, which this code takes for plain values.
        for (unsigned r : wc->second) {
            a_.mov(RDX, R(r));
            a_.mov(R(r), std::int32_t(kUndefined));
            release(RDX);
        }
    }
    a_.bind(done);
    if (plan_->defWeb[i] >= 0)
        loadTypedAfter(i);
}

// ---- read chains ----
//
// `os[i & 3].x`, `a.b.c`: reads whose intermediate objects are used once, by the next read, and then dead. The objects pass
// through a slot of the optimized frame, never written to their registers nor counted; every read in the chain is free of
// effects, so a failed guard anywhere in it runs the whole chain again in the interpreter from its start.

bool Baseline::chainable(std::uint32_t i, bool asConsumer)
{
    const Ins& in = code_->code[i];
    if (i < code_->prof.size() && (code_->prof[i] & opt::kProfFailed))
        return false;
    switch (in.opcode()) {
    case Op::GetGlobal: {
        if (asConsumer)
            return false;
        std::uint8_t attrs = 0;
        JSValue* slot = vm_.global->findOwn(vm_, code_->atoms[in.b], &attrs);
        return slot && !(attrs & attr::kAccessor);
    }
    case Op::GetElem: {
        const auto& uw = plan_->useWebs[i];
        if (uw.size() < 2 || (asConsumer ? false : uw[0] >= 0))
            return false;
        if (uw[1] >= 0 && !isI32(uw[1]))
            return false;
        const std::uint8_t ep = i < code_->elemProf.size() ? code_->elemProf[i] : 0;
        return ep == elemprof::kArray && in.b != in.c;
    }
    case Op::GetProp: {
        const std::uint16_t cacheId = code_->code[i + 1].a;
        if (cacheId == kNoCache)
            return false;
        const InlineCache& ic = code_->caches[cacheId];
        if (ic.shape == 0 || ic.next != 0 || (ic.holder && !ic.validity))
            return false;
        if (ic.pad && ic.pad <= code_->poly.size()) {
            const PolyCache& pc = code_->poly[ic.pad - 1];
            for (std::uint8_t k = 0; k < pc.n; ++k)
                if (pc.e[k].kind != PolyEntry::OwnData && !(pc.e[k].kind == PolyEntry::ProtoData && pc.e[k].validity))
                    return false;
        }
        return true;
    }
    default:
        return false;
    }
}

void Baseline::planChains()
{
    chainRole_.assign(n_ + 1, 0);
    chainHead_.assign(n_ + 1, -1);
    if (!code_->handlers.empty())
        return;
    std::vector<std::uint32_t> starts;
    for (std::uint32_t j = 0; j < n_; j += length(j))
        starts.push_back(j);
    auto reads = [&](std::uint32_t j, unsigned r) {
        for (std::uint16_t x : opt::effectsOf(*code_, j).rd)
            if (x == r)
                return true;
        return false;
    };
    auto writes = [&](std::uint32_t j, unsigned r) {
        for (std::uint16_t x : opt::effectsOf(*code_, j).wr)
            if (x == r)
                return true;
        return false;
    };
    auto deadAfter = [&](std::size_t k, unsigned r) { return regDeadAfter(starts[k], r); };
    // Between two reads of a chain: typed arithmetic whose result nothing at the chain's start holds.
    auto quiet = [&](std::uint32_t j, std::uint32_t head) {
        const Op op = code_->code[j].opcode();
        if (plan_->isLeader[j] || opt::jumpTarget(*code_, j) >= 0)
            return false;
        if (op != Op::LoadInt) {
            if (plan_->defWeb[j] < 0)
                return false;
            for (std::int32_t w : plan_->useWebs[j])
                if (w < 0)
                    return false;
        }
        const opt::Effects e = opt::effectsOf(*code_, j);
        if (e.wr.size() != 1)
            return false;
        for (const opt::LiveWeb& lw : plan_->liveBefore[head])
            if (lw.reg == e.wr[0])
                return false;
        return true;
    };
    std::vector<std::int32_t> headOf(n_ + 1, -1);
    for (std::size_t k = 0; k < starts.size(); ++k) {
        const std::uint32_t i = starts[k];
        if (plan_->defWeb[i] >= 0 || !chainable(i, false))
            continue;
        const Ins& in = code_->code[i];
        const unsigned r = in.a;
        const std::uint32_t head = headOf[i] >= 0 ? std::uint32_t(headOf[i]) : i;
        // The one reader of `r`: the next read of the chain.
        std::size_t m = k + 1;
        for (; m < starts.size(); ++m) {
            const std::uint32_t j = starts[m];
            if (reads(j, r) || writes(j, r))
                break;
            if (!quiet(j, head))
                break;
        }
        if (m >= starts.size())
            continue;
        const std::uint32_t c = starts[m];
        const Ins& cn = code_->code[c];
        const Op cop = cn.opcode();
        if ((cop != Op::GetProp && cop != Op::GetElem) || cn.b != r || plan_->isLeader[c] || !chainable(c, true))
            continue;
        if (cop == Op::GetElem && cn.c == r)
            continue;
        if (!(cn.a == r || deadAfter(m, r)))
            continue;
        chainRole_[i] |= 1;
        chainRole_[c] |= 2;
        headOf[c] = std::int32_t(head);
        for (std::size_t q = 0; q < starts.size(); ++q)
            if (starts[q] >= head && starts[q] <= c)
                chainHead_[starts[q]] = std::int32_t(head);
    }
#if defined(SCRIPT_JIT_ENV_OVERRIDE)
    if (std::getenv("T2CHAIN"))
        for (std::uint32_t j = 0; j < n_; ++j)
            if (chainRole_[j] || chainHead_[j] >= 0)
                std::fprintf(stderr, "chain %s @%u role %d head %d\n", codeName(code_).c_str(), j, chainRole_[j], chainHead_[j]);
#endif
    // A chain is only worth it when it ends in a read: a producer whose consumer was not taken writes normally.
    for (std::uint32_t j = 0; j < n_; ++j)
        if ((chainRole_[j] & 1) && !std::any_of(chainRole_.begin() + j + 1, chainRole_.end(), [](std::uint8_t x) { return x & 2; }))
            chainRole_[j] &= std::uint8_t(~1);
}

void Baseline::emitChain(std::uint32_t i)
{
    const Ins& in = code_->code[i];
    const std::uint8_t role = chainRole_[i];
    Label& bad = exitBefore(std::uint32_t(chainHead_[i] >= 0 ? chainHead_[i] : std::int32_t(i)));
    auto receiver = [&](unsigned reg) {
        if (role & 2)
            a_.mov(RAX, mem(RSP, chainSlot_));
        else
            a_.mov(RAX, R(reg));
    };
    switch (in.opcode()) {
    case Op::GetGlobal: {
        JSObject* g = vm_.global;
        std::uint8_t attrs = 0;
        JSValue* slot = g->findOwn(vm_, code_->atoms[in.b], &attrs);
        JSValue* inl = g->inl();
        const std::uint32_t index = slot >= inl && slot < inl + g->h.aux ? std::uint32_t(slot - inl)
                                                                          : std::uint32_t(slot - g->slots) + g->h.aux;
        a_.cmpI32(mem(R12, kVmGlobalEpoch), std::int32_t(vm_.globalEpoch));
        a_.jcc(CNE, bad);
        globalSlot(index);
        a_.mov(RAX, mem(RDX, 0));
        break;
    }
    case Op::GetElem: {
        const auto& uw = plan_->useWebs[i];
        receiver(in.b);
        needObject(RAX, bad);
        a_.cmpI8(mem(RAX, kHdrCls), std::uint8_t(ObjClass::Array));
        a_.jcc(CNE, bad);
        if (uw[1] >= 0) {
            a_.mov32(RCX, useI32(uw[1], RCX));
        } else {
            a_.mov(RCX, R(in.c));
            needInt(RCX, bad);
            a_.mov32(RCX, RCX);
        }
        a_.test32(RCX, RCX);
        a_.jcc(CS, bad);
        a_.alu32(7, RCX, mem(RAX, kArrayLength));
        a_.jcc(CAE, bad);
        a_.mov(RDX, mem(RAX, kArrayElems));
        a_.mov(RAX, mem(RDX, RCX, 8));
        a_.cmpI(RAX, std::int32_t(kHole));
        a_.jcc(CE, bad);
        break;
    }
    case Op::GetProp: {
        const InlineCache& ic = code_->caches[code_->code[i + 1].a];
        struct Seen
        {
            std::uint32_t shape, index;
            JSObject* holder;
            ValidityCell* validity;
        };
        Seen seen[5];
        int count = 0;
        seen[count++] = Seen{ic.shape, ic.index, ic.holder, ic.validity};
        if (ic.pad && ic.pad <= code_->poly.size()) {
            const PolyCache& pc = code_->poly[ic.pad - 1];
            for (std::uint8_t k = 0; k < pc.n; ++k)
                seen[count++] = Seen{pc.e[k].shape, pc.e[k].index, pc.e[k].holder, pc.e[k].validity};
        }
        receiver(in.b);
        needObject(RAX, bad);
        Label got;
        for (int k = 0; k < count; ++k) {
            Label next;
            a_.cmpI32(mem(RAX, 0), std::int32_t(seen[k].shape));
            a_.jcc(CNE, k + 1 < count ? next : bad);
            if (seen[k].holder) {
                hold(seen[k].validity);
                a_.movImm(RCX, reinterpret_cast<std::uint64_t>(seen[k].validity));
                a_.cmpI32(mem(RCX, kValidityValid), 0);
                a_.jcc(CE, bad);
                a_.movImm(RAX, reinterpret_cast<std::uint64_t>(seen[k].holder));
            }
            slotAddress(seen[k].index);
            a_.jmp(got);
            if (k + 1 < count)
                a_.bind(next);
        }
        a_.bind(got);
        a_.mov(RAX, mem(RDX, 0));
        break;
    }
    default:
        break;
    }
    if (role & 1) {
        a_.mov(mem(RSP, chainSlot_), RAX);
        return;
    }
    const std::int32_t dw = plan_->defWeb[i];
    if (dw >= 0) {
        unboxRax(dw, bad);
    } else {
        retain(RAX);
        write(in.a, RAX);
    }
}

/// Whether register `r` is never read after instruction `i` before something writes it again, on every path.
bool Baseline::regDeadAfter(std::uint32_t i, unsigned r)
{
    std::vector<std::uint8_t> seen(n_ + 1, 0);
    std::vector<std::uint32_t> work;
    auto successors = [&](std::uint32_t j) {
        const Op op = code_->code[j].opcode();
        const std::uint32_t nx = j + length(j);
        const std::int64_t t = opt::jumpTarget(*code_, j);
        if (t >= 0) {
            work.push_back(std::uint32_t(t));
            if (op != Op::Jmp)
                work.push_back(nx);
        } else if (op != Op::Ret && op != Op::Throw && op != Op::ThrowError) {
            work.push_back(nx);
        }
    };
    successors(i);
    while (!work.empty()) {
        const std::uint32_t j = work.back();
        work.pop_back();
        if (j >= n_)
            return false;
        if (seen[j])
            continue;
        seen[j] = 1;
        const opt::Effects e = opt::effectsOf(*code_, j);
        if (!e.ok)
            return false;
        bool read = false, written = false;
        for (std::uint16_t x : e.rd)
            read = read || x == r;
        for (std::uint16_t x : e.wr)
            written = written || x == r;
        if (read)
            return false;
        if (!written)
            successors(j);
    }
    return true;
}

// ---- constructors ----
//
// `new T(a, b)` where T only stores its arguments (and constants) into new properties of `this`: the object is made here
// with its final shape and the values written into its slots. Every check comes before the object is made; anything that
// does not hold runs the baseline sequence instead.

bool Baseline::inlineNew(std::uint32_t i)
{
    const Ins& in = code_->code[i];
    const unsigned base = in.b, argc = in.c;
    const auto& uw = plan_->useWebs[i];
    if (inlineBase_ < 0 || uw.size() < 2u + argc || uw[0] >= 0)
        return false;
    const JSValue known = constantBefore(i, base);
    if (!isFunction(known))
        return false;
    auto* f = static_cast<JSFunction*>(asObject(known));
    if ((f->flags & JSFunction::kNative) || f->h.shape != vm_.fnProtoShape || f->isDictionary(vm_))
        return false;
    CodeBlock* C = f->code;
    if (!C || !C->constructor || (C->flags & (CodeBlock::kGenerator | CodeBlock::kAsync | CodeBlock::kAsyncGen |
                                              CodeBlock::kNeedsArgs | CodeBlock::kClassConstructor | CodeBlock::kDerived)))
        return false;
    const JSValue pv = *f->slotAt(2);
    if (!isObject(pv))
        return false;
    JSObject* proto = asObject(pv);
    if (!regDeadAfter(i, base + 1))
        return false;
    // The body: stores of arguments or constants into new properties of `this`, then `return undefined`.
    struct Store
    {
        std::uint32_t index;
        IVal value;
    };
    std::vector<Store> stores;
    std::vector<ValidityCell*> cells;
    const std::uint8_t aux = thisSlotsOf(C);
    std::uint32_t shape = vm_.rootShapeFor(proto)->id;
    std::vector<std::int8_t> constReg(C->nregs, 0);
    std::vector<JSValue> constVal(C->nregs, JSValue::undefined());
    bool returned = false;
    for (std::uint32_t j = 0; j < C->code.size() && !returned; j = opt::nextOf(*C, j)) {
        const Ins& ci = C->code[j];
        switch (ci.opcode()) {
        case Op::PutProp: {
            if (ci.a != 0 || ci.c == 0 || ci.c >= C->nregs)
                return false;
            const std::uint16_t cacheId = C->code[j + 1].a;
            if (cacheId == kNoCache)
                return false;
            const InlineCache& ic = C->caches[cacheId];
            if (ic.shape != shape || ic.next == 0 || ic.index >= aux)
                return false;
            if (ic.validity) {
                if (!ic.validity->valid)
                    return false;
                cells.push_back(ic.validity);
            }
            // The value as the register holds it at this store: an argument (undefined past them) or a constant.
            IVal v;
            if (constReg[ci.c]) {
                v.c = constVal[ci.c];
            } else if (ci.c <= C->nparams) {
                const unsigned k = ci.c - 1;
                if (k < argc)
                    v = callerValue(i, base + 2 + k, 2 + k, 8);
            } else {
                return false;
            }
            stores.push_back(Store{ic.index, v});
            shape = ic.next;
            break;
        }
        case Op::LoadUndef:
            if (ci.a == 0 || ci.a <= C->nparams)
                return false;
            constReg[ci.a] = 1;
            constVal[ci.a] = JSValue::undefined();
            break;
        case Op::LoadInt:
            if (ci.a == 0 || ci.a <= C->nparams)
                return false;
            constReg[ci.a] = 1;
            constVal[ci.a] = JSValue::int32(ci.imm());
            break;
        case Op::Ret:
            if (!constReg[ci.a] || !constVal[ci.a].isUndefined())
                return false;
            returned = true;
            break;
        default:
            return false;
        }
    }
    if (!returned)
        return false;
    if (!canAllocObject(aux))
        return false;
    Label slowPath, done;
    // The callee, its prototype and the chain the stores were learned on are as they were.
    holdValue(known);
    a_.movImm(RCX, known.bits);
    a_.cmp(RCX, R(base));
    a_.jcc(CNE, slowPath);
    a_.mov(RAX, R(base));
    a_.cmpI32(mem(RAX, 0), std::int32_t(f->h.shape));
    a_.jcc(CNE, slowPath);
    a_.mov(RDX, mem(RAX, kObjSlots));
    holdValue(pv);
    a_.movImm(RCX, pv.bits);
    a_.cmp(RCX, mem(RDX, 16));
    a_.jcc(CNE, slowPath);
    for (ValidityCell* c : cells) {
        hold(c);
        a_.movImm(RCX, reinterpret_cast<std::uint64_t>(c));
        a_.cmpI32(mem(RCX, kValidityValid), 0);
        a_.jcc(CE, slowPath);
    }
    allocObject(shape, aux, slowPath);
    a_.mov(inlineSlot(0), RAX);
    for (std::size_t k = 0; k < stores.size(); ++k) {
        const IVal& v = stores[k].value;
        inlineBoxed(v);
        if (v.kind == IVal::Box || (v.kind == IVal::Const && v.c.isCell()))
            retain(RAX);
        a_.mov(RCX, inlineSlot(0));
        a_.mov(mem(RCX, kObjInline + std::int32_t(8 * stores[k].index)), RAX);
    }
    a_.mov(RAX, inlineSlot(0));
    write(in.a, RAX);
    a_.jmp(done);
    a_.bind(slowPath);
    materializeReads(i);
    call(i, in);
    a_.bind(done);
    if (plan_->defWeb[i] >= 0)
        loadTypedAfter(i);
    return true;
}

// ---- call windows ----
//
// The instructions that fill the registers of an inlined call -- the callee, `this`, the arguments -- write nothing when
// the registers are dead after it: the inlined body reads a copy's source, a constant, or (a method read) a slot. A failed
// guard of the body then runs again from the first of them, which have no effects.

void Baseline::planWindows()
{
    windowVal_.clear();
    windowRead_.clear();
    if (!code_->handlers.empty() || inlineBase_ < 0)
        return;
    std::vector<std::uint32_t> starts;
    for (std::uint32_t j = 0; j < n_; j += length(j))
        starts.push_back(j);
    auto reads = [&](std::uint32_t j, unsigned r) {
        for (std::uint16_t x : opt::effectsOf(*code_, j).rd)
            if (x == r)
                return true;
        return false;
    };
    auto writes = [&](std::uint32_t j, unsigned r) {
        for (std::uint16_t x : opt::effectsOf(*code_, j).wr)
            if (x == r)
                return true;
        return false;
    };
    for (std::size_t k = 0; k < starts.size(); ++k) {
        const std::uint32_t i = starts[k];
        const Ins& in = code_->code[i];
        if (in.opcode() != Op::Call || directCalls_.count(i) || (i < code_->prof.size() && (code_->prof[i] & opt::kProfFailed)))
            continue;
        const unsigned base = in.b, argc = in.c;
        const auto& uw = plan_->useWebs[i];
        if (uw.size() < 2u + argc)
            continue;
        std::map<std::size_t, IVal> vals;
        std::map<std::uint32_t, std::pair<std::int32_t, unsigned>> reads_;
        std::set<std::uint32_t> defs;
        std::uint32_t first = i;
        std::int32_t nextSlot = 0;
        // `this` and the arguments first, the callee last: a method read takes its receiver from the copy that filled `this`.
        for (std::size_t n = 0; n < 2u + argc; ++n) {
            const std::size_t op = n + 1 < 2u + argc ? n + 1 : 0;
            if (uw[op] >= 0)
                continue;
            const unsigned r = base + unsigned(op);
            // Its definition in this block, with nothing between reading it but the method read of this call.
            std::int64_t d = -1;
            for (std::size_t m = k; m-- > 0;) {
                const std::uint32_t j = starts[m];
                if (writes(j, r)) {
                    d = j;
                    break;
                }
                const bool methodRead = op == 1 && code_->code[j].opcode() == Op::GetProp && code_->code[j].a == base &&
                                        code_->code[j].b == r;
                if ((reads(j, r) && !methodRead) || plan_->isLeader[j])
                    break;
            }
            if (d < 0 || !regDeadAfter(i, r) || plan_->defWeb[std::size_t(d)] >= 0)
                continue;
            const Ins& di = code_->code[std::size_t(d)];
            IVal v;
            switch (di.opcode()) {
            case Op::LoadUndef: case Op::LoadNull: case Op::LoadTrue: case Op::LoadFalse:
                v.c = di.opcode() == Op::LoadUndef ? JSValue::undefined() : di.opcode() == Op::LoadNull ? JSValue::null()
                    : JSValue::boolean(di.opcode() == Op::LoadTrue);
                break;
            case Op::LoadInt:
                v.c = JSValue::int32(di.imm());
                break;
            case Op::Mov: {
                if (plan_->useWebs[std::size_t(d)].empty() || plan_->useWebs[std::size_t(d)][0] >= 0)
                    continue;
                // The source stays as it is up to the call.
                bool changed = false;
                for (std::uint32_t j = std::uint32_t(d) + length(std::uint32_t(d)); j < i; j += length(j))
                    changed = changed || writes(j, di.b);
                if (changed)
                    continue;
                v.kind = IVal::Box;
                v.frameReg = std::int32_t(di.b);
                break;
            }
            case Op::GetProp: {
                if (!chainable(std::uint32_t(d), false) || nextSlot >= std::int32_t(kWindowSlots))
                    continue;
                // The receiver: a register nothing writes up to the call, or a copy that is itself a window's.
                unsigned recv = di.b;
                for (auto& [o, val] : vals)
                    if (base + o == di.b && val.kind == IVal::Box && val.frameReg >= 0)
                        recv = unsigned(val.frameReg);
                bool changed = false;
                for (std::uint32_t j = std::uint32_t(d) + length(std::uint32_t(d)); j < i; j += length(j))
                    changed = changed || (writes(j, recv) && !(recv == di.b && defs.count(j)));
                if (changed && recv == di.b)
                    continue;
                v.kind = IVal::Box;
                v.slot = -2 - nextSlot;  // a window slot (resolved by inlineBits)
                v.hint = 8;
                reads_[std::uint32_t(d)] = {nextSlot, recv};
                ++nextSlot;
                break;
            }
            default:
                continue;
            }
            vals[op] = v;
            defs.insert(std::uint32_t(d));
            first = std::min(first, std::uint32_t(d));
        }
        if (vals.empty())
            continue;
        // From the first of them to the call: those definitions and typed arithmetic, nothing with effects.
        bool pure = true;
        for (std::uint32_t j = first; j < i && pure; j += length(j)) {
            if (defs.count(j))
                continue;
            if (j != first && (plan_->isLeader[j] || opt::jumpTarget(*code_, j) >= 0)) {
                pure = false;
                break;
            }
            const Op op = code_->code[j].opcode();
            bool typed = plan_->defWeb[j] >= 0 || op == Op::LoadInt;
            for (std::int32_t w : plan_->useWebs[j])
                typed = typed && w >= 0;
            const opt::Effects e = opt::effectsOf(*code_, j);
            if (!typed || e.wr.size() != 1) {
                pure = false;
                break;
            }
            for (const opt::LiveWeb& lw : plan_->liveBefore[first])
                if (lw.reg == e.wr[0])
                    pure = false;
            for (const auto& [o, val] : vals)
                if (val.kind == IVal::Box && val.frameReg >= 0 && e.wr[0] == unsigned(val.frameReg))
                    pure = false;
        }
        if (!pure)
            continue;
        windowVal_[i] = vals;
        const bool ok = inlineCall(i, true);
#if defined(SCRIPT_JIT_ENV_OVERRIDE)
        if (std::getenv("T2WIN"))
            std::fprintf(stderr, "window %s @%u from %u: %zu values, %s\n", codeName(code_).c_str(), i, first, vals.size(), ok ? "yes" : "no");
#endif
        if (!ok) {
            windowVal_.erase(i);
            continue;
        }
        for (std::uint32_t d : defs)
            if (!reads_.count(d))
                skipDef_[d] = 1;
        for (auto& [d, sr] : reads_)
            windowRead_[d] = sr;
        for (std::uint32_t j = first + length(first); j <= i; j += length(j))
            exitRedirect_[j] = std::int32_t(first);
    }
}

void Baseline::emitWindowRead(std::uint32_t i)
{
    // A method read for an inlined call: into its slot, from the receiver the planner chose.
    const auto& [slot, recv] = windowRead_.at(i);
    const Ins& in = code_->code[i];
    const InlineCache& ic = code_->caches[code_->code[i + 1].a];
    Label& bad = exitBefore(i);
    struct Seen
    {
        std::uint32_t shape, index;
        JSObject* holder;
        ValidityCell* validity;
    };
    Seen seen[5];
    int count = 0;
    seen[count++] = Seen{ic.shape, ic.index, ic.holder, ic.validity};
    if (ic.pad && ic.pad <= code_->poly.size()) {
        const PolyCache& pc = code_->poly[ic.pad - 1];
        for (std::uint8_t k = 0; k < pc.n; ++k)
            seen[count++] = Seen{pc.e[k].shape, pc.e[k].index, pc.e[k].holder, pc.e[k].validity};
    }
    (void)in;
    a_.mov(RAX, R(recv));
    needObject(RAX, bad);
    Label got;
    for (int k = 0; k < count; ++k) {
        Label next;
        a_.cmpI32(mem(RAX, 0), std::int32_t(seen[k].shape));
        a_.jcc(CNE, k + 1 < count ? next : bad);
        if (seen[k].holder) {
            hold(seen[k].validity);
            a_.movImm(RCX, reinterpret_cast<std::uint64_t>(seen[k].validity));
            a_.cmpI32(mem(RCX, kValidityValid), 0);
            a_.jcc(CE, bad);
            a_.movImm(RAX, reinterpret_cast<std::uint64_t>(seen[k].holder));
        }
        slotAddress(seen[k].index);
        a_.jmp(got);
        if (k + 1 < count)
            a_.bind(next);
    }
    a_.bind(got);
    a_.mov(RAX, mem(RDX, 0));
    a_.mov(mem(RSP, windowBase_ + 8 * slot), RAX);
}

// ---- Math calls ----
//
// `Math.sqrt(x)`: when the registers of `Math` and of its method are dead after the call, neither read writes them; the
// call reads both itself and checks the function, and a failed check runs again from the read of `Math`.

void Baseline::planMathCalls()
{
    mathWin_.clear();
    if (!code_->handlers.empty())
        return;
    std::vector<std::uint32_t> starts;
    for (std::uint32_t j = 0; j < n_; j += length(j))
        starts.push_back(j);
    auto reads = [&](std::uint32_t j, unsigned r) {
        for (std::uint16_t x : opt::effectsOf(*code_, j).rd)
            if (x == r)
                return true;
        return false;
    };
    for (std::size_t k = 0; k < starts.size(); ++k) {
        const std::uint32_t i = starts[k];
        const Ins& in = code_->code[i];
        if (in.opcode() != Op::Call || in.c != 1 || directCalls_.count(i) || windowVal_.count(i))
            continue;
        if (i < code_->prof.size() && (code_->prof[i] & opt::kProfFailed))
            continue;
        const auto& uw = plan_->useWebs[i];
        if (plan_->defWeb[i] < 0 || uw.size() < 3 || uw[2] < 0 || uw[0] >= 0 || uw[1] >= 0)
            continue;
        const JSValue callee = constantBefore(i, in.b);
        if (!isFunction(callee))
            continue;
        auto* fn = static_cast<JSFunction*>(asObject(callee));
        if (!(fn->flags & JSFunction::kNative) || !fn->rare() || fn->rare()->native != mathUnaryNative())
            continue;
        const unsigned base = in.b;
        // GetGlobal base+1; GetProp base base+1; then only typed arithmetic up to the call.
        if (k < 2)
            continue;
        std::size_t m = k;
        std::uint32_t d2 = 0, d1 = 0;
        bool found2 = false;
        while (m-- > 0) {
            const std::uint32_t j = starts[m];
            const Ins& ji = code_->code[j];
            if (!found2 && ji.opcode() == Op::GetProp && ji.a == base && ji.b == base + 1) {
                d2 = j;
                found2 = true;
                continue;
            }
            if (found2 && ji.opcode() == Op::GetGlobal && ji.a == base + 1) {
                d1 = j;
                break;
            }
            // Between: typed arithmetic that touches neither register.
            bool typed = plan_->defWeb[j] >= 0 || ji.opcode() == Op::LoadInt;
            for (std::int32_t w : plan_->useWebs[j])
                typed = typed && w >= 0;
            const opt::Effects e = opt::effectsOf(*code_, j);
            if (!typed || e.wr.size() != 1 || e.wr[0] == base || e.wr[0] == base + 1 || reads(j, base) ||
                reads(j, base + 1) || plan_->isLeader[j] || opt::jumpTarget(*code_, j) >= 0) {
                m = 0;
                found2 = false;
                break;
            }
        }
#if defined(SCRIPT_JIT_ENV_OVERRIDE)
        if (std::getenv("T2MATH"))
            std::fprintf(stderr, "math @%u found %d d1 %u d2 %u leader %d c1 %d c2 %d w1 %d w2 %d dead %d %d\n", i, found2, d1, d2,
                         found2 ? int(plan_->isLeader[d2]) : -1, found2 ? int(chainable(d1, false)) : -1,
                         found2 ? int(chainable(d2, false)) : -1, found2 ? plan_->defWeb[d1] : -9, found2 ? plan_->defWeb[d2] : -9,
                         int(regDeadAfter(i, base)), int(regDeadAfter(i, base + 1)));
#endif
        if (!found2 || code_->code[d1].opcode() != Op::GetGlobal || plan_->isLeader[d2] || !chainable(d1, false) ||
            !chainable(d2, false) || plan_->defWeb[d1] >= 0 || plan_->defWeb[d2] >= 0)
            continue;
        if (!regDeadAfter(i, base) || !regDeadAfter(i, base + 1))
            continue;
        // Nothing between the read of `Math` and the call holds what was there at the start, or exits elsewhere.
        bool clean = true;
        for (std::uint32_t j = d1 + length(d1); j < i && clean; j += length(j)) {
            if (j == d2)
                continue;
            const opt::Effects e = opt::effectsOf(*code_, j);
            for (const opt::LiveWeb& lw : plan_->liveBefore[d1])
                if (!e.wr.empty() && lw.reg == e.wr[0])
                    clean = false;
        }
#if defined(SCRIPT_JIT_ENV_OVERRIDE)
        if (std::getenv("T2MATH"))
            std::fprintf(stderr, "math @%u clean %d\n", i, int(clean));
#endif
        if (!clean)
            continue;
        mathWin_[i] = {d1, d2};
        skipDef_[d1] = 1;
        skipDef_[d2] = 1;
        for (std::uint32_t j = d1 + length(d1); j <= i; j += length(j))
            exitRedirect_[j] = std::int32_t(d1);
    }
}

/// The value a skipped GetGlobal or GetProp (on rax, when `fromRax`) reads, into rax; `bad` when its guards fail.
void Baseline::emitReadInto(std::uint32_t i, bool fromRax, Label& bad)
{
    const Ins& in = code_->code[i];
    if (in.opcode() == Op::GetGlobal) {
        JSObject* g = vm_.global;
        std::uint8_t attrs = 0;
        JSValue* slot = g->findOwn(vm_, code_->atoms[in.b], &attrs);
        JSValue* inl = g->inl();
        const std::uint32_t index = slot >= inl && slot < inl + g->h.aux ? std::uint32_t(slot - inl)
                                                                          : std::uint32_t(slot - g->slots) + g->h.aux;
        a_.cmpI32(mem(R12, kVmGlobalEpoch), std::int32_t(vm_.globalEpoch));
        a_.jcc(CNE, bad);
        globalSlot(index);
        a_.mov(RAX, mem(RDX, 0));
        return;
    }
    const InlineCache& ic = code_->caches[code_->code[i + 1].a];
    struct Seen
    {
        std::uint32_t shape, index;
        JSObject* holder;
        ValidityCell* validity;
    };
    Seen seen[5];
    int count = 0;
    seen[count++] = Seen{ic.shape, ic.index, ic.holder, ic.validity};
    if (ic.pad && ic.pad <= code_->poly.size()) {
        const PolyCache& pc = code_->poly[ic.pad - 1];
        for (std::uint8_t k = 0; k < pc.n; ++k)
            seen[count++] = Seen{pc.e[k].shape, pc.e[k].index, pc.e[k].holder, pc.e[k].validity};
    }
    if (!fromRax)
        a_.mov(RAX, R(in.b));
    needObject(RAX, bad);
    Label got;
    for (int k = 0; k < count; ++k) {
        Label next;
        a_.cmpI32(mem(RAX, 0), std::int32_t(seen[k].shape));
        a_.jcc(CNE, k + 1 < count ? next : bad);
        if (seen[k].holder) {
            hold(seen[k].validity);
            a_.movImm(RCX, reinterpret_cast<std::uint64_t>(seen[k].validity));
            a_.cmpI32(mem(RCX, kValidityValid), 0);
            a_.jcc(CE, bad);
            a_.movImm(RAX, reinterpret_cast<std::uint64_t>(seen[k].holder));
        }
        slotAddress(seen[k].index);
        a_.jmp(got);
        if (k + 1 < count)
            a_.bind(next);
    }
    a_.bind(got);
    a_.mov(RAX, mem(RDX, 0));
}
