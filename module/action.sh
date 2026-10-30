#!/system/bin/sh
# Magisk action button: print the live status.

MODDIR=${0%/*}
DATA=${D5_RUNTIME_DIR:-/data/adb/dlss5}
BIN="$DATA/bin/dlss5d"

# The runtime copy is always executable; fall back to the mounted binary and
# repair the execute bit if an older install left it at 0644.
[ -x "$BIN" ] || {
    [ -f "$DATA/bin/dlss5d" ] && chmod 0755 "$DATA/bin/dlss5d" 2>/dev/null
}
[ -x "$BIN" ] || [ ! -x "$MODDIR/system/bin/dlss5d" ] || BIN="$MODDIR/system/bin/dlss5d"
[ -x "$BIN" ] || {
    chmod 0755 "$MODDIR/system/bin/dlss5d" 2>/dev/null
    [ ! -x "$MODDIR/system/bin/dlss5d" ] || BIN="$MODDIR/system/bin/dlss5d"
}

if [ -x "$BIN" ]; then
    "$BIN" --status 2>/dev/null
else
    echo '{"running": false, "error": "no executable daemon"}'
fi
echo
if pidof dlss5d >/dev/null 2>&1; then
    echo "daemon: running ($BIN)"
else
    echo "daemon: stopped - open the app and tap 'Restart daemon'"
fi
echo "config: $DATA/config.json"
echo "log:    $DATA/dlss5.log"
