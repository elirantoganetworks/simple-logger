// Seeing failures: catch a setup error, and watch runtime failures without a throw.
//
// slog has two phases. Setup (init) runs on your stack, so it throws on a hard
// failure and you can react. Steady-state logging is noexcept, so a runtime
// failure is recorded three ways instead: a sticky last_error(), a handler you
// install, and a dropped() count. This program shows both.

#include <cstdio>
#include <cstdlib>

#include <slog/slog.h>

// Called once per runtime failure. It must not throw, must return promptly, and
// must not log through slog. Here it just prints the failure to stderr.
static void on_slog_error(const slog::error_info& info, void* /*user*/) {
    std::fprintf(stderr, "[slog handler] %s (code %d, errno %d) %s\n", info.message,
                 static_cast<int>(info.code), info.sys_errno, info.detail);
}

int main() {
    // Phase one: react to a hard setup failure. A directory under a path that is
    // a file (not a directory) cannot be created, so init() throws.
    std::FILE* f = std::fopen("not_a_dir", "w");
    if (f != nullptr)
        std::fclose(f);

    slog::set_log_dir("not_a_dir/logs");
    try {
        slog::init();
        std::printf("init() succeeded\n");
    } catch (const slog::error& e) {
        std::printf("caught a setup error: %s (code %d)\n", e.what(), e.code().value());
        // Fall back to a directory that works, and to stdout as well.
        slog::set_log_dir("logs");
    }

    // Phase two: install a handler and log normally. Nothing below throws.
    slog::set_error_handler(&on_slog_error, nullptr);
    slog::set_verbosity(slog::INFO);

    LOG_INFO("up and running");
    LOG_WARNING("this is fine");

    // Poll the sticky error and the drop count whenever you like.
    const slog::error_info last = slog::last_error();
    if (last.code != slog::errc::ok)
        std::printf("last recorded problem: %s\n", last.message);
    std::printf("lines dropped so far: %llu\n",
                static_cast<unsigned long long>(slog::dropped()));

    slog::shutdown();
    std::remove("not_a_dir");
    return 0;
}
