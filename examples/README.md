# slog examples

Small programs, one per feature. Each is a single file that does one thing. For
the full reference, see the [deep dive](../docs/deep-dive.md).

## Build and run

The quickest way is to compile one file with the copied sources, from the repo
root:

```sh
g++ -std=c++17 -I include examples/quickstart.cpp src/*.cpp -o quickstart -pthread
./quickstart
```

Or build all of them with CMake, from the repo root:

```sh
cmake -S . -B build -DSLOG_BUILD_TESTS=OFF
cmake --build build
./build/examples/quickstart      # and modules, files, tags, ...
```

Most examples write a `logs/` directory in the current directory. Run them from
a scratch directory if you do not want that, and `ls -R logs` to see what they
wrote.

## The programs

| File | Shows |
|------|-------|
| [quickstart.cpp](quickstart.cpp) | the smallest program: two log calls, file plus stdout |
| [modules.cpp](modules.cpp) | modules and per-module levels |
| [stdout_control.cpp](stdout_control.cpp) | trim the console by level and by module, keep a full file |
| [files.cpp](files.cpp) | where files go, and one file per module |
| [tags.cpp](tags.cpp) | tag a run so it groups under its own subdirectory |
| [custom_levels.cpp](custom_levels.cpp) | add your own levels |
| [custom_prefix.cpp](custom_prefix.cpp) | change the line prefix and add the time field |
| [multithread.cpp](multithread.cpp) | four threads into one file, no torn lines |
| [multiprocess.cpp](multiprocess.cpp) | fork workers, each with its own file |

## Config

Set slog from a file or from environment variables, without touching code. Both
drive the same [config/config_demo.cpp](config/config_demo.cpp), which logs at a
few levels and modules and sets nothing itself.

Build the demo (from the repo root, it is built with the others), then:

```sh
# From a config file:
SLOG_CONFIG=examples/config/simplelog.conf ./build/examples/config_demo

# From environment variables:
examples/config/run_with_env.sh ./build/examples/config_demo
```

- [config/simplelog.conf](config/simplelog.conf): a full config file with every
  key commented.
- [config/run_with_env.sh](config/run_with_env.sh): the same idea with `SLOG_*`
  env vars.

## Consuming slog

Three ways to pull slog into your own project. Each folder has an `app.cpp` and a
`build.sh` you can run in place.

```sh
examples/consume/copied_sources/build.sh   # compile src/*.cpp with your program
examples/consume/third_party/build.sh      # add_subdirectory + link slog::slog
examples/consume/prebuilt/build.sh         # install a shared library, link -lslog
```
