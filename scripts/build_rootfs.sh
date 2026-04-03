#!/bin/sh
# Create a compressed rootfs archive for the app bundle.
set -e

ROOTFS_DIR="$(dirname "$0")/../rootfs/alpine"
if [ ! -d "$ROOTFS_DIR" ]; then
    echo "Rootfs not found. Run rootfs/download_rootfs.sh first."
    exit 1
fi

OVERLAY_DIR="$(dirname "$0")/../rootfs/overlay"

# Apply overlay files (X11 config, profiles, etc.)
if [ -d "$OVERLAY_DIR" ]; then
    echo "Applying overlay..."
    cp -r "$OVERLAY_DIR"/* "$ROOTFS_DIR"/
    chmod +x "$ROOTFS_DIR/root/.xinitrc" 2>/dev/null || true
fi

ARCHIVE="$(dirname "$0")/../rootfs/alpine-rootfs.tar.gz"
echo "Creating rootfs archive..."
tar czf "$ARCHIVE" -C "$ROOTFS_DIR" .
echo "Rootfs archive: $ARCHIVE ($(du -h "$ARCHIVE" | cut -f1))"
