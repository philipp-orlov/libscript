// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The five-minute tour: compile, run, and move values across the boundary.
#include <cstdio>

#include "script/script.hpp"

int main()
{
    script::Context ctx;

    // Top-level statements, like a modern C# file. `return` hands a value back.
    const script::Value answer = ctx.eval("int a = 6, b = 7; return a * b;");
    std::printf("answer = %s (%s)\n", answer.toString().c_str(), answer.typeName().c_str());

    // A whole program: types, collections, string interpolation.
    ctx.eval(R"CS(
        using System.Collections.Generic;

        class Employee {
            public string Name { get; set; }
            public int Salary { get; set; }
            public Employee(string name, int salary) {
                Name = name;
                Salary = salary;
            }
            public override string ToString() => $"{Name} ({Salary:N0})";
        }

        var staff = new List<Employee> {
            new Employee("Ada", 120000),
            new Employee("Grace", 135000),
        };

        int total = 0;
        foreach (Employee e in staff) {
            Console.WriteLine(e);
            total += e.Salary;
        }
        Console.WriteLine($"payroll: {total:N0}");
    )CS",
             "staff.cs");

    // Compile once, call many times.
    script::Assembly assembly = ctx.compile(R"CS(
        static class Fib {
            public static long Of(int n) => n < 2 ? n : Of(n - 1) + Of(n - 2);
        }
    )CS",
                                        "fib.cs");
    for (int n = 10; n <= 30; n += 10) {
        const script::Value result = ctx.call(assembly, "Fib.Of", {script::Value(n)});
        std::printf("fib(%d) = %lld\n", n, static_cast<long long>(result.toLong()));
    }

    // A program that does not compile reports every reason, not just the first.
    try {
        ctx.compile("int n = \"text\";\nstring s = 42;", "bad.cs");
    } catch (const script::CompileError& error) {
        for (const script::Diagnostic& diagnostic : error.diagnostics())
            std::printf("%s\n", diagnostic.toString().c_str());
    }

    // A program that throws arrives as a C++ exception carrying the object.
    try {
        ctx.eval("throw new System.InvalidOperationException(\"nope\");");
    } catch (const script::Exception& error) {
        std::printf("caught %s: %s\n", error.value().typeName().c_str(), error.what());
    }
    return 0;
}
