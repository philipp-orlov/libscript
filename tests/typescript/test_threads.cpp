// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Contexts share nothing, and one Context serializes concurrent callers.

#include <atomic>
#include <thread>
#include <vector>

#include "lang.hpp"
#include "tests/test_util.hpp"

TEST(independentContextsRunInParallel)
{
    std::atomic<int> failures{0};
    std::vector<std::thread> workers;
    for (int t = 0; t < 8; ++t) {
        workers.emplace_back([t, &failures] {
            ts::Context ctx;
            ctx.global().set("seed", ts::Value(t));
            const double sum =
                ctx.eval("let total = 0; for (let i = 0; i < 2000; i++) total += i + seed; total")
                    .toDouble();
            if (sum != 1999.0 * 2000 / 2 + 2000.0 * t)
                ++failures;
        });
    }
    for (std::thread& w : workers)
        w.join();
    CHECK_EQ(failures.load(), 0);
}

TEST(oneContextSurvivesConcurrentUse)
{
    ts::Context ctx;
    ctx.eval("var hits = 0; function bump() { hits++; return hits; }");

    std::vector<std::thread> workers;
    for (int t = 0; t < 4; ++t) {
        workers.emplace_back([&ctx] {
            for (int i = 0; i < 250; ++i)
                ctx.eval("bump()");
        });
    }
    for (std::thread& w : workers)
        w.join();
    CHECK_EQ(ctx.eval("hits").toDouble(), 1000.0);
}

TEST(lockGroupsSeveralOperations)
{
    ts::Context ctx;
    ctx.eval("globalThis.list = []");
    std::vector<std::thread> workers;
    for (int t = 0; t < 4; ++t) {
        workers.emplace_back([&ctx, t] {
            for (int i = 0; i < 50; ++i) {
                ts::Context::Lock lock(ctx);
                ts::Value list = ctx.global().get("list");
                list.push(ts::Value(t));
                list.push(ts::Value(t));
            }
        });
    }
    for (std::thread& w : workers)
        w.join();
    CHECK_EQ(ctx.eval("list.length").toDouble(), 400.0);
    CHECK_EQ(ctx.eval("list.every((v, i) => i % 2 === 1 ? v === list[i - 1] : true)").toString(),
             std::string("true"));
}

TEST(nativeCallbacksReenterSafely)
{
    ts::Context ctx;
    ctx.global().set("twice", ctx.function("twice", [](ts::CallInfo& ci) {
        ts::Value fn = ci.arg(0);
        return ts::Value(fn.call({ci.arg(1)}).toDouble() + fn.call({ci.arg(1)}).toDouble());
    }));
    CHECK_EQ(ctx.eval("twice(x => x * 3, 7)").toDouble(), 42.0);
}
