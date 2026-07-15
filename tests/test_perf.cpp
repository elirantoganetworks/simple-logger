// Performance guardrails. A filtered-out call must be cheap (DESIGN.md 10.7: a
// few nanoseconds - just a couple of atomics, no lock, no format, no syscall).
// An emitted call does real I/O and is far slower. We assert a robust relative
// gap always, and loose absolute bounds only in an optimized, non-sanitized
// build (sanitizers slow everything by an order of magnitude).

#include "doctest.h"
#include "test_util.h"

#include <chrono>
#include <cstdio>

#include <slog/slog.h>
#include <slog/testing.h>

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define SLOG_SANITIZED 1
#endif
#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) ||               \
    __has_feature(memory_sanitizer)
#define SLOG_SANITIZED 1
#endif
#endif
#ifndef SLOG_SANITIZED
#define SLOG_SANITIZED 0
#endif

using namespace slog;
using clock_type = std::chrono::steady_clock;

namespace {
double ns_per(long long total_ns, long n) {
    return static_cast<double>(total_ns) / static_cast<double>(n);
}
}  // namespace

TEST_CASE("a filtered call is cheap, and far cheaper than an emitted one") {
    tu::fresh();
    tu::TempDir dir;
    set_log_dir(dir.str().c_str());
    stdout_off();
    set_format("[%L]: %m");

    // Filtered: DEBUG dropped at an ERROR threshold.
    set_verbosity(ERROR);
    const long kFiltered = 2000000;
    auto       f0        = clock_type::now();
    for (long i = 0; i < kFiltered; ++i) LOG_DEBUG("x=%ld", i);
    auto         f1      = clock_type::now();
    const double filt_ns = ns_per(
        std::chrono::duration_cast<std::chrono::nanoseconds>(f1 - f0).count(), kFiltered);

    // Emitted: INFO written to a real file. INFO is below the WARNING flush
    // threshold, so this measures the write path, not one fdatasync per line.
    set_verbosity(INFO);
    init();
    const long kEmitted = 100000;
    auto       e0       = clock_type::now();
    for (long i = 0; i < kEmitted; ++i) LOG_INFO("x=%ld", i);
    flush();
    auto         e1      = clock_type::now();
    const double emit_ns = ns_per(
        std::chrono::duration_cast<std::chrono::nanoseconds>(e1 - e0).count(), kEmitted);

    std::fprintf(stderr, "[perf] filtered=%.1f ns/call  emitted=%.1f ns/call\n", filt_ns,
                 emit_ns);

    // Robust on any machine: the filtered path does no I/O, so it must be well
    // under the emitted path. A regression that formats or writes on the filtered
    // path would erase this gap.
    CHECK(filt_ns * 4 < emit_ns);

#if !SLOG_SANITIZED
    // Loose absolute bounds for an optimized build. A lock or syscall creeping
    // into the filtered path would be hundreds of ns and trip this.
    CHECK(filt_ns < 100.0);
    CHECK(emit_ns < 20000.0);  // ~50k lines/sec floor, far below the real rate
#endif
}
