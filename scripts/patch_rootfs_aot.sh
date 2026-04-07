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
AOT_PATCH="$SCRIPT_DIR/.aot_patch"
ELF2MACHO="$SCRIPT_DIR/.elf2macho"

# Build tools if not present.
if [ ! -x "$AOT_PATCH" ]; then
	echo "Building aot_patch tool..."
	cc -O2 -o "$AOT_PATCH" "$SCRIPT_DIR/aot_patch.c"
fi
if [ ! -x "$ELF2MACHO" ]; then
	echo "Building elf2macho tool..."
	cc -O2 -o "$ELF2MACHO" "$SCRIPT_DIR/elf2macho.c"
fi

echo "Scanning $ROOTFS for ELF aarch64 binaries..."

converted=0
skipped=0
failed=0

# Find all regular files and check for ELF magic.
find "$ROOTFS" -type f | while read -r f; do
	# Quick check: first 4 bytes must be ELF magic.
	HEAD=$(head -c 4 "$f" 2>/dev/null | od -A n -t x1 2>/dev/null | tr -d ' ')
	if [ "$HEAD" != "7f454c46" ]; then
		continue
	fi

	# Check e_type (offset 16, 2 bytes LE).  ET_DYN = 3.
	ETYPE=$(od -A n -t u2 -N 2 -j 16 "$f" 2>/dev/null | tr -d ' ')
	if [ "$ETYPE" != "3" ]; then
		# Non-PIE ELF: skip conversion but still patch.
		"$AOT_PATCH" "$f"
		skipped=$((skipped + 1))
		continue
	fi

	"$AOT_PATCH" "$f"
	DYLIB="${f}.dylib"
	if ! "$ELF2MACHO" "$f" "$DYLIB" >/dev/null 2>&1; then
		echo "ERROR: elf2macho failed on $f" >&2
		failed=$((failed + 1))
		continue
	fi
	if command -v codesign >/dev/null 2>&1; then
		codesign --force --sign - "$DYLIB" 2>/dev/null || true
	fi
	converted=$((converted + 1))
done

echo "AOT patching complete."
