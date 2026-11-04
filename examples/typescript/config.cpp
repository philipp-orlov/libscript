// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// TypeScript as a configuration language: a config file that can compute, and
// a shape the host declares so a typo in it is a compile error, not a surprise
// at read time.
//
//   ./config

#include <cstdio>
#include <map>
#include <string>
#include <vector>

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

const char* kConfigSource = R"(
    declare const host: { env(name: string): string };

    type Environment = 'dev' | 'staging' | 'prod';

    interface Settings {
        service: string;
        environment: Environment;
        replicas: number;
        listen: string;
        features: string[];
        limits: { memoryMb: number; timeoutMs: number };
        tags: { [key: string]: string };
    }

    const env = (host.env('DEPLOY_ENV') || 'dev') as Environment;
    const replicas: Record<Environment, number> = { dev: 1, staging: 2, prod: 8 };

    var settings: Settings = {
        service: 'billing',
        environment: env,
        replicas: replicas[env] ?? 1,
        listen: `0.0.0.0:${8080 + (env === 'prod' ? 0 : 1000)}`,
        features: ['metrics', 'tracing', ...(env === 'prod' ? ['audit'] : [])],
        limits: { memoryMb: replicas[env] * 256, timeoutMs: 30_000 },
        tags: Object.fromEntries(['team', 'tier'].map((k, i) => [k, ['payments', String(i)][i]])),
    };
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

    ctx.eval(kConfigSource, "app.config.ts");
    const script::Value exported = ctx.global().get("settings");

    Config config;
    config.service = exported.get("service").toString();
    config.environment = exported.get("environment").toString();
    config.replicas = exported.get("replicas").as<int>();
    config.listen = exported.get("listen").toString();
    config.features = exported.get("features").as<std::vector<std::string>>();
    config.memoryMb = exported.get("limits").get("memoryMb").as<int>();
    config.tags = exported.get("tags").as<std::map<std::string, std::string>>();

    std::printf("service    %s (%s)\n", config.service.c_str(), config.environment.c_str());
    std::printf("listen     %s\n", config.listen.c_str());
    std::printf("replicas   %d, memory %d MB\n", config.replicas, config.memoryMb);
    std::printf("features   ");
    for (const std::string& f : config.features)
        std::printf("%s ", f.c_str());
    std::printf("\ntags       ");
    for (const auto& [key, value] : config.tags)
        std::printf("%s=%s ", key.c_str(), value.c_str());
    std::printf("\n\nas JSON:\n%s\n", exported.toJson(2).c_str());
    return 0;
}
