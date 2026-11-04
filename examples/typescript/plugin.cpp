// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// A plugin host: scripts register handlers, the application dispatches events
// to them and collects the results.
//
//   ./plugin

#include <cstdio>
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

/// Owns the script context and the handler table the scripts register into.
class PluginHost
{
public:
    PluginHost() : handlers_(ctx_.object())
    {
        ctx_.global().set("handlers", handlers_);
        ctx_.global().set("on", ctx_.function("on", [this](script::CallInfo& ci) {
            const std::string event = ci.arg(0).toString();
            script::Value list = handlers_.get(event);
            if (!list.isArray()) {
                list = ctx_.array();
                handlers_.set(event, list);
            }
            list.push(ci.arg(1));
            return script::Value();
        }));
        ctx_.global().set("log", ctx_.fn("log", [](const std::string& line) {
            std::printf("  [plugin] %s\n", line.c_str());
        }));
        // The plugin API, as the checker sees it. A plugin that misuses it is
        // rejected at load time rather than at dispatch time.
        ctx_.declare(
            "declare function on(event: string, handler: (payload: any) => any): void;\n"
            "declare function log(line: string): void;");
    }

    void load(const std::string& name, const std::string& source) { ctx_.eval(source, name); }

    /// Every handler for `event` sees `payload`; failures are isolated so one
    /// broken plugin cannot take the host down.
    std::vector<script::Value> dispatch(const std::string& event, const script::Value& payload)
    {
        std::vector<script::Value> results;
        script::Value list = handlers_.get(event);
        if (!list.isArray())
            return results;
        for (std::uint32_t i = 0; i < list.length(); ++i) {
            try {
                results.push_back(list.get(i).call({payload}));
            } catch (const script::Exception& e) {
                std::printf("  [host] handler %u failed: %s\n", i, e.what());
            }
        }
        ctx_.runJobs();
        return results;
    }

    script::Context& context() { return ctx_; }

private:
    script::Context ctx_{languageOptions()};
    script::Value handlers_;
};

}  // namespace

int main()
{
    PluginHost host;

    host.load("pricing.ts", R"(
        interface Item { price: number; qty: number }
        interface Order { id: string; items: Item[] }

        on('order', (order: Order) => {
            const total = order.items.reduce((sum, i) => sum + i.price * i.qty, 0);
            log(`priced ${order.items.length} items at ${total.toFixed(2)}`);
            return { plugin: 'pricing', total };
        });
    )");

    host.load("audit.ts", R"(
        class Audit {
            private seen = new Set<string>();
            note(id: string): number {
                this.seen.add(id);
                return this.seen.size;
            }
        }
        const audit = new Audit();
        on('order', (order: { id: string }) => ({ plugin: 'audit', unique: audit.note(order.id) }));
    )");

    host.load("broken.ts", "on('order', () => { throw new Error('bad plugin'); });");

    script::Context& ctx = host.context();
    script::Value order = ctx.parseJson(R"({
        "id": "A-1",
        "items": [ { "price": 9.99, "qty": 2 }, { "price": 4.5, "qty": 1 } ]
    })");

    std::printf("dispatching 'order'\n");
    for (const script::Value& result : host.dispatch("order", order))
        std::printf("  -> %s\n", result.toJson().c_str());

    std::printf("dispatching 'order' again\n");
    for (const script::Value& result : host.dispatch("order", order))
        std::printf("  -> %s\n", result.toJson().c_str());
    return 0;
}
