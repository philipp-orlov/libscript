// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Several compilation units making one program.
//
// Each unit is its own assembly: compiled separately, checked against what the
// earlier ones declared, and holding its own statics. What makes them one
// program is the context's type table, which every assembly publishes into and
// reads from.
#include <memory>
#include <vector>

#include "script/script.hpp"
#include "tests/test_util.hpp"

namespace {

const char* kShapes = R"CS(
    namespace Geometry {
        public interface IShape {
            double Area();
            string Name { get; }
        }

        public abstract class Shape : IShape {
            public static int Created = 0;
            protected Shape() { Created++; }
            public abstract double Area();
            public abstract string Name { get; }
            public override string ToString() => Name + "=" + Area();
        }

        public class Rect : Shape {
            private readonly double w;
            private readonly double h;
            public Rect(double width, double height) { w = width; h = height; }
            public override double Area() => w * h;
            public override string Name => "rect";
        }
    }
)CS";

const char* kCircles = R"CS(
    using Geometry;

    namespace Geometry {
        public class Circle : Shape {
            private readonly double r;
            public Circle(double radius) { r = radius; }
            public override double Area() => System.Math.PI * r * r;
            public override string Name => "circle";
        }
    }
)CS";

const char* kUtil = R"CS(
    using System.Collections.Generic;
    using Geometry;

    public static class Report {
        public static string Line(IShape shape) => shape.ToString();

        public static double Total(List<IShape> shapes) {
            double sum = 0;
            foreach (IShape s in shapes) sum += s.Area();
            return sum;
        }
    }
)CS";

const char* kMain = R"CS(
    using System.Collections.Generic;
    using Geometry;

    var shapes = new List<IShape>();
    shapes.Add(new Rect(2, 3));
    shapes.Add(new Circle(1));
    shapes.Add(new Rect(1, 1));

    string first = Report.Line(shapes[0]);
    double total = Report.Total(shapes);
    return first + " of " + shapes.Count + " shapes, " + Shape.Created + " created";
)CS";

}  // namespace

TEST(unitsSeeEachOther)
{
    script::Context ctx;
    ctx.compile(kShapes, "shapes.cs");
    ctx.compile(kCircles, "circles.cs");
    ctx.compile(kUtil, "report.cs");
    script::Assembly main = ctx.compile(kMain, "main.cs");
    CHECK_EQ(ctx.run(main).toString(), "rect=6 of 3 shapes, 3 created");
}

TEST(orderMattersOnlyForDeclarations)
{
    script::Context ctx;
    ctx.compile(kShapes, "shapes.cs");
    // `report.cs` names `Circle` nowhere, so it compiles before it exists.
    ctx.compile(kUtil, "report.cs");
    ctx.compile(kCircles, "circles.cs");
    script::Assembly main = ctx.compile(kMain, "main.cs");
    CHECK(ctx.run(main).toString().find("3 shapes") != std::string::npos);

    // The other way round is an error, and a clear one.
    script::Context fresh;
    script::Assembly out;
    const std::vector<script::Diagnostic> diagnostics = fresh.diagnose(kCircles, "circles.cs", &out);
    CHECK(!diagnostics.empty());
    CHECK(diagnostics.front().message.find("Shape") != std::string::npos);
}

TEST(staticsBelongToTheirOwnUnit)
{
    script::Context ctx;
    script::Assembly counters = ctx.compile(R"CS(
        public static class Counters {
            public static int Hits = 100;
            public static int Bump() { Hits++; return Hits; }
        }
    )CS",
                                        "counters.cs");
    script::Assembly user = ctx.compile(R"CS(
        Counters.Hits += 5;
        Counters.Bump();
        return Counters.Hits;
    )CS",
                                    "user.cs");

    // One store, whichever unit reaches it.
    CHECK_EQ(ctx.run(user).toInt(), 106);
    CHECK_EQ(ctx.call(counters, "Counters.Bump").toInt(), 107);
    CHECK_EQ(ctx.run(user).toInt(), 113);
}

TEST(staticInitializersRunBeforeAnyUnitDoes)
{
    script::Context ctx;
    ctx.compile(R"CS(
        public static class Config {
            public static string Name = "libcs";
            public static int Retries = 2 + 1;
        }
    )CS",
                "config.cs");
    script::Assembly main = ctx.compile("return Config.Name + Config.Retries;", "main.cs");
    // The initializers live in another unit that was never entered.
    CHECK_EQ(ctx.run(main).toString(), "libcs3");
}

TEST(unitsCanBeShippedAsImages)
{
    std::vector<std::uint8_t> shapesImage;
    std::vector<std::uint8_t> mainImage;
    {
        script::Context build;
        shapesImage = build.compile(kShapes, "shapes.cs").serialize();
        build.compile(kCircles, "circles.cs");
        build.compile(kUtil, "report.cs");
        mainImage = build.compile(kMain, "main.cs").serialize();
    }

    // The main unit's image refers to types and methods the other units
    // declare, by name, so they have to be loaded first -- the same order the
    // compiler needed.
    script::Context ctx;
    ctx.load(shapesImage.data(), shapesImage.size());
    ctx.compile(kCircles, "circles.cs");
    ctx.compile(kUtil, "report.cs");
    script::Assembly main = ctx.load(mainImage.data(), mainImage.size());
    CHECK_EQ(ctx.run(main).toString(), "rect=6 of 3 shapes, 3 created");
}

TEST(missingUnitIsReported)
{
    script::Context ctx;
    script::Assembly assembly = ctx.compile(kShapes, "shapes.cs");
    const std::vector<std::uint8_t> image =
        [&] {
            script::Context build;
            build.compile(kShapes, "shapes.cs");
            build.compile(kCircles, "circles.cs");
            build.compile(kUtil, "report.cs");
            return build.compile(kMain, "main.cs").serialize();
        }();

    // `Circle` and `Report` were never loaded here.
    CHECK_THROWS(ctx.load(image.data(), image.size()), script::ErrorCode::BadImage);
    (void)assembly;
}

TEST(hostClassesAreVisibleToEveryUnit)
{
    script::Context ctx;
    ctx.hostClass("Env").method("Scale", [](int n) { return n * 3; });
    ctx.compile("public static class A { public static int Use(int n) => Env.Scale(n); }",
                "a.cs");
    script::Assembly b = ctx.compile("return A.Use(14);", "b.cs");
    CHECK_EQ(ctx.run(b).toInt(), 42);
}
