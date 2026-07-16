// The public headers link together across translation units (the alone TUs in
// this binary each include a header on its own), and the version is exposed both
// as a macro and a function.

#include "doctest.h"
#include "test_util.h"

#include <cstring>

#include <slog/slog.h>
#include <slog/testing.h>

// Defined in strip_check.cpp, which was compiled with a raised ceiling.
namespace stripcheck {
extern int g_debug_args;
void       emit_debug();
void       emit_warning();
}  // namespace stripcheck

TEST_CASE("version macro and function agree and read 1.1.0") {
    CHECK(std::strcmp(slog::version(), "1.1.0") == 0);
    CHECK(std::strcmp(SLOG_VERSION_STRING, "1.1.0") == 0);
    CHECK(SLOG_VERSION_MAJOR == 1);
    CHECK(SLOG_VERSION_MINOR == 1);
    CHECK(SLOG_VERSION_PATCH == 0);
}

TEST_CASE("a compiled-out level emits nothing and skips its arguments") {
    tu::fresh();
    auto& cap = slog::testing::capture_to_memory();
    slog::stdout_off();
    slog::set_verbosity(slog::DEBUG);  // runtime would allow DEBUG...

    stripcheck::g_debug_args = 0;
    stripcheck::emit_debug();              // ...but this call was compiled out
    CHECK(cap.lines().empty());            // nothing emitted
    CHECK(stripcheck::g_debug_args == 0);  // the argument never ran

    stripcheck::emit_warning();  // a level at the ceiling still works
    CHECK(cap.lines().size() == 1);
}
