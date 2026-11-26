#!/bin/sh
# End-to-end checks for `wibo --bundle`: a bundle runs from another directory, a bundle without DLLs runs, extra
# arguments reach the guest, and the packer rejects duplicate names and mixed PE machine types.
#
# usage: bundle_test.sh WIBO FIXTURE_DIR
# Set BUNDLE_RUNNER (e.g. qemu-i386) when the host cannot execute the bundles directly.
set -eu

wibo=$1
fixtures=$2
runner=${BUNDLE_RUNNER:-}

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir "$work/cwd"

fail() {
	echo "[ FAIL ] bundle: $1" >&2
	exit 1
}

"$wibo" --bundle -o "$work/dll" "$fixtures/test_external_dll.exe" "$fixtures/external_exports.dll" ||
	fail "packing a program with a DLL failed"
(cd "$work/cwd" && $runner "$work/dll" >"$work/out") || fail "bundle with a DLL exited non-zero"
# Windows text-mode stdout ends lines with CRLF on the 32-bit build.
out=$(tr -d '\r' <"$work/out")
[ "$out" = "external_exports: sum=42 attached=1" ] || fail "bundle with a DLL printed: $out"

"$wibo" --bundle -o "$work/nodll" "$fixtures/test_msvcrt_printf.exe" || fail "packing a DLL-less program failed"
(cd "$work/cwd" && $runner "$work/nodll" >/dev/null) || fail "bundle without DLLs exited non-zero"

# The bundle's own argv must pass through to the guest; `--bundle` here must not select bundler mode.
(cd "$work/cwd" && $runner "$work/nodll" --bundle -o ignored >/dev/null) ||
	fail "bundle did not pass its arguments through to the guest"

if "$wibo" --bundle -o "$work/dup" "$fixtures/test_external_dll.exe" "$fixtures/external_exports.dll" \
	"$fixtures/external_exports.dll" 2>/dev/null; then
	fail "packer accepted a duplicate file name"
fi
mkdir "$work/other"
cp "$fixtures/external_exports.dll" "$work/other/EXTERNAL_EXPORTS.DLL"
if "$wibo" --bundle -o "$work/dup" "$fixtures/external_exports.dll" "$work/other/EXTERNAL_EXPORTS.DLL" \
	2>/dev/null; then
	fail "packer accepted a duplicate file name differing only in case"
fi

# Flip the DLL's PE machine to the other type (PE32 <-> PE32+). The packer must refuse a mixed bundle.
pe_off=$(od -An -tu4 -j60 -N4 "$fixtures/external_exports.dll" | tr -d ' ')
machine=$(od -An -tu2 -j$((pe_off + 4)) -N2 "$fixtures/external_exports.dll" | tr -d ' ')
case $machine in
332) flipped='\144\206' ;; # 0x8664 (PE32+)
34404) flipped='\114\001' ;; # 0x014c (PE32)
*) fail "unexpected PE machine $machine in external_exports.dll" ;;
esac
cp "$fixtures/external_exports.dll" "$work/mixed.dll"
printf "$flipped" | dd of="$work/mixed.dll" bs=1 seek=$((pe_off + 4)) conv=notrunc 2>/dev/null
if "$wibo" --bundle -o "$work/mixed" "$fixtures/test_external_dll.exe" "$work/mixed.dll" 2>/dev/null; then
	fail "packer accepted a DLL with a different PE machine type"
fi

echo "[  OK  ] bundle"
