// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// A program in several files.
//
// Each unit is compiled on its own and becomes its own assembly, the way a
// project's source files do. What makes them one program is the context: every
// assembly publishes its types into it, so the next unit can name them, and a
// static field has one home however many units reach it.
#include <cstdio>
#include <string>
#include <vector>

#include "script/script.hpp"

namespace {

/// The units, in the order a build system would hand them over. In a real
/// embedding these are files; here they are strings so the example is one
/// program you can run.
struct Unit
{
    const char* name;
    const char* source;
};

const Unit kUnits[] = {
    // ---------------------------------------------------------- money.cs ---
    {"money.cs", R"CS(
        namespace Shop {
            public struct Money {
                public int Cents;
                public Money(int cents) { Cents = cents; }
                public override string ToString() => "$" + (Cents / 100) + "." + (Cents % 100).ToString("D2");
            }
        }
    )CS"},

    // ------------------------------------------------------ catalogue.cs ---
    {"catalogue.cs", R"CS(
        using System.Collections.Generic;

        namespace Shop {
            public interface IPriced {
                Money Price { get; }
                string Sku { get; }
            }

            public class Item : IPriced {
                public static int Created = 0;

                public string Sku { get; set; }
                public Money Price { get; set; }
                public int Quantity { get; set; }

                public Item(string sku, int cents, int quantity) {
                    Sku = sku;
                    Price = new Money(cents);
                    Quantity = quantity;
                    Created++;
                }
                public override string ToString() => Sku + " x" + Quantity + " @ " + Price;
            }

            public static class Catalogue {
                public static Dictionary<string, Item> Items = new Dictionary<string, Item>();

                public static void Add(Item item) { Items[item.Sku] = item; }
                public static Item Find(string sku) {
                    Item found;
                    if (Items.TryGetValue(sku, out found)) return found;
                    return null;
                }
            }
        }
    )CS"},

    // --------------------------------------------------------- pricing.cs ---
    {"pricing.cs", R"CS(
        using System.Collections.Generic;
        using Shop;

        namespace Shop {
            // A second unit extending a type the first one declared.
            public class BulkItem : Item {
                public int BreakAt { get; set; }
                public BulkItem(string sku, int cents, int quantity, int breakAt)
                    : base(sku, cents, quantity) {
                    BreakAt = breakAt;
                }
                public override string ToString() => base.ToString() + " (bulk)";
            }
        }

        public static class Pricing {
            public const int DiscountPercent = 10;

            public static int LineTotal(Item item) {
                int gross = item.Price.Cents * item.Quantity;
                var bulk = item as BulkItem;
                if (bulk != null && item.Quantity >= bulk.BreakAt)
                    gross -= gross * DiscountPercent / 100;
                return gross;
            }

            public static int Total(List<Item> items) {
                int total = 0;
                foreach (Item item in items) total += LineTotal(item);
                return total;
            }
        }
    )CS"},

    // ----------------------------------------------------------- main.cs ---
    {"main.cs", R"CS(
        using System.Collections.Generic;
        using Shop;

        Catalogue.Add(new Item("bolt", 125, 4));
        Catalogue.Add(new BulkItem("washer", 15, 500, 100));
        Catalogue.Add(new Item("nut", 40, 12));

        var basket = new List<Item>();
        foreach (string sku in Catalogue.Items.Keys)
            basket.Add(Catalogue.Find(sku));

        foreach (Item item in basket)
            Host.Line(item + " = " + new Money(Pricing.LineTotal(item)));

        int total = Pricing.Total(basket);
        Host.Line("items created: " + Item.Created);
        return new Money(total).ToString();
    )CS"},
};

}  // namespace

int main()
{
    script::Context ctx;
    ctx.hostClass("Host").method("Line",
                                 [](std::string text) { std::printf("  %s\n", text.c_str()); });

    // Compile the units in order. A unit is checked against everything the
    // earlier ones declared, so a name that is not there yet is a compile
    // error rather than a surprise at run time.
    std::vector<script::Assembly> assemblies;
    for (const Unit& unit : kUnits) {
        try {
            script::Assembly assembly = ctx.compile(unit.source, unit.name);
            std::printf("compiled %-14s %2zu methods\n", unit.name, assembly.methods().size());
            assemblies.push_back(std::move(assembly));
        } catch (const script::CompileError& error) {
            for (const script::Diagnostic& diagnostic : error.diagnostics())
                std::printf("%s\n", diagnostic.toString().c_str());
            return 1;
        }
    }

    std::printf("\nrunning main.cs:\n");
    const script::Value total = ctx.run(assemblies.back());
    std::printf("  basket total: %s\n", total.toString().c_str());

    // Any unit's methods are callable from the host, not just the entry point.
    const script::Value bolt = ctx.call(assemblies[1], "Shop.Catalogue.Find", {ctx.str("bolt")});
    std::printf("\nlooked up from C++: %s\n", bolt.toString().c_str());
    std::printf("its line total:     %s\n",
                ctx.call(assemblies[2], "Pricing.LineTotal", {bolt}).toString().c_str());

    // Order is the only rule: a unit cannot name what has not been declared.
    std::printf("\ncompiling a unit out of order:\n");
    try {
        script::Context fresh;
        fresh.compile(kUnits[2].source, kUnits[2].name);
    } catch (const script::CompileError& error) {
        std::printf("  %s\n", error.diagnostics().front().toString().c_str());
    }

    // Units ship as images just as well as source, and load the same way.
    std::printf("\nas images:\n");
    script::Context runtime;
    runtime.hostClass("Host").method("Line", [](std::string text) {
        std::printf("  %s\n", text.c_str());
    });
    std::vector<script::Assembly> loaded;
    for (script::Assembly& assembly : assemblies) {
        const std::vector<std::uint8_t> image = assembly.serialize();
        loaded.push_back(runtime.load(image.data(), image.size()));
        std::printf("  loaded %-14s %5zu bytes\n", assembly.name().c_str(), image.size());
    }
    std::printf("  total: %s\n", runtime.run(loaded.back()).toString().c_str());
    return 0;
}
