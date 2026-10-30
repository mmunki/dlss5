package com.mmunki.dlss5.core;

import java.util.Map;

/** The status file the daemon writes every couple of seconds. */
public final class Status {

    public boolean running;
    public boolean enabled;
    public boolean active;
    public String pkg = "";
    public float downscale = 1.0f;
    public int fps;
    public int saturation = 100;
    public float scaleHint;
    public float fpsHint;

    public static Status fromJson(String text) {
        Status status = new Status();
        if (text == null || text.trim().isEmpty()) {
            return status;
        }
        Map<String, Object> root;
        try {
            root = MiniJson.parseObject(text);
        } catch (MiniJson.ParseException e) {
            return status;
        }
        status.running = MiniJson.bool(root, "running", false);
        status.enabled = MiniJson.bool(root, "enabled", false);
        status.active = MiniJson.bool(root, "active", false);
        status.pkg = MiniJson.string(root, "package", "");
        status.downscale = (float) MiniJson.number(root, "downscale", 1.0);
        status.fps = (int) MiniJson.number(root, "fps", 0);
        status.saturation = (int) MiniJson.number(root, "saturation", 100);
        status.scaleHint = (float) MiniJson.number(root, "scale_hint", 0.0);
        status.fpsHint = (float) MiniJson.number(root, "fps_hint", 0.0);
        return status;
    }

    public String describe() {
        if (!running) {
            return "daemon not running";
        }
        if (!enabled) {
            return "paused (tile off)";
        }
        if (active) {
            return String.format(java.util.Locale.US,
                    "boosting %s - scale %.0f%%, %d fps, colour %d",
                    pkg, downscale * 100.0f, fps, saturation);
        }
        return "waiting for a selected game";
    }
}
