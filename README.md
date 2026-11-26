# wibo

A minimal, low-fuss wrapper that runs simple command-line Windows
programs on Linux and macOS. It supports PE32 (i386) and PE32+
(x86_64) programs. It was developed to run Windows compilers faster
than Wine.

Download the latest release from [GitHub releases][releases] or
build from source.

[releases]: https://github.com/decompals/wibo/releases

Release builds:

- `wibo-i686`: Linux, 32-bit ELF; runs PE32 programs (static binary)
- `wibo-x86_64`: Linux, 64-bit ELF; runs PE32 programs (static
  binary, experimental)
- `wibo-win64`: Linux, 64-bit ELF; runs PE32+ programs
- `wibo-macos`: macOS x86_64; experimental, Rosetta 2 supported

## Building

Requires GNU Make and GCC or Clang. A native 32-bit build also needs
multilib or a 32-bit cross compiler (see below).

```sh
make
```

This produces `./wibo32`. On x86_64 hosts it is a 64-bit executable
that runs PE32 (i386) guests by switching between 32-bit and 64-bit
mode, so it needs no multilib. Use `make DEBUG=1` for a debug build
(`-O0 -g3`) in the same location.

Targets:

- `make wibo32`: PE32 loader (`./wibo32`)
- `make wibo64`: PE32+ loader (`./wibo64`)
- `make wibo32 wibo64`: both
- `make`: `wibo32`, or `wibo64` when `WIN64=1` is set

Executables are written to the top-level directory. Object files are
kept apart in `build/wibo32/obj` and `build/wibo64/obj`, because the
preprocessor configuration differs.

Build variables (pass on the command line, for example
`make DEBUG=1 wibo64`):

- `DEBUG` (default `0`): `1` builds with `-O0 -g3`; `0` builds with
  `-O3 -DNDEBUG`.
- `WIN64` (default `0`): `1` selects the 64-bit loader (`wibo64`).
  `make` and `make test` use it.
- `CC` (default `gcc`): the target C compiler. Its predefined macros
  select the architecture and OS (see below). Pass flags with it if
  needed, for example `CC="gcc -m32"`. The build stops if the target
  is neither x86_64 nor i386.
- `CFLAGS`, `LDFLAGS`: passed to the wibo compile and link steps, for
  example `make CFLAGS=-flto LDFLAGS=-flto`. Host tools such as
  `gentramp` are not affected.
- `WIBO_ENABLE_LIBURING` (default `0`): `1` enables the io_uring
  backend and links the system `liburing`.
- `WIBO_ENABLE_WINE_DLLS` (default `1`): `0` skips downloading and
  embedding the Wine CRT DLLs (see [License](#license)).
- `MINGW_CC`, `MINGW_WINDRES`: MinGW tools that build the test
  fixtures. They default to `i686-w64-mingw32-*`, or to
  `x86_64-w64-mingw32-*` when `WIN64=1`.
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
  `i686-linux-gnu-gcc`. No `-m32` is added implicitly.
- `WIN64=1` requires an x86_64 `CC`.

Available configurations:

- `make wibo32`: PE32 loader. On an x86_64 host this is a 64-bit ELF.
- `make DEBUG=1 wibo32`: PE32 loader, debug build.
- `make CC="gcc -m32" wibo32`: native 32-bit ELF that runs PE32
  guests.
- `make wibo64`: PE32+ loader.
- `make DEBUG=1 wibo64`: PE32+ loader, debug build.
- macOS: `make` builds an x86_64 binary with `gcc` (Apple clang). On
  Apple silicon, use `make CC="gcc -arch x86_64"`.

`make print-wibo` prints the output path for the current
configuration.


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
- Each file must match the build that packs it: PE32 for a default
  build, PE32+ for `WIN64=1`. The packer rejects anything else,
  including a mix of the two.
- Packing uses the wibo binary that runs `--bundle`, so the bundle
  inherits that build's architecture. The binary is located through
  `argv[0]`, searched in `PATH` for a bare name, not through
  `/proc/self/exe`. Run wibo under a path it can resolve.
- A bundle receives its arguments literally. wibo's own options (`-C`,
  `--cmdline`, `--`) do not apply, and `--bundle` is passed to the
  program. `WIBO_DEBUG` still works.
- `wibo --bundle --help` prints the usage.
- `make test-bundle` (or `make WIN64=1 test-bundle`) checks a bundle
  with a DLL, a bundle without DLLs, argument pass-through, and the
  packer's name and architecture rules. To run an i386 build on a
  64-bit host, set `BUNDLE_RUNNER=qemu-i386`.

## Known limitations

- Structured exception handling is not emulated. `__try`/`__except`
  blocks in guest code are not honored, and Win64
  `__C_specific_handler` does not run guest handlers. Win64 would
  need `.pdata`/`.xdata` unwinding and scope tables. Win32 would need
  the FS:[0] SEH chain.
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
- The 32-bit build's crash report has not been verified on native i386
  hardware. Under `qemu-i386`, the handler faulted on a `%gs` TLS
  read, so the report is not produced there.

## Tests

Self-checking Windows fixtures are built with a MinGW cross
toolchain. By default that is `i686-w64-mingw32-gcc` and
`i686-w64-mingw32-windres`. With `WIN64=1` it is the
`x86_64-w64-mingw32` tools.

On macOS, use `brew install mingw-w64`.

```sh
make test
```

This cross-compiles the fixture executables, runs them through
`wibo`, and fails if any WinAPI expectation is not met. To run one
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

Optionally, wibo embeds custom Wine CRT DLL builds from
[winedll][winedll]. Set `WIBO_ENABLE_WINE_DLLS=0` to skip them, for
example `make WIBO_ENABLE_WINE_DLLS=0`.

Wine is licensed under the LGPLv2.1+. See the `LICENSE` and
`COPYING.LIB` files in [winedll][winedll] for details.

[loadlibrary]: https://github.com/taviso/loadlibrary
[retrowin32]: https://github.com/evmar/retrowin32
[decompme]: https://decomp.me
[winedll]: https://github.com/encounter/winedll
