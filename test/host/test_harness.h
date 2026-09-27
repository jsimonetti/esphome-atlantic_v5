// Minimal assert-based test harness for L1 core tests: no
// GoogleTest dependency, no ESP headers, no exceptions.
#pragma once

#include <cstdio>

namespace atlantic_v5::test {
inline int g_failures = 0;
}  // namespace atlantic_v5::test

#define CHECK(cond)                                                                 \
  do {                                                                              \
    if (!(cond)) {                                                                 \
      std::fprintf(stderr, "CHECK FAILED: %s at %s:%d\n", #cond, __FILE__, __LINE__); \
      ::atlantic_v5::test::g_failures++;                                           \
    }                                                                               \
  } while (0)

#define TEST_MAIN_RETURN()                                                 \
  do {                                                                     \
    if (::atlantic_v5::test::g_failures > 0) {                            \
      std::fprintf(stderr, "%d check(s) failed\n", ::atlantic_v5::test::g_failures); \
      return 1;                                                            \
    }                                                                      \
    std::printf("all checks passed\n");                                   \
    return 0;                                                             \
  } while (0)
