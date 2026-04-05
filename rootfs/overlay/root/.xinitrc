#!/bin/sh

# Set up environment
export XDG_RUNTIME_DIR=/tmp/xdg
export DBUS_SESSION_BUS_ADDRESS=unix:path=/tmp/dbus-session
mkdir -p "$XDG_RUNTIME_DIR" /tmp/.X11-unix

# Start D-Bus session bus if available
if command -v dbus-daemon >/dev/null 2>&1; then
    dbus-daemon --session --address="$DBUS_SESSION_BUS_ADDRESS" \
        --nofork --nopidfile &
fi

# Start desktop environment or window manager
if command -v mate-session >/dev/null 2>&1; then
    exec mate-session
elif command -v openbox >/dev/null 2>&1; then
    openbox &
    # Start panel and file manager if available
    command -v mate-panel >/dev/null 2>&1 && mate-panel &
    command -v caja >/dev/null 2>&1 && caja --no-desktop &
elif command -v twm >/dev/null 2>&1; then
    twm &
fi

# Start terminal
if command -v mate-terminal >/dev/null 2>&1; then
    mate-terminal &
elif command -v xterm >/dev/null 2>&1; then
    xterm &
fi

wait
