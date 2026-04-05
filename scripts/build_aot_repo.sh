#!/bin/sh
#
# Build an AOT-patched Alpine APK repository.
# Downloads packages with full dependency resolution, patches
# ELF binaries (SVC->BRK), repackages, and rebuilds the index.
#
# Usage: build_aot_repo.sh <alpine_version> <arch> <output_dir> [packages...]
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
OUTDIR="$(mkdir -p "$OUTDIR" && cd "$OUTDIR" && pwd)"

trap 'rm -rf "$WORKDIR"' EXIT

# Default: full desktop set
if [ $# -eq 0 ]; then
	set -- alpine-baselayout alpine-keys apk-tools busybox \
	    musl libcrypto3 libssl3 zlib ca-certificates \
	    scanelf musl-utils libc-utils ssl_client \
	    libgcc ncurses-terminfo-base ncurses-libs readline \
	    curl wget \
	    xorg-server xf86-video-fbdev xterm xinit xauth \
	    mesa mesa-gl mesa-egl mesa-gbm mesa-dri-gallium \
	    dbus dbus-libs eudev \
	    gtk+3.0 pango harfbuzz fontconfig freetype \
	    font-noto font-noto-emoji font-liberation \
	    firefox-esr thunderbird \
	    mate-desktop-environment mate-terminal mate-panel \
	    caja marco pluma eom atril engrampa \
	    libreoffice \
	    adwaita-icon-theme hicolor-icon-theme \
	    gvfs udisks2 polkit \
	    networkmanager bash coreutils
fi

# Build the patcher tool.
if [ ! -x "$AOT_PATCH" ]; then
	echo "Building aot_patch tool..."
	cc -O2 -o "$AOT_PATCH" "$SCRIPT_DIR/aot_patch.c"
fi

# Step 1: Resolve all dependencies using Alpine's package database.
echo "Resolving dependencies for $# packages..."
RESOLVED="$WORKDIR/resolved.txt"

# Download all APKINDEX files.
for repo in $REPOS; do
	url="$MIRROR/$ALPINE_VER/$repo/$ARCH/APKINDEX.tar.gz"
	echo "  Fetching index: $repo"
	curl -sL "$url" -o "$WORKDIR/APKINDEX_${repo}.tar.gz"
	mkdir -p "$WORKDIR/idx_${repo}"
	tar -xzf "$WORKDIR/APKINDEX_${repo}.tar.gz" -C "$WORKDIR/idx_${repo}" 2>/dev/null || true
done

# Parse all packages into a dependency database.
cat "$WORKDIR"/idx_*/APKINDEX > "$WORKDIR/allindex"

# Recursive dependency resolver using awk.
REQUESTED="$*" awk '
BEGIN {
	# Read requested packages from env
	split(ENVIRON["REQUESTED"], req, " ")
	for (i in req) queue[req[i]] = 1
}

# Parse APKINDEX
/^P:/ { name = substr($0, 3) }
/^V:/ { ver = substr($0, 3) }
/^D:/ { deps = substr($0, 3) }
/^p:/ { provides = substr($0, 3) }
/^$/ {
	if (name != "") {
		versions[name] = ver
		alldeps[name] = deps

		# Register provides (including so: entries)
		n = split(provides, prov, " ")
		for (i = 1; i <= n; i++) {
			sub(/[>=<].*/, "", prov[i])
			if (prov[i] != "" && !(prov[i] in provider))
				provider[prov[i]] = name
		}
	}
	name = ""; ver = ""; deps = ""; provides = ""
}

END {
	# BFS dependency resolution
	iterations = 0
	while (1) {
		changed = 0
		for (pkg in queue) {
			if (pkg in resolved) continue
			resolved[pkg] = 1
			changed = 1

			# Resolve the package name
			actual = pkg
			if (!(actual in versions) && (actual in provider))
				actual = provider[actual]

			n = split(alldeps[actual], d, " ")
			for (i = 1; i <= n; i++) {
				dep = d[i]
				# Strip version constraints
				sub(/[>=<].*/, "", dep)
				# Resolve so: dependencies via providers
				if (dep ~ /^so:/) {
					if (dep in provider)
						dep = provider[dep]
					else
						continue
				}
				if (dep ~ /^!/) continue
				if (dep == "") continue
				if (!(dep in resolved))
					queue[dep] = 1
			}
		}
		if (!changed) break
		if (++iterations > 100) break
	}

	# Output resolved package list
	for (pkg in resolved) {
		actual = pkg
		if (!(actual in versions) && (actual in provider))
			actual = provider[actual]
		if (actual in versions)
			print actual
	}
}
' "$WORKDIR/allindex" | sort -u > "$RESOLVED"

NPKGS=$(wc -l < "$RESOLVED" | tr -d ' ')
echo "Resolved $NPKGS packages (from $# requested)"

# Step 2: Download all resolved packages.
echo "Downloading packages..."
DLDIR="$WORKDIR/downloads"
mkdir -p "$DLDIR"

# Build name->repo+version mapping.
for repo in $REPOS; do
	idx="$WORKDIR/idx_${repo}/APKINDEX"
	[ -f "$idx" ] || continue
	awk -v repo="$repo" '
		/^P:/ { name = substr($0, 3) }
		/^V:/ { ver = substr($0, 3) }
		/^$/ { if (name != "") print name, repo, ver; name=""; ver="" }
	' "$idx"
done > "$WORKDIR/pkgmap"

downloaded=0
while read -r pkg; do
	info=$(grep "^$pkg " "$WORKDIR/pkgmap" | head -1)
	if [ -z "$info" ]; then
		continue
	fi
	repo=$(echo "$info" | awk '{print $2}')
	ver=$(echo "$info" | awk '{print $3}')
	fname="${pkg}-${ver}.apk"
	url="$MIRROR/$ALPINE_VER/$repo/$ARCH/$fname"

	if [ ! -f "$DLDIR/$fname" ]; then
		curl -sL "$url" -o "$DLDIR/$fname"
		downloaded=$((downloaded + 1))
		printf "\r  Downloaded %d/%d" "$downloaded" "$NPKGS"
	fi
done < "$RESOLVED"
echo ""

# Step 3: Patch and repackage.
echo "Patching ELF binaries..."
patched_count=0
total_patches=0

for apkfile in "$DLDIR"/*.apk; do
	[ -f "$apkfile" ] || continue
	fname=$(basename "$apkfile")
	pkgdir="$WORKDIR/pkg_$$"
	rm -rf "$pkgdir"
	mkdir -p "$pkgdir"
	tar -xzf "$apkfile" -C "$pkgdir" 2>/dev/null || continue

	# Patch ELF files.
	find "$pkgdir" -type f | while read -r f; do
		head=$(head -c 4 "$f" 2>/dev/null | od -A n -t x1 2>/dev/null | tr -d ' ')
		if [ "$head" = "7f454c46" ]; then
			"$AOT_PATCH" "$f"
		fi
	done

	# Remove signatures, repackage.
	rm -f "$pkgdir"/.SIGN.*
	(cd "$pkgdir" && tar -czf "$OUTDIR/$fname" .)
	rm -rf "$pkgdir"

	patched_count=$((patched_count + 1))
	printf "\r  Patched %d/%d" "$patched_count" "$NPKGS"
done
echo ""

# Step 4: Generate APKINDEX.
echo "Generating APKINDEX..."
(
	for apkfile in "$OUTDIR"/*.apk; do
		[ -f "$apkfile" ] || continue
		pkginfo=$(tar -xzf "$apkfile" -O .PKGINFO 2>/dev/null || true)
		if [ -n "$pkginfo" ]; then
			echo "$pkginfo" | grep -E '^(pkgname|pkgver|arch|size|pkgdesc|url|depend|provides|install_if|replaces|triggers)' || true
			size=$(wc -c < "$apkfile" | tr -d ' ')
			echo "S:$size"
			echo ""
		fi
	done
) > "$WORKDIR/APKINDEX"

tar -czf "$OUTDIR/APKINDEX.tar.gz" -C "$WORKDIR" APKINDEX

final_count=$(ls "$OUTDIR"/*.apk 2>/dev/null | wc -l | tr -d ' ')
total_size=$(du -sh "$OUTDIR" | cut -f1)
echo ""
echo "AOT repository built: $final_count packages, $total_size"
echo "Output: $OUTDIR/"
