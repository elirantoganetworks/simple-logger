// Where the log files go, and one file per module.
//
// By default slog writes one file per run at
//   ./logs/latest/<pid>-<timestamp>.log
// Turn on file-per-module and each module gets its own file instead:
//   ./logs/latest/<pid>-<timestamp>-<module>.log
// To turn the file off entirely and keep only stdout, call slog::file_off().

#include <cstdio>

#include <slog/slog.h>

int main() {
    slog::set_file_per_module(true);  // one file per module
    slog::set_verbosity(slog::INFO);
    slog::stdout_off();  // this example is about files

    LOG_TO("net", slog::INFO, "listening on :8080");
    LOG_TO("db", slog::INFO, "connected");

    slog::shutdown();  // flush and close before we look at the files
    std::printf("wrote per-module files under ./logs/latest/ (try: ls -R logs)\n");
}
