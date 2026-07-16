# Example programs. Each is a small, standalone .cpp that links slog::slog and
# does one thing. Built when slog is the top-level project (see the hook in the
# top-level CMakeLists.txt); a project that adds slog as a subdirectory does not
# build these.
#
# The consume/ subdirectories are separate: they show the three ways to consume
# slog and each has its own build.sh, so they are not built from here.

set(SLOG_EXAMPLES
  quickstart
  modules
  stdout_control
  files
  tags
  custom_levels
  custom_prefix
  multithread
  multiprocess
  error_handling)

foreach(ex ${SLOG_EXAMPLES})
  add_executable(${ex} ${ex}.cpp)
  target_link_libraries(${ex} PRIVATE slog::slog)
  target_compile_options(${ex} PRIVATE -Wall -Wextra)
endforeach()

# The config demo lives in config/ next to the file and script that drive it.
add_executable(config_demo config/config_demo.cpp)
target_link_libraries(config_demo PRIVATE slog::slog)
target_compile_options(config_demo PRIVATE -Wall -Wextra)
