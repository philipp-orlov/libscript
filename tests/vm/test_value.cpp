// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// V-1: the NaN-boxed value.
#include <cmath>
#include <cstring>
#include <limits>
#include <random>

#include "script/internal/vm/value.hpp"
#include "tests/test_util.hpp"

using namespace script::internal::vm;

namespace {

std::uint64_t bitsOf(double d)
{
    std::uint64_t b;
    std::memcpy(&b, &d, sizeof b);
    return b;
}
double fromBits(std::uint64_t b)
{
    double d;
    std::memcpy(&d, &b, sizeof d);
    return d;
}

void checkIsOnlyDouble(JSValue v)
{
    CHECK(v.isNumber());
    CHECK(v.isDouble());
    CHECK(!v.isInt32());
    CHECK(!v.isCell());
    CHECK(!v.isBool());
    CHECK(!v.isNullish());
    CHECK(!v.isException());
}

}  // namespace

TEST(sizeIsEightBytes)
{
    CHECK_EQ(sizeof(JSValue), std::size_t(8));
}

TEST(doublesRoundTrip)
{
    const double samples[] = {0.0,
                              -0.0,
                              1.0,
                              -1.5,
                              3.141592653589793,
                              std::numeric_limits<double>::denorm_min(),
                              -std::numeric_limits<double>::denorm_min(),
                              std::numeric_limits<double>::min(),
                              std::numeric_limits<double>::max(),
                              std::numeric_limits<double>::lowest(),
                              std::numeric_limits<double>::infinity(),
                              -std::numeric_limits<double>::infinity(),
                              4294967296.0,
                              2147483648.0};
    for (double d : samples) {
        const JSValue v = JSValue::boxDouble(d);
        checkIsOnlyDouble(v);
        CHECK_EQ(bitsOf(v.asDouble()), bitsOf(d));
        CHECK_EQ(bitsOf(v.asNumber()), bitsOf(d));
    }
}

TEST(everyNanBecomesTheCanonicalNan)
{
    std::mt19937_64 rng(12345);
    for (int i = 0; i < 20000; ++i) {
        // Any exponent-all-ones pattern with a non-zero mantissa, both signs: the ones a program can make with typed
        // arrays, and the ones that would otherwise land among the tags.
        std::uint64_t mantissa = rng() & 0x000FFFFFFFFFFFFFull;
        if (mantissa == 0)
            mantissa = 1;
        const std::uint64_t bits = (rng() & 0x8000000000000000ull) | 0x7FF0000000000000ull | mantissa;
        const JSValue v = JSValue::boxDouble(fromBits(bits));
        checkIsOnlyDouble(v);
        CHECK_EQ(bitsOf(v.asDouble()), kCanonicalNaN);
    }
    // The extremes by hand.
    for (std::uint64_t bits : {0x7FF0000000000001ull, 0xFFFFFFFFFFFFFFFFull, 0xFFF8000000000000ull,
                               0x7FFFFFFFFFFFFFFFull, 0xFFF0000000000001ull}) {
        const JSValue v = JSValue::boxDouble(fromBits(bits));
        checkIsOnlyDouble(v);
        CHECK(std::isnan(v.asDouble()));
    }
}

TEST(everyBoxedDoubleStaysBelowTheInt32Tag)
{
    std::mt19937_64 rng(777);
    for (int i = 0; i < 100000; ++i) {
        const JSValue v = JSValue::boxDouble(fromBits(rng()));
        CHECK(v.isDouble());
        CHECK(v.bits < kNumberTag);
        CHECK(v.bits >= kDoubleOffset);
    }
}

TEST(int32Limits)
{
    for (std::int32_t i : {0, 1, -1, 42, std::numeric_limits<std::int32_t>::max(), std::numeric_limits<std::int32_t>::min(),
                           65536, -65536}) {
        const JSValue v = JSValue::int32(i);
        CHECK(v.isInt32());
        CHECK(v.isNumber());
        CHECK(!v.isDouble());
        CHECK(!v.isCell());
        CHECK(!v.isBool());
        CHECK(!v.isNullish());
        CHECK_EQ(v.asInt32(), i);
        CHECK_EQ(v.asNumber(), double(i));
    }
    CHECK_EQ(JSValue::int32(-1).bits, 0xFFFE0000FFFFFFFFull);
    CHECK_EQ(JSValue::int32(0).bits, 0xFFFE000000000000ull);
}

TEST(immediates)
{
    const JSValue all[] = {JSValue::null(),  JSValue::undefined(),  JSValue::boolean(false),
                           JSValue::boolean(true), JSValue::hole(), JSValue::uninit()};
    for (JSValue v : all) {
        CHECK(!v.isCell());
        CHECK(!v.isNumber());
        CHECK(!v.isInt32());
        CHECK(!v.isDouble());
        CHECK(!v.isException());
    }
    CHECK(JSValue::null().isNullish() && JSValue::undefined().isNullish());
    CHECK(!JSValue::boolean(false).isNullish() && !JSValue::hole().isNullish() && !JSValue::uninit().isNullish());
    CHECK(JSValue::boolean(true).isBool() && JSValue::boolean(false).isBool());
    CHECK(!JSValue::null().isBool() && !JSValue::undefined().isBool());
    CHECK(JSValue::boolean(true).asBool() && !JSValue::boolean(false).asBool());
    CHECK(JSValue::null().isNull() && !JSValue::undefined().isNull());
    CHECK(JSValue::undefined().isUndefined());
    CHECK(JSValue::hole().isHole() && JSValue::uninit().isUninit());
    CHECK(JSValue::exception().isException());
    CHECK(!JSValue::exception().isCell());
    CHECK_EQ(JSValue::null().bits, std::uint64_t(0x02));
    CHECK_EQ(JSValue::boolean(false).bits, std::uint64_t(0x06));
    CHECK_EQ(JSValue::boolean(true).bits, std::uint64_t(0x07));
    CHECK_EQ(JSValue::undefined().bits, std::uint64_t(0x0A));
    CHECK_EQ(JSValue::hole().bits, std::uint64_t(0x12));
    CHECK_EQ(JSValue::uninit().bits, std::uint64_t(0x22));
}

TEST(cellsAreSixteenByteAlignedPointers)
{
    alignas(16) static char storage[64];
    const JSValue v = JSValue::cell(reinterpret_cast<script::detail::GCObject*>(storage));
    CHECK(v.isCell());
    CHECK(!v.isNumber());
    CHECK(!v.isNullish());
    CHECK(!v.isBool());
    CHECK(v.asCell() == reinterpret_cast<script::detail::GCObject*>(storage));
    CHECK_EQ(v.bits, std::uint64_t(reinterpret_cast<std::uintptr_t>(storage)));
}

TEST(boxNumberChoosesTheRepresentation)
{
    CHECK(JSValue::boxNumber(-0.0).isDouble());
    CHECK(std::signbit(JSValue::boxNumber(-0.0).asDouble()));
    CHECK(JSValue::boxNumber(0.0).isInt32());
    CHECK(JSValue::boxNumber(5.0).isInt32());
    CHECK_EQ(JSValue::boxNumber(5.0).asInt32(), 5);
    CHECK(JSValue::boxNumber(2147483647.0).isInt32());
    CHECK(JSValue::boxNumber(-2147483648.0).isInt32());
    CHECK(JSValue::boxNumber(2147483648.0).isDouble());
    CHECK(JSValue::boxNumber(-2147483649.0).isDouble());
    CHECK(JSValue::boxNumber(0.5).isDouble());
    CHECK(JSValue::boxNumber(std::nan("")).isDouble());
    CHECK(JSValue::boxNumber(std::numeric_limits<double>::infinity()).isDouble());
    CHECK(JSValue::boxNumber(1e300).isDouble());
    // The second argument of a mixed pair: boxDouble never narrows.
    CHECK(JSValue::boxDouble(5.0).isDouble());
}

TEST(boxNumberAgreesWithAnIntegerLoop)
{
    std::mt19937_64 rng(99);
    for (int i = 0; i < 100000; ++i) {
        const std::int64_t n = std::int64_t(rng() % 8589934592ull) - 4294967296ll;
        const JSValue v = JSValue::boxNumber(double(n));
        CHECK_EQ(v.asNumber(), double(n));
        const bool fits = n >= std::numeric_limits<std::int32_t>::min() && n <= std::numeric_limits<std::int32_t>::max();
        CHECK_EQ(v.isInt32(), fits);
    }
}

TEST(handleCountsCells)
{
    // A cell of the old family: a box, whose count lives at offset 8 like every cell's.
    struct Counted
    {
        std::uint32_t pad0[2];
        std::uint32_t rc;
        std::uint32_t pad1;
    };
    // Not destroyed: the count is made to stay above zero.
    alignas(16) static Counted cell = {{0, 0}, 5, 0};
    const JSValue v = JSValue::cell(reinterpret_cast<script::detail::GCObject*>(&cell));
    {
        Handle a = Handle::retained(v);
        CHECK_EQ(cell.rc, std::uint32_t(6));
        Handle b = a;
        CHECK_EQ(cell.rc, std::uint32_t(7));
        Handle c = std::move(b);
        CHECK_EQ(cell.rc, std::uint32_t(7));
        CHECK(b.get().isUndefined());
        a = Handle();
        CHECK_EQ(cell.rc, std::uint32_t(6));
        JSValue taken = c.take();
        CHECK(taken == v);
        CHECK_EQ(cell.rc, std::uint32_t(6));
        release(taken);
    }
    CHECK_EQ(cell.rc, std::uint32_t(5));
    // Numbers and immediates are never counted.
    retain(JSValue::int32(3));
    release(JSValue::boxDouble(1.5));
    release(JSValue::undefined());
    release(JSValue::exception());
}
