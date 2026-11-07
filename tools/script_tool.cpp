// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// libscript -- run, check, compile and inspect a program in any language the
// build contains. The language comes from the file's extension or `--lang`.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <iostream>
#include <string>
#include <vector>

#include "base/status.hpp"
#include "script/script.hpp"
#include "sys/file.hpp"
#if SCRIPT_ENGINE_TYPED
#include "script/internal/typed/bytecode.hpp"
#include "script/internal/typed/metadata.hpp"
#endif

namespace {

void usage()
{
    std::printf("libscript %s -- one engine, several languages (%s)\n\n", script::version(),
                [] {
                    std::string names;
                    for (script::Language l : script::supportedLanguages())
                        names += (names.empty() ? "" : ", ") + std::string(script::toString(l));
                    return names;
                }()
                    .c_str());
    std::puts("usage: libscript [options] <file> [args...]");
    std::puts("       libscript [options] -e <source>");
    std::puts("       libscript --lang <l>            a REPL for a dynamic language (Ctrl-D to leave)");
    std::puts("");
    std::puts("  --lang <cs|ts|js|py|c|cpp>  the language, when the file name does not say");
    std::puts("  -e <source>        run the source given on the command line");
    std::puts("  --check            compile and report diagnostics, run nothing");
    std::puts("  -c <out.sxa>       compile a C#, C or C++ program to an assembly image");
    std::puts("  -r <in.sxa>        run a previously written image");
    std::puts("  --dump             disassemble every method of a C# assembly");
    std::puts("  --stats            print heap statistics when the program ends");
    std::puts("  --strict           TypeScript: strictNullChecks and noImplicitAny");
    std::puts("  --no-check         TypeScript: skip type checking");
    std::puts("  --no-eval          JavaScript: eval and new Function are errors, as in an embedding by default");
    std::puts("  --interpret        C#: no native code at all");
    std::puts("  --jit              C#: compile a method to native code once it gets hot (the default)");
    std::puts("  --native           C#: compile every method it can up front");
    std::puts("  --dump-native      with --jit or --native: print the machine code of what gets compiled");
    std::puts("  --stack <n>        maximum call depth (default 512)");
    std::puts("  --memory <mb>      heap ceiling in megabytes");
    std::puts("  --version          print the library version");
}

std::string readFile(const std::string& path)
{
    try {
        return sys::readWholeFile(path);
    } catch (const base::Error&) {
        std::fprintf(stderr, "libscript: cannot open '%s'\n", path.c_str());
        std::exit(2);
    }
}

void report(const script::CompileError& error)
{
    for (const script::Diagnostic& diagnostic : error.diagnostics())
        std::fprintf(stderr, "%s\n", diagnostic.toString().c_str());
}

void report(const script::Exception& error)
{
    std::fprintf(stderr, "Unhandled exception: %s\n", error.what());
    if (!error.stack().empty())
        std::fputs(error.stack().c_str(), stderr);
}

void printStats(script::Context& ctx)
{
    const script::Context::Stats s = ctx.stats();
    std::fprintf(stderr,
                 "heap %llu bytes, %llu objects, %llu strings, %llu gc runs, %llu pooled, "
                 "backend %s, %llu methods native (%llu bytes), %llu jobs pending\n",
                 (unsigned long long)s.heapBytes, (unsigned long long)s.liveObjects,
                 (unsigned long long)s.liveStrings, (unsigned long long)s.gcRuns,
                 (unsigned long long)s.reservedBytes, ctx.jitBackend(),
                 (unsigned long long)s.methodsCompiled, (unsigned long long)s.nativeCodeBytes,
                 (unsigned long long)s.pendingJobs);
}

void printResult(const script::Value& result)
{
    if (!result.isNullish())
        std::printf("%s\n", result.inspect().c_str());
}

int repl(script::Context& ctx, script::Language language)
{
    std::printf("libscript %s (%s) -- Ctrl-D to exit\n", script::version(),
                script::toString(language));
    std::string line;
    while (std::fputs("> ", stdout), std::fflush(stdout), std::getline(std::cin, line)) {
        if (line.empty())
            continue;
        try {
            printResult(ctx.eval(line, "<repl>", language));
            ctx.runJobs();
        } catch (const script::CompileError& e) {
            report(e);
        } catch (const script::Exception& e) {
            report(e);
        } catch (const script::Error& e) {
            std::fprintf(stderr, "%s\n", e.what());
        }
    }
    std::printf("\n");
    return 0;
}

}  // namespace

int main(int argc, char** argv)
{
    script::Context::Options options;
    // A command line runs the source it is given, and whatever that source makes of strings.
    options.runtimeCode = true;
    script::Language language = script::Language::Auto;
    std::string source;
    std::string sourceName;
    std::string compileTo;
    std::string imagePath;
    std::vector<std::string> args;
    bool checkOnly = false;
    bool dump = false;
    bool stats = false;
    bool haveSource = false;

    // Options may come in any order; everything after the program's file name
    // belongs to the program.
    bool fileGiven = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (fileGiven) {
            args.push_back(arg);
            continue;
        }
        if (arg == "-h" || arg == "--help") {
            usage();
            return 0;
        }
        if (arg == "--version") {
            std::printf("libscript %s\n", script::version());
            return 0;
        }
        if (arg == "--lang" && i + 1 < argc) {
            language = script::languageNamed(argv[++i]);
            if (language == script::Language::Auto) {
                std::fprintf(stderr, "libscript: unknown language '%s'\n", argv[i]);
                return 2;
            }
            continue;
        }
        if (arg == "-e" && i + 1 < argc) {
            source = argv[++i];
            sourceName = "<command line>";
            haveSource = true;
            continue;
        }
        if (arg == "-c" && i + 1 < argc) {
            compileTo = argv[++i];
            continue;
        }
        if (arg == "-r" && i + 1 < argc) {
            imagePath = argv[++i];
            continue;
        }
        if (arg == "--check") {
            checkOnly = true;
            continue;
        }
        if (arg == "--dump") {
            dump = true;
            continue;
        }
        if (arg == "--stats") {
            stats = true;
            continue;
        }
        if (arg == "--strict") {
            options.check = script::CheckOptions::strict();
            continue;
        }
        if (arg == "--no-check") {
            options.check.enabled = false;
            continue;
        }
        if (arg == "--no-eval") {
            options.runtimeCode = false;
            continue;
        }
        if (arg == "--interpret") {
            options.jit.mode = script::JitOptions::Mode::Off;
            continue;
        }
        if (arg == "--jit") {
            options.jit.mode = script::JitOptions::Mode::Tiered;
            continue;
        }
        if (arg == "--dump-native") {
            options.jit.dumpCode = true;
            continue;
        }
        if (arg == "--native") {
            options.compile.native = true;
            options.jit.mode = script::JitOptions::Mode::Eager;
            continue;
        }
        if (arg == "--stack" && i + 1 < argc) {
            options.stackLimit = std::size_t(std::strtoul(argv[++i], nullptr, 10));
            continue;
        }
        if (arg == "--memory" && i + 1 < argc) {
            options.memoryLimit = std::size_t(std::strtoul(argv[++i], nullptr, 10)) << 20;
            continue;
        }
        if (!arg.empty() && arg[0] == '-') {
            std::fprintf(stderr, "libscript: unknown option '%s'\n", arg.c_str());
            return 2;
        }
        sourceName = arg;
        source = readFile(arg);
        haveSource = true;
        fileGiven = true;
    }

    if (language == script::Language::Auto && haveSource)
        language = script::languageOfFile(sourceName);
    if (language != script::Language::Auto && !script::supported(language)) {
        std::fprintf(stderr, "libscript: this build has no %s support\n",
                     script::toString(language));
        return 2;
    }
    options.language = language;
    script::Context ctx(options);
    ctx.setReadLineHandler([](std::string& line) { return bool(std::getline(std::cin, line)); });

    try {
        if (!haveSource && imagePath.empty()) {
            if (language == script::Language::Auto || language == script::Language::CSharp ||
                language == script::Language::C || language == script::Language::Cpp) {
                usage();
                return 2;
            }
            return repl(ctx, language);
        }

        std::vector<script::Value> argv2;
        for (const std::string& a : args)
            argv2.push_back(ctx.str(a));
        if (script::supported(script::Language::JavaScript) ||
            script::supported(script::Language::TypeScript) ||
            script::supported(script::Language::Python)) {
            try {
                ctx.global().set("argv", ctx.array(argv2));
                // What a JavaScript shell has: `read(path)`, a file's text, which programs made for shells look for.
                ctx.global().set("read", ctx.function("read", [](script::CallInfo& ci) -> script::Value {
                    const std::string path = ci.arg(0).toString();
                    std::ifstream in(path, std::ios::binary);
                    if (!in)
                        ci.fail(script::ErrorCode::TypeError, "cannot read " + path);
                    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                    return ci.context().str(text);
                }, 1));
            } catch (const script::Error&) {
            }
        }

        script::Assembly assembly;
        if (!imagePath.empty()) {
            const std::string bytes = readFile(imagePath);
            assembly = ctx.load(bytes.data(), bytes.size());
        } else {
            assembly = ctx.compile(source, sourceName, language);
        }

        if (dump) {
#if SCRIPT_ENGINE_TYPED
            if (auto* image = dynamic_cast<script::internal::typed::AssemblyImpl*>(assembly.impl())) {
                for (const auto& method : image->methodTable) {
                    if (!method->body.code.empty())
                        std::fputs(script::internal::typed::disassemble(*method).c_str(), stdout);
                }
            } else
#endif
            {
                for (const std::string& name : assembly.methods())
                    std::printf("%s\n", name.c_str());
            }
        }
        if (!compileTo.empty()) {
            const std::vector<std::uint8_t> image = assembly.serialize();
            std::ofstream out(compileTo, std::ios::binary);
            out.write(reinterpret_cast<const char*>(image.data()), std::streamsize(image.size()));
            std::printf("wrote %s (%zu bytes, %zu methods)\n", compileTo.c_str(), image.size(),
                        assembly.methods().size());
            return 0;
        }
        if (checkOnly) {
            std::puts("ok");
            return 0;
        }

        printResult(ctx.run(assembly, argv2));
        ctx.runJobs();
        if (stats)
            printStats(ctx);
        return 0;
    } catch (const script::CompileError& error) {
        report(error);
        return 1;
    } catch (const script::Exception& error) {
        report(error);
        return 1;
    } catch (const script::Error& error) {
        std::fprintf(stderr, "libscript: %s\n", error.what());
        return 1;
    }
}
