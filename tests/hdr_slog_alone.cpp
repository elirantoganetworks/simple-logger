// Proves <slog/slog.h> is self-contained: it is the only include here. If it
// needed another header first, this would not compile.
#include <slog/slog.h>

// Include twice to exercise the include guard.
#include <slog/slog.h>

void hdr_slog_alone_use() {
    LOG_ERROR("compile-only use %d", 1);
    slog::Level lvl = slog::add_level("HDR_ONLY", 5);
    (void)lvl;
    (void)slog::version();
}
