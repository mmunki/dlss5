package com.mmunki.dlss5.core;

/**
 * The on-device layout shared by the daemon, the Magisk module and the app.
 *
 * The daemon is executed from {@link #RUNTIME_BIN}: a plain file in /data/adb
 * that exists and is executable even before the magisk mount exposes
 * /system/bin/dlss5d, and that survives a broken module permission pass.  The
 * two other candidates are fallbacks for a module flashed by hand.
 */
public final class Paths {

    public static final String DIR = "/data/adb/dlss5";
    public static final String CONFIG = DIR + "/config.json";
    public static final String STATUS = DIR + "/status.json";
    public static final String LOG = DIR + "/dlss5.log";
    public static final String STOP = DIR + "/stop";

    public static final String RUNTIME_BIN = DIR + "/bin/dlss5d";

    public static final String MODULE_DIR = "/data/adb/modules/dlss5";
    public static final String MODULE_PROP = MODULE_DIR + "/module.prop";
    public static final String MODULE_BIN = MODULE_DIR + "/system/bin/dlss5d";
    public static final String MOUNTED_BIN = "/system/bin/dlss5d";

    public static final String[] DAEMON_CANDIDATES = {
            RUNTIME_BIN, MOUNTED_BIN, MODULE_BIN
    };

    /** Space separated candidate list for the shell. */
    public static String candidateList() {
        StringBuilder out = new StringBuilder();
        for (String candidate : DAEMON_CANDIDATES) {
            if (out.length() > 0) {
                out.append(' ');
            }
            out.append(candidate);
        }
        return out.toString();
    }

    private Paths() {
    }
}
