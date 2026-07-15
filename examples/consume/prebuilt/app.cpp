// A tiny program that uses slog. The three consume examples each build this
// same file a different way.

#include <cstdio>

#include <slog/slog.h>

int main() {
    LOG_WARNING("hello from slog %s", slog::version());
    std::printf("ok, linked slog %s\n", slog::version());
    return 0;
}
