// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// A plugin host: Python scripts register handlers, the application dispatches
// events to them and collects the results.
//
//   ./python_plugin

#include <cstdio>
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

/// Owns the script context and the handler table the scripts register into.
class PluginHost
{
public:
    PluginHost()
    {
        ctx_.global().set("on", ctx_.function("on", [this](script::CallInfo& ci) {
            handlers_[ci.arg(0).toString()].push_back(ci.arg(1));
            return script::Value();
        }));
        ctx_.global().set("log", ctx_.fn("log", [](const std::string& line) {
            std::printf("  [plugin] %s\n", line.c_str());
        }));
    }

    void load(const std::string& name, const std::string& source) { ctx_.eval(source, name); }

    /// Every handler for `event` sees `payload`; failures are isolated so one
    /// broken plugin cannot take the host down.
    std::vector<script::Value> dispatch(const std::string& event, const script::Value& payload)
    {
        std::vector<script::Value> results;
        for (const script::Value& handler : handlers_[event]) {
            try {
                results.push_back(handler.call({payload}));
            } catch (const script::Exception& e) {
                std::printf("  [host] a handler failed: %s\n", e.what());
            }
        }
        return results;
    }

    script::Context& context() { return ctx_; }

private:
    script::Context ctx_{languageOptions()};
    std::map<std::string, std::vector<script::Value>> handlers_;
};

}  // namespace

int main()
{
    PluginHost host;

    host.load("pricing.py", R"(
def price(order):
    total = sum(item['price'] * item['qty'] for item in order['items'])
    log(f"priced {len(order['items'])} items at {total:.2f}")
    return {'plugin': 'pricing', 'total': round(total, 2)}

on('order', price)
)");

    host.load("audit.py", R"(
class Audit:
    def __init__(self):
        self.seen = set()
    def note(self, order_id):
        self.seen.add(order_id)
        return len(self.seen)

audit = Audit()
on('order', lambda order: {'plugin': 'audit', 'unique': audit.note(order['id'])})
)");

    host.load("broken.py", "def bad(order):\n    raise RuntimeError('bad plugin')\non('order', bad)\n");

    // The payload is built in the script's own terms: a dict.
    script::Context& ctx = host.context();
    const script::Value order = ctx.eval(
        "{'id': 'A-1', 'items': [{'price': 9.99, 'qty': 2}, {'price': 4.5, 'qty': 1}]}", "payload.py");

    for (int round = 1; round <= 2; ++round) {
        std::printf("dispatching 'order' (round %d)\n", round);
        for (const script::Value& result : host.dispatch("order", order))
            std::printf("  -> %s\n", ctx.eval("lambda r: str(sorted(r.items()))").call({result}).toString().c_str());
    }
    return 0;
}
