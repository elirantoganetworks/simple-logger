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
#include <cstdint>
#include <map>
#include <string>
#include <vector>

// ---- version ----------------------------------------------------------
#define SLOG_VERSION_MAJOR 1
#define SLOG_VERSION_MINOR 0
#define SLOG_VERSION_PATCH 0
#define SLOG_VERSION_STRING "1.0.0"

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
Level add_level(const char* name, int value);

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
// true when the record could reach at least one output.
bool should_log(CallSite& cs, const Level& level);

// Emit a record. Called by the macros only after should_log returned true. The
// printf format attribute lets GCC and Clang check the format string.
void emit(CallSite& cs, const Level& level, SourceLoc loc, const char* fmt, ...)
    __attribute__((format(printf, 4, 5)));

// Emit for a module named at the call site (used by LOG_TO). Resolves the module
// each call, so the module value may vary.
void emit_to(const char* module, const Level& level, SourceLoc loc, const char* fmt, ...)
    __attribute__((format(printf, 4, 5)));

// Cheap guard for building expensive log arguments by hand:
//   if (slog::is_enabled("net", slog::DEBUG)) { ... }
bool is_enabled(const char* module, const Level& level);

// Monotonic clock in nanoseconds, used by the rate-limit macros. Declared here
// so the header does not need to include <chrono>.
std::int64_t mono_nanos();

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
void configure(const Config& c);

// Single-setting helpers. All are highest precedence, same as configure().
void set_verbosity(const Level& level);
void set_module_verbosity(const char* module, const Level& level);
void set_disabled(bool off);
void set_log_dir(const char* path);
void set_run_tag(const char* tag);

void set_stdout_verbosity(const Level& level);  // turns stdout on at this level
void set_stdout_only(bool only);
void set_stdout_modules(const std::vector<std::string>& modules);
void stdout_off();

void set_file_verbosity(const Level& level);
void set_file_per_module(bool per_module);
void set_file_name(const char* name);
void file_off();

void set_format(const char* pattern);
void enable_time(bool on);
void enable_elapsed(bool on);
void set_flush_level(const Level& level);

// Lifecycle.
void init();   // open files now (optional; done on first log)
void flush();  // flush all sinks now; safe to call while other threads log

// Flush and close the sinks. Call it only when no other thread is logging (it
// closes the file descriptors). It is not required: the tail is flushed
// automatically at exit, and the OS closes the files then.
void shutdown();

void install_crash_handler();  // best-effort flush on a fatal signal

// Open fresh, PID-named files for a forked child. Call it from the child before
// it logs, and only from the child's single (post-fork) thread.
void restart_after_fork();

// Explicit config loads. Normally the first use does file then env for you.
void load_file(const char* path);
void load_env();

// Library version string, e.g. "1.0.0".
const char* version();

}  // namespace slog

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
