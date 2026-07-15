// Configuration: the three sources (file, env, API), the precedence between
// them, and the error policy for bad values, unknown keys, empty values, and a
// missing file. Also the before-init vs after-init timing for dir settings.

#include "doctest.h"
#include "test_util.h"

#include <slog/slog.h>
#include <slog/testing.h>

#include <cstdlib>
#include <string>

#include <sys/stat.h>

using namespace slog;

namespace {
// The highest built-in level the default module admits. With file and stdout
// left inheriting, this equals the effective global level.
int effective_global() {
    if (is_enabled("", DEBUG))
        return 40;
    if (is_enabled("", INFO))
        return 30;
    if (is_enabled("", WARNING))
        return 20;
    if (is_enabled("", ERROR))
        return 10;
    return 0;
}

bool path_exists(const std::string& p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0;
}
}  // namespace

TEST_CASE("config file sets values") {
    tu::fresh();
    tu::TempDir       dir;
    const std::string conf = dir.sub("slog.conf");
    tu::write_file(conf, "verbosity = debug\n");
    load_file(conf.c_str());
    CHECK(effective_global() == 40);
}

TEST_CASE("env var sets values, including the per-module list grammar") {
    tu::fresh();
    ::setenv("SLOG_VERBOSITY", "warning,net=debug", 1);
    load_env();
    ::unsetenv("SLOG_VERBOSITY");

    CHECK(effective_global() == 20);       // global from the bare token
    CHECK(is_enabled("net", DEBUG));       // module override from the list
    CHECK_FALSE(is_enabled("db", DEBUG));  // other modules stay at global
}

TEST_CASE("API setter sets values") {
    tu::fresh();
    set_verbosity(INFO);
    CHECK(effective_global() == 30);
}

TEST_CASE("precedence is file, then env, then API - later wins") {
    tu::fresh();
    tu::TempDir       dir;
    const std::string conf = dir.sub("slog.conf");
    tu::write_file(conf, "verbosity = error\n");

    load_file(conf.c_str());
    CHECK(effective_global() == 10);  // file alone: error

    ::setenv("SLOG_VERBOSITY", "info", 1);
    load_env();
    CHECK(effective_global() == 30);  // env beats file: info

    set_verbosity(DEBUG);
    CHECK(effective_global() == 40);  // API beats env: debug
    ::unsetenv("SLOG_VERBOSITY");
}

TEST_CASE("env beats file even when the file is loaded after") {
    // Precedence is by source, not by load order: the merge always puts env over
    // file. Loading the file last must not flip the winner.
    tu::fresh();
    tu::TempDir       dir;
    const std::string conf = dir.sub("slog.conf");
    tu::write_file(conf, "verbosity = error\n");
    ::setenv("SLOG_VERBOSITY", "info", 1);
    load_env();
    load_file(conf.c_str());
    ::unsetenv("SLOG_VERBOSITY");
    CHECK(effective_global() == 30);  // env still wins
}

TEST_CASE("a bad level value keeps the default and prints a note") {
    tu::fresh();
    tu::TempDir       dir;
    const std::string conf = dir.sub("slog.conf");
    tu::write_file(conf, "verbosity = bogus\n");
    std::string err;
    {
        tu::CaptureStderr cap;
        load_file(conf.c_str());
        err = cap.text();
    }
    CHECK(effective_global() == 20);  // unchanged default WARNING
    CHECK(err.find("slog:") != std::string::npos);
    CHECK(err.find("bogus") != std::string::npos);
}

TEST_CASE("a bad boolean value keeps the default and prints a note") {
    tu::fresh();
    tu::TempDir       dir;
    const std::string conf = dir.sub("slog.conf");
    tu::write_file(conf, "disable = ture\n");  // typo
    std::string err;
    {
        tu::CaptureStderr cap;
        load_file(conf.c_str());
        err = cap.text();
    }
    CHECK(is_enabled("", ERROR));  // logging still on
    CHECK(err.find("slog:") != std::string::npos);       // a note reached stderr
    CHECK(err.find("ture") != std::string::npos);         // it names the bad value
    CHECK(static_cast<int>(last_error().code) ==
          static_cast<int>(errc::config_bad_value));      // and set the sticky code
}

TEST_CASE("an unknown key is ignored with a note") {
    tu::fresh();
    tu::TempDir       dir;
    const std::string conf = dir.sub("slog.conf");
    tu::write_file(conf, "verbosity = debug\nmystery_key = 5\n");
    std::string err;
    {
        tu::CaptureStderr cap;
        load_file(conf.c_str());
        err = cap.text();
    }
    CHECK(effective_global() == 40);  // the good key still applied
    CHECK(err.find("unknown config key") != std::string::npos);
}

TEST_CASE("comments and the %# token survive, blank and bad lines are skipped") {
    tu::fresh();
    tu::TempDir       dir;
    const std::string conf = dir.sub("slog.conf");
    tu::write_file(conf,
                   "# a comment line\n"
                   "\n"
                   "verbosity = debug   # trailing comment\n"
                   "format = [%L][%#]: %m\n"
                   "a line with no equals\n");
    auto& cap = testing::capture_to_memory();
    stdout_off();
    load_file(conf.c_str());
    CHECK(effective_global() == 40);  // trailing comment did not corrupt the value
    LOG_ERROR("x");
    // The %# token (line number) was not eaten as a comment.
    REQUIRE(cap.lines().size() == 1);
    CHECK(cap.lines()[0].rfind("[ERROR][", 0) == 0);
    CHECK(cap.lines()[0].find("]: x") != std::string::npos);
}

TEST_CASE("a missing config file is not an error") {
    tu::fresh();
    load_file("/definitely/not/here.conf");
    CHECK(effective_global() == 20);  // stays at the default
}

TEST_CASE("an empty value does not crash and keeps the default") {
    tu::fresh();
    tu::TempDir       dir;
    const std::string conf = dir.sub("slog.conf");
    tu::write_file(conf, "verbosity =\ntag =\n");
    load_file(conf.c_str());
    CHECK(effective_global() == 20);
}

TEST_CASE("configure applies a whole Config at once") {
    tu::fresh();
    Config c;
    c.verbosity               = INFO;
    c.module_verbosity["net"] = DEBUG.value;
    configure(c);
    CHECK(effective_global() == 30);
    CHECK(is_enabled("net", DEBUG));
}

TEST_CASE("dir set before the first log takes effect") {
    tu::fresh();
    tu::TempDir dir;
    set_log_dir(dir.str().c_str());
    stdout_off();
    LOG_ERROR("into the chosen dir");
    flush();
    CHECK(path_exists(dir.sub("latest")));
    CHECK(tu::list_logs(dir.sub("latest")).size() == 1);
}

TEST_CASE("dir set after init does not move the already-open run") {
    tu::fresh();
    tu::TempDir a;
    tu::TempDir b;
    set_log_dir(a.str().c_str());
    stdout_off();
    init();                        // opens the run in a
    set_log_dir(b.str().c_str());  // too late for this run
    LOG_ERROR("still in a");
    flush();

    CHECK(tu::list_logs(a.sub("latest")).size() == 1);  // a got the log
    CHECK_FALSE(path_exists(b.sub("latest")));          // b was never opened
}
