/*
 * DLSS5 -- per-game render scaling, GPU boost and colour calibration for
 * Android arm64.  This header is the small, testable core: it has no
 * dependency on Android and it compiles both for the device (bionic,
 * aarch64) and for the host test binary (glibc, x86_64).
 */
#ifndef DLSS5_H
#define DLSS5_H

#include <stddef.h>

#define D5_VERSION "2.0.0"
#define D5_VERSION_CODE 2

#define D5_MAX_GAMES 64
#define D5_PKG_LEN 128
#define D5_PATH_LEN 320

/* Package name of the control-centre app.  It is the caller identity that
 * Android's per-layer saturation transform is attributed to. */
#define D5_APP_PACKAGE "com.mmunki.dlss5"

/* Render-scale ladder documented by GameManagerShellCommand. */
#define D5_SCALE_MIN 0.30f
#define D5_SCALE_MAX 0.90f
#define D5_SCALE_STEP 0.05f

/* Default config/state locations (overridable with --config). */
#define D5_DEFAULT_DIR "/data/adb/dlss5"
#define D5_DEFAULT_CONFIG "/data/adb/dlss5/config.json"
#define D5_DEFAULT_STATUS "/data/adb/dlss5/status.json"
#define D5_DEFAULT_APPLIED "/data/adb/dlss5/applied.json"

typedef struct {
    int enabled;          /* master switch, driven by the QS tile */
    int ai;               /* adaptive neural controller */
    int gpu_boost;        /* write the GPU governor while a game runs */
    int cpu_boost;        /* pin every CPU cluster to performance while playing */
    int msaa;             /* forced anti-aliasing: 0, 4 or 8 (setprop) */
    int refresh_lock;     /* touch peak/min refresh rate while a game runs */
    int refresh_rate;     /* hertz target, 0 = leave the panel alone */
    int ui_density_delta; /* per-game UI scale shift, -200..200 (wm density) */
    int hud;              /* app-side game HUD; the daemon only round-trips it */
    float downscale;      /* user quality floor, 1.0 = native */
    int target_fps;       /* 0 = leave the panel alone */
    int saturation;       /* 0..100, 100 = native colour */
    int game_count;
    char games[D5_MAX_GAMES][D5_PKG_LEN];
} D5Config;

typedef struct {
    float thermal;    /* 0..1, 1 = hot */
    float gpu_busy;   /* 0..1 */
    float perf_class; /* 0..1, 1 = fast SoC */
    float battery;    /* 0..1 */
} D5Signals;

typedef struct {
    float downscale; /* ladder value, 1.0 = native */
    int fps;         /* 0 = no cap */
    float scale_hint;
    float fps_hint;
} D5Decision;

/* One applied CPU cluster (sysfs cpufreq policy). */
typedef struct {
    char path[D5_PATH_LEN];
    char orig[24];
} D5CpuSlot;

typedef struct {
    int active;
    char pkg[D5_PKG_LEN];
    float downscale;
    int fps;
    int saturation;
    int game_cmd;
    int governor_set;
    char gov_path[D5_PATH_LEN];
    char gov_orig[64];
    int msaa;             /* 0/4/8 as applied */
    int msaa_set;
    char msaa_orig[24];   /* "" = the prop was not set before */
    int force_set;        /* debug.egl.force_msaa was touched */
    char force_orig_str[24];
    int cpu_set;
    int cpu_count;
    D5CpuSlot cpu[8];
    int refresh_set;      /* peak/min refresh-rate settings touched */
    int refresh_value;
    char refresh_orig_peak[24]; /* "" = the setting was unset before */
    char refresh_orig_min[24];
    int density_set;      /* wm density touched */
    int density;          /* value applied */
    char density_orig[24];/* "" = there was no override before */
    char density_base[24];/* the physical density the shift is relative to */
} D5Applied;

/* ---------------------------------------------------------------- logging */
void d5_log(const char *fmt, ...);
void d5_set_verbose(int verbose);
void d5_set_dry_run(int dry_run);
int d5_dry_run(void);

/* ------------------------------------------------------------------ utils */
float d5_clampf(float v, float lo, float hi);
int d5_valid_package(const char *pkg);
int d5_read_file(const char *path, char *buf, size_t len);
int d5_read_number(const char *path, double *out);
int d5_write_file(const char *path, const char *data);
long long d5_now_ms(void);
/* Runs a command through /bin/sh, captures stdout and returns its exit code
 * (-1 when it could not run).  Package names must be validated first. */
int d5_cmd(const char *cmd, char *out, size_t out_len);
/* Builds "cmd game ..." style commands without ever interpolating invalid
 * package names. */
int d5_build_game_set(char *buf, size_t len, float downscale, int fps,
                      const char *pkg);

/* ------------------------------------------------------------------- json */
typedef enum {
    D5_JSON_NULL,
    D5_JSON_BOOL,
    D5_JSON_NUMBER,
    D5_JSON_STRING,
    D5_JSON_ARRAY,
    D5_JSON_OBJECT
} D5JsonType;

typedef struct D5Json D5Json;

D5Json *d5_json_parse(const char *text, char *err, size_t errlen);
void d5_json_free(D5Json *v);
D5JsonType d5_json_type(const D5Json *v);
const D5Json *d5_json_get(const D5Json *obj, const char *key);
const D5Json *d5_json_at(const D5Json *arr, size_t index);
size_t d5_json_len(const D5Json *v);
int d5_json_bool(const D5Json *v, int fallback);
double d5_json_num(const D5Json *v, double fallback);
const char *d5_json_str(const D5Json *v, const char *fallback);

/* ----------------------------------------------------------------- config */
void d5_config_defaults(D5Config *cfg);
int d5_config_load(const char *path, D5Config *cfg);
int d5_config_save(const char *path, const D5Config *cfg);
int d5_config_has_game(const D5Config *cfg, const char *pkg);
float d5_snap_scale(float scale);
int d5_snap_msaa(int value);

/* ---------------------------------------------------------------- signals */
float d5_read_thermal(void);
float d5_read_gpu_busy(void);
float d5_read_battery(void);
float d5_read_perf_class(void);
int d5_gpu_governors(char paths[][D5_PATH_LEN], int max);
/* Same discovery for CPU frequency policies (cpufreq clusters). */
int d5_cpu_governors(D5CpuSlot slots[], int max);
/* Expands a path pattern with one '*' per component (bionic has no glob). */
int d5_glob_paths(const char *pattern, char paths[][D5_PATH_LEN], int max);

/* ----------------------------------------------------------------- tuning */
void d5_tune(const D5Signals *sig, const D5Config *cfg, D5Decision *out);

/* ---------------------------------------------------------------- profile */
int d5_apply_profile(const D5Config *cfg, const char *pkg, const D5Decision *dec,
                     D5Applied *applied);
void d5_restore_profile(D5Applied *applied);
int d5_applied_save(const char *path, const D5Applied *applied);
int d5_applied_load(const char *path, D5Applied *applied);
int d5_applied_clear(const char *path);

/* ------------------------------------------------------------------- misc */
/* Extracts the package of the current foreground activity. */
int d5_foreground_package(char *pkg, size_t len);
/* True when the package is a user-installed (third party) app. */
int d5_is_user_package(const char *pkg);
/* True when the screen is on. */
int d5_screen_on(void);
/* Writes the status JSON the app and the tile read. */
int d5_write_status(const char *path, int enabled, const D5Applied *applied,
                    const D5Decision *dec);
/* Self test used by the build: returns 0 when every check passed. */
int d5_selftest(void);

#endif /* DLSS5_H */
