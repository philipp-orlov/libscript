// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// From bytecode to IR.
//
// This pass runs the evaluation stack at compile time: an entry is a virtual register, a constant, or a local being read
// without a copy. Compare-and-branch pairs fuse here, constants fold here, small methods inline here, and most of the
// reference counting the bytecode implies disappears here. Each bytecode instruction is handled once, in order.
//
// Ownership of a reference on the virtual stack is one of:
//
//   Owned      the entry holds a count, and whoever consumes it takes that over or releases it;
//   Local      a read of a local that is not being copied: valid until that local is written;
//   Weak       read out of a field or an array element: valid until anything runs that could overwrite that place, so it
//              is turned into Owned before any call, any reference store and any release;
//   Immortal   a string literal, which the assembly keeps alive.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <map>
#include <set>
#include <string>
#include <unordered_map>

#include "script/internal/typed/bytecode.hpp"
#include "script/internal/typed/jit.hpp"
#include "script/internal/typed/jit_ir.hpp"
#include "script/internal/typed/jit_runtime.hpp"
#include "script/internal/typed/runtime.hpp"
#include "script/internal/typed/types.hpp"
#include "script/internal/typed/vm.hpp"

namespace script {
namespace internal {
namespace typed {

namespace {

constexpr std::size_t kInlineMaxBytes = 96;   ///< bytecode size a method may have to be inlined
constexpr int kInlineMaxDepth = 4;
constexpr std::size_t kInlineBudget = 1024;   ///< total bytecode inlined into one method

/// Thrown out of lowering to refuse a method. Refusal is normal: the method keeps interpreting.
struct Refuse
{
    std::string why;
};

[[noreturn]] void refuse(std::string why)
{
    throw Refuse{std::move(why)};
}

enum class Own : std::uint8_t
{
    None,      ///< not a reference
    Owned,
    Local,
    Weak,
    Immortal,
};

struct Entry
{
    std::int32_t vreg = -1;
    std::int32_t tagVreg = -1;  ///< `object` and interfaces: the tag
    const TypeInfo* type = nullptr;
    Own own = Own::None;
    bool isConst = false;
    bool nonNull = false;
    std::int64_t imm = 0;     ///< when isConst: the integer, or the bits of the float
    std::int32_t local = -1;  ///< the local this reads, while it has not been copied
};

enum class Cls : std::uint8_t
{
    I32,
    I64,
    F32,
    F64,
    Ref,
    Void,
    /// `object` or an interface: anything at all, so a tag travels with the payload, in a register of its own.
    Obj,
};

TypeKind baseKind(const TypeInfo* type) noexcept
{
    if (!type)
        return TypeKind::Object;
    if (type->kind == TypeKind::Enum)
        return type->underlying ? type->underlying->kind : TypeKind::Int;
    return type->kind;
}

Cls classOf(const TypeInfo* type)
{
    switch (baseKind(type)) {
        case TypeKind::Void: return Cls::Void;
        case TypeKind::Bool:
        case TypeKind::SByte:
        case TypeKind::Byte:
        case TypeKind::Short:
        case TypeKind::UShort:
        case TypeKind::Int:
        case TypeKind::UInt:
        case TypeKind::Char: return Cls::I32;
        case TypeKind::Long:
        case TypeKind::ULong: return Cls::I64;
        case TypeKind::Float: return Cls::F32;
        case TypeKind::Double: return Cls::F64;
        case TypeKind::String:
        case TypeKind::Struct:
        case TypeKind::Class:
        case TypeKind::Delegate:
        case TypeKind::Array:
        case TypeKind::Null: return Cls::Ref;
        case TypeKind::Object:
        case TypeKind::Interface: return Cls::Obj;
        default: refuse("a value of a type that has no representation");
    }
}

/// The tag a non-null reference of this static type carries. `Object` also stands for "ask the cell": an interface
/// value can be any kind of cell.
Type refTag(const TypeInfo* type) noexcept
{
    switch (baseKind(type)) {
        case TypeKind::String: return Type::String;
        case TypeKind::Array: return Type::Array;
        case TypeKind::Delegate: return Type::Delegate;
        default: return Type::Object;
    }
}


/// One method being lowered: the root, or one inlined into it.
struct Scope
{
    const MethodInfo* method = nullptr;
    Scope* parent = nullptr;
    std::int32_t parentSite = -1;
    std::uint32_t pc = 0;
    int depth = 0;

    std::vector<std::int32_t> localVreg;   ///< by slot: the value, or for a boxed slot the box
    std::vector<std::int32_t> localTag;    ///< by slot, for `object`: its tag
    std::vector<std::uint8_t> localOwned;  ///< holds a count
    std::vector<std::uint8_t> localConstKnown;
    std::vector<std::int64_t> localConst;
    std::vector<std::uint8_t> localNonNull;
    std::vector<const TypeInfo*> localType;

    std::vector<Entry> stack;
    /// Arguments an inlined call holds counts on for its duration.
    std::vector<std::int32_t> held;
    std::vector<std::pair<std::int32_t, std::int32_t>> heldTagged;

    std::vector<std::int32_t> labels;   ///< by bytecode offset, -1 for no label
    struct Join
    {
        bool seen = false;
        std::vector<Entry> entries;
    };
    std::vector<Join> joins;

    /// An inlined constructor's `this` is a cell made just now, in which no field has been written: storing to one has
    /// nothing to release. Until a label or a call, after which anything might have happened.
    std::vector<std::uint8_t> freshField;
    bool thisIsFresh = false;
    /// By parameter: the caller holds a count on the argument just for this call, and the body reads the parameter once
    /// and has no branches. Storing it can then take the count over instead of adding one and dropping the other.
    std::vector<std::uint8_t> consumable;
    std::vector<std::uint8_t> consumed;
    std::vector<std::int32_t> argHeldVreg;

    std::int32_t exitLabel = -1;   ///< inlined: where a return goes
    std::int32_t resultVreg = -1;  ///< inlined: where a return value goes
    std::int32_t resultTag = -1;
    bool resultNonNull = false;
};

bool bodyHasLoop(const std::vector<std::uint8_t>& code)
{
    for (std::size_t pc = 0; pc < code.size();) {
        const Op op = Op(code[pc]);
        if ((op == Op::Br || op == Op::BrTrue || op == Op::BrFalse) && readU32(&code[pc + 1]) <= pc)
            return true;
        pc += 1 + std::size_t(opArity(op)) * 4;
    }
    return false;
}

class Lowering
{
public:
    Lowering(Runtime& rt, const MethodInfo& root, IrFunction& fn, bool osr)
        : rt_(rt), root_(root), fn_(fn), osr_(osr), core_(rt.types->core())
    {}

    void run()
    {
        fn_.method = &root_;
        const MethodInfo& m = root_;
        if (m.body.code.empty())
            refuse("the method has no body");
        for (const Handler& handler : m.body.handlers) {
            if (handler.stackDepth != 0)
                refuse("a protected region with values on the evaluation stack");
            if (handler.kind == HandlerKind::Catch && handler.exceptionSlot < m.frameSize() &&
                handler.exceptionSlot < m.body.localBoxed.size() && m.body.localBoxed[handler.exceptionSlot])
                refuse("a caught exception that a lambda captures");
        }

        Scope scope;
        scope.method = &m;
        scope_ = &scope;
        prepareScope(scope);

        const Cls ret = returnClass(m);
        fn_.returnsValue = ret != Cls::Void;
        fn_.returnsFp = ret == Cls::F32 || ret == Cls::F64;

        // The calling convention: `this`, then the parameters.
        const std::uint32_t paramBase = m.isStatic() ? 0u : 1u;
        const std::uint32_t argCount = paramBase + std::uint32_t(m.params.size());
        for (std::uint32_t slot = 0; slot < argCount; ++slot) {
            // A `ref` or `out` parameter is a box, whatever the box holds.
            const bool byRef = slot >= paramBase && (m.params[slot - paramBase].mode == ParamMode::Ref ||
                                                     m.params[slot - paramBase].mode == ParamMode::Out);
            const Cls cls = byRef ? Cls::Ref : slotClass(scope, slot);
            if (cls == Cls::Obj) {
                scope.localTag[slot] = fn_.newVreg(false);
                fn_.params.push_back(scope.localTag[slot]);
            }
            const std::int32_t incoming = fn_.newVreg(isFpClass(cls));
            fn_.params.push_back(incoming);
            scope.localVreg[slot] = incoming;
        }
        // `this` is never null: every way in checks it.
        if (paramBase)
            scope.localNonNull[0] = 1;

        // Ownership: a parameter the body assigns is copied in and counted, so that the rules for a local hold.
        const std::vector<std::uint8_t> assigned = assignedSlots(m);
        for (std::uint32_t slot = 0; slot < argCount; ++slot) {
            if (m.body.localBoxed.size() > slot && m.body.localBoxed[slot])
                continue;  // a captured parameter moves into its box below
            const Cls cls = slotClass(scope, slot);
            if (cls != Cls::Ref && cls != Cls::Obj)
                continue;
            if (assigned[slot]) {
                incRefOf(scope.localVreg[slot], scope.localTag[slot], true);
                scope.localOwned[slot] = 1;
            }
        }
        // Locals start at their default.
        for (std::uint32_t slot = argCount; slot < scope.localVreg.size(); ++slot)
            initLocal(scope, slot);

        // A lambda's captured boxes come with the delegate, and the delegate outlives the call.
        for (std::uint32_t captureSlot : m.captureSlots) {
            if (captureSlot >= scope.localVreg.size())
                refuse("a capture slot is out of range");
            const std::int32_t box = fn_.newVreg(false);
            scope.localVreg[captureSlot] = box;
            scope.localOwned[captureSlot] = 0;
            scope.localNonNull[captureSlot] = 1;
            fn_.captures.push_back(box);
        }

        if (!m.body.handlers.empty()) {
            exceptionVreg_ = fn_.newVreg(false);
            IrInst& none = inst(IrOp::MovImm);
            none.d = exceptionVreg_;
            none.imm = 0;
            fn_.osrZero.push_back(exceptionVreg_);
        }

        lowerBody(scope);
        for (std::size_t i = 0; i < deferred_.size(); ++i) {
            const auto block = deferred_[i];
            block();
        }
        fn_.hasLoops = hasLoops_;
    }

private:
    // ---- small helpers ----
    static bool isFpClass(Cls c) noexcept { return c == Cls::F32 || c == Cls::F64; }

    IrInst& inst(IrOp op)
    {
        fn_.code.emplace_back();
        fn_.code.back().op = op;
        return fn_.code.back();
    }

    void incRefOf(std::int32_t vreg, std::int32_t tag, bool nullable)
    {
        IrInst& in = inst(tag >= 0 ? IrOp::IncRefTagged : IrOp::IncRef);
        in.a = vreg;
        in.b = tag;
        in.isNullable = nullable;
    }
    void decRefOf(std::int32_t vreg, std::int32_t tag, bool nullable)
    {
        IrInst& in = inst(tag >= 0 ? IrOp::DecRefTagged : IrOp::DecRef);
        in.a = vreg;
        in.b = tag;
        in.isNullable = nullable;
    }

    std::int32_t newLabel() { return fn_.labelCount++; }
    void bindLabel(std::int32_t label)
    {
        inst(IrOp::Label).imm = label;
        forgetFacts();
    }
    void forgetFacts()
    {
        for (Scope* s = scope_; s; s = s->parent)
            std::fill(s->localNonNull.begin(), s->localNonNull.end(), std::uint8_t(0));
        if (scope_ && !scope_->method->isStatic() && !scope_->localNonNull.empty() && !isThisAssigned_)
            scope_->localNonNull[0] = 1;
        for (Scope* s = scope_; s; s = s->parent) {
            for (std::size_t i = 0; i < s->method->captureSlots.size(); ++i) {
                const std::uint32_t slot = s->method->captureSlots[i];
                if (slot < s->localNonNull.size())
                    s->localNonNull[slot] = 1;
            }
        }
        lastBounds_ = {};
        if (scope_)
            scope_->thisIsFresh = false;
    }

    Cls returnClass(const MethodInfo& m) const
    {
        if (m.isConstructor() && m.owner && m.owner->nativeKind != NativeKind::None)
            return Cls::Ref;  // a type the engine implements returns the instance its constructor made
        return m.returnType ? classOf(m.returnType) : Cls::Void;
    }

    Cls slotClass(const Scope& s, std::uint32_t slot) const
    {
        if (slot >= s.localType.size() || !s.localType[slot])
            refuse("a slot has no static type");
        return classOf(s.localType[slot]);
    }

    /// What every slot of a method holds, by its static type; `this` takes the owner's.
    void prepareScope(Scope& s)
    {
        const MethodInfo& m = *s.method;
        const std::uint32_t frame = m.frameSize();
        s.localVreg.assign(frame, -1);
        s.localTag.assign(frame, -1);
        s.localOwned.assign(frame, 0);
        s.localConstKnown.assign(frame, 0);
        s.localConst.assign(frame, 0);
        s.localNonNull.assign(frame, 0);
        s.localType.assign(frame, nullptr);
        const std::uint32_t paramBase = m.isStatic() ? 0u : 1u;
        if (paramBase)
            s.localType[0] = m.owner;
        for (std::size_t i = 0; i < m.params.size(); ++i)
            s.localType[paramBase + i] = m.params[i].type;
        for (std::size_t i = 0; i < m.body.localTypes.size() && i < frame; ++i) {
            if (m.body.localTypes[i] && !(paramBase && i == 0))
                s.localType[i] = m.body.localTypes[i];
        }
        s.labels.assign(m.body.code.size() + 1, -1);
        s.joins.assign(m.body.code.size() + 1, {});
        scanBranches(s);
    }

    std::vector<std::uint8_t> assignedSlots(const MethodInfo& m) const
    {
        std::vector<std::uint8_t> out(m.frameSize(), 0);
        const std::vector<std::uint8_t>& code = m.body.code;
        for (std::size_t pc = 0; pc < code.size();) {
            const Op op = Op(code[pc]);
            if (op == Op::StLoc) {
                const std::uint32_t slot = readU32(&code[pc + 1]);
                if (slot < out.size())
                    out[slot] = 1;
            }
            pc += 1 + std::size_t(opArity(op)) * 4;
        }
        return out;
    }

    /// Give a label to every branch target, and find what is reachable.
    void scanBranches(Scope& s)
    {
        const std::vector<std::uint8_t>& code = s.method->body.code;
        for (std::size_t pc = 0; pc < code.size();) {
            const Op op = Op(code[pc]);
            if (pc + 1 + std::size_t(opArity(op)) * 4 > code.size())
                refuse("the body is truncated");
            if (op == Op::Br || op == Op::BrTrue || op == Op::BrFalse || op == Op::Leave) {
                const std::uint32_t target = readU32(&code[pc + 1]);
                if (target >= code.size())
                    refuse("a branch leaves the body");
                if (s.labels[target] < 0)
                    s.labels[target] = newLabel();
            }
            pc += 1 + std::size_t(opArity(op)) * 4;
        }
        for (const Handler& handler : s.method->body.handlers) {
            if (handler.target >= code.size())
                refuse("a handler outside the body");
            if (s.labels[handler.target] < 0)
                s.labels[handler.target] = newLabel();
        }
    }

    std::vector<std::uint8_t> reachable(const MethodInfo& m) const
    {
        const std::vector<std::uint8_t>& code = m.body.code;
        std::vector<std::uint8_t> seen(code.size() + 1, 0);
        std::vector<std::uint32_t> work{0};
        for (const Handler& handler : m.body.handlers)
            work.push_back(handler.target);
        while (!work.empty()) {
            std::uint32_t pc = work.back();
            work.pop_back();
            while (pc < code.size() && !seen[pc]) {
                seen[pc] = 1;
                const Op op = Op(code[pc]);
                const std::uint32_t next = pc + 1 + std::uint32_t(opArity(op)) * 4;
                if (op == Op::Ret || op == Op::RetVoid || op == Op::Throw || op == Op::Rethrow || op == Op::EndFinally)
                    break;
                if (op == Op::Br || op == Op::Leave) {
                    pc = readU32(&code[pc + 1]);
                    continue;
                }
                if (op == Op::BrTrue || op == Op::BrFalse)
                    work.push_back(readU32(&code[pc + 1]));
                pc = next;
            }
        }
        return seen;
    }

    void initLocal(Scope& s, std::uint32_t slot)
    {
        const MethodInfo& m = *s.method;
        const bool boxed = slot < m.body.localBoxed.size() && m.body.localBoxed[slot];
        const Cls c = boxed ? Cls::Ref : slotClass(s, slot);
        if (s.localVreg[slot] < 0)
            s.localVreg[slot] = fn_.newVreg(isFpClass(c));
        if (c == Cls::Obj) {
            if (s.localTag[slot] < 0)
                s.localTag[slot] = fn_.newVreg(false);
            IrInst& tag = inst(IrOp::MovImm);
            tag.d = s.localTag[slot];
            tag.imm = std::int64_t(Type::Null);
        }
        if (isFpClass(c)) {
            IrInst& in = inst(IrOp::FMovImm);
            in.d = s.localVreg[slot];
            in.width = c == Cls::F32 ? 32 : 64;
            in.imm = 0;
        } else {
            IrInst& in = inst(IrOp::MovImm);
            in.d = s.localVreg[slot];
            in.imm = 0;
        }
        if (c == Cls::Ref || c == Cls::Obj)
            s.localOwned[slot] = 1;
    }

    std::int32_t site()
    {
        IrSite entry;
        entry.method = scope_->method;
        entry.pc = scope_->pc;
        entry.parent = scope_->parentSite;
        if (!fn_.sites.empty() && lastSite_ >= 0 && fn_.sites[std::size_t(lastSite_)].method == entry.method &&
            fn_.sites[std::size_t(lastSite_)].pc == entry.pc && fn_.sites[std::size_t(lastSite_)].parent == entry.parent)
            return lastSite_;
        fn_.sites.push_back(entry);
        lastSite_ = std::int32_t(fn_.sites.size()) - 1;
        return lastSite_;
    }

    /// Everything that holds a count right now, across every scope: what an exit has to release.
    std::vector<std::int32_t> ownedNow(bool toHandler = false)
    {
        std::vector<std::int32_t> out;
        taggedNow_.clear();
        for (Scope* s = scope_; s; s = s->parent) {
            // A handler in this method carries on with the method's locals.
            const bool keepLocals = toHandler && !s->parent;
            for (std::size_t slot = 0; !keepLocals && slot < s->localVreg.size(); ++slot) {
                if (!s->localOwned[slot] || s->localVreg[slot] < 0)
                    continue;
                if (s->localTag[slot] >= 0)
                    taggedNow_.push_back({s->localVreg[slot], s->localTag[slot]});
                else
                    out.push_back(s->localVreg[slot]);
            }
            if (!keepLocals && !s->parent && exceptionVreg_ >= 0)
                out.push_back(exceptionVreg_);
            for (const Entry& e : s->stack) {
                if (e.own != Own::Owned || e.isConst)
                    continue;
                if (e.tagVreg >= 0)
                    taggedNow_.push_back({e.vreg, e.tagVreg});
                else
                    out.push_back(e.vreg);
            }
            out.insert(out.end(), s->held.begin(), s->held.end());
            taggedNow_.insert(taggedNow_.end(), s->heldTagged.begin(), s->heldTagged.end());
        }
        return out;
    }

    /// The handlers this instruction of the root method is protected by, innermost first as the table lists them.
    std::vector<const Handler*> handlersAt(std::uint32_t pc) const
    {
        std::vector<const Handler*> out;
        for (const Handler& handler : root_.body.handlers) {
            if (pc >= handler.start && pc < handler.end)
                out.push_back(&handler);
        }
        return out;
    }

    std::uint32_t rootPc() const noexcept
    {
        const Scope* s = scope_;
        while (s->parent)
            s = s->parent;
        return s->pc;
    }

    /// The code that decides, for an exception raised under these handlers, where it goes; one per distinct list.
    std::int32_t landingFor(const std::vector<const Handler*>& handlers)
    {
        const auto known = landings_.find(handlers);
        if (known != landings_.end())
            return known->second;
        const std::int32_t label = newLabel();
        landings_.emplace(handlers, label);
        const std::uint32_t pc = rootPc();
        Scope* root = scope_;
        while (root->parent)
            root = root->parent;
        deferred_.push_back([this, handlers, label, pc, root]() {
            scope_ = root;
            root->stack.clear();
            root->pc = pc;
            inst(IrOp::Label).imm = label;
            for (const Handler* handler : handlers) {
                if (handler->kind == HandlerKind::Catch) {
                    const std::int32_t cell = callPure(reinterpret_cast<void*>(&scriptJitCatch),
                                                       {rtReg(), ptrReg(handler->catchType)}, false);
                    const std::int32_t next = newLabel();
                    IrInst& test = inst(IrOp::BrCmp);
                    test.a = cell;
                    test.cond = IrCond::Eq;
                    test.width = 64;
                    test.hasImm = true;
                    test.imm = 0;
                    test.target = next;
                    // The slot and the exception a `throw;` would raise again each hold a count.
                    IrInst& second = inst(IrOp::IncRef);
                    second.a = cell;
                    assignOwned(exceptionVreg_, cell);
                    if (handler->exceptionSlot < root->localVreg.size() && root->localVreg[handler->exceptionSlot] >= 0 &&
                        root->localOwned[handler->exceptionSlot]) {
                        assignOwned(root->localVreg[handler->exceptionSlot], cell);
                    } else {
                        IrInst& drop = inst(IrOp::DecRef);
                        drop.a = cell;
                    }
                    inst(IrOp::Br).imm = root->labels[handler->target];
                    inst(IrOp::Label).imm = next;
                } else {
                    const std::int32_t saved = callPure(reinterpret_cast<void*>(&scriptJitFinallyEnter), {rtReg()}, false);
                    const std::int32_t next = newLabel();
                    IrInst& test = inst(IrOp::BrCmp);
                    test.a = saved;
                    test.cond = IrCond::Eq;
                    test.width = 32;
                    test.hasImm = true;
                    test.imm = 0;
                    test.target = next;
                    const std::int32_t negated = fn_.newVreg(false);
                    IrInst& neg = inst(IrOp::Neg);
                    neg.d = negated;
                    neg.a = saved;
                    neg.width = 32;
                    IrInst& mov = inst(IrOp::Mov);
                    mov.d = continuationVreg(*handler);
                    mov.a = negated;
                    inst(IrOp::Br).imm = root->labels[handler->target];
                    inst(IrOp::Label).imm = next;
                }
            }
            leaveMethod();
        });
        return label;
    }

    /// Carry the pending error out of this method: its locals are released on the way.
    void leaveMethod()
    {
        const std::int32_t always = immReg(0);
        const std::int32_t at = std::int32_t(fn_.code.size());
        inst(IrOp::ExitCmp);
        IrExit exit;
        exit.cleanup = ownedNow(false);
        exit.cleanupTagged = taggedNow_;
        exit.site = site();
        fn_.maxCleanup = std::max(fn_.maxCleanup, std::uint32_t(exit.cleanup.size() + exit.cleanupTagged.size()));
        fn_.exits.push_back(std::move(exit));
        IrInst& leave = fn_.code[std::size_t(at)];
        leave.a = always;
        leave.b = always;
        leave.cond = IrCond::Eq;
        leave.width = 64;
        leave.target = std::int32_t(fn_.exits.size()) - 1;
        inst(IrOp::Trap);
    }

    void assignOwned(std::int32_t vreg, std::int32_t cell)
    {
        const std::int32_t old = fn_.newVreg(false);
        IrInst& save = inst(IrOp::Mov);
        save.d = old;
        save.a = vreg;
        IrInst& mov = inst(IrOp::Mov);
        mov.d = vreg;
        mov.a = cell;
        IrInst& dec = inst(IrOp::DecRef);
        dec.a = old;
        dec.isNullable = true;
    }

    /// Where a `finally` goes when it ends: negative to carry on raising (the saved exception's number), positive for
    /// the continuation of that number.
    std::int32_t continuationVreg(const Handler& handler)
    {
        const auto it = continuation_.find(&handler);
        if (it != continuation_.end())
            return it->second;
        const std::int32_t v = fn_.newVreg(false);
        continuation_.emplace(&handler, v);
        return v;
    }

    /// `Leave`: through each `finally` it crosses, then on to the target.
    void lowerLeave(std::uint32_t pc, std::uint32_t target)
    {
        std::vector<const Handler*> chain;
        for (const Handler& handler : root_.body.handlers) {
            if (handler.kind != HandlerKind::Finally)
                continue;
            if (pc >= handler.start && pc < handler.end && !(target >= handler.start && target < handler.end))
                chain.push_back(&handler);
        }
        while (!scope_->stack.empty()) {
            const Entry dropped = pop();
            release(dropped);
        }
        std::int32_t next = scope_->labels[target];
        for (std::size_t i = chain.size(); i-- > 0;) {
            const Handler* handler = chain[i];
            std::vector<std::int32_t>& table = finallyExits_[handler];
            table.push_back(next);
            const std::int64_t id = std::int64_t(table.size());
            const std::int32_t cont = continuationVreg(*handler);
            if (i == 0) {
                IrInst& set = inst(IrOp::MovImm);
                set.d = cont;
                set.imm = id;
                inst(IrOp::Br).imm = scope_->labels[handler->target];
                return;
            }
            // The one before this ends up here.
            const std::int32_t stub = newLabel();
            const std::int32_t handlerLabel = scope_->labels[handler->target];
            deferred_.push_back([this, stub, cont, id, handlerLabel]() {
                inst(IrOp::Label).imm = stub;
                IrInst& set = inst(IrOp::MovImm);
                set.d = cont;
                set.imm = id;
                inst(IrOp::Br).imm = handlerLabel;
            });
            next = stub;
        }
        inst(IrOp::Br).imm = next;
    }

    void lowerEndFinally(std::uint32_t pc)
    {
        // The `finally` this ends: the innermost one whose body has been entered, by layout.
        const Handler* handler = nullptr;
        for (const Handler& candidate : root_.body.handlers) {
            if (candidate.kind != HandlerKind::Finally || candidate.target > pc)
                continue;
            if (finallyEnded_.count(&candidate))
                continue;
            if (!handler || candidate.target > handler->target)
                handler = &candidate;
        }
        if (!handler)
            refuse("an end of a finally with no finally");
        finallyEnded_.insert(handler);
        const std::int32_t cont = continuationVreg(*handler);
        const std::int32_t dispatch = newLabel();
        inst(IrOp::Br).imm = dispatch;
        Scope* root = scope_;
        deferred_.push_back([this, handler, cont, dispatch, pc, root]() {
            scope_ = root;
            root->stack.clear();
            root->pc = pc;
            inst(IrOp::Label).imm = dispatch;
            const std::vector<std::int32_t>& table = finallyExits_[handler];
            for (std::size_t i = 0; i < table.size(); ++i) {
                IrInst& br = inst(IrOp::BrCmp);
                br.a = cont;
                br.cond = IrCond::Eq;
                br.width = 32;
                br.hasImm = true;
                br.imm = std::int64_t(i + 1);
                br.target = table[i];
            }
            // Otherwise an exception is passing through: raise it again, for whatever protects this point.
            callPure(reinterpret_cast<void*>(&scriptJitFinallyRethrow), {rtReg(), cont}, false, false);
            const std::vector<const Handler*> outer = handlersAt(pc);
            if (!outer.empty())
                inst(IrOp::Br).imm = landingFor(outer);
            else
                leaveMethod();
        });
    }

    std::int32_t makeExit(std::uint32_t code, std::vector<std::int32_t> extra = {})
    {
        const std::vector<const Handler*> handlers =
            root_.body.handlers.empty() ? std::vector<const Handler*>() : handlersAt(rootPc());
        std::vector<std::int32_t> cleanup = ownedNow(!handlers.empty());
        std::vector<std::pair<std::int32_t, std::int32_t>> tagged = taggedNow_;
        tagged.insert(tagged.end(), extraTagged_.begin(), extraTagged_.end());
        cleanup.insert(cleanup.end(), extra.begin(), extra.end());
        const std::int32_t where = site();
        const std::int32_t handlerLabel = handlers.empty() ? -1 : landingFor(handlers);
        if (!fn_.exits.empty()) {
            const IrExit& last = fn_.exits.back();
            if (last.code == code && last.cleanup == cleanup && last.cleanupTagged == tagged && last.site == where &&
                last.handlerLabel == handlerLabel && last.indexVreg == pendingIndex_)
                return std::int32_t(fn_.exits.size()) - 1;
        }
        IrExit exit;
        exit.handlerLabel = handlerLabel;
        exit.cleanupTagged = std::move(tagged);
        exit.indexVreg = pendingIndex_;
        fn_.maxCleanup = std::max(fn_.maxCleanup, std::uint32_t(cleanup.size() + exit.cleanupTagged.size()));
        fn_.maxCleanup = std::max(fn_.maxCleanup, std::uint32_t(cleanup.size()));
        exit.cleanup = std::move(cleanup);
        exit.code = code;
        exit.site = where;
        fn_.exits.push_back(std::move(exit));
        return std::int32_t(fn_.exits.size()) - 1;
    }

    // ---- entries ----
    Entry constInt(std::int64_t v, const TypeInfo* type)
    {
        Entry e;
        e.isConst = true;
        e.imm = v;
        e.type = type;
        return e;
    }
    Entry constFp(std::uint64_t bits, const TypeInfo* type)
    {
        Entry e;
        e.isConst = true;
        e.imm = std::int64_t(bits);
        e.type = type;
        return e;
    }
    Entry valueEntry(std::int32_t vreg, const TypeInfo* type)
    {
        Entry e;
        e.vreg = vreg;
        e.type = type;
        return e;
    }
    Entry refEntry(std::int32_t vreg, const TypeInfo* type, Own own, bool nonNull = false)
    {
        Entry e;
        e.vreg = vreg;
        e.type = type;
        e.own = own;
        e.nonNull = nonNull;
        return e;
    }

    void push(Entry e) { scope_->stack.push_back(e); }
    Entry pop()
    {
        if (scope_->stack.empty())
            refuse("the evaluation stack underflows");
        Entry e = scope_->stack.back();
        scope_->stack.pop_back();
        return e;
    }
    Entry& top(std::size_t fromTop = 0)
    {
        if (scope_->stack.size() <= fromTop)
            refuse("the evaluation stack underflows");
        return scope_->stack[scope_->stack.size() - 1 - fromTop];
    }

    /// The entry's value, in a register.
    std::int32_t reg(const Entry& e)
    {
        if (!e.isConst)
            return e.vreg;
        const Cls c = e.type ? classOf(e.type) : Cls::Ref;
        if (isFpClass(c)) {
            const std::int32_t d = fn_.newVreg(true);
            IrInst& in = inst(IrOp::FMovImm);
            in.d = d;
            in.width = c == Cls::F32 ? 32 : 64;
            in.imm = e.imm;
            return d;
        }
        const std::int32_t d = fn_.newVreg(false);
        IrInst& in = inst(IrOp::MovImm);
        in.d = d;
        in.imm = e.imm;
        return d;
    }

    /// Stop an entry reading through to a local, because that local is about to change.
    void copyOutOfLocal(Entry& e)
    {
        if (e.isConst || e.local < 0)
            return;
        const Cls c = e.type ? classOf(e.type) : Cls::Ref;
        const std::int32_t copy = fn_.newVreg(isFpClass(c));
        IrInst& in = inst(IrOp::Mov);
        in.d = copy;
        in.a = e.vreg;
        e.vreg = copy;
        if (e.tagVreg >= 0) {
            const std::int32_t tagCopy = fn_.newVreg(false);
            IrInst& tagMov = inst(IrOp::Mov);
            tagMov.d = tagCopy;
            tagMov.a = e.tagVreg;
            e.tagVreg = tagCopy;
        }
        if (e.own == Own::Local) {
            incRefOf(copy, e.tagVreg, !e.nonNull);
            e.own = Own::Owned;
        }
        e.local = -1;
    }

    /// Turn an entry that borrows into one that holds a count.
    void makeOwned(Entry& e)
    {
        if (e.isConst || e.own == Own::Owned || e.own == Own::None)
            return;
        if (e.own == Own::Immortal) {
            incRefOf(e.vreg, e.tagVreg, !e.nonNull);
            e.own = Own::Owned;
            return;
        }
        // A copy, because the register itself may be the local's.
        const std::int32_t copy = fn_.newVreg(false);
        IrInst& mov = inst(IrOp::Mov);
        mov.d = copy;
        mov.a = e.vreg;
        if (e.tagVreg >= 0) {
            const std::int32_t tagCopy = fn_.newVreg(false);
            IrInst& tagMov = inst(IrOp::Mov);
            tagMov.d = tagCopy;
            tagMov.a = e.tagVreg;
            e.tagVreg = tagCopy;
        }
        incRefOf(copy, e.tagVreg, !e.nonNull);
        e.vreg = copy;
        e.own = Own::Owned;
        e.local = -1;
    }

    /// Before anything that could overwrite a field or an array element: what was read from one must hold its own count.
    void secureWeak()
    {
        for (Scope* s = scope_; s; s = s->parent) {
            for (Entry& e : s->stack) {
                if (e.own == Own::Weak)
                    makeOwned(e);
            }
        }
    }

    void release(const Entry& e)
    {
        if (e.own != Own::Owned || e.isConst)
            return;
        secureWeak();
        decRefOf(e.vreg, e.tagVreg, !e.nonNull);
    }

    /// Whatever is headed for a place of static type `object` or an interface: with its tag in a register. Boxing a
    /// primitive is nothing at run time beyond that.
    void coerceToObject(Entry& e, const TypeInfo* to)
    {
        const Cls from = e.type ? classOf(e.type) : Cls::Ref;
        if (from == Cls::Obj) {
            e.type = to;
            return;
        }
        if (from == Cls::Ref) {
            const bool isNullLiteral = e.isConst;
            const std::int32_t payload = reg(e);
            e.vreg = payload;
            e.isConst = false;
            if (isNullLiteral || !e.type || baseKind(e.type) == TypeKind::Null) {
                e.tagVreg = isNullLiteral ? immReg(std::int64_t(Type::Null))
                                          : callPure(reinterpret_cast<void*>(&scriptJitTagOf), {payload}, false);
            } else if (e.nonNull) {
                e.tagVreg = immReg(std::int64_t(refTag(e.type)));
            } else {
                const std::int32_t d = fn_.newVreg(false);
                IrInst& in = inst(IrOp::TagOfNullable);
                in.d = d;
                in.a = payload;
                in.tag = refTag(e.type);
                e.tagVreg = d;
            }
            e.type = to;
            return;
        }
        // A primitive: its bits, and the tag its type says.
        const std::int32_t tag = immReg(std::int64_t(runtimeTag(baseKind(e.type))));
        const std::int32_t bits = payloadOfEntry(e);
        e.vreg = bits;
        e.tagVreg = tag;
        e.isConst = false;
        e.local = -1;
        e.own = Own::None;
        e.type = to;
    }

    /// Make `e` fit a place of static type `to`, when the bytecode left that to the runtime tag.
    void coerceTo(Entry& e, const TypeInfo* to)
    {
        const Cls want = to ? classOf(to) : Cls::Ref;
        if (want == Cls::Obj) {
            coerceToObject(e, to);
            return;
        }
        const Cls have = e.type ? classOf(e.type) : Cls::Ref;
        if (want != have)
            refuse("a value of a different representation than the place it goes");
    }

    /// Null check an entry, unless it is known not to be null.
    void checkNotNull(Entry& e)
    {
        if (e.nonNull)
            return;
        if (e.isConst)  // the null literal
            e.vreg = reg(e), e.isConst = false;
        if (e.local >= 0 && scope_->localNonNull[std::size_t(e.local)]) {
            e.nonNull = true;
            return;
        }
        IrInst* in = &inst(IrOp::ExitCmp);
        const std::int32_t index = std::int32_t(fn_.code.size()) - 1;
        (void)in;
        const std::int32_t exit = makeExit(std::uint32_t(ErrorCode::NullReference));
        IrInst& check = fn_.code[std::size_t(index)];
        check.a = e.tagVreg >= 0 ? e.tagVreg : e.vreg;
        check.cond = IrCond::Eq;
        check.width = e.tagVreg >= 0 ? 32 : 64;
        check.hasImm = true;
        check.imm = e.tagVreg >= 0 ? std::int64_t(Type::Null) : 0;
        check.target = exit;
        e.nonNull = true;
        if (e.local >= 0)
            scope_->localNonNull[std::size_t(e.local)] = 1;
    }

    /// Counts held by entries that were popped but are still in use, for an exit taken meanwhile.
    class Held
    {
    public:
        Held(Lowering& lowering, std::initializer_list<const Entry*> entries)
            : scope_(*lowering.scope_), size_(scope_.held.size()), taggedSize_(scope_.heldTagged.size())
        {
            for (const Entry* e : entries) {
                if (e->own != Own::Owned || e->isConst)
                    continue;
                if (e->tagVreg >= 0)
                    scope_.heldTagged.push_back({e->vreg, e->tagVreg});
                else
                    scope_.held.push_back(e->vreg);
            }
        }
        ~Held()
        {
            scope_.held.resize(size_);
            scope_.heldTagged.resize(taggedSize_);
        }
        Held(const Held&) = delete;
        Held& operator=(const Held&) = delete;

    private:
        Scope& scope_;
        std::size_t size_;
        std::size_t taggedSize_;
    };

    // ---- join points ----
    /// Before an edge to `target`: the stack is moved to where that join expects it, holding counts.
    void flowTo(std::uint32_t target)
    {
        Scope::Join& join = scope_->joins[target];
        std::vector<Entry>& stack = scope_->stack;
        if (!join.seen) {
            join.seen = true;
            join.entries.resize(stack.size());
            for (std::size_t i = 0; i < stack.size(); ++i) {
                const Cls c = stack[i].type ? classOf(stack[i].type) : Cls::Ref;
                Entry canonical;
                canonical.vreg = fn_.newVreg(isFpClass(c));
                canonical.type = stack[i].type;
                canonical.own = c == Cls::Ref || c == Cls::Obj ? Own::Owned : Own::None;
                if (c == Cls::Obj)
                    canonical.tagVreg = fn_.newVreg(false);
                join.entries[i] = canonical;
            }
        } else if (join.entries.size() != stack.size()) {
            refuse("the evaluation stack is inconsistent at a join");
        }
        for (std::size_t i = 0; i < stack.size(); ++i) {
            Entry& e = stack[i];
            Entry& canonical = join.entries[i];
            if (canonical.vreg == e.vreg && !e.isConst)
                continue;
            const Cls have = e.type ? classOf(e.type) : Cls::Ref;
            const Cls want = canonical.type ? classOf(canonical.type) : Cls::Ref;
            // `cond ? null : x` may see the null first. `a?.Length` makes an int that can be null, with no type to say
            // so: that keeps interpreting.
            if (have != want && want != Cls::Obj)
                refuse("a value of a different representation on each way into a join");
            if (baseKind(canonical.type) == TypeKind::Null && e.type)
                canonical.type = e.type;
            if (canonical.tagVreg >= 0)
                coerceToObject(e, canonical.type);
            if (e.own != Own::None)
                makeOwned(e);
            const std::int32_t from = reg(e);
            IrInst& mov = inst(IrOp::Mov);
            mov.d = canonical.vreg;
            mov.a = from;
            if (canonical.tagVreg >= 0) {
                IrInst& tagMov = inst(IrOp::Mov);
                tagMov.d = canonical.tagVreg;
                tagMov.a = e.tagVreg;
                e.tagVreg = canonical.tagVreg;
            }
            e.vreg = canonical.vreg;
            e.isConst = false;
            e.local = -1;
            e.own = canonical.own;
        }
    }

    /// Entering a label: the stack is whatever the join says; an edge that never came means an empty stack.
    void enterJoin(std::uint32_t pc, bool fallsIn)
    {
        Scope::Join& join = scope_->joins[pc];
        if (fallsIn)
            flowTo(pc);
        bindLabel(scope_->labels[pc]);
        if (join.seen)
            scope_->stack = join.entries;
        else
            scope_->stack.clear();
    }

    std::int32_t labelFor(std::uint32_t target)
    {
        if (scope_->labels[target] < 0)
            refuse("a branch has no label");
        return scope_->labels[target];
    }

    // ---- the walk ----
    void lowerBody(Scope& s)
    {
        const MethodInfo& m = *s.method;
        const std::vector<std::uint8_t>& code = m.body.code;
        const std::vector<std::uint8_t> live = reachable(m);
        bool fallsIn = true;
        bool hasOsr = osr_ && !s.parent;
        // Which labels are loop heads, for the OSR entries.
        std::vector<std::uint8_t> loopHead(code.size() + 1, 0);
        for (std::size_t pc = 0; pc < code.size();) {
            const Op op = Op(code[pc]);
            if ((op == Op::Br || op == Op::BrTrue || op == Op::BrFalse) && live[pc] && readU32(&code[pc + 1]) <= pc) {
                loopHead[readU32(&code[pc + 1])] = 1;
                hasLoops_ = true;
            }
            pc += 1 + std::size_t(opArity(op)) * 4;
        }

        for (std::uint32_t pc = 0; pc < code.size();) {
            const Op op = Op(code[pc]);
            const std::uint32_t next = pc + 1 + std::uint32_t(opArity(op)) * 4;
            if (!live[pc]) {
                pc = next;
                fallsIn = false;
                continue;
            }
            s.pc = pc;
            if (s.labels[pc] >= 0) {
                enterJoin(pc, fallsIn);
                if (hasOsr && loopHead[pc] && s.stack.empty())
                    addOsrEntry(s, pc);
            }
            fallsIn = true;
            const std::uint8_t* operands = &code[pc] + 1;
            // A compare followed by a branch on it fuses, when nothing else is on the stack to move to the join.
            if (isCompare(op) && next < code.size() && s.labels[next] < 0 && s.stack.size() == 2) {
                const Op after = Op(code[next]);
                if (after == Op::BrTrue || after == Op::BrFalse) {
                    const std::uint32_t target = readU32(&code[next + 1]);
                    lowerFusedBranch(op, after == Op::BrTrue, target, pc);
                    pc = next + 5;
                    continue;
                }
            }
            switch (op) {
                case Op::Br: {
                    const std::uint32_t target = readU32(operands);
                    flowTo(target);
                    if (target <= pc)
                        emitPoll();
                    inst(IrOp::Br).imm = labelFor(target);
                    fallsIn = false;
                    break;
                }
                case Op::BrTrue:
                case Op::BrFalse: {
                    const std::uint32_t target = readU32(operands);
                    Entry cond = pop();
                    if (target <= pc)
                        emitPoll();
                    flowTo(target);
                    if (cond.isConst) {
                        if ((cond.imm != 0) == (op == Op::BrTrue)) {
                            inst(IrOp::Br).imm = labelFor(target);
                            fallsIn = false;
                        }
                        break;
                    }
                    IrInst& br = inst(IrOp::BrCmp);
                    br.a = cond.vreg;
                    br.cond = op == Op::BrTrue ? IrCond::Ne : IrCond::Eq;
                    br.width = cond.type && classOf(cond.type) == Cls::I32 ? 32 : 64;
                    br.hasImm = true;
                    br.imm = 0;
                    br.target = labelFor(target);
                    break;
                }
                case Op::Ret:
                case Op::RetVoid:
                    lowerReturn(op == Op::Ret);
                    fallsIn = false;
                    break;
                case Op::Throw:
                    lowerThrow();
                    fallsIn = false;
                    break;
                case Op::Rethrow: {
                    if (exceptionVreg_ < 0)
                        refuse("a rethrow outside a handler");
                    Entry current;
                    current.vreg = exceptionVreg_;
                    current.type = core_.exceptionType;
                    current.own = Own::Immortal;
                    push(current);
                    lowerThrow();
                    fallsIn = false;
                    break;
                }
                case Op::Leave:
                    lowerLeave(pc, readU32(operands));
                    fallsIn = false;
                    break;
                case Op::EndFinally:
                    lowerEndFinally(pc);
                    fallsIn = false;
                    break;
                default: lowerSimple(op, operands, pc);
            }
            pc = next;
        }
    }

    void addOsrEntry(Scope& s, std::uint32_t pc)
    {
        IrOsrEntry entry;
        entry.bytecodePc = pc;
        entry.label = s.labels[pc];
        const MethodInfo& m = *s.method;
        for (std::uint32_t slot = 0; slot < s.localVreg.size(); ++slot) {
            if (s.localVreg[slot] < 0)
                continue;
            const bool boxed = slot < m.body.localBoxed.size() && m.body.localBoxed[slot];
            const Cls c = boxed ? Cls::Ref : slotClass(s, slot);
            IrOsrEntry::Slot out;
            out.vreg = s.localVreg[slot];
            out.slot = slot;
            out.bytes = c == Cls::I32 || c == Cls::F32 ? 4 : 8;
            out.owned = s.localOwned[slot] != 0;
            out.tagVreg = c == Cls::Obj ? s.localTag[slot] : -1;
            entry.slots.push_back(out);
        }
        fn_.osrEntries.push_back(std::move(entry));
    }

    void emitPoll()
    {
        const std::int32_t at = std::int32_t(fn_.code.size());
        inst(IrOp::Poll);
        const std::int32_t exit = makeExit(0);
        fn_.code[std::size_t(at)].target = exit;
        hasLoops_ = true;
    }

    static bool isCompare(Op op) noexcept
    {
        return (op >= Op::CeqI4 && op <= Op::CgeR8);
    }

    static IrCond compareCond(Op op, std::uint8_t& width, bool& fp) noexcept
    {
        width = 32;
        fp = false;
        switch (op) {
            case Op::CeqI4: return IrCond::Eq;
            case Op::CneI4: return IrCond::Ne;
            case Op::CltI4: return IrCond::Lt;
            case Op::CleI4: return IrCond::Le;
            case Op::CgtI4: return IrCond::Gt;
            case Op::CgeI4: return IrCond::Ge;
            case Op::CltU4: return IrCond::Lo;
            case Op::CleU4: return IrCond::Ls;
            case Op::CgtU4: return IrCond::Hi;
            case Op::CgeU4: return IrCond::Hs;
            case Op::CeqI8: width = 64; return IrCond::Eq;
            case Op::CneI8: width = 64; return IrCond::Ne;
            case Op::CltI8: width = 64; return IrCond::Lt;
            case Op::CleI8: width = 64; return IrCond::Le;
            case Op::CgtI8: width = 64; return IrCond::Gt;
            case Op::CgeI8: width = 64; return IrCond::Ge;
            case Op::CltU8: width = 64; return IrCond::Lo;
            case Op::CleU8: width = 64; return IrCond::Ls;
            case Op::CgtU8: width = 64; return IrCond::Hi;
            case Op::CgeU8: width = 64; return IrCond::Hs;
            case Op::CeqR8: fp = true; return IrCond::FEq;
            case Op::CneR8: fp = true; return IrCond::FNe;
            case Op::CltR8: fp = true; return IrCond::FLt;
            case Op::CleR8: fp = true; return IrCond::FLe;
            case Op::CgtR8: fp = true; return IrCond::FGt;
            default: fp = true; return IrCond::FGe;
        }
    }

    static std::int64_t asWidth(std::int64_t v, std::uint8_t width) noexcept
    {
        return width == 32 ? std::int64_t(std::int32_t(v)) : v;
    }

    static bool evalIntCompare(IrCond c, std::int64_t a, std::int64_t b, std::uint8_t width) noexcept
    {
        const std::uint64_t ua = width == 32 ? std::uint64_t(std::uint32_t(a)) : std::uint64_t(a);
        const std::uint64_t ub = width == 32 ? std::uint64_t(std::uint32_t(b)) : std::uint64_t(b);
        const std::int64_t sa = asWidth(a, width), sb = asWidth(b, width);
        switch (c) {
            case IrCond::Eq: return sa == sb;
            case IrCond::Ne: return sa != sb;
            case IrCond::Lt: return sa < sb;
            case IrCond::Le: return sa <= sb;
            case IrCond::Gt: return sa > sb;
            case IrCond::Ge: return sa >= sb;
            case IrCond::Lo: return ua < ub;
            case IrCond::Ls: return ua <= ub;
            case IrCond::Hi: return ua > ub;
            default: return ua >= ub;
        }
    }

    void lowerFusedBranch(Op op, bool whenTrue, std::uint32_t target, std::uint32_t pc)
    {
        Entry b = pop();
        Entry a = pop();
        requireNumber(a);
        requireNumber(b);
        std::uint8_t width = 32;
        bool fp = false;
        IrCond cond = compareCond(op, width, fp);
        if (target <= pc)
            emitPoll();
        if (fp) {
            const Cls c = classOf(a.type);
            // `!(a < b)` is not `a >= b` when a NaN is about, so the false sense jumps around.
            if (whenTrue) {
                const std::int32_t ra = reg(a), rb = reg(b);
                IrInst& br = inst(IrOp::BrFCmp);
                br.a = ra;
                br.b = rb;
                fixFpBranch(br, cond, c, labelFor(target));
            } else {
                const std::int32_t skip = newLabel();
                const std::int32_t ra = reg(a), rb = reg(b);
                IrInst& br = inst(IrOp::BrFCmp);
                br.a = ra;
                br.b = rb;
                fixFpBranch(br, cond, c, skip);
                inst(IrOp::Br).imm = labelFor(target);
                inst(IrOp::Label).imm = skip;
            }
            return;
        }
        if (a.isConst && b.isConst) {
            if (evalIntCompare(cond, a.imm, b.imm, width) == whenTrue) {
                inst(IrOp::Br).imm = labelFor(target);
                // What follows is reachable only by a label, but lowering it is harmless.
            }
            return;
        }
        if (a.isConst) {
            std::swap(a, b);
            cond = swapOperands(cond);
        }
        const std::int32_t ra = reg(a);
        const std::int32_t rb = b.isConst ? -1 : reg(b);
        IrInst& br = inst(IrOp::BrCmp);
        br.a = ra;
        if (b.isConst) {
            br.hasImm = true;
            br.imm = asWidth(b.imm, width);
        } else {
            br.b = rb;
        }
        br.cond = whenTrue ? cond : invert(cond);
        br.width = width;
        br.target = labelFor(target);
    }

    static void fixFpBranch(IrInst& br, IrCond cond, Cls c, std::int32_t target)
    {
        br.cond = cond;
        br.width = c == Cls::F32 ? 32 : 64;
        br.target = target;
    }

    static IrCond swapOperands(IrCond c) noexcept
    {
        switch (c) {
            case IrCond::Lt: return IrCond::Gt;
            case IrCond::Le: return IrCond::Ge;
            case IrCond::Gt: return IrCond::Lt;
            case IrCond::Ge: return IrCond::Le;
            case IrCond::Lo: return IrCond::Hi;
            case IrCond::Ls: return IrCond::Hs;
            case IrCond::Hi: return IrCond::Lo;
            case IrCond::Hs: return IrCond::Ls;
            default: return c;
        }
    }

    // ---- returning ----
    /// Release a scope's owned locals, except `keep`.
    void releaseLocals(Scope& s, std::int32_t keepSlot)
    {
        for (std::size_t slot = 0; slot < s.localVreg.size(); ++slot) {
            if (!s.localOwned[slot] || s.localVreg[slot] < 0 || std::int32_t(slot) == keepSlot)
                continue;
            decRefOf(s.localVreg[slot], s.localTag[slot], true);
        }
    }

    void lowerReturn(bool hasValue)
    {
        Scope& s = *scope_;
        std::int32_t value = -1;
        std::int32_t valueTag = -1;
        std::int32_t keep = -1;
        bool nonNull = false;
        if (hasValue) {
            Entry e = pop();
            const Cls c = s.method->returnType ? classOf(s.method->returnType) : Cls::Void;
            if (c != Cls::Void)
                coerceTo(e, s.method->returnType);
            if (c == Cls::Ref || c == Cls::Obj) {
                if (e.own == Own::Local && e.local >= 0 && s.localOwned[std::size_t(e.local)]) {
                    // `return local`: the count moves with it.
                    keep = e.local;
                    e.own = Own::Owned;
                } else {
                    makeOwned(e);
                }
                nonNull = e.nonNull;
            }
            value = reg(e);
            valueTag = e.tagVreg;
            if (s.parent) {
                IrInst& mov = inst(IrOp::Mov);
                mov.d = s.resultVreg;
                mov.a = value;
                value = s.resultVreg;
                if (c == Cls::Obj) {
                    IrInst& tagMov = inst(IrOp::Mov);
                    tagMov.d = s.resultTag;
                    tagMov.a = valueTag;
                }
            }
        }
        // Anything left on the stack at a return belongs to nobody else.
        while (!s.stack.empty()) {
            Entry dropped = pop();
            release(dropped);
        }
        releaseLocals(s, keep);
        if (!s.parent && exceptionVreg_ >= 0) {
            IrInst& dec = inst(IrOp::DecRef);
            dec.a = exceptionVreg_;
            dec.isNullable = true;
        }
        if (s.parent) {
            s.resultNonNull = s.resultNonNull && nonNull;
            inst(IrOp::Br).imm = s.exitLabel;
            return;
        }
        IrInst& ret = inst(IrOp::Ret);
        ret.a = value;
        ret.b = valueTag;
    }

    void lowerThrow()
    {
        Entry thrown = pop();
        const std::int32_t ptr = reg(thrown);
        if (thrown.tagVreg >= 0 && thrown.own == Own::Owned)
            extraTagged_ = {{thrown.vreg, thrown.tagVreg}};
        IrCall call;
        call.kind = IrCallKind::Helper;
        call.helper = reinterpret_cast<void*>(&scriptJitThrow);
        call.args = {rtReg(), ptr};
        call.publishFrame = true;
        call.site = site();
        emitCall(std::move(call));
        // The helper recorded the exception: leave the way a failed call does.
        std::vector<std::int32_t> extra;
        if (thrown.own == Own::Owned && thrown.tagVreg < 0)
            extra.push_back(thrown.vreg);
        const std::int32_t at = std::int32_t(fn_.code.size());
        IrInst& always = inst(IrOp::ExitCmp);
        always.a = ptr;
        always.b = ptr;
        always.cond = IrCond::Eq;
        always.width = 64;
        const std::int32_t exit = makeExit(0, extra);
        extraTagged_.clear();
        fn_.code[std::size_t(at)].target = exit;
        inst(IrOp::Trap);
        scope_->stack.clear();
    }

    /// The runtime pointer, as a value a helper can take. The back end keeps it pinned in a register of its own, and
    /// recognises this virtual register.
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

    void emitCall(IrCall call)
    {
        fn_.calls.push_back(std::move(call));
        inst(IrOp::Call).imm = std::int64_t(fn_.calls.size()) - 1;
        forgetBoundsFacts();
    }

    void forgetBoundsFacts()
    {
        lastBounds_ = {};
        for (Scope* s = scope_; s; s = s->parent)
            s->thisIsFresh = false;
    }

    /// A helper that returns a cell (or nothing) and may raise.
    std::int32_t callHelper(void* helper, std::vector<std::int32_t> args, bool hasResult,
                            const std::vector<std::int32_t>& extraCleanup = {})
    {
        IrCall call;
        call.kind = IrCallKind::Helper;
        call.helper = helper;
        call.args = std::move(args);
        call.publishFrame = true;
        call.site = site();
        call.exit = makeExit(0, extraCleanup);
        if (hasResult)
            call.result = fn_.newVreg(false);
        const std::int32_t result = call.result;
        emitCall(std::move(call));
        return result;
    }

    /// A new instance, popped off its size's free list and written here while the heap has one ready and nothing is due;
    /// `scriptJitNewInstance` otherwise.
    std::int32_t newInstanceCell(const TypeInfo* type, const std::vector<std::int32_t>& extraCleanup)
    {
        ContextImpl& ctx = rt_.ctx;
        constexpr std::size_t kMostBytes = 256;
        const auto helper = [&] {
            return callHelper(reinterpret_cast<void*>(&scriptJitNewInstance), {rtReg(), ptrReg(type)}, true, extraCleanup);
        };
        if (!mem::LocalHeap::kCanPopInline || ctx.options.memoryLimit || !type || CSInstance::bytesFor(type->instanceSlots) > kMostBytes)
            return helper();
        // Its bytes are worked out by making one, the first time.
        if (type->instanceImage.empty()) {
            try {
                detail::decRef(rt_.newInstance(type));
            } catch (...) {
                return helper();
            }
            if (type->instanceImage.empty())
                return helper();
        }
        const std::size_t bytes = type->instanceImage.size() * 8;
        const std::int32_t result = fn_.newVreg(false);
        const std::int32_t slow = newLabel();
        const std::int32_t done = newLabel();
        const auto word = [&](std::int32_t base, std::size_t offset, std::uint8_t width = 8) {
            const std::int32_t d = fn_.newVreg(false);
            IrInst& load = inst(IrOp::Load);
            load.d = d;
            load.a = base;
            load.imm = std::int64_t(offset);
            load.width = width;
            return d;
        };
        const auto put = [&](std::int32_t base, std::size_t offset, std::int32_t value, std::uint8_t width = 8) {
            IrInst& store = inst(IrOp::Store);
            store.a = base;
            store.c = value;
            store.imm = std::int64_t(offset);
            store.width = width;
        };
        const auto plus = [&](std::int32_t a, std::int64_t imm, std::uint8_t width = 64) {
            const std::int32_t d = fn_.newVreg(false);
            IrInst& add = inst(IrOp::Add);
            add.d = d;
            add.a = a;
            add.hasImm = true;
            add.imm = imm;
            add.width = width;
            return d;
        };
        const auto slowIfNotBelow = [&](std::int32_t a, std::int32_t b) {
            IrInst& test = inst(IrOp::BrCmp);
            test.a = a;
            test.b = b;
            test.cond = IrCond::Hs;
            test.width = 64;
            test.target = slow;
        };
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
#endif
        // What `maybeCollect` asks.
        const std::int32_t heap = ptrReg(&ctx);
        const std::int32_t heapBytes = word(heap, offsetof(ContextImpl, heapBytes));
        const std::int32_t budget = word(heap, offsetof(ContextImpl, nextGcBytes));
        slowIfNotBelow(heapBytes, budget);
        const std::int32_t liveObjects = word(heap, offsetof(ContextImpl, liveObjects));
        const std::int32_t liveStrings = word(heap, offsetof(ContextImpl, liveStrings));
        const std::int32_t cells = fn_.newVreg(false);
        {
            IrInst& add = inst(IrOp::Add);
            add.d = cells;
            add.a = liveObjects;
            add.b = liveStrings;
            add.width = 64;
        }
        const std::int32_t cellBudget = word(heap, offsetof(ContextImpl, nextGcCells));
        slowIfNotBelow(cells, cellBudget);
        // What `LocalHeap::allocate` does while the current block has a free slot, or one not yet carved.
        const std::int32_t block = word(ptrReg(ctx.heap.currentBlockOf(bytes)), 0);
        const std::int32_t slot = fn_.newVreg(false);
        {
            const std::int32_t carve = newLabel();
            const std::int32_t got = newLabel();
            const std::int32_t free = word(block, mem::LocalHeap::kBlockFreeOffset);
            IrInst& test = inst(IrOp::BrCmp);
            test.a = free;
            test.cond = IrCond::Eq;
            test.hasImm = true;
            test.imm = 0;
            test.width = 64;
            test.target = carve;
            put(block, mem::LocalHeap::kBlockFreeOffset, word(free, 0));
            IrInst& fromFree = inst(IrOp::Mov);
            fromFree.d = slot;
            fromFree.a = free;
            inst(IrOp::Br).imm = got;
            inst(IrOp::Label).imm = carve;
            const std::int32_t bump = word(block, mem::LocalHeap::kBlockBumpOffset);
            slowIfNotBelow(bump, word(block, mem::LocalHeap::kBlockEndOffset));
            const std::int32_t step = word(block, mem::LocalHeap::kBlockSlotBytesOffset, 4);
            const std::int32_t after = fn_.newVreg(false);
            IrInst& add = inst(IrOp::Add);
            add.d = after;
            add.a = bump;
            add.b = step;
            add.width = 64;
            put(block, mem::LocalHeap::kBlockBumpOffset, after);
            IrInst& fromBump = inst(IrOp::Mov);
            fromBump.d = slot;
            fromBump.a = bump;
            inst(IrOp::Label).imm = got;
        }
        const std::int32_t live = word(block, mem::LocalHeap::kBlockLiveOffset, 4);
        put(block, mem::LocalHeap::kBlockLiveOffset, plus(live, 1, 32), 4);
        // What the constructors and `linkKnown` do.
        for (std::size_t i = 0; i < type->instanceImage.size(); ++i)
            put(slot, i * 8, immReg(std::int64_t(type->instanceImage[i])));
        put(heap, offsetof(ContextImpl, heapBytes), plus(heapBytes, std::int64_t(bytes)));
        const std::int32_t total = word(heap, offsetof(ContextImpl, totalAllocated));
        put(heap, offsetof(ContextImpl, totalAllocated), plus(total, std::int64_t(bytes)));
        put(heap, offsetof(ContextImpl, liveObjects), plus(liveObjects, 1));
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
        {
            IrInst& mov = inst(IrOp::Mov);
            mov.d = result;
            mov.a = slot;
        }
        inst(IrOp::ColdBegin);
        inst(IrOp::Label).imm = slow;
        const std::int32_t made = helper();
        fn_.calls.back().cold = true;
        {
            IrInst& mov = inst(IrOp::Mov);
            mov.d = result;
            mov.a = made;
        }
        inst(IrOp::Br).imm = done;
        inst(IrOp::ColdEnd);
        inst(IrOp::Label).imm = done;
        return result;
    }

    /// A helper that cannot raise.
    std::int32_t callPure(void* helper, std::vector<std::int32_t> args, bool fpResult, bool hasResult = true)
    {
        IrCall call;
        call.kind = IrCallKind::Helper;
        call.helper = helper;
        call.args = std::move(args);
        if (hasResult)
            call.result = fn_.newVreg(fpResult);
        const std::int32_t result = call.result;
        emitCall(std::move(call));
        return result;
    }

    // ---- instructions ----
    const TypeInfo* fieldType(const TypeInfo* owner, std::uint32_t slot) const
    {
        for (const TypeInfo* t = owner; t; t = t->base) {
            for (const FieldInfo& f : t->fields) {
                if (f.slot == slot && !f.isStatic)
                    return f.type;
            }
        }
        refuse("a field access could not be resolved");
    }

    static std::uint8_t payloadBytes(Cls c) noexcept { return c == Cls::I32 || c == Cls::F32 ? 4 : 8; }

    Entry loadPayload(std::int32_t base, std::int64_t offset, const TypeInfo* type, Own refOwn)
    {
        const Cls c = classOf(type);
        const std::int32_t d = fn_.newVreg(isFpClass(c));
        if (c == Cls::Obj) {
            const std::int32_t tag = fn_.newVreg(false);
            IrInst& tagLoad = inst(IrOp::Load);
            tagLoad.d = tag;
            tagLoad.a = base;
            tagLoad.imm = offset - kPayloadOffset;
            tagLoad.width = 1;
            IrInst& load = inst(IrOp::Load);
            load.d = d;
            load.a = base;
            load.imm = offset;
            load.width = 8;
            Entry e = refEntry(d, type, refOwn);
            e.tagVreg = tag;
            return e;
        }
        IrInst& load = inst(IrOp::Load);
        load.d = d;
        load.a = base;
        load.imm = offset;
        load.width = payloadBytes(c);
        Entry e = c == Cls::Ref ? refEntry(d, type, refOwn) : valueEntry(d, type);
        return e;
    }

    /// Store to a `Value` in memory that already holds a value of this static type, releasing what it held.
    void storeValueTo(std::int32_t base, std::int64_t valueOffset, const TypeInfo* type, Entry value, bool fresh = false)
    {
        const Cls c = classOf(type);
        coerceTo(value, type);
        // Null into a field of a new object is what is already there.
        if (fresh && c == Cls::Ref && value.isConst)
            return;
        if (c != Cls::Ref && c != Cls::Obj) {
            const std::int32_t v = reg(value);
            IrInst& store = inst(IrOp::Store);
            store.a = base;
            store.c = v;
            store.imm = valueOffset + kPayloadOffset;
            store.width = payloadBytes(c);
            return;
        }
        secureWeak();
        // The caller's count on an argument moves into the field, when that was the argument's whole purpose.
        if (value.own == Own::Local && value.local >= 0 && !scope_->consumable.empty() &&
            std::size_t(value.local) < scope_->consumable.size() && scope_->consumable[std::size_t(value.local)]) {
            const std::size_t param = std::size_t(value.local);
            scope_->consumable[param] = 0;
            scope_->consumed[param] = 1;
            std::vector<std::int32_t>& held = scope_->parent->held;
            const auto it = std::find(held.begin(), held.end(), scope_->argHeldVreg[param]);
            if (it != held.end())
                held.erase(it);
            value.own = Own::Owned;
        }
        makeOwned(value);
        const std::int32_t v = reg(value);
        if (c == Cls::Obj) {
            callPure(reinterpret_cast<void*>(&scriptJitStoreTagged), {addOffset(base, valueOffset), value.tagVreg, v}, false,
                     false);
            return;
        }
        std::int32_t old = -1;
        if (!fresh) {
            old = fn_.newVreg(false);
            IrInst& load = inst(IrOp::Load);
            load.d = old;
            load.a = base;
            load.imm = valueOffset + kPayloadOffset;
            load.width = 8;
        }
        IrInst& store = inst(IrOp::Store);
        store.a = base;
        store.c = v;
        store.imm = valueOffset + kPayloadOffset;
        store.width = 8;
        IrInst& tag = inst(IrOp::StoreTag);
        tag.a = base;
        tag.c = value.nonNull ? -1 : v;
        tag.tag = refTag(type);
        tag.imm = valueOffset;
        if (fresh)
            return;
        IrInst& dec = inst(IrOp::DecRef);
        dec.a = old;
        dec.isNullable = true;
    }

    std::int32_t addOffset(std::int32_t base, std::int64_t offset)
    {
        const std::int32_t d = fn_.newVreg(false);
        IrInst& add = inst(IrOp::Add);
        add.d = d;
        add.a = base;
        add.hasImm = true;
        add.imm = offset;
        add.width = 64;
        return d;
    }

    void lowerLdFld(std::uint32_t slot)
    {
        Entry obj = pop();
        const TypeInfo* owner = obj.type;
        if (!owner || owner->nativeKind == NativeKind::List || owner->nativeKind == NativeKind::Dictionary ||
            owner->nativeKind == NativeKind::StringBuilder || owner->nativeKind == NativeKind::Host)
            refuse("a field of a type the engine implements");
        const TypeInfo* type = fieldType(owner, slot);
        {
            const Held held(*this, {&obj});
            checkNotNull(obj);
        }
        const std::int64_t offset = instanceSlotOffset(slot) + kPayloadOffset;
        Entry result = loadPayload(obj.vreg, offset, type, Own::Weak);
        if (obj.own == Own::Owned) {
            // The object goes away right after this read, so what it held must first get a count of its own.
            if (result.own != Own::None)
                makeOwned(result);
            push(result);
            release(obj);
            return;
        }
        push(result);
    }

    void lowerStFld(std::uint32_t slot)
    {
        Entry value = pop();
        Entry obj = pop();
        const TypeInfo* owner = obj.type;
        if (!owner || (owner->nativeKind != NativeKind::None && owner->nativeKind != NativeKind::Tuple))
            refuse("a field of a type the engine implements");
        const TypeInfo* type = fieldType(owner, slot);
        {
            const Held held(*this, {&obj, &value});
            checkNotNull(obj);
        }
        bool fresh = false;
        if (scope_->thisIsFresh && obj.local == 0 && slot < scope_->freshField.size() && scope_->freshField[slot]) {
            scope_->freshField[slot] = 0;
            fresh = true;
        }
        storeValueTo(obj.vreg, instanceSlotOffset(slot), type, value, fresh);
        release(obj);
    }

    const TypeInfo* elementOf(const Entry& array) const
    {
        if (!array.type || baseKind(array.type) != TypeKind::Array || !array.type->element)
            refuse("an array of no known element type");
        return array.type->element;
    }

    struct ElemAccess
    {
        ElemStore store;
        std::uint8_t bytes;
        bool isSigned;
        std::uint8_t scale;
    };

    static ElemAccess elemAccess(const TypeInfo* element) noexcept
    {
        const ElemStore store = elemStoreOf(element);
        const TypeKind k = baseKind(element);
        switch (store) {
            case ElemStore::I1: return {store, 1, k == TypeKind::SByte, 0};
            case ElemStore::I2: return {store, 2, k == TypeKind::Short, 1};
            case ElemStore::I4: return {store, 4, false, 2};
            case ElemStore::I8: return {store, 8, false, 3};
            case ElemStore::R4: return {store, 4, false, 2};
            case ElemStore::R8: return {store, 8, false, 3};
            default: return {store, 8, false, 4};
        }
    }

    /// Bounds check, with the array known not to be null. The same array and index again in a row skip it.
    void checkBounds(const Entry& array, const Entry& index, std::int32_t indexReg)
    {
        if (lastBounds_.array == array.vreg && lastBounds_.index == indexReg && lastBounds_.array >= 0)
            return;
        const std::int32_t len = fn_.newVreg(false);
        IrInst& load = inst(IrOp::Load);
        load.d = len;
        load.a = array.vreg;
        load.imm = kArrayLengthOffset;
        load.width = 4;
        const std::int32_t at = std::int32_t(fn_.code.size());
        inst(IrOp::ExitCmp);
        pendingIndex_ = indexReg;
        const std::int32_t exit = makeExit(std::uint32_t(ErrorCode::IndexOutOfRange));
        pendingIndex_ = -1;
        IrInst& check = fn_.code[std::size_t(at)];
        check.a = indexReg;
        check.b = len;
        check.cond = IrCond::Hs;
        check.width = 32;
        check.target = exit;
        (void)index;
        lastBounds_.array = array.vreg;
        lastBounds_.index = indexReg;
    }

    /// An index the bounds check will treat as a 32-bit value.
    std::int32_t indexRegister(Entry& index)
    {
        return reg(index);
    }

    void lowerLdElem()
    {
        Entry index = pop();
        Entry array = pop();
        const TypeInfo* element = elementOf(array);
        std::int32_t i;
        {
            const Held held(*this, {&array});
            checkNotNull(array);
            i = indexRegister(index);
            checkBounds(array, index, i);
        }
        const ElemAccess access = elemAccess(element);
        const Cls c = classOf(element);
        // An index goes through a 64-bit address calculation.
        const std::int32_t wide = fn_.newVreg(false);
        IrInst& ext = inst(IrOp::Zext32);
        ext.d = wide;
        ext.a = i;
        if (access.store == ElemStore::Ref) {
            const std::int32_t scaled = fn_.newVreg(false);
            IrInst& shift = inst(IrOp::Shl);
            shift.d = scaled;
            shift.a = wide;
            shift.hasImm = true;
            shift.imm = 4;
            shift.width = 64;
            const std::int32_t d = fn_.newVreg(false);
            IrInst& load = inst(IrOp::LoadIdx);
            load.d = d;
            load.a = array.vreg;
            load.b = scaled;
            load.scale = 0;
            load.imm = kArrayDataOffset + kPayloadOffset;
            load.width = 8;
            Entry result = refEntry(d, element, Own::Weak);
            if (c == Cls::Obj) {
                const std::int32_t tag = fn_.newVreg(false);
                IrInst& tagLoad = inst(IrOp::LoadIdx);
                tagLoad.d = tag;
                tagLoad.a = array.vreg;
                tagLoad.b = scaled;
                tagLoad.scale = 0;
                tagLoad.imm = kArrayDataOffset;
                tagLoad.width = 1;
                result.tagVreg = tag;
            }
            if (array.own == Own::Owned) {
                makeOwned(result);
                push(result);
                release(array);
            } else {
                push(result);
            }
            return;
        }
        const std::int32_t d = fn_.newVreg(isFpClass(c));
        IrInst& load = inst(IrOp::LoadIdx);
        load.d = d;
        load.a = array.vreg;
        load.b = wide;
        load.scale = access.scale;
        load.imm = kArrayDataOffset;
        load.width = access.bytes;
        load.isSigned = access.isSigned;
        Entry result = valueEntry(d, element);
        if (baseKind(element) == TypeKind::Bool) {
            // A bool element is read as "not zero".
            const std::int32_t b = fn_.newVreg(false);
            IrInst& cset = inst(IrOp::Cset);
            cset.d = b;
            cset.a = d;
            cset.hasImm = true;
            cset.imm = 0;
            cset.cond = IrCond::Ne;
            cset.width = 32;
            result = valueEntry(b, element);
        }
        push(result);
        release(array);
    }

    void lowerStElem()
    {
        Entry value = pop();
        Entry index = pop();
        Entry array = pop();
        const TypeInfo* element = elementOf(array);
        std::int32_t i;
        {
            const Held held(*this, {&array, &value});
            checkNotNull(array);
            i = indexRegister(index);
            checkBounds(array, index, i);
        }
        const ElemAccess access = elemAccess(element);
        const std::int32_t wide = fn_.newVreg(false);
        IrInst& ext = inst(IrOp::Zext32);
        ext.d = wide;
        ext.a = i;
        if (access.store == ElemStore::Ref) {
            const std::int32_t scaled = fn_.newVreg(false);
            IrInst& shift = inst(IrOp::Shl);
            shift.d = scaled;
            shift.a = wide;
            shift.hasImm = true;
            shift.imm = 4;
            shift.width = 64;
            const std::int32_t base = fn_.newVreg(false);
            IrInst& add = inst(IrOp::Add);
            add.d = base;
            add.a = array.vreg;
            add.b = scaled;
            add.width = 64;
            storeValueTo(base, kArrayDataOffset, element, value);
            release(array);
            return;
        }
        const std::int32_t v = reg(value);
        IrInst& store = inst(IrOp::StoreIdx);
        store.a = array.vreg;
        store.b = wide;
        store.c = v;
        store.scale = access.scale;
        store.imm = kArrayDataOffset;
        store.width = access.bytes;
        release(array);
    }

    void pushLocal(std::uint32_t slot)
    {
        Scope& s = *scope_;
        if (slot >= s.localVreg.size())
            refuse("a local is out of range");
        const TypeInfo* type = s.localType[slot];
        const Cls c = slotClass(s, slot);
        if (s.localConstKnown[slot]) {
            push(constInt(s.localConst[slot], type));
            return;
        }
        Entry e;
        e.vreg = s.localVreg[slot];
        e.tagVreg = s.localTag[slot];
        e.type = type;
        e.local = std::int32_t(slot);
        if (c == Cls::Ref || c == Cls::Obj) {
            e.own = Own::Local;
            e.nonNull = s.localNonNull[slot] != 0;
        }
        push(e);
    }

    void storeLocal(std::uint32_t slot)
    {
        Scope& s = *scope_;
        if (slot >= s.localVreg.size())
            refuse("a local is out of range");
        Entry value = pop();
        const Cls c = slotClass(s, slot);
        if (lastBounds_.array == s.localVreg[slot] || lastBounds_.index == s.localVreg[slot])
            lastBounds_ = {};
        // Anything still reading this local keeps the old value.
        for (Entry& e : s.stack) {
            if (e.local == std::int32_t(slot))
                copyOutOfLocal(e);
        }
        coerceTo(value, s.localType[slot]);
        if (c != Cls::Ref && c != Cls::Obj) {
            s.localConstKnown[slot] = 0;
            if (value.isConst) {
                const Cls vc = value.type ? classOf(value.type) : c;
                if (isFpClass(vc)) {
                    IrInst& in = inst(IrOp::FMovImm);
                    in.d = s.localVreg[slot];
                    in.width = vc == Cls::F32 ? 32 : 64;
                    in.imm = value.imm;
                } else {
                    IrInst& in = inst(IrOp::MovImm);
                    in.d = s.localVreg[slot];
                    in.imm = value.imm;
                }
                return;
            }
            // Peephole: the instruction that made the value can write the local directly.
            if (value.local < 0 && !fn_.code.empty() && fn_.code.back().d == value.vreg &&
                fn_.code.back().op != IrOp::Call && !isLabelLike(fn_.code.back()) && !isOnAnyStack(value.vreg)) {
                fn_.code.back().d = s.localVreg[slot];
                return;
            }
            IrInst& mov = inst(IrOp::Mov);
            mov.d = s.localVreg[slot];
            mov.a = value.vreg;
            return;
        }
        // A reference: the new value gets its count before the old one loses its own, which also covers `x = x`.
        secureWeak();
        const bool nonNull = value.nonNull;
        makeOwned(value);
        const std::int32_t v = reg(value);
        const bool wasOwned = s.localOwned[slot] != 0;
        if (!wasOwned)
            refuse("a borrowed reference local is assigned");
        const std::int32_t old = fn_.newVreg(false);
        IrInst& save = inst(IrOp::Mov);
        save.d = old;
        save.a = s.localVreg[slot];
        IrInst& mov = inst(IrOp::Mov);
        mov.d = s.localVreg[slot];
        mov.a = v;
        s.localNonNull[slot] = nonNull ? 1 : 0;
        std::int32_t oldTag = -1;
        if (c == Cls::Obj) {
            oldTag = fn_.newVreg(false);
            IrInst& saveTag = inst(IrOp::Mov);
            saveTag.d = oldTag;
            saveTag.a = s.localTag[slot];
            IrInst& movTag = inst(IrOp::Mov);
            movTag.d = s.localTag[slot];
            movTag.a = value.tagVreg;
        }
        decRefOf(old, oldTag, true);
    }

    bool isOnAnyStack(std::int32_t vreg) const noexcept
    {
        for (const Scope* s = scope_; s; s = s->parent) {
            for (const Entry& e : s->stack) {
                if (e.vreg == vreg && !e.isConst)
                    return true;
            }
        }
        return false;
    }

    static bool isLabelLike(const IrInst& in) noexcept
    {
        return in.op == IrOp::Label || in.op == IrOp::OsrEntry;
    }

    /// An operand of an arithmetic instruction: never `object`, whose bits may be of any type.
    void requireNumber(const Entry& e) const
    {
        if (e.type && classOf(e.type) == Cls::Obj)
            refuse("arithmetic on a value whose static type is object");
    }

    void binaryInt(IrOp op, std::uint8_t width, const TypeInfo* type)
    {
        Entry b = pop();
        Entry a = pop();
        requireNumber(a);
        requireNumber(b);
        if (a.isConst && b.isConst && foldInt(op, width, a.imm, b.imm, type))
            return;
        const bool commutes = op == IrOp::Add || op == IrOp::Mul || op == IrOp::And || op == IrOp::Or || op == IrOp::Xor;
        if (a.isConst && commutes)
            std::swap(a, b);
        const std::int32_t ra = reg(a);
        const bool useImm = b.isConst && op != IrOp::Mul;
        const std::int32_t rb = useImm ? -1 : reg(b);
        const std::int32_t d = fn_.newVreg(false);
        IrInst& in = inst(op);
        in.d = d;
        in.a = ra;
        in.b = rb;
        in.width = width;
        if (useImm) {
            in.hasImm = true;
            in.imm = asWidth(b.imm, width);
        }
        push(valueEntry(d, type));
    }

    bool foldInt(IrOp op, std::uint8_t width, std::int64_t a, std::int64_t b, const TypeInfo* type)
    {
        const std::uint64_t ua = std::uint64_t(a), ub = std::uint64_t(b);
        const int shift = int(b) & (width - 1);
        std::uint64_t r = 0;
        switch (op) {
            case IrOp::Add: r = ua + ub; break;
            case IrOp::Sub: r = ua - ub; break;
            case IrOp::Mul: r = ua * ub; break;
            case IrOp::And: r = ua & ub; break;
            case IrOp::Or: r = ua | ub; break;
            case IrOp::Xor: r = ua ^ ub; break;
            case IrOp::Shl: r = width == 32 ? std::uint64_t(std::uint32_t(ua) << shift) : ua << shift; break;
            case IrOp::Sar: r = width == 32 ? std::uint64_t(std::int64_t(std::int32_t(ua) >> shift))
                                            : std::uint64_t(a >> shift);
                break;
            case IrOp::Shr: r = width == 32 ? std::uint64_t(std::uint32_t(ua) >> shift) : ua >> shift; break;
            default: return false;
        }
        push(constInt(asWidth(std::int64_t(r), width), type));
        return true;
    }

    void binaryFp(IrOp op, Cls c, const TypeInfo* type)
    {
        Entry b = pop();
        Entry a = pop();
        requireNumber(a);
        requireNumber(b);
        const std::int32_t ra = reg(a);
        const std::int32_t rb = reg(b);
        const std::int32_t d = fn_.newVreg(true);
        IrInst& in = inst(op);
        in.d = d;
        in.a = ra;
        in.b = rb;
        in.width = c == Cls::F32 ? 32 : 64;
        push(valueEntry(d, type));
    }

    void divide(IrOp op, std::uint8_t width, const TypeInfo* type)
    {
        Entry b = pop();
        Entry a = pop();
        requireNumber(a);
        requireNumber(b);
        if (b.isConst && asWidth(b.imm, width) == 0) {
            b.vreg = reg(b);
            b.isConst = false;
        }
        if (!b.isConst) {
            const std::int32_t at = std::int32_t(fn_.code.size());
            inst(IrOp::ExitCmp);
            const std::int32_t exit = makeExit(std::uint32_t(ErrorCode::DivideByZero));
            IrInst& check = fn_.code[std::size_t(at)];
            check.a = b.vreg;
            check.cond = IrCond::Eq;
            check.width = width;
            check.hasImm = true;
            check.imm = 0;
            check.target = exit;
        }
        const std::int32_t ra = reg(a);
        const std::int32_t d = fn_.newVreg(false);
        const std::int32_t rb = b.isConst ? -1 : b.vreg;
        IrInst& in = inst(op);
        in.d = d;
        in.a = ra;
        in.b = rb;
        in.width = width;
        if (b.isConst) {
            in.hasImm = true;
            in.imm = asWidth(b.imm, width);
        }
        push(valueEntry(d, type));
    }

    void compareToValue(Op op)
    {
        std::uint8_t width = 32;
        bool fp = false;
        const IrCond cond = compareCond(op, width, fp);
        Entry b = pop();
        Entry a = pop();
        requireNumber(a);
        requireNumber(b);
        const std::int32_t d = fn_.newVreg(false);
        if (fp) {
            const Cls c = classOf(a.type);
            const std::int32_t ra = reg(a), rb = reg(b);
            IrInst& in = inst(IrOp::FCset);
            in.d = d;
            in.a = ra;
            in.b = rb;
            in.cond = cond;
            in.width = c == Cls::F32 ? 32 : 64;
            push(valueEntry(d, core_.boolType));
            return;
        }
        if (a.isConst && b.isConst) {
            fn_.vregIsFp.pop_back();
            push(constInt(evalIntCompare(cond, a.imm, b.imm, width) ? 1 : 0, core_.boolType));
            return;
        }
        const std::int32_t ra = reg(a);
        const std::int32_t rb = b.isConst ? -1 : reg(b);
        IrInst& in = inst(IrOp::Cset);
        in.d = d;
        in.a = ra;
        in.b = rb;
        in.cond = cond;
        in.width = width;
        if (b.isConst) {
            in.hasImm = true;
            in.imm = asWidth(b.imm, width);
        }
        push(valueEntry(d, core_.boolType));
    }

    void unaryInt(IrOp op, std::uint8_t width, const TypeInfo* type)
    {
        Entry a = pop();
        requireNumber(a);
        const std::int32_t ra = reg(a);
        const std::int32_t d = fn_.newVreg(false);
        IrInst& in = inst(op);
        in.d = d;
        in.a = ra;
        in.width = width;
        push(valueEntry(d, type));
    }

    /// Everything the source of a conversion can be; the opcode says only the destination.
    void convertInt(Op op)
    {
        Entry a = pop();
        requireNumber(a);
        const Cls from = a.type ? classOf(a.type) : Cls::I32;
        const TypeInfo* to = nullptr;
        IrOp narrow = IrOp::Mov;
        switch (op) {
            case Op::ConvI1: to = core_.sbyteType; narrow = IrOp::Sext8; break;
            case Op::ConvI2: to = core_.shortType; narrow = IrOp::Sext16; break;
            case Op::ConvU1: to = core_.byteType; narrow = IrOp::Zext8; break;
            case Op::ConvU2: to = core_.ushortType; narrow = IrOp::Zext16; break;
            case Op::ConvChar: to = core_.charType; narrow = IrOp::Zext16; break;
            case Op::ConvI4: to = core_.intType; break;
            default: to = core_.uintType; break;
        }
        if (a.isConst && !isFpClass(from)) {
            std::int64_t v = a.imm;
            switch (narrow) {
                case IrOp::Sext8: v = std::int8_t(v); break;
                case IrOp::Sext16: v = std::int16_t(v); break;
                case IrOp::Zext8: v = std::uint8_t(v); break;
                case IrOp::Zext16: v = std::uint16_t(v); break;
                default: v = std::int32_t(v); break;
            }
            push(constInt(v, to));
            return;
        }
        std::int32_t source = reg(a);
        if (isFpClass(from)) {
            const std::int32_t i = fn_.newVreg(false);
            IrInst& conv = inst(IrOp::FpToInt);
            conv.d = i;
            conv.a = source;
            conv.srcWidth = from == Cls::F32 ? 32 : 64;
            conv.width = 64;
            source = i;
        } else if (narrow == IrOp::Mov && from == Cls::I32) {
            // Same bits: only the static type changes.
            a.type = to;
            a.vreg = source;
            a.isConst = false;
            push(a);
            return;
        }
        const std::int32_t d = fn_.newVreg(false);
        IrInst& in = inst(narrow);
        in.d = d;
        in.a = source;
        in.width = 32;
        push(valueEntry(d, to));
    }

    void convertLong(Op op)
    {
        Entry a = pop();
        requireNumber(a);
        const Cls from = a.type ? classOf(a.type) : Cls::I32;
        const TypeInfo* to = op == Op::ConvI8 ? core_.longType : core_.ulongType;
        if (a.isConst && !isFpClass(from)) {
            const std::int64_t v = from == Cls::I64 ? a.imm
                                   : op == Op::ConvU8 ? std::int64_t(std::uint32_t(a.imm))
                                                      : std::int64_t(std::int32_t(a.imm));
            push(constInt(v, to));
            return;
        }
        const std::int32_t source = reg(a);
        const std::int32_t d = fn_.newVreg(false);
        if (isFpClass(from)) {
            IrInst& conv = inst(IrOp::FpToInt);
            conv.d = d;
            conv.a = source;
            conv.srcWidth = from == Cls::F32 ? 32 : 64;
            conv.width = 64;
        } else if (from == Cls::I64) {
            a.type = to;
            a.vreg = source;
            a.isConst = false;
            push(a);
            return;
        } else {
            IrInst& ext = inst(op == Op::ConvI8 ? IrOp::Sext32 : IrOp::Zext32);
            ext.d = d;
            ext.a = source;
        }
        push(valueEntry(d, to));
    }

    void convertFp(Op op)
    {
        Entry a = pop();
        requireNumber(a);
        const Cls from = a.type ? classOf(a.type) : Cls::I32;
        const bool toDouble = op != Op::ConvR4;
        const Cls toClass = toDouble ? Cls::F64 : Cls::F32;
        const TypeInfo* to = toDouble ? core_.doubleType : core_.floatType;
        if (from == toClass) {
            a.type = to;
            push(a);
            return;
        }
        const std::int32_t source = reg(a);
        const std::int32_t d = fn_.newVreg(true);
        if (isFpClass(from)) {
            IrInst& conv = inst(IrOp::FConvert);
            conv.d = d;
            conv.a = source;
            conv.width = toDouble ? 64 : 32;
        } else {
            IrInst& conv = inst(IrOp::IntToFp);
            conv.d = d;
            conv.a = source;
            conv.width = toDouble ? 64 : 32;
            conv.srcWidth = from == Cls::I64 ? 64 : 32;
            conv.isUnsigned = op == Op::ConvR8Un;
        }
        push(valueEntry(d, to));
    }

    // ---- calls ----
    bool canInline(const MethodInfo& callee, std::uint32_t argc) const
    {
        if (callee.isNative() || callee.isAbstract() || callee.body.code.empty())
            return false;
        if (callee.body.code.size() > kInlineMaxBytes || !callee.body.handlers.empty())
            return false;
        if (!callee.captureSlots.empty() || scope_->depth >= kInlineMaxDepth)
            return false;
        if (inlinedBytes_ + callee.body.code.size() > kInlineBudget)
            return false;
        if (argc != (callee.isStatic() ? 0u : 1u) + callee.params.size())
            return false;
        for (Scope* s = scope_; s; s = s->parent) {
            if (s->method == &callee)
                return false;  // recursion
        }
        for (std::uint8_t boxed : callee.body.localBoxed) {
            if (boxed)
                return false;
        }
        if (bodyHasLoop(callee.body.code))
            return false;
        return true;
    }

    /// Args have been popped. Lower the body in place; the result, if any, is pushed.
    /// True when running this method, and what it calls, writes no reference to any field, element or static, and calls
    /// nothing unknown. Optimistic about recursion, which is right: if the rest of the cycle stores nothing, neither does
    /// the call back into it.
    bool storesNoReferences(const MethodInfo& method)
    {
        const auto known = storesNothing_.find(&method);
        if (known != storesNothing_.end())
            return known->second;
        storesNothing_.emplace(&method, true);
        bool result = !method.isNative() && !method.isAbstract() && !method.body.code.empty() &&
                      method.body.handlers.empty();
        const std::vector<std::uint8_t>& code = method.body.code;
        for (std::size_t pc = 0; result && pc < code.size();) {
            const Op op = Op(code[pc]);
            switch (op) {
                case Op::StFld:
                case Op::StSFld:
                case Op::StElem:
                case Op::StBoxed:
                case Op::StInd:
                case Op::NewObj:
                case Op::NewDlg:
                case Op::CallVirt:
                case Op::CallIface:
                case Op::CallDlg:
                case Op::Throw:
                case Op::Rethrow:
                case Op::ToStr:
                case Op::Concat:
                case Op::InitObj:
                case Op::CloneVal: result = false; break;
                case Op::Call: {
                    const AssemblyImpl* assembly = method.assembly;
                    const std::uint32_t index = readU32(&code[pc + 1]);
                    const MethodInfo* callee = index < assembly->methodRefs.size() ? assembly->methodRefs[index] : nullptr;
                    result = callee && storesNoReferences(*callee);
                    break;
                }
                default: break;
            }
            pc += 1 + std::size_t(opArity(op)) * 4;
        }
        storesNothing_[&method] = result;
        return result;
    }

    /// How many times the body reads each slot, and whether it branches at all.
    static void readCounts(const MethodInfo& m, std::vector<std::uint32_t>& reads, bool& branches)
    {
        reads.assign(m.frameSize(), 0);
        branches = false;
        const std::vector<std::uint8_t>& code = m.body.code;
        for (std::size_t pc = 0; pc < code.size();) {
            const Op op = Op(code[pc]);
            if (op == Op::LdLoc) {
                const std::uint32_t slot = readU32(&code[pc + 1]);
                if (slot < reads.size())
                    ++reads[slot];
            } else if (op == Op::Br || op == Op::BrTrue || op == Op::BrFalse) {
                branches = true;
            }
            pc += 1 + std::size_t(opArity(op)) * 4;
        }
    }

    void inlineCall(const MethodInfo& callee, std::vector<Entry>& args, bool freshThis = false)
    {
        inlinedBytes_ += callee.body.code.size();
        ++fn_.inlinedMethods;
        const std::vector<std::uint8_t> assigned = assignedSlots(callee);

        Scope inner;
        inner.method = &callee;
        inner.parent = scope_;
        inner.parentSite = site();
        inner.depth = scope_->depth + 1;
        Scope* outer = scope_;
        scope_ = &inner;  // labels and vregs of the callee from here on
        prepareScope(inner);

        if (freshThis && callee.isConstructor() && callee.owner) {
            inner.thisIsFresh = true;
            inner.freshField.assign(callee.owner->instanceSlots, 1);
        }
        std::vector<std::uint32_t> reads;
        bool branches = false;
        readCounts(callee, reads, branches);
        inner.consumable.assign(args.size(), 0);
        inner.consumed.assign(args.size(), 0);
        inner.argHeldVreg.assign(args.size(), -1);

        for (std::size_t i = 0; i < args.size(); ++i) {
            Entry& arg = args[i];
            const Cls c = inner.localType[i] ? classOf(inner.localType[i]) : Cls::Ref;
            if (c == Cls::Obj) {
                coerceToObject(arg, inner.localType[i]);
                inner.localTag[i] = arg.tagVreg;
            }
            // A parameter that is never assigned can simply be the argument.
            if (!assigned[i]) {
                if (arg.isConst && !isFpClass(c) && c != Cls::Ref) {
                    inner.localConstKnown[i] = 1;
                    inner.localConst[i] = c == Cls::I32 ? asWidth(arg.imm, 32) : arg.imm;
                    continue;
                }
                inner.localVreg[i] = reg(arg);
                if (c == Cls::Obj) {
                    if (arg.own == Own::Owned)
                        outer->heldTagged.push_back({arg.vreg, arg.tagVreg});
                } else if (c == Cls::Ref) {
                    inner.localNonNull[i] = arg.nonNull ? 1 : 0;
                    if (arg.own == Own::Owned) {
                        outer->held.push_back(arg.vreg);
                        if (!branches && reads[i] == 1) {
                            inner.consumable[i] = 1;
                            inner.argHeldVreg[i] = arg.vreg;
                        }
                    }
                }
                continue;
            }
            // Otherwise a real local.
            inner.localVreg[i] = fn_.newVreg(isFpClass(c));
            if (c == Cls::Ref || c == Cls::Obj) {
                makeOwned(arg);
                inner.localOwned[i] = 1;
            }
            const std::int32_t from = reg(arg);
            IrInst& mov = inst(IrOp::Mov);
            mov.d = inner.localVreg[i];
            mov.a = from;
            if (c == Cls::Obj) {
                inner.localTag[i] = fn_.newVreg(false);
                IrInst& tagMov = inst(IrOp::Mov);
                tagMov.d = inner.localTag[i];
                tagMov.a = arg.tagVreg;
            }
        }
        for (std::uint32_t slot = std::uint32_t(args.size()); slot < inner.localVreg.size(); ++slot)
            initLocal(inner, slot);

        const Cls ret = callee.returnType ? classOf(callee.returnType) : Cls::Void;
        inner.exitLabel = newLabel();
        if (ret != Cls::Void)
            inner.resultVreg = fn_.newVreg(isFpClass(ret));
        if (ret == Cls::Obj)
            inner.resultTag = fn_.newVreg(false);
        inner.resultNonNull = true;

        lowerBody(inner);

        scope_ = outer;
        bindLabel(inner.exitLabel);
        // The counts on arguments end with the call.
        for (std::size_t i = 0; i < args.size(); ++i) {
            const Entry& arg = args[i];
            if (inner.consumed[i])
                continue;
            if (arg.own == Own::Owned && arg.tagVreg >= 0 && !arg.isConst) {
                const std::pair<std::int32_t, std::int32_t> key{arg.vreg, arg.tagVreg};
                const auto it = std::find(outer->heldTagged.begin(), outer->heldTagged.end(), key);
                if (it != outer->heldTagged.end()) {
                    outer->heldTagged.erase(it);
                    decRefOf(arg.vreg, arg.tagVreg, !arg.nonNull);
                }
                continue;
            }
            if (arg.own == Own::Owned && !arg.isConst) {
                const auto it = std::find(outer->held.begin(), outer->held.end(), arg.vreg);
                if (it != outer->held.end()) {
                    outer->held.erase(it);
                    IrInst& dec = inst(IrOp::DecRef);
                    dec.a = arg.vreg;
                    dec.isNullable = !arg.nonNull;
                }
            }
        }
        if (ret != Cls::Void) {
            if (ret == Cls::Ref || ret == Cls::Obj) {
                Entry result = refEntry(inner.resultVreg, callee.returnType, Own::Owned, inner.resultNonNull);
                result.tagVreg = inner.resultTag;
                push(result);
            } else {
                push(valueEntry(inner.resultVreg, callee.returnType));
            }
        }
    }

    bool lowerIntrinsic(const MethodInfo& target, std::vector<Entry>& args)
    {
        if (!target.isNative() || !target.owner)
            return false;
        const String& owner = target.owner->fullName;
        const String& name = target.name;
        const Cls ret = target.returnType ? classOf(target.returnType) : Cls::Void;
        if (owner == u"System.Math" && args.size() == 1 && ret == Cls::F64 && classOf(args[0].type) == Cls::F64) {
            IrOp op = IrOp::Trap;
            if (name == u"Sqrt")
                op = IrOp::FSqrt;
            else if (name == u"Abs")
                op = IrOp::FAbs;
            else if (name == u"Floor")
                op = IrOp::FFloor;
            else if (name == u"Ceiling")
                op = IrOp::FCeil;
            else if (name == u"Truncate")
                op = IrOp::FTrunc;
            else if (name == u"Round")
                op = IrOp::FRoundEven;
            if (op == IrOp::Trap)
                return false;
            const std::int32_t a = reg(args[0]);
            const std::int32_t d = fn_.newVreg(true);
            IrInst& in = inst(op);
            in.d = d;
            in.a = a;
            in.width = 64;
            push(valueEntry(d, core_.doubleType));
            return true;
        }
        if (owner == u"libc" && args.size() == 1 && ret == Cls::F64 && classOf(args[0].type) == Cls::F64) {
            IrOp op = IrOp::Trap;
            if (name == u"sqrt")
                op = IrOp::FSqrt;
            else if (name == u"fabs")
                op = IrOp::FAbs;
            else if (name == u"floor")
                op = IrOp::FFloor;
            else if (name == u"ceil")
                op = IrOp::FCeil;
            else if (name == u"trunc")
                op = IrOp::FTrunc;
            if (op == IrOp::Trap)
                return false;
            const std::int32_t a = reg(args[0]);
            const std::int32_t d = fn_.newVreg(true);
            IrInst& in = inst(op);
            in.d = d;
            in.a = a;
            in.width = 64;
            push(valueEntry(d, core_.doubleType));
            return true;
        }
        if (lowerCollectionIntrinsic(target, args))
            return true;
        if (baseKind(target.owner) == TypeKind::String && !target.isStatic()) {
            if (name == u"get_Length" && args.size() == 1) {
                checkNotNull(args[0]);
                const std::int32_t d = fn_.newVreg(false);
                IrInst& load = inst(IrOp::Load);
                load.d = d;
                load.a = args[0].vreg;
                load.imm = kStringLengthOffset;
                load.width = 4;
                push(valueEntry(d, core_.intType));
                release(args[0]);
                return true;
            }
            if (name == u"get_Chars" && args.size() == 2) {
                checkNotNull(args[0]);
                const std::int32_t i = reg(args[1]);
                const std::int32_t len = fn_.newVreg(false);
                IrInst& load = inst(IrOp::Load);
                load.d = len;
                load.a = args[0].vreg;
                load.imm = kStringLengthOffset;
                load.width = 4;
                const std::int32_t at = std::int32_t(fn_.code.size());
                inst(IrOp::ExitCmp);
                const std::int32_t exit = makeExit(std::uint32_t(ErrorCode::IndexOutOfRange));
                IrInst& check = fn_.code[std::size_t(at)];
                check.a = i;
                check.b = len;
                check.cond = IrCond::Hs;
                check.width = 32;
                check.target = exit;
                const std::int32_t wide = fn_.newVreg(false);
                IrInst& ext = inst(IrOp::Zext32);
                ext.d = wide;
                ext.a = i;
                const std::int32_t d = fn_.newVreg(false);
                IrInst& ch = inst(IrOp::LoadIdx);
                ch.d = d;
                ch.a = args[0].vreg;
                ch.b = wide;
                ch.scale = 1;
                ch.imm = kStringDataOffset;
                ch.width = 2;
                push(valueEntry(d, core_.charType));
                release(args[0]);
                return true;
            }
        }
        return false;
    }

    /// The collections the engine implements, called directly instead of through the generic way into a native. A value
    /// goes as its tag and its payload bits; a value that comes back is its payload bits, holding a count if it has one.
    bool lowerCollectionIntrinsic(const MethodInfo& target, std::vector<Entry>& args)
    {
        const NativeKind kind = target.owner->nativeKind;
        if (kind != NativeKind::List && kind != NativeKind::Dictionary && kind != NativeKind::StringBuilder)
            return false;
        if (target.isStatic() || args.empty())
            return false;
        // A value that comes back as bits alone loses a tag it may need.
        if (target.returnType && classOf(target.returnType) == Cls::Obj)
            return false;
        const String& name = target.name;

        void* helper = nullptr;
        bool returnsSelf = false;
        if (kind == NativeKind::List) {
            if (name == u"get_Count" && args.size() == 1)
                helper = reinterpret_cast<void*>(&scriptJitListCount);
            else if (name == u"get_Item" && args.size() == 2)
                helper = reinterpret_cast<void*>(&scriptJitListGet);
            else if (name == u"set_Item" && args.size() == 3)
                helper = reinterpret_cast<void*>(&scriptJitListSet);
            else if (name == u"Add" && args.size() == 2)
                helper = reinterpret_cast<void*>(&scriptJitListAdd);
        } else if (kind == NativeKind::Dictionary) {
            if (name == u"get_Count" && args.size() == 1)
                helper = reinterpret_cast<void*>(&scriptJitDictCount);
            else if (name == u"get_Item" && args.size() == 2)
                helper = reinterpret_cast<void*>(&scriptJitDictGet);
            else if (name == u"set_Item" && args.size() == 3)
                helper = reinterpret_cast<void*>(&scriptJitDictSet);
            else if (name == u"ContainsKey" && args.size() == 2)
                helper = reinterpret_cast<void*>(&scriptJitDictContains);
        } else if (name == u"Append" && args.size() == 2) {
            const TypeKind argKind = baseKind(args[1].type);
            if (argKind == TypeKind::Char)
                helper = reinterpret_cast<void*>(&scriptJitBuilderAppendChar);
            else if (argKind == TypeKind::String)
                helper = reinterpret_cast<void*>(&scriptJitBuilderAppendString);
            returnsSelf = helper != nullptr;
        }
        if (!helper)
            return false;

        secureWeak();
        for (Entry& arg : args) {
            if (arg.own == Own::Weak)
                makeOwned(arg);
        }
        std::vector<std::int32_t> callArgs{rtReg(), reg(args[0])};
        for (std::size_t i = 1; i < args.size(); ++i) {
            // An index stays an int; everything else travels as a value.
            const bool isIndex = kind == NativeKind::List && i == 1 && name != u"Add";
            const bool isPlain = kind == NativeKind::StringBuilder;
            if (!isIndex && !isPlain)
                callArgs.push_back(tagOfEntry(args[i]));
            callArgs.push_back(payloadOfEntry(args[i]));
        }
        const Cls ret = target.returnType ? classOf(target.returnType) : Cls::Void;
        const bool hasResult = !returnsSelf && ret != Cls::Void;
        std::int32_t result = callHelper(helper, std::move(callArgs), hasResult, ownedOf(args));
        if (returnsSelf) {
            // `Append` hands back the builder it was given: the entry stays as it was.
            for (std::size_t i = 1; i < args.size(); ++i)
                release(args[i]);
            args[0].type = target.returnType;
            push(args[0]);
            return true;
        }
        for (const Entry& arg : args)
            release(arg);
        if (!hasResult)
            return true;
        if (isFpClass(ret)) {
            const std::int32_t d = fn_.newVreg(true);
            IrInst& in = inst(IrOp::BitsToFp);
            in.d = d;
            in.a = result;
            in.width = ret == Cls::F32 ? 32 : 64;
            result = d;
            push(valueEntry(result, target.returnType));
        } else if (ret == Cls::Ref) {
            push(refEntry(result, target.returnType, Own::Owned));
        } else {
            push(valueEntry(result, target.returnType));
        }
        return true;
    }

    void lowerCall(Op op, const std::uint8_t* operands)
    {
        const AssemblyImpl* assembly = scope_->method->assembly;
        const std::uint32_t index = readU32(operands);
        if (index >= assembly->methodRefs.size())
            refuse("a call to an unknown method");
        const MethodInfo* target = assembly->methodRefs[index];
        const std::uint32_t argc = readU32(operands + 4);
        if (!target)
            refuse("a call to an unknown method");
        if (target->isAbstract() && op == Op::Call)
            refuse("a direct call to an abstract method");

        std::vector<Entry> args(argc);
        for (std::uint32_t i = argc; i-- > 0;)
            args[i] = pop();
        // The receiver's static type may be narrower than the method's owner, and an argument's may be a primitive
        // headed for `object`: both are checked against what the callee declares.
        checkSignature(*target, args);
        const std::size_t paramBase = target->isStatic() ? 0 : 1;
        for (std::size_t i = 0; i < target->params.size(); ++i) {
            if (target->params[i].mode != ParamMode::Ref && target->params[i].mode != ParamMode::Out)
                coerceTo(args[paramBase + i], target->params[i].type);
        }

        // A method of `int` has a receiver that cannot be null.
        if (!target->isStatic() && argc > 0 && args[0].type && classOf(args[0].type) != Cls::I32 &&
            classOf(args[0].type) != Cls::I64 && !isFpClass(classOf(args[0].type))) {
            const std::size_t before = scope_->held.size();
            for (std::int32_t owned : ownedOf(args))
                scope_->held.push_back(owned);
            checkNotNull(args[0]);
            scope_->held.resize(before);
        }

        if (lowerIntrinsic(*target, args))
            return;

        // A virtual call with no way to be overridden is a direct call.
        bool isVirtual = op == Op::CallVirt && target->vtableSlot >= 0;
        if (isVirtual && (target->flags & MethodSealed || (target->owner && target->owner->isSealed)))
            isVirtual = false;
        const bool isInterface = op == Op::CallIface;

        // A callee that stores no reference anywhere cannot invalidate what was read from a field, so no counts are
        // needed around the call for that.
        const bool keepsWeak = !isVirtual && !isInterface && storesNoReferences(*target);
        if (!keepsWeak) {
            secureWeak();
            for (Entry& arg : args) {
                if (arg.own == Own::Weak)
                    makeOwned(arg);
            }
        }

        if (!isVirtual && !isInterface && canInline(*target, argc)) {
            inlineCall(*target, args);
            return;
        }

        std::vector<std::int32_t> extra;
        IrCall call;
        call.target = target;
        call.kind = isInterface ? IrCallKind::Interface : isVirtual ? IrCallKind::Virtual : IrCallKind::Direct;
        if (isInterface)
            call.delegate = callPure(reinterpret_cast<void*>(&scriptJitResolveInterface),
                                     {rtReg(), reg(args[0]), ptrReg(target)}, false);
        pushCallArgs(call, args, isInterface, extra);
        if (isVirtual && target->owner && target->owner->nativeKind != NativeKind::None)
            refuse("a virtual call on a type the engine implements");
        finishCall(std::move(call), args, extra, returnClass(*target), target->returnType);
    }

    /// Lay out a call's arguments. `object` is its tag and its payload; the receiver of an interface call is the payload
    /// alone, since what it dispatches to takes `this` as a plain reference.
    void pushCallArgs(IrCall& call, std::vector<Entry>& args, bool receiverIsPlain, std::vector<std::int32_t>& extra)
    {
        extraTagged_.clear();
        for (std::size_t i = 0; i < args.size(); ++i) {
            Entry& arg = args[i];
            const bool tagged = arg.tagVreg >= 0 && !(receiverIsPlain && i == 0);
            if (tagged)
                call.args.push_back(arg.tagVreg);
            call.args.push_back(reg(arg));
            if (arg.own == Own::Owned && !arg.isConst) {
                if (arg.tagVreg >= 0)
                    extraTagged_.push_back({arg.vreg, arg.tagVreg});
                else
                    extra.push_back(arg.vreg);
            }
        }
    }

    void finishCall(IrCall call, std::vector<Entry>& args, const std::vector<std::int32_t>& extra, Cls ret,
                    const TypeInfo* returnType)
    {
        call.exit = makeExit(0, extra);
        extraTagged_.clear();
        call.site = site();
        call.publishFrame = true;
        if (ret != Cls::Void)
            call.result = fn_.newVreg(isFpClass(ret));
        if (ret == Cls::Obj)
            call.resultTag = fn_.newVreg(false);
        const std::int32_t result = call.result;
        const std::int32_t resultTag = call.resultTag;
        emitCall(std::move(call));
        for (const Entry& arg : args)
            release(arg);
        if (ret == Cls::Ref || ret == Cls::Obj) {
            Entry out = refEntry(result, returnType, Own::Owned);
            out.tagVreg = resultTag;
            push(out);
        } else if (ret != Cls::Void) {
            push(valueEntry(result, returnType));
        }
    }

    void checkSignature(const MethodInfo& target, const std::vector<Entry>& args)
    {
        const std::size_t base = target.isStatic() ? 0 : 1;
        if (args.size() != base + target.params.size())
            refuse("a call whose arguments do not match the signature");
        for (std::size_t i = 0; i < target.params.size(); ++i) {
            const TypeInfo* declared = target.params[i].type;
            const ParamMode mode = target.params[i].mode;
            if (mode == ParamMode::Ref || mode == ParamMode::Out)
                continue;  // a box goes by pointer whatever it holds
            const Cls want = classOf(declared);
            const Cls have = args[base + i].type ? classOf(args[base + i].type) : Cls::Ref;
            if (want != have && want != Cls::Obj)
                refuse("an argument of a different representation than the parameter");
        }
        // What `object`'s own methods dispatch to takes `this` as it is, with no agreement on how to pass it.
        if (!target.isStatic() && target.owner && baseKind(target.owner) == TypeKind::Object)
            refuse("a method of 'object'");
    }

    void lowerNewObj(const std::uint8_t* operands)
    {
        const AssemblyImpl* assembly = scope_->method->assembly;
        const std::uint32_t index = readU32(operands);
        if (index >= assembly->methodRefs.size())
            refuse("a constructor that is not known");
        const MethodInfo* ctor = assembly->methodRefs[index];
        const std::uint32_t argc = readU32(operands + 4);
        if (!ctor || !ctor->owner)
            refuse("a constructor that is not known");
        const TypeInfo* type = ctor->owner;

        std::vector<Entry> args(argc);
        for (std::uint32_t i = argc; i-- > 0;)
            args[i] = pop();
        secureWeak();

        if (type->nativeKind != NativeKind::None) {
            // A type the engine implements makes its own cell; `this` goes in empty.
            for (std::size_t i = 0; i < ctor->params.size() && i < args.size(); ++i)
                coerceTo(args[i], ctor->params[i].type);
            IrCall call;
            call.kind = IrCallKind::Direct;
            call.target = ctor;
            call.args.push_back(immReg(0));
            std::vector<std::int32_t> extra;
            for (Entry& arg : args) {
                if (arg.own == Own::Weak)
                    makeOwned(arg);
            }
            pushCallArgs(call, args, false, extra);
            finishCall(std::move(call), args, extra, Cls::Ref, type);
            top().nonNull = true;
            return;
        }

        for (std::size_t i = 0; i < ctor->params.size() && i < args.size(); ++i)
            coerceTo(args[i], ctor->params[i].type);
        extraTagged_ = taggedOf(args);
        const std::int32_t cell = newInstanceCell(type, ownedOf(args));
        extraTagged_.clear();
        // The new object is on the stack as an owned entry while its constructor runs.
        Entry self = refEntry(cell, type, Own::Owned, true);
        push(self);
        std::vector<Entry> call;
        call.reserve(argc + 1);
        Entry borrowed = self;
        borrowed.own = Own::Immortal;  // the stack entry holds the count
        call.push_back(borrowed);
        for (Entry& arg : args)
            call.push_back(arg);
        if (!ctor->isNative() && (ctor->isAbstract() || ctor->body.code.empty())) {
            // Nothing to run beyond the slots.
            for (const Entry& arg : args)
                release(arg);
            return;
        }
        for (Entry& arg : call) {
            if (arg.own == Own::Weak)
                makeOwned(arg);
        }
        if (canInline(*ctor, argc + 1)) {
            inlineCall(*ctor, call, true);
            return;
        }
        IrCall irCall;
        irCall.kind = IrCallKind::Direct;
        irCall.target = ctor;
        std::vector<std::int32_t> extra;
        pushCallArgs(irCall, call, false, extra);
        finishCall(std::move(irCall), call, extra, Cls::Void, nullptr);
    }

    /// Plain references that hold a count. What has a tag next to it is in `taggedOf`.
    static std::vector<std::int32_t> ownedOf(const std::vector<Entry>& entries)
    {
        std::vector<std::int32_t> out;
        for (const Entry& e : entries) {
            if (e.own == Own::Owned && !e.isConst && e.tagVreg < 0)
                out.push_back(e.vreg);
        }
        return out;
    }
    static std::vector<std::pair<std::int32_t, std::int32_t>> taggedOf(const std::vector<Entry>& entries)
    {
        std::vector<std::pair<std::int32_t, std::int32_t>> out;
        for (const Entry& e : entries) {
            if (e.own == Own::Owned && !e.isConst && e.tagVreg >= 0)
                out.push_back({e.vreg, e.tagVreg});
        }
        return out;
    }

    void lowerNewDlg(const std::uint8_t* operands)
    {
        const AssemblyImpl* assembly = scope_->method->assembly;
        const TypeInfo* type = assembly->typeRefs[readU32(operands)];
        const MethodInfo* target = assembly->methodRefs[readU32(operands + 4)];
        const std::uint32_t captures = readU32(operands + 8);
        std::vector<Entry> boxes(captures);
        for (std::uint32_t i = captures; i-- > 0;)
            boxes[i] = pop();
        Entry receiver = pop();
        secureWeak();
        // The helper adds what it keeps; it reads the boxes out of a small array of pointers on the stack, which here is
        // simply one register per box passed in order, up to what the convention carries.
        if (captures > 4)
            refuse("a lambda with many captures");
        const std::int32_t self = receiver.isConst ? immReg(0) : reg(receiver);
        std::vector<std::int32_t> args{rtReg(), ptrReg(type), ptrReg(target), self,
                                       immReg(std::int64_t(captures))};
        for (const Entry& box : boxes)
            args.push_back(reg(box));
        const std::int32_t cell = callHelper(reinterpret_cast<void*>(&scriptJitNewDelegate), std::move(args), true);
        release(receiver);
        for (const Entry& box : boxes)
            release(box);
        push(refEntry(cell, type, Own::Owned, true));
    }

    void lowerCallDlg(const std::uint8_t* operands)
    {
        const std::uint32_t argc = readU32(operands);
        std::vector<Entry> args(argc);
        for (std::uint32_t i = argc; i-- > 0;)
            args[i] = pop();
        Entry dlg = pop();
        const TypeInfo* type = dlg.type;
        if (!type || baseKind(type) != TypeKind::Delegate || !type->invoke)
            refuse("a delegate of no known signature");
        const MethodInfo& sig = *type->invoke;
        if (sig.params.size() != argc)
            refuse("a delegate call with the wrong arguments");
        for (std::uint32_t i = 0; i < argc; ++i)
            coerceTo(args[i], sig.params[i].type);
        {
            const std::size_t before = scope_->held.size();
            for (std::int32_t owned : ownedOf(args))
                scope_->held.push_back(owned);
            if (dlg.own == Own::Owned)
                scope_->held.push_back(dlg.vreg);
            checkNotNull(dlg);
            scope_->held.resize(before);
        }
        secureWeak();
        if (dlg.own == Own::Weak)
            makeOwned(dlg);
        for (Entry& arg : args) {
            if (arg.own == Own::Weak)
                makeOwned(arg);
        }
        IrCall call;
        call.kind = IrCallKind::Delegate;
        call.target = &sig;
        call.delegate = reg(dlg);
        std::vector<std::int32_t> extra;
        if (dlg.own == Own::Owned)
            extra.push_back(dlg.vreg);
        pushCallArgs(call, args, false, extra);
        const Cls ret = sig.returnType ? classOf(sig.returnType) : Cls::Void;
        // The delegate's count ends with the call, after the result is safe on the stack.
        const Entry self = dlg;
        finishCall(std::move(call), args, extra, ret, sig.returnType);
        release(self);
    }

    /// A value of static type `type` read from a `Box` in memory.
    void loadBoxed(std::int32_t box, const TypeInfo* type)
    {
        Entry e = loadPayload(box, kBoxValueOffset + kPayloadOffset, type, Own::Weak);
        push(e);
    }

    const TypeInfo* typeRef(const std::uint8_t* operands) const
    {
        const AssemblyImpl* assembly = scope_->method->assembly;
        const std::uint32_t index = readU32(operands);
        if (index >= assembly->typeRefs.size() || !assembly->typeRefs[index])
            refuse("a type reference that is not known");
        return assembly->typeRefs[index];
    }

    /// A `Value` image of an entry, passed by the helper convention: (tag, payload).
    std::int32_t tagOfEntry(const Entry& e)
    {
        const Cls c = e.type ? classOf(e.type) : Cls::Ref;
        switch (c) {
            case Cls::Ref: return immReg(-1);  // the helper asks the cell
            case Cls::Obj: return e.tagVreg;
            default: return immReg(std::int64_t(runtimeTag(baseKind(e.type))));
        }
    }

    std::int32_t payloadOfEntry(Entry& e)
    {
        const Cls c = e.type ? classOf(e.type) : Cls::Ref;
        if (isFpClass(c)) {
            const std::int32_t source = reg(e);
            const std::int32_t bits = fn_.newVreg(false);
            IrInst& in = inst(IrOp::FpToBits);
            in.d = bits;
            in.a = source;
            in.width = c == Cls::F32 ? 32 : 64;
            return bits;
        }
        return reg(e);
    }

    void lowerSimple(Op op, const std::uint8_t* operands, std::uint32_t pc)
    {
        (void)pc;
        switch (op) {
            case Op::Nop: return;
            case Op::LdNull: push(constInt(0, core_.nullType)); top().own = Own::Immortal; return;
            case Op::LdTrue: push(constInt(1, core_.boolType)); return;
            case Op::LdFalse: push(constInt(0, core_.boolType)); return;
            case Op::LdcI4: push(constInt(std::int32_t(readU32(operands)), core_.intType)); return;
            case Op::LdcI8:
                push(constInt(std::int64_t(std::uint64_t(readU32(operands)) | (std::uint64_t(readU32(operands + 4)) << 32)),
                              core_.longType));
                return;
            case Op::LdcR4: push(constFp(readU32(operands), core_.floatType)); return;
            case Op::LdcR8:
                push(constFp(std::uint64_t(readU32(operands)) | (std::uint64_t(readU32(operands + 4)) << 32),
                             core_.doubleType));
                return;
            case Op::LdStr: {
                const AssemblyImpl* assembly = scope_->method->assembly;
                const std::uint32_t index = readU32(operands);
                if (index >= assembly->strings.size())
                    refuse("a string literal that is not known");
                const std::int32_t d = ptrReg(detail::Access::ptr(assembly->strings[index]));
                push(refEntry(d, core_.stringType, Own::Immortal, true));
                return;
            }
            case Op::Pop: {
                Entry e = pop();
                release(e);
                return;
            }
            case Op::Dup: {
                Entry& e = top();
                if (e.own == Own::Owned) {
                    Entry copy = e;
                    copy.own = Own::Local;  // reads the owned register, and then gets its own count
                    copy.local = -1;
                    makeOwned(copy);
                    push(copy);
                    return;
                }
                Entry copy = e;
                push(copy);
                return;
            }
            case Op::Dup2: {
                Entry a = top(1);
                Entry b = top(0);
                if (a.own == Own::Owned) {
                    a.own = Own::Local;
                    a.local = -1;
                    makeOwned(a);
                }
                if (b.own == Own::Owned) {
                    b.own = Own::Local;
                    b.local = -1;
                    makeOwned(b);
                }
                push(a);
                push(b);
                return;
            }
            case Op::Swap: std::swap(top(0), top(1)); return;
            case Op::LdLoc: pushLocal(readU32(operands)); return;
            case Op::StLoc: storeLocal(readU32(operands)); return;

            case Op::NewBox: {
                const std::uint32_t slot = readU32(operands);
                Scope& s = *scope_;
                const TypeInfo* type = s.localType[slot];
                const Cls c = classOf(type);
                // At entry a parameter's value moves into the box; a local gets its default.
                const std::uint32_t paramBase = s.method->isStatic() ? 0u : 1u;
                const bool isParam = slot >= paramBase && slot < paramBase + s.method->params.size();
                std::int32_t tag = immReg(c == Cls::Ref   ? -1
                                          : c == Cls::Obj ? std::int64_t(Type::Null)
                                                          : std::int64_t(runtimeTag(baseKind(type))));
                std::int32_t payload;
                if (isParam && !boxSeen_[slot] && c == Cls::Obj) {
                    tag = s.localTag[slot];
                    payload = s.localVreg[slot];
                } else if (isParam && !boxSeen_[slot]) {
                    if (isFpClass(c)) {
                        payload = fn_.newVreg(false);
                        IrInst& in = inst(IrOp::FpToBits);
                        in.d = payload;
                        in.a = s.localVreg[slot];
                        in.width = c == Cls::F32 ? 32 : 64;
                    } else {
                        payload = s.localVreg[slot];
                    }
                } else {
                    payload = immReg(0);
                }
                boxSeen_[slot] = 1;
                const std::int32_t box =
                    callHelper(reinterpret_cast<void*>(&scriptJitNewBox), {rtReg(), tag, payload}, true);
                if (isParam && !boxOwned_[slot]) {
                    // The slot was the plain parameter; from here on it is the box.
                    boxOwned_[slot] = 1;
                    if (s.localOwned[slot])
                        decRefOf(s.localVreg[slot], s.localTag[slot], true);
                    s.localTag[slot] = -1;
                    s.localVreg[slot] = fn_.newVreg(false);
                    IrInst& init = inst(IrOp::MovImm);
                    init.d = s.localVreg[slot];
                    init.imm = 0;
                    s.localOwned[slot] = 1;
                }
                const std::int32_t old = fn_.newVreg(false);
                IrInst& save = inst(IrOp::Mov);
                save.d = old;
                save.a = s.localVreg[slot];
                IrInst& mov = inst(IrOp::Mov);
                mov.d = s.localVreg[slot];
                mov.a = box;
                IrInst& dec = inst(IrOp::DecRef);
                dec.a = old;
                dec.isNullable = true;
                s.localNonNull[slot] = 1;
                return;
            }
            case Op::LdBoxed: {
                const std::uint32_t slot = readU32(operands);
                loadBoxed(scope_->localVreg[slot], boxedType(slot));
                return;
            }
            case Op::StBoxed: {
                const std::uint32_t slot = readU32(operands);
                Entry value = pop();
                storeBox(scope_->localVreg[slot], boxedType(slot), value);
                return;
            }
            case Op::LdBoxRef: {
                const std::uint32_t slot = readU32(operands);
                Entry e;
                e.vreg = scope_->localVreg[slot];
                e.type = core_.nullType;  // what a box holds is the callee's business
                e.own = Own::Local;
                e.local = std::int32_t(slot);
                e.nonNull = true;
                push(e);
                return;
            }
            case Op::LdFld: lowerLdFld(readU32(operands)); return;
            case Op::StFld: lowerStFld(readU32(operands)); return;
            case Op::LdSFld:
            case Op::StSFld: {
                const TypeInfo* owner = typeRef(operands);
                const std::uint32_t slot = readU32(operands + 4);
                AssemblyImpl* home = owner->assembly ? owner->assembly : scope_->method->assembly;
                if (slot >= home->statics.size())
                    refuse("a static field that is not there");
                const TypeInfo* type = staticFieldType(owner, slot);
                const std::int32_t base = ptrReg(&home->statics[slot]);
                if (op == Op::LdSFld) {
                    push(loadPayload(base, kPayloadOffset, type, Own::Weak));
                } else {
                    Entry value = pop();
                    storeValueTo(base, 0, type, value);
                    // A static a program never initialized has no tag yet, unlike the fields of an instance.
                    if (classOf(type) != Cls::Ref && classOf(type) != Cls::Obj) {
                        IrInst& tag = inst(IrOp::StoreTag);
                        tag.a = base;
                        tag.tag = runtimeTag(baseKind(type));
                        tag.imm = 0;
                    }
                }
                return;
            }

            case Op::NewArr: {
                const TypeInfo* element = typeRef(operands);
                Entry length = pop();
                const std::int32_t cell = callHelper(reinterpret_cast<void*>(&scriptJitNewArray),
                                                     {rtReg(), ptrReg(element), reg(length)}, true);
                push(refEntry(cell, rt_.types->arrayOf(element), Own::Owned, true));
                return;
            }
            case Op::LdElem: lowerLdElem(); return;
            case Op::StElem: lowerStElem(); return;
            case Op::LdLen: {
                Entry array = pop();
                if (!array.type || baseKind(array.type) != TypeKind::Array)
                    refuse("the length of something that is not known to be an array");
                {
                    const Held held(*this, {&array});
                    checkNotNull(array);
                }
                const std::int32_t d = fn_.newVreg(false);
                IrInst& load = inst(IrOp::Load);
                load.d = d;
                load.a = array.vreg;
                load.imm = kArrayLengthOffset;
                load.width = 4;
                push(valueEntry(d, core_.intType));
                release(array);
                return;
            }

            case Op::InitObj: {
                const TypeInfo* type = typeRef(operands);
                if (type->kind == TypeKind::Struct) {
                    const std::int32_t cell = newInstanceCell(type, {});
                    push(refEntry(cell, type, Own::Owned, true));
                    return;
                }
                const Cls c = classOf(type);
                if (c == Cls::Ref) {
                    push(constInt(0, type));
                    top().own = Own::Immortal;
                } else if (c == Cls::Obj) {
                    Entry e = refEntry(immReg(0), type, Own::Immortal);
                    e.tagVreg = immReg(std::int64_t(Type::Null));
                    push(e);
                } else {
                    push(constInt(0, type));
                }
                return;
            }
            case Op::CloneVal: {
                Entry e = pop();
                if (baseKind(e.type) != TypeKind::Struct) {
                    // Only a struct is cloned: anything else stays what it is.
                    push(e);
                    return;
                }
                secureWeak();
                if (e.own == Own::Weak)
                    makeOwned(e);
                const std::int32_t source = reg(e);
                const std::int32_t cell = callHelper(reinterpret_cast<void*>(&scriptJitClone), {rtReg(), source}, true,
                                                     ownedOf({e}));
                release(e);
                push(refEntry(cell, e.type, Own::Owned, e.nonNull));
                return;
            }
            case Op::Box: return;
            case Op::Unbox:
            case Op::CastClass: {
                const TypeInfo* to = typeRef(operands);
                Entry e = pop();
                secureWeak();
                if (e.own == Own::Weak)
                    makeOwned(e);
                coerceToObject(e, core_.objectType);
                const Cls toClass = classOf(to);
                extraTagged_ = taggedOf({e});
                if (toClass == Cls::I32 || toClass == Cls::I64 || isFpClass(toClass)) {
                    // Out of `object`: its tag says what it really is, and the conversion does the rest.
                    std::int32_t bits = callHelper(reinterpret_cast<void*>(&scriptJitUnbox),
                                                   {rtReg(), e.tagVreg, e.vreg, ptrReg(to)}, true);
                    extraTagged_.clear();
                    if (isFpClass(toClass)) {
                        const std::int32_t d = fn_.newVreg(true);
                        IrInst& in = inst(IrOp::BitsToFp);
                        in.d = d;
                        in.a = bits;
                        in.width = toClass == Cls::F32 ? 32 : 64;
                        bits = d;
                    }
                    release(e);
                    push(valueEntry(bits, to));
                    return;
                }
                callHelper(reinterpret_cast<void*>(&scriptJitCast), {rtReg(), e.tagVreg, e.vreg, ptrReg(to)}, false);
                extraTagged_.clear();
                if (toClass == Cls::Ref)
                    e.tagVreg = -1;  // statically known from here on, and the same count
                e.type = to;
                push(e);
                return;
            }
            case Op::IsInst:
            case Op::AsInst: {
                const TypeInfo* to = typeRef(operands);
                Entry e = pop();
                secureWeak();
                if (e.own == Own::Weak)
                    makeOwned(e);
                coerceToObject(e, core_.objectType);
                const std::int32_t source = e.vreg;
                const std::int32_t yes = callPure(reinterpret_cast<void*>(&scriptJitIsInstance),
                                                  {rtReg(), e.tagVreg, source, ptrReg(to)}, false);
                if (op == Op::IsInst) {
                    release(e);
                    push(valueEntry(yes, core_.boolType));
                    return;
                }
                if (classOf(to) != Cls::Ref) {
                    // `as` to an interface or a primitive: null or the same thing, tag included.
                    const std::int32_t d = fn_.newVreg(false);
                    const std::int32_t t = fn_.newVreg(false);
                    const std::int32_t keep = newLabel();
                    IrInst& movValue = inst(IrOp::Mov);
                    movValue.d = d;
                    movValue.a = source;
                    IrInst& movTag = inst(IrOp::Mov);
                    movTag.d = t;
                    movTag.a = e.tagVreg;
                    IrInst& test = inst(IrOp::BrCmp);
                    test.a = yes;
                    test.cond = IrCond::Ne;
                    test.hasImm = true;
                    test.imm = 0;
                    test.width = 32;
                    test.target = keep;
                    if (e.own == Own::Owned)
                        decRefOf(source, e.tagVreg, true);
                    IrInst& zero = inst(IrOp::MovImm);
                    zero.d = d;
                    zero.imm = 0;
                    IrInst& nullTag = inst(IrOp::MovImm);
                    nullTag.d = t;
                    nullTag.imm = std::int64_t(Type::Null);
                    inst(IrOp::Label).imm = keep;
                    Entry out = refEntry(d, classOf(to) == Cls::Obj ? to : core_.objectType,
                                         e.own == Own::Owned ? Own::Owned : Own::Weak);
                    out.tagVreg = t;
                    if (e.own == Own::Local)
                        makeOwned(out);
                    push(out);
                    return;
                }
                // `as`: the value, or null.
                const std::int32_t d = fn_.newVreg(false);
                const std::int32_t done = newLabel();
                IrInst& mov = inst(IrOp::Mov);
                mov.d = d;
                mov.a = source;
                IrInst& br = inst(IrOp::BrCmp);
                br.a = yes;
                br.cond = IrCond::Ne;
                br.hasImm = true;
                br.imm = 0;
                br.width = 32;
                br.target = done;
                if (e.own == Own::Owned)
                    decRefOf(source, e.tagVreg, true);
                IrInst& zero = inst(IrOp::MovImm);
                zero.d = d;
                zero.imm = 0;
                inst(IrOp::Label).imm = done;
                Entry out = refEntry(d, to, e.own == Own::Owned ? Own::Owned : Own::Weak);
                if (e.own == Own::Local)
                    makeOwned(out);
                else if (e.own == Own::Immortal)
                    out.own = Own::Immortal;
                push(out);
                return;
            }

            case Op::Call:
            case Op::CallVirt:
            case Op::CallIface: lowerCall(op, operands); return;
            case Op::NewObj: lowerNewObj(operands); return;
            case Op::NewDlg: lowerNewDlg(operands); return;
            case Op::CallDlg: lowerCallDlg(operands); return;

            case Op::ConvI1:
            case Op::ConvI2:
            case Op::ConvU1:
            case Op::ConvU2:
            case Op::ConvChar:
            case Op::ConvI4:
            case Op::ConvU4: convertInt(op); return;
            case Op::ConvI8:
            case Op::ConvU8: convertLong(op); return;
            case Op::ConvR4:
            case Op::ConvR8:
            case Op::ConvR8Un: convertFp(op); return;
            case Op::ConvBool: {
                Entry a = pop();
                requireNumber(a);
                if (a.isConst) {
                    push(constInt(a.imm != 0, core_.boolType));
                    return;
                }
                if (a.type && classOf(a.type) == Cls::Ref)
                    refuse("a reference as a bool");
                const std::int32_t d = fn_.newVreg(false);
                IrInst& in = inst(IrOp::Cset);
                in.d = d;
                in.a = a.vreg;
                in.hasImm = true;
                in.imm = 0;
                in.cond = IrCond::Ne;
                in.width = classOf(a.type) == Cls::I64 ? 64 : 32;
                push(valueEntry(d, core_.boolType));
                return;
            }

            case Op::AddI4: binaryInt(IrOp::Add, 32, core_.intType); return;
            case Op::SubI4: binaryInt(IrOp::Sub, 32, core_.intType); return;
            case Op::MulI4: binaryInt(IrOp::Mul, 32, core_.intType); return;
            case Op::AndI4: binaryInt(IrOp::And, 32, core_.intType); return;
            case Op::OrI4: binaryInt(IrOp::Or, 32, core_.intType); return;
            case Op::XorI4: binaryInt(IrOp::Xor, 32, core_.intType); return;
            case Op::ShlI4: binaryInt(IrOp::Shl, 32, core_.intType); return;
            case Op::ShrI4: binaryInt(IrOp::Sar, 32, core_.intType); return;
            case Op::ShrU4: binaryInt(IrOp::Shr, 32, core_.intType); return;
            case Op::AddI8: binaryInt(IrOp::Add, 64, core_.longType); return;
            case Op::SubI8: binaryInt(IrOp::Sub, 64, core_.longType); return;
            case Op::MulI8: binaryInt(IrOp::Mul, 64, core_.longType); return;
            case Op::AndI8: binaryInt(IrOp::And, 64, core_.longType); return;
            case Op::OrI8: binaryInt(IrOp::Or, 64, core_.longType); return;
            case Op::XorI8: binaryInt(IrOp::Xor, 64, core_.longType); return;
            case Op::ShlI8: shiftLong(IrOp::Shl); return;
            case Op::ShrI8: shiftLong(IrOp::Sar); return;
            case Op::ShrU8: shiftLong(IrOp::Shr); return;
            case Op::DivI4: divide(IrOp::SDiv, 32, core_.intType); return;
            case Op::DivU4: divide(IrOp::UDiv, 32, core_.intType); return;
            case Op::RemI4: divide(IrOp::SRem, 32, core_.intType); return;
            case Op::RemU4: divide(IrOp::URem, 32, core_.intType); return;
            case Op::DivI8: divide(IrOp::SDiv, 64, core_.longType); return;
            case Op::DivU8: divide(IrOp::UDiv, 64, core_.longType); return;
            case Op::RemI8: divide(IrOp::SRem, 64, core_.longType); return;
            case Op::RemU8: divide(IrOp::URem, 64, core_.longType); return;
            case Op::NegI4: unaryInt(IrOp::Neg, 32, core_.intType); return;
            case Op::NotI4: unaryInt(IrOp::Not, 32, core_.intType); return;
            case Op::NegI8: unaryInt(IrOp::Neg, 64, core_.longType); return;
            case Op::NotI8: unaryInt(IrOp::Not, 64, core_.longType); return;

            case Op::AddR8: binaryFp(IrOp::FAdd, Cls::F64, core_.doubleType); return;
            case Op::SubR8: binaryFp(IrOp::FSub, Cls::F64, core_.doubleType); return;
            case Op::MulR8: binaryFp(IrOp::FMul, Cls::F64, core_.doubleType); return;
            case Op::DivR8: binaryFp(IrOp::FDiv, Cls::F64, core_.doubleType); return;
            case Op::AddR4: binaryFp(IrOp::FAdd, Cls::F32, core_.floatType); return;
            case Op::SubR4: binaryFp(IrOp::FSub, Cls::F32, core_.floatType); return;
            case Op::MulR4: binaryFp(IrOp::FMul, Cls::F32, core_.floatType); return;
            case Op::DivR4: binaryFp(IrOp::FDiv, Cls::F32, core_.floatType); return;
            case Op::NegR8:
            case Op::NegR4: {
                Entry a = pop();
                requireNumber(a);
                const Cls c = op == Op::NegR8 ? Cls::F64 : Cls::F32;
                const std::int32_t ra = reg(a);
                const std::int32_t d = fn_.newVreg(true);
                IrInst& in = inst(IrOp::FNeg);
                in.d = d;
                in.a = ra;
                in.width = c == Cls::F64 ? 64 : 32;
                push(valueEntry(d, op == Op::NegR8 ? core_.doubleType : core_.floatType));
                return;
            }
            case Op::RemR8:
            case Op::RemR4: {
                Entry b = pop();
                Entry a = pop();
                requireNumber(a);
                requireNumber(b);
                const std::int32_t ra = reg(a);
                const std::int32_t rb = reg(b);
                void* helper = op == Op::RemR8 ? reinterpret_cast<void*>(&scriptJitFmod)
                                               : reinterpret_cast<void*>(&scriptJitFmodf);
                const std::int32_t d = callPure(helper, {ra, rb}, true);
                push(valueEntry(d, op == Op::RemR8 ? core_.doubleType : core_.floatType));
                return;
            }
            case Op::AddI4Ovf:
            case Op::SubI4Ovf:
            case Op::MulI4Ovf: {
                // At 64 bits nothing can overflow, so the check is whether the answer still fits in 32.
                Entry b = pop();
                Entry a = pop();
                const std::int32_t ra = reg(a), rb = reg(b);
                const std::int32_t wa = fn_.newVreg(false), wb = fn_.newVreg(false);
                IrInst& ea = inst(IrOp::Sext32);
                ea.d = wa;
                ea.a = ra;
                IrInst& eb = inst(IrOp::Sext32);
                eb.d = wb;
                eb.a = rb;
                const std::int32_t wide = fn_.newVreg(false);
                IrInst& in = inst(op == Op::AddI4Ovf ? IrOp::Add : op == Op::SubI4Ovf ? IrOp::Sub : IrOp::Mul);
                in.d = wide;
                in.a = wa;
                in.b = wb;
                in.width = 64;
                const std::int32_t narrow = fn_.newVreg(false);
                IrInst& ext = inst(IrOp::Sext32);
                ext.d = narrow;
                ext.a = wide;
                const std::int32_t at = std::int32_t(fn_.code.size());
                inst(IrOp::ExitCmp);
                const std::int32_t exit = makeExit(std::uint32_t(ErrorCode::Overflow));
                IrInst& check = fn_.code[std::size_t(at)];
                check.a = wide;
                check.b = narrow;
                check.cond = IrCond::Ne;
                check.width = 64;
                check.target = exit;
                push(valueEntry(narrow, core_.intType));
                return;
            }
            case Op::AddI8Ovf:
            case Op::SubI8Ovf:
            case Op::MulI8Ovf: {
                Entry b = pop();
                Entry a = pop();
                const std::int32_t out = fn_.newVreg(false);
                void* helper = op == Op::AddI8Ovf   ? reinterpret_cast<void*>(&scriptJitAddOvf)
                               : op == Op::SubI8Ovf ? reinterpret_cast<void*>(&scriptJitSubOvf)
                                                    : reinterpret_cast<void*>(&scriptJitMulOvf);
                (void)out;
                const std::int32_t result = callHelper(helper, {rtReg(), reg(a), reg(b)}, true);
                push(valueEntry(result, core_.longType));
                return;
            }

            case Op::CeqRef:
            case Op::CneRef: {
                Entry b = pop();
                Entry a = pop();
                refCompare(op == Op::CeqRef, a, b);
                return;
            }
            case Op::CeqStr:
            case Op::CneStr: {
                Entry b = pop();
                Entry a = pop();
                const std::int32_t ra = reg(a), rb = reg(b);
                std::int32_t same = callPure(reinterpret_cast<void*>(&scriptJitStringEquals), {ra, rb}, false);
                release(b);
                release(a);
                if (op == Op::CneStr) {
                    const std::int32_t d = fn_.newVreg(false);
                    IrInst& in = inst(IrOp::Xor);
                    in.d = d;
                    in.a = same;
                    in.hasImm = true;
                    in.imm = 1;
                    in.width = 32;
                    same = d;
                }
                push(valueEntry(same, core_.boolType));
                return;
            }
            case Op::LogicalNot: {
                Entry a = pop();
                requireNumber(a);
                if (a.isConst) {
                    push(constInt(a.imm == 0, core_.boolType));
                    return;
                }
                const std::int32_t d = fn_.newVreg(false);
                IrInst& in = inst(IrOp::Cset);
                in.d = d;
                in.a = a.vreg;
                in.hasImm = true;
                in.imm = 0;
                in.cond = IrCond::Eq;
                in.width = 32;
                push(valueEntry(d, core_.boolType));
                return;
            }
            case Op::IsNull: {
                Entry a = pop();
                if (a.isConst) {
                    push(constInt(a.imm == 0, core_.boolType));
                    return;
                }
                if (a.type && classOf(a.type) != Cls::Ref && classOf(a.type) != Cls::Obj) {
                    // A primitive headed for a null test is never null.
                    push(constInt(0, core_.boolType));
                    return;
                }
                const std::int32_t d = fn_.newVreg(false);
                IrInst& in = inst(IrOp::Cset);
                in.d = d;
                in.a = a.tagVreg >= 0 ? a.tagVreg : a.vreg;
                in.hasImm = true;
                in.imm = a.tagVreg >= 0 ? std::int64_t(Type::Null) : 0;
                in.cond = IrCond::Eq;
                in.width = a.tagVreg >= 0 ? 32 : 64;
                release(a);
                push(valueEntry(d, core_.boolType));
                return;
            }

            case Op::Concat: {
                const std::uint32_t count = readU32(operands);
                std::vector<Entry> parts(count);
                for (std::uint32_t i = count; i-- > 0;)
                    parts[i] = pop();
                if (count > 3)
                    refuse("a concatenation of many parts");
                secureWeak();
                for (Entry& part : parts) {
                    if (part.own == Own::Weak)
                        makeOwned(part);
                }
                std::vector<std::int32_t> args{rtReg(), immReg(count)};
                for (Entry& part : parts) {
                    args.push_back(tagOfEntry(part));
                    args.push_back(payloadOfEntry(part));
                }
                extraTagged_ = taggedOf(parts);
                const std::int32_t s = callHelper(reinterpret_cast<void*>(&scriptJitConcat), std::move(args), true,
                                                  ownedOf(parts));
                extraTagged_.clear();
                for (const Entry& part : parts)
                    release(part);
                push(refEntry(s, core_.stringType, Own::Owned, true));
                return;
            }
            case Op::ToStr:
            case Op::ToStrU: {
                Entry a = pop();
                secureWeak();
                if (a.own == Own::Weak)
                    makeOwned(a);
                const std::int32_t tag = tagOfEntry(a);
                const std::int32_t payload = payloadOfEntry(a);
                extraTagged_ = taggedOf({a});
                const std::int32_t s =
                    callHelper(op == Op::ToStr ? reinterpret_cast<void*>(&scriptJitToString)
                                               : reinterpret_cast<void*>(&scriptJitToStringUnsigned),
                               {rtReg(), tag, payload}, true, ownedOf({a}));
                extraTagged_.clear();
                release(a);
                push(refEntry(s, core_.stringType, Own::Owned, true));
                return;
            }

            default: break;
        }
        if (isCompare(op)) {
            compareToValue(op);
            return;
        }
        refuse(std::string(opName(op)) + " is not compiled yet");
    }

    void shiftLong(IrOp op)
    {
        // The count is an int on the stack whatever the width of the value.
        binaryInt(op, 64, core_.longType);
    }

    void refCompare(bool wantEqual, Entry& a, Entry& b)
    {
        const Cls ca = a.type ? classOf(a.type) : Cls::Ref;
        const Cls cb = b.type ? classOf(b.type) : Cls::Ref;
        std::int32_t d;
        if (ca != Cls::Ref || cb != Cls::Ref || baseKind(a.type) == TypeKind::String) {
            // Anything that may be a string or a primitive compares as a value.
            const std::int32_t tagA = tagOfEntry(a), bitsA = payloadOfEntry(a);
            const std::int32_t tagB = tagOfEntry(b), bitsB = payloadOfEntry(b);
            d = callPure(reinterpret_cast<void*>(&scriptJitValueEquals), {tagA, bitsA, tagB, bitsB}, false);
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
        } else {
            const std::int32_t ra = reg(a);
            d = fn_.newVreg(false);
            const std::int32_t rb = b.isConst ? -1 : reg(b);
            IrInst& in = inst(IrOp::Cset);
            in.d = d;
            in.a = ra;
            in.b = rb;
            if (b.isConst) {
                in.hasImm = true;
                in.imm = b.imm;
            }
            in.cond = wantEqual ? IrCond::Eq : IrCond::Ne;
            in.width = 64;
        }
        release(b);
        release(a);
        push(valueEntry(d, core_.boolType));
    }

    const TypeInfo* boxedType(std::uint32_t slot) const
    {
        const Scope& s = *scope_;
        if (slot >= s.localType.size() || !s.localType[slot])
            refuse("a boxed slot with no static type");
        return s.localType[slot];
    }

    void storeBox(std::int32_t box, const TypeInfo* type, Entry value)
    {
        const Cls c = classOf(type);
        if (c != Cls::Ref && c != Cls::Obj) {
            const std::int32_t v = reg(value);
            // The box was made with the tag its slot holds, so the payload is all that changes.
            IrInst& store = inst(IrOp::Store);
            store.a = box;
            store.c = v;
            store.imm = kBoxValueOffset + kPayloadOffset;
            store.width = payloadBytes(c);
            return;
        }
        storeValueTo(box, kBoxValueOffset, type, value);
    }

    const TypeInfo* staticFieldType(const TypeInfo* owner, std::uint32_t slot) const
    {
        for (const FieldInfo& f : owner->statics) {
            if (f.slot == slot)
                return f.type;
        }
        refuse("a static field that could not be resolved");
    }

    struct BoundsFact
    {
        std::int32_t array = -1;
        std::int32_t index = -1;
    };

    Runtime& rt_;
    const MethodInfo& root_;
    IrFunction& fn_;
    bool osr_;
    const CoreTypes& core_;
    Scope* scope_ = nullptr;
    std::int32_t lastSite_ = -1;
    bool hasLoops_ = false;
    bool isThisAssigned_ = false;
    std::size_t inlinedBytes_ = 0;
    BoundsFact lastBounds_;
    std::vector<std::pair<std::int32_t, std::int32_t>> taggedNow_;   ///< what `ownedNow` found that has a tag
    std::vector<std::pair<std::int32_t, std::int32_t>> extraTagged_;  ///< popped, tagged, and still holding a count
    std::int32_t pendingIndex_ = -1;  ///< the index a bounds failure reports
    std::int32_t exceptionVreg_ = -1;  ///< what a `throw;` raises again; holds a count
    std::vector<std::function<void()>> deferred_;  ///< code laid out after the body
    std::map<std::vector<const Handler*>, std::int32_t> landings_;
    std::map<const Handler*, std::int32_t> continuation_;
    std::map<const Handler*, std::vector<std::int32_t>> finallyExits_;
    std::set<const Handler*> finallyEnded_;
    std::unordered_map<const MethodInfo*, bool> storesNothing_;
    std::unordered_map<std::uint32_t, std::uint8_t> boxSeen_;
    std::unordered_map<std::uint32_t, std::uint8_t> boxOwned_;
};

}  // namespace

bool lowerMethod(Runtime& rt, const MethodInfo& method, IrFunction& out, bool osr, std::string* reason)
{
    try {
        Lowering lowering(rt, method, out, osr);
        lowering.run();
        return true;
    } catch (const Refuse& why) {
        if (reason)
            *reason = why.why;
        return false;
    }
}

}  // namespace typed
}  // namespace internal
}  // namespace script
