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

[[maybe_unused]] std::string codeName(const CodeBlock* c)
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
    if (!C || C == code_ || (C->flags & CodeBlock::kLazy) || C->prof.size() < C->code.size() + C->nparams || C->code.size() > kInlineMaxCode)
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
    calleeChecked_ = false;
    if (known.isUndefined() && callee.kind == IVal::Box && callee.slot <= -2) {
        std::int64_t read = -1;
        known = windowMethod(i, -2 - callee.slot, C, &read);
        if (read >= 0)
            if (const LoopWin* lw = hoistedWin(std::uint32_t(read)))
                calleeChecked_ = lw->known == known && !known.isUndefined();
    }
    std::vector<IVal> args;
    args.push_back(callerValue(i, base + 1, 1, 8));
    for (unsigned k = 0; k < argc; ++k)
        args.push_back(callerValue(i, base + 2 + k, 2 + k, k < C->nparams ? C->prof[C->code.size() + k] : 8));
    const bool done = inlineTop(i, C, callee, known, args, in.a, decideOnly);
    calleeChecked_ = false;
    return done;
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
            provenShape(in.b, e.shape, bad);
            checkValidity(e.validity, bad);
            holderRead(e.holder, e.index);
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
                holdValue(known);
                if (callee.kind != IVal::Const && !(depth == 0 && calleeChecked_)) {
                    inlineBits(callee, RAX);
                    a_.movImm(RCX, known.bits);
                    a_.cmp(RCX, RAX);
                    a_.jcc(CNE, *bad);
                }
            } else {
                // A frame register already known to hold a function of this code needs no look.
                const bool inFrame = callee.kind == IVal::Box && callee.frameReg >= 0 &&
                                     std::size_t(callee.frameReg) < factCode_.size();
                if (!(inFrame && factCode_[std::size_t(callee.frameReg)] == C)) {
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
                    if (inFrame)
                        factCode_[std::size_t(callee.frameReg)] = C;
                }
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
    // The epoch at which an upvalue's box was last seen holding a number (-1: never): one to store over needs no release.
    std::vector<std::int64_t> upNumber(C->upvals.size() + 1, -1);
    auto numeric = [](const IVal& v) {
        return v.kind == IVal::I32 || v.kind == IVal::F64 || (v.kind == IVal::Const && v.c.isNumber());
    };
    // The box of the upvalue `GetUpval` just read, still in rdi (-1: none): a `PutUpval` of it after only arithmetic and
    // loads stores through it.
    std::int64_t boxInRdi = -1;
    for (std::uint32_t j = 0; j < C->code.size(); j = opt::nextOf(*C, j)) {
        const Ins& ci = C->code[j];
        const Op op = ci.opcode();
        const std::uint8_t p = C->prof[j];
        switch (op) {
        case Op::LoadInt: case Op::LoadK: case Op::LoadUndef: case Op::LoadNull: case Op::LoadTrue: case Op::LoadFalse:
        case Op::Mov: case Op::PutUpval: case Op::Add: case Op::Sub: case Op::Mul: case Op::Div: case Op::BitAnd:
        case Op::BitOr: case Op::BitXor: case Op::Shl: case Op::Shr: case Op::AddI: case Op::SubI: case Op::MulI:
        case Op::BitAndI: case Op::BitOrI: case Op::BitXorI: case Op::ShlI: case Op::ShrI: case Op::UShrI: case Op::Inc:
        case Op::Dec:
            break;
        default:
            boxInRdi = -1;
            break;
        }
        switch (op) {
        case Op::LoadInt:
            regs[ci.a] = IVal{};
            regs[ci.a].c = JSValue::int32(ci.imm());
            break;
        case Op::LoadK:
            regs[ci.a] = IVal{};
            regs[ci.a].c = C->consts[ci.kIndex()];
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
            PolySeen seen[5];
            int count = 0;
            if (ic.shape != 0 && ic.next == 0 && (!ic.holder || ic.validity))
                seen[count++] = PolySeen{ic.shape, ic.index, ic.holder, ic.validity, ic.aux};
            if (ic.pad && ic.pad <= C->poly.size()) {
                const PolyCache& pc = C->poly[ic.pad - 1];
                for (std::uint8_t k = 0; k < pc.n; ++k) {
                    const PolyEntry& e = pc.e[k];
                    if (e.kind == PolyEntry::OwnData || (e.kind == PolyEntry::ProtoData && e.validity))
                        seen[count++] = PolySeen{e.shape, e.index, e.holder, e.validity, e.aux};
                    else
                        return false;
                }
            }
            Label* bad = cx.fail(this);
            if (!count || !bad)
                return false;
            if (emit) {
                inlineBits(recv, RAX);
                polyRead(seen, count, *bad, recv.kind == IVal::Box && recv.frameReg >= 0 ? recv.frameReg : -1);
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
                checkValidity(sc.validity, *bad);
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
        case Op::GetGlobal: {
            // A global that is there, as a data property: read with the epoch check the other global reads have.
            std::uint8_t attrs = 0;
            JSValue* slot = vm_.global->findOwn(vm_, C->atoms[ci.b], &attrs);
            Label* bad = cx.fail(this);
            if (!slot || (attrs & attr::kAccessor) || !bad)
                return false;
            if (emit) {
                JSObject* g = vm_.global;
                JSValue* inl = g->inl();
                const std::uint32_t index = slot >= inl && slot < inl + g->h.aux ? std::uint32_t(slot - inl)
                                                                                : std::uint32_t(slot - g->slots) + g->h.aux;
                a_.cmpI32(mem(R12, kVmGlobalEpoch), std::int32_t(vm_.globalEpoch));
                a_.jcc(CNE, *bad);
                globalSlot(index);
                a_.mov(RAX, mem(RDX, 0));
            }
            IVal out;
            if (!takeRead(p ? p : 8, bad, out))
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
                a_.mov(RDI, mem(RAX, kFnUpvals + std::int32_t(8 * ci.b)));
                a_.mov(RAX, mem(RDI, kBoxValue));
            }
            IVal out;
            if (!takeRead(p, bad, out))
                return false;
            if (numeric(out) && ci.b < upNumber.size())
                upNumber[ci.b] = std::int64_t(cx.epoch);
            regs[ci.a] = out;
            boxInRdi = std::int64_t(ci.b);
            break;
        }
        case Op::PutUpval: {
            if (ci.a >= upKnown.size())
                return false;
            const IVal v = regs[ci.b];
            if (stale(v))
                return false;
            if (emit && boxInRdi == std::int64_t(ci.a)) {
                // The box the read before found, in rdi: the value stored through it.
                inlineBoxed(v);
                if (v.kind == IVal::Box || (v.kind == IVal::Const && v.c.isCell()))
                    retain(RAX);
                if (upNumber[ci.a] == std::int64_t(cx.epoch)) {
                    a_.mov(mem(RDI, kBoxValue), RAX);  // over a number seen since the last effect
                } else {
                    a_.mov(RSI, mem(RDI, kBoxValue));
                    a_.mov(mem(RDI, kBoxValue), RAX);
                    release(RSI);
                }
            } else if (emit) {
                inlineBoxed(v);
                a_.mov(RCX, RAX);
                if (v.kind == IVal::Box || (v.kind == IVal::Const && v.c.isCell()))
                    retain(RCX);
                loadCallee();
                a_.mov(RAX, mem(RAX, kFnUpvals + std::int32_t(8 * ci.a)));
                if (upNumber[ci.a] == std::int64_t(cx.epoch)) {
                    a_.mov(mem(RAX, kBoxValue), RCX);  // over a number seen since the last effect
                } else {
                    a_.mov(RSI, mem(RAX, kBoxValue));
                    a_.mov(mem(RAX, kBoxValue), RCX);
                    release(RSI);
                }
            }
            boxInRdi = -1;
            cx.sideEffect = true;
            ++cx.epoch;
            upNumber[ci.a] = numeric(v) ? std::int64_t(cx.epoch) : -1;
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
        case Op::JLt: case Op::JLe: case Op::JGt: case Op::JGe: case Op::JNotLt: case Op::JNotLe: case Op::JNotGt:
        case Op::JNotGe: case Op::JStrictEq: case Op::JStrictNe: case Op::JLtI: case Op::JLeI: case Op::JGtI: case Op::JGeI:
        case Op::JNotLtI: case Op::JNotLeI: case Op::JNotGtI: case Op::JNotGeI: case Op::JStrictEqI: case Op::JStrictNeI: {
            // A test of int32s (`if (x >= 0) return x;`): the body goes on the way the test falls through, and the other
            // way leaves before any effect, running the call itself. Two constants decide it here.
            const bool imm = op == Op::JLtI || op == Op::JLeI || op == Op::JGtI || op == Op::JGeI || op == Op::JNotLtI ||
                             op == Op::JNotLeI || op == Op::JNotGtI || op == Op::JNotGeI || op == Op::JStrictEqI ||
                             op == Op::JStrictNeI;
            const IVal A = regs[ci.a];
            IVal B;
            if (imm)
                B.c = JSValue::int32(std::int16_t(ci.b));
            else
                B = regs[ci.b];
            if (stale(A) || stale(B) || !isInt(A) || !isInt(B))
                return false;
            const Relation rel = relationOf(op);
            if (A.kind == IVal::Const && B.kind == IVal::Const && A.c.isInt32() && B.c.isInt32()) {
                const std::int32_t x = A.c.asInt32(), y = B.c.asInt32();
                bool taken = false;
                switch (rel.ic) {
                case CL: taken = x < y; break;
                case CLE: taken = x <= y; break;
                case CG: taken = x > y; break;
                case CGE: taken = x >= y; break;
                case CE: taken = x == y; break;
                default: taken = x != y; break;
                }
                if (taken)
                    return false;  // the way it jumps is not followed
                break;
            }
            Label* bad = cx.fail(this);
            if (!bad)
                return false;
            if (emit) {
                inlineI32(A, RAX, bad);
                if (imm) {
                    a_.aluI32(7, RAX, std::int16_t(ci.b));
                } else {
                    inlineI32(B, RCX, bad);
                    a_.alu32(7, RAX, RCX);
                }
                a_.jcc(rel.ic, *bad);
            }
            break;
        }
        case Op::Ret:
            result = regs[ci.a];
            return !stale(result);
        default:
#if defined(SCRIPT_JIT_ENV_OVERRIDE)
            if (!emit && depth == 0)
                if (const char* t = std::getenv("T2INL"); t && *t == '2')
                    std::fprintf(stderr, "inline refused: %s in a body of %u instructions\n", opName(op), unsigned(C->code.size()));
#endif
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
        case Op::GetProp:
            // A read of data properties where the site has found them: done without the runtime, or not at all.
            return plan_->useWebs[j].size() >= 1 && plan_->useWebs[j][0] < 0 && plainReadable(j);
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
    selfGuards_.clear();
    for (std::size_t k = 0; k < starts.size(); ++k) {
        const std::uint32_t i = starts[k];
        const Ins& in = code_->code[i];
        if (in.opcode() == Op::JNotCallee && code_->inlinedFrom) {
            // The way into a call run in place: the global read before it, quiet instructions between.
            const std::int64_t g = i;
            const unsigned base = in.a;
            const JSValue f = constantBefore(i, base);
            if (!isFunction(f) || static_cast<JSFunction*>(asObject(f))->code != code_)
                continue;
            const std::int64_t fb = opt::jumpTarget(*code_, std::uint32_t(g));
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
            if (!ok || d < 0 || fb < 0 || code_->code[std::size_t(fb)].opcode() != Op::Call ||
                !regDeadAfter(std::uint32_t(fb), base))
                continue;
            JSObject* gl = vm_.global;
            std::uint8_t attrs = 0;
            JSValue* slot = gl->findOwn(vm_, code_->atoms[code_->code[std::size_t(d)].b], &attrs);
            if (!slot || (attrs & attr::kAccessor) || *slot != f)
                continue;
            JSValue* inl = gl->inl();
            const std::uint32_t index = slot >= inl && slot < inl + gl->h.aux ? std::uint32_t(slot - inl)
                                                                               : std::uint32_t(slot - gl->slots) + gl->h.aux;
            if (t >= 0) {
                // `this` only for the long way: the body run in place does not read it, nothing between does.
                bool used = !regDeadAfter(std::uint32_t(fb), base + 1);
                for (std::uint32_t j = std::uint32_t(t) + 1; j < n_ && !used; j += length(j))
                    if (j != std::uint32_t(fb) && reads(j, base + 1))
                        used = (j > std::uint32_t(t) && j < i) || (j > std::uint32_t(g) && j < std::uint32_t(fb)) ||
                               j == std::uint32_t(g);
                if (used)
                    t = -1;
            }
            if ((code_->prof[std::size_t(d)] | code_->prof[std::size_t(g)]) & opt::kProfFailed)
                continue;
            holdValue(f);
            selfGuards_[std::uint32_t(g)] = SelfGuard{f, index, std::uint32_t(d), t};
            skipDef_[std::size_t(d)] = 1;
            if (t >= 0)
                skipDef_[std::size_t(t)] = 1;
            for (std::size_t m = 0; m < k; ++m)
                if (starts[m] > std::uint32_t(d))
                    exitRedirect_[starts[m]] = std::int32_t(d);
            continue;
        }
        if (in.opcode() != Op::Call || (i < code_->prof.size() && (code_->prof[i] & opt::kProfFailed)))
            continue;
        const unsigned base = in.b, argc = in.c;
        const JSValue f = constantBefore(i, base);
        if (!isFunction(f)) {
            planCodeCall(i);
            continue;
        }
        auto* fn = static_cast<JSFunction*>(asObject(f));
        if (fn->flags & JSFunction::kNative)
            continue;
        CodeBlock* C = fn->code;
        if (!C || argc > C->nparams ||
            (C->flags & (CodeBlock::kLazy | CodeBlock::kGenerator | CodeBlock::kAsync | CodeBlock::kAsyncGen | CodeBlock::kNeedsArgs |
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

/// A call whose callee is no constant (a method, a function in a variable): the callee that site has called, by its code's
/// uid, is checked at the call and the frame is pushed from constants, as for a global function.
void Baseline::planCodeCall(std::uint32_t i)
{
    const Ins& in = code_->code[i];
    if (i >= code_->callProf.size() || !code_->callProf[i])
        return;
    CodeBlock* C = vm_.codeByUid(code_->callProf[i]);
    const unsigned argc = in.c;
    if (!C || C == code_ || C->inlinedFrom || argc > C->nparams ||
        (C->flags & (CodeBlock::kLazy | CodeBlock::kGenerator | CodeBlock::kAsync | CodeBlock::kAsyncGen | CodeBlock::kNeedsArgs |
                     CodeBlock::kClassConstructor | CodeBlock::kProgram)))
        return;
    if (inlineCall(i, true))
        return;
    directCalls_[i] = DirectCall{JSValue::raw(0), C, 0, false, 0, false, true};
#if defined(SCRIPT_JIT_ENV_OVERRIDE)
    if (std::getenv("T2INL"))
        std::fprintf(stderr, "code call @%u of %s\n", i, codeName(C).c_str());
#endif
}

/// A `new` made in place after another of the same constructor, with nothing between that could change the global, the
/// constructor or its prototype chain -- in a function that can change none of them (typed arithmetic, reads, jumps,
/// calls of itself and such `new`s) -- need not check them again.
void Baseline::planNewChecked()
{
    pureFn_ = false;
    if (!code_->handlers.empty() || !plan_->loopHeaders.empty())
        return;
    auto typedUses = [&](std::uint32_t j) {
        for (std::int32_t w : plan_->useWebs[j])
            if (w < 0)
                return false;
        return true;
    };
    for (std::uint32_t j = 0; j < n_; j += length(j)) {
        const Ins& in = code_->code[j];
        const Op op = in.opcode();
        bool ok = false;
        switch (op) {
        case Op::Nop: case Op::Jmp: case Op::LoadUndef: case Op::LoadNull: case Op::LoadInt: case Op::LoadK:
        case Op::LoadTrue: case Op::LoadFalse: case Op::Mov: case Op::Ret:
            ok = true;
            break;
        case Op::GetProp:
            ok = plan_->useWebs[j].size() >= 1 && plan_->useWebs[j][0] < 0 && plainReadable(j);
            break;
        case Op::GetGlobal: {
            std::uint8_t attrs = 0;
            ok = vm_.global->findOwn(vm_, code_->atoms[in.b], &attrs) && !(attrs & attr::kAccessor);
            break;
        }
        case Op::New:
            ok = newInline_.count(j) != 0;
            break;
        case Op::JNotCallee:
            ok = selfGuards_.count(j) != 0;
            break;
        case Op::Call: {
            ok = fallbackOf(j);
            if (auto dc = directCalls_.find(j); dc != directCalls_.end() && !selfGuards_.empty())
                ok = ok || dc->second.fn == selfGuards_.begin()->second.fn;
            break;
        }
        default:
            ok = (opt::isConditionalJump(op) && (typedUses(j) || bitTestAt(j))) ||
                 ((op == Op::Add || op == Op::Sub || op == Op::AddI || op == Op::SubI || op == Op::Inc || op == Op::Dec ||
                   op == Op::Mul || op == Op::MulI) && plan_->defWeb[j] >= 0 && typedUses(j));
            break;
        }
        if (!ok && (j < skipDef_.size() && skipDef_[j]))
            ok = true;
        if (!ok)
            return;
    }
    pureFn_ = true;
    if (newInline_.empty())
        return;
    JSValue known = JSValue::undefined();
    for (std::uint32_t j : newInline_) {
        JSValue k = constantBefore(j, code_->code[j].b);
        if (!isFunction(k))
            k = guessBefore(j, code_->code[j].b);
        if (known.isUndefined())
            known = k;
        else if (k != known)
            return;
    }
    // Forward, meeting by "and": 2 unreached, 1 checked, 0 not.
    std::vector<std::uint8_t> in(n_ + 1, 2);
    std::vector<std::uint32_t> work{0};
    in[0] = 0;
    auto flow = [&](std::uint32_t to, std::uint8_t v) {
        if (to >= n_)
            return;
        const std::uint8_t nv = in[to] == 2 ? v : std::uint8_t(in[to] & v);
        if (nv != in[to]) {
            in[to] = nv;
            work.push_back(to);
        }
    };
    while (!work.empty()) {
        const std::uint32_t j = work.back();
        work.pop_back();
        const Op op = code_->code[j].opcode();
        if (op == Op::Call && fallbackOf(j))
            continue;
        const std::uint8_t out = op == Op::New ? 1 : in[j];
        const std::int64_t t = opt::jumpTarget(*code_, j);
        if (t >= 0 && !(op == Op::JNotCallee && selfGuards_.count(j)))
            flow(std::uint32_t(t), out);
        if (op != Op::Jmp && op != Op::Ret && op != Op::Throw && op != Op::ThrowError)
            flow(j + length(j), out);
    }
    for (std::uint32_t j = 0; j < n_; ++j)
        newChecked_[j] = code_->code[j].opcode() == Op::New && in[j] == 1;
}

/// In a function that changes no object it did not make and no global (`pureFn_`), and never writes its parameters, a
/// register that only ever holds a parameter, `this`, a global, or a property read from one of those -- or no cell at all --
/// holds them without a count all through the call: what holds them cannot let go of them meanwhile.
void Baseline::planFnBorrows()
{
    fnBorrow_.clear();
    gated_ = false;
    calleeReads_.clear();
    if (!pureFn_ || code_->nregs > 250)
        return;
#if defined(SCRIPT_JIT_ENV_OVERRIDE)
    if (std::getenv("T2NOFNBORROW"))
        return;
#endif
    const unsigned first = 1 + code_->nparams;
    std::vector<std::uint8_t> cand(code_->nregs, 0), bad(code_->nregs, 0);
    // Registers past a call's arguments can be released by the long way of the call (the callee's frame covers them).
    // Registers a callee's frame covers past the arguments: its code takes them for its own (it may let go of what they
    // hold), so none of these is held without a count.
    for (std::uint32_t j = 0; j < n_; j += length(j)) {
        const Ins& in = code_->code[j];
        const opt::Effects e = opt::effectsOf(*code_, j);
        for (std::uint16_t r : e.wr)
            if (r < first)
                return;  // a parameter or `this` written
        if ((in.opcode() == Op::Call || in.opcode() == Op::New) && !(in.opcode() == Op::Call && fallbackOf(j))) {
            unsigned calleeRegs = code_->nregs + 1u;  // unknown: all the way up
            if (auto dc = directCalls_.find(j); dc != directCalls_.end()) {
                calleeRegs = dc->second.code->nregs;
            } else if (in.opcode() == Op::New && newInline_.count(j)) {
                JSValue k = constantBefore(j, in.b);
                if (!isFunction(k))
                    k = guessBefore(j, in.b);
                if (isFunction(k) && static_cast<JSFunction*>(asObject(k))->code)
                    calleeRegs = static_cast<JSFunction*>(asObject(k))->code->nregs;
            }
            for (unsigned r = unsigned(in.b) + 2 + unsigned(in.c); r < unsigned(in.b) + 1 + calleeRegs && r < code_->nregs; ++r)
                bad[r] = 1;
            // The callee and `this`: written the long way (an inlined `new` only compares its callee, and its long way
            // constructs with the one compared).
            const bool inlined = in.opcode() == Op::New &&
                                 std::find(newInline_.begin(), newInline_.end(), j) != newInline_.end();
            for (unsigned r = inlined ? in.b + 1u : in.b; r < unsigned(in.b) + 2 && r < code_->nregs; ++r)
                bad[r] = 1;
            if (auto dc = directCalls_.find(j); dc != directCalls_.end()) {
                const CodeBlock* C = dc->second.code;
                if (C != code_ || unsigned(in.c) > C->nparams)
                    return;  // arguments past the parameters are the callee's to let go of
                gated_ = true;
            }
        }
    }
    if (!gated_)
        return;  // no call of itself: nothing to gain
    planCalleeReads();
    gated_ = false;  // decided below: worth it only when something is held without a count, or read ahead
    for (std::uint32_t j = 0; j < n_; j += length(j)) {
        const Ins& in = code_->code[j];
        const Op op = in.opcode();
        if (op == Op::Ret && in.a < code_->nregs)
            bad[in.a] = 1;  // what is returned goes with its count
        const opt::Effects e = opt::effectsOf(*code_, j);
        if (e.wr.empty() || (op == Op::Call && fallbackOf(j)) || quietDefs_.count(j))
            continue;
        const unsigned r = e.wr[0];
        if (r >= code_->nregs)
            continue;
        const bool skipped = (j < skipDef_.size() && skipDef_[j]);
        const bool neutral = op == Op::LoadUndef || op == Op::LoadNull || op == Op::LoadTrue || op == Op::LoadFalse ||
                             op == Op::LoadInt || (op == Op::LoadK && !code_->consts[in.kIndex()].isCell()) ||
                             plan_->defWeb[j] >= 0;
        std::uint8_t attrs = 0;
        const bool source = !skipped && plan_->defWeb[j] < 0 &&
                            ((op == Op::GetGlobal && vm_.global->findOwn(vm_, code_->atoms[in.b], &attrs) &&
                              !(attrs & attr::kAccessor)) ||
                             (op == Op::GetProp && !(j < chainRole_.size() && chainRole_[j]) && !windowRead_.count(j) &&
                              !reuseRead_.count(j)) ||
                             op == Op::Mov);
        // A def this code leaves out writes nothing here (where it is written, the code is leaving, and counts first).
        if (skipped)
            continue;
        if (e.wr.size() != 1 || (!neutral && !source))
            bad[r] = 1;
        else if (source)
            cand[r] = 1;
    }
    // What each source reads must itself be a parameter or such a register.
    for (bool changed = true; changed;) {
        changed = false;
        for (std::uint32_t j = 0; j < n_; j += length(j)) {
            const Ins& in = code_->code[j];
            const Op op = in.opcode();
            unsigned from = ~0u;
            if (op == Op::GetProp)
                from = in.b;
            else if (op == Op::Mov)
                from = in.b;
            else
                continue;
            if (in.a >= code_->nregs || !cand[in.a] || bad[in.a])
                continue;
            if (from >= first && (from >= code_->nregs || !cand[from] || bad[from])) {
                bad[in.a] = 1;
                changed = true;
            }
        }
    }
    for (unsigned r = first; r < code_->nregs; ++r)
        if (cand[r] && !bad[r])
            fnBorrow_.insert(r);
    gated_ = !fnBorrow_.empty() || !calleeReads_.empty();
#if defined(SCRIPT_JIT_ENV_OVERRIDE)
    if (std::getenv("T2HOIST"))
        for (unsigned r : fnBorrow_)
            std::fprintf(stderr, "fnborrow %s r%u\n", codeName(code_).c_str(), r);
#endif
}

void Baseline::planCalleeReads()
{
    std::vector<std::uint32_t> starts;
    for (std::uint32_t j = 0; j < n_; j += length(j))
        starts.push_back(j);
    auto touches = [&](std::uint32_t j, unsigned r) {
        const opt::Effects e = opt::effectsOf(*code_, j);
        for (std::uint16_t x : e.rd)
            if (x == r)
                return true;
        for (std::uint16_t x : e.wr)
            if (x == r)
                return true;
        return !e.ok;
    };
    for (std::size_t k = 0; k < starts.size(); ++k) {
        const std::uint32_t i = starts[k];
        const Ins& in = code_->code[i];
        if (in.opcode() != Op::New || !newInline_.count(i) || windowVal_.count(i) || newCallee_.count(i))
            continue;
        const unsigned base = in.b;
        if (!regDeadAfter(i, base))
            continue;
        // The read of the constructor: the last instruction before that touches its register.
        std::int64_t d = -1;
        for (std::size_t m = k; m-- > 0;)
            if (touches(starts[m], base)) {
                d = starts[m];
                break;
            }
        if (d < 0)
            continue;
        const Ins& g = code_->code[std::size_t(d)];
        std::uint8_t attrs = 0;
        if (g.opcode() != Op::GetGlobal || g.a != base || (std::size_t(d) < skipDef_.size() && skipDef_[std::size_t(d)]) ||
            plan_->defWeb[std::size_t(d)] >= 0 || !vm_.global->findOwn(vm_, code_->atoms[g.b], &attrs) ||
            (attrs & attr::kAccessor))
            continue;
        // Between them: the arguments, entered only at the read and left only at the `new`.
        bool closed = true;
        for (std::uint32_t j = 0; j < n_ && closed; j += length(j)) {
            const Op op = code_->code[j].opcode();
            const std::int64_t t = opt::jumpTarget(*code_, j);
            const bool inside = std::int64_t(j) > d && j < i;
            if (inside && (op == Op::Ret || op == Op::Throw || op == Op::ThrowError))
                closed = false;
            if (t < 0)
                continue;
            const bool toInside = t > d && t <= std::int64_t(i);
            if (inside != toInside && !(inside && t == std::int64_t(i)))
                closed = false;
            if (inside && !toInside)
                closed = false;
        }
        if (!closed)
            continue;
        newCallee_[i] = std::uint32_t(d);
        skipDef_[std::size_t(d)] = 1;
        newWinDefs_.insert(std::uint32_t(d));
        calleeReads_.emplace_back(std::uint32_t(d), i);
    }
}

void Baseline::planSelfChecked()
{
    selfChecked_.assign(n_ + 1, 0);
    newChecked_.assign(n_ + 1, 0);
    newInline_.clear();
    selfFn_ = JSValue::undefined();
    fnAtEntry_ = false;
    for (std::uint32_t j = 0; j < n_; j += length(j))
        if (code_->code[j].opcode() == Op::New && inlineNew(j, true))
            newInline_.insert(j);
    planNewChecked();
    if (selfGuards_.empty())
        return;
    selfFn_ = selfGuards_.begin()->second.fn;
    fnAtEntry_ = false;
    for (const auto& [g, sg] : selfGuards_)
        if (sg.fn != selfFn_)
            return;
    // No way in but the entry (no loop to enter part way): the frame's function is checked there, once.
    fnAtEntry_ = plan_->loopHeaders.empty();
    auto typedUses = [&](std::uint32_t j) {
        for (std::int32_t w : plan_->useWebs[j])
            if (w < 0)
                return false;
        return true;
    };
    // Instructions that cannot run script or change a global.
    auto quietHere = [&](std::uint32_t j) {
        if (j < skipDef_.size() && skipDef_[j])
            return true;
        const Op op = code_->code[j].opcode();
        switch (op) {
        case Op::Nop: case Op::Jmp: case Op::LoadUndef: case Op::LoadNull: case Op::LoadInt: case Op::LoadK:
        case Op::LoadTrue: case Op::LoadFalse: case Op::LoadHole: case Op::LoadUninit: case Op::Mov:
            return true;
        case Op::GetProp:
            return plan_->useWebs[j].size() >= 1 && plan_->useWebs[j][0] < 0 && plainReadable(j);
        case Op::New:
            return newInline_.count(j) != 0;  // made in place, or the long way to the constructor it checked
        default:
            break;
        }
        if (opt::isConditionalJump(op))
            return typedUses(j) || bitTestAt(j);
        switch (op) {
        case Op::Add: case Op::Sub: case Op::Mul: case Op::BitAnd: case Op::BitOr: case Op::BitXor: case Op::Shl:
        case Op::Shr: case Op::AddI: case Op::SubI: case Op::MulI: case Op::BitAndI: case Op::BitOrI: case Op::BitXorI:
        case Op::ShlI: case Op::ShrI: case Op::Inc: case Op::Dec:
            return plan_->defWeb[j] >= 0 && typedUses(j);
        default:
            return false;
        }
    };
    // A function whose code can change no global -- typed arithmetic, reads, jumps, and calls of itself, which by the same
    // token cannot either -- keeps its global as it was all through a call: a guard after one that looked, even across
    // calls of itself, need not look again. Its frame's function is then looked at by those first guards, not the entry.
    bool pure = true;
    for (std::uint32_t j = 0; j < n_ && pure; j += length(j)) {
        const Op op = code_->code[j].opcode();
        if (quietHere(j) || op == Op::Ret || (op == Op::JNotCallee && selfGuards_.count(j)) ||
            (op == Op::Call && (fallbackOf(j) || (directCalls_.count(j) && directCalls_.at(j).fn == selfFn_))))
            continue;
        std::uint8_t attrs = 0;
        pure = op == Op::GetGlobal && vm_.global->findOwn(vm_, code_->atoms[code_->code[j].b], &attrs) &&
               !(attrs & attr::kAccessor);
    }
    if (pure)
        fnAtEntry_ = false;
    // Forward, meeting by "and": 2 unreached, 1 checked, 0 not.
    std::vector<std::uint8_t> in(n_ + 1, 2);
    std::vector<std::uint32_t> work{0};
    in[0] = 0;
    auto flow = [&](std::uint32_t to, std::uint8_t v) {
        if (to >= n_)
            return;
        const std::uint8_t nv = in[to] == 2 ? v : std::uint8_t(in[to] & v);
        if (nv != in[to]) {
            in[to] = nv;
            work.push_back(to);
        }
    };
    while (!work.empty()) {
        const std::uint32_t j = work.back();
        work.pop_back();
        const Op op = code_->code[j].opcode();
        const bool guard = op == Op::JNotCallee && selfGuards_.count(j);
        const bool selfCall = pure && op == Op::Call && directCalls_.count(j) && directCalls_.at(j).fn == selfFn_;
        const std::uint8_t out = guard ? 1 : (quietHere(j) || selfCall) ? in[j] : 0;
        // The long way of a call run in place: this code leaves before it.
        if (op == Op::Call && fallbackOf(j))
            continue;
        const std::int64_t t = opt::jumpTarget(*code_, j);
        if (t >= 0 && !guard)
            flow(std::uint32_t(t), std::uint32_t(t) <= j ? 0 : out);
        if (op != Op::Jmp && op != Op::Ret && op != Op::Throw && op != Op::ThrowError)
            flow(j + length(j), out);
    }
    for (std::uint32_t j = 0; j < n_; ++j)
        selfChecked_[j] = in[j] == 1;
}

void Baseline::emitSelfGuard(std::uint32_t i)
{
    if (selfChecked_[i])
        return;  // a guard before it checked the same, and nothing since may have changed it
    const SelfGuard& sg = selfGuards_.at(i);
    const unsigned base = code_->code[i].a;
    Label& cold = coldLabels_.emplace_back();
    a_.cmpI32(mem(R12, kVmGlobalEpoch), std::int32_t(vm_.globalEpoch));
    a_.jcc(CNE, cold);
    globalSlot(sg.globalIndex);
    a_.movImm(RAX, sg.fn.bits);
    a_.cmp(RAX, mem(RDX, 0));
    a_.jcc(CNE, cold);
    if (!fnAtEntry_) {
        a_.cmp(RAX, mem(R13, kFrameFn));
        a_.jcc(CNE, cold);
    }
    // The long way, out of line, leaves this code: the global read as the skipped instruction would have, the `this` it
    // skipped, and the guard runs again in the interpreter (registers this code never writes again). It does not come
    // back, so a loop around it still knows what it hoisted.
    deferCold(i, [this, i, base, &cold]() {
        const SelfGuard& g = selfGuards_.at(i);
        a_.bind(cold);
        emit(g.read);
        if (g.thisAt >= 0) {
            const Ins& t = code_->code[std::size_t(g.thisAt)];
            a_.movImm(RAX, t.opcode() == Op::LoadNull ? kNull : kUndefined);
            write(base + 1, RAX);
        }
        a_.jmp(exitBefore(i));
    });
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
    sideDoor();
    const Ins& in = code_->code[i];
    const DirectCall& dc = directCalls_.at(i);
    CodeBlock* C = dc.code;
    const unsigned base = in.b, argc = in.c;
    materializeReads(i);
    savePool(i);
    Label& slowPath = coldLabels_.emplace_back();
    Label& done = coldLabels_.emplace_back();
    throws_.emplace_back(i, Label{});
    Label& exc = throws_.back().second;
    const bool borrowing = borrowInfo_ && C == code_;
    Label& retry = coldLabels_.emplace_back();
    Label& tickStub = coldLabels_.emplace_back();
    Label& fillStub = coldLabels_.emplace_back();
    a_.bind(retry);
    if (dc.byCode) {
        // The register holds a function of that code (compared by uid: a block that died leaves no stale match).
        a_.mov(RAX, R(base));
        needObject(RAX, slowPath);
        a_.cmpI8(mem(RAX, kHdrCls), std::uint8_t(ObjClass::Function));
        a_.jcc(CNE, slowPath);
        a_.mov(RCX, mem(RAX, kFnCode));
        a_.test(RCX, RCX);
        a_.jcc(CE, slowPath);
        a_.movImm(RDX, C->uid);
        a_.cmp(RDX, mem(RCX, kCbUid));
        a_.jcc(CNE, slowPath);
    } else if (selfChecked_[i] && dc.fn == selfFn_) {
        a_.movImm(RAX, dc.fn.bits);
    } else {
        a_.cmpI32(mem(R12, kVmGlobalEpoch), std::int32_t(vm_.globalEpoch));
        a_.jcc(CNE, slowPath);
        globalSlot(dc.globalIndex);
        a_.movImm(RAX, dc.fn.bits);
        a_.cmp(RAX, mem(RDX, 0));
        a_.jcc(CNE, slowPath);
    }
    // Its baseline code (once made, it stays while the block lives), room for its frame (stack, initialised stack, a
    // frame, native stack) and a tick. Code that holds registers without a count calls itself, while it is the entry.
    if (!dc.byCode)
        a_.movImm(RCX, reinterpret_cast<std::uint64_t>(C));
    if (borrowing) {
        a_.movImm(RDX, reinterpret_cast<std::uint64_t>(&borrowInfo_->retired));
        a_.cmpI8(mem(RDX, 0), 0);
        a_.jcc(CNE, slowPath);
    } else {
        a_.mov(RDX, mem(RCX, kCbTier1));
        if (!C->tier1) {
            a_.test(RDX, RDX);
            a_.jcc(CE, slowPath);
        }
    }
    a_.lea(R9, R(base + 1));
    // The frame and one past it filled: the stack is never filled closer than 8 to its end (`VM::fillStack`).
    a_.lea(R10, mem(R9, std::int32_t(8 * C->nregs + 8)));
    a_.alu(7, R10, mem(R12, kVmStackInit));
    a_.jcc(CA, borrowing ? fillStub : slowPath);
    // The new frame follows this one (the top one): its index from the address, so nothing waits on the count's last
    // store.
    a_.lea(R11, mem(R13, std::int32_t(sizeof(Frame))));
    a_.alu(7, R11, mem(R12, kVmFrameLimit));
    a_.jcc(CAE, slowPath);
    a_.alu(7, RSP, mem(R12, kVmStackFloor));
    a_.jcc(CB, slowPath);
    a_.cmpI32(mem(R12, kVmTicks), 1);
    // The interrupt check runs no script: code that holds registers without a count stays (the long way may not).
    a_.jcc(CBE, borrowing ? tickStub : slowPath);
    a_.dec32(mem(R12, kVmTicks));
    // Caller registers the callee's frame covers past the arguments: plain values are overwritten, a cell goes the long way.
    const unsigned window = code_->nregs > base + 1 ? code_->nregs - (base + 1) : 0;
    for (unsigned r = 1 + argc; r < window && r < C->nregs; ++r) {
        const unsigned reg = base + 1 + r;
        if (!dirtyIn_.empty() && !(dirtyIn_[i][reg / 64] >> (reg % 64) & 1))
            continue;  // no way here wrote it: undefined still
        if (!windowPlain(i, reg)) {
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
    a_.lea(RDI, mem(R11, std::int32_t(sizeof(Frame))));
    a_.mov(mem(R12, kVmFrameTop), RDI);
    a_.mov(mem(R11, kFrameCode), RCX);
    a_.mov(mem(R11, kFrameFn), RAX);
    a_.mov(mem(R11, kFramePc), 0);
    a_.mov(mem(R11, kFrameR), R9);
    a_.movImm(RDI, (std::uint64_t(argc) << 32) | (std::uint64_t(borrowing ? Frame::kArgsBorrowed : 0) << 16) | in.a);
    a_.mov(mem(R11, kFrameRetDst), RDI);
    a_.mov(mem(R11, kFrameEntry - 1), std::int32_t(0x100));
    // A callee that never reads `new.target` (nor calls `super`) and is no generator never looks at either.
    bool reads = (C->flags & (CodeBlock::kGenerator | CodeBlock::kAsync | CodeBlock::kAsyncGen)) != 0;
    for (const Ins& ci : C->code)
        reads = reads || ci.opcode() == Op::NewTarget || ci.opcode() == Op::SetNewTarget || ci.opcode() == Op::SuperCall;
    if (reads) {
        a_.mov(mem(R11, kFrameNewTarget), std::int32_t(kUndefined));
        a_.mov(mem(R11, kFrameGen), std::int32_t(kUndefined));
    }
    a_.mov(RSI, R11);
    a_.mov(RDI, R9);
    if (borrowing) {
        a_.call(selfEntry_);
    } else {
        a_.mov(RDX, mem(RDX, kTierEntry));
        a_.callReg(RDX);
    }
    a_.test(RAX, RAX);
    a_.jcc(CE, exc);
    write(in.a, RAX);
    restorePool(i);
    if (borrowing) {
        // Other code ran below: what this frame holds was counted, and the code leaves.
        a_.testI8(mem(R13, kFrameBorrow), Frame::kOwned);
        a_.jcc(CNE, exitOwnedAfter(i));
    }
    Label typedDone;
    const std::int32_t dw = plan_->defWeb[i];
    const bool typedFast = dw >= 0 && in.a < plainRegs_.size() && plainRegs_[in.a];
    if (typedFast) {
        // The result into its variable from rax (the write called nothing), as the frame has it.
        unboxRax(dw, exitAfter(i));
        dirty_[std::size_t(dw)] = 0;
        a_.jmp(typedDone);
    }
    // The long way, out of line: the registers written as the instructions would have (the global read now), then the
    // baseline call sequence.
    a_.bind(done);
    if (plan_->defWeb[i] >= 0)
        loadTypedAfter(i);
    a_.bind(typedDone);
    // `in` and `dc` go into the code emitted later by value: the instruction and the call's record as they are now.
    deferCold(i, [this, i, in, dc, base, borrowing, &slowPath, &done, &retry, &tickStub, &fillStub, &exc]() {
    if (borrowing) {
        // The stack made ready, which runs no script: the code stays.
        a_.bind(fillStub);
        a_.mov(RDI, R12);
        a_.mov(RSI, R10);
        a_.movImm(RAX, reinterpret_cast<std::uint64_t>(&scriptVmJitFillStack));
        a_.callC(RAX);
        a_.test32(RAX, RAX);
        a_.jcc(CE, slowPath);
        a_.jmp(retry);
        a_.bind(tickStub);
        a_.mov(RDI, R12);
        a_.call(tick_);
        a_.test32(RAX, RAX);
        a_.jcc(CNE, exc);
        a_.jmp(retry);
    }
    a_.bind(slowPath);
    restorePool(i);
    savePool(i);
    if (borrowing)
        borrowGate(true);  // the long way may run other code
    if (!dc.byCode) {
        a_.mov(RDI, R12);
        a_.movImm(RSI, dc.read);
        a_.movImm(RAX, reinterpret_cast<std::uint64_t>(&helperThunk<jitGetGlobal>));
        a_.callC(RAX);
        a_.aluI32(7, RAX, 1);
        a_.jcc(CNE, exception(i));
        restorePool(i);
    }
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
    if (borrowing) {
        restorePool(i);
        a_.jmp(exitOwnedAfter(i));
    } else {
        a_.jmp(done);
    }
    });
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
        if (in.b == in.c)
            return false;
        if (ep == elemprof::kArray)
            return true;
        // A typed array's element, read straight into its destination: only the end of a chain.
        ElemKind kind;
        return asConsumer && uw[1] >= 0 && typedSiteKind(i, false, &kind);
    }
    case Op::PutElem: {
        // A number stored into the one kind of typed array the site has seen, at an int32 index: the end of a chain.
        const auto& uw = plan_->useWebs[i];
        ElemKind kind;
        return asConsumer && uw.size() >= 3 && uw[0] < 0 && uw[1] >= 0 && isI32(uw[1]) && uw[2] >= 0 && in.a != in.b &&
               in.a != in.c && typedSiteKind(i, true, &kind);
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
        // A read or definition a call window took writes its value elsewhere (or not at all): no link of a chain.
        if (plan_->defWeb[i] >= 0 || !chainable(i, false) || windowRead_.count(i) || (i < skipDef_.size() && skipDef_[i]))
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
        // The register a link reads its object from: a store's is its first operand.
        const bool store = cop == Op::PutElem;
        if ((cop != Op::GetProp && cop != Op::GetElem && !store) || (store ? cn.a : cn.b) != r || plan_->isLeader[c] ||
            !chainable(c, true) || windowRead_.count(c) || (c < skipDef_.size() && skipDef_[c]))
            continue;
        if (cop == Op::GetElem && cn.c == r)
            continue;
        if (!((!store && cn.a == r) || deadAfter(m, r)))
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
        if (role & 2) {
            // The link before left its value in rax when nothing has been emitted since and nothing jumps in here.
            if (!(chainLeft_ == a_.here() && !plan_->isLeader[i])) {
                if (chainFrom_ < 0)
                    a_.int3();  // the link before kept its value only in rax (never so: it comes right after)
                else
                    a_.mov(RAX, mem(RSP, chainFrom_));
            }
        } else {
            a_.mov(RAX, R(reg));
        }
    };
    switch (in.opcode()) {
    case Op::GetGlobal: {
        if (const LoopGlobal* lg = hoistedGlobal(i)) {
            a_.mov(RAX, mem(RSP, lg->slot));  // read before the loop, which does not change it
            if (role & 1) {
                chainFrom_ = lg->slot;  // the next link reads it from there if it must
                chainLeft_ = a_.here();
                return;
            }
            break;
        }
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
    case Op::PutElem: {
        // The end of a chain: a number into a typed array that only the chain's read holds.
        const auto& uw = plan_->useWebs[i];
        ElemKind kind = ElemKind::Count;
        typedSiteKind(i, true, &kind);
        receiver(in.a);
        needObject(RAX, bad);
        a_.mov32(RCX, useI32(uw[1], RCX));
        typedAccess(i, in, kind, true, uw[2], bad, std::uint32_t(chainHead_[i] >= 0 ? chainHead_[i] : std::int32_t(i)));
        return;
    }
    case Op::GetElem: {
        const auto& uw = plan_->useWebs[i];
        receiver(in.b);
        ElemKind typedKind = ElemKind::Count;
        const std::uint8_t ep = i < code_->elemProf.size() ? code_->elemProf[i] : 0;
        if ((role & 2) && ep != elemprof::kArray && typedSiteKind(i, false, &typedKind)) {
            needObject(RAX, bad);
            a_.mov32(RCX, useI32(uw[1], RCX));
            typedAccess(i, in, typedKind, false, -1, bad, std::uint32_t(chainHead_[i]));
            return;
        }
        bool known = false;
        for (const LoopGlobal& lg : loopGlobals_)
            known = known || (lg.array && lg.taker == i && lg.from <= factAt_ && factAt_ <= lg.to);
        if (known) {
            // A global checked to be an array before the loop.
        } else if (role & 2) {
            needObject(RAX, bad);
            a_.cmpI8(mem(RAX, kHdrCls), std::uint8_t(ObjClass::Array));
            a_.jcc(CNE, bad);
        } else {
            provenArray(in.b, 1, bad);
        }
        if (uw[1] >= 0) {
            a_.mov32(RCX, useI32(uw[1], RCX));
        } else {
            a_.mov(RCX, R(in.c));
            needInt(RCX, bad);
            a_.mov32(RCX, RCX);
        }
        // Unsigned: a negative index is past any length (an array has fewer than 2^31 elements).
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
        PolySeen seen[5];
        int count = 0;
        seen[count++] = PolySeen{ic.shape, ic.index, ic.holder, ic.validity, ic.aux};
        if (ic.pad && ic.pad <= code_->poly.size()) {
            const PolyCache& pc = code_->poly[ic.pad - 1];
            for (std::uint8_t k = 0; k < pc.n; ++k)
                seen[count++] = PolySeen{pc.e[k].shape, pc.e[k].index, pc.e[k].holder, pc.e[k].validity, pc.e[k].aux};
        }
        receiver(in.b);
        polyRead(seen, count, bad);
        break;
    }
    default:
        break;
    }
    if (role & 1) {
        // Kept for the next link unless that comes right after (nothing between can take rax).
        const std::uint32_t nx = i + length(i);
        if (nx < n_ && (chainRole_[nx] & 2) && !plan_->isLeader[nx]) {
            chainFrom_ = -1;
        } else {
            a_.mov(mem(RSP, chainSlot_), RAX);
            chainFrom_ = chainSlot_;
        }
        chainLeft_ = a_.here();
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

bool Baseline::inlineNew(std::uint32_t i, bool decideOnly)
{
    const Ins& in = code_->code[i];
    const unsigned base = in.b, argc = in.c;
    const auto& uw = plan_->useWebs[i];
    if (inlineBase_ < 0 || uw.size() < 2u + argc || uw[0] >= 0 || (i < code_->prof.size() && (code_->prof[i] & opt::kProfFailed)))
        return false;
    // The callee is compared with what is known of it below: what the global read before held is guess enough.
    JSValue known = constantBefore(i, base);
    if (!isFunction(known))
        known = guessBefore(i, base);
    if (!isFunction(known))
        return false;
    auto* f = static_cast<JSFunction*>(asObject(known));
    if ((f->flags & JSFunction::kNative) || !vm_.prototypeAt2(f))
        return false;
    CodeBlock* C = f->code;
    if (!C || !C->constructor || (C->flags & (CodeBlock::kLazy | CodeBlock::kGenerator | CodeBlock::kAsync | CodeBlock::kAsyncGen |
                                              CodeBlock::kNeedsArgs | CodeBlock::kDerived)))
        return false;
    // A base class's constructor as a function's: when the class has no field initialiser (fixed when the class is made),
    // its `FieldInit` reads undefined and the call of it is jumped over.
    if ((C->flags & CodeBlock::kClassConstructor) && f->rare() && isFunction(f->rare()->fieldInit))
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
    const std::uint8_t aux = newSlotsOf(C, vm_.rootShapeFor(proto));
    std::uint32_t shape = vm_.rootShapeFor(proto)->id;
    std::vector<std::int8_t> constReg(C->nregs, 0);
    std::vector<JSValue> constVal(C->nregs, JSValue::undefined());
    bool returned = false;
    std::int64_t jumpTo = -1;
    for (std::uint32_t j = 0; j < C->code.size() && !returned;
         j = jumpTo >= 0 ? std::uint32_t(std::exchange(jumpTo, -1)) : opt::nextOf(*C, j)) {
        const Ins& ci = C->code[j];
        switch (ci.opcode()) {
        case Op::FieldInit:
            if (!(C->flags & CodeBlock::kClassConstructor) || ci.a == 0 || ci.a <= C->nparams)
                return false;
            constReg[ci.a] = 1;
            constVal[ci.a] = JSValue::undefined();
            break;
        case Op::JUndefined: {
            const std::int64_t t = opt::jumpTarget(*C, j);
            if (ci.a >= C->nregs || !constReg[ci.a] || !constVal[ci.a].isUndefined() || t <= std::int64_t(j))
                return false;
            jumpTo = t;
            break;
        }
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
        case Op::LoadNull: case Op::LoadTrue: case Op::LoadFalse:
            if (ci.a == 0 || ci.a <= C->nparams)
                return false;
            constReg[ci.a] = 1;
            constVal[ci.a] = ci.opcode() == Op::LoadNull ? JSValue::null() : JSValue::boolean(ci.opcode() == Op::LoadTrue);
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
    if (decideOnly)
        return true;
    // The window's registers unwritten (`planWindows`): a failed guard runs them again in the interpreter.
    const bool windowed = windowVal_.count(i) != 0;
    Label& slowPath = coldLabels_.emplace_back();
    Label& done = coldLabels_.emplace_back();
    // The callee, its prototype and the chain the stores were learned on are as they were; otherwise the code leaves (so
    // that what comes after has them as they were). An earlier `new` of it with nothing between that could change them
    // checked the same (`newChecked_`).
    holdValue(known);
    holdValue(pv);
    if (!(i < newChecked_.size() && newChecked_[i])) {
        Label& bad = exitBefore(i);
        if (auto nc = newCallee_.find(i); nc != newCallee_.end()) {
            emitReadInto(nc->second, false, bad);
            a_.movImm(RCX, known.bits);
            a_.cmp(RCX, RAX);
            a_.jcc(CNE, bad);
        } else {
            a_.movImm(RCX, known.bits);
            a_.cmp(RCX, R(base));
            a_.jcc(CNE, bad);
            a_.mov(RAX, R(base));
        }
        a_.cmpI32(mem(RAX, 0), std::int32_t(f->h.shape));
        a_.jcc(CNE, bad);
        a_.mov(RDX, mem(RAX, kObjSlots));
        a_.movImm(RCX, pv.bits);
        a_.cmp(RCX, mem(RDX, 16));
        a_.jcc(CNE, bad);
        std::sort(cells.begin(), cells.end());
        cells.erase(std::unique(cells.begin(), cells.end()), cells.end());
        for (ValidityCell* c : cells) {
            hold(c);
            a_.movImm(RCX, reinterpret_cast<std::uint64_t>(c));
            a_.cmpI32(mem(RCX, kValidityValid), 0);
            a_.jcc(CE, bad);
        }
    } else {
        for (ValidityCell* c : cells)
            hold(c);
    }
    // No room without a collection: the call the long way (to the constructor just checked).
    allocObject(shape, aux, slowPath);
    // Constants that are no cell go straight into the object; the rest through rax, the object kept on the stack.
    std::vector<std::uint8_t> inPlace(stores.size(), 0);
    bool rest = false;
    for (std::size_t k = 0; k < stores.size(); ++k) {
        const IVal& v = stores[k].value;
        const std::int64_t bits = std::int64_t(v.c.bits);
        if (v.kind == IVal::Const && !v.c.isCell() && bits >= INT32_MIN && bits <= INT32_MAX) {
            a_.mov(mem(RAX, kObjInline + std::int32_t(8 * stores[k].index)), std::int32_t(bits));
            inPlace[k] = 1;
        } else {
            rest = true;
        }
    }
    if (rest)
        a_.mov(inlineSlot(0), RAX);
    std::set<int> movedFrom;
    for (std::size_t k = 0; k < stores.size(); ++k) {
        if (inPlace[k])
            continue;
        const IVal& v = stores[k].value;
        int from = -1;
        if (v.kind == IVal::Box && v.frameReg > int(code_->nparams) && v.slot > -2 && !movedFrom.count(v.frameReg) &&
            !fnBorrowed(unsigned(v.frameReg)) &&
            regDeadAfter(i, unsigned(v.frameReg))) {
            // The register dies here: the object takes its value with its count, and the register holds undefined.
            bool twice = false;
            for (std::size_t m = k + 1; m < stores.size(); ++m)
                twice = twice || (stores[m].value.kind == IVal::Box && stores[m].value.frameReg == v.frameReg);
            if (!twice)
                from = v.frameReg;
        }
        if (from >= 0) {
            a_.mov(RAX, R(unsigned(from)));
            a_.mov(R(unsigned(from)), std::int32_t(kUndefined));
            movedFrom.insert(from);
        } else {
            inlineBoxed(v);
            if (v.kind == IVal::Box || (v.kind == IVal::Const && v.c.isCell()))
                retain(RAX);
        }
        a_.mov(RCX, inlineSlot(0));
        a_.mov(mem(RCX, kObjInline + std::int32_t(8 * stores[k].index)), RAX);
    }
    // A clear of a register the object took the value of has nothing left to do.
    for (int r : movedFrom) {
        for (std::uint32_t j = i + length(i); j < n_ && !plan_->isLeader[j]; j += length(j)) {
            const Ins& x = code_->code[j];
            if (x.opcode() == Op::LoadUndef && x.a == unsigned(r)) {
                movedOut_.insert(j);
                break;
            }
            const opt::Effects e = opt::effectsOf(*code_, j);
            bool touches = !e.ok || opt::jumpTarget(*code_, j) >= 0;
            for (std::uint16_t q : e.rd)
                touches = touches || q == unsigned(r);
            for (std::uint16_t q : e.wr)
                touches = touches || q == unsigned(r);
            if (touches)
                break;
        }
    }
    if (rest)
        a_.mov(RAX, inlineSlot(0));
    if (plainAt(i, in.a))
        a_.mov(R(in.a), RAX);  // over no cell
    else
        write(in.a, RAX);
    a_.bind(done);
    if (plan_->defWeb[i] >= 0)
        loadTypedAfter(i);
    deferCold(i, [this, i, in, windowed, base, &slowPath, &done]() {
    a_.bind(slowPath);
    const bool calleeRead = newCallee_.count(i) != 0;
    if (windowed || calleeRead) {
        // The registers as the instructions would have written them (the global read now), then the call the long way.
        sideDoor();
        if (auto nc = newCallee_.find(i); nc != newCallee_.end()) {
            // The window is filled after this call from variables that may end at this instruction: all are kept.
            savePool(i, true);
            a_.mov(RDI, R12);
            a_.movImm(RSI, nc->second);
            a_.movImm(RAX, reinterpret_cast<std::uint64_t>(&helperThunk<jitGetGlobal>));
            a_.callC(RAX);
            a_.aluI32(7, RAX, 1);
            a_.jcc(CNE, exception(i));
            restorePool(i, true);
        }
        for (const auto& [k, v] : windowed ? windowVal_.at(i) : std::map<std::size_t, IVal>{}) {
            if (k == 0)
                continue;
            inlineBoxed(v);
            if (v.kind == IVal::Box || (v.kind == IVal::Const && v.c.isCell()))
                retain(RAX);
            write(base + unsigned(k), RAX);
        }
    }
    materializeReads(i);
    call(i, in);
    if (windowed || calleeRead) {
        // The registers written above are dead: as the way in place leaves them (the frame's way out takes them as such).
        std::set<unsigned> regs;
        if (calleeRead)
            regs.insert(base);
        if (windowed)
            for (const auto& [k, v] : windowVal_.at(i))
                if (k != 0)
                    regs.insert(base + unsigned(k));
        for (unsigned r : regs) {
            a_.mov(RDX, R(r));
            a_.mov(R(r), std::int32_t(kUndefined));
            release(RDX);
        }
    }
    a_.jmp(done);  // where the typed result, if any, is loaded as on the way in place
    });
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
    newCallee_.clear();
    newWinDefs_.clear();
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
        const bool isNew = in.opcode() == Op::New;
        if ((in.opcode() != Op::Call && !isNew) || directCalls_.count(i) ||
            (i < code_->prof.size() && (code_->prof[i] & opt::kProfFailed)))
            continue;
        const unsigned base = in.b, argc = in.c;
        const auto& uw = plan_->useWebs[i];
        if (uw.size() < 2u + argc)
            continue;
        std::int64_t calleeRead = -1;
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
            case Op::GetGlobal: {
                // The constructor of a `new`: the code reads the global itself, with the epoch, and compares.
                if (!isNew || op != 0 || !chainable(std::uint32_t(d), false))
                    continue;
                const JSValue known = constantBefore(i, base);
                if (!isFunction(known))
                    continue;
                v.c = known;
                calleeRead = d;
                break;
            }
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
        if (calleeRead >= 0)
            newCallee_[i] = std::uint32_t(calleeRead);
        const bool ok = isNew ? inlineNew(i, true) : inlineCall(i, true);
#if defined(SCRIPT_JIT_ENV_OVERRIDE)
        if (std::getenv("T2WIN"))
            std::fprintf(stderr, "window %s @%u from %u: %zu values, %s\n", codeName(code_).c_str(), i, first, vals.size(), ok ? "yes" : "no");
#endif
        if (!ok) {
            windowVal_.erase(i);
            newCallee_.erase(i);
            continue;
        }
        for (std::uint32_t d : defs)
            if (!reads_.count(d)) {
                skipDef_[d] = 1;
                if (isNew)
                    newWinDefs_.insert(d);
            }
        for (auto& [d, sr] : reads_)
            windowRead_[d] = sr;
        for (std::uint32_t j = first + length(first); j <= i; j += length(j))
            exitRedirect_[j] = std::int32_t(first);
    }
}

/// The function the method read of call `i` into window `slot` finds, when every way it has read it is from one prototype slot that now holds a
/// function of `C`: the read checks the chain, and the call need only check it is that function (undefined: none).
JSValue Baseline::windowMethod(std::uint32_t i, std::int32_t slot, const CodeBlock* C, std::int64_t* read) const
{
    // The call's own reads come just before it (nothing else is between them), so the last one before it with that slot.
    for (auto it = windowRead_.lower_bound(i); it != windowRead_.begin();) {
        --it;
        const auto& [d, sr] = *it;
        if (sr.first != slot)
            continue;
        const InlineCache& ic = code_->caches[code_->code[d + 1].a];
        const JSObject* holder = nullptr;
        std::uint32_t index = 0;
        bool ok = true;
        auto take = [&](const JSObject* h, std::uint32_t x, const ValidityCell* v) {
            if (!h || !v || !v->valid || (holder && (holder != h || index != x)))
                ok = false;
            holder = h;
            index = x;
        };
        if (ic.shape != 0)
            take(ic.holder, ic.index, ic.validity);
        if (ic.pad && ic.pad <= code_->poly.size()) {
            const PolyCache& pc = code_->poly[ic.pad - 1];
            for (std::uint8_t k = 0; k < pc.n; ++k)
                take(pc.e[k].kind == PolyEntry::ProtoData ? pc.e[k].holder : nullptr, pc.e[k].index, pc.e[k].validity);
        }
        if (!ok || !holder)
            return JSValue::undefined();
        const JSValue v = *const_cast<JSObject*>(holder)->slotAt(index);
        if (!isFunction(v))
            return JSValue::undefined();
        const auto* f = static_cast<const JSFunction*>(asObject(v));
        if ((f->flags & JSFunction::kNative) || f->code != C)
            return JSValue::undefined();
        if (read)
            *read = std::int64_t(d);
        return v;
    }
    return JSValue::undefined();
}

void Baseline::emitWindowRead(std::uint32_t i)
{
    // A method read for an inlined call: into its slot, from the receiver the planner chose.
    const auto& [slot, recv] = windowRead_.at(i);
    if (const LoopWin* lw = hoistedWin(i)) {
        a_.movImm(RAX, lw->known.bits);  // read and checked before the loop
        a_.mov(mem(RSP, windowBase_ + 8 * slot), RAX);
        return;
    }
    const Ins& in = code_->code[i];
    const InlineCache& ic = code_->caches[code_->code[i + 1].a];
    Label& bad = exitBefore(i);
    PolySeen seen[5];
    int count = 0;
    seen[count++] = PolySeen{ic.shape, ic.index, ic.holder, ic.validity, ic.aux};
    if (ic.pad && ic.pad <= code_->poly.size()) {
        const PolyCache& pc = code_->poly[ic.pad - 1];
        for (std::uint8_t k = 0; k < pc.n; ++k)
            seen[count++] = PolySeen{pc.e[k].shape, pc.e[k].index, pc.e[k].holder, pc.e[k].validity, pc.e[k].aux};
    }
    (void)in;
    a_.mov(RAX, R(recv));
    polyRead(seen, count, bad, int(recv));
    a_.mov(mem(RSP, windowBase_ + 8 * slot), RAX);
}

// ---- facts of a whole loop ----
//
// A frame register no instruction of a loop writes holds the same value all through it. When the loop calls (inlined) the
// function in it, that the register holds a function of the code compiled in is checked once, before the loop, and taken
// as known inside it. The loop must be entered only by falling into its header (or by OSR, which goes through the checks).

void Baseline::planLoopFacts()
{
    loopFacts_.clear();
    loopShapes_.clear();
    loopValids_.clear();
    loopNums_.clear();
    loopArrs_.clear();
    loopGlobals_.clear();
    loopMaths_.clear();
    loopReads_.clear();
    keptLoopRegs_ = 0;
    loopPromos_.clear();
    intPromos = doublePromos = 0;
    promoAccess_.clear();
    promoGuards_.clear();
    hoistSlots_ = 0;
    loopWins_.clear();
    hoistWatch_.clear();
    preLoop_.clear();
    if (!code_->handlers.empty())
        return;
    std::vector<std::int64_t> prev(n_ + 1, -1);
    for (std::uint32_t j = 0; j < n_; j += length(j))
        prev[j + length(j)] = j;
    for (std::uint32_t h : plan_->loopHeaders) {
        if (h < code_->prof.size() && (code_->prof[h] & opt::kProfFailed))
            continue;
        std::int64_t end = -1;
        for (std::uint32_t j = h; j < n_; j += length(j))
            if (opt::jumpTarget(*code_, j) == std::int64_t(h))
                end = j;
        if (end < 0)
            continue;
        bool enteredElsewhere = false;
        for (std::uint32_t j = 0; j < n_ && !enteredElsewhere; j += length(j)) {
            const std::int64_t t = opt::jumpTarget(*code_, j);
            const bool inside = j >= h && std::int64_t(j) <= end;
            if (!inside && t >= std::int64_t(h) && t <= end)
                enteredElsewhere = true;
        }
        if (enteredElsewhere)
            continue;
        if (h > 0) {
            const std::int64_t p = prev[h];
            if (p < 0)
                continue;
            const Op op = code_->code[std::size_t(p)].opcode();
            if (op == Op::Jmp || op == Op::Ret || op == Op::Throw || op == Op::ThrowError)
                continue;
        }
        // What the loop writes; a call, everything from its window up (the callee's frame).
        std::vector<char> written(code_->nregs + 1, 0);
        for (std::uint32_t j = h; std::int64_t(j) <= end; j += length(j)) {
            for (std::uint16_t r : opt::effectsOf(*code_, j).wr)
                if (r < written.size())
                    written[r] = 1;
            const Op op = code_->code[j].opcode();
            if (op == Op::Call || op == Op::New)
                for (unsigned r = code_->code[j].b; r < written.size(); ++r)
                    written[r] = 1;
        }
        std::map<unsigned, const CodeBlock*> want;
        std::set<unsigned> mixed;
        for (std::uint32_t j = h; std::int64_t(j) <= end && inlineBase_ >= 0; j += length(j)) {
            const Ins& in = code_->code[j];
            if (in.opcode() != Op::Call || (j < code_->prof.size() && (code_->prof[j] & opt::kProfFailed)))
                continue;
            if (j >= code_->callProf.size() || !code_->callProf[j] || isFunction(constantBefore(j, in.b)))
                continue;
            CodeBlock* C = vm_.codeByUid(code_->callProf[j]);
            if (!inlinable(C))
                continue;
            const IVal callee = callerValue(j, in.b, 0, 8);
            if (callee.kind != IVal::Box || callee.frameReg < 0 || std::size_t(callee.frameReg) >= written.size() ||
                written[std::size_t(callee.frameReg)])
                continue;
            const auto [it, fresh] = want.emplace(unsigned(callee.frameReg), C);
            if (!fresh && it->second != C)
                mixed.insert(unsigned(callee.frameReg));
        }
        for (const auto& [r, C] : want)
            if (!mixed.count(r)) {
                loopFacts_.push_back(LoopFact{h, std::uint32_t(end), r, C});
                preLoop_[h];
            }
        if (noHoist_.count(h))
            continue;
        // The shapes of objects in registers the loop does not write, as the reads and stores of its own code have seen
        // them, and the chains of its reads from prototypes.
        std::map<unsigned, std::pair<std::uint32_t, std::int32_t>> shapes;
        std::set<unsigned> mixedShape;
        std::set<ValidityCell*> cells;
        auto wantShape = [&](unsigned r, std::uint32_t shape, std::int32_t aux) {
            if (r >= written.size() || written[r] || shape == 0)
                return;
            const auto [it, fresh] = shapes.emplace(r, std::make_pair(shape, aux));
            if (fresh)
                return;
            if (it->second.first != shape)
                mixedShape.insert(r);
            else if (it->second.second != aux)
                it->second.second = -1;
        };
        for (std::uint32_t j = h; std::int64_t(j) <= end; j += length(j)) {
            const Ins& in = code_->code[j];
            const Op op = in.opcode();
            if (op != Op::GetProp && op != Op::PutProp)
                continue;
            if (j + 1 >= n_ || code_->code[j + 1].opcode() != Op::Cache || code_->code[j + 1].a == kNoCache)
                continue;
            const InlineCache& ic = code_->caches[code_->code[j + 1].a];
            if (ic.shape == 0 || (ic.pad && ic.pad <= code_->poly.size() && code_->poly[ic.pad - 1].n))
                continue;
            const bool failed = j < code_->prof.size() && (code_->prof[j] & opt::kProfFailed);
            const std::int32_t aux = failed || !ic.aux || ic.holder ? -1 : std::int32_t(ic.aux) - 1;
            if (op == Op::PutProp) {
                if (ic.next == 0 && !ic.holder)
                    wantShape(in.a, ic.shape, aux);
                continue;
            }
            unsigned recv = in.b;
            if (auto wr = windowRead_.find(j); wr != windowRead_.end())
                recv = wr->second.second;
            else if (j < chainRole_.size() && (chainRole_[j] & 2))
                continue;
            if (ic.holder && !(ic.validity && ic.validity->valid))
                continue;
            wantShape(recv, ic.shape, aux);
            if (ic.holder)
                cells.insert(ic.validity);
        }
        bool any = false;
        for (const auto& [r, sa] : shapes)
            if (!mixedShape.count(r)) {
                loopShapes_.push_back(LoopShape{h, std::uint32_t(end), r, sa.first, sa.second});
                any = true;
            }
        // Of those objects, the own slots the loop reads or stores numbers in (typed, so each store is a number): checked
        // to hold a number before the loop, they hold one all through it.
        std::set<std::pair<unsigned, std::uint32_t>> nums, readNotInt;
        for (std::uint32_t j = h; std::int64_t(j) <= end; j += length(j)) {
            const Ins& in = code_->code[j];
            const Op op = in.opcode();
            if ((op != Op::GetProp && op != Op::PutProp) || j + 1 >= n_ || code_->code[j + 1].a == kNoCache)
                continue;
            const auto& uw = plan_->useWebs[j];
            const bool typed = op == Op::PutProp ? uw.size() >= 2 && uw[0] < 0 && uw[1] >= 0 : plan_->defWeb[j] >= 0;
            const unsigned r = op == Op::PutProp ? in.a : in.b;
            const InlineCache& ic = code_->caches[code_->code[j + 1].a];
            if (!typed || ic.holder || !shapes.count(r) || mixedShape.count(r) || shapes[r].second < 0 ||
                shapes[r].first != ic.shape)
                continue;
            nums.insert({r, ic.index});
            const bool int32 = op == Op::PutProp ? isI32(uw[1]) : isI32(plan_->defWeb[j]);
            if (!int32)
                readNotInt.insert({r, ic.index});
        }
        // Int32 all through when every typed store of the loop to that index stores one (any object: it may be the same).
        std::set<std::uint32_t> notInt;
        for (std::uint32_t j = h; std::int64_t(j) <= end; j += length(j)) {
            const Ins& in = code_->code[j];
            if (in.opcode() != Op::PutProp || j + 1 >= n_ || code_->code[j + 1].a == kNoCache)
                continue;
            const auto& uw = plan_->useWebs[j];
            if (uw.size() < 2 || uw[1] < 0 || !isI32(uw[1])) {
                const InlineCache& ic = code_->caches[code_->code[j + 1].a];
                notInt.insert(ic.index);
                if (ic.pad && ic.pad <= code_->poly.size())
                    for (std::uint8_t k = 0; k < code_->poly[ic.pad - 1].n; ++k)
                        notInt.insert(code_->poly[ic.pad - 1].e[k].index);
            }
        }
        for (const auto& [r, k] : nums)
            loopNums_.push_back(LoopNum{h, std::uint32_t(end), r, k, !notInt.count(k) && !readNotInt.count({r, k})});
        // Number reads of those objects' own slots that no store of the loop writes (to an object of that shape, which may
        // be the same one): read once before it.
        std::set<std::pair<std::uint32_t, std::uint32_t>> stored;
        for (std::uint32_t j = h; std::int64_t(j) <= end; j += length(j))
            if (code_->code[j].opcode() == Op::PutProp && j + 1 < n_ && code_->code[j + 1].a != kNoCache) {
                const InlineCache& ic = code_->caches[code_->code[j + 1].a];
                stored.insert({ic.shape, ic.index});
                if (ic.next)
                    stored.insert({ic.next, ic.index});  // an add: the slot of the shape after it
                if (ic.pad && ic.pad <= code_->poly.size())
                    for (std::uint8_t k = 0; k < code_->poly[ic.pad - 1].n; ++k) {
                        const PolyEntry& e = code_->poly[ic.pad - 1].e[k];
                        stored.insert({e.shape, e.index});
                        if (e.kind == PolyEntry::Add)
                            stored.insert({e.next, e.index});
                    }
            }
        for (std::uint32_t j = h; std::int64_t(j) <= end; j += length(j)) {
            const Ins& in = code_->code[j];
            if (in.opcode() != Op::GetProp || j + 1 >= n_ || code_->code[j + 1].a == kNoCache || plan_->defWeb[j] < 0 ||
                (j < chainRole_.size() && chainRole_[j]) || windowRead_.count(j) ||
                hoistSlots_ >= kHoistSlots)
                continue;
            const InlineCache& ic = code_->caches[code_->code[j + 1].a];
            const unsigned r = in.b;
            if (ic.holder || (ic.pad && ic.pad <= code_->poly.size() && code_->poly[ic.pad - 1].n) || !shapes.count(r) ||
                mixedShape.count(r) || shapes[r].first != ic.shape || shapes[r].second < 0 || stored.count({ic.shape, ic.index}))
                continue;
            // The same slot read again (as the same kind) shares the first read's.
            const LoopRead* same = nullptr;
            for (const LoopRead& lr : loopReads_)
                if (lr.from == h && !lr.dup && code_->code[lr.read].b == r &&
                    code_->caches[code_->code[lr.read + 1].a].index == ic.index &&
                    isI32(plan_->defWeb[lr.read]) == isI32(plan_->defWeb[j]))
                    same = &lr;
            if (same) {
                loopReads_.push_back(LoopRead{h, std::uint32_t(end), j, same->slot, true});
                loopReads_.back().xreg = same->xreg;
            } else {
                loopReads_.push_back(LoopRead{h, std::uint32_t(end), j, hoistBase_ + std::int32_t(8 * hoistSlots_++)});
                if (!isI32(plan_->defWeb[j]) && keptLoopRegs_ < kKeptForLoops)
                    loopReads_.back().xreg = kKeptFirst + keptLoopRegs_++;
            }
            any = true;
        }
        // Number slots of those objects that the loop both reads and stores through the register: kept as a double in a
        // stack slot all through the loop, written back on every way out. Another register that may hold the same
        // object is compared with it where it gets its value (and the code leaves when it is the same).
        if (opt::isConditionalJump(code_->code[std::size_t(end)].opcode())) {
            bool leaves = false;
            for (std::uint32_t j = h; std::int64_t(j) <= end; j += length(j)) {
                const std::int64_t t = opt::jumpTarget(*code_, j);
                const Op op = code_->code[j].opcode();
                if ((t >= 0 && (t < std::int64_t(h) || t > end)) || op == Op::Ret || op == Op::Throw || op == Op::ThrowError)
                    leaves = true;
            }
            std::uint32_t before = 0;
            for (std::uint32_t j = 0; j < h; j += length(j))
                before = j;
            for (const auto& [r, sa] : shapes) {
                if (leaves || mixedShape.count(r) || sa.second < 0 || h == 0)
                    break;
                const std::uint32_t shape = sa.first;
                const std::int32_t aux = sa.second;
                std::set<std::uint32_t> idxs;
                for (std::uint32_t j = h; std::int64_t(j) <= end; j += length(j)) {
                    const Ins& in = code_->code[j];
                    if (in.opcode() != Op::PutProp || in.a != r || code_->code[j + 1].a == kNoCache)
                        continue;
                    const InlineCache& ic = code_->caches[code_->code[j + 1].a];
                    if (ic.shape == shape && !ic.holder && ic.next == 0 && std::int32_t(ic.index) < aux)
                        idxs.insert(ic.index);
                }
                for (std::uint32_t idx : idxs) {
                    bool ok = hoistSlots_ < kHoistSlots;
                    bool ints = false, doubles = false;  // what the loop's variables for this slot are
                    std::vector<std::uint32_t> own;
                    std::set<unsigned> others;
                    for (std::uint32_t j = h; std::int64_t(j) <= end && ok; j += length(j)) {
                        const Ins& in = code_->code[j];
                        const Op op = in.opcode();
                        if ((op != Op::GetProp && op != Op::PutProp) || code_->code[j + 1].a == kNoCache)
                            continue;
                        const InlineCache& ic = code_->caches[code_->code[j + 1].a];
                        bool touches = (ic.shape == shape || (ic.next && ic.next == shape)) && ic.index == idx && !ic.holder;
                        const bool poly = ic.pad && ic.pad <= code_->poly.size() && code_->poly[ic.pad - 1].n;
                        if (poly)
                            for (std::uint8_t k = 0; k < code_->poly[ic.pad - 1].n; ++k) {
                                const PolyEntry& e = code_->poly[ic.pad - 1].e[k];
                                touches = touches || ((e.shape == shape || (e.kind == PolyEntry::Add && e.next == shape)) &&
                                                      e.index == idx);
                            }
                        if (!touches)
                            continue;
                        const unsigned recv = op == Op::GetProp ? in.b : in.a;
                        if (recv != r) {
                            others.insert(recv);
                            continue;
                        }
                        const auto& uw = plan_->useWebs[j];
                        const bool failed = j < code_->prof.size() && (code_->prof[j] & opt::kProfFailed);
                        if (poly || ic.holder || failed || (j < chainRole_.size() && chainRole_[j]) || windowRead_.count(j) ||
                            (j < skipDef_.size() && skipDef_[j]))
                            ok = false;
                        else if (op == Op::GetProp) {
                            ok = plan_->defWeb[j] >= 0 && in.a != r;
                            if (ok)
                                (isI32(plan_->defWeb[j]) ? ints : doubles) = true;
                        } else {
                            ok = uw.size() >= 2 && uw[0] < 0 && uw[1] >= 0 && ic.next == 0;
                            if (ok)
                                (isI32(uw[1]) ? ints : doubles) = true;
                        }
                        // All int32 (kept in an integer register, where there is a spare one) or all doubles.
                        ok = ok && !(ints && doubles) && !(ints && intPromos >= kIntPromoRegs);
                        if (ok)
                            own.push_back(j);
                    }
                    // The other registers: given their values in the loop only by element reads, dead before it; or not
                    // written in it at all (compared once, before it).
                    std::vector<unsigned> fixed;
                    for (unsigned q : others) {
                        if (!ok)
                            break;
                        if (q >= written.size() || !written[q]) {
                            fixed.push_back(q);
                            continue;
                        }
                        ok = regDeadAfter(before, q);
                        for (std::uint32_t j = h; std::int64_t(j) <= end && ok; j += length(j)) {
                            const Ins& in = code_->code[j];
                            const opt::Effects e = opt::effectsOf(*code_, j);
                            if (std::find(e.wr.begin(), e.wr.end(), q) == e.wr.end())
                                continue;
                            const auto& uw = plan_->useWebs[j];
                            const std::uint8_t ep = j < code_->elemProf.size() ? code_->elemProf[j] : 0;
                            const bool typedElem = in.opcode() == Op::GetElem && in.a == q && plan_->defWeb[j] < 0 &&
                                                   uw.size() >= 2 && uw[0] < 0 && uw[1] >= 0 && isI32(uw[1]) &&
                                                   (ep == 0 || ep == elemprof::kArray) &&
                                                   !(j < code_->prof.size() && (code_->prof[j] & opt::kProfFailed)) &&
                                                   !(j < chainRole_.size() && chainRole_[j]) && !(j < skipDef_.size() && skipDef_[j]);
                            if (!typedElem)
                                ok = false;
                            else
                                promoGuards_[j].push_back(loopPromos_.size());
                        }
                    }
                    if (!ok || own.empty()) {
                        for (auto& [j, ks] : promoGuards_)
                            if (!ks.empty() && ks.back() == loopPromos_.size())
                                ks.pop_back();
                        continue;
                    }
                    for (std::uint32_t j : own)
                        promoAccess_[j] = loopPromos_.size();
                    loopPromos_.push_back(LoopPromo{h, std::uint32_t(end), r, idx, hoistBase_ + std::int32_t(8 * hoistSlots_++), fixed});
                    if (ints) {
                        loopPromos_.back().i32 = true;
                        loopPromos_.back().xreg = 16 + Asm::kExtraGpr - kIntPromoRegs + intPromos++;
                    } else if (doublePromos < Asm::kExtraXmm) {
                        loopPromos_.back().xreg = 16 + doublePromos++;
                    }
                    any = true;
                }
            }
#if defined(SCRIPT_JIT_ENV_OVERRIDE)
            if (std::getenv("T2HOIST"))
                for (const LoopPromo& lp : loopPromos_)
                    if (lp.from == h)
                        std::fprintf(stderr, "promote %s loop @%u..%u r%u slot %u\n", codeName(code_).c_str(), lp.from, lp.to,
                                     lp.reg, lp.index);
#endif
        }
        // Methods read from prototypes for inlined calls on those objects.
        for (const auto& [d, sr] : windowRead_) {
            if (d < h || std::int64_t(d) > end || d + 1 >= n_ || code_->code[d + 1].a == kNoCache)
                continue;
            const unsigned recv = sr.second;
            const InlineCache& ic = code_->caches[code_->code[d + 1].a];
            if (!ic.holder || !ic.validity || !ic.validity->valid || (ic.pad && ic.pad <= code_->poly.size() &&
                                                                      code_->poly[ic.pad - 1].n) ||
                !shapes.count(recv) || mixedShape.count(recv) || shapes[recv].first != ic.shape ||
                stored.count({ic.holder->h.shape, ic.index}) || (d < code_->prof.size() && (code_->prof[d] & opt::kProfFailed)))
                continue;
            const JSValue v = *const_cast<JSObject*>(ic.holder)->slotAt(ic.index);
            if (!isFunction(v) || (static_cast<const JSFunction*>(asObject(v))->flags & JSFunction::kNative))
                continue;
            loopWins_.push_back(LoopWin{h, std::uint32_t(end), d, v});
            cells.insert(ic.validity);
            any = true;
        }
        // Arrays in registers the loop does not write, which it reads or stores elements of.
        std::map<unsigned, std::uint8_t> arrs;
        for (std::uint32_t j = h; std::int64_t(j) <= end; j += length(j)) {
            const Ins& in = code_->code[j];
            const Op op = in.opcode();
            if (op != Op::GetElem && op != Op::PutElem)
                continue;
            const bool put = op == Op::PutElem;
            const auto& uw = plan_->useWebs[j];
            const unsigned r = put ? in.a : in.b;
            const std::uint8_t ep = j < code_->elemProf.size() ? code_->elemProf[j] : 0;
            if (uw.size() < 2 || uw[0] >= 0 || uw[1] < 0 || (ep != 0 && ep != elemprof::kArray) ||
                (j < code_->prof.size() && (code_->prof[j] & opt::kProfFailed)) || r >= written.size() || written[r] ||
                (j < chainRole_.size() && (chainRole_[j] & 2)))
                continue;
            arrs[r] |= put ? 3 : 1;
        }
        for (const auto& [r, bits] : arrs)
            loopArrs_.push_back(LoopArr{h, std::uint32_t(end), r, bits});
        any = any || !arrs.empty() || !nums.empty();
        // Globals and `Math` functions the loop reads: the same all through it, unless it stores to the global object or
        // to `Math` (a typed store would not call out).
        std::set<std::uint32_t> readShapes{vm_.global->h.shape};
        for (const auto& [c, reads] : mathWin_)
            if (c >= h && std::int64_t(c) <= end && reads.second + 1 < n_ && code_->code[reads.second + 1].a != kNoCache)
                readShapes.insert(code_->caches[code_->code[reads.second + 1].a].shape);
        bool storesThere = false;
        for (std::uint32_t j = h; std::int64_t(j) <= end; j += length(j)) {
            if (code_->code[j].opcode() != Op::PutProp || j + 1 >= n_ || code_->code[j + 1].a == kNoCache)
                continue;
            const InlineCache& ic = code_->caches[code_->code[j + 1].a];
            if (readShapes.count(ic.shape) || (ic.next && readShapes.count(ic.next)))
                storesThere = true;
            if (ic.pad && ic.pad <= code_->poly.size())
                for (std::uint8_t k = 0; k < code_->poly[ic.pad - 1].n; ++k) {
                    const PolyEntry& e = code_->poly[ic.pad - 1].e[k];
                    if (readShapes.count(e.shape) || (e.kind == PolyEntry::Add && readShapes.count(e.next)))
                        storesThere = true;
                }
        }
        if (!storesThere) {
            for (std::uint32_t j = h; std::int64_t(j) <= end; j += length(j)) {
                const Ins& in = code_->code[j];
                if (in.opcode() != Op::GetGlobal || j >= chainRole_.size() || !(chainRole_[j] & 1) || (chainRole_[j] & 2) ||
                    hoistSlots_ >= kHoistSlots)
                    continue;
                std::uint8_t attrs = 0;
                if (!vm_.global->findOwn(vm_, code_->atoms[in.b], &attrs) || (attrs & attr::kAccessor))
                    continue;
                // The link that takes it: the next one of the chain, reading the register it was read into.
                std::uint32_t next = j + length(j);
                while (next < n_ && std::int64_t(next) <= end && !(next < chainRole_.size() && (chainRole_[next] & 2)))
                    next += length(next);
                const std::uint8_t ep = next < code_->elemProf.size() ? code_->elemProf[next] : 0;
                const bool array = next < n_ && std::int64_t(next) <= end && code_->code[next].opcode() == Op::GetElem &&
                                   code_->code[next].b == in.a && (ep == 0 || ep == elemprof::kArray);
                loopGlobals_.push_back(LoopGlobal{h, std::uint32_t(end), j,
                                                  hoistBase_ + std::int32_t(8 * hoistSlots_++), array, next});
                any = true;
            }
            for (const auto& [c, reads] : mathWin_)
                if (c >= h && std::int64_t(c) <= end) {
                    loopMaths_.push_back(LoopMath{h, std::uint32_t(end), c});
                    any = true;
                }
        }
        for (ValidityCell* c : cells) {
            loopValids_.push_back(LoopValid{h, std::uint32_t(end), c});
            any = true;
        }
        if (any) {
            preLoop_[h];
            hoistWatch_[h] = std::make_pair(std::uint32_t(end), std::uint64_t(0));
        }
#if defined(SCRIPT_JIT_ENV_OVERRIDE)
        if (std::getenv("T2HOIST")) {
            std::fprintf(stderr, "hoist %s loop @%u..%lld: %zu shapes %zu chains %zu arrays\n", codeName(code_).c_str(), h,
                         (long long)end, shapes.size() - mixedShape.size(), cells.size(), arrs.size());
            for (const LoopShape& ls : loopShapes_)
                if (ls.from == h)
                    std::fprintf(stderr, "  shape r%u %u aux %d\n", ls.reg, ls.shape, ls.aux);
            for (const LoopRead& lr : loopReads_)
                if (lr.from == h)
                    std::fprintf(stderr, "  read @%u dup %d\n", lr.read, int(lr.dup));
        }
#endif
    }
    // A loop that reads only slots in place of an object before it needs only that many in place, not the exact count the
    // sites learned on (objects of one shape can be made with more).
    for (LoopShape& ls : loopShapes_) {
        if (ls.aux < 0)
            continue;
        std::int32_t need = 0;
        bool inPlace = true;
        auto use = [&](std::uint32_t index) {
            inPlace = inPlace && std::int32_t(index) < ls.aux;
            need = std::max(need, std::int32_t(index) + 1);
        };
        for (const LoopRead& lr : loopReads_)
            if (lr.from == ls.from && code_->code[lr.read].b == ls.reg)
                use(code_->caches[code_->code[lr.read + 1].a].index);
        for (const LoopNum& ln : loopNums_)
            if (ln.from == ls.from && ln.reg == ls.reg)
                use(ln.index);
        for (const LoopPromo& lp : loopPromos_)
            if (lp.from == ls.from && lp.reg == ls.reg)
                use(lp.index);
        if (inPlace)
            ls.atLeast = need;
    }
}

void Baseline::planBorrows()
{
    borrows_.clear();
#if defined(SCRIPT_JIT_ENV_OVERRIDE)
    if (std::getenv("T2NOBORROW"))
        return;
#endif
    for (const auto& [h, watch] : hoistWatch_) {
        const std::uint32_t end = watch.first;
        if (h == 0 || noHoist_.count(h) || !opt::isConditionalJump(code_->code[end].opcode()) ||
            opt::jumpTarget(*code_, end) != std::int64_t(h))
            continue;
        // Nothing in the loop leaves it but its last jump, and every instruction is one the optimizing code does itself
        // without a call: the checks before the loop (`hoistWatch_`) see to the rest.
        bool ok = true;
        std::uint32_t prev = h;
        for (std::uint32_t j = h; j <= end && ok; j += length(j)) {
            const Ins& in = code_->code[j];
            const std::int64_t t = opt::jumpTarget(*code_, j);
            if (t >= 0 && (t < std::int64_t(h) || t > std::int64_t(end)))
                ok = false;
            switch (in.opcode()) {
            case Op::Nop: case Op::Mov: case Op::LoadK: case Op::LoadInt: case Op::LoadUndef: case Op::LoadNull:
            case Op::LoadTrue: case Op::LoadFalse: case Op::GetProp: case Op::PutProp: case Op::GetElem: case Op::GetGlobal:
            case Op::Add: case Op::Sub: case Op::Mul: case Op::Div: case Op::Mod: case Op::Neg: case Op::Inc: case Op::Dec:
            case Op::AddI: case Op::SubI: case Op::MulI: case Op::ModI: case Op::BitAnd: case Op::BitOr: case Op::BitXor:
            case Op::Shl: case Op::Shr: case Op::UShr: case Op::BitAndI: case Op::BitOrI: case Op::BitXorI: case Op::ShlI:
            case Op::ShrI: case Op::UShrI: case Op::Lt: case Op::Le: case Op::Gt: case Op::Ge: case Op::StrictEq:
            case Op::StrictNe: case Op::Jmp:
                break;
            case Op::Call:
                ok = mathWin_.count(j) != 0;
                break;
            default:
                ok = ok && opt::isConditionalJump(in.opcode());
                break;
            }
            prev = j;
        }
        (void)prev;
        if (!ok)
            continue;
        std::uint32_t before = 0;
        for (std::uint32_t j = 0; j < h; j += length(j))
            before = j;
        // Candidates: the destinations of element reads of arrays the loop does not write.
        std::set<unsigned> cands, bad;
        for (std::uint32_t j = h; j <= end; j += length(j)) {
            const Ins& in = code_->code[j];
            if (in.opcode() != Op::GetElem)
                continue;
            const auto& uw = plan_->useWebs[j];
            const std::uint8_t ep = j < code_->elemProf.size() ? code_->elemProf[j] : 0;
            bool arr = false;
            for (const LoopArr& la : loopArrs_)
                if (la.reg == in.b && la.from <= h && end <= la.to)
                    arr = true;
            const bool chained = (j < chainRole_.size() && chainRole_[j]) || (j < skipDef_.size() && skipDef_[j]);
            if (!arr || chained || plan_->defWeb[j] >= 0 || uw.size() < 2 || uw[0] >= 0 || uw[1] < 0 || !isI32(uw[1]) ||
                (ep != 0 && ep != elemprof::kArray) || (j < code_->prof.size() && (code_->prof[j] & opt::kProfFailed)) ||
                in.a == in.b)
                bad.insert(in.a);
            else
                cands.insert(in.a);
        }
        for (unsigned r : cands) {
            if (bad.count(r) || borrowed(h, r))
                continue;
            bool fine = regDeadAfter(before, r);
            for (std::uint32_t j = h; j <= end && fine; j += length(j)) {
                const Ins& in = code_->code[j];
                const opt::Effects e = opt::effectsOf(*code_, j);
                const bool writes = std::find(e.wr.begin(), e.wr.end(), r) != e.wr.end();
                const bool reads = std::find(e.rd.begin(), e.rd.end(), r) != e.rd.end();
                if (writes && in.opcode() != Op::GetElem)
                    fine = false;
                if (!reads)
                    continue;
                // Read only as the object of a property, never as the value stored or the result's register.
                if (in.opcode() == Op::GetProp)
                    fine = fine && in.b == r && in.a != r && !(j < chainRole_.size() && chainRole_[j]) && !windowRead_.count(j);
                else if (in.opcode() == Op::PutProp)
                    fine = fine && in.a == r && in.c != r;
                else
                    fine = false;
            }
            if (fine)
                borrows_.push_back(Borrow{h, end, r});
        }
    }
#if defined(SCRIPT_JIT_ENV_OVERRIDE)
    if (std::getenv("T2HOIST"))
        for (const Borrow& b : borrows_)
            std::fprintf(stderr, "borrow %s loop @%u..%u r%u\n", codeName(code_).c_str(), b.from, b.to, b.reg);
#endif
}

/// Whether loop `h` leaves out checks an enclosing loop makes (a way in that does not come through them needs its own).
bool Baseline::guardsCovered(std::uint32_t h) const
{
    for (const LoopShape& ls : loopShapes_)
        if (ls.from == h && coveredShape(h, ls))
            return true;
    for (const LoopMath& lm : loopMaths_)
        if (lm.from == h && coveredMath(h, lm))
            return true;
    for (const LoopArr& la : loopArrs_)
        if (la.from == h && coveredArr(h, la))
            return true;
    for (const LoopNum& ln : loopNums_)
        if (ln.from == h && coveredNum(h, ln))
            return true;
    for (const LoopValid& lv : loopValids_)
        if (lv.from == h && coveredValid(h, lv))
            return true;
    return false;
}

/// Whether the loop at `from`..`to` holds all through loop `h` (it encloses it) and made its checks before it.
bool Baseline::encloses(std::uint32_t from, std::uint32_t to, std::uint32_t h) const
{
    const auto w = hoistWatch_.find(h);
    return w != hoistWatch_.end() && from < h && to >= w->second.first && !noHoist_.count(from) && hoistWatch_.count(from);
}
bool Baseline::coveredShape(std::uint32_t h, const LoopShape& x) const
{
    for (const LoopShape& o : loopShapes_)
        if (encloses(o.from, o.to, h) && o.reg == x.reg && o.shape == x.shape &&
            (x.aux < 0 || (o.aux == x.aux && (x.atLeast < 0 ? o.atLeast < 0 : o.atLeast < 0 || o.atLeast >= x.atLeast))))
            return true;
    return false;
}
bool Baseline::coveredMath(std::uint32_t h, const LoopMath& x) const
{
    for (const LoopMath& o : loopMaths_)
        if (encloses(o.from, o.to, h) && o.call == x.call)
            return true;
    return false;
}
bool Baseline::coveredArr(std::uint32_t h, const LoopArr& x) const
{
    for (const LoopArr& o : loopArrs_)
        if (encloses(o.from, o.to, h) && o.reg == x.reg && (o.bits & x.bits) == x.bits)
            return true;
    return false;
}
bool Baseline::coveredNum(std::uint32_t h, const LoopNum& x) const
{
    if (numCheckedHere(h, x))
        return true;
    for (const LoopNum& o : loopNums_)
        if (encloses(o.from, o.to, h) && o.reg == x.reg && o.index == x.index && (o.int32 || !x.int32))
            return true;
    return false;
}
/// Whether loop `h`'s own checks (after its shapes) read the slot of `x` and check it: a promotion or a hoisted read of it.
bool Baseline::numCheckedHere(std::uint32_t h, const LoopNum& x) const
{
    // A promoted slot is loaded, and so checked, before the loop.
    for (const LoopPromo& lp : loopPromos_)
        if (lp.from == h && lp.reg == x.reg && lp.index == x.index && !x.int32)
            return true;
    // A read of the slot hoisted before this loop checks it (as an int32 when its variable is one).
    for (const LoopRead& lr : loopReads_)
        if (lr.from == h && !lr.dup && code_->code[lr.read].b == x.reg &&
            code_->caches[code_->code[lr.read + 1].a].index == x.index && (!x.int32 || isI32(plan_->defWeb[lr.read])))
            return true;
    return false;
}
bool Baseline::coveredValid(std::uint32_t h, const LoopValid& x) const
{
    for (const LoopValid& o : loopValids_)
        if (encloses(o.from, o.to, h) && o.cell == x.cell)
            return true;
    return false;
}

/// The checks before loop `h`. `covered` false: the loop's own, leaving out what an enclosing loop checked before it (or a
/// hoisted read checks); true: only what was left out, for a way into the loop that did not come through those checks.
void Baseline::emitLoopGuards(std::uint32_t h, bool covered)
{
    struct Mark
    {
        std::int64_t& v;
        std::int64_t old;
        ~Mark() { v = old; }
    } mark{emittingGuards_, emittingGuards_};
    emittingGuards_ = h;
    if (!covered)
        a_.bind(preLoop_.at(h));
    Label& bad = exitBefore(h);
    if (!covered)
        if (auto w = hoistWatch_.find(h); w != hoistWatch_.end())
            w->second.second = sideDoors_;
    for (const LoopShape& ls : loopShapes_) {
        if (ls.from != h || coveredShape(h, ls) != covered)
            continue;
        a_.mov(RAX, R(ls.reg));
        needObject(RAX, bad);
        a_.cmpI32(mem(RAX, 0), std::int32_t(ls.shape));
        a_.jcc(CNE, bad);
        if (ls.atLeast > 0) {
            a_.cmpI8(mem(RAX, kHdrAux), std::uint8_t(ls.atLeast - 1));
            a_.jcc(CBE, bad);
        } else if (ls.aux >= 0 && ls.atLeast < 0) {
            a_.cmpI8(mem(RAX, kHdrAux), std::uint8_t(ls.aux));
            a_.jcc(CNE, bad);
        }
    }
    for (const LoopRead& lr : loopReads_) {
        if (lr.from != h || lr.dup || covered)
            continue;
        const Ins& in = code_->code[lr.read];
        const InlineCache& ic = code_->caches[code_->code[lr.read + 1].a];
        std::int32_t aux = -1;
        for (const LoopShape& ls : loopShapes_)
            if (ls.from == h && ls.reg == in.b)
                aux = ls.aux;
        a_.mov(RAX, R(in.b));
        if (std::int32_t(ic.index) < aux) {
            a_.mov(RAX, mem(RAX, kObjInline + std::int32_t(8 * ic.index)));
        } else {
            a_.mov(RDX, mem(RAX, kObjSlots));
            a_.mov(RAX, mem(RDX, std::int32_t(8 * (std::int32_t(ic.index) - aux))));
        }
        if (isI32(plan_->defWeb[lr.read])) {
            needInt(RAX, bad);
            a_.mov32(mem(RSP, lr.slot), RAX);
        } else if (lr.xreg >= 0) {
            unboxDouble(lr.xreg, RAX, RDX, &bad);
        } else {
            unboxDouble(0, RAX, RDX, &bad);
            a_.movsdStore(mem(RSP, lr.slot), 0);
        }
    }
    for (const LoopPromo& lp : loopPromos_) {
        if (lp.from != h || covered)
            continue;
        for (unsigned q : lp.fixed) {
            a_.mov(RAX, R(q));
            a_.cmp(RAX, R(lp.reg));
            a_.jcc(CE, bad);
        }
        a_.mov(RAX, R(lp.reg));
        a_.mov(RCX, mem(RAX, kObjInline + std::int32_t(8 * lp.index)));
        if (lp.i32) {
            needInt(RCX, bad);
            a_.mov32(Reg(lp.xreg), RCX);
            continue;
        }
        unboxDouble(0, RCX, RDX, &bad);
        promoStore(lp, 0);
    }
    for (const LoopGlobal& lg : loopGlobals_) {
        if (lg.from != h || covered)
            continue;
        emitReadInto(lg.read, false, bad);
        a_.mov(mem(RSP, lg.slot), RAX);
        if (lg.array) {
            needObject(RAX, bad);
            a_.cmpI8(mem(RAX, kHdrCls), std::uint8_t(ObjClass::Array));
            a_.jcc(CNE, bad);
        }
    }
    for (const LoopMath& lm : loopMaths_) {
        if (lm.from != h || coveredMath(h, lm) != covered)
            continue;
        const auto& reads = mathWin_.at(lm.call);
        const JSValue callee = constantBefore(lm.call, code_->code[lm.call].b);
        holdValue(callee);
        emitReadInto(reads.first, false, bad);
        emitReadInto(reads.second, true, bad);
        a_.movImm(RCX, callee.bits);
        a_.cmp(RCX, RAX);
        a_.jcc(CNE, bad);
    }
    for (const LoopArr& la : loopArrs_) {
        if (la.from != h || coveredArr(h, la) != covered)
            continue;
        a_.mov(RAX, R(la.reg));
        needObject(RAX, bad);
        a_.cmpI8(mem(RAX, kHdrCls), std::uint8_t(ObjClass::Array));
        a_.jcc(CNE, bad);
        if (la.bits & 2) {
            a_.testI8(mem(RAX, kHdrFlags), std::uint8_t(cellflags::kFrozen | cellflags::kNotExtensible));
            a_.jcc(CNE, bad);
        }
    }
    for (const LoopNum& ln : loopNums_) {
        // One the loop's own read checks is checked there, after the shapes (never in the pass for other ways in, which
        // runs before them).
        if (ln.from != h || numCheckedHere(h, ln) || coveredNum(h, ln) != covered)
            continue;
        std::int32_t aux = -1;
        for (const LoopShape& ls : loopShapes_)
            if (ls.from == h && ls.reg == ln.reg)
                aux = ls.aux;
        a_.mov(RAX, R(ln.reg));
        if (std::int32_t(ln.index) < aux) {
            a_.mov(RAX, mem(RAX, kObjInline + std::int32_t(8 * ln.index)));
        } else {
            a_.mov(RDX, mem(RAX, kObjSlots));
            a_.mov(RAX, mem(RDX, std::int32_t(8 * (std::int32_t(ln.index) - aux))));
        }
        if (ln.int32) {
            a_.cmp(RAX, R14);
            a_.jcc(CB, bad);
        } else {
            a_.test(RAX, R14);
            a_.jcc(CE, bad);
        }
    }
    for (const LoopValid& lv : loopValids_) {
        if (lv.from != h || coveredValid(h, lv) != covered)
            continue;
        hold(lv.cell);
        a_.movImm(RCX, reinterpret_cast<std::uint64_t>(lv.cell));
        a_.cmpI32(mem(RCX, kValidityValid), 0);
        a_.jcc(CE, bad);
    }
    for (const LoopWin& lw : loopWins_) {
        if (lw.from != h || covered)
            continue;
        const InlineCache& ic = code_->caches[code_->code[lw.read + 1].a];
        holdValue(lw.known);
        holderRead(ic.holder, ic.index);
        a_.movImm(RCX, lw.known.bits);
        a_.cmp(RCX, RAX);
        a_.jcc(CNE, bad);
    }
    for (const LoopFact& lf : loopFacts_) {
        if (lf.from != h || covered)
            continue;
        a_.mov(RAX, R(lf.reg));
        needObject(RAX, bad);
        a_.cmpI8(mem(RAX, kHdrCls), std::uint8_t(ObjClass::Function));
        a_.jcc(CNE, bad);
        a_.mov(RCX, mem(RAX, kFnCode));
        a_.test(RCX, RCX);
        a_.jcc(CE, bad);
        a_.movImm(RDX, lf.code->uid);
        a_.cmp(RDX, mem(RCX, kCbUid));
        a_.jcc(CNE, bad);
    }
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
    PolySeen seen[5];
    int count = 0;
    seen[count++] = PolySeen{ic.shape, ic.index, ic.holder, ic.validity, ic.aux};
    if (ic.pad && ic.pad <= code_->poly.size()) {
        const PolyCache& pc = code_->poly[ic.pad - 1];
        for (std::uint8_t k = 0; k < pc.n; ++k)
            seen[count++] = PolySeen{pc.e[k].shape, pc.e[k].index, pc.e[k].holder, pc.e[k].validity, pc.e[k].aux};
    }
    if (!fromRax)
        a_.mov(RAX, R(in.b));
    polyRead(seen, count, bad, fromRax ? -1 : int(in.b));
}
