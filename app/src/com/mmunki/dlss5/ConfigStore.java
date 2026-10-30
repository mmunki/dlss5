package com.mmunki.dlss5;

import android.content.Context;

import com.mmunki.dlss5.core.Config;
import com.mmunki.dlss5.core.Status;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.OutputStreamWriter;
import java.io.Writer;

/** Reads and writes /data/adb/dlss5 through root. */
public final class ConfigStore {

    public static final String DIR = "/data/adb/dlss5";
    public static final String CONFIG = DIR + "/config.json";
    public static final String STATUS = DIR + "/status.json";
    public static final String LOG = DIR + "/dlss5.log";

    private static final String DAEMON_CANDIDATES =
            "/system/bin/dlss5d /data/adb/modules/dlss5/system/bin/dlss5d";

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
        return RootShell.run("test -f /data/adb/modules/dlss5/module.prop").ok();
    }

    public static String restartDaemon() {
        RootShell.Result result = RootShell.run(
                "pkill -f dlss5d 2>/dev/null; sleep 0.3; "
                        + "BIN=; for c in " + DAEMON_CANDIDATES + "; do "
                        + "[ -x \"$c\" ] && BIN=\"$c\" && break; done; "
                        + "[ -n \"$BIN\" ] || { echo missing-binary; exit 1; }; "
                        + "mkdir -p " + DIR + " && chmod 700 " + DIR + "; "
                        + "nohup \"$BIN\" --daemon >>" + LOG + " 2>&1 & "
                        + "sleep 0.4; pidof dlss5d >/dev/null && echo started");
        return result.output;
    }
}
