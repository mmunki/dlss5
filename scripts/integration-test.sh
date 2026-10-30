#!/usr/bin/env bash
# Integration test of the daemon on the build host.
#
# A stub PATH provides pm/cmd/dumpsys/settings/wm/setprop/getprop, so the
# real apply/restore code runs: the commands the daemon would send to
# Android are captured in a log and asserted.  Nothing on the host is
# modified.
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

cat >"$STUBS/setprop" <<'EOF'
#!/bin/sh
printf '%s\n' "$0 $*" >>"$D5_CMD_LOG"
exit 0
EOF

cat >"$STUBS/getprop" <<'EOF'
#!/bin/sh
case "$*" in
    *debug.egl.msaa*) echo "${D5_PROP_MSAA_VALUE:-}" ;;
    *force_msaa*) echo "${D5_PROP_FORCE_VALUE:-}" ;;
    *) : ;;
esac
exit 0
EOF

cat >"$STUBS/resetprop" <<'EOF'
#!/bin/sh
# Simulates a kernel-managed Magisk build without resetprop: the daemon must
# fall back to writing the empty value and log the attempt.
printf '%s\n' "$0 $*" >>"$D5_CMD_LOG"
exit 1
EOF

cat >"$STUBS/settings" <<'EOF'
#!/bin/sh
if [ "$1" = "get" ]; then
    case "$*" in
        *peak_refresh_rate*) echo "${D5_SET_PEAK:-null}" ;;
        *min_refresh_rate*) echo "${D5_SET_MIN:-null}" ;;
        *) echo "null" ;;
    esac
    exit 0
fi
printf '%s\n' "$0 $*" >>"$D5_CMD_LOG"
exit 0
EOF

cat >"$STUBS/wm" <<'EOF'
#!/bin/sh
if [ "$1" = "density" ] && [ -z "$2" ]; then
    echo "Physical density: 420"
    [ -n "${D5_WM_OVERRIDE:-}" ] && echo "Override density: $D5_WM_OVERRIDE"
    exit 0
fi
printf '%s\n' "$0 $*" >>"$D5_CMD_LOG"
exit 0
EOF

chmod 0755 "$STUBS"/*

export PATH="$STUBS:$PATH"
export D5_CMD_LOG="$WORK/commands.log"

CONFIG="$STATE/config.json"
STATUS="$STATE/status.json"
APPLIED="$STATE/applied.json"

# write_config <enabled> <msaa> <ui_delta> <refresh_lock>
write_config() {
    cat >"$CONFIG" <<EOF
{"version": 2, "enabled": $1, "ai": true, "gpu_boost": false,
 "cpu_boost": false, "msaa": $2, "refresh_lock": $4, "refresh_rate": 90,
 "ui_density_delta": $3, "hud": true, "downscale": 0.70, "target_fps": 60,
 "saturation": 90, "games": ["com.example.game"]}
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
absent() { ! matches "$2" "$1"; } # absent "file" "pattern"

echo "==> integration: apply and restore"
: >"$D5_CMD_LOG"
write_config true 0 0 0
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
check "no governor writes without gpu_boost" absent \
    "$D5_CMD_LOG" "kgsl"
check "no MSAA prop while msaa disabled" absent \
    "$D5_CMD_LOG" "debug.egl"
check "no refresh setting while refresh lock disabled" absent \
    "$D5_CMD_LOG" "peak_refresh_rate"
check "no wm density while ui shift is 0" absent \
    "$D5_CMD_LOG" "wm density"

echo "==> integration: the ultra stages apply and restore"
: >"$D5_CMD_LOG"
write_config true 8 -60 1
"$HOST_BIN" --once --package com.example.game --config "$CONFIG" \
    --status-file "$STATUS" --applied-file "$APPLIED" >/dev/null 2>&1

check "msaa 8x prop set" contains "setprop debug.egl.msaa 8" "$D5_CMD_LOG"
check "force msaa prop set" contains "setprop debug.egl.force_msaa 1" "$D5_CMD_LOG"
check "refresh peak locked" contains "settings put system peak_refresh_rate 90" "$D5_CMD_LOG"
check "refresh min locked" contains "settings put system min_refresh_rate 90" "$D5_CMD_LOG"
check "game ui density shifted" contains "wm density 360" "$D5_CMD_LOG"

check "msaa prop cleared (was unset)" matches \
    "setprop debug\.egl\.msaa *$" "$D5_CMD_LOG"
check "force msaa prop cleared" matches \
    "debug\.egl\.force_msaa *$" "$D5_CMD_LOG"
check "refresh peak restored (was unset)" contains \
    "settings delete system peak_refresh_rate" "$D5_CMD_LOG"
check "refresh min restored (was unset)" contains \
    "settings delete system min_refresh_rate" "$D5_CMD_LOG"
check "game ui density reset" contains "wm density reset" "$D5_CMD_LOG"
check "applied state cleared" test ! -e "$APPLIED"
check "no cpu writes with cpu_boost disabled" absent \
    "$D5_CMD_LOG" "scaling_governor"

echo "==> integration: previously set msaa/refresh/override values are restored, not cleared"
: >"$D5_CMD_LOG"
D5_PROP_MSAA_VALUE=4 D5_SET_PEAK=60 D5_SET_MIN=60 D5_WM_OVERRIDE=460 \
    "$HOST_BIN" --once --package com.example.game --config "$CONFIG" \
    --status-file "$STATUS" --applied-file "$APPLIED" >/dev/null 2>&1

check "msaa 4x applied" contains "setprop debug.egl.msaa 4" "$D5_CMD_LOG"
check "ui shift relative to the existing override" contains "wm density 400" "$D5_CMD_LOG"

check "msaa original value restored" matches \
    "setprop debug.egl.msaa 4" "$D5_CMD_LOG"
check "msaa prop was not cleared" absent \
    "$D5_CMD_LOG" 'setprop debug\.egl\.msaa *$'
check "wm override restored, not reset" matches \
    "wm density 460" "$D5_CMD_LOG"
check "refresh peak restored to 60" contains \
    "settings put system peak_refresh_rate 60" "$D5_CMD_LOG"
check "refresh min restored to 60" contains \
    "settings put system min_refresh_rate 60" "$D5_CMD_LOG"
check "applied state cleared" test ! -e "$APPLIED"

echo "==> integration: packages not installed by the user are never touched"
: >"$D5_CMD_LOG"
write_config true 0 0 0
"$HOST_BIN" --once --package com.example.notinstalled --config "$CONFIG" \
    --status-file "$STATUS" --applied-file "$APPLIED" >/dev/null 2>&1
check "nothing applied" test ! -s "$D5_CMD_LOG"

echo "==> integration: master switch off does nothing"
: >"$D5_CMD_LOG"
write_config false 0 0 0
"$HOST_BIN" --once --package com.example.game --config "$CONFIG" \
    --status-file "$STATUS" --applied-file "$APPLIED" >/dev/null 2>&1
check "nothing applied while disabled" test ! -s "$D5_CMD_LOG"

echo "==> integration: daemon loop boosts the foreground game and restores on SIGTERM"
: >"$D5_CMD_LOG"
write_config true 4 0 1
LOCK="$STATE/daemon.lock"
"$HOST_BIN" --daemon --config "$CONFIG" --status-file "$STATUS" \
    --applied-file "$APPLIED" --lock-file "$LOCK" >"$WORK/daemon.log" 2>&1 &
daemon_pid=$!
sleep 5
check "daemon applied the foreground game" contains \
    "game set --downscale" "$D5_CMD_LOG"
check "daemon forces msaa in the loop" contains \
    "setprop debug.egl.msaa 4" "$D5_CMD_LOG"
check "status file written" test -s "$STATUS"
check "status names the game" grep -q "com.example.game" "$STATUS"
check "status carries the new fields" matches \
    '"msaa": (4|8)|"cpu": [0-9]+|"refresh": 90' "$STATUS"

echo "==> integration: a second daemon refuses to run twice"
lines_before=$(wc -l <"$D5_CMD_LOG")
second_rc=0
"$HOST_BIN" --daemon --config "$CONFIG" --status-file "$STATUS" \
    --applied-file "$APPLIED" --lock-file "$LOCK" >"$WORK/daemon2.log" 2>&1 \
    || second_rc=$?
check "second daemon exits with the already-running code" \
    test "$second_rc" = "3"
check "second daemon did not touch anything" \
    test "$(wc -l <"$D5_CMD_LOG")" = "$lines_before"
check "second daemon says why" grep -q "already running" "$WORK/daemon2.log"

kill -TERM "$daemon_pid" 2>/dev/null || true
wait "$daemon_pid" 2>/dev/null || true

echo "==> integration: the lock is released after a clean exit"
"$HOST_BIN" --daemon --config "$CONFIG" --status-file "$STATUS" \
    --applied-file "$APPLIED" --lock-file "$LOCK" >"$WORK/daemon3.log" 2>&1 &
third_pid=$!
sleep 1.5
check "a new daemon acquires the released lock" \
    bash -c "! grep -q 'already running' '$WORK/daemon3.log' \
        && grep -q 'starting, config' '$WORK/daemon3.log'"
kill -TERM "$third_pid" 2>/dev/null || true
wait "$third_pid" 2>/dev/null || true
check "daemon restored on SIGTERM" contains "game reset com.example.game" "$D5_CMD_LOG"
check "daemon restored msaa on SIGTERM" matches \
    "setprop debug\.egl\.msaa *$" "$D5_CMD_LOG"
check "daemon restored the refresh lock" contains \
    "settings delete system peak_refresh_rate" "$D5_CMD_LOG"
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
