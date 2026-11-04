// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The shared value model: what holds whichever languages this build contains.
#include <cmath>

#include "script/script.hpp"
#include "tests/test_util.hpp"

using script::Type;
using script::Value;

TEST(defaultIsNullAndUndefinedIsDistinct)
{
    CHECK(Value().isNull());
    CHECK(!Value().isUndefined());
    CHECK(Value::undefined().isUndefined());
    CHECK(Value::undefined().isNullish());
    CHECK(Value(nullptr).isNull());
    CHECK(!Value::undefined().strictEquals(Value::null()));
    CHECK(Value::null().strictEquals(Value::null()));
    CHECK(Value::undefined() == Value::undefined());
}

TEST(numericTagsAndConversions)
{
    CHECK_EQ(int(Value(42).type()), int(Type::Int));
    CHECK_EQ(int(Value(std::int64_t(42)).type()), int(Type::Long));
    CHECK_EQ(int(Value(4.5).type()), int(Type::Double));
    CHECK_EQ(int(Value(4.5f).type()), int(Type::Float));
    CHECK(Value(42).isNumber());
    CHECK(!Value(u'a').isNumber());
    CHECK(Value(u'a').isChar());
    CHECK_EQ(Value(3.9).toInt(), 3);
    CHECK_EQ(Value(-3.9).toInt(), -3);
    CHECK_EQ(Value(1e10).toInt(), 1410065408);  // ECMAScript ToInt32
    CHECK_EQ(Value(std::int64_t(1) << 40).toInt(), 0);
    CHECK_EQ(Value(std::int64_t(1) << 40).toLong(), std::int64_t(1) << 40);
    CHECK_EQ(Value(true).toInt(), 1);
    CHECK_EQ(Value(u'A').toInt(), 65);
    CHECK(std::isnan(Value::undefined().toDouble()));
    CHECK_EQ(Value::null().toDouble(), 0.0);
    CHECK_EQ(Value(7).toDouble(), 7.0);
}

TEST(truthiness)
{
    CHECK(!Value().toBool());
    CHECK(!Value::undefined().toBool());
    CHECK(!Value(false).toBool());
    CHECK(!Value(0).toBool());
    CHECK(!Value(0.0).toBool());
    CHECK(!Value(std::nan("")).toBool());
    CHECK(Value(1).toBool());
    CHECK(Value(-0.5).toBool());
    CHECK(Value(u'x').toBool());
    script::Context ctx;
    CHECK(!ctx.str("").toBool());
    CHECK(ctx.str("a").toBool());
}

TEST(primitivesPrintTheJsonWay)
{
    CHECK_EQ(Value().toString(), std::string("null"));
    CHECK_EQ(Value::undefined().toString(), std::string("undefined"));
    CHECK_EQ(Value(true).toString(), std::string("true"));
    CHECK_EQ(Value(false).toString(), std::string("false"));
    CHECK_EQ(Value(42).toString(), std::string("42"));
    CHECK_EQ(Value(std::int64_t(-7)).toString(), std::string("-7"));
    CHECK_EQ(Value(0.1 + 0.2).toString(), std::string("0.30000000000000004"));
    CHECK_EQ(Value(1e21).toString(), std::string("1e+21"));
    CHECK_EQ(Value(1e20).toString(), std::string("100000000000000000000"));
    CHECK_EQ(Value(1e-7).toString(), std::string("1e-7"));
    CHECK_EQ(Value(-0.0).toString(), std::string("0"));
    CHECK_EQ(Value(u'A').toString(), std::string("A"));
    CHECK_EQ(Value(std::nan("")).toString(), std::string("NaN"));
    CHECK_EQ(Value(2.5).inspect(), std::string("2.5"));
}

TEST(jsonOfPrimitives)
{
    CHECK_EQ(Value::undefined().toJson(), std::string(""));
    CHECK_EQ(Value().toJson(), std::string("null"));
    CHECK_EQ(Value(3).toJson(), std::string("3"));
    CHECK_EQ(Value(std::nan("")).toJson(), std::string("null"));
    script::Context ctx;
    CHECK_EQ(ctx.str("a\"b\n").toJson(), std::string("\"a\\\"b\\n\""));
}

TEST(equality)
{
    CHECK(Value(1) == Value(1.0));
    CHECK(Value(1) == Value(std::int64_t(1)));
    CHECK(Value(u'a') == Value(97));
    CHECK(Value(1) != Value(2));
    CHECK(Value(true) != Value(1));
    CHECK(Value() != Value(0));
    CHECK(Value(std::nan("")) != Value(std::nan("")));
    script::Context ctx;
    const Value a = ctx.str("hello world, longer than the interning limit of 32 chars");
    const Value b = ctx.str("hello world, longer than the interning limit of 32 chars");
    CHECK(a == b);
    CHECK(!a.sameReference(b));
    CHECK(a.sameReference(a));
    CHECK(ctx.str("x") == ctx.str("x"));
    CHECK(Value(1).sameReference(Value(1)));
    CHECK(!Value(1).sameReference(Value(2)));
}

TEST(stringsAreSharedCells)
{
    script::Context ctx;
    const Value s = ctx.str("héllo");
    CHECK(s.isString());
    CHECK(s.isReference());
    CHECK_EQ(s.length(), 5u);
    CHECK_EQ(s.get(1u).toString(), std::string("é"));
    CHECK_EQ(s.toString(), std::string("héllo"));
    CHECK_EQ(s.inspect(), std::string("\"héllo\""));
    CHECK(s.context() == &ctx);
    CHECK(Value(1).context() == nullptr);
    CHECK_THROWS(s.get(9u), script::ErrorCode::IndexOutOfRange);
    CHECK_EQ(ctx.str("42.5").toDouble(), 42.5);
}

TEST(valuesSurviveContextMove)
{
    script::Context a;
    Value s = a.str("moved");
    script::Context b(std::move(a));
    CHECK(s.context() == &b);
    CHECK_EQ(s.toString(), std::string("moved"));
    // A Value must not outlive its Context, and `s` was declared before `b`.
    s = Value();
}

TEST(heapStatisticsCountSharedCells)
{
    script::Context ctx;
    const auto before = ctx.stats();
    {
        std::vector<Value> keep;
        for (int i = 0; i < 1000; ++i)
            keep.push_back(ctx.str("string number " + std::to_string(i) + " is long enough"));
        CHECK(ctx.stats().liveStrings >= before.liveStrings + 1000);
    }
    ctx.collectGarbage();
    CHECK_EQ(ctx.stats().liveStrings, before.liveStrings);
    CHECK(ctx.stats().reservedBytes > 0);
}

TEST(interruptAndLimitsAreSharedOptions)
{
    script::Context::Options options;
    options.memoryLimit = 1u << 20;
    options.stackLimit = 32;
    options.threadSafe = false;
    script::Context ctx(options);
    CHECK(ctx.stats().heapBytes < options.memoryLimit);
}
