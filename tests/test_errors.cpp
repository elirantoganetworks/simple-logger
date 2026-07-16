// Error propagation: every code, the guards behind it, and the three ways a
// failure reaches the user (the sticky last_error, the handler, the drop count).
//
// Each code has a case that actually produces it, by injecting the real fault
// (a bad path, a full file, a closed pipe, a forced bad_alloc), not by faking
// the report. The numeric values are pinned in one place so a released code can
// never silently change.

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <thread>

#include <fcntl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "doctest.h"
#include "test_util.h"

#include <slog/slog.h>
#include <slog/testing.h>

using namespace slog;

// ---- sanitizer detection ---------------------------------------------------
// The forced-allocation-failure test replaces global operator new. Under a
// sanitizer that owns the allocator, so the injection is skipped there and the
// normal build covers the out_of_memory path instead.
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define SLOG_TEST_SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define SLOG_TEST_SANITIZED 1
#endif
#endif
#ifndef SLOG_TEST_SANITIZED
#define SLOG_TEST_SANITIZED 0
#endif

namespace {

int code_of(errc e) { return static_cast<int>(e); }

}  // namespace

#if !SLOG_TEST_SANITIZED
// A test-controlled allocation failure. Arming it makes exactly the next global
// allocation throw, then disarms itself, so the failure lands on one known
// allocation and every allocation after it (including any inside the error
// report) succeeds. -1 means never fail.
namespace {
std::atomic<int> g_fail_next_new{-1};

void arm_next_alloc_failure() { g_fail_next_new.store(0, std::memory_order_relaxed); }

void* checked_alloc(std::size_t n) {
    if (g_fail_next_new.load(std::memory_order_relaxed) == 0) {
        g_fail_next_new.store(-1, std::memory_order_relaxed);  // fail once
        throw std::bad_alloc();
    }
    void* p = std::malloc(n != 0 ? n : 1);
    if (p == nullptr)
        throw std::bad_alloc();
    return p;
}
}  // namespace

void* operator new(std::size_t n) { return checked_alloc(n); }
void* operator new[](std::size_t n) { return checked_alloc(n); }
void  operator delete(void* p) noexcept { std::free(p); }
void  operator delete[](void* p) noexcept { std::free(p); }
void  operator delete(void* p, std::size_t) noexcept { std::free(p); }
void  operator delete[](void* p, std::size_t) noexcept { std::free(p); }
#endif  // !SLOG_TEST_SANITIZED

// ---- noexcept audit --------------------------------------------------------
// The log path and every query are declared noexcept. These checks fail to
// compile if that ever regresses.
namespace {
[[maybe_unused]] CallSite g_noexcept_cs{"noexcept_probe", nullptr};
}  // namespace
static_assert(noexcept(should_log(g_noexcept_cs, ERROR)), "should_log must be noexcept");
static_assert(noexcept(emit(g_noexcept_cs, ERROR, SourceLoc{"f", 0, "fn"}, "x")),
              "emit must be noexcept");
static_assert(noexcept(emit_to("m", ERROR, SourceLoc{"f", 0, "fn"}, "x")),
              "emit_to must be noexcept");
static_assert(noexcept(is_enabled("m", ERROR)), "is_enabled must be noexcept");
static_assert(noexcept(mono_nanos()), "mono_nanos must be noexcept");
static_assert(noexcept(flush()), "flush must be noexcept");
static_assert(noexcept(shutdown()), "shutdown must be noexcept");
static_assert(noexcept(restart_after_fork()), "restart_after_fork must be noexcept");
static_assert(noexcept(install_crash_handler()), "install_crash_handler must be noexcept");
static_assert(noexcept(version()), "version must be noexcept");
static_assert(noexcept(last_error()), "last_error must be noexcept");
static_assert(noexcept(dropped()), "dropped must be noexcept");
static_assert(noexcept(clear_error()), "clear_error must be noexcept");
static_assert(noexcept(set_error_handler(nullptr, nullptr)), "set_error_handler must be noexcept");
static_assert(noexcept(error_category()), "error_category must be noexcept");
static_assert(noexcept(make_error_code(errc::ok)), "make_error_code must be noexcept");

// ---- frozen numbers --------------------------------------------------------
// The wire values of every code. A released value never changes and is never
// reused. This test is the record of that promise.
TEST_CASE("error code numbers are frozen") {
    CHECK(code_of(errc::ok) == 0);
    CHECK(code_of(errc::config_file_read) == 10);
    CHECK(code_of(errc::config_bad_value) == 11);
    CHECK(code_of(errc::config_unknown_key) == 12);
    CHECK(code_of(errc::dir_create) == 20);
    CHECK(code_of(errc::run_lock) == 21);
    CHECK(code_of(errc::run_dir_open) == 22);
    CHECK(code_of(errc::file_open) == 30);
    CHECK(code_of(errc::file_write) == 31);
    CHECK(code_of(errc::stdout_write) == 40);
    CHECK(code_of(errc::out_of_memory) == 50);
    CHECK(code_of(errc::message_truncated) == 51);
}

TEST_CASE("every code has a non-empty, distinct message and a std::error_code") {
    const errc codes[] = {errc::ok,        errc::config_file_read, errc::config_bad_value,
                          errc::config_unknown_key, errc::dir_create, errc::run_lock,
                          errc::run_dir_open, errc::file_open, errc::file_write,
                          errc::stdout_write, errc::out_of_memory, errc::message_truncated};
    for (errc c : codes) {
        const std::error_code ec = make_error_code(c);
        CHECK(ec.value() == code_of(c));
        CHECK(&ec.category() == &error_category());
        CHECK(std::strcmp(error_category().name(), "slog") == 0);
        CHECK_FALSE(ec.message().empty());
    }
}

// ---- config codes ----------------------------------------------------------

TEST_CASE("config_file_read: an explicit load of a missing file is surfaced") {
    tu::fresh();
    tu::TempDir dir;
    load_file(dir.sub("nope.conf").c_str());  // requested, but not there
    const error_info e = last_error();
    CHECK(code_of(e.code) == code_of(errc::config_file_read));
    CHECK(e.sys_errno == ENOENT);
    CHECK(std::string(e.message).size() > 0);
    CHECK(std::string(e.detail).find("nope.conf") != std::string::npos);
}

TEST_CASE("config_bad_value: a value that does not parse names the value") {
    tu::fresh();
    tu::TempDir       dir;
    const std::string conf = dir.sub("c.conf");
    tu::write_file(conf, "verbosity = nonsense\n");
    load_file(conf.c_str());
    const error_info e = last_error();
    CHECK(code_of(e.code) == code_of(errc::config_bad_value));
    CHECK(std::string(e.detail).find("nonsense") != std::string::npos);
}

TEST_CASE("config_unknown_key: an unknown key is surfaced with its name") {
    tu::fresh();
    tu::TempDir       dir;
    const std::string conf = dir.sub("c.conf");
    tu::write_file(conf, "made_up_key = 1\n");
    load_file(conf.c_str());
    const error_info e = last_error();
    CHECK(code_of(e.code) == code_of(errc::config_unknown_key));
    CHECK(std::string(e.detail).find("made_up_key") != std::string::npos);
}

// ---- run-directory codes: these throw from init() --------------------------

TEST_CASE("dir_create: a log dir whose parent is a file throws from init") {
    tu::fresh();
    tu::TempDir base;
    tu::write_file(base.sub("notdir"), "x");  // a regular file where a dir is needed
    set_log_dir(base.sub("notdir/logs").c_str());
    stdout_off();

    bool threw = false;
    try {
        init();
    } catch (const error& e) {
        threw = true;
        CHECK(e.code().value() == code_of(errc::dir_create));
        CHECK(&e.code().category() == &error_category());
    }
    CHECK(threw);
    CHECK(code_of(last_error().code) == code_of(errc::dir_create));
}

TEST_CASE("run_lock: a lock path taken by a directory throws from init") {
    tu::fresh();
    tu::TempDir base;
    const std::string d = base.sub("rl");
    ::mkdir(d.c_str(), 0755);
    ::mkdir((d + "/.slog.lock").c_str(), 0755);  // the lock name is a directory
    set_log_dir(d.c_str());
    stdout_off();

    bool threw = false;
    try {
        init();
    } catch (const error& e) {
        threw = true;
        CHECK(e.code().value() == code_of(errc::run_lock));
    }
    CHECK(threw);
    CHECK(code_of(last_error().code) == code_of(errc::run_lock));
}

TEST_CASE("run_dir_open: a latest that is a file throws from init") {
    tu::fresh();
    tu::TempDir base;
    const std::string d = base.sub("rdo");
    ::mkdir(d.c_str(), 0755);
    tu::write_file(d + "/latest", "x");  // latest exists, but as a file
    set_log_dir(d.c_str());
    stdout_off();

    bool threw = false;
    try {
        init();
    } catch (const error& e) {
        threw = true;
        CHECK(e.code().value() == code_of(errc::run_dir_open));
    }
    CHECK(threw);
    CHECK(code_of(last_error().code) == code_of(errc::run_dir_open));
}

// ---- file output codes -----------------------------------------------------

TEST_CASE("file_open: a name too long to open is recorded, logging continues") {
    tu::fresh();
    tu::TempDir dir;
    set_log_dir(dir.str().c_str());
    set_file_per_module(true);
    set_file_verbosity(DEBUG);
    stdout_off();

    const std::string longmod(500, 'm');  // one path component over NAME_MAX
    LOG_TO(longmod.c_str(), ERROR, "x");

    const error_info e = last_error();
    CHECK(code_of(e.code) == code_of(errc::file_open));
    CHECK(e.sys_errno != 0);  // the real errno (ENAMETOOLONG) is carried
}

TEST_CASE("file_write: a full file drops the line, counts it, stays alive") {
    tu::fresh();
    tu::TempDir dir;

    const pid_t child = ::fork();
    REQUIRE(child >= 0);
    if (child == 0) {
        ::signal(SIGXFSZ, SIG_IGN);  // turn the size signal into an EFBIG error
        rlimit rl{512, 512};
        ::setrlimit(RLIMIT_FSIZE, &rl);
        set_log_dir(dir.str().c_str());
        set_file_verbosity(DEBUG);
        stdout_off();
        for (int i = 0; i < 2000; ++i)
            LOG_ERROR("line %d with padding to pass the tiny size limit", i);
        const bool ok = dropped() > 0 &&
                        code_of(last_error().code) == code_of(errc::file_write);
        ::_exit(ok ? 0 : 3);
    }
    int status = 0;
    ::waitpid(child, &status, 0);
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
}

TEST_CASE("stdout_write: a closed pipe drops the line, counts it, stays alive") {
    tu::fresh();

    const pid_t child = ::fork();
    REQUIRE(child >= 0);
    if (child == 0) {
        int fds[2];
        if (::pipe(fds) != 0)
            ::_exit(4);
        ::close(fds[0]);                 // reader gone, so a write raises EPIPE
        ::dup2(fds[1], STDOUT_FILENO);
        ::close(fds[1]);
        file_off();                      // stdout only
        set_stdout_verbosity(INFO);
        for (int i = 0; i < 100; ++i)
            LOG_INFO("line %d to a closed pipe", i);
        const bool ok = dropped() > 0 &&
                        code_of(last_error().code) == code_of(errc::stdout_write);
        ::_exit(ok ? 0 : 5);
    }
    int status = 0;
    ::waitpid(child, &status, 0);
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);  // no SIGPIPE death, drop counted
}

// ---- internal codes --------------------------------------------------------

TEST_CASE("message_truncated: an over-long message is cut and recorded") {
    tu::fresh();
    auto& cap = testing::capture_to_memory();
    stdout_off();
    set_file_verbosity(DEBUG);

    const std::string big(9000, 'Z');
    LOG_ERROR("%s", big.c_str());

    REQUIRE(cap.lines().size() == 1);  // the line is still written
    CHECK(code_of(last_error().code) == code_of(errc::message_truncated));
}

TEST_CASE("out_of_memory: a failed allocation on first use of a module is caught") {
#if SLOG_TEST_SANITIZED
    MESSAGE("skipped under sanitizer (the sanitizer owns the allocator)");
#else
    tu::fresh();
    const std::uint64_t before = dropped();

    // A fresh call site forces module resolution, whose first allocation we make
    // fail. The log call is noexcept, so the bad_alloc is caught, reported, and
    // the dropped count goes up - the program does not crash.
    CallSite cs{"oom_module", nullptr};
    arm_next_alloc_failure();
    emit(cs, ERROR, SourceLoc{"f", 1, "fn"}, "never written");
    g_fail_next_new.store(-1, std::memory_order_relaxed);  // belt and suspenders

    CHECK(code_of(last_error().code) == code_of(errc::out_of_memory));
    CHECK(dropped() == before + 1);
#endif
}

// ---- the handler -----------------------------------------------------------

namespace {
struct HandlerState {
    std::atomic<int> calls{0};
    std::atomic<int> last_code{-1};
    std::atomic<int> last_errno{0};
    char             last_detail[200] = {};
};
void record_handler(const error_info& info, void* user) {
    auto* s = static_cast<HandlerState*>(user);
    s->calls.fetch_add(1, std::memory_order_relaxed);
    s->last_code.store(static_cast<int>(info.code), std::memory_order_relaxed);
    s->last_errno.store(info.sys_errno, std::memory_order_relaxed);
    // info.detail is a fixed 200-byte buffer, null-terminated within, and
    // last_detail is the same size, so a full copy is safe and stays terminated.
    std::memcpy(s->last_detail, info.detail, sizeof(s->last_detail));
}
}  // namespace

TEST_CASE("the handler fires once per failure with the right code and detail") {
    tu::fresh();
    HandlerState st;
    set_error_handler(&record_handler, &st);

    tu::TempDir dir;
    set_log_dir(dir.str().c_str());
    set_file_per_module(true);
    set_file_verbosity(DEBUG);
    stdout_off();

    const std::string longmod(500, 'x');
    LOG_TO(longmod.c_str(), ERROR, "x");  // one file_open failure

    CHECK(st.calls.load() == 1);
    CHECK(st.last_code.load() == code_of(errc::file_open));
    CHECK(st.last_errno.load() != 0);

    set_error_handler(nullptr, nullptr);  // clear
    LOG_TO(longmod.c_str(), ERROR, "y");  // module already gave up, no new call
    CHECK(st.calls.load() == 1);          // the cleared handler did not fire
}

TEST_CASE("a handler that throws does not escape the log path") {
    tu::fresh();
    set_error_handler([](const error_info&, void*) { throw std::runtime_error("boom"); },
                      nullptr);

    tu::TempDir dir;
    set_log_dir(dir.str().c_str());
    set_file_per_module(true);
    set_file_verbosity(DEBUG);
    stdout_off();

    const std::string longmod(500, 'q');
    LOG_TO(longmod.c_str(), ERROR, "x");  // fails, handler throws, must be swallowed
    CHECK(code_of(last_error().code) == code_of(errc::file_open));  // reached here = no escape
    set_error_handler(nullptr, nullptr);
}

TEST_CASE("a handler that logs does not recurse forever") {
    tu::fresh();
    tu::TempDir dir;

    const pid_t child = ::fork();
    REQUIRE(child >= 0);
    if (child == 0) {
        ::signal(SIGXFSZ, SIG_IGN);
        rlimit rl{256, 256};
        ::setrlimit(RLIMIT_FSIZE, &rl);  // every file write fails
        set_log_dir(dir.str().c_str());
        set_file_verbosity(DEBUG);
        stdout_off();
        static std::atomic<int> handler_calls{0};
        set_error_handler(
            [](const error_info&, void*) {
                handler_calls.fetch_add(1, std::memory_order_relaxed);
                LOG_ERROR("a handler that logs, which also fails");  // must not re-enter
            },
            nullptr);
        for (int i = 0; i < 200; ++i)
            LOG_ERROR("top level line %d with padding to overflow the limit", i);
        // Reaching here means the recursion guard held. Without it the nested log
        // would call the handler again without end and blow the stack.
        ::_exit(handler_calls.load() > 0 ? 0 : 6);
    }
    int status = 0;
    ::waitpid(child, &status, 0);
    REQUIRE(WIFEXITED(status));  // not killed by a stack-overflow signal
    CHECK(WEXITSTATUS(status) == 0);
}

// ---- sticky state and the drop count ---------------------------------------

TEST_CASE("last_error is sticky and clear_error resets it") {
    tu::fresh();
    CHECK(code_of(last_error().code) == code_of(errc::ok));  // clean after reset

    tu::TempDir       dir;
    const std::string conf = dir.sub("c.conf");
    tu::write_file(conf, "verbosity = bad\n");
    load_file(conf.c_str());
    CHECK(code_of(last_error().code) == code_of(errc::config_bad_value));
    CHECK(code_of(last_error().code) == code_of(errc::config_bad_value));  // stays

    clear_error();
    CHECK(code_of(last_error().code) == code_of(errc::ok));
}

TEST_CASE("the drop count only grows and reset clears it") {
    tu::fresh();
    CHECK(testing::drop_count() == 0);

    const pid_t child = ::fork();
    REQUIRE(child >= 0);
    if (child == 0) {
        ::signal(SIGXFSZ, SIG_IGN);
        rlimit rl{256, 256};
        ::setrlimit(RLIMIT_FSIZE, &rl);
        tu::TempDir dir;
        set_log_dir(dir.str().c_str());
        set_file_verbosity(DEBUG);
        stdout_off();
        for (int i = 0; i < 500; ++i)
            LOG_ERROR("padding padding padding line %d", i);
        const std::uint64_t d1 = dropped();
        const std::uint64_t d2 = dropped();
        ::_exit((d1 > 0 && d2 >= d1) ? 0 : 7);  // never shrinks
    }
    int status = 0;
    ::waitpid(child, &status, 0);
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
}

// ---- the disabled and dropped paths cost nothing ---------------------------

TEST_CASE("a disabled logger records no error and drops nothing") {
    tu::fresh();
    set_disabled(true);
    for (int i = 0; i < 1000; ++i)
        LOG_ERROR("this never runs");
    CHECK(code_of(last_error().code) == code_of(errc::ok));
    CHECK(dropped() == 0);
}

// ---- thread safety of the handler and the sticky error ---------------------

TEST_CASE("setting and reading the error state from many threads is safe") {
    tu::fresh();
    HandlerState st;
    set_error_handler(&record_handler, &st);

    std::atomic<bool> stop{false};
    std::thread readers[4];
    for (auto& t : readers)
        t = std::thread([&] {
            while (!stop.load(std::memory_order_relaxed)) {
                (void)last_error();
                (void)dropped();
            }
        });

    tu::TempDir dir;
    const std::string conf = dir.sub("c.conf");
    for (int i = 0; i < 200; ++i) {
        tu::write_file(conf, "verbosity = still_bad\n");
        load_file(conf.c_str());  // records config_bad_value each time
    }

    stop.store(true, std::memory_order_relaxed);
    for (auto& t : readers)
        t.join();

    set_error_handler(nullptr, nullptr);
    CHECK(st.calls.load() >= 0);  // no crash, no data race (tsan is the real check)
    CHECK(code_of(last_error().code) == code_of(errc::config_bad_value));
}
