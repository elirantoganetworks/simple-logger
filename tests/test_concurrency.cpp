// Concurrency inside one process: many threads to one file (exact count and
// line integrity), a config change while threads log (no crash), and the two
// fork behaviors from DESIGN.md 10.10.

#include "doctest.h"
#include "test_util.h"

#include <atomic>
#include <cctype>
#include <string>
#include <thread>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

using namespace slog;

namespace {
// Is line "[<digits>]: i=<digits>"?
bool well_formed(const std::string& line) {
    if (line.size() < 6 || line[0] != '[')
        return false;
    std::size_t i = 1;
    while (i < line.size() && std::isdigit(static_cast<unsigned char>(line[i]))) ++i;
    if (i == 1)
        return false;  // no tid
    const std::string rest = line.substr(i);
    return rest.rfind("]: i=", 0) == 0;
}

std::string only_log(const std::string& latest_dir) {
    auto files = tu::list_logs(latest_dir);
    if (files.size() != 1)
        return "";
    return tu::read_file(latest_dir + "/" + files[0]);
}
}  // namespace

TEST_CASE("many threads to one file: no lost, no torn, no interleaved lines") {
    tu::fresh();
    tu::TempDir dir;
    set_log_dir(dir.str().c_str());
    stdout_off();
    set_verbosity(INFO);
    set_format("[%T]: %m");
    init();

    const int                kThreads = 8;
    const int                kEach    = 1000;
    std::vector<std::thread> ts;
    for (int t = 0; t < kThreads; ++t) {
        ts.emplace_back([kEach] {
            for (int i = 0; i < kEach; ++i) LOG_INFO("i=%d", i);
        });
    }
    for (auto& th : ts) th.join();
    flush();

    auto lines = tu::split_lines(only_log(dir.sub("latest")));
    CHECK(lines.size() == static_cast<size_t>(kThreads * kEach));  // nothing lost
    bool all_ok = true;
    for (const auto& l : lines)
        if (!well_formed(l))  // a torn or interleaved line fails here
            all_ok = false;
    CHECK(all_ok);
}

TEST_CASE("a config change while threads log does not crash or tear lines") {
    tu::fresh();
    tu::TempDir dir;
    set_log_dir(dir.str().c_str());
    stdout_off();
    set_verbosity(INFO);
    set_format("[%T]: %m");
    init();

    std::atomic<bool> stop{false};
    std::thread       flipper([&] {
        while (!stop.load()) {
            set_format("[%T]: %m");  // rebuilds the format spec repeatedly
            flush();
        }
    });

    const int                kThreads = 4;
    const int                kEach    = 1000;
    std::vector<std::thread> ts;
    for (int t = 0; t < kThreads; ++t)
        ts.emplace_back([kEach] {
            for (int i = 0; i < kEach; ++i) LOG_INFO("i=%d", i);
        });
    for (auto& th : ts) th.join();
    stop.store(true);
    flipper.join();
    flush();

    auto lines = tu::split_lines(only_log(dir.sub("latest")));
    CHECK(lines.size() == static_cast<size_t>(kThreads * kEach));
    for (const auto& l : lines) CHECK(well_formed(l));
}

TEST_CASE("fork with restart_after_fork gives the child its own PID files") {
    tu::fresh();
    tu::TempDir dir;
    set_log_dir(dir.str().c_str());
    stdout_off();
    set_verbosity(INFO);
    set_format("[%L]: %m");
    init();
    const int ppid = static_cast<int>(::getpid());
    LOG_ERROR("parent");
    flush();

    const pid_t child = ::fork();
    if (child == 0) {
        restart_after_fork();
        LOG_ERROR("child");
        flush();
        ::_exit(0);
    }
    int status = 0;
    ::waitpid(child, &status, 0);

    auto latest = tu::list_logs(dir.sub("latest"));
    auto loose  = tu::list_logs(dir.str());
    REQUIRE(latest.size() == 1);
    CHECK(latest[0].rfind(std::to_string(child) + "-", 0) == 0);  // child in latest
    bool parent_moved = false;
    for (const auto& f : loose)
        if (f.rfind(std::to_string(ppid) + "-", 0) == 0)
            parent_moved = true;
    CHECK(parent_moved);  // parent moved out of latest
}

TEST_CASE("fork without restart shares the parent file with the child PID") {
    tu::fresh();
    tu::TempDir dir;
    set_log_dir(dir.str().c_str());
    stdout_off();
    set_verbosity(INFO);
    set_format("[%p]: %m");
    init();
    const int ppid = static_cast<int>(::getpid());
    LOG_ERROR("parent_line");
    flush();

    const pid_t child = ::fork();
    if (child == 0) {
        LOG_ERROR("child_line");  // no restart: writes to the inherited file
        flush();
        ::_exit(0);
    }
    int status = 0;
    ::waitpid(child, &status, 0);
    flush();

    auto lines = tu::split_lines(only_log(dir.sub("latest")));
    CHECK(tu::contains(lines, "[" + std::to_string(ppid) + "]: parent_line"));
    CHECK(tu::contains(lines, "[" + std::to_string(child) + "]: child_line"));
}
