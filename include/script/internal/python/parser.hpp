// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Python parser. Recursive descent over the token stream, producing the
// dynamic engine's ECMAScript syntax tree directly: there is no Python tree
// in between, because every Python construct the front end accepts has a
// JavaScript spelling and the rewrite is local. Whatever has none --
// generators, `with`, `**kwargs`, multiple inheritance -- is refused here with
// a `SyntaxError` that says so.
//
// The rewrites that are not one-to-one live in the `__py` helper object the
// registration installs (see python.cpp): indexing goes through
// `__py.getitem`, truth tests through `__py.truthy`, `in` through
// `__py.contains`, `==` through `__py.eq` unless one side is a literal, and
// the dozen method names whose Python and JavaScript meanings differ go
// through `__py.m_<name>`.
#pragma once

#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "script/internal/dyn/ast.hpp"
#include "script/internal/python/lexer.hpp"

namespace script {
namespace internal {
namespace python {

class Parser
{
public:
    Parser(std::string_view source, std::string name);

    /// A whole module, as an `NK::Program`. Throws `Error(SyntaxError)`.
    dyn::NodePtr parseModule();

private:
    using Node = dyn::Node;
    using NodePtr = dyn::NodePtr;
    using NK = dyn::NK;
    using JTok = dyn::Tok;

    // ---- token plumbing ----
    const Token& peek(std::size_t ahead = 0) const noexcept
    {
        const std::size_t i = pos_ + ahead;
        return i < toks_.size() ? toks_[i] : toks_.back();
    }
    Tok type(std::size_t ahead = 0) const noexcept { return peek(ahead).type; }
    const Token& advance() { return toks_[pos_ < toks_.size() - 1 ? pos_++ : pos_]; }
    bool is(Tok t) const noexcept { return type() == t; }
    bool eat(Tok t);
    void expect(Tok t, const char* what);
    [[noreturn]] void fail(const std::string& message) const;
    [[noreturn]] void fail(std::uint32_t line, const std::string& message) const;
    std::uint32_t line() const noexcept { return peek().line; }

    // ---- scopes ----
    /// One function, module or class body being parsed. A function's assigned
    /// names become a `let` at the top of its body; the module's become a
    /// `var`, which is how they land on the global object.
    struct Scope
    {
        enum class Kind : std::uint8_t
        {
            Module,
            Function,
            Class,
        } kind = Kind::Module;
        /// False for a lambda or comprehension: it emits no declarations, so
        /// its temporaries belong to the enclosing function.
        bool holdsTemps = true;
        std::vector<String> declared;
        std::set<String> seen;
        std::set<String> params;
        std::set<String> globals;
        std::set<String> nonlocals;
        /// Set on a method's scope: the name its first parameter gave `self`.
        String selfName;
        /// Set on a class scope, for `Base.__init__(self, ...)` and `super()`.
        String className;
        String baseName;
        /// A `raise` with no operand rethrows this.
        String currentException;
    };
    std::vector<Scope> scopes_;
    Scope& scope() { return scopes_.back(); }
    /// The nearest function or module scope.
    Scope& varScope();
    const Scope* classScope() const;
    void declareName(const String& name);
    String newTemp();
    void newTempNamed(const String& name);
    NodePtr shortCircuit(bool isOr, NodePtr left, NodePtr right, std::uint32_t at);
    NodePtr declarations(Scope& s, dyn::VarKind kind, std::uint32_t at);

    // ---- statements ----
    void statement(std::vector<NodePtr>& out);
    void simpleStatement(std::vector<NodePtr>& out);
    void expressionOrAssignment(std::vector<NodePtr>& out);
    NodePtr block();  ///< `:` NEWLINE INDENT ... DEDENT, or `:` simple statements
    NodePtr ifStatement();
    NodePtr whileStatement();
    NodePtr forStatement();
    NodePtr tryStatement();
    NodePtr functionDef(std::vector<String> decorators, bool inClass, dyn::PropKind& kind,
                        bool& isStatic);
    void classDef(std::vector<NodePtr>& out);
    void decorated(std::vector<NodePtr>& out);
    NodePtr importStatement();
    NodePtr assignTo(NodePtr target, NodePtr value, std::uint32_t at);
    NodePtr augmentedAssign(NodePtr target, Tok op, NodePtr value, std::uint32_t at);
    /// Turn an expression that was parsed as a value into an assignment target.
    NodePtr toTarget(NodePtr expr, bool declare);
    std::vector<NodePtr> parameters(bool isMethod, String* selfName);

    // ---- expressions ----
    NodePtr expression();       ///< anything, `a if c else b` and `lambda` included
    NodePtr exprList();         ///< `a, b` -> tuple; a single expression otherwise
    NodePtr exprListNoCond();   ///< the same, without conditional/lambda (a `for` head)
    NodePtr lambda();
    NodePtr ternary();
    NodePtr orTest();
    NodePtr andTest();
    NodePtr notTest();
    NodePtr comparison();
    NodePtr comparisonOne(Tok op, bool negate, NodePtr left, NodePtr right, std::uint32_t at);
    NodePtr bitOr();
    NodePtr bitXor();
    NodePtr bitAnd();
    NodePtr shift();
    NodePtr arith();
    NodePtr term();
    NodePtr factor();
    NodePtr power();
    NodePtr primary();
    NodePtr atom();
    NodePtr trailers(NodePtr base);
    NodePtr callExpression(NodePtr callee, std::uint32_t at);
    NodePtr subscript(NodePtr base, std::uint32_t at);
    NodePtr stringAtom();
    NodePtr fstring(const Token& token);
    NodePtr listDisplay();
    NodePtr dictOrSetDisplay();
    NodePtr parenthesized();
    /// `expr for x in xs if c ...` after the first element has been parsed.
    NodePtr comprehension(NodePtr element, NodePtr value, int kind);
    NodePtr starred();

    // ---- lowering helpers ----
    static NodePtr make(NK kind, std::uint32_t at) { return dyn::makeNode(kind, at); }
    NodePtr ident(StringView name, std::uint32_t at);
    NodePtr member(NodePtr object, StringView name, std::uint32_t at);
    NodePtr call(NodePtr callee, std::vector<NodePtr> args, std::uint32_t at);
    /// `__py.<name>(args...)`.
    NodePtr helper(const char* name, std::vector<NodePtr> args, std::uint32_t at);
    NodePtr number(double value, std::uint32_t at);
    NodePtr string(const String& text, std::uint32_t at);
    NodePtr undefinedValue(std::uint32_t at);
    NodePtr binary(JTok op, NodePtr a, NodePtr b, std::uint32_t at);
    NodePtr logical(JTok op, NodePtr a, NodePtr b, std::uint32_t at);
    NodePtr unary(JTok op, NodePtr a, std::uint32_t at);
    NodePtr assign(NodePtr target, NodePtr value, std::uint32_t at);
    NodePtr exprStatement(NodePtr e);
    NodePtr blockOf(std::vector<NodePtr> statements, std::uint32_t at);
    NodePtr constDecl(const String& name, NodePtr init, std::uint32_t at);
    /// Wrap a condition in `__py.truthy` unless it is already a boolean.
    NodePtr truth(NodePtr e);
    static bool isBoolish(const Node& n);
    static bool isLiteral(const Node& n);
    static NodePtr clone(const Node& n);
    static bool isNoneLiteral(const Node& n);
    static bool containsSuperCall(const Node& n);
    static void collectNames(const Node& target, std::vector<String>& out);

    // ---- calls with keyword arguments ----
    struct Signature
    {
        std::vector<String> params;
        bool variadic = false;
    };
    struct PendingCall
    {
        Node* call = nullptr;
        String callee;
        std::vector<std::pair<String, NodePtr>> keywords;
        std::uint32_t line = 0;
    };
    std::unordered_map<String, Signature> functions_;
    std::unordered_set<String> classes_;
    std::vector<PendingCall> pendingCalls_;
    void resolveKeywordCalls();
    bool isKnownClass(StringView name) const;

    std::vector<Token> toks_;
    std::size_t pos_ = 0;
    std::string name_;
    int tempCounter_ = 0;
};

}  // namespace python
}  // namespace internal
}  // namespace script
