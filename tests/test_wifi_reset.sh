#!/usr/bin/env bash
set -euo pipefail

fail() {
    echo "test_wifi_reset: $*" >&2
    exit 1
}

tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT
mkdir -p "$tmpdir/bin" "$tmpdir/net/wifi-test/wireless"
calls="$tmpdir/calls"

make_mock() {
    local name=$1
    local body=$2
    {
        printf '#!/bin/sh\n'
        printf '%s\n' "$body"
    } >"$tmpdir/bin/$name"
    chmod +x "$tmpdir/bin/$name"
}

run_reset() {
    PIK1_NET_CLASS_DIR="$tmpdir/net" \
    PIK1_WIFI_TEST_CALLS="$calls" \
    PATH="$tmpdir/bin:/usr/bin:/bin" \
        scripts/wifi-reset.sh
}

make_mock nmcli '
[ "$*" = "-t general status" ] && exit 0
printf "nmcli %s\n" "$*" >>"$PIK1_WIFI_TEST_CALLS"'
make_mock sleep 'printf "sleep %s\n" "$*" >>"$PIK1_WIFI_TEST_CALLS"'
run_reset
expected=$'nmcli radio wifi off\nsleep 2\nnmcli radio wifi on'
[[ $(<"$calls") == "$expected" ]] ||
    fail "NetworkManager command sequence was incorrect"

: >"$calls"
make_mock nmcli 'exit 1'
make_mock connmanctl 'exit 1'
make_mock systemctl '
printf "systemctl %s\n" "$*" >>"$PIK1_WIFI_TEST_CALLS"
case "$*" in
    "is-enabled --quiet wpa_supplicant@wifi-test.service" | \
    "is-enabled --quiet dhcpcd.service" | \
    "restart wpa_supplicant@wifi-test.service" | \
    "restart dhcpcd.service")
        exit 0
        ;;
esac
exit 1'
run_reset
expected=$'systemctl is-active --quiet wpa_supplicant@wifi-test.service\nsystemctl is-enabled --quiet wpa_supplicant@wifi-test.service\nsystemctl restart wpa_supplicant@wifi-test.service\nsystemctl is-active --quiet dhcpcd.service\nsystemctl is-enabled --quiet dhcpcd.service\nsystemctl restart dhcpcd.service'
[[ $(<"$calls") == "$expected" ]] ||
    fail "wpa_supplicant/dhcpcd command sequence was incorrect"

: >"$calls"
make_mock systemctl 'exit 1'
make_mock wpa_cli '
printf "wpa_cli %s\n" "$*" >>"$PIK1_WIFI_TEST_CALLS"
exit 0'
run_reset
expected=$'wpa_cli -i wifi-test reconfigure\nwpa_cli -i wifi-test reassociate'
[[ $(<"$calls") == "$expected" ]] ||
    fail "wpa_cli command sequence was incorrect"

echo "test_wifi_reset: ok"
