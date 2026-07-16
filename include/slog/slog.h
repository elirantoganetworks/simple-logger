// slog - a small, fast logging library for Linux.
//
// This is the only header most users include. It holds the level constants,
// the log macros, and the configuration API. See docs for the full design.
//
// Quick start:
//   #include <slog/slog.h>
//   LOG_WARNING("disk almost full: %d%% used", 92);
//
// The library is Linux only and targets C++17. See DESIGN.md for the reasons
// behind every choice here.

#ifndef SLOG_SLOG_H
#define SLOG_SLOG_H

#include <atomic>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <system_error>
#include <vector>

// ---- version ----------------------------------------------------------
#define SLOG_VERSION_MAJOR 1
#define SLOG_VERSION_MINOR 1
#define SLOG_VERSION_PATCH 0
#define SLOG_VERSION_STRING "1.1.0"

// ---- compile-time level ceiling ---------------------------------------
// These integer values fix the level order: a smaller number is a lower
// verbosity (more severe). They must match slog::ERROR/WARNING/INFO/DEBUG.
#define SLOG_LEVEL_ERROR 10
#define SLOG_LEVEL_WARNING 20
#define SLOG_LEVEL_INFO 30
#define SLOG_LEVEL_DEBUG 40

// Calls to a built-in level below this ceiling are removed at build time, so
// they cost nothing at all (no branch, no argument work). The default keeps
// every built-in level; the runtime level then decides what is actually shown.
// Set this before including the header to strip verbose calls from a release
// build. It is a hard ceiling: the runtime level can only lower what is shown,
// never re-enable a level that was compiled out.
#ifndef SLOG_ACTIVE_LEVEL
#define SLOG_ACTIVE_LEVEL SLOG_LEVEL_DEBUG
#endif

// ---- per-file module --------------------------------------------------
// Define LOG_MODULE before including this header to tag every log call in the
// file with that module name. Left undefined, calls use the default module.
#ifndef LOG_MODULE
#define LOG_MODULE ""
#endif

// ---- export marker ----------------------------------------------------
// The library is built with hidden visibility, so only the functions marked
// SLOG_API are exported from the shared library. Everything else (the internal
// detail namespace and the C++ standard-library template code) stays private.
#ifndef SLOG_API
#if defined(_WIN32)
#define SLOG_API
#elif defined(__GNUC__)
#define SLOG_API __attribute__((visibility("default")))
#else
#define SLOG_API
#endif
#endif

namespace slog {

// A level is a small value type: a name plus a verbosity value. A higher value
// means a higher verbosity (DEBUG is highest). A threshold T admits a record R
// when value(R) <= value(T), which is the "X and lower" rule.
struct Level {
    int         value;
    const char* name;
};

// Built-in levels. inline constexpr so they work in constexpr contexts and in
// default member initializers, with no static-init-order concern.
inline constexpr Level ERROR{SLOG_LEVEL_ERROR, "ERROR"};
inline constexpr Level WARNING{SLOG_LEVEL_WARNING, "WARNING"};
inline constexpr Level INFO{SLOG_LEVEL_INFO, "INFO"};
inline constexpr Level DEBUG{SLOG_LEVEL_DEBUG, "DEBUG"};

// A sentinel level for an output. An output set to INHERIT follows the module
// or global level instead of fixing its own. It is the default for the file and
// stdout outputs, so per-module and global settings reach them unless the user
// sets a real level on the output. See DESIGN.md section 4.11.
inline constexpr Level INHERIT{-1000000000, "inherit"};

// Add a custom level and place it in the order by its value. A higher value is
// more verbose. Example: add_level("TRACE", 50) sits above DEBUG;
// add_level("NOTICE", 25) sits between WARNING and INFO. The returned Level can
// be used with the LOG(level, ...) macro and as a threshold in config.
SLOG_API Level add_level(const char* name, int value);

// Source location captured at the call site by the log macros.
struct SourceLoc {
    const char* file;
    int         line;
    const char* func;
};

// A per-call-site cache. One static instance lives inside each macro
// expansion. The module state pointer it caches is stable for the life of the
// process, so it is resolved once and then read lock-free.
struct CallSite {
    const char*        module;
    std::atomic<void*> state;  // resolved ModuleState*, null until first use
};

// Hot-path filter check. Cheap: a global disable load, then (once resolved) one
// atomic load of the module collect level and one integer compare. Returns
// true when the record could reach at least one output. noexcept: a log call is
// called from anywhere (destructors, catch blocks), so it never throws. A rare
// internal failure (an allocation on first use of a module) is caught, recorded,
// and the call returns false.
SLOG_API bool should_log(CallSite& cs, const Level& level) noexcept;

// Emit a record. Called by the macros only after should_log returned true. The
// printf format attribute lets GCC and Clang check the format string. noexcept:
// a runtime failure is recorded (see the error API below), never thrown.
SLOG_API void emit(CallSite& cs, const Level& level, SourceLoc loc, const char* fmt, ...) noexcept
    __attribute__((format(printf, 4, 5)));

// Emit for a module named at the call site (used by LOG_TO). Resolves the module
// each call, so the module value may vary.
SLOG_API void emit_to(const char* module, const Level& level, SourceLoc loc, const char* fmt,
                      ...) noexcept __attribute__((format(printf, 4, 5)));

// Cheap guard for building expensive log arguments by hand:
//   if (slog::is_enabled("net", slog::DEBUG)) { ... }
SLOG_API bool is_enabled(const char* module, const Level& level) noexcept;

// Monotonic clock in nanoseconds, used by the rate-limit macros. Declared here
// so the header does not need to include <chrono>.
SLOG_API std::int64_t mono_nanos() noexcept;

// ---- errors -----------------------------------------------------------
// slog has two phases. Config and init run on the user's stack, so init() throws
// on a hard setup failure (see below). Steady-state logging is called from
// anywhere and never throws; a runtime failure is recorded and reaches the user
// three ways: a sticky last_error(), an optional handler, and a dropped() count.
// A one-line note also goes to stderr as a last resort.

// Every failure the library can report. The numbers are grouped by area and are
// frozen once released: a value never changes and is never reused. 0 is success.
enum class errc {
    ok = 0,

    // Config (10-19): a value or the config file was bad. The default is kept.
    config_file_read  = 10,  // the config file could not be read
    config_bad_value  = 11,  // a config value did not parse; kept the default
    config_unknown_key = 12,  // an unknown config key was seen and skipped

    // Run directory setup (20-29). init() throws these; a lazy first log records
    // them and falls back to stdout only.
    dir_create   = 20,  // could not create the log directory
    run_lock     = 21,  // could not create or lock the run lock file
    run_dir_open = 22,  // could not open the run directory

    // File output (30-39). Recorded on the log path, never thrown.
    file_open  = 30,  // could not open a log file
    file_write = 31,  // a write to a log file failed; the line was dropped

    // Stdout output (40-49).
    stdout_write = 40,  // a write to stdout failed; the line was dropped

    // Internal (50-59).
    out_of_memory     = 50,  // an allocation failed; the line was dropped
    message_truncated = 51,  // a message was too long and was cut to fit
};

// The error category, so an errc becomes a std::error_code with a message.
SLOG_API const std::error_category& error_category() noexcept;
SLOG_API std::error_code            make_error_code(errc e) noexcept;

// The one exception type. init() throws this on a hard setup failure. It is a
// std::system_error, so it carries the code (.code()) and a message (.what()).
// Catch this one type to catch every slog setup failure.
class SLOG_API error : public std::system_error {
   public:
    error(errc e, const std::string& what) : std::system_error(make_error_code(e), what) {}
};

// The detail of one failure, passed to the handler and returned by last_error().
// It holds no owning strings, so copying it never allocates: message points at a
// static string for the code, and detail is a fixed buffer, cut if it is long.
struct error_info {
    errc        code      = errc::ok;
    int         sys_errno = 0;   // errno at the failure, or 0 if none
    const char* message   = "";  // a short, static message for the code
    char        detail[200] = {};  // the path, key, or module involved (may be cut)
};

// A handler the user registers, called once per failure with the failure detail
// and the user pointer given to set_error_handler. It must not throw, must
// return promptly, and must not log through slog (a recursion guard drops a
// nested call). An exception from it is swallowed in a build with exceptions.
using error_handler = void (*)(const error_info& info, void* user);

// Install (or clear, with nullptr) the error handler and its user pointer.
SLOG_API void set_error_handler(error_handler handler, void* user) noexcept;

// The last failure recorded, or code == errc::ok if none. Thread safe: it
// returns a copy taken under a lock.
SLOG_API error_info last_error() noexcept;

// Clear the sticky last error back to ok.
SLOG_API void clear_error() noexcept;

// The number of log lines dropped by a failed write or a failed allocation since
// the process started. Never resets on its own.
SLOG_API std::uint64_t dropped() noexcept;

// ---- configuration ----------------------------------------------------

// Settings for one output (used for stdout). The defaults are the stdout
// defaults: on, following the module or global level (so WARNING by default),
// "and lower" mode, all modules.
struct OutputConfig {
    bool                     enabled   = true;
    Level                    verbosity = INHERIT;  // own level, or INHERIT
    bool                     only      = false;    // exact level, not "and lower"
    std::vector<std::string> modules;              // empty means all modules
};

// The whole logger config. Passing this to configure() sets every field, so use
// the single-setting helpers below for a targeted change.
struct Config {
    bool                       disabled  = false;
    Level                      verbosity = WARNING;  // global default threshold
    std::map<std::string, int> module_verbosity;     // per module, by level value

    std::string log_dir;  // empty means <cwd>/logs
    std::string run_tag;  // empty means no tag

    // File output. INHERIT means follow the module or global level.
    bool        file_enabled    = true;
    Level       file_verbosity  = INHERIT;
    bool        file_per_module = false;
    std::string file_name;  // empty means <pid>-<timestamp>.log

    // Stdout output.
    OutputConfig out;

    // Prefix and fields.
    std::string format         = "[%L][%f]: %m";
    bool        enable_time    = false;
    bool        enable_elapsed = false;
    std::string color          = "auto";  // auto | always | never

    // Flush and retention.
    Level flush_level = WARNING;  // flush on this level and more severe
    int   retain_runs = 0;        // 0 keeps all
    int   retain_days = 0;        // 0 means no age limit
};

// Apply a whole config. Highest precedence. Call it (and the dir, tag and file
// setters) before the first log call, since files open on first use.
SLOG_API void configure(const Config& c);

// Single-setting helpers. All are highest precedence, same as configure().
SLOG_API void set_verbosity(const Level& level);
SLOG_API void set_module_verbosity(const char* module, const Level& level);
SLOG_API void set_disabled(bool off);
SLOG_API void set_log_dir(const char* path);
SLOG_API void set_run_tag(const char* tag);

SLOG_API void set_stdout_verbosity(const Level& level);  // turns stdout on at this level
SLOG_API void set_stdout_only(bool only);
SLOG_API void set_stdout_modules(const std::vector<std::string>& modules);
SLOG_API void stdout_off();

SLOG_API void set_file_verbosity(const Level& level);
SLOG_API void set_file_per_module(bool per_module);
SLOG_API void set_file_name(const char* name);
SLOG_API void file_off();

SLOG_API void set_format(const char* pattern);
SLOG_API void enable_time(bool on);
SLOG_API void enable_elapsed(bool on);
SLOG_API void set_flush_level(const Level& level);

// Lifecycle.
// Open the run now, instead of on the first log. This is the setup phase, so it
// throws slog::error on a hard failure (the log directory cannot be made, the
// run lock cannot be taken, the run directory cannot be opened). A build without
// exceptions records the failure instead (see the error API). If you never call
// init(), the first log opens the run lazily and never throws: a failure there
// falls back to stdout only and is recorded.
SLOG_API void init();

SLOG_API void flush() noexcept;  // flush all sinks now; safe while other threads log

// Flush and close the sinks. Call it only when no other thread is logging (it
// closes the file descriptors). It is not required: the tail is flushed
// automatically at exit, and the OS closes the files then.
SLOG_API void shutdown() noexcept;

SLOG_API void install_crash_handler() noexcept;  // best-effort flush on a fatal signal

// Open fresh, PID-named files for a forked child. Call it from the child before
// it logs, and only from the child's single (post-fork) thread.
SLOG_API void restart_after_fork() noexcept;

// Explicit config loads. Normally the first use does file then env for you.
SLOG_API void load_file(const char* path);
SLOG_API void load_env();

// Library version string, e.g. "1.1.0".
SLOG_API const char* version() noexcept;

}  // namespace slog

// Let a slog::errc build a std::error_code directly.
namespace std {
template <>
struct is_error_code_enum<::slog::errc> : true_type {};
}  // namespace std

// ---- log macros -------------------------------------------------------
// SLOG_EMIT is the shared body for a fixed-module call site. It caches the call
// site, checks the level, and only then evaluates the message arguments and
// calls emit(). The level is bound once (so a level expression with a cost or a
// side effect is evaluated a single time). MODULE must be constant per call
// site; it is captured once when the static call site is first initialized. Use
// LOG_TO for a module that varies at a call site.
#define SLOG_EMIT(MODULE, LEVEL_OBJ, ...)                                                \
    do {                                                                                 \
        static ::slog::CallSite _slog_cs{(MODULE), nullptr};                             \
        const ::slog::Level     _slog_lvl = (LEVEL_OBJ);                                 \
        if (::slog::should_log(_slog_cs, _slog_lvl))                                     \
            ::slog::emit(_slog_cs, _slog_lvl,                                            \
                         ::slog::SourceLoc{__FILE__, __LINE__, __func__}, __VA_ARGS__);  \
    } while (0)

// Built-in level macros. The compile-time ceiling removes a call below it
// completely. The canonical names are SLOG_*; short LOG_* aliases follow.
#if SLOG_LEVEL_ERROR <= SLOG_ACTIVE_LEVEL
#define SLOG_ERROR(...) SLOG_EMIT(LOG_MODULE, ::slog::ERROR, __VA_ARGS__)
#else
#define SLOG_ERROR(...) ((void)0)
#endif

#if SLOG_LEVEL_WARNING <= SLOG_ACTIVE_LEVEL
#define SLOG_WARNING(...) SLOG_EMIT(LOG_MODULE, ::slog::WARNING, __VA_ARGS__)
#else
#define SLOG_WARNING(...) ((void)0)
#endif

#if SLOG_LEVEL_INFO <= SLOG_ACTIVE_LEVEL
#define SLOG_INFO(...) SLOG_EMIT(LOG_MODULE, ::slog::INFO, __VA_ARGS__)
#else
#define SLOG_INFO(...) ((void)0)
#endif

#if SLOG_LEVEL_DEBUG <= SLOG_ACTIVE_LEVEL
#define SLOG_DEBUG(...) SLOG_EMIT(LOG_MODULE, ::slog::DEBUG, __VA_ARGS__)
#else
#define SLOG_DEBUG(...) ((void)0)
#endif

// Custom level. Always compiled and gated at runtime only, since a custom level
// value is not known at compile time. LEVEL is a slog::Level.
#define SLOG_LOG(LEVEL, ...) SLOG_EMIT(LOG_MODULE, (LEVEL), __VA_ARGS__)

// Explicit module for one call. Unlike the fixed-module macros, MODULE may be a
// value that varies at this call site: it is resolved on every call (a small
// lookup, fine for the occasional override), not cached. Runtime gated only.
#define SLOG_LOG_TO(MODULE, LEVEL, ...)                                                  \
    do {                                                                                 \
        const char*         _slog_mod = (MODULE);                                        \
        const ::slog::Level _slog_lvl = (LEVEL);                                         \
        if (::slog::is_enabled(_slog_mod, _slog_lvl))                                    \
            ::slog::emit_to(_slog_mod, _slog_lvl,                                        \
                            ::slog::SourceLoc{__FILE__, __LINE__, __func__},             \
                            __VA_ARGS__);                                                \
    } while (0)

// Rate limits. Each keeps a per-call-site counter, so limits are per process and
// per call site, never across processes. N must be 1 or more.
#define SLOG_LOG_EVERY_N(LEVEL, N, ...)                                                  \
    do {                                                                                 \
        static ::std::atomic<::std::uint64_t> _slog_n{0};                                \
        if ((N) > 0 && _slog_n.fetch_add(1, ::std::memory_order_relaxed) %               \
                               static_cast<::std::uint64_t>(N) ==                        \
                           0)                                                            \
            SLOG_EMIT(LOG_MODULE, (LEVEL), __VA_ARGS__);                                 \
    } while (0)

#define SLOG_LOG_FIRST_N(LEVEL, N, ...)                                                  \
    do {                                                                                 \
        static ::std::atomic<::std::uint64_t> _slog_n{0};                                \
        if ((N) > 0 && _slog_n.fetch_add(1, ::std::memory_order_relaxed) <               \
                           static_cast<::std::uint64_t>(N))                              \
            SLOG_EMIT(LOG_MODULE, (LEVEL), __VA_ARGS__);                                 \
    } while (0)

#define SLOG_LOG_ONCE(LEVEL, ...) SLOG_LOG_FIRST_N((LEVEL), 1, __VA_ARGS__)

// Log at most once per SEC seconds at this call site. SEC may be fractional.
// The first call always fires: the counter starts at a sentinel so the check
// does not depend on the monotonic clock's starting value.
#define SLOG_LOG_EVERY_N_SEC(LEVEL, SEC, ...)                                            \
    do {                                                                                 \
        static ::std::atomic<::std::int64_t> _slog_last{INT64_MIN};                      \
        const ::std::int64_t                 _slog_now = ::slog::mono_nanos();           \
        ::std::int64_t       _slog_prev = _slog_last.load(::std::memory_order_relaxed);  \
        const ::std::int64_t _slog_gap =                                                 \
            static_cast<::std::int64_t>((SEC) * 1000000000.0);                           \
        const bool _slog_first = _slog_prev == INT64_MIN;                                \
        if ((_slog_first || _slog_now - _slog_prev >= _slog_gap) &&                      \
            _slog_last.compare_exchange_strong(_slog_prev, _slog_now,                    \
                                               ::std::memory_order_relaxed))             \
            SLOG_EMIT(LOG_MODULE, (LEVEL), __VA_ARGS__);                                 \
    } while (0)

// Short aliases. Suppressed by SLOG_NO_SHORT_MACROS for code that already has a
// LOG macro (for example glog); the SLOG_* names above are always available.
#ifndef SLOG_NO_SHORT_MACROS
#define LOG_ERROR(...) SLOG_ERROR(__VA_ARGS__)
#define LOG_WARNING(...) SLOG_WARNING(__VA_ARGS__)
#define LOG_INFO(...) SLOG_INFO(__VA_ARGS__)
#define LOG_DEBUG(...) SLOG_DEBUG(__VA_ARGS__)
#define LOG(LEVEL, ...) SLOG_LOG((LEVEL), __VA_ARGS__)
#define LOG_TO(MODULE, LEVEL, ...) SLOG_LOG_TO((MODULE), (LEVEL), __VA_ARGS__)
#define LOG_EVERY_N(LEVEL, N, ...) SLOG_LOG_EVERY_N((LEVEL), (N), __VA_ARGS__)
#define LOG_FIRST_N(LEVEL, N, ...) SLOG_LOG_FIRST_N((LEVEL), (N), __VA_ARGS__)
#define LOG_ONCE(LEVEL, ...) SLOG_LOG_ONCE((LEVEL), __VA_ARGS__)
#define LOG_EVERY_N_SEC(LEVEL, SEC, ...) SLOG_LOG_EVERY_N_SEC((LEVEL), (SEC), __VA_ARGS__)
#endif

#endif  // SLOG_SLOG_H
