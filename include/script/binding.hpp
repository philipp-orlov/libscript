// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// libscript -- the C++ binding layer, one declaration for every language.
//
// One line per member. The signature of a host member is derived from its C++
// signature, so the C# compiler and the TypeScript checker type-check a call
// into the host exactly the way they check a call into the program, and
// JavaScript sees the same functions on the same objects:
//
//     ctx.hostClass("Env")
//        .method("Now", [] { return std::time(nullptr); })
//        .method("Log", [](std::string message) { std::puts(message.c_str()); });
//
//     script::Class<Vector>(ctx, "Vector")
//        .constructor<double, double>()
//        .field("X", &Vector::x)
//        .method("Length", &Vector::length);
//
// Everything here is header-only template glue over the plain Context API.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "script/context.hpp"
#include "script/value.hpp"

namespace script {

// ------------------------------------------------------------ spellings ---

namespace detail {

/// The name of the registered type standing for a C++ class. Filled in by
/// `Class<T>`; its address doubles as the type key, so two translation units
/// agree on it without RTTI or a registry.
template <class T>
struct ClassInfo
{
    static std::string name;
    static const void* key() noexcept { return &name; }
};
template <class T>
std::string ClassInfo<T>::name = "object";

/// How a C++ type is spelled in each language's type system.
template <class T, class = void>
struct TypeSpelling
{
    static std::string csharp() { return "object"; }
    static std::string typescript() { return "any"; }
};
#define SCRIPT_SPELL(cpp, cs, ts)                    \
    template <>                                      \
    struct TypeSpelling<cpp>                         \
    {                                                \
        static std::string csharp() { return cs; }   \
        static std::string typescript() { return ts; } \
    }
SCRIPT_SPELL(void, "void", "void");
SCRIPT_SPELL(bool, "bool", "boolean");
SCRIPT_SPELL(char16_t, "char", "string");
SCRIPT_SPELL(std::int8_t, "sbyte", "number");
SCRIPT_SPELL(std::uint8_t, "byte", "number");
SCRIPT_SPELL(std::int16_t, "short", "number");
SCRIPT_SPELL(std::uint16_t, "ushort", "number");
SCRIPT_SPELL(std::int32_t, "int", "number");
SCRIPT_SPELL(std::uint32_t, "uint", "number");
SCRIPT_SPELL(std::int64_t, "long", "number");
SCRIPT_SPELL(std::uint64_t, "ulong", "number");
SCRIPT_SPELL(float, "float", "number");
SCRIPT_SPELL(double, "double", "number");
SCRIPT_SPELL(std::string, "string", "string");
SCRIPT_SPELL(std::string_view, "string", "string");
SCRIPT_SPELL(const char*, "string", "string");
SCRIPT_SPELL(Value, "object", "any");
#undef SCRIPT_SPELL

/// A class the host bound with `Class<T>`, as opposed to the standard types
/// the library converts itself.
template <class T>
struct IsSpecial : std::false_type
{};
template <class T>
struct IsSpecial<std::vector<T>> : std::true_type
{};
template <class T>
struct IsSpecial<std::optional<T>> : std::true_type
{};
template <class T>
struct IsSpecial<std::shared_ptr<T>> : std::true_type
{};
template <class T>
struct IsSpecial<std::map<std::string, T>> : std::true_type
{};
template <class T>
struct IsSpecial<std::unordered_map<std::string, T>> : std::true_type
{};
template <class R, class... A>
struct IsSpecial<std::function<R(A...)>> : std::true_type
{};
template <class T>
constexpr bool isHostClass =
    std::is_class_v<T> && !IsSpecial<T>::value && !std::is_same_v<T, std::string> &&
    !std::is_same_v<T, std::string_view> && !std::is_same_v<T, Value>;

/// A bound class passed or returned by value: the wrapper copies it.
template <class T>
struct TypeSpelling<T, std::enable_if_t<isHostClass<T>>>
{
    static std::string csharp() { return ClassInfo<T>::name; }
    static std::string typescript() { return ClassInfo<T>::name; }
};
template <class T>
struct TypeSpelling<T*, std::enable_if_t<std::is_class_v<T>>>
{
    static std::string csharp() { return ClassInfo<std::remove_cv_t<T>>::name; }
    static std::string typescript() { return ClassInfo<std::remove_cv_t<T>>::name; }
};
template <class T>
struct TypeSpelling<T&, std::enable_if_t<std::is_class_v<T> && !std::is_same_v<T, std::string>>>
{
    static std::string csharp() { return ClassInfo<std::remove_cv_t<T>>::name; }
    static std::string typescript() { return ClassInfo<std::remove_cv_t<T>>::name; }
};
template <class T>
struct TypeSpelling<std::shared_ptr<T>>
{
    static std::string csharp() { return ClassInfo<std::remove_cv_t<T>>::name; }
    static std::string typescript() { return ClassInfo<std::remove_cv_t<T>>::name; }
};
template <class T>
struct TypeSpelling<std::optional<T>>
{
    static std::string csharp() { return TypeSpelling<T>::csharp(); }
    static std::string typescript() { return TypeSpelling<T>::typescript() + " | null"; }
};
template <class T>
struct TypeSpelling<std::vector<T>>
{
    static std::string csharp() { return TypeSpelling<T>::csharp() + "[]"; }
    static std::string typescript() { return TypeSpelling<T>::typescript() + "[]"; }
};
template <class T>
struct TypeSpelling<std::map<std::string, T>>
{
    static std::string csharp() { return "object"; }
    static std::string typescript() { return "{ [key: string]: " + TypeSpelling<T>::typescript() + " }"; }
};
template <class T>
struct TypeSpelling<std::unordered_map<std::string, T>>
{
    static std::string csharp() { return "object"; }
    static std::string typescript() { return "{ [key: string]: " + TypeSpelling<T>::typescript() + " }"; }
};
template <class R, class... A>
struct TypeSpelling<std::function<R(A...)>>
{
    static std::string csharp() { return "object"; }
    static std::string typescript()
    {
        std::string out = "(";
        std::size_t i = 0;
        ((out += (i ? ", " : "") + ("a" + std::to_string(i++)) + ": " +
                 TypeSpelling<std::decay_t<A>>::typescript()),
         ...);
        return out + ") => " + TypeSpelling<std::decay_t<R>>::typescript();
    }
};

}  // namespace detail

/// A stable, header-only identity for a bound C++ type.
template <class T>
constexpr const void* typeKey() noexcept
{
    return detail::ClassInfo<std::remove_cv_t<T>>::key();
}

// --------------------------------------------------------------- Convert ---

/// Converts between `T` and `Value`. Specialize it to teach the library about
/// a new type; the primary template handles classes bound through `Class<T>`,
/// copying the instance into a wrapper the context owns.
template <class T, class Enable = void>
struct Convert
{
    static Value to(Context& ctx, const T& v)
    {
        return ctx.native(typeKey<T>(), new T(v), [](void* p) { delete static_cast<T*>(p); });
    }
    static T from(Context&, const Value& v)
    {
        void* p = v.nativeHandle(typeKey<T>());
        if (!p)
            throw Error(ErrorCode::InvalidCast, "value is not the expected native object");
        return *static_cast<T*>(p);
    }
};

template <>
struct Convert<Value>
{
    static Value to(Context&, const Value& v) { return v; }
    static Value from(Context&, const Value& v) { return v; }
};
template <>
struct Convert<bool>
{
    static Value to(Context&, bool v) { return Value(v); }
    static bool from(Context&, const Value& v) { return v.toBool(); }
};
template <>
struct Convert<char16_t>
{
    static Value to(Context&, char16_t v) { return Value(v); }
    static char16_t from(Context&, const Value& v) { return char16_t(v.toInt()); }
};
template <class T>
struct Convert<T, std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool> &&
                                   !std::is_same_v<T, char16_t>>>
{
    static Value to(Context&, T v)
    {
        if constexpr (sizeof(T) > 4)
            return Value(std::int64_t(v));
        else
            return Value(std::int32_t(v));
    }
    static T from(Context&, const Value& v) { return T(v.toLong()); }
};
template <class T>
struct Convert<T, std::enable_if_t<std::is_floating_point_v<T>>>
{
    static Value to(Context&, T v)
    {
        if constexpr (std::is_same_v<T, float>)
            return Value(float(v));
        else
            return Value(double(v));
    }
    static T from(Context&, const Value& v) { return T(v.toDouble()); }
};
template <class T>
struct Convert<T, std::enable_if_t<std::is_enum_v<T>>>
{
    static Value to(Context&, T v) { return Value(std::int32_t(std::underlying_type_t<T>(v))); }
    static T from(Context&, const Value& v) { return static_cast<T>(v.toLong()); }
};
template <>
struct Convert<std::string>
{
    static Value to(Context& ctx, const std::string& v) { return ctx.str(v); }
    static std::string from(Context&, const Value& v) { return v.toString(); }
};
template <>
struct Convert<std::string_view>
{
    static Value to(Context& ctx, std::string_view v) { return ctx.str(v); }
    static std::string from(Context&, const Value& v) { return v.toString(); }
};
template <>
struct Convert<const char*>
{
    static Value to(Context& ctx, const char* v) { return ctx.str(v ? v : ""); }
};
template <std::size_t N>
struct Convert<char[N]>
{
    static Value to(Context& ctx, const char (&v)[N]) { return ctx.str(v); }
};
template <class T>
struct Convert<std::optional<T>>
{
    static Value to(Context& ctx, const std::optional<T>& v)
    {
        return v ? Convert<T>::to(ctx, *v) : Value::null();
    }
    static std::optional<T> from(Context& ctx, const Value& v)
    {
        if (v.isNullish())
            return std::nullopt;
        return Convert<T>::from(ctx, v);
    }
};
template <class T>
struct Convert<std::vector<T>>
{
    static Value to(Context& ctx, const std::vector<T>& v)
    {
        std::vector<Value> items;
        items.reserve(v.size());
        for (const T& item : v)
            items.push_back(Convert<T>::to(ctx, item));
        return ctx.array(items);  // the array of whichever engine is running
    }
    static std::vector<T> from(Context& ctx, const Value& v)
    {
        if (!v.isArray())
            throw Error(ErrorCode::InvalidCast, "expected an array");
        std::vector<T> out;
        const std::uint32_t n = v.length();
        out.reserve(n);
        for (std::uint32_t i = 0; i < n; ++i)
            out.push_back(Convert<T>::from(ctx, v.get(i)));
        return out;
    }
};

namespace detail {
template <class Map, class T>
Value mapTo(Context& ctx, const Map& m)
{
    Value o = ctx.object();
    for (const auto& kv : m)
        o.set(kv.first, Convert<T>::to(ctx, kv.second));
    return o;
}
template <class Map, class T>
Map mapFrom(Context& ctx, const Value& v)
{
    if (!v.isObject())
        throw Error(ErrorCode::InvalidCast, "expected an object");
    Map out;
    for (const std::string& k : v.keys())
        out.emplace(k, Convert<T>::from(ctx, v.get(k)));
    return out;
}
}  // namespace detail

template <class T>
struct Convert<std::map<std::string, T>>
{
    static Value to(Context& ctx, const std::map<std::string, T>& m)
    {
        return detail::mapTo<std::map<std::string, T>, T>(ctx, m);
    }
    static std::map<std::string, T> from(Context& ctx, const Value& v)
    {
        return detail::mapFrom<std::map<std::string, T>, T>(ctx, v);
    }
};
template <class T>
struct Convert<std::unordered_map<std::string, T>>
{
    static Value to(Context& ctx, const std::unordered_map<std::string, T>& m)
    {
        return detail::mapTo<std::unordered_map<std::string, T>, T>(ctx, m);
    }
    static std::unordered_map<std::string, T> from(Context& ctx, const Value& v)
    {
        return detail::mapFrom<std::unordered_map<std::string, T>, T>(ctx, v);
    }
};

/// A script function seen from C++ as an ordinary callable, and back.
template <class R, class... A>
struct Convert<std::function<R(A...)>>
{
    static Value to(Context& ctx, std::function<R(A...)> f);  // defined below
    static std::function<R(A...)> from(Context&, const Value& v)
    {
        if (!v.isFunction())
            throw Error(ErrorCode::InvalidCast, "expected a function");
        return [v](A... args) -> R {
            Context& ctx = *v.context();
            std::vector<Value> argv{Convert<std::decay_t<A>>::to(ctx, args)...};
            Value r = v.call(argv);
            if constexpr (!std::is_void_v<R>)
                return Convert<std::decay_t<R>>::from(ctx, r);
            else
                (void)r;
        };
    }
};

/// Raw pointers to bound classes are passed through without taking ownership.
template <class T>
struct Convert<T*, std::enable_if_t<std::is_class_v<T>>>
{
    static Value to(Context& ctx, T* p)
    {
        if (!p)
            return Value::null();
        return ctx.native(typeKey<T>(), const_cast<std::remove_const_t<T>*>(p), nullptr);
    }
    static T* from(Context&, const Value& v)
    {
        if (v.isNullish())
            return nullptr;
        void* p = v.nativeHandle(typeKey<T>());
        if (!p)
            throw Error(ErrorCode::InvalidCast, "value is not the expected native object");
        return static_cast<T*>(p);
    }
};

/// A shared_ptr keeps its control block alive for as long as the script
/// wrapper lives: the wrapper owns a copy of the shared_ptr, not the object.
template <class T>
struct Convert<std::shared_ptr<T>>
{
    static Value to(Context& ctx, std::shared_ptr<T> p)
    {
        if (!p)
            return Value::null();
        auto* holder = new std::shared_ptr<T>(std::move(p));
        return ctx.native(
            typeKey<T>(), holder->get(),
            [](void* owner) { delete static_cast<std::shared_ptr<T>*>(owner); }, holder);
    }
    /// Non-owning: the returned pointer is valid while the wrapper is alive.
    static std::shared_ptr<T> from(Context&, const Value& v)
    {
        if (v.isNullish())
            return nullptr;
        void* p = v.nativeHandle(typeKey<T>());
        if (!p)
            throw Error(ErrorCode::InvalidCast, "value is not the expected native object");
        return std::shared_ptr<T>(static_cast<T*>(p), [](T*) {});
    }
};

// ------------------------------------------------------- function traits ---

namespace detail {

template <class F>
struct FunctionTraits : FunctionTraits<decltype(&std::remove_reference_t<F>::operator())>
{};
template <class R, class... A>
struct FunctionTraits<R (*)(A...)>
{
    using Return = R;
    using Args = std::tuple<A...>;
    static constexpr bool isMember = false;
};
template <class R, class... A>
struct FunctionTraits<R(A...)> : FunctionTraits<R (*)(A...)>
{};
template <class C, class R, class... A>
struct FunctionTraits<R (C::*)(A...)>
{
    using Return = R;
    using Class = C;
    using Args = std::tuple<A...>;
    static constexpr bool isMember = true;
};
template <class C, class R, class... A>
struct FunctionTraits<R (C::*)(A...) const> : FunctionTraits<R (C::*)(A...)>
{};
template <class C, class R, class... A>
struct FunctionTraits<R (C::*)(A...) noexcept> : FunctionTraits<R (C::*)(A...)>
{};
template <class C, class R, class... A>
struct FunctionTraits<R (C::*)(A...) const noexcept> : FunctionTraits<R (C::*)(A...)>
{};

template <class Tuple, std::size_t I>
using ArgAt = std::decay_t<std::tuple_element_t<I, Tuple>>;

template <class R, class F, class Tuple, std::size_t... I>
Value callFree(Context& ctx, F& f, CallInfo& ci, std::index_sequence<I...>)
{
    if constexpr (std::is_void_v<R>) {
        f(ci.template arg<ArgAt<Tuple, I>>(I)...);
        (void)ctx;
        return Value::undefined();  // `void` is `undefined` to JavaScript and nothing to C#
    } else {
        return Convert<std::decay_t<R>>::to(ctx, f(ci.template arg<ArgAt<Tuple, I>>(I)...));
    }
}

template <class R, class C, class M, class Tuple, std::size_t... I>
Value callMember(Context& ctx, C* self, M m, CallInfo& ci, std::index_sequence<I...>)
{
    if constexpr (std::is_void_v<R>) {
        (self->*m)(ci.template arg<ArgAt<Tuple, I>>(I)...);
        (void)ctx;
        return Value::undefined();
    } else {
        return Convert<std::decay_t<R>>::to(ctx,
                                            (self->*m)(ci.template arg<ArgAt<Tuple, I>>(I)...));
    }
}

/// The declared types of a parameter pack, spelled for each language.
template <class Tuple>
struct Spellings;
template <class... A>
struct Spellings<std::tuple<A...>>
{
    static std::vector<std::string> csharp() { return {TypeSpelling<std::decay_t<A>>::csharp()...}; }
    static std::vector<std::string> typescript()
    {
        return {TypeSpelling<std::decay_t<A>>::typescript()...};
    }
};

template <class F>
HostMember signatureOf(std::string_view name, bool isStatic)
{
    using Traits = FunctionTraits<F>;
    using R = std::decay_t<typename Traits::Return>;
    HostMember m;
    m.name = std::string(name);
    m.kind = HostMember::Method;
    m.isStatic = isStatic;
    m.csharpReturn = TypeSpelling<R>::csharp();
    m.typescriptReturn = TypeSpelling<R>::typescript();
    m.csharpParams = Spellings<typename Traits::Args>::csharp();
    m.typescriptParams = Spellings<typename Traits::Args>::typescript();
    return m;
}

}  // namespace detail

/// Wrap any C++ callable as a `NativeFunction`, converting arguments and the
/// return value automatically.
template <class F>
NativeFunction adapt(F f)
{
    using Traits = detail::FunctionTraits<F>;
    using Args = typename Traits::Args;
    constexpr std::size_t n = std::tuple_size_v<Args>;
    return [f](CallInfo& ci) mutable -> Value {
        return detail::callFree<typename Traits::Return, F, Args>(ci.context(), f, ci,
                                                                  std::make_index_sequence<n>{});
    };
}

// ---------------------------------------------------------- host classes ---

/// A static class the host exposes. Created by `Context::hostClass`. In C#
/// it is a static class; in JavaScript an object on the global scope; in
/// TypeScript the same object, with a declaration derived from the C++ types.
class Context::HostClass
{
public:
    HostClass(Context& ctx, void* type) : ctx_(&ctx), type_(type) {}

    /// Expose a free function, a lambda, or any callable.
    template <class F>
    HostClass& method(std::string_view name, F&& fn)
    {
        using D = std::decay_t<F>;
        ctx_->declareHostMember(type_, detail::signatureOf<D>(name, true),
                                adapt(std::forward<F>(fn)));
        return *this;
    }

    /// A read-only value, computed once.
    template <class T>
    HostClass& constant(std::string_view name, T&& v)
    {
        using D = std::decay_t<T>;
        HostMember m;
        m.name = std::string(name);
        m.kind = HostMember::Constant;
        m.isStatic = true;
        m.csharpReturn = detail::TypeSpelling<D>::csharp();
        m.typescriptReturn = detail::TypeSpelling<D>::typescript();
        Value value = Convert<D>::to(*ctx_, std::forward<T>(v));
        ctx_->declareHostMember(type_, m, [value](CallInfo&) { return value; });
        return *this;
    }

    void* handle() const noexcept { return type_; }

private:
    Context* ctx_;
    void* type_;
};

/// A C++ class exposed to every language.
///
///     struct Vec2 { double x, y; double len() const; };
///
///     script::Class<Vec2>(ctx, "Vec2")
///         .constructor<double, double>()
///         .field("x", &Vec2::x)
///         .method("len", &Vec2::len);
///
/// The wrapper object holds a pointer to the instance; a wrapper made by
/// `constructor` owns its instance, one made by `wrap` does not.
template <class T>
class Class
{
public:
    Class(Context& ctx, std::string_view name) : ctx_(&ctx), host_(ctx.hostClass(name))
    {
        detail::ClassInfo<T>::name = std::string(name);
        ctx.bindClassKey(host_.handle(), typeKey<T>());
    }

    /// `new Name(args...)`, constructing a `T` the context owns.
    template <class... A>
    Class& constructor()
    {
        HostMember m;
        m.name = ".ctor";
        m.kind = HostMember::Constructor;
        m.isStatic = false;
        m.csharpReturn = detail::ClassInfo<T>::name;
        m.typescriptReturn = detail::ClassInfo<T>::name;
        m.csharpParams = {detail::TypeSpelling<std::decay_t<A>>::csharp()...};
        m.typescriptParams = {detail::TypeSpelling<std::decay_t<A>>::typescript()...};
        ctx_->declareHostMember(host_.handle(), m, [](CallInfo& call) -> Value {
            return make<A...>(call, std::index_sequence_for<A...>{});
        });
        return *this;
    }

    /// A custom factory: a callable returning `T*`, `T` or `std::unique_ptr<T>`.
    template <class F, std::enable_if_t<!std::is_same_v<std::decay_t<F>, Class>, int> = 0>
    Class& constructor(F f)
    {
        using Traits = detail::FunctionTraits<F>;
        using Args = typename Traits::Args;
        HostMember m;
        m.name = ".ctor";
        m.kind = HostMember::Constructor;
        m.isStatic = false;
        m.csharpReturn = detail::ClassInfo<T>::name;
        m.typescriptReturn = detail::ClassInfo<T>::name;
        m.csharpParams = detail::Spellings<Args>::csharp();
        m.typescriptParams = detail::Spellings<Args>::typescript();
        ctx_->declareHostMember(host_.handle(), m, [f](CallInfo& call) mutable -> Value {
            T* made = build(f, call, Args{}, std::make_index_sequence<std::tuple_size_v<Args>>{});
            return adopt(call, made);
        });
        return *this;
    }

    /// An instance method.
    template <class M, std::enable_if_t<std::is_member_function_pointer_v<M>, int> = 0>
    Class& method(std::string_view name, M member)
    {
        using Traits = detail::FunctionTraits<M>;
        using Args = typename Traits::Args;
        constexpr std::size_t n = std::tuple_size_v<Args>;
        std::string label(name);
        ctx_->declareHostMember(
            host_.handle(), detail::signatureOf<M>(name, false),
            [member, label](CallInfo& call) -> Value {
                return detail::callMember<typename Traits::Return, T, M, Args>(
                    call.context(), self(call, label), member, call,
                    std::make_index_sequence<n>{});
            });
        return *this;
    }

    /// A free function or lambda exposed as a static member.
    template <class F>
    Class& staticMethod(std::string_view name, F&& fn)
    {
        host_.method(name, std::forward<F>(fn));
        return *this;
    }

    /// A public data member, exposed as a read/write property.
    template <class M>
    Class& field(std::string_view name, M T::*member)
    {
        using D = std::decay_t<M>;
        std::string label(name);
        HostMember m;
        m.name = label;
        m.kind = HostMember::Property;
        m.isStatic = false;
        m.csharpReturn = detail::TypeSpelling<D>::csharp();
        m.typescriptReturn = detail::TypeSpelling<D>::typescript();
        ctx_->declareHostMember(
            host_.handle(), m,
            [member, label](CallInfo& call) -> Value {
                return Convert<D>::to(call.context(), self(call, label)->*member);
            },
            [member, label](CallInfo& call) -> Value {
                self(call, label)->*member = call.template arg<D>(0);
                return Value::undefined();
            });
        return *this;
    }

    /// A read-only property backed by a getter method.
    template <class G>
    Class& getter(std::string_view name, G g)
    {
        using Traits = detail::FunctionTraits<G>;
        using R = std::decay_t<typename Traits::Return>;
        std::string label(name);
        HostMember m;
        m.name = label;
        m.kind = HostMember::Property;
        m.isStatic = false;
        m.csharpReturn = detail::TypeSpelling<R>::csharp();
        m.typescriptReturn = detail::TypeSpelling<R>::typescript();
        ctx_->declareHostMember(host_.handle(), m, [g, label](CallInfo& call) -> Value {
            return Convert<R>::to(call.context(), (self(call, label)->*g)());
        });
        return *this;
    }

    /// A static constant.
    template <class V>
    Class& constant(std::string_view name, V&& v)
    {
        host_.constant(name, std::forward<V>(v));
        return *this;
    }

    /// Wrap an instance the host owns. The context will not delete it.
    Value wrap(T* instance) const { return ctx_->native(typeKey<T>(), instance, nullptr); }

    /// Members are visible to every language as soon as they are declared;
    /// this exists for callers used to finishing a class explicitly.
    Class& install() { return *this; }

    /// The JavaScript constructor function, once the class is complete.
    Value constructorValue() const { return ctx_->classConstructor(typeKey<T>()); }
    /// The JavaScript prototype every wrapper of `T` inherits from.
    Value prototype() const { return ctx_->classPrototype(typeKey<T>()); }

private:
    /// The bound instance behind `this`, or an error naming the member.
    static T* self(CallInfo& call, const std::string& label)
    {
        T* p = static_cast<T*>(call.thisValue().nativeHandle(typeKey<T>()));
        if (!p)
            call.fail(ErrorCode::NullReference, label + " called on an unrelated object");
        return p;
    }

    template <class... A, std::size_t... I>
    static Value make(CallInfo& call, std::index_sequence<I...>)
    {
        // Aggregates have no matching constructor, so fall back to list
        // initialisation for them.
        if constexpr (std::is_constructible_v<T, std::decay_t<A>...>)
            return adopt(call, new T(call.template arg<std::decay_t<A>>(I)...));
        else
            return adopt(call, new T{call.template arg<std::decay_t<A>>(I)...});
    }

    template <class F, class... Args, std::size_t... I>
    static T* build(F& f, CallInfo& call, std::tuple<Args...>, std::index_sequence<I...>)
    {
        auto r = f(call.template arg<std::decay_t<Args>>(I)...);
        if constexpr (std::is_pointer_v<decltype(r)>)
            return r;
        else if constexpr (std::is_same_v<decltype(r), std::unique_ptr<T>>)
            return r.release();
        else
            return new T(std::move(r));
    }

    static Value adopt(CallInfo& call, T* instance)
    {
        auto deleter = [](void* p) { delete static_cast<T*>(p); };
        if (call.isConstructorCall()) {
            call.context().attachNative(call.thisValue(), typeKey<T>(), instance, deleter);
            return call.thisValue();
        }
        return call.context().native(typeKey<T>(), instance, deleter);
    }

    Context* ctx_;
    Context::HostClass host_;
};

// ------------------------------------------------- out-of-line templates ---

template <class T>
Value Context::value(T&& v)
{
    return Convert<std::decay_t<T>>::to(*this, std::forward<T>(v));
}

template <class T>
T Context::as(const Value& v)
{
    return Convert<std::decay_t<T>>::from(*this, v);
}

template <class F>
Value Context::fn(std::string_view name, F&& f)
{
    return function(name, adapt(std::forward<F>(f)),
                    int(std::tuple_size_v<typename detail::FunctionTraits<std::decay_t<F>>::Args>));
}

template <class T>
T Value::as() const
{
    if (Context* ctx = context())
        return Convert<std::decay_t<T>>::from(*ctx, *this);
    // A primitive belongs to no context. Conversions out of script never
    // allocate, so an empty one stands in; making it costs about a microsecond.
    Context detached;
    return Convert<std::decay_t<T>>::from(detached, *this);
}

template <class T>
T CallInfo::arg(std::size_t i) const
{
    return Convert<std::decay_t<T>>::from(context(), arg(i));
}

template <class T, std::enable_if_t<!std::is_same_v<std::decay_t<T>, Value>, int>>
void Value::set(std::string_view key, T&& v)
{
    Context* ctx = context();
    if (!ctx)
        throw Error(ErrorCode::InvalidOperation, "the value does not belong to a context");
    set(key, Convert<std::decay_t<T>>::to(*ctx, std::forward<T>(v)));
}

template <class T, std::enable_if_t<!std::is_same_v<std::decay_t<T>, Value>, int>>
void Value::set(const char* key, T&& v)
{
    set(std::string_view(key), std::forward<T>(v));
}

template <class... A>
Value Value::operator()(A&&... args) const
{
    Context* ctx = context();
    if (!ctx)
        throw Error(ErrorCode::InvalidOperation, "the value does not belong to a context");
    std::vector<Value> converted{
        Convert<std::decay_t<A>>::to(*ctx, std::forward<A>(args))...};
    return call(converted);
}

template <class R, class... A>
Value Convert<std::function<R(A...)>>::to(Context& ctx, std::function<R(A...)> f)
{
    return ctx.function(
        "",
        [f](CallInfo& ci) mutable -> Value {
            return detail::callFree<R, std::function<R(A...)>, std::tuple<A...>>(
                ci.context(), f, ci, std::make_index_sequence<sizeof...(A)>{});
        },
        int(sizeof...(A)));
}

}  // namespace script
