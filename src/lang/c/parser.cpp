// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// C parser and lowering: C source in, the typed engine's syntax tree out.
// See the header for the mapping; the C++ dialect's own members are in cxx.cpp.
#include "script/internal/c/parser.hpp"

#include <algorithm>
#include <cstring>

#include "script/error.hpp"
#include "script/internal/c/libc.hpp"
#include "script/internal/stack_guard.hpp"

namespace script {
namespace internal {
namespace c {

using typed::Arena;
using typed::BinaryOp;
using typed::ExprKind;
using typed::LiteralKind;
using typed::UnaryOp;
namespace T = typed;
using Kind = CType::Kind;

namespace {

const char* const kLibcClass = "libc";

const char* kindName(Kind k) noexcept
{
    switch (k) {
        case Kind::Void: return "void";
        case Kind::Bool: return "int";
        case Kind::Char: return "char";
        case Kind::UChar: return "unsigned char";
        case Kind::Short: return "short";
        case Kind::UShort: return "unsigned short";
        case Kind::Int: return "int";
        case Kind::UInt: return "unsigned int";
        case Kind::Long: return "long";
        case Kind::ULong: return "unsigned long";
        case Kind::Float: return "float";
        case Kind::Double: return "double";
        case Kind::Str: return "char*";
        case Kind::Struct: return "struct";
        case Kind::Array: return "array";
        case Kind::Null: return "NULL";
        case Kind::Vector: return "std::vector";
        case Kind::Map: return "std::map";
        case Kind::Pair: return "std::pair";
    }
    return "?";
}

CType kindOfLetter(char letter)
{
    switch (letter) {
        case 'i': return CType::of(Kind::Int);
        case 'u': return CType::of(Kind::UInt);
        case 'l': return CType::of(Kind::Long);
        case 'd': return CType::of(Kind::Double);
        case 'c': return CType::of(Kind::Char);
        case 's': return CType::of(Kind::Str);
        case 'A': return CType::arrayOf(CType::of(Kind::Char));
        case 'b': return CType::of(Kind::Bool);
        case 'v': return CType::of(Kind::Void);
        default: return CType::of(Kind::Int);
    }
}

}  // namespace

bool CType::same(const CType& o) const noexcept
{
    if (kind != o.kind)
        return false;
    if (kind == Kind::Struct)
        return structName == o.structName;
    if (kind == Kind::Array || kind == Kind::Vector)
        return elem && o.elem && elem->same(*o.elem);
    if (kind == Kind::Map || kind == Kind::Pair)
        return elem && o.elem && elem->same(*o.elem) && elem2 && o.elem2 && elem2->same(*o.elem2);
    return true;
}

std::string Parser::describe(const CType& t) const
{
    switch (t.kind) {
        case Kind::Bool: return cpp() ? "bool" : "int";
        case Kind::Str: return cpp() ? "std::string" : "char*";
        case Kind::Struct:
            if (cpp())
                return toUtf8(t.structName.substr(0, 7) == u"System." ? t.structName.substr(7)
                                                                        : t.structName);
            return "struct " + toUtf8(t.structName);
        case Kind::Array: return describe(*t.elem) + "[]";
        case Kind::Vector: return "std::vector<" + describe(*t.elem) + ">";
        case Kind::Map: return "std::map<" + describe(*t.elem) + ", " + describe(*t.elem2) + ">";
        case Kind::Pair: return "std::pair<" + describe(*t.elem) + ", " + describe(*t.elem2) + ">";
        default: return kindName(t.kind);
    }
}

// ---------------------------------------------------------------- plumbing ---

Parser::Parser(std::string_view source, std::string name, Arena& arena,
               std::vector<HostClassSpec> hostClasses, Dialect dialect)
    : name_(std::move(name)), arena_(arena), dialect_(dialect), hostClasses_(std::move(hostClasses))
{
    Lexer lexer(source, name_, cpp());
    toks_ = lexer.tokenize();
    if (cpp())
        namespaces_.insert(ascii("std"));

    // The fixed-width names of <stdint.h> and friends.
    struct Alias
    {
        const char* name;
        Kind kind;
    };
    static constexpr Alias aliases[] = {
        {"size_t", Kind::Int},      {"ssize_t", Kind::Int},     {"ptrdiff_t", Kind::Int},
        {"int8_t", Kind::Char},     {"uint8_t", Kind::UChar},   {"int16_t", Kind::Short},
        {"uint16_t", Kind::UShort}, {"int32_t", Kind::Int},     {"uint32_t", Kind::UInt},
        {"int64_t", Kind::Long},    {"uint64_t", Kind::ULong},  {"intptr_t", Kind::Long},
        {"uintptr_t", Kind::ULong}, {"wchar_t", Kind::Char},
    };
    for (const Alias& a : aliases)
        typedefs_[ascii(a.name)] = CType::of(a.kind);
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

String Parser::expectName(const char* what)
{
    if (!is(Tok::Identifier))
        fail(std::string("expected ") + what + " but found " + tokenName(type()));
    return advance().text;
}

void Parser::fail(const std::string& message) const
{
    fail(line(), message);
}

void Parser::fail(std::uint32_t at, const std::string& message) const
{
    Lexer::fail(name_, at, message);
}

void Parser::skipBalanced(Tok open, Tok close)
{
    expect(open, tokenName(open));
    int depth = 1;
    while (depth > 0) {
        if (is(Tok::Eof))
            fail("unexpected end of input");
        if (is(open))
            ++depth;
        else if (is(close))
            --depth;
        advance();
    }
}

// ------------------------------------------------------------- factories ---

Parser::Expr* Parser::intLiteral(std::int64_t value, std::uint32_t at)
{
    auto* lit = node<T::LiteralExpr>(at);
    if (value > 2147483647LL || value < -2147483648LL) {
        lit->literal = LiteralKind::Long;
    } else {
        lit->literal = LiteralKind::Int;
    }
    lit->intValue = value;
    return lit;
}

Parser::Expr* Parser::boolLiteral(bool value, std::uint32_t at)
{
    auto* lit = node<T::LiteralExpr>(at);
    lit->literal = LiteralKind::Bool;
    lit->intValue = value ? 1 : 0;
    return lit;
}

Parser::Expr* Parser::nullLiteral(std::uint32_t at)
{
    auto* lit = node<T::LiteralExpr>(at);
    lit->literal = LiteralKind::Null;
    return lit;
}

Parser::Expr* Parser::stringLiteral(const String& text, std::uint32_t at)
{
    auto* lit = node<T::LiteralExpr>(at);
    lit->literal = LiteralKind::String;
    lit->text = text;
    return lit;
}

Parser::Expr* Parser::nameRef(const String& name, std::uint32_t at, std::uint32_t col)
{
    auto* n = node<T::NameExpr>(at, col);
    n->name = name;
    return n;
}

Parser::Expr* Parser::globalRef(const String& name, std::uint32_t at, std::uint32_t col)
{
    if (!currentClass_)
        return nameRef(name, at, col);
    auto* m = node<T::MemberAccessExpr>(at, col);
    m->target = nameRef(globals_->name, at, col);
    m->name = name;
    return m;
}

Parser::Expr* Parser::libcRef(const char* member, std::uint32_t at)
{
    auto* m = node<T::MemberAccessExpr>(at);
    m->target = nameRef(ascii(kLibcClass), at);
    m->name = ascii(member);
    return m;
}

Parser::Expr* Parser::memberRef(Expr* target, const char* member, std::uint32_t at)
{
    auto* m = node<T::MemberAccessExpr>(at, target ? target->column : 0);
    m->target = target;
    m->name = ascii(member);
    return m;
}

Parser::Expr* Parser::invoke(Expr* callee, std::vector<Expr*> args, std::uint32_t at, std::uint32_t col)
{
    auto* call = node<T::InvocationExpr>(at, col);
    call->callee = callee;
    for (Expr* e : args) {
        T::Argument a;
        a.value = e;
        call->args.push_back(a);
    }
    return call;
}

Parser::Expr* Parser::castTo(Expr* e, const CType& to, std::uint32_t at)
{
    auto* cast = node<T::CastExpr>(at, e ? e->column : 0);
    cast->typeRef = typeRef(to, at);
    cast->operand = e;
    return cast;
}

Parser::TypeRef* Parser::typeRef(const CType& type, std::uint32_t at) const
{
    TypeRef* ref = arena_.make<TypeRef>();
    ref->line = at;
    const CType* t = &type;
    while (t->kind == Kind::Array) {
        ++ref->arrayRank;
        t = t->elem.get();
    }
    switch (t->kind) {
        case Kind::Void: ref->predefined = T::Tok::Void; break;
        case Kind::Bool: ref->predefined = T::Tok::Bool; break;
        case Kind::Char: ref->predefined = T::Tok::Char; break;
        case Kind::UChar: ref->predefined = T::Tok::Byte; break;
        case Kind::Short: ref->predefined = T::Tok::Short; break;
        case Kind::UShort: ref->predefined = T::Tok::UShort; break;
        case Kind::Int: ref->predefined = T::Tok::Int; break;
        case Kind::UInt: ref->predefined = T::Tok::UInt; break;
        case Kind::Long: ref->predefined = T::Tok::Long; break;
        case Kind::ULong: ref->predefined = T::Tok::ULong; break;
        case Kind::Float: ref->predefined = T::Tok::Float; break;
        case Kind::Double: ref->predefined = T::Tok::Double; break;
        case Kind::Str: ref->predefined = T::Tok::StringKw; break;
        case Kind::Null: ref->predefined = T::Tok::Object; break;
        case Kind::Vector:
            ref->path.push_back(ascii("List"));
            ref->typeArgs.push_back(typeRef(*t->elem, at));
            break;
        case Kind::Map:
            ref->path.push_back(ascii("Dictionary"));
            ref->typeArgs.push_back(typeRef(*t->elem, at));
            ref->typeArgs.push_back(typeRef(*t->elem2, at));
            break;
        case Kind::Pair: fail(at, "a std::pair is only available as a range-for variable over a map");
        case Kind::Struct: {
            // `System.Object` from a host signature arrives dotted.
            std::size_t start = 0;
            for (;;) {
                const std::size_t dot = t->structName.find(u'.', start);
                ref->path.push_back(t->structName.substr(start, dot == String::npos ? String::npos : dot - start));
                if (dot == String::npos)
                    break;
                start = dot + 1;
            }
            break;
        }
        case Kind::Array: break;
    }
    return ref;
}

// ------------------------------------------------------------------ scopes ---

const Parser::Symbol* Parser::lookup(const String& name) const
{
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
        auto found = it->find(name);
        if (found != it->end())
            return &found->second;
    }
    return nullptr;
}

void Parser::declare(const String& name, Symbol symbol, std::uint32_t at)
{
    if (scopes_.back().count(name))
        fail(at, "redefinition of '" + toUtf8(name) + "'");
    scopes_.back()[name] = std::move(symbol);
}

// ------------------------------------------------------------------- unit ---

T::CompilationUnit* Parser::parseUnit()
{
    unit_ = arena_.make<T::CompilationUnit>();
    unit_->sourceName = name_;

    globals_ = arena_.make<T::TypeDecl>();
    globals_->kind = T::DeclKind::Class;
    globals_->name = ascii(kGlobalsClass);
    globals_->fullName = globals_->name;
    globals_->modifiers = T::ModPublic | T::ModStatic;
    globals_->line = 1;

    // First pass: every function signature, struct, typedef and enumerator,
    // so a call may precede the definition. Second pass: everything.
    prepass_ = true;
    scopes_.assign(1, {});
    while (!is(Tok::Eof))
        external();
    prepass_ = false;
    pos_ = 0;
    scopes_.assign(1, {});
    while (!is(Tok::Eof))
        external();

    unit_->types.push_back(globals_);
    auto mainIt = functions_.find(ascii("main"));
    if (mainIt != functions_.end() && !mainIt->second.front().isLibc) {
        const FuncInfo& main = mainIt->second.front();
        // return C$.main();  -- or call it and return 0 when main is void.
        auto* invoke = node<T::InvocationExpr>(1);
        auto* callee = node<T::MemberAccessExpr>(1);
        callee->target = nameRef(globals_->name, 1);
        callee->name = ascii("main");
        invoke->callee = callee;
        for (std::size_t i = 0; i < main.params.size(); ++i) {
            T::Argument arg;
            arg.value = i == 0 ? intLiteral(0, 1) : nullLiteral(1);
            invoke->args.push_back(arg);
        }
        if (main.returnType.kind == Kind::Void) {
            unit_->topLevel.push_back(exprStatement(invoke, 1));
            auto* ret = node<T::ReturnStmt>(1);
            ret->value = intLiteral(0, 1);
            unit_->topLevel.push_back(ret);
        } else {
            auto* ret = node<T::ReturnStmt>(1);
            ret->value = invoke;
            unit_->topLevel.push_back(ret);
        }
    }
    return unit_;
}

/// One external declaration: a typedef, a struct or enum definition, a
/// function prototype or definition, or a global variable.
void Parser::external()
{
    if (eat(Tok::Semicolon))
        return;
    const std::uint32_t at = line();
    if (cpp()) {
        switch (type()) {
            case Tok::Namespace: namespaceBlock(); return;
            case Tok::Using: usingDirective(); return;
            case Tok::Template: fail("templates are not supported");
            case Tok::Extern:
                if (type(1) == Tok::StringLit) {  // extern "C" { ... }
                    advance();
                    advance();
                    if (eat(Tok::LBrace)) {
                        while (!is(Tok::RBrace) && !is(Tok::Eof))
                            external();
                        expect(Tok::RBrace, "'}'");
                        return;
                    }
                }
                break;
            case Tok::Identifier:
                // `Point::Point(...)` -- a constructor defined out of line.
                if (type(1) == Tok::ColonColon && type(2) == Tok::Identifier &&
                    peek(2).text == peek().text && type(3) == Tok::LParen) {
                    auto it = structs_.find(peek().text);
                    if (it == structs_.end())
                        fail("'" + toUtf8(peek().text) + "' is not a class");
                    advance();
                    advance();
                    advance();  // the second name; constructorDefinition starts at '('
                    constructorDefinition(it->second, 0, at);
                    return;
                }
                if (type(1) == Tok::ColonColon && type(2) == Tok::Tilde)
                    fail("destructors are not supported: the collector owns the memory");
                break;
            default: break;
        }
    }
    if (eat(Tok::Typedef)) {
        Specifiers spec = specifiers();
        // `typedef struct { ... } Name;` names the anonymous struct after the alias.
        String anonymous;
        if (spec.base.kind == Kind::Struct && spec.base.structName.size() > 1 &&
            spec.base.structName[0] == u'$')
            anonymous = spec.base.structName;
        do {
            Declarator decl = declarator(spec.base);
            if (decl.isFunction)
                fail("function typedefs are not supported");
            if (!anonymous.empty() && decl.type.kind == Kind::Struct &&
                decl.type.structName == anonymous) {
                // Rename the struct after its alias.
                auto it = structs_.find(anonymous);
                if (it != structs_.end()) {
                    ClassInfo info = it->second;
                    structs_.erase(it);
                    if (info.decl) {
                        info.decl->name = decl.name;
                        info.decl->fullName = decl.name;
                    }
                    structs_[decl.name] = std::move(info);
                }
                decl.type.structName = decl.name;
                anonymous.clear();
            }
            typedefs_[decl.name] = decl.type;
        } while (eat(Tok::Comma));
        expect(Tok::Semicolon, "';'");
        return;
    }

    Specifiers spec = specifiers();
    if (!spec.any)
        fail("expected a declaration");
    if (eat(Tok::Semicolon))
        return;  // struct X { ... };  or  enum { ... };
    if (spec.isAuto)
        fail("'auto' needs an initializer and is only allowed inside a function");
    Declarator decl = declarator(spec.base);
    if (!decl.owner.empty()) {
        outOfLineDefinition(spec, std::move(decl));
        return;
    }
    if (decl.isFunction && cpp()) {
        // `Point p(1, 2);` at file scope is a variable; `int f(int);` is a function.
        const std::size_t save = pos_;
        advance();
        const bool prototype = is(Tok::RParen) || is(Tok::Ellipsis) || startsType();
        pos_ = save;
        decl.isFunction = prototype;
    }
    if (decl.isFunction) {
        functionDefinition(spec, std::move(decl));
        return;
    }
    (void)at;
    globalDeclaration(spec, std::move(decl));
}

// ------------------------------------------------------------------ types ---

bool Parser::startsType() const
{
    switch (type()) {
        case Tok::Void:
        case Tok::Bool:
        case Tok::Char:
        case Tok::Short:
        case Tok::Int:
        case Tok::Long:
        case Tok::Float:
        case Tok::Double:
        case Tok::Signed:
        case Tok::Unsigned:
        case Tok::Struct:
        case Tok::Union:
        case Tok::Enum:
        case Tok::Const:
        case Tok::Volatile:
        case Tok::Static:
        case Tok::Extern:
        case Tok::Register:
        case Tok::Inline:
        case Tok::Auto:
        case Tok::Typedef: return true;
        case Tok::Class:
        case Tok::Virtual:
        case Tok::Explicit:
        case Tok::Constexpr:
        case Tok::Mutable:
        case Tok::Typename: return cpp();
        case Tok::Identifier: {
            const String& word = peek().text;
            if (typedefs_.count(word))
                return true;
            if (hostClassNamed(word) && type(1) != Tok::Dot && type(1) != Tok::LParen &&
                type(1) != Tok::ColonColon)
                return true;
            if (!cpp())
                return false;
            if (type(1) == Tok::ColonColon && namespaces_.count(word)) {
                // `std::string x`, but not `std::cout << x`.
                std::size_t i = 2;
                while (type(i) == Tok::Identifier && type(i + 1) == Tok::ColonColon)
                    i += 2;
                if (type(i) != Tok::Identifier)
                    return false;
                const String& name = peek(i).text;
                return name == u"string" || name == u"vector" || name == u"map" ||
                       name == u"unordered_map" || name == u"pair" || typedefs_.count(name) ||
                       (structs_.count(name) && type(i + 1) != Tok::ColonColon &&
                        type(i + 1) != Tok::LParen) ||
                       exceptionTypeFor(name) != nullptr;
            }
            if (lookup(word))
                return false;
            if (word == u"string" || word == u"vector" || word == u"map" ||
                word == u"unordered_map" || word == u"pair" || exceptionTypeFor(word))
                return true;
            // A class name is a type in C++; `Point::x` and `Point(1, 2)` are expressions.
            return structs_.count(word) != 0 && type(1) != Tok::ColonColon && type(1) != Tok::LParen;
        }
        default: return false;
    }
}

Parser::Specifiers Parser::specifiers()
{
    Specifiers spec;
    int longs = 0;
    bool sawUnsigned = false;
    bool sawSigned = false;
    bool sawBase = false;
    Kind base = Kind::Int;
    const std::uint32_t at = line();
    for (;;) {
        switch (type()) {
            case Tok::Static: spec.isStatic = true; advance(); break;
            case Tok::Extern: spec.isExtern = true; advance(); break;
            case Tok::Const: spec.isConst = true; advance(); break;
            case Tok::Volatile:
            case Tok::Register:
            case Tok::Inline:
            case Tok::Restrict: advance(); break;
            case Tok::Auto:
                advance();
                if (cpp()) {
                    spec.isAuto = true;
                    spec.any = true;
                    while (is(Tok::Const))
                        advance();
                    return spec;
                }
                break;
            case Tok::Virtual: spec.isVirtual = true; advance(); break;
            case Tok::Constexpr: spec.isConst = true; advance(); break;
            case Tok::Explicit:
            case Tok::Mutable:
            case Tok::Typename: advance(); break;
            case Tok::Friend: fail("'friend' is not supported");
            case Tok::Class:
                advance();
                spec.base = structType(at);
                spec.any = true;
                return spec;
            case Tok::Typedef: fail("'typedef' is only allowed at file scope");
            case Tok::Signed: sawSigned = true; advance(); break;
            case Tok::Unsigned: sawUnsigned = true; advance(); break;
            case Tok::Long: ++longs; advance(); break;
            case Tok::Void: base = Kind::Void; sawBase = true; spec.isVoid = true; advance(); break;
            case Tok::Bool: base = cpp() ? Kind::Bool : Kind::Int; sawBase = true; advance(); break;
            case Tok::Char: base = Kind::Char; sawBase = true; advance(); break;
            case Tok::Short: base = Kind::Short; sawBase = true; advance(); break;
            case Tok::Int: base = Kind::Int; sawBase = true; advance(); break;
            case Tok::Float: base = Kind::Float; sawBase = true; advance(); break;
            case Tok::Double: base = Kind::Double; sawBase = true; advance(); break;
            case Tok::Struct:
                advance();
                spec.base = structType(at);
                spec.any = true;
                return spec;
            case Tok::Union: fail("unions are not supported");
            case Tok::Enum:
                enumDefinition();
                spec.base = CType::of(Kind::Int);
                spec.any = true;
                return spec;
            case Tok::Identifier: {
                if (sawBase || longs || sawSigned || sawUnsigned)
                    goto done;
                if (cpp() && (skipQualifier() || !lookup(peek().text))) {
                    const String& word = peek().text;
                    if (word == u"string" || word == u"vector" || word == u"map" ||
                        word == u"unordered_map" || word == u"pair" || exceptionTypeFor(word) ||
                        (structs_.count(word) && !typedefs_.count(word))) {
                        spec.base = typeName(at);
                        spec.any = true;
                        while (is(Tok::Const) || is(Tok::Volatile))
                            advance();
                        return spec;
                    }
                }
                auto alias = typedefs_.find(peek().text);
                if (alias == typedefs_.end()) {
                    // A host class used as a type: `Vec2 v = Vec2(1, 2);`.
                    if (hostClassNamed(peek().text) && type(1) != Tok::Dot && type(1) != Tok::LParen &&
                        type(1) != Tok::ColonColon) {
                        spec.base = CType::structOf(advance().text);
                        spec.any = true;
                        return spec;
                    }
                    goto done;
                }
                advance();
                spec.base = alias->second;
                spec.any = true;
                while (is(Tok::Const) || is(Tok::Volatile))
                    advance();
                return spec;
            }
            default: goto done;
        }
        spec.any = true;
    }
done:
    if (!sawBase && !longs && !sawSigned && !sawUnsigned) {
        if (spec.any)
            fail("expected a type");
        return spec;
    }
    if (longs > 0 && (base == Kind::Int || !sawBase))
        base = Kind::Long;
    if (longs > 0 && base == Kind::Double)
        base = Kind::Double;
    if (sawUnsigned) {
        switch (base) {
            case Kind::Char: base = Kind::UChar; break;
            case Kind::Short: base = Kind::UShort; break;
            case Kind::Int: base = Kind::UInt; break;
            case Kind::Long: base = Kind::ULong; break;
            default: fail("'unsigned' does not apply to this type");
        }
    }
    spec.base = CType::of(base);
    spec.any = true;
    return spec;
}

CType Parser::pointerTo(const CType& base, std::uint32_t at) const
{
    switch (base.kind) {
        case Kind::Char: return CType::of(Kind::Str);
        case Kind::Struct: return base;  // a class is already a reference
        case Kind::Void: fail(at, "'void*' is not supported");
        case Kind::Bool:
            if (cpp())
                return CType::arrayOf(base);
            fail(at, "a pointer to this type is not supported");
        case Kind::Vector:
        case Kind::Map: return base;  // a container is a reference already
        default: return CType::arrayOf(base);
    }
}

Parser::Declarator Parser::declarator(const CType& base, bool allowAbstract)
{
    Declarator d;
    d.line = line();
    d.column = column();
    while (eat(Tok::Star)) {
        ++d.pointers;
        while (is(Tok::Const) || is(Tok::Restrict) || is(Tok::Volatile))
            advance();
    }
    if (cpp() && (is(Tok::Amp) || is(Tok::AndAnd))) {
        advance();
        d.isReference = true;
        while (is(Tok::Const))
            advance();
    }
    if (is(Tok::LParen))
        fail("function pointers are not supported");
    if (is(Tok::Identifier)) {
        d.name = advance().text;
        if (cpp() && is(Tok::ColonColon) && type(1) == Tok::Identifier) {
            // `Point::length`: a member defined outside its class.
            advance();
            d.owner = d.name;
            d.name = advance().text;
        }
    } else if (!allowAbstract) {
        fail(std::string("expected a name but found ") + tokenName(type()));
    }
    CType t = base;
    for (int i = 0; i < d.pointers; ++i)
        t = pointerTo(t, d.line);
    // `int a[3][4]`: the lengths are read outermost first; the type nests the
    // other way round.
    while (eat(Tok::LBracket)) {
        Expr* length = nullptr;
        if (!is(Tok::RBracket))
            length = coerce(assignment(), CType::of(Kind::Int));
        expect(Tok::RBracket, "']'");
        d.arrayLengths.push_back(length);
    }
    for (std::size_t i = 0; i < d.arrayLengths.size(); ++i)
        t = CType::arrayOf(t);
    d.type = t;
    d.isFunction = is(Tok::LParen);
    return d;
}

/// `struct Tag`, `struct Tag { ... }` or `struct { ... }` after the keyword.
CType Parser::structType(std::uint32_t at)
{
    String tag;
    if (is(Tok::Identifier)) {
        tag = advance().text;
    } else if (!is(Tok::LBrace)) {
        fail("expected a struct name or body");
    } else {
        tag = ascii("$anon" + std::to_string(structs_.size() + 1));
    }
    if (cpp()) {
        const bool isClass = toks_[pos_ - 2].type == Tok::Class;
        if (is(Tok::Colon) || is(Tok::LBrace)) {
            classDefinition(tag, isClass, at);
        } else if (prepass_ && !structs_.count(tag)) {
            // `class X;` -- a forward declaration; the definition may follow.
            ClassInfo forward;
            forward.name = tag;
            forward.isClass = isClass;
            structs_[tag] = std::move(forward);
        }
        return CType::structOf(tag);
    }
    if (is(Tok::LBrace))
        structDefinition(tag, at);
    return CType::structOf(tag);
}

void Parser::structDefinition(const String& tag, std::uint32_t at)
{
    expect(Tok::LBrace, "'{'");
    ClassInfo info;
    info.name = tag;
    T::TypeDecl* decl = nullptr;
    if (!prepass_) {
        decl = arena_.make<T::TypeDecl>();
        decl->kind = T::DeclKind::Class;
        decl->name = tag;
        decl->fullName = tag;
        decl->modifiers = T::ModPublic;
        decl->line = at;
    }
    // Register early so a self-referential `struct node* next` resolves.
    structs_[tag] = info;
    while (!is(Tok::RBrace)) {
        if (is(Tok::Eof))
            fail("unterminated struct body");
        Specifiers spec = specifiers();
        if (!spec.any)
            fail("expected a field declaration");
        do {
            Declarator d = declarator(spec.base);
            if (d.isFunction)
                fail("function pointers are not supported");
            if (is(Tok::Colon))
                fail("bit-fields are not supported");
            if (d.type.kind == Kind::Void)
                fail("a field cannot be void");
            info.fields.push_back({d.name, d.type, false});
            if (decl) {
                T::FieldDecl field;
                field.line = d.line;
                field.name = d.name;
                field.modifiers = T::ModPublic;
                field.typeRef = typeRef(d.type, d.line);
                // Arrays and nested structs live inline in C: make them exist.
                if (d.type.kind == Kind::Array && d.pointers == 0 && !d.arrayLengths.empty() &&
                    d.arrayLengths[0]) {
                    field.init = arrayInitializer(d.type, d.arrayLengths[0], d.line);
                } else if (d.type.kind == Kind::Struct && d.pointers == 0 &&
                           d.type.structName != tag && structs_.count(d.type.structName)) {
                    field.init = defaultValue(d.type, nullptr, d.line);
                }
                decl->fields.push_back(std::move(field));
            }
        } while (eat(Tok::Comma));
        expect(Tok::Semicolon, "';'");
    }
    advance();
    info.decl = decl;
    structs_[tag] = std::move(info);
    if (decl)
        unit_->types.push_back(decl);
}

void Parser::enumDefinition()
{
    expect(Tok::Enum, "'enum'");
    // `enum class Color`: the enumerators are only reachable as `Color::Red`.
    const bool scoped = cpp() && (eat(Tok::Class) || eat(Tok::Struct));
    String tag;
    if (is(Tok::Identifier)) {
        tag = advance().text;  // every enum is an int; the tag only scopes the names
        if (cpp())
            typedefs_[tag] = CType::of(Kind::Int);
    }
    if (cpp() && eat(Tok::Colon))
        specifiers();  // the underlying type
    if (!is(Tok::LBrace))
        return;
    advance();
    std::int64_t next = 0;
    while (!is(Tok::RBrace)) {
        const std::uint32_t at = line();
        const String name = expectName("an enumerator name");
        if (eat(Tok::Assign))
            next = constantInt("an enumerator value");
        if (!scoped)
            constants_[name] = next;
        if (!tag.empty())
            constants_[tag + u"::" + name] = next;
        ++next;
        (void)at;
        if (!eat(Tok::Comma))
            break;
    }
    expect(Tok::RBrace, "'}'");
}

/// How a function body uses a scalar pointer parameter: 1 dereferenced
/// (`*p`), 2 indexed (`p[i]`), 3 both, 0 neither.
int Parser::pointerUsage(const String& name, std::size_t from, std::size_t to)
{
    int usage = 0;
    for (std::size_t i = from; i < to; ++i) {
        const Token& t = toks_[i];
        if (t.type != Tok::Identifier || t.text != name)
            continue;
        const Tok after = i + 1 < to ? toks_[i + 1].type : Tok::Eof;
        if (after == Tok::LBracket) {
            usage |= 2;
            continue;
        }
        if (i > from && toks_[i - 1].type == Tok::Star) {
            // `*p` -- unless the star is a multiplication: `a * p`.
            const Tok before = i >= 2 ? toks_[i - 2].type : Tok::Eof;
            const bool operand = before == Tok::Identifier || before == Tok::IntLit ||
                                 before == Tok::FloatLit || before == Tok::CharLit ||
                                 before == Tok::StringLit || before == Tok::RParen ||
                                 before == Tok::RBracket;
            if (!operand) {
                usage |= 1;
                continue;
            }
        }
        // Passed straight on: `g(p)`. It is whatever g's parameter is.
        const Tok before1 = i > from ? toks_[i - 1].type : Tok::Eof;
        if ((after == Tok::Comma || after == Tok::RParen) &&
            (before1 == Tok::LParen || before1 == Tok::Comma)) {
            int depth = 0;
            std::size_t argIndex = 0;
            std::size_t k = i;
            while (k > from) {
                --k;
                const Tok t2 = toks_[k].type;
                if (t2 == Tok::RParen || t2 == Tok::RBracket)
                    ++depth;
                else if (t2 == Tok::LParen || t2 == Tok::LBracket) {
                    if (depth == 0)
                        break;
                    --depth;
                } else if (t2 == Tok::Comma && depth == 0) {
                    ++argIndex;
                }
            }
            if (k > from && toks_[k].type == Tok::LParen && toks_[k - 1].type == Tok::Identifier) {
                auto callee = functions_.find(toks_[k - 1].text);
                if (callee != functions_.end() && argIndex < callee->second.front().byRef.size()) {
                    usage |= callee->second.front().byRef[argIndex] ? 1 : 2;
                    continue;
                }
            }
        }
        if (after == Tok::PlusPlus || after == Tok::MinusMinus || after == Tok::PlusAssign ||
            after == Tok::MinusAssign || after == Tok::Arrow)
            fail(t.line, "pointer arithmetic on '" + toUtf8(name) + "' is not supported");
    }
    return usage;
}

std::vector<T::ParamDecl> Parser::parameters(FuncInfo& info, std::vector<CType>& types,
                                              std::vector<String>& names,
                                              std::vector<bool>& scalarPointer)
{
    std::vector<T::ParamDecl> params;
    expect(Tok::LParen, "'('");
    if (is(Tok::Void) && type(1) == Tok::RParen) {
        advance();
        advance();
        return params;
    }
    while (!is(Tok::RParen)) {
        if (eat(Tok::Ellipsis)) {
            info.variadic = true;
            break;
        }
        Specifiers spec = specifiers();
        if (!spec.any)
            fail("expected a parameter type");
        if (spec.isAuto)
            fail("a parameter cannot be 'auto'");
        Declarator d = declarator(spec.base, /*allowAbstract=*/true);
        if (d.isFunction)
            fail("function pointers are not supported");
        CType t = d.type;
        // `int a[]` as a parameter is a pointer, which is an array here.
        bool scalar = false;
        if (d.pointers == 1 && d.arrayLengths.empty() && spec.base.isNumeric() &&
            spec.base.kind != Kind::Char)
            scalar = true;
        if (d.name.empty())
            d.name = ascii("$p" + std::to_string(params.size()));
        T::ParamDecl param;
        param.name = d.name;
        param.line = d.line;
        param.typeRef = typeRef(t, d.line);
        // C++: `T&` passes a scalar or a string by reference; a class or a
        // container is a reference already, and `const T&` is by value.
        const bool byRef = d.isReference && !spec.isConst && d.pointers == 0 &&
                           (t.isNumeric() || t.kind == Kind::Bool || t.kind == Kind::Str);
        if (byRef)
            param.mode = T::ParamMode::Ref;
        if (cpp() && eat(Tok::Assign)) {
            if (byRef)
                fail(d.line, "a reference parameter cannot have a default argument");
            param.defaultValue = coerce(assignment(), t);
        } else {
            if (info.required != params.size())
                fail(d.line, "a parameter without a default cannot follow one with a default");
            info.required = params.size() + 1;
        }
        info.defaults.push_back(param.defaultValue);
        params.push_back(std::move(param));
        types.push_back(t);
        names.push_back(d.name);
        scalarPointer.push_back(scalar);
        info.byRef.push_back(byRef);
        info.refByPointer.push_back(false);
        if (!is(Tok::RParen))
            expect(Tok::Comma, "','");
    }
    expect(Tok::RParen, "')'");
    return params;
}

void Parser::functionDefinition(const Specifiers& spec, Declarator decl)
{
    const std::uint32_t at = decl.line;
    if (spec.isTypedef)
        fail(at, "function typedefs are not supported");
    FuncInfo info;
    info.returnType = decl.type;
    std::vector<CType> types;
    std::vector<String> names;
    std::vector<bool> scalarPointer;
    std::vector<T::ParamDecl> params = parameters(info, types, names, scalarPointer);
    if (info.variadic)
        fail(at, "variadic functions are not supported");

    if (cpp()) {
        while (is(Tok::Const) || is(Tok::Noexcept))
            advance();
    }
    const bool isDefinition = is(Tok::LBrace);
    if (!isDefinition) {
        expect(Tok::Semicolon, "';'");
    }
    resolvePointerParams(params, types, names, scalarPointer, info, isDefinition);
    registerFunction(functions_[decl.name], info, decl.name, at, &params);
    if (!isDefinition)
        return;
    if (prepass_) {
        skipBalanced(Tok::LBrace, Tok::RBrace);
        return;
    }

    auto* method = arena_.make<T::MethodDecl>();
    method->line = at;
    method->name = decl.name;
    method->modifiers = T::ModStatic | T::ModPublic;
    method->returnTypeRef = typeRef(decl.type, at);
    method->params = params;
    method->owner = globals_;
    method->body = functionBody(params, types, names, info, decl.type, decl.name);
    if (cpp() && decl.name == u"main" && decl.type.kind == Kind::Int) {
        // C++ lets main fall off its end, returning 0.
        auto* body = static_cast<T::BlockStmt*>(method->body);
        if (body->statements.empty() || !endsSection(body->statements.back())) {
            auto* ret = node<T::ReturnStmt>(at);
            ret->value = intLiteral(0, at);
            body->statements.push_back(ret);
        }
    }
    globals_->methods.push_back(method);
}

/// Scalar pointer parameters: by reference when the body dereferences them,
/// arrays when it indexes them. The body is the block that follows.
void Parser::resolvePointerParams(std::vector<T::ParamDecl>& params, std::vector<CType>& types,
                                  const std::vector<String>& names,
                                  const std::vector<bool>& scalarPointer, FuncInfo& info,
                                  bool isDefinition)
{
    std::size_t bodyStart = pos_;
    std::size_t bodyEnd = pos_;
    if (isDefinition) {
        int depth = 0;
        for (bodyEnd = pos_; bodyEnd < toks_.size(); ++bodyEnd) {
            if (toks_[bodyEnd].type == Tok::LBrace)
                ++depth;
            else if (toks_[bodyEnd].type == Tok::RBrace && --depth == 0)
                break;
        }
    }
    for (std::size_t i = 0; i < params.size(); ++i) {
        if (!scalarPointer[i])
            continue;
        const int usage = isDefinition ? pointerUsage(names[i], bodyStart, bodyEnd) : 0;
        if (usage == 3)
            fail(params[i].line, "parameter '" + toUtf8(names[i]) +
                                     "' is both dereferenced and indexed; use one or the other");
        if (usage == 1) {
            params[i].mode = T::ParamMode::Ref;
            params[i].typeRef = typeRef(*types[i].elem, params[i].line);
            CType element = *types[i].elem;  // copied out first: the array type owns it
            types[i] = std::move(element);
            info.byRef[i] = true;
            info.refByPointer[i] = true;
        }
    }
    info.params = types;
}

void Parser::registerFunction(std::vector<FuncInfo>& overloads, FuncInfo& info, const String& name,
                              std::uint32_t at, std::vector<T::ParamDecl>* params)
{
    for (FuncInfo& existing : overloads) {
        if (existing.isLibc)
            continue;
        bool same = existing.params.size() == info.params.size();
        for (std::size_t i = 0; same && i < info.params.size(); ++i)
            same = existing.params[i].same(info.params[i]);
        if (same || !cpp()) {
            // A prototype, the first pass's record of this definition, or in
            // C a redeclaration, which has to agree.
            if (!same)
                fail(at, "conflicting declaration of '" + toUtf8(name) + "'");
            // Default arguments declared by the prototype carry over.
            if (existing.required < info.required) {
                info.required = existing.required;
                info.defaults = existing.defaults;
                if (params) {
                    for (std::size_t i = 0; i < params->size() && i < info.defaults.size(); ++i)
                        if (!(*params)[i].defaultValue)
                            (*params)[i].defaultValue = info.defaults[i];
                }
            }
            if (info.access == 0)
                info.access = existing.access;
            existing = info;
            return;
        }
    }
    overloads.push_back(info);
}

Parser::Stmt* Parser::functionBody(const std::vector<T::ParamDecl>& params,
                                   const std::vector<CType>& types, const std::vector<String>& names,
                                   const FuncInfo& info, const CType& returnType,
                                   const String& qualifiedName)
{
    pushScope();
    for (std::size_t i = 0; i < names.size(); ++i) {
        Symbol s;
        s.type = types[i];
        s.isRef = info.byRef[i] && info.refByPointer[i];
        declare(names[i], s, params[i].line);
    }
    returnType_ = returnType;
    currentFunction_ = qualifiedName;
    Stmt* body = block();
    popScope();
    return body;
}

void Parser::globalDeclaration(const Specifiers& spec, Declarator first)
{
    Declarator decl = std::move(first);
    for (;;) {
        if (decl.type.kind == Kind::Void)
            fail(decl.line, "a variable cannot be void");
        if (decl.isReference)
            fail(decl.line, "a reference must be a parameter or a range-for variable");
        Expr* init = nullptr;
        if (eat(Tok::Assign)) {
            init = initializer(decl.type, &decl, decl.line);
        } else if (cpp() && (is(Tok::LParen) || is(Tok::LBrace)) && decl.arrayLengths.empty()) {
            std::vector<Typed> args = arguments();
            init = construct(decl.type, args, decl.line, decl.column).expr;
        } else {
            init = defaultValue(decl.type, &decl, decl.line);
        }
        if (scopes_.front().count(decl.name) && !spec.isExtern)
            fail(decl.line, "redefinition of '" + toUtf8(decl.name) + "'");
        std::int64_t folded = 0;
        const bool constant = spec.isConst && decl.type.isNumeric() && init && evalConst(init, folded);
        if (!prepass_) {
            T::FieldDecl field;
            field.line = decl.line;
            field.name = decl.name;
            field.modifiers = constant ? (T::ModConst | T::ModPublic) : (T::ModStatic | T::ModPublic);
            field.typeRef = typeRef(decl.type, decl.line);
            field.init = init;
            globals_->fields.push_back(std::move(field));
        }
        std::int64_t value = 0;
        if (constant && evalConst(init, value)) {
            constants_[decl.name] = value;
        } else {
            Symbol s;
            s.type = decl.type;
            s.isGlobal = true;
            s.globalName = decl.name;
            scopes_.front()[decl.name] = s;
        }
        if (!eat(Tok::Comma))
            break;
        decl = declarator(spec.base);
    }
    expect(Tok::Semicolon, "';'");
}

// ------------------------------------------------------------ initializers ---

Parser::Expr* Parser::defaultValue(const CType& type, const Declarator* decl, std::uint32_t at)
{
    if (type.kind == Kind::Struct) {
        if (cpp() && type.structName.substr(0, 7) == u"System.")
            return nullptr;  // an exception variable
        auto* create = node<T::ObjectCreationExpr>(at);
        create->typeRef = typeRef(type, at);
        return create;
    }
    if (cpp()) {
        // C++ default-constructs: an empty string, an empty container.
        if (type.kind == Kind::Str && (!decl || decl->pointers == 0))
            return stringLiteral(String(), at);
        if (type.isContainer())
            return containerCreation(type, at);
    }
    if (type.kind == Kind::Array && decl && !decl->arrayLengths.empty() && decl->arrayLengths[0]) {
        if (decl->arrayLengths.size() > 2)
            fail(at, "arrays of more than two dimensions are not supported");
        if (decl->arrayLengths.size() == 2) {
            if (!decl->arrayLengths[1])
                fail(at, "the inner dimension of a two-dimensional array needs a length");
            return newJaggedArray(*type.elem->elem, decl->arrayLengths[0], decl->arrayLengths[1], at);
        }
        return arrayInitializer(type, decl->arrayLengths[0], at);
    }
    return nullptr;
}

/// `new T[n]`, or the helper that also constructs every element when T is a struct.
Parser::Expr* Parser::arrayInitializer(const CType& type, Expr* declaredLength, std::uint32_t at)
{
    if (type.elem->kind == Kind::Struct)
        return newStructArray(*type.elem, declaredLength, at);
    auto* create = node<T::ArrayCreationExpr>(at);
    create->elementTypeRef = typeRef(*type.elem, at);
    create->size = declaredLength;
    return create;
}

/// An empty static helper method on the synthesized class, registered and
/// returned for the caller to fill in.
T::MethodDecl* Parser::newHelper(const String& name, const CType& returnType, std::uint32_t at)
{
    auto* method = arena_.make<T::MethodDecl>();
    method->line = at;
    method->name = name;
    method->modifiers = T::ModStatic | T::ModPublic;
    method->returnTypeRef = typeRef(returnType, at);
    method->owner = globals_;
    globals_->methods.push_back(method);
    return method;
}

/// A static method `C$.$new_T(int n)` that allocates and fills the array, made
/// once per struct type.
Parser::Expr* Parser::newStructArray(const CType& element, Expr* length, std::uint32_t at)
{
    const String helperName = ascii("$new_") + element.structName;
    if (!helpers_.count(helperName)) {
        helpers_[helperName] = true;
        // static T[] $new_T(int n) { T[] a = new T[n]; for (int i = 0; i < n; i++) a[i] = new T(); return a; }
        T::MethodDecl* method = newHelper(helperName, CType::arrayOf(element), at);
        T::ParamDecl n;
        n.name = ascii("n");
        n.line = at;
        n.typeRef = typeRef(CType::of(Kind::Int), at);
        method->params.push_back(std::move(n));

        auto* body = node<T::BlockStmt>(at);
        auto* declStmt = node<T::LocalDeclStmt>(at);
        declStmt->typeRef = typeRef(CType::arrayOf(element), at);
        T::VarDeclarator var;
        var.name = ascii("a");
        var.line = at;
        auto* create = node<T::ArrayCreationExpr>(at);
        create->elementTypeRef = typeRef(element, at);
        create->size = nameRef(ascii("n"), at);
        var.init = create;
        declStmt->vars.push_back(std::move(var));
        body->statements.push_back(declStmt);

        auto* loop = node<T::ForStmt>(at);
        auto* init = node<T::LocalDeclStmt>(at);
        init->typeRef = typeRef(CType::of(Kind::Int), at);
        T::VarDeclarator i;
        i.name = ascii("i");
        i.line = at;
        i.init = intLiteral(0, at);
        init->vars.push_back(std::move(i));
        loop->init.push_back(init);
        auto* cond = node<T::BinaryExpr>(at);
        cond->op = BinaryOp::Less;
        cond->left = nameRef(ascii("i"), at);
        cond->right = nameRef(ascii("n"), at);
        loop->condition = cond;
        auto* step = node<T::UnaryExpr>(at);
        step->op = UnaryOp::PostIncrement;
        step->operand = nameRef(ascii("i"), at);
        loop->update.push_back(step);
        auto* assign = node<T::AssignmentExpr>(at);
        auto* element_ = node<T::ElementAccessExpr>(at);
        element_->target = nameRef(ascii("a"), at);
        element_->index = nameRef(ascii("i"), at);
        assign->target = element_;
        auto* make = node<T::ObjectCreationExpr>(at);
        make->typeRef = typeRef(element, at);
        assign->value = make;
        loop->body = exprStatement(assign, at);
        body->statements.push_back(loop);

        auto* ret = node<T::ReturnStmt>(at);
        ret->value = nameRef(ascii("a"), at);
        body->statements.push_back(ret);
        method->body = body;
    }
    return invoke(globalRef(helperName, at), {length}, at);
}

/// `new T[a][]` with every row allocated: a two-dimensional C array.
Parser::Expr* Parser::newJaggedArray(const CType& element, Expr* outer, Expr* inner,
                                     std::uint32_t at)
{
    if (element.kind == Kind::Struct)
        fail(at, "two-dimensional arrays of structs are not supported");
    const String helperName = ascii("$new2_") + fromUtf8(kindName(element.kind));
    if (!helpers_.count(helperName)) {
        helpers_[helperName] = true;
        // static T[][] $new2_T(int a, int b) { T[][] m = new T[a][]; for (int i = 0; i < a; i++) m[i] = new T[b]; return m; }
        const CType row = CType::arrayOf(element);
        const CType matrix = CType::arrayOf(row);
        T::MethodDecl* method = newHelper(helperName, matrix, at);
        for (const char* pname : {"a", "b"}) {
            T::ParamDecl p;
            p.name = ascii(pname);
            p.line = at;
            p.typeRef = typeRef(CType::of(Kind::Int), at);
            method->params.push_back(std::move(p));
        }
        auto* body = node<T::BlockStmt>(at);
        auto* declStmt = node<T::LocalDeclStmt>(at);
        declStmt->typeRef = typeRef(matrix, at);
        T::VarDeclarator var;
        var.name = ascii("m");
        var.line = at;
        auto* create = node<T::ArrayCreationExpr>(at);
        create->elementTypeRef = typeRef(row, at);
        create->size = nameRef(ascii("a"), at);
        var.init = create;
        declStmt->vars.push_back(std::move(var));
        body->statements.push_back(declStmt);
        auto* loop = node<T::ForStmt>(at);
        auto* init = node<T::LocalDeclStmt>(at);
        init->typeRef = typeRef(CType::of(Kind::Int), at);
        T::VarDeclarator i;
        i.name = ascii("i");
        i.line = at;
        i.init = intLiteral(0, at);
        init->vars.push_back(std::move(i));
        loop->init.push_back(init);
        auto* cond = node<T::BinaryExpr>(at);
        cond->op = BinaryOp::Less;
        cond->left = nameRef(ascii("i"), at);
        cond->right = nameRef(ascii("a"), at);
        loop->condition = cond;
        auto* step = node<T::UnaryExpr>(at);
        step->op = UnaryOp::PostIncrement;
        step->operand = nameRef(ascii("i"), at);
        loop->update.push_back(step);
        auto* assign = node<T::AssignmentExpr>(at);
        auto* slot = node<T::ElementAccessExpr>(at);
        slot->target = nameRef(ascii("m"), at);
        slot->index = nameRef(ascii("i"), at);
        assign->target = slot;
        auto* rowCreate = node<T::ArrayCreationExpr>(at);
        rowCreate->elementTypeRef = typeRef(element, at);
        rowCreate->size = nameRef(ascii("b"), at);
        assign->value = rowCreate;
        loop->body = exprStatement(assign, at);
        body->statements.push_back(loop);
        auto* ret = node<T::ReturnStmt>(at);
        ret->value = nameRef(ascii("m"), at);
        body->statements.push_back(ret);
        method->body = body;
    }
    return invoke(globalRef(helperName, at), {outer, inner}, at);
}

/// The `= ...` of a declaration: a brace list for arrays and structs, a
/// string for a char array, an expression otherwise.
Parser::Expr* Parser::initializer(const CType& type, Declarator* decl, std::uint32_t at)
{
    if (type.kind == Kind::Array) {
        Expr* declared = decl && !decl->arrayLengths.empty() ? decl->arrayLengths[0] : nullptr;
        if (is(Tok::StringLit) && type.elem->kind == Kind::Char) {
            // char s[] = "text": the characters and the terminating NUL.
            const Token& lit = advance();
            std::int64_t n = 0;
            if (declared && !evalConst(declared, n))
                fail(at, "the length of an initialised char array must be a constant");
            auto* create = node<T::ArrayCreationExpr>(at);
            create->elementTypeRef = typeRef(*type.elem, at);
            create->hasInitializer = true;
            const std::size_t count = declared ? std::size_t(n) : lit.text.size() + 1;
            if (lit.text.size() > count)
                fail(at, "initializer string is longer than the array");
            for (std::size_t i = 0; i < count; ++i) {
                auto* ch = node<T::LiteralExpr>(at);
                ch->literal = LiteralKind::Char;
                ch->intValue = i < lit.text.size() ? lit.text[i] : 0;
                create->elements.push_back(ch);
            }
            return create;
        }
        if (!is(Tok::LBrace)) {
            // `int* p = malloc(...)`, `int* p = other`: another array.
            Typed value = assignment();
            return coerce(value, type);
        }
        advance();
        auto* create = node<T::ArrayCreationExpr>(at);
        create->elementTypeRef = typeRef(*type.elem, at);
        create->hasInitializer = true;
        while (!is(Tok::RBrace)) {
            if (is(Tok::LBrace) || (is(Tok::StringLit) && type.elem->kind == Kind::Array)) {
                Declarator inner;
                if (decl && decl->arrayLengths.size() > 1)
                    inner.arrayLengths.assign(decl->arrayLengths.begin() + 1, decl->arrayLengths.end());
                create->elements.push_back(initializer(*type.elem, &inner, line()));
            } else if (type.elem->kind == Kind::Struct && is(Tok::LBrace)) {
                create->elements.push_back(structInitializer(*type.elem, line()));
            } else {
                create->elements.push_back(coerce(assignment(), *type.elem));
            }
            if (!eat(Tok::Comma))
                break;
        }
        expect(Tok::RBrace, "'}'");
        // `int a[5] = {1, 2}` fills the rest with zeros; `{0}` is the idiom.
        std::int64_t n = 0;
        if (declared && evalConst(declared, n)) {
            if (std::int64_t(create->elements.size()) > n)
                fail(at, "too many initializers for the array");
            while (std::int64_t(create->elements.size()) < n) {
                if (type.elem->kind == Kind::Struct) {
                    auto* make = node<T::ObjectCreationExpr>(at);
                    make->typeRef = typeRef(*type.elem, at);
                    create->elements.push_back(make);
                } else if (type.elem->kind == Kind::Array) {
                    Declarator inner;
                    if (decl && decl->arrayLengths.size() > 1)
                        inner.arrayLengths.assign(decl->arrayLengths.begin() + 1, decl->arrayLengths.end());
                    Expr* row = defaultValue(*type.elem, &inner, at);
                    if (!row)
                        fail(at, "cannot pad a nested array without a length");
                    create->elements.push_back(row);
                } else if (type.elem->isFloating()) {
                    auto* zero = node<T::LiteralExpr>(at);
                    zero->literal = LiteralKind::Double;
                    Typed z;
                    z.expr = zero;
                    z.type = CType::of(Kind::Double);
                    create->elements.push_back(coerce(z, *type.elem));
                } else if (type.elem->kind == Kind::Str) {
                    create->elements.push_back(nullLiteral(at));
                } else {
                    Typed z;
                    z.expr = intLiteral(0, at);
                    z.type = CType::of(Kind::Int);
                    create->elements.push_back(coerce(z, *type.elem));
                }
            }
        }
        return create;
    }
    if (type.kind == Kind::Struct && is(Tok::LBrace))
        return structInitializer(type, at);
    if (cpp() && type.isContainer() && is(Tok::LBrace)) {
        std::vector<Typed> args = arguments();
        return construct(type, args, at, column()).expr;
    }
    return coerce(assignment(), type);
}

/// `{1, 2}` or `{.x = 1, .y = 2}` for a struct: an object initializer.
Parser::Expr* Parser::structInitializer(const CType& type, std::uint32_t at)
{
    expect(Tok::LBrace, "'{'");
    auto info = structs_.find(type.structName);
    if (info == structs_.end())
        fail(at, "unknown struct '" + toUtf8(type.structName) + "'");
    if (cpp() && !info->second.ctors.empty())
        fail(at, "'" + toUtf8(type.structName) + "' has constructors; call one");
    auto* create = node<T::ObjectCreationExpr>(at);
    create->typeRef = typeRef(type, at);
    std::size_t next = 0;
    while (!is(Tok::RBrace)) {
        std::size_t index = next;
        if (eat(Tok::Dot)) {
            const String field = expectName("a field name");
            index = info->second.fields.size();
            for (std::size_t i = 0; i < info->second.fields.size(); ++i) {
                if (info->second.fields[i].name == field)
                    index = i;
            }
            if (index == info->second.fields.size())
                fail("struct '" + toUtf8(type.structName) + "' has no field '" + toUtf8(field) + "'");
            expect(Tok::Assign, "'='");
        }
        while (index < info->second.fields.size() && info->second.fields[index].isStatic)
            ++index;
        if (index >= info->second.fields.size())
            fail("too many initializers for struct '" + toUtf8(type.structName) + "'");
        const CType& fieldType = info->second.fields[index].type;
        T::InitializerEntry entry;
        entry.name = info->second.fields[index].name;
        if (is(Tok::LBrace) || (is(Tok::StringLit) && fieldType.kind == Kind::Array)) {
            entry.value = initializer(fieldType, nullptr, line());
        } else {
            entry.value = coerce(assignment(), fieldType);
        }
        create->initializers.push_back(std::move(entry));
        next = index + 1;
        if (!eat(Tok::Comma))
            break;
    }
    expect(Tok::RBrace, "'}'");
    return create;
}

// -------------------------------------------------------------- statements ---

Parser::Stmt* Parser::exprStatement(Expr* e, std::uint32_t at)
{
    if (!e)
        return node<T::EmptyStmt>(at);
    auto* s = node<T::ExprStmt>(at);
    s->expr = e;
    return s;
}

Parser::Stmt* Parser::block()
{
    const std::uint32_t at = line();
    expect(Tok::LBrace, "'{'");
    if (++depth_ > 200)
        fail("nesting too deep");
    auto* b = node<T::BlockStmt>(at);
    pushScope();
    while (!is(Tok::RBrace)) {
        if (is(Tok::Eof))
            fail("unterminated block");
        statement(b->statements);
    }
    advance();
    popScope();
    --depth_;
    return b;
}

Parser::Stmt* Parser::embedded()
{
    const std::uint32_t at = line();
    std::vector<Stmt*> out;
    statement(out);
    if (out.size() == 1)
        return out[0];
    auto* b = node<T::BlockStmt>(at);
    b->statements = std::move(out);
    return b;
}

void Parser::statement(std::vector<Stmt*>& out)
{
    NestingScope nest(nest_);
    if (nest.tooDeep())
        fail("source is nested too deeply");
    const std::uint32_t at = line();
    switch (type()) {
        case Tok::LBrace: out.push_back(block()); return;
        case Tok::Semicolon: advance(); out.push_back(node<T::EmptyStmt>(at)); return;
        case Tok::If: out.push_back(ifStatement()); return;
        case Tok::While: out.push_back(whileStatement()); return;
        case Tok::Do: out.push_back(doStatement()); return;
        case Tok::For: out.push_back(forStatement()); return;
        case Tok::Switch: out.push_back(switchStatement()); return;
        case Tok::Return: out.push_back(returnStatement()); return;
        case Tok::Break:
            advance();
            expect(Tok::Semicolon, "';'");
            out.push_back(node<T::BreakStmt>(at));
            return;
        case Tok::Continue:
            advance();
            expect(Tok::Semicolon, "';'");
            out.push_back(node<T::ContinueStmt>(at));
            return;
        case Tok::Goto: fail("'goto' is not supported");
        case Tok::Case:
        case Tok::Default: fail("'case' outside of a switch");
        case Tok::Typedef: fail("'typedef' is only allowed at file scope");
        case Tok::Try: out.push_back(tryStatement()); return;
        case Tok::Throw: out.push_back(throwStatement()); return;
        case Tok::Delete: out.push_back(deleteStatement()); return;
        case Tok::Using: usingDirective(); return;
        case Tok::Class: fail("define classes at file scope");
        case Tok::LBracket: fail("lambdas are not supported");
        default: break;
    }
    if (is(Tok::Identifier) && type(1) == Tok::Colon)
        fail("labels are not supported");
    if (startsType()) {
        Specifiers spec = specifiers();
        if (eat(Tok::Semicolon))
            return;  // a struct or enum defined inside a function
        localDeclaration(spec, out);
        return;
    }
    // An expression statement; `a = 1, b = 2;` is several.
    for (;;) {
        Typed e = assignment();
        if (e.addressOf)
            fail(at, "'&' is only allowed in a call argument");
        if (!e.funcName.empty() && !e.expr)
            fail(at, "'" + toUtf8(e.funcName) + "' is a function; function pointers are not supported");
        out.push_back(exprStatement(e.expr, at));
        if (!eat(Tok::Comma))
            break;
    }
    expect(Tok::Semicolon, "';'");
}

void Parser::localDeclaration(const Specifiers& spec, std::vector<Stmt*>& out)
{
    if (spec.isExtern)
        fail("'extern' is not allowed inside a function");
    for (;;) {
        Declarator d = declarator(spec.base);
        if (d.isFunction && !cpp())
            fail("a function cannot be declared inside a function");
        if (d.isReference && !(d.type.kind == Kind::Struct || d.type.isContainer()))
            fail(d.line, "a reference to " + describe(d.type) +
                             " must be a parameter or a range-for variable (a class or container reference may be a local)");
        if (d.isReference && !is(Tok::Assign) && !is(Tok::LParen) && !is(Tok::LBrace))
            fail(d.line, "a reference needs an initializer");
        Expr* init = nullptr;
        if (spec.isAuto) {
            // `auto x = expr;` takes the initializer's type.
            if (d.pointers || !d.arrayLengths.empty())
                fail(d.line, "'auto' cannot be combined with '*' or '[]'");
            expect(Tok::Assign, "'=' after an 'auto' variable");
            Typed value = assignment();
            if (!value.expr || value.type.kind == Kind::Void)
                fail(d.line, "'auto' needs a value");
            if (value.addressOf || !value.funcName.empty() || value.classType || value.hostClass)
                fail(d.line, "'auto' cannot deduce a type from this initializer");
            d.type = value.type.kind == Kind::Null ? CType::of(Kind::Long) : value.type;
            if (value.type.kind == Kind::Null)
                fail(d.line, "'auto' cannot deduce a type from nullptr");
            init = coerce(value, d.type);
        }
        if (d.type.kind == Kind::Void)
            fail(d.line, "a variable cannot be void");
        auto* decl = node<T::LocalDeclStmt>(d.line);
        decl->typeRef = typeRef(d.type, d.line);
        if (spec.isAuto) {
        } else if (eat(Tok::Assign)) {
            init = initializer(d.type, &d, d.line);
        } else if (cpp() && (is(Tok::LParen) || is(Tok::LBrace)) && d.arrayLengths.empty()) {
            // Direct initialization: `Point p(1, 2);`, `std::vector<int> v(n, 0);`.
            std::vector<Typed> args = arguments();
            init = construct(d.type, args, d.line, d.column).expr;
        } else {
            init = defaultValue(d.type, &d, d.line);
        }

        if (spec.isStatic) {
            // A static local is a static field with a private name.
            const String fieldName = currentFunction_ + u"$" + d.name;
            T::FieldDecl field;
            field.line = d.line;
            field.name = fieldName;
            field.modifiers = T::ModStatic | T::ModPublic;
            field.typeRef = typeRef(d.type, d.line);
            field.init = init;
            globals_->fields.push_back(std::move(field));
            Symbol s;
            s.type = d.type;
            s.isGlobal = true;
            s.globalName = fieldName;
            declare(d.name, s, d.line);
        } else {
            T::VarDeclarator var;
            var.name = d.name;
            var.line = d.line;
            var.init = init;
            decl->vars.push_back(std::move(var));
            std::int64_t constant = 0;
            decl->isConst = spec.isConst && d.type.isNumeric() && init && evalConst(init, constant);
            Symbol s;
            s.type = d.type;
            declare(d.name, s, d.line);
            out.push_back(decl);
        }
        if (!eat(Tok::Comma))
            break;
    }
    expect(Tok::Semicolon, "';'");
}

Parser::Stmt* Parser::ifStatement()
{
    auto* s = node<T::IfStmt>(line());
    advance();
    expect(Tok::LParen, "'('");
    s->condition = condition(expression());
    expect(Tok::RParen, "')'");
    s->thenBranch = embedded();
    if (eat(Tok::Else))
        s->elseBranch = embedded();
    return s;
}

Parser::Stmt* Parser::whileStatement()
{
    auto* s = node<T::WhileStmt>(line());
    advance();
    expect(Tok::LParen, "'('");
    s->condition = condition(expression());
    expect(Tok::RParen, "')'");
    s->body = embedded();
    return s;
}

Parser::Stmt* Parser::doStatement()
{
    auto* s = node<T::DoWhileStmt>(line());
    advance();
    s->body = embedded();
    expect(Tok::While, "'while'");
    expect(Tok::LParen, "'('");
    s->condition = condition(expression());
    expect(Tok::RParen, "')'");
    expect(Tok::Semicolon, "';'");
    return s;
}

Parser::Stmt* Parser::forStatement()
{
    auto* s = node<T::ForStmt>(line());
    advance();
    expect(Tok::LParen, "'('");
    pushScope();
    if (!is(Tok::Semicolon)) {
        if (startsType()) {
            Specifiers spec = specifiers();
            if (cpp()) {
                // `for (T x : sequence)` is a range-for.
                const std::size_t save = pos_;
                Declarator d = declarator(spec.base);
                if (is(Tok::Colon)) {
                    Stmt* loop = rangeFor(spec, std::move(d), s->line);
                    popScope();
                    return loop;
                }
                pos_ = save;
            }
            // The declaration consumes its own ';'.
            localDeclaration(spec, s->init);
        } else {
            do {
                const std::uint32_t at = line();
                s->init.push_back(exprStatement(assignment().expr, at));
            } while (eat(Tok::Comma));
            expect(Tok::Semicolon, "';'");
        }
    } else {
        advance();
    }
    if (!is(Tok::Semicolon))
        s->condition = condition(expression());
    expect(Tok::Semicolon, "';'");
    if (!is(Tok::RParen)) {
        do {
            s->update.push_back(assignment().expr);
        } while (eat(Tok::Comma));
    }
    expect(Tok::RParen, "')'");
    s->body = embedded();
    popScope();
    return s;
}

bool Parser::endsSection(const Stmt* s) const
{
    if (!s)
        return false;
    switch (s->kind) {
        case T::StmtKind::Break:
        case T::StmtKind::Continue:
        case T::StmtKind::Return:
        case T::StmtKind::Throw: return true;
        case T::StmtKind::Block: {
            const auto* b = static_cast<const T::BlockStmt*>(s);
            return !b->statements.empty() && endsSection(b->statements.back());
        }
        case T::StmtKind::If: {
            const auto* i = static_cast<const T::IfStmt*>(s);
            return i->elseBranch && endsSection(i->thenBranch) && endsSection(i->elseBranch);
        }
        default: return false;
    }
}

Parser::Stmt* Parser::switchStatement()
{
    auto* s = node<T::SwitchStmt>(line());
    advance();
    expect(Tok::LParen, "'('");
    Typed subject = expression();
    if (!subject.type.isIntegral())
        fail("a switch needs an integer subject");
    s->subject = subject.expr;
    expect(Tok::RParen, "')'");
    expect(Tok::LBrace, "'{'");
    pushScope();
    while (!is(Tok::RBrace)) {
        if (is(Tok::Eof))
            fail("unterminated switch");
        T::SwitchSection section;
        section.line = line();
        for (;;) {
            if (eat(Tok::Case)) {
                Typed label = conditional();
                if (!label.type.isIntegral())
                    fail("a case label must be an integer constant");
                section.labels.push_back(label.expr);
                expect(Tok::Colon, "':'");
            } else if (eat(Tok::Default)) {
                section.isDefault = true;
                expect(Tok::Colon, "':'");
            } else {
                break;
            }
        }
        if (section.labels.empty() && !section.isDefault)
            fail("expected 'case' or 'default'");
        while (!is(Tok::Case) && !is(Tok::Default) && !is(Tok::RBrace)) {
            if (is(Tok::Eof))
                fail("unterminated switch");
            statement(section.statements);
        }
        if (!section.statements.empty() && !endsSection(section.statements.back())) {
            if (is(Tok::RBrace))
                section.statements.push_back(node<T::BreakStmt>(line()));
            else
                fail(section.line, "falling through to the next case is not supported; end the case with 'break'");
        }
        s->sections.push_back(std::move(section));
    }
    advance();
    popScope();
    return s;
}

Parser::Stmt* Parser::returnStatement()
{
    auto* s = node<T::ReturnStmt>(line());
    advance();
    if (!is(Tok::Semicolon)) {
        Typed v = expression();
        if (returnType_.kind == Kind::Void)
            fail(s->line, "a void function cannot return a value");
        s->value = coerce(v, returnType_);
    } else if (returnType_.kind != Kind::Void) {
        fail(s->line, "'" + toUtf8(currentFunction_) + "' must return a value");
    }
    expect(Tok::Semicolon, "';'");
    return s;
}

// ------------------------------------------------------------- expressions ---

Parser::Typed Parser::expression()
{
    Typed e = assignment();
    if (is(Tok::Comma))
        fail("the comma operator is not supported outside a 'for' header");
    return e;
}

namespace {

bool isAssignable(const T::Expr* e) noexcept
{
    return e && (e->kind == ExprKind::Name || e->kind == ExprKind::MemberAccess ||
                 e->kind == ExprKind::ElementAccess);
}

}  // namespace

Parser::Typed Parser::assignment()
{
    NestingScope nest(nest_);
    if (nest.tooDeep())
        fail("source is nested too deeply");
    Typed left = conditional();
    BinaryOp op = BinaryOp::Add;
    bool compoundOp = true;
    switch (type()) {
        case Tok::Assign: compoundOp = false; break;
        case Tok::PlusAssign: op = BinaryOp::Add; break;
        case Tok::MinusAssign: op = BinaryOp::Subtract; break;
        case Tok::StarAssign: op = BinaryOp::Multiply; break;
        case Tok::SlashAssign: op = BinaryOp::Divide; break;
        case Tok::PercentAssign: op = BinaryOp::Modulo; break;
        case Tok::ShlAssign: op = BinaryOp::ShiftLeft; break;
        case Tok::ShrAssign: op = BinaryOp::ShiftRight; break;
        case Tok::AmpAssign: op = BinaryOp::And; break;
        case Tok::PipeAssign: op = BinaryOp::Or; break;
        case Tok::CaretAssign: op = BinaryOp::Xor; break;
        default: return left;
    }
    const std::uint32_t at = line();
    const std::uint32_t col = column();
    advance();
    if (left.isRef)
        fail(at, "assign through the pointer: '*" + toUtf8(static_cast<T::NameExpr*>(left.expr)->name) + " = ...'");
    if (!isAssignable(left.store ? left.store : left.expr))
        fail(at, "the left side of an assignment must be a variable, field or element");
    if (left.type.kind == Kind::Void)
        fail(at, "cannot assign to this expression");
    Typed right = assignment();
    if (compoundOp)
        return compound(left, op, right, at, col);
    auto* assign = node<T::AssignmentExpr>(at, col);
    assign->target = left.store ? left.store : left.expr;
    assign->value = coerce(right, left.type);
    Typed result;
    result.expr = assign;
    result.type = left.type;
    return result;
}

Parser::Typed Parser::compound(Typed target, BinaryOp op, Typed value, std::uint32_t at,
                               std::uint32_t col)
{
    if (target.store) {
        // `m[k] += v`: one helper call that reads (inserting a default),
        // combines and stores, so the map and key are evaluated once.
        auto* store = static_cast<T::ElementAccessExpr*>(target.store);
        return mapUpdate(store->target, store->index, *target.mapType, op, value, at, col);
    }
    if (cpp() && target.type.kind == Kind::Str && op == BinaryOp::Add) {
        if (value.type.kind != Kind::Str && value.type.kind != Kind::Char)
            fail(at, "a std::string can only be appended with a string or a character");
        auto* assign = node<T::AssignmentExpr>(at, col);
        assign->compound = true;
        assign->op = op;
        assign->target = target.expr;
        assign->value = value.type.kind == Kind::Char ? invoke(libcRef("__chr", at), {value.expr}, at) : value.expr;
        Typed result;
        result.expr = assign;
        result.type = target.type;
        return result;
    }
    if (!target.type.isNumeric())
        fail(at, "compound assignment needs a numeric left side");
    if (value.type.kind == Kind::Bool)
        value.expr = coerce(value, CType::of(Kind::Int)), value.type = CType::of(Kind::Int);
    if (!value.type.isNumeric())
        fail(at, "compound assignment needs a numeric right side");
    const bool shift = op == BinaryOp::ShiftLeft || op == BinaryOp::ShiftRight;
    const CType promoted = shift ? promote(target.type, CType::of(Kind::Int)) : promote(target.type, value.type);
    const bool literalInt = value.expr->kind == ExprKind::Literal &&
                            static_cast<T::LiteralExpr*>(value.expr)->literal == LiteralKind::Int &&
                            target.type.isIntegral();
    Typed result;
    result.type = target.type;
    if (promoted.same(target.type) || widens(promoted, target.type) || literalInt) {
        auto* assign = node<T::AssignmentExpr>(at, col);
        assign->compound = true;
        assign->op = op;
        assign->target = target.expr;
        assign->value = shift ? coerce(value, CType::of(Kind::Int)) : value.expr;
        result.expr = assign;
        return result;
    }
    // x += 2.5 on an int: C truncates, so spell it as x = (int)(x + 2.5).
    Expr* copy = cloneSimple(target.expr);
    if (!copy)
        fail(at, "this compound assignment narrows; write it as 'x = (type)(x op y)'");
    Typed left;
    left.expr = copy;
    left.type = target.type;
    Typed sum = makeBinary(op, left, value, at, col);
    auto* assign = node<T::AssignmentExpr>(at, col);
    assign->target = target.expr;
    assign->value = coerce(sum, target.type);
    result.expr = assign;
    return result;
}

Parser::Typed Parser::conditional()
{
    Typed test = binary(1);
    if (!is(Tok::Question))
        return test;
    const std::uint32_t at = line();
    const std::uint32_t col = column();
    advance();
    Typed whenTrue = assignment();
    expect(Tok::Colon, "':'");
    Typed whenFalse = conditional();
    auto* c = node<T::ConditionalExpr>(at, col);
    c->condition = condition(test);
    Typed result;
    if (cpp() && whenTrue.type.kind == Kind::Bool && whenFalse.type.kind == Kind::Bool) {
        result.type = CType::of(Kind::Bool);
        c->whenTrue = whenTrue.expr;
        c->whenFalse = whenFalse.expr;
    } else if (whenTrue.type.isNumeric() || whenTrue.type.kind == Kind::Bool) {
        if (!whenFalse.type.isNumeric() && whenFalse.type.kind != Kind::Bool)
            fail(at, "the branches of '?:' have different types");
        CType a = whenTrue.type.kind == Kind::Bool ? CType::of(Kind::Int) : whenTrue.type;
        CType b = whenFalse.type.kind == Kind::Bool ? CType::of(Kind::Int) : whenFalse.type;
        result.type = promote(a, b);
        c->whenTrue = coerce(whenTrue, result.type);
        c->whenFalse = coerce(whenFalse, result.type);
    } else if (whenTrue.type.isReference() && whenFalse.type.isReference()) {
        result.type = whenTrue.type.kind == Kind::Null ? whenFalse.type : whenTrue.type;
        c->whenTrue = whenTrue.expr;
        c->whenFalse = whenFalse.expr;
    } else {
        fail(at, "the branches of '?:' have different types");
    }
    result.expr = c;
    return result;
}

namespace {

int precedenceOf(Tok t, BinaryOp& op) noexcept
{
    switch (t) {
        case Tok::OrOr: op = BinaryOp::LogicalOr; return 1;
        case Tok::AndAnd: op = BinaryOp::LogicalAnd; return 2;
        case Tok::Pipe: op = BinaryOp::Or; return 3;
        case Tok::Caret: op = BinaryOp::Xor; return 4;
        case Tok::Amp: op = BinaryOp::And; return 5;
        case Tok::EqEq: op = BinaryOp::Equal; return 6;
        case Tok::NotEq: op = BinaryOp::NotEqual; return 6;
        case Tok::Lt: op = BinaryOp::Less; return 7;
        case Tok::Gt: op = BinaryOp::Greater; return 7;
        case Tok::Le: op = BinaryOp::LessEqual; return 7;
        case Tok::Ge: op = BinaryOp::GreaterEqual; return 7;
        case Tok::Shl: op = BinaryOp::ShiftLeft; return 8;
        case Tok::Shr: op = BinaryOp::ShiftRight; return 8;
        case Tok::Plus: op = BinaryOp::Add; return 9;
        case Tok::Minus: op = BinaryOp::Subtract; return 9;
        case Tok::Star: op = BinaryOp::Multiply; return 10;
        case Tok::Slash: op = BinaryOp::Divide; return 10;
        case Tok::Percent: op = BinaryOp::Modulo; return 10;
        default: return 0;
    }
}

}  // namespace

Parser::Typed Parser::binary(int minPrecedence)
{
    Typed left = unary();
    std::size_t links = 0;
    for (;;) {
        if (++links > kMaxChainLength)
            fail("expression is too long; split it into parts");
        BinaryOp op = BinaryOp::Add;
        const int prec = precedenceOf(type(), op);
        if (prec == 0 || prec < minPrecedence)
            return left;
        const std::uint32_t at = line();
        const std::uint32_t col = column();
        advance();
        Typed right = binary(prec + 1);
        left = makeBinary(op, std::move(left), std::move(right), at, col);
    }
}

Parser::Typed Parser::makeBinary(BinaryOp op, Typed left, Typed right, std::uint32_t at,
                                 std::uint32_t col)
{
    if (left.stream && op == BinaryOp::ShiftLeft)
        return streamOutput(std::move(left), std::move(right), at, col);
    if (left.addressOf || right.addressOf)
        fail(at, "'&' is only allowed in a call argument");
    if (!left.expr || !right.expr)
        fail(at, "a void value cannot be used in an expression");
    if (cpp() && (left.type.kind == Kind::Str || right.type.kind == Kind::Str)) {
        // std::string: `+` concatenates, the comparisons compare by value.
        auto text = [](const CType& t) { return t.kind == Kind::Str || t.kind == Kind::Char; };
        if (op == BinaryOp::Add) {
            if (!text(left.type) || !text(right.type))
                fail(at, "a std::string can only be added to a string or a character");
            Typed result;
            auto* b = node<T::BinaryExpr>(at, col);
            b->op = op;
            // A char joins as its character, not its code.
            b->left = left.type.kind == Kind::Char ? invoke(libcRef("__chr", at), {left.expr}, at) : left.expr;
            b->right = right.type.kind == Kind::Char ? invoke(libcRef("__chr", at), {right.expr}, at) : right.expr;
            result.expr = b;
            result.type = CType::of(Kind::Str);
            return result;
        }
        if (op == BinaryOp::Less || op == BinaryOp::LessEqual || op == BinaryOp::Greater ||
            op == BinaryOp::GreaterEqual)
            return stringCompare(op, std::move(left), std::move(right), at, col);
    }
    Typed result;
    auto* b = node<T::BinaryExpr>(at, col);
    b->op = op;
    switch (op) {
        case BinaryOp::LogicalAnd:
        case BinaryOp::LogicalOr:
            b->left = condition(left);
            b->right = condition(right);
            result.type = CType::of(Kind::Bool);
            break;
        case BinaryOp::Equal:
        case BinaryOp::NotEqual:
            if (left.type.isReference() || right.type.isReference()) {
                if (isZero(left))
                    left.expr = nullLiteral(at), left.type = CType::of(Kind::Null);
                if (isZero(right))
                    right.expr = nullLiteral(at), right.type = CType::of(Kind::Null);
                if (!left.type.isReference() || !right.type.isReference())
                    fail(at, "comparison between a pointer and a number");
                b->left = left.expr;
                b->right = right.expr;
                result.type = CType::of(Kind::Bool);
                break;
            }
            [[fallthrough]];
        case BinaryOp::Less:
        case BinaryOp::LessEqual:
        case BinaryOp::Greater:
        case BinaryOp::GreaterEqual: {
            if (left.type.kind == Kind::Bool)
                left.expr = coerce(left, CType::of(Kind::Int)), left.type = CType::of(Kind::Int);
            if (right.type.kind == Kind::Bool)
                right.expr = coerce(right, CType::of(Kind::Int)), right.type = CType::of(Kind::Int);
            if (!left.type.isNumeric() || !right.type.isNumeric())
                fail(at, "invalid operands to comparison: " + describe(left.type) + " and " + describe(right.type));
            b->left = left.expr;
            b->right = right.expr;
            result.type = CType::of(Kind::Bool);
            break;
        }
        default: {
            if (left.type.kind == Kind::Bool)
                left.expr = coerce(left, CType::of(Kind::Int)), left.type = CType::of(Kind::Int);
            if (right.type.kind == Kind::Bool)
                right.expr = coerce(right, CType::of(Kind::Int)), right.type = CType::of(Kind::Int);
            if (!left.type.isNumeric() || !right.type.isNumeric()) {
                if (left.type.isReference() || right.type.isReference())
                    fail(at, "pointer arithmetic is not supported");
                fail(at, "invalid operands: " + describe(left.type) + " and " + describe(right.type));
            }
            const bool shift = op == BinaryOp::ShiftLeft || op == BinaryOp::ShiftRight;
            if (shift) {
                if (left.type.isFloating() || right.type.isFloating())
                    fail(at, "shift of a floating-point value");
                result.type = promote(left.type, CType::of(Kind::Int));
                b->left = left.expr;
                b->right = coerce(right, CType::of(Kind::Int));
            } else {
                if ((op == BinaryOp::And || op == BinaryOp::Or || op == BinaryOp::Xor ||
                     op == BinaryOp::Modulo) &&
                    (left.type.isFloating() || right.type.isFloating()) && op != BinaryOp::Modulo)
                    fail(at, "bitwise operator on a floating-point value");
                result.type = promote(left.type, right.type);
                b->left = left.expr;
                b->right = right.expr;
            }
            // n * sizeof(T) remembers T for malloc.
            if (op == BinaryOp::Multiply) {
                if (left.sizeofType && !left.countExpr && !right.sizeofType) {
                    result.sizeofType = left.sizeofType;
                    result.countExpr = right.expr;
                } else if (right.sizeofType && !right.countExpr && !left.sizeofType) {
                    result.sizeofType = right.sizeofType;
                    result.countExpr = left.expr;
                }
            }
            break;
        }
    }
    result.expr = b;
    return result;
}

Parser::Typed Parser::unary()
{
    NestingScope nest(nest_);
    if (nest.tooDeep())
        fail("source is nested too deeply");
    const std::uint32_t at = line();
    const std::uint32_t col = column();
    switch (type()) {
        case Tok::Minus:
        case Tok::Plus:
        case Tok::Tilde: {
            const Tok t = advance().type;
            Typed operand = unary();
            if (operand.type.kind == Kind::Bool)
                operand.expr = coerce(operand, CType::of(Kind::Int)), operand.type = CType::of(Kind::Int);
            if (!operand.type.isNumeric())
                fail(at, "invalid operand to unary operator");
            if (t == Tok::Tilde && operand.type.isFloating())
                fail(at, "'~' on a floating-point value");
            // Fold a negative literal so `-2147483648` and `case -1:` stay constants.
            if (t == Tok::Minus && operand.expr->kind == ExprKind::Literal) {
                auto* lit = static_cast<T::LiteralExpr*>(operand.expr);
                if (lit->literal == LiteralKind::Int || lit->literal == LiteralKind::Long) {
                    lit->intValue = -lit->intValue;
                    return operand;
                }
                if (lit->literal == LiteralKind::Double || lit->literal == LiteralKind::Float) {
                    lit->realValue = -lit->realValue;
                    return operand;
                }
            }
            auto* u = node<T::UnaryExpr>(at, col);
            u->op = t == Tok::Minus ? UnaryOp::Minus : t == Tok::Plus ? UnaryOp::Plus : UnaryOp::Complement;
            u->operand = operand.expr;
            Typed result;
            result.expr = u;
            result.type = promote(operand.type, CType::of(Kind::Int));
            return result;
        }
        case Tok::Bang: {
            advance();
            Typed operand = unary();
            Typed result;
            result.expr = logicalNot(operand, at, col);
            result.type = CType::of(Kind::Bool);
            return result;
        }
        case Tok::PlusPlus:
        case Tok::MinusMinus: {
            const Tok t = advance().type;
            return increment(unary(), t == Tok::PlusPlus, /*prefix=*/true, at, col);
        }
        case Tok::Amp: {
            advance();
            Typed operand = unary();
            if (operand.isRef) {
                // `&*p` -- the pointer itself.
                operand.isRef = true;
                return operand;
            }
            if (operand.type.kind == Kind::Struct && operand.expr)
                return operand;  // a struct is already a reference
            if (!isAssignable(operand.expr))
                fail(at, "'&' needs a variable, field or element");
            Typed result;
            result.addressOf = operand.expr;
            result.addressType = operand.type;
            return result;
        }
        case Tok::Star: {
            advance();
            Typed operand = unary();
            if (operand.isRef) {
                operand.isRef = false;
                return operand;
            }
            Typed result;
            switch (operand.type.kind) {
                case Kind::Array:
                case Kind::Str: {
                    auto* e = node<T::ElementAccessExpr>(at, col);
                    e->target = operand.expr;
                    e->index = intLiteral(0, at);
                    result.expr = e;
                    result.type = operand.type.kind == Kind::Str ? CType::of(Kind::Char) : *operand.type.elem;
                    return result;
                }
                case Kind::Struct: return operand;  // *p on a struct pointer is the struct
                default: fail(at, "'*' needs a pointer operand");
            }
        }
        case Tok::Sizeof: return sizeofExpr();
        case Tok::New: return postfix(newExpression());
        case Tok::StaticCast:
        case Tok::DynamicCast:
        case Tok::ConstCast:
        case Tok::ReinterpretCast: return postfix(castExpression(advance().type, at, col));
        case Tok::LBracket: fail("lambdas are not supported");
        case Tok::LParen: {
            // A cast, when a type follows the parenthesis.
            const std::size_t save = pos_;
            advance();
            if (startsType()) {
                Specifiers spec = specifiers();
                Declarator d = declarator(spec.base, /*allowAbstract=*/true);
                if (!d.name.empty())
                    fail(at, "unexpected name in a cast");
                expect(Tok::RParen, "')'");
                CType target = d.type;
                if (is(Tok::LBrace)) {
                    // A compound literal: (Point){1, 2}.
                    Typed literal;
                    literal.type = target;
                    literal.expr = initializer(target, &d, at);
                    return postfix(std::move(literal));
                }
                Typed operand = unary();
                if (operand.addressOf)
                    fail(at, "'&' is only allowed in a call argument");
                Typed result;
                result.type = target;
                if (target.kind == Kind::Void) {
                    result.expr = operand.expr;  // (void)f(); discards the value
                    return result;
                }
                if (target.isReference()) {
                    if (operand.type.kind == Kind::Null || operand.type.same(target)) {
                        result.expr = operand.expr;
                        return result;
                    }
                    fail(at, "cannot cast " + describe(operand.type) + " to " + describe(target));
                }
                if (operand.type.kind == Kind::Bool)
                    operand.expr = coerce(operand, CType::of(Kind::Int)), operand.type = CType::of(Kind::Int);
                if (!operand.type.isNumeric())
                    fail(at, "cannot cast " + describe(operand.type) + " to " + describe(target));
                result.expr = operand.type.same(target) ? operand.expr : castTo(operand.expr, target, at);
                return result;
            }
            pos_ = save;
            break;
        }
        default: break;
    }
    return postfix(primary());
}

Parser::Expr* Parser::logicalNot(Typed value, std::uint32_t at, std::uint32_t col)
{
    if (value.addressOf || !value.expr)
        fail(at, "invalid operand to '!'");
    if (value.type.kind == Kind::Bool) {
        auto* u = node<T::UnaryExpr>(at, col);
        u->op = UnaryOp::Not;
        u->operand = value.expr;
        return u;
    }
    auto* b = node<T::BinaryExpr>(at, col);
    b->op = BinaryOp::Equal;
    b->left = value.expr;
    if (value.type.isReference())
        b->right = nullLiteral(at);
    else if (value.type.isNumeric())
        b->right = intLiteral(0, at);
    else
        fail(at, "invalid operand to '!'");
    return b;
}

Parser::Typed Parser::sizeofExpr()
{
    const std::uint32_t at = line();
    expect(Tok::Sizeof, "'sizeof'");
    Typed result;
    result.type = CType::of(Kind::Int);
    if (is(Tok::LParen)) {
        const std::size_t save = pos_;
        advance();
        if (startsType()) {
            Specifiers spec = specifiers();
            Declarator d = declarator(spec.base, /*allowAbstract=*/true);
            if (!d.name.empty())
                fail(at, "unexpected name in sizeof");
            expect(Tok::RParen, "')'");
            result.expr = intLiteral(sizeOf(d.type), at);
            result.sizeofType = std::make_shared<CType>(d.type);
            return result;
        }
        pos_ = save;
    }
    Typed operand = unary();
    if (operand.addressOf || !operand.expr)
        fail(at, "invalid operand to sizeof");
    if (operand.type.kind == Kind::Array) {
        // sizeof(arr) / sizeof(arr[0]) works out to arr.Length.
        auto* length = node<T::MemberAccessExpr>(at);
        length->target = operand.expr;
        length->name = ascii("Length");
        auto* product = node<T::BinaryExpr>(at);
        product->op = BinaryOp::Multiply;
        product->left = length;
        product->right = intLiteral(sizeOf(*operand.type.elem), at);
        result.expr = product;
        result.sizeofType = std::make_shared<CType>(*operand.type.elem);
        result.countExpr = cloneSimple(length);
        return result;
    }
    result.expr = intLiteral(sizeOf(operand.type), at);
    result.sizeofType = std::make_shared<CType>(operand.type);
    return result;
}

int Parser::sizeOf(const CType& type) const
{
    switch (type.kind) {
        case Kind::Void: fail("sizeof(void) is not allowed");
        case Kind::Bool:
        case Kind::Char:
        case Kind::UChar: return 1;
        case Kind::Short:
        case Kind::UShort: return 2;
        case Kind::Int:
        case Kind::UInt:
        case Kind::Float: return 4;
        case Kind::Long:
        case Kind::ULong:
        case Kind::Double:
        case Kind::Str:
        case Kind::Array:
        case Kind::Vector:
        case Kind::Map:
        case Kind::Pair:
        case Kind::Null: return 8;
        case Kind::Struct: {
            auto info = structs_.find(type.structName);
            if (info == structs_.end())
                return 8;
            // A struct-typed field is a reference here, pointer or not, so it
            // counts as one; that also keeps `struct node { struct node* next; }` finite.
            int total = 0;
            for (const Field& field : info->second.fields)
                total += field.type.kind == Kind::Struct ? 8 : sizeOf(field.type);
            return total == 0 ? 1 : total;
        }
    }
    return 8;
}

Parser::Typed Parser::postfix(Typed base)
{
    std::size_t links = 0;
    for (;;) {
        if (++links > kMaxChainLength)
            fail("expression is too long; split it into parts");
        const std::uint32_t at = line();
        const std::uint32_t col = column();
        switch (type()) {
            case Tok::LBracket: base = index(std::move(base), at, col); break;
            case Tok::LParen: {
                if (base.hostClass && !base.expr) {
                    base = hostConstruct(*base.hostClass, at, col);
                    break;
                }
                if (base.classType && !base.expr) {
                    std::vector<Typed> args = arguments();
                    base = construct(base.type, args, at, col);
                    break;
                }
                if (base.funcName.empty())
                    fail(at, "calling something that is not a function; function pointers are not supported");
                const String name = base.funcName;
                base = call(std::move(base), name, at, col);
                break;
            }
            case Tok::LBrace:
                if (base.classType && !base.expr) {
                    std::vector<Typed> args = arguments();
                    base = construct(base.type, args, at, col);
                    break;
                }
                return base;
            case Tok::ColonColon:
                if (base.hostClass && !base.expr) {
                    base = hostMember(std::move(base), *base.hostClass, at, col);
                    break;
                }
                if (base.classType && !base.expr) {
                    base = staticMember(*base.classType, at, col);
                    break;
                }
                fail(at, "'::' needs a class or namespace on its left");
            case Tok::Dot:
            case Tok::Arrow: base = member(std::move(base), at, col); break;
            case Tok::PlusPlus:
            case Tok::MinusMinus: {
                const Tok t = advance().type;
                base = increment(std::move(base), t == Tok::PlusPlus, /*prefix=*/false, at, col);
                break;
            }
            default:
                if (!base.funcName.empty() && !base.expr)
                    fail(at, "'" + toUtf8(base.funcName) + "' is a function; function pointers are not supported");
                if (base.hostClass && !base.expr)
                    fail(at, "'" + toUtf8(base.hostClass->name) + "' is a class; call it or name a member");
                if (base.classType && !base.expr)
                    fail(at, "'" + toUtf8(base.classType->name) + "' is a class; construct it or name a static member");
                if (base.stream && !base.expr && !is(Tok::Shl))
                    fail(at, "std::cout is only used as 'std::cout << value'");
                return base;
        }
    }
}

/// `++x`, `x--` and friends; `m[k]++` on a map goes through the inserting
/// read and yields the new value.
Parser::Typed Parser::increment(Typed target, bool positive, bool prefix, std::uint32_t at,
                                std::uint32_t col)
{
    if (target.store) {
        Typed one;
        one.expr = intLiteral(1, at);
        one.type = CType::of(Kind::Int);
        return compound(std::move(target), positive ? BinaryOp::Add : BinaryOp::Subtract, one, at, col);
    }
    if (target.type.isReference())
        fail(at, "pointer arithmetic is not supported");
    if (target.isRef || !isAssignable(target.expr) || !target.type.isNumeric())
        fail(at, "'++' and '--' need a numeric variable");
    auto* u = node<T::UnaryExpr>(at, col);
    u->op = prefix ? (positive ? UnaryOp::PreIncrement : UnaryOp::PreDecrement)
                   : (positive ? UnaryOp::PostIncrement : UnaryOp::PostDecrement);
    u->operand = target.expr;
    target.expr = u;
    return target;
}

Parser::Typed Parser::index(Typed base, std::uint32_t at, std::uint32_t col)
{
    expect(Tok::LBracket, "'['");
    Typed i = expression();
    expect(Tok::RBracket, "']'");
    if (base.isRef)
        fail(at, "'" + toUtf8(static_cast<T::NameExpr*>(base.expr)->name) + "' is passed by reference and cannot be indexed");
    Typed result;
    if (base.type.kind == Kind::Vector) {
        auto* e = node<T::ElementAccessExpr>(at, col);
        e->target = base.expr;
        e->index = coerce(i, CType::of(Kind::Int));
        result.expr = e;
        result.type = *base.type.elem;
        return result;
    }
    if (base.type.kind == Kind::Map) {
        // A read inserts a default first, as `operator[]` does; an assignment
        // goes through the indexer. Only one of the two forms ends up in the
        // tree, so they may share the map and key nodes.
        Typed key;
        key.expr = coerce(i, *base.type.elem);
        key.type = *base.type.elem;
        auto* e = node<T::ElementAccessExpr>(at, col);
        e->target = base.expr;
        e->index = key.expr;
        result.store = e;
        result.mapType = std::make_shared<CType>(base.type);
        result.expr = mapGet(base, key, at, col);
        result.type = *base.type.elem2;
        return result;
    }
    if (base.type.kind == Kind::Str) {
        // s[i] on a string reads NUL one past the end, as C strings do, so
        // `while (s[i])` terminates.
        auto* invoke = node<T::InvocationExpr>(at, col);
        invoke->callee = libcRef("__at", at);
        T::Argument s;
        s.value = base.expr;
        T::Argument idx;
        idx.value = coerce(i, CType::of(Kind::Int));
        invoke->args.push_back(s);
        invoke->args.push_back(idx);
        result.expr = invoke;
        result.type = CType::of(Kind::Char);
        return result;
    }
    if (base.type.kind != Kind::Array)
        fail(at, "subscript on a value of type " + describe(base.type));
    auto* e = node<T::ElementAccessExpr>(at, col);
    e->target = base.expr;
    e->index = coerce(i, CType::of(Kind::Int));
    result.expr = e;
    result.type = *base.type.elem;
    return result;
}

Parser::Typed Parser::member(Typed base, std::uint32_t at, std::uint32_t col)
{
    if (base.hostClass)
        return hostMember(std::move(base), *base.hostClass, at, col);
    if (base.type.kind == Kind::Struct && !structs_.count(base.type.structName)) {
        if (const HostClassSpec* spec = hostClassNamed(base.type.structName))
            return hostMember(std::move(base), *spec, at, col);
    }
    if (base.addressOf || (!base.expr && !base.isRef))
        fail(at, "a member was requested on something that has no value");
    advance();  // . or ->
    const String name = expectName("a field name");
    if (cpp()) {
        switch (base.type.kind) {
            case Kind::Str: return stringMember(std::move(base), name, at, col);
            case Kind::Vector: return vectorMember(std::move(base), name, at, col);
            case Kind::Map: return mapMember(std::move(base), name, at, col);
            case Kind::Pair: return pairMember(std::move(base), name, at, col);
            case Kind::Struct:
                if (base.type.structName.substr(0, 7) == u"System.") {
                    // An exception: `e.what()`.
                    if (name != u"what")
                        fail(at, "an exception has only 'what()'");
                    expect(Tok::LParen, "'('");
                    expect(Tok::RParen, "')'");
                    Typed result;
                    result.expr = memberRef(base.expr, "Message", at);
                    result.type = CType::of(Kind::Str);
                    return result;
                }
                if (auto info = structs_.find(base.type.structName); info != structs_.end()) {
                    pos_ -= 1;  // classMember reads the name itself
                    return classMember(std::move(base), info->second, at, col);
                }
                break;
            default: break;
        }
    }
    if (base.type.kind != Kind::Struct)
        fail(at, "'" + toUtf8(name) + "' requested on a value of type " + describe(base.type) + ", not a struct");
    auto info = structs_.find(base.type.structName);
    if (info == structs_.end())
        fail(at, "unknown struct '" + toUtf8(base.type.structName) + "'");
    Typed result;
    for (const Field& field : info->second.fields) {
        if (field.name == name) {
            auto* m = node<T::MemberAccessExpr>(at, col);
            m->target = base.expr;
            m->name = name;
            result.expr = m;
            result.type = field.type;
            return result;
        }
    }
    fail(at, "struct '" + toUtf8(base.type.structName) + "' has no field '" + toUtf8(name) + "'");
}

Parser::Typed Parser::primary()
{
    const std::uint32_t at = line();
    const std::uint32_t col = column();
    Typed result;
    switch (type()) {
        case Tok::IntLit: {
            const Token& t = advance();
            auto* lit = node<T::LiteralExpr>(at, col);
            lit->intValue = t.intValue;
            if (t.isUnsigned && t.isLong) {
                lit->literal = LiteralKind::ULong;
                result.type = CType::of(Kind::ULong);
            } else if (t.isLong) {
                lit->literal = LiteralKind::Long;
                result.type = CType::of(Kind::Long);
            } else if (t.isUnsigned) {
                lit->literal = LiteralKind::UInt;
                result.type = CType::of(Kind::UInt);
            } else {
                lit->literal = LiteralKind::Int;
                result.type = CType::of(Kind::Int);
            }
            result.expr = lit;
            return result;
        }
        case Tok::FloatLit: {
            const Token& t = advance();
            auto* lit = node<T::LiteralExpr>(at, col);
            lit->realValue = t.realValue;
            lit->literal = t.isFloat ? LiteralKind::Float : LiteralKind::Double;
            result.type = CType::of(t.isFloat ? Kind::Float : Kind::Double);
            result.expr = lit;
            return result;
        }
        case Tok::CharLit: {
            const Token& t = advance();
            auto* lit = node<T::LiteralExpr>(at, col);
            lit->literal = LiteralKind::Char;
            lit->intValue = t.intValue;
            result.type = CType::of(Kind::Char);
            result.expr = lit;
            return result;
        }
        case Tok::StringLit: {
            const Token& t = advance();
            auto* lit = node<T::LiteralExpr>(at, col);
            lit->literal = LiteralKind::String;
            lit->text = t.text;
            result.type = CType::of(Kind::Str);
            result.expr = lit;
            return result;
        }
        case Tok::True:
        case Tok::False:
            if (cpp()) {
                result.expr = boolLiteral(advance().type == Tok::True, at);
                result.type = CType::of(Kind::Bool);
                return result;
            }
            result.expr = intLiteral(advance().type == Tok::True ? 1 : 0, at);
            result.type = CType::of(Kind::Int);
            return result;
        case Tok::Nullptr:
            advance();
            result.expr = nullLiteral(at);
            result.type = CType::of(Kind::Null);
            return result;
        case Tok::This:
            advance();
            if (!currentClass_ || currentStatic_)
                fail(at, "'this' is only available inside a non-static member function");
            result.expr = node<T::ThisExpr>(at, col);
            result.type = CType::structOf(currentClass_->name);
            return result;
        case Tok::LParen: {
            advance();
            Typed inner = expression();
            expect(Tok::RParen, "')'");
            return inner;
        }
        case Tok::Identifier: break;
        default:
            if (cpp() && startsType() && type(1) == Tok::LParen) {
                // `int(x)`, `double(n)`: a functional cast.
                Specifiers spec = specifiers();
                return functionalCast(spec.base, at, col);
            }
            fail(std::string("unexpected ") + tokenName(type()));
    }

    if (cpp()) {
        skipQualifier();
        if (startsType() && (type(1) == Tok::LParen || type(1) == Tok::Lt || type(1) == Tok::LBrace) &&
            !structs_.count(peek().text) && !hostClassNamed(peek().text)) {
            // `std::string(...)`, `std::vector<int>(n)`, `Point(1, 2)` is below.
            Specifiers spec = specifiers();
            return functionalCast(spec.base, at, col);
        }
        // `Color::Red`, `Point::count`, `Base::method`
        if (is(Tok::Identifier) && type(1) == Tok::ColonColon && type(2) == Tok::Identifier) {
            auto scoped = constants_.find(peek().text + u"::" + peek(2).text);
            if (scoped != constants_.end()) {
                advance();
                advance();
                advance();
                result.expr = intLiteral(scoped->second, at);
                result.type = CType::of(Kind::Int);
                return result;
            }
            if (peek().text == u"string" && peek(2).text == u"npos") {
                advance();
                advance();
                advance();
                result.expr = intLiteral(-1, at);
                result.type = CType::of(Kind::Int);
                return result;
            }
        }
    }

    const String name = advance().text;
    if (name == u"NULL") {
        result.expr = nullLiteral(at);
        result.type = CType::of(Kind::Null);
        return result;
    }
    auto constant = constants_.find(name);
    if (constant != constants_.end()) {
        result.expr = intLiteral(constant->second, at);
        result.type = CType::of(Kind::Int);
        return result;
    }
    if (const Symbol* symbol = lookup(name)) {
        result.expr = symbol->isGlobal ? globalRef(symbol->globalName, at, col) : nameRef(name, at, col);
        result.type = symbol->type;
        result.isRef = symbol->isRef;
        result.pairKey = symbol->pairKey;
        result.pairMap = symbol->pairMap;
        return result;
    }
    if (currentClass_) {
        // A member of the class whose method this is, named bare.
        const ClassInfo* owner = nullptr;
        if (const Field* field = findField(*currentClass_, name, &owner)) {
            if (!field->isStatic && currentStatic_)
                fail(at, "'" + toUtf8(name) + "' is an instance member; a static member function has no 'this'");
            result.expr = field->isStatic ? memberRef(nameRef(owner->name, at, col), toUtf8(name).c_str(), at)
                                          : nameRef(name, at, col);
            result.type = field->type;
            return result;
        }
        if (findMethods(*currentClass_, name)) {
            result.funcName = name;
            result.methodOwner = currentClass_;
            return result;
        }
    }
    if (functions_.count(name)) {
        result.funcName = name;
        return result;
    }
    if (const HostClassSpec* spec = hostClassNamed(name)) {
        result.hostClass = spec;
        result.type = CType::structOf(name);
        return result;
    }
    if (cpp()) {
        if (auto info = structs_.find(name); info != structs_.end()) {
            result.classType = &info->second;
            result.type = CType::structOf(name);
            return result;
        }
        if (name == u"cout" || name == u"cerr" || name == u"clog") {
            result.stream = name == u"cout" ? 1 : 2;
            return result;
        }
        if (name == u"endl") {
            result.expr = stringLiteral(u"\n", at);
            result.type = CType::of(Kind::Str);
            return result;
        }
        if (name == u"flush") {
            result.expr = stringLiteral(String(), at);
            result.type = CType::of(Kind::Str);
            return result;
        }
        if (name == u"cin")
            fail(at, "std::cin is not supported: there is no input stream");
        bool handled = false;
        Typed std = stdFunction(name, at, col, handled);
        if (handled)
            return std;
    }
    const std::string ascii8 = toUtf8(name);
    for (const LibcFunction* f = libcFunctions(); f->name; ++f) {
        if (ascii8 == f->name) {
            result.funcName = name;
            return result;
        }
    }
    if (ascii8 == "malloc" || ascii8 == "calloc" || ascii8 == "free" || ascii8 == "assert" ||
        ascii8 == "exit") {
        result.funcName = name;
        return result;
    }
    for (const LibcConstant* k = libcConstants(); k->name; ++k) {
        if (ascii8 != k->name)
            continue;
        if (k->kind == 'd') {
            auto* lit = node<T::LiteralExpr>(at, col);
            lit->literal = LiteralKind::Double;
            lit->realValue = k->realValue;
            result.expr = lit;
            result.type = CType::of(Kind::Double);
        } else if (k->kind == 'l') {
            auto* lit = node<T::LiteralExpr>(at, col);
            lit->literal = LiteralKind::Long;
            lit->intValue = k->intValue;
            result.expr = lit;
            result.type = CType::of(Kind::Long);
        } else {
            result.expr = intLiteral(k->intValue, at);
            result.type = CType::of(Kind::Int);
        }
        return result;
    }
    fail(at, "use of undeclared identifier '" + ascii8 + "'");
}

/// `f(args)`. `malloc`, `calloc`, `free`, `assert` and `exit` are rewritten;
/// a function of this program is called through the synthesized class; the
/// rest is `libc`.
/// `(a, b, c)` or `{a, b, c}`: the arguments of a call or a construction.
std::vector<Parser::Typed> Parser::arguments()
{
    const Tok close = is(Tok::LBrace) ? Tok::RBrace : Tok::RParen;
    if (close == Tok::RBrace)
        advance();
    else
        expect(Tok::LParen, "'('");
    std::vector<Typed> args;
    const bool braced = close == Tok::RBrace;
    while (!is(close)) {
        if (is(Tok::LBrace)) {
            // A nested brace list: a map entry `{key, value}`, an element of a
            // vector of classes, or a container passed to a parameter. It has
            // no type of its own until something gives it one.
            const std::uint32_t at = line();
            std::vector<Typed> entry = arguments();
            Typed list;
            list.type = CType::of(Kind::Void);
            list.braceList = std::make_shared<std::vector<Typed>>(std::move(entry));
            list.expr = nullptr;
            list.type.kind = Kind::Null;
            args.push_back(std::move(list));
            (void)at;
        } else {
            args.push_back(assignment());
        }
        if (!is(close))
            expect(Tok::Comma, "','");
    }
    expect(close, close == Tok::RBrace ? "'}'" : "')'");
    lastArgsBraced_ = braced;
    return args;
}

Parser::Typed Parser::call(Typed callee, const String& name, std::uint32_t at, std::uint32_t col)
{
    if (callee.methodOwner)
        return methodCall(nullptr, *callee.methodOwner, name, at, col);
    std::vector<Typed> args = arguments();
    const std::string ascii8 = toUtf8(name);

    auto user = functions_.find(name);
    if (user == functions_.end()) {
        if (ascii8 == "malloc" || ascii8 == "calloc") {
            const bool isCalloc = ascii8 == "calloc";
            if (args.size() != (isCalloc ? 2u : 1u))
                fail(at, ascii8 + " takes " + (isCalloc ? "two arguments" : "one argument"));
            const Typed& size = args[isCalloc ? 1 : 0];
            if (!size.sizeofType)
                fail(at, ascii8 + " is only supported as " + ascii8 + "(" +
                             (isCalloc ? "n, sizeof(T))" : "sizeof(T)) or malloc(n * sizeof(T))"));
            const CType element = *size.sizeofType;
            Expr* count = isCalloc ? args[0].expr : size.countExpr;
            Typed result;
            if (!count && element.kind == Kind::Struct) {
                auto* create = node<T::ObjectCreationExpr>(at, col);
                create->typeRef = typeRef(element, at);
                result.expr = create;
                result.type = element;
                return result;
            }
            if (!count)
                count = intLiteral(1, at);
            if (isCalloc)
                count = coerce(args[0], CType::of(Kind::Int));
            if (element.kind == Kind::Char) {
                // A char buffer: malloc(n) or malloc(n * sizeof(char)) is a string in C
                // terms, and there is no mutable string; give a char array.
                result.type = CType::arrayOf(element);
            } else {
                result.type = CType::arrayOf(element);
            }
            result.expr = arrayInitializer(result.type, count, at);
            return result;
        }
        if (ascii8 == "free") {
            if (args.size() != 1)
                fail(at, "free takes one argument");
            Typed result;
            result.type = CType::of(Kind::Void);
            return result;  // nothing to do: the collector owns the memory
        }
        if (ascii8 == "exit")
            fail(at, "exit() is not supported; return from main instead");
        if (ascii8 == "assert") {
            if (args.size() != 1)
                fail(at, "assert takes one argument");
            auto* invoke = node<T::InvocationExpr>(at, col);
            invoke->callee = libcRef("__assert", at);
            T::Argument a;
            a.value = condition(args[0]);
            invoke->args.push_back(a);
            T::Argument l;
            l.value = intLiteral(at, at);
            invoke->args.push_back(l);
            Typed result;
            result.expr = invoke;
            result.type = CType::of(Kind::Void);
            return result;
        }
        return libcCall(name, args, at, col);
    }

    const FuncInfo& info = pickOverload(user->second, args, ascii8, at);
    auto* invoke = node<T::InvocationExpr>(at, col);
    invoke->callee = globalRef(name, at, col);
    bindArguments(info, args, invoke->args, ascii8, at);
    Typed result;
    result.expr = invoke;
    result.type = info.returnType;
    return result;
}

/// The arguments of a call, converted to the parameters they bind to. A
/// scalar pointer parameter wants `&x` (C) and a reference parameter wants a
/// variable (C++); `&s` for a struct is the struct, `&a[0]` is the array.
Parser::Expr* Parser::bindArguments(const FuncInfo& info, std::vector<Typed>& args,
                                    std::vector<T::Argument>& out, const std::string& what,
                                    std::uint32_t at)
{
    for (std::size_t i = 0; i < args.size(); ++i) {
        Typed& arg = args[i];
        const CType& param = info.params[i];
        T::Argument a;
        if (info.byRef[i]) {
            if (arg.addressOf) {
                if (!arg.addressType.same(param))
                    fail(at, "argument " + std::to_string(i + 1) + " of '" + what + "': expected " +
                                 describe(param) + "*, got " + describe(arg.addressType) + "*");
                a.value = arg.addressOf;
            } else if (arg.isRef && arg.type.same(param)) {
                a.value = arg.expr;
            } else if (cpp() && isAssignable(arg.expr) && !arg.store &&
                       (arg.type.same(param) || (param.kind == Kind::Bool && arg.type.kind == Kind::Bool))) {
                a.value = arg.expr;  // a C++ reference parameter takes the variable itself
            } else if (cpp()) {
                fail(at, "argument " + std::to_string(i + 1) + " of '" + what +
                             "' is a reference to " + describe(param) + " and needs a variable of that type");
            } else {
                fail(at, "argument " + std::to_string(i + 1) + " of '" + what +
                             "' is passed by reference; write '&variable'");
            }
            a.mode = T::ParamMode::Ref;
        } else if (arg.addressOf) {
            // &s for a struct parameter is the struct; &a[0] for an array is the array.
            if (param.kind == Kind::Struct && arg.addressType.same(param)) {
                a.value = arg.addressOf;
            } else if (param.kind == Kind::Array && arg.addressOf->kind == ExprKind::ElementAccess) {
                auto* element = static_cast<T::ElementAccessExpr*>(arg.addressOf);
                std::int64_t zero = 1;
                if (!evalConst(element->index, zero) || zero != 0)
                    fail(at, "only '&array[0]' can be passed where an array is expected");
                a.value = element->target;
            } else {
                fail(at, "argument " + std::to_string(i + 1) + " of '" + what + "' is not a pointer");
            }
        } else {
            a.value = coerce(arg, param);
        }
        out.push_back(a);
    }
    return nullptr;
}

Parser::Typed Parser::libcCall(const String& name, std::vector<Typed>& args, std::uint32_t at,
                               std::uint32_t col)
{
    const std::string ascii8 = toUtf8(name);
    const LibcFunction* chosen = nullptr;
    for (const LibcFunction* f = libcFunctions(); f->name; ++f) {
        if (ascii8 != f->name)
            continue;
        chosen = chosen ? chosen : f;
        // Prefer the overload whose reference parameters match.
        bool fits = true;
        std::size_t n = std::strlen(f->params);
        for (std::size_t i = 0; i < n && fits; ++i) {
            if (f->params[i] == 'V')
                break;
            if (i >= args.size()) {
                fits = false;
                break;
            }
            const CType& t = args[i].type;
            switch (f->params[i]) {
                case 's': fits = t.kind == Kind::Str || t.kind == Kind::Null; break;
                case 'A': fits = t.kind == Kind::Array && t.elem->kind == Kind::Char; break;
                default: fits = t.isNumeric() || t.kind == Kind::Bool; break;
            }
        }
        if (fits && (n == 0 || f->params[n - 1] == 'V' || n == args.size())) {
            chosen = f;
            break;
        }
    }
    if (!chosen)
        fail(at, "use of undeclared function '" + ascii8 + "'");

    auto* invoke = node<T::InvocationExpr>(at, col);
    invoke->callee = libcRef(chosen->name, at);
    const std::size_t n = std::strlen(chosen->params);
    std::size_t i = 0;
    for (; i < n; ++i) {
        const char letter = chosen->params[i];
        if (letter == 'V') {
            // The rest, boxed: printf's arguments.
            auto* rest = node<T::ArrayCreationExpr>(at);
            TypeRef* object = arena_.make<TypeRef>();
            object->predefined = T::Tok::Object;
            object->line = at;
            rest->elementTypeRef = object;
            rest->hasInitializer = true;
            for (; i < args.size(); ++i) {
                Typed& arg = args[i];
                if (arg.addressOf || !arg.expr)
                    fail(at, "invalid argument to '" + ascii8 + "'");
                if (arg.type.kind == Kind::Bool)
                    rest->elements.push_back(coerce(arg, CType::of(Kind::Int)));
                else
                    rest->elements.push_back(arg.expr);
            }
            T::Argument a;
            a.value = rest;
            invoke->args.push_back(a);
            break;
        }
        if (i >= args.size())
            fail(at, "too few arguments to '" + ascii8 + "'");
        Typed& arg = args[i];
        if (arg.addressOf || !arg.expr)
            fail(at, "invalid argument to '" + ascii8 + "'");
        T::Argument a;
        const CType want = kindOfLetter(letter);
        if (letter == 's' || letter == 'A') {
            if (!(arg.type.same(want) || (letter == 's' && arg.type.kind == Kind::Null)))
                fail(at, "argument " + std::to_string(i + 1) + " of '" + ascii8 + "': expected " +
                             (letter == 's' ? "char*" : "char[]") + ", got " + describe(arg.type));
            a.value = arg.expr;
        } else {
            a.value = coerce(arg, want);
        }
        invoke->args.push_back(a);
    }
    if (i < args.size() && (n == 0 || chosen->params[n - 1] != 'V'))
        fail(at, "too many arguments to '" + ascii8 + "'");
    Typed result;
    result.expr = invoke;
    result.type = kindOfLetter(chosen->result);
    return result;
}

// ------------------------------------------------------------ host classes ---

const HostClassSpec* Parser::hostClassNamed(const String& name) const
{
    for (const HostClassSpec& spec : hostClasses_) {
        if (spec.name == name)
            return &spec;
    }
    return nullptr;
}

/// `Env.Greet("x")`, `v.Length()`, `v.X`: a member of a host class, whose
/// types come from the binding layer's C# spelling.
Parser::Typed Parser::hostMember(Typed base, const HostClassSpec& spec, std::uint32_t at,
                                 std::uint32_t col)
{
    advance();  // . or ->
    const String name = expectName("a member name");
    const bool staticAccess = base.hostClass != nullptr;
    const HostClassSpec::Member* found = nullptr;
    const bool isCall = is(Tok::LParen);
    for (const HostClassSpec::Member& m : spec.members) {
        if (m.name != name || m.isConstructor)
            continue;
        if (m.isMethod != isCall)
            continue;
        found = &m;
        break;
    }
    if (!found)
        fail(at, "'" + toUtf8(spec.name) + "' has no member '" + toUtf8(name) + "'");
    if (found->isStatic != staticAccess)
        fail(at, "'" + toUtf8(name) + "' is " + (found->isStatic ? "a static" : "an instance") +
                     " member of '" + toUtf8(spec.name) + "'");
    Expr* target = staticAccess ? nameRef(spec.name, at, col) : base.expr;
    auto* access = node<T::MemberAccessExpr>(at, col);
    access->target = target;
    access->name = name;
    Typed result;
    result.type = found->result;
    if (!isCall) {
        result.expr = access;
        return result;
    }
    std::vector<Typed> args = arguments();
    if (args.size() != found->params.size())
        fail(at, "'" + toUtf8(spec.name) + "." + toUtf8(name) + "' takes " +
                     std::to_string(found->params.size()) + " argument" +
                     (found->params.size() == 1 ? "" : "s"));
    auto* invoke = node<T::InvocationExpr>(at, col);
    invoke->callee = access;
    for (std::size_t i = 0; i < args.size(); ++i) {
        T::Argument a;
        a.value = coerce(args[i], found->params[i]);
        invoke->args.push_back(a);
    }
    result.expr = invoke;
    return result;
}

/// `Vec2(1, 2)`: construct an instance of a host class.
Parser::Typed Parser::hostConstruct(const HostClassSpec& spec, std::uint32_t at, std::uint32_t col)
{
    if (!spec.hasConstructor)
        fail(at, "'" + toUtf8(spec.name) + "' has no constructor");
    std::vector<Typed> args = arguments();
    const HostClassSpec::Member* ctor = nullptr;
    for (const HostClassSpec::Member& m : spec.members) {
        if (m.isConstructor && m.params.size() == args.size())
            ctor = &m;
    }
    if (!ctor)
        fail(at, "no constructor of '" + toUtf8(spec.name) + "' takes " + std::to_string(args.size()) + " arguments");
    auto* create = node<T::ObjectCreationExpr>(at, col);
    create->typeRef = typeRef(CType::structOf(spec.name), at);
    for (std::size_t i = 0; i < args.size(); ++i) {
        T::Argument a;
        a.value = coerce(args[i], ctor->params[i]);
        create->args.push_back(a);
    }
    Typed result;
    result.expr = create;
    result.type = CType::structOf(spec.name);
    return result;
}

// ------------------------------------------------------------------ typing ---

CType Parser::promote(const CType& a, const CType& b) const
{
    auto rank = [](Kind k) {
        switch (k) {
            case Kind::Double: return 7;
            case Kind::Float: return 6;
            case Kind::ULong: return 5;
            case Kind::Long: return 4;
            case Kind::UInt: return 3;
            default: return 2;  // int and everything narrower
        }
    };
    const int ra = rank(a.kind);
    const int rb = rank(b.kind);
    const Kind wider = ra >= rb ? a.kind : b.kind;
    switch (wider) {
        case Kind::Double: return CType::of(Kind::Double);
        case Kind::Float: return CType::of(Kind::Float);
        case Kind::ULong: return CType::of(Kind::ULong);
        case Kind::Long: return CType::of(Kind::Long);
        case Kind::UInt: {
            // uint with a signed int is long, as the engine's checker has it.
            const Kind other = ra >= rb ? b.kind : a.kind;
            if (other == Kind::Int || other == Kind::Short || other == Kind::Char)
                return CType::of(Kind::Long);
            return CType::of(Kind::UInt);
        }
        default: return CType::of(Kind::Int);
    }
}

bool Parser::widens(const CType& from, const CType& to)
{
    if (from.same(to))
        return true;
    if (!from.isNumeric() || !to.isNumeric())
        return false;
    const Kind f = from.kind;
    const Kind t = to.kind;
    auto in = [t](std::initializer_list<Kind> kinds) {
        for (Kind k : kinds)
            if (k == t)
                return true;
        return false;
    };
    switch (f) {
        case Kind::Char: return in({Kind::UShort, Kind::Int, Kind::UInt, Kind::Long, Kind::ULong, Kind::Float, Kind::Double});
        case Kind::UChar: return in({Kind::Short, Kind::UShort, Kind::Int, Kind::UInt, Kind::Long, Kind::ULong, Kind::Float, Kind::Double});
        case Kind::Short: return in({Kind::Int, Kind::Long, Kind::Float, Kind::Double});
        case Kind::UShort: return in({Kind::Int, Kind::UInt, Kind::Long, Kind::ULong, Kind::Float, Kind::Double});
        case Kind::Int: return in({Kind::Long, Kind::Float, Kind::Double});
        case Kind::UInt: return in({Kind::Long, Kind::ULong, Kind::Float, Kind::Double});
        case Kind::Long: return in({Kind::Float, Kind::Double});
        case Kind::ULong: return in({Kind::Float, Kind::Double});
        case Kind::Float: return t == Kind::Double;
        default: return false;
    }
}

/// The integer constant 0, which C also reads as the null pointer.
bool Parser::isZero(const Typed& v)
{
    std::int64_t n = 1;
    return v.expr && v.type.isIntegral() && v.expr->kind == ExprKind::Literal && evalConst(v.expr, n) && n == 0;
}

Parser::Expr* Parser::coerce(Typed value, const CType& to)
{
    const std::uint32_t at = value.expr ? value.expr->line : line();
    if (value.braceList) {
        // `{1, 2, 3}` where a vector, a class or an array is wanted.
        std::vector<Typed> args = *value.braceList;
        lastArgsBraced_ = true;
        if (to.kind == Kind::Array) {
            auto* create = node<T::ArrayCreationExpr>(at);
            create->elementTypeRef = typeRef(*to.elem, at);
            create->hasInitializer = true;
            for (Typed& a : args)
                create->elements.push_back(coerce(a, *to.elem));
            return create;
        }
        return construct(to, args, at, 0).expr;
    }
    if (value.addressOf)
        fail(at, "'&' is only allowed in a call argument");
    if (!value.expr) {
        if (!value.funcName.empty())
            fail(at, "'" + toUtf8(value.funcName) + "' is a function; function pointers are not supported");
        fail(at, "a void value cannot be used here");
    }
    if (value.isRef)
        fail(at, "'" + toUtf8(static_cast<T::NameExpr*>(value.expr)->name) + "' is a pointer; use '*" +
                     toUtf8(static_cast<T::NameExpr*>(value.expr)->name) + "'");
    if (value.type.kind == Kind::Void)
        fail(at, "a void value cannot be used here");
    if (to.kind == Kind::Bool)
        return condition(value);
    if (value.type.kind == Kind::Bool) {
        if (!to.isNumeric())
            fail(at, "cannot convert a comparison to " + describe(to));
        auto* pick = node<T::ConditionalExpr>(at, value.expr->column);
        pick->condition = value.expr;
        pick->whenTrue = intLiteral(1, at);
        pick->whenFalse = intLiteral(0, at);
        Typed asInt;
        asInt.expr = pick;
        asInt.type = CType::of(Kind::Int);
        return coerce(asInt, to);
    }
    if (value.type.isNumeric() && to.isNumeric()) {
        if (widens(value.type, to))
            return value.expr;
        // A literal that fits needs no cast, but one does no harm either.
        return castTo(value.expr, to, at);
    }
    if (value.type.kind == Kind::Null && to.isReference())
        return value.expr;
    if (to.isReference() && isZero(value))
        return nullLiteral(at);  // the integer constant 0 is a null pointer
    if (value.type.isReference() && to.isReference()) {
        if (!value.type.same(to) && value.type.kind != Kind::Null) {
            if (cpp() && value.type.kind == Kind::Struct && to.kind == Kind::Struct &&
                isSubclass(value.type.structName, to.structName))
                return value.expr;  // Derived* to Base*
            if (cpp() && value.type.kind == Kind::Vector && to.kind == Kind::Array &&
                value.type.elem->same(*to.elem))
                return invoke(memberRef(value.expr, "ToArray", at), {}, at);  // to a host `std::vector<T>`
            fail(at, "cannot convert " + describe(value.type) + " to " + describe(to));
        }
        return value.expr;
    }
    fail(at, "cannot convert " + describe(value.type) + " to " + describe(to));
}

Parser::Expr* Parser::condition(Typed value)
{
    const std::uint32_t at = value.expr ? value.expr->line : line();
    if (value.addressOf)
        fail(at, "'&' is only allowed in a call argument");
    if (!value.expr)
        fail(at, "a void value cannot be tested");
    if (value.isRef)
        fail(at, "'" + toUtf8(static_cast<T::NameExpr*>(value.expr)->name) + "' is a pointer; use '*" +
                     toUtf8(static_cast<T::NameExpr*>(value.expr)->name) + "'");
    if (value.type.kind == Kind::Bool)
        return value.expr;
    if (value.type.kind == Kind::Null)
        return boolLiteral(false, at);
    auto* b = node<T::BinaryExpr>(at, value.expr->column);
    b->op = BinaryOp::NotEqual;
    b->left = value.expr;
    if (value.type.isReference())
        b->right = nullLiteral(at);
    else if (value.type.isNumeric())
        b->right = intLiteral(0, at);
    else
        fail(at, "a value of type " + describe(value.type) + " cannot be tested");
    return b;
}

Parser::Expr* Parser::cloneSimple(const Expr* e) const
{
    if (!e)
        return nullptr;
    switch (e->kind) {
        case ExprKind::Name: {
            auto* src = static_cast<const T::NameExpr*>(e);
            auto* copy = arena_.make<T::NameExpr>();
            copy->line = src->line;
            copy->column = src->column;
            copy->name = src->name;
            return copy;
        }
        case ExprKind::Literal: {
            auto* src = static_cast<const T::LiteralExpr*>(e);
            auto* copy = arena_.make<T::LiteralExpr>();
            copy->line = src->line;
            copy->column = src->column;
            copy->literal = src->literal;
            copy->intValue = src->intValue;
            copy->realValue = src->realValue;
            copy->text = src->text;
            return copy;
        }
        case ExprKind::This: {
            auto* copy = arena_.make<T::ThisExpr>();
            copy->line = e->line;
            copy->column = e->column;
            return copy;
        }
        case ExprKind::MemberAccess: {
            auto* src = static_cast<const T::MemberAccessExpr*>(e);
            Expr* target = cloneSimple(src->target);
            if (!target)
                return nullptr;
            auto* copy = arena_.make<T::MemberAccessExpr>();
            copy->line = src->line;
            copy->column = src->column;
            copy->target = target;
            copy->name = src->name;
            return copy;
        }
        case ExprKind::ElementAccess: {
            auto* src = static_cast<const T::ElementAccessExpr*>(e);
            Expr* target = cloneSimple(src->target);
            Expr* index = cloneSimple(src->index);
            if (!target || !index)
                return nullptr;
            auto* copy = arena_.make<T::ElementAccessExpr>();
            copy->line = src->line;
            copy->column = src->column;
            copy->target = target;
            copy->index = index;
            return copy;
        }
        default: return nullptr;
    }
}

bool Parser::evalConst(const Expr* e, std::int64_t& out)
{
    if (!e)
        return false;
    switch (e->kind) {
        case ExprKind::Literal: {
            auto* lit = static_cast<const T::LiteralExpr*>(e);
            switch (lit->literal) {
                case LiteralKind::Int:
                case LiteralKind::UInt:
                case LiteralKind::Long:
                case LiteralKind::ULong:
                case LiteralKind::Char:
                case LiteralKind::Bool: out = lit->intValue; return true;
                default: return false;
            }
        }
        case ExprKind::Unary: {
            auto* u = static_cast<const T::UnaryExpr*>(e);
            std::int64_t v = 0;
            if (!evalConst(u->operand, v))
                return false;
            switch (u->op) {
                case UnaryOp::Minus: out = -v; return true;
                case UnaryOp::Plus: out = v; return true;
                case UnaryOp::Complement: out = ~v; return true;
                case UnaryOp::Not: out = v == 0; return true;
                default: return false;
            }
        }
        case ExprKind::Binary: {
            auto* b = static_cast<const T::BinaryExpr*>(e);
            std::int64_t l = 0;
            std::int64_t r = 0;
            if (!evalConst(b->left, l) || !evalConst(b->right, r))
                return false;
            switch (b->op) {
                case BinaryOp::Add: out = l + r; return true;
                case BinaryOp::Subtract: out = l - r; return true;
                case BinaryOp::Multiply: out = l * r; return true;
                case BinaryOp::Divide: if (r == 0) return false; out = l / r; return true;
                case BinaryOp::Modulo: if (r == 0) return false; out = l % r; return true;
                case BinaryOp::And: out = l & r; return true;
                case BinaryOp::Or: out = l | r; return true;
                case BinaryOp::Xor: out = l ^ r; return true;
                case BinaryOp::ShiftLeft: out = std::int64_t(std::uint64_t(l) << (r & 63)); return true;
                case BinaryOp::ShiftRight: out = l >> (r & 63); return true;
                case BinaryOp::Equal: out = l == r; return true;
                case BinaryOp::NotEqual: out = l != r; return true;
                case BinaryOp::Less: out = l < r; return true;
                case BinaryOp::LessEqual: out = l <= r; return true;
                case BinaryOp::Greater: out = l > r; return true;
                case BinaryOp::GreaterEqual: out = l >= r; return true;
                default: return false;
            }
        }
        case ExprKind::Cast: return evalConst(static_cast<const T::CastExpr*>(e)->operand, out);
        case ExprKind::Conditional: {
            auto* c = static_cast<const T::ConditionalExpr*>(e);
            std::int64_t test = 0;
            if (!evalConst(c->condition, test))
                return false;
            return evalConst(test ? c->whenTrue : c->whenFalse, out);
        }
        default: return false;
    }
}

std::int64_t Parser::constantInt(const char* what)
{
    const std::uint32_t at = line();
    Typed v = conditional();
    std::int64_t out = 0;
    if (!v.expr || !evalConst(v.expr, out))
        fail(at, std::string("expected a constant integer for ") + what);
    return out;
}

}  // namespace c
}  // namespace internal
}  // namespace script
