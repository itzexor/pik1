#!/usr/bin/env bash
set -euo pipefail

fail() {
    echo "test_templates: $*" >&2
    exit 1
}

contains_file() {
    local file=$1
    local needle=$2
    grep -Fq "$needle" "$file" || fail "$file missing: $needle"
}

not_contains_file() {
    local file=$1
    local needle=$2
    ! grep -Fq "$needle" "$file" || fail "$file still contains: $needle"
}

contains_file S99pik1.in "listen:\$TCP_ADDR:\$TCP_PORT"
contains_file S99pik1.in "mkdir -p /run/pik1"
contains_file pik1.service.in "forward:127.0.0.1:7125"
contains_file pik1.service.in "RuntimeDirectoryPreserve=yes"
contains_file pik1-peer-reboot.service.in "PiK1 peer"
contains_file pik1-peer-poweroff.service.in "PiK1 peer"
contains_file pik1-peer-reboot.service.in "ConditionPathExists=!/run/pik1/peer-initiated"
contains_file pik1-peer-poweroff.service.in "ConditionPathExists=!/run/pik1/peer-initiated"
contains_file shutdown_command.sh.in "poweroff-peer"
contains_file shutdown_command.sh.in "reboot-peer"
contains_file shutdown_command.sh.in "@INSTALL_DIR@/pik1d"
contains_file pik1-polkit.rules.in "org.freedesktop.login1.power-off"
contains_file pik1-polkit.rules.in "@PIK1_USER@"

not_contains_file S99pik1.in "tcp:"
not_contains_file pik1.service.in "tcp:"
not_contains_file pik1-peer-reboot.service.in "exporter"
not_contains_file pik1-peer-poweroff.service.in "exporter"

echo "test_templates: ok"
