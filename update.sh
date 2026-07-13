#!/bin/sh
# Install or update the local PiK1 install from git: uninstall with the
# current Makefile, pull, then install the new version. Run from the repo
# checkout on the target device. The target is auto-detected (the Pi runs
# systemd, the K1 does not); pass k1 or pi to override.
set -eu

usage() {
    echo "usage: $0 [-n|--no-screen] [k1|pi]" >&2
    echo "  -n, --no-screen   disable the K1 screen services and omit the TCP tunnel" >&2
    exit 1
}

screen=1
target=
for arg in "$@"; do
    case "$arg" in
    k1|pi)
        target=$arg
        ;;
    -n|--no-screen)
        screen=0
        ;;
    *)
        usage
        ;;
    esac
done

if [ -z "$target" ]; then
    if command -v systemctl > /dev/null 2>&1; then
        target=pi
    else
        target=k1
    fi
fi
cd "$(dirname "$0")"

echo "updating $target install (screen=$screen)"
case "$target" in
k1)
    installed=0
    if [ -e /etc/init.d/S99pik1 ]; then
        installed=1
    fi
    if [ "$installed" = 1 ]; then
        /etc/init.d/S99pik1 stop || true
        make uninstall-k1
    fi
    git pull --ff-only
    make install-k1 SCREEN=$screen
    /etc/init.d/S99pik1 start
    echo "update.sh: done"
    if [ "$installed" = 0 ]; then
        echo "update.sh: first install -- reboot the K1 so the stock services stay down and pik1 starts from a clean boot"
    fi
    ;;
pi)
    installed=0
    if [ -e /etc/systemd/system/pik1.service ]; then
        installed=1
    fi
    if [ "$installed" = 1 ]; then
        sudo systemctl stop pik1.service 2>/dev/null || true
        make uninstall-pi
    fi
    git pull --ff-only
    make install-pi SCREEN=$screen
    sudo systemctl start pik1.service
    echo "update.sh: done"
    ;;
esac
