#!/bin/sh

# Set up environment
export XDG_RUNTIME_DIR=/tmp/xdg
mkdir -p "$XDG_RUNTIME_DIR"

# Start a simple window manager
if command -v openbox >/dev/null 2>&1; then
    openbox &
elif command -v dwm >/dev/null 2>&1; then
    dwm &
elif command -v twm >/dev/null 2>&1; then
    twm &
fi

# Start an xterm by default
if command -v xterm >/dev/null 2>&1; then
    xterm &
fi

# Wait for child processes
wait
