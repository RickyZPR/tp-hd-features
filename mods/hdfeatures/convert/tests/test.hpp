// Unit tests on synthetic data only (no game data in the repo). They are compiled into hdf-convert and run by
// `hdf-convert self-test [filter]` (CTest: hdf_unit). Each module adds tests/<module>_test.cpp:
//
//   #include "test.hpp"
//   HDF_TEST(io_yaz0_literal_run) { HDF_CHECK(hdf::io::is_yaz0(...)); HDF_CHECK_EQ(a, b); }
//
// A test whose module is still a wave-1 stub calls HDF_SKIP("todo") instead of failing.
#pragma once

#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace hdf::test {

struct Case {
    const char* name;
    const char* file;
    void (*fn)();
};

std::vector<Case>& registry();

struct Reg {
    Reg(const char* name, const char* file, void (*fn)()) { registry().push_back(Case{name, file, fn}); }
};

void fail(const char* file, int line, const std::string& what);
void skip(const std::string& why);

// Runs the registered cases whose name contains `filter` (all when empty). Returns the number of failed cases.
int run(std::string_view filter, bool verbose);

template <class A, class B>
std::string describe_ne(const char* ea, const char* eb, const A& a, const B& b) {
    std::ostringstream s;
    s << ea << " == " << eb << " (got " << a << " vs " << b << ")";
    return s.str();
}

}  // namespace hdf::test

#define HDF_TEST(name)                                                         \
    static void hdf_test_##name();                                             \
    static ::hdf::test::Reg hdf_test_reg_##name(#name, __FILE__, &hdf_test_##name); \
    static void hdf_test_##name()

#define HDF_CHECK(cond)                                                        \
    do {                                                                       \
        if (!(cond)) ::hdf::test::fail(__FILE__, __LINE__, #cond);             \
    } while (0)

#define HDF_CHECK_EQ(a, b)                                                     \
    do {                                                                       \
        const auto& hdf_a_ = (a);                                              \
        const auto& hdf_b_ = (b);                                              \
        if (!(hdf_a_ == hdf_b_))                                               \
            ::hdf::test::fail(__FILE__, __LINE__, ::hdf::test::describe_ne(#a, #b, hdf_a_, hdf_b_)); \
    } while (0)

#define HDF_SKIP(why)                                                          \
    do {                                                                       \
        ::hdf::test::skip(why);                                                \
        return;                                                                \
    } while (0)
