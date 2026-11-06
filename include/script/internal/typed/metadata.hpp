// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The type universe and the compiled form of a program.
//
// One `TypeInfo` serves both halves of the engine: the checker asks it what
// converts to what, and the VM reads the same object to find a field slot or a
// vtable entry. There is no separate runtime type -- the metadata a compile
// produced is exactly what gets serialized, loaded and executed.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "script/internal/common.hpp"
#include "script/internal/language.hpp"

namespace script {
namespace internal {
namespace typed {

struct TypeInfo;
struct MethodInfo;
struct AssemblyImpl;
struct NativeCode;
struct Runtime;

// ---------------------------------------------------------------- kinds ---

enum class TypeKind : std::uint8_t
{
    Void,
    Bool,
    SByte,
    Byte,
    Short,
    UShort,
    Int,
    UInt,
    Long,
    ULong,
    Char,
    Float,
    Double,
    Enum,       ///< an integral type with named constants
    Struct,     ///< a user value type: copied on assignment
    String,
    Object,     ///< System.Object, the root of everything
    Class,
    Interface,
    Delegate,
    Array,
    Null,       ///< the type of the `null` literal; converts to any reference
    Error,      ///< a type the checker could not resolve; suppresses cascades
};

constexpr bool isIntegral(TypeKind k) noexcept
{
    return k >= TypeKind::SByte && k <= TypeKind::Char;
}
constexpr bool isFloating(TypeKind k) noexcept
{
    return k == TypeKind::Float || k == TypeKind::Double;
}
constexpr bool isNumeric(TypeKind k) noexcept
{
    return isIntegral(k) || isFloating(k) || k == TypeKind::Enum;
}
/// True for the types stored inline in a `Value`, with no heap cell behind.
constexpr bool isValueKind(TypeKind k) noexcept
{
    return k == TypeKind::Bool || isIntegral(k) || isFloating(k);
}
constexpr bool isReferenceKind(TypeKind k) noexcept
{
    return k == TypeKind::String || k == TypeKind::Object || k == TypeKind::Class ||
           k == TypeKind::Interface || k == TypeKind::Delegate || k == TypeKind::Array ||
           k == TypeKind::Null;
}
constexpr bool isUnsigned(TypeKind k) noexcept
{
    return k == TypeKind::Byte || k == TypeKind::UShort || k == TypeKind::UInt ||
           k == TypeKind::ULong || k == TypeKind::Char;
}
/// Width in bits of an integral kind, for truncation after arithmetic.
constexpr int bitWidth(TypeKind k) noexcept
{
    switch (k) {
        case TypeKind::SByte:
        case TypeKind::Byte: return 8;
        case TypeKind::Short:
        case TypeKind::UShort:
        case TypeKind::Char: return 16;
        case TypeKind::Int:
        case TypeKind::UInt: return 32;
        default: return 64;
    }
}

const char* kindName(TypeKind) noexcept;
/// The `Value` tag a slot of this static type holds at run time.
Type runtimeTag(TypeKind) noexcept;

enum class Access : std::uint8_t
{
    Private,
    Protected,
    Internal,
    Public,
};

/// A runtime type that is implemented in C++ rather than in C#.
enum class NativeKind : std::uint8_t
{
    None,
    List,           ///< System.Collections.Generic.List<T>
    Dictionary,     ///< System.Collections.Generic.Dictionary<K,V>
    StringBuilder,  ///< System.Text.StringBuilder
    Host,           ///< a C++ class exposed through `Class<T>`
    Tuple,          ///< `(A, B, ...)`, whose fields are Item1, Item2, ...
};

/// What the emitter has to do to turn a value of one type into another.
enum class ConvKind : std::uint8_t
{
    Identity,
    Numeric,       ///< a widening or narrowing between numeric types
    Boxing,        ///< a value type into `object`
    Unboxing,
    Reference,     ///< up or down a class hierarchy
    MethodGroup,   ///< a method name into a delegate
    NullLiteral,
    EnumToInt,
    IntToEnum,
    ToStringCall,  ///< anything into `string`, for `+` and interpolation
};

// -------------------------------------------------------------- members ---

struct FieldInfo
{
    String name;
    const TypeInfo* type = nullptr;
    const TypeInfo* owner = nullptr;
    /// Instance fields index the object's slot array, static fields index the
    /// declaring assembly's static store. Both are decided at compile time, so
    /// a field access is an indexed load with no lookup.
    std::uint32_t slot = 0;
    Access access = Access::Private;
    bool isStatic = false;
    bool isReadOnly = false;
    bool isConst = false;
    Value constValue;  ///< the folded value of a `const` field
};

enum class ParamMode : std::uint8_t
{
    Value,
    Ref,
    Out,
    Params,  ///< trailing `params T[]`
};

struct ParamInfo
{
    String name;
    const TypeInfo* type = nullptr;
    ParamMode mode = ParamMode::Value;
    bool hasDefault = false;
    Value defaultValue;
};

struct PropertyInfo
{
    String name;
    const TypeInfo* type = nullptr;
    const TypeInfo* owner = nullptr;
    MethodInfo* getter = nullptr;
    MethodInfo* setter = nullptr;
    Access access = Access::Private;
    bool isStatic = false;
    /// `init` rather than `set`: assignable only while the object is being
    /// built -- in an object initializer, or from inside its own type.
    bool isInitOnly = false;
    bool isRequired = false;
};

/// A protected region of a method body. `stackDepth` is the evaluation-stack
/// height to unwind to before entering the handler.
enum class HandlerKind : std::uint8_t
{
    Catch,
    Finally,
};

struct Handler
{
    std::uint32_t start = 0;
    std::uint32_t end = 0;
    std::uint32_t target = 0;
    std::uint32_t stackDepth = 0;
    const TypeInfo* catchType = nullptr;  ///< null catches everything
    std::uint32_t exceptionSlot = 0;      ///< local the caught object goes into
    HandlerKind kind = HandlerKind::Catch;
};

/// Bytecode offset -> source line, for stack traces. Sorted by offset and only
/// emitted when `CompileOptions::debugInfo` is on.
struct LineEntry
{
    std::uint32_t offset = 0;
    std::uint32_t line = 0;
};

/// The executable part of a method. Immutable once emitted, which is what lets
/// a back end compile it on another thread while the interpreter runs it.
struct MethodBody
{
    std::vector<std::uint8_t> code;
    std::vector<Handler> handlers;
    std::vector<LineEntry> lines;
    /// Static type of every local, in slot order. The JIT needs this to pick
    /// machine registers; the interpreter only uses it to zero-initialize.
    std::vector<const TypeInfo*> localTypes;
    /// Locals whose address is taken -- captured by a lambda, or passed as
    /// `ref`/`out`. They live in a heap box instead of the frame.
    std::vector<std::uint8_t> localBoxed;
    std::uint32_t maxStack = 8;

    std::size_t byteSize() const noexcept
    {
        return code.size() + handlers.size() * sizeof(Handler) + lines.size() * sizeof(LineEntry) +
               localTypes.size() * (sizeof(void*) + 1);
    }
};

enum MethodFlags : std::uint16_t
{
    MethodStatic = 1u << 0,
    MethodVirtual = 1u << 1,
    MethodAbstract = 1u << 2,
    MethodOverride = 1u << 3,
    MethodConstructor = 1u << 4,
    MethodNative = 1u << 5,   ///< implemented by the host or the runtime
    MethodEntry = 1u << 6,    ///< the assembly's entry point
    MethodAccessor = 1u << 7, ///< a property getter or setter
    MethodLambda = 1u << 8,   ///< synthesized from a lambda or anonymous method
    MethodSealed = 1u << 9,
};

/// The stubs a method's native entries start at; null in a build with no optimizing back end.
void* jitLazyEntry() noexcept;
void* jitLazyDelegateEntry() noexcept;

/// One method. The JIT state is the only mutable part, so a loaded assembly
/// can be shared read-only between a running interpreter and a compiling
/// back end.
struct MethodInfo
{
    static constexpr std::uint32_t kNoSlot = 0xffffffffu;
    /// Parked in `hotness` when the back end has refused this method, so the
    /// decision is made once and never revisited.
    static constexpr std::uint32_t kNeverCompile = 0xffffffffu;

    String name;
    const TypeInfo* owner = nullptr;
    const TypeInfo* returnType = nullptr;
    std::vector<ParamInfo> params;
    std::uint16_t flags = 0;
    Access access = Access::Private;
    /// Position in the owner's vtable for a virtual method, else -1.
    std::int32_t vtableSlot = -1;
    /// Position in the assembly's method table; what call sites encode.
    std::uint32_t index = 0;
    /// Frame size: parameters occupy the first slots, then locals.
    std::uint32_t numLocals = 0;
    /// A lambda reads its captured variables out of boxes the delegate
    /// carries. These are the frame slots they are copied into, in the order
    /// the delegate stores them.
    std::vector<std::uint32_t> captureSlots;
    /// An auto-property accessor has no body: it reads or writes this slot.
    /// `kNoSlot` for every other method.
    std::uint32_t accessorSlot = kNoSlot;
    MethodBody body;
    NativeFunction native;
    AssemblyImpl* assembly = nullptr;

    // ---- tiered compilation ----
    /// Calls plus loop back-edges. Bumped by the interpreter, read by the
    /// context when it decides to hand this method to the code generator.
    std::atomic<std::uint32_t> hotness{0};
    /// Non-null once a back end compiled this method: the VM jumps straight
    /// to it. Written once, under the context lock; read on every call.
    std::atomic<NativeCode*> code{nullptr};
    /// Where native code calls this method, with the native convention. Until the method is compiled, a stub that
    /// compiles it or goes through the interpreter; which is why every call site can simply jump here.
    void* fastEntry = jitLazyEntry();
    /// The same for a call through a delegate, where a receiver has to be fetched first.
    void* delegateEntry = jitLazyDelegateEntry();

    bool isStatic() const noexcept { return (flags & MethodStatic) != 0; }
    /// Abstract counts: the slot it occupies is the one an override takes.
    bool isVirtual() const noexcept
    {
        return (flags & (MethodVirtual | MethodOverride | MethodAbstract)) != 0;
    }
    bool isAbstract() const noexcept { return (flags & MethodAbstract) != 0; }
    bool isConstructor() const noexcept { return (flags & MethodConstructor) != 0; }
    bool isNative() const noexcept { return (flags & MethodNative) != 0; }
    /// Slots the frame needs: `this` (if any) plus parameters plus locals.
    std::uint32_t frameSize() const noexcept
    {
        return (isStatic() ? 0u : 1u) + std::uint32_t(params.size()) + numLocals;
    }
    /// `Type.Method`.
    std::string qualifiedName() const;
};

// ----------------------------------------------------------------- type ---

/// A named type. Layout is decided once, at compile time: instance fields get
/// consecutive slots starting after the base class's, and virtual methods get
/// consecutive vtable entries, so a field read is an index and a virtual call
/// is one indirection.
struct TypeInfo
{
    TypeKind kind = TypeKind::Class;
    NativeKind nativeKind = NativeKind::None;
    Access access = Access::Public;
    bool isSealed = false;
    bool isAbstract = false;
    bool isStaticClass = false;
    /// A struct: assignment, argument passing and field storage copy it.
    bool isValueType() const noexcept
    {
        return kind == TypeKind::Struct || isValueKind(kind) || kind == TypeKind::Enum;
    }

    String name;      ///< `List`, `Point`
    String fullName;  ///< `System.Collections.Generic.List<int>`, `Geometry.Point`

    const TypeInfo* base = nullptr;
    std::vector<const TypeInfo*> interfaces;
    const TypeInfo* element = nullptr;             ///< array element type
    std::vector<const TypeInfo*> typeArgs;         ///< a constructed generic type
    const TypeInfo* underlying = nullptr;          ///< enum backing type

    std::vector<FieldInfo> fields;      ///< instance fields declared here
    std::vector<FieldInfo> statics;     ///< static and const fields declared here
    std::vector<PropertyInfo> props;
    std::vector<MethodInfo*> methods;   ///< declared here, owned by the assembly
    std::vector<MethodInfo*> vtable;    ///< flattened, base entries first
    MethodInfo* invoke = nullptr;       ///< the signature of a delegate type

    /// Total instance slots, including the base class's.
    std::uint32_t instanceSlots = 0;
    /// The tag each slot of a new instance starts with, so that writing a primitive field is writing its payload. Made
    /// the first time an instance is.
    mutable std::vector<Type> slotTags;
    /// First slot of this type's static fields in the assembly's static store.
    std::uint32_t staticBase = 0;
    /// The C++ type this stands for, when `nativeKind == Host`.
    const void* typeKey = nullptr;
    AssemblyImpl* assembly = nullptr;

    bool derivesFrom(const TypeInfo* other) const noexcept;
    bool implements(const TypeInfo* iface) const noexcept;
    const FieldInfo* findField(StringView fieldName) const noexcept;
    const PropertyInfo* findProperty(StringView propName) const noexcept;
    /// Every method of that name, this type first, then bases.
    void collectMethods(StringView methodName, std::vector<MethodInfo*>& out) const;
};

// ------------------------------------------------------------- assembly ---

/// A compiled program: its types, its methods, its literals and its static
/// storage. One allocation-owning root, so unloading is dropping the object.
struct AssemblyImpl : CompiledUnit
{
    AssemblyImpl() = default;
    ~AssemblyImpl() override;
    AssemblyImpl(const AssemblyImpl&) = delete;
    AssemblyImpl& operator=(const AssemblyImpl&) = delete;

    Runtime* rt = nullptr;

    std::vector<std::unique_ptr<TypeInfo>> types;
    std::vector<std::unique_ptr<MethodInfo>> methodTable;
    /// What the instruction stream's type and method operands index. A
    /// reference may point outside the assembly -- at the core library, or at
    /// a host class -- which is why this is not the table above.
    std::vector<const TypeInfo*> typeRefs;
    std::vector<const MethodInfo*> methodRefs;
    /// String literals, interned once at load and shared by every use.
    std::vector<Value> strings;
    /// Backing store for every static field the assembly declares.
    std::vector<Value> statics;
    MethodInfo* entry = nullptr;
    /// Whether the source declared a type of its own. Names are global across
    /// units, so such a unit stays for as long as the context does. A unit that
    /// is only top-level code cannot be named by a later one: once nothing refers
    /// to it, a collection unloads it (`Runtime::unloadUnusedUnits`).
    bool declaresNames = true;
    bool unusedCandidate = false;  ///< scratch for `Runtime::unloadUnusedUnits`
    /// Static field initializers, run once when the assembly is loaded.
    std::vector<MethodInfo*> initializers;
    bool initialized = false;
    /// Source file names, indexed by methods' line tables.
    std::shared_ptr<const std::string> sourceName;

    TypeInfo* findType(StringView fullName) const noexcept;
    MethodInfo* findMethod(StringView qualified) const noexcept;
    std::size_t imageSize() const noexcept override;
    /// `Type.Method` for every method the assembly defines.
    std::vector<std::string> methods() const override;
    std::string entryPoint() const override;
    /// Drop every Value the assembly holds, breaking references into the heap
    /// before the context tears the heap down.
    void release() noexcept;

    std::unordered_map<String, TypeInfo*> typeIndex;
};

}  // namespace typed
}  // namespace internal
}  // namespace script
