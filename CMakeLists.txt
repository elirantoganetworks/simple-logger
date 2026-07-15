# slog - a small, fast logging library for Linux.
#
# Supports the three consumption paths from DESIGN.md:
#   - third_party drop-in:  add_subdirectory(...) then link slog::slog
#   - copied sources:       compile src/*.cpp with your own build
#   - prebuilt shared lib:  install this, then link -lslog (or find_package)

cmake_minimum_required(VERSION 3.14)
project(slog VERSION 1.0.0 LANGUAGES CXX)

# Only impose our strict warnings and tests when slog is the top-level project.
# A consumer that adds us as a subdirectory never inherits -Werror.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(SLOG_TOP_LEVEL ON)
else()
  set(SLOG_TOP_LEVEL OFF)
endif()

option(SLOG_BUILD_TESTS "Build slog tests" ${SLOG_TOP_LEVEL})

add_library(slog
  src/logger.cpp
  src/config.cpp
  src/level.cpp
  src/format.cpp
  src/sink.cpp
  src/run_dir.cpp)
add_library(slog::slog ALIAS slog)

target_include_directories(slog
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_compile_features(slog PUBLIC cxx_std_17)

find_package(Threads REQUIRED)
target_link_libraries(slog PUBLIC Threads::Threads)

set_target_properties(slog PROPERTIES
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}
  OUTPUT_NAME slog)

if(SLOG_TOP_LEVEL)
  target_compile_options(slog PRIVATE
    -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
    -Wcast-qual -Wformat=2 -Wnull-dereference -Wdouble-promotion -Werror)
endif()

# Optional sanitizer build for the test suite. Applied PUBLIC so the tests that
# link slog are instrumented together with the library under test. Example:
#   cmake -S . -B build-asan -DSLOG_SANITIZER=address,undefined
set(SLOG_SANITIZER "" CACHE STRING "Sanitizer flags, e.g. address,undefined or thread")
if(SLOG_TOP_LEVEL AND SLOG_SANITIZER)
  target_compile_options(slog PUBLIC -fsanitize=${SLOG_SANITIZER} -fno-omit-frame-pointer -g)
  target_link_options(slog PUBLIC -fsanitize=${SLOG_SANITIZER})
endif()

# ---- install (prebuilt shared library path) --------------------------
include(GNUInstallDirs)
install(TARGETS slog EXPORT slogTargets
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT slogTargets
  FILE slogTargets.cmake
  NAMESPACE slog::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/slog)

configure_file(slog.pc.in slog.pc @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/slog.pc
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

# ---- tests (added in phase 3) ----------------------------------------
if(SLOG_BUILD_TESTS AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt)
  enable_testing()
  add_subdirectory(tests)
endif()
