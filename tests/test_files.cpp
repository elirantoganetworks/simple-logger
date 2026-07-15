// File and directory behavior for a single run: the default file name shape, a
// custom name, file output off, the file's own verbosity, single file vs one
// file per module, default and custom dirs, a missing dir (auto-created), and a
// dir that cannot be created (degrade, do not crash).
//
// Multi-run behavior (moving out of latest, tags, the race) lives in
// test_multiproc.cpp, since real runs are separate processes.

#include "doctest.h"
#include "test_util.h"

#include <slog/slog.h>
#include <slog/testing.h>

#include <cctype>
#include <string>

#include <sys/stat.h>
#include <unistd.h>

using namespace slog;

namespace {
bool path_exists(const std::string& p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0;
}

bool all_digits(const std::string& s) {
    if (s.empty())
        return false;
    for (char c : s)
        if (!std::isdigit(static_cast<unsigned char>(c)))
            return false;
    return true;
}

// Does name look like "<pid>-YYYYMMDD-HHMMSS.log"?
bool is_default_name(const std::string& name, int pid) {
    const std::string prefix = std::to_string(pid) + "-";
    if (name.rfind(prefix, 0) != 0)
        return false;
    if (name.size() < 4 || name.compare(name.size() - 4, 4, ".log") != 0)
        return false;
    const std::string mid = name.substr(prefix.size(), name.size() - prefix.size() - 4);
    // mid should be YYYYMMDD-HHMMSS: 8 digits, dash, 6 digits.
    if (mid.size() != 15 || mid[8] != '-')
        return false;
    return all_digits(mid.substr(0, 8)) && all_digits(mid.substr(9));
}
}  // namespace

TEST_CASE("default file name is <PID>-<TIMESTAMP>.log") {
    tu::fresh();
    tu::TempDir dir;
    set_log_dir(dir.str().c_str());
    stdout_off();
    LOG_ERROR("x");
    flush();
    auto files = tu::list_logs(dir.sub("latest"));
    REQUIRE(files.size() == 1);
    CHECK(is_default_name(files[0], static_cast<int>(::getpid())));
}

TEST_CASE("a custom file name is used") {
    tu::fresh();
    tu::TempDir dir;
    set_log_dir(dir.str().c_str());
    set_file_name("service.log");
    stdout_off();
    LOG_ERROR("x");
    flush();
    auto files = tu::list_logs(dir.sub("latest"));
    REQUIRE(files.size() == 1);
    CHECK(files[0] == "service.log");
}

TEST_CASE("file output can be turned off") {
    tu::fresh();
    tu::TempDir dir;
    set_log_dir(dir.str().c_str());
    file_off();
    stdout_off();
    LOG_ERROR("x");
    flush();
    CHECK_FALSE(path_exists(dir.sub("latest")));  // no run dir, no files
}

TEST_CASE("the file has its own verbosity, apart from stdout") {
    tu::fresh();
    tu::TempDir dir;
    set_log_dir(dir.str().c_str());
    stdout_off();
    set_verbosity(WARNING);
    set_file_verbosity(DEBUG);  // file keeps everything
    set_format("[%L]: %m");
    LOG_DEBUG("d");
    LOG_WARNING("w");
    flush();
    auto files = tu::list_logs(dir.sub("latest"));
    REQUIRE(files.size() == 1);
    auto lines = tu::split_lines(tu::read_file(dir.sub("latest/" + files[0])));
    CHECK(tu::contains(lines, "[DEBUG]: d"));
    CHECK(tu::contains(lines, "[WARNING]: w"));
}

TEST_CASE("single file holds every module") {
    tu::fresh();
    tu::TempDir dir;
    set_log_dir(dir.str().c_str());
    stdout_off();
    set_verbosity(DEBUG);
    set_format("[%M]: %m");
    LOG_TO("net", INFO, "from_net");
    LOG_TO("db", INFO, "from_db");
    flush();
    auto files = tu::list_logs(dir.sub("latest"));
    REQUIRE(files.size() == 1);
    auto lines = tu::split_lines(tu::read_file(dir.sub("latest/" + files[0])));
    CHECK(tu::contains(lines, "[net]: from_net"));
    CHECK(tu::contains(lines, "[db]: from_db"));
}

TEST_CASE("one file per module uses <PID>-<TIMESTAMP>-<MODULE>.log") {
    tu::fresh();
    tu::TempDir dir;
    set_log_dir(dir.str().c_str());
    stdout_off();
    set_verbosity(DEBUG);
    set_file_per_module(true);
    set_format("[%M]: %m");
    LOG_TO("net", INFO, "from_net");
    LOG_TO("db", INFO, "from_db");
    flush();

    auto files = tu::list_logs(dir.sub("latest"));
    REQUIRE(files.size() == 2);
    const std::string pid    = std::to_string(::getpid());
    bool              net_ok = false, db_ok = false;
    for (const auto& f : files) {
        CHECK(f.rfind(pid + "-", 0) == 0);
        auto lines = tu::split_lines(tu::read_file(dir.sub("latest/" + f)));
        if (f.find("-net.log") != std::string::npos) {
            net_ok = tu::contains(lines, "from_net") && !tu::contains(lines, "from_db");
        } else if (f.find("-db.log") != std::string::npos) {
            db_ok = tu::contains(lines, "from_db") && !tu::contains(lines, "from_net");
        }
    }
    CHECK(net_ok);
    CHECK(db_ok);
}

TEST_CASE("default dir is <cwd>/logs") {
    tu::fresh();
    tu::TempDir dir;
    char        saved[4096];
    REQUIRE(::getcwd(saved, sizeof(saved)) != nullptr);
    REQUIRE(::chdir(dir.str().c_str()) == 0);

    stdout_off();
    LOG_ERROR("x");  // no dir set: uses ./logs
    flush();
    const bool ok = path_exists(dir.sub("logs/latest"));

    REQUIRE(::chdir(saved) == 0);  // restore cwd before asserting
    CHECK(ok);
}

TEST_CASE("a dir that does not exist yet is created") {
    tu::fresh();
    tu::TempDir       dir;
    const std::string deep = dir.sub("a/b/c");
    set_log_dir(deep.c_str());
    stdout_off();
    LOG_ERROR("x");
    flush();
    CHECK(path_exists(deep + "/latest"));
    CHECK(tu::list_logs(deep + "/latest").size() == 1);
}

TEST_CASE("a dir that cannot be created degrades to no file output") {
    tu::fresh();
    tu::TempDir dir;
    // Put a regular file where a dir would need to be, so mkdir fails with
    // ENOTDIR. This fails even for root, unlike a permission bit.
    const std::string blocker = dir.sub("blocker");
    tu::write_file(blocker, "x");
    set_log_dir((blocker + "/logs").c_str());

    std::string err;
    {
        tu::CaptureStderr cap;
        stdout_off();
        LOG_ERROR("x");  // must not throw or crash
        flush();
        err = cap.text();
    }
    CHECK_FALSE(path_exists(blocker + "/logs"));
    CHECK(err.find("slog:") != std::string::npos);  // a note was printed
}
