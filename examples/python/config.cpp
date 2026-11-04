// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Python as a configuration language: a config file that can compute, with
// the host's environment as its only input, read back into C++ structs.
//
//   ./python_config

#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#include "script/script.hpp"

namespace {

script::Context::Options languageOptions()
{
    script::Context::Options options;
    options.language = script::Language::Python;
    return options;
}

const char* kConfigSource = R"(
env = host.env('DEPLOY_ENV') or 'dev'
replicas = {'dev': 1, 'staging': 2, 'prod': 8}

class Settings:
    def __init__(self):
        self.service = 'billing'
        self.environment = env
        self.replicas = replicas.get(env, 1)
        self.listen = f'0.0.0.0:{8080 + (0 if env == "prod" else 1000)}'
        self.features = ['metrics', 'tracing'] + (['audit'] if env == 'prod' else [])
        self.memory_mb = self.replicas * 256
        self.timeout_ms = 30_000
        self.tags = {'team': 'payments', 'tier': str(self.replicas)}

settings = Settings()
)";

struct Config
{
    std::string service;
    std::string environment;
    int replicas = 0;
    std::string listen;
    std::vector<std::string> features;
    int memoryMb = 0;
    std::map<std::string, std::string> tags;
};

}  // namespace

int main()
{
    script::Context ctx(languageOptions());

    // The only host capability the config file gets.
    script::Value host = ctx.object();
    host.set("env", ctx.fn("env", [](const std::string& name) -> std::string {
        const char* value = std::getenv(name.c_str());
        return value ? value : "";
    }));
    ctx.global().set("host", host);

    ctx.eval(kConfigSource, "app.config.py");
    const script::Value exported = ctx.global().get("settings");

    // A Python object's attributes are properties; lists and dicts convert.
    Config config;
    config.service = exported.get("service").toString();
    config.environment = exported.get("environment").toString();
    config.replicas = exported.get("replicas").toInt();
    config.listen = exported.get("listen").toString();
    config.features = ctx.as<std::vector<std::string>>(exported.get("features"));
    config.memoryMb = exported.get("memory_mb").toInt();
    // A dict is a Map on the engine side; ask Python to flatten it for the host.
    for (const std::string& pair :
         ctx.as<std::vector<std::string>>(ctx.eval("[f'{k}={v}' for k, v in settings.tags.items()]")))
        config.tags[pair.substr(0, pair.find('='))] = pair.substr(pair.find('=') + 1);

    std::printf("service    %s (%s)\n", config.service.c_str(), config.environment.c_str());
    std::printf("listen     %s\n", config.listen.c_str());
    std::printf("replicas   %d, memory %d MB\n", config.replicas, config.memoryMb);
    std::printf("features   ");
    for (const std::string& f : config.features)
        std::printf("%s ", f.c_str());
    std::printf("\ntags       ");
    for (const auto& kv : config.tags)
        std::printf("%s=%s ", kv.first.c_str(), kv.second.c_str());
    std::printf("\n");

    // A mistake in the config is an error at load time, with the line.
    try {
        ctx.eval("settings.replicas = settings.replicas +\n", "typo.config.py");
    } catch (const script::Error& e) {
        std::printf("rejected: %s\n", e.what());
    }
    return 0;
}
