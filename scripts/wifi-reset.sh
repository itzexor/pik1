#!/bin/sh
set -u

fail() {
    printf 'wifi-reset: %s\n' "$*" >&2
    exit 1
}

wifi_interfaces() {
    net_class=${PIK1_NET_CLASS_DIR:-/sys/class/net}
    for path in "$net_class"/*; do
        [ -d "$path/wireless" ] || continue
        printf '%s\n' "${path##*/}"
    done
}

# Some embedded systems provide a complete radio/supplicant/DHCP reset.
if [ -x /usr/bin/wifi_down.sh ] && [ -x /usr/bin/wifi_up.sh ]; then
    /usr/bin/wifi_down.sh || fail "wifi_down.sh failed"
    sleep 2
    exec /usr/bin/wifi_up.sh
fi

if command -v nmcli >/dev/null 2>&1 &&
   nmcli -t general status >/dev/null 2>&1; then
    nmcli radio wifi off || fail "could not disable Wi-Fi"
    sleep 2
    exec nmcli radio wifi on
fi

if command -v connmanctl >/dev/null 2>&1 &&
   connmanctl state >/dev/null 2>&1; then
    connmanctl disable wifi || fail "could not disable Wi-Fi"
    sleep 2
    connmanctl enable wifi || fail "could not enable Wi-Fi"
    connmanctl scan wifi >/dev/null 2>&1 || true
    exit 0
fi

interfaces=$(wifi_interfaces)
[ -n "$interfaces" ] || fail "no Wi-Fi interface found"

if command -v systemctl >/dev/null 2>&1; then
    restarted=0
    for iface in $interfaces; do
        unit="wpa_supplicant@${iface}.service"
        if systemctl is-active --quiet "$unit" ||
           systemctl is-enabled --quiet "$unit"; then
            systemctl restart "$unit" || fail "could not restart $unit"
            restarted=1
        fi
    done
    if [ "$restarted" -eq 1 ]; then
        if systemctl is-active --quiet dhcpcd.service ||
           systemctl is-enabled --quiet dhcpcd.service; then
            systemctl restart dhcpcd.service ||
                fail "could not restart dhcpcd.service"
        fi
        exit 0
    fi

    for unit in NetworkManager.service connman.service iwd.service \
                wpa_supplicant.service; do
        if systemctl is-active --quiet "$unit" ||
           systemctl is-enabled --quiet "$unit"; then
            exec systemctl restart "$unit"
        fi
    done
fi

if command -v wpa_cli >/dev/null 2>&1; then
    reset=0
    for iface in $interfaces; do
        if wpa_cli -i "$iface" reconfigure >/dev/null 2>&1 &&
           wpa_cli -i "$iface" reassociate >/dev/null 2>&1; then
            reset=1
        fi
    done
    [ "$reset" -eq 1 ] && exit 0
fi

for init in /etc/init.d/networking /etc/init.d/network; do
    if [ -x "$init" ]; then
        exec "$init" restart
    fi
done

fail "no supported Wi-Fi manager found"
