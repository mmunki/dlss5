#!/system/bin/sh
# Runs inside the module installer.  $MODPATH is the module directory.

ui_print " "
ui_print "  DLSS5 1.0.0 - per-game render engine"
ui_print " "

ABI="$(getprop ro.product.cpu.abi)"
case "$ABI" in
    arm64*) ui_print "- ABI $ABI: supported" ;;
    *) abort "! DLSS5 only supports arm64-v8a devices (this is $ABI)" ;;
esac

if [ -d "$MODPATH/system/bin" ]; then
    set_perm_recursive "$MODPATH/system/bin" 0 0 0755 0755
fi
set_perm_recursive "$MODPATH" 0 0 0755 0644
set_perm "$MODPATH/service.sh" 0 0 0755
set_perm "$MODPATH/action.sh" 0 0 0755
set_perm "$MODPATH/uninstall.sh" 0 0 0755
set_perm "$MODPATH/customize.sh" 0 0 0755

ui_print "- daemon: /system/bin/dlss5d (static, arm64-v8a)"
ui_print "- config: /data/adb/dlss5/config.json"
ui_print "- install DLSS5.apk from the release for the app and the tile"
ui_print "- reboot, then add the DLSS5 tile to the control centre"
