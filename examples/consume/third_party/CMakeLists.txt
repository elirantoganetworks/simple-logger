# Path 2: third_party drop-in. In a real project you copy the slog repo to
# third_party/simple-logger and use a fixed relative path:
#
#   add_subdirectory(third_party/simple-logger)
#   target_link_libraries(your_app PRIVATE slog::slog)
#
# This example points at the repo through the SLOG_REPO variable so it can build
# in place without a copy (see build.sh).
cmake_minimum_required(VERSION 3.14)
project(slog_consume_third_party CXX)

add_subdirectory(${SLOG_REPO} slog_build)

add_executable(app app.cpp)
target_link_libraries(app PRIVATE slog::slog)
