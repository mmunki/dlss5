#!/system/bin/sh
# Runs inside the module installer.  $MODPATH is the module directory.
#
# Permission order matters.  Magisk applies `set_perm_recursive $MODPATH 0 0
# 0755 0644` before this script runs, and set_perm_recursive walks the whole
# tree, so every file - including system/bin/dlss5d - is 0644 at this point.
# The daemon must be made executable *after* the last recursive call: if the
# binary stays 0644 the shell answers
#
#     /system/bin/sh: dlss5d: can't execute: Permission denied
#
# and both the boot script and the app see "not executable" / "missing".
# The runtime copy in $DATA/bin does not depend on the magic mount and is what
# the app and the boot script actually execute.

ui_print " "
ui_print "  DLSS5 2.0.1 - per-game ultra graphics engine"
ui_print "  msaa force . cpu+gpu boost . refresh lock . game ui mode . ai scale"
ui_print " "

DATA="${D5_RUNTIME_DIR:-/data/adb/dlss5}"

ABI="$(getprop ro.product.cpu.abi)"
case "$ABI" in
    arm64*) ui_print "- ABI $ABI: supported" ;;
    *) abort "! DLSS5 only supports arm64-v8a devices (this is $ABI)" ;;
esac

# 1. Read-only payload first: directories 0755, files 0644.  This is the last
#    recursive call on the whole tree, everything after it is per file.
set_perm_recursive "$MODPATH" 0 0 0755 0644
rm -f "$MODPATH/update"

# 2. Scripts and the daemon are executable.  These calls must stay last.
set_perm "$MODPATH" 0 0 0755
for script in customize.sh service.sh action.sh uninstall.sh; do
    [ -f "$MODPATH/$script" ] && set_perm "$MODPATH/$script" 0 0 0755
done
if [ -d "$MODPATH/system/bin" ]; then
    set_perm_recursive "$MODPATH/system/bin" 0 0 0755 0755
fi

# 3. Verify instead of assuming: an install that cannot execute the daemon is
#    a failed install and must say so.
BIN="$MODPATH/system/bin/dlss5d"
if [ ! -f "$BIN" ]; then
    ui_print "! $BIN is missing from the archive"
    abort "! DLSS5 daemon not installed"
fi
chmod 0755 "$BIN" 2>/dev/null
if [ ! -x "$BIN" ]; then
    ui_print "! $BIN is not executable after installation"
    abort "! DLSS5 daemon is not executable"
fi

# 4. A directly executable copy that does not depend on the magisk mount.
mkdir -p "$DATA/bin" 2>/dev/null
chmod 700 "$DATA" 2>/dev/null
cp -f "$BIN" "$DATA/bin/dlss5d" 2>/dev/null
chmod 0755 "$DATA/bin/dlss5d" 2>/dev/null
chown 0:0 "$DATA/bin/dlss5d" 2>/dev/null || true
rm -f "$DATA/stop" 2>/dev/null || true
[ -x "$DATA/bin/dlss5d" ] \
    && ui_print "- daemon: $DATA/bin/dlss5d (static, arm64-v8a)" \
    || ui_print "! runtime copy failed, the magisk mount will be used after reboot"

ui_print "- module: /data/adb/modules/dlss5 (also mounted as /system/bin/dlss5d)"
ui_print "- config: $DATA/config.json"
ui_print "- install the control app from the release for the tile and the picker"
ui_print "- reboot, then add the DLSS5 tile to the control centre"
