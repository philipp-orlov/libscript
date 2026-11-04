// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Type erasure and the lowering of the three constructs that survive it.

#include <utility>

#include "script/internal/dyn/erase.hpp"

namespace script {
namespace internal {
namespace dyn {

namespace {

NodePtr identifier(const String& name, std::uint32_t line)
{
    NodePtr n = makeNode(NK::Identifier, line);
    n->text = name;
    return n;
}

NodePtr stringLiteral(const String& value, std::uint32_t line)
{
    NodePtr n = makeNode(NK::StringLit, line);
    n->text = value;
    return n;
}

/// `object[key]`, always computed so that a member name the grammar would not
/// accept as an identifier still works.
NodePtr indexOf(NodePtr object, NodePtr key, std::uint32_t line)
{
    NodePtr n = makeNode(NK::Member, line);
    n->computed = true;
    n->a = std::move(object);
    n->b = std::move(key);
    return n;
}

NodePtr assignment(NodePtr target, NodePtr value, std::uint32_t line)
{
    NodePtr n = makeNode(NK::Assign, line);
    n->op = Tok::Assign;
    n->a = std::move(target);
    n->b = std::move(value);
    NodePtr statement = makeNode(NK::ExprStatement, line);
    statement->a = std::move(n);
    return statement;
}

NodePtr declare(const String& name, NodePtr value, std::uint32_t line)
{
    NodePtr declarator = makeNode(NK::Declarator, line);
    declarator->a = identifier(name, line);
    declarator->b = std::move(value);
    NodePtr n = makeNode(NK::VarDecl, line);
    n->varKind = VarKind::Var;
    n->list.push_back(std::move(declarator));
    return n;
}

/// True when an enum member's value is a number the compiler can see, which is
/// the condition for adding the reverse `Enum[0] === "Red"` mapping.
bool isNumericMember(const Node& member, double& runningValue)
{
    if (!member.a) {
        runningValue += 1;
        return true;
    }
    if (member.a->kind == NK::NumberLit) {
        runningValue = member.a->number + 1;
        return true;
    }
    if (member.a->kind == NK::Unary && member.a->op == Tok::Minus && member.a->a &&
        member.a->a->kind == NK::NumberLit) {
        runningValue = -member.a->a->number + 1;
        return true;
    }
    return false;
}

class Eraser
{
public:
    void statements(std::vector<NodePtr>& list);
    void expression(NodePtr& slot);
    void statement(NodePtr& slot);

private:
    void lowerEnum(const Node& n, std::vector<NodePtr>& out);
    void lowerNamespace(Node& n, std::vector<NodePtr>& out);
    void lowerClass(Node& cls);
    void stripTypes(Node& n);
};

void Eraser::stripTypes(Node& n)
{
    n.type.reset();
    n.typeParams.clear();
}

void Eraser::statements(std::vector<NodePtr>& list)
{
    std::vector<NodePtr> out;
    out.reserve(list.size());
    for (NodePtr& s : list) {
        if (!s)
            continue;
        switch (s->kind) {
            case NK::TypeAlias:
            case NK::InterfaceDecl:
            case NK::TypeMember: continue;
            case NK::EnumDecl:
                if (!s->declared)
                    lowerEnum(*s, out);
                continue;
            case NK::ModuleDecl:
                if (!s->declared)
                    lowerNamespace(*s, out);
                continue;
            default: break;
        }
        // `declare function f(): void;` and `declare class C {}` describe
        // something the host provides; they emit nothing.
        if (s->declared &&
            (s->kind == NK::FunctionDecl || s->kind == NK::ClassDecl || s->kind == NK::VarDecl))
            continue;
        statement(s);
        if (s)
            out.push_back(std::move(s));
    }
    list = std::move(out);
}

void Eraser::lowerEnum(const Node& n, std::vector<NodePtr>& out)
{
    const std::uint32_t line = n.line;
    NodePtr empty = makeNode(NK::ObjectLit, line);
    out.push_back(declare(n.text, std::move(empty), line));

    double running = 0;
    for (const NodePtr& member : n.list) {
        const double before = running;
        const bool numeric = isNumericMember(*member, running);
        NodePtr value;
        if (member->a) {
            value = std::move(const_cast<Node&>(*member).a);
            expression(value);
        } else {
            value = makeNode(NK::NumberLit, line);
            value->number = before;
        }
        out.push_back(
            assignment(indexOf(identifier(n.text, line), stringLiteral(member->text, line), line),
                       std::move(value), line));
        if (!numeric)
            continue;
        // The reverse mapping reads the value back rather than recomputing it,
        // so an initialiser with a side effect still runs exactly once.
        NodePtr read = indexOf(identifier(n.text, line), stringLiteral(member->text, line), line);
        out.push_back(assignment(indexOf(identifier(n.text, line), std::move(read), line),
                                 stringLiteral(member->text, line), line));
    }
}

void Eraser::lowerNamespace(Node& n, std::vector<NodePtr>& out)
{
    const std::uint32_t line = n.line;

    // A namespace object carries the names its body exports, and only those.
    NodePtr exported = makeNode(NK::ObjectLit, line);
    auto exportName = [&](const String& name) {
        if (name.empty())
            return;
        NodePtr property = makeNode(NK::Property, line);
        property->text = name;
        property->b = identifier(name, line);
        exported->list.push_back(std::move(property));
    };
    for (const NodePtr& s : n.list) {
        if (!s->exported || s->declared)
            continue;
        switch (s->kind) {
            case NK::FunctionDecl:
            case NK::ClassDecl:
            case NK::EnumDecl:
            case NK::ModuleDecl: exportName(s->text); break;
            case NK::VarDecl:
                for (const NodePtr& d : s->list) {
                    if (d->a && d->a->kind == NK::Identifier)
                        exportName(d->a->text);
                }
                break;
            default: break;
        }
    }

    statements(n.list);
    NodePtr body = makeNode(NK::Block, line);
    body->list = std::move(n.list);
    NodePtr result = makeNode(NK::ReturnStmt, line);
    result->a = std::move(exported);
    body->list.push_back(std::move(result));

    NodePtr fn = makeNode(NK::FunctionExpr, line);
    fn->d = std::move(body);
    NodePtr call = makeNode(NK::Call, line);
    call->a = std::move(fn);
    out.push_back(declare(n.text, std::move(call), line));
}

void Eraser::lowerClass(Node& cls)
{
    cls.c.reset();  // the `implements` clause
    std::vector<NodePtr> members;
    members.reserve(cls.list.size());
    for (NodePtr& member : cls.list) {
        if (member->kind == NK::TypeMember || member->declared || member->isAbstract)
            continue;
        if (member->kind == NK::MethodDef && (!member->b || !member->b->d))
            continue;  // an overload signature
        members.push_back(std::move(member));
    }
    cls.list = std::move(members);

    for (NodePtr& member : cls.list) {
        stripTypes(*member);
        if (member->computed)
            expression(member->a);
        if (member->kind == NK::FieldDef) {
            if (member->b)
                expression(member->b);
            continue;
        }
        Node& fn = *member->b;
        if (member->propKind == PropKind::Constructor) {
            // `constructor(private x: T)` assigns the parameter to a field. The
            // assignments go after any `super(...)`, which has to run first.
            std::vector<NodePtr> prologue;
            for (NodePtr& param : fn.list) {
                if (!param->isParamProp)
                    continue;
                param->isParamProp = false;
                const Node* named = param->kind == NK::AssignPattern ? param->a.get() : param.get();
                if (!named || named->kind != NK::Identifier)
                    continue;
                NodePtr target = makeNode(NK::Member, named->line);
                target->a = makeNode(NK::ThisExpr, named->line);
                target->text = named->text;
                prologue.push_back(assignment(std::move(target),
                                              identifier(named->text, named->line), named->line));
            }
            if (!prologue.empty() && fn.d) {
                std::size_t at = 0;
                if (!fn.d->list.empty() && fn.d->list.front()->kind == NK::ExprStatement &&
                    fn.d->list.front()->a && fn.d->list.front()->a->kind == NK::Call &&
                    fn.d->list.front()->a->a && fn.d->list.front()->a->a->kind == NK::SuperExpr)
                    at = 1;
                fn.d->list.insert(fn.d->list.begin() + std::ptrdiff_t(at),
                                  std::make_move_iterator(prologue.begin()),
                                  std::make_move_iterator(prologue.end()));
            }
        }
        NodePtr held(member->b.release());
        expression(held);
        member->b = std::move(held);
    }
    if (cls.a)
        expression(cls.a);
    stripTypes(cls);
}

void Eraser::expression(NodePtr& slot)
{
    if (!slot)
        return;
    Node& n = *slot;
    switch (n.kind) {
        case NK::AsExpr:
        case NK::NonNull: {
            NodePtr inner = std::move(n.a);
            slot = std::move(inner);
            expression(slot);
            return;
        }
        case NK::ClassExpr: lowerClass(n); return;
        case NK::FunctionExpr:
        case NK::ArrowExpr:
            stripTypes(n);
            for (NodePtr& param : n.list) {
                param->type.reset();
                param->isParamProp = false;
                if (param->kind == NK::AssignPattern)
                    expression(param->b);
            }
            if (n.d)
                statements(n.d->list);
            return;
        default: break;
    }
    stripTypes(n);
    expression(n.a);
    expression(n.b);
    expression(n.c);
    if (n.d) {
        if (n.d->kind == NK::Block)
            statements(n.d->list);
        else
            expression(n.d);
    }
    for (NodePtr& child : n.list)
        expression(child);
}

void Eraser::statement(NodePtr& slot)
{
    if (!slot)
        return;
    Node& n = *slot;
    stripTypes(n);
    switch (n.kind) {
        case NK::Program:
        case NK::Block: statements(n.list); return;
        case NK::VarDecl:
            for (NodePtr& d : n.list) {
                d->type.reset();
                expression(d->b);
            }
            return;
        case NK::FunctionDecl: expression(n.a); return;
        case NK::ClassDecl: lowerClass(*n.a); return;
        case NK::If:
            expression(n.a);
            statement(n.b);
            statement(n.c);
            return;
        case NK::For:
            statement(n.a);
            expression(n.b);
            expression(n.c);
            statement(n.d);
            return;
        case NK::ForIn:
        case NK::ForOf:
            expression(n.a);
            expression(n.b);
            statement(n.d);
            return;
        case NK::While:
        case NK::DoWhile:
            expression(n.a);
            statement(n.d);
            return;
        case NK::Try:
            statement(n.a);
            if (n.b)
                n.b->type.reset();
            statement(n.c);
            statement(n.d);
            return;
        case NK::Switch:
            expression(n.a);
            for (NodePtr& clause : n.list) {
                expression(clause->a);
                statements(clause->list);
            }
            return;
        case NK::Labeled: statement(n.d); return;
        default: expression(slot); return;
    }
}

}  // namespace

void erase(Node& program)
{
    Eraser eraser;
    eraser.statements(program.list);
}

}  // namespace dyn
}  // namespace internal
}  // namespace script
