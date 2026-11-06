// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Shared internal vocabulary: reference handles, string encoding, raw access
// to the tagged `Value` representation. Everything here is used by both
// engines and every language front end.
#pragma once

#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "mem/local_heap.hpp"
#include "mem/pool_allocator.hpp"
#include "script/context.hpp"
#include "script/value.hpp"

namespace script {

class Context;

namespace internal {

/// Where a `Value`'s tag and payload sit. The code generators bake these
/// offsets into every load and store, so they ask rather than assume.
struct ValueLayout
{
    std::size_t size = 0;
    std::size_t tagOffset = 0;
    std::size_t payloadOffset = 0;

    bool isExpected() const noexcept { return size == 16 && tagOffset == 0 && payloadOffset == 8; }
};

/// Where every variable-length buffer of the engine comes from: the elements
/// of an array, a property table, a temporary string. Slabs per size class,
/// shared by all contexts and with a cache per thread, so a process that keeps
/// growing and dropping such buffers never shreds the general allocator.
/// Requests above the small classes are mappings whose idle pages go back to
/// the OS.
mem::SizeClassPool& payloadPool();
inline mem::LocalHeap::Options payloadHeapOptions()
{
    mem::LocalHeap::Options options;
    options.large = &payloadPool();
    return options;
}
/// A few freed small buffers per size, kept by each thread in front of `payloadPool`: a property table or an element
/// buffer made and dropped in a loop then costs a pop and a push. Bounded, and handed back when the thread ends.
namespace payload_cache {
inline constexpr std::size_t kGranule = 16;
inline constexpr std::size_t kMaxBytes = 1024;
inline constexpr std::size_t kBins = kMaxBytes / kGranule + 1;
struct Cache
{
    void* head[kBins];
    std::uint8_t count[kBins];
    bool registered;
    bool dead;
};
extern thread_local Cache t_cache;
void* refill(std::size_t bytes);
void spill(void* p, std::size_t bytes) noexcept;

inline void* take(std::size_t bytes)
{
    const std::size_t bin = (bytes + kGranule - 1) / kGranule;
    Cache& c = t_cache;
    if (void* p = c.head[bin]) {
        c.head[bin] = *static_cast<void**>(p);
        --c.count[bin];
        return p;
    }
    return refill(bin * kGranule);
}
inline void give(void* p, std::size_t bytes) noexcept
{
    const std::size_t bin = (bytes + kGranule - 1) / kGranule;
    Cache& c = t_cache;
    if (c.registered && c.count[bin] < (bin <= 16 ? 32 : 8)) {
        *static_cast<void**>(p) = c.head[bin];
        c.head[bin] = p;
        ++c.count[bin];
        return;
    }
    spill(p, bin * kGranule);
}
}  // namespace payload_cache

/// `mem::PoolAllocator` over `payloadPool`, with the thread's small cache in front.
template <class T>
class PoolAlloc
{
public:
    using value_type = T;
    using is_always_equal = std::true_type;
    using propagate_on_container_move_assignment = std::true_type;
    template <class U>
    struct rebind
    {
        using other = PoolAlloc<U>;
    };
    PoolAlloc() noexcept = default;
    template <class U>
    PoolAlloc(const PoolAlloc<U>&) noexcept
    {}
    T* allocate(std::size_t n)
    {
        if (n <= payload_cache::kMaxBytes / sizeof(T) && n)
            return static_cast<T*>(payload_cache::take(n * sizeof(T)));
        return mem::PoolAllocator<T, &payloadPool>().allocate(n);
    }
    void deallocate(T* p, std::size_t n) noexcept
    {
        if (n <= payload_cache::kMaxBytes / sizeof(T) && n) {
            payload_cache::give(p, n * sizeof(T));
            return;
        }
        mem::PoolAllocator<T, &payloadPool>().deallocate(p, n);
    }
    friend bool operator==(const PoolAlloc&, const PoolAlloc&) noexcept { return true; }
    friend bool operator!=(const PoolAlloc&, const PoolAlloc&) noexcept { return false; }
};
/// A vector whose buffer comes from `payloadPool`. The kind every cell uses.
template <class T>
using Vec = std::vector<T, PoolAlloc<T>>;

/// Strings are UTF-16 because `string.Length`, indexing and `char` follow the
/// languages rather than the storage. Conversion to and from UTF-8 happens
/// only at the C++ boundary.
using Char = char16_t;
using String = std::basic_string<Char, std::char_traits<Char>, PoolAlloc<Char>>;
using StringView = std::u16string_view;

std::string toUtf8(StringView s);
String fromUtf8(std::string_view s);
/// Widen an ASCII literal; the engine's own names never leave ASCII.
String ascii(std::string_view s);

/// A table hash over the code units. Not stable across versions: never stored.
std::size_t hashString(StringView s) noexcept;

/// `s` as bytes, when every unit is ASCII: what the number parsers read. Short
/// text stays on the stack, so reading a number makes no heap traffic.
class AsciiText
{
public:
    explicit AsciiText(StringView s);
    bool isAscii() const noexcept { return ascii_; }
    std::string_view view() const noexcept { return view_; }

private:
    char small_[64];
    std::string large_;
    std::string_view view_;
    bool ascii_ = true;
};

/// `"s"`, escaped as JSON says.
void appendJsonQuoted(StringView s, String& out);

/// `String(number)` as ECMAScript spells it: the shortest round-tripping
/// decimal, exponential notation exactly where the language says. This is how
/// a number with no language behind it -- a host-made `Value(1e21)` -- prints.
String numberToString(double d);
/// Appends `digits` laid out without an exponent: `1234.5`, `0.0012`, `1200`.
void appendPlainDecimal(const char* digits, int count, int pointPosition, String& out);

/// The garbage-collected cell header lives in `script::detail` so that the
/// public `Value` can name it without pulling the engine into a public header.
using GCObject = ::script::detail::GCObject;

struct StringCell;
struct Box;
struct ContextImpl;

/// An owning handle to a GC cell. Assignment and destruction maintain the
/// reference count; the cycle collector deals with what refcounting cannot.
template <class T>
class Ref
{
public:
    Ref() noexcept = default;
    /// `adopt` takes over an existing reference instead of adding one.
    Ref(T* p, bool adopt = false) noexcept : p_(p)
    {
        if (p_ && !adopt)
            detail::incRef(base(p_));
    }
    Ref(const Ref& o) noexcept : Ref(o.p_) {}
    Ref(Ref&& o) noexcept : p_(o.p_) { o.p_ = nullptr; }
    ~Ref() { reset(); }

    Ref& operator=(const Ref& o) noexcept
    {
        Ref tmp(o);
        std::swap(p_, tmp.p_);
        return *this;
    }
    Ref& operator=(Ref&& o) noexcept
    {
        if (this != &o) {
            reset();
            p_ = o.p_;
            o.p_ = nullptr;
        }
        return *this;
    }

    void reset(T* p = nullptr, bool adopt = false) noexcept
    {
        T* old = p_;
        p_ = p;
        if (p_ && !adopt)
            detail::incRef(base(p_));
        if (old)
            detail::decRef(base(old));
    }

    T* get() const noexcept { return p_; }
    T* operator->() const noexcept { return p_; }
    T& operator*() const noexcept { return *p_; }
    explicit operator bool() const noexcept { return p_ != nullptr; }
    bool operator==(const Ref& o) const noexcept { return p_ == o.p_; }
    bool operator!=(const Ref& o) const noexcept { return p_ != o.p_; }

private:
    static GCObject* base(T* p) noexcept { return static_cast<GCObject*>(p); }
    T* p_ = nullptr;
};

}  // namespace internal

namespace detail {

/// The internals' key to the private representation of `Value`.
struct Access
{
    static GCObject* ptr(const Value& v) noexcept
    {
        return v.tag_ >= Type::String ? v.obj_ : nullptr;
    }
    static bool boolean(const Value& v) noexcept { return v.bool_; }
    static std::int32_t i4(const Value& v) noexcept { return v.int_; }
    static std::int64_t i8(const Value& v) noexcept { return v.long_; }
    static float r4(const Value& v) noexcept { return v.float_; }
    static double r8(const Value& v) noexcept { return v.double_; }

    /// Reinterpret the payload without touching the tag. The C# compiler
    /// emits the conversions it wants explicitly; this is how a slot declared
    /// `uint` gets at its bits.
    static std::uint32_t u4(const Value& v) noexcept { return std::uint32_t(v.int_); }
    static std::uint64_t u8(const Value& v) noexcept { return std::uint64_t(v.long_); }

    /// Retag in place. Used where the static type says the payload is already
    /// right, e.g. `(long)someInt` is a widening the compiler proved.
    static void retag(Value& v, Type t) noexcept { v.tag_ = t; }

    /// Wrap a cell, adding a reference.
    static Value make(Type t, GCObject* o) noexcept
    {
        Value v;
        v.tag_ = t;
        v.obj_ = o;
        if (o)
            incRef(o);
        return v;
    }
    /// Wrap a cell, taking over the caller's reference.
    static Value adopt(Type t, GCObject* o) noexcept
    {
        Value v;
        v.tag_ = t;
        v.obj_ = o;
        return v;
    }

    static CallInfo makeCallInfo(Context* ctx, const Value* self, const std::vector<Value>* args,
                                 bool isNew) noexcept
    {
        CallInfo ci;
        ci.ctx_ = ctx;
        ci.this_ = self;
        ci.args_ = args;
        ci.isNew_ = isNew;
        return ci;
    }

    /// Where the tag and the payload actually sit.
    static internal::ValueLayout layout() noexcept
    {
        Value v;
        const char* base = reinterpret_cast<const char*>(&v);
        internal::ValueLayout out;
        out.size = sizeof(Value);
        out.tagOffset = std::size_t(reinterpret_cast<const char*>(&v.tag_) - base);
        out.payloadOffset = std::size_t(reinterpret_cast<const char*>(&v.long_) - base);
        return out;
    }
};

}  // namespace detail

namespace internal {

/// Any numeric tag as a double. The dynamic engine produces only `Double`,
/// so that case is the fast path; the others arrive from the host.
inline double numberOf(const Value& v) noexcept
{
    switch (v.type()) {
        case Type::Double: return detail::Access::r8(v);
        case Type::Int:
        case Type::Char: return detail::Access::i4(v);
        case Type::Long: return double(detail::Access::i8(v));
        case Type::Float: return detail::Access::r4(v);
        case Type::Bool: return detail::Access::boolean(v) ? 1 : 0;
        default: return 0;
    }
}
inline bool boolOf(const Value& v) noexcept
{
    return detail::Access::boolean(v);
}

}  // namespace internal
}  // namespace script

namespace std {
template <>
struct hash<script::internal::String>
{
    size_t operator()(const script::internal::String& s) const noexcept
    {
        return script::internal::hashString(s);
    }
};
}  // namespace std
