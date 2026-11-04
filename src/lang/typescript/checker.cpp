// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The type checker.
//
// One walk over the tree with a scope stack, a lazily resolved binding for
// every name and a stack of narrowing facts. Types come from `TypeArena`, so
// the checker itself allocates nothing that outlives the call.
//
// The rule it follows everywhere: when a construct is understood, check it; when
// it is not, produce `any` and stay quiet. A checker that blocks valid code is
// worse than one that misses an error.

#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <unordered_map>
#include <unordered_set>

#include "script/internal/dyn/checker.hpp"
#include "script/internal/dyn/numconv.hpp"
#include "script/internal/dyn/parser.hpp"

namespace script {
namespace internal {
namespace dyn {

namespace {

/// A value name. `pending` is the declaration whose type has not been worked
/// out yet: a forward reference resolves it on the spot rather than settling
/// for `any`, which is what makes mutually recursive declarations type.
struct ValueBinding
{
    const Ty* type = nullptr;
    const Node* pending = nullptr;
    bool isConst = false;
    bool resolving = false;
};

/// A type name, with its type parameters when it has any.
struct TypeBinding
{
    const Ty* type = nullptr;
    std::vector<String> params;
    std::vector<const Ty*> defaults;
    const Node* pending = nullptr;  ///< an alias body not yet resolved
    bool resolving = false;
};

struct Scope
{
    std::unordered_map<String, ValueBinding> values;
    std::unordered_map<String, TypeBinding> types;
};

using Facts = std::unordered_map<String, const Ty*>;
using Substitution = std::unordered_map<String, const Ty*>;

/// What a `return` inside the function being checked contributes.
struct FunctionContext
{
    const Ty* declaredReturn = nullptr;
    std::vector<const Ty*> returns;
    const Ty* thisType = nullptr;
    bool sawReturn = false;
};

/// True when control cannot fall out of the bottom of `n`. Knowing this is what
/// lets an early `return` narrow the statements that follow it.
bool alwaysExits(const Node& n)
{
    switch (n.kind) {
        case NK::ReturnStmt:
        case NK::ThrowStmt:
        case NK::BreakStmt:
        case NK::ContinueStmt: return true;
        case NK::Block: return !n.list.empty() && alwaysExits(*n.list.back());
        case NK::If: return n.b && n.c && alwaysExits(*n.b) && alwaysExits(*n.c);
        default: return false;
    }
}

class Checker
{
public:
    Checker(std::string sourceName, const CheckOptions& options)
        : name_(std::move(sourceName)), options_(options)
    {
        scopes_.emplace_back();
    }

    void declareGlobals(const Node& program) { collect(program.list, true); }
    /// Names the embedder installed on the global object. They are typed `any`:
    /// the checker cannot see a C++ function's signature, and refusing to
    /// resolve them would make every host binding an error.
    void declareHostGlobals(const std::vector<String>& names)
    {
        for (const String& name : names) {
            if (!findValue(name))
                defineValue(name, arena_.anyType());
        }
    }
    void run(const Node& program) { checkStatements(program.list, false); }
    std::vector<Diagnostic> take() { return std::move(diagnostics_); }

private:
    // ---- diagnostics ----
    void report(const Node& where, const std::string& message)
    {
        if (ambientDepth_ > 0 || diagnostics_.size() >= kMaxDiagnostics)
            return;
        Diagnostic d;
        d.language = Language::TypeScript;
        d.file = name_;
        d.line = where.line;
        d.message = message;
        diagnostics_.push_back(std::move(d));
    }
    void reportMismatch(const Node& where, const Ty* from, const Ty* to, const char* what)
    {
        report(where, std::string(what) + ": '" + toUtf8(TypeArena::describe(from)) +
                          "' is not assignable to '" + toUtf8(TypeArena::describe(to)) + "'");
    }

    bool assignable(const Ty* from, const Ty* to)
    {
        Assignability relation(options_.strictNullChecks);
        return relation.holds(from, to);
    }
    void requireAssignable(const Node& where, const Ty* from, const Ty* to, const char* what)
    {
        if (!from || !to)
            return;
        if (!assignable(from, to)) {
            reportMismatch(where, from, to, what);
            return;
        }
        // An object literal written right here is checked for properties the
        // target does not have. Structural typing allows the extra ones, but a
        // literal with nowhere else to go is almost always a typo.
        if (where.kind != NK::ObjectLit || to->kind != TK::Object || to->stringIndex)
            return;
        for (const NodePtr& prop : where.list) {
            if (prop->computed || prop->propKind == PropKind::Spread || prop->text.empty())
                continue;
            bool known = false;
            for (const TyMember& m : to->members)
                known = known || m.name == prop->text;
            if (!known)
                report(*prop, "'" + toUtf8(prop->text) + "' does not exist in type '" +
                                  toUtf8(TypeArena::describe(to)) + "'");
        }
    }

    // ---- scopes ----
    Scope& scope() { return scopes_.back(); }
    void push() { scopes_.emplace_back(); }
    void pop() { scopes_.pop_back(); }

    ValueBinding* findValue(StringView name)
    {
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
            auto found = it->values.find(String(name));
            if (found != it->values.end())
                return &found->second;
        }
        return nullptr;
    }
    TypeBinding* findType(StringView name)
    {
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
            auto found = it->types.find(String(name));
            if (found != it->types.end())
                return &found->second;
        }
        return nullptr;
    }
    void defineValue(StringView name, const Ty* type, bool isConst = false)
    {
        scope().values[String(name)] = ValueBinding{type, nullptr, isConst, false};
    }

    // ---- narrowing ----
    const Ty* narrowedType(StringView path) const
    {
        for (auto it = facts_.rbegin(); it != facts_.rend(); ++it) {
            auto found = it->find(String(path));
            if (found != it->end())
                return found->second;
        }
        return nullptr;
    }
    /// The dotted path a narrowable expression denotes, or empty.
    static String pathOf(const Node& n)
    {
        if (n.kind == NK::Identifier)
            return n.text;
        if (n.kind == NK::ThisExpr)
            return ascii("this");
        if (n.kind == NK::Member && !n.computed && n.a) {
            String base = pathOf(*n.a);
            if (base.empty())
                return base;
            base += u'.';
            base += n.text;
            return base;
        }
        if ((n.kind == NK::NonNull || n.kind == NK::AsExpr) && n.a)
            return pathOf(*n.a);
        return String();
    }

    // ---- the pieces, in the order a program meets them ----
    void collect(const std::vector<NodePtr>& statements, bool ambient);
    void checkStatements(const std::vector<NodePtr>& statements, bool ownScope);
    void checkStatement(const Node& n);
    const Ty* checkExpr(const Node& n, const Ty* contextual = nullptr);

    const Ty* resolveType(const Node* n);
    const Ty* resolveTypeName(const Node& n);
    const Ty* instantiate(TypeBinding& binding, std::vector<const Ty*> args, const Node& where);
    const Ty* substitute(const Ty* t, const Substitution& map, int depth = 0);

    const Ty* typeOfBinding(ValueBinding& binding);
    /// `contextual` supplies the parameter types an arrow function leaves out.
    const Ty* typeOfFunction(const Node& fn, const Ty* thisType,
                             const TySignature* contextual = nullptr);
    void checkFunctionBody(const Node& fn, const Ty* type, const Ty* thisType,
                           bool inferReturn = false);
    const Ty* typeOfClass(const Node& cls);
    void checkClassBody(const Node& cls, const Ty* instance, const Ty* statics);
    const Ty* typeOfEnum(const Node& n);

    const Ty* memberType(const Node& where, const Ty* object, StringView name, bool optional);
    const Ty* apparentType(const Ty* t);
    const Ty* globalInterface(const char* name);
    const Ty* arrayInterface(const Ty* element);

    const Ty* checkCall(const Node& n, bool isNew);
    const Ty* callSignature(const Node& n, const Ty* callee, const TySignature& sig, bool isNew);
    bool inferTypeArguments(const TySignature& sig, const std::vector<const Ty*>& args,
                            Substitution& out);
    static void inferFrom(const Ty* parameter, const Ty* argument, const std::vector<String>& names,
                          Substitution& out);

    void bindPattern(const Node& target, const Ty* type, bool isConst);
    const Ty* widen(const Ty* t);
    /// What iterating or spreading `t` yields.
    const Ty* elementTypeOf(const Ty* t);
    const Ty* literalOf(const Node& n);

    void collectFacts(const Node& condition, bool whenTrue, Facts& out);
    void narrowByTypeOf(const Node& operand, StringView name, bool equal, Facts& out);
    void narrowByTruth(const Node& operand, bool truthy, Facts& out);
    void narrowByLiteral(const Node& left, const Node& right, bool equal, Facts& out);
    const Ty* typeOfPath(const Node& n);

    static constexpr std::size_t kMaxDiagnostics = 200;

    std::string name_;
    CheckOptions options_;
    TypeArena arena_;
    std::vector<Scope> scopes_;
    std::vector<Facts> facts_;
    std::vector<FunctionContext> functions_;
    FunctionContext lastFunction_;
    std::vector<Diagnostic> diagnostics_;
    std::unordered_set<const Node*> checkedBodies_;
    std::unordered_map<std::string, const Ty*> substitutions_;
    int ambientDepth_ = 0;
    int constDepth_ = 0;
    int loopDepth_ = 0;
};

// ------------------------------------------------------ type resolution ---

const Ty* Checker::globalInterface(const char* name)
{
    TypeBinding* binding = findType(ascii(name));
    if (!binding || !binding->type)
        return arena_.anyType();
    return binding->type;
}

const Ty* Checker::arrayInterface(const Ty* element)
{
    TypeBinding* binding = findType(ascii("Array"));
    if (!binding || !binding->type || binding->params.empty())
        return arena_.anyType();
    Substitution map;
    map[binding->params.front()] = element ? element : arena_.anyType();
    return substitute(binding->type, map);
}

const Ty* Checker::resolveType(const Node* n)
{
    if (!n)
        return nullptr;
    switch (n->kind) {
        case NK::TypeRef: return resolveTypeName(*n);
        case NK::ArrayType: return arena_.arrayOf(resolveType(n->type.get()), n->readOnly);
        case NK::TupleType: {
            std::vector<const Ty*> items;
            items.reserve(n->list.size());
            for (const NodePtr& element : n->list) {
                if (element->kind == NK::RestElement) {
                    // A rest element makes the tuple open-ended; an array of
                    // the union of what it may hold is the honest answer.
                    return arena_.arrayOf(resolveType(element->type.get()), n->readOnly);
                }
                const Ty* item = resolveType(element.get());
                if (element->optional)
                    item = arena_.unionOf({item, arena_.undefinedType()});
                items.push_back(item);
            }
            return arena_.tupleOf(std::move(items), n->readOnly);
        }
        case NK::UnionType: {
            std::vector<const Ty*> members;
            members.reserve(n->list.size());
            for (const NodePtr& part : n->list)
                members.push_back(resolveType(part.get()));
            return arena_.unionOf(std::move(members));
        }
        case NK::IntersectionType: {
            // An intersection of object types is the union of their members.
            Ty built;
            for (const NodePtr& part : n->list) {
                const Ty* t = resolveType(part.get());
                if (!t)
                    continue;
                if (t->kind != TK::Object)
                    return arena_.anyType();
                for (const TyMember& m : t->members) {
                    auto existing =
                        std::find_if(built.members.begin(), built.members.end(),
                                     [&](const TyMember& other) { return other.name == m.name; });
                    if (existing == built.members.end())
                        built.members.push_back(m);
                }
                for (const TySignature& sig : t->calls)
                    built.calls.push_back(sig);
                for (const TySignature& sig : t->constructs)
                    built.constructs.push_back(sig);
                if (t->stringIndex)
                    built.stringIndex = t->stringIndex;
                if (t->numberIndex)
                    built.numberIndex = t->numberIndex;
            }
            return arena_.object(std::move(built));
        }
        case NK::LiteralType:
            switch (n->op) {
                case Tok::StringLit: return arena_.stringLiteral(n->text);
                case Tok::Number: return arena_.numberLiteral(n->number);
                case Tok::True: return arena_.booleanLiteral(true);
                case Tok::False: return arena_.booleanLiteral(false);
                default: return arena_.nullType();
            }
        case NK::FunctionType:
        case NK::ConstructorType: {
            TySignature sig;
            push();
            for (const NodePtr& param : n->typeParams) {
                const Ty* constraint = resolveType(param->a.get());
                const Ty* placeholder = arena_.paramOf(param->text, constraint);
                scope().types[param->text] = TypeBinding{placeholder, {}, {}, nullptr, false};
                sig.typeParams.push_back(param->text);
                sig.constraints.push_back(constraint);
            }
            for (const NodePtr& param : n->list) {
                TyParam p;
                p.rest = param->kind == NK::RestElement;
                const Node* named = p.rest ? param->a.get() : param.get();
                if (named && named->kind == NK::Identifier)
                    p.name = named->text;
                p.type = resolveType(param->type.get());
                if (!p.type)
                    p.type = arena_.anyType();
                p.optional = param->optional;
                sig.params.push_back(std::move(p));
            }
            const Node* returnNode = n->type.get();
            if (returnNode && returnNode->kind == NK::TypePredicate) {
                sig.predicateOn = returnNode->text;
                sig.predicateType = resolveType(returnNode->type.get());
                sig.returnType = arena_.booleanType();
            } else {
                sig.returnType = resolveType(returnNode);
            }
            if (!sig.returnType)
                sig.returnType = arena_.anyType();
            pop();
            if (n->kind == NK::FunctionType)
                return arena_.functionOf(std::move(sig));
            Ty built;
            built.constructs.push_back(std::move(sig));
            return arena_.object(std::move(built));
        }
        case NK::TypeLit: {
            Ty built;
            for (const NodePtr& member : n->list) {
                if (member->computed) {
                    const Ty* key = resolveType(member->a.get());
                    const Ty* value = resolveType(member->type.get());
                    if (key && key->kind == TK::Number)
                        built.numberIndex = value;
                    else
                        built.stringIndex = value;
                    continue;
                }
                if (member->text.empty()) {
                    const Ty* signature = resolveType(member->type.get());
                    if (!signature)
                        continue;
                    for (const TySignature& sig : signature->calls)
                        built.calls.push_back(sig);
                    for (const TySignature& sig : signature->constructs)
                        built.constructs.push_back(sig);
                    continue;
                }
                TyMember m;
                m.name = member->text;
                m.optional = member->optional;
                m.readOnly = member->readOnly;
                m.type = resolveType(member->type.get());
                if (!m.type)
                    m.type = arena_.anyType();
                if (m.optional)
                    m.type = arena_.unionOf({m.type, arena_.undefinedType()});
                built.members.push_back(std::move(m));
            }
            return arena_.object(std::move(built));
        }
        case NK::KeyOfType: {
            const Ty* operand = apparentType(resolveType(n->type.get()));
            if (!operand || operand->kind != TK::Object)
                return arena_.stringType();
            std::vector<const Ty*> keys;
            for (const TyMember& m : operand->members)
                keys.push_back(arena_.stringLiteral(m.name));
            if (keys.empty())
                return arena_.neverType();
            return arena_.unionOf(std::move(keys));
        }
        case NK::IndexedAccess: {
            const Ty* object = apparentType(resolveType(n->type.get()));
            const Ty* index = resolveType(n->b.get());
            if (!object || !index)
                return arena_.anyType();
            if (object->kind == TK::Array)
                return object->element;
            if (index->kind == TK::StrLit) {
                for (const TyMember& m : object->members) {
                    if (m.name == index->text)
                        return m.type;
                }
            }
            std::vector<const Ty*> all;
            for (const TyMember& m : object->members)
                all.push_back(m.type);
            return all.empty() ? arena_.anyType() : arena_.unionOf(std::move(all));
        }
        case NK::TypeQuery: {
            ValueBinding* binding = findValue(n->text);
            if (!binding) {
                report(*n, "cannot find name '" + toUtf8(n->text) + "'");
                return arena_.anyType();
            }
            return typeOfBinding(*binding);
        }
        case NK::TypePredicate: return arena_.booleanType();
        default: return arena_.anyType();
    }
}

const Ty* Checker::resolveTypeName(const Node& n)
{
    static const struct
    {
        const char* name;
        TK kind;
    } kPrimitives[] = {
        {"any", TK::Any},         {"unknown", TK::Unknown},     {"never", TK::Never},
        {"void", TK::Void},       {"undefined", TK::Undefined}, {"null", TK::Null},
        {"boolean", TK::Boolean}, {"number", TK::Number},       {"string", TK::Str},
        {"symbol", TK::Symbol},
    };
    for (const auto& primitive : kPrimitives) {
        if (n.text == ascii(primitive.name)) {
            switch (primitive.kind) {
                case TK::Any: return arena_.anyType();
                case TK::Unknown: return arena_.unknownType();
                case TK::Never: return arena_.neverType();
                case TK::Void: return arena_.voidType();
                case TK::Undefined: return arena_.undefinedType();
                case TK::Null: return arena_.nullType();
                case TK::Boolean: return arena_.booleanType();
                case TK::Number: return arena_.numberType();
                case TK::Str: return arena_.stringType();
                default: return arena_.symbolType();
            }
        }
    }
    if (n.text == ascii("object") || n.text == ascii("this"))
        return arena_.anyType();

    TypeBinding* binding = findType(n.text);
    if (!binding) {
        report(n, "cannot find type '" + toUtf8(n.text) + "'");
        return arena_.anyType();
    }
    std::vector<const Ty*> args;
    args.reserve(n.list.size());
    for (const NodePtr& argument : n.list)
        args.push_back(resolveType(argument.get()));
    return instantiate(*binding, std::move(args), n);
}

const Ty* Checker::instantiate(TypeBinding& binding, std::vector<const Ty*> args, const Node& where)
{
    if (binding.pending && !binding.resolving) {
        binding.resolving = true;
        const Node* body = binding.pending;
        binding.pending = nullptr;
        push();
        for (std::size_t i = 0; i < binding.params.size(); ++i)
            scope().types[binding.params[i]] =
                TypeBinding{arena_.paramOf(binding.params[i], nullptr), {}, {}, nullptr, false};
        binding.type = resolveType(body);
        pop();
        binding.resolving = false;
    }
    if (!binding.type)
        return arena_.anyType();
    if (binding.params.empty())
        return binding.type;
    // `interface Promise<T> { then(): Promise<U> }` names itself before it has
    // any members; instantiating now would freeze the empty shell in place.
    if (arena_.isOpen(binding.type))
        return binding.type;
    if (args.size() > binding.params.size()) {
        report(where, "'" + toUtf8(where.text) + "' takes " +
                          std::to_string(binding.params.size()) + " type argument(s)");
        args.resize(binding.params.size());
    }
    Substitution map;
    for (std::size_t i = 0; i < binding.params.size(); ++i) {
        if (i < args.size() && args[i])
            map[binding.params[i]] = args[i];
        else if (i < binding.defaults.size() && binding.defaults[i])
            map[binding.params[i]] = binding.defaults[i];
        else
            map[binding.params[i]] = arena_.anyType();
    }
    return substitute(binding.type, map);
}

const Ty* Checker::substitute(const Ty* t, const Substitution& map, int depth)
{
    if (!t || map.empty())
        return t;
    // A recursive generic would rebuild for ever; past this depth the
    // uninstantiated type is a sound enough stand-in.
    if (depth > 8)
        return t;
    switch (t->kind) {
        case TK::Param: {
            auto found = map.find(t->text);
            return found == map.end() ? t : found->second;
        }
        case TK::Array: return arena_.arrayOf(substitute(t->element, map, depth + 1), t->readOnly);
        case TK::Tuple: {
            std::vector<const Ty*> items;
            items.reserve(t->items.size());
            for (const Ty* item : t->items)
                items.push_back(substitute(item, map, depth + 1));
            return arena_.tupleOf(std::move(items), t->readOnly);
        }
        case TK::Union: {
            std::vector<const Ty*> items;
            items.reserve(t->items.size());
            for (const Ty* item : t->items)
                items.push_back(substitute(item, map, depth + 1));
            return arena_.unionOf(std::move(items));
        }
        case TK::Object: break;
        default: return t;
    }

    // Substituting the same type with the same arguments twice is common --
    // every mention of `Map<string, number>` asks for it -- and a recursive
    // interface would otherwise rebuild its members once per level.
    std::string key(reinterpret_cast<const char*>(&t), sizeof t);
    for (const auto& entry : std::map<String, const Ty*>(map.begin(), map.end())) {
        key += toUtf8(entry.first);
        key += '=';
        key += toUtf8(TypeArena::describe(entry.second, true));
        key += ';';
    }
    auto cached = substitutions_.find(key);
    if (cached != substitutions_.end())
        return cached->second;

    // A named type is instantiated into a shell that is registered before its
    // members are built, so a member that refers back to it -- `add(v: T):
    // Set<T>` -- sees the finished instantiation and not the generic original.
    Ty* shell = nullptr;
    if (!t->text.empty()) {
        // An already-instantiated name is re-instantiated, not nested.
        String label = t->text.substr(0, t->text.find(u'<'));
        label += u'<';
        for (const auto& entry : std::map<String, const Ty*>(map.begin(), map.end())) {
            if (label.back() != u'<')
                label += ascii(", ");
            label += TypeArena::describe(entry.second);
        }
        label += u'>';
        shell = arena_.makeNamed(label);
        substitutions_[key] = shell;
    } else {
        substitutions_.emplace(key, t);  // break the cycle while this one is built
    }

    auto substituteSignature = [&](const TySignature& sig) {
        TySignature out = sig;
        // A signature's own type parameters shadow the substitution.
        Substitution inner = map;
        for (const String& own : sig.typeParams)
            inner.erase(own);
        for (TyParam& p : out.params)
            p.type = substitute(p.type, inner, depth + 1);
        out.returnType = substitute(out.returnType, inner, depth + 1);
        out.predicateType = substitute(out.predicateType, inner, depth + 1);
        return out;
    };

    Ty built;
    built.kind = TK::Object;
    built.members.reserve(t->members.size());
    for (const TyMember& m : t->members) {
        TyMember out = m;
        out.type = substitute(m.type, map, depth + 1);
        built.members.push_back(std::move(out));
    }
    for (const TySignature& sig : t->calls)
        built.calls.push_back(substituteSignature(sig));
    for (const TySignature& sig : t->constructs)
        built.constructs.push_back(substituteSignature(sig));
    built.stringIndex = substitute(t->stringIndex, map, depth + 1);
    built.numberIndex = substitute(t->numberIndex, map, depth + 1);
    if (shell) {
        arena_.fill(shell, std::move(built));
        return shell;
    }
    const Ty* result = arena_.object(std::move(built));
    substitutions_[key] = result;
    return result;
}

// ------------------------------------------------------------- members ---

const Ty* Checker::apparentType(const Ty* t)
{
    if (!t)
        return arena_.anyType();
    switch (t->kind) {
        case TK::Str:
        case TK::StrLit: return globalInterface("String");
        case TK::Number:
        case TK::NumberLit: return globalInterface("Number");
        case TK::Boolean:
        case TK::BooleanLit: return globalInterface("Boolean");
        case TK::Symbol: return globalInterface("Symbol");
        case TK::Array: return arrayInterface(t->element);
        case TK::Tuple: return arrayInterface(arena_.unionOf(t->items));
        case TK::Param: return t->constraint ? apparentType(t->constraint) : arena_.unknownType();
        default: return t;
    }
}

const Ty* Checker::memberType(const Node& where, const Ty* object, StringView name, bool optional)
{
    if (!object || object->kind == TK::Any)
        return arena_.anyType();

    if (isNullish(object) && object->kind != TK::Unknown) {
        if (optional) {
            const Ty* defined = filterUnion(arena_, object, [](const Ty* part) {
                return part->kind != TK::Null && part->kind != TK::Undefined &&
                       part->kind != TK::Void;
            });
            const Ty* result = memberType(where, defined, name, false);
            return arena_.unionOf({result, arena_.undefinedType()});
        }
        if (options_.strictNullChecks &&
            (object->kind == TK::Null || object->kind == TK::Undefined ||
             object->kind == TK::Void || object->kind == TK::Union)) {
            report(where,
                   "'" + toUtf8(TypeArena::describe(object)) + "' is possibly null or undefined");
            object = filterUnion(arena_, object, [](const Ty* part) {
                return part->kind != TK::Null && part->kind != TK::Undefined &&
                       part->kind != TK::Void;
            });
        }
    }

    if (object->kind == TK::Union) {
        std::vector<const Ty*> results;
        for (const Ty* part : object->items)
            results.push_back(memberType(where, part, name, optional));
        return arena_.unionOf(std::move(results));
    }

    // `length` and index access on arrays and tuples come before the interface
    // so that a tuple keeps its element types.
    if (object->kind == TK::Tuple && name == ascii("length"))
        return arena_.numberLiteral(double(object->items.size()));

    const Ty* apparent = apparentType(object);
    if (!apparent || apparent->kind != TK::Object)
        return arena_.anyType();

    for (const TyMember& m : apparent->members) {
        if (m.name == name)
            return m.type;
    }
    if (apparent->stringIndex)
        return apparent->stringIndex;

    // Everything inherits from `Object`.
    const Ty* base = globalInterface("Object");
    if (base && base->kind == TK::Object && base != apparent) {
        for (const TyMember& m : base->members) {
            if (m.name == name)
                return m.type;
        }
    }
    if (apparent->callable() || !apparent->constructs.empty()) {
        const Ty* fn = globalInterface("Function");
        if (fn && fn->kind == TK::Object) {
            for (const TyMember& m : fn->members) {
                if (m.name == name)
                    return m.type;
            }
        }
    }

    report(where, "property '" + toUtf8(String(name)) + "' does not exist on type '" +
                      toUtf8(TypeArena::describe(object)) + "'");
    return arena_.anyType();
}

// ------------------------------------------------------------- widening ---

const Ty* Checker::widen(const Ty* t)
{
    if (!t)
        return t;
    switch (t->kind) {
        case TK::StrLit: return arena_.stringType();
        case TK::NumberLit: return arena_.numberType();
        case TK::BooleanLit: return arena_.booleanType();
        case TK::Union: {
            std::vector<const Ty*> items;
            items.reserve(t->items.size());
            for (const Ty* item : t->items)
                items.push_back(widen(item));
            return arena_.unionOf(std::move(items));
        }
        case TK::Array: return arena_.arrayOf(widen(t->element), t->readOnly);
        default: return t;
    }
}

const Ty* Checker::elementTypeOf(const Ty* t)
{
    if (!t)
        return arena_.anyType();
    switch (t->kind) {
        case TK::Array: return t->element;
        case TK::Tuple: return t->items.empty() ? arena_.neverType() : arena_.unionOf(t->items);
        case TK::Str:
        case TK::StrLit: return arena_.stringType();
        case TK::Union: {
            std::vector<const Ty*> parts;
            parts.reserve(t->items.size());
            for (const Ty* item : t->items)
                parts.push_back(elementTypeOf(item));
            return arena_.unionOf(std::move(parts));
        }
        case TK::Object: break;
        default: return arena_.anyType();
    }

    // Anything else is iterable only if it says so. `[Symbol.iterator]()`
    // returns an `Iterator<T>`, and `T` is what `next()` reports as `value`.
    auto method = [](const Ty* object, const char* name) -> const Ty* {
        if (!object || object->kind != TK::Object)
            return nullptr;
        for (const TyMember& m : object->members) {
            if (m.name == ascii(name) && m.type && m.type->callable())
                return m.type->calls.front().returnType;
        }
        return nullptr;
    };
    const Ty* step = method(method(t, "@@iterator"), "next");
    if (step && step->kind == TK::Object) {
        for (const TyMember& m : step->members) {
            if (m.name == ascii("value"))
                return m.type;
        }
    }
    return arena_.anyType();
}

const Ty* Checker::literalOf(const Node& n)
{
    switch (n.kind) {
        case NK::NumberLit: return arena_.numberLiteral(n.number);
        case NK::StringLit: return arena_.stringLiteral(n.text);
        case NK::BoolLit: return arena_.booleanLiteral(n.number != 0);
        case NK::NullLit: return arena_.nullType();
        default: return nullptr;
    }
}

// ---------------------------------------------------------- hoisting ---

void Checker::collect(const std::vector<NodePtr>& statements, bool ambient)
{
    if (ambient)
        ++ambientDepth_;

    // Every type name is registered before any body is resolved, so that
    // declarations may refer to each other in either order.
    std::vector<std::pair<const Node*, Ty*>> openTypes;
    for (const NodePtr& s : statements) {
        switch (s->kind) {
            case NK::InterfaceDecl:
            case NK::ClassDecl: {
                const Node& decl = *s;
                const Node& generic = decl.kind == NK::ClassDecl ? *decl.a : decl;
                TypeBinding binding;
                Ty* open = arena_.openObject(decl.text);
                binding.type = open;
                for (const NodePtr& p : generic.typeParams) {
                    binding.params.push_back(p->text);
                    binding.defaults.push_back(nullptr);
                }
                scope().types[decl.text] = std::move(binding);
                openTypes.emplace_back(&decl, open);
                break;
            }
            case NK::TypeAlias: {
                TypeBinding binding;
                binding.pending = s->type.get();
                for (const NodePtr& p : s->typeParams) {
                    binding.params.push_back(p->text);
                    binding.defaults.push_back(nullptr);
                }
                scope().types[s->text] = std::move(binding);
                break;
            }
            case NK::EnumDecl: {
                const Ty* object = typeOfEnum(*s);
                std::vector<const Ty*> values;
                for (const TyMember& m : object->members)
                    values.push_back(m.type);
                TypeBinding binding;
                binding.type =
                    values.empty() ? arena_.numberType() : arena_.unionOf(std::move(values));
                scope().types[s->text] = std::move(binding);
                defineValue(s->text, object, true);
                break;
            }
            default: break;
        }
    }

    // Type parameter defaults need the names above to exist.
    for (const NodePtr& s : statements) {
        if (s->kind != NK::TypeAlias && s->kind != NK::InterfaceDecl)
            continue;
        TypeBinding* binding = findType(s->text);
        if (!binding)
            continue;
        for (std::size_t i = 0; i < s->typeParams.size() && i < binding->defaults.size(); ++i)
            binding->defaults[i] = resolveType(s->typeParams[i]->b.get());
    }

    for (const auto& entry : openTypes) {
        const Node& decl = *entry.first;
        Ty* open = entry.second;
        const Node& generic = decl.kind == NK::ClassDecl ? *decl.a : decl;
        push();
        for (const NodePtr& p : generic.typeParams) {
            const Ty* constraint = resolveType(p->a.get());
            scope().types[p->text] =
                TypeBinding{arena_.paramOf(p->text, constraint), {}, {}, nullptr, false};
        }
        if (decl.kind == NK::InterfaceDecl) {
            // `extends` on an interface copies members in; a later member of
            // the same name wins, which is what declaration merging means.
            if (decl.c) {
                for (const NodePtr& base : decl.c->list) {
                    const Ty* baseType = resolveType(base.get());
                    if (baseType && baseType->kind == TK::Object) {
                        for (const TyMember& m : baseType->members)
                            open->members.push_back(m);
                        if (baseType->stringIndex)
                            open->stringIndex = baseType->stringIndex;
                        if (baseType->numberIndex)
                            open->numberIndex = baseType->numberIndex;
                    }
                }
            }
            const Ty* body = resolveType(decl.type.get());
            if (body) {
                for (const TyMember& m : body->members) {
                    auto existing =
                        std::find_if(open->members.begin(), open->members.end(),
                                     [&](const TyMember& o) { return o.name == m.name; });
                    if (existing != open->members.end())
                        *existing = m;
                    else
                        open->members.push_back(m);
                }
                for (const TySignature& sig : body->calls)
                    open->calls.push_back(sig);
                for (const TySignature& sig : body->constructs)
                    open->constructs.push_back(sig);
                if (body->stringIndex)
                    open->stringIndex = body->stringIndex;
                if (body->numberIndex)
                    open->numberIndex = body->numberIndex;
            }
        }
        pop();
        arena_.close(open);
    }

    // Value names next: a function or class may be called before it is written,
    // and may call itself, so each name exists before any type is worked out.
    for (const NodePtr& s : statements) {
        if (s->kind == NK::FunctionDecl || s->kind == NK::ClassDecl) {
            if (!scope().values.count(s->text))
                defineValue(s->text, arena_.anyType());
        }
    }
    for (const NodePtr& s : statements) {
        switch (s->kind) {
            case NK::FunctionDecl: defineValue(s->text, typeOfFunction(*s->a, nullptr)); break;
            case NK::ClassDecl: defineValue(s->text, typeOfClass(*s->a), true); break;
            case NK::VarDecl:
                for (const NodePtr& d : s->list) {
                    if (!d->a)
                        continue;
                    if (d->a->kind == NK::Identifier) {
                        ValueBinding binding;
                        binding.pending = d.get();
                        binding.isConst = s->varKind == VarKind::Const;
                        scope().values[d->a->text] = std::move(binding);
                    } else {
                        bindPattern(*d->a, arena_.anyType(), s->varKind == VarKind::Const);
                    }
                }
                break;
            case NK::ModuleDecl: defineValue(s->text, arena_.anyType(), true); break;
            default: break;
        }
    }

    if (ambient)
        --ambientDepth_;
}

const Ty* Checker::typeOfBinding(ValueBinding& binding)
{
    if (binding.type)
        return binding.type;
    if (!binding.pending || binding.resolving)
        return binding.type = arena_.anyType();

    binding.resolving = true;
    const Node& declarator = *binding.pending;
    binding.pending = nullptr;
    checkedBodies_.insert(&declarator);

    const Ty* declared = resolveType(declarator.type.get());
    if (declared) {
        binding.type = declared;
        if (declarator.b)
            requireAssignable(*declarator.b, checkExpr(*declarator.b, declared), declared,
                              "initialiser type mismatch");
    } else if (declarator.b) {
        const Ty* value = checkExpr(*declarator.b);
        binding.type = binding.isConst ? value : widen(value);
    } else {
        binding.type = options_.strictNullChecks ? arena_.undefinedType() : arena_.anyType();
    }
    binding.resolving = false;
    return binding.type;
}

// ------------------------------------------------------------ functions ---

const Ty* Checker::typeOfFunction(const Node& fn, const Ty* thisType, const TySignature* contextual)
{
    TySignature sig;
    push();
    for (const NodePtr& p : fn.typeParams) {
        const Ty* constraint = resolveType(p->a.get());
        scope().types[p->text] =
            TypeBinding{arena_.paramOf(p->text, constraint), {}, {}, nullptr, false};
        sig.typeParams.push_back(p->text);
        sig.constraints.push_back(constraint);
    }

    for (std::size_t index = 0; index < fn.list.size(); ++index) {
        const NodePtr& param = fn.list[index];
        TyParam p;
        const Node* named = param.get();
        p.rest = named->kind == NK::RestElement;
        if (p.rest)
            named = named->a.get();
        const Node* withDefault = nullptr;
        if (named && named->kind == NK::AssignPattern) {
            withDefault = named->b.get();
            named = named->a.get();
        }
        if (named && named->kind == NK::Identifier)
            p.name = named->text;
        p.type = resolveType(param->type.get());
        if (!p.type && contextual && index < contextual->params.size())
            p.type = contextual->params[index].type;
        // A destructuring parameter with no annotation is `any`: the shape the
        // pattern wants is not the shape its default supplies, and guessing
        // either one produces errors the programmer cannot act on.
        if (!p.type && named && named->kind != NK::Identifier)
            p.type = arena_.anyType();
        if (!p.type && withDefault)
            p.type = widen(checkExpr(*withDefault));
        if (!p.type) {
            if (options_.noImplicitAny && !p.name.empty() && ambientDepth_ == 0 &&
                fn.kind != NK::ArrowExpr)
                report(fn, "parameter '" + toUtf8(p.name) + "' implicitly has an 'any' type");
            p.type = arena_.anyType();
        }
        p.optional = param->optional || withDefault != nullptr;
        if (param->optional)
            p.type = arena_.unionOf({p.type, arena_.undefinedType()});
        sig.params.push_back(std::move(p));
    }

    const Node* returnNode = fn.type.get();
    if (returnNode && returnNode->kind == NK::TypePredicate) {
        sig.predicateOn = returnNode->text;
        sig.predicateType = resolveType(returnNode->type.get());
        sig.returnType = arena_.booleanType();
    } else {
        sig.returnType = resolveType(returnNode);
    }
    pop();

    if (!sig.returnType) {
        // No annotation: the body decides. Checking it now is what lets a
        // caller written above the declaration see a real type.
        if (fn.d && !checkedBodies_.count(&fn)) {
            TySignature provisional = sig;
            provisional.returnType = arena_.anyType();
            checkFunctionBody(fn, arena_.functionOf(std::move(provisional)), thisType, true);
            const FunctionContext& done = lastFunction_;
            sig.returnType =
                done.returns.empty() ? arena_.voidType() : arena_.unionOf(done.returns);
            if (!done.sawReturn)
                sig.returnType = arena_.voidType();
        } else {
            sig.returnType = arena_.anyType();
        }
    }
    return arena_.functionOf(std::move(sig));
}

void Checker::checkFunctionBody(const Node& fn, const Ty* type, const Ty* thisType,
                                bool inferReturn)
{
    if (!fn.d || !checkedBodies_.insert(&fn).second)
        return;
    const TySignature& sig = type->calls.front();

    push();
    facts_.emplace_back();
    for (const String& name : sig.typeParams)
        scope().types[name] = TypeBinding{arena_.paramOf(name, nullptr), {}, {}, nullptr, false};

    for (std::size_t i = 0; i < fn.list.size(); ++i) {
        const Node* param = fn.list[i].get();
        const Ty* declared = i < sig.params.size() ? sig.params[i].type : arena_.anyType();
        if (param->kind == NK::RestElement) {
            const Ty* element =
                declared && declared->kind == TK::Array ? declared->element : arena_.anyType();
            if (param->a)
                bindPattern(*param->a, arena_.arrayOf(element), false);
            continue;
        }
        const Node* target = param;
        if (target->kind == NK::AssignPattern)
            target = target->a.get();
        bindPattern(*target, declared, false);
    }

    FunctionContext context;
    context.declaredReturn = inferReturn ? nullptr : sig.returnType;
    context.thisType = thisType;
    if (thisType)
        defineValue(ascii("this"), thisType, true);
    functions_.push_back(std::move(context));
    const int savedLoops = loopDepth_;
    loopDepth_ = 0;

    checkStatements(fn.d->list, false);

    loopDepth_ = savedLoops;
    lastFunction_ = std::move(functions_.back());
    functions_.pop_back();
    facts_.pop_back();
    pop();
}

// -------------------------------------------------------------- classes ---

const Ty* Checker::typeOfEnum(const Node& n)
{
    Ty built;
    built.text = n.text;
    double next = 0;
    for (const NodePtr& member : n.list) {
        TyMember m;
        m.name = member->text;
        m.readOnly = true;
        if (!member->a) {
            m.type = arena_.numberLiteral(next);
            next += 1;
        } else if (member->a->kind == NK::NumberLit) {
            m.type = arena_.numberLiteral(member->a->number);
            next = member->a->number + 1;
        } else if (member->a->kind == NK::StringLit) {
            m.type = arena_.stringLiteral(member->a->text);
        } else {
            m.type = arena_.numberType();
        }
        built.members.push_back(std::move(m));
    }
    // Reverse mapping: a numeric enum object also maps values back to names.
    bool numeric = true;
    for (const TyMember& m : built.members)
        numeric = numeric && m.type->kind == TK::NumberLit;
    if (numeric)
        built.numberIndex = arena_.stringType();
    return arena_.object(std::move(built));
}

const Ty* Checker::typeOfClass(const Node& cls)
{
    TypeBinding* binding = findType(cls.text.empty() ? ascii("") : cls.text);
    Ty* instance = nullptr;
    if (binding && binding->type && binding->type->kind == TK::Object)
        instance = const_cast<Ty*>(binding->type);

    Ty owned;
    Ty* target = instance ? instance : &owned;
    Ty statics;

    push();
    for (const NodePtr& p : cls.typeParams) {
        const Ty* constraint = resolveType(p->a.get());
        scope().types[p->text] =
            TypeBinding{arena_.paramOf(p->text, constraint), {}, {}, nullptr, false};
    }

    // A base class contributes its members before the derived ones override.
    if (cls.a) {
        const Ty* base = checkExpr(*cls.a);
        if (base && base->kind == TK::Object && !base->constructs.empty()) {
            const Ty* baseInstance = base->constructs.front().returnType;
            if (baseInstance && baseInstance->kind == TK::Object) {
                for (const TyMember& m : baseInstance->members)
                    target->members.push_back(m);
            }
        }
    }

    TySignature constructor;
    constructor.returnType = nullptr;
    bool sawConstructor = false;

    for (const NodePtr& member : cls.list) {
        if (member->kind == NK::TypeMember) {
            const Ty* value = resolveType(member->type.get());
            (member->isStatic ? statics : *target).stringIndex = value;
            continue;
        }
        if (member->computed || !member->a)
            continue;
        const String name = member->a->text;
        Ty& into = member->isStatic ? statics : *target;

        if (member->kind == NK::FieldDef) {
            TyMember m;
            m.name = name;
            m.readOnly = member->readOnly;
            m.optional = member->optional;
            m.type = resolveType(member->type.get());
            if (!m.type && member->b)
                m.type = widen(checkExpr(*member->b));
            if (!m.type)
                m.type = arena_.anyType();
            if (member->optional)
                m.type = arena_.unionOf({m.type, arena_.undefinedType()});
            into.members.push_back(std::move(m));
            continue;
        }

        if (member->propKind == PropKind::Constructor) {
            sawConstructor = true;
            const Ty* fn = typeOfFunction(*member->b, nullptr);
            constructor = fn->calls.front();
            // `constructor(private x: T)` declares a field of the same name.
            for (const NodePtr& param : member->b->list) {
                if (!param->isParamProp)
                    continue;
                const Node* named = param->kind == NK::AssignPattern ? param->a.get() : param.get();
                if (!named || named->kind != NK::Identifier)
                    continue;
                TyMember m;
                m.name = named->text;
                m.readOnly = param->readOnly;
                m.type = resolveType(param->type.get());
                if (!m.type)
                    m.type = arena_.anyType();
                target->members.push_back(std::move(m));
            }
            continue;
        }

        const Ty* fn = typeOfFunction(*member->b, nullptr);
        TyMember m;
        m.name = name;
        if (member->propKind == PropKind::Getter) {
            m.type = fn->calls.front().returnType;
            m.readOnly = false;
        } else if (member->propKind == PropKind::Setter) {
            const TySignature& sig = fn->calls.front();
            m.type = sig.params.empty() ? arena_.anyType() : sig.params.front().type;
        } else {
            m.type = fn;
        }
        auto existing = std::find_if(into.members.begin(), into.members.end(),
                                     [&](const TyMember& o) { return o.name == m.name; });
        if (existing != into.members.end())
            *existing = std::move(m);
        else
            into.members.push_back(std::move(m));
    }
    pop();

    const Ty* instanceType = instance ? arena_.close(instance) : arena_.object(std::move(owned));
    if (!sawConstructor && cls.a) {
        // An inherited constructor: accept anything until overloads exist.
        TyParam rest;
        rest.name = ascii("args");
        rest.rest = true;
        rest.type = arena_.arrayOf(arena_.anyType());
        constructor.params.push_back(std::move(rest));
    }
    constructor.returnType = instanceType;
    statics.constructs.push_back(std::move(constructor));
    statics.members.push_back(TyMember{ascii("prototype"), instanceType, false, true});
    return arena_.object(std::move(statics));
}

void Checker::checkClassBody(const Node& cls, const Ty* instance, const Ty* statics)
{
    push();
    for (const NodePtr& p : cls.typeParams) {
        const Ty* constraint = resolveType(p->a.get());
        scope().types[p->text] =
            TypeBinding{arena_.paramOf(p->text, constraint), {}, {}, nullptr, false};
    }
    for (const NodePtr& member : cls.list) {
        if (member->kind == NK::TypeMember || member->declared || member->isAbstract)
            continue;
        if (member->kind == NK::FieldDef) {
            if (!member->b)
                continue;
            const Ty* declared = resolveType(member->type.get());
            const Ty* value = checkExpr(*member->b, declared);
            if (declared)
                requireAssignable(*member, value, declared, "field initialiser mismatch");
            continue;
        }
        if (!member->b || !member->b->d)
            continue;
        const Ty* self = member->isStatic ? statics : instance;
        checkFunctionBody(*member->b, typeOfFunction(*member->b, self), self);
    }

    // `implements` is checked once the instance type is complete.
    if (cls.c) {
        for (const NodePtr& base : cls.c->list) {
            const Ty* contract = resolveType(base.get());
            if (contract && !assignable(instance, contract))
                reportMismatch(cls, instance, contract, "class does not implement interface");
        }
    }
    pop();
}

// ------------------------------------------------------------ patterns ---

void Checker::bindPattern(const Node& target, const Ty* type, bool isConst)
{
    if (!type)
        type = arena_.anyType();
    switch (target.kind) {
        case NK::Identifier: defineValue(target.text, type, isConst); return;
        case NK::AssignPattern:
            if (target.a)
                bindPattern(*target.a, type, isConst);
            return;
        case NK::RestElement:
            if (target.a)
                bindPattern(*target.a, arena_.arrayOf(type), isConst);
            return;
        case NK::ArrayPattern: {
            const Ty* apparent = type;
            for (std::size_t i = 0; i < target.list.size(); ++i) {
                const Node& element = *target.list[i];
                if (element.kind == NK::Hole)
                    continue;
                const Ty* slot = arena_.anyType();
                if (apparent->kind == TK::Array)
                    slot = apparent->element;
                else if (apparent->kind == TK::Tuple && i < apparent->items.size())
                    slot = apparent->items[i];
                if (element.kind == NK::RestElement) {
                    if (element.a)
                        bindPattern(*element.a, arena_.arrayOf(slot), isConst);
                    continue;
                }
                bindPattern(element, slot, isConst);
            }
            return;
        }
        case NK::ObjectPattern: {
            for (const NodePtr& prop : target.list) {
                if (prop->kind == NK::RestElement) {
                    if (prop->a)
                        bindPattern(*prop->a, type, isConst);
                    continue;
                }
                if (!prop->b)
                    continue;
                const Ty* slot = arena_.anyType();
                if (!prop->computed && type->kind != TK::Any)
                    slot = memberType(*prop, type, prop->text, false);
                bindPattern(*prop->b, slot, isConst);
            }
            return;
        }
        default: return;
    }
}

// ---------------------------------------------------------- statements ---

void Checker::checkStatements(const std::vector<NodePtr>& statements, bool ownScope)
{
    if (ownScope)
        push();
    collect(statements, false);
    std::size_t narrowings = 0;
    for (const NodePtr& s : statements) {
        checkStatement(*s);
        // `if (!ok) return;` tells the rest of the block that `ok` holds.
        if (s->kind == NK::If && s->a && s->b && !s->c && alwaysExits(*s->b)) {
            Facts after;
            collectFacts(*s->a, false, after);
            if (!after.empty()) {
                facts_.push_back(std::move(after));
                ++narrowings;
            }
        }
    }
    while (narrowings-- > 0)
        facts_.pop_back();
    if (ownScope)
        pop();
}

void Checker::checkStatement(const Node& n)
{
    switch (n.kind) {
        case NK::Program:
        case NK::Block: checkStatements(n.list, true); return;
        case NK::ExprStatement:
            if (n.a)
                checkExpr(*n.a);
            return;
        case NK::VarDecl:
            if (n.declared)
                return;
            for (const NodePtr& d : n.list) {
                if (!d->a)
                    continue;
                if (d->a->kind == NK::Identifier) {
                    // A `for (let i = 0; ...)` head is not part of a statement
                    // list, so nothing hoisted it.
                    ValueBinding* binding = findValue(d->a->text);
                    if (!binding) {
                        ValueBinding fresh;
                        fresh.pending = d.get();
                        fresh.isConst = n.varKind == VarKind::Const;
                        scope().values[d->a->text] = std::move(fresh);
                        binding = &scope().values[d->a->text];
                    }
                    typeOfBinding(*binding);
                    continue;
                }
                const Ty* declared = resolveType(d->type.get());
                const Ty* value = d->b ? checkExpr(*d->b, declared) : arena_.anyType();
                if (declared && d->b)
                    requireAssignable(*d->b, value, declared, "initialiser type mismatch");
                bindPattern(*d->a, declared ? declared : widen(value), n.varKind == VarKind::Const);
            }
            return;
        case NK::FunctionDecl: {
            if (n.declared || !n.a)
                return;
            ValueBinding* binding = findValue(n.text);
            const Ty* type = binding ? typeOfBinding(*binding) : typeOfFunction(*n.a, nullptr);
            if (type && type->callable())
                checkFunctionBody(*n.a, type, nullptr);
            return;
        }
        case NK::ClassDecl: {
            if (n.declared || !n.a)
                return;
            ValueBinding* binding = findValue(n.text);
            const Ty* statics = binding ? typeOfBinding(*binding) : typeOfClass(*n.a);
            if (statics && !statics->constructs.empty())
                checkClassBody(*n.a, statics->constructs.front().returnType, statics);
            return;
        }
        case NK::If: {
            if (!n.a)
                return;
            checkExpr(*n.a);
            Facts whenTrue;
            Facts whenFalse;
            collectFacts(*n.a, true, whenTrue);
            collectFacts(*n.a, false, whenFalse);
            if (n.b) {
                facts_.push_back(std::move(whenTrue));
                checkStatement(*n.b);
                facts_.pop_back();
            }
            if (n.c) {
                facts_.push_back(std::move(whenFalse));
                checkStatement(*n.c);
                facts_.pop_back();
            }
            return;
        }
        case NK::While:
        case NK::DoWhile: {
            if (n.a)
                checkExpr(*n.a);
            Facts inside;
            if (n.a)
                collectFacts(*n.a, true, inside);
            facts_.push_back(std::move(inside));
            ++loopDepth_;
            if (n.d)
                checkStatement(*n.d);
            --loopDepth_;
            facts_.pop_back();
            return;
        }
        case NK::For: {
            push();
            if (n.a)
                checkStatement(*n.a);
            if (n.b)
                checkExpr(*n.b);
            if (n.c)
                checkExpr(*n.c);
            ++loopDepth_;
            if (n.d)
                checkStatement(*n.d);
            --loopDepth_;
            pop();
            return;
        }
        case NK::ForIn:
        case NK::ForOf: {
            push();
            const Ty* source = n.b ? checkExpr(*n.b) : arena_.anyType();
            const Ty* declared = resolveType(n.type.get());
            const Ty* element = declared;
            if (!element)
                element =
                    n.kind == NK::ForIn ? arena_.stringType() : elementTypeOf(apparentType(source));
            if (n.a)
                bindPattern(*n.a, element, n.varKind == VarKind::Const);
            ++loopDepth_;
            if (n.d)
                checkStatement(*n.d);
            --loopDepth_;
            pop();
            return;
        }
        case NK::ReturnStmt: {
            const Ty* value =
                n.a ? checkExpr(*n.a,
                                functions_.empty() ? nullptr : functions_.back().declaredReturn)
                    : arena_.voidType();
            if (functions_.empty())
                return;
            FunctionContext& context = functions_.back();
            context.sawReturn = true;
            if (context.declaredReturn)
                requireAssignable(n, value, context.declaredReturn, "return type mismatch");
            else
                context.returns.push_back(value);
            return;
        }
        case NK::ThrowStmt:
            if (n.a)
                checkExpr(*n.a);
            return;
        case NK::Try:
            if (n.a)
                checkStatement(*n.a);
            if (n.c) {
                push();
                if (n.b) {
                    const Ty* declared = resolveType(n.b->type.get());
                    bindPattern(*n.b, declared ? declared : arena_.anyType(), false);
                }
                checkStatement(*n.c);
                pop();
            }
            if (n.d)
                checkStatement(*n.d);
            return;
        case NK::Switch: {
            const Ty* subject = n.a ? checkExpr(*n.a) : arena_.anyType();
            push();
            ++loopDepth_;
            for (const NodePtr& clause : n.list) {
                Facts narrowed;
                if (clause->a) {
                    const Ty* label = checkExpr(*clause->a);
                    requireAssignable(*clause, label, widen(subject),
                                      "case label does not match the switch subject");
                    if (n.a)
                        narrowByLiteral(*n.a, *clause->a, true, narrowed);
                }
                facts_.push_back(std::move(narrowed));
                checkStatements(clause->list, true);
                facts_.pop_back();
            }
            --loopDepth_;
            pop();
            return;
        }
        case NK::Labeled:
            if (n.d)
                checkStatement(*n.d);
            return;
        case NK::BreakStmt:
        case NK::ContinueStmt:
            if (loopDepth_ == 0 && n.text.empty())
                report(n, "'break' and 'continue' are only allowed inside a loop or switch");
            return;
        case NK::ModuleDecl: {
            if (n.declared)
                return;
            push();
            checkStatements(n.list, false);
            // The namespace object carries exactly what the body exports.
            Ty built;
            auto exportName = [&](const String& name) {
                if (name.empty())
                    return;
                if (ValueBinding* binding = findValue(name))
                    built.members.push_back(TyMember{name, typeOfBinding(*binding), false, false});
            };
            for (const NodePtr& s : n.list) {
                if (!s->exported || s->declared)
                    continue;
                if (s->kind == NK::VarDecl) {
                    for (const NodePtr& d : s->list) {
                        if (d->a && d->a->kind == NK::Identifier)
                            exportName(d->a->text);
                    }
                } else {
                    exportName(s->text);
                }
            }
            pop();
            if (ValueBinding* binding = findValue(n.text)) {
                binding->pending = nullptr;
                binding->type = arena_.object(std::move(built));
            }
            return;
        }
        case NK::EnumDecl:
        case NK::InterfaceDecl:
        case NK::TypeAlias:
        case NK::Empty: return;
        default: checkExpr(n); return;
    }
}

// --------------------------------------------------------- expressions ---

const Ty* Checker::checkExpr(const Node& n, const Ty* contextual)
{
    switch (n.kind) {
        case NK::NumberLit:
        case NK::StringLit:
        case NK::BoolLit:
        case NK::NullLit: return literalOf(n);
        case NK::TemplateLit:
            for (const NodePtr& part : n.list)
                checkExpr(*part);
            return arena_.stringType();
        case NK::Identifier: {
            if (const Ty* narrowed = narrowedType(n.text))
                return narrowed;
            ValueBinding* binding = findValue(n.text);
            if (!binding) {
                report(n, "cannot find name '" + toUtf8(n.text) + "'");
                return arena_.anyType();
            }
            return typeOfBinding(*binding);
        }
        case NK::ThisExpr: {
            if (const Ty* narrowed = narrowedType(ascii("this")))
                return narrowed;
            ValueBinding* binding = findValue(ascii("this"));
            return binding ? typeOfBinding(*binding) : arena_.anyType();
        }
        case NK::SuperExpr: return arena_.anyType();
        case NK::ArrayLit: {
            const Ty* element =
                contextual && contextual->kind == TK::Array ? contextual->element : nullptr;
            std::vector<const Ty*> items;
            bool spread = false;
            for (const NodePtr& e : n.list) {
                if (e->kind == NK::Hole) {
                    items.push_back(arena_.undefinedType());
                    continue;
                }
                if (e->kind == NK::Spread) {
                    spread = true;
                    const Ty* inner = e->a ? checkExpr(*e->a) : arena_.anyType();
                    items.push_back(elementTypeOf(inner));
                    continue;
                }
                const Ty* item = checkExpr(*e, element);
                // A fresh literal in a mutable slot widens: `[1, 2]` is
                // `number[]`, not `(1 | 2)[]`, or nothing could be pushed to it.
                items.push_back(element || constDepth_ > 0 ? item : widen(item));
            }
            if (contextual && contextual->kind == TK::Tuple && !spread &&
                items.size() == contextual->items.size())
                return arena_.tupleOf(std::move(items));
            if (constDepth_ > 0 && !spread)
                return arena_.tupleOf(std::move(items), true);
            if (items.empty())
                return arena_.arrayOf(element ? element : arena_.anyType());
            return arena_.arrayOf(arena_.unionOf(std::move(items)));
        }
        case NK::ObjectLit: {
            Ty built;
            for (const NodePtr& prop : n.list) {
                if (prop->propKind == PropKind::Spread) {
                    const Ty* inner = prop->a ? checkExpr(*prop->a) : nullptr;
                    if (inner && inner->kind == TK::Object) {
                        for (const TyMember& m : inner->members)
                            built.members.push_back(m);
                    }
                    continue;
                }
                if (prop->computed) {
                    if (prop->a)
                        checkExpr(*prop->a);
                    if (prop->b)
                        built.stringIndex =
                            arena_.unionOf({built.stringIndex, checkExpr(*prop->b)});
                    continue;
                }
                const Ty* want = nullptr;
                if (contextual) {
                    // A contextual union offers a property if any constituent
                    // does, which is what keeps the literal types in
                    // `const s: Shape[] = [{ kind: 'circle', ... }]`.
                    std::vector<const Ty*> candidates;
                    eachConstituent(contextual, [&](const Ty* part) {
                        if (part->kind != TK::Object)
                            return;
                        for (const TyMember& m : part->members) {
                            if (m.name == prop->text)
                                candidates.push_back(m.type);
                        }
                    });
                    if (!candidates.empty())
                        want = arena_.unionOf(std::move(candidates));
                }
                const Ty* value = prop->b ? checkExpr(*prop->b, want) : arena_.anyType();
                if (prop->propKind == PropKind::Getter && value && value->callable())
                    value = value->calls.front().returnType;
                if (!want && constDepth_ == 0)
                    value = widen(value);
                auto existing =
                    std::find_if(built.members.begin(), built.members.end(),
                                 [&](const TyMember& o) { return o.name == prop->text; });
                if (existing != built.members.end())
                    existing->type = value;
                else
                    built.members.push_back(TyMember{prop->text, value, false, false});
            }
            return arena_.object(std::move(built));
        }
        case NK::FunctionExpr:
        case NK::ArrowExpr: {
            // A contextual function type supplies the parameter types an arrow
            // leaves out, which is what makes `xs.map(x => x + 1)` work.
            const TySignature* want =
                contextual && contextual->callable() ? &contextual->calls.front() : nullptr;
            const Ty* thisType = n.kind == NK::ArrowExpr && !functions_.empty()
                                     ? functions_.back().thisType
                                     : nullptr;
            const Ty* type = typeOfFunction(n, thisType, want);
            checkFunctionBody(n, type, thisType);
            return type;
        }
        case NK::ClassExpr: {
            const Ty* statics = typeOfClass(n);
            if (!statics->constructs.empty())
                checkClassBody(n, statics->constructs.front().returnType, statics);
            return statics;
        }
        case NK::Unary: {
            const Ty* operand = n.a ? checkExpr(*n.a) : arena_.anyType();
            switch (n.op) {
                case Tok::Bang: return arena_.booleanType();
                case Tok::TypeOf: return arena_.stringType();
                case Tok::Void: return arena_.undefinedType();
                case Tok::Delete: return arena_.booleanType();
                case Tok::Minus:
                case Tok::Plus:
                case Tok::Tilde:
                    if (operand->kind != TK::Any && !assignable(operand, arena_.numberType()))
                        report(n, "operand of a unary arithmetic operator must be a number");
                    return arena_.numberType();
                default: return arena_.anyType();
            }
        }
        case NK::Update: {
            const Ty* operand = n.a ? checkExpr(*n.a) : arena_.anyType();
            if (operand->kind != TK::Any && !assignable(operand, arena_.numberType()))
                report(n, "'++' and '--' need a number");
            return arena_.numberType();
        }
        case NK::Binary: {
            const Ty* left = n.a ? checkExpr(*n.a) : arena_.anyType();
            const Ty* right = n.b ? checkExpr(*n.b) : arena_.anyType();
            switch (n.op) {
                case Tok::Plus: {
                    const bool leftString = assignable(left, arena_.stringType());
                    const bool rightString = assignable(right, arena_.stringType());
                    if (leftString || rightString)
                        return left->kind == TK::Any || right->kind == TK::Any
                                   ? arena_.anyType()
                                   : arena_.stringType();
                    if (left->kind != TK::Any && right->kind != TK::Any &&
                        (!assignable(left, arena_.numberType()) ||
                         !assignable(right, arena_.numberType())))
                        report(n, "'+' needs two numbers or a string");
                    return arena_.numberType();
                }
                case Tok::Minus:
                case Tok::Star:
                case Tok::Slash:
                case Tok::Percent:
                case Tok::StarStar:
                case Tok::Shl:
                case Tok::Shr:
                case Tok::UShr:
                case Tok::Amp:
                case Tok::Pipe:
                case Tok::Caret:
                    for (const Ty* side : {left, right}) {
                        if (side->kind != TK::Any && !assignable(side, arena_.numberType()))
                            report(n, "arithmetic needs numbers, got '" +
                                          toUtf8(TypeArena::describe(side)) + "'");
                    }
                    return arena_.numberType();
                case Tok::Lt:
                case Tok::Gt:
                case Tok::Le:
                case Tok::Ge: return arena_.booleanType();
                case Tok::EqEqEq:
                case Tok::NotEqEq:
                    // Two types with nothing in common can never be equal, and
                    // saying so catches a whole class of refactoring mistakes.
                    if (left->kind != TK::Any && right->kind != TK::Any &&
                        !assignable(left, right) && !assignable(right, left))
                        report(n, "'" + toUtf8(TypeArena::describe(left)) + "' and '" +
                                      toUtf8(TypeArena::describe(right)) + "' have no overlap");
                    return arena_.booleanType();
                case Tok::EqEq:
                case Tok::NotEq:
                case Tok::In:
                case Tok::InstanceOf: return arena_.booleanType();
                default: return arena_.anyType();
            }
        }
        case NK::Logical: {
            const Ty* left = n.a ? checkExpr(*n.a) : arena_.anyType();
            Facts narrowed;
            if (n.a)
                collectFacts(*n.a, n.op != Tok::OrOr, narrowed);
            facts_.push_back(std::move(narrowed));
            const Ty* right = n.b ? checkExpr(*n.b, contextual) : arena_.anyType();
            facts_.pop_back();
            if (n.op == Tok::QuestionQuestion) {
                const Ty* defined = filterUnion(arena_, left, [](const Ty* part) {
                    return part->kind != TK::Null && part->kind != TK::Undefined;
                });
                return arena_.unionOf({defined, right});
            }
            if (n.op == Tok::AndAnd)
                return arena_.unionOf({right, arena_.booleanLiteral(false)});
            return arena_.unionOf({left, right});
        }
        case NK::Assign: {
            const Ty* target = n.a ? checkExpr(*n.a) : arena_.anyType();
            if (n.a && n.op == Tok::Assign) {
                const String path = pathOf(*n.a);
                if (!path.empty()) {
                    if (ValueBinding* binding = findValue(path)) {
                        if (binding->isConst)
                            report(n, "cannot assign to '" + toUtf8(path) + "', a constant");
                    }
                }
            }
            const Ty* value = n.b ? checkExpr(*n.b, target) : arena_.anyType();
            if (n.op == Tok::Assign && n.b)
                requireAssignable(*n.b, value, target, "assignment type mismatch");
            return value;
        }
        case NK::Conditional: {
            if (n.a)
                checkExpr(*n.a);
            Facts whenTrue;
            Facts whenFalse;
            if (n.a) {
                collectFacts(*n.a, true, whenTrue);
                collectFacts(*n.a, false, whenFalse);
            }
            facts_.push_back(std::move(whenTrue));
            const Ty* then = n.b ? checkExpr(*n.b, contextual) : arena_.anyType();
            facts_.pop_back();
            facts_.push_back(std::move(whenFalse));
            const Ty* otherwise = n.c ? checkExpr(*n.c, contextual) : arena_.anyType();
            facts_.pop_back();
            return arena_.unionOf({then, otherwise});
        }
        case NK::Call: return checkCall(n, false);
        case NK::NewExpr: return checkCall(n, true);
        case NK::Member: {
            const Ty* object = n.a ? checkExpr(*n.a) : arena_.anyType();
            if (n.a && n.a->kind == NK::SuperExpr)
                return arena_.anyType();
            if (const Ty* narrowed = narrowedType(pathOf(n)))
                return narrowed;
            if (n.computed) {
                const Ty* key = n.b ? checkExpr(*n.b) : arena_.anyType();
                const Ty* apparent = apparentType(object);
                if (object->kind == TK::Tuple && key->kind == TK::NumberLit) {
                    const std::size_t index = std::size_t(key->number);
                    if (index < object->items.size())
                        return object->items[index];
                }
                if (key->kind == TK::StrLit)
                    return memberType(n, object, key->text, n.optional);
                if (apparent && apparent->kind == TK::Object) {
                    if (apparent->numberIndex && assignable(key, arena_.numberType()))
                        return apparent->numberIndex;
                    if (apparent->stringIndex)
                        return apparent->stringIndex;
                }
                return arena_.anyType();
            }
            return memberType(n, object, n.text, n.optional);
        }
        case NK::Sequence: {
            const Ty* last = arena_.anyType();
            for (const NodePtr& e : n.list)
                last = checkExpr(*e);
            return last;
        }
        case NK::Spread: return n.a ? checkExpr(*n.a) : arena_.anyType();
        case NK::AsExpr: {
            // `as const` keeps literal types; everything else asserts.
            if (n.type && n.type->kind == NK::TypeRef && n.type->text == ascii("const")) {
                ++constDepth_;
                const Ty* value = n.a ? checkExpr(*n.a) : arena_.anyType();
                --constDepth_;
                return value;
            }
            const Ty* value = n.a ? checkExpr(*n.a) : arena_.anyType();
            const Ty* target = resolveType(n.type.get());
            if (!target)
                return value;
            if (n.op == Tok::Satisfies) {
                requireAssignable(n, value, target, "value does not satisfy the type");
                return value;
            }
            if (value->kind != TK::Any && target->kind != TK::Any && !assignable(value, target) &&
                !assignable(target, value))
                reportMismatch(n, value, target, "this assertion can never hold");
            return target;
        }
        case NK::NonNull: {
            const Ty* value = n.a ? checkExpr(*n.a) : arena_.anyType();
            return filterUnion(arena_, value, [](const Ty* part) {
                return part->kind != TK::Null && part->kind != TK::Undefined;
            });
        }
        default: return arena_.anyType();
    }
}

// --------------------------------------------------------------- calls ---

bool Checker::inferTypeArguments(const TySignature& sig, const std::vector<const Ty*>& args,
                                 Substitution& out)
{
    for (std::size_t i = 0; i < sig.params.size() && i < args.size(); ++i)
        inferFrom(sig.params[i].type, args[i], sig.typeParams, out);
    for (std::size_t i = 0; i < sig.typeParams.size(); ++i) {
        auto found = out.find(sig.typeParams[i]);
        if (found == out.end())
            out[sig.typeParams[i]] = sig.constraints[i] ? sig.constraints[i] : nullptr;
    }
    return true;
}

void Checker::inferFrom(const Ty* parameter, const Ty* argument, const std::vector<String>& names,
                        Substitution& out)
{
    if (!parameter || !argument)
        return;
    if (parameter->kind == TK::Param) {
        if (std::find(names.begin(), names.end(), parameter->text) == names.end())
            return;
        if (!out.count(parameter->text))
            out[parameter->text] = argument;
        return;
    }
    if (parameter->kind == TK::Array) {
        if (argument->kind == TK::Array)
            inferFrom(parameter->element, argument->element, names, out);
        return;
    }
    if (parameter->kind == TK::Object && argument->kind == TK::Object) {
        for (const TyMember& want : parameter->members) {
            for (const TyMember& got : argument->members) {
                if (got.name == want.name)
                    inferFrom(want.type, got.type, names, out);
            }
        }
        for (std::size_t i = 0; i < parameter->calls.size() && i < argument->calls.size(); ++i) {
            const TySignature& a = parameter->calls[i];
            const TySignature& b = argument->calls[i];
            for (std::size_t p = 0; p < a.params.size() && p < b.params.size(); ++p)
                inferFrom(a.params[p].type, b.params[p].type, names, out);
            inferFrom(a.returnType, b.returnType, names, out);
        }
    }
}

const Ty* Checker::callSignature(const Node& n, const Ty* callee, const TySignature& original,
                                 bool isNew)
{
    TySignature sig = original;

    // Explicit type arguments win; otherwise the arguments are matched against
    // the parameter types to work the type variables out.
    Substitution map;
    if (!sig.typeParams.empty()) {
        if (!n.typeParams.empty()) {
            for (std::size_t i = 0; i < sig.typeParams.size(); ++i) {
                map[sig.typeParams[i]] =
                    i < n.typeParams.size() ? resolveType(n.typeParams[i].get()) : arena_.anyType();
            }
        } else {
            std::vector<const Ty*> argTypes;
            for (const NodePtr& argument : n.list) {
                argTypes.push_back(argument->kind == NK::Spread ? arena_.anyType()
                                                                : checkExpr(*argument));
            }
            inferTypeArguments(sig, argTypes, map);
        }
        for (const String& name : sig.typeParams) {
            if (!map[name])
                map[name] = arena_.unknownType();
        }
        // An inferred type argument still has to satisfy its constraint;
        // otherwise `f<T extends string>(1)` would quietly infer `T = 1`.
        for (std::size_t i = 0; i < sig.typeParams.size(); ++i) {
            const Ty* constraint = sig.constraints[i];
            const Ty* argument = map[sig.typeParams[i]];
            if (constraint && argument && !assignable(argument, constraint))
                reportMismatch(n, argument, constraint, "type argument mismatch");
        }
        for (TyParam& p : sig.params)
            p.type = substitute(p.type, map);
        sig.returnType = substitute(sig.returnType, map);
    }

    std::size_t required = 0;
    bool variadic = false;
    for (const TyParam& p : sig.params) {
        variadic = variadic || p.rest;
        if (!p.optional && !p.rest)
            ++required;
    }
    bool spread = false;
    for (const NodePtr& argument : n.list)
        spread = spread || argument->kind == NK::Spread;

    if (!spread && n.list.size() < required) {
        report(n, "expected " + std::to_string(required) + " argument(s), got " +
                      std::to_string(n.list.size()));
    } else if (!spread && !variadic && n.list.size() > sig.params.size()) {
        report(n, "expected at most " + std::to_string(sig.params.size()) + " argument(s), got " +
                      std::to_string(n.list.size()));
    }

    for (std::size_t i = 0; i < n.list.size(); ++i) {
        const Node& argument = *n.list[i];
        const TyParam* p = nullptr;
        if (i < sig.params.size())
            p = &sig.params[i];
        else if (!sig.params.empty() && sig.params.back().rest)
            p = &sig.params.back();
        const Ty* want = p ? p->type : nullptr;
        if (p && p->rest && want && want->kind == TK::Array)
            want = want->element;
        if (argument.kind == NK::Spread) {
            if (argument.a)
                checkExpr(*argument.a);
            continue;
        }
        const Ty* got = checkExpr(argument, want);
        if (want)
            requireAssignable(argument, got, want,
                              ("argument " + std::to_string(i + 1) + " type mismatch").c_str());
    }
    (void)callee;
    (void)isNew;
    return sig.returnType ? sig.returnType : arena_.anyType();
}

const Ty* Checker::checkCall(const Node& n, bool isNew)
{
    const Ty* callee = n.a ? checkExpr(*n.a) : arena_.anyType();
    if (n.a && n.a->kind == NK::SuperExpr) {
        for (const NodePtr& argument : n.list)
            checkExpr(*argument);
        return arena_.anyType();
    }
    if (!callee || callee->kind == TK::Any) {
        for (const NodePtr& argument : n.list)
            checkExpr(*argument);
        return arena_.anyType();
    }
    if (n.optional && isNullish(callee)) {
        callee = filterUnion(arena_, callee, [](const Ty* part) {
            return part->kind != TK::Null && part->kind != TK::Undefined;
        });
    }

    const std::vector<TySignature>& signatures = isNew ? callee->constructs : callee->calls;
    if (signatures.empty()) {
        // A constructor with no construct signature may still be a plain
        // function used with `new`, which the runtime allows.
        if (isNew && callee->callable())
            return callSignature(n, callee, callee->calls.front(), true);
        report(n, "'" + toUtf8(TypeArena::describe(callee)) + "' is not " +
                      (isNew ? "constructible" : "callable"));
        for (const NodePtr& argument : n.list)
            checkExpr(*argument);
        return arena_.anyType();
    }

    if (signatures.size() > 1) {
        // Overloads: the first one whose arity fits wins. Argument types are
        // then checked against it, so the diagnostic names a real candidate.
        for (const TySignature& candidate : signatures) {
            std::size_t required = 0;
            bool variadic = false;
            for (const TyParam& p : candidate.params) {
                variadic = variadic || p.rest;
                if (!p.optional && !p.rest)
                    ++required;
            }
            if (n.list.size() >= required && (variadic || n.list.size() <= candidate.params.size()))
                return callSignature(n, callee, candidate, isNew);
        }
    }
    const Ty* result = callSignature(n, callee, signatures.front(), isNew);
    return n.optional ? arena_.unionOf({result, arena_.undefinedType()}) : result;
}

// ----------------------------------------------------------- narrowing ---

const Ty* Checker::typeOfPath(const Node& n)
{
    const String path = pathOf(n);
    if (path.empty())
        return nullptr;
    if (const Ty* narrowed = narrowedType(path))
        return narrowed;
    if (n.kind == NK::Identifier) {
        ValueBinding* binding = findValue(n.text);
        return binding ? typeOfBinding(*binding) : nullptr;
    }
    if (n.kind == NK::Member && n.a) {
        const Ty* object = typeOfPath(*n.a);
        if (!object)
            return nullptr;
        const Ty* apparent = apparentType(object);
        if (!apparent || apparent->kind != TK::Object)
            return nullptr;
        for (const TyMember& m : apparent->members) {
            if (m.name == n.text)
                return m.type;
        }
    }
    return nullptr;
}

void Checker::narrowByTypeOf(const Node& operand, StringView name, bool equal, Facts& out)
{
    const String path = pathOf(operand);
    if (path.empty())
        return;
    const Ty* current = typeOfPath(operand);
    if (!current)
        return;

    auto matches = [&](const Ty* part) {
        switch (part->kind) {
            case TK::Str:
            case TK::StrLit: return name == ascii("string");
            case TK::Number:
            case TK::NumberLit: return name == ascii("number");
            case TK::Boolean:
            case TK::BooleanLit: return name == ascii("boolean");
            case TK::Symbol: return name == ascii("symbol");
            case TK::Undefined:
            case TK::Void: return name == ascii("undefined");
            case TK::Null: return name == ascii("object");
            case TK::Array:
            case TK::Tuple: return name == ascii("object");
            case TK::Object:
                return part->callable() || !part->constructs.empty() ? name == ascii("function")
                                                                     : name == ascii("object");
            default: return true;
        }
    };

    if (current->kind == TK::Any || current->kind == TK::Unknown) {
        if (!equal)
            return;
        if (name == ascii("string"))
            out[path] = arena_.stringType();
        else if (name == ascii("number"))
            out[path] = arena_.numberType();
        else if (name == ascii("boolean"))
            out[path] = arena_.booleanType();
        else if (name == ascii("undefined"))
            out[path] = arena_.undefinedType();
        return;
    }
    out[path] =
        filterUnion(arena_, current, [&](const Ty* part) { return matches(part) == equal; });
}

void Checker::narrowByTruth(const Node& operand, bool truthy, Facts& out)
{
    const String path = pathOf(operand);
    if (path.empty())
        return;
    const Ty* current = typeOfPath(operand);
    if (!current || current->kind == TK::Any)
        return;
    if (truthy) {
        out[path] = filterUnion(arena_, current, [](const Ty* part) {
            if (part->kind == TK::Null || part->kind == TK::Undefined || part->kind == TK::Void)
                return false;
            if (part->kind == TK::BooleanLit)
                return part->boolValue;
            if (part->kind == TK::NumberLit)
                return part->number != 0;
            if (part->kind == TK::StrLit)
                return !part->text.empty();
            return true;
        });
        return;
    }
    out[path] = filterUnion(arena_, current, [](const Ty* part) {
        switch (part->kind) {
            case TK::Object:
            case TK::Array:
            case TK::Tuple: return false;  // an object is always truthy
            case TK::BooleanLit: return !part->boolValue;
            case TK::NumberLit: return part->number == 0;
            case TK::StrLit: return part->text.empty();
            default: return true;
        }
    });
}

void Checker::narrowByLiteral(const Node& subject, const Node& literal, bool equal, Facts& out)
{
    const Ty* value = literalOf(literal);
    if (!value && literal.kind == NK::Identifier && literal.text == ascii("undefined"))
        value = arena_.undefinedType();
    if (!value && literal.kind == NK::Member) {
        // A discriminant may be spelled as an enum member.
        value = typeOfPath(literal);
        if (value && !value->isLiteral())
            value = nullptr;
    }
    if (!value)
        return;

    // `x === 'a'` narrows `x`; `x.kind === 'a'` narrows `x` through its
    // discriminant, which is what makes tagged unions usable.
    const String path = pathOf(subject);
    if (!path.empty()) {
        if (const Ty* current = typeOfPath(subject)) {
            if (current->kind != TK::Any) {
                out[path] = filterUnion(arena_, current, [&](const Ty* part) {
                    Assignability relation(options_.strictNullChecks);
                    const bool overlaps =
                        relation.holds(part, value) || relation.holds(value, part);
                    return overlaps == equal;
                });
            }
        }
    }
    if (subject.kind != NK::Member || subject.computed || !subject.a)
        return;
    const String basePath = pathOf(*subject.a);
    if (basePath.empty())
        return;
    const Ty* base = typeOfPath(*subject.a);
    if (!base || base->kind != TK::Union)
        return;
    out[basePath] = filterUnion(arena_, base, [&](const Ty* part) {
        const Ty* apparent = apparentType(part);
        if (!apparent || apparent->kind != TK::Object)
            return true;
        for (const TyMember& m : apparent->members) {
            if (m.name != subject.text)
                continue;
            Assignability relation(options_.strictNullChecks);
            const bool overlaps = relation.holds(value, m.type) || relation.holds(m.type, value);
            return overlaps == equal;
        }
        return !equal;
    });
}

void Checker::collectFacts(const Node& condition, bool whenTrue, Facts& out)
{
    switch (condition.kind) {
        case NK::Unary:
            if (condition.op == Tok::Bang && condition.a)
                collectFacts(*condition.a, !whenTrue, out);
            return;
        case NK::Logical:
            if (!condition.a || !condition.b)
                return;
            // `a && b` tells us both hold when true; `a || b` both fail when
            // false. The other direction says nothing either way.
            if ((condition.op == Tok::AndAnd && whenTrue) ||
                (condition.op == Tok::OrOr && !whenTrue)) {
                collectFacts(*condition.a, whenTrue, out);
                collectFacts(*condition.b, whenTrue, out);
            }
            return;
        case NK::Binary: {
            if (!condition.a || !condition.b)
                return;
            const bool equality = condition.op == Tok::EqEqEq || condition.op == Tok::EqEq;
            const bool inequality = condition.op == Tok::NotEqEq || condition.op == Tok::NotEq;
            if (!equality && !inequality) {
                if (condition.op == Tok::InstanceOf && whenTrue) {
                    const Ty* constructor = typeOfPath(*condition.b);
                    if (constructor && !constructor->constructs.empty()) {
                        const String path = pathOf(*condition.a);
                        if (!path.empty())
                            out[path] = constructor->constructs.front().returnType;
                    }
                }
                return;
            }
            const bool positive = equality == whenTrue;

            // `typeof x === 'string'`
            for (int side = 0; side < 2; ++side) {
                const Node& maybeTypeOf = side == 0 ? *condition.a : *condition.b;
                const Node& maybeName = side == 0 ? *condition.b : *condition.a;
                if (maybeTypeOf.kind == NK::Unary && maybeTypeOf.op == Tok::TypeOf &&
                    maybeTypeOf.a && maybeName.kind == NK::StringLit) {
                    narrowByTypeOf(*maybeTypeOf.a, maybeName.text, positive, out);
                    return;
                }
            }
            // `x === null`, `x !== undefined`, `x.kind === 'a'`
            narrowByLiteral(*condition.a, *condition.b, positive, out);
            narrowByLiteral(*condition.b, *condition.a, positive, out);
            return;
        }
        case NK::Call: {
            // A user-defined type guard: `function isFoo(x): x is Foo`.
            if (!whenTrue || !condition.a)
                return;
            const Ty* callee = typeOfPath(*condition.a);
            if (!callee || callee->calls.empty())
                return;
            const TySignature& sig = callee->calls.front();
            if (sig.predicateOn.empty() || !sig.predicateType)
                return;
            for (std::size_t i = 0; i < sig.params.size() && i < condition.list.size(); ++i) {
                if (sig.params[i].name != sig.predicateOn)
                    continue;
                const String path = pathOf(*condition.list[i]);
                if (!path.empty())
                    out[path] = sig.predicateType;
            }
            return;
        }
        default: narrowByTruth(condition, whenTrue, out); return;
    }
}

}  // namespace

std::vector<Diagnostic> checkProgram(const Node& program, const std::string& sourceName,
                                     const CheckOptions& options,
                                     const std::vector<String>& hostGlobals,
                                     const std::string& ambient)
{
    Checker checker(sourceName, options);
    Parser lib(globalDeclarations(), "<lib>");
    const NodePtr declarations = lib.parseProgram();
    checker.declareGlobals(*declarations);
    NodePtr hostDeclarations;
    if (!ambient.empty()) {
        Parser host(ambient, "<declare>");
        hostDeclarations = host.parseProgram();
        checker.declareGlobals(*hostDeclarations);
    }
    checker.declareHostGlobals(hostGlobals);
    checker.run(program);
    return checker.take();
}

}  // namespace dyn
}  // namespace internal
}  // namespace script
