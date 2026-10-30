package com.mmunki.dlss5.core;

/**
 * The shell script that installs the module bundled in {@code assets/module.zip}.
 *
 * Pure Java, no Android class, so the ordering is covered by the host tests.
 * The order is the whole point: the recursive {@code chmod 0644} runs first and
 * the per-file {@code chmod 0755} runs after it.  Written the other way around
 * the daemon ends up non-executable, which is exactly the bug that made the
 * device shell answer "Permission denied" while every existence check was true.
 */
public final class InstallScript {

    public static final String INSTALLED = "MODULE-INSTALLED";

    public static String build(String source) {
        return "set -e; "
                + "SRC='" + source + "'; "
                + "DST='" + Paths.MODULE_DIR + "'; "
                + "DATA='" + Paths.DIR + "'; "
                + "mkdir -p \"$DST\" \"$DATA/bin\"; "
                + "cp -rf \"$SRC\"/. \"$DST\"/; "
                + "rm -f \"$DST/update\" \"$DST/disable\" \"$DST/remove\" "
                + "\"$DST/customize.sh\"; "
                // 1. read-only payload
                + "find \"$DST\" -type d -exec chmod 0755 {} +; "
                + "find \"$DST\" -type f -exec chmod 0644 {} +; "
                // 2. executables, always after the recursive pass
                + "chmod 0755 \"$DST\"/service.sh \"$DST\"/action.sh "
                + "\"$DST\"/uninstall.sh \"$DST\"/system/bin/dlss5d; "
                + "chcon -R u:object_r:system_file:s0 \"$DST\" 2>/dev/null || true; "
                // 3. the copy the app and the watchdog execute (rename, because
                //    a running binary cannot be truncated)
                + "cp -f \"$DST\"/system/bin/dlss5d \"$DATA/bin/dlss5d.new\"; "
                + "chmod 0755 \"$DATA/bin/dlss5d.new\"; "
                + "chown 0:0 \"$DATA/bin/dlss5d.new\" 2>/dev/null || true; "
                + "mv -f \"$DATA/bin/dlss5d.new\" \"$DATA/bin/dlss5d\"; "
                + "chcon u:object_r:magisk_file:s0 \"$DATA/bin/dlss5d\" 2>/dev/null || true; "
                + "chmod 700 \"$DATA\"; "
                + "rm -f \"" + Paths.STOP + "\"; "
                // 4. prove it instead of trusting it
                + "[ -f \"" + Paths.MODULE_PROP + "\" ] "
                + "|| { echo 'install failed: module.prop missing'; exit 1; }; "
                + "[ -x \"" + Paths.RUNTIME_BIN + "\" ] "
                + "|| { echo 'install failed: daemon not executable'; exit 1; }; "
                + "\"" + Paths.RUNTIME_BIN + "\" --version "
                + "|| { echo 'install failed: daemon cannot run'; exit 1; }; "
                + "echo " + INSTALLED;
    }

    private InstallScript() {
    }
}
