// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The C library subset: strings, character classes, math, stdlib.

#include "lang.hpp"
#include "tests/test_util.hpp"

using cc::output;
using cc::run;

TEST(stringFunctions)
{
    CHECK_EQ(run("#include <string.h>\nint main() { return strlen(\"hello\") + strlen(\"\"); }").toInt(), 5);
    CHECK_EQ(run("#include <string.h>\nint main() { char b[10] = \"abc\"; return strlen(b); }").toInt(), 3);
    CHECK_EQ(run("#include <string.h>\nint main() { return strcmp(\"a\", \"b\") < 0 && strcmp(\"b\", \"a\") > 0 && strcmp(\"x\", \"x\") == 0; }").toInt(), 1);
    CHECK_EQ(run("#include <string.h>\nint main() { return strncmp(\"abcd\", \"abxx\", 2) == 0 && strncmp(\"abcd\", \"abxx\", 3) != 0; }").toInt(), 1);
    CHECK_EQ(run("#include <string.h>\nint main() { return strstr(\"haystack\", \"st\") + strchr(\"abc\", 'c') + (strchr(\"abc\", 'z') < 0); }").toInt(), 3 + 2 + 1);
    CHECK_EQ(run("#include <stdlib.h>\nint main() { return atoi(\"42\") + atoi(\" -7\") + (int)atol(\"1000\") + (int)(atof(\"2.5\") * 2); }").toInt(), 42 - 7 + 1000 + 5);
    CHECK_EQ(run("#include <string.h>\nint count(const char* s, char c) { int n = 0; for (int i = 0; i < strlen(s); i++) if (s[i] == c) n++; return n; }\nint main() { return count(\"banana\", 'a'); }").toInt(), 3);
}

TEST(characterClasses)
{
    CHECK_EQ(run("#include <ctype.h>\nint main() { return isdigit('5') && !isdigit('a') && isalpha('a') && !isalpha('1') && isspace(' ') && isalnum('z') && isupper('A') && islower('b') && ispunct('!'); }").toInt(), 1);
    CHECK_EQ(run("#include <ctype.h>\nint main() { return toupper('a') == 'A' && tolower('Z') == 'z' && toupper('1') == '1'; }").toInt(), 1);
    CHECK_EQ(output("#include <stdio.h>\n#include <ctype.h>\nint main() { const char* s = \"Hello, World\"; for (int i = 0; s[i]; i++) putchar(isupper(s[i]) ? tolower(s[i]) : toupper(s[i])); return 0; }"), "hELLO, wORLD");
}

TEST(mathFunctions)
{
    CHECK_EQ(run("#include <math.h>\nint main() { return (int)(sqrt(16) + pow(2, 10) + fabs(-1.5) * 2 + floor(2.7) + ceil(2.1) + round(2.5) + trunc(-2.7)); }").toInt(), 4 + 1024 + 3 + 2 + 3 + 3 - 2);
    CHECK_EQ(run("#include <math.h>\nint main() { return (int)(fmod(7.5, 2) * 10 + fmin(1, 2) + fmax(1, 2) + hypot(3, 4)); }").toInt(), 15 + 1 + 2 + 5);
    CHECK_EQ(run("#include <math.h>\nint main() { double e = exp(1); return (int)(log(e) * 10 + log2(8) + log10(1000) + cbrt(27)); }").toInt(), 10 + 3 + 3 + 3);
    CHECK_EQ(run("#include <math.h>\nint main() { return (int)(sin(0) + cos(0) + tan(0) + atan2(1, 1) * 4 / M_PI + asin(1) * 2 / M_PI + acos(1) + atan(0)); }").toInt(), 1 + 1 + 1);
    CHECK_EQ(run("#include <math.h>\nint main() { return isnan(NAN) && isinf(INFINITY) && !isnan(1.0) && sqrt(-1) != sqrt(-1); }").toInt(), 1);
    CHECK_EQ(run("#include <stdlib.h>\nint main() { return abs(-3) + labs(-4L) + (int)(sinh(0) + cosh(0) + tanh(0)); }").toInt(), 8);
    CHECK_EQ(run("#include <stdlib.h>\nint main() { srand(1); int r = rand(); return r >= 0 && r <= RAND_MAX; }").toInt(), 1);
}

TEST(constants)
{
    CHECK_EQ(run("#include <limits.h>\nint main() { return INT_MAX == 2147483647 && INT_MIN < 0 && CHAR_MAX == 127 && UCHAR_MAX == 255 && SHRT_MAX == 32767; }").toInt(), 1);
    CHECK_EQ(run("#include <limits.h>\nint main() { long l = LONG_MAX; return l > 0 && LLONG_MIN < 0 && UINT_MAX == 4294967295L; }").toInt(), 1);
    CHECK_EQ(run("#include <float.h>\n#include <math.h>\nint main() { return DBL_MAX > 1e300 && DBL_EPSILON < 1e-10 && M_PI > 3.14 && M_PI < 3.15 && M_E > 2.7; }").toInt(), 1);
    CHECK_EQ(run("#include <stdio.h>\n#include <stdlib.h>\nint main() { return EOF == -1 && EXIT_SUCCESS == 0 && EXIT_FAILURE == 1 && NULL == 0; }").toInt(), 1);
    CHECK_EQ(run("#include <stdbool.h>\nint main() { bool t = true, f = false; return t == 1 && f == 0 && !f; }").toInt(), 1);
}
