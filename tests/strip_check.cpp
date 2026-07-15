// Compiled with a raised ceiling so DEBUG and INFO are stripped at build time.
// The functions here are called by test_headers to prove a stripped call emits
// nothing and does not even evaluate its arguments, while a kept level works.

#define SLOG_ACTIVE_LEVEL SLOG_LEVEL_WARNING
#include <slog/slog.h>

namespace stripcheck {

int g_debug_args = 0;
int g_warn_args  = 0;

int bump_debug() {
    return ++g_debug_args;
}
int bump_warn() {
    return ++g_warn_args;
}

void emit_debug() {
    LOG_DEBUG("d=%d", bump_debug());
}  // below the ceiling: no-op
void emit_warning() {
    LOG_WARNING("w=%d", bump_warn());
}  // at the ceiling: kept

}  // namespace stripcheck
