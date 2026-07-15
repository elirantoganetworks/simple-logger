// The prefix: the default shape, custom shapes, the real function name, the time
// field, and the "seconds since start" field including the shape of the number.

#include "doctest.h"
#include "test_util.h"

#include <slog/slog.h>
#include <slog/testing.h>

#include <cstdlib>
#include <ctime>
#include <string>

#include <unistd.h>

using namespace slog;

namespace {
// A named function, so we can assert the %f field is the real name.
void carefully_named_function() {
    LOG_ERROR("hello");
}

// Fixed clocks for the time fields.
std::int64_t clock_1234() {
    // 12:34:56.789 UTC on the epoch day.
    const std::int64_t secs = (12 * 3600 + 34 * 60 + 56);
    return secs * 1000000000LL + 789000000LL;
}
std::int64_t clock_1500ms() {
    return 1500000000LL;
}
std::int64_t clock_2345ms() {
    return 2345000000LL;
}
std::int64_t clock_0500ms() {
    return 500000000LL;
}
}  // namespace

TEST_CASE("default prefix is [VERBOSITY][FUNC]: MSG") {
    tu::fresh();
    auto& cap = testing::capture_to_memory();
    stdout_off();
    carefully_named_function();
    auto lines = cap.lines();
    REQUIRE(lines.size() == 1);
    CHECK(lines[0] == "[ERROR][carefully_named_function]: hello");
}

TEST_CASE("the function field is the real enclosing function name") {
    tu::fresh();
    auto& cap = testing::capture_to_memory();
    stdout_off();
    set_format("[%f]");
    carefully_named_function();
    auto lines = cap.lines();
    REQUIRE(lines.size() == 1);
    CHECK(lines[0] == "[carefully_named_function]");
}

TEST_CASE("custom prefix with module, pid, file and line") {
    tu::fresh();
    auto& cap = testing::capture_to_memory();
    stdout_off();
    set_format("[%L][%M][%p]: %m");
    LOG_TO("netmod", ERROR, "body");
    auto lines = cap.lines();
    REQUIRE(lines.size() == 1);
    const std::string expect =
        "[ERROR][netmod][" + std::to_string(::getpid()) + "]: body";
    CHECK(lines[0] == expect);
}

TEST_CASE("default module is shown as 'default'") {
    tu::fresh();
    auto& cap = testing::capture_to_memory();
    stdout_off();
    set_format("[%M]");
    LOG_ERROR("x");
    REQUIRE(cap.lines().size() == 1);
    CHECK(cap.lines()[0] == "[default]");
}

TEST_CASE("literal percent and unknown tokens are kept as text") {
    tu::fresh();
    auto& cap = testing::capture_to_memory();
    stdout_off();
    set_format("100%% %z %m");  // %% -> %, %z is not a token
    LOG_ERROR("done");
    REQUIRE(cap.lines().size() == 1);
    CHECK(cap.lines()[0] == "100% %z done");
}

TEST_CASE("time field prints as HH:MM:SS.mmm") {
    tu::fresh();
    ::setenv("TZ", "UTC0", 1);
    ::tzset();
    testing::set_clock(&clock_1234);
    auto& cap = testing::capture_to_memory();
    stdout_off();
    set_format("[%t]");
    LOG_ERROR("x");
    REQUIRE(cap.lines().size() == 1);
    CHECK(cap.lines()[0] == "[12:34:56.789]");
}

TEST_CASE("elapsed field prints seconds since start as x.xxx") {
    struct Case {
        std::int64_t (*clock)();
        const char* expect;
    };
    const Case cases[] = {
        {&clock_1500ms, "[1.500]"},
        {&clock_2345ms, "[2.345]"},
        {&clock_0500ms, "[0.500]"},
    };
    for (const Case& c : cases) {
        tu::fresh();
        testing::set_clock(c.clock);
        testing::set_start_epoch_nanos(0);  // zero point
        auto& cap = testing::capture_to_memory();
        stdout_off();
        set_format("[%e]");
        LOG_ERROR("x");
        REQUIRE(cap.lines().size() == 1);
        CAPTURE(c.expect);
        CHECK(cap.lines()[0] == c.expect);
    }
}

TEST_CASE("the time toggle is off by default and prepends the field when on") {
    tu::fresh();
    ::setenv("TZ", "UTC0", 1);
    ::tzset();
    testing::set_clock(&clock_1234);

    SUBCASE("default: no time field, prefix starts with the level") {
        auto& cap = testing::capture_to_memory();
        stdout_off();
        LOG_ERROR("plain");
        REQUIRE(cap.lines().size() == 1);
        CHECK(cap.lines()[0].rfind("[ERROR]", 0) == 0);  // starts with [ERROR]
        CHECK(cap.lines()[0].find("12:34:56") == std::string::npos);
    }
    SUBCASE("enable_time: the time is prepended to the prefix") {
        enable_time(true);
        auto& cap = testing::capture_to_memory();
        stdout_off();
        LOG_ERROR("plain");
        REQUIRE(cap.lines().size() == 1);
        CHECK(cap.lines()[0].rfind("12:34:56.789 ", 0) == 0);  // time first
        CHECK(cap.lines()[0].find("[ERROR]") != std::string::npos);
    }
}

TEST_CASE("the elapsed toggle is off by default and prepends the field when on") {
    tu::fresh();
    testing::set_clock(&clock_1500ms);
    testing::set_start_epoch_nanos(0);

    SUBCASE("default: no elapsed field") {
        auto& cap = testing::capture_to_memory();
        stdout_off();
        LOG_ERROR("plain");
        REQUIRE(cap.lines().size() == 1);
        CHECK(cap.lines()[0].find("1.500") == std::string::npos);
    }
    SUBCASE("enable_elapsed: the elapsed seconds are prepended") {
        enable_elapsed(true);
        auto& cap = testing::capture_to_memory();
        stdout_off();
        LOG_ERROR("plain");
        REQUIRE(cap.lines().size() == 1);
        CHECK(cap.lines()[0].rfind("1.500 ", 0) == 0);
    }
}
