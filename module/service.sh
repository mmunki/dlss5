#!/system/bin/sh
# late_start service: repair the installation, then keep the daemon alive.
#
# The daemon binary is executed from $DATA/bin, a normal file in /data/adb.
# That copy does not depend on the magisk mount being ready and it is always
# executable, which is what the old builds got wrong: Magisk's installer sets
# every module file to 0644, so /system/bin/dlss5d existed but could not be
# executed ("Permission denied", "daemon not running", "binary missing").

MODDIR=${0%/*}
DATA=${D5_RUNTIME_DIR:-/data/adb/dlss5}
BIN="$DATA/bin/dlss5d"
SRC="$MODDIR/system/bin/dlss5d"
LOG="$DATA/dlss5.log"

mkdir -p "$DATA/bin" 2>/dev/null
chmod 700 "$DATA" 2>/dev/null

# Repair an install made by an older version: the module binary may have lost
# its execute bit.  Fix it in place, then refresh the runtime copy.
[ -f "$SRC" ] && chmod 0755 "$SRC" 2>/dev/null
if [ -f "$SRC" ]; then
    if [ ! -x "$BIN" ] || ! cmp -s "$SRC" "$BIN" 2>/dev/null; then
        cp -f "$SRC" "$BIN" 2>/dev/null
        chmod 0755 "$BIN" 2>/dev/null
        chown 0:0 "$BIN" 2>/dev/null || true
    fi
fi

# Last resort: the mounted path, repaired in place (read-only mount, so this
# only works when the module itself is healthy).
if [ ! -x "$BIN" ]; then
    chmod 0755 "$SRC" 2>/dev/null
    [ -x "$SRC" ] && BIN="$SRC"
fi
if [ ! -x "$BIN" ]; then
    echo "no executable daemon at $DATA/bin/dlss5d or $SRC" >>"$LOG"
    exit 1
fi

# Give the package manager and GameManager time to come up.
i=0
while [ "$(getprop sys.boot_completed)" != "1" ] && [ $i -lt 120 ]; do
    sleep 2
    i=$((i + 1))
done

echo "service: daemon $BIN at $(date)" >>"$LOG"

# Watchdog.  The daemon holds a single-instance lock, so a second start (from
# the tile or the app) is refused with exit code 3 instead of applying the
# profile twice.  Touch $DATA/stop before stopping the daemon on uninstall.
for pid in $(pidof dlss5d 2>/dev/null); do
    kill "$pid" 2>/dev/null
done
sleep 1

while [ ! -f "$DATA/stop" ] && [ ! -f "$MODDIR/remove" ]; do
    if ! pidof dlss5d >/dev/null 2>&1; then
        "$BIN" --daemon >>"$LOG" 2>&1
        rc=$?
        echo "watchdog: daemon exited rc=$rc at $(date)" >>"$LOG"
        [ "$rc" = "3" ] && sleep 5
    fi
    sleep 5
done
echo "watchdog: stopped at $(date)" >>"$LOG"
