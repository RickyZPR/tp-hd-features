// The self-test runner behind `hdf-convert self-test` (no game data).
#include "test.hpp"

#include <algorithm>
#include <cstdio>
#include <exception>

namespace hdf::test {

namespace {
struct State {
    int failures = 0;
    bool skipped = false;
    std::string skip_why;
};
State g_state;
}  // namespace

std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}

void fail(const char* file, int line, const std::string& what) {
    ++g_state.failures;
    std::fprintf(stderr, "    FAIL %s:%d: %s\n", file, line, what.c_str());
}

void skip(const std::string& why) {
    g_state.skipped = true;
    g_state.skip_why = why;
}

int run(std::string_view filter, bool verbose) {
    std::vector<Case> cases = registry();
    std::stable_sort(cases.begin(), cases.end(),
                     [](const Case& a, const Case& b) { return std::string_view(a.name) < std::string_view(b.name); });
    int passed = 0, failed = 0, skipped = 0;
    for (const Case& c : cases) {
        if (!filter.empty() && std::string_view(c.name).find(filter) == std::string_view::npos) continue;
        g_state = State{};
        try {
            c.fn();
        } catch (const std::exception& e) {
            fail(c.file, 0, std::string("exception: ") + e.what());
        } catch (...) {
            fail(c.file, 0, "unknown exception");
        }
        if (g_state.failures > 0) {
            ++failed;
            std::fprintf(stderr, "FAIL  %s\n", c.name);
        } else if (g_state.skipped) {
            ++skipped;
            if (verbose) std::printf("skip  %s (%s)\n", c.name, g_state.skip_why.c_str());
        } else {
            ++passed;
            if (verbose) std::printf("ok    %s\n", c.name);
        }
    }
    std::printf("self-test: %d passed, %d failed, %d skipped\n", passed, failed, skipped);
    return failed;
}

}  // namespace hdf::test
