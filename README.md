# wibo

A minimal, low-fuss wrapper that runs simple command-line Windows
programs on Linux and macOS. It supports PE32 (i386) and PE32+
(x86_64) programs. It was developed to run Windows compilers faster
than Wine.

Download the latest release from [GitHub releases][releases] or
build from source.

[releases]: https://github.com/decompals/wibo/releases

Release builds (each runtime is a separate file in the artifact):

- `wibo-i686-wibo32`: Linux, 32-bit ELF; runs PE32 programs (static
  binary)
- `wibo-x86_64-wibo32`: Linux, 64-bit ELF; runs PE32 programs
  (static binary, experimental)
- `wibo-x86_64-wibo64`: Linux, 64-bit ELF; runs PE32+ programs
- `wibo-macos`: macOS x86_64; experimental, Rosetta 2 supported

## Building

Requires GNU Make and GCC or Clang. A native 32-bit build also needs
multilib or a 32-bit cross compiler (see below).

```sh
make
```

This produces `./wibo32`. When `CC` targets x86_64 it also produces
`./wibo64`. `./wibo32` is a 64-bit executable on x86_64 hosts that
runs PE32 (i386) guests by switching between 32-bit and 64-bit mode,
so it needs no multilib. Use `make DEBUG=1` for a debug build
(`-O0 -g3`) in the same location.

Targets:

- `make wibo32`: PE32 loader (`./wibo32`)
- `make wibo64`: PE32+ loader (`./wibo64`); needs an x86_64 `CC`
- `make`: `wibo32`, plus `wibo64` when `CC` targets x86_64

Executables are written to the top-level directory. Object files are
kept apart in `build/<variant>-<arch>/obj`, because the preprocessor
configuration differs.

Build variables (pass on the command line, for example
`make DEBUG=1 wibo64`):

- `DEBUG` (default `0`): `1` builds with `-O0 -g3`; `0` builds with
  `-O3 -DNDEBUG`.
- `CC` (default `gcc`): the target C compiler. Its predefined macros
  select the architecture and OS (see below). Pass flags with it if
  needed, for example `CC="gcc -m32"`. The build stops if the target
  is neither x86_64 nor i386.
- `HOSTCC` (default `$(CC)`): compiler for host tools such as
  `gentramp`, which run on the build machine.
- `CFLAGS`, `LDFLAGS`: passed to the wibo compile and link steps, for
  example `make CFLAGS=-flto LDFLAGS=-flto`. Host tools such as
  `gentramp` are not affected.
- `WIBO_ENABLE_LIBURING` (default `0`): `1` enables the io_uring
  backend and links the system `liburing`.
- `WIBO_ENABLE_WINE_DLLS` (default `0`): `1` embeds the Wine CRT DLLs
  other than msvcrt in `wibo32` (see [License](#license)). msvcrt is
  always built in.
- `MINGW32_CC`, `MINGW32_WINDRES`: MinGW tools that build the `wibo32`
  test fixtures (default `i686-w64-mingw32-*`).
- `MINGW64_CC`, `MINGW64_WINDRES`: MinGW tools that build the `wibo64`
  test fixtures (default `x86_64-w64-mingw32-*`).
- `EXTRA_CFLAGS`, `EXTRA_LDFLAGS`, `EXTRA_LIBS`: appended to the
  compile, link, and library steps.

## Choosing the build

The architecture and OS come only from `CC`, as is usual for POSIX
builds. The Makefile never calls `uname`, so cross-compiling works. It
asks the compiler for its predefined macros (`$(CC) -dM -E`) and
builds for `__x86_64__` or `__i386__` on `__linux__` or `__APPLE__`.
The C sources test the same macros, so the Makefile and the code
cannot disagree.

- A native 32-bit ELF that runs PE32 guests needs a 32-bit compiler:
  `CC="gcc -m32"` with multilib, or a cross compiler such as
  `i686-linux-gnu-gcc`. No `-m32` is added implicitly. Use
  `HOSTCC` as well if the host compiler cannot build 32-bit tools.
- `wibo64` is built only when `CC` targets x86_64, so `make` with an
  i386 `CC` produces `wibo32` alone.

Available configurations:

- `make`: `wibo32`, plus `wibo64` if `CC` targets x86_64. On an
  x86_64 host `wibo32` is a 64-bit ELF.
- `make DEBUG=1`: the same, as debug builds.
- `make CC="gcc -m32"`: `wibo32` as a native 32-bit ELF that runs
  PE32 guests.
- `make wibo64`: PE32+ loader.
- macOS: `make` builds an x86_64 binary with `gcc` (Apple clang). On
  Apple silicon, use `make CC="gcc -arch x86_64"`.

`make print-wibo` prints the output path of each runtime built for
the current configuration.


## Usage

```sh
wibo [options] <program.exe> [arguments...]
wibo path [subcommand options] <path> [path...]
```

### General Options

- `-h`, `--help`: show usage information and exit.
- `-V`, `--version`: show version information and exit.

### Runtime Options

- `-C DIR`, `--chdir DIR`: change the working directory before
  launching the program.
- `-D`, `--debug`: enable debug logging (same as `WIBO_DEBUG=1`).
- `--cmdline STRING`: use `STRING` as the exact guest command line.
  It must include the program name, for example
  `"test.exe a b c"`.
- `--`: stop option parsing. The following arguments are used
  verbatim as the guest command line, including the program name.

### Subcommands

- `path`: convert between host and Windows-style paths. See
  `wibo path --help` for details.
- `--bundle -o OUT FILE [FILE...]`: pack a program and its DLLs into
  one executable (see [Bundling](#bundling-programs)).

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

### Bundling programs

`wibo --bundle` packs a PE program and its DLLs into one executable.
The output is a copy of the running wibo followed by a payload, so it
needs no other files. The bundle runs its program with wibo's loader.
It serves the bundled DLLs, and any children it spawns, from memory.

```sh
./wibo32 --bundle -o app prog.exe lib.dll  # first FILE is the program
./app arg1 arg2                            # args reach the program
```

- The first file is the program that runs. The others are found by
  basename, case-insensitively. Names must be unique under that
  comparison.
- Each file must match the build that packs it: PE32 for `wibo32`,
  PE32+ for `wibo64`. The packer rejects anything else, including a
  mix of the two.
- Packing uses the wibo binary that runs `--bundle`, so the bundle
  inherits that build's architecture. The binary is located through
  `argv[0]`, searched in `PATH` for a bare name, not through
  `/proc/self/exe`. Run wibo under a path it can resolve.
- A bundle receives its arguments literally. wibo's own options (`-C`,
  `--cmdline`, `--`) do not apply, and `--bundle` is passed to the
  program. `WIBO_DEBUG` still works.
- `wibo --bundle --help` prints the usage.
- `make test-bundle` checks a bundle with a DLL, a bundle without
  DLLs, argument pass-through, and the packer's name and architecture
  rules. It runs for every runtime built. To run an i386 build on a
  64-bit host, set `BUNDLE_RUNNER=qemu-i386`. `TEST_RUNNER=qemu-i386`
  does the same for `make test`.

## Known limitations

- Structured exception handling dispatches in both 32-bit guest builds
  (i686 host, and x86_64 host with `wibo32`) and in Win64 guests
  (`wibo64`). `RaiseException` and guest faults are dispatched to
  handlers. A handler that returns `ExceptionContinueExecution` resumes
  the guest. `ExceptionContinueSearch` moves to the next frame. Any
  other disposition, including `NestedException` and `CollidedUnwind`,
  is treated as unhandled.
  - 32-bit guests walk the FS:[0] chain. Frames past the innermost
    host-to-guest boundary are not searched. Windows would search them,
    so a handler in a guest caller of host code is skipped.
  - Win64 guests walk frames using `.pdata`/`.xdata` tables registered
    at load time. The walk stops at the first frame outside the guest
    modules, so host frames end the search. `__C_specific_handler`
    runs scope tables. Filter results are honoured. A scope that
    returns `EXCEPTION_EXECUTE_HANDLER` is reported as unhandled.
  - Win64 resume restores the integer and control registers from the
    handler's `CONTEXT`. XMM and MXCSR state is not restored.
  - A `RaiseException` continued with a changed context is treated as
    unhandled. Redirecting would discard the host frame of the call.
  - `RtlUnwind` is not supported. It prints an error and exits with
    status 1. Unwinding to a target frame or running `EXECUTE_HANDLER`
    needs the unwinder to land in a frame, which is not implemented.
  - C++ `throw`/`catch` (`__CxxFrameHandler3`) is not supported.
- An unhandled exception (a guest fault or `RaiseException`) calls the
  `SetUnhandledExceptionFilter` filter. It prints
  `wibo: unhandled exception <code> at <address>` to stderr and exits
  with the low byte of the code. The filter's return value is ignored.
  `AddVectoredExceptionHandler` handlers are not called, and
  `ContextRecord` is passed as NULL.
- Host faults (SIGSEGV, SIGBUS, SIGILL, SIGFPE) are reported only on
  the main thread, which has an alternate signal stack. Other threads
  report on their own stack.
- `UnhandledExceptionFilter` is still a stub.
- Process exit codes are 8 bits wide on Linux. `GetExitCodeProcess`
  returns the low byte, so a process that dies from an access
  violation reports 5. Windows and Wine report `0xC0000005`.
- On 32-bit guests, a host fault restores the host `fs`/`gs` selectors
  from the TEB slots before the report is printed. Only GDT thread
  areas are handled. If the guest TEB is in an LDT (used when
  `set_thread_area` is unavailable), the handler still faults and the
  process dies with SIGSEGV. The report is verified on a Linux 6.1
  kernel. It is not checked under `qemu-i386`.
- The 32-bit guest's `errno` lives in a per-thread cell. The cell is
  not freed when its thread exits.
- Native i686 builds (`CC="gcc -m32"`) run under `qemu-i386` with
  `TEST_RUNNER=qemu-i386 make test`. Child processes are started by
  `execve("/proc/self/exe")`, which under qemu-user fails with
  `Exec format error` and exits 127. `test_processes` and
  `test_unhandled_exception` fail there for that reason only.
- Kernels before 5.2 have no `CLONE_PIDFD`. Children are then tracked
  by pid and polled with `waitid(WNOHANG)` every 20 ms. If another
  part of the process reaps the child first, its exit code is
  reported as 0. `test_bcrypt` can hang on such kernels until the
  entropy pool is seeded. The 4.14 test run used
  `-device virtio-rng-pci`.
- The `setlocale` result for 32-bit guests comes from one shared slot.
  A call from another thread can replace it, so an older pointer may
  dangle.
- `_putenv_s` rejects a name that contains `=` with `EINVAL`. Windows
  accepts it.
- The wide environment functions (`_wgetenv`, `_wputenv`,
  `_wputenv_s`, `_wgetenv_s`) convert one byte per UTF-16 unit. Names
  or values outside Latin-1 are lossy.
- A `_wgetenv` result is replaced when the variable changes, so an
  older pointer for that name may dangle.
- `_isatty` is not exported. msvcrt has no file descriptor table to
  map descriptors onto host descriptors.

## Tests

Self-checking Windows fixtures are built with MinGW cross toolchains:
`i686-w64-mingw32-*` for `wibo32` and `x86_64-w64-mingw32-*` for
`wibo64`. Override them with `MINGW32_*` and `MINGW64_*`.

On macOS, use `brew install mingw-w64`.

```sh
make test
```

This cross-compiles the fixture executables, runs them through each
runtime, and fails if any WinAPI expectation is not met. To run one
fixture, use `make test TEST=wibo.<name>`. `make list-tests` lists all
fixture names.

## Related Projects

- [loadlibrary][loadlibrary] - Initial inspiration for this project.
- [retrowin32][retrowin32] - A similar project with different goals
  and architecture.
- [decomp.me][decompme] - Collaborative decompilation website. It uses
  wibo to run Windows compilers.

## License

wibo is licensed under the MIT License. See [`LICENSE`](LICENSE) for
details.

Optionally, `WIBO_ENABLE_WINE_DLLS=1` embeds custom Wine CRT DLL
builds from [winedll][winedll] in `wibo32`. It is off by default.
msvcrt is always the built-in implementation, so it is not taken from
Wine.

Wine is licensed under the LGPLv2.1+. See the `LICENSE` and
`COPYING.LIB` files in [winedll][winedll] for details.

[loadlibrary]: https://github.com/taviso/loadlibrary
[retrowin32]: https://github.com/evmar/retrowin32
[decompme]: https://decomp.me
[winedll]: https://github.com/encounter/winedll
