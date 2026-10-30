#include "dlss5.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void d5_config_defaults(D5Config *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->enabled = 1;
    cfg->ai = 1;
    cfg->gpu_boost = 1;
    cfg->cpu_boost = 1;
    cfg->msaa = 4;
    cfg->refresh_lock = 1;
    cfg->refresh_rate = 90;
    cfg->ui_density_delta = 0;
    cfg->hud = 1;
    /* 2.0 defaults to quality first: the AI controller still protects the
     * thermals, but a cool phone plays at (near) native resolution. */
    cfg->downscale = 0.90f;
    cfg->target_fps = 90;
    cfg->saturation = 100;
    cfg->game_count = 0;
}

int d5_snap_msaa(int value) {
    if (value <= 2) return 0;
    if (value <= 6) return 4;
    return 8;
}

int d5_config_has_game(const D5Config *cfg, const char *pkg) {
    if (!cfg || !pkg) return 0;
    for (int i = 0; i < cfg->game_count; i++) {
        if (strcmp(cfg->games[i], pkg) == 0) return 1;
    }
    return 0;
}

static void config_add_game(D5Config *cfg, const char *pkg) {
    if (cfg->game_count >= D5_MAX_GAMES) return;
    if (!d5_valid_package(pkg)) return;
    if (d5_config_has_game(cfg, pkg)) return;
    snprintf(cfg->games[cfg->game_count], D5_PKG_LEN, "%s", pkg);
    cfg->game_count++;
}

int d5_config_load(const char *path, D5Config *cfg) {
    char *text = NULL;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0 || size > 512 * 1024) {
        fclose(f);
        return 0;
    }
    text = calloc((size_t)size + 1, 1);
    if (!text) {
        fclose(f);
        return 0;
    }
    size_t got = fread(text, 1, (size_t)size, f);
    fclose(f);
    text[got] = '\0';

    char err[128];
    D5Json *root = d5_json_parse(text, err, sizeof(err));
    free(text);
    if (!root) {
        d5_log("config: cannot parse %s: %s", path, err);
        return 0;
    }
    if (d5_json_type(root) != D5_JSON_OBJECT) {
        d5_json_free(root);
        d5_log("config: %s is not an object", path);
        return 0;
    }

    D5Config next;
    d5_config_defaults(&next);

    next.enabled = d5_json_bool(d5_json_get(root, "enabled"), next.enabled);
    next.ai = d5_json_bool(d5_json_get(root, "ai"), next.ai);
    next.gpu_boost = d5_json_bool(d5_json_get(root, "gpu_boost"), next.gpu_boost);

    float scale = (float)d5_json_num(d5_json_get(root, "downscale"), next.downscale);
    if (scale < D5_SCALE_MIN) scale = D5_SCALE_MIN;
    if (scale > 1.0f) scale = 1.0f;
    next.downscale = d5_snap_scale(scale);

    int fps = (int)d5_json_num(d5_json_get(root, "target_fps"), next.target_fps);
    if (fps < 0) fps = 0;
    if (fps > 240) fps = 240;
    if (fps > 0 && fps < 24) fps = 24;
    next.target_fps = fps;

    int sat = (int)d5_json_num(d5_json_get(root, "saturation"), next.saturation);
    if (sat < 0) sat = 0;
    if (sat > 100) sat = 100;
    next.saturation = sat;

    next.msaa = d5_snap_msaa(
        (int)d5_json_num(d5_json_get(root, "msaa"), next.msaa));
    next.cpu_boost = d5_json_bool(d5_json_get(root, "cpu_boost"), next.cpu_boost);

    int refresh = (int)d5_json_num(d5_json_get(root, "refresh_rate"), next.refresh_rate);
    if (refresh < 0) refresh = 0;
    if (refresh > 240) refresh = 240;
    if (refresh > 0 && refresh < 24) refresh = 24;
    next.refresh_rate = refresh;
    next.refresh_lock = d5_json_bool(d5_json_get(root, "refresh_lock"), next.refresh_lock);

    int ui_delta = (int)d5_json_num(d5_json_get(root, "ui_density_delta"), next.ui_density_delta);
    if (ui_delta < -200) ui_delta = -200;
    if (ui_delta > 200) ui_delta = 200;
    next.ui_density_delta = ui_delta;

    next.hud = d5_json_bool(d5_json_get(root, "hud"), next.hud);

    const D5Json *games = d5_json_get(root, "games");
    if (games && d5_json_type(games) == D5_JSON_ARRAY) {
        for (size_t i = 0; i < d5_json_len(games); i++) {
            const char *pkg = d5_json_str(d5_json_at(games, i), NULL);
            if (pkg) config_add_game(&next, pkg);
        }
    }
    d5_json_free(root);

    *cfg = next;
    return 1;
}

int d5_config_save(const char *path, const D5Config *cfg) {
    char buf[8192];
    size_t used = 0;
    int n = snprintf(buf + used, sizeof(buf) - used,
                     "{\n"
                     "  \"version\": 2,\n"
                     "  \"enabled\": %s,\n"
                     "  \"ai\": %s,\n"
                     "  \"gpu_boost\": %s,\n"
                     "  \"cpu_boost\": %s,\n"
                     "  \"msaa\": %d,\n"
                     "  \"refresh_lock\": %s,\n"
                     "  \"refresh_rate\": %d,\n"
                     "  \"ui_density_delta\": %d,\n"
                     "  \"hud\": %s,\n"
                     "  \"downscale\": %.2f,\n"
                     "  \"target_fps\": %d,\n"
                     "  \"saturation\": %d,\n"
                     "  \"games\": [",
                     cfg->enabled ? "true" : "false",
                     cfg->ai ? "true" : "false",
                     cfg->gpu_boost ? "true" : "false",
                     cfg->cpu_boost ? "true" : "false",
                     cfg->msaa,
                     cfg->refresh_lock ? "true" : "false",
                     cfg->refresh_rate,
                     cfg->ui_density_delta,
                     cfg->hud ? "true" : "false",
                     (double)cfg->downscale, cfg->target_fps, cfg->saturation);
    if (n < 0) return 0;
    used += (size_t)n;
    int written = 0;
    for (int i = 0; i < cfg->game_count && used + 160 < sizeof(buf); i++) {
        if (!d5_valid_package(cfg->games[i])) continue;
        n = snprintf(buf + used, sizeof(buf) - used, "%s\n    \"%s\"",
                     written ? "," : "\n   ", cfg->games[i]);
        if (n < 0) return 0;
        used += (size_t)n;
        written++;
    }
    if (written > 0) {
        n = snprintf(buf + used, sizeof(buf) - used, "\n  ");
        if (n < 0) return 0;
        used += (size_t)n;
    }
    n = snprintf(buf + used, sizeof(buf) - used, "]\n}\n");
    if (n < 0) return 0;
    return d5_write_file(path, buf);
}
