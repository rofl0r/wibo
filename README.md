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

This produces a release x86 (32-bit) Linux binary at `build/release/wibo`. Use `make CONFIG=debug` for a debug build at `build/debug/wibo`.

Build variables (pass on the command line, e.g. `make CONFIG=debug ARCH=x86_64`):

| Variable                  | Default                | Description                                                                                  |
| ------------------------- | ---------------------- | -------------------------------------------------------------------------------------------- |
| `CONFIG`                  | `release`              | `debug` (`-g -O0`) or `release` (`-O3 -DNDEBUG`)                                             |
| `ARCH`                    | `i686` (`x86_64` on macOS) | `i686` (32-bit, `-m32`) or `x86_64`. `WIBO_64=1` is shorthand for `ARCH=x86_64`             |
| `CC`                      | `gcc` (`cc` on macOS)  | Target C compiler                                                                            |
| `LTO`                     | `0`                    | Set to `1` to build with `-flto`                                                             |
| `WIBO_ENABLE_LIBURING`    | `0`                    | Set to `1` to enable the io_uring backend (links system `liburing`)                          |
| `WIBO_ENABLE_WINE_DLLS`   | `1`                    | Set to `0` to skip embedding the Wine CRT DLLs (see below)                                   |
| `MINGW_CC`, `MINGW_WINDRES` | `i686-w64-mingw32-gcc`, `i686-w64-mingw32-windres` | MinGW tools used to build the test fixtures              |
| `EXTRA_CFLAGS`, `EXTRA_LDFLAGS`, `EXTRA_LIBS` | empty      | Extra flags appended to compile, link, and library steps                                     |

Run `make print-wibo` to print the output path for the current configuration.

Available configurations (`CONFIG` and `ARCH`):

- `make CONFIG=debug`: Debug Linux x86
- `make`: Release Linux x86
- `make ARCH=x86_64 CONFIG=debug`: Debug Linux x86_64
- `make ARCH=x86_64`: Release Linux x86_64
- macOS: `make CONFIG=debug` and `make` build an x86_64 binary (`build/<config>/wibo`)

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
and PE32+ (64-bit) images are both supported; the image architecture selects the stub build.

```sh
make                                  # builds build/tools/wibo-bundle and build/release/wibo (PE32)
build/tools/wibo-bundle -o app --stub build/release/wibo prog.exe lib.dll
./app arg1 arg2                       # arguments are passed literally

make WIN64=1                          # builds build/release-win64/wibo64 (PE32+, no -m32 needed)
build/tools/wibo-bundle -o app64 --stub build/release-win64/wibo64 prog64.exe lib64.dll
```

Or embed the payload into a new wibo build (Linux only):

```sh
make bundle WIBO_BUNDLE_FILES="prog.exe lib.dll"               # -> build/release/wibo-bundled
make WIN64=1 bundle WIBO_BUNDLE_FILES="prog64.exe lib64.dll"   # -> build/release-win64/wibo-bundled
```

Notes:
- The first file is the program; the rest are DLLs. Names match case-insensitively. Use `--pak -o out.pak ...` to write only the payload.
- All files in a bundle must share one architecture, and `--stub` must be the wibo build for it. The bundler rejects mismatches.
- Bundled programs receive their arguments literally: wibo's own options (`-C`, `--cmdline`, `--`) do not apply. `WIBO_DEBUG` still works.
- A first argument of exactly `--bundle-child` is consumed as an internal marker.
- `make test-bundle` (or `make WIN64=1 test-bundle`) checks a bundle with a DLL, a bundle without DLLs, and the architecture check. To run the 32-bit build on a 64-bit host, set `BUNDLE_RUNNER=qemu-i386`.

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
