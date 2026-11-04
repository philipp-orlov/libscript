// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Running C++ you do not trust: limits, a time budget, no ambient authority,
// and exceptions that stop at the boundary.
//
//   ./cpp_sandbox

#include <chrono>
#include <cstdio>

#include "script/script.hpp"

namespace {

script::Context::Options languageOptions()
{
    script::Context::Options options;
    options.language = script::Language::Cpp;
    return options;
}

void attempt(script::Context& ctx, const char* label, const char* source)
{
    try {
        const script::Value result = ctx.eval(source, label);
        std::printf("%-18s ok: %s\n", label, result.toString().c_str());
    } catch (const script::CompileError& error) {
        std::printf("%-18s rejected: %s\n", label, error.diagnostics().front().message.c_str());
    } catch (const script::Exception& error) {
        std::printf("%-18s threw: %s\n", label, error.what());
    } catch (const script::Error& error) {
        std::printf("%-18s stopped: %s\n", label, error.what());
    }
}

}  // namespace

int main()
{
    script::Context::Options options = languageOptions();
    options.memoryLimit = 4u << 20;  // four megabytes of heap
    options.stackLimit = 128;        // and a shallow call stack
    script::Context ctx(options);

    // A wall-clock budget, polled while the program runs.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
    ctx.setInterruptHandler([deadline] { return std::chrono::steady_clock::now() > deadline; });

    // Output goes wherever the host says, not to the process's stdout.
    ctx.setPrintHandler([](std::string_view text, bool isError) {
        std::printf("%s%.*s", isError ? "[err] " : "[out] ", int(text.size()), text.data());
    });

    attempt(ctx, "arithmetic", "int main() { return 6 * 7; }");
    attempt(ctx, "printing", "#include <iostream>\nint main() { std::cout << \"hello from the sandbox\\n\"; std::cerr << \"and an error\\n\"; return 0; }");

    // There is no file system, no process, no input and no raw memory: a
    // program reaches only what the host handed it.
    attempt(ctx, "no files", "#include <fstream>\nint main() { std::ifstream f(\"/etc/passwd\"); return 0; }");
    attempt(ctx, "no processes", "#include <cstdlib>\nint main() { system(\"rm -rf /\"); return 0; }");
    attempt(ctx, "no input", "#include <iostream>\nint main() { int n; std::cin >> n; return n; }");
    attempt(ctx, "no addresses", "int main() { int x = 1; int* p = &x; p++; return *p; }");
    attempt(ctx, "no casts to memory", "int main() { long a = 1; return *reinterpret_cast<int*>(a); }");

    // Runtime misbehaviour is an exception, never undefined behaviour.
    attempt(ctx, "bounds", "#include <vector>\nint main() { std::vector<int> v(2); return v[5]; }");
    attempt(ctx, "null pointer", "struct P { int x; };\nint main() { P* p = nullptr; return p->x; }");
    attempt(ctx, "own exception", "#include <stdexcept>\nint main() { throw std::runtime_error(\"give up\"); }");
    attempt(ctx, "caught inside", R"(
        #include <stdexcept>
        int main() {
            try { throw std::out_of_range("index"); }
            catch (const std::exception& e) { return int(e.what().size()); }
        }
    )");

    // The limits.
    attempt(ctx, "runaway loop", "int main() { while (true) { } return 0; }");
    attempt(ctx, "deep recursion", "int f(int n) { return f(n + 1); }\nint main() { return f(0); }");
    attempt(ctx, "memory hog", R"(
        #include <string>
        #include <vector>
        int main() {
            std::vector<std::string> v;
            for (int i = 0; i < 10000000; i++) v.push_back("padding padding padding " + std::to_string(i));
            return v.size();
        }
    )");

    const script::Context::Stats stats = ctx.stats();
    std::printf("\nafterwards: %llu objects, %llu bytes on the heap, %llu collections\n",
                (unsigned long long)stats.liveObjects, (unsigned long long)stats.heapBytes,
                (unsigned long long)stats.gcRuns);
    return 0;
}
