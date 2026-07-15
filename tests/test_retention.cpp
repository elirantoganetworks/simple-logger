// Retention tests. Retention deletes old runs at startup. The audit found it had
// no test at all, so three logic-inverting mutants (keep-N, age, live-run check)
// went unnoticed. These tests guard each rule.
//
// A run is a separate process, so its files get a distinct pid stem. The helper
// program makes one dead run per call. A forked child that holds its run open
// makes a live run.

#include <ctime>
#include <set>
#include <string>
#include <utime.h>

#include <dirent.h>
#include <sys/wait.h>
#include <unistd.h>

#include "doctest.h"

#include <slog/slog.h>
#include <slog/testing.h>

#include "test_util.h"

#ifndef SLOG_HELPER
#error "SLOG_HELPER must be defined to the helper program path"
#endif

namespace {

const char* helper() { return SLOG_HELPER; }

// Names of the run lock files (.slog-run-<stem>.lock) directly in dir.
std::vector<std::string> list_locks(const std::string& dir) {
    std::vector<std::string> out;
    DIR*                     d = ::opendir(dir.c_str());
    if (d == nullptr)
        return out;
    for (dirent* e = ::readdir(d); e != nullptr; e = ::readdir(d)) {
        const std::string n = e->d_name;
        if (n.rfind(".slog-run-", 0) == 0 && n.size() > 5 &&
            n.compare(n.size() - 5, 5, ".lock") == 0)
            out.push_back(n);
    }
    ::closedir(d);
    return out;
}

// Move a file's modified time back by this many days, so an age rule sees it old.
void backdate(const std::string& path, int days) {
    utimbuf       t;
    const time_t  now = ::time(nullptr);
    t.actime          = now;
    t.modtime         = now - static_cast<time_t>(days) * 86400;
    ::utime(path.c_str(), &t);
}

}  // namespace

TEST_CASE("retention keeps only the newest N runs") {
    tu::fresh();
    tu::TempDir dir;

    // Four separate processes make four dead runs (their locks are free).
    for (int i = 0; i < 4; ++i)
        REQUIRE(tu::run_wait({helper(), "run", dir.str()}) == 0);

    // Give the four run locks distinct ages, oldest to newest, so the sort order
    // is fixed and this test does not depend on wall-clock ties.
    auto locks = list_locks(dir.str());
    REQUIRE(locks.size() == 4);
    for (std::size_t i = 0; i < locks.size(); ++i)
        backdate(dir.sub(locks[i]), static_cast<int>(4 - i));

    // A fifth run with retain.runs = 2 sweeps: it keeps the two newest runs
    // (itself, still live, plus the newest dead run) and reaps the three older
    // dead runs. One dead run's files remain.
    slog::Config c;
    c.log_dir     = dir.str();
    c.retain_runs = 2;
    slog::configure(c);
    slog::init();
    slog::shutdown();

    // Exactly one moved-out run remains. This fails if the keep-N cutoff is off
    // by one (leaves two) or the dead-run check is inverted (leaves all four).
    const auto loose = tu::list_logs(dir.str());
    CHECK(loose.size() == 1);
}

TEST_CASE("retention drops runs older than retain.days") {
    tu::fresh();
    tu::TempDir dir;

    for (int i = 0; i < 3; ++i)
        REQUIRE(tu::run_wait({helper(), "run", dir.str()}) == 0);

    // Age two of the three run locks past one day.
    auto locks = list_locks(dir.str());
    REQUIRE(locks.size() == 3);
    backdate(dir.sub(locks[0]), 2);
    backdate(dir.sub(locks[1]), 2);

    slog::Config c;
    c.log_dir     = dir.str();
    c.retain_days = 1;
    slog::configure(c);
    slog::init();
    slog::shutdown();

    // The two aged runs are gone; the recent one stays. This fails if the age
    // comparison is inverted.
    const auto loose = tu::list_logs(dir.str());
    CHECK(loose.size() == 1);
}

TEST_CASE("retention never reaps a live run") {
    tu::fresh();
    tu::TempDir dir;

    int ready[2];
    int go[2];
    REQUIRE(::pipe(ready) == 0);
    REQUIRE(::pipe(go) == 0);

    const pid_t child = ::fork();
    REQUIRE(child >= 0);
    if (child == 0) {
        ::close(ready[0]);
        ::close(go[1]);
        slog::set_log_dir(dir.str().c_str());
        slog::set_file_verbosity(slog::DEBUG);
        slog::stdout_off();
        slog::init();  // opens the run and holds its lock
        LOG_ERROR("child alive");
        char one = 1;
        ssize_t w = ::write(ready[1], &one, 1);  // tell the parent we are up
        (void)w;
        char b = 0;
        ssize_t r = ::read(go[0], &b, 1);  // block until the parent is done
        (void)r;
        slog::shutdown();
        ::_exit(0);
    }

    ::close(ready[1]);
    ::close(go[0]);
    char one = 0;
    REQUIRE(::read(ready[0], &one, 1) == 1);  // child is live now

    // Age every run lock, so an age rule would target the child if it wrongly
    // thought the child was dead.
    for (const auto& lk : list_locks(dir.str()))
        backdate(dir.sub(lk), 2);

    // The parent starts its own run with an aggressive age limit. It moves the
    // child's file out of latest, then sweeps. The child holds its lock, so it
    // is live and must not be reaped.
    slog::Config c;
    c.log_dir     = dir.str();
    c.retain_days = 1;
    slog::configure(c);
    slog::init();
    slog::shutdown();

    const auto loose = tu::list_logs(dir.str());
    CHECK(loose.size() >= 1);  // the live child's file survived

    char g = 1;
    ssize_t w = ::write(go[1], &g, 1);  // let the child exit
    (void)w;
    int status = 0;
    ::waitpid(child, &status, 0);
    ::close(ready[0]);
    ::close(go[1]);
}
