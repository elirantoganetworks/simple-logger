// The header and the log path must be usable from a translation unit compiled
// with exceptions turned off. This file is built with -fno-exceptions (see the
// tests CMakeLists). If the header pulled in a throw or a try on the log path,
// this would fail to compile. It has its own main, not doctest, because doctest
// needs exceptions.

#include <cstdio>
#include <string>

#include <slog/slog.h>
#include <slog/testing.h>

using namespace slog;

int main() {
    // A round of the whole public log surface, all noexcept, none of it throwing.
    testing::reset();
    auto& cap = testing::capture_to_memory();
    stdout_off();
    set_file_verbosity(DEBUG);

    LOG_ERROR("error %d", 1);
    LOG_WARNING("warning");
    LOG_INFO("info");
    LOG_DEBUG("debug");
    LOG_TO("net", INFO, "to a module");
    if (is_enabled("net", DEBUG))
        LOG_TO("net", DEBUG, "guarded");

    // The error API is reachable and returns without throwing.
    const error_info e = last_error();
    (void)e;
    (void)dropped();
    clear_error();
    set_error_handler(nullptr, nullptr);

    // The error type and category are usable, just not thrown here.
    const std::error_code ec = make_error_code(errc::file_open);
    const std::string     m  = ec.message();

    const std::size_t got = cap.size();
    testing::stop_capture();

    if (got < 4) {
        std::fprintf(stderr, "expected at least 4 captured lines, got %zu\n", got);
        return 1;
    }
    if (m.empty()) {
        std::fprintf(stderr, "error code message was empty\n");
        return 1;
    }
    std::printf("no-exceptions build ran, %zu lines, code message: %s\n", got, m.c_str());
    return 0;
}
