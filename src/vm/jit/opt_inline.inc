// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Inlining for the optimizing compiler (included from baseline_x64.cpp after opt_emit.inc, inside `Baseline`'s namespace).
//
// A `Call` whose callee is a small straight-line function (property reads, arithmetic, upvalues, a return) runs the callee's
// instructions in place: no frame, the arguments read where the caller keeps them, the result delivered as a typed value.
// The callee's values live in slots of the optimized frame. Every guard of the inlined body comes before its first side
// effect, so a guard that fails leaves the caller as it was before the call and the call runs again in the interpreter.

namespace {

struct IVal
{
    enum Kind : std::uint8_t { I32, F64, Box, Const };
    Kind kind = Const;
    std::int32_t web = -1;       ///< a typed web of the caller
    std::int32_t frameReg = -1;  ///< a register of the caller's frame
    std::int32_t slot = -1;      ///< a slot of the inline area
    JSValue c = JSValue::undefined();
    std::uint8_t hint = 0;   ///< Box: profBit bits of what it has held
    std::uint32_t epoch = 0;  ///< Box: side effects before it was read (a later one may have let it go)
};

constexpr std::uint32_t kInlineMaxCode = 48;

bool readsThis(const CodeBlock& c)
{
    for (std::uint32_t j = 0; j < c.code.size(); j = opt::nextOf(c, j))
        for (std::uint16_t r : opt::effectsOf(c, j).rd)
            if (r == 0)
                return true;
    return false;
}

}  // namespace

Mem Baseline::inlineSlot(std::int32_t s) const
{
    return mem(RSP, inlineBase_ + 8 * s);
}

bool Baseline::inlineCall(std::uint32_t i)
{
    const Ins& in = code_->code[i];
    if (in.opcode() != Op::Call || inlineBase_ < 0)
        return false;
    if (i < code_->prof.size() && (code_->prof[i] & opt::kProfFailed))
        return false;
    const unsigned base = in.b, argc = in.c;
    const auto& uw = plan_->useWebs[i];
    if (uw.size() < 2u + argc)
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
    if (!C || C == code_ || C->prof.size() < C->code.size() + C->nparams || C->code.size() > kInlineMaxCode || C->nregs > kInlineSlots)
        return false;
    if (C->flags & (CodeBlock::kGenerator | CodeBlock::kAsync | CodeBlock::kAsyncGen | CodeBlock::kNeedsArgs |
                    CodeBlock::kClassConstructor))
        return false;
    const bool sloppyThis = !(C->flags & (CodeBlock::kStrict | CodeBlock::kArrow)) && readsThis(*C);
    if ((C->flags & CodeBlock::kArrow) && readsThis(*C))
        return false;
    if (sloppyThis && uw[1] >= 0)
        return false;
    // The first pass decides everything and emits nothing; the second follows it.
    return inlineBody(i, C, known, sloppyThis, false) && inlineBody(i, C, known, sloppyThis, true);
}

bool Baseline::inlineBody(std::uint32_t i, CodeBlock* C, JSValue known, bool sloppyThis, bool emit)
{
    const Ins& in = code_->code[i];
    const unsigned base = in.b, argc = in.c;
    const auto& uw = plan_->useWebs[i];
    Label dummy;
    bool sideEffect = false;
    std::uint32_t epoch = 0;
    std::int32_t nextSlot = 0;
    // A label for a guard: the call again in the interpreter; none once the body has changed anything.
    auto fail = [&]() -> Label* {
        if (sideEffect)
            return nullptr;
        return emit ? &exitBefore(i) : &dummy;
    };
    auto newSlot = [&]() -> std::int32_t { return nextSlot < std::int32_t(kInlineSlots) ? nextSlot++ : -1; };

    std::vector<IVal> regs(C->nregs);
    auto fromCaller = [&](unsigned reg, std::size_t k, std::uint8_t hint) {
        IVal v;
        if (uw[k] >= 0) {
            v.kind = isI32(uw[k]) ? IVal::I32 : IVal::F64;
            v.web = uw[k];
        } else {
            v.kind = IVal::Box;
            v.frameReg = std::int32_t(reg);
            v.hint = hint;
        }
        return v;
    };
    if (C->nregs > 0)
        regs[0] = fromCaller(base + 1, 1, 8);
    for (unsigned p = 0; p < C->nparams && 1 + p < C->nregs; ++p) {
        if (p < argc)
            regs[1 + p] = fromCaller(base + 2 + p, 2 + p, C->prof[C->code.size() + p]);
    }

    // ---- loading values into scratch registers ----
    auto boxBits = [&](const IVal& v, Reg dst) {
        if (v.frameReg >= 0)
            a_.mov(dst, R(unsigned(v.frameReg)));
        else
            a_.mov(dst, inlineSlot(v.slot));
    };
    auto isInt = [](const IVal& v) {
        return v.kind == IVal::I32 || (v.kind == IVal::Const && v.c.isInt32()) || (v.kind == IVal::Box && v.hint == 1);
    };
    auto isNum = [](const IVal& v) {
        return v.kind == IVal::I32 || v.kind == IVal::F64 || (v.kind == IVal::Const && v.c.isNumber()) ||
               (v.kind == IVal::Box && v.hint && !(v.hint & ~3));
    };
    // Whether loading `v` as a number can fail (a value in the frame or a slot, checked as it is read).
    auto checked = [](const IVal& v) { return v.kind == IVal::Box; };
    auto loadI32 = [&](const IVal& v, Reg dst, Label* bad) {
        if (!emit)
            return;
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
            boxBits(v, dst);
            needInt(dst, *bad);
            a_.mov32(dst, dst);
            break;
        default:
            break;
        }
    };
    // `tmp` and rdx are clobbered.
    auto loadF64 = [&](const IVal& v, int x, Reg tmp, Label* bad) {
        if (!emit)
            return;
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
            boxBits(v, tmp);
            needNumber(tmp, *bad);
            loadNumber(x, tmp, RDX);
            break;
        }
    };
    // The value boxed (not counted) in rax. Clobbers rdx, xmm0.
    auto loadBoxed = [&](const IVal& v) {
        if (!emit)
            return;
        switch (v.kind) {
        case IVal::I32:
            loadI32(v, RAX, nullptr);
            a_.or_(RAX, R14);
            break;
        case IVal::F64:
            loadF64(v, 0, RAX, nullptr);
            boxDouble0();
            break;
        case IVal::Const:
            a_.movImm(RAX, v.c.bits);
            break;
        case IVal::Box:
            boxBits(v, RAX);
            break;
        }
    };
    // The callee in rax.
    auto loadCallee = [&]() {
        if (!emit)
            return;
        if (!known.isUndefined())
            a_.movImm(RAX, known.bits);
        else
            a_.mov(RAX, R(base));
    };
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
            out.epoch = epoch;
            if (emit)
                a_.mov(inlineSlot(out.slot), RAX);
        }
        return true;
    };

    // ---- the callee is the one compiled in ----
    {
        Label* bad = fail();
        if (emit) {
            if (!known.isUndefined()) {
                holdValue(known);
                a_.movImm(RCX, known.bits);
                a_.cmp(RCX, R(base));
                a_.jcc(CNE, *bad);
            } else {
                a_.mov(RAX, R(base));
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
                // `this` as the callee sees it: only an object is passed as it is.
                a_.mov(RAX, R(base + 1));
                needObject(RAX, *bad);
            }
        }
    }

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
            if (recv.kind != IVal::Box || recv.epoch != epoch)
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
                        return false;  // a getter: not this kind of body
                }
            }
            Label* bad = fail();
            if (!count || !bad)
                return false;
            Label got;
            if (emit) {
                boxBits(recv, RAX);
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
                    a_.mov(RAX, mem(RDX, 0));
                    a_.jmp(got);
                    if (k + 1 < count)
                        a_.bind(next);
                }
                a_.bind(got);
            }
            IVal out;
            if (!takeRead(p, bad, out))
                return false;
            regs[ci.a] = out;
            break;
        }
        case Op::GetUpval: {
            if (ci.b < upKnown.size() && upKnown[ci.b] >= 0) {
                regs[ci.a] = upVal[ci.b];
                break;
            }
            Label* bad = fail();
            loadCallee();
            if (emit) {
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
            if (v.kind == IVal::Box && v.epoch != epoch)
                return false;
            if (emit) {
                loadBoxed(v);
                a_.mov(RCX, RAX);
                if (v.kind == IVal::Box || (v.kind == IVal::Const && v.c.isCell()))
                    retain(RCX);
                loadCallee();
                a_.mov(RAX, mem(RAX, kFnUpvals + std::int32_t(8 * ci.a)));
                a_.mov(RSI, mem(RAX, kBoxValue));
                a_.mov(mem(RAX, kBoxValue), RCX);
                release(RSI);
            }
            sideEffect = true;
            ++epoch;
            upKnown[ci.a] = 1;
            upVal[ci.a] = v;
            if (v.kind == IVal::Box)
                upVal[ci.a].epoch = epoch;
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
            if (imm || unary) {
                B.c = JSValue::int32(unary ? 1 : k);
            } else {
                B = regs[ci.c];
            }
            if ((A.kind == IVal::Box && A.epoch != epoch) || (B.kind == IVal::Box && B.epoch != epoch))
                return false;
            if (op == Op::UShrI && (k & 31) == 0)
                return false;
            IVal out;
            out.slot = newSlot();
            if (out.slot < 0)
                return false;
            const bool ints = isInt(A) && isInt(B);
            if (bitwise) {
                if (!ints)
                    return false;
                Label* bad = (checked(A) || checked(B)) ? fail() : &dummy;
                if (!bad)
                    return false;
                loadI32(A, RAX, bad);
                if (!imm)
                    loadI32(B, RCX, bad);
                if (emit) {
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
            const bool intPath = ints && op != Op::Div && !(p & 2);
            if (intPath) {
                // Overflow, or a product that is a negative zero, leaves the int32s.
                Label* bad = fail();
                if (!bad)
                    return false;
                loadI32(A, RAX, bad);
                if (!imm && !unary)
                    loadI32(B, RCX, bad);
                if (emit) {
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
            Label* bad = (checked(A) || checked(B)) ? fail() : &dummy;
            if (!bad)
                return false;
            loadF64(A, 0, RAX, bad);
            loadF64(B, 1, RCX, bad);
            if (emit) {
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
        case Op::Ret: {
            const IVal v = regs[ci.a];
            if (v.kind == IVal::Box && v.epoch != epoch)
                return false;
            const std::int32_t dw = plan_->defWeb[i];
            if (dw >= 0 && isI32(dw)) {
                if (v.kind == IVal::Const) {
                    if (!v.c.isInt32())
                        return false;
                    if (emit)
                        setConstI32(dw, v.c.asInt32());
                } else if (v.kind == IVal::I32) {
                    loadI32(v, RAX, nullptr);
                    if (emit)
                        defI32(dw, RAX);
                } else if (v.kind == IVal::Box) {
                    Label* bad = fail();
                    if (!bad)
                        return false;
                    loadI32(v, RAX, bad);
                    if (emit)
                        defI32(dw, RAX);
                } else {
                    // A double for an int32 variable: only if it is one.
                    Label* bad = fail();
                    if (!bad)
                        return false;
                    loadF64(v, 0, RAX, nullptr);
                    if (emit) {
                        Label ok;
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
                        defI32(dw, RAX);
                    }
                }
            } else if (dw >= 0) {
                if (!isNum(v))
                    return false;
                Label* bad = checked(v) ? fail() : &dummy;
                if (!bad)
                    return false;
                loadF64(v, 0, RAX, bad);
                if (emit)
                    defF64(dw, 0);
            } else {
                if (emit) {
                    loadBoxed(v);
                    if (v.kind == IVal::Box || (v.kind == IVal::Const && v.c.isCell()))
                        retain(RAX);
                    write(in.a, RAX);
                }
            }
            return true;
        }
        default:
            return false;
        }
    }
    return false;
}
