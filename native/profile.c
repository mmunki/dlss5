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

static int apply_gpu_boost(D5Applied *applied) {
    if (applied->governor_set && applied->gov_path[0]) return 1;
    char paths[8][D5_PATH_LEN];
    int count = d5_gpu_governors(paths, 8);
    for (int i = 0; i < count; i++) {
        char original[64];
        if (!d5_read_file(paths[i], original, sizeof(original))) continue;
        if (strcmp(original, "performance") == 0) return 0; /* already boosted */
        if (!d5_write_file(paths[i], "performance")) {
            d5_log("cannot write %s", paths[i]);
            continue;
        }
        snprintf(applied->gov_path, sizeof(applied->gov_path), "%.*s", (int)sizeof(applied->gov_path) - 1, paths[i]);
        snprintf(applied->gov_orig, sizeof(applied->gov_orig), "%s", original);
        applied->governor_set = 1;
        d5_log("gpu governor %s: %s -> performance", paths[i], original);
        return 1;
    }
    return 0;
}

int d5_applied_save(const char *path, const D5Applied *applied) {
    char buf[1024];
    snprintf(buf, sizeof(buf),
             "{\n  \"active\": %s,\n  \"pkg\": \"%s\",\n"
             "  \"downscale\": %.2f,\n  \"fps\": %d,\n  \"saturation\": %d,\n"
             "  \"game_cmd\": %d,\n  \"governor\": %s,\n"
             "  \"gov_path\": \"%s\",\n  \"gov_original\": \"%s\"\n}\n",
             applied->active ? "true" : "false",
             d5_valid_package(applied->pkg) ? applied->pkg : "",
             (double)applied->downscale, applied->fps, applied->saturation,
             applied->game_cmd, applied->governor_set ? "true" : "false",
             applied->gov_path, applied->gov_orig);
    return d5_write_file(path, buf);
}

int d5_applied_load(const char *path, D5Applied *applied) {
    char text[2048];
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
    d5_json_free(root);
    return applied->active && applied->pkg[0] != '\0';
}

int d5_applied_clear(const char *path) {
    return unlink(path) == 0 || access(path, F_OK) != 0;
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
    /* A re-tune for the same game keeps the original GPU governor value,
     * otherwise a later restore would write "performance" back. */
    if (previous.active && strcmp(previous.pkg, pkg) == 0 && previous.governor_set) {
        applied->governor_set = previous.governor_set;
        snprintf(applied->gov_path, sizeof(applied->gov_path), "%s", previous.gov_path);
        snprintf(applied->gov_orig, sizeof(applied->gov_orig), "%s", previous.gov_orig);
    }

    apply_game_manager(dec, pkg, &applied->game_cmd);
    if (apply_color(cfg, pkg)) applied->saturation = cfg->saturation;
    else applied->saturation = 100;
    if (cfg->gpu_boost) apply_gpu_boost(applied);

    d5_log("applied %s: scale %.2f fps %d game_cmd %d saturation %d governor %d",
           pkg, (double)applied->downscale, applied->fps, applied->game_cmd,
           applied->saturation, applied->governor_set);
    return 1;
}

void d5_restore_profile(D5Applied *applied) {
    if (!applied || !applied->active) return;
    char cmd[512];
    char out[512];

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
