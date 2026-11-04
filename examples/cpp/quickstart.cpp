// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The five-minute tour, in C++: a program with a class, a container and a
// stream; a host class; compile once and run many times; an exception that
// crosses the boundary.
//
//   ./cpp_quickstart

#include <cstdio>
#include <string>

#include "script/script.hpp"

namespace {

struct Sensor
{
    double reading = 20.5;
    double read() const { return reading; }
    void calibrate(double offset) { reading += offset; }
};

script::Context::Options languageOptions()
{
    script::Context::Options options;
    options.language = script::Language::Cpp;
    return options;
}

}  // namespace

int main()
{
    script::Context ctx(languageOptions());
    ctx.setPrintHandler([](std::string_view text, bool) { std::fwrite(text.data(), 1, text.size(), stdout); });

    // 1. Run a program; main's return value comes back.
    std::printf("main returned %d\n", ctx.eval("int main() { return 6 * 7; }").toInt());

    // 2. Classes, std::string, std::vector, std::map and std::cout work the
    //    way a C++ programmer expects. Output goes wherever the host says.
    ctx.eval(R"(
        #include <iostream>
        #include <map>
        #include <string>
        #include <vector>

        class Tally {
        public:
            void add(const std::string& word) { counts_[word]++; total_++; }
            int total() const { return total_; }
            int of(const std::string& word) { return counts_.count(word) ? counts_.at(word) : 0; }
        private:
            std::map<std::string, int> counts_;
            int total_ = 0;
        };

        int main() {
            std::vector<std::string> words = {"tick", "tock", "tick"};
            Tally tally;
            for (const auto& w : words) tally.add(w);
            std::cout << tally.total() << " words, " << tally.of("tick") << " of them tick" << std::endl;
            return 0;
        }
    )", "tally.cpp");

    // 3. A C++ class from the host, callable from the script as a class.
    script::Class<Sensor>(ctx, "Sensor")
        .constructor<>()
        .method("Read", &Sensor::read)
        .method("Calibrate", &Sensor::calibrate)
        .install();
    ctx.eval(R"(
        #include <iostream>
        int main() {
            Sensor s;
            s.Calibrate(0.5);
            std::cout << "sensor reads " << s.Read() << std::endl;
            return 0;
        }
    )", "sensor.cpp");

    // 4. Compile once, run many times: statics persist between runs.
    script::Assembly counter = ctx.compile(R"(
        class Runs { public: static int count; };
        int Runs::count = 0;
        int main() { return ++Runs::count; }
    )", "counter.cpp");
    for (int i = 0; i < 3; ++i)
        std::printf("run %d\n", ctx.run(counter).toInt());

    // 5. Exceptions carry their message and the file and line.
    try {
        ctx.eval(R"(
            #include <stdexcept>
            int check(int v) {
                if (v < 0) throw std::invalid_argument("negative reading");
                return v;
            }
            int main() { return check(-1); }
        )", "check.cpp");
    } catch (const script::Exception& e) {
        std::printf("caught %s\n%s", e.what(), e.stack().c_str());
    }
    return 0;
}
