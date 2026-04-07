#!/bin/sh
#
# Patch all ELF aarch64 binaries in a rootfs for AOT execution.
# Replaces SVC #0 with BRK #1, MSR/MRS TPIDR_EL0 with BRK traps.
#
# Usage: patch_rootfs_aot.sh <rootfs_dir>
#

set -e

ROOTFS="$1"
if [ -z "$ROOTFS" ] || [ ! -d "$ROOTFS" ]; then
	echo "usage: $0 <rootfs_dir>" >&2
	exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
AOT_PATCH="$SCRIPT_DIR/aot_patch.py"

# aot_patch is a Python script so it needs no build step.
#
# elf2macho is a compiled C tool.  On macOS 26 the kernel kills
# freshly-compiled binaries that live inside Xcode's build tree
# (Killed: 9) even after ad-hoc codesign.  Work around it by
# building to a path outside the source tree, stripping any
# quarantine xattr, and ad-hoc signing before use.
ELF2MACHO_DIR="$(mktemp -d 2>/dev/null || echo /tmp/elf2macho.$$)"
mkdir -p "$ELF2MACHO_DIR"
ELF2MACHO="$ELF2MACHO_DIR/elf2macho"

if [ ! -x "$ELF2MACHO" ]; then
	echo "Building elf2macho tool in $ELF2MACHO_DIR..."
	# Diagnostic: can we run any cc-built binary at all?
	cat > "$ELF2MACHO_DIR/hello.c" <<'HELLO_EOF'
#include <stdio.h>
int main(int argc, char **argv) { (void)argv; printf("hello %d\n", argc); return 0; }
HELLO_EOF
	cc -O2 -o "$ELF2MACHO_DIR/hello" "$ELF2MACHO_DIR/hello.c"
	if command -v codesign >/dev/null 2>&1; then
		codesign --force --sign - "$ELF2MACHO_DIR/hello" 2>/dev/null || true
	fi
	echo "hello test: $("$ELF2MACHO_DIR/hello" a b 2>&1 || echo EXIT=$?)"

	cc -O2 -o "$ELF2MACHO" "$SCRIPT_DIR/elf2macho.c"
	if command -v codesign >/dev/null 2>&1; then
		codesign --force --sign - "$ELF2MACHO" 2>/dev/null || true
	fi
	echo "elf2macho test: $("$ELF2MACHO" 2>&1 || echo EXIT=$?)"
fi

echo "Scanning $ROOTFS for ELF aarch64 binaries..."

# Counters live in a temp file so the piped subshell can update them
# and the parent can read them after the loop finishes.
COUNTERS=$(mktemp)
trap 'rm -f "$COUNTERS"; rm -rf "$ELF2MACHO_DIR"' EXIT
echo "0 0 0 0" > "$COUNTERS"

# Find all regular files and check for ELF magic.
find "$ROOTFS" -type f | while read -r f; do
	# Quick check: first 4 bytes must be ELF magic.
	HEAD=$(head -c 4 "$f" 2>/dev/null | od -A n -t x1 2>/dev/null | tr -d ' ')
	if [ "$HEAD" != "7f454c46" ]; then
		continue
	fi

	read converted skipped failed errors < "$COUNTERS"

	# Check e_type (offset 16, 2 bytes LE).  ET_DYN = 3.
	ETYPE=$(od -A n -t u2 -N 2 -j 16 "$f" 2>/dev/null | tr -d ' ')
	if [ "$ETYPE" != "3" ]; then
		# Non-PIE ELF: skip conversion but still patch.
		if ! "$AOT_PATCH" "$f"; then
			echo "ERROR: aot_patch failed on $f" >&2
			errors=$((errors + 1))
		fi
		skipped=$((skipped + 1))
		echo "$converted $skipped $failed $errors" > "$COUNTERS"
		continue
	fi

	if ! "$AOT_PATCH" "$f"; then
		echo "ERROR: aot_patch failed on $f" >&2
		errors=$((errors + 1))
		echo "$converted $skipped $failed $errors" > "$COUNTERS"
		continue
	fi
	DYLIB="${f}.dylib"
	if ! "$ELF2MACHO" "$f" "$DYLIB" >/dev/null 2>&1; then
		echo "ERROR: elf2macho failed on $f" >&2
		failed=$((failed + 1))
		echo "$converted $skipped $failed $errors" > "$COUNTERS"
		continue
	fi
	if command -v codesign >/dev/null 2>&1; then
		codesign --force --sign - "$DYLIB" 2>/dev/null || true
	fi
	converted=$((converted + 1))
	echo "$converted $skipped $failed $errors" > "$COUNTERS"
done

read converted skipped failed errors < "$COUNTERS"
echo "AOT patching complete: $converted converted, $skipped skipped, $failed elf2macho failures, $errors patch errors."

if [ "$failed" -gt 0 ] || [ "$errors" -gt 0 ]; then
	exit 1
fi
