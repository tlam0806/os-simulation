#include <cstdio>
#include <exception>

#include "test.hpp"

int main() {
    for (const test::Case& c : test::registry()) {
        const int before = test::failures;
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
        std::printf("%-4s %s\n", test::failures == before ? "ok" : "FAIL", c.name);
    }
    std::printf("%zu tests, %d failed checks\n", test::registry().size(), test::failures);
    return test::failures == 0 ? 0 : 1;
}
