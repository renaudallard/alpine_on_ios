export HOME=/root
export TERM=xterm-256color
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
export SHELL=/bin/sh
export USER=root
export XDG_RUNTIME_DIR=/tmp/xdg
export LANG=C.UTF-8

# Start graphical desktop from the shell
startx() {
    mkdir -p /tmp/xdg /tmp/.X11-unix
    X :0 -config /etc/X11/xorg.conf -nolisten tcp &
    sleep 2
    export DISPLAY=:0
    . ~/.xinitrc
}
