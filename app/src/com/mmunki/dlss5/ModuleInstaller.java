package com.mmunki.dlss5;

import android.content.Context;

import com.mmunki.dlss5.core.InstallScript;

import java.io.BufferedInputStream;
import java.io.BufferedOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/**
 * Installs the Magisk module that ships <em>inside</em> the APK.
 *
 * The module used to be a separate zip download that had to be flashed in the
 * Magisk app.  {@code assets/module.zip} is that same archive, laid down by
 * the build.  Installing it needs nothing but root:
 *
 * <ol>
 *     <li>extract the payload into the app cache (the on-device shell cannot be
 *     assumed to have unzip),</li>
 *     <li>copy it to /data/adb/modules/dlss5 with the mode order the module
 *     installer uses - 0644 everywhere first, then 0755 for the scripts and
 *     the daemon,</li>
 *     <li>refresh the executable runtime copy in /data/adb/dlss5/bin, which
 *     works even before the reboot exposes /system/bin/dlss5d,</li>
 *     <li>start the daemon and report whether a reboot is still needed.</li>
 * </ol>
 */
public final class ModuleInstaller {

    public static final String ASSET = "module.zip";
    public static final String INSTALLED = InstallScript.INSTALLED;

    public static final class Result {
        public final boolean ok;
        public final String log;

        Result(boolean ok, String log) {
            this.ok = ok;
            this.log = log == null ? "" : log;
        }
    }

    private ModuleInstaller() {
    }

    /** Extracts and installs the bundled module, then starts the daemon. */
    public static Result install(Context context) {
        File source;
        try {
            source = extract(context);
        } catch (IOException e) {
            return new Result(false, "cannot read assets/" + ASSET + ": "
                    + e.getMessage());
        }
        RootShell.Result result = RootShell.run(
                InstallScript.build(source.getAbsolutePath()), 240);
        boolean ok = result.ok() && result.output.contains(INSTALLED);
        String log = result.output;
        if (ok) {
            String started = ConfigStore.restartDaemon();
            if (started != null && !started.isEmpty()) {
                log = log + "\n" + started;
            }
        }
        return new Result(ok, log);
    }

    /** Kept for the tests: the install script is built by the pure core. */
    static String installScript(String source) {
        return InstallScript.build(source);
    }

    /**
     * Writes assets/module.zip into the app cache as a directory tree.  The
     * installer-only entries (META-INF, customize.sh) are dropped, exactly
     * like Magisk does after a successful flash.
     */
    static File extract(Context context) throws IOException {
        File dir = new File(context.getCacheDir(), "module");
        delete(dir);
        if (!dir.mkdirs() && !dir.isDirectory()) {
            throw new IOException("cannot create " + dir);
        }
        String root = dir.getCanonicalPath();
        InputStream raw = context.getAssets().open(ASSET);
        ZipInputStream zip = new ZipInputStream(new BufferedInputStream(raw));
        try {
            ZipEntry entry;
            while ((entry = zip.getNextEntry()) != null) {
                String name = entry.getName();
                if (name.startsWith("META-INF/") || name.equals("customize.sh")) {
                    continue;
                }
                File out = new File(dir, name);
                String path = out.getCanonicalPath();
                if (!path.equals(root) && !path.startsWith(root + File.separator)) {
                    throw new IOException("unsafe entry " + name);
                }
                if (entry.isDirectory()) {
                    if (!out.isDirectory() && !out.mkdirs()) {
                        throw new IOException("cannot create " + out);
                    }
                    continue;
                }
                File parent = out.getParentFile();
                if (parent != null && !parent.isDirectory() && !parent.mkdirs()) {
                    throw new IOException("cannot create " + parent);
                }
                OutputStream stream = new BufferedOutputStream(
                        new FileOutputStream(out));
                try {
                    byte[] buffer = new byte[8192];
                    int read;
                    while ((read = zip.read(buffer)) > 0) {
                        stream.write(buffer, 0, read);
                    }
                } finally {
                    stream.close();
                }
                zip.closeEntry();
            }
        } finally {
            zip.close();
        }
        return dir;
    }

    private static void delete(File file) {
        if (file == null || !file.exists()) {
            return;
        }
        File[] children = file.listFiles();
        if (children != null) {
            for (File child : children) {
                delete(child);
            }
        }
        file.delete();
    }
}
