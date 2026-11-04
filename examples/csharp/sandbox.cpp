// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Running code you do not trust: limits, timeouts and no ambient authority.
#include <chrono>
#include <cstdio>

#include "script/script.hpp"

namespace {

void attempt(script::Context& ctx, const char* label, const char* source)
{
    try {
        const script::Value result = ctx.eval(source, label);
        std::printf("%-18s ok: %s\n", label, result.toString().c_str());
    } catch (const script::CompileError& error) {
        std::printf("%-18s rejected: %s\n", label, error.diagnostics().front().message.c_str());
    } catch (const script::Error& error) {
        std::printf("%-18s stopped: %s\n", label, error.what());
    }
}

}  // namespace

int main()
{
    script::Context::Options options;
    options.memoryLimit = 4u << 20;  // four megabytes of heap
    options.stackLimit = 128;        // and a shallow call stack

    script::Context ctx(options);

    // A wall-clock budget. The handler is polled while the program runs.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
    ctx.setInterruptHandler([deadline] { return std::chrono::steady_clock::now() > deadline; });

    // Output goes wherever the host says, not to the process's stdout.
    ctx.setPrintHandler([](std::string_view text, bool isError) {
        std::printf("%s%.*s", isError ? "[err] " : "[out] ", int(text.size()), text.data());
    });

    attempt(ctx, "arithmetic", "return 6 * 7;");
    attempt(ctx, "printing", "Console.WriteLine(\"hello from the sandbox\"); return 0;");

    // There is no file system, no network, no reflection and no `unsafe`: a
    // program can only reach what the host handed it.
    attempt(ctx, "no file system", "return System.IO.File.ReadAllText(\"/etc/passwd\");");
    attempt(ctx, "no reflection", "return typeof(object);");
    attempt(ctx, "no unsafe", "unsafe { int* p = null; }");

    attempt(ctx, "runaway loop", "while (true) { }");
    attempt(ctx, "deep recursion",
            "static class M { public static int F(int n) => F(n + 1); } return M.F(0);");
    attempt(ctx, "memory hog", R"(
        var list = new System.Collections.Generic.List<string>();
        for (int i = 0; i < 10000000; i++) list.Add("padding padding padding " + i);
        return list.Count;
    )");

    const script::Context::Stats stats = ctx.stats();
    std::printf("\nheap %llu bytes, %llu objects, %llu collections\n",
                static_cast<unsigned long long>(stats.heapBytes),
                static_cast<unsigned long long>(stats.liveObjects),
                static_cast<unsigned long long>(stats.gcRuns));
    return 0;
}
