#!/bin/sh
# Start X server and launch Firefox on the framebuffer.
set -e

mkdir -p /tmp/xdg /tmp/.X11-unix
export XDG_RUNTIME_DIR=/tmp/xdg
export HOME=/root

# Start X server
X :0 -config /etc/X11/xorg.conf -nolisten tcp &
sleep 2
export DISPLAY=:0

# Start D-Bus
if command -v dbus-daemon >/dev/null 2>&1; then
    export DBUS_SESSION_BUS_ADDRESS=unix:path=/tmp/dbus-session
    dbus-daemon --session --address="$DBUS_SESSION_BUS_ADDRESS" \
        --nofork --nopidfile &
fi

# Start window manager
if command -v openbox >/dev/null 2>&1; then
    openbox &
elif command -v marco >/dev/null 2>&1; then
    marco &
fi
sleep 1

# Launch Firefox
exec firefox-esr --no-remote
