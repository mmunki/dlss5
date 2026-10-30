package com.mmunki.dlss5;

import android.content.Context;

import com.mmunki.dlss5.core.Config;
import com.mmunki.dlss5.core.Paths;
import com.mmunki.dlss5.core.Status;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.OutputStreamWriter;
import java.io.Writer;

/** Reads and writes /data/adb/dlss5 through root. */
public final class ConfigStore {

    public static final String DIR = Paths.DIR;
    public static final String CONFIG = Paths.CONFIG;
    public static final String STATUS = Paths.STATUS;
    public static final String LOG = Paths.LOG;
    public static final String MODULE_DIR = Paths.MODULE_DIR;
    public static final String MODULE_PROP = Paths.MODULE_PROP;

    /** The daemon copy in /data/adb is authoritative: it is a normal file, it
     * is always executable and it exists without the magisk mount.  The
     * mounted paths are fallbacks for an install made by the module zip. */
    public static final String RUNTIME_BIN = Paths.RUNTIME_BIN;

    private static final String DAEMON_CANDIDATES = Paths.candidateList();

    /**
     * Finds the first candidate that exists and can be executed, repairing the
     * execute bit on the way.  Prints {@code BIN=<path>} (empty when none).
     * This is the fix for "Unable to find the file even the file does exists":
     * the old check was {@code [ -x ... ]} only, so a 0644 daemon looked
     * missing.
     */
    private static final String FIND_BIN =
            "BIN=; for c in " + DAEMON_CANDIDATES + "; do "
                    + "[ -f \"$c\" ] && chmod 0755 \"$c\" 2>/dev/null; "
                    + "if [ -x \"$c\" ]; then BIN=\"$c\"; break; fi; done; "
                    + "echo \"BIN=$BIN\"";

    private ConfigStore() {
    }

    public static Config load() {
        RootShell.Result result = RootShell.run("cat " + CONFIG + " 2>/dev/null");
        return Config.fromJson(result.output);
    }

    public static boolean save(Context context, Config config) {
        File temp = new File(context.getCacheDir(), "config.json");
        Writer writer = null;
        try {
            writer = new OutputStreamWriter(new FileOutputStream(temp), "UTF-8");
            writer.write(config.toJson());
            writer.flush();
        } catch (IOException e) {
            return false;
        } finally {
            if (writer != null) {
                try {
                    writer.close();
                } catch (IOException ignored) {
                    // nothing useful to do
                }
            }
        }
        String path = temp.getAbsolutePath();
        RootShell.Result result = RootShell.run(
                "mkdir -p " + DIR + " && cp " + path + " " + CONFIG
                        + " && chmod 600 " + CONFIG + " && chown root:root " + CONFIG);
        // The daemon watches the mtime of the config, but a HUP makes the
        // change apply in the same second the button was pressed.
        RootShell.run("kill -HUP $(pidof dlss5d) 2>/dev/null");
        return result.ok();
    }

    public static boolean setEnabled(Context context, boolean enabled) {
        Config config = load();
        config.enabled = enabled;
        return save(context, config);
    }

    public static Status status() {
        RootShell.Result result = RootShell.run("cat " + STATUS + " 2>/dev/null");
        return Status.fromJson(result.output);
    }

    public static boolean daemonRunning() {
        RootShell.Result result = RootShell.run("pidof dlss5d");
        return result.ok() && !result.output.trim().isEmpty();
    }

    public static boolean moduleInstalled() {
        return RootShell.run("test -f " + MODULE_PROP).ok();
    }

    /** The path of the first executable daemon binary, or "" when none works. */
    public static String daemonPath() {
        RootShell.Result result = RootShell.run(FIND_BIN);
        for (String line : result.output.split("\n")) {
            line = line.trim();
            if (line.startsWith("BIN=")) {
                return line.substring(4).trim();
            }
        }
        return "";
    }

    /** Module present and daemon executable: everything the app needs. */
    public static boolean moduleReady() {
        return moduleInstalled() && !daemonPath().isEmpty();
    }

    /** Starts the daemon from whichever binary works and reports what happened. */
    public static String restartDaemon() {
        RootShell.Result result = RootShell.run(
                "for pid in $(pidof dlss5d 2>/dev/null); do kill \"$pid\" 2>/dev/null; done; "
                        + "sleep 0.4; "
                        + FIND_BIN + "; "
                        + "if [ -z \"$BIN\" ]; then "
                        + "echo \"no executable dlss5d in: " + DAEMON_CANDIDATES + "\"; "
                        + "echo \"open the app and tap 'Install / repair the DLSS5 module'\"; "
                        + "exit 1; fi; "
                        + "mkdir -p " + DIR + "/bin && chmod 700 " + DIR + "; "
                        + "\"$BIN\" --version >/dev/null 2>&1 "
                        + "|| { echo \"$BIN exists but cannot run\"; exit 1; }; "
                        + "rm -f " + Paths.STOP + "; "
                        + "nohup \"$BIN\" --daemon >>" + LOG + " 2>&1 & "
                        + "sleep 0.8; "
                        + "if pidof dlss5d >/dev/null 2>&1; then "
                        + "echo \"daemon started: $BIN\"; "
                        + "else echo \"daemon did not start, see " + LOG + "\"; fi");
        return result.output;
    }
}
