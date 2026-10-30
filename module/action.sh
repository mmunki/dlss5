#!/system/bin/sh
# Magisk action button: print the live status.

MODDIR=${0%/*}
BIN=$MODDIR/system/bin/dlss5d
if [ ! -x "$BIN" ]; then
    BIN=/system/bin/dlss5d
fi
"$BIN" --status 2>/dev/null || echo '{"running": false}'
echo
echo "config: /data/adb/dlss5/config.json"
echo "log:    /data/adb/dlss5/dlss5.log"
