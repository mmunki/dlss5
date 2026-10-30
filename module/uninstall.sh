#!/system/bin/sh
# Remove the runtime state and stop the daemon.
#
# The boot watchdog stops on $DATA/stop, so the marker is written *before* the
# daemon is killed and before /data/adb/dlss5 is removed.

DATA=${D5_RUNTIME_DIR:-/data/adb/dlss5}
touch "$DATA/stop" 2>/dev/null
for pid in $(pidof dlss5d 2>/dev/null); do
    kill "$pid" 2>/dev/null
done
sleep 1
for pid in $(pidof dlss5d 2>/dev/null); do
    kill -9 "$pid" 2>/dev/null
done
rm -rf "$DATA"
