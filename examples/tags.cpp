// Tag a run so its logs group under their own subdirectory.
//
// A tag is handy for naming a run: "nightly", "smoke", a job id. With tag
// "nightly" the files land at
//   ./logs/latest/nightly/nightly-<pid>-<timestamp>.log
// When the next tagged run starts, the old one moves out to ./logs/nightly/.

#include <cstdio>

#include <slog/slog.h>

int main() {
    slog::set_run_tag("nightly");
    slog::set_verbosity(slog::INFO);
    slog::stdout_off();

    LOG_INFO("nightly job started");
    LOG_INFO("nightly job done");

    slog::shutdown();
    std::printf("wrote a tagged run under ./logs/latest/nightly/ (try: ls -R logs)\n");
}
