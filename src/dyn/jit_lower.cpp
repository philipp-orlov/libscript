// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// From dynamic bytecode to IR.
//
// A value on the stack is, by what is known of it at this point: a double in a floating point register, an int32 in an integer
// register, or a tag and a payload in one register each. Where nothing is known, an operation tests for its usual case inline
// and calls a helper on a cold path otherwise. Reference counting follows the same rules as `src/typed/jit/lower.cpp`: an
// entry that reads a local or a field holds no count until something could change that place.
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

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
constexpr std::int64_t kPropsOffset = offsetof(DynObject, props);  // the first word of a vector is its data
constexpr std::int64_t kFrozenOffset = offsetof(DynObject, frozen);
constexpr std::int64_t kIsProtoOffset = offsetof(DynObject, isProto);
constexpr std::int64_t kExtensibleOffset = offsetof(DynObject, extensible);
constexpr std::int64_t kStringLengthOffset = offsetof(StringCell, length);
constexpr std::int64_t kStringCharsOffset = sizeof(StringCell);
constexpr std::int64_t kIndexOffset = offsetof(DynObject, index);
constexpr std::int64_t kPropSetterOffset = offsetof(Prop, setter);
constexpr std::int64_t kPropValueOffset = offsetof(Prop, value);
constexpr std::int64_t kPropKeyOffset = offsetof(Prop, key);
constexpr std::int64_t kPropAttrsOffset = offsetof(Prop, attrs);
constexpr std::int64_t kElementsOffset = offsetof(DynArray, elements);
constexpr std::int64_t kFnCodeOffset = offsetof(DynFunction, code);  // the first word of a shared_ptr is the pointer
constexpr std::int64_t kFnUpvaluesOffset = offsetof(DynFunction, upvalues);
constexpr std::int64_t kCallEntryOffset = offsetof(FunctionCode, jitCallEntry);
constexpr std::int64_t kBoxValueOffset = offsetof(Box, value);
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
constexpr std::int64_t kPayload = 8;

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
};

enum class Rep : std::uint8_t
{
    Double,
    Int32,
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
    std::int32_t r = -1;  ///< the double, the int, or the payload
    std::int32_t t = -1;  ///< the tag
    double number = 0;    ///< constant double or int
    std::uint64_t bits = 0;  ///< constant payload
    std::int32_t local = -1;
    /// The object this was when the code was compiled, when that is known: a hint, never relied on unguarded.
    const GCObject* hint = nullptr;
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
    Lowering(Runtime& rt, const FunctionCode& code, IrFunction& fn, const std::vector<std::uint8_t>& notNumber)
        : rt_(rt), code_(code), fn_(fn), notNumber_(notNumber)
    {}

    void run()
    {
        if (!code_.handlers.empty())
            refuse("the function has exception handlers");
        prepare();
        const std::uint32_t arity = std::uint32_t(std::max(code_.arity, 0));

        // fn, this, args, argc.
        fnReg_ = fn_.newVreg(false);
        thisTag_ = fn_.newVreg(false);
        thisReg_ = fn_.newVreg(false);
        const std::int32_t args = fn_.newVreg(false);
        const std::int32_t argc = fn_.newVreg(false);
        fn_.params = {fnReg_, thisTag_, thisReg_, args, argc};
        fn_.osrArguments = {fnReg_, thisTag_, thisReg_};

        const std::uint32_t slots = code_.numLocals;
        localReg_.assign(slots, -1);
        localTag_.assign(slots, -1);
        localOwned_.assign(slots, 0);
        cellReg_.assign(slots, -1);
        cellOwned_.assign(slots, 0);
        for (std::uint32_t slot = 0; slot < slots; ++slot) {
            if (isNumberLocal(slot)) {
                localReg_[slot] = fn_.newVreg(true);
                continue;
            }
            localReg_[slot] = fn_.newVreg(false);
            localTag_[slot] = fn_.newVreg(false);
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
        if (code_.argumentsSlot != FunctionCode::kNoSlot) {
            const std::int32_t rt = rtReg();
            const std::int32_t from = immReg(0);
            const std::int32_t r = callCold(reinterpret_cast<void*>(&scriptDynArgsArray), {rt, args, argc, from}, true);
            const std::uint32_t slot = code_.argumentsSlot;
            if (code_.argumentsBoxed) {
                const std::int32_t rt2 = rtReg();
                const std::int32_t tag = immReg(std::int64_t(Type::Object));
                const std::int32_t box = callCold(reinterpret_cast<void*>(&scriptDynNewBox), {rt2, tag, r}, true);
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
            const std::int32_t box = callCold(reinterpret_cast<void*>(&scriptDynNewBox),
                                              {rtReg(), localTag_[slot], localReg_[slot]}, true);
            IrInst& mov = inst(IrOp::Mov);
            mov.d = cellReg_[slot];
            mov.a = box;
        }

        walk();
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
    std::int32_t load(std::int32_t base, std::int64_t offset, std::uint8_t bytes, bool fp = false)
    {
        const std::int32_t d = fn_.newVreg(fp);
        IrInst& in = inst(IrOp::Load);
        in.d = d;
        in.a = base;
        in.imm = offset;
        in.width = bytes;
        return d;
    }
    void store(std::int32_t base, std::int64_t offset, std::int32_t value, std::uint8_t bytes)
    {
        IrInst& in = inst(IrOp::Store);
        in.a = base;
        in.c = value;
        in.imm = offset;
        in.width = bytes;
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
        if (!fn_.sites.empty() && fn_.sites.back().pc == pc_)
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

    bool isNumberLocal(std::uint32_t slot) const noexcept
    {
        if (slot < std::uint32_t(std::max(code_.arity, 0)) || notNumber_[slot])
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
    Entry intEntry(std::int32_t r)
    {
        Entry e;
        e.rep = Rep::Int32;
        e.r = r;
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
        if (e.rep == Rep::Int32) {
            const std::int32_t d = fn_.newVreg(true);
            IrInst& in = inst(IrOp::IntToFp);
            in.d = d;
            in.a = e.r;
            in.srcWidth = 32;
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

    std::int32_t toInt32Reg(const Entry& e)
    {
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
    void release(const Entry& e)
    {
        if (e.own != Own::Owned || e.isConst || e.rep != Rep::Tagged)
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
                const std::int32_t d = fn_.newVreg(true);
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
        fn_.maxCleanup = std::max(fn_.maxCleanup, std::uint32_t(exit.cleanup.size() + exit.cleanupTagged.size()));
        fn_.exits.push_back(std::move(exit));
        return std::int32_t(fn_.exits.size()) - 1;
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
            join.entries.resize(stack_.size());
            for (Entry& canonical : join.entries) {
                canonical = Entry();
                canonical.r = fn_.newVreg(false);
                canonical.t = fn_.newVreg(false);
                canonical.own = Own::Owned;
            }
        } else if (join.entries.size() != stack_.size()) {
            refuse("the evaluation stack is inconsistent at a join");
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
            e = canonical;
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
    }

    // ---- the walk ----
    void walk()
    {
        const std::vector<std::uint8_t>& c = code_.code;
        std::vector<std::uint8_t> loopHead(c.size() + 1, 0);
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
                enterJoin(pc, fallsIn);
                if (loopHead[pc] && stack_.empty())
                    addOsrEntry(pc);
            }
            fallsIn = true;
            // A comparison of numbers followed by a branch on it fuses.
            if ((op == Op::Lt || op == Op::Le || op == Op::Gt || op == Op::Ge) && next < c.size() && labels_[next] < 0 &&
                stack_.size() == 2 && isKnownNumber(top(0)) && isKnownNumber(top(1))) {
                const Op after = Op(c[next]);
                if (after == Op::JumpIfFalse || after == Op::JumpIfTrue) {
                    fusedCompare(op, after == Op::JumpIfTrue, readOperand(c, next + 1), pc);
                    previousOp_ = Op::Nop;
                    pc = next + 5;
                    continue;
                }
            }
            if (!lowerOne(op, pc))
                fallsIn = false;
            previousOp_ = op;
            pc = next;
        }
    }

    void addOsrEntry(std::uint32_t pc)
    {
        IrOsrEntry entry;
        entry.bytecodePc = pc;
        entry.label = labels_[pc];
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
        const Entry b = pop();
        const Entry a = pop();
        if (target <= pc)
            poll();
        if (a.rep == Rep::Int32 && b.rep == Rep::Int32 && !a.isConst) {
            const IrCond intCond = op == Op::Lt ? IrCond::Lt : op == Op::Le ? IrCond::Le : op == Op::Gt ? IrCond::Gt : IrCond::Ge;
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
        if (whenTrue) {
            IrInst& br = inst(IrOp::BrFCmp);
            br.a = ra;
            br.b = rb;
            br.cond = fpCond(op);
            br.width = 64;
            br.target = labels_[target];
        } else {
            // A NaN answers false to all of these, so the false sense has to jump around.
            const std::int32_t skip = newLabel();
            IrInst& br = inst(IrOp::BrFCmp);
            br.a = ra;
            br.b = rb;
            br.cond = fpCond(op);
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
        IrInst& mov = inst(IrOp::Mov);
        mov.d = d;
        mov.a = e.r;
        branchCmp(tag, IrCond::Ne, std::int64_t(Type::Bool), slow);
        inst(IrOp::ColdBegin);
        inst(IrOp::Label).imm = slow;
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
                if (code_.sloppyThis) {
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
                push(taggedEntry(thisReg_, thisTag_, Own::Immortal));
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
                if (isNumberLocal(slot)) {
                    Entry e = doubleEntry(localReg_[slot]);
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
                const std::int32_t rt = rtReg();
                const std::int32_t box = callCold(reinterpret_cast<void*>(&scriptDynNewBox), {rt, tag, payload}, true);
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
                const std::int32_t data = load(fnReg_, kFnUpvaluesOffset, 8);
                const std::int32_t box = load(data, std::int64_t(operand()) * 8, 8);
                readBox(box);
                return true;
            }
            case Op::SetUpval: {
                const std::int32_t data = load(fnReg_, kFnUpvaluesOffset, 8);
                const std::int32_t box = load(data, std::int64_t(operand()) * 8, 8);
                writeBox(box);
                return true;
            }
            case Op::GetGlobal: getGlobal(code_.atoms[operand()].get()); return true;
            case Op::SetGlobal: {
                Entry value = pop();
                GCObject* key = code_.atoms[operand()].get();
                secureWeak();
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
            case Op::GetElem: getElem(); return true;
            case Op::SetElem: setElem(); return true;
            case Op::DefField: {
                Entry value = pop();
                const Entry& object = top();
                GCObject* key = code_.atoms[operand()].get();
                secureWeak();
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
                const std::int32_t rt = rtReg();
                const std::int32_t room = immReg(std::int64_t(operand()));
                const std::int32_t r = callCold(reinterpret_cast<void*>(&scriptDynNewObject), {rt, room}, true);
                push(objectEntry(r));
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
                Entry value = pop();
                secureWeak();
                const std::int32_t rt = rtReg();
                const std::int32_t tag = tagReg(value);
                const std::int32_t payload = payloadReg(value);
                coldCall(reinterpret_cast<void*>(&scriptDynArrayAppend), {rt, top().r, tag, payload}, -1, -1, true, {value});
                release(value);
                return true;
            }
            case Op::GetElemDup: {
                // base key -> value base
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
                secureWeak();
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
                secureWeak();
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
                secureWeak();
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
                secureWeak();
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
        if (isNumberLocal(slot)) {
            // Nothing reads the slot before a number is written to it: see `readsBeforeWrite`.
            if (previousOp_ == Op::PushUndefined && value.isConst && value.rep == Rep::Tagged && value.hasKnownTag &&
                value.known == Type::Undefined)
                return;
            if (!isKnownNumber(value))
                throw LowerAgain{slot};
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
        // A cell that was never made reads as undefined.
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
        const std::int32_t tag = tagReg(value);
        const std::int32_t payload = payloadReg(value);
        const std::int32_t skip = newLabel();
        // A cell that was never made drops the write, as in the interpreter. The helper takes a count of its own.
        branchCmp(box, IrCond::Eq, 0, skip, 64);
        coldCall(reinterpret_cast<void*>(&scriptDynStoreBox), {box, tag, payload}, -1, -1, false);
        inst(IrOp::Label).imm = skip;
        release(value);
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
        // Where the property is now, checked each time: the table only changes when a global is deleted or added.
        if (found && !found->accessor()) {
            const std::size_t index = std::size_t(found - global->props.data());
            const std::int64_t at = std::int64_t(index) * std::int64_t(sizeof(Prop));
            const std::int32_t base = ptrReg(global);
            const std::int32_t props = load(base, kPropsOffset, 8);
            const std::int32_t size = load(base, kPropsOffset + 8, 8);
            const std::int32_t count = fn_.newVreg(false);
            IrInst& sub = inst(IrOp::Sub);
            sub.d = count;
            sub.a = size;
            sub.b = props;
            sub.width = 64;
            branchCmp(count, IrCond::Ls, at, slow, 64);
            const std::int32_t k = load(props, at + kPropKeyOffset, 8);
            const std::int32_t expected = ptrReg(key);
            IrInst& test = inst(IrOp::BrCmp);
            test.a = k;
            test.b = expected;
            test.cond = IrCond::Ne;
            test.width = 64;
            test.target = slow;
            const std::int32_t attrs = load(props, at + kPropAttrsOffset, 1);
            IrInst& mask = inst(IrOp::And);
            const std::int32_t flag = fn_.newVreg(false);
            mask.d = flag;
            mask.a = attrs;
            mask.hasImm = true;
            mask.imm = PropAccessor;
            mask.width = 32;
            branchCmp(flag, IrCond::Ne, 0, slow);
            const std::int32_t lt = load(props, at + kPropValueOffset, 1);
            const std::int32_t lr = load(props, at + kPropValueOffset + kPayload, 8);
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
        if (found && !found->accessor() && found->value.isObject())
            result.hint = detail::Access::ptr(found->value);
        push(result);
    }

    /// Guards that the value is a `DynObject` with this shape: anything else goes to `slow`.
    /// The shape of a value that has to be a `DynObject`: anything else goes to `slow`.
    std::int32_t shapeOf(const Entry& base, std::int32_t slow)
    {
        if (!base.hasKnownTag)
            branchCmp(base.t, IrCond::Ne, std::int64_t(Type::Object), slow);
        else if (base.known != Type::Object)
            inst(IrOp::Br).imm = slow;
        const std::int32_t kind = load(base.r, kCellKindOffset, 1);
        branchCmp(kind, IrCond::Ne, std::int64_t(CellKind::Object), slow);
        return load(base.r, kShapeOffset, 8);
    }

    /// Branches to one label per shape the site knows a plain own property for, or to `slow`; gives each label's index.
    std::vector<std::pair<std::int32_t, std::uint32_t>> dispatchShapes(const Entry& base, const PropertySite* site, std::int32_t slow)
    {
        const std::int32_t have = shapeOf(base, slow);
        std::vector<std::pair<std::int32_t, std::uint32_t>> cases;
        cases.push_back({newLabel(), site->index});
        for (std::uint8_t i = 0; i < site->moreCount; ++i)
            cases.push_back({newLabel(), site->moreIndex[i]});
        for (std::size_t i = 0; i < cases.size(); ++i) {
            const std::int32_t want = ptrReg(i == 0 ? site->shape : site->moreShape[i - 1]);
            IrInst& test = inst(IrOp::BrCmp);
            test.a = have;
            test.b = want;
            test.cond = IrCond::Eq;
            test.width = 64;
            test.target = cases[i].first;
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
        secureWeak();
        // The receiver stays on the stack past the next slow path, whose result is what a weak one might be reading.
        if (keepBase && base.own == Own::Weak)
            makeOwned(base);
        // The operand could be a primitive with no tag register to compare: bits and a tag it is, here on.
        if (base.rep != Rep::Tagged || base.isConst) {
            const std::int32_t tag = tagReg(base);
            const std::int32_t payload = payloadReg(base);
            Own own = base.own;
            base = taggedEntry(payload, tag, own == Own::None ? Own::None : own);
        }
        const PropertySite* site = siteAt(operand);
        const std::int32_t r = fn_.newVreg(false);
        const std::int32_t t = fn_.newVreg(false);
        const std::int32_t slow = newLabel();
        const std::int32_t done = newLabel();
        const bool usable = site && (!site->holder || site->epoch == rt_.ctx.protoEpoch);
        if (usable && site->getter) {
            getterProp(base, site, operand, keepBase);
            return;
        }
        if (usable && site->moreCount) {
            for (const auto& [label, index] : dispatchShapes(base, site, slow)) {
                inst(IrOp::Label).imm = label;
                const std::int64_t at = std::int64_t(index) * std::int64_t(sizeof(Prop));
                const std::int32_t props = load(base.r, kPropsOffset, 8);
                const std::int32_t lt = load(props, at + kPropValueOffset, 1);
                const std::int32_t lr = load(props, at + kPropValueOffset + kPayload, 8);
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
            const std::int32_t props = load(holder, kPropsOffset, 8);
            const std::int32_t lt = load(props, at + kPropValueOffset, 1);
            const std::int32_t lr = load(props, at + kPropValueOffset + kPayload, 8);
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
        const std::int32_t self = ptrReg(&code_);
        const std::int32_t op = immReg(std::int64_t(operand));
        coldCall(reinterpret_cast<void*>(&scriptDynGetProp), {rt, self, tagReg(base), base.r, op}, r, t, true, {base});
        inst(IrOp::Br).imm = done;
        inst(IrOp::ColdEnd);
        inst(IrOp::Label).imm = done;
        Entry result = taggedEntry(r, t, Own::Weak);
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
        }
        if (usable && !site->holder && !site->moreCount && base.hint) {
            const auto* known = static_cast<const DynObject*>(base.hint);
            if (known->kind == CellKind::Object && known->shape == site->shape && site->index < known->props.size() &&
                known->props[site->index].value.isObject())
                result.hint = detail::Access::ptr(known->props[site->index].value);
        }
        if (keepBase) {
            // func obj: the function under the receiver.
            push(result);
            push(base);
            return;
        }
        if (base.own == Own::Owned) {
            makeOwned(result);
            push(result);
            release(base);
        } else {
            push(result);
        }
    }

    void setProp(std::uint32_t operand)
    {
        Entry value = pop();
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
        const PropertySite* site = siteAt(operand);
        const std::int32_t slow = newLabel();
        const std::int32_t done = newLabel();
        if (site && !site->addTo && site->moreCount) {
            for (const auto& [label, index] : dispatchShapes(base, site, slow)) {
                inst(IrOp::Label).imm = label;
                const std::int64_t at = std::int64_t(index) * std::int64_t(sizeof(Prop));
                const std::int32_t props = load(base.r, kPropsOffset, 8);
                const std::int32_t oldTag = load(props, at + kPropValueOffset, 1);
                const std::int32_t old = load(props, at + kPropValueOffset + kPayload, 8);
                store(props, at + kPropValueOffset, vt, 1);
                store(props, at + kPropValueOffset + kPayload, vr, 8);
                decRefOf(old, oldTag, false);
                inst(IrOp::Br).imm = done;
            }
        } else if (site && site->addTo && site->epoch == rt_.ctx.protoEpoch && site->addTo->count <= DynObject::kIndexThreshold) {
            // A store that adds the property, in place while the table has room and no index: what `appendKnown` does.
            guardShape(base, site->shape, slow);
            guardEpoch(site->epoch, slow);
            const std::int32_t isProto = load(base.r, kIsProtoOffset, 1);
            branchCmp(isProto, IrCond::Ne, 0, slow);
            const std::int32_t indexBegin = load(base.r, kIndexOffset, 8);
            const std::int32_t indexEnd = load(base.r, kIndexOffset + 8, 8);
            {
                IrInst& test = inst(IrOp::BrCmp);
                test.a = indexBegin;
                test.b = indexEnd;
                test.cond = IrCond::Ne;
                test.width = 64;
                test.target = slow;
            }
            const std::int32_t end = load(base.r, kPropsOffset + 8, 8);
            const std::int32_t cap = load(base.r, kPropsOffset + 16, 8);
            {
                IrInst& test = inst(IrOp::BrCmp);
                test.a = end;
                test.b = cap;
                test.cond = IrCond::Eq;
                test.width = 64;
                test.target = slow;
            }
            const std::int32_t key = ptrReg(code_.atoms[operand & kAtomMask].get());
            incRefOf(key, -1, false);
            store(end, kPropKeyOffset, key, 8);
            store(end, kPropValueOffset, vt, 1);
            store(end, kPropValueOffset + kPayload, vr, 8);
            const std::int32_t undefinedTag = immReg(std::int64_t(Type::Undefined));
            const std::int32_t zero = immReg(0);
            store(end, kPropSetterOffset, undefinedTag, 1);
            store(end, kPropSetterOffset + kPayload, zero, 8);
            store(end, kPropAttrsOffset, immReg(PropDefault), 1);
            const std::int32_t next = fn_.newVreg(false);
            IrInst& add = inst(IrOp::Add);
            add.d = next;
            add.a = end;
            add.hasImm = true;
            add.imm = std::int64_t(sizeof(Prop));
            add.width = 64;
            store(base.r, kPropsOffset + 8, next, 8);
            store(base.r, kShapeOffset, ptrReg(site->addTo), 8);
        } else if (site && !site->addTo) {
            // A write to a property the shape says is there and writable: what was there loses its count after.
            guardShape(base, site->shape, slow);
            const std::int64_t at = std::int64_t(site->index) * std::int64_t(sizeof(Prop));
            const std::int32_t props = load(base.r, kPropsOffset, 8);
            const std::int32_t oldTag = load(props, at + kPropValueOffset, 1);
            const std::int32_t old = load(props, at + kPropValueOffset + kPayload, 8);
            store(props, at + kPropValueOffset, vt, 1);
            store(props, at + kPropValueOffset + kPayload, vr, 8);
            decRefOf(old, oldTag, false);
        } else {
            inst(IrOp::Br).imm = slow;
        }
        inst(IrOp::ColdBegin);
        inst(IrOp::Label).imm = slow;
        {
            const std::int32_t rt = rtReg();
            const std::int32_t self = ptrReg(&code_);
            const std::int32_t op = immReg(std::int64_t(operand));
            coldCall(reinterpret_cast<void*>(&scriptDynSetProp), {rt, self, tagReg(base), base.r, op, vt, vr}, -1, -1, true,
                     {base, value});
            // The helper took its own count, so the one made above goes.
            if (value.own == Own::Owned && !value.isConst)
                decRefOf(vr, value.hasKnownTag ? -1 : vt, false);
        }
        inst(IrOp::Br).imm = done;
        inst(IrOp::ColdEnd);
        inst(IrOp::Label).imm = done;
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

    void getElem()
    {
        Entry key = pop();
        Entry base = pop();
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
        guardArray(base, slow);
        const std::int32_t i = arrayIndex(key, slow);
        const std::int32_t data = load(base.r, kElementsOffset, 8);
        const std::int32_t end = load(base.r, kElementsOffset + 8, 8);
        const std::int32_t size = fn_.newVreg(false);
        IrInst& sub = inst(IrOp::Sub);
        sub.d = size;
        sub.a = end;
        sub.b = data;
        sub.width = 64;
        const std::int32_t scaled = fn_.newVreg(false);
        IrInst& shl = inst(IrOp::Shl);
        shl.d = scaled;
        shl.a = i;
        shl.hasImm = true;
        shl.imm = 4;
        shl.width = 64;
        IrInst& bound = inst(IrOp::BrCmp);
        bound.a = scaled;
        bound.b = size;
        bound.cond = IrCond::Hs;
        bound.width = 64;
        bound.target = slow;
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
        inst(IrOp::ColdBegin);
        inst(IrOp::Label).imm = slow;
        const std::int32_t rt = rtReg();
        coldCall(reinterpret_cast<void*>(&scriptDynGetElem), {rt, tagReg(base), base.r, tagReg(key), payloadReg(key)}, r, t, true,
                 {base, key});
        inst(IrOp::Br).imm = done;
        inst(IrOp::ColdEnd);
        inst(IrOp::Label).imm = done;
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

    void guardArray(const Entry& base, std::int32_t slow)
    {
        if (!base.hasKnownTag)
            branchCmp(base.t, IrCond::Ne, std::int64_t(Type::Object), slow);
        else if (base.known != Type::Object)
            inst(IrOp::Br).imm = slow;
        const std::int32_t kind = load(base.r, kCellKindOffset, 1);
        branchCmp(kind, IrCond::Ne, std::int64_t(CellKind::Object), slow);
        const std::int32_t cls = load(base.r, kClsOffset, 1);
        branchCmp(cls, IrCond::Ne, std::int64_t(ObjClass::Array), slow);
    }

    void setElem()
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
        guardArray(base, slow);
        // Frozen, or with named properties that might include an index: the long way.
        const std::int32_t frozen = load(base.r, kFrozenOffset, 1);
        branchCmp(frozen, IrCond::Ne, 0, slow);
        const std::int32_t propsBegin = load(base.r, kPropsOffset, 8);
        const std::int32_t propsEnd = load(base.r, kPropsOffset + 8, 8);
        IrInst& noProps = inst(IrOp::BrCmp);
        noProps.a = propsBegin;
        noProps.b = propsEnd;
        noProps.cond = IrCond::Ne;
        noProps.width = 64;
        noProps.target = slow;
        const std::int32_t i = arrayIndex(key, slow);
        const std::int32_t data = load(base.r, kElementsOffset, 8);
        const std::int32_t end = load(base.r, kElementsOffset + 8, 8);
        const std::int32_t size = fn_.newVreg(false);
        IrInst& sub = inst(IrOp::Sub);
        sub.d = size;
        sub.a = end;
        sub.b = data;
        sub.width = 64;
        const std::int32_t scaled = fn_.newVreg(false);
        IrInst& shl = inst(IrOp::Shl);
        shl.d = scaled;
        shl.a = i;
        shl.hasImm = true;
        shl.imm = 4;
        shl.width = 64;
        IrInst& bound = inst(IrOp::BrCmp);
        bound.a = scaled;
        bound.b = size;
        bound.cond = IrCond::Hs;
        bound.width = 64;
        bound.target = slow;
        const std::int32_t slot = fn_.newVreg(false);
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
        inst(IrOp::ColdBegin);
        inst(IrOp::Label).imm = slow;
        {
            const std::int32_t rt = rtReg();
            coldCall(reinterpret_cast<void*>(&scriptDynSetElem), {rt, tagReg(base), base.r, tagReg(key), payloadReg(key), vt, vr}, -1,
                     -1, true, {base, key, value});
            if (value.own == Own::Owned && !value.isConst)
                decRefOf(vr, value.hasKnownTag ? -1 : vt, false);
        }
        inst(IrOp::Br).imm = done;
        inst(IrOp::ColdEnd);
        inst(IrOp::Label).imm = done;
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
                const std::int32_t data = load(fnReg_, kFnUpvaluesOffset, 8);
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
        secureWeak();
        securePopped(args);
        if (self.own == Own::Weak)
            makeOwned(self);
        if (callee.own == Own::Weak)
            makeOwned(callee);

        std::vector<Entry> held = args;
        held.push_back(self);
        held.push_back(callee);

        const std::int32_t r = fn_.newVreg(false);
        const std::int32_t t = fn_.newVreg(false);
        const std::int32_t generic = newLabel();
        const std::int32_t finished = newLabel();
        const bool inlined = intrinsic(callee, self, args, r, t, generic);
        if (inlined) {
            inst(IrOp::Br).imm = finished;
            inst(IrOp::Label).imm = generic;
        }
        const std::int32_t area = layOutArgs(args);
        const std::int32_t selfTag = tagReg(self);
        const std::int32_t selfPayload = payloadReg(self);
        const std::int32_t fnTag = tagReg(callee);
        const std::int32_t fnPayload = payloadReg(callee);
        const std::int32_t count = immReg(std::int64_t(argc));
        invoke(fnTag, fnPayload, selfTag, selfPayload, area, count, r, t, held);
        if (inlined)
            inst(IrOp::Label).imm = finished;
        for (const Entry& arg : args)
            release(arg);
        release(self);
        release(callee);
        push(taggedEntry(r, t, Own::Owned));
    }

    /// Calls a function value: straight into its native code when it has some, through the runtime otherwise.
    void invoke(std::int32_t fnTag, std::int32_t fnPayload, std::int32_t selfTag, std::int32_t selfPayload, std::int32_t area,
                std::int32_t count, std::int32_t r, std::int32_t t, const std::vector<Entry>& held)
    {
        const std::int32_t slow = newLabel();
        const std::int32_t done = newLabel();
        // A function compiled to native code is called directly; anything else takes the long way.
        branchCmp(fnTag, IrCond::Ne, std::int64_t(Type::Object), slow);
        const std::int32_t kind = load(fnPayload, kCellKindOffset, 1);
        branchCmp(kind, IrCond::Ne, std::int64_t(CellKind::Object), slow);
        const std::int32_t cls = load(fnPayload, kClsOffset, 1);
        branchCmp(cls, IrCond::Ne, std::int64_t(ObjClass::Function), slow);
        const std::int32_t codePtr = load(fnPayload, kFnCodeOffset, 8);
        branchCmp(codePtr, IrCond::Eq, 0, slow, 64);
        const std::int32_t entry = load(codePtr, kCallEntryOffset, 8);
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
                          (which == Runtime::StringFromCharCode && argc == 1);
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
            case Runtime::ArrayPush: inlinePush(self, args[0], r, t, generic); return true;
            case Runtime::ArrayPop: inlinePop(self, r, t, generic); return true;
            case Runtime::StringCharCodeAt: inlineCharCodeAt(self, args[0], r, t, generic); return true;
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
        const std::int32_t extensible = load(self.r, kExtensibleOffset, 1);
        branchCmp(extensible, IrCond::Eq, 0, generic);
        const std::int32_t frozen = load(self.r, kFrozenOffset, 1);
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

    /// The new length, as a double, from the byte span of the elements.
    void lengthResult(std::int32_t bytes, std::int32_t r, std::int32_t t)
    {
        const std::int32_t count = fn_.newVreg(false);
        IrInst& shr = inst(IrOp::Shr);
        shr.d = count;
        shr.a = bytes;
        shr.hasImm = true;
        shr.imm = 4;
        shr.width = 64;
        const std::int32_t d = fn_.newVreg(true);
        IrInst& conv = inst(IrOp::IntToFp);
        conv.d = d;
        conv.a = count;
        conv.srcWidth = 64;
        conv.width = 64;
        setDoubleResult(d, r, t);
    }

    /// `array.push(v)` while the buffer has room: the value's count goes to the array.
    void inlinePush(const Entry& self, const Entry& value, std::int32_t r, std::int32_t t, std::int32_t generic)
    {
        guardGrowableArray(self, generic);
        const std::int32_t begin = load(self.r, kElementsOffset, 8);
        const std::int32_t end = load(self.r, kElementsOffset + 8, 8);
        const std::int32_t cap = load(self.r, kElementsOffset + 16, 8);
        {
            IrInst& full = inst(IrOp::BrCmp);
            full.a = end;
            full.b = cap;
            full.cond = IrCond::Eq;
            full.width = 64;
            full.target = generic;
        }
        // The arguments are owned by now, but the generic path releases them after the call; this one keeps a count.
        const std::int32_t vt = tagReg(value);
        const std::int32_t vr = payloadReg(value);
        if (!value.hasKnownTag)
            incRefOf(vr, vt, false);
        else if (value.known >= Type::String)
            incRefOf(vr, -1, false);
        store(end, 0, vt, 1);
        store(end, kPayload, vr, 8);
        const std::int32_t next = fn_.newVreg(false);
        IrInst& add = inst(IrOp::Add);
        add.d = next;
        add.a = end;
        add.hasImm = true;
        add.imm = std::int64_t(sizeof(Value));
        add.width = 64;
        store(self.r, kElementsOffset + 8, next, 8);
        lengthResult(subtract(next, begin), r, t);
    }

    /// `array.pop()` on an array that is not empty: the element's count comes along with it.
    void inlinePop(const Entry& self, std::int32_t r, std::int32_t t, std::int32_t generic)
    {
        guardGrowableArray(self, generic);
        const std::int32_t begin = load(self.r, kElementsOffset, 8);
        const std::int32_t end = load(self.r, kElementsOffset + 8, 8);
        {
            IrInst& empty = inst(IrOp::BrCmp);
            empty.a = end;
            empty.b = begin;
            empty.cond = IrCond::Eq;
            empty.width = 64;
            empty.target = generic;
        }
        const std::int32_t last = fn_.newVreg(false);
        IrInst& sub = inst(IrOp::Sub);
        sub.d = last;
        sub.a = end;
        sub.hasImm = true;
        sub.imm = std::int64_t(sizeof(Value));
        sub.width = 64;
        const std::int32_t lt = load(last, 0, 1);
        const std::int32_t lr = load(last, kPayload, 8);
        store(self.r, kElementsOffset + 8, last, 8);
        IrInst& mt = inst(IrOp::Mov);
        mt.d = t;
        mt.a = lt;
        IrInst& mr = inst(IrOp::Mov);
        mr.d = r;
        mr.a = lr;
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
        const std::int32_t length = load(cell, kStringLengthOffset, 4);
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
        const std::int32_t props = load(ptrReg(site->holder), kPropsOffset, 8);
        const std::int32_t ft = load(props, at + kPropValueOffset, 1);
        const std::int32_t fr = load(props, at + kPropValueOffset + kPayload, 8);
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
        const std::int32_t kind = load(fnPayload, kCellKindOffset, 1);
        branchCmp(kind, IrCond::Ne, std::int64_t(CellKind::Object), slow);
        const std::int32_t cls = load(fnPayload, kClsOffset, 1);
        branchCmp(cls, IrCond::Ne, std::int64_t(ObjClass::Function), slow);
        const std::int32_t codePtr = load(fnPayload, kFnCodeOffset, 8);
        branchCmp(codePtr, IrCond::Eq, 0, slow, 64);
        const std::int32_t entry = load(codePtr, kCallEntryOffset, 8);
        branchCmp(entry, IrCond::Eq, 0, slow, 64);
        const std::int32_t self = fn_.newVreg(false);
        coldCall(reinterpret_cast<void*>(&scriptDynNewThis), {rt, fnPayload}, self, -1, true, held);
        branchCmp(self, IrCond::Eq, 0, slow, 64);
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

    void arithmetic(Op op)
    {
        const Entry b = pop();
        const Entry a = pop();
        secureWeak();
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
        secureWeak();
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
        branchCmp(wa, IrCond::Lt, 0, slow);
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
        secureWeak();
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
        if (op == Op::UShr) {
            // A uint32, which only a double holds in general.
            const std::int32_t fp = fn_.newVreg(true);
            IrInst& to = inst(IrOp::IntToFp);
            to.d = fp;
            to.a = d;
            to.srcWidth = 32;
            to.width = 64;
            to.isUnsigned = true;
            push(doubleEntry(fp));
            return;
        }
        push(intEntry(d));
    }

    void relational(Op op)
    {
        const Entry b = pop();
        const Entry a = pop();
        secureWeak();
        const std::int32_t d = fn_.newVreg(false);
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

    void equality(Op op)
    {
        const Entry b = pop();
        const Entry a = pop();
        secureWeak();
        const bool strict = op == Op::StrictEq || op == Op::StrictNe;
        const bool wantEqual = op == Op::StrictEq || op == Op::Eq;
        std::int32_t d = fn_.newVreg(false);
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
        if (strict) {
            coldCall(reinterpret_cast<void*>(&scriptDynStrictEquals), {ta, pa, tb, pb}, d, -1, false);
        } else {
            const std::int32_t rt = rtReg();
            coldCall(reinterpret_cast<void*>(&scriptDynLooseEquals), {rt, ta, pa, tb, pb}, d, -1, true, {a, b});
        }
        if (!wantEqual) {
            const std::int32_t inverted = fn_.newVreg(false);
            IrInst& in = inst(IrOp::Xor);
            in.d = inverted;
            in.a = d;
            in.hasImm = true;
            in.imm = 1;
            in.width = 32;
            d = inverted;
        }
        release(b);
        release(a);
        push(boolEntry(d));
    }

    struct Join
    {
        bool seen = false;
        std::vector<Entry> entries;
    };

    Runtime& rt_;
    const FunctionCode& code_;
    IrFunction& fn_;
    const std::vector<std::uint8_t>& notNumber_;
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
    for (;;) {
        IrFunction fn;
        try {
            Lowering lowering(rt, code, fn, notNumber);
            lowering.run();
            out = std::move(fn);
            return true;
        } catch (const LowerAgain& again) {
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
