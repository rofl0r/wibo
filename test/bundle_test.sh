#!/bin/sh
# End-to-end checks for wibo-bundle: a bundle runs from another directory, a bundle without DLLs runs, and a stub
# built for the other architecture is rejected at bundle time.
#
# usage: bundle_test.sh TOOL STUB FIXTURE_DIR
# Set BUNDLE_RUNNER (e.g. qemu-i386) when the host cannot execute STUB directly.
set -eu

tool=$1
stub=$2
fixtures=$3
runner=${BUNDLE_RUNNER:-}

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir "$work/cwd"

fail() {
	echo "[ FAIL ] bundle: $1" >&2
	exit 1
}

"$tool" -o "$work/dll" --stub "$stub" "$fixtures/test_external_dll.exe" "$fixtures/external_exports.dll"
(cd "$work/cwd" && $runner "$work/dll" >"$work/out") || fail "bundle with a DLL exited non-zero"
# Windows text-mode stdout ends lines with CRLF on the 32-bit build.
out=$(tr -d '\r' <"$work/out")
[ "$out" = "external_exports: sum=42 attached=1" ] || fail "bundle with a DLL printed: $out"

"$tool" -o "$work/nodll" --stub "$stub" "$fixtures/test_msvcrt_printf.exe"
(cd "$work/cwd" && $runner "$work/nodll" >/dev/null) || fail "bundle without DLLs exited non-zero"

# Only the ELF header matters to the bundler, so a header-sized file stands in for a stub of the other architecture.
machine=$(od -An -tu1 -j18 -N1 "$stub" | tr -d ' ')
if [ "$machine" = 3 ]; then
	printf '\177ELF\002\001\001\000\000\000\000\000\000\000\000\000\002\000\076\000\001\000\000\000' >"$work/other"
else
	printf '\177ELF\001\001\001\000\000\000\000\000\000\000\000\000\002\000\003\000\001\000\000\000' >"$work/other"
fi
if "$tool" -o "$work/mismatch" --stub "$work/other" "$fixtures/test_external_dll.exe" 2>/dev/null; then
	fail "wibo-bundle accepted a stub of the wrong architecture"
fi

echo "[  OK  ] bundle"
