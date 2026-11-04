// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Running C you do not trust: what a C program cannot reach here, and the
// limits that stop the one that tries anyway.
//
//   ./c_sandbox

#include <chrono>
#include <cstdio>

#include "script/script.hpp"

namespace {

script::Context::Options languageOptions()
{
    script::Context::Options options;
    options.language = script::Language::C;
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
    attempt(ctx, "printing", "#include <stdio.h>\nint main() { puts(\"hello from the sandbox\"); fprintf(stderr, \"and an error\\n\"); return 0; }");

    // The C library here has no files, no processes, no input and no raw
    // memory. Each is refused by name before anything runs.
    attempt(ctx, "no files", "#include <stdio.h>\nint main() { FILE* f = fopen(\"/etc/passwd\", \"r\"); return f != 0; }");
    attempt(ctx, "no processes", "#include <stdlib.h>\nint main() { return system(\"rm -rf /\"); }");
    attempt(ctx, "no input", "#include <stdio.h>\nint main() { int n; scanf(\"%d\", &n); return n; }");
    attempt(ctx, "no exit", "#include <stdlib.h>\nint main() { exit(3); }");
    attempt(ctx, "no addresses", "int main() { int a[4]; int* p = a; p += 2; return *p; }");
    attempt(ctx, "no void*", "#include <stdlib.h>\nint main() { void* p = malloc(16); return p != 0; }");

    // Misbehaviour is an exception with a line number, never undefined behaviour.
    attempt(ctx, "bounds", "int main() { int a[2]; return a[5]; }");
    attempt(ctx, "null pointer", "struct P { int x; };\nint main() { struct P* p = 0; return p->x; }");
    attempt(ctx, "divide by zero", "int main() { int z = 0; return 1 / z; }");
    attempt(ctx, "assert", "#include <assert.h>\nint main() { assert(1 == 2); return 0; }");

    // The limits.
    attempt(ctx, "runaway loop", "int main() { for (;;) { } }");
    attempt(ctx, "deep recursion", "int f(int n) { return f(n + 1); }\nint main() { return f(0); }");
    attempt(ctx, "memory hog", R"(
        #include <stdlib.h>
        struct Node { int pad[64]; struct Node* next; };
        int main() {
            struct Node* head = NULL;
            for (int i = 0; i < 10000000; i++) {
                struct Node* n = malloc(sizeof(struct Node));
                n->next = head;
                head = n;
            }
            return 0;
        }
    )");

    const script::Context::Stats stats = ctx.stats();
    std::printf("\nafterwards: %llu objects, %llu bytes on the heap, %llu collections\n",
                (unsigned long long)stats.liveObjects, (unsigned long long)stats.heapBytes,
                (unsigned long long)stats.gcRuns);
    return 0;
}
