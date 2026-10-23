// Runs the tests.
//
//   ossim_tests                 run every test
//   ossim_tests sbrk lazy_      run only tests whose name contains "sbrk" or "lazy_"
//
// Each line shows how long the test took, so slow tests are easy to spot.
#include <chrono>
#include <cstdio>
#include <cstring>
#include <exception>

#include "test.hpp"

namespace {

bool selected(const char* name, int argc, char** argv) {
    if (argc < 2) return true;
    for (int i = 1; i < argc; i++)
        if (std::strstr(name, argv[i])) return true;
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    using Clock = std::chrono::steady_clock;
    const auto start_all = Clock::now();
    size_t ran = 0;
    for (const test::Case& c : test::registry()) {
        if (!selected(c.name, argc, argv)) continue;
        ran++;
        const int before = test::failures;
        const auto start = Clock::now();
        // An exception (e.g. .value() on an empty optional) fails this test only.
        try {
            c.fn();
        } catch (const std::exception& e) {
            test::failures++;
            std::fprintf(stderr, "%s: FAILED: uncaught exception: %s\n", c.name, e.what());
        } catch (...) {
            test::failures++;
            std::fprintf(stderr, "%s: FAILED: uncaught exception\n", c.name);
        }
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
        std::printf("%-4s %6lld ms  %s\n", test::failures == before ? "ok" : "FAIL", static_cast<long long>(ms), c.name);
    }
    const auto total = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start_all).count();
    std::printf("%zu tests, %d failed checks, %lld ms\n", ran, test::failures, static_cast<long long>(total));
    if (ran == 0) std::printf("no test name matched the filter\n");
    return test::failures == 0 && ran > 0 ? 0 : 1;
}
