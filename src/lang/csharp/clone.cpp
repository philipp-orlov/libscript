// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Deep copies of declaration trees, for generic instantiation.
//
// The checker writes its conclusions into the nodes it checks -- the type of
// every expression, the slot of every local, the method every call resolved
// to. That is what makes the emitter simple, and it is also why `Box<int>` and
// `Box<string>` cannot share a tree: the second instantiation would overwrite
// the first one's answers. So each instantiation gets its own copy, and the
// type parameters are bound per copy.
//
// Everything here is structural. Nothing resolves a name, and no annotation
// field is carried over -- a clone is exactly what the parser would have
// produced.
#include "script/internal/typed/ast.hpp"

namespace script {
namespace internal {
namespace typed {

namespace {

class Cloner
{
public:
    explicit Cloner(Arena& arena) : arena_(arena) {}

    TypeRef* type(const TypeRef* ref)
    {
        if (!ref)
            return nullptr;
        TypeRef* copy = arena_.make<TypeRef>();
        copy->predefined = ref->predefined;
        copy->path = ref->path;
        copy->arrayRank = ref->arrayRank;
        copy->nullable = ref->nullable;
        copy->isVar = ref->isVar;
        copy->isTuple = ref->isTuple;
        copy->tupleNames = ref->tupleNames;
        copy->line = ref->line;
        copy->column = ref->column;
        for (const TypeRef* arg : ref->typeArgs)
            copy->typeArgs.push_back(type(arg));
        return copy;
    }

    Expr* expr(const Expr* node)
    {
        if (!node)
            return nullptr;
        switch (node->kind) {
            case ExprKind::Literal: {
                const auto* from = static_cast<const LiteralExpr*>(node);
                auto* to = start<LiteralExpr>(from);
                to->literal = from->literal;
                to->intValue = from->intValue;
                to->realValue = from->realValue;
                to->text = from->text;
                return to;
            }
            case ExprKind::Interpolated: {
                const auto* from = static_cast<const InterpolatedExpr*>(node);
                auto* to = start<InterpolatedExpr>(from);
                to->parts = from->parts;
                to->formats = from->formats;
                for (const Expr* hole : from->holes)
                    to->holes.push_back(expr(hole));
                return to;
            }
            case ExprKind::Name: {
                const auto* from = static_cast<const NameExpr*>(node);
                auto* to = start<NameExpr>(from);
                to->name = from->name;
                to->typeArgs = cloneList(from->typeArgs);
                return to;
            }
            case ExprKind::MemberAccess: {
                const auto* from = static_cast<const MemberAccessExpr*>(node);
                auto* to = start<MemberAccessExpr>(from);
                to->target = expr(from->target);
                to->name = from->name;
                to->nullConditional = from->nullConditional;
                to->typeArgs = cloneList(from->typeArgs);
                return to;
            }
            case ExprKind::Invocation: {
                const auto* from = static_cast<const InvocationExpr*>(node);
                auto* to = start<InvocationExpr>(from);
                to->callee = expr(from->callee);
                to->args = arguments(from->args);
                to->nullConditional = from->nullConditional;
                return to;
            }
            case ExprKind::ElementAccess: {
                const auto* from = static_cast<const ElementAccessExpr*>(node);
                auto* to = start<ElementAccessExpr>(from);
                to->target = expr(from->target);
                to->index = expr(from->index);
                to->nullConditional = from->nullConditional;
                return to;
            }
            case ExprKind::ObjectCreation: {
                const auto* from = static_cast<const ObjectCreationExpr*>(node);
                auto* to = start<ObjectCreationExpr>(from);
                to->typeRef = type(from->typeRef);
                to->args = arguments(from->args);
                for (const InitializerEntry& entry : from->initializers) {
                    InitializerEntry copy;
                    copy.name = entry.name;
                    copy.key = expr(entry.key);
                    copy.value = expr(entry.value);
                    to->initializers.push_back(std::move(copy));
                }
                for (const Expr* element : from->elements)
                    to->elements.push_back(expr(element));
                return to;
            }
            case ExprKind::ArrayCreation: {
                const auto* from = static_cast<const ArrayCreationExpr*>(node);
                auto* to = start<ArrayCreationExpr>(from);
                to->elementTypeRef = type(from->elementTypeRef);
                to->size = expr(from->size);
                to->hasInitializer = from->hasInitializer;
                for (const Expr* element : from->elements)
                    to->elements.push_back(expr(element));
                return to;
            }
            case ExprKind::Unary: {
                const auto* from = static_cast<const UnaryExpr*>(node);
                auto* to = start<UnaryExpr>(from);
                to->op = from->op;
                to->operand = expr(from->operand);
                return to;
            }
            case ExprKind::Binary: {
                const auto* from = static_cast<const BinaryExpr*>(node);
                auto* to = start<BinaryExpr>(from);
                to->op = from->op;
                to->left = expr(from->left);
                to->right = expr(from->right);
                return to;
            }
            case ExprKind::Assignment: {
                const auto* from = static_cast<const AssignmentExpr*>(node);
                auto* to = start<AssignmentExpr>(from);
                to->op = from->op;
                to->compound = from->compound;
                to->target = expr(from->target);
                to->value = expr(from->value);
                return to;
            }
            case ExprKind::Conditional: {
                const auto* from = static_cast<const ConditionalExpr*>(node);
                auto* to = start<ConditionalExpr>(from);
                to->condition = expr(from->condition);
                to->whenTrue = expr(from->whenTrue);
                to->whenFalse = expr(from->whenFalse);
                return to;
            }
            case ExprKind::Cast: {
                const auto* from = static_cast<const CastExpr*>(node);
                auto* to = start<CastExpr>(from);
                to->typeRef = type(from->typeRef);
                to->operand = expr(from->operand);
                return to;
            }
            case ExprKind::TypeTest: {
                const auto* from = static_cast<const TypeTestExpr*>(node);
                auto* to = start<TypeTestExpr>(from);
                to->operand = expr(from->operand);
                to->typeRef = type(from->typeRef);
                to->isAs = from->isAs;
                to->bindingName = from->bindingName;
                to->pattern = pattern(from->pattern);
                return to;
            }
            case ExprKind::Lambda: {
                const auto* from = static_cast<const LambdaExpr*>(node);
                auto* to = start<LambdaExpr>(from);
                for (const LambdaParam& p : from->params) {
                    LambdaParam copy;
                    copy.name = p.name;
                    copy.typeRef = type(p.typeRef);
                    copy.line = p.line;
                    copy.defaultValue = expr(p.defaultValue);
                    to->params.push_back(std::move(copy));
                }
                to->bodyExpr = expr(from->bodyExpr);
                to->bodyBlock = stmt(from->bodyBlock);
                to->isStatic = from->isStatic;
                return to;
            }
            case ExprKind::This: return start<ThisExpr>(node);
            case ExprKind::Base: return start<BaseExpr>(node);
            case ExprKind::Default: {
                const auto* from = static_cast<const DefaultExpr*>(node);
                auto* to = start<DefaultExpr>(from);
                to->typeRef = type(from->typeRef);
                return to;
            }
            case ExprKind::TypeOf: {
                const auto* from = static_cast<const TypeOfExpr*>(node);
                auto* to = start<TypeOfExpr>(from);
                to->typeRef = type(from->typeRef);
                return to;
            }
            case ExprKind::NameOf: {
                const auto* from = static_cast<const NameOfExpr*>(node);
                auto* to = start<NameOfExpr>(from);
                to->name = from->name;
                return to;
            }
            case ExprKind::Convert: {
                // The checker inserts these; a tree being cloned has none yet.
                const auto* from = static_cast<const ConvertExpr*>(node);
                auto* to = start<ConvertExpr>(from);
                to->operand = expr(from->operand);
                to->conv = from->conv;
                return to;
            }
            case ExprKind::Switch: {
                const auto* from = static_cast<const SwitchExpr*>(node);
                auto* to = start<SwitchExpr>(from);
                to->subject = expr(from->subject);
                for (const SwitchArm& arm : from->arms) {
                    SwitchArm copy;
                    copy.pattern = pattern(arm.pattern);
                    copy.guard = expr(arm.guard);
                    copy.value = expr(arm.value);
                    copy.line = arm.line;
                    copy.column = arm.column;
                    to->arms.push_back(std::move(copy));
                }
                return to;
            }
            case ExprKind::Tuple: {
                const auto* from = static_cast<const TupleExpr*>(node);
                auto* to = start<TupleExpr>(from);
                for (const TupleElement& element : from->elements) {
                    TupleElement copy;
                    copy.name = element.name;
                    copy.value = expr(element.value);
                    to->elements.push_back(std::move(copy));
                }
                return to;
            }
            case ExprKind::Throw: {
                const auto* from = static_cast<const ThrowExpr*>(node);
                auto* to = start<ThrowExpr>(from);
                to->value = expr(from->value);
                return to;
            }
            case ExprKind::Discard: return start<DiscardExpr>(node);
            case ExprKind::Range: {
                const auto* from = static_cast<const RangeExpr*>(node);
                auto* to = start<RangeExpr>(from);
                to->start = expr(from->start);
                to->end = expr(from->end);
                return to;
            }
            case ExprKind::Index: {
                const auto* from = static_cast<const IndexExpr*>(node);
                auto* to = start<IndexExpr>(from);
                to->operand = expr(from->operand);
                return to;
            }
            case ExprKind::Collection: {
                const auto* from = static_cast<const CollectionExpr*>(node);
                auto* to = start<CollectionExpr>(from);
                for (const CollectionElement& element : from->elements) {
                    CollectionElement copy;
                    copy.value = expr(element.value);
                    copy.spread = element.spread;
                    to->elements.push_back(std::move(copy));
                }
                return to;
            }
            case ExprKind::With: {
                const auto* from = static_cast<const WithExpr*>(node);
                auto* to = start<WithExpr>(from);
                to->target = expr(from->target);
                for (const InitializerEntry& entry : from->initializers) {
                    InitializerEntry copy;
                    copy.name = entry.name;
                    copy.key = expr(entry.key);
                    copy.value = expr(entry.value);
                    to->initializers.push_back(std::move(copy));
                }
                return to;
            }
            case ExprKind::Sequence: {
                const auto* from = static_cast<const SequenceExpr*>(node);
                auto* to = start<SequenceExpr>(from);
                for (const Expr* item : from->before)
                    to->before.push_back(expr(item));
                to->value = expr(from->value);
                return to;
            }
        }
        return nullptr;
    }

    Pattern* pattern(const Pattern* node)
    {
        if (!node)
            return nullptr;
        Pattern* copy = arena_.make<Pattern>();
        copy->kind = node->kind;
        copy->line = node->line;
        copy->column = node->column;
        copy->typeRef = type(node->typeRef);
        copy->bindingName = node->bindingName;
        copy->value = expr(node->value);
        copy->op = node->op;
        copy->negated = node->negated;
        for (const Pattern* child : node->children)
            copy->children.push_back(pattern(child));
        for (const PropertySubpattern& sub : node->properties) {
            PropertySubpattern entry;
            entry.name = sub.name;
            entry.pattern = pattern(sub.pattern);
            copy->properties.push_back(std::move(entry));
        }
        return copy;
    }

    Stmt* stmt(const Stmt* node)
    {
        if (!node)
            return nullptr;
        switch (node->kind) {
            case StmtKind::Block: {
                const auto* from = static_cast<const BlockStmt*>(node);
                auto* to = start<BlockStmt>(from);
                for (const Stmt* child : from->statements)
                    to->statements.push_back(stmt(child));
                return to;
            }
            case StmtKind::LocalDeclaration: {
                const auto* from = static_cast<const LocalDeclStmt*>(node);
                auto* to = start<LocalDeclStmt>(from);
                to->typeRef = type(from->typeRef);
                to->isConst = from->isConst;
                to->isUsing = from->isUsing;
                for (const VarDeclarator& var : from->vars) {
                    VarDeclarator copy;
                    copy.name = var.name;
                    copy.init = expr(var.init);
                    copy.line = var.line;
                    to->vars.push_back(std::move(copy));
                }
                to->deconstruction = deconstruction(from->deconstruction);
                to->deconstructionValue = expr(from->deconstructionValue);
                return to;
            }
            case StmtKind::Expression: {
                const auto* from = static_cast<const ExprStmt*>(node);
                auto* to = start<ExprStmt>(from);
                to->expr = expr(from->expr);
                return to;
            }
            case StmtKind::If: {
                const auto* from = static_cast<const IfStmt*>(node);
                auto* to = start<IfStmt>(from);
                to->condition = expr(from->condition);
                to->thenBranch = stmt(from->thenBranch);
                to->elseBranch = stmt(from->elseBranch);
                return to;
            }
            case StmtKind::While: {
                const auto* from = static_cast<const WhileStmt*>(node);
                auto* to = start<WhileStmt>(from);
                to->condition = expr(from->condition);
                to->body = stmt(from->body);
                return to;
            }
            case StmtKind::DoWhile: {
                const auto* from = static_cast<const DoWhileStmt*>(node);
                auto* to = start<DoWhileStmt>(from);
                to->condition = expr(from->condition);
                to->body = stmt(from->body);
                return to;
            }
            case StmtKind::For: {
                const auto* from = static_cast<const ForStmt*>(node);
                auto* to = start<ForStmt>(from);
                for (const Stmt* init : from->init)
                    to->init.push_back(stmt(init));
                to->condition = expr(from->condition);
                for (const Expr* update : from->update)
                    to->update.push_back(expr(update));
                to->body = stmt(from->body);
                return to;
            }
            case StmtKind::ForEach: {
                const auto* from = static_cast<const ForEachStmt*>(node);
                auto* to = start<ForEachStmt>(from);
                to->typeRef = type(from->typeRef);
                to->name = from->name;
                to->sequence = expr(from->sequence);
                to->body = stmt(from->body);
                to->deconstruction = deconstruction(from->deconstruction);
                return to;
            }
            case StmtKind::Switch: {
                const auto* from = static_cast<const SwitchStmt*>(node);
                auto* to = start<SwitchStmt>(from);
                to->subject = expr(from->subject);
                for (const SwitchSection& section : from->sections) {
                    SwitchSection copy;
                    copy.isDefault = section.isDefault;
                    copy.line = section.line;
                    for (const Expr* label : section.labels)
                        copy.labels.push_back(expr(label));
                    for (const SwitchCaseLabel& label : section.patterns) {
                        SwitchCaseLabel entry;
                        entry.pattern = pattern(label.pattern);
                        entry.guard = expr(label.guard);
                        copy.patterns.push_back(std::move(entry));
                    }
                    for (const Stmt* child : section.statements)
                        copy.statements.push_back(stmt(child));
                    to->sections.push_back(std::move(copy));
                }
                return to;
            }
            case StmtKind::Break: return start<BreakStmt>(node);
            case StmtKind::Continue: return start<ContinueStmt>(node);
            case StmtKind::Return: {
                const auto* from = static_cast<const ReturnStmt*>(node);
                auto* to = start<ReturnStmt>(from);
                to->value = expr(from->value);
                return to;
            }
            case StmtKind::Throw: {
                const auto* from = static_cast<const ThrowStmt*>(node);
                auto* to = start<ThrowStmt>(from);
                to->value = expr(from->value);
                return to;
            }
            case StmtKind::Try: {
                const auto* from = static_cast<const TryStmt*>(node);
                auto* to = start<TryStmt>(from);
                to->body = stmt(from->body);
                to->finallyBlock = stmt(from->finallyBlock);
                for (const CatchClause& clause : from->catches) {
                    CatchClause copy;
                    copy.typeRef = type(clause.typeRef);
                    copy.name = clause.name;
                    copy.body = stmt(clause.body);
                    copy.filter = expr(clause.filter);
                    copy.line = clause.line;
                    to->catches.push_back(std::move(copy));
                }
                return to;
            }
            case StmtKind::Yield: {
                const auto* from = static_cast<const YieldStmt*>(node);
                auto* to = start<YieldStmt>(from);
                to->value = expr(from->value);
                to->isBreak = from->isBreak;
                return to;
            }
            case StmtKind::Using: {
                const auto* from = static_cast<const UsingStmt*>(node);
                auto* to = start<UsingStmt>(from);
                to->declaration = stmt(from->declaration);
                to->resource = expr(from->resource);
                to->body = stmt(from->body);
                return to;
            }
            case StmtKind::LocalFunction: {
                const auto* from = static_cast<const LocalFunctionStmt*>(node);
                auto* to = start<LocalFunctionStmt>(from);
                to->method = method(from->method, nullptr);
                return to;
            }
            case StmtKind::Empty: return start<EmptyStmt>(node);
        }
        return nullptr;
    }

    MethodDecl* method(const MethodDecl* from, TypeDecl* owner)
    {
        if (!from)
            return nullptr;
        MethodDecl* to = arena_.make<MethodDecl>();
        to->name = from->name;
        to->modifiers = from->modifiers;
        to->returnTypeRef = type(from->returnTypeRef);
        to->params = params(from->params);
        to->body = stmt(from->body);
        to->expressionBody = expr(from->expressionBody);
        to->isConstructor = from->isConstructor;
        to->chainsToBase = from->chainsToBase;
        to->chainsToThis = from->chainsToThis;
        to->chainArgs = arguments(from->chainArgs);
        to->line = from->line;
        to->owner = owner;
        to->typeParams = from->typeParams;
        to->constraints = constraints(from->constraints);
        to->genericFrom = const_cast<MethodDecl*>(from);
        return to;
    }

    TypeDecl* typeDecl(const TypeDecl* from)
    {
        TypeDecl* to = arena_.make<TypeDecl>();
        to->kind = from->kind;
        to->name = from->name;
        to->fullName = from->fullName;
        to->modifiers = from->modifiers;
        to->line = from->line;
        to->isRecord = from->isRecord;
        to->isPrimaryConstructed = from->isPrimaryConstructed;
        to->typeParams = from->typeParams;
        to->constraints = constraints(from->constraints);
        to->genericFrom = const_cast<TypeDecl*>(from);
        for (const TypeRef* base : from->baseTypes)
            to->baseTypes.push_back(type(base));
        for (const FieldDecl& field : from->fields) {
            FieldDecl copy;
            copy.name = field.name;
            copy.modifiers = field.modifiers;
            copy.typeRef = type(field.typeRef);
            copy.init = expr(field.init);
            copy.line = field.line;
            to->fields.push_back(std::move(copy));
        }
        for (const MethodDecl* m : from->methods)
            to->methods.push_back(method(m, to));
        for (const PropertyDecl& prop : from->props) {
            PropertyDecl copy;
            copy.name = prop.name;
            copy.modifiers = prop.modifiers;
            copy.typeRef = type(prop.typeRef);
            copy.getterBody = stmt(prop.getterBody);
            copy.getterExpr = expr(prop.getterExpr);
            copy.setterBody = stmt(prop.setterBody);
            copy.hasGetter = prop.hasGetter;
            copy.hasSetter = prop.hasSetter;
            copy.autoProperty = prop.autoProperty;
            copy.isInitOnly = prop.isInitOnly;
            copy.isRequired = prop.isRequired;
            copy.init = expr(prop.init);
            copy.line = prop.line;
            to->props.push_back(std::move(copy));
        }
        for (const EnumMemberDecl& member : from->enumMembers) {
            EnumMemberDecl copy;
            copy.name = member.name;
            copy.value = expr(member.value);
            copy.line = member.line;
            to->enumMembers.push_back(std::move(copy));
        }
        to->returnTypeRef = type(from->returnTypeRef);
        to->params = params(from->params);
        return to;
    }

private:
    template <class T, class From>
    T* start(const From* from)
    {
        T* node = arena_.make<T>();
        node->line = from->line;
        if constexpr (std::is_base_of_v<Expr, T>)
            node->column = static_cast<const Expr*>(from)->column;
        return node;
    }

    std::vector<TypeRef*> cloneList(const std::vector<TypeRef*>& refs)
    {
        std::vector<TypeRef*> out;
        out.reserve(refs.size());
        for (const TypeRef* ref : refs)
            out.push_back(type(ref));
        return out;
    }

    std::vector<Argument> arguments(const std::vector<Argument>& args)
    {
        std::vector<Argument> out;
        out.reserve(args.size());
        for (const Argument& arg : args) {
            Argument copy;
            copy.value = expr(arg.value);
            copy.name = arg.name;
            copy.mode = arg.mode;
            copy.outTypeRef = type(arg.outTypeRef);
            copy.outName = arg.outName;
            copy.outLine = arg.outLine;
            out.push_back(std::move(copy));
        }
        return out;
    }

    std::vector<ParamDecl> params(const std::vector<ParamDecl>& in)
    {
        std::vector<ParamDecl> out;
        out.reserve(in.size());
        for (const ParamDecl& param : in) {
            ParamDecl copy;
            copy.name = param.name;
            copy.typeRef = type(param.typeRef);
            copy.mode = param.mode;
            copy.defaultValue = expr(param.defaultValue);
            copy.line = param.line;
            out.push_back(std::move(copy));
        }
        return out;
    }

    std::vector<TypeConstraint> constraints(const std::vector<TypeConstraint>& in)
    {
        std::vector<TypeConstraint> out;
        out.reserve(in.size());
        for (const TypeConstraint& constraint : in) {
            TypeConstraint copy;
            copy.name = constraint.name;
            copy.hasClass = constraint.hasClass;
            copy.hasStruct = constraint.hasStruct;
            copy.hasNew = constraint.hasNew;
            copy.line = constraint.line;
            copy.column = constraint.column;
            copy.types = cloneList(constraint.types);
            out.push_back(std::move(copy));
        }
        return out;
    }

    Deconstruction* deconstruction(const Deconstruction* from)
    {
        if (!from)
            return nullptr;
        Deconstruction* to = arena_.make<Deconstruction>();
        to->typeRef = type(from->typeRef);
        for (const DeconstructTarget& target : from->targets) {
            DeconstructTarget copy;
            copy.name = target.name;
            copy.typeRef = type(target.typeRef);
            copy.discard = target.discard;
            copy.nested = deconstruction(target.nested);
            copy.typeRef = type(target.typeRef);
            copy.line = target.line;
            to->targets.push_back(std::move(copy));
        }
        return to;
    }

    Arena& arena_;
};

}  // namespace

TypeDecl* cloneTypeDecl(Arena& arena, const TypeDecl* decl)
{
    Cloner cloner(arena);
    return cloner.typeDecl(decl);
}

MethodDecl* cloneMethodDecl(Arena& arena, const MethodDecl* decl, TypeDecl* owner)
{
    Cloner cloner(arena);
    return cloner.method(decl, owner);
}

}  // namespace typed
}  // namespace internal
}  // namespace script
