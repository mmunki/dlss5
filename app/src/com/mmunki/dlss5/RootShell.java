package com.mmunki.dlss5;

import java.io.IOException;
import java.io.InputStream;
import java.util.concurrent.TimeUnit;

/** Runs a command through {@code su -c} and captures its output. */
public final class RootShell {

    public static final class Result {
        public final int code;
        public final String output;

        Result(int code, String output) {
            this.code = code;
            this.output = output == null ? "" : output;
        }

        public boolean ok() {
            return code == 0;
        }
    }

    /** First launch shows the su prompt, so the default is generous. */
    private static final int DEFAULT_TIMEOUT = 60;

    private RootShell() {
    }

    public static Result run(String command) {
        return run(command, DEFAULT_TIMEOUT);
    }

    public static Result run(String command, int timeoutSeconds) {
        Process process = null;
        try {
            ProcessBuilder builder = new ProcessBuilder("su", "-c", command);
            builder.redirectErrorStream(true);
            process = builder.start();
            final InputStream stream = process.getInputStream();
            final StringBuilder output = new StringBuilder();
            Thread reader = new Thread(new Runnable() {
                @Override
                public void run() {
                    byte[] buffer = new byte[4096];
                    try {
                        int read;
                        while ((read = stream.read(buffer)) > 0) {
                            output.append(new String(buffer, 0, read));
                        }
                    } catch (IOException ignored) {
                        // the process died, the exit code tells the story
                    }
                }
            });
            reader.setDaemon(true);
            reader.start();
            if (!process.waitFor(timeoutSeconds, TimeUnit.SECONDS)) {
                process.destroy();
                return new Result(-1, "timeout");
            }
            reader.join(2000);
            return new Result(process.exitValue(), output.toString().trim());
        } catch (IOException e) {
            return new Result(-1, "su unavailable: " + e.getMessage());
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            return new Result(-1, "interrupted");
        } finally {
            if (process != null) {
                process.destroy();
            }
        }
    }

    public static boolean available() {
        Result result = run("id");
        return result.ok() && result.output.contains("uid=0");
    }
}
