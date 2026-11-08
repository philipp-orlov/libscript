// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Recursive-descent parser.

#include <algorithm>
#include <utility>

#include "script/internal/dyn/parser.hpp"
#include "script/internal/dyn/bigint.hpp"
#include "script/internal/dyn/numconv.hpp"
#include "script/internal/dyn/regexp.hpp"
#include "script/internal/stack_guard.hpp"

namespace script {
namespace internal {
namespace dyn {

namespace {

/// A `/` right after one of these is division; anywhere else it starts a
/// regular expression literal.
bool operandJustEnded(Tok t) noexcept
{
    switch (t) {
        case Tok::Identifier:
        case Tok::Number:
        case Tok::BigIntLit:
        case Tok::StringLit:
        case Tok::Template:
        case Tok::Regex:
        case Tok::RParen:
        case Tok::RBracket:
        case Tok::RBrace:
        case Tok::This:
        case Tok::Super:
        case Tok::True:
        case Tok::False:
        case Tok::Null:
        case Tok::PlusPlus:
        case Tok::MinusMinus: return true;
        default: return false;
    }
}

/// Binding power of a binary operator, or 0 when it is not one.
int precedenceOf(Tok t) noexcept
{
    switch (t) {
        case Tok::QuestionQuestion: return 1;
        case Tok::OrOr: return 2;
        case Tok::AndAnd: return 3;
        case Tok::Pipe: return 4;
        case Tok::Caret: return 5;
        case Tok::Amp: return 6;
        case Tok::EqEq:
        case Tok::NotEq:
        case Tok::EqEqEq:
        case Tok::NotEqEq: return 7;
        case Tok::Lt:
        case Tok::Gt:
        case Tok::Le:
        case Tok::Ge:
        case Tok::In:
        case Tok::InstanceOf: return 8;
        case Tok::Shl:
        case Tok::Shr:
        case Tok::UShr: return 9;
        case Tok::Plus:
        case Tok::Minus: return 10;
        case Tok::Star:
        case Tok::Slash:
        case Tok::Percent: return 11;
        case Tok::StarStar: return 12;
        default: return 0;
    }
}

/// Where `as` and `satisfies` sit: with the relational operators.
constexpr int kRelationalPrecedence = 8;

/// Clears a parser flag for the lifetime of a nested construct and puts it
/// back afterwards. Used to let `in` mean the operator again inside brackets
/// while a `for (... in ...)` head is being parsed.
struct FlagReset
{
    bool& flag;
    bool saved;
    explicit FlagReset(bool& f) noexcept : flag(f), saved(f) { flag = false; }
    ~FlagReset() { flag = saved; }
};

/// Tokens that can never appear inside a type argument list. Meeting one means
/// the `<` that started the scan was a less-than after all.
bool typeTokenRejected(Tok t) noexcept
{
    switch (t) {
        case Tok::Plus:
        case Tok::Star:
        case Tok::Slash:
        case Tok::Percent:
        case Tok::StarStar:
        case Tok::PlusPlus:
        case Tok::MinusMinus:
        case Tok::AndAnd:
        case Tok::OrOr:
        case Tok::QuestionQuestion:
        case Tok::EqEq:
        case Tok::NotEq:
        case Tok::EqEqEq:
        case Tok::NotEqEq:
        case Tok::Le:
        case Tok::Ge:
        case Tok::Bang:
        case Tok::Tilde:
        case Tok::Caret:
        case Tok::Regex:
        case Tok::Template: return true;
        default: return false;
    }
}

/// Scan a probe lexer from just past a `<` to the matching `>`. Brackets are
/// tracked so that `<T extends (a: number) => void>` stays balanced, and `>>`
/// and `>>>` close two and three levels because that is how the lexer hands
/// nested generics over.
bool scanPastAngles(Lexer& probe)
{
    int angle = 1;
    int bracket = 0;
    for (int guard = 0; guard < 4096; ++guard) {
        const Token t = probe.next(false);
        switch (t.type) {
            case Tok::LParen:
            case Tok::LBracket:
            case Tok::LBrace: ++bracket; continue;
            case Tok::RParen:
            case Tok::RBracket:
            case Tok::RBrace:
                if (--bracket < 0)
                    return false;
                continue;
            case Tok::Eof:
            case Tok::Semicolon: return false;
            default: break;
        }
        if (bracket > 0)
            continue;
        switch (t.type) {
            case Tok::Lt: ++angle; break;
            case Tok::Gt:
                if (--angle == 0)
                    return true;
                break;
            case Tok::Shr:
                angle -= 2;
                if (angle <= 0)
                    return angle == 0;
                break;
            case Tok::UShr:
                angle -= 3;
                if (angle <= 0)
                    return angle == 0;
                break;
            default:
                if (typeTokenRejected(t.type))
                    return false;
                break;
        }
    }
    return false;
}

/// Skip over a type on a probe lexer and report the token that ended it. Used
/// to tell `(a): T => ...` from a parenthesised expression.
Tok scanPastType(Lexer& probe)
{
    int depth = 0;
    for (int guard = 0; guard < 4096; ++guard) {
        const Token t = probe.next(false);
        if (t.type == Tok::Eof)
            return Tok::Eof;
        switch (t.type) {
            case Tok::LParen:
            case Tok::LBracket:
            case Tok::LBrace:
            case Tok::Lt: ++depth; break;
            case Tok::RParen:
            case Tok::RBracket:
            case Tok::RBrace:
            case Tok::Gt:
                if (depth == 0)
                    return t.type;
                --depth;
                break;
            case Tok::Shr:
                depth -= 2;
                if (depth < 0)
                    return t.type;
                break;
            case Tok::Arrow:
            case Tok::Comma:
            case Tok::Semicolon:
            case Tok::Assign:
            case Tok::Colon:
                if (depth == 0)
                    return t.type;
                break;
            default: break;
        }
    }
    return Tok::Eof;
}

bool isAssignOp(Tok t) noexcept
{
    switch (t) {
        case Tok::Assign:
        case Tok::PlusAssign:
        case Tok::MinusAssign:
        case Tok::StarAssign:
        case Tok::SlashAssign:
        case Tok::PercentAssign:
        case Tok::StarStarAssign:
        case Tok::ShlAssign:
        case Tok::ShrAssign:
        case Tok::UShrAssign:
        case Tok::AmpAssign:
        case Tok::PipeAssign:
        case Tok::CaretAssign:
        case Tok::AndAssign:
        case Tok::OrAssign:
        case Tok::NullishAssign: return true;
        default: return false;
    }
}

}  // namespace

Parser::Parser(std::string_view source, std::string sourceName, bool typescript)
    : lexer_(source, sourceName), name_(std::move(sourceName)), source_(source),
      typescript_(typescript)
{
    lookaheadLimit_ = 32 * source.size() + 100000;
    cur_ = lexer_.next(false);
}

void Parser::advance()
{
    cur_ = lexer_.next(false);
}

bool Parser::eat(Tok t)
{
    if (cur_.type != t)
        return false;
    advance();
    return true;
}

void Parser::expect(Tok t, const char* what)
{
    if (cur_.type != t)
        fail(std::string("expected ") + what + " but found " + tokenName(cur_.type));
    advance();
}

void Parser::fail(const std::string& message) const
{
    Lexer::fail(name_, cur_.line, message);
}

void Parser::closeAngle()
{
    switch (cur_.type) {
        case Tok::Gt: advance(); return;
        case Tok::Shr: cur_.type = Tok::Gt; return;
        case Tok::UShr: cur_.type = Tok::Shr; return;
        case Tok::Ge: cur_.type = Tok::Assign; return;
        default: fail(std::string("expected '>' but found ") + tokenName(cur_.type));
    }
}

bool Parser::identifierLike(Tok t) noexcept
{
    switch (t) {
        case Tok::Identifier:
        case Tok::Of:
        case Tok::Get:
        case Tok::Set:
        case Tok::Static:
        case Tok::Async:
        case Tok::Await:
        case Tok::Let:
        case Tok::Yield:
        // Every TypeScript keyword but `enum` is contextual, so each one is
        // still a legal variable, parameter or property name.
        case Tok::Interface:
        case Tok::Type:
        case Tok::Implements:
        case Tok::Declare:
        case Tok::Namespace:
        case Tok::Abstract:
        case Tok::Readonly:
        case Tok::KeyOf:
        case Tok::As:
        case Tok::Satisfies:
        case Tok::Is:
        case Tok::Public:
        case Tok::Private:
        case Tok::Protected:
        case Tok::Override:
        case Tok::Export: return true;
        default: return false;
    }
}

/// `type`, `interface` and friends open a declaration only when the next token
/// could be the name being declared. Everywhere else they are just names.
bool Parser::startsDeclaration(Tok keyword) const
{
    if (!typescript_ || cur_.type != keyword)
        return false;
    Lexer probe = lexer_;
    const Token after = probe.next(false);
    switch (keyword) {
        case Tok::Declare:
        case Tok::Export:
            switch (after.type) {
                case Tok::Var:
                case Tok::Let:
                case Tok::Const:
                case Tok::Function:
                case Tok::Class:
                case Tok::Enum:
                case Tok::Abstract:
                case Tok::Namespace:
                case Tok::Interface:
                case Tok::Type: return !after.newlineBefore;
                case Tok::Declare: return keyword == Tok::Export && !after.newlineBefore;
                default: return false;
            }
        case Tok::Abstract: return after.type == Tok::Class && !after.newlineBefore;
        default: return identifierLike(after.type) && !after.newlineBefore;
    }
}

String Parser::propertyName()
{
    if (cur_.type == Tok::StringLit || cur_.type == Tok::Number || !cur_.text.empty()) {
        String name = cur_.text;
        if (cur_.type == Tok::Number)
            name = numberToString(cur_.number);
        else if (cur_.type == Tok::BigIntLit)
            name = bigint::literalToDecimal(cur_.text);
        advance();
        return name;
    }
    fail("expected a property name");
}

void Parser::semicolon()
{
    if (eat(Tok::Semicolon))
        return;
    if (at(Tok::RBrace) || at(Tok::Eof) || cur_.newlineBefore)
        return;
    fail(std::string("expected ';' but found ") + tokenName(cur_.type));
}

// ------------------------------------------------------------- program ---

NodePtr Parser::parseProgram()
{
    NodePtr program = makeNode(NK::Program, 1);
    while (!at(Tok::Eof))
        program->list.push_back(statement());
    return program;
}

NodePtr Parser::statement()
{
    if (++depth_ > 400)
        fail("source is nested too deeply");
    struct Pop
    {
        std::size_t* d;
        ~Pop() { --*d; }
    } pop{&depth_};

    const std::uint32_t line = cur_.line;
    switch (cur_.type) {
        case Tok::LBrace: return block();
        case Tok::Semicolon: advance(); return makeNode(NK::Empty, line);
        case Tok::Var: advance(); return varStatement(VarKind::Var);
        case Tok::Let: advance(); return varStatement(VarKind::Let);
        case Tok::Const: {
            Lexer probe = lexer_;
            if (typescript_ && probe.next(false).type == Tok::Enum) {
                advance();
                return enumDeclaration(true);
            }
            advance();
            return varStatement(VarKind::Const);
        }
        case Tok::If: return ifStatement();
        case Tok::For: return forStatement();
        case Tok::While: return whileStatement();
        case Tok::Do: return doStatement();
        case Tok::Try: return tryStatement();
        case Tok::Switch: return switchStatement();
        case Tok::Function: return functionDeclaration();
        case Tok::Class: return classDeclaration(false);
        case Tok::Enum:
            if (!typescript_)
                fail("'enum' is TypeScript syntax");
            return enumDeclaration(false);
        case Tok::Async:
            if (asyncFunctionAhead())
                return functionDeclaration();
            break;
        case Tok::With: fail("'with' is not supported"); break;
        case Tok::Debugger:
            advance();
            semicolon();
            return makeNode(NK::Empty, line);
        case Tok::Return: {
            advance();
            NodePtr n = makeNode(NK::ReturnStmt, line);
            if (!at(Tok::Semicolon) && !at(Tok::RBrace) && !at(Tok::Eof) && !cur_.newlineBefore)
                n->a = expression();
            semicolon();
            return n;
        }
        case Tok::Break:
        case Tok::Continue: {
            const bool isBreak = at(Tok::Break);
            advance();
            NodePtr n = makeNode(isBreak ? NK::BreakStmt : NK::ContinueStmt, line);
            if (identifierLike(cur_.type) && !cur_.newlineBefore) {
                n->text = cur_.text;
                advance();
            }
            semicolon();
            return n;
        }
        case Tok::Throw: {
            advance();
            if (cur_.newlineBefore)
                fail("no line break is allowed after 'throw'");
            NodePtr n = makeNode(NK::ThrowStmt, line);
            n->a = expression();
            semicolon();
            return n;
        }
        default: break;
    }

    // Contextual keywords that open a declaration when a name follows.
    if (startsDeclaration(Tok::Export)) {
        // There is no module system, so `export` says only that a namespace
        // makes this name visible on the object it becomes.
        advance();
        NodePtr n = statement();
        n->exported = true;
        return n;
    }
    if (startsDeclaration(Tok::Type))
        return typeAliasDeclaration();
    if (startsDeclaration(Tok::Interface))
        return interfaceDeclaration();
    if (startsDeclaration(Tok::Namespace))
        return namespaceDeclaration();
    if (startsDeclaration(Tok::Declare))
        return ambientDeclaration();
    if (startsDeclaration(Tok::Abstract)) {
        advance();
        return classDeclaration(true);
    }

    // A labelled statement is an identifier followed by ':'.
    if (identifierLike(cur_.type)) {
        Lexer probe = lexer_;
        const Token after = probe.next(false);
        if (after.type == Tok::Colon) {
            checkReference();
            NodePtr n = makeNode(NK::Labeled, line);
            n->text = cur_.text;
            advance();
            advance();
            n->d = bodyStatement(true);
            return n;
        }
    }

    NodePtr n = makeNode(NK::ExprStatement, line);
    n->a = expression();
    semicolon();
    return n;
}

NodePtr Parser::block()
{
    NodePtr n = makeNode(NK::Block, cur_.line);
    expect(Tok::LBrace, "'{'");
    while (!at(Tok::RBrace)) {
        if (at(Tok::Eof))
            fail("unterminated block");
        n->list.push_back(statement());
    }
    advance();
    return n;
}

NodePtr Parser::declarator(NodePtr target, VarKind kind)
{
    NodePtr d = makeNode(NK::Declarator, target->line);
    // `let x!: T` only suppresses a definite-assignment check we do not make.
    if (typescript_ && at(Tok::Bang) && !cur_.newlineBefore)
        advance();
    d->type = typeAnnotation();
    d->a = std::move(target);
    if (eat(Tok::Assign))
        d->b = assignment();
    else if (kind == VarKind::Const && !d->type && !ambient_)
        fail("a 'const' declaration must have an initialiser");
    return d;
}

NodePtr Parser::varStatement(VarKind kind)
{
    NodePtr n = makeNode(NK::VarDecl, cur_.line);
    n->varKind = kind;
    n->declared = ambient_;
    do
        n->list.push_back(declarator(bindingTarget(), kind));
    while (eat(Tok::Comma));
    semicolon();
    return n;
}

NodePtr Parser::ifStatement()
{
    NodePtr n = makeNode(NK::If, cur_.line);
    advance();
    expect(Tok::LParen, "'('");
    n->a = expression();
    expect(Tok::RParen, "')'");
    n->b = bodyStatement(true);
    if (eat(Tok::Else))
        n->c = bodyStatement(true);
    return n;
}

NodePtr Parser::whileStatement()
{
    NodePtr n = makeNode(NK::While, cur_.line);
    advance();
    expect(Tok::LParen, "'('");
    n->a = expression();
    expect(Tok::RParen, "')'");
    n->d = bodyStatement(false);
    return n;
}

NodePtr Parser::doStatement()
{
    NodePtr n = makeNode(NK::DoWhile, cur_.line);
    advance();
    n->d = bodyStatement(false);
    expect(Tok::While, "'while'");
    expect(Tok::LParen, "'('");
    n->a = expression();
    expect(Tok::RParen, "')'");
    eat(Tok::Semicolon);
    return n;
}

NodePtr Parser::forStatement()
{
    const std::uint32_t line = cur_.line;
    advance();
    const bool forAwait = at(Tok::Await) && inAsync_;
    if (forAwait)
        advance();
    expect(Tok::LParen, "'('");

    NodePtr init;
    VarKind kind = VarKind::None;
    if (at(Tok::Var) || at(Tok::Let) || at(Tok::Const)) {
        kind = at(Tok::Var) ? VarKind::Var : at(Tok::Let) ? VarKind::Let : VarKind::Const;
        advance();
        NodePtr target = bindingTarget();
        NodePtr annotation = typeAnnotation();
        if (at(Tok::In) || at(Tok::Of)) {
            const bool isOf = at(Tok::Of);
            advance();
            NodePtr n = makeNode(isOf ? NK::ForOf : NK::ForIn, line);
            n->isAsync = forAwait;
            n->varKind = kind;
            n->type = std::move(annotation);
            n->a = std::move(target);
            n->b = isOf ? assignment() : expression();
            expect(Tok::RParen, "')'");
            n->d = bodyStatement(false);
            return n;
        }
        // Not a for-in/of: the target starts an ordinary declaration list.
        init = makeNode(NK::VarDecl, line);
        init->varKind = kind;
        NodePtr first = declarator(std::move(target), kind);
        if (annotation)
            first->type = std::move(annotation);
        init->list.push_back(std::move(first));
        while (eat(Tok::Comma))
            init->list.push_back(declarator(bindingTarget(), kind));
    } else if (!at(Tok::Semicolon)) {
        // `for (x in o)`: the head must stop at `in` instead of parsing
        // `x in o` as one expression.
        noIn_ = true;
        NodePtr head = expression();
        noIn_ = false;
        if (at(Tok::In) || at(Tok::Of)) {
            const bool isOf = at(Tok::Of);
            advance();
            NodePtr n = makeNode(isOf ? NK::ForOf : NK::ForIn, line);
            n->isAsync = forAwait;
            n->varKind = VarKind::None;
            n->a = toPattern(std::move(head));
            n->b = isOf ? assignment() : expression();
            expect(Tok::RParen, "')'");
            n->d = bodyStatement(false);
            return n;
        }
        init = makeNode(NK::ExprStatement, line);
        init->a = std::move(head);
    }

    NodePtr n = makeNode(NK::For, line);
    n->varKind = kind;
    n->a = std::move(init);
    expect(Tok::Semicolon, "';'");
    if (!at(Tok::Semicolon))
        n->b = expression();
    expect(Tok::Semicolon, "';'");
    if (!at(Tok::RParen))
        n->c = expression();
    expect(Tok::RParen, "')'");
    n->d = bodyStatement(false);
    return n;
}

NodePtr Parser::tryStatement()
{
    NodePtr n = makeNode(NK::Try, cur_.line);
    advance();
    n->a = block();
    if (eat(Tok::Catch)) {
        if (eat(Tok::LParen)) {
            n->b = bindingTarget();
            n->b->type = typeAnnotation();
            expect(Tok::RParen, "')'");
        }
        n->c = block();
    }
    if (eat(Tok::Finally))
        n->d = block();
    if (!n->c && !n->d)
        fail("'try' needs a 'catch' or a 'finally'");
    return n;
}

NodePtr Parser::switchStatement()
{
    NodePtr n = makeNode(NK::Switch, cur_.line);
    advance();
    expect(Tok::LParen, "'('");
    n->a = expression();
    expect(Tok::RParen, "')'");
    expect(Tok::LBrace, "'{'");
    bool sawDefault = false;
    while (!at(Tok::RBrace)) {
        if (at(Tok::Eof))
            fail("unterminated switch");
        NodePtr c = makeNode(NK::SwitchCase, cur_.line);
        if (eat(Tok::Case)) {
            c->a = expression();
        } else {
            expect(Tok::Default, "'case' or 'default'");
            if (sawDefault)
                fail("more than one 'default' clause");
            sawDefault = true;
        }
        expect(Tok::Colon, "':'");
        while (!at(Tok::Case) && !at(Tok::Default) && !at(Tok::RBrace))
            c->list.push_back(statement());
        n->list.push_back(std::move(c));
    }
    advance();
    return n;
}

bool Parser::asyncMethodAhead()
{
    Lexer probe = lexer_;
    const Token after = probe.next(false);
    if (after.newlineBefore)
        return false;
    switch (after.type) {
        case Tok::LParen:
        case Tok::Assign:
        case Tok::Semicolon:
        case Tok::RBrace:
        case Tok::Colon:
        case Tok::Question:
        case Tok::Comma:
        case Tok::Lt: return false;
        default: return true;
    }
}

bool Parser::asyncFunctionAhead()
{
    Lexer probe = lexer_;
    const Token after = probe.next(false);
    return after.type == Tok::Function && !after.newlineBefore;
}

bool Parser::asyncArrowAhead()
{
    Lexer probe = lexer_;
    const Token after = probe.next(false);
    if (after.newlineBefore)
        return false;
    if (identifierLike(after.type))
        return probe.next(false).type == Tok::Arrow;
    if (after.type != Tok::LParen)
        return false;
    // `async (...)` is a call unless an arrow follows the closing parenthesis.
    int depth = 1;
    while (depth > 0) {
        const Token t = probe.next(false);
        if (t.type == Tok::Eof)
            return false;
        if (t.type == Tok::LParen || t.type == Tok::LBracket || t.type == Tok::LBrace)
            ++depth;
        else if (t.type == Tok::RParen || t.type == Tok::RBracket || t.type == Tok::RBrace)
            --depth;
    }
    const Token next = probe.next(false);
    return next.type == Tok::Arrow || (typescript_ && next.type == Tok::Colon);
}

NodePtr Parser::functionDeclaration()
{
    const std::uint32_t line = cur_.line;
    NodePtr fn = functionExpression(true);
    NodePtr n = makeNode(NK::FunctionDecl, line);
    n->text = fn->text;
    n->declared = fn->declared;
    n->a = std::move(fn);
    return n;
}

NodePtr Parser::classDeclaration(bool isAbstract)
{
    const std::uint32_t line = cur_.line;
    NodePtr cls = classExpression(true, isAbstract);
    NodePtr n = makeNode(NK::ClassDecl, line);
    n->text = cls->text;
    n->declared = ambient_;
    n->a = std::move(cls);
    return n;
}

// --------------------------------------------- TypeScript declarations ---

NodePtr Parser::typeAliasDeclaration()
{
    NodePtr n = makeNode(NK::TypeAlias, cur_.line);
    advance();  // `type`
    n->text = cur_.text;
    advance();
    if (at(Tok::Lt))
        n->typeParams = typeParameterList();
    expect(Tok::Assign, "'='");
    n->type = typeExpr();
    semicolon();
    return n;
}

NodePtr Parser::interfaceDeclaration()
{
    NodePtr n = makeNode(NK::InterfaceDecl, cur_.line);
    advance();  // `interface`
    n->text = cur_.text;
    advance();
    if (at(Tok::Lt))
        n->typeParams = typeParameterList();
    if (at(Tok::Extends)) {
        advance();
        NodePtr heritage = makeNode(NK::HeritageClause, cur_.line);
        do
            heritage->list.push_back(primaryType());
        while (eat(Tok::Comma));
        n->c = std::move(heritage);
    }
    n->type = typeLiteral();
    return n;
}

NodePtr Parser::enumDeclaration(bool isConst)
{
    NodePtr n = makeNode(NK::EnumDecl, cur_.line);
    n->isStatic = isConst;
    n->declared = ambient_;
    expect(Tok::Enum, "'enum'");
    if (!identifierLike(cur_.type))
        fail("an enum declaration needs a name");
    n->text = cur_.text;
    advance();
    expect(Tok::LBrace, "'{'");
    while (!at(Tok::RBrace)) {
        if (at(Tok::Eof))
            fail("unterminated enum body");
        NodePtr member = makeNode(NK::EnumMember, cur_.line);
        member->text = propertyName();
        if (eat(Tok::Assign))
            member->a = assignment();
        n->list.push_back(std::move(member));
        if (!at(Tok::RBrace))
            expect(Tok::Comma, "','");
    }
    advance();
    return n;
}

NodePtr Parser::namespaceDeclaration()
{
    advance();  // `namespace`
    NodePtr outer = makeNode(NK::ModuleDecl, cur_.line);
    outer->declared = ambient_;
    if (!identifierLike(cur_.type))
        fail("a namespace declaration needs a name");
    outer->text = cur_.text;
    advance();

    // `namespace a.b {}` is shorthand for a namespace inside a namespace, and
    // the inner one is exported from the outer.
    Node* innermost = outer.get();
    while (eat(Tok::Dot)) {
        NodePtr inner = makeNode(NK::ModuleDecl, cur_.line);
        inner->exported = true;
        inner->declared = ambient_;
        inner->text = propertyName();
        Node* next = inner.get();
        innermost->list.push_back(std::move(inner));
        innermost = next;
    }

    expect(Tok::LBrace, "'{'");
    while (!at(Tok::RBrace)) {
        if (at(Tok::Eof))
            fail("unterminated namespace body");
        innermost->list.push_back(statement());
    }
    advance();
    return outer;
}

NodePtr Parser::ambientDeclaration()
{
    const bool wasAmbient = ambient_;
    ambient_ = true;
    advance();  // `declare`
    NodePtr n = statement();
    ambient_ = wasAmbient;
    n->declared = true;
    return n;
}

// ---------------------------------------------------------- type syntax ---

NodePtr Parser::typeAnnotation()
{
    if (!typescript_ || !eat(Tok::Colon))
        return nullptr;
    // `x: this is Foo` is a type predicate; so is a bare `x is Foo`.
    if (identifierLike(cur_.type) || at(Tok::This)) {
        Lexer probe = lexer_;
        if (probe.next(false).type == Tok::Is) {
            NodePtr n = makeNode(NK::TypePredicate, cur_.line);
            n->text = cur_.text;
            advance();
            advance();
            n->type = typeExpr();
            return n;
        }
    }
    return typeExpr();
}

NodePtr Parser::typeExpr()
{
    if (++depth_ > 400)
        fail("type is nested too deeply");
    struct Pop
    {
        std::size_t* d;
        ~Pop() { --*d; }
    } pop{&depth_};

    eat(Tok::Pipe);  // a leading `|` is allowed, for multi-line unions
    NodePtr first = intersectionType();
    if (!at(Tok::Pipe))
        return first;
    NodePtr n = makeNode(NK::UnionType, first->line);
    n->list.push_back(std::move(first));
    while (eat(Tok::Pipe))
        n->list.push_back(intersectionType());
    return n;
}

NodePtr Parser::intersectionType()
{
    eat(Tok::Amp);
    NodePtr first = postfixType();
    if (!at(Tok::Amp))
        return first;
    NodePtr n = makeNode(NK::IntersectionType, first->line);
    n->list.push_back(std::move(first));
    while (eat(Tok::Amp))
        n->list.push_back(postfixType());
    return n;
}

NodePtr Parser::postfixType()
{
    NodePtr base = primaryType();
    while (at(Tok::LBracket) && !cur_.newlineBefore) {
        advance();
        if (eat(Tok::RBracket)) {
            NodePtr n = makeNode(NK::ArrayType, base->line);
            n->type = std::move(base);
            base = std::move(n);
            continue;
        }
        NodePtr n = makeNode(NK::IndexedAccess, base->line);
        n->type = std::move(base);
        n->b = typeExpr();
        expect(Tok::RBracket, "']'");
        base = std::move(n);
    }
    return base;
}

NodePtr Parser::primaryType()
{
    const std::uint32_t line = cur_.line;
    switch (cur_.type) {
        case Tok::KeyOf: {
            advance();
            NodePtr n = makeNode(NK::KeyOfType, line);
            n->type = primaryType();
            return n;
        }
        case Tok::Readonly: {
            // `readonly T[]` and `readonly [A, B]`: checked, then forgotten.
            advance();
            NodePtr n = postfixType();
            n->readOnly = true;
            return n;
        }
        case Tok::TypeOf: {
            advance();
            NodePtr n = makeNode(NK::TypeQuery, line);
            if (!identifierLike(cur_.type))
                fail("expected a name after 'typeof'");
            n->text = cur_.text;
            advance();
            while (eat(Tok::Dot)) {
                n->text += u'.';
                n->text += propertyName();
            }
            return n;
        }
        case Tok::New: {
            advance();
            NodePtr n = makeNode(NK::ConstructorType, line);
            if (at(Tok::Lt))
                n->typeParams = typeParameterList();
            n->list = parameterList();
            expect(Tok::Arrow, "'=>'");
            n->type = typeExpr();
            return n;
        }
        case Tok::LParen:
        case Tok::Lt: return parenOrFunctionType();
        case Tok::LBrace: return typeLiteral();
        case Tok::LBracket: return tupleType();
        case Tok::StringLit: {
            NodePtr n = makeNode(NK::LiteralType, line);
            n->op = Tok::StringLit;
            n->text = cur_.text;
            advance();
            return n;
        }
        case Tok::Number: {
            NodePtr n = makeNode(NK::LiteralType, line);
            n->op = Tok::Number;
            n->number = cur_.number;
            advance();
            return n;
        }
        case Tok::Minus: {
            advance();
            if (!at(Tok::Number))
                fail("expected a number after '-' in a type");
            NodePtr n = makeNode(NK::LiteralType, line);
            n->op = Tok::Number;
            n->number = -cur_.number;
            advance();
            return n;
        }
        case Tok::True:
        case Tok::False:
        case Tok::Null: {
            NodePtr n = makeNode(NK::LiteralType, line);
            n->op = cur_.type;
            advance();
            return n;
        }
        case Tok::Void: {
            advance();
            NodePtr n = makeNode(NK::TypeRef, line);
            n->text = ascii("void");
            return n;
        }
        case Tok::This: {
            advance();
            NodePtr n = makeNode(NK::TypeRef, line);
            n->text = ascii("this");
            return n;
        }
        case Tok::Const: {
            // Only ever the `const` of `x as const`.
            advance();
            NodePtr n = makeNode(NK::TypeRef, line);
            n->text = ascii("const");
            return n;
        }
        default: break;
    }
    if (!identifierLike(cur_.type))
        fail(std::string("expected a type but found ") + tokenName(cur_.type));

    NodePtr n = makeNode(NK::TypeRef, line);
    n->text = cur_.text;
    advance();
    while (at(Tok::Dot)) {
        advance();
        n->text += u'.';
        n->text += propertyName();
    }
    if (at(Tok::Lt))
        n->list = typeArguments();
    return n;
}

NodePtr Parser::parenOrFunctionType()
{
    // `(` opens either a parenthesised type or a function type; the same probe
    // that finds an arrow function finds the difference.
    if (at(Tok::Lt) || looksLikeArrow()) {
        NodePtr n = makeNode(NK::FunctionType, cur_.line);
        if (at(Tok::Lt))
            n->typeParams = typeParameterList();
        n->list = parameterList();
        expect(Tok::Arrow, "'=>'");
        n->type = typeExpr();
        return n;
    }
    advance();  // `(`
    NodePtr inner = typeExpr();
    expect(Tok::RParen, "')'");
    return inner;
}

NodePtr Parser::tupleType()
{
    NodePtr n = makeNode(NK::TupleType, cur_.line);
    expect(Tok::LBracket, "'['");
    while (!at(Tok::RBracket)) {
        if (at(Tok::Eof))
            fail("unterminated tuple type");
        const bool rest = eat(Tok::Ellipsis);
        // A labelled element -- `[start: number, end: number]` -- names the
        // slot for readers only.
        if (identifierLike(cur_.type)) {
            Lexer probe = lexer_;
            const Token after = probe.next(false);
            if (after.type == Tok::Colon) {
                advance();
                advance();
            } else if (after.type == Tok::Question) {
                advance();
                advance();
                expect(Tok::Colon, "':'");
            }
        }
        NodePtr element = typeExpr();
        element->optional = eat(Tok::Question);
        if (rest) {
            NodePtr r = makeNode(NK::RestElement, element->line);
            r->type = std::move(element);
            element = std::move(r);
        }
        n->list.push_back(std::move(element));
        if (!at(Tok::RBracket))
            expect(Tok::Comma, "','");
    }
    advance();
    return n;
}

NodePtr Parser::typeLiteral()
{
    NodePtr n = makeNode(NK::TypeLit, cur_.line);
    expect(Tok::LBrace, "'{'");
    while (!at(Tok::RBrace)) {
        if (at(Tok::Eof))
            fail("unterminated type body");
        if (eat(Tok::Semicolon) || eat(Tok::Comma))
            continue;
        n->list.push_back(typeMember());
        if (!at(Tok::RBrace) && !at(Tok::Semicolon) && !at(Tok::Comma) && !cur_.newlineBefore)
            fail(std::string("expected ';' between members but found ") + tokenName(cur_.type));
    }
    advance();
    return n;
}

NodePtr Parser::typeMember()
{
    NodePtr m = makeNode(NK::TypeMember, cur_.line);
    if (at(Tok::Readonly)) {
        Lexer probe = lexer_;
        const Token after = probe.next(false);
        if (identifierLike(after.type) || after.type == Tok::LBracket ||
            after.type == Tok::StringLit) {
            m->readOnly = true;
            advance();
        }
    }

    // A call or construct signature has no name at all.
    if (at(Tok::LParen) || at(Tok::Lt) || at(Tok::New)) {
        const bool isNew = at(Tok::New);
        if (isNew)
            advance();
        m->propKind = isNew ? PropKind::Constructor : PropKind::Method;
        NodePtr fn = makeNode(isNew ? NK::ConstructorType : NK::FunctionType, m->line);
        if (at(Tok::Lt))
            fn->typeParams = typeParameterList();
        fn->list = parameterList();
        fn->type = typeAnnotation();
        m->type = std::move(fn);
        return m;
    }

    // `[key: string]: T` -- an index signature, as opposed to a computed key.
    if (at(Tok::LBracket)) {
        Lexer probe = lexer_;
        const Token name = probe.next(false);
        if (identifierLike(name.type) && probe.next(false).type == Tok::Colon) {
            advance();
            m->computed = true;
            advance();  // the key's name, which nothing reads
            advance();  // ':'
            m->a = typeExpr();
            expect(Tok::RBracket, "']'");
            m->type = typeAnnotation();
            if (!m->type)
                fail("an index signature needs a type");
            return m;
        }
        // `[Symbol.iterator]()` is the one computed key that means something.
        // It is stored under a name no identifier can spell.
        if (name.type == Tok::Identifier && name.text == ascii("Symbol")) {
            advance();
            advance();
            expect(Tok::Dot, "'.'");
            m->text = ascii("@@") + propertyName();
            expect(Tok::RBracket, "']'");
            m->optional = eat(Tok::Question);
            if (at(Tok::LParen) || at(Tok::Lt)) {
                m->propKind = PropKind::Method;
                NodePtr fn = makeNode(NK::FunctionType, m->line);
                if (at(Tok::Lt))
                    fn->typeParams = typeParameterList();
                fn->list = parameterList();
                fn->type = typeAnnotation();
                m->type = std::move(fn);
                return m;
            }
            m->type = typeAnnotation();
            if (!m->type)
                fail("expected ':' and a type after a symbol member");
            return m;
        }
        fail("computed property names are not allowed in a type");
    }

    m->text = propertyName();
    m->optional = eat(Tok::Question);
    if (at(Tok::LParen) || at(Tok::Lt)) {
        m->propKind = PropKind::Method;
        NodePtr fn = makeNode(NK::FunctionType, m->line);
        if (at(Tok::Lt))
            fn->typeParams = typeParameterList();
        fn->list = parameterList();
        fn->type = typeAnnotation();
        m->type = std::move(fn);
        return m;
    }
    m->type = typeAnnotation();
    if (!m->type)
        fail("expected ':' and a type after '" + toUtf8(m->text) + "'");
    return m;
}

std::vector<NodePtr> Parser::typeParameterList()
{
    std::vector<NodePtr> params;
    expect(Tok::Lt, "'<'");
    while (!at(Tok::Gt) && !at(Tok::Shr) && !at(Tok::UShr)) {
        if (at(Tok::Eof))
            fail("unterminated type parameter list");
        NodePtr p = makeNode(NK::TypeParam, cur_.line);
        if (!identifierLike(cur_.type))
            fail("expected a type parameter name");
        p->text = cur_.text;
        advance();
        if (eat(Tok::Extends))
            p->a = typeExpr();
        if (eat(Tok::Assign))
            p->b = typeExpr();
        params.push_back(std::move(p));
        if (!at(Tok::Gt) && !at(Tok::Shr) && !at(Tok::UShr))
            expect(Tok::Comma, "','");
    }
    closeAngle();
    return params;
}

std::vector<NodePtr> Parser::typeArguments()
{
    std::vector<NodePtr> args;
    expect(Tok::Lt, "'<'");
    while (!at(Tok::Gt) && !at(Tok::Shr) && !at(Tok::UShr)) {
        if (at(Tok::Eof))
            fail("unterminated type argument list");
        args.push_back(typeExpr());
        if (!at(Tok::Gt) && !at(Tok::Shr) && !at(Tok::UShr))
            expect(Tok::Comma, "','");
    }
    closeAngle();
    return args;
}

// ------------------------------------------------------------ patterns ---

/// `yield` and `await` where they are operators, and a keyword written with an escape, name nothing.
void Parser::checkReference()
{
    if (cur_.escapedKeyword)
        fail("a keyword cannot be written with an escape");
    if (at(Tok::Yield) && inGenerator_)
        fail("'yield' is not a name in a generator");
    if (at(Tok::Await) && (inAsync_ || inStaticBlock_))
        fail("'await' is not a name here");
}

NodePtr Parser::bodyStatement(bool allowsFunction)
{
    bool refused = at(Tok::Class) || at(Tok::Const);
    if (at(Tok::Function))
        refused = !allowsFunction;
    if (at(Tok::Async) && asyncFunctionAhead())
        refused = true;
    if (at(Tok::Let)) {
        Lexer probe = lexer_;
        const Token after = probe.next(false);
        refused = after.type == Tok::LBracket || (!after.newlineBefore && (identifierLike(after.type) || after.type == Tok::LBrace));
    }
    if (refused)
        fail("a declaration cannot be the body of this statement");
    if (at(Tok::Function) && allowsFunction) {
        Lexer probe = lexer_;
        if (probe.next(false).type == Tok::Star)
            fail("a generator cannot be the body of this statement");
    }
    return statement();
}

NodePtr Parser::bindingTarget()
{
    if (identifierLike(cur_.type))
        checkReference();
    if (at(Tok::LBracket))
        return arrayPattern();
    if (at(Tok::LBrace))
        return objectPattern();
    if (!identifierLike(cur_.type))
        fail(std::string("expected a binding name but found ") + tokenName(cur_.type));
    NodePtr n = makeNode(NK::Identifier, cur_.line);
    n->text = cur_.text;
    advance();
    return n;
}

NodePtr Parser::bindingElement()
{
    NodePtr target = bindingTarget();
    if (!eat(Tok::Assign))
        return target;
    NodePtr def = makeNode(NK::AssignPattern, target->line);
    def->a = std::move(target);
    def->b = assignment();
    return def;
}

NodePtr Parser::arrayPattern()
{
    NodePtr n = makeNode(NK::ArrayPattern, cur_.line);
    expect(Tok::LBracket, "'['");
    while (!at(Tok::RBracket)) {
        if (at(Tok::Comma)) {
            advance();
            n->list.push_back(makeNode(NK::Hole, cur_.line));
            continue;
        }
        if (eat(Tok::Ellipsis)) {
            NodePtr rest = makeNode(NK::RestElement, cur_.line);
            rest->a = bindingTarget();
            n->list.push_back(std::move(rest));
            if (!at(Tok::RBracket))
                fail("a rest element must be the last");
            continue;
        } else {
            n->list.push_back(bindingElement());
        }
        if (!at(Tok::RBracket))
            expect(Tok::Comma, "','");
    }
    advance();
    return n;
}

NodePtr Parser::objectPattern()
{
    NodePtr n = makeNode(NK::ObjectPattern, cur_.line);
    expect(Tok::LBrace, "'{'");
    while (!at(Tok::RBrace)) {
        if (eat(Tok::Ellipsis)) {
            NodePtr rest = makeNode(NK::RestElement, cur_.line);
            rest->a = bindingTarget();
            if (rest->a->kind != NK::Identifier)
                fail("the rest of an object pattern is a name");
            n->list.push_back(std::move(rest));
            if (!at(Tok::RBrace))
                fail("a rest element must be the last");
            continue;
        } else {
            NodePtr prop = makeNode(NK::Property, cur_.line);
            if (at(Tok::LBracket)) {
                advance();
                prop->computed = true;
                prop->a = assignment();
                expect(Tok::RBracket, "']'");
                expect(Tok::Colon, "':'");
                prop->b = bindingTarget();
            } else {
                prop->text = propertyName();
                if (eat(Tok::Colon)) {
                    prop->b = bindingTarget();
                } else {
                    NodePtr id = makeNode(NK::Identifier, prop->line);
                    id->text = prop->text;
                    prop->b = std::move(id);
                }
            }
            if (eat(Tok::Assign)) {
                NodePtr def = makeNode(NK::AssignPattern, prop->line);
                def->a = std::move(prop->b);
                def->b = assignment();
                prop->b = std::move(def);
            }
            n->list.push_back(std::move(prop));
        }
        if (!at(Tok::RBrace))
            expect(Tok::Comma, "','");
    }
    advance();
    return n;
}

NodePtr Parser::toPattern(NodePtr expr)
{
    if (!expr)
        return expr;
    switch (expr->kind) {
        case NK::ArrayLit: {
            NodePtr n = makeNode(NK::ArrayPattern, expr->line);
            for (NodePtr& e : expr->list) {
                if (e->kind == NK::Spread) {
                    if (&e != &expr->list.back())
                        fail("a rest element must be the last");
                    NodePtr rest = makeNode(NK::RestElement, e->line);
                    rest->a = toPattern(std::move(e->a));
                    n->list.push_back(std::move(rest));
                } else {
                    n->list.push_back(toPattern(std::move(e)));
                }
            }
            return n;
        }
        case NK::ObjectLit: {
            NodePtr n = makeNode(NK::ObjectPattern, expr->line);
            for (NodePtr& p : expr->list) {
                if (p->propKind == PropKind::Spread) {
                    NodePtr rest = makeNode(NK::RestElement, p->line);
                    rest->a = toPattern(std::move(p->a));
                    n->list.push_back(std::move(rest));
                    continue;
                }
                if (p->propKind != PropKind::Init)
                    fail("this property cannot be an assignment target");
                p->b = toPattern(std::move(p->b));
                n->list.push_back(std::move(p));
            }
            return n;
        }
        case NK::Assign: {
            if (expr->op != Tok::Assign)
                fail("invalid assignment target");
            NodePtr n = makeNode(NK::AssignPattern, expr->line);
            n->a = toPattern(std::move(expr->a));
            n->b = std::move(expr->b);
            return n;
        }
        case NK::Spread: {
            NodePtr n = makeNode(NK::RestElement, expr->line);
            n->a = toPattern(std::move(expr->a));
            return n;
        }
        case NK::Identifier:
        case NK::Member:
        case NK::Hole:
        case NK::ArrayPattern:
        case NK::ObjectPattern:
        case NK::AssignPattern:
        case NK::RestElement: return expr;
        default: fail("invalid assignment target");
    }
}

// --------------------------------------------------------- expressions ---

NodePtr Parser::expression()
{
    NodePtr first = assignment();
    if (!at(Tok::Comma))
        return first;
    NodePtr n = makeNode(NK::Sequence, first->line);
    n->list.push_back(std::move(first));
    while (eat(Tok::Comma))
        n->list.push_back(assignment());
    return n;
}

NodePtr Parser::assignment()
{
    NestingScope nest(exprDepth_);
    if (nest.tooDeep())
        fail("expression is nested too deeply");
    if (at(Tok::Yield) && inGenerator_) {
        if (inParameters_)
            fail("'yield' cannot be used in a generator's parameters");
        NodePtr n = makeNode(NK::Yield, cur_.line);
        advance();
        if (!cur_.newlineBefore) {
            n->prefix = eat(Tok::Star);
            switch (cur_.type) {
                case Tok::RParen:
                case Tok::RBracket:
                case Tok::RBrace:
                case Tok::Comma:
                case Tok::Semicolon:
                case Tok::Colon:
                case Tok::In:
                case Tok::Of:
                case Tok::Eof:
                    if (n->prefix)
                        fail("'yield*' needs an operand");
                    break;
                default: n->a = assignment(); break;
            }
        }
        return n;
    }
    if (at(Tok::Async) && asyncArrowAhead()) {
        const std::uint32_t line = cur_.line;
        advance();
        std::vector<NodePtr> params;
        if (at(Tok::LParen)) {
            const FunctionKind kind(*this, false, true);
            params = parameterList();
        } else {
            NodePtr param = makeNode(NK::Identifier, cur_.line);
            param->text = cur_.text;
            advance();
            params.push_back(std::move(param));
        }
        NodePtr returnType = at(Tok::Colon) ? typeAnnotation() : nullptr;
        expect(Tok::Arrow, "'=>'");
        arrowIsAsync_ = true;
        return arrowFromParams(std::move(params), std::move(returnType), line);
    }

    // `x => ...` and `(a, b) => ...`
    if (identifierLike(cur_.type)) {
        Lexer probe = lexer_;
        if (probe.next(false).type == Tok::Arrow) {
            NodePtr param = makeNode(NK::Identifier, cur_.line);
            param->text = cur_.text;
            const std::uint32_t line = cur_.line;
            advance();
            advance();
            std::vector<NodePtr> params;
            params.push_back(std::move(param));
            return arrowFromParams(std::move(params), nullptr, line);
        }
    }
    if ((at(Tok::LParen) && looksLikeArrow()) || (at(Tok::Lt) && looksLikeGenericArrow())) {
        const std::uint32_t line = cur_.line;
        std::vector<NodePtr> generics;
        if (at(Tok::Lt))
            generics = typeParameterList();
        std::vector<NodePtr> params = parameterList();
        NodePtr returnType = typeAnnotation();
        expect(Tok::Arrow, "'=>'");
        NodePtr arrow = arrowFromParams(std::move(params), std::move(returnType), line);
        arrow->typeParams = std::move(generics);
        return arrow;
    }

    NodePtr left = conditional();
    if (!isAssignOp(cur_.type))
        return left;

    NodePtr n = makeNode(NK::Assign, left->line);
    n->op = cur_.type;
    advance();
    n->a = (n->op == Tok::Assign) ? toPattern(std::move(left)) : std::move(left);
    n->b = assignment();
    return n;
}

NodePtr Parser::conditional()
{
    NodePtr test = binary(1);
    if (!at(Tok::Question))
        return test;
    NodePtr n = makeNode(NK::Conditional, test->line);
    advance();
    n->a = std::move(test);
    n->b = assignment();
    expect(Tok::Colon, "':'");
    n->c = assignment();
    return n;
}

NodePtr Parser::binary(int minPrecedence)
{
    NodePtr left = unary();
    std::size_t links = 0;
    for (;;) {
        if (++links > kMaxChainLength)
            fail("expression is too long; split it into parts");
        // `as` and `satisfies` bind like a relational operator, and like one
        // they may not start a new line of their own.
        if (typescript_ && (at(Tok::As) || at(Tok::Satisfies)) && !cur_.newlineBefore) {
            if (kRelationalPrecedence < minPrecedence)
                return left;
            NodePtr n = makeNode(NK::AsExpr, left->line);
            n->op = cur_.type;
            advance();
            n->a = std::move(left);
            n->type = typeExpr();
            left = std::move(n);
            continue;
        }
        const int prec = precedenceOf(cur_.type);
        if (prec == 0 || prec < minPrecedence)
            return left;
        if (noIn_ && cur_.type == Tok::In)
            return left;  // the `in` of a for-in head, not the operator
        const Tok op = cur_.type;
        advance();
        // `**` is the only right-associative binary operator.
        NodePtr right = binary(op == Tok::StarStar ? prec : prec + 1);
        const bool logical = op == Tok::AndAnd || op == Tok::OrOr || op == Tok::QuestionQuestion;
        NodePtr n = makeNode(logical ? NK::Logical : NK::Binary, left->line);
        n->op = op;
        n->a = std::move(left);
        n->b = std::move(right);
        left = std::move(n);
    }
}

NodePtr Parser::unary()
{
    NestingScope nest(exprDepth_);
    if (nest.tooDeep())
        fail("expression is nested too deeply");
    const std::uint32_t line = cur_.line;
    if (at(Tok::Await) && inAsync_) {
        if (inParameters_)
            fail("'await' cannot be used in an async function's parameters");
        NodePtr n = makeNode(NK::Await, line);
        advance();
        n->a = unary();
        return n;
    }
    switch (cur_.type) {
        case Tok::Bang:
        case Tok::Tilde:
        case Tok::Plus:
        case Tok::Minus:
        case Tok::TypeOf:
        case Tok::Void:
        case Tok::Delete: {
            NodePtr n = makeNode(NK::Unary, line);
            n->op = cur_.type;
            advance();
            n->a = unary();
            return n;
        }
        case Tok::PlusPlus:
        case Tok::MinusMinus: {
            NodePtr n = makeNode(NK::Update, line);
            n->op = cur_.type;
            n->prefix = true;
            advance();
            n->a = unary();
            return n;
        }
        default: return postfix();
    }
}

NodePtr Parser::postfix()
{
    NodePtr base = callOrMember(primary(), true);
    if ((at(Tok::PlusPlus) || at(Tok::MinusMinus)) && !cur_.newlineBefore) {
        NodePtr n = makeNode(NK::Update, base->line);
        n->op = cur_.type;
        n->prefix = false;
        n->a = std::move(base);
        advance();
        return n;
    }
    return base;
}

void Parser::privateMember(Node& member)
{
    // A computed access whose key is the variable holding the private name: every kind of assignment to a member then
    // works the same, and what is different about it is in what such a key does when it is used.
    member.computed = true;
    member.b = makeNode(NK::Identifier, cur_.line);
    member.b->text = ascii("#") + cur_.text;
    member.b->isPrivate = true;
    advance();
}

NodePtr Parser::callOrMember(NodePtr base, bool allowCall)
{
    std::size_t links = 0;
    for (;;) {
        if (++links > kMaxChainLength)
            fail("expression is too long; split it into parts");
        if (at(Tok::Dot)) {
            advance();
            NodePtr n = makeNode(NK::Member, base->line);
            n->a = std::move(base);
            if (at(Tok::PrivateName))
                privateMember(*n);
            else
                n->text = propertyName();
            base = std::move(n);
        } else if (at(Tok::QuestionDot)) {
            advance();
            if (at(Tok::LParen) && allowCall) {
                NodePtr n = makeNode(NK::Call, base->line);
                n->optional = true;
                n->a = std::move(base);
                n->list = argumentList();
                base = std::move(n);
                continue;
            }
            NodePtr n = makeNode(NK::Member, base->line);
            n->optional = true;
            n->a = std::move(base);
            if (at(Tok::LBracket)) {
                advance();
                n->computed = true;
                {
                    FlagReset allowIn(noIn_);
                    n->b = expression();
                }
                expect(Tok::RBracket, "']'");
            } else if (at(Tok::PrivateName)) {
                privateMember(*n);
            } else {
                n->text = propertyName();
            }
            base = std::move(n);
        } else if (at(Tok::LBracket)) {
            advance();
            NodePtr n = makeNode(NK::Member, base->line);
            n->computed = true;
            n->a = std::move(base);
            {
                FlagReset allowIn(noIn_);
                n->b = expression();
            }
            expect(Tok::RBracket, "']'");
            base = std::move(n);
        } else if (at(Tok::LParen) && allowCall) {
            NodePtr n = makeNode(NK::Call, base->line);
            n->a = std::move(base);
            n->list = argumentList();
            base = std::move(n);
        } else if (typescript_ && at(Tok::Bang) && !cur_.newlineBefore) {
            // A non-null assertion. `!=` and `!==` lex as single tokens, so a
            // bare `!` right after an operand can only be this.
            advance();
            NodePtr n = makeNode(NK::NonNull, base->line);
            n->a = std::move(base);
            base = std::move(n);
        } else if (at(Tok::Lt) && allowCall && looksLikeTypeArguments()) {
            std::vector<NodePtr> args = typeArguments();
            if (!at(Tok::LParen))
                fail("expected an argument list after type arguments");
            NodePtr n = makeNode(NK::Call, base->line);
            n->a = std::move(base);
            n->typeParams = std::move(args);
            n->list = argumentList();
            base = std::move(n);
        } else if (at(Tok::Template)) {
            NodePtr n = makeNode(NK::Call, base->line);
            n->a = std::move(base);
            NodePtr strings = makeNode(NK::TemplateObject, cur_.line);
            strings->strs = cur_.parts;
            strings->raws = cur_.raws;
            NodePtr literal = templateLiteral();
            n->list.push_back(std::move(strings));
            for (NodePtr& e : literal->list)
                n->list.push_back(std::move(e));
            base = std::move(n);
        } else {
            return base;
        }
    }
}

std::vector<NodePtr> Parser::argumentList()
{
    std::vector<NodePtr> args;
    FlagReset allowIn(noIn_);
    expect(Tok::LParen, "'('");
    while (!at(Tok::RParen)) {
        if (eat(Tok::Ellipsis)) {
            NodePtr s = makeNode(NK::Spread, cur_.line);
            s->a = assignment();
            args.push_back(std::move(s));
        } else {
            args.push_back(assignment());
        }
        if (!at(Tok::RParen))
            expect(Tok::Comma, "','");
    }
    advance();
    return args;
}

bool Parser::looksLikeArrow()
{
    Lexer probe = lexer_;
    int depth = 1;
    Token t;
    // The scan guesses at `/` after `}` without knowing whether a block or an object literal ended there; when it guesses
    // a division and the rest is a regular expression that does not lex as tokens, what it scanned was no parameter list.
    try {
        t = probe.next(true);
        while (t.type != Tok::Eof) {
            if (++lookahead_ > lookaheadLimit_)
                fail("source is too deeply parenthesised to parse");
            if (t.type == Tok::LParen) {
                ++depth;
            } else if (t.type == Tok::RParen) {
                if (--depth == 0)
                    break;
            }
            t = probe.next(!operandJustEnded(t.type));
        }
    } catch (const Error&) {
        if (lookahead_ > lookaheadLimit_)
            throw;
        return false;
    }
    if (t.type != Tok::RParen)
        return false;
    t = probe.next(false);
    if (t.type == Tok::Arrow)
        return true;
    if (!typescript_ || t.type != Tok::Colon)
        return false;
    // A return type annotation stands between the `)` and the `=>`. Scan over
    // it: anything that closes the enclosing expression first means this was a
    // parenthesised expression after all.
    return scanPastType(probe) == Tok::Arrow;
}

bool Parser::looksLikeGenericArrow()
{
    if (!typescript_)
        return false;
    Lexer probe = lexer_;
    return scanPastAngles(probe) && probe.next(false).type == Tok::LParen;
}

bool Parser::looksLikeTypeArguments()
{
    if (!typescript_)
        return false;
    Lexer probe = lexer_;
    if (!scanPastAngles(probe))
        return false;
    const Tok after = probe.next(false).type;
    return after == Tok::LParen || after == Tok::Template;
}

NodePtr Parser::arrowFromParams(std::vector<NodePtr> params, NodePtr returnType, std::uint32_t line)
{
    NodePtr n = makeNode(NK::ArrowExpr, line);
    n->list = std::move(params);
    n->type = std::move(returnType);
    n->isAsync = arrowIsAsync_;
    arrowIsAsync_ = false;
    const FunctionKind kind(*this, false, n->isAsync, true);
    if (at(Tok::LBrace)) {
        n->d = block();
    } else {
        NodePtr body = makeNode(NK::Block, cur_.line);
        NodePtr ret = makeNode(NK::ReturnStmt, cur_.line);
        ret->a = assignment();
        body->list.push_back(std::move(ret));
        n->d = std::move(body);
    }
    return n;
}

std::vector<NodePtr> Parser::parameterList(bool allowModifiers)
{
    FlagSet inParameters(inParameters_, true);
    std::vector<NodePtr> params;
    expect(Tok::LParen, "'('");
    while (!at(Tok::RParen)) {
        const std::uint32_t line = cur_.line;
        if (eat(Tok::Ellipsis)) {
            NodePtr rest = makeNode(NK::RestElement, line);
            rest->a = bindingTarget();
            rest->type = typeAnnotation();
            params.push_back(std::move(rest));
            if (!at(Tok::RParen))
                fail("a rest parameter must be the last");
            continue;
        }

        // `constructor(private readonly x: T)` declares a field as well as a
        // parameter. The modifiers are only legal where a constructor body can
        // receive the assignment.
        Visibility visibility = Visibility::Public;
        bool isParamProp = false;
        bool readOnly = false;
        for (;;) {
            const Tok t = cur_.type;
            if (!typescript_ || (t != Tok::Public && t != Tok::Private && t != Tok::Protected &&
                                 t != Tok::Readonly && t != Tok::Override))
                break;
            Lexer probe = lexer_;
            const Token after = probe.next(false);
            if (!identifierLike(after.type) && after.type != Tok::Readonly &&
                after.type != Tok::LBrace && after.type != Tok::LBracket)
                break;  // the modifier word is really the parameter's name
            if (!allowModifiers)
                fail("parameter properties are only allowed in a constructor");
            if (t == Tok::Readonly)
                readOnly = true;
            else if (t == Tok::Private)
                visibility = Visibility::Private;
            else if (t == Tok::Protected)
                visibility = Visibility::Protected;
            isParamProp = true;
            advance();
        }

        NodePtr target = bindingTarget();
        const bool optional = typescript_ && eat(Tok::Question);
        NodePtr annotation = typeAnnotation();
        NodePtr param = std::move(target);
        if (eat(Tok::Assign)) {
            NodePtr def = makeNode(NK::AssignPattern, line);
            def->a = std::move(param);
            def->b = assignment();
            param = std::move(def);
        }
        param->type = std::move(annotation);
        param->optional = optional;
        param->readOnly = readOnly;
        param->visibility = visibility;
        param->isParamProp = isParamProp;
        params.push_back(std::move(param));
        if (!at(Tok::RParen))
            expect(Tok::Comma, "','");
    }
    advance();
    return params;
}

NodePtr Parser::functionExpression(bool isDeclaration)
{
    NodePtr n = makeNode(NK::FunctionExpr, cur_.line);
    n->isAsync = eat(Tok::Async);
    expect(Tok::Function, "'function'");
    n->isGenerator = eat(Tok::Star);
    if (identifierLike(cur_.type)) {
        n->text = cur_.text;
        n->namedExpr = !isDeclaration;
        advance();
    } else if (isDeclaration) {
        fail("a function declaration needs a name");
    }
    const FunctionKind kind(*this, n->isGenerator, n->isAsync);
    if (typescript_ && at(Tok::Lt))
        n->typeParams = typeParameterList();
    n->list = parameterList();
    n->type = typeAnnotation();
    if (ambient_) {
        n->declared = true;
        semicolon();
        return n;
    }
    n->d = block();
    return n;
}

NodePtr Parser::classExpression(bool isDeclaration, bool isAbstract)
{
    NodePtr n = makeNode(NK::ClassExpr, cur_.line);
    n->isAbstract = isAbstract;
    expect(Tok::Class, "'class'");
    if (identifierLike(cur_.type)) {
        n->text = cur_.text;
        advance();
    } else if (isDeclaration) {
        fail("a class declaration needs a name");
    }
    if (typescript_ && at(Tok::Lt))
        n->typeParams = typeParameterList();
    if (eat(Tok::Extends)) {
        n->a = callOrMember(primary(), true);
        if (typescript_ && at(Tok::Lt))
            n->a->typeParams = typeArguments();
    }
    if (typescript_ && eat(Tok::Implements)) {
        NodePtr heritage = makeNode(NK::HeritageClause, cur_.line);
        do
            heritage->list.push_back(primaryType());
        while (eat(Tok::Comma));
        n->c = std::move(heritage);
    }
    expect(Tok::LBrace, "'{'");

    while (!at(Tok::RBrace)) {
        if (at(Tok::Eof))
            fail("unterminated class body");
        if (eat(Tok::Semicolon))
            continue;

        bool isStatic = false;
        bool readOnly = false;
        bool memberAbstract = false;
        bool ambientMember = ambient_;
        Visibility visibility = Visibility::Public;
        // Modifiers, in any order. Each word is a modifier only when something
        // that can follow one comes next; otherwise it names the member.
        for (;;) {
            const Tok t = cur_.type;
            if (!typescript_ && t != Tok::Static)
                break;
            if (t != Tok::Static && t != Tok::Public && t != Tok::Private && t != Tok::Protected &&
                t != Tok::Readonly && t != Tok::Abstract && t != Tok::Override && t != Tok::Declare)
                break;
            Lexer probe = lexer_;
            const Token after = probe.next(false);
            if (after.type == Tok::LParen || after.type == Tok::Assign ||
                after.type == Tok::Colon || after.type == Tok::Semicolon ||
                after.type == Tok::RBrace || after.type == Tok::Question || after.type == Tok::Lt ||
                (after.newlineBefore && t == Tok::Static && after.type == Tok::Eof))
                break;  // the modifier word is really the member's name
            switch (t) {
                case Tok::Static: isStatic = true; break;
                case Tok::Readonly: readOnly = true; break;
                case Tok::Abstract: memberAbstract = true; break;
                case Tok::Private: visibility = Visibility::Private; break;
                case Tok::Protected: visibility = Visibility::Protected; break;
                case Tok::Declare: ambientMember = true; break;
                default: break;
            }
            advance();
        }

        if (isStatic && at(Tok::LBrace)) {
            NodePtr staticBlock = makeNode(NK::StaticBlock, cur_.line);
            staticBlock->isStatic = true;
            const FunctionKind blockKind(*this, false, false);
            inStaticBlock_ = true;
            staticBlock->d = block();
            n->list.push_back(std::move(staticBlock));
            continue;
        }
        const bool memberAsync = at(Tok::Async) && asyncMethodAhead();
        if (memberAsync)
            advance();
        const bool memberGenerator = eat(Tok::Star);
        PropKind kind = PropKind::Method;
        if (!memberAsync && !memberGenerator && (at(Tok::Get) || at(Tok::Set))) {
            Lexer probe = lexer_;
            const Token after = probe.next(false);
            if (after.type != Tok::LParen && after.type != Tok::Assign &&
                after.type != Tok::Semicolon && after.type != Tok::RBrace &&
                after.type != Tok::Colon && after.type != Tok::Question) {
                kind = at(Tok::Get) ? PropKind::Getter : PropKind::Setter;
                advance();
            }
        }

        NodePtr key;
        bool computed = false;
        bool isPrivate = false;
        const std::uint32_t line = cur_.line;
        if (at(Tok::LBracket)) {
            Lexer probe = lexer_;
            const Token name = probe.next(false);
            if (typescript_ && identifierLike(name.type) && probe.next(false).type == Tok::Colon) {
                // An index signature declares nothing at run time.
                NodePtr member = typeMember();
                member->isStatic = isStatic;
                n->list.push_back(std::move(member));
                semicolon();
                continue;
            }
            advance();
            computed = true;
            key = assignment();
            expect(Tok::RBracket, "']'");
        } else if (at(Tok::PrivateName)) {
            isPrivate = true;
            key = makeNode(NK::StringLit, line);
            key->text = ascii("#") + cur_.text;
            advance();
        } else {
            key = makeNode(NK::StringLit, line);
            key->text = propertyName();
        }

        const bool optional = typescript_ && eat(Tok::Question);
        if (typescript_ && at(Tok::Bang) && !cur_.newlineBefore)
            advance();  // definite assignment; nothing to check here

        if (at(Tok::LParen) || (typescript_ && at(Tok::Lt))) {
            const bool isConstructor = kind == PropKind::Method && !computed && !isStatic &&
                                       key->text == ascii("constructor");
            NodePtr fn = makeNode(NK::FunctionExpr, line);
            if (!computed)
                fn->text = key->text;
            fn->isAsync = memberAsync;
            fn->isGenerator = memberGenerator;
            fn->isMethod = true;
            const FunctionKind functionKind(*this, memberGenerator, memberAsync);
            if (at(Tok::Lt))
                fn->typeParams = typeParameterList();
            fn->list = parameterList(isConstructor);
            fn->type = typeAnnotation();
            // An abstract or ambient method, and a bare overload signature,
            // declare a type and emit nothing.
            if (memberAbstract || ambientMember || (typescript_ && !at(Tok::LBrace))) {
                fn->declared = true;
                semicolon();
            } else {
                fn->d = block();
            }

            NodePtr m = makeNode(NK::MethodDef, line);
            m->isStatic = isStatic;
            m->computed = computed;
            m->propKind = isConstructor ? PropKind::Constructor : kind;
            m->optional = optional;
            m->visibility = visibility;
            m->isAbstract = memberAbstract;
            m->isPrivate = isPrivate;
            m->declared = fn->declared;
            m->a = std::move(key);
            m->b = std::move(fn);
            n->list.push_back(std::move(m));
            continue;
        }

        NodePtr f = makeNode(NK::FieldDef, line);
        f->isStatic = isStatic;
        f->computed = computed;
        f->optional = optional;
        f->readOnly = readOnly;
        f->visibility = visibility;
        f->isAbstract = memberAbstract;
        f->declared = memberAbstract || ambientMember;
        f->isPrivate = isPrivate;
        f->a = std::move(key);
        f->type = typeAnnotation();
        if (eat(Tok::Assign)) {
            const FunctionKind fieldKind(*this, false, false);
            f->b = assignment();
        }
        semicolon();
        n->list.push_back(std::move(f));
    }
    advance();
    return n;
}

NodePtr Parser::arrayLiteral()
{
    NodePtr n = makeNode(NK::ArrayLit, cur_.line);
    expect(Tok::LBracket, "'['");
    while (!at(Tok::RBracket)) {
        if (at(Tok::Comma)) {
            advance();
            n->list.push_back(makeNode(NK::Hole, cur_.line));
            continue;
        }
        if (eat(Tok::Ellipsis)) {
            NodePtr s = makeNode(NK::Spread, cur_.line);
            s->a = assignment();
            n->list.push_back(std::move(s));
        } else {
            n->list.push_back(assignment());
        }
        if (!at(Tok::RBracket))
            expect(Tok::Comma, "','");
    }
    advance();
    return n;
}

NodePtr Parser::objectLiteral()
{
    NodePtr n = makeNode(NK::ObjectLit, cur_.line);
    expect(Tok::LBrace, "'{'");
    while (!at(Tok::RBrace)) {
        const std::uint32_t line = cur_.line;
        if (eat(Tok::Ellipsis)) {
            NodePtr p = makeNode(NK::Property, line);
            p->propKind = PropKind::Spread;
            p->a = assignment();
            n->list.push_back(std::move(p));
            if (!at(Tok::RBrace))
                expect(Tok::Comma, "','");
            continue;
        }

        const bool memberAsync = at(Tok::Async) && asyncMethodAhead();
        if (memberAsync)
            advance();
        const bool memberGenerator = eat(Tok::Star);
        PropKind kind = PropKind::Init;
        if (!memberAsync && !memberGenerator && (at(Tok::Get) || at(Tok::Set))) {
            Lexer probe = lexer_;
            const Token after = probe.next(false);
            if (after.type != Tok::Colon && after.type != Tok::Comma && after.type != Tok::RBrace &&
                after.type != Tok::LParen) {
                kind = at(Tok::Get) ? PropKind::Getter : PropKind::Setter;
                advance();
            }
        }

        NodePtr p = makeNode(NK::Property, line);
        p->propKind = kind;
        bool shorthandOk = false;
        if (at(Tok::LBracket)) {
            advance();
            p->computed = true;
            p->a = assignment();
            expect(Tok::RBracket, "']'");
        } else {
            shorthandOk = identifierLike(cur_.type) && !cur_.escapedKeyword;
            p->text = propertyName();
        }

        if (at(Tok::LParen) || (typescript_ && at(Tok::Lt))) {
            NodePtr fn = makeNode(NK::FunctionExpr, line);
            fn->text = p->text;
            fn->isAsync = memberAsync;
            fn->isGenerator = memberGenerator;
            fn->isMethod = true;
            const FunctionKind functionKind(*this, memberGenerator, memberAsync);
            if (at(Tok::Lt))
                fn->typeParams = typeParameterList();
            fn->list = parameterList();
            fn->type = typeAnnotation();
            fn->d = block();
            p->b = std::move(fn);
            if (kind == PropKind::Init)
                p->propKind = PropKind::Method;
        } else if (memberAsync || memberGenerator) {
            fail("a generator or async method needs a parameter list");
        } else if (kind != PropKind::Init) {
            fail("an accessor needs a parameter list");
        } else if (eat(Tok::Colon)) {
            p->setsProto = !p->computed && p->text == u"__proto__";
            p->b = assignment();
        } else {
            // `{ x }` is a read of the variable x, which a keyword cannot be.
            if (!shorthandOk)
                fail("expected ':' after a property name that is no variable's");
            NodePtr id = makeNode(NK::Identifier, line);
            id->text = p->text;
            if (at(Tok::Assign)) {
                // Only meaningful once the literal is reinterpreted as a
                // destructuring pattern; `toPattern` picks it up from here.
                advance();
                NodePtr def = makeNode(NK::Assign, line);
                def->op = Tok::Assign;
                def->a = std::move(id);
                def->b = assignment();
                p->b = std::move(def);
            } else {
                p->b = std::move(id);
            }
        }
        n->list.push_back(std::move(p));
        if (!at(Tok::RBrace))
            expect(Tok::Comma, "','");
    }
    advance();
    return n;
}

NodePtr Parser::templateLiteral()
{
    NodePtr n = makeNode(NK::TemplateLit, cur_.line);
    n->strs = cur_.parts;
    for (const std::string& raw : cur_.exprs) {
        Parser sub(raw, name_);
        sub.inGenerator_ = inGenerator_;
        sub.inAsync_ = inAsync_;
        NodePtr e = sub.expression();
        n->list.push_back(std::move(e));
    }
    advance();
    return n;
}

NodePtr Parser::primary()
{
    FlagReset allowIn(noIn_);
    const std::uint32_t line = cur_.line;
    switch (cur_.type) {
        case Tok::Number: {
            NodePtr n = makeNode(NK::NumberLit, line);
            n->number = cur_.number;
            n->legacyOctal = cur_.legacyOctal;
            advance();
            return n;
        }
        case Tok::StringLit: {
            NodePtr n = makeNode(NK::StringLit, line);
            n->text = cur_.text;
            n->legacyOctal = cur_.legacyOctal;
            advance();
            return n;
        }
        case Tok::BigIntLit: {
            NodePtr n = makeNode(NK::BigIntLit, line);
            n->text = cur_.text;
            advance();
            return n;
        }
        case Tok::Template: return templateLiteral();
        case Tok::True:
        case Tok::False: {
            NodePtr n = makeNode(NK::BoolLit, line);
            n->number = at(Tok::True) ? 1 : 0;
            advance();
            return n;
        }
        case Tok::Null: advance(); return makeNode(NK::NullLit, line);
        case Tok::This: advance(); return makeNode(NK::ThisExpr, line);
        case Tok::Super: advance(); return makeNode(NK::SuperExpr, line);
        case Tok::Function: return functionExpression(false);
        case Tok::Class: return classExpression(false, false);
        case Tok::LBracket: return arrayLiteral();
        case Tok::LBrace: return objectLiteral();
        case Tok::Slash:
        case Tok::SlashAssign:
            // Only a parser knows an operand is wanted here: what was read as a division is the start of a literal.
            cur_ = lexer_.rescanAsRegex(cur_);
            if (!at(Tok::Regex))
                fail("unterminated regular expression literal");
            [[fallthrough]];
        case Tok::Regex: {
            const std::size_t close = cur_.text.rfind(u'/');
            NodePtr n = makeNode(NK::RegExpLit, line);
            n->text = cur_.text.substr(1, close - 1);
            n->strs.push_back(cur_.text.substr(close + 1));
            std::string error;
            if (regex::literalError(n->text, n->strs[0], error))
                fail("invalid regular expression: " + error);
            advance();
            return n;
        }
        case Tok::Async:
            if (asyncFunctionAhead())
                return functionExpression(false);
            break;
        case Tok::PrivateName: {
            // Only `#x in o`.
            NodePtr n = makeNode(NK::Identifier, line);
            n->text = ascii("#") + cur_.text;
            n->isPrivate = true;
            advance();
            if (!at(Tok::In))
                fail("a private name is only valid in a member access or before 'in'");
            return n;
        }
        case Tok::New: {
            advance();
            if (eat(Tok::Dot)) {
                if (cur_.text != ascii("target"))
                    fail("expected 'new.target'");
                advance();
                return makeNode(NK::NewTarget, line);
            }
            NodePtr n = makeNode(NK::NewExpr, line);
            n->a = callOrMember(primary(), false);
            if (at(Tok::Lt) && looksLikeTypeArguments())
                n->typeParams = typeArguments();
            if (at(Tok::LParen))
                n->list = argumentList();
            return n;
        }
        case Tok::LParen: {
            advance();
            NodePtr inner = expression();
            expect(Tok::RParen, "')'");
            return inner;
        }
        default: break;
    }
    if (identifierLike(cur_.type)) {
        checkReference();
        NodePtr n = makeNode(NK::Identifier, line);
        n->text = cur_.text;
        advance();
        return n;
    }
    fail(std::string("unexpected ") + tokenName(cur_.type));
}

}  // namespace dyn
}  // namespace internal
}  // namespace script
