// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// From dynamic bytecode to IR.
//
// A value on the stack is, by what is known of it at this point: a double in a floating point register, an int32 in an integer
// register, or a tag and a payload in one register each. Where nothing is known, an operation tests for its usual case inline
// and calls a helper on a cold path otherwise. Reference counting follows the same rules as `src/typed/jit/lower.cpp`: an
// entry that reads a local or a field holds no count until something could change that place.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <map>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "script/internal/dyn/numconv.hpp"
#include "script/internal/dyn/jit.hpp"
#include "script/internal/dyn/jit_helpers.hpp"
#include "script/internal/dyn/object.hpp"
#include "script/internal/dyn/runtime.hpp"
#include "script/internal/dyn/vm.hpp"

namespace script {
namespace internal {
namespace dyn {

namespace {

using typed::IrCall;
using typed::IrCallKind;
using typed::IrCond;
using typed::IrExit;
using typed::IrFunction;
using typed::IrInst;
using typed::IrOp;
using typed::IrOsrEntry;
using typed::IrSite;

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
#endif
constexpr std::int64_t kCellKindOffset = offsetof(GCObject, kind);
constexpr std::int64_t kClsOffset = offsetof(DynObject, cls);
constexpr std::int64_t kShapeOffset = offsetof(DynObject, shape);
constexpr std::int64_t kProtoOffset = offsetof(DynObject, proto);  // the first word of a reference is the pointer
constexpr std::int64_t kPropsOffset = offsetof(DynObject, props);  // the first word of the table is its data
constexpr std::int64_t kPropsSizeOffset = kPropsOffset + 8;
constexpr std::int64_t kPropsCapOffset = kPropsOffset + 12;
constexpr std::int64_t kFrozenOffset = offsetof(DynObject, frozen);
constexpr std::int64_t kIsProtoOffset = offsetof(DynObject, isProto);
constexpr std::int64_t kExtensibleOffset = offsetof(DynObject, extensible);
constexpr std::int64_t kRcOffset = offsetof(GCObject, rc);
constexpr std::int64_t kStringLengthOffset = offsetof(StringCell, length);
constexpr std::int64_t kStringCharsOffset = sizeof(StringCell);
constexpr std::int64_t kRareOffset = offsetof(DynObject, rare);
constexpr std::int64_t kPropValueOffset = offsetof(Prop, value);
constexpr std::int64_t kElementsOffset = offsetof(DynArray, store);  // a pointer, then two 32-bit counts
constexpr std::int64_t kElementCountOffset = kElementsOffset + 8;
constexpr std::int64_t kElementCapOffset = kElementsOffset + 12;
constexpr std::int64_t kNumbersOnlyOffset = offsetof(DynObject, numbersOnly);
constexpr std::int64_t kFnCodeOffset = offsetof(DynFunction, code);  // the first word of a shared_ptr is the pointer
constexpr std::int64_t kFnUpvaluesOffset = offsetof(DynFunction, upvalues);
constexpr std::int64_t kCallEntryOffset = offsetof(FunctionCode, jitCallEntry);
constexpr std::int64_t kBoxValueOffset = offsetof(Box, value);
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
constexpr std::int64_t kPayload = 8;
/// `IrInst::alias` classes.
/// A cell's kind and, in an object, its class, read together.
static_assert(offsetof(DynObject, cls) == offsetof(GCObject, kind) + 1);
constexpr std::int64_t kKindAndClassOfArray = std::int64_t(CellKind::Object) | std::int64_t(ObjClass::Array) << 8;
constexpr std::uint8_t kAliasHeader = 1;
constexpr std::uint8_t kAliasValue = 2;

struct Refuse
{
    std::string why;
};
[[noreturn]] void refuse(std::string why)
{
    throw Refuse{std::move(why)};
}
/// A local turned out not to hold only numbers: lower again knowing that.
struct LowerAgain
{
    std::uint32_t slot;
    /// Only the guess that it holds whole numbers was wrong.
    bool onlyInt = false;
};

enum class Rep : std::uint8_t
{
    Double,
    Int32,
    /// An integer in 64 bits, of a size `mag` says, which a double holds exactly: it needs no overflow check until it is
    /// used as something other than the operand of more of the same, or of a bitwise operator, whose low word is enough.
    Exact,
    Tagged,
};

enum class Own : std::uint8_t
{
    None,
    Owned,
    Local,
    Weak,
    Immortal,
};

struct Entry
{
    Rep rep = Rep::Tagged;
    Own own = Own::None;
    bool isConst = false;
    bool hasKnownTag = false;  ///< the tag is `known`, and `t` is unused
    Type known = Type::Undefined;
    bool nonNull = false;
    /// An int32 whose register holds it sign-extended to 64 bits, not just in its low half.
    bool wide = false;
    /// Int32 or Exact: its magnitude is below 2^mag, and when `atLeastZero` it is not negative.
    std::uint8_t mag = 31;
    bool atLeastZero = false;
    std::int32_t r = -1;  ///< the double, the int, or the payload
    std::int32_t t = -1;  ///< the tag
    double number = 0;    ///< constant double or int
    std::uint64_t bits = 0;  ///< constant payload
    std::int32_t local = -1;
    /// The object this was when the code was compiled, when that is known: a hint, never relied on unguarded.
    const GCObject* hint = nullptr;
    /// `arguments`, not made: the call this is about to be an argument of says what it comes to.
    bool lazyArgs = false;
    /// The value is `hint`, checked, and the code keeps it alive: it needs no count to be held.
    bool pinned = false;
    /// An object literal under construction, which nothing else can see yet: its shape so far and the room in its table.
    Shape* fresh = nullptr;
    std::uint32_t freshRoom = 0;

    /// A number, of no particular value: what an operation with one operand has as its other.
    static Entry someNumber()
    {
        Entry e;
        e.rep = Rep::Double;
        e.isConst = true;
        return e;
    }
};

/// How many 32-bit operands follow an opcode.
int operandsOf(Op op) noexcept
{
    switch (op) {
        case Op::DefGlobal: return 2;
        case Op::PushConst:
        case Op::PopN:
        case Op::GetLocal:
        case Op::SetLocal:
        case Op::GetCell:
        case Op::SetCell:
        case Op::NewCell:
        case Op::RenewCell:
        case Op::GetUpval:
        case Op::SetUpval:
        case Op::GetGlobal:
        case Op::SetGlobal:
        case Op::TypeofGlobal:
        case Op::DeleteGlobal:
        case Op::GetElem:
        case Op::SetElem:
        case Op::GetElemDup:
        case Op::GetProp:
        case Op::SetProp:
        case Op::GetPropDup:
        case Op::GetPropBound:
        case Op::DeleteProp:
        case Op::DefField:
        case Op::DefGetter:
        case Op::DefSetter:
        case Op::DefMethod:
        case Op::ClassSetup:
        case Op::GetSuper:
        case Op::SetSuper:
        case Op::NewArray:
        case Op::NewObject:
        case Op::Closure:
        case Op::Call:
        case Op::New:
        case Op::CallSuper:
        case Op::Jump:
        case Op::JumpIfFalse:
        case Op::JumpIfTrue:
        case Op::JumpIfFalseKeep:
        case Op::JumpIfTrueKeep:
        case Op::JumpIfNullishKeep:
        case Op::JumpIfNotNullishKeep:
        case Op::JumpSub:
        case Op::ForInNext:
        case Op::IterNext: return 1;
        default: return 0;
    }
}

bool isBranch(Op op) noexcept
{
    switch (op) {
        case Op::Jump:
        case Op::JumpIfFalse:
        case Op::JumpIfTrue:
        case Op::JumpIfFalseKeep:
        case Op::JumpIfTrueKeep:
        case Op::JumpIfNullishKeep:
        case Op::JumpIfNotNullishKeep:
        case Op::ForInNext:
        case Op::IterNext: return true;
        default: return false;
    }
}

class Lowering
{
public:
    Lowering(Runtime& rt, const FunctionCode& code, IrFunction& fn, const std::vector<std::uint8_t>& notNumber,
             const std::vector<std::uint8_t>& notInt)
        : rt_(rt), code_(code), fn_(fn), notNumber_(notNumber), notInt_(notInt)
    {}

    void run()
    {
        if (!code_.handlers.empty())
            refuse("the function has exception handlers");
        speculate_ = code_.jitMayGuess() && !code_.isClassConstructor;
        prepare();
        const std::uint32_t arity = std::uint32_t(std::max(code_.arity, 0));
        // An int32 parameter costs a conversion on the way in, and pays it back in a loop or where it meets what wants ints.
        for (std::uint32_t pc = 0; pc < code_.code.size();) {
            const Op op = Op(code_.code[pc]);
            if (op == Op::GetElem || op == Op::SetElem || op == Op::BitAnd || op == Op::BitOr || op == Op::BitXor || op == Op::Shl ||
                op == Op::Shr || op == Op::UShr || (isBranch(op) && readOperand(code_.code, pc + 1) <= pc))
                intWork_ = true;
            pc += 1 + std::uint32_t(operandsOf(op)) * 4;
        }
        // Parameters only ever given numbers are number locals like any other, checked on the way in.
        numberParam_.assign(code_.numLocals, 0);
        for (std::uint32_t slot = 0; speculate_ && slot < std::min<std::uint32_t>(arity, 32) && slot < code_.numLocals; ++slot) {
            if ((code_.argsNotNumber >> slot) & 1 ||
                std::find(code_.boxedParams.begin(), code_.boxedParams.end(), slot) != code_.boxedParams.end())
                continue;
            numberParam_[slot] = 1;
        }

        // fn, this, args, argc.
        fnReg_ = fn_.newVreg(false);
        thisTag_ = fn_.newVreg(false);
        thisReg_ = fn_.newVreg(false);
        const std::int32_t args = fn_.newVreg(false);
        const std::int32_t argc = fn_.newVreg(false);
        argsReg_ = args;
        argcReg_ = argc;
        argsLazy_ = speculate_ && onlyForwardsArguments();
        fn_.params = {fnReg_, thisTag_, thisReg_, args, argc};
        fn_.osrArguments = {fnReg_, thisTag_, thisReg_};
        // A parameter guessed wrong is found out before anything has happened: the call starts over, some other way.
        const std::int32_t entryThisTag = thisTag_;
        const std::int32_t entryThis = thisReg_;
        const std::int32_t entryMiss = newLabel();
        bool checksEntry = false;
        const auto checkParam = [&](std::uint32_t slot) {
            checksEntry = true;
            IrInst& given = inst(IrOp::BrCmp);
            given.a = argc;
            given.cond = IrCond::Ls;
            given.hasImm = true;
            given.imm = std::int64_t(slot);
            given.width = 64;
            given.target = entryMiss;
            const std::int32_t tag = load(args, std::int64_t(slot) * 16, 1);
            branchCmp(tag, IrCond::Ne, std::int64_t(Type::Double), entryMiss);
        };
        // Its register is made below, with the other locals'.
        std::vector<std::uint32_t> numberParams;
        for (std::uint32_t slot = 0; slot < numberParam_.size(); ++slot) {
            if (numberParam_[slot] && isNumberLocal(slot)) {
                checkParam(slot);
                numberParams.push_back(slot);
            }
        }
        // Non-strict: nullish `this` reads as the global object, so it might as well be that from the start. Entering at
        // a loop does the same before it gets here.
        if (code_.sloppyThis && readsThis())
            normalizeThis();

        const std::uint32_t slots = code_.numLocals;
        localReg_.assign(slots, -1);
        localTag_.assign(slots, -1);
        localOwned_.assign(slots, 0);
        cellReg_.assign(slots, -1);
        cellOwned_.assign(slots, 0);
        for (std::uint32_t slot = 0; slot < slots; ++slot) {
            if (isNumberLocal(slot)) {
                localReg_[slot] = fn_.newVreg(!isIntLocal(slot));
                continue;
            }
            localReg_[slot] = fn_.newVreg(false);
            localTag_[slot] = fn_.newVreg(false);
        }
        for (std::uint32_t slot : numberParams) {
            if (isIntLocal(slot)) {
                const std::int32_t bits = load(args, std::int64_t(slot) * 16 + kPayload, 8);
                {
                    // Minus zero is the one number the comparison below takes for an int32 that is not.
                    const std::int32_t minusZero = immReg(std::numeric_limits<std::int64_t>::min());
                    IrInst& test = inst(IrOp::BrCmp);
                    test.a = bits;
                    test.b = minusZero;
                    test.cond = IrCond::Eq;
                    test.width = 64;
                    test.target = entryMiss;
                }
                const std::int32_t fp = fn_.newVreg(true);
                IrInst& asFp = inst(IrOp::BitsToFp);
                asFp.d = fp;
                asFp.a = bits;
                const std::int32_t raw = fn_.newVreg(false);
                IrInst& conv = inst(IrOp::FpToIntRaw);
                conv.d = raw;
                conv.a = fp;
                IrInst& ext = inst(IrOp::Sext32);
                ext.d = localReg_[slot];
                ext.a = raw;
                const std::int32_t back = fn_.newVreg(true);
                IrInst& to = inst(IrOp::IntToFp);
                to.d = back;
                to.a = localReg_[slot];
                to.srcWidth = 32;
                to.width = 64;
                const std::int32_t same = newLabel();
                IrInst& eq = inst(IrOp::BrFCmp);
                eq.a = fp;
                eq.b = back;
                eq.cond = IrCond::FEq;
                eq.width = 64;
                eq.target = same;
                inst(IrOp::Br).imm = entryMiss;
                inst(IrOp::Label).imm = same;
            } else {
                IrInst& payload = inst(IrOp::Load);
                payload.d = localReg_[slot];
                payload.a = args;
                payload.imm = std::int64_t(slot) * 16 + kPayload;
                payload.width = 8;
            }
        }
        if (checksEntry) {
            inst(IrOp::ColdBegin);
            inst(IrOp::Label).imm = entryMiss;
            const std::int32_t r = fn_.newVreg(false);
            const std::int32_t t = fn_.newVreg(false);
            IrExit exit;
            exit.site = site();
            fn_.exits.push_back(std::move(exit));
            IrCall call;
            call.kind = IrCallKind::Helper;
            call.helper = reinterpret_cast<void*>(&scriptDynEntryMiss);
            call.args = {rtReg(), ptrReg(&code_), fnReg_, entryThisTag, entryThis, args, argc};
            call.result = r;
            call.resultTag = t;
            call.cold = true;
            call.publishFrame = true;
            call.site = site();
            call.exit = std::int32_t(fn_.exits.size()) - 1;
            fn_.calls.push_back(std::move(call));
            inst(IrOp::Call).imm = std::int64_t(fn_.calls.size()) - 1;
            IrInst& ret = inst(IrOp::Ret);
            ret.a = r;
            ret.b = t;
            inst(IrOp::ColdEnd);
        }

        // Parameters: a borrowed argument, or undefined. One the body assigns gets a count, as any local holds.
        for (std::uint32_t slot = 0; slot < slots; ++slot) {
            if (isNumberLocal(slot))
                continue;
            IrInst& tag = inst(IrOp::MovImm);
            tag.d = localTag_[slot];
            tag.imm = std::int64_t(Type::Undefined);
            IrInst& payload = inst(IrOp::MovImm);
            payload.d = localReg_[slot];
            payload.imm = 0;
            if (slot >= arity) {
                localOwned_[slot] = 1;
                continue;
            }
            const std::int32_t skip = newLabel();
            IrInst& test = inst(IrOp::BrCmp);
            test.a = argc;
            test.cond = IrCond::Ls;
            test.hasImm = true;
            test.imm = std::int64_t(slot);
            test.width = 64;
            test.target = skip;
            IrInst& loadTag = inst(IrOp::Load);
            loadTag.d = localTag_[slot];
            loadTag.a = args;
            loadTag.imm = std::int64_t(slot) * 16;
            loadTag.width = 1;
            IrInst& loadPayload = inst(IrOp::Load);
            loadPayload.d = localReg_[slot];
            loadPayload.a = args;
            loadPayload.imm = std::int64_t(slot) * 16 + kPayload;
            loadPayload.width = 8;
            inst(IrOp::Label).imm = skip;
            if (assigned_[slot]) {
                incRefOf(localReg_[slot], localTag_[slot], false);
                localOwned_[slot] = 1;
            }
        }
        // Cells start empty; a parameter a closure captures moves into one straight away.
        for (std::uint32_t slot = 0; slot < slots; ++slot) {
            cellReg_[slot] = fn_.newVreg(false);
            IrInst& none = inst(IrOp::MovImm);
            none.d = cellReg_[slot];
            none.imm = 0;
            cellOwned_[slot] = 1;
        }
        // `arguments` and a rest parameter are arrays made at entry, from the arguments as they came.
        if (code_.hasRest) {
            const std::int32_t rt = rtReg();
            const std::int32_t from = immReg(std::int64_t(code_.arity));
            const std::int32_t r = callCold(reinterpret_cast<void*>(&scriptDynArgsArray), {rt, args, argc, from}, true);
            const std::uint32_t slot = std::uint32_t(code_.arity);
            if (slot < slots) {
                IrInst& mov = inst(IrOp::Mov);
                mov.d = localReg_[slot];
                mov.a = r;
                IrInst& tag = inst(IrOp::MovImm);
                tag.d = localTag_[slot];
                tag.imm = std::int64_t(Type::Object);
                localOwned_[slot] = 1;
            } else {
                decRefOf(r, -1, false);
            }
        }
        if (code_.argumentsSlot != FunctionCode::kNoSlot && !argsLazy_) {
            const std::int32_t rt = rtReg();
            const std::int32_t from = immReg(0);
            const std::int32_t r = callCold(reinterpret_cast<void*>(&scriptDynArgsArray), {rt, args, argc, from}, true);
            const std::uint32_t slot = code_.argumentsSlot;
            if (code_.argumentsBoxed) {
                const std::int32_t tag = immReg(std::int64_t(Type::Object));
                const std::int32_t box = newBox(tag, r);
                decRefOf(r, -1, false);
                IrInst& mov = inst(IrOp::Mov);
                mov.d = cellReg_[slot];
                mov.a = box;
            } else {
                IrInst& mov = inst(IrOp::Mov);
                mov.d = localReg_[slot];
                mov.a = r;
                IrInst& tag = inst(IrOp::MovImm);
                tag.d = localTag_[slot];
                tag.imm = std::int64_t(Type::Object);
                localOwned_[slot] = 1;
            }
        }
        for (std::uint32_t slot : code_.boxedParams) {
            if (slot >= slots || isNumberLocal(slot))
                refuse("a boxed parameter out of the ordinary");
            const std::int32_t box = newBox(localTag_[slot], localReg_[slot]);
            IrInst& mov = inst(IrOp::Mov);
            mov.d = cellReg_[slot];
            mov.a = box;
        }

        // `this` is an object in any function that reads it, as a rule, and it does not change: asked once, here.
        code_.jitThisIsObject = false;
        if (betHere() && readsThis()) {
            const std::int32_t bail = newLabel();
            branchCmp(thisTag_, IrCond::Ne, std::int64_t(Type::Object), bail);
            const std::int32_t kind = load(thisReg_, kCellKindOffset, 1, false, kAliasHeader);
            branchCmp(kind, IrCond::Ne, std::int64_t(CellKind::Object), bail);
            emitBail(bail, {});
            thisIsCell_ = true;
            code_.jitThisIsObject = true;
        }
        walk();
        code_.jitNumberLocals.clear();
        code_.jitIntLocals.clear();
        for (std::uint32_t slot = 0; slot < slots; ++slot) {
            if (isNumberLocal(slot))
                code_.jitNumberLocals.push_back(slot);
            if (isIntLocal(slot))
                code_.jitIntLocals.push_back(slot);
        }
    }

    /// A callee inlined at a call in `parent`: nothing assumed, so nothing to bail, and no number locals.
    Lowering(const Lowering& parent, const FunctionCode& callee, const std::vector<std::uint8_t>& none,
             const std::vector<Entry>& held, std::int32_t site)
        : rt_(parent.rt_), code_(callee), fn_(parent.fn_), notNumber_(none), notInt_(none)
    {
        parent_ = &parent;
        parentHeld_ = &held;
        parentSite_ = site;
        depth_ = parent.depth_ + 1;
    }

    /// The body with `self` as this and `args` as the parameters, all borrowed from the caller, whose result ends up in
    /// `r` and `t` with a count of its own, and which then goes to `done`.
    void runInline(std::int32_t target, const Entry& self, const std::vector<Entry>& args, std::int32_t r,
                   std::int32_t t, std::int32_t done)
    {
        speculate_ = parent_->speculate_;
        prepare();
        inlineDone_ = done;
        inlineR_ = r;
        inlineT_ = t;
        fnReg_ = target;
        thisTag_ = fn_.newVreg(false);
        thisReg_ = fn_.newVreg(false);
        {
            const std::int32_t tag = tagReg(self);
            const std::int32_t payload = payloadReg(self);
            IrInst& mt = inst(IrOp::Mov);
            mt.d = thisTag_;
            mt.a = tag;
            IrInst& mp = inst(IrOp::Mov);
            mp.d = thisReg_;
            mp.a = payload;
        }
        if (code_.sloppyThis && readsThis())
            normalizeThis();
        const std::uint32_t arity = std::uint32_t(std::max(code_.arity, 0));
        const std::uint32_t slots = code_.numLocals;
        if (startShape_) {
            learnShape(thisReg_, startShape_);
            thisIsCell_ = true;
        } else if (parent_->thisIsCell_ && self.r == parent_->thisReg_ && self.rep == Rep::Tagged && !self.isConst) {
            thisIsCell_ = true;
        }
        localReg_.assign(slots, -1);
        localTag_.assign(slots, -1);
        localOwned_.assign(slots, 0);
        cellReg_.assign(slots, -1);
        cellOwned_.assign(slots, 0);
        numberParam_.assign(slots, 0);
        for (std::uint32_t slot = 0; slot < slots; ++slot) {
            localReg_[slot] = fn_.newVreg(false);
            localTag_[slot] = fn_.newVreg(false);
            std::int32_t tag = -1, payload = -1;
            if (slot < arity && slot < args.size()) {
                tag = tagReg(args[slot]);
                payload = payloadReg(args[slot]);
            }
            if (tag < 0) {
                IrInst& t0 = inst(IrOp::MovImm);
                t0.d = localTag_[slot];
                t0.imm = std::int64_t(Type::Undefined);
                IrInst& p0 = inst(IrOp::MovImm);
                p0.d = localReg_[slot];
                p0.imm = 0;
                localOwned_[slot] = 1;
                continue;
            }
            IrInst& mt = inst(IrOp::Mov);
            mt.d = localTag_[slot];
            mt.a = tag;
            IrInst& mp = inst(IrOp::Mov);
            mp.d = localReg_[slot];
            mp.a = payload;
            if (assigned_[slot]) {
                incRefOf(localReg_[slot], localTag_[slot], false);
                localOwned_[slot] = 1;
            }
        }
        for (std::uint32_t slot = 0; slot < slots; ++slot) {
            cellReg_[slot] = fn_.newVreg(false);
            IrInst& none = inst(IrOp::MovImm);
            none.d = cellReg_[slot];
            none.imm = 0;
            cellOwned_[slot] = 1;
        }
        walk();
    }

    /// `arguments` read nowhere but as the last argument of a call, in a function with no loop to enter part way through.
    bool onlyForwardsArguments() const
    {
        if (code_.argumentsSlot == FunctionCode::kNoSlot || code_.argumentsBoxed)
            return false;
        for (std::uint32_t pc = 0; pc < code_.code.size();) {
            const Op op = Op(code_.code[pc]);
            const std::uint32_t next = pc + 1 + std::uint32_t(operandsOf(op)) * 4;
            if (isBranch(op) && readOperand(code_.code, pc + 1) <= pc)
                return false;
            if (op == Op::SetLocal && readOperand(code_.code, pc + 1) == code_.argumentsSlot)
                return false;
            if (op == Op::GetLocal && readOperand(code_.code, pc + 1) == code_.argumentsSlot &&
                (next >= code_.code.size() || Op(code_.code[next]) != Op::Call))
                return false;
            pc = next;
        }
        return true;
    }

    bool readsThis() const
    {
        for (std::uint32_t pc = 0; pc < code_.code.size();) {
            const Op op = Op(code_.code[pc]);
            if (op == Op::PushThis)
                return true;
            pc += 1 + std::uint32_t(operandsOf(op)) * 4;
        }
        return false;
    }

    void normalizeThis()
    {
        const std::int32_t done = newLabel();
        branchCmp(thisTag_, IrCond::Hi, std::int64_t(Type::Null), done);
        IrInst& g = inst(IrOp::MovImm);
        g.d = thisReg_;
        g.imm = std::int64_t(reinterpret_cast<std::intptr_t>(rt_.globalObject.get()));
        IrInst& gt = inst(IrOp::MovImm);
        gt.d = thisTag_;
        gt.imm = std::int64_t(Type::Object);
        inst(IrOp::Label).imm = done;
        thisNormalized_ = true;
    }

    /// Small enough, simple enough, and loop-free: what a call may do in place of calling.
    static bool inlinable(const FunctionCode& c)
    {
        if (c.code.size() > 160 || !c.handlers.empty() || c.hasRest || c.argumentsSlot != FunctionCode::kNoSlot ||
            c.isClassConstructor || c.isDerivedConstructor || c.numLocals > 8 || !c.boxedParams.empty())
            return false;
        bool returns = false;
        for (std::uint32_t pc = 0; pc < c.code.size();) {
            const Op op = Op(c.code[pc]);
            switch (op) {
                case Op::Closure:
                case Op::GetCell:
                case Op::SetCell:
                case Op::NewCell:
                case Op::RenewCell:
                case Op::PushCallee:
                case Op::GetSuper:
                case Op::SetSuper:
                case Op::CallSuper:
                case Op::New:
                case Op::JumpSub:
                case Op::ForInStart:
                case Op::ForInNext:
                case Op::IterNext: return false;
                case Op::Return:
                case Op::ReturnUndefined: returns = true; break;
                default: break;
            }
            if (isBranch(op) && readOperand(c.code, pc + 1) <= pc)
                return false;
            pc += 1 + std::uint32_t(operandsOf(op)) * 4;
        }
        return returns;
    }

    /// Nothing in the function can run any other code or change anything the caller might be borrowing from: each read is
    /// one that hands over to the interpreter rather than call a getter.
    bool onlyReads(const FunctionCode& c) const
    {
        if (!speculate_ || std::find(c.jitBailedAt.begin(), c.jitBailedAt.end(), 0u) != c.jitBailedAt.end())
            return false;
        for (std::uint32_t pc = 0; pc < c.code.size();) {
            const Op op = Op(c.code[pc]);
            const std::uint32_t next = pc + 1 + std::uint32_t(operandsOf(op)) * 4;
            switch (op) {
                case Op::GetLocal:
                case Op::PushThis:
                case Op::PushConst:
                case Op::PushUndefined:
                case Op::PushNull:
                case Op::PushTrue:
                case Op::PushFalse:
                case Op::Pop:
                case Op::Jump:
                case Op::JumpIfFalse:
                case Op::JumpIfTrue:
                case Op::JumpIfFalseKeep:
                case Op::JumpIfTrueKeep:
                case Op::Return:
                case Op::ReturnUndefined:
                case Op::Not: break;
                case Op::GetProp: {
                    const std::uint32_t operand = readOperand(c.code, pc + 1);
                    const std::uint32_t index = operand >> kSiteShift;
                    if (index == kNoSite || index >= c.sites.size() || std::find(c.jitBailedAt.begin(), c.jitBailedAt.end(), pc) != c.jitBailedAt.end())
                        return false;
                    const PropertySite& site = c.sites[index];
                    if (!site.shape || site.getter || site.addTo || site.moreCount ||
                        (site.holder && site.epoch != rt_.ctx.protoEpoch) || c.atoms[operand & kAtomMask].get() == rt_.atom.length.get())
                        return false;
                    break;
                }
                default: return false;
            }
            pc = next;
        }
        return true;
    }

    /// Where an inlined lowering refused, everything it emitted goes.
    struct Mark
    {
        std::size_t code, calls, exits, sites, vregs;
        std::int32_t labels;
    };
    Mark mark() const
    {
        return {fn_.code.size(), fn_.calls.size(), fn_.exits.size(), fn_.sites.size(), fn_.vregIsFp.size(), fn_.labelCount};
    }
    void rollBack(const Mark& m)
    {
        fn_.code.resize(m.code);
        fn_.calls.resize(m.calls);
        fn_.exits.resize(m.exits);
        fn_.sites.resize(m.sites);
        fn_.vregIsFp.resize(m.vregs);
        fn_.labelCount = m.labels;
    }

    /// The callee's body in place of the call, when the callee is the function it was when this was compiled and that
    /// one is `inlinable`; anything else goes to `generic`. True when the body is there, ending in a branch to `done`.
    bool inlineCall(const Entry& callee, const Entry& self, const std::vector<Entry>& args, const std::vector<Entry>& held,
                    std::int32_t r, std::int32_t t, std::int32_t generic, std::int32_t done)
    {
        if (depth_ >= 3 || callee.rep != Rep::Tagged || callee.isConst)
            return false;
        if (!callee.hint)
            return inlineSeen(callee, self, args, held, r, t, generic, done);
        const auto* target = static_cast<const DynObject*>(callee.hint);
        if (target->kind != CellKind::Object || target->cls != ObjClass::Function)
            return false;
        const auto* fn = static_cast<const DynFunction*>(target);
        if (fn->fnKind != FnKind::Bytecode || !fn->code || fn->isClassConstructor || !inlinable(*fn->code))
            return false;
        // A recursive function goes in twice more: most of its calls become branches, and the last copy calls itself for real.
        std::uint32_t copies = 0;
        for (const Lowering* up = this; up; up = up->parent_) {
            if (&up->code_ == fn->code.get())
                ++copies;
        }
        if (copies > 2)
            return false;
        const Mark before = mark();
        const std::int32_t at = site();
        if (!callee.pinned) {
            if (!callee.hasKnownTag)
                branchCmp(callee.t, IrCond::Ne, std::int64_t(Type::Object), generic);
            const std::int32_t want = ptrReg(fn);
            IrInst& same = inst(IrOp::BrCmp);
            same.a = callee.r;
            same.b = want;
            same.cond = IrCond::Ne;
            same.width = 64;
            same.target = generic;
        }
        const std::vector<std::uint8_t> none(fn->code->numLocals + 1, 1);
        try {
            Lowering inner(*this, *fn->code, none, held, at);
            inner.runInline(ptrReg(fn), self, args, r, t, done);
            inlinedReturnTag_ = inner.returnTag_;
        } catch (const Refuse&) {
            rollBack(before);
            return false;
        }
        ++fn_.inlinedMethods;
        return true;
    }

    /// The same for a callee with nothing known of it but what this site has called while interpreted: a function of that
    /// code, whichever closure it is, reading its captured variables through what it was called as.
    bool inlineSeen(const Entry& callee, const Entry& self, const std::vector<Entry>& args, const std::vector<Entry>& held,
                    std::int32_t r, std::int32_t t, std::int32_t generic, std::int32_t done)
    {
        std::shared_ptr<const FunctionCode> code;
        for (const FunctionCode::CallTarget& known : code_.jitCallTargets) {
            if (known.pc == pc_ && known.seen)
                code = known.code.lock();
        }
        if (!code || code->isClassConstructor || !inlinable(*code))
            return false;
        std::uint32_t copies = 0;
        for (const Lowering* up = this; up; up = up->parent_) {
            if (&up->code_ == code.get())
                ++copies;
        }
        if (copies > 1)
            return false;
        const Mark before = mark();
        const std::int32_t at = site();
        if (!callee.hasKnownTag)
            branchCmp(callee.t, IrCond::Ne, std::int64_t(Type::Object), generic);
        const std::int32_t kind = load(callee.r, kCellKindOffset, 1, false, kAliasHeader);
        branchCmp(kind, IrCond::Ne, std::int64_t(CellKind::Object), generic);
        const std::int32_t cls = load(callee.r, kClsOffset, 1, false, kAliasHeader);
        branchCmp(cls, IrCond::Ne, std::int64_t(ObjClass::Function), generic);
        const std::int32_t have = load(callee.r, kFnCodeOffset, 8, false, kAliasHeader);
        const std::int32_t want = ptrReg(code.get());
        IrInst& same = inst(IrOp::BrCmp);
        same.a = have;
        same.b = want;
        same.cond = IrCond::Ne;
        same.width = 64;
        same.target = generic;
        const std::vector<std::uint8_t> none(code->numLocals + 1, 1);
        try {
            Lowering inner(*this, *code, none, held, at);
            inner.runInline(callee.r, self, args, r, t, done);
            inlinedReturnTag_ = inner.returnTag_;
        } catch (const Refuse&) {
            rollBack(before);
            return false;
        }
        ++fn_.inlinedMethods;
        return true;
    }

private:
    // ---- small things ----
    IrInst& inst(IrOp op)
    {
        fn_.code.emplace_back();
        fn_.code.back().op = op;
        return fn_.code.back();
    }
    std::int32_t newLabel() { return fn_.labelCount++; }
    void bindLabel(std::int32_t label)
    {
        inst(IrOp::Label).imm = label;
        nonNullLocals_.clear();
        forgetAtLabel();
    }
    void forgetFacts()
    {
        shapeFacts_.clear();
        epochKnown_ = false;
    }
    void forgetAtLabel()
    {
        forgetFacts();
        knownArrays_.clear();
    }
    const Shape* knownShape(std::int32_t vreg) const noexcept
    {
        for (const ShapeFact& f : shapeFacts_) {
            if (f.vreg == vreg)
                return f.shape;
        }
        return nullptr;
    }
    void learnShape(std::int32_t vreg, const Shape* shape)
    {
        forgetShape(vreg);
        shapeFacts_.push_back({vreg, shape});
    }
    void forgetShape(std::int32_t vreg)
    {
        knownArrays_.erase(std::remove(knownArrays_.begin(), knownArrays_.end(), vreg), knownArrays_.end());
        for (std::size_t i = 0; i < shapeFacts_.size(); ++i) {
            if (shapeFacts_[i].vreg == vreg) {
                shapeFacts_[i] = shapeFacts_.back();
                shapeFacts_.pop_back();
                return;
            }
        }
    }
    std::int32_t rtReg()
    {
        const std::int32_t d = fn_.newVreg(false);
        inst(IrOp::MovRuntime).d = d;
        return d;
    }
    std::int32_t immReg(std::int64_t v)
    {
        const std::int32_t d = fn_.newVreg(false);
        IrInst& in = inst(IrOp::MovImm);
        in.d = d;
        in.imm = v;
        return d;
    }
    std::int32_t ptrReg(const void* p) { return immReg(std::int64_t(reinterpret_cast<std::intptr_t>(p))); }
    std::int32_t fpImm(double v)
    {
        std::uint64_t bits = 0;
        std::memcpy(&bits, &v, sizeof bits);
        const std::int32_t d = fn_.newVreg(true);
        IrInst& in = inst(IrOp::FMovImm);
        in.d = d;
        in.imm = std::int64_t(bits);
        in.width = 64;
        return d;
    }
    void incRefOf(std::int32_t r, std::int32_t t, bool nullable)
    {
        IrInst& in = inst(t >= 0 ? IrOp::IncRefTagged : IrOp::IncRef);
        in.a = r;
        in.b = t;
        in.isNullable = nullable;
    }
    void decRefOf(std::int32_t r, std::int32_t t, bool nullable)
    {
        IrInst& in = inst(t >= 0 ? IrOp::DecRefTagged : IrOp::DecRef);
        in.a = r;
        in.b = t;
        in.isNullable = nullable;
    }
    void mov(std::int32_t d, std::int32_t a)
    {
        IrInst& in = inst(IrOp::Mov);
        in.d = d;
        in.a = a;
    }
    std::int32_t load(std::int32_t base, std::int64_t offset, std::uint8_t bytes, bool fp = false, std::uint8_t alias = 0)
    {
        const std::int32_t d = fn_.newVreg(fp);
        IrInst& in = inst(IrOp::Load);
        in.d = d;
        in.a = base;
        in.imm = offset;
        in.width = bytes;
        in.alias = alias;
        return d;
    }
    /// Values: a property, an element, a box, a frame's slot. Never an object's shape or the vectors it keeps its
    /// properties and elements in, which `storeHeader` writes.
    void store(std::int32_t base, std::int64_t offset, std::int32_t value, std::uint8_t bytes, std::uint8_t alias = kAliasValue)
    {
        IrInst& in = inst(IrOp::Store);
        in.a = base;
        in.c = value;
        in.imm = offset;
        in.width = bytes;
        in.alias = alias;
    }
    /// An object's shape, or where its properties or elements are: what only a call or an append changes, so a value
    /// stored at the same offset elsewhere does not make the optimizer read it again.
    std::int32_t loadHeader(std::int32_t base, std::int64_t offset)
    {
        return load(base, offset, 8, false, kAliasHeader);
    }
    void storeHeader(std::int32_t base, std::int64_t offset, std::int32_t value)
    {
        store(base, offset, value, 8, kAliasHeader);
    }
    void branchCmp(std::int32_t a, IrCond cond, std::int64_t imm, std::int32_t target, std::uint8_t width = 32)
    {
        IrInst& in = inst(IrOp::BrCmp);
        in.a = a;
        in.cond = cond;
        in.hasImm = true;
        in.imm = imm;
        in.width = width;
        in.target = target;
    }
    std::int32_t site()
    {
        IrSite entry;
        entry.method = &code_;
        entry.pc = pc_;
        entry.parent = parentSite_;
        if (!fn_.sites.empty() && fn_.sites.back().pc == pc_ && fn_.sites.back().method == &code_ &&
            fn_.sites.back().parent == parentSite_)
            return std::int32_t(fn_.sites.size()) - 1;
        fn_.sites.push_back(entry);
        return std::int32_t(fn_.sites.size()) - 1;
    }

    // ---- the code ----
    void prepare()
    {
        const std::vector<std::uint8_t>& c = code_.code;
        labels_.assign(c.size() + 1, -1);
        joins_.assign(c.size() + 1, {});
        assigned_.assign(code_.numLocals, 0);
        reachable_.assign(c.size() + 1, 0);
        for (std::size_t pc = 0; pc < c.size();) {
            const Op op = Op(c[pc]);
            if (op == Op::SetLocal)
                assigned_[readOperand(c, pc + 1)] = 1;
            if (isBranch(op)) {
                const std::uint32_t target = readOperand(c, pc + 1);
                if (labels_[target] < 0)
                    labels_[target] = newLabel();
            }
            if (op == Op::JumpSub || op == Op::EndFinally)
                refuse("a finally block");
            pc += 1 + std::size_t(operandsOf(op)) * 4;
        }
        std::vector<std::uint32_t> work{0};
        while (!work.empty()) {
            std::uint32_t pc = work.back();
            work.pop_back();
            while (pc < c.size() && !reachable_[pc]) {
                reachable_[pc] = 1;
                const Op op = Op(c[pc]);
                if (op == Op::Return || op == Op::ReturnUndefined || op == Op::Throw)
                    break;
                if (op == Op::Jump) {
                    pc = readOperand(c, pc + 1);
                    continue;
                }
                if (isBranch(op))
                    work.push_back(readOperand(c, pc + 1));
                pc += 1 + std::uint32_t(operandsOf(op)) * 4;
            }
        }
    }

    /// A number local guessed to hold only int32 values, in a general register; anything else stored to it bails, or
    /// when that is known at compile time, makes it a double local.
    bool isIntLocal(std::uint32_t slot) const noexcept
    {
        if (!speculate_ || notInt_[slot] || !isNumberLocal(slot))
            return false;
        return slot >= std::uint32_t(std::max(code_.arity, 0)) || (intWork_ && !((code_.argsNotInt >> slot) & 1));
    }

    bool isNumberLocal(std::uint32_t slot) const noexcept
    {
        if (notNumber_[slot])
            return false;
        if (slot < std::uint32_t(std::max(code_.arity, 0)) && !(slot < numberParam_.size() && numberParam_[slot]))
            return false;
        // What the entry sequence fills in is an object.
        if (code_.hasRest && slot == std::uint32_t(code_.arity))
            return false;
        return slot != code_.argumentsSlot;
    }

    // ---- entries ----
    Entry doubleEntry(std::int32_t r)
    {
        Entry e;
        e.rep = Rep::Double;
        e.r = r;
        return e;
    }
    Entry intEntry(std::int32_t r, bool wide = false)
    {
        Entry e;
        e.rep = Rep::Int32;
        e.r = r;
        e.wide = wide;
        return e;
    }
    Entry constDouble(double v)
    {
        Entry e;
        e.rep = Rep::Double;
        e.isConst = true;
        e.number = v;
        return e;
    }
    Entry constTagged(Type tag, std::uint64_t bits = 0)
    {
        Entry e;
        e.isConst = true;
        e.hasKnownTag = true;
        e.known = tag;
        e.bits = bits;
        e.own = tag >= Type::String ? Own::Immortal : Own::None;
        e.nonNull = true;
        return e;
    }
    Entry boolEntry(std::int32_t r)
    {
        Entry e;
        e.r = r;
        e.hasKnownTag = true;
        e.known = Type::Bool;
        return e;
    }
    Entry taggedEntry(std::int32_t r, std::int32_t t, Own own)
    {
        Entry e;
        e.r = r;
        e.t = t;
        e.own = own;
        return e;
    }
    /// An owned object fresh from a helper.
    Entry objectEntry(std::int32_t r)
    {
        Entry e;
        e.r = r;
        e.hasKnownTag = true;
        e.known = Type::Object;
        e.own = Own::Owned;
        e.nonNull = true;
        return e;
    }

    void push(const Entry& e) { stack_.push_back(e); }
    Entry pop()
    {
        if (stack_.empty())
            refuse("the evaluation stack underflows");
        Entry e = stack_.back();
        stack_.pop_back();
        return e;
    }
    Entry& top(std::size_t fromTop = 0)
    {
        if (stack_.size() <= fromTop)
            refuse("the evaluation stack underflows");
        return stack_[stack_.size() - 1 - fromTop];
    }

    std::int32_t tagReg(const Entry& e)
    {
        if (e.rep != Rep::Tagged)
            return immReg(std::int64_t(Type::Double));
        return e.hasKnownTag ? immReg(std::int64_t(e.known)) : e.t;
    }
    /// The entry as payload bits.
    std::int32_t payloadReg(const Entry& e)
    {
        if (e.rep == Rep::Tagged) {
            if (e.isConst)
                return immReg(std::int64_t(e.bits));
            return e.r;
        }
        if (e.isConst) {
            std::uint64_t bits = 0;
            std::memcpy(&bits, &e.number, sizeof bits);
            return immReg(std::int64_t(bits));
        }
        const std::int32_t fp = doubleReg(e);
        const std::int32_t d = fn_.newVreg(false);
        IrInst& in = inst(IrOp::FpToBits);
        in.d = d;
        in.a = fp;
        in.width = 64;
        return d;
    }
    /// An entry known to be a number, as a double in a register.
    std::int32_t doubleReg(const Entry& e)
    {
        if (e.isConst)
            return fpImm(e.number);
        if (e.rep == Rep::Double)
            return e.r;
        if (e.rep == Rep::Int32 || e.rep == Rep::Exact) {
            const std::int32_t d = fn_.newVreg(true);
            IrInst& in = inst(IrOp::IntToFp);
            in.d = d;
            in.a = e.r;
            in.srcWidth = e.rep == Rep::Exact ? 64 : 32;
            in.width = 64;
            return d;
        }
        const std::int32_t d = fn_.newVreg(true);
        IrInst& in = inst(IrOp::BitsToFp);
        in.d = d;
        in.a = e.r;
        in.width = 64;
        return d;
    }

    bool isKnownNumber(const Entry& e) const noexcept
    {
        return e.rep != Rep::Tagged || (e.hasKnownTag && e.known == Type::Double);
    }

    /// `ToNumber`, as a double: an inline test when the entry could be anything.
    std::int32_t toNumberReg(const Entry& e)
    {
        if (isKnownNumber(e))
            return doubleReg(e);
        const std::int32_t tag = tagReg(e);
        const std::int32_t payload = payloadReg(e);
        const std::int32_t d = fn_.newVreg(true);
        const std::int32_t slow = newLabel();
        const std::int32_t done = newLabel();
        branchCmp(tag, IrCond::Ne, std::int64_t(Type::Double), slow);
        IrInst& bits = inst(IrOp::BitsToFp);
        bits.d = d;
        bits.a = payload;
        bits.width = 64;
        inst(IrOp::ColdBegin);
        inst(IrOp::Label).imm = slow;
        const std::int32_t rt = rtReg();
        coldCall(reinterpret_cast<void*>(&scriptDynToNumber), {rt, tag, payload}, d, -1, true);
        inst(IrOp::Br).imm = done;
        inst(IrOp::ColdEnd);
        inst(IrOp::Label).imm = done;
        return d;
    }

    /// `ToInt32` of a double.
    std::int32_t int32OfDouble(std::int32_t fp)
    {
        const std::int32_t raw = fn_.newVreg(false);
        if (rt_.jitBackend->hasJsToInt32()) {
            IrInst& conv = inst(IrOp::FpToInt32Js);
            conv.d = raw;
            conv.a = fp;
            return raw;
        }
        IrInst& conv = inst(IrOp::FpToIntRaw);
        conv.d = raw;
        conv.a = fp;
        // Anything within 62 bits truncated correctly and its low word is the answer. Otherwise the instruction may have
        // saturated, which is not the modular arithmetic the language wants.
        const std::int32_t high = fn_.newVreg(false);
        IrInst& sar = inst(IrOp::Sar);
        sar.d = high;
        sar.a = raw;
        sar.hasImm = true;
        sar.imm = 62;
        sar.width = 64;
        const std::int32_t bias = fn_.newVreg(false);
        IrInst& add = inst(IrOp::Add);
        add.d = bias;
        add.a = high;
        add.hasImm = true;
        add.imm = 1;
        add.width = 64;
        const std::int32_t slow = newLabel();
        const std::int32_t done = newLabel();
        branchCmp(bias, IrCond::Hi, 1, slow, 64);
        inst(IrOp::ColdBegin);
        inst(IrOp::Label).imm = slow;
        coldCall(reinterpret_cast<void*>(&scriptDynToInt32), {fp}, raw, -1, false);
        inst(IrOp::Br).imm = done;
        inst(IrOp::ColdEnd);
        inst(IrOp::Label).imm = done;
        return raw;
    }

    /// An int32, or a constant that is one: what int32 arithmetic takes. Minus zero is not one.
    static bool isIntLike(const Entry& e) noexcept
    {
        if (e.rep == Rep::Int32)
            return true;
        return e.isConst && e.rep == Rep::Double && e.number >= -2147483648.0 && e.number <= 2147483647.0 &&
               double(std::int32_t(e.number)) == e.number && !(e.number == 0 && std::signbit(e.number));
    }
    std::int32_t intReg(const Entry& e)
    {
        if (e.isConst)
            return immReg(std::int64_t(std::int32_t(e.number)));
        return e.r;
    }

    std::int32_t toInt32Reg(const Entry& e)
    {
        if (e.rep == Rep::Exact)
            return e.r;
        if (e.rep == Rep::Int32) {
            if (e.isConst)
                return immReg(std::int64_t(std::int32_t(e.number)));
            return e.r;
        }
        return int32OfDouble(toNumberReg(e));
    }

    // ---- ownership ----
    void makeOwned(Entry& e)
    {
        if (e.isConst && e.rep == Rep::Tagged && e.hasKnownTag && e.known >= Type::String) {
            // A string constant the code keeps alive, on its way to somewhere that holds a count.
            e.r = immReg(std::int64_t(e.bits));
            e.isConst = false;
            incRefOf(e.r, -1, false);
            e.own = Own::Owned;
            return;
        }
        if (e.isConst || e.own == Own::Owned || e.own == Own::None || e.rep != Rep::Tagged)
            return;
        if (e.hasKnownTag && e.known < Type::String)
            return;
        const std::int32_t copy = fn_.newVreg(false);
        IrInst& mov = inst(IrOp::Mov);
        mov.d = copy;
        mov.a = e.r;
        std::int32_t tag = -1;
        if (!e.hasKnownTag) {
            tag = fn_.newVreg(false);
            IrInst& tagMov = inst(IrOp::Mov);
            tagMov.d = tag;
            tagMov.a = e.t;
            e.t = tag;
        }
        incRefOf(copy, tag, false);
        e.r = copy;
        e.own = Own::Owned;
        e.local = -1;
    }
    void secureWeak()
    {
        for (Entry& e : stack_) {
            if (e.own == Own::Weak)
                makeOwned(e);
        }
    }
    /// Before an operation on these: what reads a borrowed value has to own it first, unless the operation can run none of
    /// the program's code, which numbers guarantee, so nothing can change where a borrowed value is.
    void secureWeakFor(const Entry& a, const Entry& b = Entry::someNumber())
    {
        if (!isKnownNumber(a) || !isKnownNumber(b))
            secureWeak();
    }
    void release(const Entry& e)
    {
        if (e.own != Own::Owned || e.isConst || e.rep != Rep::Tagged || (e.hasKnownTag && e.known < Type::String))
            return;
        secureWeak();
        decRefOf(e.r, e.hasKnownTag ? -1 : e.t, false);
    }

    /// A local is about to change: anything reading it keeps the old value.
    void copyOutReaders(std::uint32_t slot)
    {
        for (Entry& e : stack_) {
            if (e.local != std::int32_t(slot))
                continue;
            if (e.rep == Rep::Tagged) {
                makeOwned(e);
            } else {
                const std::int32_t d = fn_.newVreg(e.rep == Rep::Double);
                IrInst& mov = inst(IrOp::Mov);
                mov.d = d;
                mov.a = e.r;
                e.r = d;
                e.local = -1;
            }
        }
    }

    // ---- exits ----
    std::int32_t makeExit(std::uint32_t code, const std::vector<Entry>& popped = {})
    {
        IrExit exit;
        exit.code = code;
        exit.site = site();
        const auto add = [&exit](const Entry& e) {
            if (e.own != Own::Owned || e.isConst || e.rep != Rep::Tagged)
                return;
            if (e.hasKnownTag)
                exit.cleanup.push_back(e.r);
            else
                exit.cleanupTagged.push_back({e.r, e.t});
        };
        for (std::size_t slot = 0; slot < localReg_.size(); ++slot) {
            if (localOwned_[slot] && localTag_[slot] >= 0)
                exit.cleanupTagged.push_back({localReg_[slot], localTag_[slot]});
            if (cellOwned_[slot] && cellReg_[slot] >= 0)
                exit.cleanup.push_back(cellReg_[slot]);
        }
        for (const Entry& e : stack_)
            add(e);
        for (const Entry& e : popped)
            add(e);
        // Code inlined into a caller unwinds the caller's frame too.
        if (parent_)
            parent_->addCleanup(exit, *parentHeld_);
        fn_.maxCleanup = std::max(fn_.maxCleanup, std::uint32_t(exit.cleanup.size() + exit.cleanupTagged.size()));
        fn_.exits.push_back(std::move(exit));
        return std::int32_t(fn_.exits.size()) - 1;
    }
    /// What this frame owns, and `held` besides, for an exit taken from code inlined into it.
    void addCleanup(IrExit& exit, const std::vector<Entry>& held) const
    {
        const auto add = [&exit](const Entry& e) {
            if (e.own != Own::Owned || e.isConst || e.rep != Rep::Tagged)
                return;
            if (e.hasKnownTag)
                exit.cleanup.push_back(e.r);
            else
                exit.cleanupTagged.push_back({e.r, e.t});
        };
        for (std::size_t slot = 0; slot < localReg_.size(); ++slot) {
            if (localOwned_[slot] && localTag_[slot] >= 0)
                exit.cleanupTagged.push_back({localReg_[slot], localTag_[slot]});
            if (cellOwned_[slot] && cellReg_[slot] >= 0)
                exit.cleanup.push_back(cellReg_[slot]);
        }
        for (const Entry& e : stack_)
            add(e);
        for (const Entry& e : held)
            add(e);
        if (parent_)
            parent_->addCleanup(exit, *parentHeld_);
    }

    // ---- bailing out ----
    /// The block `label` names gives the call to the interpreter, which runs the instruction at `pc_` again with `popped`
    /// back where they were on its stack: everything the frame holds goes with it, so nothing is released here. Only for
    /// code that has done nothing yet that the instruction would do over.
    void emitBail(std::int32_t label, const std::vector<Entry>& popped)
    {
        inst(IrOp::ColdBegin);
        inst(IrOp::Label).imm = label;
        inst(IrOp::SaveRegs);
        // A table says what is where, and nothing is written out until it is needed.
        const std::uint32_t locals = std::uint32_t(localReg_.size());
        auto owned = std::make_unique<DeoptInfo>();
        DeoptInfo* info = owned.get();
        outermost().jitDeopts.push_back(std::move(owned));
        info->code = &code_;
        info->outer = &outermost();
        info->pc = pc_;
        info->stack = std::uint32_t(stack_.size() + popped.size());
        std::vector<std::int32_t> places;
        const auto at = [&](std::int32_t vreg) {
            places.push_back(vreg);
            return std::int32_t(places.size()) - 1;
        };
        info->fnAt = at(fnReg_);
        info->thisTagAt = at(thisTag_);
        info->thisAt = at(thisReg_);
        info->values.reserve(2 * locals + info->stack);
        for (std::uint32_t slot = 0; slot < locals; ++slot) {
            DeoptValue v;
            if (argsLazy_ && slot == code_.argumentsSlot) {
                v.form = DeoptValue::Arguments;
                v.tag = std::uint8_t(Type::Object);
                v.owned = 1;
                v.tagAt = -1;
                v.payloadAt = at(argsReg_);
                v.bits = std::uint64_t(at(argcReg_));
            } else if (isNumberLocal(slot)) {
                v.tag = std::uint8_t(Type::Double);
                v.form = isIntLocal(slot) ? DeoptValue::Int64 : DeoptValue::FpBits;
                v.payloadAt = at(localReg_[slot]);
            } else {
                v.tagAt = at(localTag_[slot]);
                v.owned = localOwned_[slot];
                v.payloadAt = at(localReg_[slot]);
            }
            info->values.push_back(v);
        }
        for (std::uint32_t slot = 0; slot < locals; ++slot) {
            DeoptValue v;
            v.owned = cellOwned_[slot];
            v.payloadAt = at(cellReg_[slot]);
            info->values.push_back(v);
        }
        const auto put = [&](const Entry& e) {
            DeoptValue v;
            if (e.lazyArgs) {
                v.form = DeoptValue::Arguments;
                v.tag = std::uint8_t(Type::Object);
                v.owned = 1;
                v.payloadAt = at(argsReg_);
                v.bits = std::uint64_t(at(argcReg_));
            } else if (e.rep != Rep::Tagged) {
                v.tag = std::uint8_t(Type::Double);
                if (e.isConst) {
                    v.form = DeoptValue::Constant;
                    std::memcpy(&v.bits, &e.number, sizeof v.bits);
                } else {
                    v.form = e.rep == Rep::Double ? DeoptValue::FpBits : e.rep == Rep::Exact ? DeoptValue::Int64 : DeoptValue::Int32;
                    v.payloadAt = at(e.r);
                }
            } else {
                if (e.hasKnownTag || e.isConst)
                    v.tag = std::uint8_t(e.known);
                else
                    v.tagAt = at(e.t);
                v.owned = e.own == Own::Owned && !e.isConst ? 1 : 0;
                if (e.isConst) {
                    v.form = DeoptValue::Constant;
                    v.bits = e.bits;
                } else {
                    v.payloadAt = at(e.r);
                }
            }
            info->values.push_back(v);
        };
        for (const Entry& e : stack_)
            put(e);
        for (const Entry& e : popped)
            put(e);
        const std::int32_t saved = fn_.newVreg(false);
        IrInst& addr = inst(IrOp::FrameAddr);
        addr.d = saved;
        addr.imm = 0;
        const std::int32_t r = fn_.newVreg(false);
        const std::int32_t t = fn_.newVreg(false);
        IrExit exit;
        exit.site = site();
        // Inlined, the interpreter finishes this call alone, and the caller's frame is still ours to unwind.
        if (parent_)
            parent_->addCleanup(exit, *parentHeld_);
        fn_.maxCleanup = std::max(fn_.maxCleanup, std::uint32_t(exit.cleanup.size() + exit.cleanupTagged.size()));
        fn_.exits.push_back(std::move(exit));
        IrCall call;
        call.kind = IrCallKind::Helper;
        call.helper = reinterpret_cast<void*>(&scriptDynBail);
        call.args = {rtReg(), ptrReg(info), saved};
        call.abiArgs = 3;
        call.readsPlaces = info;
        call.args.insert(call.args.end(), places.begin(), places.end());
        call.result = r;
        call.resultTag = t;
        call.cold = true;
        call.publishFrame = true;
        call.site = site();
        call.exit = std::int32_t(fn_.exits.size()) - 1;
        fn_.calls.push_back(std::move(call));
        inst(IrOp::Call).imm = std::int64_t(fn_.calls.size()) - 1;
        if (inlineDone_ >= 0) {
            noteReturn(-1);
            // What the call came to, for the caller to carry on with in native code.
            IrInst& movTag = inst(IrOp::Mov);
            movTag.d = inlineT_;
            movTag.a = t;
            IrInst& mov = inst(IrOp::Mov);
            mov.d = inlineR_;
            mov.a = r;
            inst(IrOp::Br).imm = inlineDone_;
        } else {
            IrInst& ret = inst(IrOp::Ret);
            ret.a = r;
            ret.b = t;
        }
        // The fast path carries on from here; the optimizer knows nothing worked out in the block above is there for it.
        inst(IrOp::ColdEnd);
    }

    /// Arithmetic and comparisons on operands of unknown type: numbers is the guess, checked once here, so what follows is
    /// the code for numbers alone. An operand known to be something else, a string above all, makes no guess.
    void expectNumbers(Op op)
    {
        std::size_t n = 0;
        switch (op) {
            case Op::Add:
            case Op::Sub:
            case Op::Mul:
            case Op::Div:
            case Op::Mod:
            case Op::BitAnd:
            case Op::BitOr:
            case Op::BitXor:
            case Op::Shl:
            case Op::Shr:
            case Op::UShr:
            case Op::Lt:
            case Op::Le:
            case Op::Gt:
            case Op::Ge: n = 2; break;
            case Op::Neg:
            case Op::Pos:
            case Op::BitNot:
            case Op::Inc:
            case Op::Dec:
            case Op::ToNumber: n = 1; break;
            default: return;
        }
        if (stack_.size() < n)
            return;
        bool any = false;
        for (std::size_t i = 0; i < n; ++i) {
            const Entry& e = top(i);
            if (e.rep == Rep::Tagged && (e.hasKnownTag || e.isConst) && e.known != Type::Double)
                return;
            any = any || (e.rep == Rep::Tagged && !e.isConst && !e.hasKnownTag);
        }
        if (!any)
            return;
        const std::int32_t bail = newLabel();
        for (std::size_t i = 0; i < n; ++i) {
            const Entry& e = top(i);
            if (e.rep == Rep::Tagged && !e.isConst && !e.hasKnownTag)
                branchCmp(e.t, IrCond::Ne, std::int64_t(Type::Double), bail);
        }
        emitBail(bail, {});
        for (std::size_t i = 0; i < n; ++i) {
            Entry& e = top(i);
            if (e.rep != Rep::Tagged || e.isConst || e.hasKnownTag)
                continue;
            // A number holds no count, and a read of a local is a copy from here on, since the local may change first.
            const std::int32_t bits = fn_.newVreg(false);
            IrInst& mov = inst(IrOp::Mov);
            mov.d = bits;
            mov.a = e.r;
            e = taggedEntry(bits, -1, Own::None);
            e.hasKnownTag = true;
            e.known = Type::Double;
            e.nonNull = true;
        }
    }

    /// Bails to `bail` unless `base` is an object of `shape`, which is known from then on.
    void expectShape(const Entry& base, const Shape* shape, std::int32_t bail)
    {
        if (knownShape(base.r) == shape)
            return;
        guardShape(base, shape, bail);
        learnShape(base.r, shape);
    }
    const FunctionCode& outermost() const noexcept { return parent_ ? parent_->outermost() : code_; }

    /// Whether to assume what the interpreter has seen at this instruction: not where that has already been wrong.
    bool betHere() const
    {
        return speculate_ && std::find(code_.jitBailedAt.begin(), code_.jitBailedAt.end(), pc_) == code_.jitBailedAt.end();
    }
    void expectEpoch(std::uint64_t epoch, std::int32_t bail)
    {
        if (epochKnown_)
            return;
        guardEpoch(epoch, bail);
        epochKnown_ = true;
    }

    // ---- calling helpers ----
    /// A helper that keeps the caller's registers: it costs saving what is live, and nothing when it is not reached.
    std::int32_t callCold(void* helper, std::vector<std::int32_t> args, bool hasResult, const std::vector<Entry>& popped = {},
                          bool canRaise = true)
    {
        const std::int32_t result = hasResult ? fn_.newVreg(false) : -1;
        coldCall(helper, std::move(args), result, -1, canRaise, popped);
        return result;
    }
    void coldCall(void* helper, std::vector<std::int32_t> args, std::int32_t result, std::int32_t resultTag, bool canRaise,
                  const std::vector<Entry>& popped = {})
    {
        IrCall call;
        call.kind = IrCallKind::Helper;
        call.helper = helper;
        call.args = std::move(args);
        call.result = result;
        call.resultTag = resultTag;
        call.cold = true;
        if (canRaise) {
            forgetFacts();
            call.publishFrame = true;
            call.site = site();
            call.exit = makeExit(0, popped);
        }
        fn_.calls.push_back(std::move(call));
        inst(IrOp::Call).imm = std::int64_t(fn_.calls.size()) - 1;
    }

    // ---- joins ----
    void flowTo(std::uint32_t target)
    {
        Join& join = joins_[target];
        if (!join.seen) {
            join.seen = true;
            join.sameFunction.assign(stack_.size(), nullptr);
            join.sameTag.assign(stack_.size(), -1);
            for (std::size_t i = 0; i < stack_.size(); ++i) {
                if (stack_[i].pinned && stack_[i].rep == Rep::Tagged && !stack_[i].isConst)
                    join.sameFunction[i] = stack_[i].hint;
                join.sameTag[i] = knownTagOf(stack_[i]);
            }
            join.entries.resize(stack_.size());
            for (Entry& canonical : join.entries) {
                canonical = Entry();
                canonical.r = fn_.newVreg(false);
                canonical.t = fn_.newVreg(false);
                canonical.own = Own::Owned;
            }
        } else if (join.entries.size() != stack_.size()) {
            refuse("the evaluation stack is inconsistent at a join");
        } else {
            for (std::size_t i = 0; i < stack_.size(); ++i) {
                if (!stack_[i].pinned || stack_[i].hint != join.sameFunction[i])
                    join.sameFunction[i] = nullptr;
                if (knownTagOf(stack_[i]) != join.sameTag[i])
                    join.sameTag[i] = -1;
            }
        }
        for (std::size_t i = 0; i < stack_.size(); ++i) {
            Entry& e = stack_[i];
            const Entry& canonical = join.entries[i];
            if (e.r == canonical.r && e.t == canonical.t && !e.isConst)
                continue;
            makeOwned(e);
            const std::int32_t tag = tagReg(e);
            const std::int32_t payload = payloadReg(e);
            IrInst& movTag = inst(IrOp::Mov);
            movTag.d = canonical.t;
            movTag.a = tag;
            IrInst& mov = inst(IrOp::Mov);
            mov.d = canonical.r;
            mov.a = payload;
            // A branch carries on here too, with the same value in the join's registers: what is known of it still is.
            const bool pinned = e.pinned;
            const GCObject* hint = e.hint;
            e = canonical;
            if (pinned) {
                e.pinned = true;
                e.hint = hint;
                e.hasKnownTag = true;
                e.known = Type::Object;
            }
        }
    }
    void enterJoin(std::uint32_t pc, bool fallsIn)
    {
        if (fallsIn)
            flowTo(pc);
        bindLabel(labels_[pc]);
        if (joins_[pc].seen)
            stack_ = joins_[pc].entries;
        else
            stack_.clear();
        // A function every way in had at this place is that function here too: `new F(a, c ? x : new F(...))` keeps
        // knowing which `F` it calls. Each way in counted it, so the entry stays owned. Not at a loop head, which a way in
        // reaches only after this.
        if (joins_[pc].seen && !loopHead_[pc]) {
            // A tag every way in knew is known here: what `a || b` of two booleans is, say.
            for (std::size_t i = 0; i < stack_.size() && i < joins_[pc].sameTag.size(); ++i) {
                if (joins_[pc].sameTag[i] >= 0) {
                    stack_[i].hasKnownTag = true;
                    stack_[i].known = Type(joins_[pc].sameTag[i]);
                    if (stack_[i].known < Type::String)
                        stack_[i].own = Own::None;
                }
            }
            for (std::size_t i = 0; i < stack_.size() && i < joins_[pc].sameFunction.size(); ++i) {
                if (const void* fn = joins_[pc].sameFunction[i]) {
                    stack_[i].pinned = true;
                    stack_[i].hint = static_cast<const GCObject*>(fn);
                    stack_[i].hasKnownTag = true;
                    stack_[i].known = Type::Object;
                }
            }
        }
    }

    // ---- the walk ----
    void walk()
    {
        const std::vector<std::uint8_t>& c = code_.code;
        std::vector<std::uint8_t>& loopHead = loopHead_;
        loopHead.assign(c.size() + 1, 0);
        for (std::size_t pc = 0; pc < c.size();) {
            const Op op = Op(c[pc]);
            if (isBranch(op) && reachable_[pc] && readOperand(c, pc + 1) <= pc)
                loopHead[readOperand(c, pc + 1)] = 1;
            pc += 1 + std::size_t(operandsOf(op)) * 4;
        }
        bool fallsIn = true;
        for (std::uint32_t pc = 0; pc < c.size();) {
            const Op op = Op(c[pc]);
            const std::uint32_t next = pc + 1 + std::uint32_t(operandsOf(op)) * 4;
            if (!reachable_[pc]) {
                pc = next;
                fallsIn = false;
                continue;
            }
            pc_ = pc;
            if (labels_[pc] >= 0) {
                previousOp_ = Op::Nop;
                // A loop entered from above is entered through what it can then count on all the way round; so is it when
                // the interpreter hands over, which is why that comes in ahead of it too.
                std::int32_t entry = labels_[pc];
                if (loopHead[pc] && fallsIn && stack_.empty()) {
                    entry = newLabel();
                    bindLabel(entry);
                    checkBeforeLoop(pc);
                }
                enterJoin(pc, fallsIn);
                if (loopHead[pc] && stack_.empty())
                    addOsrEntry(pc, entry);
            }
            fallsIn = true;
            if (betHere())
                expectNumbers(op);
            // A comparison of numbers followed by a branch on it fuses.
            const bool comparison = op == Op::Lt || op == Op::Le || op == Op::Gt || op == Op::Ge || op == Op::StrictEq ||
                                    op == Op::StrictNe || op == Op::Eq || op == Op::Ne;
            if (comparison && next < c.size() && labels_[next] < 0 && stack_.size() == 2 && isKnownNumber(top(0)) &&
                isKnownNumber(top(1))) {
                const Op after = Op(c[next]);
                if (after == Op::JumpIfFalse || after == Op::JumpIfTrue) {
                    fusedCompare(op, after == Op::JumpIfTrue, readOperand(c, next + 1), pc);
                    previousOp_ = Op::Nop;
                    pc = next + 5;
                    continue;
                }
            }
            // `x = e;` as a statement: the copy the expression's value would be is never made.
            if (op == Op::Dup && next + 6 <= c.size() && Op(c[next]) == Op::SetLocal && Op(c[next + 5]) == Op::Pop &&
                labels_[next] < 0 && labels_[next + 5] < 0) {
                // Not the `PushUndefined; SetLocal` of a declaration, which `storeLocal` treats as one.
                previousOp_ = Op::Dup;
                settleExact(Op::SetLocal, next);
                storeLocal(readOperand(c, next + 1));
                previousOp_ = Op::Pop;
                pc = next + 6;
                continue;
            }
            // `this` made in place stays as it was made while nothing could have seen it: what is done here is store
            // locals, constants and their sums into it.
            if (freshThis_) {
                switch (op) {
                    case Op::PushThis:
                    case Op::GetLocal:
                    case Op::PushConst:
                    case Op::PushUndefined:
                    case Op::PushNull:
                    case Op::PushTrue:
                    case Op::PushFalse:
                    case Op::Pop:
                    case Op::Add:
                    case Op::Sub:
                    case Op::Mul: break;
                    case Op::SetProp:
                        if (stack_.size() >= 2 && stack_[stack_.size() - 2].r == thisReg_ && stack_.back().r != thisReg_)
                            break;
                        freshThis_ = false;
                        break;
                    default: freshThis_ = false; break;
                }
            }
            // These change an object under construction behind the back of what `fresh` says about it.
            switch (op) {
                case Op::DefMethod:
                case Op::DefGetter:
                case Op::DefSetter:
                case Op::DefElem:
                case Op::DefMethodElem:
                case Op::ObjectSpread:
                    for (Entry& e : stack_)
                        e.fresh = nullptr;
                    break;
                default: break;
            }
            settleExact(op, pc);
            if (!lowerOne(op, pc))
                fallsIn = false;
            previousOp_ = op;
            pc = next;
        }
    }

    /// Ahead of the loop at `head`: what its accesses will want to know about locals it does not assign. A loop that turns out
    /// to run nothing that could change them then finds it known, and asks no more. Being wrong here costs one bail.
    void checkBeforeLoop(std::uint32_t head)
    {
        if (parent_ || !betHere())
            return;
        const std::vector<std::uint8_t>& c = code_.code;
        std::uint32_t last = head;
        for (std::uint32_t pc = head; pc < c.size();) {
            const Op op = Op(c[pc]);
            if (isBranch(op) && readOperand(c, pc + 1) == head)
                last = pc;
            pc += 1 + std::uint32_t(operandsOf(op)) * 4;
        }
        std::vector<std::uint8_t> assigned(localReg_.size(), 0);
        for (std::uint32_t pc = head; pc <= last;) {
            const Op op = Op(c[pc]);
            if (op == Op::SetLocal)
                assigned[readOperand(c, pc + 1)] = 1;
            pc += 1 + std::uint32_t(operandsOf(op)) * 4;
        }
        struct Wanted
        {
            const Shape* shape = nullptr;
            bool mixedShapes = false;
            std::uint8_t elements = 0;
            /// Properties read through it, and whether all a read there has seen are numbers.
            std::vector<std::pair<std::uint32_t, bool>> reads;
        };
        std::map<std::uint32_t, Wanted> wanted;
        // Globals this function never assigns, read where the loop runs: their places, and what is read through them.
        struct WantedGlobal
        {
            Wanted w;
        };
        std::map<GCObject*, WantedGlobal> globals;
        for (std::uint32_t pc = head; pc <= last;) {
            const Op op = Op(c[pc]);
            const std::uint32_t next = pc + 1 + std::uint32_t(operandsOf(op)) * 4;
            if (op == Op::GetGlobal) {
                GCObject* key = code_.atoms[readOperand(c, pc + 1)].get();
                if (!assignsGlobal(key)) {
                    WantedGlobal& g = globals[key];
                    if (const std::int64_t site = propertySiteAfter(next); site >= 0) {
                        const PropertySite* known = siteAt(std::uint32_t(site));
                        if (known && known->shape && !known->holder && !known->getter && !known->addTo && !known->moreCount) {
                            g.w.mixedShapes = g.w.mixedShapes || (g.w.shape && g.w.shape != known->shape);
                            g.w.shape = known->shape;
                            g.w.reads.emplace_back(known->index, known->saw == PropertySite::kSawNumber);
                        }
                    } else if (const std::uint8_t saw = elementsSeenAfter(next)) {
                        g.w.elements |= saw;
                    }
                }
            }
            if (op == Op::GetLocal) {
                const std::uint32_t slot = readOperand(c, pc + 1);
                if (slot < localReg_.size() && !assigned[slot] && !isNumberLocal(slot) && !(argsLazy_ && slot == code_.argumentsSlot)) {
                    if (const std::int64_t site = propertySiteAfter(next); site >= 0) {
                        const PropertySite* known = siteAt(std::uint32_t(site));
                        if (known && known->shape && !known->holder && !known->getter && !known->addTo && !known->moreCount) {
                            Wanted& w = wanted[slot];
                            w.mixedShapes = w.mixedShapes || (w.shape && w.shape != known->shape);
                            w.shape = known->shape;
                            w.reads.emplace_back(known->index, known->saw == PropertySite::kSawNumber);
                        }
                    } else if (const std::uint8_t saw = elementsSeenAfter(next)) {
                        wanted[slot].elements |= saw;
                    }
                }
            }
            pc = next;
        }
        if (wanted.empty() && globals.empty())
            return;
        const std::int32_t bail = newLabel();
        DynObject* global = rt_.globalObject.get();
        for (const auto& [key, g] : globals) {
            const Prop* found = global->findOwn(key);
            // A number is a constant to `getGlobal`, and needs nothing from here.
            if (!found || global->accessorOf(*found) || found->value.isDouble())
                continue;
            const std::int32_t slot = globalSlot(found, bail);
            const Entry value = taggedEntry(load(slot, kPropValueOffset + kPayload, 8, false, kAliasValue),
                                            load(slot, kPropValueOffset, 1, false, kAliasValue), Own::Weak);
            if (g.w.elements && !g.w.shape && found->value.isObject()) {
                guardArray(value, bail);
                loadHeader(value.r, kElementsOffset);
                load(value.r, kElementCountOffset, 4, false, kAliasHeader);
                load(value.r, kFrozenOffset, 1, false, kAliasHeader);
                load(value.r, kPropsSizeOffset, 4, false, kAliasHeader);
                load(value.r, kNumbersOnlyOffset, 1, false, kAliasHeader);
            } else if (g.w.shape && !g.w.mixedShapes && found->value.isObject()) {
                guardShape(value, g.w.shape, bail);
                readsBeforeLoop(loadHeader(value.r, kPropsOffset), g.w.reads, bail);
            }
        }
        for (const auto& [slot, w] : wanted) {
            const Entry local = taggedEntry(localReg_[slot], localTag_[slot], Own::Weak);
            if (w.elements) {
                if (w.shape)
                    continue;  // an array that has been read like an object as well: not one thing to know about
                guardArray(local, bail);
                loadHeader(local.r, kElementsOffset);
                load(local.r, kElementCountOffset, 4, false, kAliasHeader);
                load(local.r, kFrozenOffset, 1, false, kAliasHeader);
                load(local.r, kPropsSizeOffset, 4, false, kAliasHeader);
                const std::int32_t numbersOnly = load(local.r, kNumbersOnlyOffset, 1, false, kAliasHeader);
                if (w.elements == ElemSite::kNumbers)
                    branchCmp(numbersOnly, IrCond::Eq, 0, bail);
                else if (w.elements == ElemSite::kValues)
                    branchCmp(numbersOnly, IrCond::Ne, 0, bail);
            } else if (w.shape && !w.mixedShapes) {
                guardShape(local, w.shape, bail);
                readsBeforeLoop(loadHeader(local.r, kPropsOffset), w.reads, bail);
            }
        }
        emitBail(bail, {});
        forgetFacts();
    }

    /// Ahead of a loop, the reads it makes of properties of what it does not assign, as it makes them: where the loop does
    /// not store over one, it finds it read, and a number checked, already.
    void readsBeforeLoop(std::int32_t props, std::vector<std::pair<std::uint32_t, bool>> reads, std::int32_t bail)
    {
        std::sort(reads.begin(), reads.end());
        reads.erase(std::unique(reads.begin(), reads.end()), reads.end());
        for (std::size_t i = 0; i < reads.size(); ++i) {
            const auto [index, number] = reads[i];
            // Read both ways in the loop: each read checks for itself.
            if (i + 1 < reads.size() && reads[i + 1].first == index)
                continue;
            if (i > 0 && reads[i - 1].first == index)
                continue;
            const std::int64_t at = std::int64_t(index) * std::int64_t(sizeof(Prop));
            const std::int32_t lt = load(props, at + kPropValueOffset, 1, false, kAliasValue);
            if (number)
                branchCmp(lt, IrCond::Ne, std::int64_t(Type::Double), bail);
            load(props, at + kPropValueOffset + kPayload, 8, number, kAliasValue);
        }
    }

    /// The operand of the `GetProp` or `GetPropDup` at `pc`, if that is what is there.
    std::int64_t propertySiteAfter(std::uint32_t pc) const
    {
        const std::vector<std::uint8_t>& c = code_.code;
        if (pc >= c.size() || labels_[pc] >= 0)
            return -1;
        const Op op = Op(c[pc]);
        return op == Op::GetProp || op == Op::GetPropDup ? std::int64_t(readOperand(c, pc + 1)) : -1;
    }

    /// What the element access this array on the stack is the base of has seen, if a short pure expression for the key,
    /// and for a store its value, is all that comes between.
    std::uint8_t elementsSeenAfter(std::uint32_t pc) const
    {
        const std::vector<std::uint8_t>& c = code_.code;
        int depth = 1;
        for (int steps = 0; steps < 16 && pc < c.size(); ++steps) {
            if (labels_[pc] >= 0)
                return 0;
            const Op op = Op(c[pc]);
            switch (op) {
                case Op::GetLocal:
                case Op::PushConst:
                case Op::PushUndefined:
                case Op::PushNull:
                case Op::PushTrue:
                case Op::PushFalse: ++depth; break;
                case Op::Add:
                case Op::Sub:
                case Op::Mul:
                case Op::BitAnd:
                case Op::BitOr:
                case Op::BitXor:
                case Op::Shl:
                case Op::Shr: --depth; break;
                case Op::Dup2:
                    if (depth != 2)
                        return 0;
                    depth += 2;
                    break;
                case Op::GetElem:
                    // With a `Dup2` ahead of it the base is two further down, and that is this one.
                    if (depth != 2 && depth != 4)
                        return 0;
                    return code_.elemSaw[readOperand(c, pc + 1)];
                case Op::SetElem:
                    return depth == 3 ? code_.elemSaw[readOperand(c, pc + 1)] : 0;
                default: return 0;
            }
            if (depth < 1)
                return 0;
            pc += 1 + std::uint32_t(operandsOf(op)) * 4;
        }
        return 0;
    }

    void addOsrEntry(std::uint32_t pc, std::int32_t label)
    {
        IrOsrEntry entry;
        entry.bytecodePc = pc;
        entry.label = label;
        for (std::uint32_t slot = 0; slot < localReg_.size(); ++slot) {
            IrOsrEntry::Slot out;
            out.vreg = localReg_[slot];
            out.slot = slot;
            out.bytes = 8;
            out.owned = localOwned_[slot] != 0;
            out.tagVreg = localTag_[slot];
            entry.slots.push_back(out);
            if (cellOwned_[slot]) {
                IrOsrEntry::Slot cell;
                cell.vreg = cellReg_[slot];
                cell.slot = slot;
                cell.bytes = 8;
                cell.owned = true;
                entry.pointers.push_back(cell);
            }
        }
        fn_.osrEntries.push_back(std::move(entry));
    }

    void poll()
    {
        const std::int32_t at = std::int32_t(fn_.code.size());
        inst(IrOp::Poll);
        const std::int32_t exit = makeExit(0);
        fn_.code[std::size_t(at)].target = exit;
    }

    static IrCond fpCond(Op op) noexcept
    {
        switch (op) {
            case Op::Lt: return IrCond::FLt;
            case Op::Le: return IrCond::FLe;
            case Op::Gt: return IrCond::FGt;
            default: return IrCond::FGe;
        }
    }

    void fusedCompare(Op op, bool whenTrue, std::uint32_t target, std::uint32_t pc)
    {
        Entry b = pop();
        Entry a = pop();
        if (target <= pc)
            poll();
        // Two int32s, or one and a constant that is one: compared as integers, the constant second.
        if (isIntLike(a) && isIntLike(b) && !(a.isConst && b.isConst)) {
            if (a.isConst) {
                std::swap(a, b);
                op = op == Op::Lt ? Op::Gt : op == Op::Le ? Op::Ge : op == Op::Gt ? Op::Lt : op == Op::Ge ? Op::Le : op;
            }
            IrCond intCond = IrCond::Eq;
            switch (op) {
                case Op::Lt: intCond = IrCond::Lt; break;
                case Op::Le: intCond = IrCond::Le; break;
                case Op::Gt: intCond = IrCond::Gt; break;
                case Op::Ge: intCond = IrCond::Ge; break;
                case Op::StrictNe:
                case Op::Ne: intCond = IrCond::Ne; break;
                default: intCond = IrCond::Eq; break;
            }
            const std::int32_t rb = b.isConst ? -1 : b.r;
            IrInst& br = inst(IrOp::BrCmp);
            br.a = a.r;
            br.b = rb;
            if (b.isConst) {
                br.hasImm = true;
                br.imm = std::int64_t(b.number);
            }
            br.cond = whenTrue ? intCond : typed::invert(intCond);
            br.width = 32;
            br.target = labels_[target];
            return;
        }
        const std::int32_t ra = doubleReg(a);
        const std::int32_t rb = doubleReg(b);
        const IrCond cond = op == Op::StrictEq || op == Op::Eq   ? IrCond::FEq
                            : op == Op::StrictNe || op == Op::Ne ? IrCond::FNe
                                                                 : fpCond(op);
        if (whenTrue) {
            IrInst& br = inst(IrOp::BrFCmp);
            br.a = ra;
            br.b = rb;
            br.cond = cond;
            br.width = 64;
            br.target = labels_[target];
        } else {
            // A NaN answers false to all of these but `!=`, so the false sense has to jump around.
            const std::int32_t skip = newLabel();
            IrInst& br = inst(IrOp::BrFCmp);
            br.a = ra;
            br.b = rb;
            br.cond = cond;
            br.width = 64;
            br.target = skip;
            inst(IrOp::Br).imm = labels_[target];
            inst(IrOp::Label).imm = skip;
        }
    }

    /// The entry's truthiness, as 0 or 1 in a register.
    std::int32_t truthyReg(const Entry& e)
    {
        if (e.isConst && e.rep == Rep::Tagged) {
            if (e.known == Type::String)
                return immReg(reinterpret_cast<const StringCell*>(e.bits)->length != 0 ? 1 : 0);
            if (e.known == Type::Bool)
                return immReg(e.bits != 0 ? 1 : 0);
            return immReg(e.known >= Type::Symbol ? 1 : 0);
        }
        if (e.rep == Rep::Tagged && e.hasKnownTag && e.known == Type::Bool)
            return e.r;
        if (e.rep == Rep::Int32) {
            const std::int32_t d = fn_.newVreg(false);
            IrInst& in = inst(IrOp::Cset);
            in.d = d;
            in.a = e.isConst ? immReg(std::int64_t(e.number)) : e.r;
            in.cond = IrCond::Ne;
            in.hasImm = true;
            in.imm = 0;
            in.width = 32;
            return d;
        }
        if (e.rep == Rep::Double) {
            // Neither zero nor NaN.
            const std::int32_t fp = doubleReg(e);
            const std::int32_t abs = fn_.newVreg(true);
            IrInst& a = inst(IrOp::FAbs);
            a.d = abs;
            a.a = fp;
            a.width = 64;
            const std::int32_t zero = fpImm(0.0);
            const std::int32_t d = fn_.newVreg(false);
            IrInst& in = inst(IrOp::FCset);
            in.d = d;
            in.a = abs;
            in.b = zero;
            in.cond = IrCond::FGt;
            in.width = 64;
            return d;
        }
        const std::int32_t tag = tagReg(e);
        const std::int32_t d = fn_.newVreg(false);
        const std::int32_t slow = newLabel();
        const std::int32_t done = newLabel();
        // An object or a symbol is true, undefined and null false, a boolean itself; a number is neither zero nor NaN.
        // A string and a host's kinds of number ask the helper.
        static_assert(Type::Undefined < Type::Null && Type::Null < Type::Bool && Type::Symbol < Type::Object, "tag order");
        IrInst& one = inst(IrOp::MovImm);
        one.d = d;
        one.imm = 1;
        branchCmp(tag, IrCond::Hs, std::int64_t(Type::Symbol), done);
        IrInst& zero = inst(IrOp::MovImm);
        zero.d = d;
        zero.imm = 0;
        branchCmp(tag, IrCond::Ls, std::int64_t(Type::Null), done);
        IrInst& mov = inst(IrOp::Mov);
        mov.d = d;
        mov.a = e.r;
        branchCmp(tag, IrCond::Ne, std::int64_t(Type::Bool), slow);
        inst(IrOp::ColdBegin);
        inst(IrOp::Label).imm = slow;
        {
            const std::int32_t helper = newLabel();
            branchCmp(tag, IrCond::Ne, std::int64_t(Type::Double), helper);
            const std::int32_t fp = fn_.newVreg(true);
            IrInst& bits = inst(IrOp::BitsToFp);
            bits.d = fp;
            bits.a = e.r;
            bits.width = 64;
            const std::int32_t abs = fn_.newVreg(true);
            IrInst& fabs = inst(IrOp::FAbs);
            fabs.d = abs;
            fabs.a = fp;
            fabs.width = 64;
            const std::int32_t fzero = fpImm(0.0);
            IrInst& gt = inst(IrOp::FCset);
            gt.d = d;
            gt.a = abs;
            gt.b = fzero;
            gt.cond = IrCond::FGt;
            gt.width = 64;
            inst(IrOp::Br).imm = done;
            inst(IrOp::Label).imm = helper;
        }
        coldCall(reinterpret_cast<void*>(&scriptDynToBoolean), {tag, e.r}, d, -1, false);
        inst(IrOp::Br).imm = done;
        inst(IrOp::ColdEnd);
        inst(IrOp::Label).imm = done;
        return d;
    }

    /// Everything else. False when control does not fall through.
    bool lowerOne(Op op, std::uint32_t pc)
    {
        const std::vector<std::uint8_t>& c = code_.code;
        const auto operand = [&]() { return readOperand(c, pc + 1); };
        switch (op) {
            case Op::Nop: return true;
            case Op::PushUndefined: push(constTagged(Type::Undefined)); return true;
            case Op::PushNull: push(constTagged(Type::Null)); return true;
            case Op::PushTrue: push(constTagged(Type::Bool, 1)); return true;
            case Op::PushFalse: push(constTagged(Type::Bool, 0)); return true;
            case Op::PushConst: {
                const Value& k = code_.consts[operand()];
                if (k.isDouble()) {
                    push(constDouble(detail::Access::r8(k)));
                } else if (k.isString()) {
                    push(constTagged(Type::String, reinterpret_cast<std::uint64_t>(detail::Access::ptr(k))));
                } else {
                    refuse("a constant of an unusual type");
                }
                return true;
            }
            case Op::PushThis: {
                if (code_.sloppyThis && !thisNormalized_) {
                    // Nullish reads as the global object, which outlives everything.
                    const std::int32_t t = fn_.newVreg(false);
                    const std::int32_t r = fn_.newVreg(false);
                    IrInst& movT = inst(IrOp::Mov);
                    movT.d = t;
                    movT.a = thisTag_;
                    IrInst& movR = inst(IrOp::Mov);
                    movR.d = r;
                    movR.a = thisReg_;
                    const std::int32_t done = newLabel();
                    branchCmp(thisTag_, IrCond::Hi, std::int64_t(Type::Null), done);
                    IrInst& g = inst(IrOp::MovImm);
                    g.d = r;
                    g.imm = std::int64_t(reinterpret_cast<std::intptr_t>(rt_.globalObject.get()));
                    IrInst& gt = inst(IrOp::MovImm);
                    gt.d = t;
                    gt.imm = std::int64_t(Type::Object);
                    inst(IrOp::Label).imm = done;
                    push(taggedEntry(r, t, Own::Immortal));
                    return true;
                }
                {
                    Entry self = taggedEntry(thisReg_, thisTag_, Own::Immortal);
                    if (thisIsCell_) {
                        self.hasKnownTag = true;
                        self.known = Type::Object;
                    }
                    push(self);
                }
                return true;
            }
            case Op::PushCallee: {
                Entry e;
                e.r = fnReg_;
                e.hasKnownTag = true;
                e.known = Type::Object;
                e.own = Own::Immortal;
                e.nonNull = true;
                push(e);
                return true;
            }
            case Op::Pop: {
                const Entry e = pop();
                release(e);
                return true;
            }
            case Op::PopN: {
                for (std::uint32_t i = 0; i < operand(); ++i) {
                    const Entry e = pop();
                    release(e);
                }
                return true;
            }
            case Op::Dup: {
                Entry copy = top();
                copy.fresh = nullptr;
                if (copy.own == Own::Owned)
                    copy.own = Own::Weak, makeOwned(copy);
                push(copy);
                return true;
            }
            case Op::Dup2: {
                Entry a = top(1), b = top(0);
                if (a.own == Own::Owned)
                    a.own = Own::Weak, makeOwned(a);
                if (b.own == Own::Owned)
                    b.own = Own::Weak, makeOwned(b);
                push(a);
                push(b);
                return true;
            }
            case Op::Swap: std::swap(top(0), top(1)); return true;
            case Op::Rot3: {
                const Entry t = pop(), m = pop(), l = pop();
                push(t);
                push(l);
                push(m);
                return true;
            }
            case Op::GetLocal: {
                const std::uint32_t slot = operand();
                if (argsLazy_ && slot == code_.argumentsSlot) {
                    Entry e = constTagged(Type::Undefined);
                    e.lazyArgs = true;
                    push(e);
                    return true;
                }
                if (isNumberLocal(slot)) {
                    Entry e = isIntLocal(slot) ? intEntry(localReg_[slot], true) : doubleEntry(localReg_[slot]);
                    e.local = std::int32_t(slot);
                    push(e);
                } else {
                    Entry e = taggedEntry(localReg_[slot], localTag_[slot], Own::Local);
                    e.local = std::int32_t(slot);
                    push(e);
                }
                return true;
            }
            case Op::SetLocal: storeLocal(operand()); return true;
            case Op::GetCell: {
                const std::int32_t box = cellReg_[operand()];
                readBox(box);
                return true;
            }
            case Op::SetCell: {
                const std::int32_t box = cellReg_[operand()];
                writeBox(box);
                return true;
            }
            case Op::NewCell:
            case Op::RenewCell: {
                const std::uint32_t slot = operand();
                secureWeak();
                std::int32_t tag, payload;
                if (op == Op::RenewCell) {
                    // A copy of the current binding, when there is one.
                    tag = fn_.newVreg(false);
                    payload = fn_.newVreg(false);
                    const std::int32_t none = newLabel();
                    const std::int32_t done = newLabel();
                    branchCmp(cellReg_[slot], IrCond::Eq, 0, none, 64);
                    const std::int32_t t = load(cellReg_[slot], kBoxValueOffset, 1);
                    const std::int32_t p = load(cellReg_[slot], kBoxValueOffset + kPayload, 8);
                    IrInst& mt = inst(IrOp::Mov);
                    mt.d = tag;
                    mt.a = t;
                    IrInst& mp = inst(IrOp::Mov);
                    mp.d = payload;
                    mp.a = p;
                    inst(IrOp::Br).imm = done;
                    inst(IrOp::Label).imm = none;
                    IrInst& it = inst(IrOp::MovImm);
                    it.d = tag;
                    it.imm = std::int64_t(Type::Undefined);
                    IrInst& ip = inst(IrOp::MovImm);
                    ip.d = payload;
                    ip.imm = 0;
                    inst(IrOp::Label).imm = done;
                } else {
                    tag = immReg(std::int64_t(Type::Undefined));
                    payload = immReg(0);
                }
                const std::int32_t box = newBox(tag, payload);
                const std::int32_t old = fn_.newVreg(false);
                IrInst& save = inst(IrOp::Mov);
                save.d = old;
                save.a = cellReg_[slot];
                IrInst& mov = inst(IrOp::Mov);
                mov.d = cellReg_[slot];
                mov.a = box;
                decRefOf(old, -1, true);
                return true;
            }
            case Op::GetUpval: {
                const std::int32_t data = load(fnReg_, kFnUpvaluesOffset, 8, false, kAliasHeader);
                const std::int32_t box = load(data, std::int64_t(operand()) * 8, 8);
                readBox(box);
                return true;
            }
            case Op::SetUpval: {
                const std::int32_t data = load(fnReg_, kFnUpvaluesOffset, 8, false, kAliasHeader);
                const std::int32_t box = load(data, std::int64_t(operand()) * 8, 8);
                writeBox(box);
                return true;
            }
            case Op::GetGlobal: getGlobal(code_.atoms[operand()].get()); return true;
            case Op::SetGlobal: {
                Entry value = pop();
                GCObject* key = code_.atoms[operand()].get();
                secureWeak();
                const Prop* found = rt_.globalObject->findOwn(key);
                if (found && !rt_.globalObject->accessorOf(*found) && (rt_.globalObject->attrsOf(*found) & PropWritable)) {
                    // A writable global where it was: the property takes the value's count, what it held loses one.
                    makeOwned(value);
                    const std::int32_t tag = tagReg(value);
                    const std::int32_t payload = payloadReg(value);
                    const std::int32_t slow = newLabel();
                    const std::int32_t done = newLabel();
                    const std::int32_t slot = globalSlot(found, slow);
                    const std::int32_t oldTag = load(slot, kPropValueOffset, 1, false, kAliasValue);
                    const std::int32_t old = load(slot, kPropValueOffset + kPayload, 8, false, kAliasValue);
                    store(slot, kPropValueOffset, tag, 1);
                    store(slot, kPropValueOffset + kPayload, payload, 8);
                    decRefOf(old, oldTag, false);
                    inst(IrOp::ColdBegin);
                    inst(IrOp::Label).imm = slow;
                    {
                        const std::int32_t rt = rtReg();
                        const std::int32_t k = ptrReg(key);
                        coldCall(reinterpret_cast<void*>(&scriptDynSetGlobal), {rt, k, tag, payload}, -1, -1, true, {value});
                        // The helper took its own count.
                        if (value.own == Own::Owned && !value.isConst && value.rep == Rep::Tagged &&
                            !(value.hasKnownTag && value.known < Type::String))
                            decRefOf(payload, value.hasKnownTag ? -1 : tag, false);
                    }
                    inst(IrOp::Br).imm = done;
                    inst(IrOp::ColdEnd);
                    inst(IrOp::Label).imm = done;
                    return true;
                }
                const std::int32_t rt = rtReg();
                const std::int32_t k = ptrReg(key);
                const std::int32_t tag = tagReg(value);
                const std::int32_t payload = payloadReg(value);
                coldCall(reinterpret_cast<void*>(&scriptDynSetGlobal), {rt, k, tag, payload}, -1, -1, true, {value});
                release(value);
                return true;
            }
            case Op::GetProp: getProp(operand(), false); return true;
            case Op::GetPropDup: getProp(operand(), true); return true;
            case Op::SetProp: setProp(operand()); return true;
            case Op::GetElem: getElem(operand()); return true;
            case Op::SetElem: setElem(operand()); return true;
            case Op::DefField: {
                Entry value = pop();
                Entry& object = stack_.back();
                GCObject* key = code_.atoms[operand()].get();
                secureWeak();
                if (object.fresh && key != rt_.atom.protoName.get() && object.fresh->count < object.freshRoom &&
                    !object.fresh->has(key)) {
                    Shape* next = rt_.shapeAfter(object.fresh, key, PropDefault);
                    if (next && next->count == object.fresh->count + 1) {
                        // A key the literal has not had yet goes on the end of a table with room for it, as `putOwn` would.
                        makeOwned(value);
                        const std::int32_t vt = tagReg(value);
                        const std::int32_t vr = payloadReg(value);
                        const std::int64_t at = std::int64_t(object.fresh->count) * std::int64_t(sizeof(Prop));
                        const std::int32_t props = loadHeader(object.r, kPropsOffset);
                        store(props, at + kPropValueOffset, vt, 1);
                        store(props, at + kPropValueOffset + kPayload, vr, 8);
                        store(object.r, kPropsSizeOffset, immReg(std::int64_t(next->count)), 4, kAliasHeader);
                        storeHeader(object.r, kShapeOffset, ptrReg(next));
                        object.fresh = next;
                        return true;
                    }
                }
                object.fresh = nullptr;
                const std::int32_t rt = rtReg();
                const std::int32_t k = ptrReg(key);
                const std::int32_t tag = tagReg(value);
                const std::int32_t payload = payloadReg(value);
                coldCall(reinterpret_cast<void*>(&scriptDynDefField), {rt, object.r, k, tag, payload}, -1, -1, true, {value});
                release(value);
                return true;
            }
            case Op::NewObject: {
                secureWeak();
                const std::int32_t r = fn_.newVreg(false);
                allocateObject(rt_.objectProto.get(), rt_.rootShapeFor(rt_.objectProto.get()), operand(), r,
                               reinterpret_cast<void*>(&scriptDynNewObject), {rtReg(), immReg(std::int64_t(operand()))}, {});
                Entry made = objectEntry(r);
                // Where `newObjectSized` starts it: the shape its prototype's children start from, with this much room.
                made.fresh = rt_.rootShapeFor(rt_.objectProto.get());
                made.freshRoom = std::min<std::uint32_t>(operand(), DynObject::kIndexThreshold);
                push(made);
                return true;
            }
            case Op::NewArray: {
                secureWeak();
                const std::int32_t rt = rtReg();
                const std::int32_t n = immReg(std::int64_t(operand()));
                const std::int32_t r = callCold(reinterpret_cast<void*>(&scriptDynNewArray), {rt, n}, true);
                push(objectEntry(r));
                return true;
            }
            case Op::ArrayAppend: {
                // An element of a literal, into the array it is making: nothing else has seen that array, so it is one and
                // can grow. In place while there is room and the form need not change.
                Entry value = pop();
                secureWeak();
                const std::int32_t array = top().r;
                const std::int32_t tag = tagReg(value);
                const std::int32_t payload = payloadReg(value);
                const std::int32_t slow = newLabel();
                const std::int32_t done = newLabel();
                const std::int32_t values = newLabel();
                const std::int32_t stored = newLabel();
                const std::int32_t data = loadHeader(array, kElementsOffset);
                const std::int32_t count = load(array, kElementCountOffset, 4, false, kAliasHeader);
                const std::int32_t cap = load(array, kElementCapOffset, 4, false, kAliasHeader);
                {
                    IrInst& full = inst(IrOp::BrCmp);
                    full.a = count;
                    full.b = cap;
                    full.cond = IrCond::Eq;
                    full.width = 64;
                    full.target = slow;
                }
                const std::int32_t numbersOnly = load(array, kNumbersOnlyOffset, 1, false, kAliasHeader);
                branchCmp(numbersOnly, IrCond::Eq, 0, values);
                if (!isKnownNumber(value)) {
                    if (value.hasKnownTag || value.isConst || value.rep != Rep::Tagged)
                        inst(IrOp::Br).imm = slow;
                    else
                        branchCmp(tag, IrCond::Ne, std::int64_t(Type::Double), slow);
                }
                {
                    const std::int32_t slot = fn_.newVreg(false);
                    const std::int32_t scaled = shiftLeft(count, 3);
                    IrInst& add = inst(IrOp::Add);
                    add.d = slot;
                    add.a = data;
                    add.b = scaled;
                    add.width = 64;
                    store(slot, 0, payload, 8);
                }
                inst(IrOp::Br).imm = stored;
                inst(IrOp::Label).imm = values;
                // The array takes a count of its own; the value's is released below, as after the helper.
                if (!value.hasKnownTag)
                    incRefOf(payload, tag, false);
                else if (value.known >= Type::String)
                    incRefOf(payload, -1, false);
                {
                    const std::int32_t slot = fn_.newVreg(false);
                    const std::int32_t scaled = shiftLeft(count, 4);
                    IrInst& add = inst(IrOp::Add);
                    add.d = slot;
                    add.a = data;
                    add.b = scaled;
                    add.width = 64;
                    store(slot, 0, tag, 1);
                    store(slot, kPayload, payload, 8);
                }
                inst(IrOp::Label).imm = stored;
                {
                    const std::int32_t next = fn_.newVreg(false);
                    IrInst& plus = inst(IrOp::Add);
                    plus.d = next;
                    plus.a = count;
                    plus.hasImm = true;
                    plus.imm = 1;
                    plus.width = 64;
                    store(array, kElementCountOffset, next, 4, kAliasHeader);
                }
                inst(IrOp::Br).imm = done;
                inst(IrOp::ColdBegin);
                inst(IrOp::Label).imm = slow;
                coldCall(reinterpret_cast<void*>(&scriptDynArrayAppend), {rtReg(), array, tag, payload}, -1, -1, true, {value});
                inst(IrOp::Br).imm = done;
                inst(IrOp::ColdEnd);
                inst(IrOp::Label).imm = done;
                release(value);
                return true;
            }
            case Op::GetElemDup: {
                // base key -> value base
                (void)operand();
                Entry key = pop();
                Entry base = pop();
                secureWeak();
                makeOwned(base);
                const std::int32_t rt = rtReg();
                const std::int32_t r = fn_.newVreg(false);
                const std::int32_t t = fn_.newVreg(false);
                coldCall(reinterpret_cast<void*>(&scriptDynGetElem), {rt, tagReg(base), payloadReg(base), tagReg(key), payloadReg(key)}, r,
                         t, true, {base, key});
                release(key);
                push(taggedEntry(r, t, Own::Weak));
                push(base);
                return true;
            }
            case Op::InstanceOf: {
                Entry ctor = pop();
                Entry obj = pop();
                if (!instanceOfInPlace(obj, ctor))
                    binaryPredicate(reinterpret_cast<void*>(&scriptDynInstanceOf), obj, ctor);
                return true;
            }
            case Op::In: {
                Entry container = pop();
                Entry key = pop();
                binaryPredicate(reinterpret_cast<void*>(&scriptDynIn), key, container);
                return true;
            }
            case Op::DeleteProp: {
                Entry base = pop();
                secureWeak();
                const std::int32_t rt = rtReg();
                const std::int32_t k = ptrReg(code_.atoms[operand()].get());
                const std::int32_t d = fn_.newVreg(false);
                coldCall(reinterpret_cast<void*>(&scriptDynDeleteProp), {rt, tagReg(base), payloadReg(base), k}, d, -1, true, {base});
                release(base);
                push(boolEntry(d));
                return true;
            }
            case Op::DeleteElem: {
                Entry key = pop();
                Entry base = pop();
                secureWeak();
                const std::int32_t rt = rtReg();
                const std::int32_t d = fn_.newVreg(false);
                coldCall(reinterpret_cast<void*>(&scriptDynDeleteElem), {rt, tagReg(base), payloadReg(base), tagReg(key), payloadReg(key)}, d,
                         -1, true, {base, key});
                release(key);
                release(base);
                push(boolEntry(d));
                return true;
            }
            case Op::DefGlobal: {
                secureWeak();
                const std::int32_t rt = rtReg();
                const std::int32_t k = ptrReg(code_.atoms[operand()].get());
                const std::int32_t attrs = immReg(std::int64_t(readOperand(c, pc + 5)));
                coldCall(reinterpret_cast<void*>(&scriptDynDefGlobal), {rt, k, attrs}, -1, -1, true);
                return true;
            }
            case Op::TypeofGlobal: {
                secureWeak();
                const std::int32_t rt = rtReg();
                const std::int32_t k = ptrReg(code_.atoms[operand()].get());
                const std::int32_t r = fn_.newVreg(false);
                const std::int32_t t = fn_.newVreg(false);
                coldCall(reinterpret_cast<void*>(&scriptDynTypeofGlobal), {rt, k}, r, t, true);
                push(taggedEntry(r, t, Own::Owned));
                return true;
            }
            case Op::DefMethod:
            case Op::DefGetter:
            case Op::DefSetter: {
                Entry value = pop();
                const Entry& object = top();
                secureWeak();
                const std::int32_t rt = rtReg();
                const std::int32_t k = ptrReg(code_.atoms[operand()].get());
                const std::int32_t mode = immReg(op == Op::DefMethod ? 0 : op == Op::DefGetter ? 1 : 2);
                coldCall(reinterpret_cast<void*>(&scriptDynDefMember), {rt, mode, object.r, k, tagReg(value), payloadReg(value)}, -1, -1, true,
                         {value});
                release(value);
                return true;
            }
            case Op::DefElem:
            case Op::DefMethodElem: {
                Entry value = pop();
                Entry key = pop();
                const Entry& object = top();
                secureWeak();
                const std::int32_t rt = rtReg();
                const std::int32_t mode = immReg(op == Op::DefElem ? 0 : 1);
                coldCall(reinterpret_cast<void*>(&scriptDynDefElem), {rt, mode, object.r, tagReg(key), payloadReg(key), tagReg(value), payloadReg(value)},
                         -1, -1, true, {value, key});
                release(value);
                release(key);
                return true;
            }
            case Op::ClassSetup: {
                // ctor [super] -> ctor
                Entry ctor = pop();
                const bool hasSuper = operand() != 0;
                Entry super = hasSuper ? pop() : constTagged(Type::Undefined);
                secureWeak();
                const std::int32_t rt = rtReg();
                coldCall(reinterpret_cast<void*>(&scriptDynClassSetup),
                         {rt, immReg(hasSuper ? 1 : 0), tagReg(super), payloadReg(super), tagReg(ctor), payloadReg(ctor)}, -1, -1, true, {ctor, super});
                release(super);
                makeOwned(ctor);
                push(ctor);
                return true;
            }
            case Op::ForInStart: {
                Entry src = pop();
                secureWeak();
                const std::int32_t rt = rtReg();
                const std::int32_t keys = callCold(reinterpret_cast<void*>(&scriptDynForInKeys), {rt, tagReg(src), payloadReg(src)}, true, {src});
                release(src);
                push(objectEntry(keys));
                push(constDouble(0));
                return true;
            }
            case Op::ForInNext: {
                // keys index -> keys index key, or on to the target at the end.
                const std::uint32_t target = operand();
                secureWeak();
                flowTo(target);
                const std::int32_t rt = rtReg();
                const Entry& keys = top(1);
                const Entry& index = top(0);
                const std::int32_t r = fn_.newVreg(false);
                const std::int32_t t = fn_.newVreg(false);
                coldCall(reinterpret_cast<void*>(&scriptDynForInNext), {rt, keys.r, tagReg(index), payloadReg(index)}, r, t, true);
                branchCmp(t, IrCond::Eq, kForInDone, labels_[target]);
                Entry stepped = pop();
                const std::int32_t current = doubleReg(stepped);
                const std::int32_t one = doubleReg(constDouble(1));
                const std::int32_t next = fn_.newVreg(true);
                IrInst& add = inst(IrOp::FAdd);
                add.d = next;
                add.a = current;
                add.b = one;
                release(stepped);
                Entry advanced = constDouble(0);
                advanced.isConst = false;
                advanced.r = next;
                push(advanced);
                push(taggedEntry(r, t, Own::Owned));
                return true;
            }
            case Op::ToObject: {
                Entry a = pop();
                secureWeak();
                const std::int32_t rt = rtReg();
                const std::int32_t r = fn_.newVreg(false);
                const std::int32_t t = fn_.newVreg(false);
                coldCall(reinterpret_cast<void*>(&scriptDynToObject), {rt, tagReg(a), payloadReg(a)}, r, t, true, {a});
                release(a);
                push(taggedEntry(r, t, Own::Owned));
                return true;
            }
            case Op::GetSuper: {
                secureWeak();
                const std::int32_t rt = rtReg();
                const std::int32_t k = ptrReg(code_.atoms[operand()].get());
                const std::int32_t r = fn_.newVreg(false);
                const std::int32_t t = fn_.newVreg(false);
                coldCall(reinterpret_cast<void*>(&scriptDynGetSuper), {rt, fnReg_, thisTag_, thisReg_, k}, r, t, true);
                push(taggedEntry(r, t, Own::Owned));
                return true;
            }
            case Op::Closure: closure(operand()); return true;
            case Op::Call: call(operand()); return true;
            case Op::New: construct(operand()); return true;
            case Op::Return:
            case Op::ReturnUndefined: {
                Entry value = op == Op::Return ? pop() : constTagged(Type::Undefined);
                noteReturn(knownTagOf(value));
                // The count moves with the value; a read of an owned local gives that local's away.
                std::int32_t keep = -1;
                if (value.own == Own::Local && value.local >= 0 && localOwned_[std::size_t(value.local)]) {
                    keep = value.local;
                    value.own = Own::Owned;
                } else {
                    makeOwned(value);
                }
                const std::int32_t tag = tagReg(value);
                const std::int32_t payload = payloadReg(value);
                while (!stack_.empty()) {
                    const Entry dropped = pop();
                    release(dropped);
                }
                releaseLocals(keep);
                if (inlineDone_ >= 0) {
                    // Inlined: the caller carries on with the value, and its count.
                    IrInst& movTag = inst(IrOp::Mov);
                    movTag.d = inlineT_;
                    movTag.a = tag;
                    IrInst& mov = inst(IrOp::Mov);
                    mov.d = inlineR_;
                    mov.a = payload;
                    inst(IrOp::Br).imm = inlineDone_;
                    return false;
                }
                IrInst& ret = inst(IrOp::Ret);
                ret.a = payload;
                ret.b = tag;
                return false;
            }
            case Op::Jump: {
                const std::uint32_t target = operand();
                flowTo(target);
                if (target <= pc)
                    poll();
                inst(IrOp::Br).imm = labels_[target];
                return false;
            }
            case Op::JumpIfFalse:
            case Op::JumpIfTrue: {
                const std::uint32_t target = operand();
                const Entry cond = pop();
                if (target <= pc)
                    poll();
                const std::int32_t t = truthyReg(cond);
                release(cond);
                flowTo(target);
                branchCmp(t, op == Op::JumpIfTrue ? IrCond::Ne : IrCond::Eq, 0, labels_[target]);
                return true;
            }
            case Op::JumpIfFalseKeep:
            case Op::JumpIfTrueKeep: {
                const std::uint32_t target = operand();
                flowTo(target);
                const std::int32_t t = truthyReg(top());
                branchCmp(t, op == Op::JumpIfTrueKeep ? IrCond::Ne : IrCond::Eq, 0, labels_[target]);
                return true;
            }
            case Op::JumpIfNullishKeep:
            case Op::JumpIfNotNullishKeep: {
                const std::uint32_t target = operand();
                flowTo(target);
                const std::int32_t tag = tagReg(top());
                branchCmp(tag, op == Op::JumpIfNullishKeep ? IrCond::Ls : IrCond::Hi, std::int64_t(Type::Null), labels_[target]);
                return true;
            }
            case Op::Throw: {
                Entry value = pop();
                secureWeak();
                const std::int32_t rt = rtReg();
                const std::int32_t tag = tagReg(value);
                const std::int32_t payload = payloadReg(value);
                coldCall(reinterpret_cast<void*>(&scriptDynThrow), {rt, tag, payload}, -1, -1, true, {value});
                inst(IrOp::Trap);
                stack_.clear();
                return false;
            }
            case Op::IterStart: {
                Entry src = pop();
                secureWeak();
                const std::int32_t rt = rtReg();
                const std::int32_t tag = tagReg(src);
                const std::int32_t payload = payloadReg(src);
                const std::int32_t r = fn_.newVreg(false);
                const std::int32_t t = fn_.newVreg(false);
                coldCall(reinterpret_cast<void*>(&scriptDynGetIterator), {rt, tag, payload}, r, t, true, {src});
                release(src);
                push(taggedEntry(r, t, Own::Owned));
                return true;
            }
            case Op::IterNext: {
                const std::uint32_t target = operand();
                secureWeak();
                flowTo(target);
                const Entry& iterator = top();
                fn_.frameAreaBytes = std::max<std::uint32_t>(fn_.frameAreaBytes, 16);
                const std::int32_t out = fn_.newVreg(false);
                IrInst& addr = inst(IrOp::FrameAddr);
                addr.d = out;
                addr.imm = 0;
                const std::int32_t rt = rtReg();
                const std::int32_t more = callCold(reinterpret_cast<void*>(&scriptDynIterNext),
                                                   {rt, tagReg(iterator), iterator.r, out}, true);
                branchCmp(more, IrCond::Eq, 0, labels_[target]);
                const std::int32_t r = load(out, 0, 8);
                const std::int32_t t = load(out, 8, 8);
                push(taggedEntry(r, t, Own::Owned));
                return true;
            }
            case Op::IterClose: {
                Entry iterator = pop();
                secureWeak();
                const std::int32_t rt = rtReg();
                coldCall(reinterpret_cast<void*>(&scriptDynIterClose), {rt, tagReg(iterator), iterator.r}, -1, -1, true, {iterator});
                release(iterator);
                return true;
            }
            case Op::Add: lowerAdd(appendsToLocal(pc)); return true;
            case Op::Sub:
            case Op::Mul:
            case Op::Div: arithmetic(op); return true;
            case Op::Mod: remainder(); return true;
            case Op::Pow: {
                const Entry b = pop();
                const Entry a = pop();
                secureWeakFor(a, b);
                const std::int32_t x = toNumberReg(a);
                const std::int32_t y = toNumberReg(b);
                const std::int32_t d = fn_.newVreg(true);
                coldCall(reinterpret_cast<void*>(&scriptDynPow), {x, y}, d, -1, false);
                release(b);
                release(a);
                push(doubleEntry(d));
                return true;
            }
            case Op::Neg: {
                const Entry a = pop();
                secureWeakFor(a);
                const std::int32_t x = toNumberReg(a);
                const std::int32_t d = fn_.newVreg(true);
                IrInst& in = inst(IrOp::FNeg);
                in.d = d;
                in.a = x;
                in.width = 64;
                release(a);
                push(doubleEntry(d));
                return true;
            }
            case Op::Pos:
            case Op::ToNumber: {
                const Entry a = pop();
                if (isKnownNumber(a)) {
                    push(a);
                    return true;
                }
                secureWeak();
                const std::int32_t x = toNumberReg(a);
                release(a);
                push(doubleEntry(x));
                return true;
            }
            case Op::Inc:
            case Op::Dec: {
                const Entry a = pop();
                if (a.rep == Rep::Int32 && intArithmetic(op == Op::Inc ? Op::Add : Op::Sub, a, constDouble(1)))
                    return true;
                secureWeakFor(a);
                const std::int32_t x = toNumberReg(a);
                const std::int32_t one = fpImm(1.0);
                const std::int32_t d = fn_.newVreg(true);
                IrInst& in = inst(op == Op::Inc ? IrOp::FAdd : IrOp::FSub);
                in.d = d;
                in.a = x;
                in.b = one;
                in.width = 64;
                release(a);
                push(doubleEntry(d));
                return true;
            }
            case Op::Void: {
                const Entry a = pop();
                release(a);
                push(constTagged(Type::Undefined));
                return true;
            }
            case Op::BitAnd:
            case Op::BitOr:
            case Op::BitXor:
            case Op::Shl:
            case Op::Shr:
            case Op::UShr: bitwise(op); return true;
            case Op::BitNot: {
                const Entry a = pop();
                secureWeakFor(a);
                const std::int32_t x = toInt32Reg(a);
                const std::int32_t d = fn_.newVreg(false);
                IrInst& in = inst(IrOp::Not);
                in.d = d;
                in.a = x;
                in.width = 32;
                release(a);
                push(intEntry(d));
                return true;
            }
            case Op::Lt:
            case Op::Le:
            case Op::Gt:
            case Op::Ge: relational(op); return true;
            case Op::StrictEq:
            case Op::StrictNe:
            case Op::Eq:
            case Op::Ne: equality(op); return true;
            case Op::Not: {
                const Entry a = pop();
                const std::int32_t t = truthyReg(a);
                release(a);
                const std::int32_t d = fn_.newVreg(false);
                IrInst& in = inst(IrOp::Cset);
                in.d = d;
                in.a = t;
                in.cond = IrCond::Eq;
                in.hasImm = true;
                in.imm = 0;
                in.width = 32;
                push(boolEntry(d));
                return true;
            }
            case Op::TypeOf:
            case Op::ToString:
            case Op::ToPropertyKey: {
                Entry a = pop();
                secureWeak();
                void* helper = op == Op::TypeOf     ? reinterpret_cast<void*>(&scriptDynTypeOf)
                               : op == Op::ToString ? reinterpret_cast<void*>(&scriptDynToStringOp)
                                                    : reinterpret_cast<void*>(&scriptDynToPropertyKeyOp);
                const std::int32_t rt = rtReg();
                const std::int32_t tag = tagReg(a);
                const std::int32_t payload = payloadReg(a);
                const std::int32_t r = fn_.newVreg(false);
                const std::int32_t t = fn_.newVreg(false);
                coldCall(helper, {rt, tag, payload}, r, t, true, {a});
                release(a);
                push(taggedEntry(r, t, Own::Owned));
                return true;
            }
            default: break;
        }
        refuse(std::string(opName(op)) + " is not compiled yet");
    }

    /// `x instanceof C` for the plain function C is known to be: up the prototype chain right here, while no prototype's
    /// table and not C's has changed, which says nothing defines `Symbol.hasInstance`. False, with nothing emitted, when it
    /// is not that.
    bool instanceOfInPlace(Entry& obj, Entry& ctor)
    {
        if (!ctor.pinned || !betHere() || obj.rep != Rep::Tagged || obj.isConst)
            return false;
        auto* fn = const_cast<DynFunction*>(static_cast<const DynFunction*>(ctor.hint));
        GCObject* hasInstance = rt_.sym.hasInstance.get();
        if (fn->proto.get() != rt_.functionProto.get() || !fn->shape || fn->findOwn(hasInstance))
            return false;
        for (DynObject* cursor = rt_.functionProto.get(); cursor; cursor = cursor->proto.get()) {
            if (cursor->findOwn(hasInstance))
                return false;
        }
        const Prop* prototype = fn->findOwn(rt_.atom.prototype.get());
        if (!prototype || fn->accessorOf(*prototype) || !prototype->value.isObject())
            return false;
        const std::int64_t at = std::int64_t(prototype - fn->props.data()) * std::int64_t(sizeof(Prop));
        const std::int32_t bail = newLabel();
        const std::int32_t no = newLabel();
        const std::int32_t yes = newLabel();
        const std::int32_t done = newLabel();
        const std::int32_t d = fn_.newVreg(false);
        expectEpoch(rt_.ctx.protoEpoch, bail);
        expectShape(ctor, fn->shape, bail);
        const std::int32_t props = loadHeader(ctor.r, kPropsOffset);
        const std::int32_t protoTag = load(props, at + kPropValueOffset, 1, false, kAliasValue);
        branchCmp(protoTag, IrCond::Ne, std::int64_t(Type::Object), bail);
        const std::int32_t target = load(props, at + kPropValueOffset + kPayload, 8, false, kAliasValue);
        if (!obj.hasKnownTag)
            branchCmp(obj.t, IrCond::Ne, std::int64_t(Type::Object), no);
        else if (obj.known != Type::Object)
            inst(IrOp::Br).imm = no;
        // Anything else tagged an object, a host's cell above all, has its prototype elsewhere.
        const std::int32_t kind = load(obj.r, kCellKindOffset, 1, false, kAliasHeader);
        branchCmp(kind, IrCond::Ne, std::int64_t(CellKind::Object), bail);
        const std::int32_t cursor = fn_.newVreg(false);
        {
            IrInst& first = inst(IrOp::Load);
            first.d = cursor;
            first.a = obj.r;
            first.imm = kProtoOffset;
            first.width = 8;
        }
        const std::int32_t walk = newLabel();
        inst(IrOp::Label).imm = walk;
        branchCmp(cursor, IrCond::Eq, 0, no, 64);
        {
            IrInst& same = inst(IrOp::BrCmp);
            same.a = cursor;
            same.b = target;
            same.cond = IrCond::Eq;
            same.width = 64;
            same.target = yes;
        }
        {
            IrInst& up = inst(IrOp::Load);
            up.d = cursor;
            up.a = cursor;
            up.imm = kProtoOffset;
            up.width = 8;
        }
        inst(IrOp::Br).imm = walk;
        inst(IrOp::Label).imm = yes;
        {
            IrInst& one = inst(IrOp::MovImm);
            one.d = d;
            one.imm = 1;
        }
        inst(IrOp::Br).imm = done;
        inst(IrOp::Label).imm = no;
        {
            IrInst& zero = inst(IrOp::MovImm);
            zero.d = d;
            zero.imm = 0;
        }
        inst(IrOp::Label).imm = done;
        emitBail(bail, {obj, ctor});
        forgetFacts();
        release(ctor);
        release(obj);
        push(boolEntry(d));
        return true;
    }

    void binaryPredicate(void* helper, Entry& a, Entry& b)
    {
        secureWeak();
        const std::int32_t rt = rtReg();
        const std::int32_t d = fn_.newVreg(false);
        coldCall(helper, {rt, tagReg(a), payloadReg(a), tagReg(b), payloadReg(b)}, d, -1, true, {a, b});
        release(b);
        release(a);
        push(boolEntry(d));
    }

    void releaseLocals(std::int32_t keep)
    {
        for (std::size_t slot = 0; slot < localReg_.size(); ++slot) {
            if (localOwned_[slot] && localTag_[slot] >= 0 && std::int32_t(slot) != keep)
                decRefOf(localReg_[slot], localTag_[slot], false);
            if (cellOwned_[slot] && cellReg_[slot] >= 0)
                decRefOf(cellReg_[slot], -1, true);
        }
    }

    void storeLocal(std::uint32_t slot)
    {
        Entry value = pop();
        copyOutReaders(slot);
        forgetShape(localReg_[slot]);
        if (isNumberLocal(slot)) {
            // Nothing reads the slot before a number is written to it: see `readsBeforeWrite`.
            if (previousOp_ == Op::PushUndefined && value.isConst && value.rep == Rep::Tagged && value.hasKnownTag &&
                value.known == Type::Undefined)
                return;
            if (!isKnownNumber(value))
                throw LowerAgain{slot};
            if (isIntLocal(slot)) {
                if (!isIntLike(value))
                    throw LowerAgain{slot, true};
                // Kept sign-extended, so that reading it needs no extension.
                const std::int32_t from = wideInt(value);
                IrInst& mov = inst(IrOp::Mov);
                mov.d = localReg_[slot];
                mov.a = from;
                return;
            }
            const std::int32_t from = doubleReg(value);
            IrInst& mov = inst(IrOp::Mov);
            mov.d = localReg_[slot];
            mov.a = from;
            return;
        }
        secureWeak();
        makeOwned(value);
        const std::int32_t tag = tagReg(value);
        const std::int32_t payload = payloadReg(value);
        if (!localOwned_[slot])
            refuse("a borrowed parameter is assigned");
        const std::int32_t oldTag = fn_.newVreg(false);
        const std::int32_t old = fn_.newVreg(false);
        IrInst& saveTag = inst(IrOp::Mov);
        saveTag.d = oldTag;
        saveTag.a = localTag_[slot];
        IrInst& save = inst(IrOp::Mov);
        save.d = old;
        save.a = localReg_[slot];
        IrInst& movTag = inst(IrOp::Mov);
        movTag.d = localTag_[slot];
        movTag.a = tag;
        IrInst& mov = inst(IrOp::Mov);
        mov.d = localReg_[slot];
        mov.a = payload;
        decRefOf(old, oldTag, false);
    }

    void readBox(std::int32_t box)
    {
        // A cell that was never made reads as undefined. That hardly ever happens, and code that stops there has only one
        // way on, along which what it knows of the cell holds.
        if (betHere()) {
            const std::int32_t bail = newLabel();
            branchCmp(box, IrCond::Eq, 0, bail, 64);
            const std::int32_t lt = load(box, kBoxValueOffset, 1);
            const std::int32_t lr = load(box, kBoxValueOffset + kPayload, 8);
            emitBail(bail, {});
            secureWeak();
            push(taggedEntry(lr, lt, Own::Weak));
            return;
        }
        const std::int32_t t = fn_.newVreg(false);
        const std::int32_t r = fn_.newVreg(false);
        const std::int32_t none = newLabel();
        const std::int32_t done = newLabel();
        branchCmp(box, IrCond::Eq, 0, none, 64);
        const std::int32_t lt = load(box, kBoxValueOffset, 1);
        const std::int32_t lr = load(box, kBoxValueOffset + kPayload, 8);
        IrInst& mt = inst(IrOp::Mov);
        mt.d = t;
        mt.a = lt;
        IrInst& mr = inst(IrOp::Mov);
        mr.d = r;
        mr.a = lr;
        inst(IrOp::ColdBegin);
        inst(IrOp::Label).imm = none;
        IrInst& it = inst(IrOp::MovImm);
        it.d = t;
        it.imm = std::int64_t(Type::Undefined);
        IrInst& ir = inst(IrOp::MovImm);
        ir.d = r;
        ir.imm = 0;
        inst(IrOp::Br).imm = done;
        inst(IrOp::ColdEnd);
        inst(IrOp::Label).imm = done;
        secureWeak();
        push(taggedEntry(r, t, Own::Weak));
    }

    void writeBox(std::int32_t box)
    {
        Entry value = pop();
        secureWeak();
        // The cell takes the value's count; what it held loses one, after, since releasing it could free the new value
        // were they the same.
        makeOwned(value);
        const std::int32_t tag = tagReg(value);
        const std::int32_t payload = payloadReg(value);
        if (betHere()) {
            const std::int32_t bail = newLabel();
            branchCmp(box, IrCond::Eq, 0, bail, 64);
            const std::int32_t oldTag = load(box, kBoxValueOffset, 1);
            const std::int32_t old = load(box, kBoxValueOffset + kPayload, 8);
            store(box, kBoxValueOffset, tag, 1);
            store(box, kBoxValueOffset + kPayload, payload, 8);
            decRefOf(old, oldTag, false);
            emitBail(bail, {value});
            return;
        }
        const std::int32_t skip = newLabel();
        const std::int32_t done = newLabel();
        // A cell that was never made drops the write, as in the interpreter.
        branchCmp(box, IrCond::Eq, 0, skip, 64);
        const std::int32_t oldTag = load(box, kBoxValueOffset, 1);
        const std::int32_t old = load(box, kBoxValueOffset + kPayload, 8);
        store(box, kBoxValueOffset, tag, 1);
        store(box, kBoxValueOffset + kPayload, payload, 8);
        decRefOf(old, oldTag, false);
        inst(IrOp::Br).imm = done;
        inst(IrOp::Label).imm = skip;
        release(value);
        inst(IrOp::Label).imm = done;
    }

    /// The address of a global's property, after a check that the table has not changed since it was found there.
    std::int32_t globalSlot(const Prop* found, std::int32_t slow)
    {
        const std::int32_t runtime = rtReg();
        const std::int64_t offset =
            std::int64_t(reinterpret_cast<std::uintptr_t>(&rt_.globalEpoch) - reinterpret_cast<std::uintptr_t>(&rt_));
        // Only the runtime's own code changes it, never a store here.
        const std::int32_t epoch = load(runtime, offset, 8, false, kAliasHeader);
        if (rt_.globalEpoch < 4096) {
            branchCmp(epoch, IrCond::Ne, std::int64_t(rt_.globalEpoch), slow, 64);
        } else {
            const std::int32_t want = immReg(std::int64_t(rt_.globalEpoch));
            IrInst& test = inst(IrOp::BrCmp);
            test.a = epoch;
            test.b = want;
            test.cond = IrCond::Ne;
            test.width = 64;
            test.target = slow;
        }
        return ptrReg(found);
    }

    bool assignsGlobal(const GCObject* key)
    {
        if (!globalsScanned_) {
            globalsScanned_ = true;
            for (std::uint32_t pc = 0; pc < code_.code.size();) {
                const Op op = Op(code_.code[pc]);
                if (op == Op::SetGlobal)
                    assignedGlobals_.push_back(code_.atoms[readOperand(code_.code, pc + 1)].get());
                pc += 1 + std::uint32_t(operandsOf(op)) * 4;
            }
        }
        return std::find(assignedGlobals_.begin(), assignedGlobals_.end(), key) != assignedGlobals_.end();
    }

    void getGlobal(GCObject* key)
    {
        secureWeak();
        const std::int32_t r = fn_.newVreg(false);
        const std::int32_t t = fn_.newVreg(false);
        const std::int32_t slow = newLabel();
        const std::int32_t done = newLabel();
        DynObject* global = rt_.globalObject.get();
        const Prop* found = global->findOwn(key);
        // A global holding a number, which nothing here assigns, is taken to be a constant, and treated as one from here on.
        if (found && !global->accessorOf(*found) && found->value.isDouble() && betHere() && !assignsGlobal(key)) {
            const std::int32_t bail = newLabel();
            const std::int32_t slot = globalSlot(found, bail);
            const std::int32_t lt = load(slot, kPropValueOffset, 1, false, kAliasValue);
            branchCmp(lt, IrCond::Ne, std::int64_t(Type::Double), bail);
            const std::int32_t lr = load(slot, kPropValueOffset + kPayload, 8, false, kAliasValue);
            const double value = detail::Access::r8(found->value);
            std::uint64_t bits = 0;
            std::memcpy(&bits, &value, sizeof bits);
            const std::int32_t want = immReg(std::int64_t(bits));
            IrInst& same = inst(IrOp::BrCmp);
            same.a = lr;
            same.b = want;
            same.cond = IrCond::Ne;
            same.width = 64;
            same.target = bail;
            emitBail(bail, {});
            push(constDouble(value));
            return;
        }
        // Where the property was when this was compiled, while the global object's table has not changed since; once the
        // table has changed under this, the interpreter reads it, and later compiles call for it.
        if (found && !global->accessorOf(*found) && betHere()) {
            const std::int32_t bail = newLabel();
            const std::int32_t slot = globalSlot(found, bail);
            Entry result = taggedEntry(load(slot, kPropValueOffset + kPayload, 8, false, kAliasValue),
                                       load(slot, kPropValueOffset, 1, false, kAliasValue), Own::Weak);
            emitBail(bail, {});
            if (found->value.isObject())
                result.hint = detail::Access::ptr(found->value);
            pin(result);
            push(result);
            return;
        }
        if (found && !global->accessorOf(*found)) {
            const std::int32_t slot = globalSlot(found, slow);
            const std::int32_t lt = load(slot, kPropValueOffset, 1, false, kAliasValue);
            const std::int32_t lr = load(slot, kPropValueOffset + kPayload, 8, false, kAliasValue);
            IrInst& mt = inst(IrOp::Mov);
            mt.d = t;
            mt.a = lt;
            IrInst& mr = inst(IrOp::Mov);
            mr.d = r;
            mr.a = lr;
        } else {
            inst(IrOp::Br).imm = slow;
        }
        inst(IrOp::ColdBegin);
        inst(IrOp::Label).imm = slow;
        const std::int32_t rt = rtReg();
        const std::int32_t k2 = ptrReg(key);
        coldCall(reinterpret_cast<void*>(&scriptDynGetGlobal), {rt, k2}, r, t, true);
        inst(IrOp::Br).imm = done;
        inst(IrOp::ColdEnd);
        inst(IrOp::Label).imm = done;
        Entry result = taggedEntry(r, t, Own::Weak);
        if (found && !global->accessorOf(*found) && found->value.isObject())
            result.hint = detail::Access::ptr(found->value);
        pin(result);
        push(result);
    }

    /// A function read from somewhere that may change, a global or a property, is the one it was when this was compiled,
    /// or the interpreter carries on from here. The code keeps that function alive, so holding it needs no count, and
    /// calling it needs nothing checked but this.
    void pin(Entry& e, const std::vector<Entry>& popped = {})
    {
        if (!e.hint || e.own != Own::Weak || e.rep != Rep::Tagged || e.isConst || !betHere())
            return;
        const auto* target = static_cast<const DynObject*>(e.hint);
        if (target->kind != CellKind::Object || target->cls != ObjClass::Function)
            return;
        const auto* fn = static_cast<const DynFunction*>(target);
        if (fn->fnKind != FnKind::Bytecode || !fn->code || fn->isClassConstructor)
            return;
        const std::int32_t bail = newLabel();
        if (!e.hasKnownTag)
            branchCmp(e.t, IrCond::Ne, std::int64_t(Type::Object), bail);
        const std::int32_t want = ptrReg(fn);
        IrInst& same = inst(IrOp::BrCmp);
        same.a = e.r;
        same.b = want;
        same.cond = IrCond::Ne;
        same.width = 64;
        same.target = bail;
        emitBail(bail, popped);
        keepAlive(const_cast<DynFunction*>(fn));
        e.own = Own::Immortal;
        e.hasKnownTag = true;
        e.known = Type::Object;
        e.pinned = true;
    }

    /// Guards that the value is a `DynObject` with this shape: anything else goes to `slow`.
    /// The shape of a value that has to be a `DynObject`: anything else goes to `slow`.
    std::int32_t shapeOf(const Entry& base, std::int32_t slow)
    {
        if (thisIsCell_ && base.r == thisReg_ && !base.isConst)
            return loadHeader(base.r, kShapeOffset);
        if (!base.hasKnownTag)
            branchCmp(base.t, IrCond::Ne, std::int64_t(Type::Object), slow);
        else if (base.known != Type::Object)
            inst(IrOp::Br).imm = slow;
        // Whatever else the tag may point at keeps something there that is never a shape; `heap.cpp` checks.
        return loadHeader(base.r, kShapeOffset);
    }

    struct ShapeCase
    {
        std::int32_t label;
        std::uint32_t index;
        const DynObject* holder;  ///< where the property is, null for the receiver
    };

    /// Branches to one label per shape the site knows, or to `slow`. Cases up a prototype chain are left out unless the
    /// epoch they were learnt at is still current, and then the epoch is checked once at run time.
    std::vector<ShapeCase> dispatchShapes(const Entry& base, const PropertySite* site, std::int32_t slow)
    {
        const bool chainOk = site->epoch == rt_.ctx.protoEpoch;
        std::vector<std::pair<const Shape*, ShapeCase>> wanted;
        if (!site->holder || chainOk)
            wanted.push_back({site->shape, {newLabel(), site->index, site->holder}});
        for (std::uint8_t i = 0; i < site->moreCount; ++i) {
            if (!site->moreHolder[i] || chainOk)
                wanted.push_back({site->moreShape[i], {newLabel(), site->moreIndex[i], site->moreHolder[i]}});
        }
        bool anyHolder = false;
        for (const auto& w : wanted)
            anyHolder = anyHolder || w.second.holder;
        const std::int32_t have = shapeOf(base, slow);
        if (anyHolder)
            guardEpoch(site->epoch, slow);
        std::vector<ShapeCase> cases;
        for (const auto& [shape, c] : wanted) {
            const std::int32_t want = ptrReg(shape);
            IrInst& test = inst(IrOp::BrCmp);
            test.a = have;
            test.b = want;
            test.cond = IrCond::Eq;
            test.width = 64;
            test.target = c.label;
            cases.push_back(c);
        }
        inst(IrOp::Br).imm = slow;
        return cases;
    }

    struct StoreCase
    {
        std::int32_t label;
        std::uint32_t index;
        const Shape* addTo;  ///< the shape a store that adds the property leads to, null for one that overwrites
    };

    /// The same for a store. Cases that add the property need the epoch they were learnt at, and room for an index not to
    /// be needed; any others are left to `slow`.
    std::vector<StoreCase> dispatchStores(const Entry& base, const PropertySite* site, std::int32_t slow)
    {
        const bool chainOk = site->epoch == rt_.ctx.protoEpoch;
        std::vector<std::pair<const Shape*, StoreCase>> wanted;
        const auto want = [&](const Shape* shape, std::uint32_t index, const Shape* addTo) {
            if (!addTo || (chainOk && addTo->count <= DynObject::kIndexThreshold))
                wanted.push_back({shape, {newLabel(), index, addTo}});
        };
        want(site->shape, site->index, site->addTo);
        for (std::uint8_t i = 0; i < site->moreCount; ++i)
            want(site->moreShape[i], site->moreIndex[i], site->moreAddTo[i]);
        bool anyAdds = false;
        for (const auto& w : wanted)
            anyAdds = anyAdds || w.second.addTo;
        const std::int32_t have = shapeOf(base, slow);
        if (anyAdds)
            guardEpoch(site->epoch, slow);
        std::vector<StoreCase> cases;
        for (const auto& [shape, c] : wanted) {
            const std::int32_t wantShape = ptrReg(shape);
            IrInst& test = inst(IrOp::BrCmp);
            test.a = have;
            test.b = wantShape;
            test.cond = IrCond::Eq;
            test.width = 64;
            test.target = c.label;
            cases.push_back(c);
        }
        inst(IrOp::Br).imm = slow;
        return cases;
    }

    void guardShape(const Entry& base, const Shape* shape, std::int32_t slow)
    {
        const std::int32_t have = shapeOf(base, slow);
        const std::int32_t want = ptrReg(shape);
        IrInst& test = inst(IrOp::BrCmp);
        test.a = have;
        test.b = want;
        test.cond = IrCond::Ne;
        test.width = 64;
        test.target = slow;
    }

    void guardEpoch(std::uint64_t epoch, std::int32_t slow)
    {
        const std::int32_t where = ptrReg(&rt_.ctx.protoEpoch);
        const std::int32_t now = load(where, 0, 8);
        const std::int32_t want = immReg(std::int64_t(epoch));
        IrInst& test = inst(IrOp::BrCmp);
        test.a = now;
        test.b = want;
        test.cond = IrCond::Ne;
        test.width = 64;
        test.target = slow;
    }

    const PropertySite* siteAt(std::uint32_t operand) const noexcept
    {
        const std::uint32_t index = operand >> kSiteShift;
        if (index == kNoSite || code_.sites.empty() || index >= code_.sites.size())
            return nullptr;
        const PropertySite& site = code_.sites[index];
        return site.shape ? &site : nullptr;
    }

    void getProp(std::uint32_t operand, bool keepBase)
    {
        Entry base = pop();
        // Anything but a read the interpreter takes over from when it is not what was guessed can run a getter, which
        // could change what a borrowed entry reads; and the receiver stays on the stack past that.
        const auto secure = [&] {
            secureWeak();
            if (keepBase && base.own == Own::Weak)
                makeOwned(base);
        };
        // The operand could be a primitive with no tag register to compare: bits and a tag it is, here on.
        if (base.rep != Rep::Tagged || base.isConst) {
            const std::int32_t tag = tagReg(base);
            const std::int32_t payload = payloadReg(base);
            Own own = base.own;
            base = taggedEntry(payload, tag, own == Own::None ? Own::None : own);
        }
        if (!keepBase && code_.atoms[operand & kAtomMask].get() == rt_.atom.length.get()) {
            secure();
            lengthProp(base, operand);
            return;
        }
        const PropertySite* site = siteAt(operand);
        if (!site && stringMethod(base, operand, keepBase))
            return;
        if (neverRan(operand)) {
            std::vector<Entry> popped{base};
            leaveForTheInterpreter(popped);
            Entry result = taggedEntry(fn_.newVreg(false), fn_.newVreg(false), Own::Weak);
            IrInst& tag = inst(IrOp::MovImm);
            tag.d = result.t;
            tag.imm = std::int64_t(Type::Undefined);
            IrInst& payload = inst(IrOp::MovImm);
            payload.d = result.r;
            payload.imm = 0;
            push(result);
            if (keepBase)
                push(base);
            return;
        }
        const std::int32_t r = fn_.newVreg(false);
        const std::int32_t t = fn_.newVreg(false);
        const std::int32_t slow = newLabel();
        const std::int32_t done = newLabel();
        const bool usable = site && (!site->holder || site->epoch == rt_.ctx.protoEpoch);
        if (usable && site->getter) {
            secure();
            getterProp(base, site, operand, keepBase);
            return;
        }
        const bool bet = betHere() && usable && !site->getter && !site->addTo && !site->moreCount &&
                         base.rep == Rep::Tagged && !base.isConst;
        const bool polyBet = !bet && betHere() && site && !site->getter && !site->addTo && site->moreCount &&
                             base.rep == Rep::Tagged && !base.isConst;
        if (!bet && !polyBet)
            secure();
        Entry result;
        if (bet) {
            // One shape seen: guard it, or what is known of it, and leave the rest to the interpreter.
            const std::int32_t bail = newLabel();
            expectShape(base, site->shape, bail);
            std::int32_t holder = base.r;
            if (site->holder) {
                expectEpoch(site->epoch, bail);
                holder = ptrReg(site->holder);
            }
            const std::int64_t at = std::int64_t(site->index) * std::int64_t(sizeof(Prop));
            const std::int32_t props = loadHeader(holder, kPropsOffset);
            const std::int32_t lt = load(props, at + kPropValueOffset, 1, false, kAliasValue);
            if (site->saw == PropertySite::kSawNumber) {
                branchCmp(lt, IrCond::Ne, std::int64_t(Type::Double), bail);
                result = doubleEntry(load(props, at + kPropValueOffset + kPayload, 8, true, kAliasValue));
            } else {
                result = taggedEntry(load(props, at + kPropValueOffset + kPayload, 8, false, kAliasValue), lt, Own::Weak);
            }
            emitBail(bail, {base});
        } else if (polyBet) {
            // A few shapes seen: one case each, and none of them the interpreter's.
            const std::int32_t bail = newLabel();
            const bool number = site->saw == PropertySite::kSawNumber;
            const std::int32_t d = number ? fn_.newVreg(true) : -1;
            for (const ShapeCase& c : dispatchShapes(base, site, bail)) {
                inst(IrOp::Label).imm = c.label;
                const std::int64_t at = std::int64_t(c.index) * std::int64_t(sizeof(Prop));
                const std::int32_t props = loadHeader(c.holder ? ptrReg(c.holder) : base.r, kPropsOffset);
                const std::int32_t lt = load(props, at + kPropValueOffset, 1, false, kAliasValue);
                if (number) {
                    branchCmp(lt, IrCond::Ne, std::int64_t(Type::Double), bail);
                    const std::int32_t v = load(props, at + kPropValueOffset + kPayload, 8, true, kAliasValue);
                    IrInst& md = inst(IrOp::Mov);
                    md.d = d;
                    md.a = v;
                } else {
                    const std::int32_t lr = load(props, at + kPropValueOffset + kPayload, 8, false, kAliasValue);
                    IrInst& mt = inst(IrOp::Mov);
                    mt.d = t;
                    mt.a = lt;
                    IrInst& mr = inst(IrOp::Mov);
                    mr.d = r;
                    mr.a = lr;
                }
                inst(IrOp::Br).imm = done;
            }
            emitBail(bail, {base});
            inst(IrOp::Label).imm = done;
            result = number ? doubleEntry(d) : taggedEntry(r, t, Own::Weak);
        } else if (site && !site->getter && !site->addTo && site->moreCount) {
            for (const ShapeCase& c : dispatchShapes(base, site, slow)) {
                inst(IrOp::Label).imm = c.label;
                const std::int64_t at = std::int64_t(c.index) * std::int64_t(sizeof(Prop));
                const std::int32_t props = loadHeader(c.holder ? ptrReg(c.holder) : base.r, kPropsOffset);
                const std::int32_t lt = load(props, at + kPropValueOffset, 1, false, kAliasValue);
                const std::int32_t lr = load(props, at + kPropValueOffset + kPayload, 8, false, kAliasValue);
                IrInst& mt = inst(IrOp::Mov);
                mt.d = t;
                mt.a = lt;
                IrInst& mr = inst(IrOp::Mov);
                mr.d = r;
                mr.a = lr;
                inst(IrOp::Br).imm = done;
            }
        } else if (usable) {
            guardShape(base, site->shape, slow);
            std::int32_t holder = base.r;
            if (site->holder) {
                guardEpoch(site->epoch, slow);
                holder = ptrReg(site->holder);
            }
            const std::int64_t at = std::int64_t(site->index) * std::int64_t(sizeof(Prop));
            const std::int32_t props = loadHeader(holder, kPropsOffset);
            const std::int32_t lt = load(props, at + kPropValueOffset, 1, false, kAliasValue);
            const std::int32_t lr = load(props, at + kPropValueOffset + kPayload, 8, false, kAliasValue);
            IrInst& mt = inst(IrOp::Mov);
            mt.d = t;
            mt.a = lt;
            IrInst& mr = inst(IrOp::Mov);
            mr.d = r;
            mr.a = lr;
        } else {
            inst(IrOp::Br).imm = slow;
        }
        if (!bet && !polyBet) {
            inst(IrOp::ColdBegin);
            inst(IrOp::Label).imm = slow;
            const std::int32_t rt = rtReg();
            const std::int32_t self = ptrReg(&code_);
            const std::int32_t op = immReg(std::int64_t(operand));
            coldCall(reinterpret_cast<void*>(&scriptDynGetProp), {rt, self, tagReg(base), base.r, op}, r, t, true, {base});
            inst(IrOp::Br).imm = done;
            inst(IrOp::ColdEnd);
            inst(IrOp::Label).imm = done;
            result = taggedEntry(r, t, Own::Weak);
        }
        // What the callee is likely to be, for a built-in done in place: guarded where it is used, so a guess is fine.
        if (usable && site->holder && !site->getter && site->index < site->holder->props.size() &&
            site->holder->props[site->index].value.isObject())
            result.hint = detail::Access::ptr(site->holder->props[site->index].value);
        if (!result.hint && keepBase) {
            GCObject* key = code_.atoms[operand & kAtomMask].get();
            const auto named = [&](const char* name, Runtime::Intrinsic which) {
                if (key == rt_.intern(name))
                    result.hint = rt_.intrinsics[which].get();
            };
            named("push", Runtime::ArrayPush);
            named("pop", Runtime::ArrayPop);
            named("charCodeAt", Runtime::StringCharCodeAt);
            named("apply", Runtime::FunctionApply);
            named("call", Runtime::FunctionCall);
        }
        if (usable && !site->holder && !site->moreCount && base.hint) {
            const auto* known = static_cast<const DynObject*>(base.hint);
            if (known->kind == CellKind::Object && known->shape == site->shape && site->index < known->props.size() &&
                known->props[site->index].value.isObject())
                result.hint = detail::Access::ptr(known->props[site->index].value);
        }
        if (usable)
            pin(result, {base});
        if (keepBase) {
            // func obj: the function under the receiver.
            push(result);
            push(base);
            return;
        }
        if (base.own == Own::Owned && !result.pinned) {
            makeOwned(result);
            push(result);
            release(base);
        } else {
            push(result);
        }
    }

    /// A site the interpreter has been through without learning anything has been there to see that; one it has never been
    /// through is code that has not run. That is a guess too, and the interpreter finds out more than this would.
    bool neverRan(std::uint32_t operand) const
    {
        if (!betHere() || rt_.ctx.options.jit.threshold <= 1 || rt_.ctx.options.jit.mode == JitOptions::Mode::Eager)
            return false;
        const std::uint32_t index = operand >> kSiteShift;
        if (index == kNoSite)
            return false;
        if (code_.sites.empty())
            return true;
        return index < code_.sites.size() && !code_.sites[index].shape && code_.sites[index].misses == 0;
    }
    void leaveForTheInterpreter(const std::vector<Entry>& popped)
    {
        const std::int32_t bail = newLabel();
        inst(IrOp::Br).imm = bail;
        emitBail(bail, popped);
    }
    /// `s.charCodeAt` and its kind on a string, at a site that has seen no object: the method read from where it is on
    /// `String.prototype`, while no prototype's table has changed. Otherwise the interpreter carries on, or where a bail
    /// has been blamed on this instruction, which may have been for a check of an earlier one left until here, the long
    /// way is taken. False when this is no place for it.
    bool stringMethod(Entry base, std::uint32_t operand, bool keepBase)
    {
        if (!speculate_ || base.rep != Rep::Tagged || (base.hasKnownTag && base.known != Type::String))
            return false;
        DynObject* proto = rt_.stringProto.get();
        GCObject* key = code_.atoms[operand & kAtomMask].get();
        const Prop* found = proto ? proto->findOwn(key) : nullptr;
        if (!found || proto->accessorOf(*found))
            return false;
        const std::uint32_t index = std::uint32_t(found - proto->props.data());
        const bool bet = betHere();
        const std::int32_t miss = newLabel();
        if (!bet) {
            secureWeak();
            if (keepBase && base.own == Own::Weak)
                makeOwned(base);
        }
        if (!base.hasKnownTag)
            branchCmp(base.t, IrCond::Ne, std::int64_t(Type::String), miss);
        if (bet)
            expectEpoch(rt_.ctx.protoEpoch, miss);
        else
            guardEpoch(rt_.ctx.protoEpoch, miss);
        const std::int64_t at = std::int64_t(index) * std::int64_t(sizeof(Prop));
        const std::int32_t props = loadHeader(ptrReg(proto), kPropsOffset);
        const std::int32_t lt = load(props, at + kPropValueOffset, 1, false, kAliasValue);
        const std::int32_t lr = load(props, at + kPropValueOffset + kPayload, 8, false, kAliasValue);
        Entry result;
        if (bet) {
            emitBail(miss, {base});
            result = taggedEntry(lr, lt, Own::Weak);
        } else {
            const std::int32_t r = fn_.newVreg(false);
            const std::int32_t t = fn_.newVreg(false);
            const std::int32_t done = newLabel();
            IrInst& mt = inst(IrOp::Mov);
            mt.d = t;
            mt.a = lt;
            IrInst& mr = inst(IrOp::Mov);
            mr.d = r;
            mr.a = lr;
            inst(IrOp::Br).imm = done;
            inst(IrOp::ColdBegin);
            inst(IrOp::Label).imm = miss;
            const std::int32_t rt = rtReg();
            const std::int32_t self = ptrReg(&code_);
            const std::int32_t op = immReg(std::int64_t(operand));
            coldCall(reinterpret_cast<void*>(&scriptDynGetProp), {rt, self, tagReg(base), base.r, op}, r, t, true, {base});
            inst(IrOp::Br).imm = done;
            inst(IrOp::ColdEnd);
            inst(IrOp::Label).imm = done;
            result = taggedEntry(r, t, Own::Weak);
        }
        if (found->value.isObject())
            result.hint = detail::Access::ptr(found->value);
        // The prototype is held by the runtime for as long as the code runs: what it holds is borrowed from there. What the
        // long way found is only borrowed for now, and it could be the receiver's alone.
        if (keepBase) {
            push(result);
            push(base);
            return true;
        }
        if (!bet && base.own == Own::Owned)
            makeOwned(result);
        push(result);
        release(base);
        return true;
    }

    /// A store that adds the property, in place while the table has room and no index: what `appendKnown` does. The
    /// shape and the epoch are checked already.
    void appendInPlace(const Entry& base, const Shape* addTo, std::int32_t vt, std::int32_t vr, std::int32_t slow)
    {
        // The shape this adds to says how many there are.
        const std::uint32_t count = addTo->count - 1;
        // The object a constructor made in place, which nothing else has seen yet, is all as it was made.
        if (!freshThis_ || base.r != thisReg_ || addTo->count > freshRoom_) {
            const std::int32_t isProto = load(base.r, kIsProtoOffset, 1, false, kAliasHeader);
            branchCmp(isProto, IrCond::Ne, 0, slow);
            // No index to keep up, nor anything else out of the ordinary.
            const std::int32_t rare = load(base.r, kRareOffset, 8, false, kAliasHeader);
            branchCmp(rare, IrCond::Ne, 0, slow, 64);
            const std::int32_t cap = load(base.r, kPropsCapOffset, 4, false, kAliasHeader);
            branchCmp(cap, IrCond::Le, std::int64_t(count), slow);
        }
        const std::int32_t props = loadHeader(base.r, kPropsOffset);
        const std::int64_t at = std::int64_t(count) * std::int64_t(sizeof(Prop));
        store(props, at + kPropValueOffset, vt, 1);
        store(props, at + kPropValueOffset + kPayload, vr, 8);
        store(base.r, kPropsSizeOffset, immReg(std::int64_t(count) + 1), 4, kAliasHeader);
        storeHeader(base.r, kShapeOffset, ptrReg(addTo));
    }

    void setProp(std::uint32_t operand)
    {
        Entry value = pop();
        Entry base = pop();
        if (neverRan(operand)) {
            leaveForTheInterpreter({base, value});
            // What follows expects the value back, and is not reached.
            value.own = Own::None;
            push(value);
            return;
        }
        secureWeak();
        makeOwned(value);
        if (base.rep != Rep::Tagged || base.isConst) {
            const std::int32_t tag = tagReg(base);
            const std::int32_t payload = payloadReg(base);
            base = taggedEntry(payload, tag, base.own);
        }
        const std::int32_t vt = tagReg(value);
        const std::int32_t vr = payloadReg(value);
        const PropertySite* site = siteAt(operand);
        const std::int32_t slow = newLabel();
        const std::int32_t done = newLabel();
        const bool bet = betHere() && site && !site->moreCount && base.rep == Rep::Tagged && !base.isConst &&
                         (!site->addTo || (site->epoch == rt_.ctx.protoEpoch && site->addTo->count <= DynObject::kIndexThreshold));
        if (bet) {
            const std::int32_t bail = newLabel();
            expectShape(base, site->shape, bail);
            if (site->addTo) {
                expectEpoch(site->epoch, bail);
                appendInPlace(base, site->addTo, vt, vr, bail);
            } else if (isKnownNumber(value)) {
                // A number over a number, the usual thing for a field that holds one, is only its bits.
                const std::int64_t at = std::int64_t(site->index) * std::int64_t(sizeof(Prop));
                const std::int32_t props = loadHeader(base.r, kPropsOffset);
                const std::int32_t oldTag = load(props, at + kPropValueOffset, 1, false, kAliasValue);
                const std::int32_t other = newLabel();
                const std::int32_t stored = newLabel();
                branchCmp(oldTag, IrCond::Ne, std::int64_t(Type::Double), other);
                const std::int32_t bits = value.rep == Rep::Double && !value.isConst ? value.r : vr;
                store(props, at + kPropValueOffset + kPayload, bits, 8);
                inst(IrOp::ColdBegin);
                inst(IrOp::Label).imm = other;
                const std::int32_t old = load(props, at + kPropValueOffset + kPayload, 8, false, kAliasValue);
                store(props, at + kPropValueOffset, vt, 1);
                store(props, at + kPropValueOffset + kPayload, bits, 8);
                decRefOf(old, oldTag, false);
                inst(IrOp::Br).imm = stored;
                inst(IrOp::ColdEnd);
                // Whichever way it came, the field holds this.
                IrInst& join = inst(IrOp::Label);
                join.imm = stored;
                join.alias = 1;
            } else {
                const std::int64_t at = std::int64_t(site->index) * std::int64_t(sizeof(Prop));
                const std::int32_t props = loadHeader(base.r, kPropsOffset);
                const std::int32_t oldTag = load(props, at + kPropValueOffset, 1, false, kAliasValue);
                const std::int32_t old = load(props, at + kPropValueOffset + kPayload, 8, false, kAliasValue);
                store(props, at + kPropValueOffset, vt, 1);
                store(props, at + kPropValueOffset + kPayload, vr, 8);
                decRefOf(old, oldTag, false);
            }
            emitBail(bail, {base, value});
            // What the old value's release did cannot touch a shape: releasing never runs code.
            if (site->addTo) {
                for (std::size_t i = shapeFacts_.size(); i-- > 0;) {
                    if (shapeFacts_[i].shape == site->shape) {
                        shapeFacts_[i] = shapeFacts_.back();
                        shapeFacts_.pop_back();
                    }
                }
                learnShape(base.r, site->addTo);
            }
        } else if (site && site->moreCount) {
            for (const StoreCase& c : dispatchStores(base, site, slow)) {
                inst(IrOp::Label).imm = c.label;
                if (c.addTo) {
                    appendInPlace(base, c.addTo, vt, vr, slow);
                    inst(IrOp::Br).imm = done;
                    continue;
                }
                const std::int64_t at = std::int64_t(c.index) * std::int64_t(sizeof(Prop));
                const std::int32_t props = loadHeader(base.r, kPropsOffset);
                const std::int32_t oldTag = load(props, at + kPropValueOffset, 1, false, kAliasValue);
                const std::int32_t old = load(props, at + kPropValueOffset + kPayload, 8, false, kAliasValue);
                store(props, at + kPropValueOffset, vt, 1);
                store(props, at + kPropValueOffset + kPayload, vr, 8);
                decRefOf(old, oldTag, false);
                inst(IrOp::Br).imm = done;
            }
        } else if (site && site->addTo && site->epoch == rt_.ctx.protoEpoch && site->addTo->count <= DynObject::kIndexThreshold) {
            guardShape(base, site->shape, slow);
            guardEpoch(site->epoch, slow);
            appendInPlace(base, site->addTo, vt, vr, slow);
        } else if (site && !site->addTo) {
            // A write to a property the shape says is there and writable: what was there loses its count after.
            guardShape(base, site->shape, slow);
            const std::int64_t at = std::int64_t(site->index) * std::int64_t(sizeof(Prop));
            const std::int32_t props = loadHeader(base.r, kPropsOffset);
            const std::int32_t oldTag = load(props, at + kPropValueOffset, 1, false, kAliasValue);
            const std::int32_t old = load(props, at + kPropValueOffset + kPayload, 8, false, kAliasValue);
            store(props, at + kPropValueOffset, vt, 1);
            store(props, at + kPropValueOffset + kPayload, vr, 8);
            decRefOf(old, oldTag, false);
        } else {
            inst(IrOp::Br).imm = slow;
        }
        if (!bet) {
            inst(IrOp::ColdBegin);
            inst(IrOp::Label).imm = slow;
            {
                const std::int32_t rt = rtReg();
                const std::int32_t self = ptrReg(&code_);
                const std::int32_t op = immReg(std::int64_t(operand));
                coldCall(reinterpret_cast<void*>(&scriptDynSetProp), {rt, self, tagReg(base), base.r, op, vt, vr}, -1, -1,
                         true, {base, value});
                // The helper took its own count, so the one made above goes.
                if (value.own == Own::Owned && !value.isConst && !(value.hasKnownTag && value.known < Type::String))
                    decRefOf(vr, value.hasKnownTag ? -1 : vt, false);
            }
            inst(IrOp::Br).imm = done;
            inst(IrOp::ColdEnd);
            inst(IrOp::Label).imm = done;
        }
        release(base);
        // The value of the assignment expression, read back without a count of its own: the field holds one.
        if (value.rep != Rep::Tagged) {
            push(value);
            return;
        }
        Entry result = value;
        if (result.own == Own::Owned)
            result.own = Own::Weak;
        result.local = -1;
        push(result);
    }

    /// An index in a register, if it is a whole number that can be one; everything else goes to `slow`.
    std::int32_t arrayIndex(const Entry& key, std::int32_t slow)
    {
        if (key.rep == Rep::Int32 && !key.isConst) {
            const std::int32_t wide = fn_.newVreg(false);
            IrInst& ext = inst(IrOp::Zext32);
            ext.d = wide;
            ext.a = key.r;
            // Negative wraps far past any length, which the bounds check then catches.
            IrInst& sign = inst(IrOp::BrCmp);
            sign.a = key.r;
            sign.cond = IrCond::Lt;
            sign.hasImm = true;
            sign.imm = 0;
            sign.width = 32;
            sign.target = slow;
            return wide;
        }
        std::int32_t fp;
        if (isKnownNumber(key)) {
            fp = doubleReg(key);
        } else {
            // A key whose tag is known and not a number has nothing to test: it goes the long way.
            if (key.hasKnownTag || key.isConst || key.rep != Rep::Tagged)
                inst(IrOp::Br).imm = slow;
            else
                branchCmp(key.t, IrCond::Ne, std::int64_t(Type::Double), slow);
            const std::int32_t keyBits = payloadReg(key);
            fp = fn_.newVreg(true);
            IrInst& bits = inst(IrOp::BitsToFp);
            bits.d = fp;
            bits.a = keyBits;
            bits.width = 64;
        }
        const std::int32_t i = fn_.newVreg(false);
        IrInst& conv = inst(IrOp::FpToIntRaw);
        conv.d = i;
        conv.a = fp;
        const std::int32_t back = fn_.newVreg(true);
        IrInst& to = inst(IrOp::IntToFp);
        to.d = back;
        to.a = i;
        to.srcWidth = 64;
        to.width = 64;
        const std::int32_t same = newLabel();
        IrInst& eq = inst(IrOp::BrFCmp);
        eq.a = fp;
        eq.b = back;
        eq.cond = IrCond::FEq;
        eq.width = 64;
        eq.target = same;
        inst(IrOp::Br).imm = slow;
        inst(IrOp::Label).imm = same;
        return i;
    }

    void getElem(std::uint32_t elemOperand)
    {
        Entry key = pop();
        Entry base = pop();
        // Where the interpreter takes over rather than the code carrying on from a getter, nothing can have changed.
        if (!betHere())
            secureWeak();
        if (base.rep != Rep::Tagged || base.isConst) {
            const std::int32_t tag = tagReg(base);
            const std::int32_t payload = payloadReg(base);
            base = taggedEntry(payload, tag, base.own);
        }
        const std::int32_t r = fn_.newVreg(false);
        const std::int32_t t = fn_.newVreg(false);
        const std::int32_t slow = newLabel();
        const std::int32_t done = newLabel();
        const bool bet = betHere();
        const std::uint8_t saw = code_.elemSaw[elemOperand];
        guardArray(base, slow, bet);
        const std::int32_t i = arrayIndex(key, slow);
        const std::int32_t data = elementSpan(base, i, slow);
        if (bet && saw == ElemSite::kNumbers) {
            // Only ever an element of an array of numbers: what it is, known here.
            const std::int32_t numbersOnly = load(base.r, kNumbersOnlyOffset, 1, false, kAliasHeader);
            branchCmp(numbersOnly, IrCond::Eq, 0, slow);
            const std::int32_t bits = loadBareElement(data, i, slow);
            const std::int32_t fp = fn_.newVreg(true);
            IrInst& conv = inst(IrOp::BitsToFp);
            conv.d = fp;
            conv.a = bits;
            conv.width = 64;
            emitBail(slow, {base, key});
            release(key);
            release(base);
            push(doubleEntry(fp));
            return;
        }
        const std::int32_t values = newLabel();
        const std::int32_t joined = newLabel();
        const bool onlyValues = bet && saw == ElemSite::kValues;
        const std::int32_t numbersOnly = load(base.r, kNumbersOnlyOffset, 1, false, kAliasHeader);
        if (onlyValues) {
            branchCmp(numbersOnly, IrCond::Ne, 0, slow);
        } else {
            branchCmp(numbersOnly, IrCond::Eq, 0, values);
            const std::int32_t bits = loadBareElement(data, i, slow);
            IrInst& mr = inst(IrOp::Mov);
            mr.d = r;
            mr.a = bits;
            IrInst& mt = inst(IrOp::MovImm);
            mt.d = t;
            mt.imm = std::int64_t(Type::Double);
            inst(IrOp::Br).imm = joined;
            inst(IrOp::Label).imm = values;
        }
        const std::int32_t scaled = shiftLeft(i, 4);
        IrInst& lt = inst(IrOp::LoadIdx);
        lt.d = t;
        lt.a = data;
        lt.b = scaled;
        lt.width = 1;
        IrInst& lr = inst(IrOp::LoadIdx);
        lr.d = r;
        lr.a = data;
        lr.b = scaled;
        lr.imm = kPayload;
        lr.width = 8;
        if (!onlyValues)
            inst(IrOp::Label).imm = joined;
        if (bet) {
            // An index into an array it has; anything else, the interpreter does.
            emitBail(slow, {base, key});
        } else {
            inst(IrOp::ColdBegin);
            inst(IrOp::Label).imm = slow;
            const std::int32_t rt = rtReg();
            coldCall(reinterpret_cast<void*>(&scriptDynGetElem), {rt, tagReg(base), base.r, tagReg(key), payloadReg(key)}, r,
                     t, true, {base, key});
            inst(IrOp::Br).imm = done;
            inst(IrOp::ColdEnd);
            inst(IrOp::Label).imm = done;
        }
        Entry result = taggedEntry(r, t, Own::Weak);
        if (base.own == Own::Owned || key.own == Own::Owned) {
            makeOwned(result);
            push(result);
            release(key);
            release(base);
        } else {
            push(result);
        }
    }

    std::int32_t shiftLeft(std::int32_t a, std::int64_t by)
    {
        const std::int32_t d = fn_.newVreg(false);
        IrInst& shl = inst(IrOp::Shl);
        shl.d = d;
        shl.a = a;
        shl.hasImm = true;
        shl.imm = by;
        shl.width = 64;
        return d;
    }

    /// Where an array's elements are, once `i` is known to be one of them; otherwise to `slow`.
    std::int32_t elementSpan(const Entry& base, std::int32_t i, std::int32_t slow)
    {
        const std::int32_t data = loadHeader(base.r, kElementsOffset);
        const std::int32_t count = load(base.r, kElementCountOffset, 4, false, kAliasHeader);
        IrInst& bound = inst(IrOp::BrCmp);
        bound.a = i;
        bound.b = count;
        bound.cond = IrCond::Hs;
        bound.width = 64;
        bound.target = slow;
        return data;
    }

    /// The bits of element `i` of an array of numbers; `undefined` goes to `slow`.
    std::int32_t loadBareElement(std::int32_t data, std::int32_t i, std::int32_t slow)
    {
        const std::int32_t bits = fn_.newVreg(false);
        const std::int32_t scaled = shiftLeft(i, 3);
        IrInst& load8 = inst(IrOp::LoadIdx);
        load8.d = bits;
        load8.a = data;
        load8.b = scaled;
        load8.width = 8;
        const std::int32_t undefinedBits = immReg(std::int64_t(kUndefinedElement));
        IrInst& undefinedHere = inst(IrOp::BrCmp);
        undefinedHere.a = bits;
        undefinedHere.b = undefinedBits;
        undefinedHere.cond = IrCond::Eq;
        undefinedHere.width = 64;
        undefinedHere.target = slow;
        return bits;
    }

    /// `leaves` says that `slow` does not come back. What an object is does not change, so one check then holds until the
    /// register holds something else; a way round the check that rejoins here says nothing of the kind.
    void guardArray(const Entry& base, std::int32_t slow, bool leaves = false)
    {
        if (!base.isConst && std::find(knownArrays_.begin(), knownArrays_.end(), base.r) != knownArrays_.end())
            return;
        const auto learn = [&] {
            if (leaves && !base.isConst && base.local >= 0)
                knownArrays_.push_back(base.r);
        };
        if (!base.hasKnownTag)
            branchCmp(base.t, IrCond::Ne, std::int64_t(Type::Object), slow);
        else if (base.known != Type::Object)
            inst(IrOp::Br).imm = slow;
        const std::int32_t both = load(base.r, kCellKindOffset, 2, false, kAliasHeader);
        branchCmp(both, IrCond::Ne, kKindAndClassOfArray, slow);
        learn();
    }

    void setElem(std::uint32_t elemOperand)
    {
        Entry value = pop();
        Entry key = pop();
        Entry base = pop();
        secureWeak();
        makeOwned(value);
        if (base.rep != Rep::Tagged || base.isConst) {
            const std::int32_t tag = tagReg(base);
            const std::int32_t payload = payloadReg(base);
            base = taggedEntry(payload, tag, base.own);
        }
        const std::int32_t vt = tagReg(value);
        const std::int32_t vr = payloadReg(value);
        const std::int32_t slow = newLabel();
        const std::int32_t done = newLabel();
        guardArray(base, slow, betHere());
        // Frozen, or with named properties that might include an index: the long way.
        const std::int32_t frozen = load(base.r, kFrozenOffset, 1, false, kAliasHeader);
        branchCmp(frozen, IrCond::Ne, 0, slow);
        const std::int32_t named = load(base.r, kPropsSizeOffset, 4, false, kAliasHeader);
        branchCmp(named, IrCond::Ne, 0, slow);
        const std::int32_t i = arrayIndex(key, slow);
        const std::int32_t data = elementSpan(base, i, slow);
        const std::uint8_t saw = code_.elemSaw[elemOperand];
        const bool bet = betHere();
        const std::int32_t numbersOnly = load(base.r, kNumbersOnlyOffset, 1, false, kAliasHeader);
        const std::int32_t stored = newLabel();
        // Into an array of numbers, while what comes is one; the rest of what can be there takes a call to sort out.
        const auto storeBare = [&] {
            if (!isKnownNumber(value)) {
                if (value.hasKnownTag || value.isConst || value.rep != Rep::Tagged)
                    inst(IrOp::Br).imm = slow;
                else
                    branchCmp(vt, IrCond::Ne, std::int64_t(Type::Double), slow);
            }
            const std::int32_t slot = fn_.newVreg(false);
            const std::int32_t scaled = shiftLeft(i, 3);
            IrInst& add = inst(IrOp::Add);
            add.d = slot;
            add.a = data;
            add.b = scaled;
            add.width = 64;
            store(slot, 0, vr, 8);
        };
        const auto storeValue = [&] {
            const std::int32_t slot = fn_.newVreg(false);
            const std::int32_t scaled = shiftLeft(i, 4);
            IrInst& add = inst(IrOp::Add);
            add.d = slot;
            add.a = data;
            add.b = scaled;
            add.width = 64;
            const std::int32_t oldTag = load(slot, 0, 1);
            const std::int32_t old = load(slot, kPayload, 8);
            store(slot, 0, vt, 1);
            store(slot, kPayload, vr, 8);
            decRefOf(old, oldTag, false);
        };
        if (bet && saw == ElemSite::kNumbers) {
            branchCmp(numbersOnly, IrCond::Eq, 0, slow);
            storeBare();
        } else if (bet && saw == ElemSite::kValues) {
            branchCmp(numbersOnly, IrCond::Ne, 0, slow);
            storeValue();
        } else {
            const std::int32_t values = newLabel();
            branchCmp(numbersOnly, IrCond::Eq, 0, values);
            storeBare();
            inst(IrOp::Br).imm = stored;
            inst(IrOp::Label).imm = values;
            storeValue();
            inst(IrOp::Label).imm = stored;
        }
        if (betHere()) {
            emitBail(slow, {base, key, value});
        } else {
            inst(IrOp::ColdBegin);
            inst(IrOp::Label).imm = slow;
            {
                const std::int32_t rt = rtReg();
                coldCall(reinterpret_cast<void*>(&scriptDynSetElem),
                         {rt, tagReg(base), base.r, tagReg(key), payloadReg(key), vt, vr}, -1, -1, true, {base, key, value});
                if (value.own == Own::Owned && !value.isConst && !(value.hasKnownTag && value.known < Type::String))
                    decRefOf(vr, value.hasKnownTag ? -1 : vt, false);
            }
            inst(IrOp::Br).imm = done;
            inst(IrOp::ColdEnd);
            inst(IrOp::Label).imm = done;
        }
        release(key);
        release(base);
        if (value.rep != Rep::Tagged) {
            push(value);
            return;
        }
        Entry result = value;
        if (result.own == Own::Owned)
            result.own = Own::Weak;
        result.local = -1;
        push(result);
    }

    void closure(std::uint32_t index)
    {
        secureWeak();
        const auto& child = code_.children[index];
        const std::uint32_t bytes = std::uint32_t(child->upvals.size()) * 8;
        fn_.frameAreaBytes = std::max(fn_.frameAreaBytes, bytes);
        const std::int32_t area = fn_.newVreg(false);
        IrInst& addr = inst(IrOp::FrameAddr);
        addr.d = area;
        addr.imm = 0;
        for (std::size_t i = 0; i < child->upvals.size(); ++i) {
            const UpvalDesc& desc = child->upvals[i];
            std::int32_t box;
            if (desc.fromParentLocal) {
                box = cellReg_[desc.index];
            } else {
                const std::int32_t data = load(fnReg_, kFnUpvaluesOffset, 8, false, kAliasHeader);
                box = load(data, std::int64_t(desc.index) * 8, 8);
            }
            store(area, std::int64_t(i) * 8, box, 8);
        }
        const std::int32_t rt = rtReg();
        const std::int32_t codeRef = ptrReg(&child);
        const std::int32_t r = fn_.newVreg(false);
        const std::int32_t t = fn_.newVreg(false);
        coldCall(reinterpret_cast<void*>(&scriptDynClosure), {rt, codeRef, fnReg_, area}, r, t, true);
        Entry e = taggedEntry(r, t, Own::Owned);
        push(e);
    }

    /// Lay the arguments out as `Value`s in this frame's memory, for a callee that borrows them.
    std::int32_t layOutArgs(std::vector<Entry>& args)
    {
        const std::uint32_t bytes = std::uint32_t(args.size()) * 16 + 16;
        fn_.frameAreaBytes = std::max(fn_.frameAreaBytes, bytes);
        const std::int32_t area = fn_.newVreg(false);
        IrInst& addr = inst(IrOp::FrameAddr);
        addr.d = area;
        addr.imm = 0;
        for (std::size_t i = 0; i < args.size(); ++i) {
            const Entry& arg = args[i];
            if (arg.rep != Rep::Tagged || (arg.hasKnownTag && !arg.isConst) || arg.isConst) {
                const Type known = arg.rep != Rep::Tagged ? Type::Double : arg.known;
                IrInst& tag = inst(IrOp::StoreTag);
                tag.a = area;
                tag.tag = known;
                tag.imm = std::int64_t(i) * 16;
            } else {
                store(area, std::int64_t(i) * 16, arg.t, 1);
            }
            if (arg.rep == Rep::Double && !arg.isConst)
                store(area, std::int64_t(i) * 16 + kPayload, arg.r, 8);
            else
                store(area, std::int64_t(i) * 16 + kPayload, payloadReg(arg), 8);
        }
        return area;
    }

    void securePopped(std::vector<Entry>& entries)
    {
        for (Entry& e : entries) {
            if (e.own == Own::Weak)
                makeOwned(e);
        }
    }

    void call(std::uint32_t argc)
    {
        std::vector<Entry> args(argc);
        for (std::uint32_t i = argc; i-- > 0;)
            args[i] = pop();
        Entry self = pop();
        Entry callee = pop();
        if (mathInPlace(callee, self, args))
            return;
        // Set when the stack below is not the one the interpreter would see at this instruction.
        bool viaCall = false;
        // `f.call(x, a)` is `f(a)` on `x`, with everything a call to `f` could have, while `call` is the built-in.
        if (callee.hint && callee.hint == rt_.intrinsics[Runtime::FunctionCall].get() && betHere() && argc >= 1 &&
            callee.rep == Rep::Tagged && !callee.isConst && !args[0].lazyArgs) {
            viaCall = true;
            const std::int32_t bail = newLabel();
            if (!callee.hasKnownTag)
                branchCmp(callee.t, IrCond::Ne, std::int64_t(Type::Object), bail);
            const std::int32_t want = ptrReg(callee.hint);
            IrInst& same = inst(IrOp::BrCmp);
            same.a = callee.r;
            same.b = want;
            same.cond = IrCond::Ne;
            same.width = 64;
            same.target = bail;
            std::vector<Entry> popped = {callee, self};
            popped.insert(popped.end(), args.begin(), args.end());
            emitBail(bail, popped);
            // The runtime holds on to the built-in itself.
            release(callee);
            callee = self;
            self = args[0];
            args.erase(args.begin());
            --argc;
        }
        const DynFunction* known = expectCallee(callee);
        // A small function that goes in place of the call and only reads can release nothing, so nothing here needs a
        // count to stay alive through it.
        const bool holdsNothing = known && depth_ < 3 && inlinable(*known->code) && onlyReads(*known->code);
        if (!holdsNothing) {
            secureWeak();
            securePopped(args);
            if (self.own == Own::Weak)
                makeOwned(self);
        }
        if (callee.own == Own::Weak)
            makeOwned(callee);

        std::vector<Entry> held = args;
        held.push_back(self);
        held.push_back(callee);

        const std::int32_t r = fn_.newVreg(false);
        const std::int32_t t = fn_.newVreg(false);
        const std::int32_t generic = newLabel();
        const std::int32_t finished = newLabel();
        const bool lazy = !args.empty() && args.back().lazyArgs;
        bool inlined = lazy ? forwardArguments(callee, self, args, r, t, generic, held) : intrinsic(callee, self, args, r, t, generic);
        const std::size_t bodyStart = fn_.code.size();
        if (!inlined && !lazy) {
            inlinedReturnTag_ = -1;
            inlined = inlineCall(callee, self, args, held, r, t, generic, finished);
            // A function's body in place of the call, and something else turning up instead is for the interpreter to deal
            // with: no call to come back from here, so what the body leaves known stays known.
            if (inlined && betHere() && !viaCall) {
                inst(IrOp::Br).imm = finished;
                bool guarded = false;
                for (std::size_t i = bodyStart; i < fn_.code.size() && !guarded; ++i) {
                    const IrInst& in = fn_.code[i];
                    guarded = (in.op == IrOp::Br && in.imm == generic) || ((in.op == IrOp::BrCmp || in.op == IrOp::BrFCmp) && in.target == generic);
                }
                if (guarded) {
                    std::vector<Entry> popped = {callee, self};
                    popped.insert(popped.end(), args.begin(), args.end());
                    emitBail(generic, popped);
                }
                inst(IrOp::Label).imm = finished;
                for (const Entry& arg : args)
                    release(arg);
                release(self);
                release(callee);
                // What every return of the body knew of the value's tag, the caller knows.
                Entry result = taggedEntry(r, t, Own::Owned);
                if (inlinedReturnTag_ >= 0) {
                    result.hasKnownTag = true;
                    result.known = Type(inlinedReturnTag_);
                    if (result.known < Type::String)
                        result.own = Own::None;
                }
                push(result);
                return;
            }
        }
        if (inlined) {
            inst(IrOp::Br).imm = finished;
            inst(IrOp::Label).imm = generic;
        }
        // Anything but the call it was kept for wants the array after all.
        Entry made;
        if (lazy) {
            const std::int32_t array = callCold(reinterpret_cast<void*>(&scriptDynArgsArray), {rtReg(), argsReg_, argcReg_, immReg(0)}, true);
            made = objectEntry(array);
            args.back() = made;
            held.push_back(made);
        }
        const std::int32_t area = layOutArgs(args);
        const std::int32_t selfTag = tagReg(self);
        const std::int32_t selfPayload = payloadReg(self);
        const std::int32_t fnTag = tagReg(callee);
        const std::int32_t fnPayload = payloadReg(callee);
        const std::int32_t count = immReg(std::int64_t(argc));
        invoke(fnTag, fnPayload, selfTag, selfPayload, area, count, r, t, held, known);
        if (lazy) {
            release(made);
            args.back() = constTagged(Type::Undefined);
        }
        if (inlined)
            inst(IrOp::Label).imm = finished;
        for (const Entry& arg : args)
            release(arg);
        release(self);
        release(callee);
        push(taggedEntry(r, t, Own::Owned));
    }

    /// `Math.sqrt(x)` and the like of a number, where it has been the built-in: an instruction, with nothing kept alive for it
    /// and nothing left in the loop that could be taken for a call. Anything else, the interpreter does.
    bool mathInPlace(const Entry& callee, const Entry& self, const std::vector<Entry>& args)
    {
        if (!callee.hint || callee.rep != Rep::Tagged || callee.isConst || args.empty() || args.size() > 2 || !betHere() ||
            args.back().lazyArgs)
            return false;
        int which = -1;
        for (int i = Runtime::MathSqrt; i <= Runtime::MathAtan2; ++i) {
            if (rt_.intrinsics[i].get() == callee.hint)
                which = i;
        }
        if (which < 0 || args.size() != (which >= Runtime::MathMax ? 2u : 1u))
            return false;
        const Entry& arg = args[0];
        const std::int32_t bail = newLabel();
        if (!callee.hasKnownTag)
            branchCmp(callee.t, IrCond::Ne, std::int64_t(Type::Object), bail);
        const std::int32_t want = ptrReg(callee.hint);
        {
            IrInst& same = inst(IrOp::BrCmp);
            same.a = callee.r;
            same.b = want;
            same.cond = IrCond::Ne;
            same.width = 64;
            same.target = bail;
        }
        for (const Entry& each : args) {
            if (isKnownNumber(each))
                continue;
            if (each.hasKnownTag || each.isConst)
                inst(IrOp::Br).imm = bail;
            else
                branchCmp(each.t, IrCond::Ne, std::int64_t(Type::Double), bail);
        }
        const std::int32_t x = doubleReg(arg);
        const std::int32_t y = args.size() == 2 ? doubleReg(args[1]) : -1;
        const std::int32_t d = fn_.newVreg(true);
        if (which <= Runtime::MathTrunc) {
            static constexpr IrOp kOps[] = {IrOp::FSqrt, IrOp::FAbs, IrOp::FFloor, IrOp::FCeil, IrOp::FTrunc};
            IrInst& op = inst(kOps[which - Runtime::MathSqrt]);
            op.d = d;
            op.a = x;
            op.width = 64;
        } else if (which == Runtime::MathMax || which == Runtime::MathMin) {
            // Whichever compares greater or less; a tie, which may be of zeros of either sign, or a NaN, the long way.
            const IrCond better = which == Runtime::MathMax ? IrCond::FGt : IrCond::FLt;
            const std::int32_t done = newLabel();
            const std::int32_t other = newLabel();
            const auto take = [&](std::int32_t from, std::int32_t against) {
                IrInst& mov = inst(IrOp::Mov);
                mov.d = d;
                mov.a = from;
                IrInst& test = inst(IrOp::BrFCmp);
                test.a = from;
                test.b = against;
                test.cond = better;
                test.width = 64;
                test.target = done;
            };
            take(x, y);
            take(y, x);
            inst(IrOp::Br).imm = other;
            inst(IrOp::ColdBegin);
            inst(IrOp::Label).imm = other;
            coldCall(reinterpret_cast<void*>(which == Runtime::MathMax ? &scriptDynMax : &scriptDynMin), {x, y}, d, -1, false);
            inst(IrOp::Br).imm = done;
            inst(IrOp::ColdEnd);
            inst(IrOp::Label).imm = done;
        } else {
            void* helper = nullptr;
            switch (which) {
                case Runtime::MathSin: helper = reinterpret_cast<void*>(&scriptDynSin); break;
                case Runtime::MathCos: helper = reinterpret_cast<void*>(&scriptDynCos); break;
                case Runtime::MathTan: helper = reinterpret_cast<void*>(&scriptDynTan); break;
                case Runtime::MathAtan: helper = reinterpret_cast<void*>(&scriptDynAtan); break;
                case Runtime::MathExp: helper = reinterpret_cast<void*>(&scriptDynExp); break;
                case Runtime::MathLog: helper = reinterpret_cast<void*>(&scriptDynLog); break;
                case Runtime::MathRound: helper = reinterpret_cast<void*>(&scriptDynRound); break;
                case Runtime::ParseInt: helper = reinterpret_cast<void*>(&scriptDynParseInt); break;
                case Runtime::MathPow: helper = reinterpret_cast<void*>(&scriptDynPow); break;
                default: helper = reinterpret_cast<void*>(&scriptDynAtan2); break;
            }
            if (y >= 0)
                coldCall(helper, {x, y}, d, -1, false);
            else
                coldCall(helper, {x}, d, -1, false);
        }
        std::vector<Entry> popped = {callee, self};
        popped.insert(popped.end(), args.begin(), args.end());
        emitBail(bail, popped);
        for (std::size_t i = args.size(); i-- > 0;)
            release(args[i]);
        release(self);
        release(callee);
        push(doubleEntry(d));
        return true;
    }

    /// `f.apply(x, arguments)`: `f` called on this call's own arguments as they are, with no array between. True with the
    /// call emitted, its way out for anything else being `generic`.
    bool forwardArguments(const Entry& callee, const Entry& self, const std::vector<Entry>& args, std::int32_t r, std::int32_t t,
                          std::int32_t generic, const std::vector<Entry>& held)
    {
        GCObject* apply = rt_.intrinsics[Runtime::FunctionApply].get();
        if (callee.hint != apply || callee.rep != Rep::Tagged || callee.isConst || args.size() != 2)
            return false;
        if (!callee.hasKnownTag)
            branchCmp(callee.t, IrCond::Ne, std::int64_t(Type::Object), generic);
        const std::int32_t want = ptrReg(apply);
        IrInst& same = inst(IrOp::BrCmp);
        same.a = callee.r;
        same.b = want;
        same.cond = IrCond::Ne;
        same.width = 64;
        same.target = generic;
        invoke(tagReg(self), payloadReg(self), tagReg(args[0]), payloadReg(args[0]), argsReg_, argcReg_, r, t, held);
        return true;
    }

    /// The function a pinned callee is.
    static const DynFunction* expectCallee(const Entry& callee)
    {
        return callee.pinned ? static_cast<const DynFunction*>(callee.hint) : nullptr;
    }
    /// A new object of pinned prototype `proto`, starting from `root`, in `result`: popped off its size's free list and
    /// written here while the heap has one ready and nothing is due; otherwise `helper` makes it, which it also does when
    /// something about this context or build needs it to.
    std::int32_t plusImm(std::int32_t a, std::int64_t imm, std::uint8_t width)
    {
        const std::int32_t d = fn_.newVreg(false);
        IrInst& add = inst(IrOp::Add);
        add.d = d;
        add.a = a;
        add.hasImm = true;
        add.imm = imm;
        add.width = width;
        return d;
    }

    /// A cell of `bytes` from the heap, as `newCell` would get it and account for it, while no collection is due and the
    /// allocator's current block has room; otherwise to `slow`. The caller writes the cell.
    std::int32_t carveCell(std::size_t bytes, std::int32_t slow)
    {
        ContextImpl& ctx = rt_.ctx;
        const auto word = [&](std::int32_t base, std::int64_t offset) { return load(base, offset, 8, false, kAliasHeader); };
        const auto plus = [&](std::int32_t a, std::int64_t imm, std::uint8_t width) {
            const std::int32_t d = fn_.newVreg(false);
            IrInst& add = inst(IrOp::Add);
            add.d = d;
            add.a = a;
            add.hasImm = true;
            add.imm = imm;
            add.width = width;
            return d;
        };
        const auto branchNotBelow = [&](std::int32_t a, std::int32_t b) {
            IrInst& test = inst(IrOp::BrCmp);
            test.a = a;
            test.b = b;
            test.cond = IrCond::Hs;
            test.width = 64;
            test.target = slow;
        };
        // What `maybeCollect` asks.
        const std::int32_t heap = ptrReg(&ctx);
        const std::int32_t heapBytes = word(heap, std::int64_t(offsetof(ContextImpl, heapBytes)));
        branchNotBelow(heapBytes, word(heap, std::int64_t(offsetof(ContextImpl, nextGcBytes))));
        const std::int32_t liveObjects = word(heap, std::int64_t(offsetof(ContextImpl, liveObjects)));
        const std::int32_t cells = fn_.newVreg(false);
        const std::int32_t liveStrings = word(heap, std::int64_t(offsetof(ContextImpl, liveStrings)));
        {
            IrInst& add = inst(IrOp::Add);
            add.d = cells;
            add.a = liveObjects;
            add.b = liveStrings;
            add.width = 64;
        }
        branchNotBelow(cells, word(heap, std::int64_t(offsetof(ContextImpl, nextGcCells))));
        // What `LocalHeap::allocate` does while the current block has a free slot, or one not yet carved.
        const std::int32_t block = word(ptrReg(ctx.heap.currentBlockOf(bytes)), 0);
        const std::int32_t slot = fn_.newVreg(false);
        {
            const std::int32_t carve = newLabel();
            const std::int32_t got = newLabel();
            const std::int32_t free = word(block, std::int64_t(mem::LocalHeap::kBlockFreeOffset));
            branchCmp(free, IrCond::Eq, 0, carve, 64);
            store(block, std::int64_t(mem::LocalHeap::kBlockFreeOffset), word(free, 0), 8, kAliasHeader);
            IrInst& fromFree = inst(IrOp::Mov);
            fromFree.d = slot;
            fromFree.a = free;
            inst(IrOp::Br).imm = got;
            inst(IrOp::Label).imm = carve;
            const std::int32_t bump = word(block, std::int64_t(mem::LocalHeap::kBlockBumpOffset));
            branchNotBelow(bump, word(block, std::int64_t(mem::LocalHeap::kBlockEndOffset)));
            const std::int32_t step = load(block, std::int64_t(mem::LocalHeap::kBlockSlotBytesOffset), 4, false, kAliasHeader);
            const std::int32_t after = fn_.newVreg(false);
            IrInst& add = inst(IrOp::Add);
            add.d = after;
            add.a = bump;
            add.b = step;
            add.width = 64;
            store(block, std::int64_t(mem::LocalHeap::kBlockBumpOffset), after, 8, kAliasHeader);
            IrInst& fromBump = inst(IrOp::Mov);
            fromBump.d = slot;
            fromBump.a = bump;
            inst(IrOp::Label).imm = got;
        }
        const std::int32_t live = load(block, std::int64_t(mem::LocalHeap::kBlockLiveOffset), 4, false, kAliasHeader);
        store(block, std::int64_t(mem::LocalHeap::kBlockLiveOffset), plus(live, 1, 32), 4, kAliasHeader);
        store(heap, std::int64_t(offsetof(ContextImpl, heapBytes)), plus(heapBytes, std::int64_t(bytes), 64), 8, kAliasHeader);
        const std::int32_t total = word(heap, std::int64_t(offsetof(ContextImpl, totalAllocated)));
        store(heap, std::int64_t(offsetof(ContextImpl, totalAllocated)), plus(total, std::int64_t(bytes), 64), 8, kAliasHeader);
        store(heap, std::int64_t(offsetof(ContextImpl, liveObjects)), plus(liveObjects, 1, 64), 8, kAliasHeader);
        return slot;
    }

    /// A box holding the value `tag`/`payload`, which it counts: made here for undefined, null, a number or a reference,
    /// whose bits need no tidying, and by `scriptDynNewBox` for anything else, or when no slot is ready.
    std::int32_t newBox(std::int32_t tag, std::int32_t payload)
    {
        ContextImpl& ctx = rt_.ctx;
        const std::int32_t result = fn_.newVreg(false);
        if (!mem::LocalHeap::kCanPopInline || ctx.options.memoryLimit) {
            coldCall(reinterpret_cast<void*>(&scriptDynNewBox), {rtReg(), tag, payload}, result, -1, true);
            return result;
        }
        constexpr std::size_t kWords = sizeof(Box) / 8;
        static_assert(sizeof(Box) % 8 == 0, "a box is written a word at a time");
        const std::int32_t slow = newLabel();
        const std::int32_t done = newLabel();
        const std::int32_t plain = newLabel();
        const std::int32_t ready = newLabel();
        const std::int32_t counted = newLabel();
        // The bits the box holds: a number's or a reference's own, nothing for undefined and null.
        const std::int32_t bits = fn_.newVreg(false);
        {
            IrInst& mov = inst(IrOp::Mov);
            mov.d = bits;
            mov.a = payload;
        }
        branchCmp(tag, IrCond::Eq, std::int64_t(Type::Double), ready);
        branchCmp(tag, IrCond::Ls, std::int64_t(Type::Null), plain);
        branchCmp(tag, IrCond::Lo, std::int64_t(Type::String), slow);
        incRefOf(payload, -1, false);
        inst(IrOp::Br).imm = ready;
        inst(IrOp::Label).imm = plain;
        {
            IrInst& zero = inst(IrOp::MovImm);
            zero.d = bits;
            zero.imm = 0;
        }
        inst(IrOp::Label).imm = ready;
        {
            // The count taken above is the box's; should no slot be ready, the helper takes its own, and this one goes.
            const std::int32_t slot = carveCell(sizeof(Box), counted);
            std::uint64_t image[kWords];
            {
                Box made;
                made.cellBytes = std::uint32_t(sizeof(Box));
                made.gcBytes = std::uint32_t(sizeof(Box));
                made.rt = &ctx;
                std::memcpy(image, static_cast<const void*>(&made), sizeof(Box));
            }
            for (std::size_t i = 0; i < kWords; ++i)
                store(slot, std::int64_t(i * 8), immReg(std::int64_t(image[i])), 8, kAliasHeader);
            store(slot, kBoxValueOffset, tag, 1);
            store(slot, kBoxValueOffset + kPayload, bits, 8);
            IrInst& mov = inst(IrOp::Mov);
            mov.d = result;
            mov.a = slot;
        }
        inst(IrOp::Br).imm = done;
        inst(IrOp::ColdBegin);
        inst(IrOp::Label).imm = counted;
        {
            // Back to what the helper expects: the value with no count from here.
            const std::int32_t uncounted = newLabel();
            branchCmp(tag, IrCond::Lo, std::int64_t(Type::String), uncounted);
            decRefOf(payload, -1, false);
            inst(IrOp::Label).imm = uncounted;
        }
        inst(IrOp::Label).imm = slow;
        coldCall(reinterpret_cast<void*>(&scriptDynNewBox), {rtReg(), tag, payload}, result, -1, true);
        inst(IrOp::Br).imm = done;
        inst(IrOp::ColdEnd);
        inst(IrOp::Label).imm = done;
        forgetFacts();
        return result;
    }

    void allocateObject(DynObject* proto, Shape* root, std::uint32_t room, std::int32_t result, void* helper,
                        std::vector<std::int32_t> helperArgs, const std::vector<Entry>& held)
    {
        ContextImpl& ctx = rt_.ctx;
        constexpr std::size_t kWords = sizeof(DynObject) / 8;
        if (!mem::LocalHeap::kCanPopInline || ctx.options.memoryLimit || !root || !proto->pinned || room > 32) {
            coldCall(helper, std::move(helperArgs), result, -1, true, held);
            return;
        }
        const std::size_t bytes = sizeof(DynObject) + std::size_t(room) * sizeof(Prop);
        const std::int32_t slow = newLabel();
        const std::int32_t done = newLabel();
        const std::int32_t slot = carveCell(bytes, slow);
        // It is a prototype from now on, as `newObjectSized` would say: changing it has to tell what was learned about the
        // objects made here, this code among them, that it no longer holds.
        proto->isProto = true;
        // What the constructors and `linkKnown` do.
        std::uint64_t image[kWords];
        rt_.objectImage(proto, root, room, image);
        for (std::size_t i = 0; i < kWords; ++i)
            store(slot, std::int64_t(i * 8), immReg(std::int64_t(image[i])), 8, kAliasHeader);
        if (room)
            store(slot, kPropsOffset, plusImm(slot, std::int64_t(sizeof(DynObject)), 64), 8, kAliasHeader);
        {
            IrInst& mov = inst(IrOp::Mov);
            mov.d = result;
            mov.a = slot;
        }
        inst(IrOp::ColdBegin);
        inst(IrOp::Label).imm = slow;
        coldCall(helper, std::move(helperArgs), result, -1, true, held);
        inst(IrOp::Br).imm = done;
        inst(IrOp::ColdEnd);
        inst(IrOp::Label).imm = done;
        forgetFacts();
    }

    void keepAlive(GCObject* object)
    {
        std::vector<Value>& keep = outermost().jitKeep;
        for (const Value& v : keep) {
            if (detail::Access::ptr(v) == object)
                return;
        }
        keep.push_back(detail::Access::make(Type::Object, object));
    }

    /// Calls a function value: straight into its native code when it has some, through the runtime otherwise.
    void invoke(std::int32_t fnTag, std::int32_t fnPayload, std::int32_t selfTag, std::int32_t selfPayload, std::int32_t area,
                std::int32_t count, std::int32_t r, std::int32_t t, const std::vector<Entry>& held,
                const DynFunction* known = nullptr)
    {
        const std::int32_t slow = newLabel();
        const std::int32_t done = newLabel();
        // A function compiled to native code is called directly; anything else takes the long way. A function this code
        // keeps alive and has checked the identity of needs none of that asked.
        std::int32_t codePtr = -1;
        if (known) {
            codePtr = ptrReg(known->code.get());
        } else {
            branchCmp(fnTag, IrCond::Ne, std::int64_t(Type::Object), slow);
            const std::int32_t kind = load(fnPayload, kCellKindOffset, 1, false, kAliasHeader);
            branchCmp(kind, IrCond::Ne, std::int64_t(CellKind::Object), slow);
            const std::int32_t cls = load(fnPayload, kClsOffset, 1, false, kAliasHeader);
            branchCmp(cls, IrCond::Ne, std::int64_t(ObjClass::Function), slow);
            codePtr = load(fnPayload, kFnCodeOffset, 8, false, kAliasHeader);
            branchCmp(codePtr, IrCond::Eq, 0, slow, 64);
        }
        const std::int32_t entry = load(codePtr, kCallEntryOffset, 8, false, kAliasHeader);
        branchCmp(entry, IrCond::Eq, 0, slow, 64);
        {
            IrCall direct;
            direct.kind = IrCallKind::Indirect;
            direct.delegate = entry;
            direct.args = {fnPayload, selfTag, selfPayload, area, count};
            direct.result = r;
            direct.resultTag = t;
            direct.site = site();
            direct.publishFrame = false;
            direct.exit = makeExit(0, held);
            forgetFacts();
            fn_.calls.push_back(std::move(direct));
            inst(IrOp::Call).imm = std::int64_t(fn_.calls.size()) - 1;
        }
        inst(IrOp::ColdBegin);
        inst(IrOp::Label).imm = slow;
        {
            const std::int32_t rt = rtReg();
            IrCall viaHelper;
            viaHelper.kind = IrCallKind::Helper;
            viaHelper.helper = reinterpret_cast<void*>(&scriptDynCall);
            viaHelper.args = {rt, fnTag, fnPayload, selfTag, selfPayload, area, count};
            viaHelper.result = r;
            viaHelper.resultTag = t;
            viaHelper.site = site();
            viaHelper.publishFrame = true;
            viaHelper.exit = makeExit(0, held);
            fn_.calls.push_back(std::move(viaHelper));
            inst(IrOp::Call).imm = std::int64_t(fn_.calls.size()) - 1;
        }
        inst(IrOp::Br).imm = done;
        inst(IrOp::ColdEnd);
        inst(IrOp::Label).imm = done;
    }

    /// A built-in done in place -- `Math.sqrt(x)` and its kind, `push`, `pop`, `charCodeAt`, `String.fromCharCode` -- when
    /// the callee is the built-in it was when compiled and the operands are what the inline code handles. Anything else
    /// goes to `generic`.
    bool intrinsic(const Entry& callee, const Entry& self, const std::vector<Entry>& args, std::int32_t r, std::int32_t t,
                   std::int32_t generic)
    {
        if (!callee.hint || callee.rep != Rep::Tagged || callee.isConst)
            return false;
        int which = -1;
        for (int i = 0; i < Runtime::IntrinsicCount; ++i) {
            if (rt_.intrinsics[i].get() == callee.hint)
                which = i;
        }
        const std::size_t argc = args.size();
        const bool fits = (which >= Runtime::MathSqrt && which <= Runtime::MathTrunc && argc == 1) ||
                          (which == Runtime::ArrayPush && argc == 1) || (which == Runtime::ArrayPop && argc == 0) ||
                          (which == Runtime::StringCharCodeAt && argc == 1) ||
                          (which == Runtime::StringFromCharCode && argc == 1) || (which == Runtime::FunctionApply && argc == 2) ||
                          ((which == Runtime::MapGet || which == Runtime::MapHas || which == Runtime::SetHas || which == Runtime::SetAdd) && argc == 1) ||
                          (which == Runtime::MapSet && argc == 2);
        if (!fits)
            return false;
        if (!callee.hasKnownTag)
            branchCmp(callee.t, IrCond::Ne, std::int64_t(Type::Object), generic);
        const std::int32_t want = ptrReg(callee.hint);
        {
            IrInst& same = inst(IrOp::BrCmp);
            same.a = callee.r;
            same.b = want;
            same.cond = IrCond::Ne;
            same.width = 64;
            same.target = generic;
        }
        switch (which) {
            case Runtime::ArrayPush: inlinePush(callee, self, args[0], r, t, generic); return true;
            case Runtime::ArrayPop: inlinePop(self, r, t, generic); return true;
            case Runtime::StringCharCodeAt: inlineCharCodeAt(self, args[0], r, t, generic); return true;
            case Runtime::FunctionApply: inlineApply(callee, self, args, r, t, generic); return true;
            case Runtime::MapGet:
            case Runtime::MapSet:
            case Runtime::MapHas:
            case Runtime::SetAdd:
            case Runtime::SetHas: {
                // The table's own methods, with no call of the built-in in between, once it is known to be the table asked for.
                const bool isMap = which == Runtime::MapGet || which == Runtime::MapSet || which == Runtime::MapHas;
                if (self.rep != Rep::Tagged || self.isConst || (self.hasKnownTag && self.known != Type::Object))
                    inst(IrOp::Br).imm = generic;
                else if (!self.hasKnownTag)
                    branchCmp(self.t, IrCond::Ne, std::int64_t(Type::Object), generic);
                if (self.rep == Rep::Tagged && !self.isConst) {
                    const std::int32_t both = load(self.r, kCellKindOffset, 2, false, kAliasHeader);
                    branchCmp(both, IrCond::Ne, std::int64_t(CellKind::Object) | std::int64_t(isMap ? ObjClass::Map : ObjClass::Set) << 8, generic);
                }
                std::vector<Entry> popped = {callee, self};
                popped.insert(popped.end(), args.begin(), args.end());
                const std::int32_t rt = rtReg();
                const std::int32_t cell = payloadReg(self);
                const std::int32_t keyTag = tagReg(args[0]);
                const std::int32_t keyBits = payloadReg(args[0]);
                if (which == Runtime::MapSet) {
                    const std::int32_t valueTag = tagReg(args[1]);
                    const std::int32_t valueBits = payloadReg(args[1]);
                    coldCall(reinterpret_cast<void*>(&scriptDynMapSet), {rt, cell, keyTag, keyBits, valueTag, valueBits}, r, t, true, popped);
                } else if (which == Runtime::SetAdd) {
                    coldCall(reinterpret_cast<void*>(&scriptDynMapSet), {rt, cell, keyTag, keyBits, keyTag, keyBits}, r, t, true, popped);
                } else {
                    coldCall(which == Runtime::MapGet ? reinterpret_cast<void*>(&scriptDynMapGet) : reinterpret_cast<void*>(&scriptDynMapHas),
                             {rt, cell, keyTag, keyBits}, r, t, true, popped);
                }
                return true;
            }
            case Runtime::StringFromCharCode: {
                const std::int32_t rt = rtReg();
                coldCall(reinterpret_cast<void*>(&scriptDynFromCharCode), {rt, tagReg(args[0]), payloadReg(args[0])}, r, t, true,
                         {callee, self, args[0]});
                return true;
            }
            default: break;
        }
        static constexpr IrOp kOps[] = {IrOp::FSqrt, IrOp::FAbs, IrOp::FFloor, IrOp::FCeil, IrOp::FTrunc};
        const Entry& arg = args[0];
        if (!isKnownNumber(arg))
            branchCmp(tagReg(arg), IrCond::Ne, std::int64_t(Type::Double), generic);
        const std::int32_t x = doubleReg(arg);
        const std::int32_t d = fn_.newVreg(true);
        IrInst& op = inst(kOps[which - Runtime::MathSqrt]);
        op.d = d;
        op.a = x;
        op.width = 64;
        setDoubleResult(d, r, t);
        return true;
    }

    void setDoubleResult(std::int32_t d, std::int32_t r, std::int32_t t)
    {
        IrInst& bits = inst(IrOp::FpToBits);
        bits.d = r;
        bits.a = d;
        bits.width = 64;
        IrInst& tag = inst(IrOp::MovImm);
        tag.d = t;
        tag.imm = std::int64_t(Type::Double);
    }

    /// An array that can change length: extensible and not frozen.
    void guardGrowableArray(const Entry& self, std::int32_t generic)
    {
        guardArray(self, generic);
        const std::int32_t extensible = load(self.r, kExtensibleOffset, 1, false, kAliasHeader);
        branchCmp(extensible, IrCond::Eq, 0, generic);
        const std::int32_t frozen = load(self.r, kFrozenOffset, 1, false, kAliasHeader);
        branchCmp(frozen, IrCond::Ne, 0, generic);
    }

    std::int32_t subtract(std::int32_t a, std::int32_t b)
    {
        const std::int32_t d = fn_.newVreg(false);
        IrInst& sub = inst(IrOp::Sub);
        sub.d = d;
        sub.a = a;
        sub.b = b;
        sub.width = 64;
        return d;
    }

    /// A count of elements as the number it is to script.
    void lengthResult(std::int32_t count, std::int32_t r, std::int32_t t)
    {
        const std::int32_t d = fn_.newVreg(true);
        IrInst& conv = inst(IrOp::IntToFp);
        conv.d = d;
        conv.a = count;
        conv.srcWidth = 64;
        conv.width = 64;
        setDoubleResult(d, r, t);
    }

    /// `array.push(v)` while the buffer has room: the value's count goes to the array.
    void inlinePush(const Entry& callee, const Entry& self, const Entry& value, std::int32_t r, std::int32_t t, std::int32_t generic)
    {
        guardGrowableArray(self, generic);
        // An array, wanting to grow or to hold something other than numbers: no call of the built-in is needed for that.
        const std::int32_t grow = newLabel();
        const std::int32_t pushed = newLabel();
        // Ahead of the first way there, which wants them too.
        const std::int32_t vt = tagReg(value);
        const std::int32_t vr = payloadReg(value);
        const std::int32_t data = loadHeader(self.r, kElementsOffset);
        const std::int32_t count = load(self.r, kElementCountOffset, 4, false, kAliasHeader);
        const std::int32_t cap = load(self.r, kElementCapOffset, 4, false, kAliasHeader);
        {
            IrInst& full = inst(IrOp::BrCmp);
            full.a = count;
            full.b = cap;
            full.cond = IrCond::Eq;
            full.width = 64;
            full.target = grow;
        }
        const std::int32_t numbersOnly = load(self.r, kNumbersOnlyOffset, 1, false, kAliasHeader);
        const std::int32_t values = newLabel();
        const std::int32_t stored = newLabel();
        branchCmp(numbersOnly, IrCond::Eq, 0, values);
        // Into an array of numbers, if it is one.
        if (!isKnownNumber(value)) {
            if (value.hasKnownTag || value.isConst || value.rep != Rep::Tagged)
                inst(IrOp::Br).imm = grow;
            else
                branchCmp(vt, IrCond::Ne, std::int64_t(Type::Double), grow);
        }
        {
            const std::int32_t slot = fn_.newVreg(false);
            const std::int32_t scaled = shiftLeft(count, 3);
            IrInst& add = inst(IrOp::Add);
            add.d = slot;
            add.a = data;
            add.b = scaled;
            add.width = 64;
            store(slot, 0, vr, 8);
        }
        inst(IrOp::Br).imm = stored;
        inst(IrOp::Label).imm = values;
        // The arguments are owned by now, but the generic path releases them after the call; this one keeps a count.
        if (!value.hasKnownTag)
            incRefOf(vr, vt, false);
        else if (value.known >= Type::String)
            incRefOf(vr, -1, false);
        {
            const std::int32_t slot = fn_.newVreg(false);
            const std::int32_t scaled = shiftLeft(count, 4);
            IrInst& add = inst(IrOp::Add);
            add.d = slot;
            add.a = data;
            add.b = scaled;
            add.width = 64;
            store(slot, 0, vt, 1);
            store(slot, kPayload, vr, 8);
        }
        inst(IrOp::Label).imm = stored;
        const std::int32_t next = fn_.newVreg(false);
        IrInst& plus = inst(IrOp::Add);
        plus.d = next;
        plus.a = count;
        plus.hasImm = true;
        plus.imm = 1;
        plus.width = 64;
        store(self.r, kElementCountOffset, next, 4, kAliasHeader);
        lengthResult(next, r, t);
        inst(IrOp::Br).imm = pushed;
        inst(IrOp::ColdBegin);
        inst(IrOp::Label).imm = grow;
        coldCall(reinterpret_cast<void*>(&scriptDynArrayPush), {rtReg(), self.r, vt, vr}, r, t, true, {callee, self, value});
        inst(IrOp::Br).imm = pushed;
        inst(IrOp::ColdEnd);
        inst(IrOp::Label).imm = pushed;
    }

    /// `array.pop()` on an array that is not empty: the element's count comes along with it.
    void inlinePop(const Entry& self, std::int32_t r, std::int32_t t, std::int32_t generic)
    {
        guardGrowableArray(self, generic);
        const std::int32_t data = loadHeader(self.r, kElementsOffset);
        const std::int32_t count = load(self.r, kElementCountOffset, 4, false, kAliasHeader);
        branchCmp(count, IrCond::Eq, 0, generic);
        const std::int32_t last = fn_.newVreg(false);
        IrInst& sub = inst(IrOp::Sub);
        sub.d = last;
        sub.a = count;
        sub.hasImm = true;
        sub.imm = 1;
        sub.width = 64;
        const std::int32_t numbersOnly = load(self.r, kNumbersOnlyOffset, 1, false, kAliasHeader);
        const std::int32_t values = newLabel();
        const std::int32_t loaded = newLabel();
        branchCmp(numbersOnly, IrCond::Eq, 0, values);
        {
            const std::int32_t bits = loadBareElement(data, last, generic);
            IrInst& mr = inst(IrOp::Mov);
            mr.d = r;
            mr.a = bits;
            IrInst& mt = inst(IrOp::MovImm);
            mt.d = t;
            mt.imm = std::int64_t(Type::Double);
        }
        inst(IrOp::Br).imm = loaded;
        inst(IrOp::Label).imm = values;
        {
            const std::int32_t scaled = shiftLeft(last, 4);
            IrInst& lt = inst(IrOp::LoadIdx);
            lt.d = t;
            lt.a = data;
            lt.b = scaled;
            lt.width = 1;
            IrInst& lr = inst(IrOp::LoadIdx);
            lr.d = r;
            lr.a = data;
            lr.b = scaled;
            lr.imm = kPayload;
            lr.width = 8;
        }
        inst(IrOp::Label).imm = loaded;
        store(self.r, kElementCountOffset, last, 4, kAliasHeader);
    }

    /// `fn.apply(thisArg, array)` with an array nothing else holds -- `arguments` above all -- called with the array's
    /// own elements as the arguments, since no one can change them while the call runs.
    void inlineApply(const Entry& callee, const Entry& self, const std::vector<Entry>& args, std::int32_t r, std::int32_t t,
                     std::int32_t generic)
    {
        const Entry& list = args[1];
        guardArray(list, generic);
        const std::int32_t rc = load(list.r, kRcOffset, 4);
        branchCmp(rc, IrCond::Ne, 1, generic);
        // Its elements are the arguments only if they are values.
        const std::int32_t numbersOnly = load(list.r, kNumbersOnlyOffset, 1, false, kAliasHeader);
        branchCmp(numbersOnly, IrCond::Ne, 0, generic);
        const std::int32_t begin = loadHeader(list.r, kElementsOffset);
        const std::int32_t count = load(list.r, kElementCountOffset, 4, false, kAliasHeader);
        std::vector<Entry> held = args;
        held.push_back(self);
        held.push_back(callee);
        invoke(tagReg(self), payloadReg(self), tagReg(args[0]), payloadReg(args[0]), begin, count, r, t, held);
    }

    /// `string.charCodeAt(i)` for a whole number in range.
    void inlineCharCodeAt(const Entry& self, const Entry& index, std::int32_t r, std::int32_t t, std::int32_t generic)
    {
        if (self.hasKnownTag) {
            if (self.known != Type::String)
                inst(IrOp::Br).imm = generic;
        } else {
            branchCmp(tagReg(self), IrCond::Ne, std::int64_t(Type::String), generic);
        }
        const std::int32_t cell = payloadReg(self);
        std::int32_t i;
        if (index.rep == Rep::Int32 && !index.isConst) {
            i = index.r;
        } else {
            if (!isKnownNumber(index))
                branchCmp(tagReg(index), IrCond::Ne, std::int64_t(Type::Double), generic);
            const std::int32_t fp = doubleReg(index);
            i = fn_.newVreg(false);
            IrInst& conv = inst(IrOp::FpToIntRaw);
            conv.d = i;
            conv.a = fp;
            conv.width = 64;
            const std::int32_t back = fn_.newVreg(true);
            IrInst& again = inst(IrOp::IntToFp);
            again.d = back;
            again.a = i;
            again.srcWidth = 64;
            again.width = 64;
            IrInst& exact = inst(IrOp::BrFCmp);
            exact.a = fp;
            exact.b = back;
            exact.cond = IrCond::Ne;
            exact.width = 64;
            exact.target = generic;
        }
        // Unsigned against the length: a negative index is out of range too.
        const std::int32_t length = load(cell, kStringLengthOffset, 4, false, kAliasHeader);
        {
            IrInst& bound = inst(IrOp::BrCmp);
            bound.a = i;
            bound.b = length;
            bound.cond = IrCond::Hs;
            bound.width = index.rep == Rep::Int32 && !index.isConst ? 32 : 64;
            bound.target = generic;
        }
        const std::int32_t code = fn_.newVreg(false);
        IrInst& ld = inst(IrOp::LoadIdx);
        ld.d = code;
        ld.a = cell;
        ld.b = i;
        ld.scale = 1;
        ld.imm = kStringCharsOffset;
        ld.width = 2;
        const std::int32_t d = fn_.newVreg(true);
        IrInst& conv = inst(IrOp::IntToFp);
        conv.d = d;
        conv.a = code;
        conv.srcWidth = 32;
        conv.width = 64;
        setDoubleResult(d, r, t);
    }

    /// `x.length`: an array's from its elements, a string's from its cell, anything else the long way.
    void lengthProp(Entry base, std::uint32_t operand)
    {
        // Betting it is an array's or a string's, which is an int32 as long as it is below 2^31: anything else is the
        // interpreter's, which reads it again, and the next compile bets no more here.
        if (betHere()) {
            const std::int32_t n = fn_.newVreg(false);
            const std::int32_t bail = newLabel();
            const std::int32_t done = newLabel();
            const std::int32_t notObject = newLabel();
            const std::int32_t tag = base.hasKnownTag ? immReg(std::int64_t(base.known)) : base.t;
            branchCmp(tag, IrCond::Ne, std::int64_t(Type::Object), notObject);
            const std::int32_t both = load(base.r, kCellKindOffset, 2, false, kAliasHeader);
            branchCmp(both, IrCond::Ne, kKindAndClassOfArray, bail);
            const std::int32_t count = load(base.r, kElementCountOffset, 4, false, kAliasHeader);
            branchCmp(count, IrCond::Lt, 0, bail);
            mov(n, count);
            inst(IrOp::Br).imm = done;
            inst(IrOp::Label).imm = notObject;
            branchCmp(tag, IrCond::Ne, std::int64_t(Type::String), bail);
            mov(n, load(base.r, kStringLengthOffset, 4, false, kAliasHeader));
            inst(IrOp::Br).imm = done;
            emitBail(bail, {base});
            inst(IrOp::Label).imm = done;
            Entry length = intEntry(n, true);
            length.atLeastZero = true;
            push(length);
            if (base.own == Own::Owned)
                release(base);
            return;
        }
        const std::int32_t r = fn_.newVreg(false);
        const std::int32_t t = fn_.newVreg(false);
        const std::int32_t slow = newLabel();
        const std::int32_t done = newLabel();
        const std::int32_t notObject = newLabel();
        const std::int32_t tag = base.hasKnownTag ? immReg(std::int64_t(base.known)) : base.t;
        branchCmp(tag, IrCond::Ne, std::int64_t(Type::Object), notObject);
        const std::int32_t both = load(base.r, kCellKindOffset, 2, false, kAliasHeader);
        branchCmp(both, IrCond::Ne, kKindAndClassOfArray, slow);
        lengthResult(load(base.r, kElementCountOffset, 4, false, kAliasHeader), r, t);
        inst(IrOp::Br).imm = done;
        inst(IrOp::Label).imm = notObject;
        branchCmp(tag, IrCond::Ne, std::int64_t(Type::String), slow);
        {
            const std::int32_t length = load(base.r, kStringLengthOffset, 4, false, kAliasHeader);
            const std::int32_t d = fn_.newVreg(true);
            IrInst& conv = inst(IrOp::IntToFp);
            conv.d = d;
            conv.a = length;
            conv.srcWidth = 32;
            conv.width = 64;
            setDoubleResult(d, r, t);
        }
        inst(IrOp::Br).imm = done;
        inst(IrOp::ColdBegin);
        inst(IrOp::Label).imm = slow;
        const std::int32_t rt = rtReg();
        coldCall(reinterpret_cast<void*>(&scriptDynGetProp), {rt, ptrReg(&code_), tagReg(base), base.r, immReg(std::int64_t(operand))},
                 r, t, true, {base});
        inst(IrOp::Br).imm = done;
        inst(IrOp::ColdEnd);
        inst(IrOp::Label).imm = done;
        Entry result = taggedEntry(r, t, Own::Weak);
        if (base.own == Own::Owned) {
            makeOwned(result);
            push(result);
            release(base);
        } else {
            push(result);
        }
    }

    /// A read the site says goes to a getter on the prototype chain: called with the receiver, the result owned.
    void getterProp(Entry base, const PropertySite* site, std::uint32_t operand, bool keepBase)
    {
        if (base.own == Own::Weak)
            makeOwned(base);
        const std::int32_t r = fn_.newVreg(false);
        const std::int32_t t = fn_.newVreg(false);
        const std::int32_t slow = newLabel();
        const std::int32_t done = newLabel();
        guardShape(base, site->shape, slow);
        guardEpoch(site->epoch, slow);
        const std::int64_t at = std::int64_t(site->index) * std::int64_t(sizeof(Prop));
        const std::int32_t props = loadHeader(ptrReg(site->holder), kPropsOffset);
        const std::int32_t ft = load(props, at + kPropValueOffset, 1, false, kAliasValue);
        const std::int32_t fr = load(props, at + kPropValueOffset + kPayload, 8, false, kAliasValue);
        // The getter keeps a count of its own while it runs: it could take itself off the prototype.
        incRefOf(fr, ft, false);
        Entry getter = taggedEntry(fr, ft, Own::Owned);
        std::vector<Entry> none;
        const std::int32_t area = layOutArgs(none);
        invoke(ft, fr, tagReg(base), base.r, area, immReg(0), r, t, {base, getter});
        decRefOf(fr, ft, false);
        inst(IrOp::Br).imm = done;
        inst(IrOp::ColdBegin);
        inst(IrOp::Label).imm = slow;
        const std::int32_t rt = rtReg();
        coldCall(reinterpret_cast<void*>(&scriptDynGetProp), {rt, ptrReg(&code_), tagReg(base), base.r, immReg(std::int64_t(operand))},
                 r, t, true, {base});
        incRefOf(r, t, false);
        inst(IrOp::Br).imm = done;
        inst(IrOp::ColdEnd);
        inst(IrOp::Label).imm = done;
        Entry result = taggedEntry(r, t, Own::Owned);
        push(result);
        // func obj, for a method call: the function under the receiver.
        if (keepBase)
            push(base);
        else
            release(base);
    }

    void construct(std::uint32_t argc)
    {
        std::vector<Entry> args(argc);
        for (std::uint32_t i = argc; i-- > 0;)
            args[i] = pop();
        Entry callee = pop();
        secureWeak();
        securePopped(args);
        if (callee.own == Own::Weak)
            makeOwned(callee);
        std::vector<Entry> held = args;
        held.push_back(callee);
        if (constructInPlace(callee, args, held))
            return;
        const std::int32_t area = layOutArgs(args);
        const std::int32_t rt = rtReg();
        const std::int32_t fnTag = tagReg(callee);
        const std::int32_t fnPayload = payloadReg(callee);
        const std::int32_t count = immReg(std::int64_t(argc));
        const std::int32_t r = fn_.newVreg(false);
        const std::int32_t t = fn_.newVreg(false);
        const std::int32_t slow = newLabel();
        const std::int32_t done = newLabel();
        // A constructor with native code gets its object here and is called directly.
        branchCmp(fnTag, IrCond::Ne, std::int64_t(Type::Object), slow);
        const std::int32_t kind = load(fnPayload, kCellKindOffset, 1, false, kAliasHeader);
        branchCmp(kind, IrCond::Ne, std::int64_t(CellKind::Object), slow);
        const std::int32_t cls = load(fnPayload, kClsOffset, 1, false, kAliasHeader);
        branchCmp(cls, IrCond::Ne, std::int64_t(ObjClass::Function), slow);
        const std::int32_t codePtr = load(fnPayload, kFnCodeOffset, 8, false, kAliasHeader);
        branchCmp(codePtr, IrCond::Eq, 0, slow, 64);
        const std::int32_t entry = load(codePtr, kCallEntryOffset, 8, false, kAliasHeader);
        branchCmp(entry, IrCond::Eq, 0, slow, 64);
        const std::int32_t self = fn_.newVreg(false);
        if (!newOfKnownPrototype(callee, self, slow, held)) {
            coldCall(reinterpret_cast<void*>(&scriptDynNewThis), {rt, fnPayload}, self, -1, true, held);
            branchCmp(self, IrCond::Eq, 0, slow, 64);
        }
        {
            std::vector<Entry> heldWithSelf = held;
            heldWithSelf.push_back(objectEntry(self));
            const std::int32_t selfTag = immReg(std::int64_t(Type::Object));
            IrCall direct;
            direct.kind = IrCallKind::Indirect;
            direct.delegate = entry;
            direct.args = {fnPayload, selfTag, self, area, count};
            direct.result = r;
            direct.resultTag = t;
            direct.site = site();
            direct.publishFrame = false;
            direct.exit = makeExit(0, heldWithSelf);
            forgetFacts();
            fn_.calls.push_back(std::move(direct));
            inst(IrOp::Call).imm = std::int64_t(fn_.calls.size()) - 1;
        }
        // What it returned counts only if that is an object.
        const std::int32_t returned = newLabel();
        branchCmp(t, IrCond::Eq, std::int64_t(Type::Object), returned);
        release(taggedEntry(r, t, Own::Owned));
        {
            IrInst& mov = inst(IrOp::Mov);
            mov.d = r;
            mov.a = self;
            IrInst& tag = inst(IrOp::MovImm);
            tag.d = t;
            tag.imm = std::int64_t(Type::Object);
        }
        inst(IrOp::Br).imm = done;
        inst(IrOp::Label).imm = returned;
        release(objectEntry(self));
        inst(IrOp::ColdBegin);
        inst(IrOp::Label).imm = slow;
        {
            const std::int32_t rtSlow = rtReg();
            IrCall viaHelper;
            viaHelper.kind = IrCallKind::Helper;
            viaHelper.helper = reinterpret_cast<void*>(&scriptDynNew);
            viaHelper.args = {rtSlow, fnTag, fnPayload, area, count};
            viaHelper.result = r;
            viaHelper.resultTag = t;
            viaHelper.site = site();
            viaHelper.publishFrame = true;
            viaHelper.exit = makeExit(0, held);
            fn_.calls.push_back(std::move(viaHelper));
            inst(IrOp::Call).imm = std::int64_t(fn_.calls.size()) - 1;
        }
        inst(IrOp::Br).imm = done;
        inst(IrOp::ColdEnd);
        inst(IrOp::Label).imm = done;
        for (const Entry& arg : args)
            release(arg);
        release(callee);
        push(taggedEntry(r, t, Own::Owned));
    }

    /// The object `new F(...)` starts with, for the function F is known to be, where F's `prototype` is still the object it was
    /// when this was compiled: made here as `scriptDynNewThis` would, or the call goes to `slow`. False, with nothing
    /// emitted, when F is not known so. The prototype is pinned, as for a constructor compiled in place.
    bool newOfKnownPrototype(const Entry& callee, std::int32_t self, std::int32_t slow, const std::vector<Entry>& held)
    {
        if (!callee.pinned || !betHere())
            return false;
        auto* fn = const_cast<DynFunction*>(static_cast<const DynFunction*>(callee.hint));
        if (fn->fnKind != FnKind::Bytecode || fn->isArrow || fn->isClassConstructor || !fn->shape)
            return false;
        const Prop* prototype = fn->findOwn(rt_.atom.prototype.get());
        if (!prototype || fn->accessorOf(*prototype) || !prototype->value.isObject())
            return false;
        auto* proto = static_cast<DynObject*>(detail::Access::ptr(prototype->value));
        if (proto->kind != CellKind::Object)
            return false;
        Shape* root = rt_.rootShapeFor(proto);
        if (!root)
            return false;
        const std::int64_t at = std::int64_t(prototype - fn->props.data()) * std::int64_t(sizeof(Prop));
        guardShape(callee, fn->shape, slow);
        const std::int32_t props = loadHeader(callee.r, kPropsOffset);
        const std::int32_t protoTag = load(props, at + kPropValueOffset, 1, false, kAliasValue);
        branchCmp(protoTag, IrCond::Ne, std::int64_t(Type::Object), slow);
        const std::int32_t protoNow = load(props, at + kPropValueOffset + kPayload, 8, false, kAliasValue);
        keepAlive(proto);
        {
            const std::int32_t want = ptrReg(proto);
            IrInst& same = inst(IrOp::BrCmp);
            same.a = protoNow;
            same.b = want;
            same.cond = IrCond::Ne;
            same.width = 64;
            same.target = slow;
        }
        const std::uint32_t room = std::min<std::uint32_t>(root->expect, 32);
        proto->pin();
        allocateObject(proto, root, room, self, reinterpret_cast<void*>(&scriptDynNewOf), {rtReg(), protoNow, immReg(std::int64_t(room))},
                       held);
        return true;
    }

    /// `new F(...)` for the plain function F is known to be, small enough to go in place of the call: its object is made
    /// here, of a shape known from the start, and its body writes the fields of that. False, with nothing emitted, when it
    /// is not that.
    bool constructInPlace(const Entry& callee, const std::vector<Entry>& args, const std::vector<Entry>& held)
    {
        if (!callee.pinned || depth_ >= 2)
            return false;
        auto* fn = const_cast<DynFunction*>(static_cast<const DynFunction*>(callee.hint));
        if (fn->isArrow || !fn->shape || fn->code->isDerivedConstructor || !inlinable(*fn->code))
            return false;
        std::uint32_t copies = 0;
        for (const Lowering* up = this; up; up = up->parent_) {
            if (&up->code_ == fn->code.get())
                ++copies;
        }
        const Prop* prototype = fn->findOwn(rt_.atom.prototype.get());
        if (copies > 1 || !prototype || fn->accessorOf(*prototype) || !prototype->value.isObject())
            return false;
        auto* proto = static_cast<DynObject*>(detail::Access::ptr(prototype->value));
        Shape* root = rt_.rootShapeFor(proto);
        if (!root)
            return false;
        const std::int64_t at = std::int64_t(prototype - fn->props.data()) * std::int64_t(sizeof(Prop));
        const Mark before = mark();
        const std::int32_t siteHere = site();
        const std::int32_t r = fn_.newVreg(false);
        const std::int32_t t = fn_.newVreg(false);
        const std::int32_t slow = newLabel();
        const std::int32_t done = newLabel();
        const std::int32_t finished = newLabel();
        guardShape(callee, fn->shape, slow);
        const std::int32_t props = loadHeader(callee.r, kPropsOffset);
        const std::int32_t protoTag = load(props, at + kPropValueOffset, 1, false, kAliasValue);
        branchCmp(protoTag, IrCond::Ne, std::int64_t(Type::Object), slow);
        const std::int32_t protoNow = load(props, at + kPropValueOffset + kPayload, 8, false, kAliasValue);
        // Its objects then start from the shape that prototype's do. It stays alive, or something else could come to be
        // at that address.
        keepAlive(proto);
        {
            const std::int32_t want = ptrReg(proto);
            IrInst& same = inst(IrOp::BrCmp);
            same.a = protoNow;
            same.b = want;
            same.cond = IrCond::Ne;
            same.width = 64;
            same.target = slow;
        }
        const std::int32_t self = fn_.newVreg(false);
        const std::uint32_t room = std::min<std::uint32_t>(root->expect, 32);
        // Hot enough to be compiled in place, and its address is in the code from here on.
        proto->pin();
        allocateObject(proto, root, room, self, reinterpret_cast<void*>(&scriptDynNewOf), {rtReg(), protoNow, immReg(std::int64_t(room))},
                       held);
        const std::vector<std::uint8_t> none(fn->code->numLocals + 1, 1);
        std::vector<Entry> heldWithSelf = held;
        heldWithSelf.push_back(objectEntry(self));
        try {
            Lowering inner(*this, *fn->code, none, heldWithSelf, siteHere);
            inner.startShape_ = root;
            inner.freshThis_ = true;
            inner.freshRoom_ = room;
            inner.runInline(callee.r, objectEntry(self), args, r, t, finished);
        } catch (const Refuse&) {
            rollBack(before);
            return false;
        }
        ++fn_.inlinedMethods;
        inst(IrOp::Label).imm = finished;
        forgetFacts();
        // What it returned counts only if that is an object.
        const std::int32_t returned = newLabel();
        branchCmp(t, IrCond::Eq, std::int64_t(Type::Object), returned);
        release(taggedEntry(r, t, Own::Owned));
        {
            IrInst& mov = inst(IrOp::Mov);
            mov.d = r;
            mov.a = self;
            IrInst& tag = inst(IrOp::MovImm);
            tag.d = t;
            tag.imm = std::int64_t(Type::Object);
        }
        inst(IrOp::Br).imm = done;
        inst(IrOp::Label).imm = returned;
        release(objectEntry(self));
        inst(IrOp::Br).imm = done;
        inst(IrOp::ColdBegin);
        inst(IrOp::Label).imm = slow;
        {
            const std::int32_t area = layOutArgs(const_cast<std::vector<Entry>&>(args));
            IrCall viaHelper;
            viaHelper.kind = IrCallKind::Helper;
            viaHelper.helper = reinterpret_cast<void*>(&scriptDynNew);
            viaHelper.args = {rtReg(), immReg(std::int64_t(Type::Object)), callee.r, area, immReg(std::int64_t(args.size()))};
            viaHelper.result = r;
            viaHelper.resultTag = t;
            viaHelper.site = site();
            viaHelper.publishFrame = true;
            viaHelper.exit = makeExit(0, held);
            fn_.calls.push_back(std::move(viaHelper));
            inst(IrOp::Call).imm = std::int64_t(fn_.calls.size()) - 1;
        }
        inst(IrOp::Br).imm = done;
        inst(IrOp::ColdEnd);
        inst(IrOp::Label).imm = done;
        for (const Entry& arg : args)
            release(arg);
        push(taggedEntry(r, t, Own::Owned));
        return true;
    }

    /// `local = local + x`: the left operand borrows a local nothing else on the stack borrows, which holds a count of
    /// its own, and the sum goes straight back into it.
    bool appendsToLocal(std::uint32_t pc) const
    {
        const std::vector<std::uint8_t>& c = code_.code;
        // `s += x` compiles to Add, Dup, SetLocal: the copy left behind is of the new value.
        std::uint32_t at = pc + 1;
        if (at < c.size() && Op(c[at]) == Op::Dup)
            ++at;
        if (at + 4 >= c.size() || Op(c[at]) != Op::SetLocal || stack_.size() < 2)
            return false;
        const Entry& a = stack_[stack_.size() - 2];
        const std::uint32_t slot = readOperand(c, at + 1);
        if (a.own != Own::Local || a.local != std::int32_t(slot) || slot < std::uint32_t(std::max(code_.arity, 0)) ||
            slot >= localOwned_.size() || !localOwned_[slot])
            return false;
        for (std::size_t i = 0; i + 2 < stack_.size(); ++i) {
            if (stack_[i].local == std::int32_t(slot))
                return false;
        }
        return true;
    }

    void lowerAdd(bool appendsToLocal = false)
    {
        Entry b = pop();
        Entry a = pop();
        if (intArithmetic(Op::Add, a, b))
            return;
        if (isKnownNumber(a) && isKnownNumber(b)) {
            const std::int32_t x = doubleReg(a);
            const std::int32_t y = doubleReg(b);
            const std::int32_t d = fn_.newVreg(true);
            IrInst& in = inst(IrOp::FAdd);
            in.d = d;
            in.a = x;
            in.b = y;
            in.width = 64;
            push(doubleEntry(d));
            return;
        }
        secureWeak();
        // Two numbers inline; anything else, strings above all, in a helper.
        const std::int32_t ta = tagReg(a);
        const std::int32_t pa = payloadReg(a);
        const std::int32_t tb = tagReg(b);
        const std::int32_t pb = payloadReg(b);
        const std::int32_t r = fn_.newVreg(false);
        const std::int32_t t = fn_.newVreg(false);
        const std::int32_t slow = newLabel();
        const std::int32_t done = newLabel();
        branchCmp(ta, IrCond::Ne, std::int64_t(Type::Double), slow);
        branchCmp(tb, IrCond::Ne, std::int64_t(Type::Double), slow);
        {
            const std::int32_t x = fn_.newVreg(true), y = fn_.newVreg(true), sum = fn_.newVreg(true);
            IrInst& bx = inst(IrOp::BitsToFp);
            bx.d = x;
            bx.a = pa;
            bx.width = 64;
            IrInst& by = inst(IrOp::BitsToFp);
            by.d = y;
            by.a = pb;
            by.width = 64;
            IrInst& add = inst(IrOp::FAdd);
            add.d = sum;
            add.a = x;
            add.b = y;
            add.width = 64;
            IrInst& back = inst(IrOp::FpToBits);
            back.d = r;
            back.a = sum;
            back.width = 64;
            IrInst& tag = inst(IrOp::MovImm);
            tag.d = t;
            tag.imm = std::int64_t(Type::Double);
        }
        inst(IrOp::ColdBegin);
        inst(IrOp::Label).imm = slow;
        const std::int32_t rt = rtReg();
        coldCall(appendsToLocal ? reinterpret_cast<void*>(&scriptDynAppendLocal) : reinterpret_cast<void*>(&scriptDynAdd),
                 {rt, ta, pa, tb, pb}, r, t, true, {a, b});
        inst(IrOp::Br).imm = done;
        inst(IrOp::ColdEnd);
        inst(IrOp::Label).imm = done;
        release(b);
        release(a);
        push(taggedEntry(r, t, Own::Owned));
    }

    /// The 64-bit sign extension of an int32 entry.
    std::int32_t wideInt(const Entry& e)
    {
        if (e.isConst)
            return immReg(std::int64_t(std::int32_t(e.number)));
        if (e.wide || e.rep == Rep::Exact)
            return e.r;
        const std::int32_t d = fn_.newVreg(false);
        IrInst& in = inst(IrOp::Sext32);
        in.d = d;
        in.a = e.r;
        return d;
    }
    /// `x % divisor` for `0 <= x < 2^31` into `d`: a power of two is a mask, anything else a multiply by its reciprocal
    /// rather than a division, which takes the machine ten times as long.
    void remainderByConstant(std::int32_t x, std::uint32_t divisor, std::int32_t d)
    {
        if ((divisor & (divisor - 1)) == 0) {
            IrInst& in = inst(IrOp::And);
            in.d = d;
            in.a = x;
            in.hasImm = true;
            in.imm = std::int64_t(divisor - 1);
            in.width = 64;
            return;
        }
        // With l = ceil(log2 divisor) and m = ceil(2^(31+l) / divisor), (x * m) >> (31 + l) is x / divisor for every x
        // below 2^31; m is below 2^32, so the product fits in 64 bits.
        unsigned l = 0;
        while ((std::uint64_t(1) << l) < divisor)
            ++l;
        const std::uint64_t m = ((std::uint64_t(1) << (31 + l)) + divisor - 1) / divisor;
        const std::int32_t reciprocal = immReg(std::int64_t(m));
        const std::int32_t product = fn_.newVreg(false);
        IrInst& mul = inst(IrOp::Mul);
        mul.d = product;
        mul.a = x;
        mul.b = reciprocal;
        mul.width = 64;
        const std::int32_t quotient = fn_.newVreg(false);
        IrInst& shr = inst(IrOp::Shr);
        shr.d = quotient;
        shr.a = product;
        shr.hasImm = true;
        shr.imm = std::int64_t(31 + l);
        shr.width = 64;
        const std::int32_t by = immReg(std::int64_t(divisor));
        const std::int32_t back = fn_.newVreg(false);
        IrInst& times = inst(IrOp::Mul);
        times.d = back;
        times.a = quotient;
        times.b = by;
        times.width = 64;
        IrInst& sub = inst(IrOp::Sub);
        sub.d = d;
        sub.a = x;
        sub.b = back;
        sub.width = 64;
    }

    /// Bails unless `wide`, a 64-bit result, is an int32.
    void expectInt32(std::int32_t wide, std::int32_t bail)
    {
        const std::int32_t narrow = fn_.newVreg(false);
        IrInst& ext = inst(IrOp::Sext32);
        ext.d = narrow;
        ext.a = wide;
        IrInst& test = inst(IrOp::BrCmp);
        test.a = narrow;
        test.b = wide;
        test.cond = IrCond::Ne;
        test.width = 64;
        test.target = bail;
    }
    static std::uint8_t bitLength(std::uint64_t v) noexcept { return v ? std::uint8_t(64 - __builtin_clzll(v)) : 0; }
    static std::uint8_t magOf(const Entry& e) noexcept
    {
        return e.isConst ? bitLength(std::uint64_t(std::fabs(e.number))) : e.mag;
    }
    static bool notNegative(const Entry& e) noexcept { return e.isConst ? e.number >= 0 : e.atLeastZero; }
    /// `+`, `-` and `*` of integers small enough that the answer is exact in 64 bits and in a double: there is nothing to
    /// check, for now, and what uses it decides what it needs to be. False, with nothing emitted, when it is not that.
    bool exactArithmetic(Op op, const Entry& a, const Entry& b)
    {
        if ((op != Op::Add && op != Op::Sub && op != Op::Mul) || (a.isConst && b.isConst))
            return false;
        if ((a.rep != Rep::Exact && !isIntLike(a)) || (b.rep != Rep::Exact && !isIntLike(b)))
            return false;
        const unsigned mag = op == Op::Mul ? unsigned(magOf(a)) + magOf(b) : std::max(magOf(a), magOf(b)) + 1u;
        if (mag > 52)
            return false;
        // Zero times a negative is minus zero, which no integer is.
        const bool noMinusZero = op != Op::Mul || (notNegative(a) && notNegative(b)) || (a.isConst && a.number > 0) ||
                                 (b.isConst && b.number > 0);
        if (!noMinusZero && !betHere())
            return false;
        const std::int32_t x = wideInt(a);
        const bool imm = b.isConst && op != Op::Mul;
        const std::int32_t y = imm ? -1 : wideInt(b);
        const std::int32_t d = fn_.newVreg(false);
        IrInst& in = inst(op == Op::Add ? IrOp::Add : op == Op::Sub ? IrOp::Sub : IrOp::Mul);
        in.d = d;
        in.a = x;
        in.b = y;
        in.width = 64;
        if (imm) {
            in.hasImm = true;
            in.imm = std::int64_t(std::int32_t(b.number));
        }
        if (!noMinusZero) {
            const std::int32_t bail = newLabel();
            const std::int32_t skip = newLabel();
            branchCmp(d, IrCond::Ne, 0, skip, 64);
            const std::int32_t either = fn_.newVreg(false);
            IrInst& orr = inst(IrOp::Or);
            orr.d = either;
            orr.a = x;
            orr.b = y;
            orr.width = 64;
            branchCmp(either, IrCond::Lt, 0, bail, 64);
            inst(IrOp::Label).imm = skip;
            emitBail(bail, {a, b});
        }
        Entry e;
        e.rep = Rep::Exact;
        e.r = d;
        e.mag = std::uint8_t(mag);
        e.atLeastZero = op != Op::Sub && notNegative(a) && notNegative(b);
        push(e);
        return true;
    }
    /// What is about to use the stack as something other than the operands of arithmetic gets the integers it holds as
    /// int32s, the guess, or else as doubles.
    void settleExact(Op op, std::uint32_t pc)
    {
        switch (op) {
            case Op::Add:
            case Op::Sub:
            case Op::Mul:
            case Op::BitAnd:
            case Op::BitOr:
            case Op::BitXor:
            case Op::BitNot:
            case Op::Shl:
            case Op::Shr:
            case Op::UShr:
            case Op::GetLocal:
            case Op::PushConst:
            case Op::Pop: return;
            default: break;
        }
        bool wantsDouble = false;
        if (op == Op::SetLocal) {
            const std::uint32_t slot = readOperand(code_.code, pc + 1);
            wantsDouble = isNumberLocal(slot) && !isIntLocal(slot);
        }
        for (std::size_t i = 0; i < stack_.size(); ++i) {
            if (stack_[i].rep != Rep::Exact)
                continue;
            if (wantsDouble || !betHere()) {
                const std::int32_t d = doubleReg(stack_[i]);
                stack_[i] = doubleEntry(d);
                continue;
            }
            const std::int32_t bail = newLabel();
            if (stack_[i].mag > 31)
                expectInt32(stack_[i].r, bail);
            emitBail(bail, {});
            stack_[i].rep = Rep::Int32;
            stack_[i].wide = true;
            stack_[i].mag = std::min<std::uint8_t>(stack_[i].mag, 31);
        }
    }

    /// `a + b`, `a - b`, `a * b` and `a % b` of int32s as an int32, where that is what the answer is; where it is not, the
    /// interpreter does it. False, with nothing emitted, when this is no place for the guess.
    bool intArithmetic(Op op, const Entry& a, const Entry& b)
    {
        if (exactArithmetic(op, a, b))
            return true;
        if (!betHere() || !isIntLike(a) || !isIntLike(b) || (a.isConst && b.isConst))
            return false;
        if (op == Op::Mod && !(b.isConst ? b.number >= 1 : true))
            return false;
        const std::int32_t bail = newLabel();
        const std::int32_t d = fn_.newVreg(false);
        if (op == Op::Mod) {
            // Only what the machine's remainder agrees with: a dividend of at least zero, a divisor of at least one.
            const std::int32_t x = wideInt(a);
            branchCmp(x, IrCond::Lt, 0, bail, 64);
            IrInst* rem = nullptr;
            if (b.isConst) {
                remainderByConstant(x, std::uint32_t(b.number), d);
                emitBail(bail, {a, b});
                Entry e = intEntry(d, true);
                e.mag = bitLength(std::uint64_t(b.number));
                e.atLeastZero = true;
                push(e);
                return true;
            }
            if (b.isConst) {
                IrInst& in = inst(IrOp::SRem);
                in.hasImm = true;
                in.imm = std::int64_t(b.number);
                rem = &in;
            } else {
                const std::int32_t y = wideInt(b);
                branchCmp(y, IrCond::Le, 0, bail, 64);
                IrInst& in = inst(IrOp::SRem);
                in.b = y;
                rem = &in;
            }
            rem->d = d;
            rem->a = x;
            rem->width = 64;
        } else {
            const std::int32_t x = wideInt(a);
            const bool imm = b.isConst && op != Op::Mul;
            const std::int32_t y = imm ? -1 : wideInt(b);
            IrInst& in = inst(op == Op::Add ? IrOp::Add : op == Op::Sub ? IrOp::Sub : IrOp::Mul);
            in.d = d;
            in.a = x;
            in.b = y;
            in.width = 64;
            if (imm) {
                in.hasImm = true;
                in.imm = std::int64_t(std::int32_t(b.number));
            }
            expectInt32(d, bail);
            if (op == Op::Mul) {
                // Zero from a negative operand is minus zero, which no int32 is.
                const std::int32_t skip = newLabel();
                branchCmp(d, IrCond::Ne, 0, skip, 64);
                const std::int32_t either = fn_.newVreg(false);
                IrInst& orr = inst(IrOp::Or);
                orr.d = either;
                orr.a = x;
                orr.b = y;
                orr.width = 64;
                branchCmp(either, IrCond::Lt, 0, bail, 64);
                inst(IrOp::Label).imm = skip;
            }
        }
        emitBail(bail, {a, b});
        push(intEntry(d, true));
        return true;
    }

    void arithmetic(Op op)
    {
        const Entry b = pop();
        const Entry a = pop();
        if (op != Op::Div && intArithmetic(op, a, b))
            return;
        secureWeakFor(a, b);
        const std::int32_t x = toNumberReg(a);
        const std::int32_t y = toNumberReg(b);
        const std::int32_t d = fn_.newVreg(true);
        IrInst& in = inst(op == Op::Sub ? IrOp::FSub : op == Op::Mul ? IrOp::FMul : IrOp::FDiv);
        in.d = d;
        in.a = x;
        in.b = y;
        in.width = 64;
        release(b);
        release(a);
        push(doubleEntry(d));
    }

    /// `%`: integers when both are, positive ones at least, which is what the machine's remainder agrees with; `fmod`
    /// otherwise.
    void remainder()
    {
        const Entry b = pop();
        const Entry a = pop();
        if (intArithmetic(Op::Mod, a, b))
            return;
        secureWeakFor(a, b);
        const std::int32_t x = toNumberReg(a);
        const bool constDivisor = b.isConst && b.number >= 2 && b.number < 2147483648.0 && double(std::int32_t(b.number)) == b.number;
        const std::int32_t y = constDivisor ? -1 : toNumberReg(b);
        const std::int32_t d = fn_.newVreg(true);
        const std::int32_t slow = newLabel();
        const std::int32_t done = newLabel();
        const std::int32_t ia = fn_.newVreg(false);
        IrInst& conv = inst(IrOp::FpToIntRaw);
        conv.d = ia;
        conv.a = x;
        const std::int32_t wa = fn_.newVreg(false);
        IrInst& extA = inst(IrOp::Sext32);
        extA.d = wa;
        extA.a = ia;
        IrInst& fits = inst(IrOp::BrCmp);
        fits.a = wa;
        fits.b = ia;
        fits.cond = IrCond::Ne;
        fits.width = 64;
        fits.target = slow;
        // The sign of the double, not of the integer: minus zero has a remainder of minus zero.
        {
            const std::int32_t bits = fn_.newVreg(false);
            IrInst& raw = inst(IrOp::FpToBits);
            raw.d = bits;
            raw.a = x;
            raw.width = 64;
            branchCmp(bits, IrCond::Lt, 0, slow, 64);
        }
        {
            const std::int32_t back = fn_.newVreg(true);
            IrInst& to = inst(IrOp::IntToFp);
            to.d = back;
            to.a = wa;
            to.srcWidth = 32;
            to.width = 64;
            const std::int32_t same = newLabel();
            IrInst& eq = inst(IrOp::BrFCmp);
            eq.a = x;
            eq.b = back;
            eq.cond = IrCond::FEq;
            eq.width = 64;
            eq.target = same;
            inst(IrOp::Br).imm = slow;
            inst(IrOp::Label).imm = same;
        }
        std::int32_t rem = fn_.newVreg(false);
        if (constDivisor) {
            IrInst& in = inst(IrOp::SRem);
            in.d = rem;
            in.a = wa;
            in.hasImm = true;
            in.imm = std::int64_t(b.number);
            in.width = 32;
        } else {
            const std::int32_t ib = fn_.newVreg(false);
            IrInst& convB = inst(IrOp::FpToIntRaw);
            convB.d = ib;
            convB.a = y;
            const std::int32_t wb = fn_.newVreg(false);
            IrInst& extB = inst(IrOp::Sext32);
            extB.d = wb;
            extB.a = ib;
            IrInst& fitsB = inst(IrOp::BrCmp);
            fitsB.a = wb;
            fitsB.b = ib;
            fitsB.cond = IrCond::Ne;
            fitsB.width = 64;
            fitsB.target = slow;
            branchCmp(wb, IrCond::Le, 0, slow);
            const std::int32_t back = fn_.newVreg(true);
            IrInst& to = inst(IrOp::IntToFp);
            to.d = back;
            to.a = wb;
            to.srcWidth = 32;
            to.width = 64;
            const std::int32_t same = newLabel();
            IrInst& eq = inst(IrOp::BrFCmp);
            eq.a = y;
            eq.b = back;
            eq.cond = IrCond::FEq;
            eq.width = 64;
            eq.target = same;
            inst(IrOp::Br).imm = slow;
            inst(IrOp::Label).imm = same;
            IrInst& in = inst(IrOp::SRem);
            in.d = rem;
            in.a = wa;
            in.b = wb;
            in.width = 32;
        }
        IrInst& result = inst(IrOp::IntToFp);
        result.d = d;
        result.a = rem;
        result.srcWidth = 32;
        result.width = 64;
        inst(IrOp::ColdBegin);
        inst(IrOp::Label).imm = slow;
        {
            const std::int32_t divisor = constDivisor ? fpImm(b.number) : y;
            coldCall(reinterpret_cast<void*>(&scriptDynFmod), {x, divisor}, d, -1, false);
        }
        inst(IrOp::Br).imm = done;
        inst(IrOp::ColdEnd);
        inst(IrOp::Label).imm = done;
        release(b);
        release(a);
        push(doubleEntry(d));
    }

    void bitwise(Op op)
    {
        const Entry b = pop();
        const Entry a = pop();
        // `x | 0` on an integer of more than 32 bits: its low word.
        if (a.rep == Rep::Exact && b.isConst && b.rep != Rep::Tagged && b.number == 0 && (op == Op::BitOr || op == Op::BitXor)) {
            const std::int32_t d = fn_.newVreg(false);
            IrInst& ext = inst(IrOp::Sext32);
            ext.d = d;
            ext.a = a.r;
            Entry e = intEntry(d, true);
            e.mag = std::min<std::uint8_t>(a.mag, 31);
            e.atLeastZero = a.atLeastZero && a.mag <= 31;
            push(e);
            return;
        }
        // `x | 0` and its kind on an int32: x itself.
        if (a.rep == Rep::Int32 && b.isConst && b.rep != Rep::Tagged) {
            const bool zero = b.number == 0;
            const bool allOnes = b.number == -1;
            if ((zero && (op == Op::BitOr || op == Op::BitXor || op == Op::Shl || op == Op::Shr)) ||
                (allOnes && op == Op::BitAnd)) {
                push(a);
                return;
            }
        }
        secureWeakFor(a, b);
        const std::int32_t x = toInt32Reg(a);
        const bool isImm = b.isConst && b.rep != Rep::Tagged && double(std::int32_t(b.number)) == b.number;
        const std::int32_t y = isImm ? -1 : toInt32Reg(b);
        const std::int32_t d = fn_.newVreg(false);
        IrOp irOp = IrOp::And;
        switch (op) {
            case Op::BitAnd: irOp = IrOp::And; break;
            case Op::BitOr: irOp = IrOp::Or; break;
            case Op::BitXor: irOp = IrOp::Xor; break;
            case Op::Shl: irOp = IrOp::Shl; break;
            case Op::Shr: irOp = IrOp::Sar; break;
            default: irOp = IrOp::Shr; break;
        }
        IrInst& in = inst(irOp);
        in.d = d;
        in.a = x;
        in.b = y;
        in.width = 32;
        if (isImm) {
            in.hasImm = true;
            in.imm = std::int64_t(std::int32_t(b.number));
        }
        release(b);
        release(a);
        const std::uint32_t count = isImm ? std::uint32_t(std::int32_t(b.number)) & 31 : 0;
        if (op == Op::UShr) {
            // A uint32, in the 64 bits a 32-bit operation leaves it zero-extended in.
            Entry e;
            e.rep = Rep::Exact;
            e.r = d;
            e.mag = std::uint8_t(32 - count);
            e.atLeastZero = true;
            push(e);
            return;
        }
        Entry e = intEntry(d);
        if (op == Op::BitAnd && isImm && b.number >= 0) {
            // Only the bits of a mask of at most 31: zero-extended and sign-extended are the same.
            e.wide = true;
            e.mag = bitLength(std::uint64_t(b.number));
            e.atLeastZero = true;
        } else if (op == Op::Shr && isImm) {
            e.mag = std::uint8_t(31 - count);
            e.atLeastZero = notNegative(a) && a.rep == Rep::Int32;
        }
        push(e);
    }

    void relational(Op op)
    {
        const Entry b = pop();
        const Entry a = pop();
        secureWeakFor(a, b);
        const std::int32_t d = fn_.newVreg(false);
        if (intCompare(a, b, op == Op::Lt ? IrCond::Lt : op == Op::Le ? IrCond::Le : op == Op::Gt ? IrCond::Gt : IrCond::Ge, d))
            return;
        if (isKnownNumber(a) && isKnownNumber(b)) {
            const std::int32_t x = doubleReg(a);
            const std::int32_t y = doubleReg(b);
            IrInst& in = inst(IrOp::FCset);
            in.d = d;
            in.a = x;
            in.b = y;
            in.cond = fpCond(op);
            in.width = 64;
            push(boolEntry(d));
            return;
        }
        const std::int32_t ta = tagReg(a);
        const std::int32_t pa = payloadReg(a);
        const std::int32_t tb = tagReg(b);
        const std::int32_t pb = payloadReg(b);
        const std::int32_t slow = newLabel();
        const std::int32_t done = newLabel();
        branchCmp(ta, IrCond::Ne, std::int64_t(Type::Double), slow);
        branchCmp(tb, IrCond::Ne, std::int64_t(Type::Double), slow);
        {
            const std::int32_t x = fn_.newVreg(true), y = fn_.newVreg(true);
            IrInst& bx = inst(IrOp::BitsToFp);
            bx.d = x;
            bx.a = pa;
            bx.width = 64;
            IrInst& by = inst(IrOp::BitsToFp);
            by.d = y;
            by.a = pb;
            by.width = 64;
            IrInst& in = inst(IrOp::FCset);
            in.d = d;
            in.a = x;
            in.b = y;
            in.cond = fpCond(op);
            in.width = 64;
        }
        inst(IrOp::ColdBegin);
        inst(IrOp::Label).imm = slow;
        {
            const std::int32_t rt = rtReg();
            const std::int32_t which = immReg(std::int64_t(op));
            coldCall(reinterpret_cast<void*>(&scriptDynCompare), {rt, which, ta, pa, tb, pb}, d, -1, true, {a, b});
        }
        inst(IrOp::Br).imm = done;
        inst(IrOp::ColdEnd);
        inst(IrOp::Label).imm = done;
        release(b);
        release(a);
        push(boolEntry(d));
    }

    /// Two int32s, or one and a constant, compared as what they are.
    bool intCompare(Entry a, Entry b, IrCond cond, std::int32_t d)
    {
        if (!isIntLike(a) || !isIntLike(b) || (a.isConst && b.isConst))
            return false;
        if (a.isConst) {
            std::swap(a, b);
            cond = cond == IrCond::Lt ? IrCond::Gt : cond == IrCond::Le ? IrCond::Ge : cond == IrCond::Gt ? IrCond::Lt : cond == IrCond::Ge ? IrCond::Le : cond;
        }
        IrInst& in = inst(IrOp::Cset);
        in.d = d;
        in.a = a.r;
        in.cond = cond;
        in.width = 32;
        if (b.isConst) {
            in.hasImm = true;
            in.imm = std::int64_t(b.number);
        } else {
            in.b = b.r;
        }
        push(boolEntry(d));
        return true;
    }

    void equality(Op op)
    {
        const Entry b = pop();
        const Entry a = pop();
        secureWeak();
        const bool strict = op == Op::StrictEq || op == Op::StrictNe;
        const bool wantEqual = op == Op::StrictEq || op == Op::Eq;
        std::int32_t d = fn_.newVreg(false);
        if (intCompare(a, b, wantEqual ? IrCond::Eq : IrCond::Ne, d))
            return;
        if (isKnownNumber(a) && isKnownNumber(b)) {
            const std::int32_t x = doubleReg(a);
            const std::int32_t y = doubleReg(b);
            IrInst& in = inst(IrOp::FCset);
            in.d = d;
            in.a = x;
            in.b = y;
            in.cond = wantEqual ? IrCond::FEq : IrCond::FNe;
            in.width = 64;
            push(boolEntry(d));
            return;
        }
        // Strictly against a number: a double compares inline; any other tag -- a host's integer is a number too -- goes
        // to the helper.
        if (strict && isKnownNumber(a) != isKnownNumber(b)) {
            const Entry& number = isKnownNumber(a) ? a : b;
            const Entry& other = isKnownNumber(a) ? b : a;
            const std::int32_t x = doubleReg(number);
            const std::int32_t tag = tagReg(other);
            const std::int32_t payload = payloadReg(other);
            const std::int32_t done = newLabel();
            const std::int32_t slow = newLabel();
            branchCmp(tag, IrCond::Ne, std::int64_t(Type::Double), slow);
            const std::int32_t y = fn_.newVreg(true);
            IrInst& bits = inst(IrOp::BitsToFp);
            bits.d = y;
            bits.a = payload;
            bits.width = 64;
            IrInst& in = inst(IrOp::FCset);
            in.d = d;
            in.a = x;
            in.b = y;
            in.cond = wantEqual ? IrCond::FEq : IrCond::FNe;
            in.width = 64;
            inst(IrOp::Br).imm = done;
            inst(IrOp::ColdBegin);
            inst(IrOp::Label).imm = slow;
            {
                const std::int32_t nt = tagReg(number);
                const std::int32_t np = payloadReg(number);
                const std::int32_t same = fn_.newVreg(false);
                coldCall(reinterpret_cast<void*>(&scriptDynStrictEquals), {tag, payload, nt, np}, same, -1, false);
                IrInst& set = inst(IrOp::Xor);
                set.d = d;
                set.a = same;
                set.hasImm = true;
                set.imm = wantEqual ? 0 : 1;
                set.width = 32;
            }
            inst(IrOp::Br).imm = done;
            inst(IrOp::ColdEnd);
            inst(IrOp::Label).imm = done;
            release(other);
            release(number);
            push(boolEntry(d));
            return;
        }
        // Against undefined or null, only the tags matter.
        const auto isNullish = [](const Entry& e) {
            return e.isConst && e.rep == Rep::Tagged && (e.known == Type::Undefined || e.known == Type::Null);
        };
        if ((isNullish(a) || isNullish(b)) && !(isNullish(a) && isNullish(b))) {
            const Entry& nullish = isNullish(a) ? a : b;
            const Entry& other = isNullish(a) ? b : a;
            const std::int32_t tag = tagReg(other);
            IrInst& in = inst(IrOp::Cset);
            in.d = d;
            in.a = tag;
            in.hasImm = true;
            in.width = 32;
            if (strict) {
                in.imm = std::int64_t(nullish.known);
                in.cond = wantEqual ? IrCond::Eq : IrCond::Ne;
            } else {
                in.imm = std::int64_t(Type::Null);
                in.cond = wantEqual ? IrCond::Ls : IrCond::Hi;
            }
            release(other);
            push(boolEntry(d));
            return;
        }
        const std::int32_t ta = tagReg(a);
        const std::int32_t pa = payloadReg(a);
        const std::int32_t tb = tagReg(b);
        const std::int32_t pb = payloadReg(b);
        // Two objects are equal, loosely or strictly, when they are the same object.
        const std::int32_t slow = newLabel();
        const std::int32_t done = newLabel();
        const std::int32_t notObject = newLabel();
        branchCmp(ta, IrCond::Ne, std::int64_t(Type::Object), notObject);
        branchCmp(tb, IrCond::Ne, std::int64_t(Type::Object), slow);
        {
            IrInst& same = inst(IrOp::Cset);
            same.d = d;
            same.a = pa;
            same.b = pb;
            same.cond = wantEqual ? IrCond::Eq : IrCond::Ne;
            same.width = 64;
        }
        inst(IrOp::Br).imm = done;
        inst(IrOp::ColdBegin);
        inst(IrOp::Label).imm = notObject;
        {
            // The same tag, of a kind its bits answer for: numbers by value, booleans by their byte, undefined and null
            // always. Anything else -- strings, mixed kinds -- the helper.
            IrInst& sameTag = inst(IrOp::BrCmp);
            sameTag.a = ta;
            sameTag.b = tb;
            sameTag.cond = IrCond::Ne;
            sameTag.width = 32;
            sameTag.target = slow;
            const std::int32_t notDouble = newLabel();
            branchCmp(ta, IrCond::Ne, std::int64_t(Type::Double), notDouble);
            const std::int32_t x = fn_.newVreg(true);
            IrInst& bx = inst(IrOp::BitsToFp);
            bx.d = x;
            bx.a = pa;
            bx.width = 64;
            const std::int32_t y = fn_.newVreg(true);
            IrInst& by = inst(IrOp::BitsToFp);
            by.d = y;
            by.a = pb;
            by.width = 64;
            IrInst& cmp = inst(IrOp::FCset);
            cmp.d = d;
            cmp.a = x;
            cmp.b = y;
            cmp.cond = wantEqual ? IrCond::FEq : IrCond::FNe;
            cmp.width = 64;
            inst(IrOp::Br).imm = done;
            inst(IrOp::Label).imm = notDouble;
            const std::int32_t notBool = newLabel();
            branchCmp(ta, IrCond::Ne, std::int64_t(Type::Bool), notBool);
            const auto low = [&](std::int32_t v) {
                const std::int32_t out = fn_.newVreg(false);
                IrInst& in = inst(IrOp::And);
                in.d = out;
                in.a = v;
                in.hasImm = true;
                in.imm = 0xff;
                in.width = 32;
                return out;
            };
            const std::int32_t la = low(pa);
            const std::int32_t lb = low(pb);
            IrInst& eq = inst(IrOp::Cset);
            eq.d = d;
            eq.a = la;
            eq.b = lb;
            eq.cond = wantEqual ? IrCond::Eq : IrCond::Ne;
            eq.width = 32;
            inst(IrOp::Br).imm = done;
            inst(IrOp::Label).imm = notBool;
            branchCmp(ta, IrCond::Hi, std::int64_t(Type::Null), slow);
            IrInst& always = inst(IrOp::MovImm);
            always.d = d;
            always.imm = wantEqual ? 1 : 0;
            inst(IrOp::Br).imm = done;
        }
        inst(IrOp::Label).imm = slow;
        const std::int32_t answer = fn_.newVreg(false);
        if (strict) {
            coldCall(reinterpret_cast<void*>(&scriptDynStrictEquals), {ta, pa, tb, pb}, answer, -1, false);
        } else {
            const std::int32_t rt = rtReg();
            coldCall(reinterpret_cast<void*>(&scriptDynLooseEquals), {rt, ta, pa, tb, pb}, answer, -1, true, {a, b});
        }
        {
            IrInst& in = inst(IrOp::Xor);
            in.d = d;
            in.a = answer;
            in.hasImm = true;
            in.imm = wantEqual ? 0 : 1;
            in.width = 32;
        }
        inst(IrOp::Br).imm = done;
        inst(IrOp::ColdEnd);
        inst(IrOp::Label).imm = done;
        release(b);
        release(a);
        push(boolEntry(d));
    }

    struct Join
    {
        bool seen = false;
        std::vector<Entry> entries;
        /// Per position, the function every way in so far has had there, known and pinned; null where they differ.
        std::vector<const void*> sameFunction;
        /// Per position, the tag every way in so far has known the value there to have; -1 where one did not, or they differ.
        std::vector<std::int16_t> sameTag;
    };
    static std::int16_t knownTagOf(const Entry& e) noexcept
    {
        if (e.rep != Rep::Tagged)
            return std::int16_t(Type::Double);
        return e.hasKnownTag ? std::int16_t(e.known) : std::int16_t(-1);
    }

    Runtime& rt_;
    const FunctionCode& code_;
    IrFunction& fn_;
    const std::vector<std::uint8_t>& notNumber_;
    const std::vector<std::uint8_t>& notInt_;
    std::uint32_t pc_ = 0;
    std::int32_t fnReg_ = -1, thisTag_ = -1, thisReg_ = -1;
    /// The instruction lowered just before this one, in straight-line order; `Nop` after a label.
    Op previousOp_ = Op::Nop;
    std::vector<std::int32_t> localReg_, localTag_, cellReg_;
    std::vector<std::uint8_t> localOwned_, cellOwned_, assigned_, reachable_;
    std::vector<std::int32_t> labels_;
    std::vector<Join> joins_;
    std::vector<Entry> stack_;
    std::set<std::int32_t> nonNullLocals_;
    /// Parameters only ever given numbers, by slot.
    std::vector<std::uint8_t> numberParam_;
    /// Non-strict code whose `this` was made the global object on entry, when nullish: every read is the same register.
    bool thisNormalized_ = false;
    // ---- inlining ----
    /// The lowering this one is inlined into, what that one holds for the call, and where its result goes.
    const Lowering* parent_ = nullptr;
    const std::vector<Entry>* parentHeld_ = nullptr;
    std::int32_t parentSite_ = -1;
    std::int32_t inlineDone_ = -1, inlineR_ = -1, inlineT_ = -1;
    /// Inlined: the tag every return so far knows its value to have, -1 where they differ or one does not, -2 before any.
    std::int16_t returnTag_ = -2;
    /// What the last function inlined here had for that, once done.
    std::int16_t inlinedReturnTag_ = -1;
    void noteReturn(std::int16_t tag) noexcept { returnTag_ = returnTag_ == -2 || returnTag_ == tag ? tag : -1; }
    int depth_ = 0;
    /// Whether code may assume what the interpreter has seen so far, and hand the call back to it when that stops being so.
    bool speculate_ = false;
    /// Objects whose shape is known here, by the register holding them: a guard that dominates, with nothing since that
    /// could have changed a shape. Forgotten at joins and at anything that can run arbitrary code.
    struct ShapeFact
    {
        std::int32_t vreg;
        const Shape* shape;
    };
    std::vector<ShapeFact> shapeFacts_;
    /// `protoEpoch` is known to be what the code was compiled against.
    bool epochKnown_ = false;
    std::vector<std::uint8_t> loopHead_;
    std::vector<std::int32_t> knownArrays_;
    bool intWork_ = false;
    bool argsLazy_ = false;
    std::int32_t argsReg_ = -1;
    std::int32_t argcReg_ = -1;
    bool globalsScanned_ = false;
    std::vector<const GCObject*> assignedGlobals_;
    /// `this` has been checked to be a `DynObject`.
    bool thisIsCell_ = false;
    /// Inlined as a constructor: the shape `this` starts with.
    const Shape* startShape_ = nullptr;
    /// `this` is the object `constructInPlace` made, with room for so many properties, and nothing has seen it yet.
    bool freshThis_ = false;
    std::uint32_t freshRoom_ = 0;
};

/// Which locals may be read before anything was written to them, along some path. Those start as `undefined`, so they are no
/// candidates for holding only numbers.
std::vector<std::uint8_t> readsBeforeWrite(const FunctionCode& code)
{
    const std::vector<std::uint8_t>& c = code.code;
    const std::size_t slots = code.numLocals;
    std::vector<std::uint8_t> out(slots, 0);
    if (slots == 0)
        return out;
    // Must-analysis over the instruction graph: at each pc, the set of slots definitely written.
    std::vector<std::vector<std::uint8_t>> at(c.size() + 1);
    std::vector<std::uint32_t> work{0};
    at[0].assign(slots, 0);
    // A parameter has its argument, which is what entering the code checks.
    for (std::size_t slot = 0; slot < slots && slot < std::size_t(std::max(code.arity, 0)); ++slot)
        at[0][slot] = 1;
    while (!work.empty()) {
        std::uint32_t pc = work.back();
        work.pop_back();
        std::vector<std::uint8_t> state = at[pc];
        Op previous = Op::Nop;
        while (pc < c.size()) {
            const Op op = Op(c[pc]);
            const std::uint32_t next = pc + 1 + std::uint32_t(operandsOf(op)) * 4;
            if (op == Op::GetLocal) {
                const std::uint32_t slot = readOperand(c, pc + 1);
                if (slot < slots && !state[slot])
                    out[slot] = 1;
            } else if (op == Op::SetLocal) {
                // Storing `undefined` -- a hoisted `var` above all -- leaves the slot as good as unwritten: a read after it
                // sees what a read before any write would, and a number local skips the store.
                const std::uint32_t slot = readOperand(c, pc + 1);
                if (slot < slots)
                    state[slot] = previous == Op::PushUndefined ? 0 : 1;
            }
            previous = op;
            const auto flow = [&](std::uint32_t target) {
                std::vector<std::uint8_t>& known = at[target];
                if (known.empty()) {
                    known = state;
                    work.push_back(target);
                    return;
                }
                bool changed = false;
                for (std::size_t i = 0; i < slots; ++i) {
                    if (known[i] && !state[i]) {
                        known[i] = 0;
                        changed = true;
                    }
                }
                if (changed)
                    work.push_back(target);
            };
            if (op == Op::Return || op == Op::ReturnUndefined || op == Op::Throw)
                break;
            if (op == Op::Jump) {
                flow(readOperand(c, pc + 1));
                break;
            }
            if (isBranch(op))
                flow(readOperand(c, pc + 1));
            // Stay in the loop only while this instruction is the sole way to the next.
            if (next < c.size() && !at[next].empty()) {
                flow(next);
                break;
            }
            if (next < c.size())
                at[next] = state;
            pc = next;
        }
    }
    return out;
}

}  // namespace

bool lowerFunction(Runtime& rt, const FunctionCode& code, IrFunction& out, std::string* why)
{
    std::vector<std::uint8_t> notNumber = readsBeforeWrite(code);
    notNumber.resize(code.numLocals, 0);
    std::vector<std::uint8_t> notInt(code.numLocals, 0);
    for (;;) {
        IrFunction fn;
        try {
            Lowering lowering(rt, code, fn, notNumber, notInt);
            lowering.run();
            out = std::move(fn);
            return true;
        } catch (const LowerAgain& again) {
            if (again.onlyInt)
                notInt[again.slot] = 1;
            else
                notNumber[again.slot] = 1;
        } catch (const Refuse& refusal) {
            if (why)
                *why = refusal.why;
            return false;
        }
    }
}

}  // namespace dyn
}  // namespace internal
}  // namespace script
