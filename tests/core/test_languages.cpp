// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The language table: what the build contains, how a source picks its front
// end, and what a missing language reports.
#include <algorithm>

#include "script/script.hpp"
#include "tests/test_util.hpp"

using script::Language;

TEST(configHeaderMatchesTheTable)
{
    CHECK_EQ(script::supported(Language::CSharp), bool(SCRIPT_LANG_CSHARP));
    CHECK_EQ(script::supported(Language::TypeScript), bool(SCRIPT_LANG_TYPESCRIPT));
    CHECK_EQ(script::supported(Language::JavaScript), bool(SCRIPT_LANG_JAVASCRIPT));
    CHECK_EQ(script::supported(Language::Python), bool(SCRIPT_LANG_PYTHON));
    CHECK_EQ(script::supported(Language::C), bool(SCRIPT_LANG_C));
    CHECK_EQ(script::supported(Language::Cpp), bool(SCRIPT_LANG_CPP));
    CHECK(!script::supported(Language::Auto));
    const auto& all = script::supportedLanguages();
    CHECK(!all.empty());
    CHECK_EQ(all.size(), std::size_t(SCRIPT_LANG_CSHARP + SCRIPT_LANG_TYPESCRIPT +
                                     SCRIPT_LANG_JAVASCRIPT + SCRIPT_LANG_PYTHON +
                                     SCRIPT_LANG_C + SCRIPT_LANG_CPP));
    // Preference order is fixed: C#, TypeScript, JavaScript, Python, C, C++.
    CHECK(std::is_sorted(all.begin(), all.end()));
}

TEST(namesAndExtensions)
{
    CHECK_EQ(std::string(script::toString(Language::CSharp)), std::string("csharp"));
    CHECK_EQ(std::string(script::toString(Language::TypeScript)), std::string("typescript"));
    CHECK_EQ(std::string(script::toString(Language::JavaScript)), std::string("javascript"));
    CHECK_EQ(std::string(script::toString(Language::Python)), std::string("python"));
    CHECK_EQ(std::string(script::toString(Language::C)), std::string("c"));
    CHECK_EQ(std::string(script::toString(Language::Cpp)), std::string("cpp"));
    CHECK(script::languageNamed("cs") == Language::CSharp);
    CHECK(script::languageNamed("C#") == Language::CSharp);
    CHECK(script::languageNamed("TypeScript") == Language::TypeScript);
    CHECK(script::languageNamed(".js") == Language::JavaScript);
    CHECK(script::languageNamed("python") == Language::Python);
    CHECK(script::languageNamed("py") == Language::Python);
    CHECK(script::languageNamed("c") == Language::C);
    CHECK(script::languageNamed("cpp") == Language::Cpp);
    CHECK(script::languageNamed("C++") == Language::Cpp);
    CHECK(script::languageNamed("rust") == Language::Auto);
    CHECK(script::languageOfFile("dir/prog.cs") == Language::CSharp);
    CHECK(script::languageOfFile("a.b.TS") == Language::TypeScript);
    CHECK(script::languageOfFile("mod.mjs") == Language::JavaScript);
    CHECK(script::languageOfFile("<eval>") == Language::Auto);
    CHECK(script::languageOfFile("noext") == Language::Auto);
    CHECK(script::languageOfFile("x.py") == Language::Python);
    CHECK(script::languageOfFile("main.c") == Language::C);
    CHECK(script::languageOfFile("main.cpp") == Language::Cpp);
    CHECK(script::languageOfFile("lib.hpp") == Language::Cpp);
    CHECK(script::languageOfFile("a.CC") == Language::Cpp);
    CHECK(script::languageOfFile("x.rs") == Language::Auto);
}

TEST(missingLanguageIsReportedNotGuessed)
{
    script::Context ctx;
    for (Language language : {Language::CSharp, Language::TypeScript, Language::JavaScript,
                              Language::Python, Language::C, Language::Cpp}) {
        if (script::supported(language))
            continue;
        CHECK_THROWS(ctx.eval("1", "<eval>", language), script::ErrorCode::NotSupported);
    }
}

TEST(everySupportedLanguageEvaluatesInOneContext)
{
    script::Context ctx;
    for (Language language : script::supportedLanguages()) {
        script::Value v;
        switch (language) {
            case Language::CSharp: v = ctx.eval("return 6 * 7;", "<eval>", language); break;
            case Language::TypeScript: v = ctx.eval("const n: number = 6; n * 7", "<eval>", language); break;
            case Language::JavaScript: v = ctx.eval("6 * 7", "<eval>", language); break;
            case Language::Python: v = ctx.eval("6 * 7", "<eval>", language); break;
            case Language::C: v = ctx.eval("int main() { return 6 * 7; }", "<eval>", language); break;
            case Language::Cpp: v = ctx.eval("int main() { auto n = 6; return n * 7; }", "<eval>", language); break;
            default: break;
        }
        CHECK_EQ(v.toInt(), 42);
    }
}

TEST(extensionPicksTheFrontEnd)
{
    script::Context ctx;
    if (script::supported(Language::CSharp))
        CHECK_EQ(ctx.eval("return 1 + 1;", "sum.cs").toInt(), 2);
    if (script::supported(Language::JavaScript))
        CHECK_EQ(ctx.eval("1 + 1", "sum.js").toInt(), 2);
    if (script::supported(Language::TypeScript))
        CHECK_EQ(ctx.eval("1 + 1", "sum.ts").toInt(), 2);
    if (script::supported(Language::Python))
        CHECK_EQ(ctx.eval("1 + 1", "sum.py").toInt(), 2);
    if (script::supported(Language::C))
        CHECK_EQ(ctx.eval("int main() { return 1 + 1; }", "sum.c").toInt(), 2);
    if (script::supported(Language::Cpp))
        CHECK_EQ(ctx.eval("int main() { bool b = true; return b + 1; }", "sum.cpp").toInt(), 2);
}

TEST(defaultLanguageOptionAppliesWithoutExtension)
{
    for (Language language : script::supportedLanguages()) {
        script::Context::Options options;
        options.language = language;
        script::Context ctx(options);
        const char* source = language == Language::CSharp ? "return 2;"
                             : language == Language::C || language == Language::Cpp
                                 ? "int main() { return 2; }"
                                 : "2";
        CHECK_EQ(ctx.eval(source).toInt(), 2);
        script::Assembly assembly = ctx.compile(source);
        CHECK(assembly.language() == language);
        CHECK_EQ(ctx.run(assembly).toInt(), 2);
    }
}

TEST(diagnosticsCarryTheirLanguage)
{
    script::Diagnostic d;
    d.file = "a.cs";
    d.line = 2;
    d.column = 5;
    d.code = 1002;
    d.language = Language::CSharp;
    d.message = "; expected";
    CHECK_EQ(d.toString(), std::string("a.cs(2,5): error CS1002: ; expected"));
    script::Diagnostic t;
    t.file = "a.ts";
    t.line = 3;
    t.language = Language::TypeScript;
    t.message = "not assignable";
    CHECK_EQ(t.toString(), std::string("a.ts(3): error: not assignable"));
}
