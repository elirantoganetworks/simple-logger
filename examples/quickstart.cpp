// The smallest useful program. Build it, run it, and you have logs.
//
// You get WARNING and ERROR on your terminal and a full log file at
// ./logs/latest/<pid>-<timestamp>.log. The default level is WARNING, so an
// INFO or DEBUG call would be dropped until you raise the level.

#include <slog/slog.h>

int main() {
    LOG_WARNING("disk almost full: %d%% used", 92);
    LOG_ERROR("cannot open %s", "config.ini");
}
