// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "tests/test_util.hpp"

int main(int argc, char** argv)
{
    // One optional argument: run only the tests whose name contains it.
    return tst::runAll(argv[0], argc > 1 ? argv[1] : nullptr);
}
