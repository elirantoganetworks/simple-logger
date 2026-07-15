// Error codes, the sticky last error, and the user handler.
//
// slog reports a failure on three channels at once: a sticky last_error(), an
// optional handler the user set, and a dropped() count. Everything here is
// allocation-free, so it is safe on the log path and even when the failure is
// out of memory.
//
// record() sets the sticky error and writes a one-time note to stderr. It never
// runs user code, so it is safe to call while the logger mutex is held. report()
// adds the handler call, so it is called only where that mutex is not held (the
// dispatch path and the emit catch blocks). This split is what keeps a handler
// that logs from deadlocking on the logger mutex.

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <system_error>

#include "internal.h"
#include "slog/slog.h"

namespace slog {
namespace {

class Category : public std::error_category {
   public:
    const char* name() const noexcept override { return "slog"; }
    std::string message(int ev) const override {
        return detail::message_for(static_cast<errc>(ev));
    }
};

std::mutex& err_mu() {
    static std::mutex m;
    return m;
}
error_info& sticky() {
    static error_info e;  // guarded by err_mu()
    return e;
}
error_handler g_handler = nullptr;  // guarded by err_mu()
void*         g_user    = nullptr;  // guarded by err_mu()

// A per-thread guard so a handler that logs (and thus fails and reports) does not
// call itself again in a loop.
thread_local int t_in_handler = 0;

std::atomic<std::uint64_t> g_drops{0};

// One stderr note per code, so a repeated failure (a full disk) does not flood
// stderr. A fixed array keyed by a compact index keeps this allocation-free.
constexpr int     kNoteSlots = 16;
std::atomic<bool> g_noted[kNoteSlots];

int note_slot(errc c) {
    switch (c) {
        case errc::ok: return 0;
        case errc::config_file_read: return 1;
        case errc::config_bad_value: return 2;
        case errc::config_unknown_key: return 3;
        case errc::dir_create: return 4;
        case errc::run_lock: return 5;
        case errc::run_dir_open: return 6;
        case errc::file_open: return 7;
        case errc::file_write: return 8;
        case errc::stdout_write: return 9;
        case errc::out_of_memory: return 10;
        case errc::message_truncated: return 11;
    }
    return 12;
}

}  // namespace

const std::error_category& error_category() noexcept {
    static Category c;
    return c;
}

std::error_code make_error_code(errc e) noexcept {
    return std::error_code(static_cast<int>(e), error_category());
}

void set_error_handler(error_handler handler, void* user) noexcept {
    std::lock_guard<std::mutex> lk(err_mu());
    g_handler = handler;
    g_user    = user;
}

error_info last_error() noexcept {
    std::lock_guard<std::mutex> lk(err_mu());
    return sticky();
}

void clear_error() noexcept {
    std::lock_guard<std::mutex> lk(err_mu());
    sticky() = error_info{};
}

std::uint64_t dropped() noexcept {
    return g_drops.load(std::memory_order_relaxed);
}

namespace detail {

const char* message_for(errc code) {
    switch (code) {
        case errc::ok: return "ok";
        case errc::config_file_read: return "could not read the config file";
        case errc::config_bad_value: return "a config value did not parse, kept the default";
        case errc::config_unknown_key: return "unknown config key, skipped it";
        case errc::dir_create: return "could not create the log directory";
        case errc::run_lock: return "could not create or lock the run lock file";
        case errc::run_dir_open: return "could not open the run directory";
        case errc::file_open: return "could not open a log file";
        case errc::file_write: return "a write to a log file failed, the line was dropped";
        case errc::stdout_write: return "a write to stdout failed, the line was dropped";
        case errc::out_of_memory: return "an allocation failed, the line was dropped";
        case errc::message_truncated: return "a message was too long and was cut to fit";
    }
    return "unknown error";
}

void count_drop() {
    g_drops.fetch_add(1, std::memory_order_relaxed);
}
std::uint64_t drop_count() {
    return g_drops.load(std::memory_order_relaxed);
}

void reset_errors() {
    {
        std::lock_guard<std::mutex> lk(err_mu());
        sticky() = error_info{};
    }
    g_drops.store(0, std::memory_order_relaxed);
    for (int i = 0; i < kNoteSlots; ++i) g_noted[i].store(false, std::memory_order_relaxed);
}

// Set the sticky error and note it once to stderr. No user code runs here, so it
// is safe with the logger mutex held.
void record(errc code, int err, const char* what) {
    error_info info;
    info.code      = code;
    info.sys_errno = err;
    info.message   = message_for(code);
    std::size_t n  = 0;
    if (what != nullptr)
        for (; what[n] != '\0' && n + 1 < sizeof(info.detail); ++n) info.detail[n] = what[n];
    info.detail[n] = '\0';

    {
        std::lock_guard<std::mutex> lk(err_mu());
        sticky() = info;
    }

    const int slot = note_slot(code);
    if (slot >= 0 && slot < kNoteSlots &&
        !g_noted[slot].exchange(true, std::memory_order_relaxed)) {
        // errno is left as a number, not a string: strerror is not thread safe,
        // and the number is in error_info.sys_errno for the handler to format.
        if (err != 0 && info.detail[0] != '\0')
            std::fprintf(stderr, "slog: %s (%s): errno %d\n", info.message, info.detail, err);
        else if (err != 0)
            std::fprintf(stderr, "slog: %s: errno %d\n", info.message, err);
        else if (info.detail[0] != '\0')
            std::fprintf(stderr, "slog: %s: %s\n", info.message, info.detail);
        else
            std::fprintf(stderr, "slog: %s\n", info.message);
    }
}

// record() plus the handler call. Call it only where the logger mutex is not
// held, so a handler that logs cannot deadlock on it.
void report(errc code, int err, const char* what) {
    record(code, err, what);

    error_handler h = nullptr;
    void*         u = nullptr;
    {
        std::lock_guard<std::mutex> lk(err_mu());
        h = g_handler;
        u = g_user;
    }
    if (h == nullptr || t_in_handler != 0)
        return;

    error_info info = last_error();  // the copy the handler sees
    t_in_handler    = 1;
#if SLOG_HAS_EXCEPTIONS
    try {
        h(info, u);
    } catch (...) {
        // A handler must not throw. Swallow it so the log path stays noexcept.
    }
#else
    h(info, u);
#endif
    t_in_handler = 0;
}

}  // namespace detail
}  // namespace slog
