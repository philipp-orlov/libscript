// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Recursive-descent parser. Produces the tree in `ast.hpp`; resolves nothing.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "script/internal/typed/ast.hpp"
#include "script/internal/typed/lexer.hpp"

namespace script {
namespace internal {
namespace typed {

class Parser
{
public:
    Parser(std::string_view source, std::string name, Arena& arena);

    CompilationUnit* parseUnit();
    /// Parse one expression from its own source text. Interpolation holes
    /// arrive this way, already carved out by the lexer.
    Expr* parseExpressionSource(std::string_view source, std::uint32_t line);

private:
    // ---- token stream ----
    const Token& peek(std::size_t ahead = 0) const noexcept
    {
        const std::size_t at = pos_ + ahead;
        return at < tokens_.size() ? tokens_[at] : tokens_.back();
    }
    Tok type(std::size_t ahead = 0) const noexcept { return peek(ahead).type; }
    const Token& advance() noexcept { return tokens_[pos_ < tokens_.size() - 1 ? pos_++ : pos_]; }
    bool check(Tok t) const noexcept { return type() == t; }
    bool match(Tok t) noexcept
    {
        if (!check(t))
            return false;
        ++pos_;
        return true;
    }
    const Token& expect(Tok t, const char* what);
    /// Any token an identifier may be spelled with, including the contextual
    /// keywords: `namespace Set { }` is legal C#.
    String expectName(const char* what);
    [[noreturn]] void fail(const std::string& message) const;
    [[noreturn]] void failAt(const Token& token, const std::string& message) const;
    /// `>>` closes two type argument lists at once; split it in two in place.
    void splitShift();

    // ---- declarations ----
    void parseNamespaceBody(const String& prefix, CompilationUnit* unit, bool braced);
    bool looksLikeUsingDirective() const;
    std::uint16_t parseModifiers();
    TypeDecl* parseTypeDecl(const String& prefix, std::uint16_t modifiers);
    void parseClassBody(TypeDecl* decl);
    void parseMember(TypeDecl* decl);
    void parseEnumBody(TypeDecl* decl);
    MethodDecl* parseMethodRest(TypeDecl* decl, std::uint16_t modifiers, TypeRef* returnType,
                                String name, std::uint32_t line);
    void parsePropertyRest(TypeDecl* decl, std::uint16_t modifiers, TypeRef* typeRef, String name,
                           std::uint32_t line);
    std::vector<ParamDecl> parseParams();
    /// `<T, U>` after a type or method name. Empty when there was none.
    std::vector<String> parseTypeParams();
    /// Every `where T : ...` clause that follows a signature.
    std::vector<TypeConstraint> parseConstraints();

    // ---- types ----
    TypeRef* parseType();
    TypeRef* tryParseType();
    bool looksLikeType(std::size_t at, std::size_t& end) const;
    bool isTypeArgumentList(std::size_t at, std::size_t& end) const;

    // ---- patterns ----
    Pattern* parsePattern();
    Pattern* parseOrPattern();
    Pattern* parseAndPattern();
    Pattern* parseUnaryPattern();
    Pattern* parsePrimaryPattern();
    void parsePropertySubpatterns(Pattern* node);
    /// The `{ ... }` and binding name a positional or list pattern may carry.
    void parsePatternTail(Pattern* node);

    // ---- statements ----
    Stmt* parseStatement();
    BlockStmt* parseBlock();
    Stmt* parseLocalDecl(bool expectSemicolon);
    Stmt* parseIf();
    Stmt* parseWhile();
    Stmt* parseDo();
    Stmt* parseFor();
    Stmt* parseForEach();
    Stmt* parseSwitch();
    Stmt* parseTry();
    bool looksLikeLocalDecl() const;
    bool looksLikeLocalFunction() const;
    bool looksLikeDeconstruction() const;
    Deconstruction* parseDeconstructionTargets();
    Stmt* parseDeconstruction();
    /// True when a `case` label is the pre-pattern constant form.
    bool looksLikeConstantLabel() const;

    // ---- expressions ----
    Expr* parseExpression();
    Expr* parseAssignment();
    Expr* parseConditional();
    Expr* parseCoalesce();
    Expr* parseBinary(int precedence);
    Expr* parseUnary();
    Expr* parsePostfix(Expr* target);
    Expr* parsePrimary();
    Expr* parseNewExpression();
    Expr* parseLambda();
    Expr* parseInterpolated(const Token& token);
    std::vector<Argument> parseArguments();
    void parseInitializer(ObjectCreationExpr* node);
    bool looksLikeLambda() const;
    bool looksLikeCast() const;

    template <class T>
    T* newExpr(std::uint32_t line, std::uint32_t column)
    {
        T* n = arena_.make<T>();
        n->line = line;
        n->column = column;
        return n;
    }
    template <class T>
    T* here()
    {
        return newExpr<T>(peek().line, peek().column);
    }
    template <class T>
    T* newStmt(std::uint32_t line)
    {
        T* n = arena_.make<T>();
        n->line = line;
        return n;
    }

    Arena& arena_;
    std::string name_;
    std::vector<Token> tokens_;
    std::size_t pos_ = 0;
    /// Non-zero inside a type declaration, so `parseStatement` can refuse to
    /// swallow a member.
    int depth_ = 0;
};

}  // namespace typed
}  // namespace internal
}  // namespace script
