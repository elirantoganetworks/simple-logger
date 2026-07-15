// A small program the multi-process tests drive. Each invocation is one real
// run of the logger in its own process. The command chooses what it does.

#include <slog/slog.h>

#include <cstdlib>
#include <cstring>
#include <string>

#include <unistd.h>

using namespace slog;

int main(int argc, char** argv) {
    if (argc < 3)
        return 2;
    const std::string cmd = argv[1];
    const char*       dir = argv[2];

    set_log_dir(dir);
    set_verbosity(INFO);
    set_format("[%L]: %m");

    if (cmd == "run") {  // run <dir>: one file run
        stdout_off();
        LOG_ERROR("hello");
        shutdown();
    } else if (cmd == "runtag" && argc >= 4) {  // runtag <dir> <tag>
        stdout_off();
        set_run_tag(argv[3]);
        LOG_ERROR("hello");
        shutdown();
    } else if (cmd == "shared" && argc >= 5) {  // shared <dir> <name> <n>
        stdout_off();
        set_file_name(argv[3]);
        const int n   = std::atoi(argv[4]);
        const int pid = static_cast<int>(::getpid());
        for (int i = 0; i < n; ++i) LOG_INFO("L%d-%d", pid, i);
        shutdown();
    } else if (cmd == "race") {  // race <dir>
        stdout_off();
        LOG_ERROR("hi");
        shutdown();
    } else if (cmd == "stdout" && argc >= 4) {  // stdout <dir> <n>: n lines to fd 1
        file_off();  // stdout stays on, inheriting the INFO level
        const int n   = std::atoi(argv[3]);
        const int pid = static_cast<int>(::getpid());
        for (int i = 0; i < n; ++i) LOG_INFO("L%d-%d", pid, i);
        shutdown();
    } else if (cmd == "crash") {  // crash <dir>
        stdout_off();
        install_crash_handler();
        LOG_ERROR("before crash");
        // Unbuffered writes plus the crash handler must preserve this line.
        volatile int* p = nullptr;
        *p              = 1;  // SIGSEGV
    } else {
        return 2;
    }
    return 0;
}
