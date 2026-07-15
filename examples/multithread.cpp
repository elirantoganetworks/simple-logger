// Many threads, one log file, no torn lines.
//
// The filter check is lock-free and each line is written with one write call,
// which is atomic. Lines from different threads never tear or interleave. You
// do not lock anything yourself.

#include <thread>
#include <vector>

#include <slog/slog.h>

int main() {
    slog::set_verbosity(slog::INFO);
    slog::stdout_off();  // keep the console clean; the file gets all 400 lines

    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([t] {
            for (int i = 0; i < 100; ++i)
                LOG_INFO("thread %d line %d", t, i);
        });
    }
    for (std::thread& th : threads)
        th.join();

    slog::shutdown();
    // The run's log file holds 400 whole, non-torn lines from the four threads.
}
