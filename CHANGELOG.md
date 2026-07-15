# Changelog

All notable changes to slog are recorded here.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and
slog follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.0.0] - 2026-07-15

The first release. slog is a small, fast logging library for Linux C++ programs.
It gives you named modules, a level per module, and a clean log directory,
without pulling in a large framework. It needs only C++17, libc, pthread, and
POSIX.

### Added

- Log macros for four built-in levels: `LOG_ERROR`, `LOG_WARNING`, `LOG_INFO`,
  and `LOG_DEBUG`. A level shows its own severity and every more severe one. The
  default level is `WARNING`.
- Modules. Tag a whole file with `#define LOG_MODULE "name"`, or a single call
  with `LOG_TO`. Each module has its own level.
- Two independent outputs, a log file and stdout, each with its own level, so
  you can keep a full file and a quiet console at once.
- A log directory model. The newest run lives under `logs/latest`; older runs
  move out beside it. Files are named `<pid>-<timestamp>.log`. A run can be
  tagged, and can split into one file per module.
- Config from three sources: a config file, `SLOG_*` environment variables, and
  a code API. Later wins: file, then env, then API.
- Custom levels with `add_level`, placed in the order by their value.
- A configurable line prefix, with tokens for the level, function, message,
  module, wall-clock time, elapsed time, pid, tid, source file, and line.
- Safety across many threads and many processes: whole lines, no tearing. A
  helper, `restart_after_fork`, gives a forked child its own files.
- Rate-limit macros: `LOG_EVERY_N`, `LOG_FIRST_N`, `LOG_ONCE`, and
  `LOG_EVERY_N_SEC`.
- A compile-time level ceiling, `SLOG_ACTIVE_LEVEL`, to strip verbose calls from
  a build.
- An optional crash handler that flushes on a fatal signal.
- Three ways to consume the library: copied sources, a `third_party`
  subdirectory with CMake, or a prebuilt shared library.
