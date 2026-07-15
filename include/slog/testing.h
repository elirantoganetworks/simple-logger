// slog test hooks.
//
// This header is opt-in. It never touches the normal build. It gives tests two
// things that are hard to do from the outside: capture log lines in memory, and
// control the clock so time-based output is deterministic.

#ifndef SLOG_TESTING_H
#define SLOG_TESTING_H

#include <cstdint>
#include <string>
#include <vector>

// Export marker, matching slog/slog.h, so these hooks stay reachable when the
// library is built with hidden visibility.
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
namespace testing {

// Sends the file output to memory instead of a file, and returns a handle to
// read it back. Call before the first log. Each captured entry is one finished
// line, without the trailing newline. Safe to call from many threads.
class SLOG_API MemoryCapture {
   public:
    // A copy of the lines captured so far.
    std::vector<std::string> lines() const;

    // Number of captured lines.
    std::size_t size() const;

    // Drop all captured lines.
    void clear();
};

// Turn on memory capture and return the handle. Replaces file output for the
// run. Idempotent: repeated calls return the same handle.
SLOG_API MemoryCapture& capture_to_memory();

// Stop memory capture and restore normal file output.
SLOG_API void stop_capture();

// Reset the whole logger to a pristine state: default config (every layer -
// file, env, and API - cleared), any open run closed, memory capture off, and
// the clocks back to real time. This lets one test process run many independent
// cases with no shared state. It does not invalidate call-site caches, so it is
// safe to call between cases in the same program.
SLOG_API void reset();

// ---- clock control ----------------------------------------------------
// The wall-clock field (%t) and the elapsed field (%e) read the clock through a
// hook. Tests can pin it so output does not depend on the real time.

// A clock hook returns nanoseconds. The wall clock counts from the Unix epoch;
// the monotonic clock counts from an unspecified but steady point.
using NanosFn = std::int64_t (*)();

// Use fn as the wall clock (drives the %t and %e fields). Pass nullptr, or call
// reset_clock(), to restore the real clock.
SLOG_API void set_clock(NanosFn fn);
SLOG_API void reset_clock();

// Use fn as the monotonic clock (drives the LOG_EVERY_N_SEC rate limit). Pass
// nullptr, or call reset_mono_clock(), to restore the real clock.
SLOG_API void set_mono_clock(NanosFn fn);
SLOG_API void reset_mono_clock();

// Set the fixed zero point used by the elapsed field (%e). Lets a test assert
// exact "seconds since start" values.
SLOG_API void set_start_epoch_nanos(std::int64_t nanos);

// ---- counters ---------------------------------------------------------
// Small counters a test can read to check work that has no visible output.
// reset() sets them all back to zero.

// Number of distinct prefix formats the logger has parsed and kept. Repeated
// config changes that do not change the format must not grow this.
SLOG_API std::size_t format_spec_count();

// Number of file flushes (fdatasync on a written line) since the last reset.
SLOG_API std::uint64_t flush_count();

// Number of log lines dropped because a write failed since the last reset.
SLOG_API std::uint64_t drop_count();

}  // namespace testing
}  // namespace slog

#endif  // SLOG_TESTING_H
