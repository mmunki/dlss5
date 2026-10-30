#!/usr/bin/env bash
# Regression test for the "dlss5d: can't execute: Permission denied" bug.
#
# Magisk's installer runs `set_perm_recursive $MODPATH 0 0 0755 0644` over the
# whole module tree before customize.sh, and customize.sh itself did the same
# recursive call again *after* making system/bin executable.  The daemon ended
# up 0644, so:
#
#   * `dlss5d` on the device shell said "Permission denied" (the file was found
#     on PATH but could not be executed),
#   * service.sh's `[ -x "$BIN" ]` was false, so no daemon started,
#   * the app's `[ -x "$c" ]` was false, so it reported a missing binary even
#     though the file was right there.
#
# The test reproduces the exact Magisk permission pass with real, recursive
# set_perm/set_perm_recursive functions, then sources the shipped customize.sh
# and asserts that the daemon is executable afterwards.  With an APK argument
# it also proves that the APK carries the module as assets/module.zip.
#
#   ./scripts/module-test.sh <module.zip> [apk]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MODULE_ZIP="${1:-}"
APK="${2:-}"

[ -n "$MODULE_ZIP" ] && [ -f "$MODULE_ZIP" ] \
    || { echo "usage: $0 <module.zip> [apk]" >&2; exit 2; }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

failures=0
check() {
    local what="$1"
    shift
    if ! "$@"; then
        echo "FAIL: $what" >&2
        failures=$((failures + 1))
    fi
}

# The Magisk helpers, verbatim semantics: recursive over the whole tree.
set_perm() {
    chown "$2:$3" "$1" 2>/dev/null || true
    chmod "$4" "$1" 2>/dev/null || true
    chcon "${5:-u:object_r:system_file:s0}" "$1" 2>/dev/null || true
    return 0
}

set_perm_recursive() {
    find "$1" -type d 2>/dev/null | while read -r dir; do
        set_perm "$dir" "$2" "$3" "$4" "${6:-}"
    done
    find "$1" -type f 2>/dev/null | while read -r file; do
        set_perm "$file" "$2" "$3" "$5" "${6:-}"
    done
    return 0
}

ui_print() { :; }
abort() { echo "ABORT: $1" >&2; exit 1; }

# The real installer sources customize.sh with the Magisk helpers in scope, so
# the test does the same in a fresh shell.
mkdir -p "$WORK/nobin"
cat >"$WORK/install.sh" <<EOF
#!/bin/sh
set -e
MODPATH="\$1"
ui_print() { :; }
abort() { echo "ABORT: \$1" >&2; exit 1; }
set_perm() {
    chown "\$2:\$3" "\$1" 2>/dev/null || true
    chmod "\$4" "\$1" 2>/dev/null || true
    return 0
}
set_perm_recursive() {
    find "\$1" -type d 2>/dev/null | while read -r dir; do
        set_perm "\$dir" "\$2" "\$3" "\$4"
    done
    find "\$1" -type f 2>/dev/null | while read -r file; do
        set_perm "\$file" "\$2" "\$3" "\$5"
    done
    return 0
}
cd "\$MODPATH"
. "\$MODPATH/customize.sh"
EOF

mode_of() { stat -c '%a' "$1"; }

STUBS="$WORK/stubs"
mkdir -p "$STUBS"
cat >"$STUBS/getprop" <<'EOF'
#!/bin/sh
case "$*" in
    *sys.boot_completed*) echo 1 ;;
    *ro.product.cpu.abi*) echo arm64-v8a ;;
    *) : ;;
esac
exit 0
EOF
cat >"$STUBS/pidof" <<'EOF'
#!/bin/sh
# nothing is running (pidof returns 1 when there is no such process), so the
# watchdog has to start the daemon
exit 1
EOF
chmod 0755 "$STUBS"/*
export PATH="$STUBS:$PATH"

echo "==> module install: Magisk permission pass + customize.sh"
MODPATH="$WORK/modules/dlss5"
RUNTIME="$WORK/runtime"
mkdir -p "$MODPATH"
unzip -q "$MODULE_ZIP" -x 'META-INF/*' -d "$MODPATH"

# What Magisk does before every customize.sh.
set_perm_recursive "$MODPATH" 0 0 0755 0644
check "Magisk's own pass leaves the daemon 0644" \
    test "$(mode_of "$MODPATH/system/bin/dlss5d")" = "644"

D5_RUNTIME_DIR="$RUNTIME" sh "$WORK/install.sh" "$MODPATH" \
    >"$WORK/customize.log" 2>&1 || {
    echo "FAIL: customize.sh exited non-zero" >&2
    cat "$WORK/customize.log" >&2
    failures=$((failures + 1))
}

check "daemon is executable after install" test -x "$MODPATH/system/bin/dlss5d"
check "daemon mode is 0755" \
    test "$(mode_of "$MODPATH/system/bin/dlss5d")" = "755"
check "service.sh is executable" test -x "$MODPATH/service.sh"
check "action.sh is executable" test -x "$MODPATH/action.sh"
check "uninstall.sh is executable" test -x "$MODPATH/uninstall.sh"
check "module.prop stays 0644" test "$(mode_of "$MODPATH/module.prop")" = "644"
check "runtime copy exists" test -f "$RUNTIME/bin/dlss5d"
check "runtime copy is executable" test -x "$RUNTIME/bin/dlss5d"
check "runtime copy mode is 0755" \
    test "$(mode_of "$RUNTIME/bin/dlss5d")" = "755"
check "runtime copy is byte-identical to the module binary" \
    cmp -s "$MODPATH/system/bin/dlss5d" "$RUNTIME/bin/dlss5d"
check "no leftover stop marker" test ! -e "$RUNTIME/stop"

echo "==> service.sh repairs an install left broken by the old build"
BROKEN="$WORK/broken-module"
BROKEN_RUNTIME="$WORK/broken-runtime"
mkdir -p "$BROKEN/system/bin" "$BROKEN_RUNTIME"
cp "$MODPATH/service.sh" "$BROKEN/service.sh"
# The bug: an executable binary that an upgrade demoted to 0644.
cat >"$BROKEN/system/bin/dlss5d" <<'EOF'
#!/bin/sh
exit 0
EOF
chmod 0644 "$BROKEN/system/bin/dlss5d"

D5_RUNTIME_DIR="$BROKEN_RUNTIME" PATH="$STUBS:$PATH" \
    sh "$BROKEN/service.sh" >"$WORK/service.log" 2>&1 &
service_pid=$!
for _ in $(seq 1 20); do
    [ -x "$BROKEN_RUNTIME/bin/dlss5d" ] && break
    sleep 0.25
done
check "service.sh repairs the 0644 module binary" \
    test -x "$BROKEN/system/bin/dlss5d"
check "service.sh installs an executable runtime copy" \
    test -x "$BROKEN_RUNTIME/bin/dlss5d"
for _ in $(seq 1 40); do
    grep -q "watchdog" "$BROKEN_RUNTIME/dlss5.log" 2>/dev/null && break
    sleep 0.25
done
check "service.sh logs the watchdog starting the daemon" \
    grep -q "watchdog" "$BROKEN_RUNTIME/dlss5.log"
touch "$BROKEN_RUNTIME/stop"
for _ in $(seq 1 60); do
    kill -0 "$service_pid" 2>/dev/null || break
    sleep 0.25
done
if kill -0 "$service_pid" 2>/dev/null; then
    kill -9 "$service_pid" 2>/dev/null || true
    check "watchdog exits on the stop marker" false
else
    check "watchdog exits on the stop marker" true
fi

echo "==> app installer: the exact shell script the APK executes"
if [ -d "$ROOT/build/test" ] && command -v java >/dev/null 2>&1; then
    APP_WORK="$WORK/app-install"
    SRC="$APP_WORK/module-src"
    ROOTFS="$APP_WORK/rootfs"
    mkdir -p "$SRC" "$ROOTFS"
    unzip -q "$MODULE_ZIP" -d "$SRC"
    # The real daemon is aarch64; the script only has to prove it is
    # executable and responds to --version, so a shell stand-in is used here.
    cat >"$SRC/system/bin/dlss5d" <<'EOF'
#!/bin/sh
echo "dlss5d 2.0.1 (test stand-in)"
exit 0
EOF
    chmod 0644 "$SRC/system/bin/dlss5d"
    SCRIPT="$(java -cp "$ROOT/build/test" CoreTests \
        --dump-install-script "$SRC")"
    SCRIPT="${SCRIPT//\/data\/adb/$ROOTFS/data/adb}"
    APP_DST="$ROOTFS/data/adb/modules/dlss5"
    APP_DATA="$ROOTFS/data/adb/dlss5"
    if sh -c "$SCRIPT" >"$APP_WORK/install.log" 2>&1; then
        check "app install script reports success" \
            grep -q "MODULE-INSTALLED" "$APP_WORK/install.log"
    else
        check "app install script succeeds" false
        cat "$APP_WORK/install.log" >&2
    fi
    check "app install leaves the module daemon 0755" \
        test "$(mode_of "$APP_DST/system/bin/dlss5d")" = "755"
    check "app install leaves the scripts 0755" \
        test "$(mode_of "$APP_DST/service.sh")" = "755"
    check "app install keeps module.prop 0644" \
        test "$(mode_of "$APP_DST/module.prop")" = "644"
    check "app install drops customize.sh" \
        test ! -e "$APP_DST/customize.sh"
    check "app install writes the runtime daemon" \
        test -x "$APP_DATA/bin/dlss5d"
    check "app install leaves no .new file behind" \
        test ! -e "$APP_DATA/bin/dlss5d.new"
    check "app install ran the daemon version check" \
        grep -q "test stand-in" "$APP_WORK/install.log"
else
    echo "skipped: build/test is not compiled"
fi

if [ -n "$APK" ]; then
    [ -f "$APK" ] || { echo "error: $APK does not exist" >&2; exit 2; }
    echo "==> APK: the module is bundled in assets"
    if ! python3 - "$APK" "$MODULE_ZIP" <<'PY'
import io
import sys
import zipfile

apk_path, module_path = sys.argv[1], sys.argv[2]
failures = 0


def check(cond, what):
    global failures
    if not cond:
        failures += 1
        print("FAIL: " + what, file=sys.stderr)


with zipfile.ZipFile(apk_path) as apk:
    names = apk.namelist()
    check("assets/module.zip" in names, "assets/module.zip is inside the APK")
    if "assets/module.zip" not in names:
        sys.exit(1)
    embedded = apk.read("assets/module.zip")

with zipfile.ZipFile(module_path) as shipped:
    expected = shipped.read("system/bin/dlss5d")
    modes = {i.filename: (i.external_attr >> 16) & 0o777
             for i in shipped.infolist()}

check(modes.get("system/bin/dlss5d") == 0o755,
      "system/bin/dlss5d is 0755 in the module zip")
check(modes.get("service.sh") == 0o755, "service.sh is 0755 in the module zip")
check(expected.startswith(b"\x7fELF"), "the module binary is an ELF file")

with zipfile.ZipFile(io.BytesIO(embedded)) as inner:
    inner_names = inner.namelist()
    check("module.prop" in inner_names, "embedded module has module.prop")
    check("service.sh" in inner_names, "embedded module has service.sh")
    check("system/bin/dlss5d" in inner_names, "embedded module has the daemon")
    check("customize.sh" in inner_names,
          "embedded module keeps customize.sh for manual flashing")
    check(inner.read("system/bin/dlss5d") == expected,
          "embedded daemon is byte-identical to the release daemon")

sys.exit(1 if failures else 0)
PY
    then
        failures=$((failures + 1))
    else
        echo "APK asset checks passed"
    fi
fi

if [ "$failures" -ne 0 ]; then
    echo "$failures module check(s) failed" >&2
    echo "--- customize.sh ---" >&2
    cat "$WORK/customize.log" >&2
    echo "--- service.sh ---" >&2
    cat "$WORK/service.log" >&2
    exit 1
fi
echo "module checks passed"
