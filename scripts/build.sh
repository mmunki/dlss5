#!/usr/bin/env bash
# Builds the DLSS5 daemon (arm64-v8a), the APK and the Magisk module, then
# runs every test that does not need a physical phone.
#
#   ./scripts/build.sh
#
# Output: dist/DLSS5-<version>-arm64.zip, dist/DLSS5-<version>.apk,
#         dist/SHA256SUMS
#
# The arm64 daemon is static, so the very same file can be executed under
# qemu-aarch64 on the build host (the CI does that) and on the phone.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

VERSION="$(sed -n 's/^#define D5_VERSION "\(.*\)"/\1/p' native/dlss5.h)"
VERSION_CODE="$(sed -n 's/^#define D5_VERSION_CODE \([0-9]*\)/\1/p' native/dlss5.h)"
[ -n "$VERSION" ] || { echo "error: cannot read D5_VERSION" >&2; exit 1; }

ANDROID_SDK="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-}}"
[ -n "$ANDROID_SDK" ] && [ -d "$ANDROID_SDK" ] \
    || { echo "error: set ANDROID_HOME to an Android SDK" >&2; exit 1; }

find_ndk() {
    if [ -n "${ANDROID_NDK_HOME:-}" ] && [ -d "$ANDROID_NDK_HOME" ]; then
        printf '%s\n' "$ANDROID_NDK_HOME"
        return
    fi
    if [ -n "${ANDROID_NDK:-}" ] && [ -d "$ANDROID_NDK" ]; then
        printf '%s\n' "$ANDROID_NDK"
        return
    fi
    find "$ANDROID_SDK/ndk" -maxdepth 1 -mindepth 1 -type d 2>/dev/null \
        | sort -V | tail -n1
}

NDK="$(find_ndk)"
[ -n "$NDK" ] && [ -d "$NDK" ] \
    || { echo "error: no NDK found (set ANDROID_NDK_HOME)" >&2; exit 1; }
case "$(uname -s)-$(uname -m)" in
    Linux-x86_64) NDK_HOST=linux-x86_64 ;;
    Linux-aarch64) NDK_HOST=linux-aarch64 ;;
    Darwin-*) NDK_HOST=darwin-x86_64 ;;
    *) echo "error: unsupported build host" >&2; exit 1 ;;
esac
CC_ARMV8="$NDK/toolchains/llvm/prebuilt/$NDK_HOST/bin/aarch64-linux-android26-clang"
[ -x "$CC_ARMV8" ] || { echo "error: missing $CC_ARMV8" >&2; exit 1; }

BUILD_TOOLS="$(find "$ANDROID_SDK/build-tools" -maxdepth 1 -mindepth 1 -type d \
    | sort -V | tail -n1)"
[ -d "$BUILD_TOOLS" ] || { echo "error: no build-tools" >&2; exit 1; }
ANDROID_JAR="$(find "$ANDROID_SDK/platforms" -maxdepth 2 -name android.jar \
    | sort -V | tail -n1)"
[ -f "$ANDROID_JAR" ] || { echo "error: no android.jar" >&2; exit 1; }

echo "==> dlss5 $VERSION ($VERSION_CODE)"
echo "==> SDK   $ANDROID_SDK"
echo "==> NDK   $NDK"
echo "==> tools $BUILD_TOOLS"
echo "==> jar   $ANDROID_JAR"

rm -rf build dist
mkdir -p build/gen build/classes build/dex build/res build/test build/module \
         build/apk dist

# ---------------------------------------------------------------- model
if command -v python3 >/dev/null 2>&1; then
    echo "==> verifying the generated controller weights"
    python3 tools/train_model.py --check native/model.h
else
    echo "==> python3 not found, using the committed model.h"
fi

# -------------------------------------------------- host tests of the core
echo "==> building the host selftest"
HOST_CC="${CC:-cc}"
$HOST_CC -std=c11 -D_GNU_SOURCE -O2 -Wall -Wextra -Wno-unused-parameter \
    -Inative native/*.c -o build/dlss5-host -lm
./build/dlss5-host --selftest

echo "==> running the daemon integration test"
./scripts/integration-test.sh build/dlss5-host

# ------------------------------------------------------ arm64 daemon
echo "==> cross compiling the arm64-v8a daemon"
# Bionic aborts at startup unless PT_TLS is 64-byte aligned and its virtual
# address has no skew.  lld emits p_align = 8 for static links, so the
# program header is patched after linking; the layout flags below are tried
# until the TLS virtual address happens to land on a 64-byte boundary.
# Without a static layout that satisfies bionic, the daemon is linked
# dynamically against bionic instead (also valid, just with runtime deps).
command -v python3 >/dev/null 2>&1 \
    || { echo "error: python3 is required for the TLS alignment fixup" >&2; exit 1; }

tls_aligned() {
    command -v readelf >/dev/null 2>&1 || return 0
    local vaddr align
    vaddr="$(readelf -l "$1" | awk '/ TLS / {print $3; exit}')"
    align="$(readelf -l "$1" | awk '/ TLS / {getline; print $NF; exit}')"
    [ -n "$vaddr" ] && [ $((vaddr % 64)) -eq 0 ] && [ "$align" = "0x40" ]
}

DAEMON_KIND="static"
DAEMON_LINKED=0
for layout in "-Wl,-z,separate-loadable-segments" "-Wl,-z,norelro" \
              "-Wl,--no-rosegment" "-Wl,--no-rosegment -Wl,-z,norelro"; do
    # shellcheck disable=SC2086
    "$CC_ARMV8" -O2 -std=c11 -D_GNU_SOURCE -Wall -Wextra -Wno-unused-parameter \
        -static $layout -Inative native/*.c -o build/dlss5d -lm
    python3 tools/patch-tls.py build/dlss5d >/dev/null
    if tls_aligned build/dlss5d; then
        DAEMON_LINKED=1
        echo "==> static link layout: $layout"
        break
    fi
done

if [ "$DAEMON_LINKED" -ne 1 ]; then
    echo "==> no TLS-aligned static layout, linking dynamically against bionic"
    "$CC_ARMV8" -O2 -std=c11 -D_GNU_SOURCE -Wall -Wextra -Wno-unused-parameter \
        -Inative native/*.c -o build/dlss5d -lm
    DAEMON_KIND="dynamic"
fi

if command -v readelf >/dev/null 2>&1; then
    readelf -h build/dlss5d | grep -q "AArch64" \
        || { echo "error: build/dlss5d is not an AArch64 ELF" >&2; exit 1; }
    echo "==> ELF: $(readelf -h build/dlss5d | sed -n 's/.*Machine: *//p') ($DAEMON_KIND)"
fi

ABI_RESULT="built, selftest skipped (qemu not available)"
if [ -n "${DLSS5_QEMU:-}" ] && [ -x "${DLSS5_QEMU}" ] && [ "$DAEMON_KIND" = "static" ]; then
    echo "==> running the arm64 binary under qemu"
    "${DLSS5_QEMU}" build/dlss5d --selftest
    ABI_RESULT="pass (qemu-aarch64)"
elif [ "$DAEMON_KIND" = "dynamic" ]; then
    ABI_RESULT="built (dynamic bionic, qemu cannot load bionic)"
fi

# ------------------------------------------------------------------ APK
echo "==> compiling resources"
"$BUILD_TOOLS/aapt2" compile --dir app/res -o build/res/resources.zip
"$BUILD_TOOLS/aapt2" link -o build/apk/app-unsigned.apk \
    -I "$ANDROID_JAR" \
    --manifest app/AndroidManifest.xml \
    -R build/res/resources.zip \
    --java build/gen \
    --min-sdk-version 26 \
    --target-sdk-version 36 \
    --version-code "$VERSION_CODE" \
    --version-name "$VERSION" \
    --auto-add-overlay

echo "==> compiling Java"
find app/src build/gen -name '*.java' | sort > build/sources.txt
javac --release 8 -encoding UTF-8 -Xlint:-options \
    -classpath "$ANDROID_JAR" -d build/classes @build/sources.txt

echo "==> dexing"
mapfile -t CLASSES < <(find build/classes -name '*.class' | sort)
"$BUILD_TOOLS/d8" --lib "$ANDROID_JAR" --min-api 26 \
    --output build/dex "${CLASSES[@]}"
(cd build/dex && zip -q -X ../apk/app-unsigned.apk classes.dex)

echo "==> aligning"
"$BUILD_TOOLS/zipalign" -f -p 4 build/apk/app-unsigned.apk build/apk/app-aligned.apk

KEYSTORE="${KEYSTORE_PATH:-$ROOT/keys/dlss5-test.jks}"
if [ ! -f "$KEYSTORE" ]; then
    echo "==> creating the throwaway test signing key"
    mkdir -p "$(dirname "$KEYSTORE")"
    keytool -genkeypair -keystore "$KEYSTORE" -alias dlss5 \
        -keyalg RSA -keysize 2048 -validity 10000 \
        -storepass dlss5test -keypass dlss5test \
        -dname "CN=DLSS5 Test Key, O=mmunki, C=US" >/dev/null 2>&1
fi

echo "==> signing"
"$BUILD_TOOLS/apksigner" sign --ks "$KEYSTORE" --ks-key-alias dlss5 \
    --ks-pass pass:dlss5test --key-pass pass:dlss5test \
    --out "dist/DLSS5-$VERSION.apk" build/apk/app-aligned.apk
"$BUILD_TOOLS/apksigner" verify --verbose "dist/DLSS5-$VERSION.apk" \
    > build/apk/verify.txt
grep -q "Verified using v2 scheme (APK Signature Scheme v2): true" \
    build/apk/verify.txt

"$BUILD_TOOLS/aapt2" dump badging "dist/DLSS5-$VERSION.apk" \
    | grep -E "^(package|launchable-activity|sdkVersion|targetSdkVersion)" \
    > build/apk/badging.txt
cat build/apk/badging.txt

# ------------------------------------------------------------ Java tests
echo "==> running the Java core tests"
javac -encoding UTF-8 -d build/test \
    app/src/com/mmunki/dlss5/core/*.java app/tests/CoreTests.java
java -ea -cp build/test CoreTests

# ---------------------------------------------------------- Magisk module
echo "==> packaging the Magisk module"
MOD=build/module
cp module/module.prop module/customize.sh module/service.sh module/action.sh \
   module/uninstall.sh "$MOD/"
mkdir -p "$MOD/META-INF/com/google/android"
cp module/META-INF/com/google/android/update-binary \
   module/META-INF/com/google/android/updater-script \
   "$MOD/META-INF/com/google/android/"
mkdir -p "$MOD/system/bin"
cp build/dlss5d "$MOD/system/bin/dlss5d"
chmod 0755 "$MOD/system/bin/dlss5d" "$MOD"/*.sh "$MOD/META-INF/com/google/android/update-binary"
cp README.md "$MOD/README.md"
(cd "$MOD" && zip -q -r -X "../../dist/DLSS5-$VERSION-arm64.zip" .)

# --------------------------------------------------------------- checksums
(cd dist && sha256sum ./*.apk ./*.zip | sed 's|\./||' > SHA256SUMS)
cat dist/SHA256SUMS

cat <<EOF

==> test matrix
ABI        build           selftest
arm64-v8a  ok ($DAEMON_KIND)  $ABI_RESULT

==> artifacts
dist/DLSS5-$VERSION.apk
dist/DLSS5-$VERSION-arm64.zip
EOF
