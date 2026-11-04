// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "script/internal/typed/parser.hpp"

#include <algorithm>

namespace script {
namespace internal {
namespace typed {

namespace {

/// Binding power of each binary operator, lowest first. `is` and `as` sit at
/// the relational level, which is what the language says.
int precedenceOf(Tok t) noexcept
{
    switch (t) {
        case Tok::OrOr: return 1;
        case Tok::AndAnd: return 2;
        case Tok::Pipe: return 3;
        case Tok::Caret: return 4;
        case Tok::Amp: return 5;
        case Tok::EqEq:
        case Tok::NotEq: return 6;
        case Tok::Lt:
        case Tok::Gt:
        case Tok::Le:
        case Tok::Ge:
        case Tok::Is:
        case Tok::As: return 7;
        case Tok::Shl:
        case Tok::Shr: return 8;
        case Tok::Plus:
        case Tok::Minus: return 9;
        case Tok::Star:
        case Tok::Slash:
        case Tok::Percent: return 10;
        default: return 0;
    }
}

/// True when a token can only begin a constant, never a type name. It is what
/// tells `case 0:` from `case Foo:` in a pattern.
bool constantStart(Tok t) noexcept
{
    switch (t) {
        case Tok::IntLit:
        case Tok::LongLit:
        case Tok::UIntLit:
        case Tok::ULongLit:
        case Tok::FloatLit:
        case Tok::DoubleLit:
        case Tok::DecimalLit:
        case Tok::CharLit:
        case Tok::StringLit:
        case Tok::InterpolatedLit:
        case Tok::True:
        case Tok::False:
        case Tok::Null:
        case Tok::Minus:
        case Tok::Plus: return true;
        default: return false;
    }
}

BinaryOp binaryOpOf(Tok t) noexcept
{
    switch (t) {
        case Tok::Plus: return BinaryOp::Add;
        case Tok::Minus: return BinaryOp::Subtract;
        case Tok::Star: return BinaryOp::Multiply;
        case Tok::Slash: return BinaryOp::Divide;        case Tok::Percent: return BinaryOp::Modulo;
        case Tok::Amp: return BinaryOp::And;
        case Tok::Pipe: return BinaryOp::Or;
        case Tok::Caret: return BinaryOp::Xor;
        case Tok::Shl: return BinaryOp::ShiftLeft;
        case Tok::Shr: return BinaryOp::ShiftRight;
        case Tok::EqEq: return BinaryOp::Equal;
        case Tok::NotEq: return BinaryOp::NotEqual;
        case Tok::Lt: return BinaryOp::Less;
        case Tok::Le: return BinaryOp::LessEqual;
        case Tok::Gt: return BinaryOp::Greater;
        case Tok::Ge: return BinaryOp::GreaterEqual;
        case Tok::AndAnd: return BinaryOp::LogicalAnd;
        case Tok::OrOr: return BinaryOp::LogicalOr;
        default: return BinaryOp::Add;
    }
}

bool compoundAssign(Tok t, BinaryOp& op) noexcept
{
    switch (t) {
        case Tok::PlusAssign: op = BinaryOp::Add; return true;
        case Tok::MinusAssign: op = BinaryOp::Subtract; return true;
        case Tok::StarAssign: op = BinaryOp::Multiply; return true;
        case Tok::SlashAssign: op = BinaryOp::Divide; return true;
        case Tok::PercentAssign: op = BinaryOp::Modulo; return true;
        case Tok::AmpAssign: op = BinaryOp::And; return true;
        case Tok::PipeAssign: op = BinaryOp::Or; return true;
        case Tok::CaretAssign: op = BinaryOp::Xor; return true;
        case Tok::ShlAssign: op = BinaryOp::ShiftLeft; return true;
        case Tok::ShrAssign: op = BinaryOp::ShiftRight; return true;
        case Tok::QuestionQuestionAssign: op = BinaryOp::Coalesce; return true;
        default: return false;
    }
}

/// Tokens that can begin an operand. Used to tell `(T)x` from `(a)-b`.
bool startsOperand(Tok t) noexcept
{
    switch (t) {
        case Tok::IntLit:
        case Tok::LongLit:
        case Tok::UIntLit:
        case Tok::ULongLit:
        case Tok::FloatLit:
        case Tok::DoubleLit:
        case Tok::DecimalLit:
        case Tok::CharLit:
        case Tok::StringLit:
        case Tok::InterpolatedLit:
        case Tok::LParen:
        case Tok::Tilde:
        case Tok::Bang:
        case Tok::This:
        case Tok::Base:
        case Tok::New:
        case Tok::True:
        case Tok::False:
        case Tok::Null:
        case Tok::Typeof:
        case Tok::Default:
        case Tok::Checked:
        case Tok::Unchecked: return true;
        default: return identifierLike(t) || typeKeyword(t);
    }
}

}  // namespace

const char* operatorText(BinaryOp op) noexcept
{
    switch (op) {
        case BinaryOp::Add: return "+";
        case BinaryOp::Subtract: return "-";
        case BinaryOp::Multiply: return "*";
        case BinaryOp::Divide: return "/";
        case BinaryOp::Modulo: return "%";
        case BinaryOp::And: return "&";
        case BinaryOp::Or: return "|";
        case BinaryOp::Xor: return "^";
        case BinaryOp::ShiftLeft: return "<<";
        case BinaryOp::ShiftRight: return ">>";
        case BinaryOp::Equal: return "==";
        case BinaryOp::NotEqual: return "!=";
        case BinaryOp::Less: return "<";
        case BinaryOp::LessEqual: return "<=";
        case BinaryOp::Greater: return ">";
        case BinaryOp::GreaterEqual: return ">=";
        case BinaryOp::LogicalAnd: return "&&";
        case BinaryOp::LogicalOr: return "||";
        case BinaryOp::Coalesce: return "??";
    }
    return "?";
}

const char* operatorText(UnaryOp op) noexcept
{
    switch (op) {
        case UnaryOp::Plus: return "+";
        case UnaryOp::Minus: return "-";
        case UnaryOp::Not: return "!";
        case UnaryOp::Complement: return "~";
        case UnaryOp::PreIncrement:
        case UnaryOp::PostIncrement: return "++";
        case UnaryOp::PreDecrement:
        case UnaryOp::PostDecrement: return "--";
    }
    return "?";
}

// -------------------------------------------------------------- parser ---

Parser::Parser(std::string_view source, std::string name, Arena& arena)
    : arena_(arena), name_(std::move(name))
{
    Lexer lexer(source, name_);
    tokens_ = lexer.tokenize();
}

const Token& Parser::expect(Tok t, const char* what)
{
    if (!check(t))
        failAt(peek(), std::string("expected ") + what + ", found " + tokenName(type()));
    return tokens_[pos_++];
}

String Parser::expectName(const char* what)
{
    if (!identifierLike(type()))
        failAt(peek(), std::string("expected ") + what + ", found " + tokenName(type()));
    return advance().text;
}

void Parser::fail(const std::string& message) const
{
    failAt(peek(), message);
}

void Parser::failAt(const Token& token, const std::string& message) const
{
    throw Error(ErrorCode::SyntaxError, name_ + "(" + std::to_string(token.line) + "," +
                                            std::to_string(token.column) +
                                            "): error CS1003: " + message);
}

void Parser::splitShift()
{
    if (type() != Tok::Shr)
        return;
    // Leave a `>` where the `>>` was and put a second one after it, so the
    // nested type argument list and its parent each close their own.
    Token second = tokens_[pos_];
    second.type = Tok::Gt;
    ++second.column;
    tokens_[pos_].type = Tok::Gt;
    tokens_.insert(tokens_.begin() + std::ptrdiff_t(pos_) + 1, second);
}

// --------------------------------------------------------- declarations ---

CompilationUnit* Parser::parseUnit()
{
    CompilationUnit* unit = arena_.make<CompilationUnit>();
    unit->sourceName = name_;
    parseNamespaceBody(String(), unit, false);
    if (!check(Tok::Eof))
        fail("expected a declaration or a statement");
    return unit;
}

/// `using System;` and `using static Math;` are directives. `using (r)` and
/// `using var r = ...;` are statements, which at top level can appear in the
/// same place -- what tells them apart is that a directive is a dotted name
/// and then a semicolon.
bool Parser::looksLikeUsingDirective() const
{
    std::size_t i = pos_ + 1;
    if (i < tokens_.size() && tokens_[i].type == Tok::Static)
        ++i;
    if (i >= tokens_.size() || tokens_[i].type == Tok::LParen || tokens_[i].type == Tok::Var)
        return false;
    while (i < tokens_.size() && identifierLike(tokens_[i].type)) {
        ++i;
        if (i < tokens_.size() && tokens_[i].type == Tok::Dot) {
            ++i;
            continue;
        }
        break;
    }
    return i < tokens_.size() && tokens_[i].type == Tok::Semicolon;
}

void Parser::parseNamespaceBody(const String& prefix, CompilationUnit* unit, bool braced)
{
    while (!check(Tok::Eof) && !(braced && check(Tok::RBrace))) {
        if (check(Tok::Using) && looksLikeUsingDirective()) {
            advance();
            const bool isStatic = match(Tok::Static);
            String path;
            for (;;) {
                if (!identifierLike(type()) && !check(Tok::Global))
                    fail("expected a namespace name");
                if (!path.empty())
                    path.push_back(u'.');
                path += advance().text;
                if (!match(Tok::Dot))
                    break;
            }
            expect(Tok::Semicolon, "';'");
            if (isStatic)
                unit->staticUsings.push_back(path);
            else
                unit->usings.push_back(path);
            continue;
        }
        if (check(Tok::Namespace)) {
            advance();
            String name;
            for (;;) {
                if (!name.empty())
                    name.push_back(u'.');
                name += expectName("a namespace name");
                if (!match(Tok::Dot))
                    break;
            }
            const String inner = prefix.empty() ? name : prefix + u"." + name;
            if (match(Tok::Semicolon)) {
                // A file-scoped namespace covers everything that follows.
                parseNamespaceBody(inner, unit, braced);
                return;
            }
            expect(Tok::LBrace, "'{'");
            parseNamespaceBody(inner, unit, true);
            expect(Tok::RBrace, "'}'");
            continue;
        }

        const std::size_t mark = pos_;
        const std::uint16_t modifiers = parseModifiers();
        if (check(Tok::Class) || check(Tok::Struct) || check(Tok::Interface) ||
            check(Tok::Enum) || check(Tok::Delegate) ||
            (check(Tok::Record) && (identifierLike(type(1)) || type(1) == Tok::Struct ||
                                    type(1) == Tok::Class))) {
            unit->types.push_back(parseTypeDecl(prefix, modifiers));
            continue;
        }
        if (modifiers != 0 && modifiers != ModConst) {
            // `static` and friends only introduce a type at this level.
            pos_ = mark;
            fail("expected a type declaration");
        }
        pos_ = mark;

        // A function declared beside the top-level statements: `int F(int x)
        // => x * 2;`. It becomes a static method of the synthesized class.
        std::size_t afterType = 0;
        if (looksLikeType(pos_, afterType) && afterType + 1 < tokens_.size() &&
            identifierLike(tokens_[afterType].type) &&
            tokens_[afterType + 1].type == Tok::LParen) {
            const std::uint32_t line = peek().line;
            TypeRef* returnType = parseType();
            String name = expectName("a method name");
            unit->topLevelMethods.push_back(
                parseMethodRest(nullptr, ModStatic | ModPublic, returnType, std::move(name),
                                line));
            continue;
        }
        unit->topLevel.push_back(parseStatement());
    }
}

std::uint16_t Parser::parseModifiers()
{
    std::uint16_t modifiers = 0;
    for (;;) {
        switch (type()) {
            case Tok::Public: modifiers |= ModPublic; break;
            case Tok::Private: modifiers |= ModPrivate; break;
            case Tok::Protected: modifiers |= ModProtected; break;
            case Tok::Internal: modifiers |= ModInternal; break;
            case Tok::Static: modifiers |= ModStatic; break;
            case Tok::Virtual: modifiers |= ModVirtual; break;
            case Tok::Override: modifiers |= ModOverride; break;
            case Tok::Abstract: modifiers |= ModAbstract; break;
            case Tok::Sealed: modifiers |= ModSealed; break;
            case Tok::Readonly: modifiers |= ModReadonly; break;
            case Tok::Extern: modifiers |= ModExtern; break;
            case Tok::Const: modifiers |= ModConst; break;
            case Tok::Volatile: break;
            case Tok::Required:
                // Contextual: a modifier only before a member's type.
                if (type(1) == Tok::Assign || type(1) == Tok::Dot || type(1) == Tok::Semicolon)
                    return modifiers;
                modifiers |= ModRequired;
                break;
            case Tok::Partial:
                // `partial` is contextual: only a modifier before a type.
                if (type(1) != Tok::Class && type(1) != Tok::Struct && type(1) != Tok::Interface)
                    return modifiers;
                modifiers |= ModPartial;
                break;
            default: return modifiers;
        }
        advance();
    }
}

TypeDecl* Parser::parseTypeDecl(const String& prefix, std::uint16_t modifiers)
{
    TypeDecl* decl = arena_.make<TypeDecl>();
    decl->line = peek().line;
    decl->modifiers = modifiers;

    switch (advance().type) {
        case Tok::Class: decl->kind = DeclKind::Class; break;
        case Tok::Struct: decl->kind = DeclKind::Struct; break;
        case Tok::Interface: decl->kind = DeclKind::Interface; break;
        case Tok::Enum: decl->kind = DeclKind::Enum; break;
        case Tok::Record:
            decl->isRecord = true;
            // `record struct` is a value type; `record class` and a bare
            // `record` are reference types.
            if (match(Tok::Struct))
                decl->kind = DeclKind::Struct;
            else if (match(Tok::Class))
                decl->kind = DeclKind::Class;
            else
                decl->kind = DeclKind::Class;
            break;
        default: decl->kind = DeclKind::Delegate; break;
    }

    if (decl->kind == DeclKind::Delegate) {
        decl->returnTypeRef = parseType();
        decl->name = expectName("a delegate name");
        decl->fullName = prefix.empty() ? decl->name : prefix + u"." + decl->name;
        decl->typeParams = parseTypeParams();
        decl->params = parseParams();
        decl->constraints = parseConstraints();
        expect(Tok::Semicolon, "';'");
        return decl;
    }

    decl->name = expectName("a type name");
    decl->fullName = prefix.empty() ? decl->name : prefix + u"." + decl->name;
    decl->typeParams = parseTypeParams();

    // `record Point(int X, int Y)`: the primary constructor's parameters.
    if (check(Tok::LParen)) {
        decl->params = parseParams();
        decl->isPrimaryConstructed = true;
    }

    if (match(Tok::Colon)) {
        do {
            decl->baseTypes.push_back(parseType());
        } while (match(Tok::Comma));
    }
    decl->constraints = parseConstraints();

    // A positional record may end at a semicolon instead of a body.
    if (decl->isRecord && match(Tok::Semicolon))
        return decl;

    expect(Tok::LBrace, "'{'");
    ++depth_;
    if (decl->kind == DeclKind::Enum)
        parseEnumBody(decl);
    else
        parseClassBody(decl);
    --depth_;
    expect(Tok::RBrace, "'}'");
    return decl;
}

void Parser::parseEnumBody(TypeDecl* decl)
{
    while (!check(Tok::RBrace) && !check(Tok::Eof)) {
        EnumMemberDecl member;
        member.line = peek().line;
        member.name = expectName("an enum member name");
        if (match(Tok::Assign))
            member.value = parseExpression();
        decl->enumMembers.push_back(member);
        if (!match(Tok::Comma))
            break;
    }
}

void Parser::parseClassBody(TypeDecl* decl)
{
    while (!check(Tok::RBrace) && !check(Tok::Eof))
        parseMember(decl);
}

void Parser::parseMember(TypeDecl* decl)
{
    const std::uint32_t line = peek().line;
    const std::uint16_t modifiers = parseModifiers();

    if (check(Tok::Class) || check(Tok::Struct) || check(Tok::Interface) || check(Tok::Enum) ||
        check(Tok::Delegate))
        fail("nested type declarations are not supported");
    if (check(Tok::Tilde))
        fail("finalizers are not supported; use IDisposable");

    // A constructor is the only member whose name is the type's own and which
    // has no return type.
    if (identifierLike(type()) && peek().text == decl->name && type(1) == Tok::LParen) {
        advance();
        MethodDecl* ctor = arena_.make<MethodDecl>();
        ctor->line = line;
        ctor->name = ascii(".ctor");
        ctor->modifiers = modifiers;
        ctor->isConstructor = true;
        ctor->owner = decl;
        ctor->params = parseParams();
        if (match(Tok::Colon)) {
            if (match(Tok::Base))
                ctor->chainsToBase = true;
            else if (match(Tok::This))
                ctor->chainsToThis = true;
            else
                fail("expected 'base' or 'this'");
            ctor->chainArgs = parseArguments();
        }
        if (check(Tok::LBrace))
            ctor->body = parseBlock();
        else
            expect(Tok::Semicolon, "';'");
        decl->methods.push_back(ctor);
        return;
    }

    TypeRef* memberType = parseType();
    if (check(Tok::Operator))
        fail("operator overloading is not supported");
    if (check(Tok::This))
        fail("indexers are not supported");
    if (!identifierLike(type()))
        fail("expected a member name");
    String name = advance().text;

    std::vector<String> typeParams;
    if (check(Tok::Lt)) {
        std::size_t end = 0;
        if (isTypeArgumentList(pos_, end))
            typeParams = parseTypeParams();
    }
    if (check(Tok::LParen)) {
        MethodDecl* method = parseMethodRest(decl, modifiers, memberType, name, line);
        method->typeParams = std::move(typeParams);
        decl->methods.push_back(method);
        return;
    }
    if (!typeParams.empty())
        fail("only a method may declare type parameters");
    if (check(Tok::LBrace) || check(Tok::Arrow)) {
        parsePropertyRest(decl, modifiers, memberType, std::move(name), line);
        return;
    }

    // One or more fields.
    for (;;) {
        FieldDecl field;
        field.line = line;
        field.name = name;
        field.modifiers = modifiers;
        field.typeRef = memberType;
        if (match(Tok::Assign))
            field.init = parseExpression();
        decl->fields.push_back(std::move(field));
        if (!match(Tok::Comma))
            break;
        if (!identifierLike(type()))
            fail("expected a field name");
        name = advance().text;
    }
    expect(Tok::Semicolon, "';'");
}

MethodDecl* Parser::parseMethodRest(TypeDecl* decl, std::uint16_t modifiers, TypeRef* returnType,
                                    String name, std::uint32_t line)
{
    MethodDecl* method = arena_.make<MethodDecl>();
    method->line = line;
    method->name = std::move(name);
    method->modifiers = modifiers;
    method->returnTypeRef = returnType;
    method->owner = decl;
    method->params = parseParams();
    method->constraints = parseConstraints();

    if (match(Tok::Arrow)) {
        method->expressionBody = parseExpression();
        expect(Tok::Semicolon, "';'");
    } else if (check(Tok::LBrace)) {
        method->body = parseBlock();
    } else {
        expect(Tok::Semicolon, "';'");
    }
    return method;
}

void Parser::parsePropertyRest(TypeDecl* decl, std::uint16_t modifiers, TypeRef* typeRef,
                               String name, std::uint32_t line)
{
    PropertyDecl prop;
    prop.line = line;
    prop.name = std::move(name);
    prop.modifiers = modifiers;
    prop.isRequired = (modifiers & ModRequired) != 0;
    prop.typeRef = typeRef;

    if (match(Tok::Arrow)) {
        prop.hasGetter = true;
        prop.getterExpr = parseExpression();
        expect(Tok::Semicolon, "';'");
        decl->props.push_back(std::move(prop));
        return;
    }

    expect(Tok::LBrace, "'{'");
    bool autoGet = false;
    bool autoSet = false;
    while (!check(Tok::RBrace) && !check(Tok::Eof)) {
        parseModifiers();  // accessor accessibility is parsed and ignored
        if (match(Tok::Get)) {
            prop.hasGetter = true;
            if (match(Tok::Arrow)) {
                prop.getterExpr = parseExpression();
                expect(Tok::Semicolon, "';'");
            } else if (check(Tok::LBrace)) {
                prop.getterBody = parseBlock();
            } else {
                expect(Tok::Semicolon, "';'");
                autoGet = true;
            }
        } else if (check(Tok::Set) || check(Tok::Init)) {
            prop.isInitOnly = check(Tok::Init);
            advance();
            prop.hasSetter = true;
            if (check(Tok::LBrace)) {
                prop.setterBody = parseBlock();
            } else if (match(Tok::Arrow)) {
                ExprStmt* s = arena_.make<ExprStmt>();
                s->line = peek().line;
                s->expr = parseExpression();
                BlockStmt* block = arena_.make<BlockStmt>();
                block->line = s->line;
                block->statements.push_back(s);
                prop.setterBody = block;
                expect(Tok::Semicolon, "';'");
            } else {
                expect(Tok::Semicolon, "';'");
                autoSet = true;
            }
        } else {
            fail("expected 'get' or 'set'");
        }
    }
    expect(Tok::RBrace, "'}'");

    prop.autoProperty = (autoGet || autoSet) && !prop.getterBody && !prop.getterExpr &&
                        !prop.setterBody;
    if (prop.autoProperty && match(Tok::Assign)) {
        prop.init = parseExpression();
        expect(Tok::Semicolon, "';'");
    }
    decl->props.push_back(std::move(prop));
}

std::vector<String> Parser::parseTypeParams()
{
    std::vector<String> params;
    if (!match(Tok::Lt))
        return params;
    do {
        // Variance is accepted and ignored: with reified instantiation there
        // is no shared representation for it to describe.
        if (check(Tok::Out) || check(Tok::In))
            advance();
        if (!identifierLike(type()))
            fail("expected a type parameter name");
        const String name = advance().text;
        for (const String& seen : params) {
            if (seen == name)
                fail("duplicate type parameter '" + toUtf8(name) + "'");
        }
        params.push_back(name);
    } while (match(Tok::Comma));
    if (check(Tok::Shr))
        splitShift();
    expect(Tok::Gt, "'>'");
    return params;
}

std::vector<TypeConstraint> Parser::parseConstraints()
{
    std::vector<TypeConstraint> constraints;
    while (check(Tok::Where)) {
        advance();
        TypeConstraint constraint;
        constraint.line = peek().line;
        constraint.column = peek().column;
        if (!identifierLike(type()))
            fail("expected a type parameter name");
        constraint.name = advance().text;
        expect(Tok::Colon, "':'");
        do {
            if (check(Tok::Class) && type(1) != Tok::Dot) {
                advance();
                constraint.hasClass = true;
            } else if (check(Tok::Struct)) {
                advance();
                constraint.hasStruct = true;
            } else if (check(Tok::New) && type(1) == Tok::LParen) {
                advance();
                advance();
                expect(Tok::RParen, "')'");
                constraint.hasNew = true;
            } else {
                constraint.types.push_back(parseType());
            }
        } while (match(Tok::Comma));
        constraints.push_back(std::move(constraint));
    }
    return constraints;
}

std::vector<ParamDecl> Parser::parseParams()
{
    std::vector<ParamDecl> params;
    expect(Tok::LParen, "'('");
    if (match(Tok::RParen))
        return params;
    do {
        ParamDecl param;
        param.line = peek().line;
        if (match(Tok::Ref))
            param.mode = ParamMode::Ref;
        else if (match(Tok::Out))
            param.mode = ParamMode::Out;
        else if (match(Tok::Params))
            param.mode = ParamMode::Params;
        else if (match(Tok::In))
            param.mode = ParamMode::Ref;
        param.typeRef = parseType();
        if (!identifierLike(type()))
            fail("expected a parameter name");
        param.name = advance().text;
        if (match(Tok::Assign))
            param.defaultValue = parseExpression();
        params.push_back(std::move(param));
    } while (match(Tok::Comma));
    expect(Tok::RParen, "')'");
    return params;
}

// ---------------------------------------------------------------- types ---

bool Parser::isTypeArgumentList(std::size_t at, std::size_t& end) const
{
    if (at >= tokens_.size() || tokens_[at].type != Tok::Lt)
        return false;
    int depth = 1;
    std::size_t i = at + 1;
    while (i < tokens_.size() && depth > 0) {
        const Tok t = tokens_[i].type;
        if (t == Tok::Lt) {
            ++depth;
        } else if (t == Tok::Gt) {
            --depth;
        } else if (t == Tok::Shr) {
            // `>>` closes two lists, unless only one is open here -- then the
            // second `>` belongs to whatever list encloses this one.
            depth -= depth >= 2 ? 2 : 1;
        } else if (t == Tok::Comma || t == Tok::Dot || t == Tok::Question ||
                   t == Tok::LBracket || t == Tok::RBracket || typeKeyword(t) ||
                   identifierLike(t)) {
            // part of a type
        } else {
            return false;
        }
        ++i;
    }
    if (depth != 0)
        return false;
    end = i;
    return true;
}

bool Parser::looksLikeType(std::size_t at, std::size_t& end) const
{
    std::size_t i = at;
    if (i >= tokens_.size())
        return false;
    // `(int, string)` and `(int n, string s)` are types too.
    if (tokens_[i].type == Tok::LParen) {
        int depth = 0;
        bool sawComma = false;
        for (std::size_t j = i; j < tokens_.size(); ++j) {
            if (tokens_[j].type == Tok::LParen) {
                ++depth;
            } else if (tokens_[j].type == Tok::RParen) {
                if (--depth == 0) {
                    if (!sawComma)
                        return false;
                    i = j + 1;
                    break;
                }
            } else if (depth == 1 && tokens_[j].type == Tok::Comma) {
                sawComma = true;
            } else if (depth == 1 && !identifierLike(tokens_[j].type) &&
                       !typeKeyword(tokens_[j].type) && tokens_[j].type != Tok::Dot &&
                       tokens_[j].type != Tok::LBracket && tokens_[j].type != Tok::RBracket &&
                       tokens_[j].type != Tok::Lt && tokens_[j].type != Tok::Gt &&
                       tokens_[j].type != Tok::Question) {
                return false;
            }
        }
        if (i == at)
            return false;
        while (i + 1 < tokens_.size() && tokens_[i].type == Tok::LBracket &&
               tokens_[i + 1].type == Tok::RBracket)
            i += 2;
        end = i;
        return true;
    }
    if (typeKeyword(tokens_[i].type)) {
        ++i;
    } else if (identifierLike(tokens_[i].type)) {
        ++i;
        while (i + 1 < tokens_.size() && tokens_[i].type == Tok::Dot &&
               identifierLike(tokens_[i + 1].type))
            i += 2;
        if (i < tokens_.size() && tokens_[i].type == Tok::Lt) {
            std::size_t after = 0;
            if (!isTypeArgumentList(i, after))
                return false;
            i = after;
        }
    } else {
        return false;
    }
    if (i < tokens_.size() && tokens_[i].type == Tok::Question && i + 1 < tokens_.size() &&
        (identifierLike(tokens_[i + 1].type) || tokens_[i + 1].type == Tok::LBracket))
        ++i;
    while (i + 1 < tokens_.size() && tokens_[i].type == Tok::LBracket &&
           tokens_[i + 1].type == Tok::RBracket)
        i += 2;
    end = i;
    return true;
}

TypeRef* Parser::parseType()
{
    TypeRef* ref = arena_.make<TypeRef>();
    ref->line = peek().line;
    ref->column = peek().column;

    // `(int, string)` is a tuple type; it becomes the same constructed type
    // the tuple expression produces, so it is spelled as one here.
    if (check(Tok::LParen)) {
        advance();
        ref->isTuple = true;
        do {
            TypeRef* element = parseType();
            ref->typeArgs.push_back(element);
            if (identifierLike(type()) && type(1) != Tok::Comma && type(1) != Tok::RParen)
                ref->tupleNames.push_back(String());
            else if (identifierLike(type()))
                ref->tupleNames.push_back(advance().text);
            else
                ref->tupleNames.push_back(String());
        } while (match(Tok::Comma));
        expect(Tok::RParen, "')'");
        while (match(Tok::Question))
            ref->nullable = true;
        while (check(Tok::LBracket) && type(1) == Tok::RBracket) {
            advance();
            advance();
            ++ref->arrayRank;
        }
        return ref;
    }

    if (typeKeyword(type())) {
        ref->predefined = advance().type;
    } else if (check(Tok::Var)) {
        advance();
        ref->isVar = true;
    } else {
        for (;;) {
            if (!identifierLike(type()))
                fail("expected a type name");
            ref->path.push_back(advance().text);
            std::size_t end = 0;
            if (check(Tok::Lt) && isTypeArgumentList(pos_, end)) {
                advance();
                do {
                    ref->typeArgs.push_back(parseType());
                } while (match(Tok::Comma));
                splitShift();
                expect(Tok::Gt, "'>'");
            }
            if (!check(Tok::Dot) || !identifierLike(type(1)))
                break;
            advance();
        }
    }

    if (check(Tok::Question) && (identifierLike(type(1)) || type(1) == Tok::LBracket ||
                                 type(1) == Tok::Gt || type(1) == Tok::Comma ||
                                 type(1) == Tok::RParen)) {
        advance();
        ref->nullable = true;
    }
    while (check(Tok::LBracket) && type(1) == Tok::RBracket) {
        advance();
        advance();
        ++ref->arrayRank;
    }
    return ref;
}

TypeRef* Parser::tryParseType()
{
    std::size_t end = 0;
    if (!looksLikeType(pos_, end))
        return nullptr;
    return parseType();
}

// ----------------------------------------------------------- statements ---

BlockStmt* Parser::parseBlock()
{
    BlockStmt* block = newStmt<BlockStmt>(peek().line);
    expect(Tok::LBrace, "'{'");
    while (!check(Tok::RBrace) && !check(Tok::Eof))
        block->statements.push_back(parseStatement());
    expect(Tok::RBrace, "'}'");
    return block;
}

bool Parser::looksLikeLocalDecl() const
{
    if (check(Tok::Const))
        return true;
    if (check(Tok::Var) && identifierLike(type(1)))
        return true;
    if (typeKeyword(type())) {
        // `int.Parse(...)` is an expression; `int x` and `int[] a` are not.
        return type(1) != Tok::Dot;
    }
    std::size_t end = 0;
    if (!looksLikeType(pos_, end) || end >= tokens_.size())
        return false;
    if (!identifierLike(tokens_[end].type))
        return false;
    const Tok after = end + 1 < tokens_.size() ? tokens_[end + 1].type : Tok::Eof;
    return after == Tok::Assign || after == Tok::Semicolon || after == Tok::Comma ||
           after == Tok::In;
}

Stmt* Parser::parseStatement()
{
    const std::uint32_t line = peek().line;
    switch (type()) {
        case Tok::LBrace: return parseBlock();
        case Tok::Semicolon: advance(); return newStmt<EmptyStmt>(line);
        case Tok::If: return parseIf();
        case Tok::While: return parseWhile();
        case Tok::Do: return parseDo();
        case Tok::For: return parseFor();
        case Tok::Foreach: return parseForEach();
        case Tok::Switch: return parseSwitch();
        case Tok::Try: return parseTry();
        case Tok::Break: {
            advance();
            expect(Tok::Semicolon, "';'");
            return newStmt<BreakStmt>(line);
        }
        case Tok::Continue: {
            advance();
            expect(Tok::Semicolon, "';'");
            return newStmt<ContinueStmt>(line);
        }
        case Tok::Return: {
            advance();
            ReturnStmt* node = newStmt<ReturnStmt>(line);
            if (!check(Tok::Semicolon))
                node->value = parseExpression();
            expect(Tok::Semicolon, "';'");
            return node;
        }
        case Tok::Throw: {
            advance();
            ThrowStmt* node = newStmt<ThrowStmt>(line);
            if (!check(Tok::Semicolon))
                node->value = parseExpression();
            expect(Tok::Semicolon, "';'");
            return node;
        }
        case Tok::Checked:
        case Tok::Unchecked:
            if (type(1) == Tok::LBrace) {
                advance();
                return parseBlock();
            }
            break;
        case Tok::Lock:
            fail("lock statements are not supported; a Context is single-threaded");
        case Tok::Goto:
            fail("goto is not supported");
        case Tok::Using: {
            // `using (r) body` and `using var r = ...;` -- the declaration
            // form is scoped to the rest of the block, which the checker
            // arranges by rewriting it.
            advance();
            UsingStmt* node = newStmt<UsingStmt>(line);
            if (match(Tok::LParen)) {
                if (looksLikeLocalDecl())
                    node->declaration = parseLocalDecl(false);
                else
                    node->resource = parseExpression();
                expect(Tok::RParen, "')'");
                node->body = parseStatement();
                return node;
            }
            node->declaration = parseLocalDecl(true);
            static_cast<LocalDeclStmt*>(node->declaration)->isUsing = true;
            return node;
        }
        case Tok::Yield:
            if (type(1) == Tok::Return || type(1) == Tok::Break) {
                advance();
                YieldStmt* node = newStmt<YieldStmt>(line);
                if (match(Tok::Break)) {
                    node->isBreak = true;
                } else {
                    advance();
                    node->value = parseExpression();
                }
                expect(Tok::Semicolon, "';'");
                return node;
            }
            break;
        default: break;
    }

    if (looksLikeLocalFunction()) {
        LocalFunctionStmt* node = newStmt<LocalFunctionStmt>(line);
        const std::uint16_t modifiers = parseModifiers();
        TypeRef* returnType = parseType();
        String name = advance().text;
        std::vector<String> typeParams;
        std::size_t end = 0;
        if (check(Tok::Lt) && isTypeArgumentList(pos_, end))
            typeParams = parseTypeParams();
        MethodDecl* method = parseMethodRest(nullptr, modifiers | ModStatic, returnType,
                                             std::move(name), line);
        method->typeParams = std::move(typeParams);
        method->isLocalFunction = true;
        node->method = method;
        return node;
    }

    if (looksLikeDeconstruction())
        return parseDeconstruction();

    if (looksLikeLocalDecl())
        return parseLocalDecl(true);

    ExprStmt* node = newStmt<ExprStmt>(line);
    node->expr = parseExpression();
    expect(Tok::Semicolon, "';'");
    return node;
}

/// `int Helper(int x) { }` inside a method body: a type, a name, a parameter
/// list, and then a body rather than a semicolon.
bool Parser::looksLikeLocalFunction() const
{
    std::size_t i = pos_;
    while (i < tokens_.size() &&
           (tokens_[i].type == Tok::Static || tokens_[i].type == Tok::Async))
        ++i;
    std::size_t end = 0;
    if (!looksLikeType(i, end) || end >= tokens_.size())
        return false;
    if (!identifierLike(tokens_[end].type))
        return false;
    std::size_t after = end + 1;
    if (after < tokens_.size() && tokens_[after].type == Tok::Lt) {
        std::size_t close = 0;
        if (!isTypeArgumentList(after, close))
            return false;
        after = close;
    }
    if (after >= tokens_.size() || tokens_[after].type != Tok::LParen)
        return false;
    // Find the matching `)` and see whether a body follows.
    int depth = 0;
    for (std::size_t j = after; j < tokens_.size(); ++j) {
        if (tokens_[j].type == Tok::LParen) {
            ++depth;
        } else if (tokens_[j].type == Tok::RParen) {
            if (--depth == 0) {
                const std::size_t next = j + 1;
                if (next >= tokens_.size())
                    return false;
                return tokens_[next].type == Tok::LBrace || tokens_[next].type == Tok::Arrow ||
                       tokens_[next].type == Tok::Where;
            }
        }
    }
    return false;
}

/// `var (a, b) = ...` or `(int a, var b) = ...`.
bool Parser::looksLikeDeconstruction(Tok follow) const
{
    std::size_t i = pos_;
    if (i < tokens_.size() && tokens_[i].type == Tok::Var && i + 1 < tokens_.size() &&
        tokens_[i + 1].type == Tok::LParen) {
        i += 1;
    } else if (i >= tokens_.size() || tokens_[i].type != Tok::LParen) {
        return false;
    }
    int depth = 0;
    for (std::size_t j = i; j < tokens_.size(); ++j) {
        if (tokens_[j].type == Tok::LParen) {
            ++depth;
        } else if (tokens_[j].type == Tok::RParen) {
            if (--depth == 0)
                return j + 1 < tokens_.size() && tokens_[j + 1].type == follow;
        } else if (depth == 0) {
            return false;
        }
    }
    return false;
}

Deconstruction* Parser::parseDeconstructionTargets()
{
    Deconstruction* node = arena_.make<Deconstruction>();
    if (check(Tok::Var))
        node->typeRef = parseType();
    expect(Tok::LParen, "'('");
    do {
        DeconstructTarget target;
        target.line = peek().line;
        if (check(Tok::LParen) || (check(Tok::Var) && type(1) == Tok::LParen)) {
            target.nested = parseDeconstructionTargets();
        } else if (identifierLike(type()) && peek().text == u"_" &&
                   (type(1) == Tok::Comma || type(1) == Tok::RParen)) {
            advance();
            target.discard = true;
        } else {
            std::size_t end = 0;
            if (!node->typeRef && looksLikeType(pos_, end) && end < tokens_.size() &&
                identifierLike(tokens_[end].type))
                target.typeRef = parseType();
            if (!identifierLike(type()))
                fail("expected a name");
            target.name = advance().text;
        }
        node->targets.push_back(std::move(target));
    } while (match(Tok::Comma));
    expect(Tok::RParen, "')'");
    return node;
}

Stmt* Parser::parseDeconstruction()
{
    LocalDeclStmt* decl = newStmt<LocalDeclStmt>(peek().line);
    decl->deconstruction = parseDeconstructionTargets();
    expect(Tok::Assign, "'='");
    decl->deconstructionValue = parseExpression();
    expect(Tok::Semicolon, "';'");
    return decl;
}

Stmt* Parser::parseLocalDecl(bool expectSemicolon)
{
    LocalDeclStmt* decl = newStmt<LocalDeclStmt>(peek().line);
    decl->isConst = match(Tok::Const);
    decl->typeRef = parseType();
    do {
        VarDeclarator var;
        var.line = peek().line;
        if (!identifierLike(type()))
            fail("expected a variable name");
        var.name = advance().text;
        if (match(Tok::Assign)) {
            if (check(Tok::LBrace)) {
                // `int[] a = { 1, 2 }`: an array initializer without `new`.
                ArrayCreationExpr* array = here<ArrayCreationExpr>();
                TypeRef* element = arena_.make<TypeRef>(*decl->typeRef);
                if (element->arrayRank > 0)
                    --element->arrayRank;
                array->elementTypeRef = element;
                array->hasInitializer = true;
                advance();
                while (!check(Tok::RBrace) && !check(Tok::Eof)) {
                    array->elements.push_back(parseExpression());
                    if (!match(Tok::Comma))
                        break;
                }
                expect(Tok::RBrace, "'}'");
                var.init = array;
            } else {
                var.init = parseExpression();
            }
        }
        decl->vars.push_back(std::move(var));
    } while (match(Tok::Comma));
    if (expectSemicolon)
        expect(Tok::Semicolon, "';'");
    return decl;
}

Stmt* Parser::parseIf()
{
    IfStmt* node = newStmt<IfStmt>(peek().line);
    advance();
    expect(Tok::LParen, "'('");
    node->condition = parseExpression();
    expect(Tok::RParen, "')'");
    node->thenBranch = parseStatement();
    if (match(Tok::Else))
        node->elseBranch = parseStatement();
    return node;
}

Stmt* Parser::parseWhile()
{
    WhileStmt* node = newStmt<WhileStmt>(peek().line);
    advance();
    expect(Tok::LParen, "'('");
    node->condition = parseExpression();
    expect(Tok::RParen, "')'");
    node->body = parseStatement();
    return node;
}

Stmt* Parser::parseDo()
{
    DoWhileStmt* node = newStmt<DoWhileStmt>(peek().line);
    advance();
    node->body = parseStatement();
    expect(Tok::While, "'while'");
    expect(Tok::LParen, "'('");
    node->condition = parseExpression();
    expect(Tok::RParen, "')'");
    expect(Tok::Semicolon, "';'");
    return node;
}

Stmt* Parser::parseFor()
{
    ForStmt* node = newStmt<ForStmt>(peek().line);
    advance();
    expect(Tok::LParen, "'('");
    if (!check(Tok::Semicolon)) {
        if (looksLikeLocalDecl()) {
            node->init.push_back(parseLocalDecl(false));
        } else {
            do {
                ExprStmt* s = newStmt<ExprStmt>(peek().line);
                s->expr = parseExpression();
                node->init.push_back(s);
            } while (match(Tok::Comma));
        }
    }
    expect(Tok::Semicolon, "';'");
    if (!check(Tok::Semicolon))
        node->condition = parseExpression();
    expect(Tok::Semicolon, "';'");
    if (!check(Tok::RParen)) {
        do {
            node->update.push_back(parseExpression());
        } while (match(Tok::Comma));
    }
    expect(Tok::RParen, "')'");
    node->body = parseStatement();
    return node;
}

Stmt* Parser::parseForEach()
{
    ForEachStmt* node = newStmt<ForEachStmt>(peek().line);
    advance();
    expect(Tok::LParen, "'('");
    // `foreach (var (k, v) in pairs) body` is
    // `foreach (var e in pairs) { var (k, v) = e; body }`, with an `e` no program can name.
    Deconstruction* targets = nullptr;
    if (looksLikeDeconstruction(Tok::In)) {
        const std::uint32_t line = peek().line;
        targets = parseDeconstructionTargets();
        node->typeRef = arena_.make<TypeRef>();
        node->typeRef->isVar = true;
        node->typeRef->line = line;
        node->name = ascii("$each" + std::to_string(pos_));
    } else {
        node->typeRef = parseType();
        if (!identifierLike(type()))
            fail("expected a variable name");
        node->name = advance().text;
    }
    expect(Tok::In, "'in'");
    node->sequence = parseExpression();
    expect(Tok::RParen, "')'");
    Stmt* const body = parseStatement();
    if (!targets) {
        node->body = body;
        return node;
    }
    LocalDeclStmt* decl = newStmt<LocalDeclStmt>(node->line);
    decl->deconstruction = targets;
    NameExpr* each = newExpr<NameExpr>(node->line, 1);
    each->name = node->name;
    decl->deconstructionValue = each;
    BlockStmt* block = newStmt<BlockStmt>(node->line);
    block->statements.push_back(decl);
    block->statements.push_back(body);
    node->body = block;
    return node;
}

/// A `case` label is the old constant form only when it starts like a literal
/// and has no pattern operator in it. A name is always a pattern: whether
/// `case Red:` means a type or an enum member is the checker's to decide.
bool Parser::looksLikeConstantLabel() const
{
    // `null` needs a reference comparison, which the constant chain cannot
    // make, so it goes through the pattern path.
    if (check(Tok::Null))
        return false;
    // A dotted name -- `Color.Red` -- is an enum member or a constant, and
    // keeping it on the constant chain keeps enum switches cheap. A bare name
    // is ambiguous, so it becomes a pattern and the checker decides.
    bool dotted = false;
    if (identifierLike(type())) {
        std::size_t i = pos_;
        while (i + 1 < tokens_.size() && identifierLike(tokens_[i].type) &&
               tokens_[i + 1].type == Tok::Dot) {
            dotted = true;
            i += 2;
        }
        if (!dotted || i >= tokens_.size() || !identifierLike(tokens_[i].type) ||
            i + 1 >= tokens_.size() || tokens_[i + 1].type != Tok::Colon)
            return false;
        return true;
    }
    if (!constantStart(type()))
        return false;
    int depth = 0;
    for (std::size_t i = pos_; i < tokens_.size(); ++i) {
        const Tok t = tokens_[i].type;
        if (t == Tok::LParen || t == Tok::LBracket || t == Tok::LBrace) {
            ++depth;
        } else if (t == Tok::RParen || t == Tok::RBracket || t == Tok::RBrace) {
            if (depth == 0)
                return true;
            --depth;
        } else if (depth == 0) {
            if (t == Tok::Colon)
                return true;
            if (t == Tok::When || t == Tok::And || t == Tok::Or || t == Tok::Not)
                return false;
            if (t == Tok::Semicolon || t == Tok::Eof)
                return true;
        }
    }
    return true;
}

Stmt* Parser::parseSwitch()
{    SwitchStmt* node = newStmt<SwitchStmt>(peek().line);
    advance();
    expect(Tok::LParen, "'('");
    node->subject = parseExpression();
    expect(Tok::RParen, "')'");
    expect(Tok::LBrace, "'{'");

    while (!check(Tok::RBrace) && !check(Tok::Eof)) {
        SwitchSection section;
        section.line = peek().line;
        for (;;) {
            if (match(Tok::Case)) {
                // A constant label is still a constant label: keeping the
                // simple form on `labels` is what lets the emitter build the
                // comparison chain it already knows how to build.
                if (looksLikeConstantLabel()) {
                    section.labels.push_back(parseExpression());
                } else {
                    SwitchCaseLabel label;
                    label.pattern = parsePattern();
                    if (match(Tok::When))
                        label.guard = parseExpression();
                    section.patterns.push_back(std::move(label));
                }
                expect(Tok::Colon, "':'");
            } else if (match(Tok::Default)) {
                section.isDefault = true;
                expect(Tok::Colon, "':'");
            } else {
                break;
            }
        }
        if (section.labels.empty() && section.patterns.empty() && !section.isDefault)
            fail("expected 'case' or 'default'");
        while (!check(Tok::Case) && !check(Tok::Default) && !check(Tok::RBrace) &&
               !check(Tok::Eof))
            section.statements.push_back(parseStatement());
        node->sections.push_back(std::move(section));
    }
    expect(Tok::RBrace, "'}'");
    return node;
}

Stmt* Parser::parseTry()
{
    TryStmt* node = newStmt<TryStmt>(peek().line);
    advance();
    node->body = parseBlock();
    while (check(Tok::Catch)) {
        CatchClause clause;
        clause.line = peek().line;
        advance();
        if (match(Tok::LParen)) {
            clause.typeRef = parseType();
            if (identifierLike(type()))
                clause.name = advance().text;
            expect(Tok::RParen, "')'");
        }
        if (match(Tok::When)) {
            expect(Tok::LParen, "'('");
            clause.filter = parseExpression();
            expect(Tok::RParen, "')'");
        }
        clause.body = parseBlock();
        node->catches.push_back(std::move(clause));
    }
    if (match(Tok::Finally))
        node->finallyBlock = parseBlock();
    if (node->catches.empty() && !node->finallyBlock)
        fail("expected 'catch' or 'finally'");
    return node;
}

// ---------------------------------------------------------- expressions ---

Expr* Parser::parseExpressionSource(std::string_view source, std::uint32_t line)
{
    Parser sub(source, name_, arena_);
    for (Token& token : sub.tokens_)
        token.line += line - 1;
    Expr* expr = sub.parseExpression();
    if (!sub.check(Tok::Eof))
        sub.fail("unexpected tokens in an interpolated expression");
    return expr;
}

Expr* Parser::parseExpression()
{
    return parseAssignment();
}

Expr* Parser::parseAssignment()
{
    if (looksLikeLambda())
        return parseLambda();

    Expr* left = parseConditional();
    BinaryOp op = BinaryOp::Add;
    if (check(Tok::Assign) || compoundAssign(type(), op)) {
        AssignmentExpr* node = newExpr<AssignmentExpr>(peek().line, peek().column);
        node->compound = !check(Tok::Assign);
        node->op = op;
        advance();
        node->target = left;
        node->value = parseAssignment();
        return node;
    }
    return left;
}

Expr* Parser::parseConditional()
{
    Expr* condition = parseCoalesce();
    if (!check(Tok::Question))
        return condition;

    ConditionalExpr* node = newExpr<ConditionalExpr>(peek().line, peek().column);
    advance();
    node->condition = condition;
    node->whenTrue = parseAssignment();
    expect(Tok::Colon, "':'");
    node->whenFalse = parseAssignment();
    return node;
}

Expr* Parser::parseCoalesce()
{
    Expr* left = parseBinary(1);
    if (!check(Tok::QuestionQuestion))
        return left;

    BinaryExpr* node = newExpr<BinaryExpr>(peek().line, peek().column);
    advance();
    node->op = BinaryOp::Coalesce;
    node->left = left;
    // Right associative: `a ?? b ?? c` is `a ?? (b ?? c)`.
    node->right = parseCoalesce();
    return node;
}

Expr* Parser::parseBinary(int precedence)
{
    Expr* left = parseUnary();
    for (;;) {
        const Tok t = type();
        const int power = precedenceOf(t);
        if (power < precedence)
            return left;

        if (t == Tok::Is || t == Tok::As) {
            TypeTestExpr* test = newExpr<TypeTestExpr>(peek().line, peek().column);
            test->isAs = t == Tok::As;
            advance();
            test->operand = left;
            if (test->isAs) {
                test->typeRef = parseType();
            } else {
                test->pattern = parsePattern();
                // `x is T` and `x is T name` are the shapes that predate
                // patterns; keep them on the fields the checker already reads.
                if (test->pattern->kind == PatternKind::Declaration &&
                    test->pattern->children.empty()) {
                    test->typeRef = test->pattern->typeRef;
                    test->bindingName = test->pattern->bindingName;
                    test->pattern = nullptr;
                }
            }
            left = test;
            continue;
        }

        BinaryExpr* node = newExpr<BinaryExpr>(peek().line, peek().column);
        node->op = binaryOpOf(t);
        advance();
        node->left = left;
        node->right = parseBinary(power + 1);
        left = node;
    }
}

Expr* Parser::parseUnary()
{
    const std::uint32_t line = peek().line;
    const std::uint32_t column = peek().column;
    switch (type()) {
        case Tok::Plus:
        case Tok::Minus:
        case Tok::Bang:
        case Tok::Tilde:
        case Tok::PlusPlus:
        case Tok::MinusMinus: {
            UnaryExpr* node = newExpr<UnaryExpr>(line, column);
            switch (advance().type) {
                case Tok::Plus: node->op = UnaryOp::Plus; break;
                case Tok::Minus: node->op = UnaryOp::Minus; break;
                case Tok::Bang: node->op = UnaryOp::Not; break;
                case Tok::Tilde: node->op = UnaryOp::Complement; break;
                case Tok::PlusPlus: node->op = UnaryOp::PreIncrement; break;
                default: node->op = UnaryOp::PreDecrement; break;
            }
            node->operand = parseUnary();
            return node;
        }
        case Tok::LParen:
            if (looksLikeCast()) {
                CastExpr* node = newExpr<CastExpr>(line, column);
                advance();
                node->typeRef = parseType();
                expect(Tok::RParen, "')'");
                node->operand = parseUnary();
                return node;
            }
            break;
        default: break;
    }
    return parsePostfix(parsePrimary());
}

bool Parser::looksLikeCast() const
{
    if (!check(Tok::LParen))
        return false;
    std::size_t end = 0;
    if (!looksLikeType(pos_ + 1, end))
        return false;
    if (end >= tokens_.size() || tokens_[end].type != Tok::RParen)
        return false;
    const Tok after = end + 1 < tokens_.size() ? tokens_[end + 1].type : Tok::Eof;
    // `(int)x` is unambiguous; `(Name)x` is a cast only when what follows
    // cannot continue an expression.
    if (typeKeyword(tokens_[pos_ + 1].type) && end == pos_ + 2)
        return after != Tok::Dot && after != Tok::RParen && after != Tok::Comma &&
               after != Tok::Semicolon;
    if (tokens_[pos_ + 1].type == Tok::LParen)
        return false;
    return startsOperand(after) && after != Tok::LParen;
}

bool Parser::looksLikeLambda() const
{
    if (identifierLike(type()) && type(1) == Tok::Arrow)
        return true;
    if (check(Tok::Async) && (type(1) == Tok::LParen || identifierLike(type(1))))
        return true;
    if (!check(Tok::LParen))
        return false;
    int depth = 0;
    for (std::size_t i = pos_; i < tokens_.size(); ++i) {
        const Tok t = tokens_[i].type;
        if (t == Tok::LParen) {
            ++depth;
        } else if (t == Tok::RParen) {
            if (--depth == 0)
                return i + 1 < tokens_.size() && tokens_[i + 1].type == Tok::Arrow;
        } else if (t == Tok::Semicolon || t == Tok::LBrace || t == Tok::Eof) {
            return false;
        }
    }
    return false;
}

Expr* Parser::parseLambda()
{
    LambdaExpr* node = here<LambdaExpr>();
    if (check(Tok::Async))
        fail("async lambdas are not supported");

    if (identifierLike(type())) {
        LambdaParam param;
        param.line = peek().line;
        param.name = advance().text;
        node->params.push_back(std::move(param));
    } else {
        expect(Tok::LParen, "'('");
        if (!check(Tok::RParen)) {
            do {
                LambdaParam param;
                param.line = peek().line;
                std::size_t end = 0;
                const bool typed = !identifierLike(type()) ||
                                   (looksLikeType(pos_, end) && end < tokens_.size() &&
                                    identifierLike(tokens_[end].type));
                if (typed)
                    param.typeRef = parseType();
                param.name = expectName("a parameter name");
                node->params.push_back(std::move(param));
            } while (match(Tok::Comma));
        }
        expect(Tok::RParen, "')'");
    }
    expect(Tok::Arrow, "'=>'");
    if (check(Tok::LBrace))
        node->bodyBlock = parseBlock();
    else
        node->bodyExpr = parseExpression();
    return node;
}

std::vector<Argument> Parser::parseArguments()
{
    std::vector<Argument> args;
    expect(Tok::LParen, "'('");
    if (match(Tok::RParen))
        return args;
    do {
        Argument arg;
        if (identifierLike(type()) && type(1) == Tok::Colon) {
            arg.name = advance().text;
            advance();
        }
        if (match(Tok::Ref))
            arg.mode = ParamMode::Ref;
        else if (match(Tok::Out))
            arg.mode = ParamMode::Out;
        // `out var x` and `out int x` declare the variable here.
        if (arg.mode == ParamMode::Out) {
            std::size_t end = 0;
            arg.outLine = peek().line;
            if (check(Tok::Var) && identifierLike(type(1))) {
                arg.outTypeRef = parseType();
                arg.outName = advance().text;
                args.push_back(std::move(arg));
                continue;
            }
            if (looksLikeType(pos_, end) && end < tokens_.size() &&
                identifierLike(tokens_[end].type) &&
                (end + 1 >= tokens_.size() || tokens_[end + 1].type == Tok::Comma ||
                 tokens_[end + 1].type == Tok::RParen)) {
                arg.outTypeRef = parseType();
                arg.outName = advance().text;
                args.push_back(std::move(arg));
                continue;
            }
        }
        arg.value = parseExpression();
        args.push_back(std::move(arg));
    } while (match(Tok::Comma));
    expect(Tok::RParen, "')'");
    return args;
}

Expr* Parser::parsePrimary()
{
    const Token& token = peek();
    const std::uint32_t line = token.line;
    const std::uint32_t column = token.column;

    // `throw` where a value is expected, and `_` as a target or argument.
    if (check(Tok::Throw)) {
        advance();
        ThrowExpr* node = newExpr<ThrowExpr>(line, column);
        node->value = parseExpression();
        return node;
    }
    if (check(Tok::LBracket)) {
        // `[1, 2, ..rest]`, typed by whatever it is assigned to.
        advance();
        CollectionExpr* node = newExpr<CollectionExpr>(line, column);
        while (!check(Tok::RBracket) && !check(Tok::Eof)) {
            CollectionElement element;
            if (match(Tok::DotDot))
                element.spread = true;
            element.value = parseExpression();
            node->elements.push_back(std::move(element));
            if (!match(Tok::Comma))
                break;
        }
        expect(Tok::RBracket, "']'");
        return node;
    }
    if (check(Tok::DotDot)) {
        // A range with no start: `..end` or `..`.
        advance();
        RangeExpr* node = newExpr<RangeExpr>(line, column);
        if (!check(Tok::RBracket) && !check(Tok::Comma))
            node->end = parseExpression();
        return node;
    }
    if (check(Tok::Caret)) {
        advance();
        IndexExpr* node = newExpr<IndexExpr>(line, column);
        node->operand = parseUnary();
        return node;
    }
    if (identifierLike(token.type) && token.text == u"_" && type(1) != Tok::Dot &&
        type(1) != Tok::LParen) {
        advance();
        return newExpr<DiscardExpr>(line, column);
    }

    switch (token.type) {
        case Tok::IntLit:
        case Tok::UIntLit:
        case Tok::LongLit:
        case Tok::ULongLit:
        case Tok::FloatLit:
        case Tok::DoubleLit:
        case Tok::CharLit:
        case Tok::StringLit:
        case Tok::True:
        case Tok::False:
        case Tok::Null: {
            LiteralExpr* node = newExpr<LiteralExpr>(line, column);
            switch (advance().type) {
                case Tok::IntLit: node->literal = LiteralKind::Int; break;
                case Tok::UIntLit: node->literal = LiteralKind::UInt; break;
                case Tok::LongLit: node->literal = LiteralKind::Long; break;
                case Tok::ULongLit: node->literal = LiteralKind::ULong; break;
                case Tok::FloatLit: node->literal = LiteralKind::Float; break;
                case Tok::DoubleLit: node->literal = LiteralKind::Double; break;
                case Tok::CharLit: node->literal = LiteralKind::Char; break;
                case Tok::StringLit: node->literal = LiteralKind::String; break;
                case Tok::True:
                case Tok::False: node->literal = LiteralKind::Bool; break;
                default: node->literal = LiteralKind::Null; break;
            }
            node->intValue = token.type == Tok::True ? 1 : token.intValue;
            node->realValue = token.realValue;
            node->text = token.text;
            return node;
        }
        case Tok::DecimalLit:
            fail("the decimal type is not supported");
        case Tok::InterpolatedLit: {
            advance();
            return parseInterpolated(token);
        }
        case Tok::This: {
            advance();
            return newExpr<ThisExpr>(line, column);
        }
        case Tok::Base: {
            advance();
            return newExpr<BaseExpr>(line, column);
        }
        case Tok::New:
            return parseNewExpression();
        case Tok::Typeof: {
            advance();
            TypeOfExpr* node = newExpr<TypeOfExpr>(line, column);
            expect(Tok::LParen, "'('");
            node->typeRef = parseType();
            expect(Tok::RParen, "')'");
            return node;
        }
        case Tok::Nameof: {
            advance();
            NameOfExpr* node = newExpr<NameOfExpr>(line, column);
            expect(Tok::LParen, "'('");
            for (;;) {
                if (!identifierLike(type()) && !typeKeyword(type()))
                    fail("expected a name");
                node->name = advance().text;
                if (!match(Tok::Dot))
                    break;
            }
            expect(Tok::RParen, "')'");
            return node;
        }
        case Tok::Default: {
            advance();
            DefaultExpr* node = newExpr<DefaultExpr>(line, column);
            if (match(Tok::LParen)) {
                node->typeRef = parseType();
                expect(Tok::RParen, "')'");
            }
            return node;
        }
        case Tok::Checked:
        case Tok::Unchecked: {
            advance();
            expect(Tok::LParen, "'('");
            Expr* inner = parseExpression();
            expect(Tok::RParen, "')'");
            return inner;
        }
        case Tok::LParen: {
            advance();
            // `(a, b)` is a tuple; `(a)` is just grouping. A name followed by
            // a colon is an element name, not a label.
            TupleElement first;
            if (identifierLike(type()) && type(1) == Tok::Colon) {
                first.name = advance().text;
                advance();
            }
            first.value = parseExpression();
            if (!check(Tok::Comma)) {
                expect(Tok::RParen, "')'");
                return first.value;
            }
            TupleExpr* node = newExpr<TupleExpr>(line, column);
            node->elements.push_back(std::move(first));
            while (match(Tok::Comma)) {
                TupleElement element;
                if (identifierLike(type()) && type(1) == Tok::Colon) {
                    element.name = advance().text;
                    advance();
                }
                element.value = parseExpression();
                node->elements.push_back(std::move(element));
            }
            expect(Tok::RParen, "')'");
            return node;
        }
        default: break;
    }

    // A predefined type used as a receiver: `int.Parse`, `string.Join`.
    if (typeKeyword(token.type)) {
        NameExpr* node = newExpr<NameExpr>(line, column);
        node->name = ascii(tokenName(advance().type));
        return node;
    }
    if (identifierLike(token.type)) {
        NameExpr* node = newExpr<NameExpr>(line, column);
        node->name = advance().text;
        // `Max<int>(a, b)`: only a call can follow, which is what tells this
        // apart from `a < b > (c)`.
        std::size_t end = 0;
        if (check(Tok::Lt) && isTypeArgumentList(pos_, end) && end < tokens_.size() &&
            tokens_[end].type == Tok::LParen) {
            advance();
            do {
                node->typeArgs.push_back(parseType());
            } while (match(Tok::Comma));
            splitShift();
            expect(Tok::Gt, "'>'");
        }
        return node;
    }
    fail(std::string("unexpected ") + tokenName(token.type));
}

Expr* Parser::parseInterpolated(const Token& token)
{
    InterpolatedExpr* node = newExpr<InterpolatedExpr>(token.line, token.column);
    node->parts = token.parts;
    for (std::size_t i = 0; i < token.exprs.size(); ++i) {
        node->holes.push_back(parseExpressionSource(token.exprs[i], token.line));
        node->formats.push_back(fromUtf8(token.formats[i]));
    }
    return node;
}

Expr* Parser::parseNewExpression()
{
    const std::uint32_t line = peek().line;
    const std::uint32_t column = peek().column;
    advance();

    // `new(...)` takes its type from what it is assigned to.
    if (check(Tok::LParen) || check(Tok::LBrace)) {
        ObjectCreationExpr* node = newExpr<ObjectCreationExpr>(line, column);
        if (check(Tok::LParen))
            node->args = parseArguments();
        if (check(Tok::LBrace))
            parseInitializer(node);
        return node;
    }

    TypeRef* typeRef = parseType();
    if (typeRef->arrayRank > 0 || check(Tok::LBracket)) {
        ArrayCreationExpr* node = newExpr<ArrayCreationExpr>(line, column);
        node->elementTypeRef = typeRef;
        if (typeRef->arrayRank > 0) {
            // `new int[] { ... }`: the rank came from the type.
            --typeRef->arrayRank;
            node->hasInitializer = true;
            expect(Tok::LBrace, "'{'");
            while (!check(Tok::RBrace) && !check(Tok::Eof)) {
                node->elements.push_back(parseExpression());
                if (!match(Tok::Comma))
                    break;
            }
            expect(Tok::RBrace, "'}'");
            return node;
        }
        expect(Tok::LBracket, "'['");
        node->size = parseExpression();
        expect(Tok::RBracket, "']'");
        while (check(Tok::LBracket) && type(1) == Tok::RBracket) {
            advance();
            advance();
            ++typeRef->arrayRank;
        }
        if (check(Tok::LBrace))
            fail("an array cannot have both a size and an initializer");
        return node;
    }

    ObjectCreationExpr* node = newExpr<ObjectCreationExpr>(line, column);
    node->typeRef = typeRef;
    if (check(Tok::LParen))
        node->args = parseArguments();
    if (check(Tok::LBrace))
        parseInitializer(node);
    return node;
}

void Parser::parseInitializer(ObjectCreationExpr* node)
{
    expect(Tok::LBrace, "'{'");
    while (!check(Tok::RBrace) && !check(Tok::Eof)) {
        if (identifierLike(type()) && type(1) == Tok::Assign) {
            InitializerEntry entry;
            entry.name = advance().text;
            advance();
            entry.value = parseExpression();
            node->initializers.push_back(std::move(entry));
        } else if (check(Tok::LBracket)) {
            InitializerEntry entry;
            advance();
            entry.key = parseExpression();
            expect(Tok::RBracket, "']'");
            expect(Tok::Assign, "'='");
            entry.value = parseExpression();
            node->initializers.push_back(std::move(entry));
        } else if (check(Tok::LBrace)) {
            // `new Dictionary<K,V> { { k, v }, ... }`
            advance();
            InitializerEntry entry;
            entry.key = parseExpression();
            expect(Tok::Comma, "','");
            entry.value = parseExpression();
            expect(Tok::RBrace, "'}'");
            node->initializers.push_back(std::move(entry));
        } else {
            node->elements.push_back(parseExpression());
        }
        if (!match(Tok::Comma))
            break;
    }
    expect(Tok::RBrace, "'}'");
}

// -------------------------------------------------------------- patterns ---

Pattern* Parser::parsePattern()
{
    return parseOrPattern();
}

Pattern* Parser::parseOrPattern()
{
    Pattern* left = parseAndPattern();
    while (check(Tok::Or)) {
        Pattern* node = arena_.make<Pattern>();
        node->kind = PatternKind::Or;
        node->line = peek().line;
        node->column = peek().column;
        advance();
        node->children.push_back(left);
        node->children.push_back(parseAndPattern());
        left = node;
    }
    return left;
}

Pattern* Parser::parseAndPattern()
{
    Pattern* left = parseUnaryPattern();
    while (check(Tok::And)) {
        Pattern* node = arena_.make<Pattern>();
        node->kind = PatternKind::And;
        node->line = peek().line;
        node->column = peek().column;
        advance();
        node->children.push_back(left);
        node->children.push_back(parseUnaryPattern());
        left = node;
    }
    return left;
}

Pattern* Parser::parseUnaryPattern()
{
    if (check(Tok::Not)) {
        Pattern* node = arena_.make<Pattern>();
        node->kind = PatternKind::Not;
        node->line = peek().line;
        node->column = peek().column;
        advance();
        node->children.push_back(parseUnaryPattern());
        return node;
    }
    return parsePrimaryPattern();
}

Pattern* Parser::parsePrimaryPattern()
{
    Pattern* node = arena_.make<Pattern>();
    node->line = peek().line;
    node->column = peek().column;

    // A relational pattern: `> 10`, `<= 0`.
    switch (type()) {
        case Tok::Lt:
        case Tok::Le:
        case Tok::Gt:
        case Tok::Ge: {
            node->kind = PatternKind::Relational;
            node->op = binaryOpOf(advance().type);
            node->value = parseUnary();
            return node;
        }
        default: break;
    }

    if (match(Tok::LParen)) {
        // Either a parenthesized pattern or a positional one. A single
        // element with no name and no comma is just grouping.
        Pattern* first = parsePattern();
        if (!check(Tok::Comma)) {
            expect(Tok::RParen, "')'");
            return first;
        }
        node->kind = PatternKind::Positional;
        node->children.push_back(first);
        while (match(Tok::Comma))
            node->children.push_back(parsePattern());
        expect(Tok::RParen, "')'");
        parsePatternTail(node);
        return node;
    }

    if (check(Tok::LBracket)) {
        advance();
        node->kind = PatternKind::List;
        if (!check(Tok::RBracket)) {
            do {
                if (check(Tok::DotDot)) {
                    Pattern* slice = arena_.make<Pattern>();
                    slice->kind = PatternKind::Slice;
                    slice->line = peek().line;
                    slice->column = peek().column;
                    advance();
                    if (!check(Tok::Comma) && !check(Tok::RBracket))
                        slice->children.push_back(parsePattern());
                    node->children.push_back(slice);
                } else {
                    node->children.push_back(parsePattern());
                }
            } while (match(Tok::Comma));
        }
        expect(Tok::RBracket, "']'");
        parsePatternTail(node);
        return node;
    }

    if (check(Tok::LBrace)) {
        node->kind = PatternKind::Property;
        parsePropertySubpatterns(node);
        parsePatternTail(node);
        return node;
    }

    if (check(Tok::Var) && identifierLike(type(1))) {
        advance();
        if (check(Tok::LParen)) {
            // `var (a, b)` binds each piece rather than the whole.
            advance();
            node->kind = PatternKind::Positional;
            do {
                Pattern* piece = arena_.make<Pattern>();
                piece->kind = PatternKind::Var;
                piece->line = peek().line;
                piece->column = peek().column;
                piece->bindingName = expectName("a name");
                node->children.push_back(piece);
            } while (match(Tok::Comma));
            expect(Tok::RParen, "')'");
            return node;
        }
        node->kind = PatternKind::Var;
        node->bindingName = advance().text;
        return node;
    }

    if (identifierLike(type()) && peek().text == u"_" && type(1) != Tok::Dot) {
        advance();
        node->kind = PatternKind::Discard;
        return node;
    }

    // A type, optionally followed by a binding, a property list or a
    // positional list. Anything else is a constant.
    std::size_t end = 0;
    const bool typeAhead = looksLikeType(pos_, end) && end > pos_ &&
                           (identifierLike(tokens_[end].type) || tokens_[end].type == Tok::LBrace ||
                            tokens_[end].type == Tok::LParen || tokens_[end].type == Tok::RParen ||
                            tokens_[end].type == Tok::Comma || tokens_[end].type == Tok::Colon ||
                            tokens_[end].type == Tok::Arrow || tokens_[end].type == Tok::RBracket ||
                            tokens_[end].type == Tok::When || tokens_[end].type == Tok::Semicolon ||
                            tokens_[end].type == Tok::And || tokens_[end].type == Tok::Or);
    if (typeAhead && !constantStart(type())) {
        node->kind = PatternKind::Declaration;
        node->typeRef = parseType();
        if (check(Tok::LParen)) {
            advance();
            node->kind = PatternKind::Positional;
            if (!check(Tok::RParen)) {
                do {
                    node->children.push_back(parsePattern());
                } while (match(Tok::Comma));
            }
            expect(Tok::RParen, "')'");
        }
        if (check(Tok::LBrace))
            parsePropertySubpatterns(node);
        if (identifierLike(type()) && !check(Tok::When) && !check(Tok::And) && !check(Tok::Or))
            node->bindingName = advance().text;
        return node;
    }

    node->kind = PatternKind::Constant;
    node->value = parseUnary();
    return node;
}

void Parser::parsePropertySubpatterns(Pattern* node)
{
    if (node->kind != PatternKind::Positional)
        node->kind = PatternKind::Property;
    expect(Tok::LBrace, "'{'");
    if (!check(Tok::RBrace)) {
        do {
            if (check(Tok::RBrace))
                break;
            PropertySubpattern entry;
            entry.name = expectName("a property name");
            expect(Tok::Colon, "':'");
            entry.pattern = parsePattern();
            node->properties.push_back(std::move(entry));
        } while (match(Tok::Comma));
    }
    expect(Tok::RBrace, "'}'");
}

void Parser::parsePatternTail(Pattern* node)
{
    if (check(Tok::LBrace))
        parsePropertySubpatterns(node);
    if (identifierLike(type()) && !check(Tok::When) && !check(Tok::And) && !check(Tok::Or))
        node->bindingName = advance().text;
}

Expr* Parser::parsePostfix(Expr* target)
{
    for (;;) {
        const std::uint32_t line = peek().line;
        const std::uint32_t column = peek().column;
        if (check(Tok::Dot) || check(Tok::QuestionDot)) {
            const bool conditional = type() == Tok::QuestionDot;
            advance();
            MemberAccessExpr* node = newExpr<MemberAccessExpr>(line, column);
            node->target = target;
            node->nullConditional = conditional;
            if (!identifierLike(type()) && !typeKeyword(type()))
                fail("expected a member name");
            node->name = advance().text;
            std::size_t end = 0;
            if (check(Tok::Lt) && isTypeArgumentList(pos_, end) && end < tokens_.size() &&
                tokens_[end].type == Tok::LParen) {
                advance();
                do {
                    node->typeArgs.push_back(parseType());
                } while (match(Tok::Comma));
                splitShift();
                expect(Tok::Gt, "'>'");
            }
            target = node;
            continue;
        }
        if (check(Tok::Switch) && type(1) == Tok::LBrace) {
            SwitchExpr* node = newExpr<SwitchExpr>(line, column);
            node->subject = target;
            advance();
            advance();
            while (!check(Tok::RBrace) && !check(Tok::Eof)) {
                SwitchArm arm;
                arm.line = peek().line;
                arm.column = peek().column;
                arm.pattern = parsePattern();
                if (match(Tok::When))
                    arm.guard = parseExpression();
                expect(Tok::Arrow, "'=>'");
                arm.value = parseExpression();
                node->arms.push_back(std::move(arm));
                if (!match(Tok::Comma))
                    break;
            }
            expect(Tok::RBrace, "'}'");
            target = node;
            continue;
        }
        if (check(Tok::With) && type(1) == Tok::LBrace) {
            WithExpr* node = newExpr<WithExpr>(line, column);
            node->target = target;
            advance();
            advance();
            while (!check(Tok::RBrace) && !check(Tok::Eof)) {
                InitializerEntry entry;
                entry.name = expectName("a member name");
                expect(Tok::Assign, "'='");
                entry.value = parseExpression();
                node->initializers.push_back(std::move(entry));
                if (!match(Tok::Comma))
                    break;
            }
            expect(Tok::RBrace, "'}'");
            target = node;
            continue;
        }
        if (check(Tok::LParen)) {
            InvocationExpr* node = newExpr<InvocationExpr>(line, column);
            node->callee = target;
            node->args = parseArguments();
            target = node;
            continue;
        }
        if (check(Tok::LBracket)) {
            advance();
            ElementAccessExpr* node = newExpr<ElementAccessExpr>(line, column);
            node->target = target;
            node->index = parseExpression();
            // `a[1..3]`: the index parsed as the start, and `..` follows.
            if (check(Tok::DotDot)) {
                advance();
                RangeExpr* range = newExpr<RangeExpr>(line, column);
                range->start = node->index;
                if (!check(Tok::RBracket))
                    range->end = parseExpression();
                node->index = range;
            }
            expect(Tok::RBracket, "']'");
            target = node;
            continue;
        }
        if (check(Tok::PlusPlus) || check(Tok::MinusMinus)) {
            UnaryExpr* node = newExpr<UnaryExpr>(line, column);
            node->op = advance().type == Tok::PlusPlus ? UnaryOp::PostIncrement
                                                       : UnaryOp::PostDecrement;
            node->operand = target;
            target = node;
            continue;
        }
        if (check(Tok::Bang) && (type(1) == Tok::Dot || type(1) == Tok::Semicolon ||
                                 type(1) == Tok::RParen || type(1) == Tok::Comma)) {
            // The null-forgiving operator asserts something the checker is not
            // tracking anyway, so it is accepted and dropped.
            advance();
            continue;
        }
        return target;
    }
}

}  // namespace typed
}  // namespace internal
}  // namespace script
