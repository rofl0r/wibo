# Repository Guidelines

## Project Structure & Module Organization
- Core loader logic and headers live in `src/`.
- Windows API shims reside in `dll/`; source files grouped by DLL (e.g. `dll/kernel32/`).
- Sample fixtures for exercising the loader live in `test/`.

## Build, Test, and Development Commands
- `make wibo32` builds the 32-bit loader at `./wibo32`, and `make wibo64` builds the Win64 (PE32+) loader at `./wibo64`. Object files go under `build/<name>/obj`. Use `DEBUG=1` for a debug build (`-O0 -g3`). The 32-bit build needs multilib packages or a 32-bit compiler; pass `CC="gcc -m32"` if needed. `ARCH=x86_64` or `WIBO_64=1` selects a 64-bit compat build.
- `./wibo32 /path/to/program.exe` runs a Windows binary. Use `-D` (or `WIBO_DEBUG=1`) for verbose logging. Use `-C` to set the working directory.
- `make test` runs the self-checking WinAPI fixtures (requires `i686-w64-mingw32-gcc` and `i686-w64-mingw32-windres`). Use `make test TEST=wibo.<name>` to run one fixture.
- `clang-format -i path/to/file.cpp` and `clang-tidy path/to/file.c -- -I dll -I src` keep contributions aligned with the repo's tooling.

## Coding Style & Naming Conventions
- Formatting follows `.clang-format` (LLVM base, tabbed indentation width 4, 120 column limit).
- Use PascalCase for Win32 entry points, camelCase for internal helpers, SCREAMING_SNAKE_CASE for Win32 constants, kCamelCase for internal constants, g_camelCase for globals, and mPascalCase for member variables.
- Put static functions and variables in anonymous namespaces at the top of the file.
- Prefer scoping types to the header or source file that uses them; avoid polluting `common.h` unless widely shared.
- Keep new comments concise and focused on non-obvious constraints or reasons; avoid API documentation dumps and change-history narration. Preserve existing explanatory comments unless the change makes them inaccurate.
- Win32 APIs generally do NOT set `ERROR_SUCCESS` on success, though there are a few exceptions; check the docs.

## Shim Implementation Guidelines
- Target pre-XP behavior; our binaries are old and don't expect modern WinAPI behavior.
- Use the `microsoft_docs` tools to fetch WinAPI signatures and documentation; always fetch the documentation when working on an API function. When searching, simply include the function name; nothing else.
- Create minimal, self-contained repros in `test/` when implementing or debugging APIs; this aids both development and future testing.
- Add `DEBUG_LOG` calls to trace execution and parameter values; these are invaluable when diagnosing issues with real-world binaries.
- Add prototypes to the public shim headers (e.g., `dll/kernel32/*.h`), rebuild, and the code generator automatically handles exports.

## Testing Guidelines
- Fixture tests live in `test/` and are compiled automatically with `i686-w64-mingw32-gcc`.
- Keep new repros small and self-contained (`test_<feature>.c`).
- All fixtures must self-assert; use `test_assert.h` helpers so `make test` fails on mismatched WinAPI behaviour.
- Add new fixture names to `FIXTURE_NAMES` (or `FIXTURE_DLL_NAMES` for DLL fixtures) in the `Makefile`, then rebuild. (`make fixtures`)
- ALWAYS run tests against `wine` FIRST to establish expected behaviour. (`WINEDEBUG=-all wine build/wibo32/test/test_<feature>.exe`)
- Run tests against wibo with `make test` (or `make DEBUG=1 test`, or `make WIN64=1 test` for the Win64 loader). Use `make test TEST=wibo.<name>` for a single fixture.

## Debugging Workflow
- Reproduce crashes under `gdb` (or `lldb`) with `-q -batch` to capture backtraces, register state, and the faulting instruction without interactive prompts.
- Use `-D` (or `WIBO_DEBUG=1`) and output to a log (i.e. `&>/tmp/wibo.log`) when running the guest binary; loader traces often pinpoint missing imports, resource lookups, or API shims that misbehave. The answer is usually in the last few dozen lines before the crash.
- Inspect relevant source right away—most issues stem from stubbed shims in `dll/`.
- Missing stubs generally do _not_ cause a crash; we return valid function pointers for unknown imports. Only when the missing stub is _called_ do we abort with a message. Therefore, don't preemptively add stubs for every missing import; wait until the binary actually calls it.

## Implementation Workflow
- Fetch API documentation with `microsoft_docs`.
- Create test cases in `test/test_<feature>.c`.
- Build, then run the test(s) against `wine` to establish expected behaviour. (Important!)
- Plan and implement the API.
- Build, then run tests against `wibo` for validation.
- Format with `clang-format` and lint with `clang-tidy`.
