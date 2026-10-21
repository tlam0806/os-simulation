// A minimal test framework, so the project has no external dependencies.
//
//   TEST(name) { CHECK(cond); CHECK_EQ(actual, expected); CHECK_THROWS(expr); }
//
// A failed check prints its location and values, and the test keeps going.
// The test binary exits with status 1 if any check failed.
#pragma once

#include <iostream>
#include <type_traits>
#include <utility>
#include <vector>

namespace test {

struct Case {
    const char* name;
    void (*fn)();
};

inline std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}

inline int failures = 0;

struct Registrar {
    Registrar(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

inline void fail(const char* file, int line, const char* what) {
    failures++;
    std::cerr << file << ":" << line << ": FAILED: " << what << "\n";
}

// Integers print in decimal and hex, so encodings are easy to compare.
template <class T>
void show(std::ostream& os, const T& v) {
    if constexpr (std::is_integral_v<T> && !std::is_same_v<T, bool>)
        os << +v << " (0x" << std::hex << +v << std::dec << ")";
    else
        os << v;
}

template <class A, class B>
void check_eq(const A& a, const B& b, const char* expr_a, const char* expr_b,
              const char* file, int line) {
    bool equal;
    if constexpr (std::is_integral_v<A> && std::is_integral_v<B> &&
                  !std::is_same_v<A, bool> && !std::is_same_v<B, bool>)
        equal = std::cmp_equal(a, b);  // compares signed and unsigned correctly
    else
        equal = a == b;
    if (equal) return;
    fail(file, line, "CHECK_EQ");
    std::cerr << "    " << expr_a << " = ";
    show(std::cerr, a);
    std::cerr << "\n    " << expr_b << " = ";
    show(std::cerr, b);
    std::cerr << "\n";
}

}  // namespace test

#define TEST(name)                                         \
    static void name();                                    \
    static test::Registrar name##_registrar(#name, name);  \
    static void name()

#define CHECK(cond) \
    do { if (!(cond)) test::fail(__FILE__, __LINE__, "CHECK(" #cond ")"); } while (0)

#define CHECK_EQ(a, b) test::check_eq((a), (b), #a, #b, __FILE__, __LINE__)

#define CHECK_THROWS(expr)                                                 \
    do {                                                                   \
        bool threw_ = false;                                               \
        try { (void)(expr); } catch (...) { threw_ = true; }               \
        if (!threw_) test::fail(__FILE__, __LINE__, "CHECK_THROWS(" #expr ")"); \
    } while (0)
