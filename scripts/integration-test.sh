#!/usr/bin/env bash
# Integration test of the daemon on the build host.
#
# A stub PATH provides pm/cmd/dumpsys, so the real apply/restore code runs:
# the commands the daemon would send to Android are captured in a log and
# asserted.  Nothing on the host is modified.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HOST_BIN="${1:-$ROOT/build/dlss5-host}"
[ -x "$HOST_BIN" ] || { echo "error: $HOST_BIN not built" >&2; exit 1; }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
STUBS="$WORK/bin"
STATE="$WORK/state"
mkdir -p "$STUBS" "$STATE"

cat >"$STUBS/pm" <<'EOF'
#!/bin/sh
if [ "$1" = "list" ]; then
    echo "package:com.example.game"
    echo "package:com.example.other"
fi
exit 0
EOF

cat >"$STUBS/cmd" <<'EOF'
#!/bin/sh
printf '%s\n' "$*" >>"$D5_CMD_LOG"
exit 0
EOF

cat >"$STUBS/dumpsys" <<'EOF'
#!/bin/sh
case "$1" in
    power) echo "mWakefulness=Awake" ;;
    activity) echo "  topResumedActivity=ActivityRecord{1a2b u0 com.example.game/.Main t42}" ;;
    *) echo "" ;;
esac
EOF
chmod 0755 "$STUBS"/*

export PATH="$STUBS:$PATH"
export D5_CMD_LOG="$WORK/commands.log"

CONFIG="$STATE/config.json"
STATUS="$STATE/status.json"
APPLIED="$STATE/applied.json"

write_config() {
    cat >"$CONFIG" <<EOF
{"version": 1, "enabled": $1, "ai": true, "gpu_boost": false,
 "downscale": 0.70, "target_fps": 60, "saturation": 90,
 "games": ["com.example.game"]}
EOF
}

failures=0
check() {
    local what="$1"
    shift
    if ! "$@"; then
        echo "FAIL: $what" >&2
        failures=$((failures + 1))
    fi
}

contains() { grep -qF -- "$1" "$2"; }
matches() { grep -qE -- "$1" "$2"; }

echo "==> integration: apply and restore"
: >"$D5_CMD_LOG"
write_config true
"$HOST_BIN" --once --package com.example.game --config "$CONFIG" \
    --status-file "$STATUS" --applied-file "$APPLIED" >/dev/null 2>&1

check "render scale command" matches \
    "game set --downscale (0\.70|0\.75|0\.80|0\.85|0\.90) --fps 60 com.example.game" "$D5_CMD_LOG"
check "colour stage command" contains \
    "color_display set-layer-saturation 90 com.mmunki.dlss5 com.example.game" "$D5_CMD_LOG"
check "restore game mode" contains "game reset com.example.game" "$D5_CMD_LOG"
check "restore colour" contains \
    "color_display set-layer-saturation 100 com.mmunki.dlss5 com.example.game" "$D5_CMD_LOG"
check "applied state cleared" test ! -e "$APPLIED"
check "no governor writes without gpu_boost" bash -c \
    "! grep -q kgsl '$D5_CMD_LOG'"

echo "==> integration: packages not installed by the user are never touched"
: >"$D5_CMD_LOG"
write_config true
"$HOST_BIN" --once --package com.example.notinstalled --config "$CONFIG" \
    --status-file "$STATUS" --applied-file "$APPLIED" >/dev/null 2>&1
check "nothing applied" test ! -s "$D5_CMD_LOG"

echo "==> integration: master switch off does nothing"
: >"$D5_CMD_LOG"
write_config false
"$HOST_BIN" --once --package com.example.game --config "$CONFIG" \
    --status-file "$STATUS" --applied-file "$APPLIED" >/dev/null 2>&1
check "nothing applied while disabled" test ! -s "$D5_CMD_LOG"

echo "==> integration: daemon loop boosts the foreground game and restores on SIGTERM"
: >"$D5_CMD_LOG"
write_config true
"$HOST_BIN" --daemon --config "$CONFIG" --status-file "$STATUS" \
    --applied-file "$APPLIED" >"$WORK/daemon.log" 2>&1 &
daemon_pid=$!
sleep 5
check "daemon applied the foreground game" contains \
    "game set --downscale" "$D5_CMD_LOG"
check "status file written" test -s "$STATUS"
check "status names the game" grep -q "com.example.game" "$STATUS"
kill -TERM "$daemon_pid" 2>/dev/null || true
wait "$daemon_pid" 2>/dev/null || true
check "daemon restored on SIGTERM" contains "game reset com.example.game" "$D5_CMD_LOG"
check "daemon cleaned its state" test ! -e "$APPLIED"

if [ "$failures" -ne 0 ]; then
    echo "$failures integration check(s) failed" >&2
    echo "--- commands ---" >&2
    cat "$D5_CMD_LOG" >&2
    echo "--- daemon log ---" >&2
    cat "$WORK/daemon.log" >&2
    exit 1
fi
echo "integration checks passed"
