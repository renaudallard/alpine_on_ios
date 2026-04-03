export HOME=/root
export TERM=xterm-256color
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
export SHELL=/bin/sh
export USER=root
export XDG_RUNTIME_DIR=/tmp/xdg

# Helper to start X
startx_fb() {
    mkdir -p /tmp/xdg
    X :0 -config /etc/X11/xorg.conf &
    sleep 1
    export DISPLAY=:0
    if [ -f ~/.xinitrc ]; then
        . ~/.xinitrc
    fi
}
