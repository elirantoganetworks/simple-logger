// Many processes, each with its own log file.
//
// After fork, a child calls slog::restart_after_fork() so it writes its own
// PID-named file instead of sharing the parent's. Each process is its own run:
// the newest sits under logs/latest/, the earlier ones are moved out next to
// it, and every process keeps its own <pid>-<timestamp>.log. Writes are atomic
// appends, so even if two processes shared a file, their lines would not tear.

#include <sys/wait.h>
#include <unistd.h>

#include <slog/slog.h>

int main() {
    slog::set_verbosity(slog::INFO);
    slog::stdout_off();
    slog::init();  // open the parent's run before forking

    LOG_INFO("parent %d starting workers", static_cast<int>(getpid()));

    for (int i = 0; i < 3; ++i) {
        const pid_t pid = fork();
        if (pid == 0) {                    // child
            slog::restart_after_fork();    // fresh PID-named file for this child
            LOG_INFO("worker %d running", i);
            slog::shutdown();
            _exit(0);
        }
    }
    for (int i = 0; i < 3; ++i)
        wait(nullptr);

    LOG_INFO("parent done");
    slog::shutdown();
    // Look under ./logs for one file per process (try: ls -R logs).
}
