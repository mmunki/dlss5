#include "dlss5.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------- foreground app */

static const char *const kFocusKeys[] = {
    "topResumedActivity=",
    "mResumedActivity:",
    "ResumedActivity:",
    "mFocusedApp=",
};

static int package_from_line(const char *line, char *pkg, size_t len) {
    const char *p = line;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '{' || *p == '}') p++;
        const char *start = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '{' && *p != '}') p++;
        size_t n = (size_t)(p - start);
        const char *slash = memchr(start, '/', n);
        if (!slash) continue;
        size_t name_len = (size_t)(slash - start);
        if (name_len == 0 || name_len >= len) continue;
        char candidate[D5_PKG_LEN];
        memcpy(candidate, start, name_len);
        candidate[name_len] = '\0';
        if (d5_valid_package(candidate)) {
            snprintf(pkg, len, "%s", candidate);
            return 1;
        }
    }
    return 0;
}

int d5_foreground_package(char *pkg, size_t len) {
    static char *dump;
    if (!dump) {
        dump = malloc(768 * 1024);
        if (!dump) return 0;
    }
    int rc = d5_cmd("dumpsys activity activities 2>/dev/null", dump, 768 * 1024);
    if (rc == 0 && dump[0]) {
        for (size_t k = 0; k < sizeof(kFocusKeys) / sizeof(kFocusKeys[0]); k++) {
            const char *line = dump;
            while (line && *line) {
                const char *next = strchr(line, '\n');
                if (strstr(line, kFocusKeys[k]) &&
                    package_from_line(line, pkg, len)) {
                    return 1;
                }
                line = next ? next + 1 : NULL;
            }
        }
    }
    /* Some ROMs only report the focused window. */
    rc = d5_cmd("dumpsys window 2>/dev/null | grep -m1 mCurrentFocus", dump,
                768 * 1024);
    if (rc == 0 && dump[0] && strstr(dump, "mCurrentFocus")) {
        return package_from_line(dump, pkg, len);
    }
    return 0;
}

/* --------------------------------------------------- third party packages */

#define D5_PACKAGE_CACHE 768

static char (*g_cache)[D5_PKG_LEN];
static int g_cache_count;
static long long g_cache_ms;

static void cache_refresh(void) {
    if (!g_cache) {
        g_cache = calloc(D5_PACKAGE_CACHE, D5_PKG_LEN);
        if (!g_cache) return;
    }
    static char *out;
    if (!out) {
        out = malloc(512 * 1024);
        if (!out) return;
    }
    g_cache_count = 0;
    g_cache_ms = d5_now_ms();
    if (d5_cmd("pm list packages -3 2>/dev/null", out, 512 * 1024) != 0) return;
    char *save = NULL;
    for (char *line = strtok_r(out, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        char *name = line;
        if (strncmp(name, "package:", 8) == 0) name += 8;
        while (*name == ' ') name++;
        for (char *p = name; *p; p++) {
            if (*p == '\r' || *p == ' ' || *p == '\t') {
                *p = '\0';
                break;
            }
        }
        if (!d5_valid_package(name)) continue;
        if (g_cache_count >= D5_PACKAGE_CACHE) break;
        snprintf(g_cache[g_cache_count], D5_PKG_LEN, "%s", name);
        g_cache_count++;
    }
    d5_log("package cache: %d user packages", g_cache_count);
}

int d5_is_user_package(const char *pkg) {
    if (!pkg || !d5_valid_package(pkg)) return 0;
    if (strcmp(pkg, D5_APP_PACKAGE) == 0) return 1;
    if (!g_cache || g_cache_count == 0 || d5_now_ms() - g_cache_ms > 300000) {
        cache_refresh();
    }
    for (int i = 0; i < g_cache_count; i++) {
        if (strcmp(g_cache[i], pkg) == 0) return 1;
    }
    /* The cache can be cold right after boot, retry once. */
    if (d5_now_ms() - g_cache_ms > 10000) {
        cache_refresh();
        for (int i = 0; i < g_cache_count; i++) {
            if (strcmp(g_cache[i], pkg) == 0) return 1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------ screen on */

int d5_screen_on(void) {
    static char *out;
    if (!out) {
        out = malloc(256 * 1024);
        if (!out) return 1;
    }
    if (d5_cmd("dumpsys power 2>/dev/null | grep -m1 -E "
               "'mWakefulness=|Display Power'", out, 256 * 1024) == 0 &&
        out[0]) {
        if (strstr(out, "mWakefulness=Awake") || strstr(out, "state=ON")) return 1;
        if (strstr(out, "mWakefulness=") || strstr(out, "state=OFF")) return 0;
    }
    return 1;
}

/* --------------------------------------------------------------- status */

int d5_write_status(const char *path, int enabled, const D5Applied *applied,
                    const D5Decision *dec) {
    char buf[1024];
    snprintf(buf, sizeof(buf),
             "{\n  \"running\": true,\n  \"enabled\": %s,\n"
             "  \"active\": %s,\n  \"package\": \"%s\",\n"
             "  \"downscale\": %.2f,\n  \"fps\": %d,\n"
             "  \"saturation\": %d,\n  \"scale_hint\": %.3f,\n"
             "  \"fps_hint\": %.3f,\n"
             "  \"msaa\": %d,\n  \"cpu\": %d,\n  \"gpu\": %d,\n"
             "  \"refresh\": %d,\n  \"density\": %d\n}\n",
             enabled ? "true" : "false",
             applied && applied->active ? "true" : "false",
             applied && applied->active && d5_valid_package(applied->pkg)
                 ? applied->pkg : "",
             applied && applied->active ? (double)applied->downscale : 1.0,
             applied && applied->active ? applied->fps : 0,
             applied && applied->active ? applied->saturation : 100,
             dec ? (double)dec->scale_hint : 0.0,
             dec ? (double)dec->fps_hint : 0.0,
             applied && applied->active ? applied->msaa : 0,
             applied && applied->active && applied->cpu_set ? applied->cpu_count
                                                            : 0,
             applied && applied->active && applied->governor_set ? 1 : 0,
             applied && applied->active ? applied->refresh_value : 0,
             applied && applied->active && applied->density_set ? applied->density
                                                                : 0);
    return d5_write_file(path, buf);
}
