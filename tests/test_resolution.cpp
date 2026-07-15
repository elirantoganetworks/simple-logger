// The combine rule for the global, per-module, file and stdout thresholds, as
// stated in DESIGN.md 4.11. The rule: an output uses its own level if set, else
// the per-module level if set, else the global level. File and stdout are
// independent, so an output can be more or less verbose than the global default.
//
// File output is checked through memory capture; stdout through a real fd-1
// capture. Both are checked in the same run so their independence is proven.

#include "doctest.h"
#include "test_util.h"

#include <slog/slog.h>
#include <slog/testing.h>

using namespace slog;

namespace {
// Build a config with color off, so captured stdout has no ANSI codes.
Config base() {
    Config c;
    c.color  = "never";
    c.format = "[%L]: %m";
    return c;
}
}  // namespace

TEST_CASE("default: global WARNING gates both file and stdout at WARNING") {
    tu::fresh();
    auto&       mcap = testing::capture_to_memory();  // file -> memory
    std::string out;
    {
        tu::CaptureStdout scap;
        Config            c = base();  // verbosity default WARNING, both on, inherit
        configure(c);
        LOG_WARNING("w");
        LOG_INFO("i");
        out = scap.text();
    }
    auto file = mcap.lines();
    CHECK(tu::contains(file, "w"));
    CHECK_FALSE(tu::contains(file, "i"));
    CHECK(out.find("w") != std::string::npos);
    CHECK(out.find("i") == std::string::npos);
}

TEST_CASE("file DEBUG shows DEBUG while stdout stays at the global WARNING") {
    tu::fresh();
    auto&       mcap = testing::capture_to_memory();
    std::string out;
    {
        tu::CaptureStdout scap;
        Config            c = base();
        c.verbosity         = WARNING;  // global default
        c.file_verbosity    = DEBUG;    // file explicitly verbose
        // stdout left at INHERIT, so it follows the global WARNING
        configure(c);
        LOG_DEBUG("d");
        LOG_WARNING("w");
        out = scap.text();
    }
    auto file = mcap.lines();
    CHECK(tu::contains(file, "d"));  // DEBUG reaches the file
    CHECK(tu::contains(file, "w"));
    CHECK(out.find("d") == std::string::npos);  // but not stdout
    CHECK(out.find("w") != std::string::npos);
}

TEST_CASE("stdout can be more verbose than the file") {
    tu::fresh();
    auto&       mcap = testing::capture_to_memory();
    std::string out;
    {
        tu::CaptureStdout scap;
        Config            c = base();
        c.verbosity         = ERROR;  // global
        c.file_verbosity    = ERROR;  // file only ERROR
        c.out.verbosity     = DEBUG;  // stdout everything
        configure(c);
        LOG_INFO("i");
        LOG_ERROR("e");
        out = scap.text();
    }
    auto file = mcap.lines();
    CHECK_FALSE(tu::contains(file, "i"));  // file dropped INFO
    CHECK(tu::contains(file, "e"));
    CHECK(out.find("i") != std::string::npos);  // stdout kept INFO
    CHECK(out.find("e") != std::string::npos);
}

TEST_CASE("a per-module level reaches every output that did not set its own") {
    tu::fresh();
    auto&       mcap = testing::capture_to_memory();
    std::string out;
    {
        tu::CaptureStdout scap;
        Config            c       = base();
        c.verbosity               = WARNING;
        c.module_verbosity["net"] = DEBUG.value;  // net is verbose
        // file and stdout both inherit, so both see net at DEBUG
        configure(c);
        LOG_TO("net", DEBUG, "n");
        LOG_TO("db", DEBUG, "x");
        out = scap.text();
    }
    auto file = mcap.lines();
    CHECK(tu::contains(file, "n"));
    CHECK_FALSE(tu::contains(file, "x"));
    CHECK(out.find("n") != std::string::npos);
    CHECK(out.find("x") == std::string::npos);
}

TEST_CASE("an explicit output level beats a per-module level for that output") {
    tu::fresh();
    auto&       mcap = testing::capture_to_memory();
    std::string out;
    {
        tu::CaptureStdout scap;
        Config            c       = base();
        c.verbosity               = WARNING;
        c.module_verbosity["net"] = DEBUG.value;  // module wants DEBUG
        c.file_verbosity          = WARNING;      // but file is pinned at WARNING
        // stdout inherits, so stdout still honors the module DEBUG
        configure(c);
        LOG_TO("net", DEBUG, "nd");
        out = scap.text();
    }
    auto file = mcap.lines();
    CHECK_FALSE(tu::contains(file, "nd"));       // file pinned WARNING wins
    CHECK(out.find("nd") != std::string::npos);  // stdout inherits module DEBUG
}

TEST_CASE("a record that passes no output is collected by none") {
    tu::fresh();
    auto&       mcap = testing::capture_to_memory();
    std::string out;
    {
        tu::CaptureStdout scap;
        Config            c = base();
        c.verbosity         = WARNING;
        c.file_verbosity    = ERROR;  // file only ERROR
        c.out.verbosity     = ERROR;  // stdout only ERROR
        configure(c);
        LOG_WARNING("w");  // passes neither
        out = scap.text();
    }
    CHECK(mcap.lines().empty());
    CHECK(out.empty());
    CHECK_FALSE(is_enabled("", WARNING));  // and the guard agrees
    CHECK(is_enabled("", ERROR));
}
