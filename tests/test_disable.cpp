// Disable: nothing is written, nothing is opened, and the arguments of a log
// call are never evaluated. The argument side effect is the strong proof: if the
// guard let the arguments run, the counter would move.

#include "doctest.h"
#include "test_util.h"

#include <slog/slog.h>
#include <slog/testing.h>

#include <sys/stat.h>

using namespace slog;

namespace {
int g_side_effect = 0;
int bump() {
    return ++g_side_effect;
}

bool path_exists(const std::string& p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0;
}
}  // namespace

TEST_CASE("disable collects nothing and does not evaluate the arguments") {
    tu::fresh();
    auto& cap = testing::capture_to_memory();
    stdout_off();
    set_disabled(true);
    g_side_effect = 0;

    LOG_ERROR("value=%d", bump());  // even ERROR is dropped
    LOG_WARNING("value=%d", bump());

    CHECK(cap.lines().empty());  // nothing written
    CHECK(g_side_effect == 0);   // the arguments never ran
}

TEST_CASE("a filtered-out level also skips its arguments") {
    tu::fresh();
    auto& cap = testing::capture_to_memory();
    stdout_off();
    set_verbosity(WARNING);  // INFO and below are filtered
    g_side_effect = 0;

    LOG_INFO("value=%d", bump());
    LOG_DEBUG("value=%d", bump());

    CHECK(cap.lines().empty());
    CHECK(g_side_effect == 0);  // guard runs before the arguments
}

TEST_CASE("disable opens no files") {
    tu::fresh();
    tu::TempDir dir;
    set_log_dir(dir.str().c_str());
    stdout_off();
    set_disabled(true);

    LOG_ERROR("nope");
    flush();

    // The run dir is created lazily on the first real write, so it must be absent.
    CHECK_FALSE(path_exists(dir.sub("latest")));
    CHECK(tu::list_logs(dir.sub("latest")).empty());
}

TEST_CASE("re-enabling after disable brings logging back") {
    tu::fresh();
    auto& cap = testing::capture_to_memory();
    stdout_off();

    set_disabled(true);
    LOG_ERROR("while_off");
    CHECK(cap.lines().empty());

    set_disabled(false);
    LOG_ERROR("while_on");
    CHECK(tu::contains(cap.lines(), "while_on"));
}
