#include "dlss5.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int output_is_error(const char *out) {
    if (!out) return 0;
    return strstr(out, "Error") != NULL || strstr(out, "not of game type") != NULL ||
           strstr(out, "not supported") != NULL || strstr(out, "not found") != NULL ||
           strstr(out, "Exception") != NULL;
}

/* ------------------------------------------------------------ helpers */

static void trim(char *text) {
    size_t len = strlen(text);
    while (len > 0 && (text[len - 1] == '\n' || text[len - 1] == '\r' ||
                       text[len - 1] == ' ' || text[len - 1] == '\t')) {
        text[--len] = '\0';
    }
}

/* Accepts only a short number ("60", "60.0"): anything else that came back
 * from a shell is never echoed into another command. */
static int safe_number(const char *text, char *out, size_t len) {
    if (!text || !*text || strlen(text) > 8) return 0;
    for (const char *p = text; *p; p++) {
        if ((*p < '0' || *p > '9') && *p != '.') return 0;
    }
    snprintf(out, len, "%s", text);
    return 1;
}

/* ------------------------------------------------------------ sysprops */

#define D5_PROP_MSAA "debug.egl.msaa"
#define D5_PROP_FORCE "debug.egl.force_msaa"

static void prop_probe(const char *name, char *out, size_t len) {
    char cmd[128];
    char buf[256];
    out[0] = '\0';
    snprintf(cmd, sizeof(cmd), "getprop %s 2>/dev/null", name);
    if (d5_cmd(cmd, buf, sizeof(buf)) != 0) return;
    trim(buf);
    if (*buf) snprintf(out, len, "%.*s", (int)len - 1, buf);
}

static int prop_put(const char *name, const char *value) {
    char checked[16];
    if (!safe_number(value, checked, sizeof(checked))) return 0;
    char cmd[192];
    snprintf(cmd, sizeof(cmd), "setprop %s %s", name, checked);
    return d5_cmd(cmd, NULL, 0) == 0;
}

/* A prop that did not exist before is deleted (resetprop when available,
 * plain setprop to the empty value as the fallback; the empty value behaves
 * like an unset prop). */
static void prop_restore(const char *name, const char *orig) {
    char cmd[256];
    char checked[16];
    if (orig && *orig && safe_number(orig, checked, sizeof(checked))) {
        snprintf(cmd, sizeof(cmd), "setprop %s %s", name, checked);
        if (d5_cmd(cmd, NULL, 0) != 0) d5_log("cannot restore prop %s", name);
        return;
    }
    snprintf(cmd, sizeof(cmd), "resetprop -d %s 2>/dev/null", name);
    if (d5_cmd(cmd, NULL, 0) != 0) {
        snprintf(cmd, sizeof(cmd), "setprop %s %s 2>/dev/null", name, "\"\"");
        if (d5_cmd(cmd, NULL, 0) != 0) d5_log("cannot clear prop %s", name);
    }
}

/* --------------------------------------------------- settings put/get */

static void setting_probe(const char *nspace, const char *key, char *out,
                          size_t len) {
    char cmd[160];
    char buf[256];
    out[0] = '\0';
    snprintf(cmd, sizeof(cmd), "settings get %s %s 2>/dev/null", nspace, key);
    if (d5_cmd(cmd, buf, sizeof(buf)) != 0) return;
    trim(buf);
    if (*buf && strcmp(buf, "null") != 0) {
        snprintf(out, len, "%.*s", (int)len - 1, buf);
    }
    /* Trailing ".0" from float semantics is fine, the safe-number check
     * happens before any value is echoed back into a command. */
}

static int setting_put(const char *nspace, const char *key, const char *value) {
    char checked[16];
    if (!safe_number(value, checked, sizeof(checked))) return 0;
    char cmd[200];
    snprintf(cmd, sizeof(cmd), "settings put %s %s %s 2>/dev/null", nspace, key,
             checked);
    int rc = d5_cmd(cmd, NULL, 0);
    return rc == 0;
}

static void setting_restore(const char *nspace, const char *key,
                            const char *orig) {
    char cmd[200];
    if (orig && *orig) {
        snprintf(cmd, sizeof(cmd), "settings put %s %s %s 2>/dev/null", nspace,
                 key, orig);
        if (d5_cmd(cmd, NULL, 0) != 0) d5_log("cannot restore setting %s", key);
        return;
    }
    snprintf(cmd, sizeof(cmd), "settings delete %s %s 2>/dev/null", nspace, key);
    d5_cmd(cmd, NULL, 0);
}

/* -------------------------------------------------------- wm density */

/* Reads the current "wm density" report: the physical base density and,
 * when present, the existing override. */
static int density_probe(char *base, size_t base_len, char *override,
                         size_t override_len) {
    char buf[512];
    base[0] = '\0';
    override[0] = '\0';
    if (d5_cmd("wm density 2>/dev/null", buf, sizeof(buf)) != 0) return 0;
    const char *hit = strstr(buf, "Override density:");
    if (hit) {
        char token[32];
        if (sscanf(hit, "Override density: %15s", token) == 1) {
            safe_number(token, override, override_len);
        }
    }
    hit = strstr(buf, "Physical density:");
    if (hit) {
        char token[32];
        if (sscanf(hit, "Physical density: %15s", token) == 1) {
            safe_number(token, base, base_len);
        }
    }
    if (override[0] && !base[0]) snprintf(base, base_len, "%s", override);
    return base[0] != '\0';
}

static int density_apply(D5Applied *a, const D5Config *cfg) {
    char base[16];
    char have[16];
    if (!a->density_set) {
        if (!density_probe(base, sizeof(base), have, sizeof(have))) {
            d5_log("density stage: wm density unreadable, skipped");
            a->density_set = 0;
            return 0;
        }
        snprintf(a->density_base, sizeof(a->density_base), "%s", base);
        snprintf(a->density_orig, sizeof(a->density_orig), "%s", have);
        a->density_set = 1;
    }
    /* The shift is relative to what the game sees now: an existing
     * override when there is one, otherwise the physical panel density. */
    float current = atof(a->density_orig[0] ? a->density_orig : a->density_base);
    if (current < 120.0f || current > 2000.0f) {
        d5_log("density stage: implausible base %s%s, skipped", a->density_base,
               a->density_orig[0] ? a->density_orig : "");
        return 0;
    }
    float target = current + (float)cfg->ui_density_delta;
    /* Stay inside what Android itself allows for a display. */
    if (target < 200.0f) target = 200.0f;
    if (target > 960.0f) target = 960.0f;
    a->density = (int)(target + 0.5f);
    char cmd[96];
    snprintf(cmd, sizeof(cmd), "wm density %d 2>/dev/null", a->density);
    if (d5_cmd(cmd, NULL, 0) != 0) {
        d5_log("density stage: cannot write %d", a->density);
        return 0;
    }
    d5_log("game ui: density %s -> %d (delta %d)", a->density_base, a->density,
           cfg->ui_density_delta);
    return 1;
}

static void density_restore(D5Applied *a) {
    if (!a->density_set) return;
    char cmd[96];
    if (a->density_orig[0]) {
        snprintf(cmd, sizeof(cmd), "wm density %s 2>/dev/null", a->density_orig);
    } else {
        snprintf(cmd, sizeof(cmd), "wm density reset 2>/dev/null");
    }
    if (d5_cmd(cmd, NULL, 0) != 0) {
        d5_log("density stage: cannot restore (was %s)", a->density_orig);
    }
    a->density_set = 0;
}

/* ------------------------------------------------------- render scale */

static int apply_game_manager(const D5Decision *dec, const char *pkg, int *mode) {
    char cmd[512];
    char out[1024];
    *mode = 0;
    if (dec->downscale >= 0.975f && dec->fps <= 0) return 0;

    if (!d5_build_game_set(cmd, sizeof(cmd), dec->downscale, dec->fps, pkg)) return 0;
    int rc = d5_cmd(cmd, out, sizeof(out));
    if (rc == 0 && !output_is_error(out)) {
        *mode = 1;
        return 1;
    }
    d5_log("game set rejected for %s: %s", pkg, out[0] ? out : "(no output)");

    /* Older builds only know "cmd game mode"; it still pins the performance
     * profile even when the render scale cannot be overridden. */
    snprintf(cmd, sizeof(cmd), "cmd game mode performance %s", pkg);
    rc = d5_cmd(cmd, out, sizeof(out));
    if (rc == 0 && !output_is_error(out)) {
        *mode = 2;
        return 1;
    }
    return 0;
}

static int apply_color(const D5Config *cfg, const char *pkg) {
    if (cfg->saturation >= 100) return 0;
    if (cfg->saturation < 0) return 0;
    char cmd[512];
    char out[512];
    snprintf(cmd, sizeof(cmd),
             "cmd color_display set-layer-saturation %d %s %s",
             cfg->saturation, D5_APP_PACKAGE, pkg);
    int rc = d5_cmd(cmd, out, sizeof(out));
    if (rc != 0 || output_is_error(out)) {
        d5_log("colour stage unavailable: %s", out[0] ? out : "(no output)");
        return 0;
    }
    return 1;
}

/* ------------------------------------------------------------ GPU/CPU */

static int apply_gpu_boost(D5Applied *applied) {
    if (applied->governor_set && applied->gov_path[0]) return 1;
    char paths[8][D5_PATH_LEN];
    int count = d5_gpu_governors(paths, 8);
    for (int i = 0; i < count; i++) {
        char original[64];
        if (!d5_read_file(paths[i], original, sizeof(original))) continue;
        if (!*original) continue;
        if (strcmp(original, "performance") == 0) return 0; /* already boosted */
        if (!d5_write_file(paths[i], "performance")) {
            d5_log("cannot write %s", paths[i]);
            continue;
        }
        snprintf(applied->gov_path, sizeof(applied->gov_path), "%.*s",
                 (int)sizeof(applied->gov_path) - 1, paths[i]);
        snprintf(applied->gov_orig, sizeof(applied->gov_orig), "%s", original);
        applied->governor_set = 1;
        d5_log("gpu governor %s: %s -> performance", paths[i], original);
        return 1;
    }
    return 0;
}

static int cpu_slot_find(D5Applied *a, const char *real_path) {
    for (int i = 0; i < a->cpu_count; i++) {
        if (strcmp(a->cpu[i].path, real_path) == 0) return i;
    }
    return -1;
}

static void cpu_restore(D5Applied *a) {
    for (int i = 0; i < a->cpu_count; i++) {
        if (!a->cpu[i].orig[0]) continue;
        if (!d5_write_file(a->cpu[i].path, a->cpu[i].orig)) {
            d5_log("cannot restore %s to %s", a->cpu[i].path, a->cpu[i].orig);
        } else {
            d5_log("cpu governor %s restored to %s", a->cpu[i].path,
                   a->cpu[i].orig);
        }
    }
    a->cpu_set = 0;
    a->cpu_count = 0;
}

static int apply_cpu_boost(D5Applied *applied) {
    D5CpuSlot discovered[16];
    int count = d5_cpu_governors(discovered, 16);
    if (count <= 0) return applied->cpu_set > 0 ? 1 : 0;
    int changed = 0;
    for (int i = 0; i < count; i++) {
        /* The same cluster can appear twice (policyN and cpuN); only the
         * first name wins, the physical file is identical. */
        int slot = cpu_slot_find(applied, discovered[i].path);
        if (slot < 0) {
            if (applied->cpu_count >= 8) break;
            char original[64];
            if (!d5_read_file(discovered[i].path, original, sizeof(original))) continue;
            if (!*original || strcmp(original, "performance") == 0) {
                /* A cluster that is already running "performance" is left
                 * alone: there is nothing to record and nothing to restore. */
                continue;
            }
            if (!d5_write_file(discovered[i].path, "performance")) {
                d5_log("cannot write %s", discovered[i].path);
                continue;
            }
            slot = applied->cpu_count++;
            snprintf(applied->cpu[slot].path, D5_PATH_LEN, "%.*s",
                     D5_PATH_LEN - 1, discovered[i].path);
            snprintf(applied->cpu[slot].orig, sizeof(applied->cpu[slot].orig),
                     "%.*s", (int)sizeof(applied->cpu[slot].orig) - 1, original);
            applied->cpu_set = 1;
            changed = 1;
            d5_log("cpu governor %s: %s -> performance", discovered[i].path,
                   original);
            continue;
        }
        /* Already recorded: a re-tune just re-writes the same value. */
        if (!d5_write_file(applied->cpu[slot].path, "performance")) continue;
        changed = 1;
    }
    return changed || applied->cpu_set > 0;
}

/* ---------------------------------------------------------------- MSAA */

static int apply_msaa(D5Applied *a, int msaa) {
    if (msaa != 4 && msaa != 8) return a->msaa_set > 0 ? 1 : 0;
    if (!a->msaa_set) {
        prop_probe(D5_PROP_MSAA, a->msaa_orig, sizeof(a->msaa_orig));
        prop_probe(D5_PROP_FORCE, a->force_orig_str, sizeof(a->force_orig_str));
        a->msaa_set = 1;
    }
    char value[8];
    snprintf(value, sizeof(value), "%d", msaa);
    int ok = prop_put(D5_PROP_MSAA, value);
    a->msaa = msaa;
    if (ok) {
        a->force_set = prop_put(D5_PROP_FORCE, "1") ? 1 : 0;
        d5_log("anti-aliasing: forced %dx MSAA (debug.egl.msaa=%d, force_msaa=1)",
               msaa, msaa);
    }
    return ok;
}

static void restore_msaa(D5Applied *a) {
    if (a->force_set) prop_restore(D5_PROP_FORCE, a->force_orig_str);
    if (a->msaa_set) prop_restore(D5_PROP_MSAA, a->msaa_orig);
    a->msaa_set = 0;
    a->force_set = 0;
    a->msaa = 0;
}

/* ------------------------------------------------------------ refresh */

static int apply_refresh(D5Applied *a, const D5Config *cfg) {
    if (cfg->refresh_rate <= 0 || !cfg->refresh_lock) {
        return a->refresh_set > 0 ? 1 : 0;
    }
    if (!a->refresh_set) {
        setting_probe("system", "peak_refresh_rate", a->refresh_orig_peak,
                      sizeof(a->refresh_orig_peak));
        setting_probe("system", "min_refresh_rate", a->refresh_orig_min,
                      sizeof(a->refresh_orig_min));
        a->refresh_set = 1;
    }
    a->refresh_value = cfg->refresh_rate;
    char value[8];
    snprintf(value, sizeof(value), "%d", cfg->refresh_rate);
    int ok = setting_put("system", "peak_refresh_rate", value);
    ok = setting_put("system", "min_refresh_rate", value) && ok;
    if (ok) d5_log("display: peak/min refresh rate pinned to %d Hz",
                   cfg->refresh_rate);
    return ok;
}

static void restore_refresh(D5Applied *a) {
    if (!a->refresh_set) return;
    setting_restore("system", "peak_refresh_rate", a->refresh_orig_peak);
    setting_restore("system", "min_refresh_rate", a->refresh_orig_min);
    a->refresh_set = 0;
    a->refresh_value = 0;
}

/* ------------------------------------------------- applied-state file */

int d5_applied_save(const char *path, const D5Applied *applied) {
    char buf[8192];
    size_t used = 0;
    int n = snprintf(buf + used, sizeof(buf) - used,
                     "{\n  \"active\": %s,\n  \"pkg\": \"%s\",\n"
                     "  \"downscale\": %.2f,\n  \"fps\": %d,\n  \"saturation\": %d,\n"
                     "  \"game_cmd\": %d,\n  \"governor\": %s,\n"
                     "  \"gov_path\": \"%s\",\n  \"gov_original\": \"%s\",\n"
                     "  \"msaa\": %d,\n  \"msaa_set\": %s,\n  \"msaa_original\": \"%s\",\n"
                     "  \"force_set\": %s,\n  \"force_original\": \"%s\",\n"
                     "  \"cpu_set\": %s,\n  \"cpu_count\": %d,\n",
                     applied->active ? "true" : "false",
                     d5_valid_package(applied->pkg) ? applied->pkg : "",
                     (double)applied->downscale, applied->fps, applied->saturation,
                     applied->game_cmd, applied->governor_set ? "true" : "false",
                     applied->gov_path, applied->gov_orig,
                     applied->msaa, applied->msaa_set ? "true" : "false",
                     applied->msaa_orig,
                     applied->force_set ? "true" : "false",
                     applied->force_orig_str,
                     applied->cpu_set ? "true" : "false",
                     applied->cpu_count);
    if (n < 0) return 0;
    used += (size_t)n;
    for (int i = 0; i < applied->cpu_count && i < 8 && used + 700 < sizeof(buf); i++) {
        n = snprintf(buf + used, sizeof(buf) - used,
                     "  \"cpu_path%d\": \"%s\",\n  \"cpu_orig%d\": \"%s\",\n",
                     i, applied->cpu[i].path, i, applied->cpu[i].orig);
        if (n < 0) return 0;
        used += (size_t)n;
    }
    n = snprintf(buf + used, sizeof(buf) - used,
                 "  \"refresh_set\": %s,\n  \"refresh_value\": %d,\n"
                 "  \"refresh_orig_peak\": \"%s\",\n  \"refresh_orig_min\": \"%s\",\n"
                 "  \"density_set\": %s,\n  \"density\": %d,\n"
                 "  \"density_original\": \"%s\",\n  \"density_base\": \"%s\"\n}\n",
                 applied->refresh_set ? "true" : "false",
                 applied->refresh_value,
                 applied->refresh_orig_peak, applied->refresh_orig_min,
                 applied->density_set ? "true" : "false",
                 applied->density,
                 applied->density_orig, applied->density_base);
    if (n < 0) return 0;
    used += (size_t)n;
    (void)used;
    return d5_write_file(path, buf);
}

static void load_cpu_slot(const D5Json *root, D5Applied *applied, int index) {
    char key[32];
    snprintf(key, sizeof(key), "cpu_path%d", index);
    const char *path = d5_json_str(d5_json_get(root, key), "");
    snprintf(key, sizeof(key), "cpu_orig%d", index);
    const char *orig = d5_json_str(d5_json_get(root, key), "");
    /* Only real sysfs governor paths are accepted for a restore slot. */
    if (index >= 8 || !path || path[0] != '/' || strstr(path, "..") ||
        !strstr(path, "scaling_governor")) {
        return;
    }
    snprintf(applied->cpu[index].path, D5_PATH_LEN, "%.*s", D5_PATH_LEN - 1, path);
    snprintf(applied->cpu[index].orig, sizeof(applied->cpu[index].orig), "%.*s",
             (int)sizeof(applied->cpu[index].orig) - 1, orig);
    if (index + 1 > applied->cpu_count) applied->cpu_count = index + 1;
}

int d5_applied_load(const char *path, D5Applied *applied) {
    char text[8192];
    if (!d5_read_file(path, text, sizeof(text))) return 0;
    D5Json *root = d5_json_parse(text, NULL, 0);
    if (!root) return 0;
    memset(applied, 0, sizeof(*applied));
    const char *pkg = d5_json_str(d5_json_get(root, "pkg"), "");
    if (d5_valid_package(pkg)) snprintf(applied->pkg, sizeof(applied->pkg), "%s", pkg);
    applied->active = d5_json_bool(d5_json_get(root, "active"), 0);
    applied->downscale = (float)d5_json_num(d5_json_get(root, "downscale"), 1.0);
    applied->fps = (int)d5_json_num(d5_json_get(root, "fps"), 0);
    applied->saturation = (int)d5_json_num(d5_json_get(root, "saturation"), 100);
    applied->game_cmd = (int)d5_json_num(d5_json_get(root, "game_cmd"), 0);
    applied->governor_set = d5_json_bool(d5_json_get(root, "governor"), 0);
    const char *gov_path = d5_json_str(d5_json_get(root, "gov_path"), "");
    const char *gov_orig = d5_json_str(d5_json_get(root, "gov_original"), "");
    snprintf(applied->gov_path, sizeof(applied->gov_path), "%s", gov_path);
    snprintf(applied->gov_orig, sizeof(applied->gov_orig), "%s", gov_orig);

    applied->msaa = (int)d5_json_num(d5_json_get(root, "msaa"), 0);
    applied->msaa_set = d5_json_bool(d5_json_get(root, "msaa_set"), 0);
    const char *msaa_orig = d5_json_str(d5_json_get(root, "msaa_original"), "");
    snprintf(applied->msaa_orig, sizeof(applied->msaa_orig), "%s", msaa_orig);
    applied->force_set = d5_json_bool(d5_json_get(root, "force_set"), 0);
    const char *force_orig = d5_json_str(d5_json_get(root, "force_original"), "");
    snprintf(applied->force_orig_str, sizeof(applied->force_orig_str), "%s",
             force_orig);

    applied->cpu_set = d5_json_bool(d5_json_get(root, "cpu_set"), 0);
    for (int i = 0; i < 8; i++) load_cpu_slot(root, applied, i);
    if (applied->cpu_count > 8) applied->cpu_count = 8;
    if (applied->cpu_count > 0) {
        int real_count = 0;
        for (int i = 0; i < applied->cpu_count; i++) {
            if (applied->cpu[i].path[0]) applied->cpu[real_count++] = applied->cpu[i];
        }
        applied->cpu_count = real_count;
    }

    applied->refresh_set = d5_json_bool(d5_json_get(root, "refresh_set"), 0);
    applied->refresh_value = (int)d5_json_num(d5_json_get(root, "refresh_value"), 0);
    const char *orig_peak = d5_json_str(d5_json_get(root, "refresh_orig_peak"), "");
    const char *orig_min = d5_json_str(d5_json_get(root, "refresh_orig_min"), "");
    snprintf(applied->refresh_orig_peak, sizeof(applied->refresh_orig_peak),
             "%s", orig_peak);
    snprintf(applied->refresh_orig_min, sizeof(applied->refresh_orig_min), "%s",
             orig_min);

    applied->density_set = d5_json_bool(d5_json_get(root, "density_set"), 0);
    applied->density = (int)d5_json_num(d5_json_get(root, "density"), 0);
    const char *density_orig = d5_json_str(d5_json_get(root, "density_original"), "");
    const char *density_base = d5_json_str(d5_json_get(root, "density_base"), "");
    snprintf(applied->density_orig, sizeof(applied->density_orig), "%s",
             density_orig);
    snprintf(applied->density_base, sizeof(applied->density_base), "%s",
             density_base);
    d5_json_free(root);
    return applied->active && applied->pkg[0] != '\0';
}

int d5_applied_clear(const char *path) {
    return unlink(path) == 0 || access(path, F_OK) != 0;
}

/* Copies the already-recorded originals forward when the same game is
 * re-tuned, so a reboot-crash recovery or config change never mixes restore
 * data from two entries. */
static void carry_over(const D5Applied *previous, D5Applied *next) {
    if (!previous->pkg[0] || next->pkg[0] == '\0' ||
        strcmp(previous->pkg, next->pkg) != 0) return;
    next->governor_set = previous->governor_set;
    snprintf(next->gov_path, sizeof(next->gov_path), "%s", previous->gov_path);
    snprintf(next->gov_orig, sizeof(next->gov_orig), "%s", previous->gov_orig);
    next->msaa_set = previous->msaa_set;
    next->msaa = previous->msaa;
    snprintf(next->msaa_orig, sizeof(next->msaa_orig), "%s", previous->msaa_orig);
    next->force_set = previous->force_set;
    snprintf(next->force_orig_str, sizeof(next->force_orig_str), "%s",
             previous->force_orig_str);
    next->cpu_set = previous->cpu_set;
    next->cpu_count = previous->cpu_count;
    for (int i = 0; i < previous->cpu_count && i < 8; i++) {
        next->cpu[i] = previous->cpu[i];
    }
    next->refresh_set = previous->refresh_set;
    next->refresh_value = previous->refresh_value;
    snprintf(next->refresh_orig_peak, sizeof(next->refresh_orig_peak), "%s",
             previous->refresh_orig_peak);
    snprintf(next->refresh_orig_min, sizeof(next->refresh_orig_min), "%s",
             previous->refresh_orig_min);
    next->density_set = previous->density_set;
    next->density = previous->density;
    snprintf(next->density_orig, sizeof(next->density_orig), "%s",
             previous->density_orig);
    snprintf(next->density_base, sizeof(next->density_base), "%s",
             previous->density_base);
}

int d5_apply_profile(const D5Config *cfg, const char *pkg, const D5Decision *dec,
                     D5Applied *applied) {
    if (!cfg || !dec || !applied || !d5_valid_package(pkg)) return 0;
    D5Applied previous = *applied;
    memset(applied, 0, sizeof(*applied));
    snprintf(applied->pkg, sizeof(applied->pkg), "%s", pkg);
    applied->downscale = dec->downscale;
    applied->fps = dec->fps;
    applied->saturation = 100;
    applied->active = 1;
    carry_over(&previous, applied);

    apply_game_manager(dec, pkg, &applied->game_cmd);
    if (apply_color(cfg, pkg)) applied->saturation = cfg->saturation;
    else applied->saturation = 100;
    if (cfg->gpu_boost) apply_gpu_boost(applied);
    if (cfg->cpu_boost) apply_cpu_boost(applied);
    if (cfg->msaa >= 4) apply_msaa(applied, cfg->msaa);
    if (cfg->refresh_lock && cfg->refresh_rate > 0) apply_refresh(applied, cfg);
    if (cfg->ui_density_delta != 0) density_apply(applied, cfg);

    d5_log("applied %s: scale %.2f fps %d game_cmd %d saturation %d "
           "governor(gpu %d cpu %d cpu_slots %d) msaa %d refresh %d ui %d",
           pkg, (double)applied->downscale, applied->fps, applied->game_cmd,
           applied->saturation, applied->governor_set, applied->cpu_set,
           applied->cpu_count, applied->msaa, applied->refresh_value,
           applied->density);
    return 1;
}

void d5_restore_profile(D5Applied *applied) {
    if (!applied || !applied->active) return;
    char cmd[512];
    char out[512];

    /* Release the per-game UI first, then the graphics, then the governor:
     * the game regains a normal interface before the SoC leaves its
     * performance state. */
    density_restore(applied);
    restore_refresh(applied);
    restore_msaa(applied);
    cpu_restore(applied);

    if (d5_valid_package(applied->pkg)) {
        if (applied->game_cmd) {
            snprintf(cmd, sizeof(cmd), "cmd game reset %s", applied->pkg);
            d5_cmd(cmd, out, sizeof(out));
        }
        if (applied->saturation < 100) {
            snprintf(cmd, sizeof(cmd),
                     "cmd color_display set-layer-saturation 100 %s %s",
                     D5_APP_PACKAGE, applied->pkg);
            d5_cmd(cmd, out, sizeof(out));
        }
    }
    if (applied->governor_set && applied->gov_path[0] && applied->gov_orig[0]) {
        if (!d5_write_file(applied->gov_path, applied->gov_orig)) {
            d5_log("cannot restore %s to %s", applied->gov_path, applied->gov_orig);
        } else {
            d5_log("gpu governor %s restored to %s", applied->gov_path,
                   applied->gov_orig);
        }
    }
    d5_log("restored %s", applied->pkg);
    memset(applied, 0, sizeof(*applied));
}
