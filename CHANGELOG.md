# Changelog

All notable changes to slog are recorded here.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and
slog follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.1.0] - 2026-07-15

Real error propagation. Every failure now reaches you with a meaningful code.
Nothing fails in silence, and a log call still never throws and never crashes.

### Added

- An error code for every failure site (`slog::errc`), grouped by area and frozen
  once released. See the table in the [deep dive](docs/deep-dive.md#error-codes).
- Three ways to see a runtime failure without a throw: a sticky `last_error()`, an
  `error_handler` you install with `set_error_handler`, and a `dropped()` count of
  lines lost to a failed write or allocation. `clear_error()` resets the sticky
  error. Each channel is allocation-free, so it is safe even when the failure is
  out of memory.
- `slog::error`, a single `std::system_error` type that `init()` throws on a hard
  setup failure, and `error_category()` / `make_error_code()` for interop with
  `std::error_code`.
- An `error_handling` example, and a full error section in the deep dive.

### Changed

- `init()` now throws `slog::error` on a hard setup failure (the log directory
  cannot be created, the run lock cannot be taken, or the run directory cannot be
  opened), where before it printed a note and carried on. The lazy path (logging
  without calling `init()`) is unchanged and still never throws. See the migration
  note in the [deep dive](docs/deep-dive.md#migrating-from-100).
- Config problems (a bad value, an unknown key, an unreadable file you loaded)
  now set a code and are readable through `last_error()`, in addition to the
  stderr note they already printed.

### Fixed

- A closed stdout (as in `app | head`) no longer risks ending the process with
  `SIGPIPE`; the write is turned into a recorded, counted drop. slog installs the
  `SIGPIPE` ignore only if the program has not set its own handler.

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
- An opt-in test header, `<slog/testing.h>`: in-memory log capture, deterministic
  clocks, and counters, so your own tests can check what you logged.
- Three ways to consume the library: copied sources, a `third_party`
  subdirectory with CMake, or a prebuilt shared library.
