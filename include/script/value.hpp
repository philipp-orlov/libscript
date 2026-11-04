// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// libscript -- a script value as seen from C++.
//
// `Value` is a 16-byte tagged union shared by every language the library
// runs. Primitives live inline; strings, symbols, objects, arrays and
// functions are reference-counted pointers into the owning Context's heap.
// Copying a Value is a refcount bump; holding one keeps the referent alive.
//
// The statically typed languages know which arithmetic to emit and use the
// tag only for boxing and the embedding boundary; the dynamic languages
// inspect it on every operation. Numbers produced by a dynamic language are
// always `Double`; a host may hand any numeric tag to any language.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "script/error.hpp"

namespace script {

namespace detail {
struct GCObject;
struct Access;
void incRef(GCObject*) noexcept;
void decRef(GCObject*) noexcept;
}  // namespace detail

class Context;
class Value;

/// The runtime shape of a value. The narrow integer types (`sbyte` .. `uint`)
/// travel as `Int`; `long`/`ulong` as `Long`. `Undefined` and `Symbol` exist
/// for the ECMAScript languages; a C# program never produces either.
enum class Type : std::uint8_t
{
    Undefined = 0,
    Null,
    Bool,
    Char,
    Int,     ///< sbyte, byte, short, ushort, int, uint
    Long,    ///< long, ulong
    Float,
    Double,  ///< every JavaScript number
    // --- everything below is a heap reference ---
    String,
    Symbol,
    Object,    ///< a class instance, or any JavaScript object (arrays and functions included)
    Array,     ///< a C# array
    Delegate,  ///< a C# delegate
};

const char* toString(Type) noexcept;

/// Property attributes, matching the ECMAScript property descriptor bits.
enum class PropertyFlags : std::uint8_t
{
    None = 0,
    Writable = 1u << 0,
    Enumerable = 1u << 1,
    Configurable = 1u << 2,
    Default = Writable | Enumerable | Configurable,
    ReadOnly = Enumerable,
    Hidden = Writable | Configurable,
};
constexpr PropertyFlags operator|(PropertyFlags a, PropertyFlags b) noexcept
{
    return PropertyFlags(std::uint8_t(a) | std::uint8_t(b));
}
constexpr bool operator&(PropertyFlags a, PropertyFlags b) noexcept
{
    return (std::uint8_t(a) & std::uint8_t(b)) != 0;
}

// ---------------------------------------------------------------- value ---

class Value
{
public:
    Value() noexcept : tag_(Type::Null), long_(0) {}
    ~Value()
    {
        if (heap())
            detail::decRef(obj_);
    }

    Value(const Value& o) noexcept : tag_(o.tag_), long_(o.long_)
    {
        if (heap())
            detail::incRef(obj_);
    }
    Value(Value&& o) noexcept : tag_(o.tag_), long_(o.long_) { o.tag_ = Type::Null; }
    Value& operator=(const Value& o) noexcept
    {
        Value tmp(o);
        swap(tmp);
        return *this;
    }
    Value& operator=(Value&& o) noexcept
    {
        if (this != &o) {
            Value tmp(std::move(o));
            swap(tmp);
        }
        return *this;
    }
    void swap(Value& o) noexcept
    {
        std::swap(tag_, o.tag_);
        std::swap(long_, o.long_);
    }

    Value(std::nullptr_t) noexcept : Value() {}
    Value(bool b) noexcept : tag_(Type::Bool), long_(0) { bool_ = b; }
    Value(char16_t c) noexcept : tag_(Type::Char), long_(0) { int_ = std::int32_t(c); }
    Value(std::int32_t i) noexcept : tag_(Type::Int), long_(0) { int_ = i; }
    Value(std::uint32_t u) noexcept : tag_(Type::Int), long_(0) { int_ = std::int32_t(u); }
    Value(std::int64_t l) noexcept : tag_(Type::Long), long_(l) {}
    Value(std::uint64_t u) noexcept : tag_(Type::Long), long_(std::int64_t(u)) {}
    Value(float f) noexcept : tag_(Type::Float), long_(0) { float_ = f; }
    Value(double d) noexcept : tag_(Type::Double), long_(0) { double_ = d; }
    /// Strings are heap cells, so they need the Context that owns the heap:
    /// use `ctx.str("text")`, `ctx.value("text")` or a templated setter.
    Value(const char*) = delete;

    static Value null() noexcept { return Value(); }
    static Value undefined() noexcept
    {
        Value v;
        v.tag_ = Type::Undefined;
        return v;
    }

    // ------------------------------------------------------------ kinds ---

    Type type() const noexcept { return tag_; }
    bool isUndefined() const noexcept { return tag_ == Type::Undefined; }
    bool isNull() const noexcept { return tag_ == Type::Null; }
    bool isNullish() const noexcept { return tag_ <= Type::Null; }
    bool isBool() const noexcept { return tag_ == Type::Bool; }
    bool isChar() const noexcept { return tag_ == Type::Char; }
    bool isInt() const noexcept { return tag_ == Type::Int; }
    bool isLong() const noexcept { return tag_ == Type::Long; }
    bool isFloat() const noexcept { return tag_ == Type::Float; }
    bool isDouble() const noexcept { return tag_ == Type::Double; }
    bool isNumber() const noexcept { return tag_ >= Type::Int && tag_ <= Type::Double; }
    bool isString() const noexcept { return tag_ == Type::String; }
    bool isSymbol() const noexcept { return tag_ == Type::Symbol; }
    bool isObject() const noexcept { return tag_ == Type::Object; }
    bool isReference() const noexcept { return tag_ >= Type::String; }
    /// A C# array or a JavaScript array.
    bool isArray() const noexcept;
    /// A delegate or a JavaScript function: something `call` accepts.
    bool isFunction() const noexcept;
    /// A `System.Exception` or an `Error` instance.
    bool isError() const noexcept;
    bool isPromise() const noexcept;
    /// True when the object wraps a C++ instance registered through `Class<T>`.
    bool isNative() const noexcept;

    /// The runtime type name: "System.Int32", "Geometry.Point", "object", "function".
    std::string typeName() const;
    /// The heap this value belongs to, or nullptr for a primitive.
    Context* context() const noexcept;

    // ------------------------------------------------------- conversion ---

    /// Truthiness: false for `null`, `undefined`, `false`, zero, NaN and the
    /// empty string; true for everything else. Never runs script.
    bool toBool() const noexcept;
    /// Numeric conversions. Integers truncate towards zero the way a cast
    /// does; a JavaScript object goes through its `valueOf` and may throw.
    std::int32_t toInt() const;
    std::int64_t toLong() const;
    double toDouble() const;
    /// `ToString()`, encoded as UTF-8. May run a user `ToString`/`toString`
    /// override and therefore throw. A primitive with no heap behind it prints
    /// the way JSON would: `true`, `null`, `undefined`, `1e+21`.
    std::string toString() const;
    /// A short, human-readable form for logs: strings are quoted, objects
    /// show their members one level deep.
    std::string inspect() const;
    /// `JSON.stringify(value, null, indent)` for JavaScript values and
    /// primitives. Returns "" for `undefined`.
    std::string toJson(int indent = 0) const;

    /// Identity: the same heap cell, or equal primitives of the same tag.
    bool sameReference(const Value& o) const noexcept;
    /// `===`: value equality for primitives and strings, identity for the rest.
    bool strictEquals(const Value& o) const noexcept;
    /// The language's own equality: `Object.Equals` for a C# value, `==` for
    /// a JavaScript one.
    bool equals(const Value& o) const;
    friend bool operator==(const Value& a, const Value& b) noexcept { return a.strictEquals(b); }
    friend bool operator!=(const Value& a, const Value& b) noexcept { return !a.strictEquals(b); }

    // ----------------------------------------------------------- members ---

    /// Read a field, property or JavaScript property by name. A C# type with
    /// no such member throws `ArgumentException`; a missing JavaScript
    /// property reads as `undefined`. A nullish receiver throws.
    Value get(std::string_view key) const;
    Value get(std::uint32_t index) const;
    Value get(const Value& key) const;
    /// String literals would otherwise be ambiguous between the overloads above.
    Value get(const char* key) const { return get(std::string_view(key)); }

    void set(std::string_view key, const Value& v);
    void set(std::uint32_t index, const Value& v);
    void set(const Value& key, const Value& v);
    void set(const char* key, const Value& v) { set(std::string_view(key), v); }
    /// Convert and store in one step: `obj.set("n", 42)`, `obj.set("s", "hi")`.
    template <class T, std::enable_if_t<!std::is_same_v<std::decay_t<T>, Value>, int> = 0>
    void set(std::string_view key, T&& v);
    template <class T, std::enable_if_t<!std::is_same_v<std::decay_t<T>, Value>, int> = 0>
    void set(const char* key, T&& v);

    bool has(std::string_view key) const;
    /// The member names the runtime type declares, base first; for a
    /// JavaScript object, its own enumerable keys in insertion order.
    std::vector<std::string> keys() const;
    /// Element count of an array, list or string; 0 otherwise.
    std::uint32_t length() const;
    /// Append to a JavaScript array or a `List<T>`.
    void push(const Value& v);

    // ------------------------------------------------ JavaScript objects ---

    void defineProperty(std::string_view key, const Value& v, PropertyFlags flags);
    void defineAccessor(std::string_view key, const Value& getter, const Value& setter,
                        PropertyFlags flags = PropertyFlags::Configurable | PropertyFlags::Enumerable);
    bool hasOwn(std::string_view key) const;
    bool remove(std::string_view key);  ///< `delete object[key]`
    Value prototype() const;
    void setPrototype(const Value& proto);

    // --------------------------------------------------------- callable ---

    /// Invoke a delegate or a function. Throws when the value is not callable.
    Value call(const std::vector<Value>& args = {}) const;
    Value callWithThis(const Value& thisValue, const std::vector<Value>& args) const;
    /// `this.Method(args...)`; a C# overload is picked from the arguments.
    Value invoke(std::string_view method, const std::vector<Value>& args = {}) const;
    /// `new f(args...)` for a JavaScript constructor.
    Value construct(const std::vector<Value>& args = {}) const;
    /// Convert every argument, then call: `fn(1, "two", true)`.
    template <class... A>
    Value operator()(A&&... args) const;

    // ----------------------------------------------------------- native ---

    /// The wrapped C++ instance if this object was created for `typeKey`,
    /// else nullptr. Prefer `Value::as<T*>()` from binding.hpp.
    void* nativeHandle(const void* typeKey) const noexcept;
    template <class T>
    T as() const;

private:
    bool heap() const noexcept { return tag_ >= Type::String; }

    Type tag_;
    union
    {
        bool bool_;
        std::int32_t int_;
        std::int64_t long_;
        float float_;
        double double_;
        detail::GCObject* obj_;
    };
    friend struct detail::Access;
};

static_assert(sizeof(Value) <= 16, "Value should stay pointer+tag sized");

// ------------------------------------------------------------ exception ---

/// A script exception that escaped into C++. `value()` is the exception
/// object: a `System.Exception` instance or whatever the script threw.
///
/// An exception naturally outlives the scope it unwinds through, and often
/// the Context that scope owned. The value is released only while its Context
/// is alive; afterwards `value()` reads as `null`.
class Exception : public Error
{
public:
    Exception(ErrorCode code, std::string message, Value value, std::string stack);
    ~Exception() override;
    Exception(const Exception&);
    Exception& operator=(const Exception&);

    const Value& value() const noexcept { return value_; }
    /// Newline-separated call stack captured where the throw happened.
    const std::string& stack() const noexcept { return stack_; }

private:
    void release() noexcept;

    Value value_;
    std::string stack_;
    std::weak_ptr<const bool> alive_;  ///< set while the value's Context exists
};

}  // namespace script
