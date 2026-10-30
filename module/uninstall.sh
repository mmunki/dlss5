#!/system/bin/sh
# Remove the runtime state and stop the daemon.

pkill -f dlss5d 2>/dev/null
sleep 1
rm -rf /data/adb/dlss5
