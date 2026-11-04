// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The static type lattice.
//
// Types are immutable, interned and allocated from an arena that lives exactly
// as long as one check, so a program that is evaluated a thousand times never
// leaves type-checker allocations behind. Interning makes identity comparison
// the fast path of assignability and keeps a union of the same members from
// being built twice.
#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "script/internal/common.hpp"

namespace script {
namespace internal {
namespace dyn {

enum class TK : std::uint8_t
{
    Any,      ///< assignable both ways; the checker's escape hatch
    Unknown,  ///< everything is assignable to it, it to nothing
    Never,    ///< assignable to everything, nothing to it
    Void,
    Undefined,
    Null,
    Boolean,
    Number,
    Str,  ///< spelled `string`; abbreviated so it does not shadow `internal::String`
    Symbol,
    BooleanLit,
    NumberLit,
    StrLit,
    Object,  ///< members, index signatures, call and construct signatures
    Array,   ///< `element`
    Tuple,   ///< `items`
    Union,   ///< `items`, normalised and flattened
    Param,   ///< an unsubstituted type parameter
};

struct Ty;

struct TyMember
{
    String name;
    const Ty* type = nullptr;
    bool optional = false;
    bool readOnly = false;
};

struct TyParam
{
    String name;
    const Ty* type = nullptr;
    bool optional = false;
    bool rest = false;
};

/// One call signature. A generic signature names its type parameters here; the
/// call site infers them and substitutes before checking arguments.
struct TySignature
{
    std::vector<TyParam> params;
    const Ty* returnType = nullptr;
    std::vector<String> typeParams;
    std::vector<const Ty*> constraints;  ///< parallel to `typeParams`, may hold null
    /// `x is Foo`: the parameter this predicate narrows, empty when there is none.
    String predicateOn;
    const Ty* predicateType = nullptr;
};

/// One type. The unused fields of a given kind stay empty; the flat shape keeps
/// the checker to a single switch, exactly as the syntax tree does for the
/// compiler.
struct Ty
{
    TK kind = TK::Any;
    bool boolValue = false;  ///< BooleanLit
    bool readOnly = false;   ///< a `readonly` array or tuple
    double number = 0;       ///< NumberLit
    String text;             ///< StringLit value, or the name of a named object type

    const Ty* element = nullptr;    ///< Array
    std::vector<const Ty*> items;   ///< Tuple elements, Union members
    std::vector<TyMember> members;  ///< Object, sorted by name
    const Ty* stringIndex = nullptr;
    const Ty* numberIndex = nullptr;
    std::vector<TySignature> calls;
    std::vector<TySignature> constructs;
    const Ty* constraint = nullptr;  ///< Param

    bool isLiteral() const noexcept
    {
        return kind == TK::BooleanLit || kind == TK::NumberLit || kind == TK::StrLit;
    }
    bool callable() const noexcept { return !calls.empty(); }
};

/// Owns every type made during one check. `std::deque` is what makes this work:
/// it hands out stable addresses while allocating in blocks, so interned types
/// can be compared by pointer and the whole arena is released in one go.
class TypeArena
{
public:
    TypeArena();

    // ---- the types that have exactly one instance ----
    const Ty* anyType() const noexcept { return any_; }
    const Ty* unknownType() const noexcept { return unknown_; }
    const Ty* neverType() const noexcept { return never_; }
    const Ty* voidType() const noexcept { return void_; }
    const Ty* undefinedType() const noexcept { return undefined_; }
    const Ty* nullType() const noexcept { return null_; }
    const Ty* booleanType() const noexcept { return boolean_; }
    const Ty* numberType() const noexcept { return number_; }
    const Ty* stringType() const noexcept { return string_; }
    const Ty* symbolType() const noexcept { return symbol_; }

    const Ty* stringLiteral(StringView value);
    const Ty* numberLiteral(double value);
    const Ty* booleanLiteral(bool value);

    const Ty* arrayOf(const Ty* element, bool readOnly = false);
    const Ty* tupleOf(std::vector<const Ty*> items, bool readOnly = false);
    /// Flattens nested unions, drops duplicates and absorbs `never`. A union of
    /// one is that one type; a union containing `any` is `any`.
    const Ty* unionOf(std::vector<const Ty*> members);
    const Ty* functionOf(TySignature signature);
    const Ty* paramOf(StringView name, const Ty* constraint);
    /// Interns a freshly built object type. `name` makes it recursive-safe: a
    /// named type prints and interns as its name, so `interface N { next: N }`
    /// terminates.
    const Ty* object(Ty prototype);
    /// A named object type that members are filled into afterwards, so that an
    /// interface can refer to itself while it is being built.
    /// A named object that is filled in later and never interned. Generic
    /// instantiation needs one: `Map<string, number>` may mention itself, and a
    /// reference taken while it is being built must end up pointing at the
    /// finished type rather than at the shell.
    Ty* makeNamed(StringView name);
    void fill(Ty* shell, Ty&& contents);

    Ty* openObject(StringView name);
    const Ty* close(Ty* open);
    /// True while `t` is still being filled in. An instantiation of a type that
    /// mentions itself has to wait, or it would capture the empty shell.
    bool isOpen(const Ty* t) const;

    /// The printed form. `forKey` distinguishes types that display the same but
    /// are not the same, which is only ever true of type parameters.
    static String describe(const Ty* t);
    static String describe(const Ty* t, bool forKey);

private:
    const Ty* intern(Ty&& t);
    Ty* make(Ty&& t);

    std::deque<Ty> storage_;
    std::unordered_map<std::string, const Ty*> interned_;
    std::unordered_map<std::string, double> paramIds_;
    std::vector<const Ty*> open_;
    double nextParamId_ = 1;
    const Ty* any_ = nullptr;
    const Ty* unknown_ = nullptr;
    const Ty* never_ = nullptr;
    const Ty* void_ = nullptr;
    const Ty* undefined_ = nullptr;
    const Ty* null_ = nullptr;
    const Ty* boolean_ = nullptr;
    const Ty* number_ = nullptr;
    const Ty* string_ = nullptr;
    const Ty* symbol_ = nullptr;
};

/// Structural assignability, the one relation the checker is built on.
///
/// Recursive types would make a naive walk diverge, so pairs under test are
/// remembered and assumed to hold: a cycle that never reaches a mismatch is a
/// match, which is what coinduction means here and what every structural type
/// checker does.
class Assignability
{
public:
    explicit Assignability(bool strictNullChecks = true) : strict_(strictNullChecks) {}

    bool holds(const Ty* from, const Ty* to);

private:
    bool compare(const Ty* from, const Ty* to);
    bool signatureHolds(const TySignature& from, const TySignature& to);
    const TyMember* findMember(const Ty* t, StringView name) const;

    std::vector<std::pair<const Ty*, const Ty*>> inProgress_;
    bool strict_ = true;
};

/// True when `t` admits `undefined` or `null`.
bool isNullish(const Ty* t) noexcept;
/// Every constituent of a union, or the type itself.
void eachConstituent(const Ty* t, const std::function<void(const Ty*)>& fn);
/// The type a value of type `t` has once truthiness, `typeof` or a literal
/// comparison has ruled constituents out.
const Ty* filterUnion(TypeArena& arena, const Ty* t, const std::function<bool(const Ty*)>& keep);

}  // namespace dyn
}  // namespace internal
}  // namespace script
