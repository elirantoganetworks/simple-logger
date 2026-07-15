# Consumes slog as a third_party subdirectory: add_subdirectory then link the
# exported target. SLOG_REPO and APP are passed in by the driver script.
cmake_minimum_required(VERSION 3.14)
project(slog_third_party_consumer CXX)

add_subdirectory(${SLOG_REPO} slog_build)

add_executable(consumer ${APP})
target_link_libraries(consumer PRIVATE slog::slog)
