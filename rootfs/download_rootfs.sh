#!/bin/sh
# Download Alpine Linux aarch64 minirootfs.
set -e

ALPINE_VERSION="3.21"
ALPINE_RELEASE="3.21.3"
ARCH="aarch64"
URL="https://dl-cdn.alpinelinux.org/alpine/v${ALPINE_VERSION}/releases/${ARCH}/alpine-minirootfs-${ALPINE_RELEASE}-${ARCH}.tar.gz"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
OUTDIR="$SCRIPT_DIR/alpine"
TMPDIR="$PROJECT_DIR/tmp"

mkdir -p "$OUTDIR" "$TMPDIR"

echo "Downloading Alpine ${ALPINE_RELEASE} minirootfs for ${ARCH}..."
curl -fL -o "$TMPDIR/alpine-minirootfs.tar.gz" "$URL"

echo "Extracting to $OUTDIR..."
tar xzf "$TMPDIR/alpine-minirootfs.tar.gz" -C "$OUTDIR"
rm -f "$TMPDIR/alpine-minirootfs.tar.gz"

mkdir -p "$OUTDIR/tmp"

echo "Done. Rootfs at $OUTDIR"
