// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Compiling to an image and loading it back.
#include <memory>
#include <vector>

#include "script/script.hpp"
#include "tests/test_util.hpp"

namespace {

const char* kProgram = R"(
    using System;
    using System.Collections.Generic;

    namespace Demo {
        public enum Level { Low, High = 10 }

        public interface INamed { string Name(); }

        public class Widget : INamed {
            private readonly string name;
            public int Size { get; set; }
            public static int Made = 0;
            public const double Ratio = 1.5;

            public Widget(string n, int size) {
                name = n;
                Size = size;
                Made++;
            }
            public string Name() { return name; }
            public override string ToString() { return name + ":" + Size; }
            public virtual int Scaled() { return (int)(Size * Ratio); }
        }

        public class BigWidget : Widget {
            public BigWidget(string n) : base(n, 100) { }
            public override int Scaled() { return Size * 2; }
        }
    }

    static class Runner {
        public static string Describe() {
            var widgets = new List<Demo.Widget>();
            widgets.Add(new Demo.Widget("a", 2));
            widgets.Add(new Demo.BigWidget("b"));
            var text = new System.Text.StringBuilder();
            foreach (Demo.Widget w in widgets) {
                text.Append(w.ToString());
                text.Append("/");
                text.Append(w.Scaled());
                text.Append(" ");
            }
            text.Append(Demo.Widget.Made);
            text.Append((int)Demo.Level.High);
            return text.ToString();
        }
    }

    return Runner.Describe();
)";

}  // namespace

TEST(roundTrip)
{
    std::vector<std::uint8_t> image;
    std::string direct;
    {
        script::Context ctx;
        script::Assembly assembly = ctx.compile(kProgram, "demo.cs");
        direct = ctx.run(assembly).toString();
        image = assembly.serialize();
    }
    CHECK(!image.empty());
    CHECK(!direct.empty());

    // A fresh context that never saw the source has to produce the same run.
    script::Context ctx;
    script::Assembly loaded = ctx.load(image.data(), image.size());
    CHECK_EQ(ctx.run(loaded).toString(), direct);
    CHECK_EQ(loaded.entryPoint(), std::string("Program.Main"));
}

TEST(imageIsStable)
{
    script::Context first;
    script::Context second;
    const std::vector<std::uint8_t> a = first.compile(kProgram, "demo.cs").serialize();
    const std::vector<std::uint8_t> b = second.compile(kProgram, "demo.cs").serialize();
    CHECK(a == b);
}

TEST(loadedAssemblyKeepsItsApi)
{
    std::vector<std::uint8_t> image;
    {
        script::Context ctx;
        image = ctx
                    .compile(R"(
            public class Point {
                public int X;
                public int Y;
                public Point(int x, int y) { X = x; Y = y; }
                public int Sum() { return X + Y; }
            }
            static class Library {
                public static int Twice(int n) { return n * 2; }
            }
        )",
                             "<lib>")
                    .serialize();
    }

    script::Context ctx;
    script::Assembly assembly = ctx.load(image.data(), image.size());
    CHECK_EQ(ctx.call(assembly, "Library.Twice", {script::Value(21)}).toInt(), 42);
    script::Value point = ctx.newObject(assembly, "Point", {script::Value(1), script::Value(2)});
    CHECK_EQ(point.invoke("Sum").toInt(), 3);
}

TEST(badImagesAreRejected)
{
    script::Context ctx;
    const std::vector<std::uint8_t> garbage(64, 0x7f);
    CHECK_THROWS(ctx.load(garbage.data(), garbage.size()), script::ErrorCode::BadImage);

    script::Assembly assembly = ctx.compile("return 1;", "<x>");
    std::vector<std::uint8_t> image = assembly.serialize();
    image.resize(image.size() / 2);
    CHECK_THROWS(ctx.load(image.data(), image.size()), script::ErrorCode::BadImage);
}

TEST(exceptionsSurviveTheRoundTrip)
{
    std::vector<std::uint8_t> image;
    {
        script::Context ctx;
        image = ctx
                    .compile(R"(
            static class M {
                public static int Risky(int n) {
                    try {
                        if (n == 0) throw new System.InvalidOperationException("zero");
                        return 100 / n;
                    } catch (System.InvalidOperationException) {
                        return -1;
                    } finally {
                        Trace.Note();
                    }
                }
            }
            static class Trace { public static int Count = 0; public static void Note() { Count++; } }
            return M.Risky(0) + M.Risky(4) + Trace.Count;
        )",
                             "<x>")
                    .serialize();
    }
    script::Context ctx;
    script::Assembly assembly = ctx.load(image.data(), image.size());
    CHECK_EQ(ctx.run(assembly).toInt(), -1 + 25 + 2);
}

TEST(loadingIsCheaperThanCompiling)
{
    script::Context ctx;
    script::Assembly assembly = ctx.compile(kProgram, "demo.cs");
    const std::vector<std::uint8_t> image = assembly.serialize();
    // Not a timing assertion -- just that the image is a compact, bounded
    // thing rather than a copy of the source plus the world.
    CHECK(image.size() < 64u * 1024u);
    CHECK(assembly.imageSize() > 0);
    CHECK(!assembly.methods().empty());
}
