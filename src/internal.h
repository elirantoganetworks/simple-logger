// slog internal shared types. Not installed, not part of the public API.
//
// The public header (slog/slog.h) is deliberately light. Everything that the
// source files share but users never see lives here.

#ifndef SLOG_INTERNAL_H
#define SLOG_INTERNAL_H

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "slog/slog.h"

namespace slog {
namespace detail {

// ---- constants --------------------------------------------------------

// A module whose collect level is kOff drops every record. It is the "nothing
// passes" value, chosen well below any real level so value(R) <= kOff is false.
constexpr int kOff = -2000000000;

// An output at kInherit follows the module or global level. Must equal
// slog::INHERIT.value.
constexpr int kInherit = -1000000000;

// One log line is written with a single write() call. Keeping it at or below
// PIPE_BUF (4096 on Linux) is what makes a line atomic across threads and
// processes, so lines never tear. The message is capped below that to leave
// room for the prefix and the trailing newline.
constexpr std::size_t kMaxLine = 4096;
constexpr std::size_t kMaxMsg  = 3072;

// ---- levels (level.cpp) ----------------------------------------------

// Parse a level from a config value. Accepts a known level name (any case) or a
// plain integer. Returns false if it is neither.
bool parse_level(const std::string& text, int& value_out);

// The registered name for a level value, or the value itself as text if no
// level has that value.
std::string level_name(int value);

// ---- format (format.cpp) ---------------------------------------------

// One piece of a parsed prefix pattern. A Literal carries its text; every other
// field pulls its text from the record at build time.
enum class Field {
    Literal,
    Level,
    Func,
    Msg,
    Module,
    Time,
    Elapsed,
    Pid,
    Tid,
    File,
    Line,
};

struct Token {
    Field       field;
    std::string literal;  // used only for Field::Literal
};

// A parsed prefix pattern, ready to render. Immutable once built and shared by
// value through a shared_ptr, so the emit path reads it without a lock.
struct FormatSpec {
    std::vector<Token> tokens;
    bool               needs_time     = false;  // has %t or %e, so read the clock
    std::int64_t       start_epoch_ns = 0;      // zero point for %e
};

// Parse a pattern into a FormatSpec. add_time and add_elapsed prepend %t and %e
// if the pattern does not already contain them (the enable_time/enable_elapsed
// convenience toggles). The logger owns the result and publishes a raw pointer
// to it for the lock-free read path.
std::unique_ptr<FormatSpec> parse_format(const std::string& pattern, bool add_time,
                                         bool add_elapsed, std::int64_t start_epoch_ns);

// Everything a line needs, gathered at the call site.
struct Record {
    const Level* level;
    const char*  module;  // display name, never null or empty
    SourceLoc    loc;
    const char*  msg;
    std::size_t  msg_len;
    int          pid;
    long         tid;
    std::int64_t now_epoch_ns;  // read once per record when needs_time
};

// Build one line (with trailing newline) into out. Colors the level tag when
// colored is true. Returns the number of bytes written, always <= cap, and
// always leaves a complete line (truncated with a marker if it would overflow).
std::size_t build_line(const FormatSpec& fs, const Record& rec, bool colored, char* out,
                       std::size_t cap);

// ---- sinks (sink.cpp) ------------------------------------------------

// Write all n bytes to fd, retrying on EINTR and short writes. Returns false on
// a real error (the caller then counts a drop).
bool write_all(int fd, const char* buf, std::size_t n);

// Append one line to the in-memory capture buffer used by tests.
void mem_capture_add(const char* line, std::size_t n);

// True while tests route file output to memory instead of a real file.
bool mem_capture_active();

// Turn capture off and drop every captured line. Used by testing::reset().
void mem_capture_reset();

// Print a one-line diagnostic to stderr, prefixed with "slog: ". Used for init
// and config problems. Logging itself never throws, so problems surface here.
void note(const std::string& msg);

// ---- config (config.cpp) ---------------------------------------------

// The resolved settings, after merging the three config layers. Unlike the
// public Config, this records whether each output level was set, using
// kInherit, so the resolution chain in DESIGN.md 4.11 is exact.
struct Settings {
    bool                       disabled     = false;
    int                        global_level = SLOG_LEVEL_WARNING;
    std::map<std::string, int> module_level;

    // File output.
    bool        file_enabled    = true;
    int         file_level      = kInherit;
    bool        file_per_module = false;
    std::string file_name;

    // Stdout output.
    bool                     stdout_enabled = true;
    int                      stdout_level   = kInherit;
    bool                     stdout_only    = false;
    std::vector<std::string> stdout_modules;  // empty means all modules

    // Dir and tag.
    std::string log_dir;  // empty means <cwd>/logs
    std::string run_tag;

    // Prefix and fields.
    std::string format         = "[%L][%f]: %m";
    bool        enable_time    = false;
    bool        enable_elapsed = false;
    std::string color          = "auto";  // auto | always | never

    // Flush and retention.
    int flush_level = SLOG_LEVEL_WARNING;
    int retain_runs = 0;
    int retain_days = 0;
};

using Kv = std::map<std::string, std::string>;

// Read the config file into raw key/value pairs. Returns an empty map if the
// file is missing. Bad lines are skipped with a note to stderr.
Kv read_config_file(const std::string& path);

// Read all SLOG_* environment variables into raw key/value pairs.
Kv read_env();

// The config file to load: SLOG_CONFIG if set, else ./simplelog.conf if it
// exists, else empty.
std::string find_config_path();

// Overlay one layer of key/value pairs onto settings. Later layers win because
// the caller applies them in precedence order (file, then env, then API).
void apply_kv(Settings& s, const Kv& kv);

// ---- run directory (run_dir.cpp) -------------------------------------

// The result of opening a run: an fd to the run directory (files are opened
// relative to it with openat, so a later move does not matter), and the run
// lock fd held for the whole run so retention can tell this run is alive.
struct RunDir {
    int         dir_fd      = -1;
    int         run_lock_fd = -1;
    int         first_fd    = -1;  // the first_file fd, opened under the lock
    std::string dir_path;
    bool        ok = false;
    std::string err;
};

// Rotate latest and open this run's directory. Serialized across processes by a
// file lock. Also runs a best-effort retention sweep. See DESIGN.md 10.3.
//
// If first_file is not empty, that file is created inside the run directory while
// the lock is still held. That establishes the run's presence in latest before
// the lock is released, so a run starting right after this one sees the file and
// moves it out. Its fd comes back in RunDir::first_fd.
RunDir open_run(const std::string& log_dir, const std::string& tag, int retain_runs,
                int retain_days, int pid, const std::string& timestamp,
                const std::string& first_file);

// Open (create) a log file inside the run directory. Returns the fd or -1.
int open_log_file(int dir_fd, const std::string& name);

}  // namespace detail
}  // namespace slog

#endif  // SLOG_INTERNAL_H
