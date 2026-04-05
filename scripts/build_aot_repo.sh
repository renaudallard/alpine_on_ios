#!/bin/sh
#
# Build an AOT-patched Alpine APK repository.
# Downloads packages, patches ELF binaries (SVC->BRK),
# repackages, and rebuilds the APKINDEX.
#
# Usage: build_aot_repo.sh <alpine_version> <arch> <output_dir> [packages...]
#   e.g.: build_aot_repo.sh v3.21 aarch64 repo/ busybox musl apk-tools
#
# If no packages specified, patches the base set needed for a
# working shell + package manager.
#

set -e

ALPINE_VER="${1:-v3.21}"
ARCH="${2:-aarch64}"
OUTDIR="${3:-repo}"
shift 3 2>/dev/null || true

MIRROR="http://dl-cdn.alpinelinux.org/alpine"
REPOS="main community"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
AOT_PATCH="$SCRIPT_DIR/.aot_patch"
WORKDIR="$(mktemp -d)"

trap 'rm -rf "$WORKDIR"' EXIT

# Default package set: shell + networking + package manager
if [ $# -eq 0 ]; then
	set -- alpine-baselayout alpine-keys apk-tools busybox \
	    musl libcrypto3 libssl3 zlib ca-certificates \
	    scanelf musl-utils libc-utils ssl_client
fi

# Build the patcher tool.
if [ ! -x "$AOT_PATCH" ]; then
	echo "Building aot_patch tool..."
	cc -O2 -o "$AOT_PATCH" "$SCRIPT_DIR/aot_patch.c"
fi

mkdir -p "$OUTDIR"

# Download APKINDEX for each repo to find package URLs.
for repo in $REPOS; do
	url="$MIRROR/$ALPINE_VER/$repo/$ARCH/APKINDEX.tar.gz"
	echo "Fetching index: $url"
	curl -sL "$url" -o "$WORKDIR/APKINDEX_${repo}.tar.gz"
	mkdir -p "$WORKDIR/idx_${repo}"
	tar -xzf "$WORKDIR/APKINDEX_${repo}.tar.gz" -C "$WORKDIR/idx_${repo}" 2>/dev/null || true
done

# Parse APKINDEX to find package filenames.
find_pkg() {
	local pkg="$1"
	for repo in $REPOS; do
		local idx="$WORKDIR/idx_${repo}/APKINDEX"
		[ -f "$idx" ] || continue
		# APKINDEX format: P:name\nV:version\n\n
		local ver
		ver=$(awk -v pkg="$pkg" '
			/^P:/ { name = substr($0, 3) }
			/^V:/ { ver = substr($0, 3) }
			/^$/ { if (name == pkg) { print ver; exit } }
		' "$idx")
		if [ -n "$ver" ]; then
			echo "$repo/${pkg}-${ver}.apk"
			return
		fi
	done
}

# Download and patch each package.
PATCHED_PKGS=""
for pkg in "$@"; do
	pkgfile=$(find_pkg "$pkg")
	if [ -z "$pkgfile" ]; then
		echo "WARNING: package '$pkg' not found in index"
		continue
	fi

	repo=$(echo "$pkgfile" | cut -d/ -f1)
	fname=$(echo "$pkgfile" | cut -d/ -f2)
	url="$MIRROR/$ALPINE_VER/$repo/$ARCH/$fname"

	echo "Downloading: $fname"
	curl -sL "$url" -o "$WORKDIR/$fname"

	# Extract, patch, repackage.
	pkgdir="$WORKDIR/pkg_$pkg"
	mkdir -p "$pkgdir"
	tar -xzf "$WORKDIR/$fname" -C "$pkgdir" 2>/dev/null || true

	# Patch all ELF files.
	patched=0
	find "$pkgdir" -type f | while read -r f; do
		head=$(head -c 4 "$f" 2>/dev/null | od -A n -t x1 2>/dev/null | tr -d ' ')
		if [ "$head" = "7f454c46" ]; then
			"$AOT_PATCH" "$f"
		fi
	done

	# Repackage (without signature - use --allow-untrusted on client).
	# Remove old signatures.
	rm -f "$pkgdir"/.SIGN.*

	# Create new .apk (tar.gz with control + data).
	(cd "$pkgdir" && tar -czf "$OUTDIR/$fname" .PKGINFO .* * 2>/dev/null || \
	 cd "$pkgdir" && tar -czf "$OUTDIR/$fname" .)

	PATCHED_PKGS="$PATCHED_PKGS $OUTDIR/$fname"
	echo "  Patched: $fname"
done

# Generate APKINDEX for the patched packages.
echo "Generating APKINDEX..."
(
	for apkfile in $PATCHED_PKGS; do
		[ -f "$apkfile" ] || continue
		fname=$(basename "$apkfile")
		size=$(wc -c < "$apkfile" | tr -d ' ')
		# Extract .PKGINFO for metadata.
		pkginfo=$(tar -xzf "$apkfile" -O .PKGINFO 2>/dev/null || true)
		if [ -n "$pkginfo" ]; then
			echo "$pkginfo" | grep -E '^(pkgname|pkgver|arch|size|pkgdesc|url|depend|provides|install_if)' || true
			echo "S:$size"
			printf "I:%s\n" "$(echo "$pkginfo" | grep '^pkgname' | cut -d= -f2 | tr -d ' ')"
			echo ""
		fi
	done
) > "$WORKDIR/APKINDEX"

tar -czf "$OUTDIR/APKINDEX.tar.gz" -C "$WORKDIR" APKINDEX

echo ""
echo "AOT repository built in $OUTDIR/"
echo "Packages: $(echo $PATCHED_PKGS | wc -w | tr -d ' ')"
echo ""
echo "To use on device, add to /etc/apk/repositories:"
echo "  /path/to/repo"
echo "And run: apk update --allow-untrusted"
