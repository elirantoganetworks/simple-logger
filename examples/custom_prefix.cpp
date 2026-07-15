// Change the line prefix, and add the time field.
//
// Every line starts with a prefix built from a pattern. The default is
// "[%L][%f]: %m" (level, function, message). Here we add the wall-clock time
// %t and the module %M. Putting %t in the pattern is enough to turn the time
// field on; slog::enable_time(true) is the other way to add it.
//
// Tokens: %L level, %f func, %m msg, %M module, %t time, %e elapsed, %p pid,
// %T tid, %F file, %# line, %% a literal percent.

#include <slog/slog.h>

int main() {
    slog::set_format("[%t][%L][%M][%f]: %m");
    slog::set_verbosity(slog::INFO);

    LOG_TO("net", slog::INFO, "server up");
    // A line now looks like: [14:32:05.123][INFO][net][main]: server up
}
