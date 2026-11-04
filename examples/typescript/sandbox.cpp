// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Running untrusted script safely: no host access, a memory ceiling, a time
// budget and a cancel switch.
//
//   ./sandbox

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include "script/script.hpp"

namespace {

/// Every snippet in this example is TypeScript, so the context says so once
/// instead of every `eval` naming a file.
script::Context::Options languageOptions()
{
    script::Context::Options options;
    options.language = script::Language::TypeScript;
    return options;
}

}  // namespace

namespace {

/// Runs `source` with the given budgets and reports what happened.
void attempt(const char* label, const char* source, std::size_t memoryLimit,
             std::chrono::milliseconds timeLimit)
{
    script::Context::Options options = languageOptions();
    options.memoryLimit = memoryLimit;
    options.stackLimit = 128;
    options.gcThreshold = 1u << 12;
    script::Context ctx(options);

    // Nothing in the default global object reaches the host: there is no file
    // system, no network and no way to compile new code from a string.
    ctx.eval("delete globalThis.console");

    const auto deadline = std::chrono::steady_clock::now() + timeLimit;
    ctx.setInterruptHandler([deadline] { return std::chrono::steady_clock::now() > deadline; });

    try {
        const std::string result = ctx.eval(source, label).toString();
        std::printf("%-12s ok: %s\n", label, result.c_str());
    } catch (const script::Exception& e) {
        std::printf("%-12s script error: %s\n", label, e.what());
    } catch (const script::Error& e) {
        std::printf("%-12s stopped: %s\n", label, e.what());
    }
    const auto stats = ctx.stats();
    std::printf("             %llu objects, %llu bytes, %llu collections\n",
                (unsigned long long)stats.liveObjects, (unsigned long long)stats.heapBytes,
                (unsigned long long)stats.gcRuns);
}

}  // namespace

int main()
{
    using namespace std::chrono_literals;

    attempt("well-behaved", "[1,2,3,4,5].map(n => n * n).join(',')", 8u << 20, 1000ms);
    attempt("infinite-loop", "let n = 0; while (true) n++; n", 8u << 20, 50ms);
    attempt("runaway-heap", "const a = []; for (;;) a.push({ pad: 'x'.repeat(64) }); 'never'",
            2u << 20, 5000ms);
    attempt("deep-recursion", "function f(n) { return f(n + 1); } f(0)", 8u << 20, 1000ms);
    attempt("no-host-access", "typeof require + ' ' + typeof process + ' ' + typeof eval", 8u << 20,
            1000ms);

    // Cancellation from another thread uses the same interrupt hook.
    std::atomic<bool> cancel{false};
    script::Context ctx(languageOptions());
    ctx.setInterruptHandler([&cancel] { return cancel.load(); });
    std::thread canceller([&cancel] {
        std::this_thread::sleep_for(20ms);
        cancel = true;
    });
    try {
        ctx.eval("for (;;) {}");
    } catch (const script::Exception& e) {
        std::printf("%-12s cancelled: %s\n", "external", e.what());
    }
    canceller.join();
    return 0;
}
