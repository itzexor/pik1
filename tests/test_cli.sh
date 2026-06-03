#!/usr/bin/env bash
set -euo pipefail

PIK1D=${PIK1D:-build/pik1d}
TCPBRIDGE=${TCPBRIDGE:-build/tcpbridge}

fail() {
    echo "test_cli: $*" >&2
    exit 1
}

contains() {
    local haystack=$1
    local needle=$2
    [[ "$haystack" == *"$needle"* ]] || fail "expected output to contain: $needle"
}

not_contains() {
    local haystack=$1
    local needle=$2
    [[ "$haystack" != *"$needle"* ]] || fail "expected output not to contain: $needle"
}

out=$("$PIK1D" --version)
contains "$out" "pik1d 0.2.0 protocol=2"

set +e
out=$("$PIK1D" --usb 1d6b:0104 pty:0:/tmp/test tcp:127.0.0.1:7125 2>&1)
rc=$?
set -e
[[ $rc -ne 0 ]] || fail "legacy tcp: form unexpectedly succeeded"
contains "$out" "bad pty spec: tcp:127.0.0.1:7125"

set +e
out=$(timeout 0.2s "$PIK1D" --usb 1d6b:0104 mcu:0:/dev/null:230400 listen:127.0.0.1:7125 2>&1)
rc=$?
set -e
[[ $rc -eq 124 ]] || fail "mcu/listen smoke exited with $rc"
contains "$out" "uart=mcu"
contains "$out" "tcp=listen:127.0.0.1:7125"
contains "$out" "waiting for USB endpoint 0"
[[ "$out" =~ ^[0-9]{4}-[0-9]{2}-[0-9]{2}\ [0-9]{2}:[0-9]{2}:[0-9]{2}\ \[pik1\] ]] || \
    fail "mcu logs did not start with timestamp"

set +e
out=$(timeout 0.2s "$PIK1D" --usb 1d6b:0104 mcu:0:/dev/null:230400 2>&1)
rc=$?
set -e
[[ $rc -eq 124 ]] || fail "mcu/no-tcp smoke exited with $rc"
contains "$out" "uart=mcu"
not_contains "$out" "tcp="

set +e
out=$(timeout 0.2s "$PIK1D" --usb 1d6b:0104 pty:0:/tmp/test forward:127.0.0.1:7125 2>&1)
rc=$?
set -e
[[ $rc -ne 0 ]] || fail "pty/forward smoke unexpectedly kept running"
contains "$out" "uart=pty"
contains "$out" "tcp=forward:127.0.0.1:7125"
[[ ! "$out" =~ ^[0-9]{4}-[0-9]{2}-[0-9]{2}\ [0-9]{2}:[0-9]{2}:[0-9]{2}\ \[pik1\] ]] || \
    fail "pty logs unexpectedly started with timestamp"

set +e
out=$("$PIK1D" --usb 1d6b:0104 mcu:300:/dev/null:230400 2>&1)
rc=$?
set -e
[[ $rc -ne 0 ]] || fail "invalid mcu channel unexpectedly succeeded"
contains "$out" "bad mcu channel id"

set +e
out=$("$PIK1D" --usb 1d6b:0104 mcu:0:/dev/null:0 2>&1)
rc=$?
set -e
[[ $rc -ne 0 ]] || fail "invalid mcu baud unexpectedly succeeded"
contains "$out" "bad mcu baud"

set +e
out=$("$PIK1D" --usb 1d6b:0104 pty:0:/tmp/a pty:0:/tmp/b 2>&1)
rc=$?
set -e
[[ $rc -ne 0 ]] || fail "duplicate pty channel unexpectedly succeeded"
contains "$out" "duplicate channel id: 0"

set +e
out=$("$PIK1D" --usb 1d6b:0104 pty:0:/tmp/test forward:127.0.0.1:0 2>&1)
rc=$?
set -e
[[ $rc -ne 0 ]] || fail "invalid forward port unexpectedly succeeded"
contains "$out" "bad tcp spec"

set +e
out=$("$TCPBRIDGE" /tmp/serial nope 127.0.0.1:1 2>&1)
rc=$?
set -e
[[ $rc -ne 0 ]] || fail "invalid tcpbridge mode unexpectedly succeeded"
contains "$out" "Usage:"

set +e
out=$(timeout 0.2s "$TCPBRIDGE" /tmp/no-such forward 127.0.0.1:1 2>&1)
rc=$?
set -e
[[ $rc -eq 124 ]] || fail "tcpbridge forward smoke exited with $rc"
contains "$out" "[tcp] tcpbridge /tmp/no-such forward 127.0.0.1:1"
[[ ! "$out" =~ ^[0-9]{4}-[0-9]{2}-[0-9]{2}\ [0-9]{2}:[0-9]{2}:[0-9]{2}\ \[tcp\] ]] || \
    fail "tcpbridge forward logs unexpectedly started with timestamp"

echo "test_cli: ok"
