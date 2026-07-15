// Multi-process behavior, driven by the helper program so every run is a real
// separate process: moving out of latest, tags and the <tag>-<index> fallback,
// the concurrent-start race, cross-process line atomicity on a shared stream,
// and the crash handler.

#include "doctest.h"
#include "test_util.h"

#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef SLOG_HELPER
#error "SLOG_HELPER must be defined to the helper program path"
#endif

namespace {
const char* helper() {
    return SLOG_HELPER;
}

bool path_exists(const std::string& p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0;
}
}  // namespace

TEST_CASE("a second run moves the first run out of latest") {
    tu::TempDir dir;
    REQUIRE(tu::run_wait({helper(), "run", dir.str()}) == 0);
    auto after_first = tu::list_logs(dir.sub("latest"));
    REQUIRE(after_first.size() == 1);
    const std::string first_name = after_first[0];

    REQUIRE(tu::run_wait({helper(), "run", dir.str()}) == 0);

    auto latest = tu::list_logs(dir.sub("latest"));
    auto loose  = tu::list_logs(dir.str());
    CHECK(latest.size() == 1);       // only the second run is in latest
    CHECK(loose.size() == 1);        // the first run moved out, loose
    CHECK(loose[0] == first_name);   // and kept its name
    CHECK(latest[0] != first_name);  // the second run is a different file
    // The moved-out file still holds its line.
    CHECK(tu::contains(tu::split_lines(tu::read_file(dir.sub(loose[0]))), "hello"));
}

TEST_CASE("tags: dir path, move-out path, and the index fallback") {
    tu::TempDir dir;
    // Three runs, all tagged nightly.
    REQUIRE(tu::run_wait({helper(), "runtag", dir.str(), "nightly"}) == 0);
    REQUIRE(tu::run_wait({helper(), "runtag", dir.str(), "nightly"}) == 0);
    REQUIRE(tu::run_wait({helper(), "runtag", dir.str(), "nightly"}) == 0);

    // The newest run is under latest/nightly.
    CHECK(path_exists(dir.sub("latest/nightly")));
    CHECK(tu::list_logs(dir.sub("latest/nightly")).size() == 1);
    // The tagged file name starts with the tag.
    CHECK(tu::list_logs(dir.sub("latest/nightly"))[0].rfind("nightly-", 0) == 0);
    // The first moved-out run is <dir>/nightly, the second is <dir>/nightly-1.
    CHECK(path_exists(dir.sub("nightly")));
    CHECK(path_exists(dir.sub("nightly-1")));
    CHECK(tu::list_logs(dir.sub("nightly")).size() == 1);
    CHECK(tu::list_logs(dir.sub("nightly-1")).size() == 1);
}

TEST_CASE("many runs starting at once: exactly one wins latest, none is lost") {
    tu::TempDir                           dir;
    const int                             kRuns = 24;
    std::vector<std::vector<std::string>> cmds;
    for (int i = 0; i < kRuns; ++i) cmds.push_back({helper(), "race", dir.str()});

    const int ok = tu::run_all_concurrent(cmds);
    CHECK(ok == kRuns);  // none crashed on the latest race

    // Count every .log under the dir (latest + loose).
    auto latest = tu::list_logs(dir.sub("latest"));
    auto loose  = tu::list_logs(dir.str());
    CHECK(latest.size() == 1);                              // one current run
    CHECK(loose.size() == static_cast<size_t>(kRuns - 1));  // the rest moved out
    // Every file kept its line (nothing was clobbered by the race).
    CHECK(tu::contains(tu::split_lines(tu::read_file(dir.sub("latest/" + latest[0]))),
                       "hi"));
    for (const auto& f : loose)
        CHECK(tu::contains(tu::split_lines(tu::read_file(dir.sub(f))), "hi"));
}

TEST_CASE("shared stdout across processes stays line-atomic") {
    tu::TempDir       dir;
    const std::string shared = dir.sub("shared_stdout.txt");
    // A single O_APPEND file, shared by all children as their stdout. Atomic
    // single-write() appends must keep every line whole.
    const int fd = ::open(shared.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_TRUNC, 0644);
    REQUIRE(fd >= 0);
    const int saved = ::dup(STDOUT_FILENO);
    ::dup2(fd, STDOUT_FILENO);
    ::close(fd);

    const int                             kProcs = 8;
    const int                             kLines = 400;
    std::vector<std::vector<std::string>> cmds;
    for (int i = 0; i < kProcs; ++i)
        cmds.push_back({helper(), "stdout", dir.str(), std::to_string(kLines)});
    const int ok = tu::run_all_concurrent(cmds);

    ::dup2(saved, STDOUT_FILENO);  // restore before asserting
    ::close(saved);

    CHECK(ok == kProcs);
    auto lines = tu::split_lines(tu::read_file(shared));
    CHECK(lines.size() == static_cast<size_t>(kProcs * kLines));
    bool all_whole = true;
    for (const auto& l : lines)
        if (l.rfind("[INFO]: L", 0) != 0)  // a torn line would fail this
            all_whole = false;
    CHECK(all_whole);
}

TEST_CASE("the crash handler preserves the log up to a fatal signal") {
    tu::TempDir dir;
    // The child logs a line then segfaults. The line must survive.
    tu::run_wait({helper(), "crash", dir.str()});  // exit code is the signal, not 0
    auto latest = tu::list_logs(dir.sub("latest"));
    REQUIRE(latest.size() == 1);
    auto lines = tu::split_lines(tu::read_file(dir.sub("latest/" + latest[0])));
    CHECK(tu::contains(lines, "before crash"));
}
