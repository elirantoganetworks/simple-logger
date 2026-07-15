# Test suite. Uses doctest (a single vendored header, test-only). Each test file
# is its own executable, which CTest runs as a separate process - that gives a
# fresh logger singleton per file and lets cases run in any order.

# doctest entry point, compiled once and shared by every test binary.
add_library(slog_doctest_main STATIC doctest_main.cpp)
target_include_directories(slog_doctest_main PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# The helper program the multi-process tests drive.
add_executable(slog_helper slog_helper.cpp)
target_link_libraries(slog_helper PRIVATE slog::slog)

# Add a doctest-based test executable and register it with CTest.
function(slog_add_test name)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE slog::slog slog_doctest_main)
  target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_compile_options(${name} PRIVATE -Wall -Wextra)
  add_test(NAME ${name} COMMAND ${name})
endfunction()

slog_add_test(test_levels test_levels.cpp)
slog_add_test(test_resolution test_resolution.cpp)
slog_add_test(test_disable test_disable.cpp)
slog_add_test(test_stdout test_stdout.cpp)
slog_add_test(test_format test_format.cpp)
slog_add_test(test_config test_config.cpp)
slog_add_test(test_files test_files.cpp)
slog_add_test(test_rate_limit test_rate_limit.cpp)
slog_add_test(test_lifecycle test_lifecycle.cpp)
slog_add_test(test_concurrency test_concurrency.cpp)
slog_add_test(test_perf test_perf.cpp)
slog_add_test(test_audit_fixes test_audit_fixes.cpp)
slog_add_test(test_errors test_errors.cpp)
slog_add_test(test_headers
  test_headers.cpp hdr_slog_alone.cpp hdr_testing_alone.cpp hdr_no_short.cpp
  strip_check.cpp)

# The header and log path must build with exceptions off. Its own executable,
# compiled with -fno-exceptions, with a plain main (doctest needs exceptions).
add_executable(test_no_exceptions no_exceptions.cpp)
target_link_libraries(test_no_exceptions PRIVATE slog::slog)
target_include_directories(test_no_exceptions PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(test_no_exceptions PRIVATE -Wall -Wextra -fno-exceptions)
add_test(NAME test_no_exceptions COMMAND test_no_exceptions)

# The multi-process test needs the helper path and must be built after it.
slog_add_test(test_multiproc test_multiproc.cpp)
target_compile_definitions(test_multiproc PRIVATE
  SLOG_HELPER="$<TARGET_FILE:slog_helper>")
add_dependencies(test_multiproc slog_helper)

# Retention drives the helper too (one dead run per call).
slog_add_test(test_retention test_retention.cpp)
target_compile_definitions(test_retention PRIVATE
  SLOG_HELPER="$<TARGET_FILE:slog_helper>")
add_dependencies(test_retention slog_helper)

# The three consumption paths, driven by a script. Labeled so a sanitizer run can
# skip it (it builds its own copies of the library without sanitizers).
add_test(NAME consume
  COMMAND ${CMAKE_COMMAND} -E env "CXX=${CMAKE_CXX_COMPILER}"
          bash ${CMAKE_CURRENT_SOURCE_DIR}/consume/run_consume.sh
          ${CMAKE_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/consume_work)
set_tests_properties(consume PROPERTIES LABELS "consume" TIMEOUT 300)
