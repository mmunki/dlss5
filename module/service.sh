#!/system/bin/sh
# late_start service: wait for the framework, then keep the daemon alive.

MODDIR=${0%/*}
DATA=/data/adb/dlss5
LOG=$DATA/dlss5.log
BIN=$MODDIR/system/bin/dlss5d

mkdir -p "$DATA"
chmod 700 "$DATA"

# Give the package manager and GameManager time to come up.
i=0
while [ "$(getprop sys.boot_completed)" != "1" ] && [ $i -lt 120 ]; do
    sleep 2
    i=$((i + 1))
done

[ -x "$BIN" ] || { echo "missing $BIN" >>"$LOG"; exit 1; }

pkill -f dlss5d 2>/dev/null
sleep 1
nohup "$BIN" --daemon >>"$LOG" 2>&1 &
echo "started pid $! at $(date)" >>"$LOG"
