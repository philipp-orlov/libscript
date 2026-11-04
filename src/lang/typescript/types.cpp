// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The type lattice: construction, interning, printing and assignability.

#include <algorithm>

#include "script/internal/dyn/types.hpp"
#include "script/internal/dyn/numconv.hpp"

namespace script {
namespace internal {
namespace dyn {

namespace {

void appendParams(String& out, const TySignature& sig, bool forKey)
{
    out += u'(';
    for (std::size_t i = 0; i < sig.params.size(); ++i) {
        if (i)
            out += ascii(", ");
        const TyParam& p = sig.params[i];
        if (p.rest)
            out += ascii("...");
        out += p.name.empty() ? ascii("arg") : p.name;
        if (p.optional)
            out += u'?';
        out += ascii(": ");
        out += TypeArena::describe(p.type, forKey);
    }
    out += u')';
}

/// Object members are kept sorted so that two structurally equal types print
/// the same way and therefore intern to the same pointer.
void sortMembers(std::vector<TyMember>& members)
{
    std::sort(members.begin(), members.end(),
              [](const TyMember& a, const TyMember& b) { return a.name < b.name; });
}

}  // namespace

String TypeArena::describe(const Ty* t)
{
    return describe(t, false);
}

String TypeArena::describe(const Ty* t, bool forKey)
{
    if (!t)
        return ascii("any");
    switch (t->kind) {
        case TK::Any: return ascii("any");
        case TK::Unknown: return ascii("unknown");
        case TK::Never: return ascii("never");
        case TK::Void: return ascii("void");
        case TK::Undefined: return ascii("undefined");
        case TK::Null: return ascii("null");
        case TK::Boolean: return ascii("boolean");
        case TK::Number: return ascii("number");
        case TK::Str: return ascii("string");
        case TK::Symbol: return ascii("symbol");
        case TK::BooleanLit: return ascii(t->boolValue ? "true" : "false");
        case TK::NumberLit: return numberToString(t->number);
        case TK::StrLit: {
            String out = ascii("\"");
            out += t->text;
            out += u'"';
            return out;
        }
        case TK::Param:
            // Two type parameters may both be called `T` and mean different
            // things, so the key carries the identity the display leaves out.
            return forKey ? t->text + ascii("#") + numberToString(t->number) : t->text;
        case TK::Array: {
            String inner = describe(t->element, forKey);
            const bool needsParens =
                inner.find(u'|') != String::npos || inner.find(ascii("=>")) != String::npos;
            String out = t->readOnly ? ascii("readonly ") : String();
            if (needsParens)
                out += u'(';
            out += inner;
            if (needsParens)
                out += u')';
            out += ascii("[]");
            return out;
        }
        case TK::Tuple: {
            String out = t->readOnly ? ascii("readonly [") : ascii("[");
            for (std::size_t i = 0; i < t->items.size(); ++i) {
                if (i)
                    out += ascii(", ");
                out += describe(t->items[i], forKey);
            }
            out += u']';
            return out;
        }
        case TK::Union: {
            String out;
            for (std::size_t i = 0; i < t->items.size(); ++i) {
                if (i)
                    out += ascii(" | ");
                out += describe(t->items[i], forKey);
            }
            return out;
        }
        case TK::Object: break;
    }

    // A named object type prints as its name. That is what stops a recursive
    // interface from printing for ever, and it is also what makes the name the
    // interning key.
    if (!t->text.empty())
        return t->text;

    if (t->members.empty() && t->constructs.empty() && t->calls.size() == 1 && !t->stringIndex &&
        !t->numberIndex) {
        String out;
        appendParams(out, t->calls.front(), forKey);
        out += ascii(" => ");
        out += describe(t->calls.front().returnType, forKey);
        return out;
    }

    String out = ascii("{ ");
    for (const TySignature& sig : t->constructs) {
        out += ascii("new ");
        appendParams(out, sig, forKey);
        out += ascii(": ");
        out += describe(sig.returnType, forKey);
        out += ascii("; ");
    }
    for (const TySignature& sig : t->calls) {
        appendParams(out, sig, forKey);
        out += ascii(": ");
        out += describe(sig.returnType, forKey);
        out += ascii("; ");
    }
    if (t->stringIndex) {
        out += ascii("[key: string]: ");
        out += describe(t->stringIndex, forKey);
        out += ascii("; ");
    }
    if (t->numberIndex) {
        out += ascii("[index: number]: ");
        out += describe(t->numberIndex, forKey);
        out += ascii("; ");
    }
    for (const TyMember& m : t->members) {
        if (m.readOnly)
            out += ascii("readonly ");
        out += m.name;
        if (m.optional)
            out += u'?';
        out += ascii(": ");
        out += describe(m.type, forKey);
        out += ascii("; ");
    }
    out += u'}';
    return out;
}

TypeArena::TypeArena()
{
    auto primitive = [this](TK kind) {
        Ty t;
        t.kind = kind;
        return intern(std::move(t));
    };
    any_ = primitive(TK::Any);
    unknown_ = primitive(TK::Unknown);
    never_ = primitive(TK::Never);
    void_ = primitive(TK::Void);
    undefined_ = primitive(TK::Undefined);
    null_ = primitive(TK::Null);
    boolean_ = primitive(TK::Boolean);
    number_ = primitive(TK::Number);
    string_ = primitive(TK::Str);
    symbol_ = primitive(TK::Symbol);
}

Ty* TypeArena::make(Ty&& t)
{
    storage_.push_back(std::move(t));
    return &storage_.back();
}

const Ty* TypeArena::intern(Ty&& t)
{
    // The printed form is a faithful structural key, and the checker needs it
    // for diagnostics anyway, so there is no second hashing scheme to keep in
    // step with this one.
    std::string key = std::to_string(int(t.kind)) + ':' + toUtf8(describe(&t, true));
    auto it = interned_.find(key);
    if (it != interned_.end())
        return it->second;
    const Ty* made = make(std::move(t));
    interned_.emplace(std::move(key), made);
    return made;
}

const Ty* TypeArena::stringLiteral(StringView value)
{
    Ty t;
    t.kind = TK::StrLit;
    t.text = String(value);
    return intern(std::move(t));
}

const Ty* TypeArena::numberLiteral(double value)
{
    Ty t;
    t.kind = TK::NumberLit;
    t.number = value;
    return intern(std::move(t));
}

const Ty* TypeArena::booleanLiteral(bool value)
{
    Ty t;
    t.kind = TK::BooleanLit;
    t.boolValue = value;
    return intern(std::move(t));
}

const Ty* TypeArena::arrayOf(const Ty* element, bool readOnly)
{
    Ty t;
    t.kind = TK::Array;
    t.element = element ? element : any_;
    t.readOnly = readOnly;
    return intern(std::move(t));
}

const Ty* TypeArena::tupleOf(std::vector<const Ty*> items, bool readOnly)
{
    Ty t;
    t.kind = TK::Tuple;
    t.items = std::move(items);
    t.readOnly = readOnly;
    return intern(std::move(t));
}

const Ty* TypeArena::unionOf(std::vector<const Ty*> members)
{
    std::vector<const Ty*> flat;
    flat.reserve(members.size());
    for (const Ty* m : members) {
        if (!m || m->kind == TK::Never)
            continue;
        if (m->kind == TK::Any)
            return any_;
        if (m->kind == TK::Union) {
            for (const Ty* inner : m->items)
                flat.push_back(inner);
            continue;
        }
        flat.push_back(m);
    }
    // Interned members mean duplicate removal is a pointer comparison.
    std::sort(flat.begin(), flat.end());
    flat.erase(std::unique(flat.begin(), flat.end()), flat.end());
    if (flat.empty())
        return never_;
    if (flat.size() == 1)
        return flat.front();

    Ty t;
    t.kind = TK::Union;
    t.items = std::move(flat);
    return intern(std::move(t));
}

const Ty* TypeArena::functionOf(TySignature signature)
{
    // A null type prints as `any`, so leaving one in place would let a
    // signature intern under a name that does not describe it.
    if (!signature.returnType)
        signature.returnType = any_;
    for (TyParam& p : signature.params) {
        if (!p.type)
            p.type = any_;
    }
    Ty t;
    t.kind = TK::Object;
    t.calls.push_back(std::move(signature));
    return intern(std::move(t));
}

const Ty* TypeArena::paramOf(StringView name, const Ty* constraint)
{
    // `T` in one declaration and `T` in another are different types even though
    // they print the same, so each (name, constraint) pair gets its own id.
    const std::string key = toUtf8(String(name)) + '|' + toUtf8(describe(constraint, true));
    auto found = paramIds_.find(key);
    if (found == paramIds_.end())
        found = paramIds_.emplace(key, nextParamId_++).first;

    Ty t;
    t.kind = TK::Param;
    t.text = String(name);
    t.number = found->second;
    t.constraint = constraint;
    return intern(std::move(t));
}

const Ty* TypeArena::object(Ty prototype)
{
    prototype.kind = TK::Object;
    sortMembers(prototype.members);
    return intern(std::move(prototype));
}

Ty* TypeArena::makeNamed(StringView name)
{
    Ty t;
    t.kind = TK::Object;
    t.text = String(name);
    return make(std::move(t));
}

void TypeArena::fill(Ty* shell, Ty&& contents)
{
    const String name = shell->text;
    *shell = std::move(contents);
    shell->kind = TK::Object;
    shell->text = name;
    sortMembers(shell->members);
}

Ty* TypeArena::openObject(StringView name)
{
    Ty t;
    t.kind = TK::Object;
    t.text = String(name);
    Ty* open = make(std::move(t));
    // Register the name now so that members mentioning it resolve to this very
    // type while it is still being filled in.
    std::string key = std::to_string(int(TK::Object)) + ':' + toUtf8(String(name));
    interned_.emplace(std::move(key), open);
    open_.push_back(open);
    return open;
}

const Ty* TypeArena::close(Ty* open)
{
    sortMembers(open->members);
    open_.erase(std::remove(open_.begin(), open_.end(), open), open_.end());
    return open;
}

bool TypeArena::isOpen(const Ty* t) const
{
    return std::find(open_.begin(), open_.end(), t) != open_.end();
}

// ------------------------------------------------------- assignability ---

const TyMember* Assignability::findMember(const Ty* t, StringView name) const
{
    if (!t || t->kind != TK::Object)
        return nullptr;
    for (const TyMember& m : t->members) {
        if (m.name == name)
            return &m;
    }
    return nullptr;
}

bool Assignability::holds(const Ty* from, const Ty* to)
{
    if (from == to)
        return true;
    if (!from || !to)
        return true;
    for (const auto& pair : inProgress_) {
        if (pair.first == from && pair.second == to)
            return true;  // coinduction: an unbroken cycle is a match
    }
    inProgress_.emplace_back(from, to);
    const bool result = compare(from, to);
    inProgress_.pop_back();
    return result;
}

bool Assignability::compare(const Ty* from, const Ty* to)
{
    if (from->kind == TK::Any || to->kind == TK::Any)
        return true;
    if (from->kind == TK::Never)
        return true;
    if (to->kind == TK::Unknown)
        return true;
    if (from->kind == TK::Unknown)
        return false;
    // Without `strictNullChecks`, `null` and `undefined` belong to every type.
    if (!strict_ &&
        (from->kind == TK::Null || from->kind == TK::Undefined || from->kind == TK::Void))
        return true;

    // A union source has to satisfy the target through every constituent; a
    // union target needs only one that fits.
    if (from->kind == TK::Union) {
        for (const Ty* part : from->items) {
            if (!holds(part, to))
                return false;
        }
        return true;
    }
    if (to->kind == TK::Union) {
        for (const Ty* part : to->items) {
            if (holds(from, part))
                return true;
        }
        return false;
    }

    // A literal is its base type, narrowed.
    if (from->kind == TK::StrLit && to->kind == TK::Str)
        return true;
    if (from->kind == TK::NumberLit && to->kind == TK::Number)
        return true;
    if (from->kind == TK::BooleanLit && to->kind == TK::Boolean)
        return true;
    if (from->kind == TK::Void && to->kind == TK::Undefined)
        return true;
    if (from->kind == TK::Undefined && to->kind == TK::Void)
        return true;
    // Two different literals are never interchangeable. Identical ones were
    // caught by the pointer comparison in `holds`, so reaching here means they
    // differ -- which is what makes `'a' === 'b'` a reportable mistake.
    if (from->isLiteral() || to->isLiteral())
        return false;

    if (from->kind == TK::Param)
        return from->constraint && holds(from->constraint, to);
    if (to->kind == TK::Param)
        return false;

    if (from->kind == TK::Tuple && to->kind == TK::Array) {
        if (to->readOnly || !from->readOnly) {
            for (const Ty* item : from->items) {
                if (!holds(item, to->element))
                    return false;
            }
            return true;
        }
        return false;
    }
    if (from->kind == TK::Tuple && to->kind == TK::Tuple) {
        if (from->items.size() != to->items.size())
            return false;
        for (std::size_t i = 0; i < from->items.size(); ++i) {
            if (!holds(from->items[i], to->items[i]))
                return false;
        }
        return true;
    }
    if (from->kind == TK::Array && to->kind == TK::Array) {
        if (from->readOnly && !to->readOnly)
            return false;
        // Arrays are covariant. Unsound, and the same choice every mainstream
        // TypeScript checker makes, because the sound alternative rejects the
        // code people actually write.
        return holds(from->element, to->element);
    }
    if (from->kind == TK::Array && to->kind == TK::Object && to->members.empty() && !to->callable())
        return true;

    if (from->kind != TK::Object || to->kind != TK::Object)
        return from->kind == to->kind;

    for (const TyMember& want : to->members) {
        const TyMember* got = findMember(from, want.name);
        if (!got) {
            if (want.optional)
                continue;
            if (from->stringIndex && holds(from->stringIndex, want.type))
                continue;
            return false;
        }
        if (!want.optional && got->optional)
            return false;
        if (!holds(got->type, want.type))
            return false;
    }
    if (to->stringIndex) {
        if (from->stringIndex) {
            if (!holds(from->stringIndex, to->stringIndex))
                return false;
        } else {
            for (const TyMember& got : from->members) {
                if (!holds(got.type, to->stringIndex))
                    return false;
            }
        }
    }
    for (const TySignature& want : to->calls) {
        bool matched = false;
        for (const TySignature& got : from->calls)
            matched = matched || signatureHolds(got, want);
        if (!matched)
            return false;
    }
    for (const TySignature& want : to->constructs) {
        bool matched = false;
        for (const TySignature& got : from->constructs)
            matched = matched || signatureHolds(got, want);
        if (!matched)
            return false;
    }
    return true;
}

bool Assignability::signatureHolds(const TySignature& from, const TySignature& to)
{
    // A function that ignores trailing arguments still satisfies a signature
    // that supplies them, which is why callbacks like `map(x => x)` type-check.
    std::size_t required = 0;
    for (const TyParam& p : from.params) {
        if (!p.optional && !p.rest)
            ++required;
    }
    if (required > to.params.size())
        return false;
    const std::size_t shared = std::min(from.params.size(), to.params.size());
    for (std::size_t i = 0; i < shared; ++i) {
        // Parameters are compared bivariantly, as TypeScript does for methods.
        if (!holds(to.params[i].type, from.params[i].type) &&
            !holds(from.params[i].type, to.params[i].type))
            return false;
    }
    if (to.returnType && to.returnType->kind == TK::Void)
        return true;
    return holds(from.returnType, to.returnType);
}

// ------------------------------------------------------------ helpers ---

bool isNullish(const Ty* t) noexcept
{
    if (!t)
        return false;
    if (t->kind == TK::Null || t->kind == TK::Undefined || t->kind == TK::Void ||
        t->kind == TK::Any || t->kind == TK::Unknown)
        return true;
    if (t->kind != TK::Union)
        return false;
    for (const Ty* part : t->items) {
        if (isNullish(part))
            return true;
    }
    return false;
}

void eachConstituent(const Ty* t, const std::function<void(const Ty*)>& fn)
{
    if (!t)
        return;
    if (t->kind != TK::Union) {
        fn(t);
        return;
    }
    for (const Ty* part : t->items)
        fn(part);
}

const Ty* filterUnion(TypeArena& arena, const Ty* t, const std::function<bool(const Ty*)>& keep)
{
    if (!t || t->kind == TK::Any)
        return t;
    std::vector<const Ty*> kept;
    eachConstituent(t, [&](const Ty* part) {
        if (keep(part))
            kept.push_back(part);
    });
    return arena.unionOf(std::move(kept));
}

}  // namespace dyn
}  // namespace internal
}  // namespace script
