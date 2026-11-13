// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Reading and writing the elements of typed arrays.
#pragma once

#include <cmath>
#include <cstring>

#include "script/internal/vm/object.hpp"
#include "script/internal/vm/operators.hpp"

namespace script {
namespace internal {
namespace vm {

/// IEEE half precision: the nearest half (ties to even) of a double, as its bits, and back.
inline std::uint16_t doubleToHalf(double d) noexcept
{
    if (d != d)
        return 0x7E00;
    const std::uint16_t sign = std::signbit(d) ? 0x8000 : 0;
    const double a = std::fabs(d);
    if (a >= 65520.0)
        return std::uint16_t(sign | 0x7C00);
    if (a < 6.103515625e-05) {  // 2^-14: subnormal halves
        const double m = std::nearbyint(a * 16777216.0);  // 2^24
        return std::uint16_t(sign | std::uint16_t(m));
    }
    int ex;
    const double m = std::frexp(a, &ex);  // m in [0.5, 1)
    int e = ex - 1;
    double mantissa = std::nearbyint((m * 2 - 1) * 1024);
    if (mantissa >= 1024) {
        mantissa = 0;
        ++e;
    }
    if (e > 15)
        return std::uint16_t(sign | 0x7C00);
    return std::uint16_t(sign | ((e + 15) << 10) | std::uint16_t(mantissa));
}

inline double halfToDouble(std::uint16_t h) noexcept
{
    const double sign = (h & 0x8000) ? -1.0 : 1.0;
    const int exp = (h >> 10) & 31, mant = h & 1023;
    if (exp == 0)
        return sign * std::ldexp(double(mant), -24);
    if (exp == 31)
        return mant ? std::nan("") : sign * HUGE_VAL;
    return sign * std::ldexp(double(1024 + mant), exp - 25);
}

/// A number written to an element of a kind that holds numbers, as the language converts it (modulo 2^n, clamped).
inline void storeNumber(ElemKind kind, std::uint8_t* p, double d) noexcept
{
    switch (kind) {
        case ElemKind::Int8:
        case ElemKind::Uint8: {
            const std::uint8_t v = std::uint8_t(toInt32(d));
            *p = v;
            break;
        }
        case ElemKind::Uint8Clamped: {
            const double c = d != d ? 0 : d < 0 ? 0 : d > 255 ? 255 : d;
            // Round half to even.
            double r = std::floor(c);
            const double diff = c - r;
            if (diff > 0.5 || (diff == 0.5 && std::fmod(r, 2) != 0))
                r += 1;
            *p = std::uint8_t(r);
            break;
        }
        case ElemKind::Int16:
        case ElemKind::Uint16: {
            const std::uint16_t v = std::uint16_t(toInt32(d));
            std::memcpy(p, &v, 2);
            break;
        }
        case ElemKind::Int32:
        case ElemKind::Uint32: {
            const std::uint32_t v = std::uint32_t(toInt32(d));
            std::memcpy(p, &v, 4);
            break;
        }
        case ElemKind::Float32: {
            const float v = float(d);
            std::memcpy(p, &v, 4);
            break;
        }
        case ElemKind::Float64: std::memcpy(p, &d, 8); break;
        case ElemKind::Float16: {
            const std::uint16_t v = doubleToHalf(d);
            std::memcpy(p, &v, 2);
            break;
        }
        default: break;
    }
}

/// An element of a kind that holds numbers.
inline JSValue loadNumber(ElemKind kind, const std::uint8_t* p) noexcept
{
    switch (kind) {
        case ElemKind::Int8: return JSValue::int32(std::int8_t(*p));
        case ElemKind::Uint8:
        case ElemKind::Uint8Clamped: return JSValue::int32(*p);
        case ElemKind::Int16: {
            std::int16_t v;
            std::memcpy(&v, p, 2);
            return JSValue::int32(v);
        }
        case ElemKind::Uint16: {
            std::uint16_t v;
            std::memcpy(&v, p, 2);
            return JSValue::int32(v);
        }
        case ElemKind::Int32: {
            std::int32_t v;
            std::memcpy(&v, p, 4);
            return JSValue::int32(v);
        }
        case ElemKind::Uint32: {
            std::uint32_t v;
            std::memcpy(&v, p, 4);
            return v <= 0x7FFFFFFFu ? JSValue::int32(std::int32_t(v)) : JSValue::boxDouble(double(v));
        }
        case ElemKind::Float32: {
            float v;
            std::memcpy(&v, p, 4);
            return JSValue::boxNumber(double(v));
        }
        case ElemKind::Float64: {
            double v;
            std::memcpy(&v, p, 8);
            return JSValue::boxNumber(v);
        }
        case ElemKind::Float16: {
            std::uint16_t v;
            std::memcpy(&v, p, 2);
            return JSValue::boxNumber(halfToDouble(v));
        }
        default: return JSValue::undefined();
    }
}

inline bool isBigKind(ElemKind k) noexcept { return k == ElemKind::BigInt64 || k == ElemKind::BigUint64; }

/// Element `i` as a value, owned. A 64-bit kind makes a BigInt.
JSValue typedGet(VM& vm, const JSTypedArray* t, std::uint32_t i);
/// Convert `v` as the language does for this kind (even when `i` is out of range) and store it: false, with an exception
/// pending, when the conversion failed.
bool typedSet(VM& vm, JSTypedArray* t, double index, JSValue v);
/// What an element of `kind` is made from `v`: a number, or a BigInt for the 64-bit kinds, owned; false with an exception
/// pending when the conversion failed.
bool typedConvert(VM& vm, ElemKind kind, JSValue v, JSValue* out);

}  // namespace vm
}  // namespace internal
}  // namespace script
