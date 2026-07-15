// A program that sets nothing in code. All of its behavior comes from a config
// file (./simplelog.conf, or the file named by SLOG_CONFIG) and the SLOG_* env
// vars. Drive it with simplelog.conf and run_with_env.sh in this folder.

#include <slog/slog.h>

int main() {
    LOG_INFO("info from the default module");
    LOG_WARNING("warning from the default module");
    LOG_TO("net", slog::DEBUG, "debug from net");
    LOG_TO("db", slog::INFO, "info from db");
    slog::shutdown();
}
