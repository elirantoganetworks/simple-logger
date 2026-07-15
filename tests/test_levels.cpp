// Level correctness: the order, the boundary cases, per-module override, and
// custom levels next to the built-in ones. All checked through memory capture.

#include "doctest.h"
#include "test_util.h"

#include <slog/slog.h>
#include <slog/testing.h>

using namespace slog;

namespace {
// Log one message at each built-in level to the default module.
void log_all_builtins() {
    LOG_ERROR("m_error");
    LOG_WARNING("m_warning");
    LOG_INFO("m_info");
    LOG_DEBUG("m_debug");
}
}  // namespace

TEST_CASE("WARNING threshold emits WARNING and ERROR, drops INFO and DEBUG") {
    tu::fresh();
    auto& cap = testing::capture_to_memory();
    stdout_off();
    set_verbosity(WARNING);

    log_all_builtins();
    auto lines = cap.lines();

    CHECK(lines.size() == 2);
    CHECK(tu::contains(lines, "m_error"));
    CHECK(tu::contains(lines, "m_warning"));
    CHECK_FALSE(tu::contains(lines, "m_info"));
    CHECK_FALSE(tu::contains(lines, "m_debug"));
}

TEST_CASE("each threshold admits itself and every more severe level") {
    struct Case {
        Level       t;
        std::size_t expected;  // how many of the 4 built-ins pass
    };
    // ERROR admits 1, WARNING 2, INFO 3, DEBUG 4.
    const Case cases[] = {{ERROR, 1}, {WARNING, 2}, {INFO, 3}, {DEBUG, 4}};
    for (const Case& c : cases) {
        tu::fresh();
        auto& cap = testing::capture_to_memory();
        stdout_off();
        set_verbosity(c.t);
        log_all_builtins();
        CAPTURE(std::string(c.t.name));
        CHECK(cap.lines().size() == c.expected);
    }
}

TEST_CASE("ERROR is the lowest verbosity and always survives") {
    tu::fresh();
    auto& cap = testing::capture_to_memory();
    stdout_off();
    set_verbosity(ERROR);
    log_all_builtins();
    auto lines = cap.lines();
    CHECK(lines.size() == 1);
    CHECK(tu::contains(lines, "m_error"));
}

TEST_CASE("per-module verbosity beats the global default") {
    tu::fresh();
    auto& cap = testing::capture_to_memory();
    stdout_off();
    set_verbosity(WARNING);
    set_module_verbosity("net", DEBUG);  // net is verbose, others follow global

    LOG_TO("net", DEBUG, "net_debug");
    LOG_TO("db", DEBUG, "db_debug");

    auto lines = cap.lines();
    CHECK(tu::contains(lines, "net_debug"));       // net raised to DEBUG
    CHECK_FALSE(tu::contains(lines, "db_debug"));  // db still at global WARNING
}

TEST_CASE("a module can be made quieter than the global default too") {
    tu::fresh();
    auto& cap = testing::capture_to_memory();
    stdout_off();
    set_verbosity(DEBUG);                  // global shows everything
    set_module_verbosity("quiet", ERROR);  // but this module only ERROR

    LOG_TO("quiet", WARNING, "q_warning");
    LOG_TO("quiet", ERROR, "q_error");
    LOG_TO("loud", WARNING, "l_warning");

    auto lines = cap.lines();
    CHECK_FALSE(tu::contains(lines, "q_warning"));
    CHECK(tu::contains(lines, "q_error"));
    CHECK(tu::contains(lines, "l_warning"));
}

TEST_CASE("custom level placed above DEBUG is the most verbose") {
    tu::fresh();
    Level trace = add_level("TRACE", 50);  // above DEBUG (40)
    auto& cap   = testing::capture_to_memory();
    stdout_off();

    SUBCASE("dropped when the threshold is DEBUG") {
        set_verbosity(DEBUG);
        LOG(trace, "trace_line");
        CHECK_FALSE(tu::contains(cap.lines(), "trace_line"));
    }
    SUBCASE("shown when the threshold is TRACE") {
        set_verbosity(trace);
        LOG(trace, "trace_line");
        LOG_DEBUG("debug_line");
        auto lines = cap.lines();
        CHECK(tu::contains(lines, "trace_line"));
        CHECK(tu::contains(lines, "debug_line"));  // built-ins still work
    }
}

TEST_CASE("custom level placed between WARNING and INFO filters and prints right") {
    tu::fresh();
    Level notice = add_level("NOTICE", 25);  // between WARNING(20) and INFO(30)
    auto& cap    = testing::capture_to_memory();
    stdout_off();

    SUBCASE("dropped at a WARNING threshold") {
        set_verbosity(WARNING);
        LOG(notice, "notice_line");
        CHECK_FALSE(tu::contains(cap.lines(), "notice_line"));
    }
    SUBCASE("shown at an INFO threshold, and printed with its name") {
        set_verbosity(INFO);
        set_format("[%L]: %m");
        LOG(notice, "notice_line");
        auto lines = cap.lines();
        REQUIRE(lines.size() == 1);
        CHECK(lines[0] == "[NOTICE]: notice_line");  // name printed, not a number
    }
}

TEST_CASE("a custom level used as a per-module threshold works") {
    tu::fresh();
    Level notice = add_level("NOTICE2", 25);
    auto& cap    = testing::capture_to_memory();
    stdout_off();
    set_verbosity(WARNING);
    set_module_verbosity("svc", notice);  // svc admits NOTICE2 and more severe

    LOG_TO("svc", notice, "svc_notice");
    LOG_TO("svc", INFO, "svc_info");  // INFO(30) is more verbose than NOTICE2(25)

    auto lines = cap.lines();
    CHECK(tu::contains(lines, "svc_notice"));
    CHECK_FALSE(tu::contains(lines, "svc_info"));
}
