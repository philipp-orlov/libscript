// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The five-minute tour, in Python: run a script, read a result, expose a
// function and a class, call back into script.
//
//   ./python_quickstart

#include <cstdio>
#include <string>

#include "script/script.hpp"

namespace {

struct Thermostat
{
    double target = 20;
    void set(double t) { target = t; }
    double get() const { return target; }
};

script::Context::Options languageOptions()
{
    script::Context::Options options;
    options.language = script::Language::Python;
    return options;
}

}  // namespace

int main()
{
    script::Context ctx(languageOptions());

    // 1. Run some Python and take the value of the last expression.
    std::printf("6 * 7 = %d\n", ctx.eval("6 * 7").toInt());

    // 2. `print` goes wherever the host says.
    ctx.setPrintHandler([](std::string_view text, bool) { std::fwrite(text.data(), 1, text.size(), stdout); });
    ctx.eval("for n in range(1, 4):\n"
             "    print(f'{n} squared is {n * n}')\n");

    // 3. Expose a C++ function and a C++ class; Python sees both.
    ctx.global().set("slugify", ctx.fn("slugify", [](std::string text) {
        for (char& c : text)
            c = c == ' ' ? '-' : char(std::tolower(static_cast<unsigned char>(c)));
        return text;
    }));
    script::Class<Thermostat>(ctx, "Thermostat")
        .constructor<>()
        .method("set", &Thermostat::set)
        .method("get", &Thermostat::get)
        .install();
    ctx.eval("thermo = Thermostat()\n"
             "thermo.set(21.5)\n"
             "print(slugify('Hello Embedded World'), thermo.get())\n");

    // 4. Definitions persist between evaluations, so call a Python function from C++.
    ctx.eval("def describe(readings):\n"
             "    lo, hi = min(readings), max(readings)\n"
             "    return f'{len(readings)} readings between {lo} and {hi}'\n");
    script::Value describe = ctx.global().get("describe");
    std::printf("%s\n", describe.call({ctx.value(std::vector<double>{19.5, 21, 22.5})}).toString().c_str());

    // 5. Exceptions cross the boundary with their message and stack.
    try {
        ctx.eval("def check(v):\n"
                 "    if v < 0:\n"
                 "        raise ValueError('negative reading')\n"
                 "check(-1)\n", "readings.py");
    } catch (const script::Exception& e) {
        std::printf("caught %s\n%s", e.what(), e.stack().c_str());
    }
    return 0;
}
