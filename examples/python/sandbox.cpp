// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Running Python you do not trust: no host access, a memory ceiling, a time
// budget and a cancel switch.
//
//   ./python_sandbox

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include "script/script.hpp"

namespace {

script::Context::Options languageOptions()
{
    script::Context::Options options;
    options.language = script::Language::Python;
    return options;
}

/// Runs `source` with the given budgets and reports what happened.
void attempt(const char* label, const char* source, std::size_t memoryLimit,
             std::chrono::milliseconds timeLimit)
{
    script::Context::Options options = languageOptions();
    options.memoryLimit = memoryLimit;
    options.stackLimit = 128;
    options.gcThreshold = 1u << 12;
    script::Context ctx(options);

    // Output goes through the host, and only there.
    ctx.setPrintHandler([label](std::string_view text, bool) {
        std::printf("%-14s | %.*s", label, int(text.size()), text.data());
    });
    const auto deadline = std::chrono::steady_clock::now() + timeLimit;
    ctx.setInterruptHandler([deadline] { return std::chrono::steady_clock::now() > deadline; });

    try {
        const std::string result = ctx.eval(source, label).toString();
        std::printf("%-14s ok: %s\n", label, result.c_str());
    } catch (const script::Exception& e) {
        std::printf("%-14s script error: %s\n", label, e.what());
    } catch (const script::Error& e) {
        std::printf("%-14s stopped: %s\n", label, e.what());
    }
    const auto stats = ctx.stats();
    std::printf("               %llu objects, %llu bytes, %llu collections\n",
                (unsigned long long)stats.liveObjects, (unsigned long long)stats.heapBytes,
                (unsigned long long)stats.gcRuns);
}

}  // namespace

int main()
{
    using namespace std::chrono_literals;

    attempt("well-behaved", "sum(n * n for n in range(1, 6))", 8u << 20, 1000ms);
    attempt("printing", "print('hello from the sandbox')\n'done'", 8u << 20, 1000ms);

    // There is no `open`, no `os`, no `sys`, no `subprocess`, no `eval`: the
    // built-ins are the pure ones, and `import` knows only math and random.
    attempt("no files", "open('/etc/passwd').read()", 8u << 20, 1000ms);
    attempt("no os", "import os\nos.listdir('/')", 8u << 20, 1000ms);
    attempt("no eval", "eval('1 + 1')", 8u << 20, 1000ms);

    attempt("infinite loop", "n = 0\nwhile True:\n    n += 1\nn", 8u << 20, 50ms);
    attempt("runaway heap", "a = []\nwhile True:\n    a.append({'pad': 'x' * 64})\n'never'", 2u << 20, 5000ms);
    attempt("deep recursion", "def f(n):\n    return f(n + 1)\nf(0)", 8u << 20, 1000ms);

    // Cancellation from another thread uses the same interrupt hook.
    std::atomic<bool> cancel{false};
    script::Context ctx(languageOptions());
    ctx.setInterruptHandler([&cancel] { return cancel.load(); });
    std::thread canceller([&cancel] {
        std::this_thread::sleep_for(20ms);
        cancel = true;
    });
    try {
        ctx.eval("while True:\n    pass\n");
    } catch (const script::Error& e) {
        std::printf("%-14s cancelled: %s\n", "external", e.what());
    }
    canceller.join();
    return 0;
}
