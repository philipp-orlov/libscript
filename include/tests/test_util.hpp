// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// libcore's test harness, with checks that end the test, and what these suites check beyond it.
#pragma once

#include <string>

#define TST_FAIL_FAST
#include "mini_test.hpp"
#include "script/error.hpp"

#define CHECK_THROWS(expr, expectedCode)                                               \
    do {                                                                               \
        bool caught = false;                                                           \
        try {                                                                          \
            (void)(expr);                                                              \
        } catch (const ::script::Error& err) {                                         \
            caught = err.code() == (expectedCode);                                     \
            if (!caught)                                                               \
                ::tst::fail(__FILE__, __LINE__,                                        \
                            std::string("wrong error from " #expr ": ") + err.what()); \
        }                                                                              \
        if (!caught)                                                                   \
            ::tst::fail(__FILE__, __LINE__, #expr " did not throw " #expectedCode);    \
    } while (0)
