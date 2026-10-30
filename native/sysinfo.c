#include "dlss5.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define D5_MAX_ZONES 64

float d5_read_thermal(void) {
    static char paths[D5_MAX_ZONES][D5_PATH_LEN];
    float hottest = -1.0f;
    int count = d5_glob_paths("/sys/class/thermal/thermal_zone*/temp", paths,
                              D5_MAX_ZONES);
    for (int i = 0; i < count; i++) {
        double raw;
        if (!d5_read_number(paths[i], &raw)) continue;
        double celsius = raw;
        if (celsius > 1000.0) celsius /= 1000.0;
        if (celsius < -40.0 || celsius > 200.0) continue;
        if (celsius > hottest) hottest = (float)celsius;
    }
    if (hottest < 0.0f) {
        double battery;
        if (d5_read_number("/sys/class/power_supply/battery/temp", &battery)) {
            hottest = battery > 1000.0 ? (float)(battery / 1000.0) : (float)battery;
        }
    }
    if (hottest < 0.0f) return 0.25f;
    /* 35 C is idle, 65 C is where a phone starts to throttle hard. */
    return d5_clampf((hottest - 35.0f) / 30.0f, 0.0f, 1.0f);
}

float d5_read_gpu_busy(void) {
    static const char *paths[] = {
        "/sys/class/kgsl/kgsl-3d0/gpu_busy_percentage",
        "/sys/class/kgsl/kgsl-3d0/gpu_utilization",
        "/sys/class/misc/mali0/device/utilization",
        "/sys/kernel/gpu/gpu_utilization",
    };
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        double v;
        if (d5_read_number(paths[i], &v)) {
            return d5_clampf((float)(v / 100.0), 0.0f, 1.0f);
        }
    }
    /* Adreno exposes "busy total" (jiffies); their ratio is the load. */
    char buf[128];
    if (d5_read_file("/sys/class/kgsl/kgsl-3d0/gpubusy", buf, sizeof(buf))) {
        double busy = 0, total = 0;
        if (sscanf(buf, "%lf %lf", &busy, &total) == 2 && total > 0) {
            return d5_clampf((float)(busy / total), 0.0f, 1.0f);
        }
    }
    static char load_paths[8][D5_PATH_LEN];
    int count = d5_glob_paths("/sys/class/devfreq/*/gpu_load", load_paths, 8);
    for (int i = 0; i < count; i++) {
        double v;
        if (d5_read_number(load_paths[i], &v)) {
            return d5_clampf((float)(v / 100.0), 0.0f, 1.0f);
        }
    }
    return 0.5f;
}

float d5_read_battery(void) {
    double cap;
    if (d5_read_number("/sys/class/power_supply/battery/capacity", &cap)) {
        return d5_clampf((float)(cap / 100.0), 0.0f, 1.0f);
    }
    return 0.7f;
}

float d5_read_perf_class(void) {
    double khz;
    if (d5_read_number("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq", &khz)) {
        /* 1.0 GHz .. 3.4 GHz maps to 0 .. 1 */
        return d5_clampf((float)((khz / 1000.0 - 1000.0) / 2400.0), 0.0f, 1.0f);
    }
    long cores = sysconf(_SC_NPROCESSORS_CONF);
    if (cores <= 0) cores = 4;
    return d5_clampf((float)(cores - 2) / 6.0f, 0.0f, 1.0f);
}

static int is_gpu_governor_path(const char *path) {
    static const char *needles[] = {"gpu", "mali", "kgsl", "3d0", "3d", "gfx",
                                    "adreno"};
    char lower[D5_PATH_LEN];
    size_t i;
    for (i = 0; i + 1 < sizeof(lower) && path[i]; i++) {
        lower[i] = (char)(path[i] >= 'A' && path[i] <= 'Z' ? path[i] + 32 : path[i]);
    }
    lower[i] = '\0';
    for (size_t n = 0; n < sizeof(needles) / sizeof(needles[0]); n++) {
        if (strstr(lower, needles[n])) return 1;
    }
    return 0;
}

static int governor_supports_performance(const char *path) {
    char dir[D5_PATH_LEN + 1];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (!slash) return 1;
    *slash = '\0';
    char available[D5_PATH_LEN + 40];
    snprintf(available, sizeof(available), "%s/available_governors", dir);
    char buf[512];
    if (!d5_read_file(available, buf, sizeof(buf))) return 1;
    return strstr(buf, "performance") != NULL;
}

int d5_gpu_governors(char paths[][D5_PATH_LEN], int max) {
    static const char *patterns[] = {
        "/sys/class/kgsl/kgsl-3d0/devfreq/governor",
        "/sys/class/kgsl/kgsl-3d0/governor",
        "/sys/kernel/gpu/gpu_governor",
        "/sys/class/devfreq/*/governor",
        "/sys/devices/platform/*/devfreq/*/governor",
        "/sys/devices/platform/*/*/devfreq/*/governor",
        "/sys/devices/platform/*/*/*/devfreq/*/governor",
    };
    static char candidates[32][D5_PATH_LEN];
    int count = 0;
    for (size_t p = 0; p < sizeof(patterns) / sizeof(patterns[0]) && count < max; p++) {
        int found = d5_glob_paths(patterns[p], candidates, 32);
        for (int i = 0; i < found && count < max; i++) {
            const char *path = candidates[i];
            if (!is_gpu_governor_path(path)) continue;
            if (access(path, R_OK | W_OK) != 0) continue;
            if (!governor_supports_performance(path)) continue;
            int duplicate = 0;
            for (int k = 0; k < count; k++) {
                if (strcmp(paths[k], path) == 0) {
                    duplicate = 1;
                    break;
                }
            }
            if (duplicate) continue;
            snprintf(paths[count], D5_PATH_LEN, "%.*s", D5_PATH_LEN - 1, path);
            count++;
        }
    }
    return count;
}
