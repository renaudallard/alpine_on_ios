#!/bin/sh
# Install and launch Firefox with all dependencies.
set -e

echo "Checking packages..."

# Install X11 components if missing
if ! command -v X >/dev/null 2>&1; then
    apk update
    apk add xorg-server xf86-video-fbdev xf86-input-evdev xterm
fi

# Install window manager if missing
if ! command -v openbox >/dev/null 2>&1; then
    apk add openbox
fi

# Install Firefox if missing
if ! command -v firefox >/dev/null 2>&1; then
    apk add firefox font-noto ca-certificates dbus
fi

# Set up runtime directories
mkdir -p /tmp/xdg /tmp/.X11-unix
export XDG_RUNTIME_DIR=/tmp/xdg
export HOME=/root

# Start X server on framebuffer
X :0 -config /etc/X11/xorg.conf -nolisten tcp &
sleep 2
export DISPLAY=:0

# Start window manager
openbox &
sleep 1

# Start Firefox
exec firefox --no-remote
