// Sinks: the small layer that turns bytes into output.
//
// On Linux a file and stdout are both just file descriptors, so there is one
// write path (write_all). The memory capture is a test-only sink that keeps
// finished lines in a buffer. This file also holds the shared note() helper.

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include <unistd.h>

#include "internal.h"
#include "slog/testing.h"

namespace slog {
namespace detail {

bool write_all(int fd, const char* buf, std::size_t n) {
    std::size_t done = 0;
    while (done < n) {
        const ssize_t w = ::write(fd, buf + done, n - done);
        if (w < 0) {
            if (errno == EINTR)
                continue;  // signal, just retry
            return false;
        }
        done += static_cast<std::size_t>(w);
    }
    return true;
}

void note(const std::string& msg) {
    // stderr, one line, best effort. Never throws, never blocks the caller.
    std::fprintf(stderr, "slog: %s\n", msg.c_str());
}

// ---- memory capture (tests) ------------------------------------------
namespace {

std::mutex& capture_mu() {
    static std::mutex m;
    return m;
}
std::vector<std::string>& capture_buf() {
    static std::vector<std::string> b;
    return b;
}
std::atomic<bool> g_capture_on{false};

}  // namespace

bool mem_capture_active() {
    return g_capture_on.load(std::memory_order_relaxed);
}

void mem_capture_add(const char* line, std::size_t n) {
    // Store the line without its trailing newline, which is easier to assert on.
    std::size_t len = n;
    if (len > 0 && line[len - 1] == '\n')
        --len;
    std::lock_guard<std::mutex> lock(capture_mu());
    capture_buf().emplace_back(line, len);
}

void mem_capture_reset() {
    g_capture_on.store(false, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(capture_mu());
    capture_buf().clear();
}

}  // namespace detail

namespace testing {

std::vector<std::string> MemoryCapture::lines() const {
    std::lock_guard<std::mutex> lock(detail::capture_mu());
    return detail::capture_buf();
}

std::size_t MemoryCapture::size() const {
    std::lock_guard<std::mutex> lock(detail::capture_mu());
    return detail::capture_buf().size();
}

void MemoryCapture::clear() {
    std::lock_guard<std::mutex> lock(detail::capture_mu());
    detail::capture_buf().clear();
}

MemoryCapture& capture_to_memory() {
    static MemoryCapture handle;
    detail::g_capture_on.store(true, std::memory_order_relaxed);
    return handle;
}

void stop_capture() {
    detail::g_capture_on.store(false, std::memory_order_relaxed);
}

}  // namespace testing
}  // namespace slog
