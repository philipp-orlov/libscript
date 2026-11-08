// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Recursive-descent parser with operator-precedence expression parsing.
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "script/internal/dyn/ast.hpp"
#include "script/internal/dyn/lexer.hpp"

namespace script {
namespace internal {
namespace dyn {

/// Parses a whole script. Throws `Error(ErrorCode::SyntaxError)` on the first
/// problem, with the source name and line in the message.
///
/// One parser serves both ECMAScript languages: with `typescript` false every
/// TypeScript-only construct -- annotations, `as`, generics, `enum`,
/// `interface`, access modifiers -- is refused where JavaScript would refuse
/// it, and the contextual keywords are just names.
class Parser
{
public:
    Parser(std::string_view source, std::string sourceName, bool typescript = true);

    NodePtr parseProgram();

private:
    // ---- token plumbing ----
    void advance();
    bool at(Tok t) const noexcept { return cur_.type == t; }
    bool eat(Tok t);
    void expect(Tok t, const char* what);
    void semicolon();  ///< consumes `;`, or applies automatic insertion
    /// Consumes one `>`. Nested generics close as `>>` or `>>>`, which the
    /// lexer has already merged into a shift, so the surplus is pushed back by
    /// rewriting the current token rather than by re-lexing.
    void closeAngle();
    [[noreturn]] void fail(const std::string& message) const;
    /// Tokens that may stand in for an identifier: contextual keywords.
    static bool identifierLike(Tok t) noexcept;
    /// After a `.` or in a property key, every keyword is just a name.
    String propertyName();

    // ---- statements ----
    NodePtr statement();
    NodePtr block();
    NodePtr varStatement(VarKind kind);
    /// The `= initialiser` part of a declaration whose target is parsed.
    NodePtr declarator(NodePtr target, VarKind kind);
    NodePtr ifStatement();
    NodePtr forStatement();
    NodePtr whileStatement();
    NodePtr doStatement();
    NodePtr tryStatement();
    NodePtr switchStatement();
    NodePtr functionDeclaration();
    NodePtr classDeclaration(bool isAbstract);

    // ---- TypeScript declarations ----
    NodePtr typeAliasDeclaration();
    NodePtr interfaceDeclaration();
    NodePtr enumDeclaration(bool isConst);
    NodePtr namespaceDeclaration();
    /// `declare ...`: the body is parsed and then marked ambient.
    NodePtr ambientDeclaration();
    /// True when the current contextual keyword really does open a declaration
    /// rather than name a variable -- `type x = 1` against `type = 1`.
    bool startsDeclaration(Tok keyword) const;

    // ---- types ----
    /// `: T` if one is written, else null.
    NodePtr typeAnnotation();
    NodePtr typeExpr();  ///< the whole grammar, unions included
    NodePtr intersectionType();
    NodePtr postfixType();  ///< `T[]` and `T[K]` suffixes
    NodePtr primaryType();
    NodePtr typeLiteral();
    NodePtr typeMember();
    NodePtr tupleType();
    /// A parenthesised type or a function type; told apart the same way an
    /// arrow function is.
    NodePtr parenOrFunctionType();
    std::vector<NodePtr> typeParameterList();  ///< `<T extends C = D, ...>`
    std::vector<NodePtr> typeArguments();      ///< `<A, B>`
    /// True when the `<` at the cursor opens type arguments to a call rather
    /// than a less-than. Implemented by scanning to the matching `>`.
    bool looksLikeTypeArguments();
    /// True when the `<` at the cursor opens a generic arrow function.
    bool looksLikeGenericArrow();

    // ---- expressions ----
    NodePtr expression();  ///< comma operator
    NodePtr assignment();
    NodePtr conditional();
    NodePtr binary(int minPrecedence);
    NodePtr unary();
    NodePtr postfix();
    NodePtr callOrMember(NodePtr base, bool allowCall);
    NodePtr primary();
    NodePtr objectLiteral();
    NodePtr arrayLiteral();
    NodePtr templateLiteral();
    NodePtr functionExpression(bool isDeclaration);
    NodePtr classExpression(bool isDeclaration, bool isAbstract);
    NodePtr arrowFromParams(std::vector<NodePtr> params, NodePtr returnType, std::uint32_t line);
    std::vector<NodePtr> argumentList();
    /// `allowModifiers` admits `private x: T` constructor parameter properties.
    std::vector<NodePtr> parameterList(bool allowModifiers = false);

    // ---- patterns ----
    NodePtr bindingTarget();
    /// A binding target with an optional `= default`.
    NodePtr bindingElement();
    NodePtr arrayPattern();
    NodePtr objectPattern();
    /// Reinterpret an already-parsed expression as an assignment target.
    NodePtr toPattern(NodePtr expr);
    /// True when the parenthesised run starting at the current `(` is an arrow
    /// parameter list. Implemented by scanning to the matching `)`.
    bool looksLikeArrow();

    Lexer lexer_;
    Token cur_;
    /// Where the token before `cur_` ended, in bytes: the end of whatever was parsed last.
    std::uint32_t prevEnd_ = 0;
    /// Parsing a piece of a template literal, whose offsets are not the source's.
    bool noSourceRanges_ = false;
    /// Records `n`'s text as running from `start` to the end of the last token, unless offsets mean nothing here.
    NodePtr withSource(NodePtr n, std::uint32_t start)
    {
        if (!noSourceRanges_ && n) {
            n->srcStart = start;
            n->srcEnd = prevEnd_;
        }
        return n;
    }
    std::string name_;
    std::string_view source_;
    std::size_t depth_ = 0;  ///< guards against pathological nesting
    std::size_t exprDepth_ = 0;  ///< the same for expressions
    /// Tokens `looksLikeArrow` has scanned ahead so far, and the most it may scan in this source. Every `(` that
    /// starts an expression is scanned to its closing `)`, so nested parentheses make the work grow with the product
    /// of the nesting and the length; the cap keeps hostile input at a few times the length of the source.
    std::size_t lookahead_ = 0;
    std::size_t lookaheadLimit_ = 0;
    bool ambient_ = false;   ///< inside `declare`: bodies are not required
    bool noIn_ = false;      ///< parsing a for-in head: `in` ends the expression
    bool arrowIsAsync_ = false;  ///< the arrow function about to be parsed was written `async`
    bool inParameters_ = false;
    bool inStaticBlock_ = false;
    bool inGenerator_ = false;  ///< `yield` is an operator here, not a name
    bool inAsync_ = false;      ///< `await` is an operator here, not a name
    /// The kind of function whose parameters and body are about to be parsed, which those words depend on.
    struct FunctionKind
    {
        Parser& parser;
        bool generator, async;
        bool parameters, staticBlock;
        FunctionKind(Parser& p, bool isGenerator, bool isAsync, bool isArrow = false)
            : parser(p), generator(p.inGenerator_), async(p.inAsync_), parameters(p.inParameters_), staticBlock(p.inStaticBlock_)
        {
            p.inGenerator_ = isGenerator;
            p.inAsync_ = isAsync;
            p.inParameters_ = false;
            if (!isArrow)
                p.inStaticBlock_ = false;
        }
        ~FunctionKind()
        {
            parser.inGenerator_ = generator;
            parser.inAsync_ = async;
            parser.inParameters_ = parameters;
            parser.inStaticBlock_ = staticBlock;
        }
    };
    struct FlagSet
    {
        bool& flag;
        bool old;
        FlagSet(bool& f, bool value) : flag(f), old(f) { f = value; }
        ~FlagSet() { flag = old; }
    };
    /// `async` followed by what makes it a modifier of a method rather than the method's name.
    void checkReference();
    void privateMember(Node& member);
    /// A statement that is the body of an `if`, a loop or a label: no declarations.
    NodePtr bodyStatement(bool allowsFunction);
    bool asyncMethodAhead();
    bool asyncFunctionAhead();
    bool asyncArrowAhead();
    bool typescript_ = true; ///< accept the TypeScript syntax on top of JavaScript
};

}  // namespace dyn
}  // namespace internal
}  // namespace script
