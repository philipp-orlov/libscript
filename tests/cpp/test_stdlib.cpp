// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The standard library subset: std::string, std::vector, std::map, the
// streams, and the std:: functions that are not libc names.

#include "lang.hpp"
#include "tests/test_util.hpp"

using cpp::output;
using cpp::run;

TEST(strings)
{
    CHECK_EQ(run("#include <string>\nint main() { std::string s = \"hello\"; return s.size() + s.length(); }").toInt(), 10);
    CHECK_EQ(run("#include <string>\nusing namespace std;\nint main() { string a = \"ab\"; string b = a + \"cd\" + 'e'; return b.size() + (b == \"abcde\") * 10; }").toInt(), 15);
    CHECK_EQ(run("#include <string>\nint main() { std::string s; s += \"x\"; s += 'y'; s.append(\"z\"); s.push_back('!'); return s.size() + s.empty(); }").toInt(), 4);
    CHECK_EQ(run("#include <string>\nint main() { std::string s = \"hello world\"; return s.find(\"world\") + s.find('o') * 100 + (s.find(\"zz\") == std::string::npos) * 1000; }").toInt(), 6 + 400 + 1000);
    CHECK_EQ(output("#include <iostream>\n#include <string>\nint main() { std::string s = \"hello world\"; std::cout << s.substr(6) << '|' << s.substr(0, 5) << '|' << s.substr(6, 100); return 0; }"), "world|hello|world");
    CHECK_EQ(run("#include <string>\nint main() { std::string s = \"abc\"; return s[0] + s.at(1) + s.front() + s.back() - 4 * 'a'; }").toInt(), 0 + 1 + 0 + 2);
    CHECK_EQ(run("#include <string>\nint main() { std::string a = \"apple\", b = \"banana\"; return (a < b) + (b > a) * 2 + (a <= a) * 4 + (a >= b) * 8 + (a != b) * 16; }").toInt(), 23);
    CHECK_EQ(run("#include <string>\nint main() { return std::to_string(42).size() + std::stoi(\"17\") + (int)std::stod(\"2.5\"); }").toInt(), 21);
    CHECK_EQ(run("#include <string>\nint main() { std::string s = \"ab\"; s.clear(); std::string t(3, 'z'); return s.empty() * 10 + t.size(); }").toInt(), 13);
    CHECK_EQ(run("#include <string>\nint main() { std::string s = \"hello\"; return s.starts_with(\"he\") + s.ends_with(\"lo\") * 2 + s.contains(\"ell\") * 4 + s.compare(\"hello\") * 8; }").toInt(), 7);
    CHECK_EQ(run("#include <cstring>\n#include <string>\nint main() { std::string s = \"four\"; const char* c = s.c_str(); return strlen(c); }").toInt(), 4);
}

TEST(vectors)
{
    CHECK_EQ(run("#include <vector>\nint main() { std::vector<int> v; v.push_back(3); v.push_back(4); return v.size() * 10 + v[1]; }").toInt(), 24);
    CHECK_EQ(run("#include <vector>\nint main() { std::vector<int> v = {5, 6, 7}; v[0] = 1; return v[0] + v.back() + v.front() * 100; }").toInt(), 108);
    CHECK_EQ(run("#include <vector>\nint main() { std::vector<int> v(4); std::vector<int> w(3, 9); return v.size() + v[3] + w[2]; }").toInt(), 13);
    CHECK_EQ(run("#include <vector>\nint main() { std::vector<double> v{1.5, 2.5}; double t = 0; for (double d : v) t += d; v.pop_back(); return (int)t + v.size(); }").toInt(), 5);
    CHECK_EQ(run("#include <vector>\nint main() { std::vector<int> v = {1}; v.clear(); return v.empty() + v.size(); }").toInt(), 1);
    CHECK_EQ(run("#include <vector>\n#include <string>\nint main() { std::vector<std::string> names = {\"ann\", \"bob\"}; names.push_back(\"cy\"); int n = 0; for (const auto& s : names) n += s.size(); return n; }").toInt(), 8);
    CHECK_EQ(run("#include <vector>\nint main() { std::vector<std::vector<int>> grid(3, std::vector<int>(4, 1)); grid[1][2] = 5; return grid[1][2] + grid[2][2] + grid.size() * 10 + grid[0].size() * 100; }").toInt(), 436);
    CHECK_EQ(run("#include <vector>\nint main() { std::vector<std::vector<int>> g(2); g[0].push_back(1); return g[0].size() * 10 + g[1].size(); }").toInt(), 10);
    CHECK_EQ(run("struct P { int x; int y; };\n#include <vector>\nint main() { std::vector<P> ps = {{1, 2}, {3, 4}}; return ps[1].x * ps[0].y; }").toInt(), 6);
    CHECK_EQ(run("#include <vector>\nint sum(std::vector<int> v) { int s = 0; for (size_t i = 0; i < v.size(); i++) s += v.at(i); return s; }\nint main() { return sum({1, 2, 3}); }").toInt(), 6);
    CHECK_EQ(run("#include <vector>\nstd::vector<int> make() { std::vector<int> v; for (int i = 0; i < 3; i++) v.push_back(i * i); return v; }\nint main() { auto v = make(); return v[2]; }").toInt(), 4);
}

TEST(maps)
{
    CHECK_EQ(run("#include <map>\n#include <string>\nint main() { std::map<std::string, int> m; m[\"a\"] = 1; m[\"b\"] = 2; return m[\"a\"] + m[\"b\"] * 10 + m.size() * 100; }").toInt(), 221);
    // operator[] inserts a default, as it does in C++.
    CHECK_EQ(run("#include <map>\n#include <string>\nint main() { std::map<std::string, int> counts; counts[\"x\"]++; counts[\"x\"]++; counts[\"y\"] += 5; return counts[\"x\"] * 10 + counts[\"y\"] + counts[\"z\"] + counts.size() * 100; }").toInt(), 325);
    CHECK_EQ(run("#include <map>\nint main() { std::map<int, int> m = {{1, 10}, {2, 20}}; return m.count(1) + m.count(3) * 10 + m.at(2) + m.contains(2) * 100; }").toInt(), 121);
    CHECK_EQ(run("#include <map>\n#include <string>\nint main() { std::map<std::string, int> m; m[\"k\"] = 1; m.erase(\"k\"); return m.empty() + m.size(); }").toInt(), 1);
    CHECK_EQ(run("#include <map>\n#include <string>\nint main() { std::map<std::string, int> m; m[\"a\"] = 1; m[\"b\"] = 2; int keys = 0, values = 0; for (const auto& kv : m) { keys += kv.first.size(); values += kv.second; } return keys * 10 + values; }").toInt(), 23);
    CHECK_EQ(run("#include <map>\n#include <string>\nint main() { std::map<std::string, int> m; m[\"a\"] = 1; for (auto& kv : m) kv.second *= 10; return m[\"a\"]; }").toInt(), 10);
    CHECK_EQ(run("#include <map>\n#include <string>\n#include <vector>\nint main() { std::map<std::string, std::vector<int>> g; g[\"a\"].push_back(1); g[\"a\"].push_back(2); return g[\"a\"].size() + g[\"b\"].size(); }").toInt(), 2);
    cpp::Context ctx;
    CHECK_THROWS(ctx.eval("#include <map>\nint main() { std::map<int, int> m; return m.at(3); }"), cpp::ErrorCode::KeyNotFound);
}

TEST(streams)
{
    CHECK_EQ(output("#include <iostream>\nint main() { std::cout << \"n=\" << 42 << ' ' << 2.5 << ' ' << 1.0 / 3 << std::endl; return 0; }"), "n=42 2.5 0.333333\n");
    CHECK_EQ(output("#include <iostream>\nusing namespace std;\nint main() { cout << 1e10 << \" \" << 100.0 << \" \" << 'c' << \" \" << 7L << endl; cerr << \"err\"; return 0; }"), "1e+10 100 c 7\nerr");
    cpp::Context ctx;
    std::string out;
    std::string err;
    ctx.setPrintHandler([&](std::string_view text, bool isError) { (isError ? err : out) += text; });
    ctx.eval("#include <iostream>\nint main() { std::cout << \"out\"; std::cerr << \"err\" << 1; std::clog << \"!\"; return 0; }");
    CHECK_EQ(out, std::string("out"));
    CHECK_EQ(err, std::string("err1!"));
    CHECK_EQ(output("#include <iostream>\n#include <string>\nint main() { std::string s = \"txt\"; bool b = true; std::cout << s << b << (s + \"!\"); return 0; }"), "txt1txt!");
    CHECK_EQ(output("#include <cstdio>\n#include <iostream>\nint main() { printf(\"%d;\", 1); std::cout << 2 << \";\"; puts(\"3\"); return 0; }"), "1;2;3\n");
}

TEST(mathAndFriends)
{
    CHECK_EQ(run("#include <cmath>\n#include <algorithm>\nint main() { return (int)std::sqrt(16.0) + std::max(2, 7) + std::min(3, 1) + std::abs(-4) + (int)std::abs(-2.5) + (int)std::max(1.5, 2.5); }").toInt(), 4 + 7 + 1 + 4 + 2 + 2);
    CHECK_EQ(run("#include <cmath>\nint main() { return (int)(std::pow(2, 10) + floor(2.7) + std::fabs(-1.0) + M_PI); }").toInt(), 1024 + 2 + 1 + 3);
    CHECK_EQ(run("#include <cstdlib>\n#include <climits>\nint main() { return abs(-3) + (INT_MAX > 0) + (int)std::atoi(\"5\"); }").toInt(), 9);
    CHECK_EQ(run("#include <algorithm>\nint main() { long a = 1L << 40, b = 3; return (int)(std::max(a, b) >> 38) + (int)std::min(a, b); }").toInt(), 7);
}
