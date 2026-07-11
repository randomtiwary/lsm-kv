#pragma once

#include <cstdint>
#include <iostream>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "lsmkv/status.h"

namespace test {

struct Stats {
    int passed = 0;
    int failed = 0;
};

inline Stats& stats() {
    static Stats s;
    return s;
}

inline void expect(bool cond, const std::string& msg) {
    if (cond) {
        ++stats().passed;
    } else {
        ++stats().failed;
        std::cerr << "FAIL: " << msg << "\n";
    }
}

template <typename A, typename B>
inline void expect_eq(const A& a, const B& b, const std::string& msg) {
    const bool equal = [&]() {
        if constexpr (std::is_integral<A>::value && std::is_integral<B>::value) {
            return static_cast<std::uint64_t>(a) == static_cast<std::uint64_t>(b);
        } else {
            return a == b;
        }
    }();
    if (equal) {
        ++stats().passed;
    } else {
        ++stats().failed;
        std::cerr << "FAIL: " << msg << "\n  expected: " << b << "\n  actual:   " << a << "\n";
    }
}

inline void expect_ok(const lsmkv::Status& st, const std::string& msg) {
    if (st.ok()) {
        ++stats().passed;
    } else {
        ++stats().failed;
        std::cerr << "FAIL: " << msg << ": " << st.ToString() << "\n";
    }
}

// Asserts that a Status-producing expression is OK.
#define EXPECT_OK(expr, msg) ::test::expect_ok((expr), (msg))

using TestFn = void (*)();

inline std::vector<std::pair<std::string, TestFn>>& registry() {
    static std::vector<std::pair<std::string, TestFn>> r;
    return r;
}

struct Register {
    Register(const char* name, TestFn fn) { registry().emplace_back(name, fn); }
};

#define TEST(name)                                 \
    void name();                                   \
    static test::Register reg_##name(#name, name); \
    void name()

inline int run_all() {
    for (auto& entry : registry()) {
        std::cout << "[ RUN  ] " << entry.first << "\n";
        entry.second();
        std::cout << "[ DONE ] " << entry.first << "\n";
    }
    std::cout << "\nPassed: " << stats().passed << "  Failed: " << stats().failed << "\n";
    return stats().failed == 0 ? 0 : 1;
}

}  // namespace test

using test::expect;
using test::expect_eq;
