// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The C++ dialect of the C parser: classes, references, `new`, `bool`,
// `std::string`, `std::vector`, `std::map`, streams, exceptions, namespaces
// and the rest of what C++ adds. Everything here lowers onto the same
// C#-shaped tree the C parser produces, so the checker, the emitter and the
// code generators never see a difference. See the header for the mapping.
#include <cstring>

#include "script/error.hpp"
#include "script/internal/c/parser.hpp"

namespace script {
namespace internal {
namespace c {

using typed::BinaryOp;
using typed::ExprKind;
using typed::LiteralKind;
using typed::UnaryOp;
namespace T = typed;
using Kind = CType::Kind;

namespace {

bool isAssignableExpr(const T::Expr* e) noexcept
{
    return e && (e->kind == ExprKind::Name || e->kind == ExprKind::MemberAccess ||
                 e->kind == ExprKind::ElementAccess);
}

}  // namespace

// ------------------------------------------------------------------ classes ---

const Parser::ClassInfo* Parser::classNamed(const String& name) const
{
    auto it = structs_.find(name);
    return it == structs_.end() ? nullptr : &it->second;
}

const Parser::Field* Parser::findField(const ClassInfo& info, const String& name,
                                       const ClassInfo** owner) const
{
    for (const ClassInfo* c = &info; c; c = c->base.empty() ? nullptr : classNamed(c->base)) {
        for (const Field& f : c->fields) {
            if (f.name == name) {
                if (owner)
                    *owner = c;
                return &f;
            }
        }
    }
    return nullptr;
}

const std::vector<Parser::FuncInfo>* Parser::findMethods(const ClassInfo& info, const String& name) const
{
    const ClassInfo* owner = methodOwner(info, name);
    return owner ? &owner->methods.at(name) : nullptr;
}

const Parser::ClassInfo* Parser::methodOwner(const ClassInfo& info, const String& name) const
{
    for (const ClassInfo* c = &info; c; c = c->base.empty() ? nullptr : classNamed(c->base)) {
        if (c->methods.count(name))
            return c;
    }
    return nullptr;
}

void Parser::checkAccess(const ClassInfo& owner, std::uint16_t access, const String& member,
                         std::uint32_t at) const
{
    if (!(access & (T::ModPrivate | T::ModProtected)))
        return;
    const bool inside = currentClass_ && (currentClass_->name == owner.name ||
                                          ((access & T::ModProtected) && isSubclass(currentClass_->name, owner.name)));
    if (!inside)
        fail(at, "'" + toUtf8(member) + "' is a " + ((access & T::ModPrivate) ? "private" : "protected") +
                     " member of '" + toUtf8(owner.name) + "'");
}

bool Parser::isSubclass(const String& derived, const String& base) const
{
    for (const ClassInfo* c = classNamed(derived); c; c = c->base.empty() ? nullptr : classNamed(c->base)) {
        if (c->name == base)
            return true;
    }
    return false;
}

/// `class X : public B { ... }` after the tag. The first pass records the
/// members; the second builds the declaration and parses the bodies, with
/// the whole class known, so a method may use a member declared below it.
void Parser::classDefinition(const String& name, bool isClass, std::uint32_t at)
{
    ClassInfo* info = nullptr;
    T::TypeDecl* decl = nullptr;
    if (prepass_) {
        ClassInfo fresh;
        fresh.name = name;
        fresh.isClass = isClass;
        structs_[name] = std::move(fresh);  // registered early: `X* next` inside X
        info = &structs_[name];
    } else {
        info = &structs_.at(name);
        decl = arena_.make<T::TypeDecl>();
        decl->kind = T::DeclKind::Class;
        decl->name = name;
        decl->fullName = name;
        decl->modifiers = T::ModPublic | (info->isAbstract ? T::ModAbstract : 0);
        decl->line = at;
        info->decl = decl;
    }
    if (eat(Tok::Colon)) {
        while (is(Tok::Public) || is(Tok::Private) || is(Tok::Protected) || is(Tok::Virtual))
            advance();
        skipQualifier();
        const String base = expectName("a base class");
        if (!structs_.count(base) || base == name)
            fail("unknown base class '" + toUtf8(base) + "'");
        if (is(Tok::Comma))
            fail("multiple inheritance is not supported");
        if (prepass_)
            info->base = base;
        else
            decl->baseTypes.push_back(typeRef(CType::structOf(base), at));
    }
    expect(Tok::LBrace, "'{'");
    std::uint16_t access = isClass ? T::ModPrivate : T::ModPublic;
    while (!is(Tok::RBrace)) {
        if (is(Tok::Eof))
            fail("unterminated class body");
        if (is(Tok::Public) || is(Tok::Private) || is(Tok::Protected)) {
            const Tok t = advance().type;
            expect(Tok::Colon, "':'");
            access = t == Tok::Public ? T::ModPublic : t == Tok::Private ? T::ModPrivate : T::ModProtected;
            continue;
        }
        memberDeclaration(*info, access);
    }
    advance();
    if (decl)
        unit_->types.push_back(decl);
}

void Parser::memberDeclaration(ClassInfo& info, std::uint16_t access)
{
    const std::uint32_t at = line();
    if (eat(Tok::Semicolon))
        return;
    if (is(Tok::Virtual) && type(1) == Tok::Tilde)
        advance();
    if (is(Tok::Tilde)) {
        // A destructor has nothing to do here: the collector owns the memory.
        // An empty one is accepted so ordinary classes compile; a body is not.
        advance();
        if (!is(Tok::Identifier) || advance().text != info.name)
            fail(at, "a destructor is named after its class");
        expect(Tok::LParen, "'('");
        expect(Tok::RParen, "')'");
        while (is(Tok::Override) || is(Tok::Noexcept))
            advance();
        if (eat(Tok::Assign)) {
            expect(Tok::Default, "'default'");
            expect(Tok::Semicolon, "';'");
            return;
        }
        if (eat(Tok::Semicolon))
            return;
        expect(Tok::LBrace, "'{'");
        if (!eat(Tok::RBrace))
            fail(at, "a destructor with a body is not supported: the collector owns the memory");
        return;
    }
    switch (type()) {
        case Tok::Friend: fail("'friend' is not supported");
        case Tok::Template: fail("templates are not supported");
        case Tok::Operator: fail("operator overloading is not supported");
        case Tok::Using:
        case Tok::Typedef: fail("type aliases inside a class are not supported; declare them at file scope");
        case Tok::Struct:
        case Tok::Class:
            if (type(1) == Tok::Identifier && (type(2) == Tok::LBrace || type(2) == Tok::Colon))
                fail("nested classes are not supported; declare the class at file scope");
            break;
        case Tok::Enum:
            enumDefinition();
            expect(Tok::Semicolon, "';'");
            return;
        default: break;
    }
    const bool isExplicit = eat(Tok::Explicit);
    if (is(Tok::Identifier) && peek().text == info.name && type(1) == Tok::LParen) {
        advance();
        constructorDefinition(info, access, at);
        return;
    }
    if (isExplicit)
        fail("'explicit' applies to a constructor");
    Specifiers spec = specifiers();
    if (!spec.any)
        fail("expected a member declaration");
    if (spec.isAuto)
        fail("a member cannot be 'auto'");
    if (is(Tok::Operator))
        fail("operator overloading is not supported");
    do {
        Declarator d = declarator(spec.base);
        if (!d.owner.empty())
            fail(d.line, "a qualified name is not allowed inside a class body");
        if (d.isFunction) {
            methodDefinition(info, spec, std::move(d), access, /*inClass=*/true);
            return;
        }
        if (d.isReference)
            fail(d.line, "a reference member is not supported");
        if (d.type.kind == Kind::Void)
            fail(d.line, "a member cannot be void");
        if (is(Tok::Colon))
            fail(d.line, "bit-fields are not supported");
        Expr* init = nullptr;
        if (eat(Tok::Assign)) {
            init = initializer(d.type, &d, d.line);
        } else if (is(Tok::LBrace)) {
            std::vector<Typed> args = arguments();
            init = construct(d.type, args, d.line, d.column).expr;
        } else if (d.type.kind == Kind::Struct && d.pointers == 0) {
            // A member object is built with its owner -- unless its class
            // has constructors, when the owner's initializer list does it.
            const ClassInfo* member = classNamed(d.type.structName);
            const HostClassSpec* host = member ? nullptr : hostClassNamed(d.type.structName);
            bool parameterless = member ? member->ctors.empty() : false;
            if (host) {
                for (const HostClassSpec::Member& m : host->members)
                    parameterless = parameterless || (m.isConstructor && m.params.empty());
            }
            if (parameterless)
                init = defaultValue(d.type, &d, d.line);
        } else if (d.pointers == 0) {
            init = defaultValue(d.type, &d, d.line);
        }
        if (d.type.kind == Kind::Struct && d.pointers == 0 && d.type.structName == info.name)
            fail(d.line, "a class cannot contain itself by value; use a pointer");
        if (prepass_) {
            info.fields.push_back({d.name, d.type, spec.isStatic, access});
        } else {
            T::FieldDecl field;
            field.line = d.line;
            field.name = d.name;
            field.modifiers = access | (spec.isStatic ? T::ModStatic : 0);
            if (spec.isStatic && spec.isConst && d.type.isNumeric() && init)
                field.modifiers = access | T::ModConst;
            field.typeRef = typeRef(d.type, d.line);
            field.init = init;
            info.decl->fields.push_back(std::move(field));
        }
    } while (eat(Tok::Comma));
    expect(Tok::Semicolon, "';'");
}

void Parser::methodDefinition(ClassInfo& info, const Specifiers& spec, Declarator d,
                              std::uint16_t access, bool inClass)
{
    const std::uint32_t at = d.line;
    FuncInfo fi;
    fi.returnType = d.type;
    fi.isStatic = spec.isStatic;
    fi.isVirtual = spec.isVirtual;
    fi.access = access;
    std::vector<CType> types;
    std::vector<String> names;
    std::vector<bool> scalarPointer;
    std::vector<T::ParamDecl> params = parameters(fi, types, names, scalarPointer);
    if (fi.variadic)
        fail(at, "variadic functions are not supported");
    while (is(Tok::Const) || is(Tok::Noexcept))
        advance();
    bool isOverride = eat(Tok::Override);
    bool isPure = false;
    if (eat(Tok::Assign)) {
        if (is(Tok::IntLit) && peek().intValue == 0) {
            advance();
            isPure = true;
        } else {
            fail("'= default' and '= delete' are not supported");
        }
    }
    const bool isDefinition = is(Tok::LBrace);
    if (!isDefinition)
        expect(Tok::Semicolon, "';'");
    resolvePointerParams(params, types, names, scalarPointer, fi, isDefinition);

    // A method with the signature of a virtual one in a base class overrides
    // it, whether or not `override` was written.
    if (!info.base.empty()) {
        if (const std::vector<FuncInfo>* inherited = findMethods(*classNamed(info.base), d.name)) {
            for (const FuncInfo& m : *inherited) {
                bool same = m.params.size() == fi.params.size();
                for (std::size_t i = 0; same && i < fi.params.size(); ++i)
                    same = m.params[i].same(fi.params[i]);
                if (same && m.isVirtual) {
                    fi.isVirtual = true;
                    isOverride = true;
                }
            }
        }
    }
    if (isPure) {
        fi.isVirtual = true;
        info.isAbstract = true;
    }
    std::vector<FuncInfo>& overloads = info.methods[d.name];
    if (!inClass) {
        // Out of line: the class body declared it, and knows its access,
        // storage and virtuality.
        const FuncInfo* declared = nullptr;
        for (const FuncInfo& m : overloads) {
            bool same = m.params.size() == fi.params.size();
            for (std::size_t i = 0; same && i < fi.params.size(); ++i)
                same = m.params[i].same(fi.params[i]);
            if (same)
                declared = &m;
        }
        if (!declared)
            fail(at, "'" + toUtf8(info.name) + "::" + toUtf8(d.name) + "' was not declared in the class");
        fi.access = declared->access;
        fi.isStatic = declared->isStatic;
        fi.isVirtual = fi.isVirtual || declared->isVirtual;
        if (!isDefinition)
            fail(at, "expected a function body");
    }
    if (prepass_) {
        registerFunction(overloads, fi, d.name, at, &params);
        if (isDefinition)
            skipBalanced(Tok::LBrace, Tok::RBrace);
        return;
    }
    if (!isDefinition && !isPure)
        return;  // the body is defined out of line
    if (!inClass) {
        for (const FuncInfo& m : overloads) {
            bool same = m.params.size() == fi.params.size();
            for (std::size_t i = 0; same && i < fi.params.size(); ++i)
                same = m.params[i].same(fi.params[i]);
            if (same)
                for (std::size_t i = 0; i < params.size() && i < m.defaults.size(); ++i)
                    if (!params[i].defaultValue)
                        params[i].defaultValue = m.defaults[i];
        }
    }

    auto* method = arena_.make<T::MethodDecl>();
    method->line = at;
    method->name = d.name;
    method->modifiers = fi.access;
    if (fi.isStatic)
        method->modifiers |= T::ModStatic;
    else if (isPure)
        method->modifiers |= T::ModAbstract | T::ModVirtual;
    else if (isOverride)
        method->modifiers |= T::ModOverride;
    else if (fi.isVirtual)
        method->modifiers |= T::ModVirtual;
    method->returnTypeRef = typeRef(d.type, at);
    method->params = params;
    method->owner = info.decl;
    if (isDefinition) {
        currentClass_ = &info;
        currentStatic_ = fi.isStatic;
        method->body = functionBody(params, types, names, fi, d.type, info.name + u"$" + d.name);
        currentClass_ = nullptr;
        currentStatic_ = false;
    }
    info.decl->methods.push_back(method);
}

/// After the class name: `(params) [: inits] { body }`, or a prototype.
void Parser::constructorDefinition(ClassInfo& info, std::uint16_t access, std::uint32_t at)
{
    FuncInfo fi;
    fi.returnType = CType::of(Kind::Void);
    fi.access = access;
    std::vector<CType> types;
    std::vector<String> names;
    std::vector<bool> scalarPointer;
    std::vector<T::ParamDecl> params = parameters(fi, types, names, scalarPointer);
    while (is(Tok::Noexcept))
        advance();
    bool defaulted = false;
    if (eat(Tok::Assign)) {
        if (eat(Tok::Default))
            defaulted = true;
        else
            fail("'= delete' is not supported");
    }
    const bool isDefinition = is(Tok::Colon) || is(Tok::LBrace);
    if (!isDefinition)
        expect(Tok::Semicolon, "';'");
    resolvePointerParams(params, types, names, scalarPointer, fi, isDefinition);
    if (prepass_) {
        registerFunction(info.ctors, fi, info.name, at, &params);
        if (isDefinition) {
            // Skip the initializer list, then the body.
            while (!(is(Tok::LBrace) && toks_[pos_ - 1].type != Tok::Identifier)) {
                if (is(Tok::Eof))
                    fail("unterminated constructor");
                if (is(Tok::LParen))
                    skipBalanced(Tok::LParen, Tok::RParen);
                else if (is(Tok::LBrace))
                    skipBalanced(Tok::LBrace, Tok::RBrace);
                else
                    advance();
            }
            skipBalanced(Tok::LBrace, Tok::RBrace);
        }
        return;
    }
    if (!isDefinition && !defaulted)
        return;
    // Out of line: take the access and defaults the class body gave this signature.
    for (const FuncInfo& c : info.ctors) {
        bool same = c.params.size() == fi.params.size();
        for (std::size_t i = 0; same && i < fi.params.size(); ++i)
            same = c.params[i].same(fi.params[i]);
        if (same) {
            fi.access = c.access;
            for (std::size_t i = 0; i < params.size() && i < c.defaults.size(); ++i)
                if (!params[i].defaultValue)
                    params[i].defaultValue = c.defaults[i];
        }
    }
    if (fi.access == 0)
        fi.access = T::ModPublic;
    auto* method = arena_.make<T::MethodDecl>();
    method->line = at;
    method->name = ascii(".ctor");
    method->isConstructor = true;
    method->modifiers = fi.access;
    method->returnTypeRef = typeRef(CType::of(Kind::Void), at);
    method->params = params;
    method->owner = info.decl;
    if (defaulted) {
        method->body = node<T::BlockStmt>(at);
    } else {
        constructorBody(info, method, types, names, fi);
    }
    info.decl->methods.push_back(method);
}

void Parser::constructorBody(ClassInfo& info, T::MethodDecl* method, const std::vector<CType>& types,
                             const std::vector<String>& names, const FuncInfo& fi)
{
    pushScope();
    for (std::size_t i = 0; i < names.size(); ++i) {
        Symbol s;
        s.type = types[i];
        s.isRef = fi.byRef[i] && fi.refByPointer[i];
        declare(names[i], s, method->params[i].line);
    }
    returnType_ = CType::of(Kind::Void);
    currentFunction_ = info.name + u"$ctor";
    currentClass_ = &info;
    currentStatic_ = false;

    std::vector<Stmt*> inits;
    if (eat(Tok::Colon)) {
        do {
            const std::uint32_t at = line();
            const std::uint32_t col = column();
            skipQualifier();
            const String target = expectName("a member or the base class");
            std::vector<Typed> args = arguments();
            if (target == info.base) {
                const ClassInfo& base = *classNamed(info.base);
                if (base.ctors.empty()) {
                    if (!args.empty())
                        fail(at, "'" + toUtf8(base.name) + "' has no constructor taking arguments");
                    continue;
                }
                const FuncInfo& ctor = pickOverload(base.ctors, args, toUtf8(base.name), at);
                method->chainsToBase = true;
                bindArguments(ctor, args, method->chainArgs, toUtf8(base.name), at);
                continue;
            }
            const ClassInfo* owner = nullptr;
            const Field* field = findField(info, target, &owner);
            if (!field || owner != &info)
                fail(at, "'" + toUtf8(target) + "' is not a member of '" + toUtf8(info.name) + "'");
            if (field->isStatic)
                fail(at, "'" + toUtf8(target) + "' is static and cannot be initialised here");
            Expr* value = nullptr;
            if (args.empty()) {
                value = zeroValue(field->type, at);
            } else if (args.size() == 1 && !(field->type.kind == Kind::Struct && !args[0].type.same(field->type) &&
                                             args[0].type.kind != Kind::Null)) {
                value = coerce(args[0], field->type);
            } else {
                value = construct(field->type, args, at, col).expr;
            }
            auto* assign = node<T::AssignmentExpr>(at, col);
            auto* member = node<T::MemberAccessExpr>(at, col);
            member->target = node<T::ThisExpr>(at, col);
            member->name = target;
            assign->target = member;
            assign->value = value;
            inits.push_back(exprStatement(assign, at));
        } while (eat(Tok::Comma));
    }
    auto* body = static_cast<T::BlockStmt*>(block());
    body->statements.insert(body->statements.begin(), inits.begin(), inits.end());
    popScope();
    currentClass_ = nullptr;
    method->body = body;
}

/// `T X::m(...) { }` or `int X::count = 0;` at file scope.
void Parser::outOfLineDefinition(const Specifiers& spec, Declarator decl)
{
    auto it = structs_.find(decl.owner);
    if (it == structs_.end())
        fail(decl.line, "'" + toUtf8(decl.owner) + "' is not a class");
    ClassInfo& info = it->second;
    if (decl.isFunction) {
        methodDefinition(info, spec, std::move(decl), 0, /*inClass=*/false);
        return;
    }
    // A static data member's definition.
    Expr* init = nullptr;
    if (eat(Tok::Assign)) {
        init = initializer(decl.type, &decl, decl.line);
    } else if (is(Tok::LParen) || is(Tok::LBrace)) {
        std::vector<Typed> args = arguments();
        init = construct(decl.type, args, decl.line, decl.column).expr;
    }
    expect(Tok::Semicolon, "';'");
    if (prepass_)
        return;
    for (T::FieldDecl& field : info.decl->fields) {
        if (field.name == decl.name && (field.modifiers & (T::ModStatic | T::ModConst))) {
            if (init)
                field.init = init;
            return;
        }
    }
    fail(decl.line, "'" + toUtf8(decl.owner) + "::" + toUtf8(decl.name) + "' is not a static member");
}

// ---------------------------------------------------------- calls, members ---

const Parser::FuncInfo& Parser::pickOverload(const std::vector<FuncInfo>& overloads,
                                             const std::vector<Typed>& args, const std::string& what,
                                             std::uint32_t at) const
{
    if (overloads.size() == 1 && !cpp()) {
        const FuncInfo& only = overloads.front();
        if (args.size() != only.params.size())
            fail(at, "'" + what + "' takes " + std::to_string(only.params.size()) + " argument" +
                         (only.params.size() == 1 ? "" : "s") + ", " + std::to_string(args.size()) + " given");
        return only;
    }
    const FuncInfo* best = nullptr;
    int bestScore = -1;
    bool ambiguous = false;
    for (const FuncInfo& f : overloads) {
        if (args.size() < f.required || args.size() > f.params.size())
            continue;
        int score = 0;
        bool ok = true;
        for (std::size_t i = 0; ok && i < args.size(); ++i) {
            const CType& p = f.params[i];
            const Typed& a = args[i];
            if (a.addressOf) {
                ok = f.byRef[i] ? a.addressType.same(p) : (p.kind == Kind::Struct || p.kind == Kind::Array);
                score += ok ? 2 : 0;
            } else if (a.braceList) {
                ok = p.isContainer() || p.kind == Kind::Struct || p.kind == Kind::Array;
                score += ok ? 1 : 0;
            } else if (!a.expr) {
                ok = false;
            } else if (a.type.same(p)) {
                score += 3;
            } else if (a.type.isNumeric() && p.isNumeric()) {
                score += widens(a.type, p) ? 2 : 1;
            } else if ((a.type.kind == Kind::Bool && p.isNumeric()) || (a.type.isNumeric() && p.kind == Kind::Bool)) {
                score += 1;
            } else if (a.type.kind == Kind::Null && p.isReference()) {
                score += 1;
            } else if (a.type.kind == Kind::Struct && p.kind == Kind::Struct &&
                       isSubclass(a.type.structName, p.structName)) {
                score += 2;
            } else if (a.type.kind == Kind::Vector && p.kind == Kind::Array && a.type.elem->same(*p.elem)) {
                score += 1;
            } else if (isZero(a) && p.isReference()) {
                score += 0;
            } else {
                ok = false;
            }
        }
        if (!ok)
            continue;
        if (score > bestScore) {
            best = &f;
            bestScore = score;
            ambiguous = false;
        } else if (score == bestScore) {
            ambiguous = true;
        }
    }
    if (!best) {
        std::string given;
        for (const Typed& a : args)
            given += (given.empty() ? "" : ", ") + (a.expr ? describe(a.type) : std::string("?"));
        fail(at, "no overload of '" + what + "' takes (" + given + ")");
    }
    if (ambiguous)
        fail(at, "the call to '" + what + "' is ambiguous");
    return *best;
}

/// `obj.m(args)`, `X::m(args)`, or `m(args)` inside a method (`target` null).
Parser::Typed Parser::methodCall(Expr* target, const ClassInfo& owner, const String& name,
                                 std::uint32_t at, std::uint32_t col)
{
    const std::vector<FuncInfo>* overloads = findMethods(owner, name);
    if (!overloads)
        fail(at, "'" + toUtf8(owner.name) + "' has no method '" + toUtf8(name) + "'");
    std::vector<Typed> args = arguments();
    const std::string what = toUtf8(owner.name) + "::" + toUtf8(name);
    const FuncInfo& fi = pickOverload(*overloads, args, what, at);
    checkAccess(*methodOwner(owner, name), fi.access, name, at);
    Expr* callee = nullptr;
    if (fi.isStatic) {
        callee = memberRef(nameRef(owner.name, at, col), toUtf8(name).c_str(), at);
    } else if (target) {
        callee = memberRef(target, toUtf8(name).c_str(), at);
    } else {
        // Bare, inside a method of the class or a subclass.
        if (!currentClass_ || currentStatic_ || !isSubclass(currentClass_->name, owner.name))
            fail(at, "'" + what + "' is an instance method; call it on an object");
        callee = nameRef(name, at, col);
    }
    auto* call = node<T::InvocationExpr>(at, col);
    call->callee = callee;
    bindArguments(fi, args, call->args, what, at);
    Typed result;
    result.expr = call;
    result.type = fi.returnType;
    return result;
}

/// After `.` or `->` on a class instance, positioned on the member name.
Parser::Typed Parser::classMember(Typed base, const ClassInfo& info, std::uint32_t at, std::uint32_t col)
{
    const String name = expectName("a member name");
    const ClassInfo* owner = nullptr;
    if (const Field* field = findField(info, name, &owner)) {
        if (is(Tok::LParen))
            fail(at, "'" + toUtf8(name) + "' is a field, not a method");
        checkAccess(*owner, field->access, name, at);
        Typed result;
        result.type = field->type;
        if (field->isStatic) {
            result.expr = memberRef(nameRef(owner->name, at, col), toUtf8(name).c_str(), at);
        } else {
            auto* m = node<T::MemberAccessExpr>(at, col);
            m->target = base.expr;
            m->name = name;
            result.expr = m;
        }
        return result;
    }
    if (findMethods(info, name)) {
        if (!is(Tok::LParen))
            fail(at, "'" + toUtf8(name) + "' is a method; call it");
        return methodCall(base.expr, info, name, at, col);
    }
    fail(at, "'" + toUtf8(info.name) + "' has no member '" + toUtf8(name) + "'");
}

/// `X::member` after a class name, positioned on `::`.
Parser::Typed Parser::staticMember(const ClassInfo& info, std::uint32_t at, std::uint32_t col)
{
    expect(Tok::ColonColon, "'::'");
    const String name = expectName("a member name");
    // `Base::method(...)` inside a subclass calls the base's version.
    if (currentClass_ && !currentStatic_ && currentClass_->name != info.name &&
        isSubclass(currentClass_->name, info.name) && is(Tok::LParen)) {
        if (const std::vector<FuncInfo>* overloads = findMethods(info, name)) {
            if (!overloads->front().isStatic)
                return methodCall(node<T::BaseExpr>(at, col), info, name, at, col);
        }
    }
    const ClassInfo* owner = nullptr;
    if (const Field* field = findField(info, name, &owner)) {
        if (!field->isStatic)
            fail(at, "'" + toUtf8(name) + "' is an instance member of '" + toUtf8(info.name) + "'");
        checkAccess(*owner, field->access, name, at);
        Typed result;
        result.type = field->type;
        result.expr = memberRef(nameRef(owner->name, at, col), toUtf8(name).c_str(), at);
        return result;
    }
    if (findMethods(info, name)) {
        if (!is(Tok::LParen))
            fail(at, "'" + toUtf8(name) + "' is a method; call it");
        return methodCall(nullptr, info, name, at, col);
    }
    fail(at, "'" + toUtf8(info.name) + "' has no static member '" + toUtf8(name) + "'");
}

// ----------------------------------------------------------- construction ---

/// `T(args)`, `T{args}`, `new T(args)`, `T x(args)`: a value of `type` from
/// its arguments. A scalar takes one argument and converts it.
Parser::Typed Parser::construct(const CType& type, std::vector<Typed>& args, std::uint32_t at,
                                std::uint32_t col)
{
    const bool braced = lastArgsBraced_;
    Typed result;
    result.type = type;
    for (Typed& a : args) {
        if (a.addressOf)
            fail(at, "'&' is only allowed in a call argument");
        if (!a.expr && !a.braceList)
            fail(at, "a void value cannot be an argument");
    }
    switch (type.kind) {
        case Kind::Struct: {
            if (const HostClassSpec* spec = hostClassNamed(type.structName); spec && !structs_.count(type.structName)) {
                if (!spec->hasConstructor)
                    fail(at, "'" + toUtf8(spec->name) + "' has no constructor");
                const HostClassSpec::Member* ctor = nullptr;
                for (const HostClassSpec::Member& m : spec->members) {
                    if (m.isConstructor && m.params.size() == args.size())
                        ctor = &m;
                }
                if (!ctor)
                    fail(at, "no constructor of '" + toUtf8(spec->name) + "' takes " +
                                 std::to_string(args.size()) + " arguments");
                auto* create = node<T::ObjectCreationExpr>(at, col);
                create->typeRef = typeRef(type, at);
                for (std::size_t i = 0; i < args.size(); ++i) {
                    T::Argument a;
                    a.value = coerce(args[i], ctor->params[i]);
                    create->args.push_back(a);
                }
                result.expr = create;
                return result;
            }
            if (type.structName.substr(0, 7) == u"System.") {
                // An exception: `std::runtime_error("message")`.
                auto* create = node<T::ObjectCreationExpr>(at, col);
                create->typeRef = typeRef(type, at);
                if (args.size() > 1)
                    fail(at, "an exception takes one message");
                for (Typed& a : args) {
                    T::Argument arg;
                    arg.value = coerce(a, CType::of(Kind::Str));
                    create->args.push_back(arg);
                }
                result.expr = create;
                return result;
            }
            auto it = structs_.find(type.structName);
            if (it == structs_.end())
                fail(at, "unknown class '" + toUtf8(type.structName) + "'");
            const ClassInfo& info = it->second;
            if (info.isAbstract)
                fail(at, "'" + toUtf8(info.name) + "' is abstract and cannot be constructed");
            if (args.size() == 1 && args[0].type.kind == Kind::Struct && args[0].type.same(type)) {
                result.expr = args[0].expr;  // a copy shares the object; see the docs
                return result;
            }
            auto* create = node<T::ObjectCreationExpr>(at, col);
            create->typeRef = typeRef(type, at);
            if (info.ctors.empty()) {
                // An aggregate: the arguments initialise the fields in order.
                std::size_t next = 0;
                for (Typed& a : args) {
                    while (next < info.fields.size() && info.fields[next].isStatic)
                        ++next;
                    if (next >= info.fields.size())
                        fail(at, "too many initializers for '" + toUtf8(info.name) + "'");
                    T::InitializerEntry entry;
                    entry.name = info.fields[next].name;
                    entry.value = coerce(a, info.fields[next].type);
                    create->initializers.push_back(std::move(entry));
                    ++next;
                }
                result.expr = create;
                return result;
            }
            const FuncInfo& ctor = pickOverload(info.ctors, args, toUtf8(info.name), at);
            checkAccess(info, ctor.access, info.name, at);
            bindArguments(ctor, args, create->args, toUtf8(info.name), at);
            result.expr = create;
            return result;
        }
        case Kind::Vector: {
            const CType& elem = *type.elem;
            if (args.empty()) {
                result.expr = containerCreation(type, at);
                return result;
            }
            if (args.size() == 1 && args[0].type.same(type)) {
                result.expr = args[0].expr;  // a copy shares the list; see the docs
                return result;
            }
            if (braced) {
                // `{1, 2, 3}`, or `{{1, 2}, {3, 4}}` with each entry an element.
                auto* create = node<T::ObjectCreationExpr>(at, col);
                create->typeRef = typeRef(type, at);
                for (Typed& a : args)
                    create->elements.push_back(coerce(a, elem));
                result.expr = create;
                return result;
            }
            if (args.size() > 2)
                fail(at, "std::vector takes (count) or (count, value)");
            Expr* count = coerce(args[0], CType::of(Kind::Int));
            result.expr = vectorFill(type, count, args.size() == 2 ? &args[1] : nullptr, at);
            return result;
        }
        case Kind::Map: {
            if (args.empty()) {
                result.expr = containerCreation(type, at);
                return result;
            }
            if (args.size() == 1 && args[0].type.same(type)) {
                result.expr = args[0].expr;
                return result;
            }
            auto* create = node<T::ObjectCreationExpr>(at, col);
            create->typeRef = typeRef(type, at);
            for (Typed& a : args) {
                if (!a.braceList || a.braceList->size() != 2)
                    fail(at, "a std::map is initialised with {key, value} entries");
                T::InitializerEntry entry;
                entry.key = coerce((*a.braceList)[0], *type.elem);
                entry.value = coerce((*a.braceList)[1], *type.elem2);
                create->initializers.push_back(std::move(entry));
            }
            result.expr = create;
            return result;
        }
        case Kind::Str: {
            if (args.empty()) {
                result.expr = stringLiteral(String(), at);
                return result;
            }
            if (args.size() == 1 && (args[0].type.kind == Kind::Str || args[0].type.kind == Kind::Null)) {
                result.expr = coerce(args[0], type);
                return result;
            }
            if (args.size() == 2 && args[0].type.isIntegral() && args[1].type.kind == Kind::Char) {
                // std::string(n, c)
                result.expr = invoke(libcRef("__repeat", at),
                                     {coerce(args[0], CType::of(Kind::Int)), coerce(args[1], CType::of(Kind::Char))},
                                     at, col);
                return result;
            }
            fail(at, "std::string is constructed from a string, or from (count, character)");
        }
        case Kind::Void:
        case Kind::Array:
        case Kind::Pair:
        case Kind::Null: fail(at, "cannot construct a value of type " + describe(type));
        default: {
            // A scalar: `int(x)`, `double(n)`, `bool(p)`.
            if (args.size() != 1)
                fail(at, describe(type) + "(...) takes one argument");
            Typed& v = args[0];
            if (type.kind == Kind::Bool) {
                result.expr = condition(v);
                return result;
            }
            if (v.type.kind == Kind::Bool)
                v.expr = coerce(v, CType::of(Kind::Int)), v.type = CType::of(Kind::Int);
            if (!v.type.isNumeric())
                fail(at, "cannot convert " + describe(v.type) + " to " + describe(type));
            result.expr = v.type.same(type) ? v.expr : castTo(v.expr, type, at);
            return result;
        }
    }
}

Parser::Typed Parser::functionalCast(const CType& type, std::uint32_t at, std::uint32_t col)
{
    if (!is(Tok::LParen) && !is(Tok::LBrace))
        fail(at, "expected '(' after the type");
    std::vector<Typed> args = arguments();
    return construct(type, args, at, col);
}

/// `new T(args)`, `new T`, `new T[n]`.
Parser::Typed Parser::newExpression()
{
    const std::uint32_t at = line();
    const std::uint32_t col = column();
    expect(Tok::New, "'new'");
    Specifiers spec;
    if (is(Tok::Identifier) && hostClassNamed(peek().text) && !structs_.count(peek().text)) {
        spec.base = CType::structOf(advance().text);  // `new Vec2(1, 2)` on a host class
        spec.any = true;
    } else {
        spec = specifiers();
    }
    if (!spec.any || spec.isAuto)
        fail(at, "expected a type after 'new'");
    while (eat(Tok::Star)) {
    }
    if (eat(Tok::LBracket)) {
        Typed n = expression();
        expect(Tok::RBracket, "']'");
        if (is(Tok::LBracket))
            fail(at, "'new T[a][b]' is not supported; make a vector of vectors");
        if (is(Tok::LBrace))
            fail(at, "'new T[n]{...}' is not supported; assign the elements");
        Typed result;
        result.type = CType::arrayOf(spec.base);
        result.expr = arrayInitializer(result.type, coerce(n, CType::of(Kind::Int)), at);
        return result;
    }
    std::vector<Typed> args;
    if (is(Tok::LParen) || is(Tok::LBrace))
        args = arguments();
    else
        lastArgsBraced_ = false;
    return construct(spec.base, args, at, col);
}

Parser::Stmt* Parser::deleteStatement()
{
    const std::uint32_t at = line();
    expect(Tok::Delete, "'delete'");
    if (eat(Tok::LBracket))
        expect(Tok::RBracket, "']'");
    Typed target = expression();
    expect(Tok::Semicolon, "';'");
    if (!target.expr || !target.type.isReference())
        fail(at, "'delete' needs a pointer");
    // Nothing to free: the collector owns the memory. Keep any side effect.
    if (cloneSimple(target.expr))
        return node<T::EmptyStmt>(at);
    return exprStatement(target.expr, at);
}

// -------------------------------------------------------------- exceptions ---

const char* Parser::exceptionTypeFor(const String& name) const
{
    struct Known
    {
        const char* cpp;
        const char* engine;
    };
    static constexpr Known known[] = {
        {"exception", "System.Exception"},
        {"runtime_error", "System.InvalidOperationException"},
        {"logic_error", "System.InvalidOperationException"},
        {"invalid_argument", "System.ArgumentException"},
        {"out_of_range", "System.ArgumentOutOfRangeException"},
        {"overflow_error", "System.OverflowException"},
        {"domain_error", "System.ArgumentOutOfRangeException"},
        {"length_error", "System.ArgumentOutOfRangeException"},
        {"range_error", "System.ArgumentOutOfRangeException"},
        {"bad_alloc", "System.OutOfMemoryException"},
    };
    const std::string ascii8 = toUtf8(name);
    for (const Known& k : known) {
        if (ascii8 == k.cpp)
            return k.engine;
    }
    return nullptr;
}

Parser::Stmt* Parser::tryStatement()
{
    auto* t = node<T::TryStmt>(line());
    expect(Tok::Try, "'try'");
    t->body = block();
    while (is(Tok::Catch)) {
        T::CatchClause clause;
        clause.line = line();
        advance();
        expect(Tok::LParen, "'('");
        if (!eat(Tok::Ellipsis)) {
            Specifiers spec = specifiers();
            if (!spec.any || spec.base.kind != Kind::Struct || spec.base.structName.substr(0, 7) != u"System.")
                fail(clause.line, "catch needs a std::exception type or '...'");
            Declarator d = declarator(spec.base, /*allowAbstract=*/true);
            clause.typeRef = typeRef(spec.base, clause.line);
            clause.name = d.name;
        }
        expect(Tok::RParen, "')'");
        pushScope();
        if (!clause.name.empty()) {
            Symbol s;
            s.type = CType::structOf(static_cast<T::TypeRef*>(clause.typeRef)->path.empty()
                                         ? String()
                                         : String());
            // The symbol's type is the exception class; rebuild it from the clause.
            String full;
            for (std::size_t i = 0; i < clause.typeRef->path.size(); ++i)
                full += (i ? u"." : u"") + clause.typeRef->path[i];
            s.type = CType::structOf(full);
            declare(clause.name, s, clause.line);
        }
        clause.body = block();
        popScope();
        t->catches.push_back(std::move(clause));
    }
    if (t->catches.empty())
        fail(t->line, "'try' needs at least one 'catch'");
    return t;
}

Parser::Stmt* Parser::throwStatement()
{
    auto* t = node<T::ThrowStmt>(line());
    expect(Tok::Throw, "'throw'");
    if (eat(Tok::Semicolon))
        return t;  // rethrow
    Typed v = expression();
    expect(Tok::Semicolon, "';'");
    if (!v.expr)
        fail(t->line, "'throw' needs an exception");
    if (v.type.kind == Kind::Struct && v.type.structName.substr(0, 7) == u"System.") {
        t->value = v.expr;
    } else if (v.type.kind == Kind::Str) {
        // `throw "message"`: wrapped, so a `catch (const std::exception&)` sees it.
        std::vector<Typed> args{v};
        t->value = construct(CType::structOf(ascii("System.Exception")), args, t->line, 0).expr;
    } else {
        fail(t->line, "throw a std::exception type, such as std::runtime_error(\"message\")");
    }
    return t;
}

// --------------------------------------------------------------- range-for ---

/// `for (T x : sequence) body`, positioned on the `:`.
Parser::Stmt* Parser::rangeFor(const Specifiers& spec, Declarator decl, std::uint32_t at)
{
    expect(Tok::Colon, "':'");
    Typed sequence = expression();
    expect(Tok::RParen, "')'");
    if (!sequence.expr)
        fail(at, "a range-for needs a sequence");
    if ((decl.pointers && decl.type.kind != Kind::Struct) || !decl.arrayLengths.empty())
        fail(decl.line, "the range-for variable cannot be a pointer or an array");
    CType element;
    switch (sequence.type.kind) {
        case Kind::Array:
        case Kind::Vector: element = *sequence.type.elem; break;
        case Kind::Str: element = CType::of(Kind::Char); break;
        case Kind::Map: element = CType::pairOf(*sequence.type.elem, *sequence.type.elem2); break;
        default: fail(at, "a range-for needs an array, a std::vector, a std::map or a std::string, not " +
                              describe(sequence.type));
    }
    const CType variable = spec.isAuto ? element : decl.type;
    if (!spec.isAuto && element.kind == Kind::Pair && variable.kind != Kind::Pair)
        fail(decl.line, "iterate a map with 'auto' or std::pair");
    auto* loop = node<T::ForEachStmt>(at);
    pushScope();
    Symbol s;
    s.type = variable;
    if (element.kind == Kind::Pair) {
        // foreach (K $k in m.Keys) with kv.first -> $k and kv.second -> m[$k].
        Expr* map = cloneSimple(sequence.expr);
        if (!map)
            fail(at, "store the map in a variable before iterating it");
        const String key = ascii(" k" + std::to_string(staticCounter_++));
        loop->typeRef = typeRef(*sequence.type.elem, at);
        loop->name = key;
        loop->sequence = memberRef(sequence.expr, "Keys", at);
        s.pairKey = key;
        s.pairMap = map;
    } else {
        loop->typeRef = typeRef(variable, at);
        loop->name = decl.name;
        loop->sequence = sequence.expr;
    }
    declare(decl.name, s, decl.line);
    loop->body = embedded();
    popScope();
    return loop;
}

// --------------------------------------------------------- namespaces, using ---

void Parser::namespaceBlock()
{
    expect(Tok::Namespace, "'namespace'");
    if (is(Tok::Identifier)) {
        namespaces_.insert(advance().text);
        while (eat(Tok::ColonColon))
            namespaces_.insert(expectName("a namespace name"));
    }
    if (eat(Tok::Assign)) {
        // namespace alias
        while (!is(Tok::Semicolon) && !is(Tok::Eof))
            advance();
        expect(Tok::Semicolon, "';'");
        return;
    }
    // Names inside are global here: `N::f()` drops the qualifier.
    expect(Tok::LBrace, "'{'");
    while (!is(Tok::RBrace)) {
        if (is(Tok::Eof))
            fail("unterminated namespace");
        external();
    }
    advance();
}

void Parser::usingDirective()
{
    const std::uint32_t at = line();
    expect(Tok::Using, "'using'");
    if (eat(Tok::Namespace)) {
        skipQualifier();
        namespaces_.insert(expectName("a namespace name"));
        while (eat(Tok::ColonColon))
            namespaces_.insert(expectName("a namespace name"));
        expect(Tok::Semicolon, "';'");
        return;
    }
    if (is(Tok::Identifier) && type(1) == Tok::Assign) {
        // `using Name = type;` is a typedef.
        const String name = advance().text;
        advance();
        Specifiers spec = specifiers();
        if (!spec.any || spec.isAuto)
            fail(at, "expected a type");
        Declarator d = declarator(spec.base, /*allowAbstract=*/true);
        if (!d.name.empty() || d.isFunction)
            fail(at, "a type alias names a type");
        typedefs_[name] = d.type;
        expect(Tok::Semicolon, "';'");
        return;
    }
    // `using std::cout;` and friends: everything is in scope already.
    while (!is(Tok::Semicolon) && !is(Tok::Eof))
        advance();
    expect(Tok::Semicolon, "';'");
}

bool Parser::skipQualifier()
{
    bool skipped = false;
    while (is(Tok::Identifier) && type(1) == Tok::ColonColon && type(2) == Tok::Identifier &&
           namespaces_.count(peek().text) && !structs_.count(peek().text)) {
        advance();
        advance();
        skipped = true;
    }
    return skipped;
}

// -------------------------------------------------------------- std types ---

void Parser::expectCloseAngle()
{
    if (eat(Tok::Gt))
        return;
    if (is(Tok::Shr)) {
        // `vector<vector<int>>`: the `>>` closes two lists.
        toks_[pos_].type = Tok::Gt;
        return;
    }
    fail(std::string("expected '>' but found ") + tokenName(type()));
}

std::vector<CType> Parser::templateArguments(std::size_t count, const char* what)
{
    if (!is(Tok::Lt))
        fail(std::string(what) + " needs its type arguments: " + what + "<...>");
    advance();
    std::vector<CType> out;
    for (std::size_t i = 0; i < count; ++i) {
        Specifiers spec = specifiers();
        if (!spec.any || spec.isAuto)
            fail("expected a type argument");
        Declarator d = declarator(spec.base, /*allowAbstract=*/true);
        if (!d.name.empty() || d.isReference || !d.arrayLengths.empty())
            fail("a type argument is a plain type");
        if (d.type.kind == Kind::Void)
            fail("a type argument cannot be void");
        out.push_back(d.type);
        if (i + 1 < count)
            expect(Tok::Comma, "','");
    }
    expectCloseAngle();
    return out;
}

/// Positioned on `string`, `vector`, `map`, `pair`, an exception name or a
/// class name (the `std::` already skipped).
CType Parser::typeName(std::uint32_t at)
{
    const String word = advance().text;
    if (word == u"string")
        return CType::of(Kind::Str);
    if (word == u"vector") {
        std::vector<CType> args = templateArguments(1, "std::vector");
        if (args[0].kind == Kind::Array)
            fail(at, "a std::vector of arrays is not supported; use a vector of vectors");
        return CType::vectorOf(args[0]);
    }
    if (word == u"map" || word == u"unordered_map") {
        std::vector<CType> args = templateArguments(2, "std::map");
        const CType& key = args[0];
        if (!(key.isNumeric() || key.kind == Kind::Str || key.kind == Kind::Bool))
            fail(at, "a std::map key must be a number, a character, a bool or a std::string");
        return CType::mapOf(key, args[1]);
    }
    if (word == u"pair") {
        std::vector<CType> args = templateArguments(2, "std::pair");
        return CType::pairOf(args[0], args[1]);
    }
    if (const char* engine = exceptionTypeFor(word))
        return CType::structOf(ascii(engine));
    if (structs_.count(word))
        return CType::structOf(word);
    fail(at, "unknown type '" + toUtf8(word) + "'");
}

String Parser::mangle(const CType& type) const
{
    switch (type.kind) {
        case Kind::Struct: return type.structName;
        case Kind::Array: return u"arr_" + mangle(*type.elem);
        case Kind::Vector: return u"vec_" + mangle(*type.elem);
        case Kind::Map: return u"map_" + mangle(*type.elem) + u"_" + mangle(*type.elem2);
        case Kind::Pair: return u"pair_" + mangle(*type.elem) + u"_" + mangle(*type.elem2);
        case Kind::Str: return ascii("string");
        case Kind::Bool: return ascii("bool");
        default: {
            std::string name = describe(type);
            for (char& c : name)
                if (c == ' ')
                    c = '_';
            return fromUtf8(name);
        }
    }
}

/// What a fresh element or a missing map value holds: C++'s value
/// initialization.
Parser::Expr* Parser::zeroValue(const CType& type, std::uint32_t at)
{
    switch (type.kind) {
        case Kind::Bool: return boolLiteral(false, at);
        case Kind::Str: return stringLiteral(String(), at);
        case Kind::Struct:
        case Kind::Vector:
        case Kind::Map: return defaultValue(type, nullptr, at);
        case Kind::Array:
        case Kind::Null: return nullLiteral(at);
        case Kind::Void:
        case Kind::Pair: fail(at, "no default value for " + describe(type));
        default: {
            Typed zero;
            zero.expr = intLiteral(0, at);
            zero.type = CType::of(Kind::Int);
            return coerce(zero, type);
        }
    }
}

Parser::Expr* Parser::containerCreation(const CType& type, std::uint32_t at)
{
    auto* create = node<T::ObjectCreationExpr>(at);
    create->typeRef = typeRef(type, at);
    return create;
}

/// `m[k]` read: `$mapget(m, k)` inserts a default first, as `operator[]` does.
Parser::Expr* Parser::mapGet(Typed map, Typed key, std::uint32_t at, std::uint32_t col)
{
    const String helperName = u"$mapget_" + mangle(map.type);
    if (!helpers_.count(helperName)) {
        helpers_[helperName] = true;
        // static V $mapget(Dictionary<K,V> m, K k) { if (!m.ContainsKey(k)) m[k] = <zero>; return m[k]; }
        T::MethodDecl* method = newHelper(helperName, *map.type.elem2, at);
        for (const char* pname : {"m", "k"}) {
            T::ParamDecl p;
            p.name = ascii(pname);
            p.line = at;
            p.typeRef = typeRef(std::strcmp(pname, "m") == 0 ? map.type : *map.type.elem, at);
            method->params.push_back(std::move(p));
        }
        auto* body = node<T::BlockStmt>(at);
        auto* test = node<T::IfStmt>(at);
        auto* has = node<T::UnaryExpr>(at);
        has->op = UnaryOp::Not;
        has->operand = invoke(memberRef(nameRef(ascii("m"), at), "ContainsKey", at), {nameRef(ascii("k"), at)}, at);
        test->condition = has;
        auto* assign = node<T::AssignmentExpr>(at);
        auto* slot = node<T::ElementAccessExpr>(at);
        slot->target = nameRef(ascii("m"), at);
        slot->index = nameRef(ascii("k"), at);
        assign->target = slot;
        assign->value = zeroValue(*map.type.elem2, at);
        test->thenBranch = exprStatement(assign, at);
        body->statements.push_back(test);
        auto* ret = node<T::ReturnStmt>(at);
        auto* read = node<T::ElementAccessExpr>(at);
        read->target = nameRef(ascii("m"), at);
        read->index = nameRef(ascii("k"), at);
        ret->value = read;
        body->statements.push_back(ret);
        method->body = body;
    }
    return invoke(globalRef(helperName, at, col), {map.expr, key.expr}, at, col);
}

/// `m[k] op= v`: `static V $mapadd(Dictionary<K,V> m, K k, V v) { m[k] = $mapget(m, k) op v; return m[k]; }`
Parser::Typed Parser::mapUpdate(Expr* map, Expr* key, const CType& mapType, BinaryOp op, Typed value,
                                std::uint32_t at, std::uint32_t col)
{
    const CType& valueType = *mapType.elem2;
    if (valueType.kind == Kind::Str) {
        if (op != BinaryOp::Add || (value.type.kind != Kind::Str && value.type.kind != Kind::Char))
            fail(at, "a std::string map value can only be appended to");
    } else if (!valueType.isNumeric()) {
        fail(at, "compound assignment needs a numeric or string map value");
    }
    const String helperName = u"$map" + fromUtf8(T::operatorText(op)) + u"_" + mangle(mapType);
    if (!helpers_.count(helperName)) {
        helpers_[helperName] = true;
        T::MethodDecl* method = newHelper(helperName, valueType, at);
        for (const char* pname : {"m", "k", "v"}) {
            T::ParamDecl p;
            p.name = ascii(pname);
            p.line = at;
            p.typeRef = typeRef(pname[0] == 'm' ? mapType : pname[0] == 'k' ? *mapType.elem : valueType, at);
            method->params.push_back(std::move(p));
        }
        auto* body = node<T::BlockStmt>(at);
        Typed current;
        current.type = valueType;
        Typed m;
        m.expr = nameRef(ascii("m"), at);
        m.type = mapType;
        Typed k;
        k.expr = nameRef(ascii("k"), at);
        k.type = *mapType.elem;
        current.expr = mapGet(m, k, at, 0);
        Typed v;
        v.expr = nameRef(ascii("v"), at);
        v.type = valueType;
        Typed combined = makeBinary(op, current, v, at, 0);
        auto* assign = node<T::AssignmentExpr>(at);
        auto* slot = node<T::ElementAccessExpr>(at);
        slot->target = nameRef(ascii("m"), at);
        slot->index = nameRef(ascii("k"), at);
        assign->target = slot;
        assign->value = coerce(combined, valueType);
        body->statements.push_back(exprStatement(assign, at));
        auto* ret = node<T::ReturnStmt>(at);
        auto* read = node<T::ElementAccessExpr>(at);
        read->target = nameRef(ascii("m"), at);
        read->index = nameRef(ascii("k"), at);
        ret->value = read;
        body->statements.push_back(ret);
        method->body = body;
    }
    Typed result;
    result.type = valueType;
    Expr* delta = valueType.kind == Kind::Str && value.type.kind == Kind::Char
                      ? invoke(libcRef("__chr", at), {value.expr}, at)
                      : coerce(value, valueType);
    result.expr = invoke(globalRef(helperName, at, col), {map, key, delta}, at, col);
    return result;
}

/// `std::vector<T>(n)` and `(n, x)`: a helper that adds `n` elements. A
/// reference-typed element is made fresh per slot, so rows never alias;
/// `vector<vector<T>>(n, vector<T>(m[, x]))` nests one level the same way.
Parser::Expr* Parser::vectorFill(const CType& type, Expr* count, Typed* value, std::uint32_t at)
{
    const CType& elem = *type.elem;
    if (value && elem.isReference() && elem.kind != Kind::Str) {
        // The value has to be created per element. Recognise a nested vector
        // construction and rebuild it inside the loop.
        auto* inner = value->expr && value->expr->kind == ExprKind::Invocation
                          ? static_cast<T::InvocationExpr*>(value->expr)
                          : nullptr;
        String innerName;
        if (inner && inner->callee->kind == ExprKind::Name)
            innerName = static_cast<T::NameExpr*>(inner->callee)->name;
        else if (inner && inner->callee->kind == ExprKind::MemberAccess)
            innerName = static_cast<T::MemberAccessExpr*>(inner->callee)->name;
        if (elem.kind != Kind::Vector || innerName.substr(0, 4) != u"$vec")
            fail(at, "a std::vector of " + describe(elem) +
                         " cannot be filled with one shared value; add the elements in a loop");
        const String helperName = u"$vec2_" + mangle(elem) + u"_" + ascii(std::to_string(inner->args.size()));
        if (!helpers_.count(helperName)) {
            helpers_[helperName] = true;
            // static List<List<T>> $vec2(int n, int m[, T x]) { var a = new List<List<T>>(); for (...) a.Add($vec(m[, x])); return a; }
            T::MethodDecl* method = newHelper(helperName, type, at);
            T::ParamDecl n;
            n.name = ascii("n");
            n.line = at;
            n.typeRef = typeRef(CType::of(Kind::Int), at);
            method->params.push_back(std::move(n));
            std::vector<Expr*> innerArgs;
            for (std::size_t i = 0; i < inner->args.size(); ++i) {
                T::ParamDecl p;
                p.name = ascii("p" + std::to_string(i));
                p.line = at;
                p.typeRef = typeRef(i == 0 ? CType::of(Kind::Int) : *elem.elem, at);
                method->params.push_back(std::move(p));
                innerArgs.push_back(nameRef(ascii("p" + std::to_string(i)), at));
            }
            auto* body = node<T::BlockStmt>(at);
            auto* declStmt = node<T::LocalDeclStmt>(at);
            declStmt->typeRef = typeRef(type, at);
            T::VarDeclarator var;
            var.name = ascii("a");
            var.line = at;
            var.init = containerCreation(type, at);
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
            loop->body = exprStatement(
                invoke(memberRef(nameRef(ascii("a"), at), "Add", at), {invoke(nameRef(innerName, at), innerArgs, at)}, at),
                at);
            body->statements.push_back(loop);
            auto* ret = node<T::ReturnStmt>(at);
            ret->value = nameRef(ascii("a"), at);
            body->statements.push_back(ret);
            method->body = body;
        }
        std::vector<Expr*> args{count};
        for (const T::Argument& a : inner->args)
            args.push_back(a.value);
        return invoke(globalRef(helperName, at), args, at);
    }
    const String helperName = (value ? u"$vecfill_" : u"$vecnew_") + mangle(elem);
    if (!helpers_.count(helperName)) {
        helpers_[helperName] = true;
        // static List<T> $vecfill(int n, T x) { var a = new List<T>(); for (int i = 0; i < n; i++) a.Add(x); return a; }
        T::MethodDecl* method = newHelper(helperName, type, at);
        T::ParamDecl n;
        n.name = ascii("n");
        n.line = at;
        n.typeRef = typeRef(CType::of(Kind::Int), at);
        method->params.push_back(std::move(n));
        if (value) {
            T::ParamDecl x;
            x.name = ascii("x");
            x.line = at;
            x.typeRef = typeRef(elem, at);
            method->params.push_back(std::move(x));
        }
        auto* body = node<T::BlockStmt>(at);
        auto* declStmt = node<T::LocalDeclStmt>(at);
        declStmt->typeRef = typeRef(type, at);
        T::VarDeclarator var;
        var.name = ascii("a");
        var.line = at;
        var.init = containerCreation(type, at);
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
        loop->body = exprStatement(invoke(memberRef(nameRef(ascii("a"), at), "Add", at),
                                          {value ? nameRef(ascii("x"), at) : zeroValue(elem, at)}, at),
                                   at);
        body->statements.push_back(loop);
        auto* ret = node<T::ReturnStmt>(at);
        ret->value = nameRef(ascii("a"), at);
        body->statements.push_back(ret);
        method->body = body;
    }
    std::vector<Expr*> args{count};
    if (value)
        args.push_back(coerce(*value, elem));
    return invoke(globalRef(helperName, at), args, at);
}

Parser::Expr* Parser::countOf(Typed& container, std::uint32_t at, std::uint32_t col)
{
    return memberRef(container.expr, "Count", at + 0 * col);
}

// --------------------------------------------------------- std members ---

Parser::Typed Parser::stringMember(Typed base, const String& name, std::uint32_t at, std::uint32_t col)
{
    const std::string ascii8 = toUtf8(name);
    const bool call = is(Tok::LParen);
    std::vector<Typed> args;
    if (call)
        args = arguments();
    auto need = [&](std::size_t n, const char* shape) {
        if (!call || args.size() != n)
            fail(at, "std::string::" + ascii8 + " is called as " + shape);
    };
    auto count = [&] { return memberRef(base.expr, "Length", at); };
    Typed result;
    if (ascii8 == "size" || ascii8 == "length") {
        need(0, "s.size()");
        result.expr = count();
        result.type = CType::of(Kind::Int);
        return result;
    }
    if (ascii8 == "empty") {
        need(0, "s.empty()");
        auto* b = node<T::BinaryExpr>(at, col);
        b->op = BinaryOp::Equal;
        b->left = count();
        b->right = intLiteral(0, at);
        result.expr = b;
        result.type = CType::of(Kind::Bool);
        return result;
    }
    if (ascii8 == "substr") {
        if (!call || args.empty() || args.size() > 2)
            fail(at, "std::string::substr is called as s.substr(pos) or s.substr(pos, count)");
        std::vector<Expr*> a{base.expr, coerce(args[0], CType::of(Kind::Int))};
        if (args.size() == 2)
            a.push_back(coerce(args[1], CType::of(Kind::Int)));
        result.expr = invoke(libcRef("__substr", at), a, at, col);
        result.type = CType::of(Kind::Str);
        return result;
    }
    if (ascii8 == "find" || ascii8 == "rfind") {
        need(1, "s.find(text)");
        if (args[0].type.kind != Kind::Str && args[0].type.kind != Kind::Char)
            fail(at, "std::string::find takes a string or a character");
        result.expr = invoke(memberRef(base.expr, ascii8 == "find" ? "IndexOf" : "LastIndexOf", at),
                             {ascii8 == "find" ? args[0].expr : coerce(args[0], CType::of(Kind::Str))}, at, col);
        result.type = CType::of(Kind::Int);
        return result;
    }
    if (ascii8 == "at" || ascii8 == "front" || ascii8 == "back") {
        Expr* index = nullptr;
        if (ascii8 == "at") {
            need(1, "s.at(i)");
            index = coerce(args[0], CType::of(Kind::Int));
        } else {
            need(0, ("s." + ascii8 + "()").c_str());
            if (ascii8 == "front") {
                index = intLiteral(0, at);
            } else {
                Expr* copy = cloneSimple(base.expr);
                if (!copy)
                    fail(at, "store the string in a variable before calling back()");
                auto* last = node<T::BinaryExpr>(at, col);
                last->op = BinaryOp::Subtract;
                last->left = memberRef(copy, "Length", at);
                last->right = intLiteral(1, at);
                index = last;
            }
        }
        result.expr = invoke(libcRef("__at", at), {base.expr, index}, at, col);
        result.type = CType::of(Kind::Char);
        return result;
    }
    if (ascii8 == "c_str" || ascii8 == "data") {
        need(0, "s.c_str()");
        result.expr = base.expr;
        result.type = CType::of(Kind::Str);
        return result;
    }
    if (ascii8 == "compare") {
        need(1, "s.compare(other)");
        result.expr = invoke(memberRef(base.expr, "CompareTo", at), {coerce(args[0], CType::of(Kind::Str))}, at, col);
        result.type = CType::of(Kind::Int);
        return result;
    }
    if (ascii8 == "starts_with" || ascii8 == "ends_with" || ascii8 == "contains") {
        need(1, ("s." + ascii8 + "(text)").c_str());
        const char* method = ascii8 == "starts_with" ? "StartsWith" : ascii8 == "ends_with" ? "EndsWith" : "Contains";
        result.expr = invoke(memberRef(base.expr, method, at), {coerce(args[0], CType::of(Kind::Str))}, at, col);
        result.type = CType::of(Kind::Bool);
        return result;
    }
    if (ascii8 == "append" || ascii8 == "push_back" || ascii8 == "clear") {
        // Strings are immutable: these rebind the variable.
        if (!isAssignableExpr(base.expr))
            fail(at, "std::string::" + ascii8 + " needs a variable to modify");
        if (ascii8 == "clear") {
            need(0, "s.clear()");
            auto* assign = node<T::AssignmentExpr>(at, col);
            assign->target = base.expr;
            assign->value = stringLiteral(String(), at);
            result.expr = assign;
        } else {
            need(1, ("s." + ascii8 + "(x)").c_str());
            if (args[0].type.kind != Kind::Str && args[0].type.kind != Kind::Char)
                fail(at, "std::string::" + ascii8 + " takes a string or a character");
            result = compound(base, BinaryOp::Add, args[0], at, col);
        }
        result.type = CType::of(Kind::Str);
        return result;
    }
    if (ascii8 == "begin" || ascii8 == "end")
        fail(at, "iterators are not supported; index the string or use a range-for");
    fail(at, "std::string has no member '" + ascii8 + "'");
}

Parser::Typed Parser::vectorMember(Typed base, const String& name, std::uint32_t at, std::uint32_t col)
{
    const std::string ascii8 = toUtf8(name);
    const CType elem = *base.type.elem;
    const bool call = is(Tok::LParen);
    std::vector<Typed> args;
    if (call)
        args = arguments();
    auto need = [&](std::size_t n, const char* shape) {
        if (!call || args.size() != n)
            fail(at, "std::vector::" + ascii8 + " is called as " + shape);
    };
    auto copy = [&] {
        Expr* c = cloneSimple(base.expr);
        if (!c)
            fail(at, "store the vector in a variable before calling " + ascii8 + "()");
        return c;
    };
    auto lastIndex = [&] {
        auto* last = node<T::BinaryExpr>(at, col);
        last->op = BinaryOp::Subtract;
        last->left = memberRef(copy(), "Count", at);
        last->right = intLiteral(1, at);
        return last;
    };
    Typed result;
    if (ascii8 == "size") {
        need(0, "v.size()");
        result.expr = countOf(base, at, col);
        result.type = CType::of(Kind::Int);
        return result;
    }
    if (ascii8 == "empty") {
        need(0, "v.empty()");
        auto* b = node<T::BinaryExpr>(at, col);
        b->op = BinaryOp::Equal;
        b->left = countOf(base, at, col);
        b->right = intLiteral(0, at);
        result.expr = b;
        result.type = CType::of(Kind::Bool);
        return result;
    }
    if (ascii8 == "push_back" || ascii8 == "emplace_back") {
        need(1, "v.push_back(x)");
        result.expr = invoke(memberRef(base.expr, "Add", at), {coerce(args[0], elem)}, at, col);
        result.type = CType::of(Kind::Void);
        return result;
    }
    if (ascii8 == "pop_back") {
        need(0, "v.pop_back()");
        result.expr = invoke(memberRef(base.expr, "RemoveAt", at), {lastIndex()}, at, col);
        result.type = CType::of(Kind::Void);
        return result;
    }
    if (ascii8 == "clear") {
        need(0, "v.clear()");
        result.expr = invoke(memberRef(base.expr, "Clear", at), {}, at, col);
        result.type = CType::of(Kind::Void);
        return result;
    }
    if (ascii8 == "at" || ascii8 == "front" || ascii8 == "back") {
        auto* e = node<T::ElementAccessExpr>(at, col);
        e->target = base.expr;
        if (ascii8 == "at") {
            need(1, "v.at(i)");
            e->index = coerce(args[0], CType::of(Kind::Int));
        } else {
            need(0, ("v." + ascii8 + "()").c_str());
            e->index = ascii8 == "front" ? intLiteral(0, at) : lastIndex();
        }
        result.expr = e;
        result.type = elem;
        return result;
    }
    if (ascii8 == "reserve" || ascii8 == "shrink_to_fit") {
        if (call)
            for (Typed& a : args)
                coerce(a, CType::of(Kind::Int));
        result.type = CType::of(Kind::Void);  // nothing to do
        return result;
    }
    if (ascii8 == "begin" || ascii8 == "end" || ascii8 == "insert" || ascii8 == "erase" ||
        ascii8 == "rbegin" || ascii8 == "rend")
        fail(at, "iterators are not supported; index the vector or use a range-for");
    fail(at, "std::vector has no member '" + ascii8 + "'");
}

Parser::Typed Parser::mapMember(Typed base, const String& name, std::uint32_t at, std::uint32_t col)
{
    const std::string ascii8 = toUtf8(name);
    const bool call = is(Tok::LParen);
    std::vector<Typed> args;
    if (call)
        args = arguments();
    auto need = [&](std::size_t n, const char* shape) {
        if (!call || args.size() != n)
            fail(at, "std::map::" + ascii8 + " is called as " + shape);
    };
    Typed result;
    if (ascii8 == "size") {
        need(0, "m.size()");
        result.expr = countOf(base, at, col);
        result.type = CType::of(Kind::Int);
        return result;
    }
    if (ascii8 == "empty") {
        need(0, "m.empty()");
        auto* b = node<T::BinaryExpr>(at, col);
        b->op = BinaryOp::Equal;
        b->left = countOf(base, at, col);
        b->right = intLiteral(0, at);
        result.expr = b;
        result.type = CType::of(Kind::Bool);
        return result;
    }
    if (ascii8 == "clear") {
        need(0, "m.clear()");
        result.expr = invoke(memberRef(base.expr, "Clear", at), {}, at, col);
        result.type = CType::of(Kind::Void);
        return result;
    }
    if (ascii8 == "count" || ascii8 == "contains" || ascii8 == "erase") {
        need(1, ("m." + ascii8 + "(key)").c_str());
        const char* method = ascii8 == "erase" ? "Remove" : "ContainsKey";
        result.expr = invoke(memberRef(base.expr, method, at), {coerce(args[0], *base.type.elem)}, at, col);
        result.type = CType::of(Kind::Bool);
        return result;
    }
    if (ascii8 == "at") {
        need(1, "m.at(key)");
        auto* e = node<T::ElementAccessExpr>(at, col);
        e->target = base.expr;
        e->index = coerce(args[0], *base.type.elem);
        result.expr = e;  // throws on a missing key, as at() does
        result.type = *base.type.elem2;
        return result;
    }
    if (ascii8 == "find" || ascii8 == "begin" || ascii8 == "end")
        fail(at, "iterators are not supported; use m.count(key), m[key] or a range-for");
    if (ascii8 == "insert" || ascii8 == "emplace")
        fail(at, "std::map::" + ascii8 + " is not supported; write m[key] = value");
    fail(at, "std::map has no member '" + ascii8 + "'");
}

Parser::Typed Parser::pairMember(Typed base, const String& name, std::uint32_t at, std::uint32_t col)
{
    if (base.pairKey.empty())
        fail(at, "a std::pair is only available as a range-for variable over a map");
    Typed result;
    if (name == u"first") {
        result.expr = nameRef(base.pairKey, at, col);
        result.type = *base.type.elem;
        return result;
    }
    if (name == u"second") {
        auto* e = node<T::ElementAccessExpr>(at, col);
        e->target = cloneSimple(base.pairMap);
        e->index = nameRef(base.pairKey, at, col);
        result.expr = e;
        result.type = *base.type.elem2;
        return result;
    }
    fail(at, "a std::pair has 'first' and 'second'");
}

/// The handful of `std::` functions that are not `libc` names: `to_string`,
/// `stoi` and friends, `min`, `max`, `abs`, `swap`. Positioned after the
/// name; `handled` says whether the name was one of them.
Parser::Typed Parser::stdFunction(const String& name, std::uint32_t at, std::uint32_t col, bool& handled)
{
    const std::string ascii8 = toUtf8(name);
    handled = false;
    Typed result;
    if (!is(Tok::LParen))
        return result;
    static const char* const names[] = {"to_string", "stoi", "stol", "stoll", "stod", "stof",
                                        "min", "max", "abs", "swap", "sort", "getline", nullptr};
    bool known = false;
    for (const char* const* n = names; *n; ++n)
        known = known || ascii8 == *n;
    if (!known)
        return result;
    handled = true;
    if (ascii8 == "sort")
        fail(at, "std::sort is not supported; sort with a loop");
    if (ascii8 == "getline")
        fail(at, "std::getline is not supported: there is no input stream");
    std::vector<Typed> args = arguments();
    if (ascii8 == "to_string") {
        if (args.size() != 1 || !args[0].expr)
            fail(at, "std::to_string takes one value");
        Typed& v = args[0];
        if (v.type.kind == Kind::Bool || v.type.kind == Kind::Char)
            v.expr = coerce(v, CType::of(Kind::Int)), v.type = CType::of(Kind::Int);
        if (!v.type.isNumeric())
            fail(at, "std::to_string takes a number");
        result.expr = invoke(memberRef(v.expr, "ToString", at), {}, at, col);
        result.type = CType::of(Kind::Str);
        return result;
    }
    if (ascii8 == "stoi" || ascii8 == "stol" || ascii8 == "stoll" || ascii8 == "stod" || ascii8 == "stof") {
        if (args.size() != 1)
            fail(at, "std::" + ascii8 + " takes one string");
        const char* libc = ascii8 == "stoi" ? "atoi" : (ascii8 == "stod" || ascii8 == "stof") ? "atof" : "atol";
        return libcCall(ascii(libc), args, at, col);
    }
    if (ascii8 == "min" || ascii8 == "max") {
        if (args.size() != 2 || !args[0].expr || !args[1].expr)
            fail(at, "std::" + ascii8 + " takes two values");
        for (Typed& a : args) {
            if (a.type.kind == Kind::Bool)
                a.expr = coerce(a, CType::of(Kind::Int)), a.type = CType::of(Kind::Int);
            if (!a.type.isNumeric())
                fail(at, "std::" + ascii8 + " takes numbers");
        }
        const CType promoted = promote(args[0].type, args[1].type);
        // Math.Min/Max exist for int, long and double.
        const CType math = promoted.isFloating() ? CType::of(Kind::Double)
                           : promoted.kind == Kind::Long || promoted.kind == Kind::ULong ? CType::of(Kind::Long)
                                                                                        : CType::of(Kind::Int);
        result.expr = invoke(memberRef(nameRef(ascii("Math"), at, col), ascii8 == "min" ? "Min" : "Max", at),
                             {coerce(args[0], math), coerce(args[1], math)}, at, col);
        result.type = math;
        if (promoted.kind == Kind::Float) {
            result.expr = castTo(result.expr, promoted, at);
            result.type = promoted;
        }
        return result;
    }
    if (ascii8 == "abs") {
        if (args.size() != 1 || !args[0].expr || !args[0].type.isNumeric())
            fail(at, "std::abs takes one number");
        const CType& t = args[0].type;
        const CType math = t.isFloating() ? CType::of(Kind::Double)
                           : t.kind == Kind::Long || t.kind == Kind::ULong ? CType::of(Kind::Long)
                                                                          : CType::of(Kind::Int);
        result.expr = invoke(memberRef(nameRef(ascii("Math"), at, col), "Abs", at), {coerce(args[0], math)}, at, col);
        result.type = math;
        if (t.kind == Kind::Float) {
            result.expr = castTo(result.expr, t, at);
            result.type = t;
        }
        return result;
    }
    // swap(a, b): a helper with two ref parameters.
    if (args.size() != 2 || !isAssignableExpr(args[0].expr) || !isAssignableExpr(args[1].expr) ||
        args[0].store || args[1].store)
        fail(at, "std::swap takes two variables, fields or elements");
    if (!args[0].type.same(args[1].type))
        fail(at, "std::swap needs two values of the same type");
    const CType& t = args[0].type;
    const String helperName = u"$swap_" + mangle(t);
    if (!helpers_.count(helperName)) {
        helpers_[helperName] = true;
        // static void $swap(ref T a, ref T b) { T t = a; a = b; b = t; }
        T::MethodDecl* method = newHelper(helperName, CType::of(Kind::Void), at);
        for (const char* pname : {"a", "b"}) {
            T::ParamDecl p;
            p.name = ascii(pname);
            p.line = at;
            p.typeRef = typeRef(t, at);
            p.mode = T::ParamMode::Ref;
            method->params.push_back(std::move(p));
        }
        auto* body = node<T::BlockStmt>(at);
        auto* declStmt = node<T::LocalDeclStmt>(at);
        declStmt->typeRef = typeRef(t, at);
        T::VarDeclarator var;
        var.name = ascii("t");
        var.line = at;
        var.init = nameRef(ascii("a"), at);
        declStmt->vars.push_back(std::move(var));
        body->statements.push_back(declStmt);
        auto* first = node<T::AssignmentExpr>(at);
        first->target = nameRef(ascii("a"), at);
        first->value = nameRef(ascii("b"), at);
        body->statements.push_back(exprStatement(first, at));
        auto* second = node<T::AssignmentExpr>(at);
        second->target = nameRef(ascii("b"), at);
        second->value = nameRef(ascii("t"), at);
        body->statements.push_back(exprStatement(second, at));
        method->body = body;
    }
    auto* call = node<T::InvocationExpr>(at, col);
    call->callee = globalRef(helperName, at, col);
    for (Typed& a : args) {
        T::Argument arg;
        arg.value = a.expr;
        arg.mode = T::ParamMode::Ref;
        call->args.push_back(arg);
    }
    result.expr = call;
    result.type = CType::of(Kind::Void);
    return result;
}

// ------------------------------------------------------------------ streams ---

/// `stream << value`: the first `<<` makes the `libc.__cout(which, items)`
/// call, each further one appends to its item list.
Parser::Typed Parser::streamOutput(Typed left, Typed right, std::uint32_t at, std::uint32_t col)
{
    if (right.addressOf || !right.expr)
        fail(at, "nothing to print");
    Expr* item = nullptr;
    switch (right.type.kind) {
        case Kind::Bool: item = coerce(right, CType::of(Kind::Int)); break;  // prints 1 or 0
        case Kind::Char: item = invoke(libcRef("__chr", at), {right.expr}, at); break;
        case Kind::Null: item = intLiteral(0, at); break;
        case Kind::Struct:
        case Kind::Vector:
        case Kind::Map:
        case Kind::Pair:
        case Kind::Array:
        case Kind::Void: fail(at, "cannot print a " + describe(right.type) + "; print its members");
        default: item = right.expr; break;
    }
    Typed result;
    result.stream = left.stream;
    result.type = CType::of(Kind::Void);
    if (!left.expr) {
        auto* items = node<T::ArrayCreationExpr>(at);
        TypeRef* object = arena_.make<TypeRef>();
        object->predefined = T::Tok::Object;
        object->line = at;
        items->elementTypeRef = object;
        items->hasInitializer = true;
        items->elements.push_back(item);
        result.expr = invoke(libcRef("__cout", at), {intLiteral(left.stream, at), items}, at, col);
        return result;
    }
    auto* call = static_cast<T::InvocationExpr*>(left.expr);
    static_cast<T::ArrayCreationExpr*>(call->args[1].value)->elements.push_back(item);
    result.expr = left.expr;
    return result;
}

// -------------------------------------------------------------------- casts ---

/// `static_cast<T>(e)`, `dynamic_cast<T*>(p)`, `const_cast<T>(e)`.
Parser::Typed Parser::castExpression(Tok kind, std::uint32_t at, std::uint32_t col)
{
    if (kind == Tok::ReinterpretCast)
        fail(at, "reinterpret_cast is not supported: there are no addresses");
    expect(Tok::Lt, "'<'");
    Specifiers spec = specifiers();
    if (!spec.any || spec.isAuto)
        fail(at, "expected a type");
    Declarator d = declarator(spec.base, /*allowAbstract=*/true);
    if (!d.name.empty())
        fail(at, "unexpected name in a cast");
    expectCloseAngle();
    expect(Tok::LParen, "'('");
    Typed operand = expression();
    expect(Tok::RParen, "')'");
    if (!operand.expr || operand.addressOf)
        fail(at, "invalid operand to the cast");
    const CType target = d.type;
    Typed result;
    result.type = target;
    if (kind == Tok::ConstCast) {
        result.expr = operand.expr;
        return result;
    }
    if (kind == Tok::DynamicCast) {
        if (target.kind != Kind::Struct || operand.type.kind != Kind::Struct)
            fail(at, "dynamic_cast converts between class pointers");
        auto* test = node<T::TypeTestExpr>(at, col);
        test->operand = operand.expr;
        test->typeRef = typeRef(target, at);
        test->isAs = true;
        result.expr = test;
        return result;
    }
    if (target.kind == Kind::Struct && operand.type.kind == Kind::Struct) {
        result.expr = isSubclass(operand.type.structName, target.structName) ? operand.expr
                                                                             : castTo(operand.expr, target, at);
        return result;
    }
    if (target.kind == Kind::Void) {
        result.expr = operand.expr;
        return result;
    }
    if (target.kind == Kind::Bool) {
        result.expr = condition(operand);
        return result;
    }
    if (operand.type.kind == Kind::Bool)
        operand.expr = coerce(operand, CType::of(Kind::Int)), operand.type = CType::of(Kind::Int);
    if (!target.isNumeric() || !operand.type.isNumeric())
        fail(at, "cannot cast " + describe(operand.type) + " to " + describe(target));
    result.expr = operand.type.same(target) ? operand.expr : castTo(operand.expr, target, at);
    return result;
}

/// `a < b` on strings: `a.CompareTo(b) < 0`.
Parser::Typed Parser::stringCompare(BinaryOp op, Typed left, Typed right, std::uint32_t at, std::uint32_t col)
{
    if (left.type.kind != Kind::Str || right.type.kind != Kind::Str)
        fail(at, "a std::string can only be compared with a std::string");
    auto* b = node<T::BinaryExpr>(at, col);
    b->op = op;
    b->left = invoke(memberRef(left.expr, "CompareTo", at), {right.expr}, at, col);
    b->right = intLiteral(0, at);
    Typed result;
    result.expr = b;
    result.type = CType::of(Kind::Bool);
    return result;
}

}  // namespace c
}  // namespace internal
}  // namespace script
