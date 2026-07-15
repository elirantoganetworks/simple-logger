// Stdout output: default verbosity, "and lower" mode, "this level only" mode,
// and module selection. The real stdout (fd 1) is captured and asserted on.
// File output is turned off so only stdout is under test.

#include "doctest.h"
#include "test_util.h"

#include <slog/slog.h>
#include <slog/testing.h>

using namespace slog;

namespace {
Config stdout_only_config() {
    Config c;
    c.file_enabled = false;  // isolate stdout
    c.color        = "never";
    c.format       = "[%L]: %m";
    return c;
}
}  // namespace

TEST_CASE("stdout default keeps WARNING and ERROR, drops INFO and DEBUG") {
    tu::fresh();
    std::string out;
    {
        tu::CaptureStdout scap;
        configure(stdout_only_config());  // out.verbosity defaults to inherit (WARNING)
        LOG_ERROR("e");
        LOG_WARNING("w");
        LOG_INFO("i");
        LOG_DEBUG("d");
        out = scap.text();
    }
    auto lines = tu::split_lines(out);
    CHECK(lines.size() == 2);
    CHECK(tu::contains(lines, "[ERROR]: e"));
    CHECK(tu::contains(lines, "[WARNING]: w"));
    CHECK_FALSE(tu::contains(lines, "i"));
    CHECK_FALSE(tu::contains(lines, "d"));
}

TEST_CASE("and-lower mode: a WARNING threshold keeps ERROR too") {
    tu::fresh();
    std::string out;
    {
        tu::CaptureStdout scap;
        Config            c = stdout_only_config();
        c.out.verbosity     = WARNING;
        c.out.only          = false;  // "and lower" (the default)
        configure(c);
        LOG_ERROR("e");
        LOG_WARNING("w");
        LOG_INFO("i");
        out = scap.text();
    }
    auto lines = tu::split_lines(out);
    CHECK(lines.size() == 2);
    CHECK(tu::contains(lines, "e"));
    CHECK(tu::contains(lines, "w"));
    CHECK_FALSE(tu::contains(lines, "i"));
}

TEST_CASE("this-level-only mode: a WARNING threshold keeps only WARNING") {
    tu::fresh();
    std::string out;
    {
        tu::CaptureStdout scap;
        Config            c = stdout_only_config();
        c.out.verbosity     = WARNING;
        c.out.only          = true;  // exact level only
        configure(c);
        LOG_ERROR("e");
        LOG_WARNING("w");
        LOG_INFO("i");
        out = scap.text();
    }
    auto lines = tu::split_lines(out);
    CHECK(lines.size() == 1);
    CHECK(tu::contains(lines, "w"));
    CHECK_FALSE(tu::contains(lines, "e"));  // ERROR is not exactly WARNING
    CHECK_FALSE(tu::contains(lines, "i"));
}

TEST_CASE("module selection sends only chosen modules to stdout") {
    tu::fresh();
    std::string out;
    {
        tu::CaptureStdout scap;
        Config            c = stdout_only_config();
        c.verbosity         = DEBUG;  // collect everything
        c.out.verbosity     = DEBUG;
        c.out.modules       = {"net", "auth"};  // stdout only for these
        configure(c);
        LOG_TO("net", INFO, "net_line");
        LOG_TO("auth", INFO, "auth_line");
        LOG_TO("db", INFO, "db_line");  // not selected
        out = scap.text();
    }
    auto lines = tu::split_lines(out);
    CHECK(tu::contains(lines, "net_line"));
    CHECK(tu::contains(lines, "auth_line"));
    CHECK_FALSE(tu::contains(lines, "db_line"));
}

TEST_CASE("stdout can be turned off entirely") {
    tu::fresh();
    std::string out;
    {
        tu::CaptureStdout scap;
        Config            c = stdout_only_config();
        c.out.enabled       = false;
        configure(c);
        LOG_ERROR("e");
        out = scap.text();
    }
    CHECK(out.empty());
}
