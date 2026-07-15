// Shared helpers for the slog tests. Header only, test-side only.
//
// The library is a process-global singleton, so each test case starts with
// slog::testing::reset() (see fresh()) and uses its own temp dir. That keeps
// every case hermetic and order-independent.

#ifndef SLOG_TEST_UTIL_H
#define SLOG_TEST_UTIL_H

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <ftw.h>
#include <sys/wait.h>
#include <unistd.h>

#include <slog/slog.h>
#include <slog/testing.h>

namespace tu {

// Reset the logger to defaults. Call at the start of every test case.
inline void fresh() {
    slog::testing::reset();
}

inline int rm_entry(const char* p, const struct stat*, int, struct FTW*) {
    return ::remove(p);
}

// A unique temp dir, removed on destruction. Hermetic per test.
class TempDir {
   public:
    TempDir() {
        char        t[] = "/tmp/slogtest_XXXXXX";
        const char* d   = ::mkdtemp(t);
        path_           = d != nullptr ? d : "/tmp/slogtest_fallback";
    }
    ~TempDir() { ::nftw(path_.c_str(), rm_entry, 16, FTW_DEPTH | FTW_PHYS); }

    TempDir(const TempDir&)            = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::string& str() const { return path_; }
    std::string        sub(const std::string& s) const { return path_ + "/" + s; }

   private:
    std::string path_;
};

// Read a whole file into a string. Empty if it cannot be read.
inline std::string read_file(const std::string& path) {
    std::string out;
    const int   fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0)
        return out;
    char    buf[8192];
    ssize_t r;
    while ((r = ::read(fd, buf, sizeof(buf))) > 0)
        out.append(buf, static_cast<size_t>(r));
    ::close(fd);
    return out;
}

// Split text into lines. The trailing newline does not make an empty line.
inline std::vector<std::string> split_lines(const std::string& s) {
    std::vector<std::string> out;
    std::size_t              start = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\n') {
            out.push_back(s.substr(start, i - start));
            start = i + 1;
        }
    }
    if (start < s.size())
        out.push_back(s.substr(start));
    return out;
}

// Names of the *.log files directly in dir (not recursive).
inline std::vector<std::string> list_logs(const std::string& dir) {
    std::vector<std::string> out;
    DIR*                     d = ::opendir(dir.c_str());
    if (d == nullptr)
        return out;
    for (dirent* e = ::readdir(d); e != nullptr; e = ::readdir(d)) {
        const std::string n = e->d_name;
        if (n.size() > 4 && n.compare(n.size() - 4, 4, ".log") == 0)
            out.push_back(n);
    }
    ::closedir(d);
    return out;
}

inline bool contains(const std::vector<std::string>& v, const std::string& sub) {
    for (const auto& s : v)
        if (s.find(sub) != std::string::npos)
            return true;
    return false;
}

inline int count_containing(const std::vector<std::string>& v, const std::string& sub) {
    int n = 0;
    for (const auto& s : v)
        if (s.find(sub) != std::string::npos)
            ++n;
    return n;
}

// Redirect the real stdout (fd 1) to a temp file for the object's lifetime, so a
// test can read back exactly what the stdout sink wrote. Assertions should run
// after the object is destroyed, so failure messages reach the real stdout.
class CaptureStdout {
   public:
    CaptureStdout() {
        char t[] = "/tmp/slogout_XXXXXX";
        fd_      = ::mkstemp(t);
        file_    = t;
        saved_   = ::dup(STDOUT_FILENO);
        ::fflush(stdout);
        ::dup2(fd_, STDOUT_FILENO);
    }
    ~CaptureStdout() {
        ::fflush(stdout);
        ::dup2(saved_, STDOUT_FILENO);
        ::close(saved_);
        ::close(fd_);
        ::unlink(file_.c_str());
    }
    CaptureStdout(const CaptureStdout&)            = delete;
    CaptureStdout& operator=(const CaptureStdout&) = delete;

    // The bytes written to stdout so far.
    std::string text() const { return read_file(file_); }

   private:
    int         fd_    = -1;
    int         saved_ = -1;
    std::string file_;
};

// Same idea for stderr (fd 2), used to check diagnostic notes.
class CaptureStderr {
   public:
    CaptureStderr() {
        char t[] = "/tmp/slogerr_XXXXXX";
        fd_      = ::mkstemp(t);
        file_    = t;
        saved_   = ::dup(STDERR_FILENO);
        ::fflush(stderr);
        ::dup2(fd_, STDERR_FILENO);
    }
    ~CaptureStderr() {
        ::fflush(stderr);
        ::dup2(saved_, STDERR_FILENO);
        ::close(saved_);
        ::close(fd_);
        ::unlink(file_.c_str());
    }
    CaptureStderr(const CaptureStderr&)            = delete;
    CaptureStderr& operator=(const CaptureStderr&) = delete;

    std::string text() const { return read_file(file_); }

   private:
    int         fd_    = -1;
    int         saved_ = -1;
    std::string file_;
};

// Run argv (argv[0] is the program path) as a child and wait. Returns the exit
// code, or -1 if it did not exit normally. fork+exec keeps the child a fresh
// process, which is what a real second run is.
inline int run_wait(const std::vector<std::string>& argv) {
    std::vector<char*> a;
    a.reserve(argv.size() + 1);
    for (const auto& s : argv) a.push_back(const_cast<char*>(s.c_str()));
    a.push_back(nullptr);
    const pid_t pid = ::fork();
    if (pid == 0) {
        ::execv(a[0], a.data());
        ::_exit(127);
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

// Start every command at once (each its own process) and wait for all. Returns
// how many exited with code 0. Used for the concurrent-start race.
inline int run_all_concurrent(const std::vector<std::vector<std::string>>& cmds) {
    std::vector<pid_t> pids;
    for (const auto& argv : cmds) {
        std::vector<char*> a;
        a.reserve(argv.size() + 1);
        for (const auto& s : argv) a.push_back(const_cast<char*>(s.c_str()));
        a.push_back(nullptr);
        const pid_t pid = ::fork();
        if (pid == 0) {
            ::execv(a[0], a.data());
            ::_exit(127);
        }
        pids.push_back(pid);
    }
    int ok = 0;
    for (pid_t pid : pids) {
        int status = 0;
        ::waitpid(pid, &status, 0);
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
            ++ok;
    }
    return ok;
}

// Write text to a file, replacing any existing content.
inline void write_file(const std::string& path, const std::string& content) {
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return;
    const char* p = content.data();
    std::size_t n = content.size();
    while (n > 0) {
        const ssize_t w = ::write(fd, p, n);
        if (w <= 0)
            break;
        p += w;
        n -= static_cast<std::size_t>(w);
    }
    ::close(fd);
}

}  // namespace tu

#endif  // SLOG_TEST_UTIL_H
