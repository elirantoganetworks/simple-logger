// Run directory lifecycle.
//
// Each run writes into <log-dir>/latest (loose files, or latest/<tag> with a
// tag). At startup a run moves whatever is already in latest out to <log-dir>,
// then creates its own place in latest. This is safe for concurrent runs:
//
//   - A file lock (.slog.lock) serializes the structure change, so two runs
//     never race the same rename or both create latest/<tag>.
//   - rename() does not disturb an open fd, so a previous run that is still
//     writing keeps writing to its file after it is moved out of latest.
//   - Files open relative to a held directory fd (openat), so a later move of
//     the directory does not send new files to the wrong place.
//
// See DESIGN.md sections 4.7 and 10.3 for the full reasoning.

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include "internal.h"

namespace slog {
namespace detail {
namespace {

const char* kRunLockPrefix = ".slog-run-";
const char* kRunLockSuffix = ".lock";
const char* kStructLock    = "/.slog.lock";

// Create path and any missing parents. EEXIST is fine.
bool mkdir_p(const std::string& path) {
    std::string p;
    for (std::size_t i = 0; i < path.size(); ++i) {
        p += path[i];
        if (path[i] == '/' && p.size() > 1) {
            const std::string dir = p.substr(0, p.size() - 1);
            if (::mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST)
                return false;
        }
    }
    if (!p.empty() && p.back() != '/') {
        if (::mkdir(p.c_str(), 0755) != 0 && errno != EEXIST)
            return false;
    }
    return true;
}

bool exists(const std::string& path) {
    return ::access(path.c_str(), F_OK) == 0;
}

// A destination path in dir for name that does not exist yet. A taken name gets
// a "-1", "-2" suffix, which is how a tag dir gets its running index.
std::string unique_dest(const std::string& dir, const std::string& name) {
    const std::string base = dir + "/" + name;
    if (!exists(base))
        return base;
    // Bounded, so a pathological directory full of collisions cannot spin the
    // loop forever or overflow the counter. A million tries never happens.
    for (int i = 1; i < (1 << 20); ++i) {
        const std::string cand = base + "-" + std::to_string(i);
        if (!exists(cand))
            return cand;
    }
    return base + "-full";
}

// Move every entry out of latest into log_dir. Called while holding the lock, so
// no other run touches latest at the same time.
void move_out(const std::string& latest, const std::string& log_dir) {
    DIR* d = ::opendir(latest.c_str());
    if (d == nullptr)
        return;
    std::vector<std::string> names;
    for (struct dirent* e = ::readdir(d); e != nullptr; e = ::readdir(d)) {
        const std::string n = e->d_name;
        if (n == "." || n == "..")
            continue;
        names.push_back(n);
    }
    ::closedir(d);
    for (const std::string& n : names) {
        const std::string src  = latest + "/" + n;
        const std::string dest = unique_dest(log_dir, n);
        if (::rename(src.c_str(), dest.c_str()) != 0)
            note("could not move " + src + ": " + std::strerror(errno));
    }
}

// The modification time of a path, or 0 if it cannot be read.
std::time_t mtime_of(const std::string& path) {
    struct stat st;
    if (::stat(path.c_str(), &st) != 0)
        return 0;
    return st.st_mtime;
}

// Delete every loose output file of an untagged run, by its <pid>-<ts> stem.
void reap_run_files(const std::string& log_dir, const std::string& stem) {
    DIR* d = ::opendir(log_dir.c_str());
    if (d == nullptr)
        return;
    std::vector<std::string> names;
    for (struct dirent* e = ::readdir(d); e != nullptr; e = ::readdir(d)) {
        const std::string n = e->d_name;
        // Match "<stem>.log" and "<stem>-<module>.log".
        if (n.rfind(stem, 0) == 0 && n.size() >= 4 &&
            n.compare(n.size() - 4, 4, ".log") == 0)
            names.push_back(n);
    }
    ::closedir(d);
    for (const std::string& n : names) ::unlink((log_dir + "/" + n).c_str());
}

// Best-effort retention. Only dead runs (their lock is free) can be reaped, so a
// live run's files are never touched. Only untagged loose files are removed;
// tagged run directories are left for the user. Runs on startup, off the hot
// path.
void retention(const std::string& log_dir, int retain_runs, int retain_days) {
    if (retain_runs <= 0 && retain_days <= 0)
        return;

    DIR* d = ::opendir(log_dir.c_str());
    if (d == nullptr)
        return;

    struct Run {
        std::string stem;  // <pid>-<ts>
        std::string lock_path;
        std::time_t mtime;
        bool        dead;
    };
    std::vector<Run>  runs;
    const std::string prefix = kRunLockPrefix;
    const std::string suffix = kRunLockSuffix;
    for (struct dirent* e = ::readdir(d); e != nullptr; e = ::readdir(d)) {
        const std::string n = e->d_name;
        if (n.rfind(prefix, 0) != 0)
            continue;
        if (n.size() < prefix.size() + suffix.size())
            continue;
        if (n.compare(n.size() - suffix.size(), suffix.size(), suffix) != 0)
            continue;
        const std::string stem =
            n.substr(prefix.size(), n.size() - prefix.size() - suffix.size());
        const std::string lock_path = log_dir + "/" + n;

        // A run is dead if its lock can be taken without blocking.
        bool      dead = false;
        const int fd   = ::open(lock_path.c_str(), O_RDWR | O_CLOEXEC);
        if (fd >= 0) {
            if (::flock(fd, LOCK_EX | LOCK_NB) == 0) {
                dead = true;
                ::flock(fd, LOCK_UN);
            }
            ::close(fd);
        }
        runs.push_back({stem, lock_path, mtime_of(lock_path), dead});
    }
    ::closedir(d);

    // Newest first, so the keep-last-N window is the front of the list.
    std::sort(runs.begin(), runs.end(),
              [](const Run& a, const Run& b) { return a.mtime > b.mtime; });

    const std::time_t now     = ::time(nullptr);
    const std::time_t max_age = static_cast<std::time_t>(retain_days) * 86400;
    for (std::size_t i = 0; i < runs.size(); ++i) {
        if (!runs[i].dead)
            continue;  // never reap a live run
        bool reap = false;
        if (retain_runs > 0 && i >= static_cast<std::size_t>(retain_runs))
            reap = true;
        if (retain_days > 0 && runs[i].mtime > 0 && (now - runs[i].mtime) > max_age)
            reap = true;
        if (reap) {
            reap_run_files(log_dir, runs[i].stem);
            ::unlink(runs[i].lock_path.c_str());
        }
    }
}

}  // namespace

RunDir open_run(const std::string& log_dir, const std::string& tag, int retain_runs,
                int retain_days, int pid, const std::string& timestamp,
                const std::string& first_file) {
    RunDir rd;

    const std::string latest = log_dir + "/latest";
    if (!mkdir_p(log_dir) || !mkdir_p(latest)) {
        rd.sys_errno = errno;
        rd.err       = "cannot create log dir " + log_dir;
        rd.code      = errc::dir_create;
        return rd;
    }

    // The per-run lock is held for the whole run so retention can tell this run
    // is alive. Its unique name means the lock is always free to take here. If it
    // cannot open, the run still writes; only retention loses sight of it, so
    // this is recorded but not fatal.
    const std::string run_lock = log_dir + "/" + kRunLockPrefix + std::to_string(pid) +
                                 "-" + timestamp + kRunLockSuffix;
    rd.run_lock_fd = ::open(run_lock.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0644);
    if (rd.run_lock_fd >= 0)
        ::flock(rd.run_lock_fd, LOCK_EX | LOCK_NB);
    else
        record(errc::run_lock, errno, run_lock.c_str());

    // The structure lock serializes the move/create across processes.
    const std::string struct_lock = log_dir + kStructLock;
    const int slfd = ::open(struct_lock.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0644);
    if (slfd < 0) {
        rd.sys_errno = errno;
        rd.err       = "cannot open lock " + struct_lock;
        rd.code      = errc::run_lock;
        if (rd.run_lock_fd >= 0)
            ::close(rd.run_lock_fd);
        rd.run_lock_fd = -1;
        return rd;
    }
    ::flock(slfd, LOCK_EX);

    move_out(latest, log_dir);

    const std::string run_dir = tag.empty() ? latest : (latest + "/" + tag);
    if (!tag.empty() && !mkdir_p(run_dir)) {
        rd.sys_errno = errno;
        rd.err       = "cannot create run dir " + run_dir;
        rd.code      = errc::dir_create;
        ::flock(slfd, LOCK_UN);
        ::close(slfd);
        if (rd.run_lock_fd >= 0)
            ::close(rd.run_lock_fd);
        rd.run_lock_fd = -1;
        return rd;
    }

    rd.dir_fd = ::open(run_dir.c_str(), O_DIRECTORY | O_RDONLY | O_CLOEXEC);
    if (rd.dir_fd < 0) {
        rd.sys_errno = errno;
        rd.err       = "cannot open run dir " + run_dir;
        rd.code      = errc::run_dir_open;
        ::flock(slfd, LOCK_UN);
        ::close(slfd);
        if (rd.run_lock_fd >= 0)
            ::close(rd.run_lock_fd);
        rd.run_lock_fd = -1;
        return rd;
    }
    rd.dir_path = run_dir;

    // Create the run's first file while the lock is still held, so the run has a
    // presence in latest before another run can rotate. Without this, two runs
    // starting at once could both create loose files in latest. A failure here is
    // not fatal to the run directory, so ok stays true; the caller sees the -1 fd
    // and records a file_open with this errno.
    if (!first_file.empty()) {
        rd.first_fd = open_log_file(rd.dir_fd, first_file);
        if (rd.first_fd < 0)
            rd.sys_errno = errno;
    }

    retention(log_dir, retain_runs, retain_days);

    ::flock(slfd, LOCK_UN);
    ::close(slfd);
    rd.ok = true;
    return rd;
}

int open_log_file(int dir_fd, const std::string& name) {
    return ::openat(dir_fd, name.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC,
                    0644);
}

}  // namespace detail
}  // namespace slog
