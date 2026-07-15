// Control the console output on its own, separate from the file.
//
// The file and stdout are independent outputs. Here the file keeps everything
// from INFO down, while the console is trimmed to just one level and just one
// module. This is the "full file, quiet console" setup.

#include <slog/slog.h>

int main() {
    slog::set_verbosity(slog::INFO);  // file gets INFO and more severe

    slog::set_stdout_verbosity(slog::WARNING);  // console: WARNING...
    slog::set_stdout_only(true);                // ...this level only, not "and lower"
    slog::set_stdout_modules({"net"});          // ...and only from the "net" module

    LOG_TO("net", slog::WARNING, "retrying");   // console and file
    LOG_TO("net", slog::ERROR, "gave up");      // file only (console is WARNING-only)
    LOG_TO("db", slog::WARNING, "slow query");  // file only (console is net-only)
}
