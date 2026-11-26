# wibo

A minimal, low-fuss wrapper that can run simple command-line 32-bit Windows binaries on Linux and macOS - developed to run Windows compilers faster than Wine.

Download the latest release from [GitHub releases](https://github.com/decompals/wibo/releases) or build from source.

Available builds:

- `wibo-i686`: Linux x86 (static binary)
- `wibo-x86_64`: Linux x86_64 (static binary, experimental)
- `wibo-macos`: macOS x86_64 (experimental, Rosetta 2 supported)

## Building

Requires GNU Make, GCC (or Clang) and, for 32-bit builds, multilib support.

```sh
make
```

This produces a release Linux binary at `./wibo32`. On x86_64 hosts `wibo32` is a 64-bit executable that runs PE32 (i386) guests by switching between 32-bit and 64-bit mode, so it needs no multilib. Use `make DEBUG=1` for a debug build (`-O0 -g3`) in the same location.

Top-level targets:

- `make wibo32`: loader for PE32 programs (`./wibo32`; 64-bit host binary on x86_64)
- `make wibo64`: 64-bit loader (`./wibo64`), PE32+ programs
- `make wibo32 wibo64`: both
- `make` builds `wibo32`, or `wibo64` when `WIN64=1` is set

The executables are written to the top-level directory. Object files are kept apart: the 32-bit and 64-bit builds use separate object directories (`build/wibo32/obj`, `build/wibo64/obj`) because the preprocessor configuration differs.

Build variables (pass on the command line, e.g. `make DEBUG=1 wibo64`):

| Variable                  | Default                | Description                                                                                  |
| ------------------------- | ---------------------- | -------------------------------------------------------------------------------------------- |
| `DEBUG`                   | `0`                    | `1` builds with `-O0 -g3`; `0` builds with `-O3 -DNDEBUG`                                    |
| `WIN64`                   | `0`                    | `1` selects the 64-bit loader (`wibo64`), used by `make` and `make test`                     |
| `CC`                      | `gcc`                  | Target C compiler. The architecture and OS it generates select the build: `__x86_64__` or `__i386__` picks the loader architecture (see below), and `__linux__` or `__APPLE__` picks the platform. Pass flags with it if needed, e.g. `CC="gcc -m32"`. The build stops early if the target is neither |
| `CFLAGS`, `LDFLAGS`       | empty                  | Passed to the wibo compile and link steps, e.g. `make CFLAGS=-flto LDFLAGS=-flto` for link-time optimization. Host tools (`gentramp`, `wibo-bundle`) are not affected |
| `WIBO_ENABLE_LIBURING`    | `0`                    | Set to `1` to enable the io_uring backend (links system `liburing`)                          |
| `WIBO_ENABLE_WINE_DLLS`   | `1`                    | Set to `0` to skip embedding the Wine CRT DLLs (see below)                                   |
| `MINGW_CC`, `MINGW_WINDRES` | `i686-w64-mingw32-gcc`, `i686-w64-mingw32-windres` (`x86_64-w64-mingw32-gcc` when `WIN64=1`) | MinGW tools used to build the test fixtures |
| `EXTRA_CFLAGS`, `EXTRA_LDFLAGS`, `EXTRA_LIBS` | empty      | Extra flags appended to compile, link, and library steps                                     |

Architecture and OS are decided only by `CC`, as is usual for POSIX builds. The Makefile never calls `uname`, so cross-compiling works. It asks the compiler for its predefined macros (`$(CC) -dM -E`) and builds for whichever of `__x86_64__` or `__i386__` it defines, on `__linux__` or `__APPLE__`. The C sources test the same macros, so the Makefile and the code cannot disagree. A 32-bit build therefore needs a 32-bit compiler: `CC="gcc -m32"` with multilib, or a cross compiler such as `i686-linux-gnu-gcc`. No `-m32` is added implicitly. Setting `WIN64=1` requires an x86_64 `CC`.

Run `make print-wibo` to print the output path for the current configuration.

Available configurations:

- `make wibo32`: PE32 loader (release; x86_64 host binary by default)
- `make DEBUG=1 wibo32`: PE32 loader (debug)
- `make CC="gcc -m32" wibo32`: native 32-bit ELF that runs PE32 guests (needs multilib or a 32-bit cross compiler; the default `wibo32` on an x86_64 host is a 64-bit ELF)
- `make wibo64`: 64-bit Linux loader for PE32+ programs (release)
- `make DEBUG=1 wibo64`: 64-bit Linux loader for PE32+ programs (debug)
- macOS: `make` builds an x86_64 binary with `gcc` (Apple clang). On Apple silicon, use `make CC="gcc -arch x86_64"`


## Usage

```sh
wibo [options] <program.exe> [arguments...]
wibo path [subcommand options] <path> [path...]
```

### General Options

| Option          | Description                       |
| --------------- | --------------------------------- |
| `-h, --help`    | Show usage information and exit   |
| `-V, --version` | Show version information and exit |

### Runtime Options

| Option             | Description                                                                                                      |
| ------------------ | ---------------------------------------------------------------------------------------------------------------- |
| `-C, --chdir DIR`  | Change working directory before launching the program                                                            |
| `-D, --debug`      | Enable debug logging (same as `WIBO_DEBUG=1`)                                                                    |
| `--cmdline STRING` | Use `STRING` as the exact guest command line (must include the program name, e.g. `"test.exe a b c"`)            |
| `--`               | Stop option parsing; following arguments are used verbatim as the guest command line, including the program name |

### Subcommands

| Subcommand | Description                                                                       |
| ---------- | --------------------------------------------------------------------------------- |
| `path`     | Convert between host and Windows-style paths (see `wibo path --help` for details) |

### Examples

#### Normal usage

```sh
wibo path/to/test.exe a b c
wibo -C path/to test.exe a b c
```

#### Advanced: full control over the guest command line

```sh
wibo path/to/test.exe -- test.exe a b c
wibo --cmdline 'test.exe a b c' path/to/test.exe
wibo -- test.exe a b c
```

### Bundling a program into one Linux executable (experimental)

`wibo-bundle` packs a PE program and its DLLs into a single ELF. The bundle runs the program
with wibo's loader and serves the bundled DLLs and spawned children from memory. PE32 (32-bit)
and PE32+ (64-bit) images are both supported. A PE32+ program needs an x86_64 `--stub` (`wibo64`). A PE32 program accepts
an x86_64 `--stub` (the default `wibo32`) or an i386 one (`CC="gcc -m32"` build).

```sh
make                                  # builds build/tools/wibo-bundle and ./wibo32 (x86_64 ELF running PE32)
build/tools/wibo-bundle -o app --stub ./wibo32 prog.exe lib.dll
./app arg1 arg2                       # arguments are passed literally

make wibo64                          # builds ./wibo64 (PE32+, no -m32 needed)
build/tools/wibo-bundle -o app64 --stub ./wibo64 prog64.exe lib64.dll
```

Or embed the payload into a new wibo build (Linux only):

```sh
make bundle WIBO_BUNDLE_FILES="prog.exe lib.dll"               # -> ./wibo32-bundled
make WIN64=1 bundle WIBO_BUNDLE_FILES="prog64.exe lib64.dll"   # -> ./wibo64-bundled
```

Notes:
- The first file is the program; the rest are DLLs. Names match case-insensitively. Use `--pak -o out.pak ...` to write only the payload.
- All files in a bundle must share one architecture. The bundler checks the `--stub` ELF machine: PE32+ needs an x86_64 stub, and PE32 accepts an i386 or x86_64 stub. It cannot tell `wibo64` from `wibo32` apart, so a PE32+ bundle built on a `wibo32` stub fails at run time.
- Bundled programs receive their arguments literally: wibo's own options (`-C`, `--cmdline`, `--`) do not apply. `WIBO_DEBUG` still works.
- A first argument of exactly `--bundle-child` is consumed as an internal marker.
- `make test-bundle` (or `make WIN64=1 test-bundle`) checks a bundle with a DLL, a bundle without DLLs, and the stub architecture rules. To run an i386 build on a 64-bit host, set `BUNDLE_RUNNER=qemu-i386`.

## Known limitations

- Structured exception handling is not emulated. `__try`/`__except` blocks in guest code are not honored, and Win64 `__C_specific_handler` does not run guest handlers. Win64 would need `.pdata`/`.xdata` unwinding and scope tables; Win32 would need the FS:[0] SEH chain.
- An unhandled exception (a guest fault or `RaiseException`) calls the `SetUnhandledExceptionFilter` filter, prints `wibo: unhandled exception <code> at <address>` to stderr, and exits with the low byte of the code. The filter's return value is ignored. `AddVectoredExceptionHandler` handlers are not called, and `ContextRecord` is passed as NULL.
- Host faults (SIGSEGV, SIGBUS, SIGILL, SIGFPE) are reported only on the main thread, which has an alternate signal stack. Other threads report on their own stack.
- `UnhandledExceptionFilter` is still a stub.
- Process exit codes are 8 bits wide on Linux. `GetExitCodeProcess` returns the low byte, so a process that dies from an access violation reports 5 where Windows and Wine report `0xC0000005`.
- The 32-bit build's crash report has not been verified on native i386 hardware. Under `qemu-i386`, the handler faulted on a `%gs` TLS read, so the report is not produced there.

## Tests

Self-checking Windows fixtures are built with a 32-bit MinGW cross toolchain (`i686-w64-mingw32-gcc` and `i686-w64-mingw32-windres`).

On macOS: use `brew install mingw-w64`.

```sh
make test
```

This will cross-compile the fixture executables, run them through `wibo`, and fail if any WinAPI expectations are not met. To run a single fixture, use `make test TEST=wibo.<name>`, and `make list-tests` lists all fixture names.

## Related Projects

- [taviso/loadlibrary](https://github.com/taviso/loadlibrary) - Initial inspiration for this project.
- [evmar/retrowin32](https://github.com/evmar/retrowin32) - A similar project with different goals and architecture.
- [decomp.me](https://decomp.me) - Collaborative decompilation website; uses wibo to run Windows compilers.

## License

wibo is licensed under the MIT License. See [`LICENSE`](LICENSE) for details.

Optionally, wibo embeds custom Wine CRT DLL builds from [encounter/winedll](https://github.com/encounter/winedll). To disable, build with `WIBO_ENABLE_WINE_DLLS=0` (for example `make WIBO_ENABLE_WINE_DLLS=0`).

Wine is licensed under the LGPLv2.1+. See [`winedll/LICENSE`](https://github.com/encounter/winedll/blob/main/LICENSE) and [`winedll/COPYING.LIB`](https://github.com/encounter/winedll/blob/main/COPYING.LIB) for details.
