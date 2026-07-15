// Add your own levels.
//
// A level is a name plus a value. The value places it in the order: a higher
// value is more verbose. A custom level works everywhere a built-in does, as a
// threshold, in config, and in the %L prefix field.

#include <slog/slog.h>

int main() {
    slog::Level TRACE  = slog::add_level("TRACE", 50);   // above DEBUG (40)
    slog::Level NOTICE = slog::add_level("NOTICE", 25);  // between WARNING (20) and INFO (30)

    slog::set_verbosity(TRACE);  // show everything up to TRACE

    LOG(NOTICE, "config reloaded");
    LOG(TRACE, "entering %s", "main loop");
    LOG_INFO("normal levels still work");
}
