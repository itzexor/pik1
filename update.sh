#!/bin/sh
# Install or update the local PiK1 install from git: uninstall with the
# current Makefile, pull, then re-exec the freshly pulled script to run the
# install phase, so script changes take effect within the same update. Run
# from the repo checkout on the target device. The target is auto-detected
# (the Pi runs systemd, the K1 does not); pass k1 or pi to override.
set -eu

usage() {
    echo "usage: $0 [-n|--no-screen] [k1|pi]" >&2
    echo "  -n, --no-screen   disable the K1 screen services and omit the TCP tunnel" >&2
    exit 1
}

# Internal second phase, exec'd from the new script version after git pull.
if [ "${1:-}" = "--post-pull" ]; then
    [ $# -eq 4 ] || usage
    target=$2
    screen=$3
    installed=$4
    cd "$(dirname "$0")"

    case "$target" in
    k1)
        make install-k1 SCREEN=$screen
        /etc/init.d/S99pik1 start
        echo "update.sh: done"
        if [ "$installed" = 0 ]; then
            echo "update.sh: first install -- reboot the K1 so the stock services stay down and pik1 starts from a clean boot"
        fi
        ;;
    pi)
        make install-pi SCREEN=$screen
        sudo systemctl start pik1.service
        echo "update.sh: done"
        ;;
    *)
        usage
        ;;
    esac
    exit 0
fi

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
    if [ -d /etc/systemd ]; then
        target=pi
    else
        target=k1
    fi
fi
cd "$(dirname "$0")"

echo "updating $target install (screen=$screen)"
installed=0
case "$target" in
k1)
    if [ -e /etc/init.d/S99pik1 ]; then
        installed=1
        /etc/init.d/S99pik1 stop || true
        make uninstall-k1
    fi
    ;;
pi)
    if [ -e /etc/systemd/system/pik1.service ]; then
        installed=1
        sudo systemctl stop pik1.service 2>/dev/null || true
        make uninstall-pi
    fi
    ;;
esac

git pull --ff-only
exec "$0" --post-pull "$target" "$screen" "$installed"
