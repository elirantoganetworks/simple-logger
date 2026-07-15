// Rate limits. Each macro keeps a per-call-site counter, so every case keeps its
// macro at a single source line and is its own TEST_CASE (no SUBCASE, which
// would re-enter the body and reuse the static counter).

#include "doctest.h"
#include "test_util.h"

#include <slog/slog.h>
#include <slog/testing.h>

#include <cstdint>

using namespace slog;

namespace {
std::int64_t g_mono = 0;
std::int64_t mono_fn() {
    return g_mono;
}
}  // namespace

TEST_CASE("LOG_EVERY_N fires on the 1st, N+1th, 2N+1th, ... call") {
    tu::fresh();
    auto& cap = testing::capture_to_memory();
    stdout_off();
    set_verbosity(WARNING);
    for (int i = 0; i < 10; ++i) LOG_EVERY_N(WARNING, 3, "e%d", i);  // i=0,3,6,9
    CHECK(cap.lines().size() == 4);
}

TEST_CASE("LOG_FIRST_N fires only for the first N calls") {
    tu::fresh();
    auto& cap = testing::capture_to_memory();
    stdout_off();
    set_verbosity(WARNING);
    for (int i = 0; i < 10; ++i) LOG_FIRST_N(WARNING, 3, "f%d", i);  // i=0,1,2
    CHECK(cap.lines().size() == 3);
}

TEST_CASE("LOG_ONCE fires exactly once") {
    tu::fresh();
    auto& cap = testing::capture_to_memory();
    stdout_off();
    set_verbosity(WARNING);
    for (int i = 0; i < 5; ++i) LOG_ONCE(WARNING, "o%d", i);
    CHECK(cap.lines().size() == 1);
}

TEST_CASE("LOG_EVERY_N_SEC honors the time window at one call site") {
    tu::fresh();
    testing::set_mono_clock(&mono_fn);
    auto& cap = testing::capture_to_memory();
    stdout_off();
    set_verbosity(WARNING);
    // Advance the clock one second per step, with a 2-second window. The first
    // call always fires; then it fires at 2s, 4s, 6s, 8s -> 5 total.
    for (int sec = 0; sec < 10; ++sec) {
        g_mono = static_cast<std::int64_t>(sec) * 1000000000LL;
        LOG_EVERY_N_SEC(WARNING, 2.0, "t%d", sec);
    }
    CHECK(cap.lines().size() == 5);
}

TEST_CASE("a rate-limited call still respects the level filter") {
    tu::fresh();
    auto& cap = testing::capture_to_memory();
    stdout_off();
    set_verbosity(ERROR);  // WARNING is filtered out
    for (int i = 0; i < 10; ++i) LOG_EVERY_N(WARNING, 1, "w%d", i);
    CHECK(cap.lines().empty());  // the level gate still applies
}
