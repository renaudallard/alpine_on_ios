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

# Build the patcher tool if not present.
if [ ! -x "$AOT_PATCH" ]; then
	echo "Building aot_patch tool..."
	cc -O2 -o "$AOT_PATCH" "$SCRIPT_DIR/aot_patch.c"
fi

echo "Scanning $ROOTFS for ELF aarch64 binaries..."

# Find all regular files and check for ELF magic.
find "$ROOTFS" -type f | while read -r f; do
	# Quick check: first 4 bytes must be ELF magic.
	HEAD=$(head -c 4 "$f" 2>/dev/null | od -A n -t x1 2>/dev/null | tr -d ' ')
	if [ "$HEAD" = "7f454c46" ]; then
		"$AOT_PATCH" "$f"
		# On macOS, ad-hoc codesign each patched ELF so
		# file-backed mmap(PROT_EXEC) is allowed.
		if command -v codesign >/dev/null 2>&1; then
			codesign --force --sign - "$f" 2>/dev/null || true
		fi
	fi
done

echo "AOT patching complete."
