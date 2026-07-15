# slog deep dive

The full manual for slog. If you just want to get going, read the
[README](../README.md) first. This doc is the reference.

- [Concepts](#concepts)
- [Levels and the filtering rule](#levels-and-the-filtering-rule)
- [How the four thresholds combine](#how-the-four-thresholds-combine)
- [Config reference](#config-reference)
- [Files and directories](#files-and-directories)
- [Custom levels](#custom-levels)
- [The prefix](#the-prefix)
- [Threads and processes](#threads-and-processes)
- [Performance](#performance)
- [Error handling](#error-handling)
- [Rate limits](#rate-limits)
- [Crash safety](#crash-safety)
- [API reference](#api-reference)
- [Testing code that logs](#testing-code-that-logs)
- [Troubleshooting](#troubleshooting)
- [Versioning](#versioning)

## Concepts

Three ideas carry the whole library.

**Module.** A name you attach to a log call, like `net` or `db`. Each module has
its own level, so you can turn one part of your program up to `DEBUG` and leave
the rest quiet. A call with no module belongs to the default module, shown as
`default`.

**Level (verbosity).** How important a log line is. Four built-in levels, and you
can add your own. A level is both what you log at and what you filter by.

**Output.** Where a line goes. slog has two outputs: a log file and stdout. Each
has its own level, so you can keep a full file and a quiet console at the same
time.

You do not create logger objects. You log through macros, and you set behavior
through a small config API, env vars, or a file.

## Levels and the filtering rule

The four built-in levels, from least verbose (most severe) to most verbose:

| Level | Value | Meaning |
|-------|-------|---------|
| `ERROR` | 10 | something broke |
| `WARNING` | 20 | something looks wrong |
| `INFO` | 30 | normal progress |
| `DEBUG` | 40 | detail for digging in |

A higher value is more verbose. The default level is `WARNING`.

The rule: **a threshold `T` shows a record `R` when `value(R) <= value(T)`.** In
plain words, a threshold shows its own level and every more severe level, and
drops the more verbose ones.

| Threshold | Shows | Drops |
|-----------|-------|-------|
| `ERROR` | ERROR | WARNING, INFO, DEBUG |
| `WARNING` | ERROR, WARNING | INFO, DEBUG |
| `INFO` | ERROR, WARNING, INFO | DEBUG |
| `DEBUG` | ERROR, WARNING, INFO, DEBUG | (none) |

So a `WARNING` threshold shows `WARNING` and `ERROR` and drops `INFO` and
`DEBUG`.

## How the four thresholds combine

There are up to four level knobs: the global level, a per-module level, the file
level, and the stdout level. They combine by a simple "most specific wins" rule,
worked out for each output and module:

```
level for (output, module) =
    the output's own level         if the output set one
    else the module's level        if the module set one
    else the global level
```

The file and stdout outputs are independent. Each starts at `INHERIT`, which
means "follow the module or global level". Because the global default is
`WARNING`, an output that inherits sits at `WARNING` until you change something.

Key points that follow from the rule:

- **Per-module beats global.** Set `net` to `DEBUG` and every output that has not
  fixed its own level shows `net` at `DEBUG`, while other modules stay at the
  global level.
- **An output can be more verbose than the global level.** Set the file to
  `DEBUG` and the file shows `DEBUG` even when the global level is `WARNING`. This
  is the common "full file, quiet console" setup.
- **An output's own level beats a per-module level for that output.** If you pin
  the file at `WARNING`, a module set to `DEBUG` still shows only `WARNING` in the
  file, though it can still reach stdout if stdout inherits.

A record is collected only if it passes at least one enabled output. If it passes
none, slog drops it before it formats anything. The `disable` switch drops
everything and collects nothing at all.

Worked example:

```cpp
slog::set_verbosity(slog::WARNING);         // global default
slog::set_module_verbosity("net", slog::DEBUG);
slog::set_file_verbosity(slog::DEBUG);      // file keeps everything
// stdout is left inheriting: WARNING for most modules, but DEBUG for "net",
// because net's per-module level flows through the inherit

LOG_TO("net", slog::DEBUG, "packet %d", 7); // file and stdout (net inherits DEBUG)
LOG_INFO("startup done");                   // in the file (DEBUG file), not stdout
LOG_ERROR("out of memory");                 // in the file and on stdout
```

## Config reference

Set config three ways. The precedence is **file, then env, then API. Later
wins.** The three sources share one set of keys, so anything you can put in the
file you can also set with an env var or an API call.

At the first log call, slog loads the config file, then the env vars. Any API
call you make overrides both. Settings that pick the directory, the tag, or the
file name take effect when the run opens (the first log, or an explicit
`slog::init()`), so set them before you log. Level, format, and flush settings
can change at any time.

| Setting | Type | Default | Env var | File key | API setter |
|---------|------|---------|---------|----------|------------|
| Disable all | bool | `false` | `SLOG_DISABLE` | `disable` | `set_disabled(bool)` |
| Global level | level | `WARNING` | `SLOG_VERBOSITY` | `verbosity` | `set_verbosity(Level)` |
| Per-module level | map | none | `SLOG_VERBOSITY` list, e.g. `warning,net=debug` | `module.<name>` | `set_module_verbosity(name, Level)` |
| Log dir | path | `<cwd>/logs` | `SLOG_DIR` | `dir` | `set_log_dir(path)` |
| Run tag | string | none | `SLOG_TAG` | `tag` | `set_run_tag(tag)` |
| File output | `off` or level | on at inherit | `SLOG_FILE` | `file` | `set_file_verbosity(Level)`, `file_off()` |
| File per module | bool | `false` | `SLOG_FILE_PER_MODULE` | `file.per_module` | `set_file_per_module(bool)` |
| File name | string | `<pid>-<timestamp>.log` | `SLOG_FILE_NAME` | `file.name` | `set_file_name(name)` |
| Stdout output | `off` or level | on at inherit | `SLOG_STDOUT` | `stdout` | `set_stdout_verbosity(Level)`, `stdout_off()` |
| Stdout only | bool | `false` | `SLOG_STDOUT_ONLY` | `stdout.only` | `set_stdout_only(bool)` |
| Stdout modules | list | all | `SLOG_STDOUT_MODULES` | `stdout.modules` | `set_stdout_modules(vector)` |
| Prefix format | string | `[%L][%f]: %m` | `SLOG_FORMAT` | `format` | `set_format(pattern)` |
| Time field | bool | `false` | `SLOG_TIME` | `time` | `enable_time(bool)` |
| Elapsed field | bool | `false` | `SLOG_ELAPSED` | `elapsed` | `enable_elapsed(bool)` |
| Color | `auto`/`always`/`never` | `auto` | `SLOG_COLOR`, and `NO_COLOR` | `color` | (in `Config`) |
| Flush level | level | `WARNING` | `SLOG_FLUSH` | `flush` | `set_flush_level(Level)` |
| Keep last N runs | int | `0` (keep all) | `SLOG_RETAIN_RUNS` | `retain.runs` | (in `Config`) |
| Drop runs older than N days | int | `0` (no limit) | `SLOG_RETAIN_DAYS` | `retain.days` | (in `Config`) |
| Config file path | path | `./simplelog.conf` | `SLOG_CONFIG` | (n/a) | `load_file(path)` |

A level in any of these is a level name (any case), or a plain integer to place a
value directly. `off`, `on`, `inherit`, and `default` are also accepted where they
make sense.

### The env var for levels

`SLOG_VERBOSITY` is a comma list. The first bare token is the global level. The
rest are `module=level`:

```sh
export SLOG_VERBOSITY="warning,net=debug,db=info"
```

That sets the global level to `WARNING`, `net` to `DEBUG`, and `db` to `INFO`.

### The config file

A small INI-style file. One `key = value` per line. A `#` starts a comment, at
the start of a line or after a space or tab, so a `%#` in a format value is safe.
slog loads the file named by `SLOG_CONFIG`, or `./simplelog.conf` if it exists, or
no file at all.

```ini
# simplelog.conf
verbosity      = warning
module.net     = debug
dir            = ./logs
stdout         = warning
file           = on
format         = [%L][%f]: %m
flush          = warning
```

### Everything with one call

`slog::configure(const Config&)` applies a whole `Config` struct at once, at API
precedence (it wins over the file and env). Fields you leave at their default in
the struct are still applied, so use the single setters for a targeted change.

## Files and directories

By default slog writes to `<cwd>/logs`. The logs of the newest run live under
`<log-dir>/latest`. When a new run starts, it moves whatever is in `latest` out
to `<log-dir>`, so only the newest run stays under `latest`.

The default file name is `<PID>-<TIMESTAMP>.log`. The timestamp is
`YYYYMMDD-HHMMSS` in local time, taken at the run's start. The PID and timestamp
make the name unique.

The examples below use log dir `logs`, and two runs with PID 111 then PID 222.

### No tag, one file per run

Run 111 writes to:

```
logs/latest/111-20260715-101500.log
```

Run 222 starts, moves run 111 out, writes its own:

```
logs/111-20260715-101500.log          <- run 111, moved out, still readable
logs/latest/222-20260715-101507.log   <- run 222, the newest
```

### No tag, one file per module

Turn on `file.per_module`. Each module gets its own file, named
`<PID>-<TIMESTAMP>-<MODULE>.log`:

```
logs/latest/111-20260715-101500-net.log
logs/latest/111-20260715-101500-db.log
```

### With a tag

A tag groups a run under its own subdir. Set `tag = nightly`. Run 111:

```
logs/latest/nightly/nightly-111-20260715-101500.log
```

The run dir is `latest/<tag>`, and the file name gets the tag in front. Run 222
(also tagged `nightly`) moves the old one out:

```
logs/nightly/nightly-111-20260715-101500.log
logs/latest/nightly/nightly-222-20260715-101507.log
```

### A tag whose target already exists

If `logs/nightly` is already taken when a run moves out, the move uses a running
index:

```
logs/nightly           <- from an earlier run
logs/nightly-1         <- this move-out
```

If `nightly-1` is also taken it uses `nightly-2`, and so on.

### Concurrent runs

Several runs can start at the same moment. A file lock serializes the one step
that changes the directory structure, so two runs never race the same move or
both create `latest/<tag>`. Whichever run takes the lock last ends up as the
newest under `latest`; the rest are moved out at once. No line is lost, because
moving a file with `rename` does not disturb a descriptor that is already open, so
a moved-out run keeps writing to its file at its new path.

### Retention

By default slog keeps every run. Set `retain.runs = N` to keep only the newest N
runs, or `retain.days = D` to drop runs older than D days. The sweep runs once at
startup and only ever deletes a finished run, never one that is still writing. It
only reaps untagged loose files; tagged runs are left for you.

## Custom levels

Add a level with `slog::add_level(name, value)`. The value places it in the
order. A higher value is more verbose.

```cpp
slog::Level TRACE  = slog::add_level("TRACE", 50);   // above DEBUG (40)
slog::Level NOTICE = slog::add_level("NOTICE", 25);  // between WARNING (20) and INFO (30)

LOG(TRACE, "entering %s", "loop");   // shown only when the threshold is TRACE or higher
LOG(NOTICE, "config reloaded");
```

A custom level works everywhere a built-in does: as a threshold
(`set_verbosity(TRACE)`), in config (`verbosity = trace`, or a plain integer like
`verbosity = 25`), and in the prefix (its name prints in the `%L` field).

Custom levels are runtime only. The compile-time ceiling (below) knows only the
built-in levels.

## The prefix

Every line starts with a prefix built from a format pattern. The default is
`[%L][%f]: %m`, which gives lines like:

```
[WARNING][main]: disk almost full: 92% used
```

The tokens:

| Token | Field |
|-------|-------|
| `%L` | level name |
| `%f` | function name |
| `%m` | the message |
| `%M` | module name |
| `%t` | wall-clock time, `HH:MM:SS.mmm` |
| `%e` | seconds since the logger started, `x.xxx` |
| `%p` | process id |
| `%T` | thread id |
| `%F` | source file |
| `%#` | source line |
| `%%` | a literal percent |

Set your own with `set_format("...")` (or the `format` key, or `SLOG_FORMAT`). An
unknown token like `%z` is kept as plain text.

The time and elapsed fields are off by default. There are two ways to turn them
on. Put `%t` or `%e` in the format yourself, or flip `enable_time(true)` /
`enable_elapsed(true)`, which prepend the field if it is not already there.

```cpp
slog::set_format("[%t][%L][%M][%f]: %m");
slog::enable_time(true);   // no-op here, %t is already in the format
```

The message uses printf style. GCC and Clang check the format string against the
arguments at compile time. One gotcha: a `std::string` argument needs `.c_str()`.

## Threads and processes

slog is safe to use from many threads and many processes at once.

**Threads.** The check that decides whether to log is lock-free. A line is
written with one `write` call, which is atomic, so lines from different threads
never tear or interleave. A lock guards only the rare things: a config change,
opening a file, and the startup retention sweep. None of that is on the path of a
normal log call.

**Processes.** Each process gets its own PID-named file, so processes do not share
a file unless you give them the same fixed file name. Writes use one `write` call
to a file opened with `O_APPEND`, so even a shared file, or a shared stdout under
a service manager, stays line-atomic across processes. The move of `latest` is
serialized by a file lock.

**What is guaranteed:** on a local Linux filesystem, a single line up to the
atomic-write size is written whole, with no tearing across threads or processes.
A line longer than that size is truncated to fit, with a `...` marker, so it stays
one line.

**What is not:** slog does not claim these guarantees on NFS or other network
mounts. It does not coordinate rotation of a single shared file across processes;
the per-run directory model is how it keeps runs apart instead.

**fork.** After `fork`, the child inherits the parent's open descriptors. All log
descriptors are `O_CLOEXEC`, so a `fork` then `exec` closes them. A bare `fork`
child that keeps logging without calling anything writes into the parent's file;
because writes are atomic appends, the two do not corrupt each other, and the
child's own PID shows up in the line. If the child wants its own PID-named files,
it calls `slog::restart_after_fork()` before it logs, from its single post-fork
thread. slog does not run a background thread, so there is nothing to revive in
the child.

## Performance

The path of a log call that is dropped is the one that has to be cheap, because a
program peppered with `DEBUG` calls should cost almost nothing in production.

**A filtered-out call** reads a global "disabled" flag and, once the call site has
resolved its module the first time, one atomic level value, then compares. That is
a couple of atomic loads and an integer compare, a few nanoseconds, with no lock,
no allocation, no syscall, and the message arguments are never evaluated.

**A live call** formats the message into a thread-local buffer with `vsnprintf`,
builds the prefix, and issues one `write`.

Two ways to make disabled logging cost even less:

- **Compile it out.** Define `SLOG_ACTIVE_LEVEL` before you include the header to
  a level value. Calls to a built-in level below that ceiling are removed at build
  time, so they cost zero and their arguments never compile in. The runtime level
  can only lower what shows, never re-enable a compiled-out level.

  ```cpp
  #define SLOG_ACTIVE_LEVEL SLOG_LEVEL_INFO   // strip DEBUG from this build
  #include <slog/slog.h>
  ```

- **Guard an expensive argument.** If building a log argument is costly, wrap it:

  ```cpp
  if (slog::is_enabled("net", slog::DEBUG))
      LOG_TO("net", slog::DEBUG, "state: %s", dump_expensive_state().c_str());
  ```

What to avoid: logging at a level that triggers a flush (`WARNING` and above, by
default) inside a hot loop, since each such line calls `fdatasync`. Use a lower
level, or raise the flush level, or rate-limit the call.

## Error handling

A log call never throws and never crashes your program. When something goes wrong,
slog prints one line to stderr, prefixed `slog:`, and keeps going.

- **The log dir cannot be created** (no permission, read-only filesystem, a file
  in the way): slog turns file output off for the run and keeps stdout working.
- **A write fails** (disk full, I/O error): slog drops that line, counts it, and
  prints a one-time note so the failure is visible without flooding stderr.
- **A config value is bad** (an unknown level, a bad boolean, an unknown key):
  slog prints a note, skips that setting, and keeps the default.
- **A message is longer than the line limit:** slog truncates it with a `...`
  marker so the line stays whole and atomic.

## Rate limits

For a call that fires too often, slog has glog-style throttles. Each keeps a
counter for its own call site, so the limit is per process and per call site.

| Macro | Fires |
|-------|-------|
| `LOG_EVERY_N(level, n, fmt, ...)` | on the 1st, `n+1`th, `2n+1`th, ... call |
| `LOG_FIRST_N(level, n, fmt, ...)` | only the first `n` calls |
| `LOG_ONCE(level, fmt, ...)` | once |
| `LOG_EVERY_N_SEC(level, sec, fmt, ...)` | at most once per `sec` seconds (the first call always fires) |

```cpp
for (const Packet& p : packets)
    LOG_EVERY_N(slog::INFO, 1000, "processed %d packets", ++count);
```

## Crash safety

slog writes each line straight through with an unbuffered `write`, so a normal
crash does not lose lines that were already logged.

For a bit more, install a crash handler:

```cpp
slog::install_crash_handler();
```

It catches the fatal signals (`SIGSEGV`, `SIGABRT`, `SIGFPE`, `SIGILL`, `SIGBUS`),
does the minimum safe work (a raw `write` of a short note and an `fdatasync` on the
open files), then re-raises the signal so you still get the normal core dump and
exit status. It does not capture stack traces; use a core dump and a debugger for
that.

## API reference

Include `<slog/slog.h>`. Everything is in namespace `slog`.

### Log macros

The primary way to log. The message is printf style.

```cpp
LOG_ERROR(fmt, ...);      // built-in levels
LOG_WARNING(fmt, ...);
LOG_INFO(fmt, ...);
LOG_DEBUG(fmt, ...);

LOG(level, fmt, ...);            // a custom slog::Level
LOG_TO(module, level, fmt, ...); // a module named at the call site

LOG_EVERY_N(level, n, fmt, ...);
LOG_FIRST_N(level, n, fmt, ...);
LOG_ONCE(level, fmt, ...);
LOG_EVERY_N_SEC(level, sec, fmt, ...);
```

Macro knobs, defined before the include:

- `LOG_MODULE` - a string that tags every call in this file. Left undefined, the
  file uses the default module.
- `SLOG_ACTIVE_LEVEL` - the compile-time ceiling (see [Performance](#performance)).
- `SLOG_NO_SHORT_MACROS` - drop the short `LOG_*` names and keep only the
  `SLOG_*` names (`SLOG_ERROR`, `SLOG_INFO`, and so on). Use this if another
  library already defines `LOG_*` (for example glog).

### Levels

```cpp
struct Level { int value; const char* name; };

constexpr Level ERROR, WARNING, INFO, DEBUG;   // built-in
constexpr Level INHERIT;                        // "follow the module or global"

Level add_level(const char* name, int value);
bool  is_enabled(const char* module, const Level& level);  // cheap guard
```

### Configuration

```cpp
struct OutputConfig {           // the stdout output
    bool                     enabled   = true;
    Level                    verbosity = INHERIT;
    bool                     only      = false;   // this level only, not "and lower"
    std::vector<std::string> modules;             // empty = all modules
};

struct Config {                 // pass to configure()
    bool disabled = false;
    Level verbosity = WARNING;
    std::map<std::string, int> module_verbosity;   // by level value
    std::string log_dir, run_tag;
    bool  file_enabled = true;
    Level file_verbosity = INHERIT;
    bool  file_per_module = false;
    std::string file_name;
    OutputConfig out;
    std::string format = "[%L][%f]: %m";
    bool enable_time = false, enable_elapsed = false;
    std::string color = "auto";
    Level flush_level = WARNING;
    int retain_runs = 0, retain_days = 0;
};

void configure(const Config& c);

void set_verbosity(const Level&);
void set_module_verbosity(const char* module, const Level&);
void set_disabled(bool off);
void set_log_dir(const char* path);
void set_run_tag(const char* tag);

void set_stdout_verbosity(const Level&);   // turns stdout on at this level
void set_stdout_only(bool);
void set_stdout_modules(const std::vector<std::string>&);
void stdout_off();

void set_file_verbosity(const Level&);
void set_file_per_module(bool);
void set_file_name(const char* name);
void file_off();

void set_format(const char* pattern);
void enable_time(bool);
void enable_elapsed(bool);
void set_flush_level(const Level&);

void load_file(const char* path);   // load a config file now
void load_env();                    // read the SLOG_* env vars now
```

### Lifecycle

```cpp
void init();                   // open the run now (optional; happens on first log)
void flush();                  // flush all outputs now; safe while other threads log
void shutdown();               // flush and close; call only when no thread is logging
void install_crash_handler();  // best-effort flush on a fatal signal
void restart_after_fork();     // open fresh PID-named files in a forked child
const char* version();         // "1.0.0"
```

## Testing code that logs

`<slog/testing.h>` gives your own tests a way to check what your code logged, and
to make time deterministic. It is opt-in and does not touch a normal build.

```cpp
#include <slog/testing.h>

slog::testing::reset();                          // pristine state for this test
auto& cap = slog::testing::capture_to_memory();  // send file output to memory
slog::stdout_off();

LOG_WARNING("hello %d", 1);

assert(cap.lines().size() == 1);
assert(cap.lines()[0].find("hello 1") != std::string::npos);
```

Other hooks: `stop_capture()`, `set_clock(fn)` / `reset_clock()` for the `%t` and
`%e` fields, `set_mono_clock(fn)` / `reset_mono_clock()` for `LOG_EVERY_N_SEC`,
and `set_start_epoch_nanos(n)` to fix the zero point of `%e`.

## Troubleshooting

**I see nothing in my log file.** The default level is `WARNING`, so `INFO` and
`DEBUG` are dropped. Raise the level: `slog::set_verbosity(slog::DEBUG)`, or set
`SLOG_VERBOSITY=debug`.

**A `logs` directory appeared where I did not want it.** The default log dir is
`<cwd>/logs`. Point it somewhere else with `slog::set_log_dir(...)`, or
`SLOG_DIR`, before the first log.

**My `LOG_*` macros clash with another library.** Define `SLOG_NO_SHORT_MACROS`
before the include and use the `SLOG_*` names instead.

**My `std::string` prints garbage or crashes.** The message is printf style. Pass
`.c_str()`: `LOG_INFO("name=%s", name.c_str())`.

**My dir setting had no effect.** Directory, tag, and file-name settings take
effect when the run opens. Set them before your first log call, or before you call
`slog::init()`.

## Versioning

slog follows [semantic versioning](https://semver.org). The first release is
`1.0.0`.

- A patch release (`1.0.x`) fixes bugs and does not change the API.
- A minor release (`1.x.0`) adds features without breaking the API.
- A major release changes the API in a breaking way.

The public API is `<slog/slog.h>` and `<slog/testing.h>`. Anything in a `detail`
namespace or in `src/` is internal and can change at any time.
