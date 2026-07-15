// Proves SLOG_NO_SHORT_MACROS suppresses the short LOG_* names but keeps the
// canonical SLOG_* names. The checks are at compile time.
#define SLOG_NO_SHORT_MACROS
#include <slog/slog.h>

#ifdef LOG_INFO
#error "LOG_INFO must be suppressed by SLOG_NO_SHORT_MACROS"
#endif
#ifdef LOG_ERROR
#error "LOG_ERROR must be suppressed by SLOG_NO_SHORT_MACROS"
#endif
#ifndef SLOG_INFO
#error "SLOG_INFO must still be defined"
#endif

void hdr_no_short_use() {
    SLOG_ERROR("still works %d", 1);
}
