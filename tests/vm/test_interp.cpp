// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The interpreter against the dynamic engine: every program in snippets.inc runs on both cores and must give the same
// value, or the same kind of error.
#include <string>
#include <vector>

#include "script/script.hpp"
#include "tests/test_util.hpp"

namespace {

struct Outcome
{
    bool threw = false;
    std::string text;
    std::string type;
    script::ErrorCode code = script::ErrorCode::Ok;
};

Outcome runOn(script::Context::Options::Core core, const char* source)
{
    script::Context::Options options;
    options.language = script::Language::JavaScript;
    options.core = core;
    script::Context ctx(options);
    Outcome out;
    try {
        script::Value v = ctx.eval(source);
        out.text = v.toString();
        out.type = script::toString(v.type());
        // An int and a double that print alike are the same number to a program.
        if (out.type == "int" || out.type == "double" || out.type == "long" || out.type == "float")
            out.type = "number";
    } catch (const script::Error& e) {
        out.threw = true;
        out.code = e.code();
        out.text = e.what();
    }
    return out;
}

const char* const kSnippets[] = {
#define S(text) text,
#include "snippets.inc"
#undef S
};

}  // namespace

TEST(everySnippetAgreesWithTheDynamicEngine)
{
    int checked = 0, failed = 0;
    for (const char* source : kSnippets) {
        const Outcome want = runOn(script::Context::Options::Core::Dyn, source);
        const Outcome got = runOn(script::Context::Options::Core::Vm, source);
        ++checked;
        const bool same = want.threw == got.threw && want.text == got.text && want.type == got.type && want.code == got.code;
        if (!same) {
            ++failed;
            std::fprintf(stderr, "MISMATCH: %s\n   dyn: %s%s [%s]\n    vm: %s%s [%s]\n", source, want.threw ? "THROW " : "",
                         want.text.c_str(), want.type.c_str(), got.threw ? "THROW " : "", got.text.c_str(), got.type.c_str());
        }
    }
    std::fprintf(stderr, "%d snippets, %d differ\n", checked, failed);
    CHECK_EQ(failed, 0);
}

// What the language says where the dynamic engine does not: a read-only property on a prototype stops a store from
// creating an own property of the same name.
TEST(readOnlyOnThePrototypeBlocksTheStoreEvenWhenTheSiteLearnedToAdd)
{
    const Outcome got = runOn(script::Context::Options::Core::Vm,
                              "var P = {}; function mk() { var o = Object.create(P); o.x = 1; return o; } mk(); mk(); "
                              "Object.defineProperty(P, 'x', {value: 7, writable: false}); var o = mk(); String(o.x)");
    CHECK(!got.threw);
    CHECK_EQ(got.text, std::string("7"));
}
