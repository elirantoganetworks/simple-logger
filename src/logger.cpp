// The logger core: global state, the hot path (should_log and emit), dispatch to
// outputs, the public configuration API, and the fork and crash handlers.
//
// The design keeps the filtered path lock-free. A log call that is dropped only
// reads a couple of atomics. A call that is kept formats into a thread-local
// buffer and writes each line with one write() syscall, which is what keeps
// lines whole across threads and processes. A mutex guards only the rare things:
// config changes, opening files, and the retention sweep.

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#include "internal.h"
#include "slog/slog.h"
#include "slog/testing.h"

namespace slog {
namespace {

using detail::kInherit;
using detail::kOff;

// ---- clocks -----------------------------------------------------------
using NanosFn = std::int64_t (*)();

std::int64_t default_wall() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<std::int64_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}
std::int64_t default_mono() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::int64_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

std::atomic<NanosFn> g_wall{&default_wall};
std::atomic<NanosFn> g_mono{&default_mono};

std::int64_t wall_now() {
    return g_wall.load(std::memory_order_relaxed)();
}

// ---- pid and tid caches ----------------------------------------------
// Cached so a log call does not pay a syscall each time. The fork child handler
// resets them so a child never logs a stale id.
std::atomic<int> g_pid{0};

int cached_pid() {
    int p = g_pid.load(std::memory_order_relaxed);
    if (p == 0) {
        p = static_cast<int>(::getpid());
        g_pid.store(p, std::memory_order_relaxed);
    }
    return p;
}

thread_local long t_tid = 0;
long              cached_tid() {
    if (t_tid == 0)
        t_tid = ::syscall(SYS_gettid);
    return t_tid;
}

// ---- global disable ---------------------------------------------------
// Checked first on the hot path. When set, a log call stops before it even
// resolves its module, so a disabled logger collects nothing.
std::atomic<int> g_disabled{0};

// Counts flush syncs and dropped lines, for the test hooks. Off the hot path:
// a flush already does an fdatasync syscall, and a drop is a rare error.
std::atomic<std::uint64_t> g_flush_count{0};
std::atomic<std::uint64_t> g_drop_count{0};

// ---- crash handler fd table ------------------------------------------
// The signal handler may only touch async-signal-safe state, so open file fds
// are mirrored into this fixed table. The handler writes a note and syncs them.
constexpr int    kMaxCrashFds = 64;
std::atomic<int> g_crash_fds[kMaxCrashFds];
std::atomic<int> g_crash_fd_count{0};

void register_crash_fd(int fd) {
    // Called only under mu_, so the count is not contended here. Publish the fd
    // BEFORE bumping the count, both with release, so the async crash handler
    // (which loads the count with acquire) never sees a counted-but-unwritten
    // slot and writes the crash note to the wrong fd.
    const int i = g_crash_fd_count.load(std::memory_order_relaxed);
    if (i >= kMaxCrashFds)
        return;
    g_crash_fds[i].store(fd, std::memory_order_release);
    g_crash_fd_count.store(i + 1, std::memory_order_release);
}
void clear_crash_fds() {
    g_crash_fd_count.store(0, std::memory_order_release);
}

void crash_handler(int sig) {
    static const char kMsg[] = "slog: fatal signal, flushing logs\n";
    int               cnt    = g_crash_fd_count.load(std::memory_order_acquire);
    if (cnt > kMaxCrashFds)
        cnt = kMaxCrashFds;
    for (int i = 0; i < cnt; ++i) {
        const int fd = g_crash_fds[i].load(std::memory_order_acquire);
        if (fd >= 0) {
            const ssize_t r = ::write(fd, kMsg, sizeof(kMsg) - 1);
            (void)r;
            ::fdatasync(fd);
        }
    }
    // Restore the default action and re-raise, so the OS still makes a core dump
    // and reports the right exit status.
    ::signal(sig, SIG_DFL);
    ::raise(sig);
}

// ---- per-module state -------------------------------------------------
// One of these per distinct module name. Pointers are stable for the life of
// the process, so a call site caches one and then reads it lock-free.
struct ModuleState {
    std::string       name;     // the module key, "" for the default module
    std::string       display;  // name shown in files and %M ("default" if empty)
    std::atomic<int>  collect_level{kOff};
    std::atomic<int>  file_level{kOff};
    std::atomic<int>  stdout_level{kOff};
    std::atomic<bool> stdout_only{false};
    std::atomic<int>  file_fd{-1};  // -1 not opened, -2 open failed, else the fd
};

std::string sanitize(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
        out.push_back(ok ? c : '_');
    }
    return out;
}

// Make a run tag safe to use as one path and file-name part. sanitize() turns a
// slash (and any other odd byte) into '_', so the tag can never add a path
// level. A tag of only dots ("." or "..") would still name the current or parent
// directory, so those become underscores. Without this a tag could send a run
// outside the log dir or silently break file output.
std::string sanitize_tag(const std::string& tag) {
    if (tag.empty())
        return tag;
    std::string t = sanitize(tag);
    if (t.find_first_not_of('.') == std::string::npos)
        t = std::string(t.size(), '_');
    return t;
}

std::string format_timestamp(std::int64_t epoch_ns) {
    const std::time_t secs = epoch_ns / 1000000000;
    std::tm           tmv;
    localtime_r(&secs, &tmv);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d%02d%02d-%02d%02d%02d", tmv.tm_year + 1900,
                  tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    return buf;
}

// ---- the logger -------------------------------------------------------
struct Logger {
    std::mutex mu_;

    // Config layers, merged in precedence order file < env < api.
    detail::Kv file_kv_;
    detail::Kv env_kv_;
    detail::Kv api_kv_;

    detail::Settings                                    settings_;
    std::map<std::string, std::unique_ptr<ModuleState>> modules_;

    // The prefix format is published as a raw atomic pointer so the emit path
    // reads it with no lock and no libstdc++ shared_ptr spinlock (which a fork
    // could otherwise inherit locked). The specs_ vector owns every published
    // spec for the life of the process, so an in-flight reader's pointer never
    // dangles. Config changes are rare, so this list stays short.
    std::atomic<const detail::FormatSpec*>                 fmt_ptr_{nullptr};
    std::vector<std::unique_ptr<const detail::FormatSpec>> specs_;
    // A new spec is parsed only when its inputs are new. Without this, every
    // config change (even one that does not touch the format) would allocate and
    // keep a spec forever, so a program that changes the level at runtime would
    // grow without bound. The cache keys on the format inputs, so the retained
    // set is bounded by the number of distinct formats the program ever uses.
    std::map<std::string, const detail::FormatSpec*> spec_cache_;

    std::atomic<int>  flush_level_{SLOG_LEVEL_WARNING};
    std::atomic<bool> stdout_color_{false};

    std::int64_t start_epoch_ns_ = 0;

    // File state. Opened lazily on the first line that needs a real file.
    int              init_state_  = 0;  // 0 not tried, 1 ok, 2 failed
    int              run_dir_fd_  = -1;
    int              run_lock_fd_ = -1;
    int              shared_fd_   = -1;  // single-file mode
    std::string      timestamp_;
    std::vector<int> open_fds_;
    bool             closed_ = false;

    Logger() {
        start_epoch_ns_ = wall_now();
        file_kv_        = detail::read_config_file(detail::find_config_path());
        env_kv_         = detail::read_env();
        reload_locked();  // no other thread yet, safe without the mutex
        pthread_atfork(&Logger::atfork_prepare, &Logger::atfork_parent,
                       &Logger::atfork_child);
        // No separate atexit handler: the destructor below flushes at exit, and a
        // separate handler could run after this singleton is destroyed and touch
        // freed members.
    }

    static Logger& instance() {
        static Logger inst;
        return inst;
    }

    // Rebuild settings_ from the layers, then publish the format spec and every
    // module. Caller holds mu_ (or is the constructor).
    void reload_locked() {
        detail::Settings s;
        detail::Kv       merged = file_kv_;
        for (const auto& kv : env_kv_) merged[kv.first] = kv.second;
        for (const auto& kv : api_kv_) merged[kv.first] = kv.second;
        detail::apply_kv(s, merged);
        settings_ = s;

        g_disabled.store(settings_.disabled ? 1 : 0, std::memory_order_relaxed);
        flush_level_.store(settings_.flush_level, std::memory_order_relaxed);
        stdout_color_.store(resolve_color(), std::memory_order_relaxed);

        // Reuse a spec for a format we have already parsed. The key holds every
        // input parse_format depends on. This keeps repeated config changes from
        // leaking a spec each time (see spec_cache_).
        const std::string key = settings_.format + '\x01' +
                                (settings_.enable_time ? '1' : '0') +
                                (settings_.enable_elapsed ? '1' : '0') + '\x01' +
                                std::to_string(start_epoch_ns_);
        const detail::FormatSpec* raw = nullptr;
        const auto                cached = spec_cache_.find(key);
        if (cached != spec_cache_.end()) {
            raw = cached->second;
        } else {
            std::unique_ptr<const detail::FormatSpec> spec =
                detail::parse_format(settings_.format, settings_.enable_time,
                                     settings_.enable_elapsed, start_epoch_ns_);
            raw = spec.get();
            specs_.push_back(std::move(spec));  // keep it alive for lock-free readers
            spec_cache_[key] = raw;
        }
        fmt_ptr_.store(raw, std::memory_order_release);

        for (auto& kv : modules_) compute_module(*kv.second);
    }

    bool resolve_color() const {
        if (settings_.color == "always")
            return true;
        if (settings_.color == "never")
            return false;
        return ::isatty(STDOUT_FILENO) != 0;  // "auto"
    }

    bool stdout_selects(const std::string& module) const {
        if (settings_.stdout_modules.empty())
            return true;
        for (const std::string& m : settings_.stdout_modules)
            if (m == module)
                return true;
        return false;
    }

    // Compute a module's thresholds from the current settings. See the chain in
    // DESIGN.md 4.11.
    void compute_module(ModuleState& ms) {
        const auto it = settings_.module_level.find(ms.name);
        const int  base =
            it != settings_.module_level.end() ? it->second : settings_.global_level;
        int file_lvl = kOff;
        if (settings_.file_enabled)
            file_lvl = settings_.file_level == kInherit ? base : settings_.file_level;

        int stdout_lvl = kOff;
        if (settings_.stdout_enabled && stdout_selects(ms.name))
            stdout_lvl =
                settings_.stdout_level == kInherit ? base : settings_.stdout_level;

        ms.file_level.store(file_lvl, std::memory_order_relaxed);
        ms.stdout_level.store(stdout_lvl, std::memory_order_relaxed);
        ms.stdout_only.store(settings_.stdout_only, std::memory_order_relaxed);
        ms.collect_level.store(file_lvl > stdout_lvl ? file_lvl : stdout_lvl,
                               std::memory_order_relaxed);
    }

    ModuleState* resolve_module(const char* module) {
        const std::string           key = module != nullptr ? module : "";
        std::lock_guard<std::mutex> lock(mu_);
        auto                        it = modules_.find(key);
        if (it != modules_.end())
            return it->second.get();
        auto ms     = std::make_unique<ModuleState>();
        ms->name    = key;
        ms->display = key.empty() ? "default" : key;
        compute_module(*ms);
        ModuleState* raw = ms.get();
        modules_.emplace(key, std::move(ms));
        return raw;
    }

    void reload() {
        std::lock_guard<std::mutex> lock(mu_);
        reload_locked();
    }

    // Open the run directory once. Returns true if file output is usable.
    bool ensure_run_open() {
        if (init_state_ == 1)
            return true;
        if (init_state_ == 2)
            return false;
        std::string dir = settings_.log_dir.empty() ? "logs" : settings_.log_dir;
        // The default <project-dir>/logs is <cwd>/logs; "logs" is relative to cwd.
        timestamp_ = format_timestamp(wall_now());
        // For single-file mode, create the shared file under the rotation lock, so
        // the run has a presence in latest before the lock is released. Per-module
        // files are opened lazily as modules appear.
        const std::string first_file =
            settings_.file_per_module ? std::string() : make_filename("default");
        detail::RunDir rd =
            detail::open_run(dir, sanitize_tag(settings_.run_tag), settings_.retain_runs,
                             settings_.retain_days, cached_pid(), timestamp_, first_file);
        if (!rd.ok) {
            detail::note(rd.err + "; file output is off for this run");
            init_state_ = 2;
            return false;
        }
        run_dir_fd_  = rd.dir_fd;
        run_lock_fd_ = rd.run_lock_fd;
        if (!settings_.file_per_module) {
            if (rd.first_fd >= 0) {
                shared_fd_ = rd.first_fd;
                open_fds_.push_back(shared_fd_);
                register_crash_fd(shared_fd_);
            } else {
                shared_fd_ = -2;
            }
        }
        init_state_ = 1;
        return true;
    }

    std::string make_filename(const std::string& module_display) {
        std::string base = settings_.file_name;
        std::string ext  = ".log";
        std::string stem;
        if (base.empty()) {
            stem = std::to_string(cached_pid()) + "-" + timestamp_;
        } else {
            const std::size_t dot = base.find_last_of('.');
            if (dot != std::string::npos) {
                stem = base.substr(0, dot);
                ext  = base.substr(dot);
            } else {
                stem = base;
                ext  = "";
            }
        }
        if (settings_.file_per_module)
            stem += "-" + sanitize(module_display);
        std::string name = stem + ext;
        if (!settings_.run_tag.empty())
            name = sanitize_tag(settings_.run_tag) + "-" + name;
        return name;
    }

    // Return this module's file fd, opening it if needed. -2 means "gave up".
    int ensure_file_open(ModuleState& ms) {
        int fd = ms.file_fd.load(std::memory_order_acquire);
        if (fd != -1)
            return fd;  // already opened or already failed
        std::lock_guard<std::mutex> lock(mu_);
        fd = ms.file_fd.load(std::memory_order_relaxed);
        if (fd != -1)
            return fd;  // another thread won the race
        if (!ensure_run_open()) {
            ms.file_fd.store(-2, std::memory_order_release);
            return -2;
        }
        if (!settings_.file_per_module) {
            // The shared file was opened under the lock by ensure_run_open, so it
            // is already set here (a real fd, or -2 if that open failed).
            fd = shared_fd_;
        } else {
            fd = detail::open_log_file(run_dir_fd_, make_filename(ms.display));
            if (fd >= 0) {
                open_fds_.push_back(fd);
                register_crash_fd(fd);
            }
        }
        if (fd < 0) {
            detail::note("cannot open log file for module '" + ms.display + "'");
            ms.file_fd.store(-2, std::memory_order_release);
            return -2;
        }
        ms.file_fd.store(fd, std::memory_order_release);
        return fd;
    }

    void dispatch(ModuleState& ms, const Level& level, SourceLoc loc, const char* msg,
                  std::size_t msg_len) {
        const detail::FormatSpec* fs = fmt_ptr_.load(std::memory_order_acquire);
        if (fs == nullptr)
            return;  // never happens after construction; be safe

        detail::Record rec;
        rec.level        = &level;
        rec.module       = ms.display.c_str();
        rec.loc          = loc;
        rec.msg          = msg;
        rec.msg_len      = msg_len;
        rec.pid          = cached_pid();
        rec.tid          = cached_tid();
        rec.now_epoch_ns = fs->needs_time ? wall_now() : 0;

        thread_local char line[detail::kMaxLine];

        // File output.
        const int flvl = ms.file_level.load(std::memory_order_relaxed);
        if (level.value <= flvl) {
            const std::size_t n = detail::build_line(*fs, rec, false, line, sizeof(line));
            if (detail::mem_capture_active()) {
                detail::mem_capture_add(line, n);
            } else {
                const int fd = ensure_file_open(ms);
                if (fd >= 0) {
                    if (!detail::write_all(fd, line, n))
                        count_drop();
                    else if (level.value <= flush_level_.load(std::memory_order_relaxed)) {
                        ::fdatasync(fd);
                        g_flush_count.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
        }

        // Stdout output.
        const int  slvl = ms.stdout_level.load(std::memory_order_relaxed);
        const bool only = ms.stdout_only.load(std::memory_order_relaxed);
        const bool pass = only ? (level.value == slvl) : (level.value <= slvl);
        if (slvl != kOff && pass) {
            const bool        color = stdout_color_.load(std::memory_order_relaxed);
            const std::size_t n = detail::build_line(*fs, rec, color, line, sizeof(line));
            if (!detail::write_all(STDOUT_FILENO, line, n))
                count_drop();
        }
    }

    std::atomic<std::uint64_t> drops_{0};
    void                       count_drop() {
        const std::uint64_t d = drops_.fetch_add(1, std::memory_order_relaxed);
        g_drop_count.fetch_add(1, std::memory_order_relaxed);
        if (d == 0)
            detail::note("a log write failed; further failures are silent");
    }

    void do_flush() {
        std::lock_guard<std::mutex> lock(mu_);
        for (int fd : open_fds_) ::fdatasync(fd);
    }

    void do_shutdown() {
        std::lock_guard<std::mutex> lock(mu_);
        if (closed_)
            return;
        for (int fd : open_fds_) {
            ::fdatasync(fd);
            ::close(fd);
        }
        open_fds_.clear();
        clear_crash_fds();
        if (run_dir_fd_ >= 0)
            ::close(run_dir_fd_);
        if (run_lock_fd_ >= 0)
            ::close(run_lock_fd_);
        run_dir_fd_  = -1;
        run_lock_fd_ = -1;
        shared_fd_   = -1;
        closed_      = true;
        // Invalidate cached fds so a stray log after shutdown drops its file
        // output instead of writing into a closed (and maybe reused) descriptor.
        // -2 means "gave up": the run dir is closed, so do not try to reopen.
        for (auto& kv : modules_) kv.second->file_fd.store(-2, std::memory_order_relaxed);
    }

    void do_restart_after_fork() {
        std::lock_guard<std::mutex> lock(mu_);
        // The child inherits copies of the parent's fds. Close the child's copies
        // and start a fresh run so the child writes its own PID-named files.
        // Closing here is safe: the parent keeps its own fd copies, so its files
        // and its run lock stay open in the parent.
        for (int fd : open_fds_) ::close(fd);
        open_fds_.clear();
        clear_crash_fds();
        if (run_dir_fd_ >= 0)
            ::close(run_dir_fd_);
        if (run_lock_fd_ >= 0)
            ::close(run_lock_fd_);
        run_dir_fd_  = -1;
        run_lock_fd_ = -1;
        shared_fd_   = -1;
        init_state_  = 0;
        closed_      = false;
        for (auto& kv : modules_) kv.second->file_fd.store(-1, std::memory_order_relaxed);
    }

    // Reset to a pristine state for tests. Close any open run, clear every config
    // layer, and reload the defaults. Module state objects stay (call-site caches
    // point at them), but their thresholds and cached fds are reset.
    void do_reset(std::int64_t start_epoch) {
        std::lock_guard<std::mutex> lock(mu_);
        for (int fd : open_fds_) ::close(fd);
        open_fds_.clear();
        clear_crash_fds();
        if (run_dir_fd_ >= 0)
            ::close(run_dir_fd_);
        if (run_lock_fd_ >= 0)
            ::close(run_lock_fd_);
        run_dir_fd_  = -1;
        run_lock_fd_ = -1;
        shared_fd_   = -1;
        init_state_  = 0;
        closed_      = false;
        for (auto& kv : modules_) kv.second->file_fd.store(-1, std::memory_order_relaxed);
        file_kv_.clear();
        env_kv_.clear();
        api_kv_.clear();
        // Drop the spec cache so a test starts from a known count. The specs_
        // objects stay alive so any pointer a reader still holds never dangles.
        spec_cache_.clear();
        drops_.store(0, std::memory_order_relaxed);
        g_flush_count.store(0, std::memory_order_relaxed);
        g_drop_count.store(0, std::memory_order_relaxed);
        start_epoch_ns_ = start_epoch;
        reload_locked();
    }

    // Fork handlers. The prepare/parent/child trio keeps mu_ in a known state so
    // a child never inherits a locked mutex.
    static void atfork_prepare() { instance().mu_.lock(); }
    static void atfork_parent() { instance().mu_.unlock(); }
    static void atfork_child() {
        instance().mu_.unlock();
        g_pid.store(static_cast<int>(::getpid()), std::memory_order_relaxed);
        t_tid = 0;
    }
    // The destructor only flushes; it does not close. A worker thread might still
    // be mid-write() as the process tears down, and the OS closes the fds at exit
    // anyway. Explicit shutdown() does close, under the documented contract that
    // no other thread is logging at that point.
    ~Logger() { do_flush(); }
};

std::string join(const std::vector<std::string>& v, char sep) {
    std::string out;
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i != 0)
            out.push_back(sep);
        out += v[i];
    }
    return out;
}

// Set one API key and reload. The API layer is the highest precedence.
void set_api(const std::string& key, const std::string& value) {
    Logger&                     lg = Logger::instance();
    std::lock_guard<std::mutex> lock(lg.mu_);
    lg.api_kv_[key] = value;
    lg.reload_locked();
}

std::string level_kv(const Level& level) {
    if (level.value == kInherit)
        return "inherit";
    return detail::level_name(level.value);
}

}  // namespace

// ---- hot path ---------------------------------------------------------
bool should_log(CallSite& cs, const Level& level) {
    if (g_disabled.load(std::memory_order_relaxed) != 0)
        return false;
    ModuleState* ms = static_cast<ModuleState*>(cs.state.load(std::memory_order_acquire));
    if (ms == nullptr) {
        ms = Logger::instance().resolve_module(cs.module);
        cs.state.store(ms, std::memory_order_release);
    }
    return level.value <= ms->collect_level.load(std::memory_order_relaxed);
}

namespace {
// Format the message into a thread-local buffer and hand it to dispatch. The
// format(printf, 4, 0) attribute marks this as a vprintf-style function so the
// format string is checked through its callers.
void format_and_dispatch(ModuleState& ms, const Level& level, SourceLoc loc,
                         const char* fmt, va_list ap)
    __attribute__((format(printf, 4, 0)));
void format_and_dispatch(ModuleState& ms, const Level& level, SourceLoc loc,
                         const char* fmt, va_list ap) {
    thread_local char msgbuf[detail::kMaxMsg];
    const int         mn      = std::vsnprintf(msgbuf, sizeof(msgbuf), fmt, ap);
    std::size_t       msg_len = 0;
    if (mn > 0) {
        msg_len = static_cast<std::size_t>(mn);
        if (msg_len > sizeof(msgbuf) - 1) {
            // The message did not fit. Mark the cut with a trailing "..." so a
            // reader can see the line was truncated, not just short.
            msg_len = sizeof(msgbuf) - 1;
            if (msg_len >= 3)
                msgbuf[msg_len - 3] = msgbuf[msg_len - 2] = msgbuf[msg_len - 1] = '.';
        }
    }
    // Keep one record on one line: turn any newline or carriage return in the
    // message into a space. This stops a message (often caller or user data) from
    // forging extra log lines. See DESIGN.md 4.9.
    for (std::size_t i = 0; i < msg_len; ++i)
        if (msgbuf[i] == '\n' || msgbuf[i] == '\r')
            msgbuf[i] = ' ';
    Logger::instance().dispatch(ms, level, loc, msgbuf, msg_len);
}
}  // namespace

void emit(CallSite& cs, const Level& level, SourceLoc loc, const char* fmt, ...) {
    ModuleState* ms = static_cast<ModuleState*>(cs.state.load(std::memory_order_acquire));
    if (ms == nullptr) {
        ms = Logger::instance().resolve_module(cs.module);
        cs.state.store(ms, std::memory_order_release);
    }
    va_list ap;
    va_start(ap, fmt);
    format_and_dispatch(*ms, level, loc, fmt, ap);
    va_end(ap);
}

void emit_to(const char* module, const Level& level, SourceLoc loc, const char* fmt,
             ...) {
    ModuleState* ms = Logger::instance().resolve_module(module);
    va_list      ap;
    va_start(ap, fmt);
    format_and_dispatch(*ms, level, loc, fmt, ap);
    va_end(ap);
}

bool is_enabled(const char* module, const Level& level) {
    if (g_disabled.load(std::memory_order_relaxed) != 0)
        return false;
    ModuleState* ms = Logger::instance().resolve_module(module);
    return level.value <= ms->collect_level.load(std::memory_order_relaxed);
}

std::int64_t mono_nanos() {
    return g_mono.load(std::memory_order_relaxed)();
}

const char* version() {
    return SLOG_VERSION_STRING;
}

// ---- configuration API ------------------------------------------------
void set_verbosity(const Level& level) {
    set_api("verbosity", level_kv(level));
}

void set_module_verbosity(const char* module, const Level& level) {
    set_api(std::string("module.") + (module != nullptr ? module : ""), level_kv(level));
}

void set_disabled(bool off) {
    set_api("disable", off ? "true" : "false");
}
void set_log_dir(const char* path) {
    set_api("dir", path != nullptr ? path : "");
}
void set_run_tag(const char* tag) {
    set_api("tag", tag != nullptr ? tag : "");
}

void set_stdout_verbosity(const Level& level) {
    set_api("stdout", level.value == kInherit ? "on" : detail::level_name(level.value));
}
void set_stdout_only(bool only) {
    set_api("stdout.only", only ? "true" : "false");
}
void set_stdout_modules(const std::vector<std::string>& modules) {
    set_api("stdout.modules", join(modules, ','));
}
void stdout_off() {
    set_api("stdout", "off");
}

void set_file_verbosity(const Level& level) {
    set_api("file", level.value == kInherit ? "on" : detail::level_name(level.value));
}
void set_file_per_module(bool per_module) {
    set_api("file.per_module", per_module ? "true" : "false");
}
void set_file_name(const char* name) {
    set_api("file.name", name != nullptr ? name : "");
}
void file_off() {
    set_api("file", "off");
}

void set_format(const char* pattern) {
    set_api("format", pattern != nullptr ? pattern : "");
}
void enable_time(bool on) {
    set_api("time", on ? "true" : "false");
}
void enable_elapsed(bool on) {
    set_api("elapsed", on ? "true" : "false");
}
void set_flush_level(const Level& level) {
    set_api("flush", detail::level_name(level.value));
}

void configure(const Config& c) {
    Logger&                     lg = Logger::instance();
    std::lock_guard<std::mutex> lock(lg.mu_);
    detail::Kv&                 api = lg.api_kv_;
    api["disable"]                  = c.disabled ? "true" : "false";
    api["verbosity"]                = level_kv(c.verbosity);
    for (const auto& kv : c.module_verbosity)
        api["module." + kv.first] = detail::level_name(kv.second);
    // Write these unconditionally, so an empty field resets to the default and
    // does not leak a value from an earlier configure() or setter call. This is
    // what "configure sets every field" means.
    api["dir"]             = c.log_dir;
    api["tag"]             = c.run_tag;
    api["file"]            = !c.file_enabled ? "off"
                                             : (c.file_verbosity.value == kInherit
                                                    ? "on"
                                                    : detail::level_name(c.file_verbosity.value));
    api["file.per_module"] = c.file_per_module ? "true" : "false";
    api["file.name"]       = c.file_name;
    api["stdout"]         = !c.out.enabled ? "off"
                                           : (c.out.verbosity.value == kInherit
                                                  ? "on"
                                                  : detail::level_name(c.out.verbosity.value));
    api["stdout.only"]    = c.out.only ? "true" : "false";
    api["stdout.modules"] = join(c.out.modules, ',');
    api["format"]         = c.format;
    api["time"]           = c.enable_time ? "true" : "false";
    api["elapsed"]        = c.enable_elapsed ? "true" : "false";
    api["color"]          = c.color;
    api["flush"]          = detail::level_name(c.flush_level.value);
    api["retain.runs"]    = std::to_string(c.retain_runs);
    api["retain.days"]    = std::to_string(c.retain_days);
    lg.reload_locked();
}

void load_file(const char* path) {
    Logger&                     lg = Logger::instance();
    std::lock_guard<std::mutex> lock(lg.mu_);
    lg.file_kv_ = detail::read_config_file(path != nullptr ? path : "");
    lg.reload_locked();
}

void load_env() {
    Logger&                     lg = Logger::instance();
    std::lock_guard<std::mutex> lock(lg.mu_);
    lg.env_kv_ = detail::read_env();
    lg.reload_locked();
}

void init() {
    Logger&                     lg = Logger::instance();
    std::lock_guard<std::mutex> lock(lg.mu_);
    lg.ensure_run_open();
}

void flush() {
    Logger::instance().do_flush();
}
void shutdown() {
    Logger::instance().do_shutdown();
}
void restart_after_fork() {
    Logger::instance().do_restart_after_fork();
}

void install_crash_handler() {
    static std::atomic<bool> installed{false};
    bool                     expected = false;
    if (!installed.compare_exchange_strong(expected, true))
        return;

    // A dedicated stack lets the handler run even on a stack-overflow crash.
    static char altstack[64 * 1024];
    stack_t     ss{};
    ss.ss_sp    = altstack;
    ss.ss_size  = sizeof(altstack);
    ss.ss_flags = 0;
    ::sigaltstack(&ss, nullptr);

    struct sigaction sa {};
    sa.sa_handler = &crash_handler;
    sigemptyset(&sa.sa_mask);
    // reset so a re-fault dumps core; cast because the flag macros are unsigned.
    sa.sa_flags = static_cast<int>(SA_RESETHAND | SA_ONSTACK);
    for (int s : {SIGSEGV, SIGABRT, SIGFPE, SIGILL, SIGBUS}) ::sigaction(s, &sa, nullptr);
}

// ---- testing hooks ----------------------------------------------------
namespace testing {

void set_clock(NanosFn fn) {
    g_wall.store(fn != nullptr ? fn : &default_wall);
}
void reset_clock() {
    g_wall.store(&default_wall);
}
void set_mono_clock(NanosFn fn) {
    g_mono.store(fn != nullptr ? fn : &default_mono);
}
void reset_mono_clock() {
    g_mono.store(&default_mono);
}

void set_start_epoch_nanos(std::int64_t nanos) {
    Logger&                     lg = Logger::instance();
    std::lock_guard<std::mutex> lock(lg.mu_);
    lg.start_epoch_ns_ = nanos;
    lg.reload_locked();  // rebuild the format so %e uses the new zero point
}

void reset() {
    detail::mem_capture_reset();  // capture off and buffer emptied
    reset_clock();
    reset_mono_clock();
    Logger::instance().do_reset(wall_now());
}

std::size_t format_spec_count() {
    // The number of specs actually retained in memory. The bug was that this
    // grew on every config change; the fix keeps it at the number of distinct
    // formats. A test measures the change across same-format calls, which must
    // be zero. (This count only ever grows over a process, since a retained spec
    // is never freed while a lock-free reader might hold it.)
    Logger&                     lg = Logger::instance();
    std::lock_guard<std::mutex> lock(lg.mu_);
    return lg.specs_.size();
}

std::uint64_t flush_count() {
    return g_flush_count.load(std::memory_order_relaxed);
}

std::uint64_t drop_count() {
    return g_drop_count.load(std::memory_order_relaxed);
}

}  // namespace testing
}  // namespace slog
