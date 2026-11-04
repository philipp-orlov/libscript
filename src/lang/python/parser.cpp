// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Python parser and lowering: Python source in, the dynamic engine's syntax
// tree out. See the header for the shape of the rewrite.
#include "script/internal/python/parser.hpp"

#include <algorithm>

#include "script/error.hpp"

namespace script {
namespace internal {
namespace python {

using dyn::PropKind;
using dyn::VarKind;

namespace {

/// The Python exception classes the prelude defines, so `raise ValueError`
/// and `ValueError("x")` know to construct rather than call.
const char* const kBuiltinClasses[] = {
    "Exception",       "BaseException",   "ValueError",        "TypeError",
    "KeyError",        "IndexError",      "ZeroDivisionError", "RuntimeError",
    "AssertionError",  "AttributeError",  "NotImplementedError", "StopIteration",
    "LookupError",     "ArithmeticError", "NameError",         "OverflowError",
    "RecursionError",  nullptr,
};

/// Method names whose Python meaning differs from the JavaScript method of
/// the same name on the same kind of object. Calls to these go through a
/// helper that looks at the receiver.
const char* const kDispatchedMethods[] = {
    "sort", "join", "split", "replace", "pop", "keys", "values", nullptr,
};

bool inList(const char* const* list, StringView name)
{
    for (; *list; ++list) {
        if (name == ascii(*list))
            return true;
    }
    return false;
}

}  // namespace

// ---------------------------------------------------------------- plumbing ---

Parser::Parser(std::string_view source, std::string name) : name_(std::move(name))
{
    Lexer lexer(source, name_);
    toks_ = lexer.tokenize();
}

bool Parser::eat(Tok t)
{
    if (!is(t))
        return false;
    advance();
    return true;
}

void Parser::expect(Tok t, const char* what)
{
    if (!eat(t))
        fail(std::string("expected ") + what + " but found " + tokenName(type()));
}

void Parser::fail(const std::string& message) const
{
    fail(line(), message);
}

void Parser::fail(std::uint32_t at, const std::string& message) const
{
    Lexer::fail(name_, at, message);
}

// ------------------------------------------------------------------ scopes ---

Parser::Scope& Parser::varScope()
{
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
        if (it->kind != Scope::Kind::Class)
            return *it;
    }
    return scopes_.front();
}

const Parser::Scope* Parser::classScope() const
{
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
        if (it->kind == Scope::Kind::Class)
            return &*it;
        if (it->kind == Scope::Kind::Function && it->selfName.empty())
            return nullptr;  // a plain nested function, not a method
    }
    return nullptr;
}

void Parser::declareName(const String& name)
{
    Scope& s = varScope();
    if (s.params.count(name) || s.globals.count(name) || s.nonlocals.count(name))
        return;
    if (s.seen.insert(name).second)
        s.declared.push_back(name);
}

void Parser::newTempNamed(const String& name)
{
    Scope* owner = &scopes_.front();
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
        if (it->kind != Scope::Kind::Class && it->holdsTemps) {
            owner = &*it;
            break;
        }
    }
    if (owner->seen.insert(name).second)
        owner->declared.push_back(name);
}

String Parser::newTemp()
{
    // A leading space keeps the name out of reach of any source text.
    String name = ascii(" t" + std::to_string(++tempCounter_));
    Scope* owner = &scopes_.front();
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
        if (it->kind != Scope::Kind::Class && it->holdsTemps) {
            owner = &*it;
            break;
        }
    }
    if (owner->seen.insert(name).second)
        owner->declared.push_back(name);
    return name;
}

/// `let a, b, c;` (or `var`) for everything the scope assigns.
Parser::NodePtr Parser::declarations(Scope& s, VarKind kind, std::uint32_t at)
{
    if (s.declared.empty())
        return nullptr;
    NodePtr decl = make(NK::VarDecl, at);
    decl->varKind = kind;
    for (const String& name : s.declared) {
        NodePtr d = make(NK::Declarator, at);
        d->a = ident(name, at);
        decl->list.push_back(std::move(d));
    }
    return decl;
}

// ------------------------------------------------------------------ module ---

Parser::NodePtr Parser::parseModule()
{
    NodePtr program = make(NK::Program, 1);
    scopes_.clear();
    scopes_.push_back(Scope{});
    scopes_.back().kind = Scope::Kind::Module;

    std::vector<NodePtr> body;
    while (!is(Tok::Eof)) {
        if (eat(Tok::Newline))
            continue;
        if (is(Tok::Indent))
            fail("unexpected indent");
        statement(body);
    }
    resolveKeywordCalls();

    // A module whose last statement is an assignment evaluates to nothing,
    // the way Python's does; only a trailing expression hands a value back.
    if (!body.empty() && body.back()->kind == NK::ExprStatement) {
        const Node& last = *body.back()->a;
        const bool isAssignment =
            last.kind == NK::Assign ||
            (last.kind == NK::Call && last.a->kind == NK::Member && last.a->a->kind == NK::Identifier &&
             last.a->a->text == ascii("__py") &&
             (last.a->text == ascii("setitem") || last.a->text == ascii("setslice")));
        if (isAssignment)
            body.back()->a = unary(JTok::Void, std::move(body.back()->a), last.line);
    }

    if (NodePtr decl = declarations(scopes_.back(), VarKind::Var, 1))
        program->list.push_back(std::move(decl));
    for (NodePtr& s : body)
        program->list.push_back(std::move(s));
    scopes_.pop_back();
    return program;
}

// -------------------------------------------------------------- statements ---

void Parser::statement(std::vector<NodePtr>& out)
{
    switch (type()) {
        case Tok::If: out.push_back(ifStatement()); return;
        case Tok::While: out.push_back(whileStatement()); return;
        case Tok::For: out.push_back(forStatement()); return;
        case Tok::Try: out.push_back(tryStatement()); return;
        case Tok::Def: {
            PropKind kind = PropKind::Method;
            bool isStatic = false;
            out.push_back(functionDef({}, false, kind, isStatic));
            return;
        }
        case Tok::Class: classDef(out); return;
        case Tok::At: decorated(out); return;
        case Tok::With: fail("'with' statements are not supported");
        case Tok::Async: fail("'async' is not supported");
        default: break;
    }
    // Simple statements: one or more on a line, separated by ';'.
    for (;;) {
        simpleStatement(out);
        if (eat(Tok::Semicolon)) {
            if (is(Tok::Newline) || is(Tok::Eof))
                break;
            continue;
        }
        break;
    }
    if (!eat(Tok::Newline) && !is(Tok::Eof) && !is(Tok::Dedent))
        fail(std::string("unexpected ") + tokenName(type()));
}

void Parser::simpleStatement(std::vector<NodePtr>& out)
{
    const std::uint32_t at = line();
    switch (type()) {
        case Tok::Pass: advance(); return;
        case Tok::Break: advance(); out.push_back(make(NK::BreakStmt, at)); return;
        case Tok::Continue: advance(); out.push_back(make(NK::ContinueStmt, at)); return;
        case Tok::Return: {
            advance();
            NodePtr n = make(NK::ReturnStmt, at);
            if (!is(Tok::Newline) && !is(Tok::Semicolon) && !is(Tok::Eof))
                n->a = exprList();
            out.push_back(std::move(n));
            return;
        }
        case Tok::Raise: {
            advance();
            NodePtr n = make(NK::ThrowStmt, at);
            if (is(Tok::Newline) || is(Tok::Semicolon) || is(Tok::Eof)) {
                const String& current = scope().currentException;
                if (current.empty())
                    fail("a bare 'raise' is only valid inside an 'except' block");
                n->a = ident(current, at);
            } else {
                NodePtr value = expression();
                // `raise ValueError` names the class; construct it.
                if (value->kind == NK::Identifier && isKnownClass(value->text)) {
                    NodePtr construct = make(NK::NewExpr, at);
                    construct->a = std::move(value);
                    value = std::move(construct);
                }
                if (eat(Tok::From))
                    expression();  // the cause is accepted and dropped
                n->a = std::move(value);
            }
            out.push_back(std::move(n));
            return;
        }
        case Tok::Global:
        case Tok::Nonlocal: {
            const bool isGlobal = type() == Tok::Global;
            advance();
            do {
                if (!is(Tok::Name))
                    fail("expected a name");
                const String name = advance().text;
                Scope& s = varScope();
                if (isGlobal)
                    s.globals.insert(name);
                else
                    s.nonlocals.insert(name);
            } while (eat(Tok::Comma));
            return;
        }
        case Tok::Del: {
            advance();
            do {
                NodePtr target = primary();
                if (target->kind == NK::Identifier) {
                    out.push_back(exprStatement(assign(std::move(target), undefinedValue(at), at)));
                } else if (target->kind == NK::Member && !target->computed) {
                    out.push_back(exprStatement(unary(JTok::Delete, std::move(target), at)));
                } else if (target->kind == NK::Call && target->a->kind == NK::Member &&
                           target->a->text == ascii("getitem")) {
                    // `del a[i]` parsed as `__py.getitem(a, i)`.
                    std::vector<NodePtr> args = std::move(target->list);
                    out.push_back(exprStatement(helper("delitem", std::move(args), at)));
                } else if (target->kind == NK::Call && target->a->kind == NK::Member &&
                           target->a->text == ascii("slice")) {
                    std::vector<NodePtr> args = std::move(target->list);
                    out.push_back(exprStatement(helper("delslice", std::move(args), at)));
                } else {
                    fail("cannot delete this expression");
                }
            } while (eat(Tok::Comma));
            return;
        }
        case Tok::Assert: {
            advance();
            NodePtr test = expression();
            NodePtr message;
            if (eat(Tok::Comma))
                message = expression();
            NodePtr n = make(NK::If, at);
            n->a = unary(JTok::Bang, truth(std::move(test)), at);
            NodePtr thr = make(NK::ThrowStmt, at);
            NodePtr construct = make(NK::NewExpr, at);
            construct->a = ident(ascii("AssertionError"), at);
            if (message)
                construct->list.push_back(std::move(message));
            thr->a = std::move(construct);
            n->b = std::move(thr);
            out.push_back(std::move(n));
            return;
        }
        case Tok::Import:
        case Tok::From: {
            if (NodePtr n = importStatement())
                out.push_back(std::move(n));
            return;
        }
        case Tok::Yield: fail("generators ('yield') are not supported");
        case Tok::Await: fail("'await' is not supported");
        default: expressionOrAssignment(out); return;
    }
}

void Parser::expressionOrAssignment(std::vector<NodePtr>& out)
{
    const std::uint32_t at = line();
    NodePtr first = is(Tok::Star) ? starred() : exprList();

    // `x: int = 5` -- the annotation is parsed and dropped.
    if (is(Tok::Colon)) {
        advance();
        expression();
        if (!eat(Tok::Assign)) {
            if (first->kind == NK::Identifier)
                declareName(first->text);
            return;  // a bare annotation declares nothing at run time
        }
        NodePtr value = exprList();
        out.push_back(exprStatement(assignTo(std::move(first), std::move(value), at)));
        return;
    }

    if (is(Tok::Assign)) {
        // `a = b = value`: collect the targets, then chain right to left.
        std::vector<NodePtr> targets;
        targets.push_back(std::move(first));
        while (eat(Tok::Assign)) {
            if (is(Tok::Yield))
                fail("generators ('yield') are not supported");
            targets.push_back(is(Tok::Star) ? starred() : exprList());
        }
        NodePtr value = std::move(targets.back());
        targets.pop_back();
        for (auto it = targets.rbegin(); it != targets.rend(); ++it)
            value = assignTo(std::move(*it), std::move(value), at);
        out.push_back(exprStatement(std::move(value)));
        return;
    }

    switch (type()) {
        case Tok::PlusAssign:
        case Tok::MinusAssign:
        case Tok::StarAssign:
        case Tok::SlashAssign:
        case Tok::DoubleSlashAssign:
        case Tok::PercentAssign:
        case Tok::StarStarAssign:
        case Tok::AmpAssign:
        case Tok::PipeAssign:
        case Tok::CaretAssign:
        case Tok::ShlAssign:
        case Tok::ShrAssign: {
            const Tok op = advance().type;
            NodePtr value = exprList();
            out.push_back(
                exprStatement(augmentedAssign(std::move(first), op, std::move(value), at)));
            return;
        }
        case Tok::AtAssign: fail("the '@' operator is not supported");
        default: break;
    }
    out.push_back(exprStatement(std::move(first)));
}

/// `target = value`, for every target shape: a name, an attribute, a
/// subscript (which becomes `__py.setitem`), or a tuple/list of targets
/// (which becomes a destructuring assignment).
Parser::NodePtr Parser::assignTo(NodePtr target, NodePtr value, std::uint32_t at)
{
    if (target->kind == NK::Call && target->a->kind == NK::Member &&
        target->a->a->kind == NK::Identifier && target->a->a->text == ascii("__py")) {
        // Parsed as a read: `__py.getitem(a, i)` -> `__py.setitem(a, i, v)`,
        // `__py.slice(a, lo, hi, step)` -> `__py.setslice(a, lo, hi, step, v)`.
        if (target->a->text == ascii("getitem")) {
            std::vector<NodePtr> args = std::move(target->list);
            args.push_back(std::move(value));
            return helper("setitem", std::move(args), at);
        }
        if (target->a->text == ascii("slice")) {
            std::vector<NodePtr> args = std::move(target->list);
            args.push_back(std::move(value));
            return helper("setslice", std::move(args), at);
        }
    }
    return assign(toTarget(std::move(target), true), std::move(value), at);
}

Parser::NodePtr Parser::toTarget(NodePtr expr, bool declare)
{
    switch (expr->kind) {
        case NK::Identifier:
            if (declare)
                declareName(expr->text);
            return expr;
        case NK::Member: return expr;
        case NK::Spread: {
            NodePtr rest = make(NK::RestElement, expr->line);
            rest->a = toTarget(std::move(expr->a), declare);
            return rest;
        }
        case NK::ArrayLit: {
            NodePtr pattern = make(NK::ArrayPattern, expr->line);
            for (NodePtr& element : expr->list)
                pattern->list.push_back(toTarget(std::move(element), declare));
            return pattern;
        }
        case NK::Call:
            // `a[i]` inside a tuple target: use a plain computed member, the
            // one place a subscript cannot go through the helper.
            if (expr->a->kind == NK::Member && expr->a->text == ascii("getitem") &&
                expr->list.size() == 2) {
                NodePtr m = make(NK::Member, expr->line);
                m->computed = true;
                m->a = std::move(expr->list[0]);
                m->b = std::move(expr->list[1]);
                return m;
            }
            [[fallthrough]];
        default: fail(expr->line, "cannot assign to this expression");
    }
}

Parser::NodePtr Parser::augmentedAssign(NodePtr target, Tok op, NodePtr value, std::uint32_t at)
{
    // A subscript target reads and writes through the helpers; the container
    // and index expressions are evaluated twice.
    const bool isItem = target->kind == NK::Call && target->a->kind == NK::Member &&
                        target->a->text == ascii("getitem");
    NodePtr read = clone(*target);
    NodePtr combined;
    switch (op) {
        case Tok::DoubleSlashAssign:
            combined = call(member(ident(ascii("Math"), at), ascii("floor"), at),
                            [&] {
                                std::vector<NodePtr> v;
                                v.push_back(binary(JTok::Slash, std::move(read), std::move(value), at));
                                return v;
                            }(),
                            at);
            break;
        case Tok::PercentAssign: {
            std::vector<NodePtr> args;
            args.push_back(std::move(read));
            args.push_back(std::move(value));
            combined = helper("mod", std::move(args), at);
            break;
        }
        default: break;
    }
    if (isItem) {
        if (!combined) {
            JTok jop = JTok::Plus;
            switch (op) {
                case Tok::PlusAssign: jop = JTok::Plus; break;
                case Tok::MinusAssign: jop = JTok::Minus; break;
                case Tok::StarAssign: jop = JTok::Star; break;
                case Tok::SlashAssign: jop = JTok::Slash; break;
                case Tok::StarStarAssign: jop = JTok::StarStar; break;
                case Tok::AmpAssign: jop = JTok::Amp; break;
                case Tok::PipeAssign: jop = JTok::Pipe; break;
                case Tok::CaretAssign: jop = JTok::Caret; break;
                case Tok::ShlAssign: jop = JTok::Shl; break;
                case Tok::ShrAssign: jop = JTok::Shr; break;
                default: break;
            }
            combined = binary(jop, std::move(read), std::move(value), at);
        }
        return assignTo(std::move(target), std::move(combined), at);
    }
    if (target->kind != NK::Identifier && target->kind != NK::Member)
        fail(at, "illegal target for an augmented assignment");
    if (target->kind == NK::Identifier)
        declareName(target->text);
    if (combined)
        return assign(std::move(target), std::move(combined), at);
    NodePtr n = make(NK::Assign, at);
    switch (op) {
        case Tok::PlusAssign: n->op = JTok::PlusAssign; break;
        case Tok::MinusAssign: n->op = JTok::MinusAssign; break;
        case Tok::StarAssign: n->op = JTok::StarAssign; break;
        case Tok::SlashAssign: n->op = JTok::SlashAssign; break;
        case Tok::StarStarAssign: n->op = JTok::StarStarAssign; break;
        case Tok::AmpAssign: n->op = JTok::AmpAssign; break;
        case Tok::PipeAssign: n->op = JTok::PipeAssign; break;
        case Tok::CaretAssign: n->op = JTok::CaretAssign; break;
        case Tok::ShlAssign: n->op = JTok::ShlAssign; break;
        case Tok::ShrAssign: n->op = JTok::ShrAssign; break;
        default: break;
    }
    n->a = std::move(target);
    n->b = std::move(value);
    return n;
}

Parser::NodePtr Parser::block()
{
    const std::uint32_t at = line();
    expect(Tok::Colon, "':'");
    std::vector<NodePtr> statements;
    if (eat(Tok::Newline)) {
        expect(Tok::Indent, "an indented block");
        while (!is(Tok::Dedent) && !is(Tok::Eof)) {
            if (eat(Tok::Newline))
                continue;
            statement(statements);
        }
        eat(Tok::Dedent);
    } else {
        // `if x: return 1` -- simple statements on the same line.
        for (;;) {
            simpleStatement(statements);
            if (eat(Tok::Semicolon) && !is(Tok::Newline) && !is(Tok::Eof))
                continue;
            break;
        }
        if (!eat(Tok::Newline) && !is(Tok::Eof))
            fail(std::string("unexpected ") + tokenName(type()));
    }
    return blockOf(std::move(statements), at);
}

Parser::NodePtr Parser::ifStatement()
{
    const std::uint32_t at = line();
    advance();  // if / elif
    NodePtr n = make(NK::If, at);
    n->a = truth(expression());
    n->b = block();
    if (is(Tok::Elif)) {
        n->c = ifStatement();
    } else if (eat(Tok::Else)) {
        n->c = block();
    }
    return n;
}

Parser::NodePtr Parser::whileStatement()
{
    const std::uint32_t at = line();
    advance();
    NodePtr n = make(NK::While, at);
    n->a = truth(expression());
    n->d = block();
    if (is(Tok::Else))
        fail("'else' on a loop is not supported");
    return n;
}

Parser::NodePtr Parser::forStatement()
{
    const std::uint32_t at = line();
    advance();
    NodePtr target = exprListNoCond();
    expect(Tok::In, "'in'");
    NodePtr iterable = exprList();

    // `for i in range(...)` with a plain name: a counting loop, no array.
    if (target->kind == NK::Identifier && iterable->kind == NK::Call &&
        iterable->a->kind == NK::Identifier && iterable->a->text == ascii("range") &&
        !iterable->list.empty() && iterable->list.size() <= 3) {
        std::vector<NodePtr>& args = iterable->list;
        bool simple = true;
        for (const NodePtr& a : args)
            simple = simple && a->kind != NK::Spread;
        double step = 1;
        if (args.size() == 3) {
            if (args[2]->kind == NK::NumberLit)
                step = args[2]->number;
            else if (args[2]->kind == NK::Unary && args[2]->op == JTok::Minus &&
                     args[2]->a->kind == NK::NumberLit)
                step = -args[2]->a->number;
            else
                simple = false;
        }
        if (simple && step != 0) {
            // for (c = start, bound = stop; c < bound; c += step) { i = c; ... }
            // A hidden counter, so the body may assign the variable without
            // steering the loop and the variable keeps its last value after.
            declareName(target->text);
            const String name = target->text;
            const String counter = newTemp();
            NodePtr start = args.size() == 1 ? number(0, at) : std::move(args[0]);
            NodePtr stop = std::move(args[args.size() == 1 ? 0 : 1]);
            NodePtr seq = make(NK::Sequence, at);
            seq->list.push_back(assign(ident(counter, at), std::move(start), at));
            NodePtr test;
            if (stop->kind == NK::NumberLit) {
                test = binary(step > 0 ? JTok::Lt : JTok::Gt, ident(counter, at), std::move(stop), at);
            } else {
                const String bound = newTemp();
                seq->list.push_back(assign(ident(bound, at), std::move(stop), at));
                test = binary(step > 0 ? JTok::Lt : JTok::Gt, ident(counter, at), ident(bound, at), at);
            }
            NodePtr n = make(NK::For, at);
            n->varKind = VarKind::None;
            n->a = exprStatement(std::move(seq));
            n->b = std::move(test);
            NodePtr update = make(NK::Assign, at);
            update->op = JTok::PlusAssign;
            update->a = ident(counter, at);
            update->b = number(step, at);
            n->c = std::move(update);
            NodePtr body = block();
            body->list.insert(body->list.begin(),
                              exprStatement(assign(ident(name, at), ident(counter, at), at)));
            n->d = std::move(body);
            if (is(Tok::Else))
                fail("'else' on a loop is not supported");
            return n;
        }
    }

    NodePtr n = make(NK::ForOf, at);
    n->varKind = VarKind::None;
    n->a = toTarget(std::move(target), true);
    std::vector<NodePtr> args;
    args.push_back(std::move(iterable));
    n->b = helper("iter", std::move(args), at);
    n->d = block();
    if (is(Tok::Else))
        fail("'else' on a loop is not supported");
    return n;
}

Parser::NodePtr Parser::tryStatement()
{
    const std::uint32_t at = line();
    advance();
    NodePtr n = make(NK::Try, at);
    n->a = block();

    struct Handler
    {
        std::vector<NodePtr> types;  // empty: catch everything
        String name;
        NodePtr body;
        std::uint32_t line;
    };
    std::vector<Handler> handlers;
    while (is(Tok::Except)) {
        Handler h;
        h.line = line();
        advance();
        if (!is(Tok::Colon)) {
            NodePtr types = expression();
            if (types->kind == NK::ArrayLit) {
                for (NodePtr& t : types->list)
                    h.types.push_back(std::move(t));
            } else {
                h.types.push_back(std::move(types));
            }
            if (eat(Tok::As)) {
                if (!is(Tok::Name))
                    fail("expected a name after 'as'");
                h.name = advance().text;
                declareName(h.name);
            }
        }
        const String caught = ascii(" exc");
        const String saved = scope().currentException;
        scope().currentException = caught;
        h.body = block();
        scope().currentException = saved;
        handlers.push_back(std::move(h));
    }
    if (is(Tok::Else))
        fail("'else' on 'try' is not supported");
    if (eat(Tok::Finally))
        n->d = block();
    if (handlers.empty() && !n->d)
        fail("'try' needs an 'except' or a 'finally'");

    if (!handlers.empty()) {
        const String caught = ascii(" exc");
        n->b = ident(caught, at);
        // if (e instanceof A) {...} else if (e instanceof B) {...} else throw e;
        NodePtr chain;
        NodePtr* tail = &chain;
        bool catchesAll = false;
        for (Handler& h : handlers) {
            std::vector<NodePtr> body;
            if (!h.name.empty())
                body.push_back(exprStatement(assign(ident(h.name, h.line), ident(caught, h.line), h.line)));
            body.push_back(std::move(h.body));
            NodePtr handlerBlock = blockOf(std::move(body), h.line);
            if (h.types.empty()) {
                *tail = std::move(handlerBlock);
                catchesAll = true;
                break;
            }
            NodePtr test;
            for (NodePtr& t : h.types) {
                std::vector<NodePtr> args;
                args.push_back(ident(caught, h.line));
                args.push_back(std::move(t));
                NodePtr one = helper("isinstance", std::move(args), h.line);
                test = test ? logical(JTok::OrOr, std::move(test), std::move(one), h.line)
                            : std::move(one);
            }
            NodePtr branch = make(NK::If, h.line);
            branch->a = std::move(test);
            branch->b = std::move(handlerBlock);
            *tail = std::move(branch);
            tail = &(*tail)->c;
        }
        if (!catchesAll) {
            NodePtr rethrow = make(NK::ThrowStmt, at);
            rethrow->a = ident(caught, at);
            *tail = std::move(rethrow);
        }
        std::vector<NodePtr> body;
        body.push_back(std::move(chain));
        n->c = blockOf(std::move(body), at);
    }
    return n;
}

std::vector<Parser::NodePtr> Parser::parameters(bool isMethod, String* selfName)
{
    std::vector<NodePtr> params;
    expect(Tok::LParen, "'('");
    bool first = true;
    while (!is(Tok::RParen)) {
        const std::uint32_t at = line();
        if (eat(Tok::StarStar))
            fail("'**kwargs' parameters are not supported");
        if (eat(Tok::Slash)) {
            if (!is(Tok::RParen))
                expect(Tok::Comma, "','");
            continue;
        }
        if (eat(Tok::Star)) {
            if (!is(Tok::Name))
                fail("keyword-only parameters are not supported");
            NodePtr rest = make(NK::RestElement, at);
            const String name = advance().text;
            scope().params.insert(name);
            rest->a = ident(name, at);
            if (eat(Tok::Colon))
                expression();
            params.push_back(std::move(rest));
            if (!is(Tok::RParen))
                expect(Tok::Comma, "','");
            if (!is(Tok::RParen))
                fail("keyword-only parameters are not supported");
            continue;
        }
        if (!is(Tok::Name))
            fail("expected a parameter name");
        const String name = advance().text;
        if (eat(Tok::Colon))
            expression();  // annotation
        NodePtr param = ident(name, at);
        if (eat(Tok::Assign)) {
            NodePtr withDefault = make(NK::AssignPattern, at);
            withDefault->a = std::move(param);
            withDefault->b = expression();
            param = std::move(withDefault);
        }
        if (first && isMethod && selfName) {
            *selfName = name;
            // `self` is bound from `this` in the body rather than passed.
        } else {
            params.push_back(std::move(param));
        }
        scope().params.insert(name);
        first = false;
        if (!is(Tok::RParen))
            expect(Tok::Comma, "','");
    }
    advance();
    return params;
}

/// `def name(params): body`, as a function declaration or -- inside a class
/// -- a method definition. `decorators` may hold staticmethod, classmethod,
/// property or `<name>.setter`.
Parser::NodePtr Parser::functionDef(std::vector<String> decorators, bool inClass, PropKind& kind,
                                    bool& isStatic)
{
    const std::uint32_t at = line();
    expect(Tok::Def, "'def'");
    if (!is(Tok::Name))
        fail("expected a function name");
    const String name = advance().text;

    bool bindSelf = inClass;
    bool bindCls = false;
    kind = PropKind::Method;
    isStatic = false;
    for (const String& d : decorators) {
        if (d == ascii("staticmethod")) {
            isStatic = true;
            bindSelf = false;
        } else if (d == ascii("classmethod")) {
            isStatic = true;
            bindCls = true;
        } else if (d == ascii("property")) {
            kind = PropKind::Getter;
        } else if (d.size() > 7 && d.compare(d.size() - 7, 7, ascii(".setter")) == 0) {
            kind = PropKind::Setter;
        } else {
            fail("decorator '@" + toUtf8(d) + "' is not supported");
        }
    }

    // The declaration itself binds the name (hoisted, as JavaScript does),
    // so it must not also appear in the scope's `let` list.
    if (!inClass)
        varScope().seen.insert(name);

    scopes_.push_back(Scope{});
    scope().kind = Scope::Kind::Function;
    String selfName;
    std::vector<NodePtr> params = parameters(bindSelf || bindCls, &selfName);
    if (!selfName.empty())
        scope().selfName = selfName;
    if (eat(Tok::Arrow))
        expression();  // return annotation

    if (!inClass) {
        Signature sig;
        for (const NodePtr& p : params) {
            if (p->kind == NK::RestElement)
                sig.variadic = true;
            else
                sig.params.push_back(p->kind == NK::AssignPattern ? p->a->text : p->text);
        }
        if (scopes_.size() == 2)
            functions_[name] = std::move(sig);
    } else if (name == ascii("__init__")) {
        Signature sig;
        for (const NodePtr& p : params) {
            if (p->kind == NK::RestElement)
                sig.variadic = true;
            else
                sig.params.push_back(p->kind == NK::AssignPattern ? p->a->text : p->text);
        }
        const Scope* cls = classScope();
        if (cls && scopes_.size() == 3)
            functions_[cls->className] = std::move(sig);
    }

    NodePtr body = block();
    Scope fnScope = std::move(scopes_.back());
    scopes_.pop_back();

    std::vector<NodePtr> prologue;
    if (!selfName.empty())
        prologue.push_back(constDecl(selfName, make(NK::ThisExpr, at), at));
    if (NodePtr decl = declarations(fnScope, VarKind::Let, at))
        prologue.push_back(std::move(decl));
    if (!prologue.empty())
        body->list.insert(body->list.begin(), std::make_move_iterator(prologue.begin()),
                          std::make_move_iterator(prologue.end()));

    NodePtr fn = make(NK::FunctionExpr, at);
    fn->text = name;
    fn->list = std::move(params);
    fn->d = std::move(body);

    if (inClass) {
        NodePtr m = make(NK::MethodDef, at);
        m->isStatic = isStatic;
        m->propKind = kind;
        if (name == ascii("__init__")) {
            m->propKind = PropKind::Constructor;
            fn->text = ascii("constructor");
            m->a = string(ascii("constructor"), at);
        } else if (name == ascii("__iter__")) {
            m->computed = true;
            m->a = member(ident(ascii("Symbol"), at), ascii("iterator"), at);
        } else {
            m->a = string(name, at);
        }
        m->b = std::move(fn);
        return m;
    }

    NodePtr decl = make(NK::FunctionDecl, at);
    decl->text = name;
    decl->a = std::move(fn);
    return decl;
}

void Parser::decorated(std::vector<NodePtr>& out)
{
    std::vector<String> decorators;
    while (eat(Tok::At)) {
        if (!is(Tok::Name))
            fail("expected a decorator name");
        String name = advance().text;
        while (eat(Tok::Dot)) {
            if (!is(Tok::Name))
                fail("expected a name");
            name += u".";
            name += advance().text;
        }
        if (is(Tok::LParen))
            fail("decorator '@" + toUtf8(name) + "' is not supported");
        decorators.push_back(std::move(name));
        expect(Tok::Newline, "end of line");
    }
    if (is(Tok::Class)) {
        if (!decorators.empty())
            fail("class decorators are not supported");
        classDef(out);
        return;
    }
    if (!is(Tok::Def))
        fail("expected 'def' after a decorator");
    if (scope().kind != Scope::Kind::Class) {
        for (const String& d : decorators)
            fail("decorator '@" + toUtf8(d) + "' is only supported on a method");
    }
    PropKind kind = PropKind::Method;
    bool isStatic = false;
    out.push_back(functionDef(std::move(decorators), true, kind, isStatic));
}

/// `class Name(Base): body` -> `var Name = class Name extends Base { ... }`
/// followed by one assignment per class attribute.
void Parser::classDef(std::vector<NodePtr>& out)
{
    const std::uint32_t at = line();
    expect(Tok::Class, "'class'");
    if (!is(Tok::Name))
        fail("expected a class name");
    const String name = advance().text;
    declareName(name);
    classes_.insert(name);

    NodePtr cls = make(NK::ClassExpr, at);
    cls->text = name;
    String baseName;
    if (eat(Tok::LParen)) {
        if (!is(Tok::RParen)) {
            NodePtr base = expression();
            if (is(Tok::Comma) && type(1) != Tok::RParen)
                fail("multiple inheritance is not supported");
            eat(Tok::Comma);
            if (base->kind == NK::Identifier && base->text == ascii("object")) {
                // `class X(object)` is the same as `class X`.
            } else {
                if (base->kind == NK::Identifier)
                    baseName = base->text;
                cls->a = std::move(base);
            }
        }
        expect(Tok::RParen, "')'");
    }

    scopes_.push_back(Scope{});
    scope().kind = Scope::Kind::Class;
    scope().className = name;
    scope().baseName = baseName;

    std::vector<std::pair<String, NodePtr>> attributes;
    expect(Tok::Colon, "':'");
    expect(Tok::Newline, "end of line");
    expect(Tok::Indent, "an indented class body");
    while (!is(Tok::Dedent) && !is(Tok::Eof)) {
        if (eat(Tok::Newline))
            continue;
        const std::uint32_t memberLine = line();
        if (is(Tok::Pass)) {
            advance();
            expect(Tok::Newline, "end of line");
            continue;
        }
        if (is(Tok::String) && (type(1) == Tok::Newline || type(1) == Tok::Eof)) {
            advance();  // docstring
            eat(Tok::Newline);
            continue;
        }
        if (is(Tok::Def) || is(Tok::At)) {
            std::vector<NodePtr> members;
            if (is(Tok::At)) {
                decorated(members);
            } else {
                PropKind kind = PropKind::Method;
                bool isStatic = false;
                members.push_back(functionDef({}, true, kind, isStatic));
            }
            for (NodePtr& m : members)
                cls->list.push_back(std::move(m));
            continue;
        }
        if (is(Tok::Name) && (type(1) == Tok::Assign || type(1) == Tok::Colon)) {
            const String attr = advance().text;
            if (eat(Tok::Colon)) {
                expression();
                if (!eat(Tok::Assign)) {
                    expect(Tok::Newline, "end of line");
                    continue;
                }
            } else {
                advance();  // =
            }
            attributes.emplace_back(attr, exprList());
            if (!eat(Tok::Newline) && !is(Tok::Eof))
                fail(std::string("unexpected ") + tokenName(type()));
            continue;
        }
        fail(memberLine, "only methods and attributes are allowed in a class body");
    }
    eat(Tok::Dedent);
    scopes_.pop_back();

    // A derived class whose __init__ never reaches the base's has to call
    // super() somewhere for `this` to exist; do it first.
    if (cls->a) {
        for (NodePtr& m : cls->list) {
            if (m->propKind != PropKind::Constructor)
                continue;
            if (!containsSuperCall(*m->b->d)) {
                NodePtr superCall = make(NK::Call, at);
                superCall->a = make(NK::SuperExpr, at);
                std::vector<NodePtr>& body = m->b->d->list;
                body.insert(body.begin(), exprStatement(std::move(superCall)));
            }
        }
    }

    NodePtr decl = make(NK::VarDecl, at);
    decl->varKind = varScope().kind == Scope::Kind::Module ? VarKind::Var : VarKind::Let;
    NodePtr d = make(NK::Declarator, at);
    d->a = ident(name, at);
    d->b = std::move(cls);
    decl->list.push_back(std::move(d));
    out.push_back(std::move(decl));
    // The module-level `var` list would declare it again.
    Scope& vs = varScope();
    vs.declared.erase(std::remove(vs.declared.begin(), vs.declared.end(), name), vs.declared.end());

    for (auto& attribute : attributes) {
        // Name.attr = Name.prototype.attr = value: reachable from the class
        // and from every instance, as a Python class attribute is.
        NodePtr onProto =
            assign(member(member(ident(name, at), ascii("prototype"), at), attribute.first, at),
                   std::move(attribute.second), at);
        out.push_back(exprStatement(
            assign(member(ident(name, at), attribute.first, at), std::move(onProto), at)));
    }
}

Parser::NodePtr Parser::importStatement()
{
    const std::uint32_t at = line();
    static const char* const kModules[] = {"math", "random", "sys", "time", nullptr};
    if (eat(Tok::Import)) {
        do {
            if (!is(Tok::Name))
                fail("expected a module name");
            const String module = advance().text;
            if (is(Tok::Dot))
                fail("only the top-level modules math, random, sys and time are available");
            if (!inList(kModules, module))
                fail("module '" + toUtf8(module) + "' is not available");
            if (eat(Tok::As)) {
                if (!is(Tok::Name))
                    fail("expected a name after 'as'");
                const String alias = advance().text;
                declareName(alias);
                return exprStatement(assign(ident(alias, at), ident(module, at), at));
            }
        } while (eat(Tok::Comma));
        return nullptr;
    }
    expect(Tok::From, "'from'");
    if (!is(Tok::Name))
        fail("expected a module name");
    const String module = advance().text;
    if (!inList(kModules, module))
        fail("module '" + toUtf8(module) + "' is not available");
    expect(Tok::Import, "'import'");
    if (eat(Tok::Star))
        fail("'from module import *' is not supported");
    const bool parens = eat(Tok::LParen);
    NodePtr seq = make(NK::Sequence, at);
    do {
        if (parens && is(Tok::RParen))
            break;
        if (!is(Tok::Name))
            fail("expected a name");
        const String what = advance().text;
        String alias = what;
        if (eat(Tok::As)) {
            if (!is(Tok::Name))
                fail("expected a name after 'as'");
            alias = advance().text;
        }
        declareName(alias);
        seq->list.push_back(assign(ident(alias, at), member(ident(module, at), what, at), at));
    } while (eat(Tok::Comma));
    if (parens)
        expect(Tok::RParen, "')'");
    if (seq->list.size() == 1) {
        NodePtr only = std::move(seq->list[0]);
        return exprStatement(std::move(only));
    }
    return exprStatement(std::move(seq));
}

// ------------------------------------------------------------- expressions ---

Parser::NodePtr Parser::expression()
{
    if (is(Tok::Lambda))
        return lambda();
    return ternary();
}

/// `a, b, c` is a tuple; `a` alone is itself.
Parser::NodePtr Parser::exprList()
{
    const std::uint32_t at = line();
    NodePtr first = is(Tok::Star) ? starred() : expression();
    if (!is(Tok::Comma))
        return first;
    NodePtr tuple = make(NK::ArrayLit, at);
    tuple->list.push_back(std::move(first));
    while (eat(Tok::Comma)) {
        if (is(Tok::Newline) || is(Tok::Assign) || is(Tok::RParen) || is(Tok::Eof) ||
            is(Tok::Semicolon) || is(Tok::Colon) || is(Tok::In) || is(Tok::RBracket))
            break;  // a trailing comma
        tuple->list.push_back(is(Tok::Star) ? starred() : expression());
    }
    return tuple;
}

/// The target list of a `for`: stops at `in`, so `a if c else b` is not
/// tried there.
Parser::NodePtr Parser::exprListNoCond()
{
    const std::uint32_t at = line();
    NodePtr first = is(Tok::Star) ? starred() : bitOr();
    if (!is(Tok::Comma))
        return first;
    NodePtr tuple = make(NK::ArrayLit, at);
    tuple->list.push_back(std::move(first));
    while (eat(Tok::Comma)) {
        if (is(Tok::In))
            break;
        tuple->list.push_back(is(Tok::Star) ? starred() : bitOr());
    }
    return tuple;
}

Parser::NodePtr Parser::starred()
{
    const std::uint32_t at = line();
    expect(Tok::Star, "'*'");
    NodePtr s = make(NK::Spread, at);
    s->a = bitOr();
    return s;
}

Parser::NodePtr Parser::lambda()
{
    const std::uint32_t at = line();
    expect(Tok::Lambda, "'lambda'");
    scopes_.push_back(Scope{});
    scope().kind = Scope::Kind::Function;
    scope().holdsTemps = false;
    std::vector<NodePtr> params;
    while (!is(Tok::Colon)) {
        const std::uint32_t pl = line();
        if (eat(Tok::Star)) {
            if (!is(Tok::Name))
                fail("keyword-only parameters are not supported");
            NodePtr rest = make(NK::RestElement, pl);
            const String name = advance().text;
            scope().params.insert(name);
            rest->a = ident(name, pl);
            params.push_back(std::move(rest));
        } else {
            if (!is(Tok::Name))
                fail("expected a parameter name");
            const String name = advance().text;
            scope().params.insert(name);
            NodePtr p = ident(name, pl);
            if (eat(Tok::Assign)) {
                NodePtr withDefault = make(NK::AssignPattern, pl);
                withDefault->a = std::move(p);
                withDefault->b = expression();
                p = std::move(withDefault);
            }
            params.push_back(std::move(p));
        }
        if (!is(Tok::Colon))
            expect(Tok::Comma, "','");
    }
    advance();
    NodePtr value = expression();
    scopes_.pop_back();

    NodePtr fn = make(NK::ArrowExpr, at);
    fn->list = std::move(params);
    NodePtr body = make(NK::Block, at);
    NodePtr ret = make(NK::ReturnStmt, at);
    ret->a = std::move(value);
    body->list.push_back(std::move(ret));
    fn->d = std::move(body);
    return fn;
}

Parser::NodePtr Parser::ternary()
{
    const std::uint32_t at = line();
    NodePtr value = orTest();
    if (!is(Tok::If))
        return value;
    advance();
    NodePtr test = orTest();
    expect(Tok::Else, "'else'");
    NodePtr other = expression();
    NodePtr n = make(NK::Conditional, at);
    n->a = truth(std::move(test));
    n->b = std::move(value);
    n->c = std::move(other);
    return n;
}

/// `a or b` and `a and b` yield an operand, chosen by Python truth. When the
/// left side is already a boolean the JavaScript operator does the same
/// thing; otherwise the operand is held in a temporary and tested.
Parser::NodePtr Parser::shortCircuit(bool isOr, NodePtr left, NodePtr right, std::uint32_t at)
{
    if (isBoolish(*left))
        return logical(isOr ? JTok::OrOr : JTok::AndAnd, std::move(left), std::move(right), at);
    const String temp = newTemp();
    NodePtr seq = make(NK::Sequence, at);
    seq->list.push_back(assign(ident(temp, at), std::move(left), at));
    NodePtr pick = make(NK::Conditional, at);
    pick->a = truth(ident(temp, at));
    if (isOr) {
        pick->b = ident(temp, at);
        pick->c = std::move(right);
    } else {
        pick->b = std::move(right);
        pick->c = ident(temp, at);
    }
    seq->list.push_back(std::move(pick));
    return seq;
}

Parser::NodePtr Parser::orTest()
{
    NodePtr left = andTest();
    while (is(Tok::Or)) {
        const std::uint32_t at = line();
        advance();
        left = shortCircuit(true, std::move(left), andTest(), at);
    }
    return left;
}

Parser::NodePtr Parser::andTest()
{
    NodePtr left = notTest();
    while (is(Tok::And)) {
        const std::uint32_t at = line();
        advance();
        left = shortCircuit(false, std::move(left), notTest(), at);
    }
    return left;
}

Parser::NodePtr Parser::notTest()
{
    if (is(Tok::Not)) {
        const std::uint32_t at = line();
        advance();
        return unary(JTok::Bang, truth(notTest()), at);
    }
    return comparison();
}

/// One link of a comparison chain.
Parser::NodePtr Parser::comparisonOne(Tok op, bool negate, NodePtr left, NodePtr right,
                                      std::uint32_t at)
{
    NodePtr result;
    switch (op) {
        case Tok::Lt: result = binary(JTok::Lt, std::move(left), std::move(right), at); break;
        case Tok::Gt: result = binary(JTok::Gt, std::move(left), std::move(right), at); break;
        case Tok::Le: result = binary(JTok::Le, std::move(left), std::move(right), at); break;
        case Tok::Ge: result = binary(JTok::Ge, std::move(left), std::move(right), at); break;
        case Tok::EqEq:
        case Tok::NotEq: {
            const bool equal = op == Tok::EqEq;
            if (isNoneLiteral(*left) || isNoneLiteral(*right)) {
                // `x == None` is `x is None`: null or undefined.
                result = binary(equal ? JTok::EqEq : JTok::NotEq, std::move(left),
                                std::move(right), at);
            } else if (isLiteral(*left) || isLiteral(*right)) {
                result = binary(equal ? JTok::EqEqEq : JTok::NotEqEq, std::move(left),
                                std::move(right), at);
            } else {
                std::vector<NodePtr> args;
                args.push_back(std::move(left));
                args.push_back(std::move(right));
                result = helper("eq", std::move(args), at);
                if (!equal)
                    result = unary(JTok::Bang, std::move(result), at);
            }
            break;
        }
        case Tok::Is: {
            if (isNoneLiteral(*left) || isNoneLiteral(*right))
                result = binary(negate ? JTok::NotEq : JTok::EqEq, std::move(left),
                                std::move(right), at);
            else
                result = binary(negate ? JTok::NotEqEq : JTok::EqEqEq, std::move(left),
                                std::move(right), at);
            negate = false;
            break;
        }
        case Tok::In: {
            std::vector<NodePtr> args;
            args.push_back(std::move(right));
            args.push_back(std::move(left));
            result = helper("contains", std::move(args), at);
            break;
        }
        default: fail(at, "bad comparison operator");
    }
    if (negate)
        result = unary(JTok::Bang, std::move(result), at);
    return result;
}

Parser::NodePtr Parser::comparison()
{
    NodePtr left = bitOr();
    NodePtr chain;
    for (;;) {
        const std::uint32_t at = line();
        Tok op = type();
        bool negate = false;
        if (op == Tok::Not && type(1) == Tok::In) {
            advance();
            op = Tok::In;
            negate = true;
        } else if (op == Tok::Is) {
            if (type(1) == Tok::Not) {
                advance();
                negate = true;
            }
        } else if (op != Tok::Lt && op != Tok::Gt && op != Tok::Le && op != Tok::Ge &&
                   op != Tok::EqEq && op != Tok::NotEq && op != Tok::In) {
            break;
        }
        advance();
        NodePtr right = bitOr();
        // `a < b < c`: the middle operand is evaluated twice.
        NodePtr rightCopy = is(Tok::Lt) || is(Tok::Gt) || is(Tok::Le) || is(Tok::Ge) ||
                                    is(Tok::EqEq) || is(Tok::NotEq) || is(Tok::In) ||
                                    is(Tok::Is) || (is(Tok::Not) && type(1) == Tok::In)
                                ? clone(*right)
                                : nullptr;
        NodePtr link = comparisonOne(op, negate, std::move(left), std::move(right), at);
        chain = chain ? logical(JTok::AndAnd, std::move(chain), std::move(link), at)
                      : std::move(link);
        if (!rightCopy)
            return chain;
        left = std::move(rightCopy);
    }
    return left;
}

Parser::NodePtr Parser::bitOr()
{
    NodePtr left = bitXor();
    while (is(Tok::Pipe)) {
        const std::uint32_t at = line();
        advance();
        left = binary(JTok::Pipe, std::move(left), bitXor(), at);
    }
    return left;
}

Parser::NodePtr Parser::bitXor()
{
    NodePtr left = bitAnd();
    while (is(Tok::Caret)) {
        const std::uint32_t at = line();
        advance();
        left = binary(JTok::Caret, std::move(left), bitAnd(), at);
    }
    return left;
}

Parser::NodePtr Parser::bitAnd()
{
    NodePtr left = shift();
    while (is(Tok::Amp)) {
        const std::uint32_t at = line();
        advance();
        left = binary(JTok::Amp, std::move(left), shift(), at);
    }
    return left;
}

Parser::NodePtr Parser::shift()
{
    NodePtr left = arith();
    while (is(Tok::Shl) || is(Tok::Shr)) {
        const std::uint32_t at = line();
        const JTok op = advance().type == Tok::Shl ? JTok::Shl : JTok::Shr;
        left = binary(op, std::move(left), arith(), at);
    }
    return left;
}

Parser::NodePtr Parser::arith()
{
    NodePtr left = term();
    while (is(Tok::Plus) || is(Tok::Minus)) {
        const std::uint32_t at = line();
        const Tok op = advance().type;
        NodePtr right = term();
        if (op == Tok::Plus && (left->kind == NK::ArrayLit || right->kind == NK::ArrayLit)) {
            // List concatenation, recognisable when either side is a display.
            std::vector<NodePtr> args;
            args.push_back(std::move(left));
            args.push_back(std::move(right));
            left = helper("add", std::move(args), at);
        } else {
            left = binary(op == Tok::Plus ? JTok::Plus : JTok::Minus, std::move(left),
                          std::move(right), at);
        }
    }
    return left;
}

Parser::NodePtr Parser::term()
{
    NodePtr left = factor();
    for (;;) {
        const std::uint32_t at = line();
        switch (type()) {
            case Tok::Star: {
                advance();
                NodePtr right = factor();
                if (left->kind == NK::StringLit || right->kind == NK::StringLit ||
                    left->kind == NK::ArrayLit || right->kind == NK::ArrayLit ||
                    left->kind == NK::TemplateLit || right->kind == NK::TemplateLit) {
                    std::vector<NodePtr> args;
                    args.push_back(std::move(left));
                    args.push_back(std::move(right));
                    left = helper("mul", std::move(args), at);
                } else {
                    left = binary(JTok::Star, std::move(left), std::move(right), at);
                }
                break;
            }
            case Tok::Slash:
                advance();
                left = binary(JTok::Slash, std::move(left), factor(), at);
                break;
            case Tok::DoubleSlash: {
                advance();
                std::vector<NodePtr> args;
                args.push_back(binary(JTok::Slash, std::move(left), factor(), at));
                left = call(member(ident(ascii("Math"), at), ascii("floor"), at), std::move(args), at);
                break;
            }
            case Tok::Percent: {
                advance();
                NodePtr right = factor();
                if (right->kind == NK::NumberLit && right->number > 0 &&
                    left->kind != NK::StringLit && left->kind != NK::TemplateLit) {
                    // ((a % n) + n) % n, the floored modulo, with no call.
                    const double n = right->number;
                    NodePtr inner = binary(JTok::Percent, std::move(left), number(n, at), at);
                    NodePtr shifted = binary(JTok::Plus, std::move(inner), number(n, at), at);
                    left = binary(JTok::Percent, std::move(shifted), number(n, at), at);
                } else {
                    std::vector<NodePtr> args;
                    args.push_back(std::move(left));
                    args.push_back(std::move(right));
                    left = helper("mod", std::move(args), at);
                }
                break;
            }
            case Tok::At: fail("the '@' operator is not supported");
            default: return left;
        }
    }
}

Parser::NodePtr Parser::factor()
{
    const std::uint32_t at = line();
    switch (type()) {
        case Tok::Minus: {
            advance();
            NodePtr operand = factor();
            if (operand->kind == NK::NumberLit) {
                operand->number = -operand->number;
                return operand;
            }
            return unary(JTok::Minus, std::move(operand), at);
        }
        case Tok::Plus: advance(); return unary(JTok::Plus, factor(), at);
        case Tok::Tilde: advance(); return unary(JTok::Tilde, factor(), at);
        default: return power();
    }
}

Parser::NodePtr Parser::power()
{
    NodePtr base = primary();
    if (is(Tok::StarStar)) {
        const std::uint32_t at = line();
        advance();
        return binary(JTok::StarStar, std::move(base), factor(), at);
    }
    return base;
}

Parser::NodePtr Parser::primary()
{
    if (is(Tok::Await))
        fail("'await' is not supported");
    return trailers(atom());
}

Parser::NodePtr Parser::trailers(NodePtr base)
{
    for (;;) {
        const std::uint32_t at = line();
        if (eat(Tok::Dot)) {
            if (!is(Tok::Name))
                fail("expected an attribute name");
            base = member(std::move(base), advance().text, at);
            base->bindMethod = true;  // a call takes the receiver another way, and never reads this flag
            continue;
        }
        if (is(Tok::LParen)) {
            base = callExpression(std::move(base), at);
            continue;
        }
        if (is(Tok::LBracket)) {
            base = subscript(std::move(base), at);
            continue;
        }
        return base;
    }
}

/// `callee(args)`, with the rewrites a call may need: `Cls(...)` becomes
/// `new Cls(...)`, `super().__init__(...)` becomes `super(...)`,
/// `Base.__init__(self, ...)` in a derived class becomes `super(...)`, a
/// method whose Python meaning differs goes through `__py.m_<name>`, and
/// keyword arguments are resolved once the module has been read.
Parser::NodePtr Parser::callExpression(NodePtr callee, std::uint32_t at)
{
    expect(Tok::LParen, "'('");
    std::vector<NodePtr> args;
    std::vector<std::pair<String, NodePtr>> keywords;
    while (!is(Tok::RParen)) {
        if (eat(Tok::StarStar))
            fail("'**' in a call is not supported");
        if (is(Tok::Star)) {
            args.push_back(starred());
        } else if (is(Tok::Name) && type(1) == Tok::Assign) {
            const String key = advance().text;
            advance();
            keywords.emplace_back(key, expression());
        } else {
            if (!keywords.empty())
                fail("a positional argument cannot follow a keyword argument");
            NodePtr value = expression();
            if (is(Tok::For)) {
                // f(x for x in xs): a generator expression as the only argument.
                value = comprehension(std::move(value), nullptr, 0);
            }
            args.push_back(std::move(value));
        }
        if (!is(Tok::RParen))
            expect(Tok::Comma, "','");
    }
    advance();

    // super().m(args) / super().__init__(args) / super(C, self).m(args)
    if (callee->kind == NK::Member && callee->a->kind == NK::Call &&
        callee->a->a->kind == NK::Identifier && callee->a->a->text == ascii("super")) {
        if (callee->text == ascii("__init__")) {
            NodePtr n = make(NK::Call, at);
            n->a = make(NK::SuperExpr, at);
            n->list = std::move(args);
            return n;
        }
        callee->a = make(NK::SuperExpr, at);
    }
    // Base.__init__(self, args) and Base.method(self, args)
    if (callee->kind == NK::Member && callee->a->kind == NK::Identifier && !args.empty()) {
        const Scope* cls = classScope();
        if (cls && !cls->baseName.empty() && callee->a->text == cls->baseName &&
            args[0]->kind == NK::Identifier) {
            bool isSelf = false;
            for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
                if (!it->selfName.empty()) {
                    isSelf = it->selfName == args[0]->text;
                    break;
                }
            }
            if (isSelf) {
                args.erase(args.begin());
                if (callee->text == ascii("__init__")) {
                    NodePtr n = make(NK::Call, at);
                    n->a = make(NK::SuperExpr, at);
                    n->list = std::move(args);
                    return n;
                }
                callee->a = make(NK::SuperExpr, at);
            }
        }
    }

    NodePtr n;
    String calleeName;
    if (callee->kind == NK::Identifier) {
        calleeName = callee->text;
        const bool known = functions_.count(calleeName) || classes_.count(calleeName);
        if (isKnownClass(calleeName)) {
            n = make(NK::NewExpr, at);
            n->a = std::move(callee);
            n->list = std::move(args);
        } else if (!known && !calleeName.empty() && calleeName[0] >= u'A' &&
                   calleeName[0] <= u'Z' && !varScope().params.count(calleeName)) {
            // Capitalised and not defined here: a class from an earlier
            // evaluation, or a function. `__py.new` decides at run time.
            args.insert(args.begin(), std::move(callee));
            n = helper("new", std::move(args), at);
        }
    } else if (callee->kind == NK::Member && !callee->computed &&
               inList(kDispatchedMethods, callee->text)) {
        // obj.sort(...) -> __py.m_sort(obj, ...)
        const std::string helperName = "m_" + toUtf8(callee->text);
        args.insert(args.begin(), std::move(callee->a));
        n = helper(helperName.c_str(), std::move(args), at);
    }
    if (!n)
        n = call(std::move(callee), std::move(args), at);

    if (!keywords.empty()) {
        PendingCall pending;
        pending.call = n.get();
        pending.callee = calleeName;
        pending.keywords = std::move(keywords);
        pending.line = at;
        pendingCalls_.push_back(std::move(pending));
    }
    return n;
}

/// `a[i]` -> `__py.getitem(a, i)`; `a[lo:hi:step]` -> `__py.slice(a, lo, hi, step)`.
Parser::NodePtr Parser::subscript(NodePtr base, std::uint32_t at)
{
    expect(Tok::LBracket, "'['");
    NodePtr lo, hi, step;
    bool isSlice = false;
    if (!is(Tok::Colon))
        lo = expression();
    if (eat(Tok::Colon)) {
        isSlice = true;
        if (!is(Tok::Colon) && !is(Tok::RBracket))
            hi = expression();
        if (eat(Tok::Colon) && !is(Tok::RBracket))
            step = expression();
    }
    if (is(Tok::Comma))
        fail("multi-dimensional subscripts are not supported");
    expect(Tok::RBracket, "']'");
    std::vector<NodePtr> args;
    args.push_back(std::move(base));
    if (!isSlice) {
        args.push_back(std::move(lo));
        return helper("getitem", std::move(args), at);
    }
    args.push_back(lo ? std::move(lo) : undefinedValue(at));
    args.push_back(hi ? std::move(hi) : undefinedValue(at));
    args.push_back(step ? std::move(step) : undefinedValue(at));
    return helper("slice", std::move(args), at);
}

Parser::NodePtr Parser::atom()
{
    const std::uint32_t at = line();
    switch (type()) {
        case Tok::Name: {
            NodePtr n = make(NK::Identifier, at);
            n->text = advance().text;
            return n;
        }
        case Tok::Number: return number(advance().number, at);
        case Tok::String:
        case Tok::FString: return stringAtom();
        case Tok::True:
        case Tok::False: {
            NodePtr n = make(NK::BoolLit, at);
            n->number = advance().type == Tok::True ? 1 : 0;
            return n;
        }
        case Tok::None: advance(); return make(NK::NullLit, at);
        case Tok::LParen: return parenthesized();
        case Tok::LBracket: return listDisplay();
        case Tok::LBrace: return dictOrSetDisplay();
        case Tok::Ellipsis: advance(); return undefinedValue(at);
        case Tok::Lambda: return lambda();
        case Tok::Yield: fail("generators ('yield') are not supported");
        default: fail(std::string("unexpected ") + tokenName(type()));
    }
}

/// Adjacent literals concatenate; any f-string among them makes the whole
/// thing a template.
Parser::NodePtr Parser::stringAtom()
{
    const std::uint32_t at = line();
    NodePtr result;
    while (is(Tok::String) || is(Tok::FString)) {
        const Token& t = advance();
        NodePtr piece;
        if (t.type == Tok::String) {
            piece = string(t.text, at);
        } else {
            piece = fstring(t);
        }
        if (!result) {
            result = std::move(piece);
            continue;
        }
        if (result->kind == NK::StringLit && piece->kind == NK::StringLit) {
            result->text += piece->text;
            continue;
        }
        // Merge into one template.
        if (result->kind == NK::StringLit) {
            NodePtr tpl = make(NK::TemplateLit, at);
            tpl->strs.push_back(result->text);
            result = std::move(tpl);
        }
        if (piece->kind == NK::StringLit) {
            result->strs.back() += piece->text;
        } else {
            result->strs.back() += piece->strs.front();
            for (std::size_t i = 1; i < piece->strs.size(); ++i)
                result->strs.push_back(piece->strs[i]);
            for (NodePtr& e : piece->list)
                result->list.push_back(std::move(e));
        }
    }
    return result;
}

Parser::NodePtr Parser::fstring(const Token& token)
{
    NodePtr tpl = make(NK::TemplateLit, token.line);
    tpl->strs = token.parts;
    for (std::size_t i = 0; i < token.exprs.size(); ++i) {
        Parser sub(token.exprs[i], name_);
        for (Token& t : sub.toks_)
            t.line = token.line;
        sub.scopes_.push_back(Scope{});
        sub.scopes_.back().kind = Scope::Kind::Function;
        sub.tempCounter_ = tempCounter_;
        NodePtr value = sub.exprList();
        if (!sub.is(Tok::Newline) && !sub.is(Tok::Eof))
            fail(token.line, "invalid expression in f-string");
        tempCounter_ = sub.tempCounter_;
        for (const String& temp : sub.scopes_.back().declared)
            newTempNamed(temp);
        for (PendingCall& p : sub.pendingCalls_)
            pendingCalls_.push_back(std::move(p));
        const char conv = token.convs[i];
        std::vector<NodePtr> args;
        if (conv == 'r') {
            std::vector<NodePtr> inner;
            inner.push_back(std::move(value));
            value = helper("repr", std::move(inner), token.line);
        }
        args.push_back(std::move(value));
        if (!token.specs[i].empty()) {
            const std::string& spec = token.specs[i];
            if (spec.find('{') != std::string::npos) {
                // `{x:{width}.{prec}f}`: the spec is an f-string of its own.
                Lexer lexer(spec, name_);
                Token inner;
                inner.line = token.line;
                lexer.splitFString(spec, /*isRaw=*/true, inner);
                args.push_back(fstring(inner));
            } else {
                args.push_back(string(fromUtf8(spec), token.line));
            }
            tpl->list.push_back(helper("format", std::move(args), token.line));
        } else if (conv == 'r') {
            tpl->list.push_back(std::move(args[0]));
        } else {
            tpl->list.push_back(helper("str", std::move(args), token.line));
        }
    }
    return tpl;
}

Parser::NodePtr Parser::parenthesized()
{
    const std::uint32_t at = line();
    expect(Tok::LParen, "'('");
    if (eat(Tok::RParen))
        return make(NK::ArrayLit, at);  // the empty tuple
    if (is(Tok::Yield))
        fail("generators ('yield') are not supported");
    NodePtr first = is(Tok::Star) ? starred() : expression();
    if (is(Tok::For)) {
        NodePtr result = comprehension(std::move(first), nullptr, 0);
        expect(Tok::RParen, "')'");
        return result;
    }
    if (is(Tok::Walrus))
        fail("assignment expressions (':=') are not supported");
    if (!is(Tok::Comma)) {
        expect(Tok::RParen, "')'");
        return first;
    }
    NodePtr tuple = make(NK::ArrayLit, at);
    tuple->list.push_back(std::move(first));
    while (eat(Tok::Comma)) {
        if (is(Tok::RParen))
            break;
        tuple->list.push_back(is(Tok::Star) ? starred() : expression());
    }
    expect(Tok::RParen, "')'");
    return tuple;
}

Parser::NodePtr Parser::listDisplay()
{
    const std::uint32_t at = line();
    expect(Tok::LBracket, "'['");
    NodePtr list = make(NK::ArrayLit, at);
    if (eat(Tok::RBracket))
        return list;
    NodePtr first = is(Tok::Star) ? starred() : expression();
    if (is(Tok::For)) {
        NodePtr result = comprehension(std::move(first), nullptr, 1);
        expect(Tok::RBracket, "']'");
        return result;
    }
    list->list.push_back(std::move(first));
    while (eat(Tok::Comma)) {
        if (is(Tok::RBracket))
            break;
        list->list.push_back(is(Tok::Star) ? starred() : expression());
    }
    expect(Tok::RBracket, "']'");
    return list;
}

/// `{}` and `{k: v}` are Maps; `{a, b}` is a Set; either may be a comprehension.
Parser::NodePtr Parser::dictOrSetDisplay()
{
    const std::uint32_t at = line();
    expect(Tok::LBrace, "'{'");
    if (eat(Tok::RBrace)) {
        NodePtr n = make(NK::NewExpr, at);
        n->a = ident(ascii("Map"), at);
        return n;
    }
    if (is(Tok::StarStar))
        fail("'**' in a dict display is not supported");
    NodePtr first = is(Tok::Star) ? starred() : expression();
    if (eat(Tok::Colon)) {
        NodePtr value = expression();
        if (is(Tok::For)) {
            NodePtr result = comprehension(std::move(first), std::move(value), 2);
            expect(Tok::RBrace, "'}'");
            return result;
        }
        NodePtr entries = make(NK::ArrayLit, at);
        auto pair = [&](NodePtr k, NodePtr v) {
            NodePtr p = make(NK::ArrayLit, at);
            p->list.push_back(std::move(k));
            p->list.push_back(std::move(v));
            entries->list.push_back(std::move(p));
        };
        pair(std::move(first), std::move(value));
        while (eat(Tok::Comma)) {
            if (is(Tok::RBrace))
                break;
            if (is(Tok::StarStar))
                fail("'**' in a dict display is not supported");
            NodePtr k = expression();
            expect(Tok::Colon, "':'");
            pair(std::move(k), expression());
        }
        expect(Tok::RBrace, "'}'");
        NodePtr n = make(NK::NewExpr, at);
        n->a = ident(ascii("Map"), at);
        n->list.push_back(std::move(entries));
        return n;
    }
    if (is(Tok::For)) {
        NodePtr result = comprehension(std::move(first), nullptr, 3);
        expect(Tok::RBrace, "'}'");
        return result;
    }
    NodePtr items = make(NK::ArrayLit, at);
    items->list.push_back(std::move(first));
    while (eat(Tok::Comma)) {
        if (is(Tok::RBrace))
            break;
        items->list.push_back(is(Tok::Star) ? starred() : expression());
    }
    expect(Tok::RBrace, "'}'");
    NodePtr n = make(NK::NewExpr, at);
    n->a = ident(ascii("Set"), at);
    n->list.push_back(std::move(items));
    return n;
}

/// The `for x in xs if c ...` tail of a comprehension, wrapped as an
/// immediately invoked arrow function that fills and returns the result.
/// `kind`: 0/1 list, 2 dict, 3 set.
Parser::NodePtr Parser::comprehension(NodePtr element, NodePtr value, int kind)
{
    const std::uint32_t at = line();
    const String result = ascii(" r");
    scopes_.push_back(Scope{});
    scope().kind = Scope::Kind::Function;
    scope().holdsTemps = false;

    struct Clause
    {
        NodePtr target;
        NodePtr iterable;
        std::vector<NodePtr> conditions;
    };
    std::vector<Clause> clauses;
    std::vector<String> loopVars;
    while (is(Tok::For)) {
        advance();
        Clause c;
        NodePtr target = exprListNoCond();
        expect(Tok::In, "'in'");
        c.target = toTarget(std::move(target), false);
        collectNames(*c.target, loopVars);
        c.iterable = orTest();
        while (is(Tok::If)) {
            advance();
            c.conditions.push_back(truth(orTest()));
        }
        clauses.push_back(std::move(c));
    }
    if (is(Tok::Async))
        fail("'async' is not supported");

    // Innermost: r.push(element) / r.set(k, v) / r.add(element).
    NodePtr innermost;
    {
        std::vector<NodePtr> args;
        const char* method = kind == 2 ? "set" : kind == 3 ? "add" : "push";
        args.push_back(std::move(element));
        if (kind == 2)
            args.push_back(std::move(value));
        innermost = exprStatement(call(member(ident(result, at), ascii(method), at), std::move(args), at));
    }
    NodePtr body = std::move(innermost);
    for (auto it = clauses.rbegin(); it != clauses.rend(); ++it) {
        for (auto c = it->conditions.rbegin(); c != it->conditions.rend(); ++c) {
            NodePtr test = make(NK::If, at);
            test->a = std::move(*c);
            test->b = std::move(body);
            body = std::move(test);
        }
        // One binding per variable for the whole comprehension, as Python has
        // it: a lambda made inside sees the final value.
        NodePtr loop = make(NK::ForOf, at);
        loop->varKind = VarKind::None;
        loop->a = std::move(it->target);
        std::vector<NodePtr> args;
        args.push_back(std::move(it->iterable));
        loop->b = helper("iter", std::move(args), at);
        loop->d = std::move(body);
        body = std::move(loop);
    }
    scopes_.pop_back();

    NodePtr init;
    if (kind == 2 || kind == 3) {
        init = make(NK::NewExpr, at);
        init->a = ident(ascii(kind == 2 ? "Map" : "Set"), at);
    } else {
        init = make(NK::ArrayLit, at);
    }
    std::vector<NodePtr> statements;
    statements.push_back(constDecl(result, std::move(init), at));
    if (!loopVars.empty()) {
        NodePtr decl = make(NK::VarDecl, at);
        decl->varKind = VarKind::Let;
        for (const String& name : loopVars) {
            NodePtr d = make(NK::Declarator, at);
            d->a = ident(name, at);
            decl->list.push_back(std::move(d));
        }
        statements.push_back(std::move(decl));
    }
    statements.push_back(std::move(body));
    NodePtr ret = make(NK::ReturnStmt, at);
    ret->a = ident(result, at);
    statements.push_back(std::move(ret));

    NodePtr fn = make(NK::ArrowExpr, at);
    fn->d = blockOf(std::move(statements), at);
    return call(std::move(fn), {}, at);
}

// -------------------------------------------------------- lowering helpers ---

Parser::NodePtr Parser::ident(StringView name, std::uint32_t at)
{
    NodePtr n = make(NK::Identifier, at);
    n->text = String(name);
    return n;
}

Parser::NodePtr Parser::member(NodePtr object, StringView name, std::uint32_t at)
{
    NodePtr n = make(NK::Member, at);
    n->a = std::move(object);
    n->text = String(name);
    return n;
}

Parser::NodePtr Parser::call(NodePtr callee, std::vector<NodePtr> args, std::uint32_t at)
{
    NodePtr n = make(NK::Call, at);
    n->a = std::move(callee);
    n->list = std::move(args);
    return n;
}

Parser::NodePtr Parser::helper(const char* name, std::vector<NodePtr> args, std::uint32_t at)
{
    return call(member(ident(ascii("__py"), at), ascii(name), at), std::move(args), at);
}

Parser::NodePtr Parser::number(double value, std::uint32_t at)
{
    NodePtr n = make(NK::NumberLit, at);
    n->number = value;
    return n;
}

Parser::NodePtr Parser::string(const String& text, std::uint32_t at)
{
    NodePtr n = make(NK::StringLit, at);
    n->text = text;
    return n;
}

Parser::NodePtr Parser::undefinedValue(std::uint32_t at)
{
    return ident(ascii("undefined"), at);
}

Parser::NodePtr Parser::binary(JTok op, NodePtr a, NodePtr b, std::uint32_t at)
{
    NodePtr n = make(NK::Binary, at);
    n->op = op;
    n->a = std::move(a);
    n->b = std::move(b);
    return n;
}

Parser::NodePtr Parser::logical(JTok op, NodePtr a, NodePtr b, std::uint32_t at)
{
    NodePtr n = make(NK::Logical, at);
    n->op = op;
    n->a = std::move(a);
    n->b = std::move(b);
    return n;
}

Parser::NodePtr Parser::unary(JTok op, NodePtr a, std::uint32_t at)
{
    NodePtr n = make(NK::Unary, at);
    n->op = op;
    n->a = std::move(a);
    return n;
}

Parser::NodePtr Parser::assign(NodePtr target, NodePtr value, std::uint32_t at)
{
    NodePtr n = make(NK::Assign, at);
    n->op = JTok::Assign;
    n->a = std::move(target);
    n->b = std::move(value);
    return n;
}

Parser::NodePtr Parser::exprStatement(NodePtr e)
{
    NodePtr n = make(NK::ExprStatement, e->line);
    n->a = std::move(e);
    return n;
}

Parser::NodePtr Parser::blockOf(std::vector<NodePtr> statements, std::uint32_t at)
{
    NodePtr n = make(NK::Block, at);
    n->list = std::move(statements);
    return n;
}

Parser::NodePtr Parser::constDecl(const String& name, NodePtr init, std::uint32_t at)
{
    NodePtr decl = make(NK::VarDecl, at);
    decl->varKind = VarKind::Const;
    NodePtr d = make(NK::Declarator, at);
    d->a = ident(name, at);
    d->b = std::move(init);
    decl->list.push_back(std::move(d));
    return decl;
}

Parser::NodePtr Parser::truth(NodePtr e)
{
    if (isBoolish(*e))
        return e;
    const std::uint32_t at = e->line;
    std::vector<NodePtr> args;
    args.push_back(std::move(e));
    return helper("truthy", std::move(args), at);
}

bool Parser::isBoolish(const Node& n)
{
    switch (n.kind) {
        case NK::BoolLit: return true;
        case NK::Binary:
            return n.op == JTok::Lt || n.op == JTok::Gt || n.op == JTok::Le || n.op == JTok::Ge ||
                   n.op == JTok::EqEq || n.op == JTok::NotEq || n.op == JTok::EqEqEq ||
                   n.op == JTok::NotEqEq;
        case NK::Logical: return isBoolish(*n.a) && isBoolish(*n.b);
        case NK::Unary: return n.op == JTok::Bang;
        case NK::Call:
            if (n.a->kind == NK::Member && n.a->a->kind == NK::Identifier &&
                n.a->a->text == ascii("__py")) {
                const String& h = n.a->text;
                return h == ascii("eq") || h == ascii("contains") || h == ascii("truthy") ||
                       h == ascii("isinstance");
            }
            if (n.a->kind == NK::Identifier)
                return n.a->text == ascii("isinstance") || n.a->text == ascii("bool") ||
                       n.a->text == ascii("any") || n.a->text == ascii("all") ||
                       n.a->text == ascii("hasattr") || n.a->text == ascii("callable");
            return false;
        default: return false;
    }
}

bool Parser::isLiteral(const Node& n)
{
    return n.kind == NK::NumberLit || n.kind == NK::StringLit || n.kind == NK::BoolLit ||
           n.kind == NK::NullLit;
}

bool Parser::isNoneLiteral(const Node& n)
{
    return n.kind == NK::NullLit;
}

Parser::NodePtr Parser::clone(const Node& n)
{
    NodePtr c = make(n.kind, n.line);
    c->op = n.op;
    c->varKind = n.varKind;
    c->propKind = n.propKind;
    c->computed = n.computed;
    c->isStatic = n.isStatic;
    c->prefix = n.prefix;
    c->optional = n.optional;
    c->namedExpr = n.namedExpr;
    c->text = n.text;
    c->number = n.number;
    c->strs = n.strs;
    if (n.a)
        c->a = clone(*n.a);
    if (n.b)
        c->b = clone(*n.b);
    if (n.c)
        c->c = clone(*n.c);
    if (n.d)
        c->d = clone(*n.d);
    for (const NodePtr& child : n.list)
        c->list.push_back(child ? clone(*child) : nullptr);
    return c;
}

bool Parser::containsSuperCall(const Node& n)
{
    if (n.kind == NK::Call && n.a && n.a->kind == NK::SuperExpr)
        return true;
    if (n.kind == NK::FunctionExpr || n.kind == NK::ClassExpr)
        return false;
    for (const Node* child : {n.a.get(), n.b.get(), n.c.get(), n.d.get()}) {
        if (child && containsSuperCall(*child))
            return true;
    }
    for (const NodePtr& child : n.list) {
        if (child && containsSuperCall(*child))
            return true;
    }
    return false;
}

void Parser::collectNames(const Node& target, std::vector<String>& out)
{
    switch (target.kind) {
        case NK::Identifier:
            if (std::find(out.begin(), out.end(), target.text) == out.end())
                out.push_back(target.text);
            return;
        case NK::ArrayPattern:
            for (const NodePtr& element : target.list)
                collectNames(*element, out);
            return;
        case NK::RestElement: collectNames(*target.a, out); return;
        default: return;
    }
}

bool Parser::isKnownClass(StringView name) const
{
    return classes_.count(String(name)) || inList(kBuiltinClasses, name);
}

// -------------------------------------------------- keyword arguments ---

/// `f(a, k=v)`: for a function defined in this module, the keyword is placed
/// at its parameter's position (with `undefined` filling any gap, which lets
/// the default apply). Otherwise the keywords travel as a trailing `__py.kw`
/// object, which the built-in functions understand.
void Parser::resolveKeywordCalls()
{
    for (PendingCall& pending : pendingCalls_) {
        Node& call = *pending.call;
        const Signature* sig = nullptr;
        if (!pending.callee.empty()) {
            auto it = functions_.find(pending.callee);
            if (it != functions_.end() && !it->second.variadic)
                sig = &it->second;
        }
        bool placed = false;
        if (sig) {
            bool ok = true;
            for (const NodePtr& a : call.list)
                ok = ok && a->kind != NK::Spread;
            std::vector<NodePtr> positional;
            for (NodePtr& a : call.list)
                positional.push_back(std::move(a));
            for (auto& kw : pending.keywords) {
                std::size_t index = sig->params.size();
                for (std::size_t i = 0; i < sig->params.size(); ++i) {
                    if (sig->params[i] == kw.first)
                        index = i;
                }
                if (index == sig->params.size() || index < call.list.size()) {
                    ok = false;
                    break;
                }
                while (positional.size() <= index)
                    positional.push_back(nullptr);
                if (positional[index]) {
                    ok = false;
                    break;
                }
                positional[index] = std::move(kw.second);
            }
            if (ok) {
                for (NodePtr& p : positional) {
                    if (!p)
                        p = undefinedValue(pending.line);
                }
                call.list = std::move(positional);
                placed = true;
            } else {
                // Put things back for the fallback.
                call.list.clear();
                for (NodePtr& p : positional) {
                    if (p)
                        call.list.push_back(std::move(p));
                }
                for (auto& kw : pending.keywords) {
                    if (!kw.second) {
                        fail(pending.line, "keyword argument '" + toUtf8(kw.first) +
                                               "' does not match a parameter of '" +
                                               toUtf8(pending.callee) + "'");
                    }
                }
            }
        }
        if (!placed) {
            NodePtr object = make(NK::ObjectLit, pending.line);
            for (auto& kw : pending.keywords) {
                NodePtr p = make(NK::Property, pending.line);
                p->text = kw.first;
                p->b = std::move(kw.second);
                object->list.push_back(std::move(p));
            }
            std::vector<NodePtr> args;
            args.push_back(std::move(object));
            call.list.push_back(helper("kw", std::move(args), pending.line));
        }
    }
    pendingCalls_.clear();
}

}  // namespace python
}  // namespace internal
}  // namespace script
