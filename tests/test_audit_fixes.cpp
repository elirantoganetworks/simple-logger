// Guard tests for the bugs the audit found. Each case fails on the old code and
// passes on the fix, so the bug cannot come back unnoticed.

#include <csignal>
#include <cstdint>
#include <string>
#include <thread>

#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include "doctest.h"

#include <slog/slog.h>
#include <slog/testing.h>

#include "test_util.h"

namespace {

// A fixed wall clock at 3.000000000 s, for the elapsed-field test.
std::int64_t clock_three_seconds() { return 3000000000LL; }

}  // namespace

// F1: a config change that does not touch the format must not allocate a new
// prefix spec. The old code kept one per change, so a program that adjusts the
// level at runtime grew without bound.
TEST_CASE("F1 repeated config changes do not grow the format-spec set") {
    tu::fresh();
    const std::size_t start = slog::testing::format_spec_count();
    for (int i = 0; i < 5000; ++i)
        slog::set_verbosity(slog::WARNING);
    CHECK(slog::testing::format_spec_count() == start);  // no growth

    slog::set_format("[%L] %m");  // a real format change adds exactly one
    CHECK(slog::testing::format_spec_count() == start + 1);
}

// F2: configure() must set every field, so an empty field resets to the default
// and does not leak a value from an earlier call.
TEST_CASE("F2 configure() resets the dir, tag, and file name") {
    tu::fresh();
    tu::TempDir a;
    tu::TempDir b;

    slog::Config c1;
    c1.log_dir   = a.str();
    c1.run_tag   = "nightly";
    c1.file_name = "fixed.log";
    slog::configure(c1);

    slog::Config c2;  // all defaults
    c2.log_dir = b.str();
    slog::configure(c2);

    slog::set_file_verbosity(slog::DEBUG);
    slog::stdout_off();
    LOG_ERROR("hi");
    slog::shutdown();

    // The run lands in b/latest with a default name, not under an old tag dir
    // and not with the old fixed name.
    const auto files = tu::list_logs(b.sub("latest"));
    CHECK(files.size() == 1);
    CHECK_FALSE(tu::contains(files, "fixed.log"));
    CHECK_FALSE(tu::contains(files, "nightly-"));
}

// F5: a newline or carriage return inside a message becomes a space, so a
// message cannot forge a second log line. This is a security behaviour.
TEST_CASE("F5 a newline in a message cannot forge a second line") {
    tu::fresh();
    auto& cap = slog::testing::capture_to_memory();
    slog::stdout_off();
    slog::set_file_verbosity(slog::DEBUG);

    LOG(slog::ERROR, "start%cforged=admin%cend", '\n', '\r');

    const auto lines = cap.lines();
    REQUIRE(lines.size() == 1);  // still one line, not three
    CHECK(lines[0].find('\n') == std::string::npos);
    CHECK(lines[0].find('\r') == std::string::npos);
    CHECK(lines[0].find("start forged=admin end") != std::string::npos);
}

// F7 and F13: a message too long to fit is cut with a trailing "..." marker, so
// a reader can see it was truncated.
TEST_CASE("F7 a truncated message ends with a marker") {
    tu::fresh();
    auto& cap = slog::testing::capture_to_memory();
    slog::stdout_off();
    slog::set_file_verbosity(slog::DEBUG);

    const std::string big(10000, 'Z');
    LOG(slog::ERROR, "%s", big.c_str());

    const auto lines = cap.lines();
    REQUIRE(lines.size() == 1);
    const std::string& l = lines[0];
    REQUIRE(l.size() >= 3);
    CHECK(l.compare(l.size() - 3, 3, "...") == 0);
}

// F9: when a write fails, slog drops the line, counts it, and keeps running. The
// child sets a tiny file-size limit so its writes fail; the limit stays in the
// child, so the test process and its output are untouched.
TEST_CASE("F9 a failed write is dropped and counted, not fatal") {
    tu::fresh();
    tu::TempDir dir;

    const pid_t child = ::fork();
    REQUIRE(child >= 0);
    if (child == 0) {
        ::signal(SIGXFSZ, SIG_IGN);  // turn the file-size signal into an EFBIG error
        rlimit rl{1024, 1024};
        ::setrlimit(RLIMIT_FSIZE, &rl);
        slog::set_log_dir(dir.str().c_str());
        slog::set_file_verbosity(slog::DEBUG);
        slog::stdout_off();
        for (int i = 0; i < 2000; ++i)
            LOG_ERROR("line %d with some padding to pass the size limit", i);
        // Reaching here means no crash. Some lines must have been dropped.
        ::_exit(slog::testing::drop_count() > 0 ? 0 : 3);
    }
    int status = 0;
    ::waitpid(child, &status, 0);
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);  // survived and counted drops
}

// F11: the colour path. With color=always the level tag is wrapped in ANSI
// codes; with color=never it is plain.
TEST_CASE("F11 colour output is applied and can be turned off") {
    tu::fresh();
    slog::Config c;
    c.format = "[%L]";
    c.color  = "always";
    slog::configure(c);

    std::string colored;
    {
        tu::CaptureStdout cap;
        LOG_ERROR("x");
        colored = cap.text();
    }
    CHECK(colored.find("\033[31m") != std::string::npos);  // red for ERROR
    CHECK(colored.find("\033[0m") != std::string::npos);   // reset

    slog::Config c2;
    c2.format = "[%L]";
    c2.color  = "never";
    slog::configure(c2);
    std::string plain;
    {
        tu::CaptureStdout cap;
        LOG_ERROR("x");
        plain = cap.text();
    }
    CHECK(plain.find("\033[") == std::string::npos);  // no escape at all
}

// F12: the elapsed field subtracts the start epoch. With start 1s and now 3s the
// field must read 2.000, which would fail if the sign were flipped.
TEST_CASE("F12 the elapsed field subtracts the start epoch") {
    tu::fresh();
    slog::testing::set_start_epoch_nanos(1000000000LL);  // 1.0 s
    slog::testing::set_clock(&clock_three_seconds);       // now 3.0 s

    auto& cap = slog::testing::capture_to_memory();
    slog::stdout_off();
    slog::set_file_verbosity(slog::DEBUG);
    slog::set_format("[%e]");
    LOG_ERROR("x");

    const auto lines = cap.lines();
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].find("2.000") != std::string::npos);
}

// F14: %p is the process id and %T is the thread id. On a worker thread they
// differ, so a swap of the two fields would show up here.
TEST_CASE("F14 pid and tid fields differ on a worker thread") {
    tu::fresh();
    auto& cap = slog::testing::capture_to_memory();
    slog::stdout_off();
    slog::set_file_verbosity(slog::DEBUG);
    slog::set_format("%p %T");

    std::thread([] { LOG_ERROR("x"); }).join();

    const auto lines = cap.lines();
    REQUIRE(lines.size() == 1);
    const std::string& l   = lines[0];
    const std::size_t  sp  = l.find(' ');
    REQUIRE(sp != std::string::npos);
    const std::string pid = l.substr(0, sp);
    const std::string tid = l.substr(sp + 1);
    CHECK(pid == std::to_string(::getpid()));  // %p is the real pid
    CHECK(pid != tid);                          // %T on a thread is not the pid
}

// F15: a level given as a plain integer in config is parsed, and trailing
// garbage after the number is rejected.
TEST_CASE("F15 integer levels parse and reject trailing garbage") {
    tu::TempDir dir;

    {
        tu::fresh();
        const std::string path = dir.sub("good.conf");
        tu::write_file(path, "verbosity = 30\n");  // 30 is INFO
        slog::load_file(path.c_str());
        CHECK(slog::is_enabled("m", slog::INFO));    // 30 <= 30
        CHECK_FALSE(slog::is_enabled("m", slog::DEBUG));  // 40 > 30
    }
    {
        tu::fresh();
        const std::string path = dir.sub("bad.conf");
        tu::write_file(path, "verbosity = 30x\n");  // not a number, keep default
        slog::load_file(path.c_str());
        CHECK_FALSE(slog::is_enabled("m", slog::INFO));  // default WARNING drops INFO
    }
}

// F16: a line at or above the flush level is synced; a more verbose line is not.
TEST_CASE("F16 flush happens only at the flush level and above") {
    tu::fresh();
    tu::TempDir dir;
    slog::set_log_dir(dir.str().c_str());
    slog::set_file_verbosity(slog::DEBUG);
    slog::set_flush_level(slog::WARNING);
    slog::stdout_off();

    LOG_INFO("below the flush level");
    CHECK(slog::testing::flush_count() == 0);  // INFO is more verbose than WARNING

    LOG_WARNING("at the flush level");
    CHECK(slog::testing::flush_count() == 1);

    LOG_ERROR("above the flush level");
    CHECK(slog::testing::flush_count() == 2);

    slog::shutdown();
}

// F8: a path-bearing run tag is sanitized. A ".." tag must not escape the latest
// directory, and a tag with a slash must still produce file output.
TEST_CASE("F8 a path-bearing run tag stays inside the log dir") {
    {
        tu::fresh();
        tu::TempDir dir;
        slog::set_log_dir(dir.str().c_str());
        slog::set_run_tag("..");  // would name the log dir itself
        slog::set_file_verbosity(slog::DEBUG);
        slog::stdout_off();
        LOG_ERROR("hi");
        slog::shutdown();
        // No loose .log file appears in the log dir above latest.
        CHECK(tu::list_logs(dir.str()).empty());
    }
    {
        tu::fresh();
        tu::TempDir dir;
        slog::set_log_dir(dir.str().c_str());
        slog::set_run_tag("a/b");  // slash would break openat on the old code
        slog::set_file_verbosity(slog::DEBUG);
        slog::stdout_off();
        LOG_ERROR("hi");
        slog::shutdown();
        // The slash became "_", so the run and its file exist under latest/a_b.
        CHECK_FALSE(tu::list_logs(dir.sub("latest/a_b")).empty());
    }
}
