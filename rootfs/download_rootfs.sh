#!/bin/sh
# Download Alpine Linux aarch64 minirootfs.
set -e

ALPINE_VERSION="3.21"
ALPINE_RELEASE="3.21.3"
ARCH="aarch64"
URL="https://dl-cdn.alpinelinux.org/alpine/v${ALPINE_VERSION}/releases/${ARCH}/alpine-minirootfs-${ALPINE_RELEASE}-${ARCH}.tar.gz"
OUTDIR="$(dirname "$0")/alpine"

mkdir -p "$OUTDIR"

echo "Downloading Alpine ${ALPINE_RELEASE} minirootfs for ${ARCH}..."
curl -L -o /tmp/alpine-minirootfs.tar.gz "$URL"

echo "Extracting to $OUTDIR..."
tar xzf /tmp/alpine-minirootfs.tar.gz -C "$OUTDIR"
rm /tmp/alpine-minirootfs.tar.gz

# Ensure /tmp exists in rootfs
mkdir -p "$OUTDIR/tmp"

echo "Done. Rootfs at $OUTDIR"
