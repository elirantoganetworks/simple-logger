// A real program that uses slog. The three consumption paths each build and run
// this same file, so one program proves all three ways of consuming the library.

#include <slog/slog.h>
#include <slog/testing.h>

#include <cstdio>
#include <string>

int main() {
    auto& cap = slog::testing::capture_to_memory();
    slog::stdout_off();
    slog::set_verbosity(slog::WARNING);

    LOG_WARNING("consume check %d", 42);
    LOG_INFO("this one is filtered");

    const auto lines = cap.lines();
    if (lines.size() != 1) {
        std::fprintf(stderr, "consume: wrong line count %zu\n", lines.size());
        return 1;
    }
    if (lines[0].find("consume check 42") == std::string::npos) {
        std::fprintf(stderr, "consume: wrong content: %s\n", lines[0].c_str());
        return 1;
    }
    std::printf("consume OK (%s)\n", slog::version());
    return 0;
}
