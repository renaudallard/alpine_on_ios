#!/bin/sh
#
# Patch all ELF aarch64 binaries in a rootfs for AOT execution.
# Replaces SVC #0 with BRK #1, MSR/MRS TPIDR_EL0 with BRK traps,
# converts each AOT-patched ELF to a Mach-O dylib companion, and
# (optionally) mirrors relative symlinks from a source rootfs so
# dyld can resolve LC_LOAD_DYLIB names like libc.musl-aarch64.so.1.
#
# Usage: patch_rootfs_aot.sh <rootfs_dir> [<src_rootfs>]
#

set -e

ROOTFS="$1"
SRC_ROOTFS="$2"
if [ -z "$ROOTFS" ] || [ ! -d "$ROOTFS" ]; then
	echo "usage: $0 <rootfs_dir> [<src_rootfs>]" >&2
	exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
AOT_PATCH="$SCRIPT_DIR/aot_patch.py"

# aot_patch is a Python script so it needs no build step.
#
# elf2macho is a compiled C tool.  Xcode's post-build script
# environment kills freshly compiled binaries even after they
# are ad-hoc signed (diagnostic: hello world exits 137 inside
# xcodebuild but exit 0 in a plain shell).  Accept a pre-built
# binary via $ELF2MACHO_PREBUILT so a CI step or a local dev
# can build the tool outside xcodebuild and point the script
# at it.  Fall back to an in-script build for standalone use.
if [ -n "$ELF2MACHO_PREBUILT" ] && [ -x "$ELF2MACHO_PREBUILT" ]; then
	ELF2MACHO="$ELF2MACHO_PREBUILT"
	ELF2MACHO_DIR=""
else
	ELF2MACHO_DIR="$(mktemp -d 2>/dev/null || echo /tmp/elf2macho.$$)"
	mkdir -p "$ELF2MACHO_DIR"
	ELF2MACHO="$ELF2MACHO_DIR/elf2macho"
	echo "Building elf2macho tool in $ELF2MACHO_DIR..."
	cc -O2 -o "$ELF2MACHO" "$SCRIPT_DIR/elf2macho.c"
	if command -v codesign >/dev/null 2>&1; then
		codesign --force --sign - "$ELF2MACHO" 2>/dev/null || true
	fi
fi

# Detect platform: PLATFORM_NAME is set by Xcode or the caller.
ELF2MACHO_FLAGS=""
case "$PLATFORM_NAME" in
	iphonesimulator)
		ELF2MACHO_FLAGS="--simulator"
		echo "Platform: iOS Simulator (PLATFORM_IOSSIMULATOR)."
		;;
	macosx)
		ELF2MACHO_FLAGS="--macos"
		echo "Platform: macOS (PLATFORM_MACOS)."
		;;
esac

echo "Scanning $ROOTFS for ELF aarch64 binaries..."

# Counters live in a temp file so the piped subshell can update them
# and the parent can read them after the loop finishes.
COUNTERS=$(mktemp)
trap 'rm -f "$COUNTERS"; [ -n "$ELF2MACHO_DIR" ] && rm -rf "$ELF2MACHO_DIR"' EXIT
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
	if ! "$ELF2MACHO" $ELF2MACHO_FLAGS "$f" "$DYLIB" >/dev/null 2>&1; then
		echo "ERROR: elf2macho failed on $f" >&2
		failed=$((failed + 1))
		echo "$converted $skipped $failed $errors" > "$COUNTERS"
		continue
	fi
	if command -v codesign >/dev/null 2>&1; then
		# Remove any invalid placeholder signature first, then
		# re-sign from scratch.  elf2macho's LC_CODE_SIGNATURE
		# layout isn't accepted by macOS 26's codesign; stripping
		# and re-adding works around that.
		codesign --remove-signature "$DYLIB" 2>/dev/null || true
		if ! codesign --force --sign - "$DYLIB" 2>&1; then
			echo "WARNING: codesign failed on $DYLIB" >&2
		fi
	fi
	converted=$((converted + 1))
	echo "$converted $skipped $failed $errors" > "$COUNTERS"
done

read converted skipped failed errors < "$COUNTERS"
echo "AOT patching complete: $converted converted, $skipped skipped, $failed elf2macho failures, $errors patch errors."

if [ "$failed" -gt 0 ] || [ "$errors" -gt 0 ]; then
	exit 1
fi

# --------------------------------------------------------------------
# Materialise relative symlinks from the source rootfs as real
# file copies.
#
# rsync --no-links drops every symlink, but Alpine relies on a few
# of them for dyld resolution: lib/libc.musl-aarch64.so.1 is a
# relative symlink to ld-musl-aarch64.so.1 and busybox's
# DT_NEEDED references the libc name, so the converted dylib has
# LC_LOAD_DYLIB @rpath/libc.musl-aarch64.so.1.dylib.
#
# We CANNOT just recreate the symlinks because iOS installd
# strips symbolic links from .app bundles when it unpacks the
# IPA on the device.  The IPA still has them, but the installed
# bundle does not, and dyld fails with "Library not loaded".
# So instead we copy the resolved file content under the
# symlink's name (and its .dylib companion).  Bytes are
# duplicated, but the bundle works on iOS.
#
# Absolute symlinks are skipped because they would be broken
# inside the app bundle anyway.
# --------------------------------------------------------------------
if [ -n "$SRC_ROOTFS" ] && [ -d "$SRC_ROOTFS" ]; then
	echo "Materialising symlinks from $SRC_ROOTFS..."
	mirrored=0
	dylib_mirrored=0
	(cd "$SRC_ROOTFS" && find . -type l) | while IFS= read -r link; do
		# Strip the leading "./".
		rel="${link#./}"
		tgt=$(readlink "$SRC_ROOTFS/$rel")
		case "$tgt" in
		/*)
			# Absolute symlink — broken inside the bundle.
			continue
			;;
		esac

		dst="$ROOTFS/$rel"
		dstdir=$(dirname "$dst")
		mkdir -p "$dstdir"

		# Materialise the original file if its target exists.
		# Use cp -L to follow any chain of symlinks back to a
		# real file.  Skip if the destination already exists.
		if [ ! -e "$dst" ] && [ ! -L "$dst" ] && \
		    [ -f "$dstdir/$tgt" ]; then
			cp -L "$dstdir/$tgt" "$dst"
			mirrored=$((mirrored + 1))
		fi

		# If the target became a .dylib, copy that too so dyld
		# can resolve LC_LOAD_DYLIB by the symlinked name.
		if [ -f "$dstdir/$tgt.dylib" ] && \
		    [ ! -e "$dst.dylib" ] && [ ! -L "$dst.dylib" ]; then
			cp -L "$dstdir/$tgt.dylib" "$dst.dylib"
			dylib_mirrored=$((dylib_mirrored + 1))
		fi
		echo "$mirrored $dylib_mirrored" > "$COUNTERS.symlinks"
	done
	if [ -f "$COUNTERS.symlinks" ]; then
		read mirrored dylib_mirrored < "$COUNTERS.symlinks"
		rm -f "$COUNTERS.symlinks"
		echo "Materialised $mirrored relative symlinks ($dylib_mirrored .dylib companions)."
	fi
fi
