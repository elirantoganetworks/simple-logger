// Modules and per-module levels.
//
// A module is a name you attach to a log call. Each module has its own level,
// so you can turn one part of the program up to DEBUG and leave the rest quiet.
//
// The usual way to set a module is `#define LOG_MODULE "net"` at the top of a
// file, above the include, which tags every call in that file. To show two
// modules in one small program we use LOG_TO, which names the module per call.

#include <slog/slog.h>

int main() {
    slog::set_verbosity(slog::WARNING);              // global default
    slog::set_module_verbosity("net", slog::DEBUG);  // but "net" is verbose

    LOG_TO("net", slog::DEBUG, "socket connected fd=%d", 7);  // shown: net is DEBUG
    LOG_TO("net", slog::INFO, "handshake done");             // shown
    LOG_TO("db", slog::INFO, "query took %d ms", 12);         // dropped: db is WARNING
    LOG_TO("db", slog::ERROR, "connection lost");             // shown: ERROR is severe
}
