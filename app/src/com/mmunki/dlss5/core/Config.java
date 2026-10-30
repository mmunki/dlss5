package com.mmunki.dlss5.core;

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;

/** The config object shared with the dlss5d daemon. */
public final class Config {

    public static final float SCALE_MIN = 0.30f;
    public static final float SCALE_MAX = 0.90f;
    public static final float SCALE_STEP = 0.05f;
    public static final int MAX_GAMES = 64;

    public boolean enabled = true;
    public boolean ai = true;
    public boolean gpuBoost = true;
    public float downscale = 0.70f;
    public int targetFps = 60;
    public int saturation = 100;
    public final List<String> games = new ArrayList<String>();

    public static Config defaults() {
        return new Config();
    }

    public static String snapScaleText(float scale) {
        return String.format(Locale.US, "%.2f", snapScale(scale));
    }

    public static float snapScale(float scale) {
        if (scale >= 0.975f) {
            return 1.0f;
        }
        if (scale < SCALE_MIN) {
            scale = SCALE_MIN;
        }
        if (scale > SCALE_MAX) {
            scale = SCALE_MAX;
        }
        float snapped = SCALE_MIN + Math.round((scale - SCALE_MIN) / SCALE_STEP) * SCALE_STEP;
        if (snapped < SCALE_MIN) {
            snapped = SCALE_MIN;
        }
        if (snapped > SCALE_MAX) {
            snapped = SCALE_MAX;
        }
        return snapped;
    }

    public boolean hasGame(String pkg) {
        return pkg != null && games.contains(pkg);
    }

    public boolean addGame(String pkg) {
        if (!GameFilter.validPackage(pkg) || games.contains(pkg) || games.size() >= MAX_GAMES) {
            return false;
        }
        games.add(pkg);
        return true;
    }

    public boolean removeGame(String pkg) {
        return games.remove(pkg);
    }

    public void clamp() {
        downscale = snapScale(downscale);
        if (targetFps < 0) {
            targetFps = 0;
        }
        if (targetFps > 240) {
            targetFps = 240;
        }
        if (targetFps > 0 && targetFps < 24) {
            targetFps = 24;
        }
        if (saturation < 0) {
            saturation = 0;
        }
        if (saturation > 100) {
            saturation = 100;
        }
        while (games.size() > MAX_GAMES) {
            games.remove(games.size() - 1);
        }
    }

    public String toJson() {
        clamp();
        StringBuilder out = new StringBuilder(256);
        out.append("{\n");
        out.append("  \"version\": 1,\n");
        out.append("  \"enabled\": ").append(enabled).append(",\n");
        out.append("  \"ai\": ").append(ai).append(",\n");
        out.append("  \"gpu_boost\": ").append(gpuBoost).append(",\n");
        out.append("  \"downscale\": ").append(String.format(Locale.US, "%.2f", downscale))
                .append(",\n");
        out.append("  \"target_fps\": ").append(targetFps).append(",\n");
        out.append("  \"saturation\": ").append(saturation).append(",\n");
        out.append("  \"games\": [");
        for (int i = 0; i < games.size(); i++) {
            out.append(i == 0 ? "\n    " : ",\n    ");
            out.append(MiniJson.quote(games.get(i)));
        }
        if (!games.isEmpty()) {
            out.append("\n  ");
        }
        out.append("]\n}\n");
        return out.toString();
    }

    public static Config fromJson(String text) {
        Config config = defaults();
        if (text == null || text.trim().isEmpty()) {
            return config;
        }
        Map<String, Object> root;
        try {
            root = MiniJson.parseObject(text);
        } catch (MiniJson.ParseException e) {
            return config;
        }
        config.enabled = MiniJson.bool(root, "enabled", config.enabled);
        config.ai = MiniJson.bool(root, "ai", config.ai);
        config.gpuBoost = MiniJson.bool(root, "gpu_boost", config.gpuBoost);
        config.downscale = (float) MiniJson.number(root, "downscale", config.downscale);
        config.targetFps = (int) MiniJson.number(root, "target_fps", config.targetFps);
        config.saturation = (int) MiniJson.number(root, "saturation", config.saturation);
        List<Object> entries = MiniJson.list(root, "games");
        if (entries != null) {
            for (Object entry : entries) {
                if (entry instanceof String) {
                    config.addGame((String) entry);
                }
            }
        }
        config.clamp();
        return config;
    }

    public Map<String, Object> toMap() {
        Map<String, Object> map = new LinkedHashMap<String, Object>();
        map.put("enabled", enabled);
        map.put("ai", ai);
        map.put("gpu_boost", gpuBoost);
        map.put("downscale", downscale);
        map.put("target_fps", targetFps);
        map.put("saturation", saturation);
        map.put("games", new ArrayList<Object>(games));
        return map;
    }
}
