#!/bin/sh
# End-to-end checks for wibo-bundle: a bundle runs from another directory, a bundle without DLLs runs, and the stub
# architecture rules hold (PE32+ needs an x86_64 stub; PE32 accepts an i386 or x86_64 stub).
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

# Only the ELF header matters to the bundler, so header-sized files stand in for stubs of the other architecture.
# The fixture's PE machine decides the expected outcome: PE32 (0x14c = 332) takes an x86_64 stub, PE32+ (0x8664 = 34404)
# rejects an i386 stub.
pe_off=$(od -An -tu4 -j60 -N4 "$fixtures/test_external_dll.exe" | tr -d ' ')
machine=$(od -An -tu2 -j$((pe_off + 4)) -N2 "$fixtures/test_external_dll.exe" | tr -d ' ')
case $machine in
34404)
	printf '\177ELF\001\001\001\000\000\000\000\000\000\000\000\000\002\000\003\000\001\000\000\000' >"$work/other"
	if "$tool" -o "$work/mismatch" --stub "$work/other" "$fixtures/test_external_dll.exe" 2>/dev/null; then
		fail "wibo-bundle accepted an i386 stub for a PE32+ program"
	fi
	;;
332)
	printf '\177ELF\002\001\001\000\000\000\000\000\000\000\000\000\002\000\076\000\001\000\000\000' >"$work/other"
	"$tool" -o "$work/x64stub" --stub "$work/other" "$fixtures/test_external_dll.exe" ||
		fail "wibo-bundle rejected an x86_64 stub for a PE32 program"
	;;
*)
	fail "unexpected PE machine $machine in $fixtures/test_external_dll.exe"
	;;
esac

echo "[  OK  ] bundle"
