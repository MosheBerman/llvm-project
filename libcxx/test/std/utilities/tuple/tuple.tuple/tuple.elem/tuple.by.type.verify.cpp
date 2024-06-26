//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

// UNSUPPORTED: c++03, c++11

#include <tuple>
#include <string>
#include <memory>

#include "test_macros.h"

struct UserType {};

void test_bad_index() {
    std::tuple<long, long, char, std::string, char, UserType, char> t1;
    TEST_IGNORE_NODISCARD std::get<int>(t1); // expected-error@*:* {{type not found}}
    TEST_IGNORE_NODISCARD std::get<long>(t1); // expected-note {{requested here}}
    TEST_IGNORE_NODISCARD std::get<char>(t1); // expected-note {{requested here}}
        // expected-error@*:* 2 {{type occurs more than once}}
    std::tuple<> t0;
    TEST_IGNORE_NODISCARD std::get<char*>(t0); // expected-node {{requested here}}
        // expected-error@*:* {{type not in empty type list}}
}

void test_bad_return_type() {
    typedef std::unique_ptr<int> upint;
    std::tuple<upint> t;
    upint p = std::get<upint>(t); // expected-error{{deleted copy constructor}}
}

void f() {
    test_bad_index();
    test_bad_return_type();
}
