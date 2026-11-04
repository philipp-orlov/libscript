// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The type table: the core library's types, the ones an assembly declares,
// and the conversion rules that relate them.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "script/internal/common.hpp"
#include "script/internal/typed/metadata.hpp"

namespace script {
namespace internal {
namespace typed {

struct Runtime;

/// Where `ConvKind` alone is not enough: whether the conversion exists at all,
/// and whether the language insists on an explicit `(T)`.
struct ConvKindResult
{
    bool exists = false;
    bool requiresCast = false;
    ConvKind kind = ConvKind::Identity;
};

/// Every type the engine defines itself, reachable without a lookup.
struct CoreTypes
{
    const TypeInfo* voidType = nullptr;
    const TypeInfo* boolType = nullptr;
    const TypeInfo* sbyteType = nullptr;
    const TypeInfo* byteType = nullptr;
    const TypeInfo* shortType = nullptr;
    const TypeInfo* ushortType = nullptr;
    const TypeInfo* intType = nullptr;
    const TypeInfo* uintType = nullptr;
    const TypeInfo* longType = nullptr;
    const TypeInfo* ulongType = nullptr;
    const TypeInfo* charType = nullptr;
    const TypeInfo* floatType = nullptr;
    const TypeInfo* doubleType = nullptr;
    const TypeInfo* stringType = nullptr;
    const TypeInfo* objectType = nullptr;
    const TypeInfo* nullType = nullptr;
    const TypeInfo* errorType = nullptr;
    const TypeInfo* exceptionType = nullptr;
    const TypeInfo* enumerableType = nullptr;  ///< IEnumerable, the foreach shape
    const TypeInfo* disposableType = nullptr;
};

/// Owns every `TypeInfo` the engine created and indexes them by name.
///
/// The table belongs to the context, so a type an assembly declares outlives
/// the compilation that produced it and can be named by the next one. An
/// assembly deregisters its types when it is dropped.
class TypeTable
{
public:
    explicit TypeTable(Runtime& runtime);
    ~TypeTable();
    TypeTable(const TypeTable&) = delete;
    TypeTable& operator=(const TypeTable&) = delete;

    const CoreTypes& core() const noexcept { return core_; }
    /// The core library installs itself, so it needs to fill these in.
    CoreTypes& mutableCore() noexcept { return core_; }
    void registerException(ErrorCode code, const TypeInfo* type) { exceptions_[int(code)] = type; }
    Runtime& runtime() const noexcept { return *rt_; }

    /// The type that stands for a primitive kind.
    const TypeInfo* forKind(TypeKind kind) const noexcept;
    /// Look a name up: fully qualified first, then each `using` namespace.
    const TypeInfo* find(StringView fullName) const noexcept;
    const TypeInfo* find(const char* asciiName) const noexcept { return find(ascii(asciiName)); }

    /// `T[]`, created once per element type and cached.
    const TypeInfo* arrayOf(const TypeInfo* element);
    /// `List<T>`, `Dictionary<K,V>`: constructed on demand, methods included.
    const TypeInfo* listOf(const TypeInfo* element);
    const TypeInfo* dictionaryOf(const TypeInfo* key, const TypeInfo* value);
    /// `KeyValuePair<K,V>`: what walking a dictionary yields. A tuple in every
    /// way that matters -- deconstruction, patterns, value equality -- with
    /// read-only `Key` and `Value` as its elements.
    const TypeInfo* keyValuePairOf(const TypeInfo* key, const TypeInfo* value);
    /// `IEnumerable<T>`: the element type is what makes `foreach` and the
    /// query operators know what they are walking.
    const TypeInfo* enumerableOf(const TypeInfo* element);
    /// `(int, string)`. Element names are aliases onto the same slots, so
    /// `p.x` and `p.Item1` are the same field.
    const TypeInfo* tupleOf(const std::vector<const TypeInfo*>& elements,
                            const std::vector<String>& names = {});
    /// `Func<...>` and `Action<...>`, which is how a lambda gets a type.
    const TypeInfo* funcOf(const std::vector<const TypeInfo*>& params, const TypeInfo* result);
    const TypeInfo* actionOf(const std::vector<const TypeInfo*>& params);

    /// Create a type the table owns. Used for the core library and for the
    /// built-in generics; an assembly owns the types it declares itself and
    /// registers them with `adopt`.
    TypeInfo* create(TypeKind kind, const char* fullName);
    void adopt(TypeInfo* type);
    void forget(TypeInfo* type) noexcept;

    /// Add a method implemented in C++. The parameter list is written the way
    /// the C# signature reads, so the checker sees no difference between this
    /// and a method the program declared.
    MethodInfo* addMethod(TypeInfo* owner, const char* name, const TypeInfo* returnType,
                          std::vector<ParamInfo> params, std::uint16_t flags, NativeFunction fn);
    PropertyInfo* addProperty(TypeInfo* owner, const char* name, const TypeInfo* type,
                              NativeFunction getter, NativeFunction setter, bool isStatic = false);
    ParamInfo param(const char* name, const TypeInfo* type,
                    ParamMode mode = ParamMode::Value) const;

    // ---- conversions ----

    /// Is a value of `from` assignable to a slot of `to` with no cast?
    bool implicitlyConverts(const TypeInfo* from, const TypeInfo* to) const noexcept;
    /// Is `(to)value` legal at all?
    bool explicitlyConverts(const TypeInfo* from, const TypeInfo* to) const noexcept;
    /// The kind of conversion the emitter has to perform.
    ConvKindResult classify(const TypeInfo* from, const TypeInfo* to) const noexcept;
    /// The type both operands of a binary operator are promoted to, or null
    /// when the operator does not apply.
    const TypeInfo* promote(const TypeInfo* a, const TypeInfo* b) const noexcept;
    /// `System.Int32` -> "int" where the language has a keyword for it.
    std::string describe(const TypeInfo* type) const;

    /// The exception type a runtime error code maps to.
    const TypeInfo* exceptionFor(ErrorCode code) const noexcept;
    ErrorCode errorCodeFor(const TypeInfo* type) const noexcept;

private:
    void installPrimitives();

    Runtime* rt_ = nullptr;
    CoreTypes core_;
    std::vector<std::unique_ptr<TypeInfo>> owned_;
    std::vector<std::unique_ptr<MethodInfo>> methods_;
    std::unordered_map<String, TypeInfo*> index_;
    std::unordered_map<const TypeInfo*, TypeInfo*> arrays_;
    std::unordered_map<String, TypeInfo*> constructed_;
    std::unordered_map<int, const TypeInfo*> exceptions_;
    const TypeInfo* byKind_[std::size_t(TypeKind::Error) + 1] = {};
};

/// Install the core library. Each of these lives in src/builtins.
void installCore(TypeTable& table);
void installString(TypeTable& table);
void installMath(TypeTable& table);
void installCollections(TypeTable& table);
/// Fill in a freshly created `List<T>` / `Dictionary<K,V>` instantiation.
void instantiateList(TypeTable& table, TypeInfo* type);
void instantiateDictionary(TypeTable& table, TypeInfo* type);
void instantiateTuple(TypeTable& table, TypeInfo* type);

}  // namespace typed
}  // namespace internal
}  // namespace script
