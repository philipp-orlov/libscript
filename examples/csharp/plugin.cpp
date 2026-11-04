// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// A plugin system: the host defines the contract, C# implements it.
#include <cstdio>
#include <string>
#include <vector>

#include "script/script.hpp"

namespace {

/// What the host wants from a plugin, in C#. Everything the plugin may touch
/// is either declared here or exposed as a host class -- there is no other way
/// in or out.
const char* kContract = R"(
    public interface IPlugin {
        string Name { get; }
        int Handle(string request);
    }
)";

/// Two plugins, as an embedder would load them from disk.
const char* kPlugins[] = {
    R"(
        public class Upper : IPlugin {
            public string Name { get { return "upper"; } }
            public int Handle(string request) {
                Host.Emit(request.ToUpper());
                return request.Length;
            }
        }
    )",
    R"(
        public class Counter : IPlugin {
            private int seen = 0;
            public string Name => "counter";
            public int Handle(string request) {
                seen++;
                Host.Emit($"{Name}: {seen} request(s), last was {request.Length} chars");
                return seen;
            }
        }
    )",
};

}  // namespace

int main()
{
    script::Context ctx;

    std::vector<std::string> emitted;
    ctx.hostClass("Host").method("Emit", [&emitted](std::string line) {
        emitted.push_back(line);
        std::printf("  %s\n", line.c_str());
    });

    // The contract is compiled once; the plugins are compiled against it.
    ctx.compile(kContract, "contract.cs");

    struct Loaded
    {
        script::Assembly assembly;
        script::Value instance;
    };
    std::vector<Loaded> plugins;

    for (const char* source : kPlugins) {
        try {
            script::Assembly assembly = ctx.compile(source, "plugin.cs");
            // Every type in the image is a candidate; construct the ones that
            // implement the contract.
            for (const std::string& method : assembly.methods()) {
                // `Type..ctor` is how a constructor is spelled.
                const std::string suffix = "..ctor";
                if (method.size() <= suffix.size() ||
                    method.compare(method.size() - suffix.size(), suffix.size(), suffix) != 0)
                    continue;
                const std::string typeName = method.substr(0, method.size() - suffix.size());
                script::Value instance = ctx.newObject(assembly, typeName);
                std::printf("loaded plugin '%s'\n", instance.get("Name").toString().c_str());
                plugins.push_back({std::move(assembly), std::move(instance)});
                break;
            }
        } catch (const script::CompileError& error) {
            std::printf("plugin rejected: %s\n", error.diagnostics().front().toString().c_str());
        }
    }

    const char* requests[] = {"first request", "second"};
    for (const char* request : requests) {
        for (Loaded& plugin : plugins) {
            const script::Value result = plugin.instance.invoke("Handle", {ctx.str(request)});
            std::printf("  -> %s returned %d\n", plugin.instance.get("Name").toString().c_str(),
                        result.toInt());
        }
    }

    // A plugin that does not honour the contract never gets loaded.
    try {
        ctx.compile("public class Broken : IPlugin { public int Handle(int n) { return n; } }",
                    "broken.cs");
    } catch (const script::CompileError& error) {
        std::printf("broken plugin refused: %s\n", error.diagnostics().front().message.c_str());
    }

    std::printf("%zu lines emitted\n", emitted.size());
    return 0;
}
