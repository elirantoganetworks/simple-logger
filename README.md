# slog

A small, fast logging library for Linux C++ programs that want named modules,
per-module levels, and a clean log directory without pulling in a big framework.

- C++17, Linux only.
- No third-party dependencies (just libc, pthread, POSIX).
- Multi-thread and multi-process safe: no torn lines.

## Install

Pick one of three ways. slog is a handful of `.cpp` and `.h` files.

**1. Copy the sources (fastest to try).** Compile `src/*.cpp` with your program.

```sh
g++ -std=c++17 -I include app.cpp src/*.cpp -o app -pthread
```

**2. Drop under `third_party` and use CMake.**

```cmake
add_subdirectory(third_party/simple-logger)
target_link_libraries(your_app PRIVATE slog::slog)
```

**3. Build and install a shared library.**

```sh
cmake -S . -B build -DBUILD_SHARED_LIBS=ON -DSLOG_BUILD_TESTS=OFF
cmake --build build
sudo cmake --install build --prefix /usr/local
# then link your program with: -lslog
```

## Quick start

```cpp
// app.cpp
#include <slog/slog.h>

int main() {
    LOG_WARNING("disk almost full: %d%% used", 92);
    LOG_ERROR("cannot open %s", "config.ini");
}
```

Build and run it with the copied-sources path:

```sh
g++ -std=c++17 -I include app.cpp src/*.cpp -o app -pthread
./app
```

You get warnings and errors on your terminal, and a full log file at
`./logs/latest/<pid>-<timestamp>.log`. The default level is `WARNING`, so the
`INFO` and `DEBUG` calls you add later are dropped until you raise it.

Tag a whole file with a module name to control it on its own:

```cpp
#define LOG_MODULE "net"   // put this above the include, at the top of the file
#include <slog/slog.h>

void on_packet() {
    LOG_DEBUG("got packet, len=%d", 512);   // shown only if "net" is at DEBUG
}
```

```cpp
slog::set_verbosity(slog::WARNING);              // global default
slog::set_module_verbosity("net", slog::DEBUG);  // but "net" is verbose
```

## Config, the five keys you will use most

Set config three ways: a file, `SLOG_*` env vars, or the code API. Later wins:
file, then env, then API. Every key has all three forms.

| What | Env var | File key | API |
|------|---------|----------|-----|
| Global level | `SLOG_VERBOSITY=debug` | `verbosity = debug` | `slog::set_verbosity(slog::DEBUG)` |
| One module's level | `SLOG_VERBOSITY=warning,net=debug` | `module.net = debug` | `slog::set_module_verbosity("net", slog::DEBUG)` |
| Log dir | `SLOG_DIR=/var/log/app` | `dir = /var/log/app` | `slog::set_log_dir("/var/log/app")` |
| Stdout level (or `off`) | `SLOG_STDOUT=error` | `stdout = error` | `slog::set_stdout_verbosity(slog::ERROR)` |
| Turn the file off | `SLOG_FILE=off` | `file = off` | `slog::file_off()` |

Levels, from least verbose to most: `ERROR`, `WARNING`, `INFO`, `DEBUG`. A level
threshold shows that level and every more severe one. See the
[deep dive](docs/deep-dive.md) for the full config reference and the rest.

## More

- [Deep dive](docs/deep-dive.md): the full manual.
- [Examples](examples/): small programs, one per feature.
- [Changelog](CHANGELOG.md).
- [License](LICENSE): MIT.
