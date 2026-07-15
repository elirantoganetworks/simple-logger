// Lifecycle and message edge cases: logging from a static constructor (DESIGN.md
// 10.12 says this is safe), and messages that are long, empty, full of format
// specifiers, hold a null byte, hold non-ascii bytes, or use long/many modules.

#include "doctest.h"
#include "test_util.h"

#include <slog/slog.h>
#include <slog/testing.h>

#include <cstddef>
#include <string>

using namespace slog;

namespace {
// A global object that logs from its constructor, before main(). If the Meyers
// singleton were not safe from a static ctor, this would crash before any test
// runs. It records the captured line count, so a test can prove the line was
// actually delivered.
struct EarlyLogger {
    EarlyLogger() {
        auto& c = testing::capture_to_memory();
        c.clear();
        LOG_ERROR("from_static_ctor");
        captured = c.size();
        line     = c.lines().empty() ? "" : c.lines()[0];
        testing::stop_capture();
    }
    static std::size_t captured;
    static std::string line;
};
std::size_t EarlyLogger::captured = 999;  // sentinel, overwritten by the ctor
std::string EarlyLogger::line;
EarlyLogger g_early;
}  // namespace

TEST_CASE("logging from a static constructor is safe and delivered") {
    // Reached main(), so the static ctor did not crash. And it captured its line.
    CHECK(EarlyLogger::captured == 1);
    CHECK(EarlyLogger::line.find("from_static_ctor") != std::string::npos);
}

TEST_CASE("a very long message is truncated, not overflowed, and stays one line") {
    tu::fresh();
    auto& cap = testing::capture_to_memory();
    stdout_off();
    set_format("[%L]: %m");
    const std::string big(5000, 'a');
    LOG_ERROR("%s", big.c_str());
    REQUIRE(cap.lines().size() == 1);
    const std::string line = cap.lines()[0];
    CHECK(line.size() <= 4096);  // capped at the atomic-write size
    CHECK(line.size() > 3000);   // but most of the message survived
    CHECK(line.rfind("[ERROR]: aaaa", 0) == 0);
}

TEST_CASE("an empty message still produces a line with the prefix") {
    tu::fresh();
    auto& cap = testing::capture_to_memory();
    stdout_off();
    set_format("[%L]: %m");
    LOG_ERROR("%s", "");
    REQUIRE(cap.lines().size() == 1);
    CHECK(cap.lines()[0] == "[ERROR]: ");
}

TEST_CASE("percent signs and specifiers in the message are handled by printf") {
    tu::fresh();
    auto& cap = testing::capture_to_memory();
    stdout_off();
    set_format("[%L]: %m");

    LOG_ERROR("100%% done, n=%d", 5);
    LOG_ERROR("%s", "a raw %d and %s stay literal");  // arg is not re-scanned

    auto lines = cap.lines();
    REQUIRE(lines.size() == 2);
    CHECK(lines[0] == "[ERROR]: 100% done, n=5");
    CHECK(lines[1] == "[ERROR]: a raw %d and %s stay literal");
}

TEST_CASE("a null byte in the message does not cut the line short") {
    tu::fresh();
    auto& cap = testing::capture_to_memory();
    stdout_off();
    set_format("[%L]: %m");
    LOG_ERROR("a%cb", 0);  // message bytes are 'a', '\0', 'b'
    REQUIRE(cap.lines().size() == 1);
    const std::string line = cap.lines()[0];
    // "[ERROR]: " (9) + "a\0b" (3) = 12 bytes; the null is kept, not a terminator.
    CHECK(line.size() == 12);
    CHECK(line[9] == 'a');
    CHECK(line[10] == '\0');
    CHECK(line[11] == 'b');
}

TEST_CASE("non-ascii (UTF-8) bytes pass through unchanged") {
    tu::fresh();
    auto& cap = testing::capture_to_memory();
    stdout_off();
    set_format("[%L]: %m");
    // "caf" + U+00E9 (0xC3 0xA9) + " " + U+2603 snowman (0xE2 0x98 0x83).
    const std::string utf = "caf\xc3\xa9 \xe2\x98\x83";
    LOG_ERROR("%s", utf.c_str());
    REQUIRE(cap.lines().size() == 1);
    CHECK(cap.lines()[0] == "[ERROR]: " + utf);
}

TEST_CASE("a very long module name works") {
    tu::fresh();
    auto& cap = testing::capture_to_memory();
    stdout_off();
    set_verbosity(DEBUG);
    set_format("[%M]: %m");
    const std::string longmod(500, 'm');
    LOG_TO(longmod.c_str(), INFO, "hi");
    REQUIRE(cap.lines().size() == 1);
    CHECK(cap.lines()[0] == "[" + longmod + "]: hi");
}

TEST_CASE("many distinct modules all resolve and filter correctly") {
    tu::fresh();
    auto& cap = testing::capture_to_memory();
    stdout_off();
    set_verbosity(INFO);
    const int kModules = 200;
    for (int i = 0; i < kModules; ++i) {
        const std::string m = "mod" + std::to_string(i);
        LOG_TO(m.c_str(), INFO, "line%d", i);
    }
    CHECK(cap.lines().size() == static_cast<size_t>(kModules));
}
