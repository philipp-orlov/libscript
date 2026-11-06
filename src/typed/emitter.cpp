// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "script/internal/typed/emitter.hpp"

#include <algorithm>
#include <cstring>

#include "script/internal/stack_guard.hpp"

#include "script/internal/typed/object.hpp"
#include "script/internal/typed/types.hpp"
#include "script/internal/typed/vm.hpp"

namespace script {
namespace internal {
namespace typed {

namespace {

std::uint32_t bitsOfFloat(float f) noexcept
{
    std::uint32_t bits = 0;
    std::memcpy(&bits, &f, sizeof bits);
    return bits;
}

std::uint64_t bitsOfDouble(double d) noexcept
{
    std::uint64_t bits = 0;
    std::memcpy(&bits, &d, sizeof bits);
    return bits;
}

bool isLong(const TypeInfo* type) noexcept
{
    return type && (type->kind == TypeKind::Long || type->kind == TypeKind::ULong);
}

/// Which opcode turns a value of `type` into text. A `uint` and a `ulong` share their slot with the signed types, so
/// only the static type can say that their bits are to be read as unsigned.
Op toStringOp(const TypeInfo* type) noexcept
{
    return type && (type->kind == TypeKind::UInt || type->kind == TypeKind::ULong) ? Op::ToStrU : Op::ToStr;
}

/// The type an operand had before the checker boxed it for a string `+`, which is the type that says whether it is
/// unsigned. Boxing emits nothing for a primitive, so looking through it changes no code.
Op toStringOp(const Expr* operand) noexcept
{
    while (operand && operand->kind == ExprKind::Convert) {
        const auto* convert = static_cast<const ConvertExpr*>(operand);
        if (convert->conv != ConvKind::Boxing || !convert->operand)
            break;
        operand = convert->operand;
    }
    return operand ? toStringOp(operand->type) : Op::ToStr;
}

}  // namespace

Emitter::Emitter(Runtime& runtime, AssemblyImpl& assembly, const CompileOptions& options)
    : rt_(runtime), assembly_(assembly), options_(options)
{}

// ------------------------------------------------------- instructions ---

void Emitter::adjust(int delta)
{
    stack_ += delta;
    if (stack_ > maxStack_)
        maxStack_ = stack_;
    if (stack_ < 0)
        stack_ = 0;
}

void Emitter::op(Op code)
{
    adjust(opStackDelta(code));
    emitOp(this->code(), code);
}

void Emitter::op(Op code, std::uint32_t a)
{
    adjust(opStackDelta(code));
    emitOp(this->code(), code);
    emitU32(this->code(), a);
}

void Emitter::op(Op code, std::uint32_t a, std::uint32_t b)
{
    adjust(opStackDelta(code));
    emitOp(this->code(), code);
    emitU32(this->code(), a);
    emitU32(this->code(), b);
}

void Emitter::op(Op code, std::uint32_t a, std::uint32_t b, std::uint32_t c)
{
    adjust(opStackDelta(code));
    emitOp(this->code(), code);
    emitU32(this->code(), a);
    emitU32(this->code(), b);
    emitU32(this->code(), c);
}

std::size_t Emitter::jump(Op code)
{
    adjust(opStackDelta(code));
    emitOp(this->code(), code);
    const std::size_t at = this->code().size();
    emitU32(this->code(), 0);
    return at;
}

void Emitter::patchTo(std::size_t at, std::uint32_t target)
{
    patchU32(code(), at, target);
}

void Emitter::patchHere(std::size_t at)
{
    patchU32(code(), at, here());
}

void Emitter::line(std::uint32_t number)
{
    if (!options_.debugInfo || number == 0 || number == lastLine_)
        return;
    lastLine_ = number;
    method_->body.lines.push_back({here(), number});
}

std::uint32_t Emitter::typeRef(const TypeInfo* type)
{
    for (std::size_t i = 0; i < assembly_.typeRefs.size(); ++i) {
        if (assembly_.typeRefs[i] == type)
            return std::uint32_t(i);
    }
    assembly_.typeRefs.push_back(type);
    return std::uint32_t(assembly_.typeRefs.size() - 1);
}

std::uint32_t Emitter::methodRef(const MethodInfo* method)
{
    for (std::size_t i = 0; i < assembly_.methodRefs.size(); ++i) {
        if (assembly_.methodRefs[i] == method)
            return std::uint32_t(i);
    }
    assembly_.methodRefs.push_back(method);
    return std::uint32_t(assembly_.methodRefs.size() - 1);
}

std::uint32_t Emitter::stringRef(StringView text)
{
    Value literal = rt_.str(text);
    for (std::size_t i = 0; i < assembly_.strings.size(); ++i) {
        if (detail::Access::ptr(assembly_.strings[i]) == detail::Access::ptr(literal))
            return std::uint32_t(i);
    }
    assembly_.strings.push_back(std::move(literal));
    return std::uint32_t(assembly_.strings.size() - 1);
}

std::uint32_t Emitter::scratch(const TypeInfo* type)
{
    const std::uint32_t slot = std::uint32_t(method_->body.localTypes.size());
    method_->body.localTypes.push_back(type);
    method_->body.localBoxed.push_back(0);
    const std::uint32_t base =
        (method_->isStatic() ? 0u : 1u) + std::uint32_t(method_->params.size());
    method_->numLocals = std::uint32_t(method_->body.localTypes.size()) - base;
    return slot;
}

// ------------------------------------------------------------- methods ---

void Emitter::emitUnit(CompilationUnit* unit)
{
    for (TypeDecl* decl : unit->types) {
        if (!decl->info)
            continue;
        for (MethodDecl* method : decl->methods) {
            if (method->info)
                emitMethod(decl, method);
        }
    }
}

void Emitter::emitMethod(TypeDecl* decl, MethodDecl* method)
{
    MethodInfo* info = method->info;
    if (info->isAbstract() || info->isNative())
        return;

    // Save whatever method is already being emitted: a lambda's body is
    // generated in the middle of the one that creates the delegate.
    MethodInfo* savedMethod = method_;
    TypeDecl* savedType = typeDecl_;
    MethodDecl* savedDecl = decl_;
    const int savedStack = stack_;
    const int savedMax = maxStack_;
    const std::uint32_t savedLine = lastLine_;
    const int savedProtected = protectedDepth_;
    const std::uint32_t savedReturnSlot = returnSlot_;
    const bool savedHasReturnSlot = hasReturnSlot_;
    std::vector<std::size_t> savedReturns;
    savedReturns.swap(returnJumps_);
    std::vector<Loop> savedLoops;
    savedLoops.swap(loops_);

    method_ = info;
    typeDecl_ = decl;
    decl_ = method;
    stack_ = 0;
    maxStack_ = 0;
    lastLine_ = 0;
    protectedDepth_ = 0;
    hasReturnSlot_ = false;
    returnSlot_ = 0;

    if (info->accessorSlot != MethodInfo::kNoSlot && !method->body && !method->expressionBody) {
        emitAccessor(info);
    } else {
        // Boxed locals -- captured by a lambda, or the target of a `ref` --
        // live in a heap cell. Make the cells once, on entry. A `ref`
        // parameter already arrives as a box the caller owns.
        const std::uint32_t paramBase = info->isStatic() ? 0u : 1u;
        for (std::size_t slot = 0; slot < info->body.localBoxed.size(); ++slot) {
            if (!info->body.localBoxed[slot])
                continue;
            if (slot >= paramBase && slot < paramBase + info->params.size()) {
                const ParamMode mode = info->params[slot - paramBase].mode;
                if (mode == ParamMode::Ref || mode == ParamMode::Out)
                    continue;
            }
            const bool isCapture =
                std::find(info->captureSlots.begin(), info->captureSlots.end(),
                          std::uint32_t(slot)) != info->captureSlots.end();
            if (!isCapture)
                op(Op::NewBox, std::uint32_t(slot));
        }

        if (method->isConstructor)
            emitConstructorPrologue(decl, method);
        if (method->name == ascii(".cctor"))
            emitStaticInitializers(decl);

        if (method->expressionBody) {
            if (info->returnType->kind == TypeKind::Void) {
                emitDiscard(method->expressionBody);
                op(Op::RetVoid);
            } else {
                emitExpr(method->expressionBody);
                op(Op::Ret);
            }
        } else {
            emitStatement(method->body);
            // The epilogue every `return` inside a `finally`-guarded region
            // leaves to, and the fall-off-the-end path, are the same code.
            for (std::size_t at : returnJumps_)
                patchHere(at);
            if (info->returnType->kind == TypeKind::Void) {
                op(Op::RetVoid);
            } else if (hasReturnSlot_) {
                op(Op::LdLoc, returnSlot_);
                op(Op::Ret);
            } else {
                // Falling off the end of a value-returning method is a checker
                // error; emitting the default keeps the VM total either way.
                emitConstant(defaultValue(info->returnType), info->returnType);
                op(Op::Ret);
            }
        }
    }

    info->body.maxStack = std::uint32_t(maxStack_ + 4);

    method_ = savedMethod;
    typeDecl_ = savedType;
    decl_ = savedDecl;
    stack_ = savedStack;
    maxStack_ = savedMax;
    lastLine_ = savedLine;
    protectedDepth_ = savedProtected;
    returnSlot_ = savedReturnSlot;
    hasReturnSlot_ = savedHasReturnSlot;
    returnJumps_.swap(savedReturns);
    loops_.swap(savedLoops);
}

void Emitter::emitAccessor(MethodInfo* info)
{
    const bool isSetter = info->name.compare(0, 4, u"set_") == 0;
    if (isSetter) {
        if (info->isStatic()) {
            op(Op::LdLoc, 0);
            op(Op::StSFld, typeRef(info->owner), info->accessorSlot);
        } else {
            op(Op::LdLoc, 0);
            op(Op::LdLoc, 1);
            op(Op::StFld, info->accessorSlot);
        }
        op(Op::RetVoid);
        return;
    }
    if (info->isStatic())
        op(Op::LdSFld, typeRef(info->owner), info->accessorSlot);
    else {
        op(Op::LdLoc, 0);
        op(Op::LdFld, info->accessorSlot);
    }
    op(Op::Ret);
}

void Emitter::emitConstructorPrologue(TypeDecl* decl, MethodDecl* method)
{
    // `this(...)` runs the other constructor, which already did all of this.
    if (method->chainsToThis) {
        if (method->chainCtor) {
            op(Op::LdLoc, 0);
            for (const Argument& arg : method->chainArgs)
                emitExpr(arg.value);
            emitCall(method->chainCtor, std::uint32_t(method->chainArgs.size()) + 1, false, false);
        }
        return;
    }

    if (method->chainsToBase && method->chainCtor) {
        op(Op::LdLoc, 0);
        for (const Argument& arg : method->chainArgs)
            emitExpr(arg.value);
        emitCall(method->chainCtor, std::uint32_t(method->chainArgs.size()) + 1, false, false);
    } else if (decl->info->base && decl->info->base->assembly) {
        // The base may live in another assembly; what matters is that it is a
        // declared type with a constructor to run, not where it was compiled.
        std::vector<MethodInfo*> ctors;
        decl->info->base->collectMethods(ascii(".ctor"), ctors);
        for (MethodInfo* ctor : ctors) {
            if (ctor->params.empty()) {
                op(Op::LdLoc, 0);
                emitCall(ctor, 1, false, false);
                break;
            }
        }
    }

    for (const FieldDecl& field : decl->fields) {
        if (!field.init || (field.modifiers & (ModStatic | ModConst)))
            continue;
        const FieldInfo* info = decl->info->findField(field.name);
        if (!info)
            continue;
        line(field.line);
        op(Op::LdLoc, 0);
        emitExpr(field.init);
        op(Op::StFld, info->slot);
    }
    for (const PropertyDecl& prop : decl->props) {
        if (!prop.init || (prop.modifiers & ModStatic))
            continue;
        const FieldInfo* info = decl->info->findField(prop.backingField);
        if (!info)
            continue;
        line(prop.line);
        op(Op::LdLoc, 0);
        emitExpr(prop.init);
        op(Op::StFld, info->slot);
    }
}

void Emitter::emitStaticInitializers(TypeDecl* decl)
{
    for (const FieldDecl& field : decl->fields) {
        if (!field.init || !(field.modifiers & ModStatic) || (field.modifiers & ModConst))
            continue;
        const FieldInfo* info = decl->info->findField(field.name);
        if (!info)
            continue;
        line(field.line);
        emitExpr(field.init);
        emitStatic(Op::StSFld, info);
    }
    for (const PropertyDecl& prop : decl->props) {
        if (!prop.init || !(prop.modifiers & ModStatic))
            continue;
        const FieldInfo* info = decl->info->findField(prop.backingField);
        if (!info)
            continue;
        line(prop.line);
        emitExpr(prop.init);
        emitStatic(Op::StSFld, info);
    }
}

// ---------------------------------------------------------- statements ---

void Emitter::emitStatement(Stmt* stmt)
{
    checkNativeStack();
    if (!stmt)
        return;
    line(stmt->line);
    switch (stmt->kind) {
        case StmtKind::Block: emitBlock(static_cast<BlockStmt*>(stmt)); return;
        case StmtKind::Empty: return;
        case StmtKind::Expression:
            emitDiscard(static_cast<ExprStmt*>(stmt)->expr);
            return;
        case StmtKind::LocalDeclaration: {
            auto* node = static_cast<LocalDeclStmt*>(stmt);
            if (node->deconstruction) {
                emitDeconstruction(node->deconstruction, node->deconstructionValue);
                return;
            }
            for (const VarDeclarator& var : node->vars) {
                if (!var.init)
                    continue;
                emitExpr(var.init);
                if (var.type && var.type->kind == TypeKind::Struct &&
                    var.init->kind != ExprKind::ObjectCreation)
                    op(Op::CloneVal);
                op(method_->body.localBoxed[var.slot] ? Op::StBoxed : Op::StLoc, var.slot);
            }
            return;
        }
        case StmtKind::If: emitIf(static_cast<IfStmt*>(stmt)); return;
        case StmtKind::While: emitWhile(static_cast<WhileStmt*>(stmt)); return;
        case StmtKind::DoWhile: emitDoWhile(static_cast<DoWhileStmt*>(stmt)); return;
        case StmtKind::For: emitFor(static_cast<ForStmt*>(stmt)); return;
        case StmtKind::ForEach: emitForEach(static_cast<ForEachStmt*>(stmt)); return;
        case StmtKind::Switch: emitSwitch(static_cast<SwitchStmt*>(stmt)); return;
        case StmtKind::Try: emitTry(static_cast<TryStmt*>(stmt)); return;
        case StmtKind::Return: emitReturn(static_cast<ReturnStmt*>(stmt)); return;
        case StmtKind::Break:
            if (!loops_.empty())
                loops_.back().breaks.push_back(jump(Op::Br));
            return;
        case StmtKind::Continue:
            if (!loops_.empty())
                loops_.back().continues.push_back(jump(Op::Br));
            return;
        case StmtKind::Throw: {
            auto* node = static_cast<ThrowStmt*>(stmt);
            if (node->value) {
                emitExpr(node->value);
                op(Op::Throw);
            } else {
                op(Op::Rethrow);
            }
            return;
        }
        case StmtKind::Using: emitUsing(static_cast<UsingStmt*>(stmt)); return;
        case StmtKind::LocalFunction:
            // The body was emitted with the other methods of the type; the
            // declaration itself produces no code.
            return;
        case StmtKind::Yield:
            // An iterator body is rewritten before it reaches here.
            return;
    }
}

void Emitter::emitDeconstruction(Deconstruction* node, Expr* value)
{
    // Hold the source in its slot, then pull each piece out of it.
    emitExpr(value);
    op(Op::StLoc, node->sourceSlot);

    if (node->call) {
        emitDiscard(node->call);
    } else {
        for (const DeconstructTarget& target : node->targets) {
            op(Op::LdLoc, node->sourceSlot);
            op(Op::LdFld, node->sourceType->fields[&target - node->targets.data()].slot);
            op(method_->body.localBoxed[target.slot] ? Op::StBoxed : Op::StLoc, target.slot);
        }
    }

    for (const DeconstructTarget& target : node->targets) {
        if (target.nested)
            emitDeconstruction(target.nested, target.nestedValue);
    }
}

void Emitter::emitUsing(UsingStmt* stmt)
{
    // The checker turned this into try/finally; there is nothing else to do.
    if (stmt->lowered)
        emitStatement(stmt->lowered);
}

void Emitter::emitBlock(BlockStmt* block)
{
    for (Stmt* statement : block->statements)
        emitStatement(statement);
}

void Emitter::emitReturn(ReturnStmt* stmt)
{
    if (protectedDepth_ > 0) {
        // Leave the protected region so the intervening `finally` blocks run;
        // the value waits in a slot until the epilogue.
        if (stmt->value) {
            emitExpr(stmt->value);
            if (!hasReturnSlot_) {
                returnSlot_ = scratch(method_->returnType);
                hasReturnSlot_ = true;
            }
            op(Op::StLoc, returnSlot_);
        }
        returnJumps_.push_back(jump(Op::Leave));
        return;
    }
    if (!stmt->value) {
        op(Op::RetVoid);
        return;
    }
    emitExpr(stmt->value);
    op(Op::Ret);
}

void Emitter::emitIf(IfStmt* stmt)
{
    emitExpr(stmt->condition);
    const std::size_t toElse = jump(Op::BrFalse);
    emitStatement(stmt->thenBranch);
    if (stmt->elseBranch) {
        const std::size_t toEnd = jump(Op::Br);
        patchHere(toElse);
        emitStatement(stmt->elseBranch);
        patchHere(toEnd);
    } else {
        patchHere(toElse);
    }
}

void Emitter::emitWhile(WhileStmt* stmt)
{
    const std::uint32_t top = here();
    emitExpr(stmt->condition);
    const std::size_t out = jump(Op::BrFalse);
    loops_.push_back({});
    emitStatement(stmt->body);
    Loop loop = loops_.back();
    loops_.pop_back();
    for (std::size_t at : loop.continues)
        patchTo(at, top);
    op(Op::Br, top);
    patchHere(out);
    for (std::size_t at : loop.breaks)
        patchHere(at);
}

void Emitter::emitDoWhile(DoWhileStmt* stmt)
{
    const std::uint32_t top = here();
    loops_.push_back({});
    emitStatement(stmt->body);
    Loop loop = loops_.back();
    loops_.pop_back();
    const std::uint32_t test = here();
    for (std::size_t at : loop.continues)
        patchTo(at, test);
    emitExpr(stmt->condition);
    op(Op::BrTrue, top);
    for (std::size_t at : loop.breaks)
        patchHere(at);
}

void Emitter::emitFor(ForStmt* stmt)
{
    for (Stmt* init : stmt->init)
        emitStatement(init);
    const std::uint32_t top = here();
    std::size_t out = 0;
    bool hasTest = false;
    if (stmt->condition) {
        emitExpr(stmt->condition);
        out = jump(Op::BrFalse);
        hasTest = true;
    }
    loops_.push_back({});
    emitStatement(stmt->body);
    Loop loop = loops_.back();
    loops_.pop_back();

    const std::uint32_t step = here();
    for (std::size_t at : loop.continues)
        patchTo(at, step);
    for (Expr* update : stmt->update)
        emitDiscard(update);
    op(Op::Br, top);
    if (hasTest)
        patchHere(out);
    for (std::size_t at : loop.breaks)
        patchHere(at);
}

void Emitter::emitForEach(ForEachStmt* stmt)
{
    // Every supported sequence is indexable, so one shape of loop covers them
    // all: evaluate once, keep a cursor, read the element each time round.
    emitExpr(stmt->sequence);
    if (stmt->over == ForEachStmt::Over::Dictionary)
        emitCall(stmt->getEnumerator, 1, false, false);
    op(Op::StLoc, stmt->sequenceSlot);
    op(Op::LdcI4, 0);
    op(Op::StLoc, stmt->cursorSlot);

    const std::uint32_t top = here();
    op(Op::LdLoc, stmt->cursorSlot);
    switch (stmt->over) {
        case ForEachStmt::Over::Array:
            op(Op::LdLoc, stmt->sequenceSlot);
            op(Op::LdLen);
            break;
        case ForEachStmt::Over::String:
        case ForEachStmt::Over::List:
        case ForEachStmt::Over::Dictionary:
        case ForEachStmt::Over::Enumerable: {
            op(Op::LdLoc, stmt->sequenceSlot);
            const MethodInfo* count = stmt->countGetter;
            if (!count && stmt->sequence->type) {
                std::vector<MethodInfo*> getters;
                stmt->sequence->type->collectMethods(ascii("get_Length"), getters);
                if (getters.empty())
                    stmt->sequence->type->collectMethods(ascii("get_Count"), getters);
                count = getters.empty() ? nullptr : getters[0];
            }
            if (count)
                emitCall(count, 1, false, false);
            else
                op(Op::LdLen);
            break;
        }
    }
    op(Op::CltI4);
    const std::size_t out = jump(Op::BrFalse);

    if (method_->body.localBoxed[stmt->slot]) {
        // A lambda made inside the loop captures this iteration's variable,
        // which the language says is a fresh one each time round.
        op(Op::NewBox, stmt->slot);
    }
    op(Op::LdLoc, stmt->sequenceSlot);
    op(Op::LdLoc, stmt->cursorSlot);
    if (stmt->over == ForEachStmt::Over::Array)
        op(Op::LdElem);
    else if (stmt->itemGetter)
        emitCall(stmt->itemGetter, 2, false, false);
    else
        op(Op::LdElem);
    op(method_->body.localBoxed[stmt->slot] ? Op::StBoxed : Op::StLoc, stmt->slot);

    loops_.push_back({});
    emitStatement(stmt->body);
    Loop loop = loops_.back();
    loops_.pop_back();

    const std::uint32_t step = here();
    for (std::size_t at : loop.continues)
        patchTo(at, step);
    op(Op::LdLoc, stmt->cursorSlot);
    op(Op::LdcI4, 1);
    op(Op::AddI4);
    op(Op::StLoc, stmt->cursorSlot);
    op(Op::Br, top);
    patchHere(out);
    for (std::size_t at : loop.breaks)
        patchHere(at);
}

void Emitter::emitSwitch(SwitchStmt* stmt)
{
    // A comparison chain: the subject is evaluated once into a slot, then
    // tested against each label. Dense integer switches would be a jump table;
    // that is an optimisation for the day the profile asks for it.
    const std::uint32_t subject =
        stmt->usesPatterns ? stmt->subjectSlot : scratch(stmt->subject->type);
    emitExpr(stmt->subject);
    op(Op::StLoc, subject);

    const bool isString = stmt->subject->type &&
                          stmt->subject->type->kind == TypeKind::String;
    const bool wide = isLong(stmt->subject->type);

    std::vector<std::size_t> entries;
    std::size_t defaultJump = 0;
    bool hasDefault = false;

    // One entry per label, in source order, so the first match wins and no
    // later test is evaluated for its side effects.
    std::vector<std::size_t> sectionEntryCount;
    for (const SwitchSection& section : stmt->sections) {
        std::size_t count = 0;
        for (const Expr* label : section.labels) {
            op(Op::LdLoc, subject);
            emitExpr(label);
            op(isString ? Op::CeqStr : (wide ? Op::CeqI8 : Op::CeqI4));
            entries.push_back(jump(Op::BrTrue));
            ++count;
        }
        if (section.test) {
            emitExpr(section.test);
            entries.push_back(jump(Op::BrTrue));
            ++count;
        }
        sectionEntryCount.push_back(count);
    }
    for (std::size_t i = 0; i < stmt->sections.size(); ++i) {
        if (stmt->sections[i].isDefault) {
            hasDefault = true;
            break;
        }
    }
    defaultJump = jump(Op::Br);

    loops_.push_back({});
    std::vector<std::uint32_t> bodyOffsets;
    std::size_t labelIndex = 0;
    std::vector<std::uint32_t> defaults;
    std::size_t sectionIndex = 0;
    for (const SwitchSection& section : stmt->sections) {
        const std::uint32_t at = here();
        for (std::size_t i = 0; i < sectionEntryCount[sectionIndex]; ++i)
            patchTo(entries[labelIndex++], at);
        ++sectionIndex;
        if (section.isDefault)
            defaults.push_back(at);
        for (Stmt* inner : section.statements)
            emitStatement(inner);
        bodyOffsets.push_back(at);
    }
    Loop loop = loops_.back();
    loops_.pop_back();

    const std::uint32_t end = here();
    patchTo(defaultJump, hasDefault && !defaults.empty() ? defaults.front() : end);
    for (std::size_t at : loop.breaks)
        patchTo(at, end);
    for (std::size_t at : loop.continues)
        patchTo(at, end);
}

void Emitter::emitTry(TryStmt* stmt)
{
    const bool hasFinally = stmt->finallyBlock != nullptr;
    if (hasFinally)
        ++protectedDepth_;
    const std::uint32_t protectedStart = here();
    emitStatement(stmt->body);
    const std::size_t leaveBody = jump(Op::Leave);
    const std::uint32_t protectedEnd = here();

    std::vector<std::size_t> leaves{leaveBody};
    for (const CatchClause& clause : stmt->catches) {
        Handler handler;
        handler.start = protectedStart;
        handler.end = protectedEnd;
        handler.target = here();
        handler.stackDepth = 0;
        handler.catchType = clause.type;
        handler.exceptionSlot = clause.slot;
        handler.kind = HandlerKind::Catch;
        method_->body.handlers.push_back(handler);

        emitStatement(clause.body);
        leaves.push_back(jump(Op::Leave));
    }

    if (stmt->finallyBlock) {
        Handler handler;
        handler.start = protectedStart;
        // The finally protects the catch blocks too, which is why its region
        // runs to here rather than to the end of the try body.
        handler.end = here();
        handler.target = here();
        handler.stackDepth = 0;
        handler.kind = HandlerKind::Finally;
        method_->body.handlers.push_back(handler);

        --protectedDepth_;
        emitStatement(stmt->finallyBlock);
        op(Op::EndFinally);
    }
    for (std::size_t at : leaves)
        patchHere(at);
}

// --------------------------------------------------------- expressions ---

void Emitter::emitConstant(const Value& value, const TypeInfo* type)
{
    switch (value.type()) {
        case Type::Null:
            if (type && type->kind == TypeKind::Struct)
                op(Op::InitObj, typeRef(type));
            else
                op(Op::LdNull);
            return;
        case Type::Bool: op(value.toBool() ? Op::LdTrue : Op::LdFalse); return;
        case Type::Char:
            // The tag is what `Append(char)` and `ToString` print by, so it has to be there at run time.
            op(Op::LdcI4, std::uint32_t(value.toInt()));
            op(Op::ConvChar);
            return;
        case Type::Int: op(Op::LdcI4, std::uint32_t(value.toInt())); return;
        case Type::Long: {
            const std::uint64_t bits = std::uint64_t(value.toLong());
            op(Op::LdcI8, std::uint32_t(bits), std::uint32_t(bits >> 32));
            return;
        }
        case Type::Float: op(Op::LdcR4, bitsOfFloat(float(value.toDouble()))); return;
        case Type::Double: {
            const std::uint64_t bits = bitsOfDouble(value.toDouble());
            op(Op::LdcR8, std::uint32_t(bits), std::uint32_t(bits >> 32));
            return;
        }
        case Type::String: {
            auto* text = static_cast<StringCell*>(detail::Access::ptr(value));
            op(Op::LdStr, stringRef(text ? text->text() : StringView()));
            return;
        }
        default: op(Op::LdNull); return;
    }
}

void Emitter::emitReceiver(const Expr* expr)
{
    if (expr->kind == ExprKind::MemberAccess) {
        const auto* member = static_cast<const MemberAccessExpr*>(expr);
        if (member->target)
            emitExpr(member->target);
        return;
    }
    if (expr->kind == ExprKind::Name) {
        const auto* name = static_cast<const NameExpr*>(expr);
        if (name->symbol == SymbolKind::Field || name->symbol == SymbolKind::Property)
            op(Op::LdLoc, 0);  // the implicit `this`
    }
}

void Emitter::emitLoad(const Expr* expr)
{
    switch (expr->kind) {
        case ExprKind::Name: {
            const auto* name = static_cast<const NameExpr*>(expr);
            switch (name->symbol) {
                case SymbolKind::Local:
                case SymbolKind::Parameter:
                    op(method_->body.localBoxed[name->slot] ? Op::LdBoxed : Op::LdLoc, name->slot);
                    return;
                case SymbolKind::Field:
                    op(Op::LdLoc, 0);
                    op(Op::LdFld, name->slot);
                    return;
                case SymbolKind::StaticField:
                    emitStatic(Op::LdSFld, name->field);
                    return;
                case SymbolKind::Property:
                    op(Op::LdLoc, 0);
                    if (name->prop && name->prop->getter)
                        emitCall(name->prop->getter, 1, name->prop->getter->isVirtual(), false);
                    return;
                case SymbolKind::StaticProperty:
                    if (name->prop && name->prop->getter)
                        emitCall(name->prop->getter, 0, false, false);
                    return;
                default: op(Op::LdNull); return;
            }
        }
        case ExprKind::MemberAccess: {
            const auto* member = static_cast<const MemberAccessExpr*>(expr);
            std::size_t skip = 0;
            const bool guarded = member->nullConditional && member->target;
            if (member->target) {
                emitExpr(member->target);
                if (guarded) {
                    op(Op::Dup);
                    op(Op::IsNull);
                    skip = jump(Op::BrTrue);
                }
            }
            if (member->arrayLength) {
                op(Op::LdLen);
            } else if (member->symbol == SymbolKind::Field) {
                op(Op::LdFld, member->field->slot);
            } else if (member->symbol == SymbolKind::StaticField) {
                emitStatic(Op::LdSFld, member->field);
            } else if (member->symbol == SymbolKind::Property && member->prop->getter) {
                emitCall(member->prop->getter, 1, member->prop->getter->isVirtual(),
                         member->prop->owner && member->prop->owner->kind == TypeKind::Interface);
            } else if (member->symbol == SymbolKind::StaticProperty && member->prop->getter) {
                emitCall(member->prop->getter, 0, false, false);
            } else {
                op(Op::LdNull);
            }
            if (guarded) {
                const std::size_t done = jump(Op::Br);
                patchHere(skip);
                op(Op::Pop);
                op(Op::LdNull);
                patchHere(done);
            }
            return;
        }
        case ExprKind::ElementAccess: {
            const auto* element = static_cast<const ElementAccessExpr*>(expr);
            emitExpr(element->target);
            emitExpr(element->index);
            if (element->getter)
                emitCall(element->getter, 2, false, false);
            else
                op(Op::LdElem);
            return;
        }
        default: emitExpr(expr); return;
    }
}

void Emitter::emitExpr(const Expr* expr)
{
    checkNativeStack();
    if (!expr) {
        op(Op::LdNull);
        return;
    }
    if (expr->isConstant) {
        emitConstant(expr->constant, expr->type);
        return;
    }

    switch (expr->kind) {
        case ExprKind::Literal: {
            const auto* literal = static_cast<const LiteralExpr*>(expr);
            if (literal->literal == LiteralKind::Null) {
                if (expr->type && expr->type->kind == TypeKind::Struct)
                    op(Op::InitObj, typeRef(expr->type));
                else
                    op(Op::LdNull);
                return;
            }
            emitConstant(literal->constant, literal->type);
            return;
        }
        case ExprKind::Interpolated:
            emitInterpolated(static_cast<const InterpolatedExpr*>(expr));
            return;
        case ExprKind::Name:
        case ExprKind::MemberAccess: emitLoad(expr); return;
        case ExprKind::ElementAccess: {
            const auto* node = static_cast<const ElementAccessExpr*>(expr);
            if (node->slice) {
                emitExpr(node->slice);
                return;
            }
            emitLoad(expr);
            return;
        }
        case ExprKind::Invocation:
            emitInvocation(static_cast<const InvocationExpr*>(expr), true);
            return;
        case ExprKind::ObjectCreation:
            emitObjectCreation(static_cast<const ObjectCreationExpr*>(expr));
            return;
        case ExprKind::ArrayCreation:
            emitArrayCreation(static_cast<const ArrayCreationExpr*>(expr));
            return;
        case ExprKind::Unary: emitUnary(static_cast<const UnaryExpr*>(expr)); return;
        case ExprKind::Binary: emitBinary(static_cast<const BinaryExpr*>(expr)); return;
        case ExprKind::Assignment:
            emitAssignment(static_cast<const AssignmentExpr*>(expr), true);
            return;
        case ExprKind::Conditional: {
            const auto* node = static_cast<const ConditionalExpr*>(expr);
            emitExpr(node->condition);
            const std::size_t toElse = jump(Op::BrFalse);
            const int before = stack_;
            emitExpr(node->whenTrue);
            const std::size_t toEnd = jump(Op::Br);
            patchHere(toElse);
            stack_ = before;
            emitExpr(node->whenFalse);
            patchHere(toEnd);
            return;
        }
        case ExprKind::Cast: {
            const auto* node = static_cast<const CastExpr*>(expr);
            emitExpr(node->operand);
            if (node->operand->type != node->type && isReferenceKind(node->type->kind) &&
                node->type->kind != TypeKind::String)
                op(Op::CastClass, typeRef(node->type));
            return;
        }
        case ExprKind::TypeTest: {
            const auto* node = static_cast<const TypeTestExpr*>(expr);
            if (node->loweredPattern) {
                emitExpr(node->loweredPattern);
                return;
            }
            emitExpr(node->operand);
            if (node->isAs) {
                op(Op::AsInst, typeRef(node->type));
            } else if (!node->bindingName.empty()) {
                op(Op::Dup);
                op(Op::AsInst, typeRef(node->typeRef->resolved));
                op(Op::StLoc, node->bindingSlot);
                op(Op::IsInst, typeRef(node->typeRef->resolved));
            } else {
                op(Op::IsInst, typeRef(node->typeRef->resolved));
            }
            return;
        }
        case ExprKind::Sequence: {
            const auto* node = static_cast<const SequenceExpr*>(expr);
            for (const Expr* before : node->before)
                emitDiscard(before);
            emitExpr(node->value);
            return;
        }
        case ExprKind::Switch: {
            emitExpr(static_cast<const SwitchExpr*>(expr)->lowered);
            return;
        }
        case ExprKind::Tuple: {
            const auto* node = static_cast<const TupleExpr*>(expr);
            for (const TupleElement& element : node->elements)
                emitExpr(element.value);
            emitOp(code(), Op::NewObj);
            emitU32(code(), methodRef(node->ctor));
            emitU32(code(), std::uint32_t(node->elements.size()));
            adjust(-int(node->elements.size()) + 1);
            return;
        }
        case ExprKind::With: {
            const auto* node = static_cast<const WithExpr*>(expr);
            emitExpr(node->target);
            emitOp(code(), node->cloneMethod->isVirtual() ? Op::CallVirt : Op::Call);
            emitU32(code(), methodRef(node->cloneMethod));
            emitU32(code(), 1);
            for (const InitializerEntry& entry : node->initializers) {
                op(Op::Dup);
                emitExpr(entry.value);
                if (entry.field) {
                    op(Op::StFld, entry.field->slot);
                } else if (entry.setter) {
                    emitOp(code(), Op::Call);
                    emitU32(code(), methodRef(entry.setter));
                    emitU32(code(), 2);
                    adjust(-2);
                }
            }
            return;
        }
        case ExprKind::Throw: {
            const auto* node = static_cast<const ThrowExpr*>(expr);
            emitExpr(node->value);
            op(Op::Throw);
            // The stack never gets here, but every path has to look like it
            // leaves a value for the verifier's benefit.
            op(Op::LdNull);
            return;
        }
        case ExprKind::Lambda: emitLambda(static_cast<const LambdaExpr*>(expr)); return;
        case ExprKind::This: op(Op::LdLoc, 0); return;
        case ExprKind::Base: op(Op::LdLoc, 0); return;
        case ExprKind::Default:
            emitConstant(defaultValue(expr->type), expr->type);
            return;
        case ExprKind::Convert: emitConvert(static_cast<const ConvertExpr*>(expr)); return;
        case ExprKind::Collection: {
            emitExpr(static_cast<const CollectionExpr*>(expr)->lowered);
            return;
        }
        case ExprKind::Discard:
        case ExprKind::Range:
        case ExprKind::Index:
            // All three are consumed by the construct that contains them.
            op(Op::LdNull);
            return;
        case ExprKind::TypeOf:
        case ExprKind::NameOf: op(Op::LdNull); return;
    }
}

void Emitter::emitDiscard(const Expr* expr)
{
    if (!expr)
        return;
    switch (expr->kind) {
        case ExprKind::Assignment:
            emitAssignment(static_cast<const AssignmentExpr*>(expr), false);
            return;
        case ExprKind::Invocation:
            emitInvocation(static_cast<const InvocationExpr*>(expr), false);
            return;
        case ExprKind::Unary: {
            const auto* node = static_cast<const UnaryExpr*>(expr);
            if (node->op == UnaryOp::PreIncrement || node->op == UnaryOp::PreDecrement ||
                node->op == UnaryOp::PostIncrement || node->op == UnaryOp::PostDecrement) {
                emitIncDec(node, false);
                return;
            }
            break;
        }
        default: break;
    }
    emitExpr(expr);
    op(Op::Pop);
}

void Emitter::emitConvert(const ConvertExpr* expr)
{
    if (expr->conv == ConvKind::MethodGroup) {
        // A method group becomes a delegate over that method.
        const std::vector<MethodInfo*>* group =
            expr->operand->kind == ExprKind::Name
                ? &static_cast<const NameExpr*>(expr->operand)->group
                : &static_cast<const MemberAccessExpr*>(expr->operand)->group;
        const MethodInfo* target = group->empty() ? nullptr : group->front();
        if (!target) {
            op(Op::LdNull);
            return;
        }
        if (target->isStatic())
            op(Op::LdNull);
        else
            emitReceiver(expr->operand);
        op(Op::NewDlg, typeRef(expr->type), methodRef(target), 0);
        return;
    }

    emitExpr(expr->operand);
    const TypeInfo* from = expr->operand->type;
    const TypeInfo* to = expr->type;
    // Whatever the conversion is called, a uint or ulong turned into text must be printed as unsigned.
    if (to && to->kind == TypeKind::String && toStringOp(expr->operand) == Op::ToStrU) {
        op(Op::ToStrU);
        return;
    }
    switch (expr->conv) {
        case ConvKind::Numeric:
        case ConvKind::EnumToInt:
        case ConvKind::IntToEnum: {
            const bool sourceUnsigned = from && isUnsigned(from->kind);
            switch (to->kind) {
                case TypeKind::SByte: op(Op::ConvI1); break;
                case TypeKind::Byte: op(Op::ConvU1); break;
                case TypeKind::Short: op(Op::ConvI2); break;
                case TypeKind::UShort: op(Op::ConvU2); break;
                case TypeKind::Char: op(Op::ConvChar); break;
                case TypeKind::Int:
                case TypeKind::Enum: op(Op::ConvI4); break;
                case TypeKind::UInt: op(Op::ConvU4); break;
                case TypeKind::Long: op(sourceUnsigned ? Op::ConvU8 : Op::ConvI8); break;
                case TypeKind::ULong: op(Op::ConvU8); break;
                case TypeKind::Float: op(Op::ConvR4); break;
                case TypeKind::Double: op(sourceUnsigned ? Op::ConvR8Un : Op::ConvR8); break;
                default: break;
            }
            return;
        }
        case ConvKind::Boxing:
            // A primitive already carries its tag; a struct is copied, which
            // is what boxing one means.
            if (from && from->kind == TypeKind::Struct)
                op(Op::CloneVal);
            return;
        case ConvKind::Unboxing:
            op(Op::Unbox, typeRef(to));
            return;
        case ConvKind::ToStringCall:
            op(toStringOp(from));
            return;
        case ConvKind::Reference:
            // A widening reference conversion is free; a narrowing one was
            // written as a cast and carries its own check.
            return;
        default: return;
    }
}

void Emitter::emitToString(const Expr* expr)
{
    emitExpr(expr);
    if (!expr->type || expr->type->kind != TypeKind::String)
        op(toStringOp(expr));
}

void Emitter::emitInterpolated(const InterpolatedExpr* expr)
{
    std::uint32_t count = 0;
    for (std::size_t i = 0; i < expr->parts.size(); ++i) {
        if (!expr->parts[i].empty()) {
            op(Op::LdStr, stringRef(expr->parts[i]));
            ++count;
        }
        if (i >= expr->holes.size())
            continue;
        const String& format = i < expr->formats.size() ? expr->formats[i] : String();
        if (!format.empty()) {
            const TypeInfo* stringType = rt_.types->core().stringType;
            std::vector<MethodInfo*> formatters;
            stringType->collectMethods(ascii("FormatValue"), formatters);
            if (!formatters.empty()) {
                emitExpr(expr->holes[i]);
                op(Op::LdStr, stringRef(format));
                emitCall(formatters.front(), 2, false, false);
                ++count;
                continue;
            }
        }
        emitToString(expr->holes[i]);
        ++count;
    }
    if (count == 0)
        op(Op::LdStr, stringRef(StringView()));
    else if (count > 1) {
        op(Op::Concat, count);
        adjust(-(int(count) - 1));
    }
}

void Emitter::emitLambda(const LambdaExpr* expr)
{
    if (!expr->method) {
        op(Op::LdNull);
        return;
    }
    // Generate the lambda's own body first; the emitter saves and restores
    // whatever method it was in the middle of.
    MethodDecl body;
    body.name = expr->method->name;
    body.info = expr->method;
    body.owner = typeDecl_;
    body.line = expr->line;
    body.body = expr->bodyBlock;
    body.expressionBody = expr->bodyExpr;
    emitMethod(typeDecl_, &body);

    if (expr->method->isStatic())
        op(Op::LdNull);
    else
        op(Op::LdLoc, 0);
    for (std::uint32_t slot : expr->capturedSlots)
        op(Op::LdBoxRef, slot);
    op(Op::NewDlg, typeRef(expr->type), methodRef(expr->method),
       std::uint32_t(expr->capturedSlots.size()));
    adjust(-int(expr->capturedSlots.size()));
}

Op Emitter::arithmeticOp(BinaryOp binary, const TypeInfo* type)
{
    const bool wide = isLong(type);
    const bool real = type && isFloating(type->kind);
    const bool single = type && type->kind == TypeKind::Float;
    const bool unsignedType = type && isUnsigned(type->kind);
    switch (binary) {
        case BinaryOp::Add:
            return real ? (single ? Op::AddR4 : Op::AddR8) : (wide ? Op::AddI8 : Op::AddI4);
        case BinaryOp::Subtract:
            return real ? (single ? Op::SubR4 : Op::SubR8) : (wide ? Op::SubI8 : Op::SubI4);
        case BinaryOp::Multiply:
            return real ? (single ? Op::MulR4 : Op::MulR8) : (wide ? Op::MulI8 : Op::MulI4);
        case BinaryOp::Divide:
            if (real)
                return single ? Op::DivR4 : Op::DivR8;
            if (wide)
                return unsignedType ? Op::DivU8 : Op::DivI8;
            return unsignedType ? Op::DivU4 : Op::DivI4;
        case BinaryOp::Modulo:
            if (real)
                return single ? Op::RemR4 : Op::RemR8;
            if (wide)
                return unsignedType ? Op::RemU8 : Op::RemI8;
            return unsignedType ? Op::RemU4 : Op::RemI4;
        case BinaryOp::And: return wide ? Op::AndI8 : Op::AndI4;
        case BinaryOp::Or: return wide ? Op::OrI8 : Op::OrI4;
        case BinaryOp::Xor: return wide ? Op::XorI8 : Op::XorI4;
        case BinaryOp::ShiftLeft: return wide ? Op::ShlI8 : Op::ShlI4;
        case BinaryOp::ShiftRight:
            if (wide)
                return unsignedType ? Op::ShrU8 : Op::ShrI8;
            return unsignedType ? Op::ShrU4 : Op::ShrI4;
        default: return Op::Nop;
    }
}

Op Emitter::compareOp(BinaryOp binary, const TypeInfo* type)
{
    if (type && type->kind == TypeKind::String)
        return binary == BinaryOp::Equal ? Op::CeqStr : Op::CneStr;
    if (type && (type->kind == TypeKind::Null || isReferenceKind(type->kind)))
        return binary == BinaryOp::Equal ? Op::CeqRef : Op::CneRef;

    const bool wide = isLong(type);
    const bool real = type && isFloating(type->kind);
    const bool unsignedType = type && isUnsigned(type->kind);
    switch (binary) {
        case BinaryOp::Equal: return real ? Op::CeqR8 : (wide ? Op::CeqI8 : Op::CeqI4);
        case BinaryOp::NotEqual: return real ? Op::CneR8 : (wide ? Op::CneI8 : Op::CneI4);
        case BinaryOp::Less:
            if (real)
                return Op::CltR8;
            if (wide)
                return unsignedType ? Op::CltU8 : Op::CltI8;
            return unsignedType ? Op::CltU4 : Op::CltI4;
        case BinaryOp::LessEqual:
            if (real)
                return Op::CleR8;
            if (wide)
                return unsignedType ? Op::CleU8 : Op::CleI8;
            return unsignedType ? Op::CleU4 : Op::CleI4;
        case BinaryOp::Greater:
            if (real)
                return Op::CgtR8;
            if (wide)
                return unsignedType ? Op::CgtU8 : Op::CgtI8;
            return unsignedType ? Op::CgtU4 : Op::CgtI4;
        case BinaryOp::GreaterEqual:
            if (real)
                return Op::CgeR8;
            if (wide)
                return unsignedType ? Op::CgeU8 : Op::CgeI8;
            return unsignedType ? Op::CgeU4 : Op::CgeI4;
        default: return Op::Nop;
    }
}

void Emitter::emitBinary(const BinaryExpr* expr)
{
    switch (expr->op) {
        case BinaryOp::LogicalAnd: {
            emitExpr(expr->left);
            const std::size_t shortCircuit = jump(Op::BrFalse);
            emitExpr(expr->right);
            const std::size_t done = jump(Op::Br);
            patchHere(shortCircuit);
            op(Op::LdFalse);
            patchHere(done);
            return;
        }
        case BinaryOp::LogicalOr: {
            emitExpr(expr->left);
            const std::size_t shortCircuit = jump(Op::BrTrue);
            emitExpr(expr->right);
            const std::size_t done = jump(Op::Br);
            patchHere(shortCircuit);
            op(Op::LdTrue);
            patchHere(done);
            return;
        }
        case BinaryOp::Coalesce: {
            emitExpr(expr->left);
            op(Op::Dup);
            op(Op::IsNull);
            const std::size_t useRight = jump(Op::BrTrue);
            const std::size_t done = jump(Op::Br);
            patchHere(useRight);
            op(Op::Pop);
            emitExpr(expr->right);
            patchHere(done);
            return;
        }
        case BinaryOp::Add:
            if (expr->operandType && expr->operandType->kind == TypeKind::String) {
                emitExpr(expr->left);
                emitExpr(expr->right);
                op(Op::Concat, 2);
                adjust(-1);
                return;
            }
            break;
        default: break;
    }

    emitExpr(expr->left);
    emitExpr(expr->right);
    switch (expr->op) {
        case BinaryOp::Equal:
        case BinaryOp::NotEqual:
        case BinaryOp::Less:
        case BinaryOp::LessEqual:
        case BinaryOp::Greater:
        case BinaryOp::GreaterEqual: op(compareOp(expr->op, expr->operandType)); return;
        default: op(arithmeticOp(expr->op, expr->operandType)); return;
    }
}

void Emitter::emitUnary(const UnaryExpr* expr)
{
    switch (expr->op) {
        case UnaryOp::Plus: emitExpr(expr->operand); return;
        case UnaryOp::Not:
            emitExpr(expr->operand);
            op(Op::LogicalNot);
            return;
        case UnaryOp::Minus:
            emitExpr(expr->operand);
            if (expr->type && expr->type->kind == TypeKind::Double)
                op(Op::NegR8);
            else if (expr->type && expr->type->kind == TypeKind::Float)
                op(Op::NegR4);
            else
                op(isLong(expr->type) ? Op::NegI8 : Op::NegI4);
            return;
        case UnaryOp::Complement:
            emitExpr(expr->operand);
            op(isLong(expr->type) ? Op::NotI8 : Op::NotI4);
            return;
        default: emitIncDec(expr, true); return;
    }
}

void Emitter::emitIncDec(const UnaryExpr* expr, bool keepValue)
{
    const Expr* target = expr->operand;
    const bool increment =
        expr->op == UnaryOp::PreIncrement || expr->op == UnaryOp::PostIncrement;
    const bool prefix = expr->op == UnaryOp::PreIncrement || expr->op == UnaryOp::PreDecrement;
    const TypeInfo* type = expr->type;

    auto emitOne = [&]() {
        if (type && type->kind == TypeKind::Double)
            emitConstant(Value(1.0), type);
        else if (type && type->kind == TypeKind::Float)
            emitConstant(Value(1.0f), type);
        else if (isLong(type))
            emitConstant(Value(std::int64_t(1)), type);
        else
            op(Op::LdcI4, 1);
        op(arithmeticOp(increment ? BinaryOp::Add : BinaryOp::Subtract, type));
        // Narrow integer variables wrap at their own width.
        if (type && isIntegral(type->kind) && bitWidth(type->kind) < 32) {
            switch (type->kind) {
                case TypeKind::SByte: op(Op::ConvI1); break;
                case TypeKind::Byte: op(Op::ConvU1); break;
                case TypeKind::Short: op(Op::ConvI2); break;
                case TypeKind::UShort: op(Op::ConvU2); break;
                case TypeKind::Char: op(Op::ConvChar); break;
                default: break;
            }
        }
    };

    // A local is the common case and needs no scratch slot.
    if (target->kind == ExprKind::Name) {
        const auto* name = static_cast<const NameExpr*>(target);
        if (name->symbol == SymbolKind::Local || name->symbol == SymbolKind::Parameter) {
            const bool boxed = method_->body.localBoxed[name->slot] != 0;
            op(boxed ? Op::LdBoxed : Op::LdLoc, name->slot);
            if (keepValue && !prefix)
                op(Op::Dup);
            emitOne();
            if (keepValue && prefix)
                op(Op::Dup);
            op(boxed ? Op::StBoxed : Op::StLoc, name->slot);
            return;
        }
        if (name->symbol == SymbolKind::StaticField) {
            emitStatic(Op::LdSFld, name->field);
            if (keepValue && !prefix)
                op(Op::Dup);
            emitOne();
            if (keepValue && prefix)
                op(Op::Dup);
            emitStatic(Op::StSFld, name->field);
            return;
        }
    }

    // Everything else keeps the old value in a scratch slot so the receiver is
    // only evaluated once.
    const std::uint32_t temp = scratch(type);
    if (target->kind == ExprKind::ElementAccess) {
        const auto* element = static_cast<const ElementAccessExpr*>(target);
        emitExpr(element->target);
        emitExpr(element->index);
        op(Op::Dup2);
        if (element->getter)
            emitCall(element->getter, 2, false, false);
        else
            op(Op::LdElem);
        if (!prefix) {
            op(Op::Dup);
            op(Op::StLoc, temp);
        }
        emitOne();
        if (prefix) {
            op(Op::Dup);
            op(Op::StLoc, temp);
        }
        if (element->setter)
            emitCall(element->setter, 3, false, false);
        else
            op(Op::StElem);
        if (keepValue)
            op(Op::LdLoc, temp);
        return;
    }

    // A field or property access.
    const auto* member = target->kind == ExprKind::MemberAccess
                             ? static_cast<const MemberAccessExpr*>(target)
                             : nullptr;
    const FieldInfo* field = member ? member->field : static_cast<const NameExpr*>(target)->field;
    const PropertyInfo* prop = member ? member->prop : static_cast<const NameExpr*>(target)->prop;
    if (member && member->symbol == SymbolKind::StaticField && field) {
        // `++Type.field`: no receiver to load.
        emitStatic(Op::LdSFld, field);
        if (!prefix) {
            op(Op::Dup);
            op(Op::StLoc, temp);
        }
        emitOne();
        if (prefix) {
            op(Op::Dup);
            op(Op::StLoc, temp);
        }
        emitStatic(Op::StSFld, field);
        if (keepValue)
            op(Op::LdLoc, temp);
        return;
    }
    if (member && member->target)
        emitExpr(member->target);
    else
        op(Op::LdLoc, 0);
    op(Op::Dup);
    if (field)
        op(Op::LdFld, field->slot);
    else if (prop && prop->getter)
        emitCall(prop->getter, 1, prop->getter->isVirtual(), false);
    if (!prefix) {
        op(Op::Dup);
        op(Op::StLoc, temp);
    }
    emitOne();
    if (prefix) {
        op(Op::Dup);
        op(Op::StLoc, temp);
    }
    if (field)
        op(Op::StFld, field->slot);
    else if (prop && prop->setter)
        emitCall(prop->setter, 2, prop->setter->isVirtual(), false);
    if (keepValue)
        op(Op::LdLoc, temp);
}

void Emitter::emitAssignment(const AssignmentExpr* expr, bool keepValue)
{
    const Expr* target = expr->target;
    const TypeInfo* valueType = expr->type;

    auto emitValue = [&](bool compoundLoaded) {
        if (expr->compound) {
            if (expr->op == BinaryOp::Coalesce) {
                // `a ??= b` only evaluates and stores `b` when `a` is null.
                return;
            }
            emitExpr(expr->value);
            if (expr->operandType && expr->operandType->kind == TypeKind::String) {
                op(Op::Concat, 2);
                adjust(-1);
            } else {
                op(arithmeticOp(expr->op, expr->operandType));
            }
            if (valueType && expr->operandType && valueType != expr->operandType &&
                isIntegral(valueType->kind)) {
                switch (valueType->kind) {
                    case TypeKind::SByte: op(Op::ConvI1); break;
                    case TypeKind::Byte: op(Op::ConvU1); break;
                    case TypeKind::Short: op(Op::ConvI2); break;
                    case TypeKind::UShort: op(Op::ConvU2); break;
                    case TypeKind::Char: op(Op::ConvChar); break;
                    case TypeKind::Int: op(Op::ConvI4); break;
                    case TypeKind::UInt: op(Op::ConvU4); break;
                    default: break;
                }
            }
        } else {
            emitExpr(expr->value);
            if (valueType && valueType->kind == TypeKind::Struct &&
                expr->value->kind != ExprKind::ObjectCreation)
                op(Op::CloneVal);
        }
        (void)compoundLoaded;
    };

    // `a ??= b` is a conditional store, not an operator.
    if (expr->compound && expr->op == BinaryOp::Coalesce) {
        emitLoad(target);
        op(Op::Dup);
        op(Op::IsNull);
        const std::size_t assign = jump(Op::BrTrue);
        const std::size_t done = jump(Op::Br);
        patchHere(assign);
        op(Op::Pop);
        AssignmentExpr plain = *expr;
        plain.compound = false;
        emitAssignment(&plain, true);
        patchHere(done);
        if (!keepValue)
            op(Op::Pop);
        return;
    }

    switch (target->kind) {
        case ExprKind::Name: {
            const auto* name = static_cast<const NameExpr*>(target);
            switch (name->symbol) {
                case SymbolKind::Local:
                case SymbolKind::Parameter: {
                    const bool boxed = method_->body.localBoxed[name->slot] != 0;
                    if (expr->compound)
                        op(boxed ? Op::LdBoxed : Op::LdLoc, name->slot);
                    emitValue(expr->compound);
                    if (keepValue)
                        op(Op::Dup);
                    op(boxed ? Op::StBoxed : Op::StLoc, name->slot);
                    return;
                }
                case SymbolKind::StaticField:
                    if (expr->compound)
                        emitStatic(Op::LdSFld, name->field);
                    emitValue(expr->compound);
                    if (keepValue)
                        op(Op::Dup);
                    emitStatic(Op::StSFld, name->field);
                    return;
                case SymbolKind::Field:
                    op(Op::LdLoc, 0);
                    if (expr->compound) {
                        op(Op::Dup);
                        op(Op::LdFld, name->field->slot);
                    }
                    emitValue(expr->compound);
                    op(Op::StFld, name->field->slot);
                    if (keepValue) {
                        op(Op::LdLoc, 0);
                        op(Op::LdFld, name->field->slot);
                    }
                    return;
                case SymbolKind::Property:
                    op(Op::LdLoc, 0);
                    if (expr->compound) {
                        op(Op::Dup);
                        emitCall(name->prop->getter, 1, false, false);
                    }
                    emitValue(expr->compound);
                    emitCall(name->prop->setter, 2, false, false);
                    if (keepValue) {
                        op(Op::LdLoc, 0);
                        emitCall(name->prop->getter, 1, false, false);
                    }
                    return;
                case SymbolKind::StaticProperty:
                    if (expr->compound)
                        emitCall(name->prop->getter, 0, false, false);
                    emitValue(expr->compound);
                    emitCall(name->prop->setter, 1, false, false);
                    if (keepValue)
                        emitCall(name->prop->getter, 0, false, false);
                    return;
                default: return;
            }
        }
        case ExprKind::MemberAccess: {
            const auto* member = static_cast<const MemberAccessExpr*>(target);
            const bool isStatic = member->target == nullptr;
            if (!isStatic)
                emitExpr(member->target);
            if (member->symbol == SymbolKind::Field ||
                member->symbol == SymbolKind::StaticField) {
                if (member->field->isStatic) {
                    if (expr->compound)
                        emitStatic(Op::LdSFld, member->field);
                    emitValue(expr->compound);
                    if (keepValue)
                        op(Op::Dup);
                    emitStatic(Op::StSFld, member->field);
                    return;
                }
                if (expr->compound) {
                    op(Op::Dup);
                    op(Op::LdFld, member->field->slot);
                }
                emitValue(expr->compound);
                std::uint32_t keep = 0;
                if (keepValue) {
                    keep = scratch(expr->type);
                    op(Op::Dup);
                    op(Op::StLoc, keep);
                }
                op(Op::StFld, member->field->slot);
                if (keepValue)
                    op(Op::LdLoc, keep);
                return;
            }
            if (member->prop) {
                if (member->prop->isStatic) {
                    if (expr->compound)
                        emitCall(member->prop->getter, 0, false, false);
                    emitValue(expr->compound);
                    emitCall(member->prop->setter, 1, false, false);
                    return;
                }
                if (expr->compound) {
                    op(Op::Dup);
                    emitCall(member->prop->getter, 1, member->prop->getter->isVirtual(), false);
                }
                emitValue(expr->compound);
                std::uint32_t keep = 0;
                if (keepValue) {
                    keep = scratch(expr->type);
                    op(Op::Dup);
                    op(Op::StLoc, keep);
                }
                emitCall(member->prop->setter, 2,
                         member->prop->setter && member->prop->setter->isVirtual(), false);
                if (keepValue)
                    op(Op::LdLoc, keep);
                return;
            }
            return;
        }
        case ExprKind::ElementAccess: {
            const auto* element = static_cast<const ElementAccessExpr*>(target);
            emitExpr(element->target);
            emitExpr(element->index);
            if (expr->compound) {
                op(Op::Dup2);
                if (element->getter)
                    emitCall(element->getter, 2, false, false);
                else
                    op(Op::LdElem);
            }
            emitValue(expr->compound);
            std::uint32_t keep = 0;
            if (keepValue) {
                keep = scratch(expr->type);
                op(Op::Dup);
                op(Op::StLoc, keep);
            }
            if (element->setter)
                emitCall(element->setter, 3, false, false);
            else
                op(Op::StElem);
            if (keepValue)
                op(Op::LdLoc, keep);
            return;
        }
        default: return;
    }
}

void Emitter::emitStatic(Op code, const FieldInfo* field)
{
    op(code, typeRef(field->owner), field->slot);
}

void Emitter::emitCall(const MethodInfo* method, std::uint32_t argc, bool isVirtual,
                       bool isInterface)
{
    if (!method)
        return;  // the checker already reported whatever went missing
    const Op code = isInterface ? Op::CallIface : (isVirtual ? Op::CallVirt : Op::Call);
    emitOp(this->code(), code);
    emitU32(this->code(), methodRef(method));
    emitU32(this->code(), argc);
    adjust(-int(argc) + (method->returnType && method->returnType->kind != TypeKind::Void ? 1 : 0));
}

void Emitter::emitInvocation(const InvocationExpr* expr, bool keepValue)
{
    if (expr->delegateCall) {
        emitExpr(expr->callee);
        for (const Argument& arg : expr->args)
            emitExpr(arg.value);
        emitOp(code(), Op::CallDlg);
        emitU32(code(), std::uint32_t(expr->args.size()));
        const bool returnsValue = expr->type && expr->type->kind != TypeKind::Void;
        adjust(-int(expr->args.size()) - 1 + (returnsValue ? 1 : 0));
        if (!keepValue && returnsValue)
            op(Op::Pop);
        return;
    }

    const MethodInfo* method = expr->method;
    if (!method) {
        if (keepValue)
            op(Op::LdNull);
        return;
    }

    // `u.ToString()` on a uint or ulong: the call would go to Object.ToString, which cannot tell the value is unsigned.
    if (!expr->nullConditional && expr->receiver && expr->args.empty() && !method->isStatic() &&
        method->name == u"ToString" && toStringOp(expr->receiver) == Op::ToStrU) {
        emitExpr(expr->receiver);
        op(Op::ToStrU);
        if (!keepValue)
            op(Op::Pop);
        return;
    }

    std::size_t guard = 0;
    const bool guarded = expr->nullConditional && expr->receiver;
    std::uint32_t argc = 0;
    if (!method->isStatic()) {
        emitExpr(expr->receiver);
        if (guarded) {
            op(Op::Dup);
            op(Op::IsNull);
            guard = jump(Op::BrTrue);
        }
        ++argc;
    }
    for (std::size_t i = 0; i < expr->args.size(); ++i) {
        const Argument& arg = expr->args[i];
        const ParamInfo& param = method->params[i];
        if (param.mode == ParamMode::Ref || param.mode == ParamMode::Out) {
            // `ref`/`out` pass the box the variable lives in.
            if (arg.value->kind == ExprKind::Name) {
                const auto* name = static_cast<const NameExpr*>(arg.value);
                op(Op::LdBoxRef, name->slot);
            } else {
                op(Op::LdNull);
            }
        } else {
            emitExpr(arg.value);
            if (param.type && param.type->kind == TypeKind::Struct &&
                arg.value->kind != ExprKind::ObjectCreation)
                op(Op::CloneVal);
        }
        ++argc;
    }
    emitCall(method, argc, expr->virtualCall, expr->interfaceCall);

    const bool returnsValue = method->returnType && method->returnType->kind != TypeKind::Void;
    if (guarded) {
        if (!returnsValue) {
            const std::size_t done = jump(Op::Br);
            patchHere(guard);
            op(Op::Pop);
            patchHere(done);
        } else {
            const std::size_t done = jump(Op::Br);
            patchHere(guard);
            op(Op::Pop);
            op(Op::LdNull);
            patchHere(done);
        }
    }
    if (!keepValue && returnsValue)
        op(Op::Pop);
    if (keepValue && !returnsValue)
        op(Op::LdNull);
}

void Emitter::emitObjectCreation(const ObjectCreationExpr* expr)
{
    const TypeInfo* type = expr->type;
    if (expr->ctor) {
        for (const Argument& arg : expr->args)
            emitExpr(arg.value);
        emitOp(code(), Op::NewObj);
        emitU32(code(), methodRef(expr->ctor));
        emitU32(code(), std::uint32_t(expr->args.size()));
        adjust(-int(expr->args.size()) + 1);
    } else {
        op(Op::InitObj, typeRef(type));
    }

    for (const InitializerEntry& entry : expr->initializers) {
        op(Op::Dup);
        if (entry.field) {
            emitExpr(entry.value);
            op(Op::StFld, entry.field->slot);
        } else if (entry.prop && entry.prop->setter) {
            emitExpr(entry.value);
            emitCall(entry.prop->setter, 2, false, false);
        } else if (entry.key) {
            std::vector<MethodInfo*> setters;
            type->collectMethods(ascii("set_Item"), setters);
            emitExpr(entry.key);
            emitExpr(entry.value);
            if (!setters.empty())
                emitCall(setters.front(), 3, false, false);
        }
    }
    if (!expr->elements.empty()) {
        std::vector<MethodInfo*> adders;
        type->collectMethods(ascii("Add"), adders);
        for (const Expr* item : expr->elements) {
            op(Op::Dup);
            emitExpr(item);
            if (!adders.empty())
                emitCall(adders.front(), 2, false, false);
        }
    }
}

void Emitter::emitArrayCreation(const ArrayCreationExpr* expr)
{
    const TypeInfo* element = expr->type ? expr->type->element : nullptr;
    if (expr->size) {
        emitExpr(expr->size);
        op(Op::NewArr, typeRef(element));
        return;
    }
    op(Op::LdcI4, std::uint32_t(expr->elements.size()));
    op(Op::NewArr, typeRef(element));
    for (std::size_t i = 0; i < expr->elements.size(); ++i) {
        op(Op::Dup);
        op(Op::LdcI4, std::uint32_t(i));
        emitExpr(expr->elements[i]);
        op(Op::StElem);
    }
}

}  // namespace typed
}  // namespace internal
}  // namespace script
