// Proves <slog/testing.h> is self-contained.
#include <slog/testing.h>
#include <slog/testing.h>  // include guard

void hdr_testing_alone_use() {
    slog::testing::reset();
    (void)slog::testing::capture_to_memory().size();
}
