/*
 * Self test of the daemon core.  It runs on the host during the build and on
 * the device with `dlss5d --selftest`, so the exact same binary that ships in
 * the Magisk module proves its logic on the target ABI.
 */
#include "dlss5.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_failures;
static int g_checks;

#define CHECK(cond)                                                            \
    do {                                                                       \
        g_checks++;                                                            \
        if (!(cond)) {                                                         \
            g_failures++;                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
        }                                                                      \
    } while (0)

static const char *tmp_dir(void) {
    const char *env = getenv("DLSS5_TMP");
    if (env && *env) return env;
    if (access("/data/local/tmp", W_OK) == 0) return "/data/local/tmp";
    if (access("/tmp", W_OK) == 0) return "/tmp";
    return ".";
}

static void test_json(void) {
    const char *text =
        "{\"a\": 1, \"b\": [true, false, \"x\\u00e9\"], \"c\": {\"d\": -2.5}}";
    char err[128];
    D5Json *root = d5_json_parse(text, err, sizeof(err));
    CHECK(root != NULL);
    if (!root) return;
    CHECK(d5_json_type(root) == D5_JSON_OBJECT);
    CHECK(d5_json_num(d5_json_get(root, "a"), 0) == 1);
    const D5Json *b = d5_json_get(root, "b");
    CHECK(d5_json_len(b) == 3);
    CHECK(d5_json_bool(d5_json_at(b, 0), 0) == 1);
    CHECK(d5_json_bool(d5_json_at(b, 1), 1) == 0);
    CHECK(strlen(d5_json_str(d5_json_at(b, 2), "")) == 3); /* x + UTF-8 e-acute */
    CHECK(d5_json_num(d5_json_get(d5_json_get(root, "c"), "d"), 0) == -2.5);
    d5_json_free(root);

    CHECK(d5_json_parse("{\"a\": }", err, sizeof(err)) == NULL);
    CHECK(d5_json_parse("[1,2,3]", err, sizeof(err)) != NULL);
}

static void test_scale(void) {
    CHECK(fabsf(d5_snap_scale(0.10f) - 0.30f) < 1e-4f);
    CHECK(fabsf(d5_snap_scale(0.71f) - 0.70f) < 1e-4f);
    CHECK(fabsf(d5_snap_scale(0.73f) - 0.75f) < 1e-4f);
    CHECK(fabsf(d5_snap_scale(0.88f) - 0.90f) < 1e-4f);
    CHECK(fabsf(d5_snap_scale(1.00f) - 1.00f) < 1e-4f);
    CHECK(fabsf(d5_snap_scale(0.99f) - 1.00f) < 1e-4f);
}

static void test_packages(void) {
    CHECK(d5_valid_package("com.example.game"));
    CHECK(d5_valid_package("a.b"));
    CHECK(!d5_valid_package(""));
    CHECK(!d5_valid_package("nodots"));
    CHECK(!d5_valid_package(".leading"));
    CHECK(!d5_valid_package("trailing."));
    CHECK(!d5_valid_package("double..dot"));
    CHECK(!d5_valid_package("bad pkg"));
    CHECK(!d5_valid_package("semi;colon"));
    CHECK(!d5_valid_package("$(reboot)"));
}

static void test_config(void) {
    char path[D5_PATH_LEN];
    snprintf(path, sizeof(path), "%s/dlss5-selftest-config.json", tmp_dir());

    D5Config cfg;
    d5_config_defaults(&cfg);
    CHECK(cfg.enabled == 1);
    CHECK(fabsf(cfg.downscale - 0.70f) < 1e-6f);
    cfg.downscale = 0.75f;
    cfg.target_fps = 45;
    cfg.saturation = 92;
    cfg.games[0][0] = '\0';
    snprintf(cfg.games[0], D5_PKG_LEN, "com.example.one");
    snprintf(cfg.games[1], D5_PKG_LEN, "com.example.two");
    cfg.game_count = 2;
    CHECK(d5_config_save(path, &cfg));

    D5Config loaded;
    d5_config_defaults(&loaded);
    CHECK(d5_config_load(path, &loaded));
    CHECK(loaded.enabled == 1);
    CHECK(fabsf(loaded.downscale - 0.75f) < 1e-4f);
    CHECK(loaded.target_fps == 45);
    CHECK(loaded.saturation == 92);
    CHECK(loaded.game_count == 2);
    CHECK(d5_config_has_game(&loaded, "com.example.two"));
    CHECK(!d5_config_has_game(&loaded, "com.example.three"));

    /* A hostile config cannot smuggle a shell command into the games list. */
    const char *evil = "{\"enabled\": true, \"games\": [\"com.x; reboot\", \"ok.pkg\"]}";
    CHECK(d5_write_file(path, evil));
    D5Config parsed;
    d5_config_defaults(&parsed);
    CHECK(d5_config_load(path, &parsed));
    CHECK(parsed.game_count == 1);
    CHECK(d5_config_has_game(&parsed, "ok.pkg"));

    /* Out of range values are clamped, not trusted. */
    const char *wild = "{\"downscale\": 5, \"target_fps\": 999, \"saturation\": -4}";
    CHECK(d5_write_file(path, wild));
    CHECK(d5_config_load(path, &parsed));
    CHECK(parsed.downscale <= 1.0f && parsed.downscale >= D5_SCALE_MIN);
    CHECK(parsed.target_fps == 240);
    CHECK(parsed.saturation == 0);

    unlink(path);
}

static void test_applied(void) {
    char path[D5_PATH_LEN];
    snprintf(path, sizeof(path), "%s/dlss5-selftest-applied.json", tmp_dir());
    D5Applied applied;
    memset(&applied, 0, sizeof(applied));
    applied.active = 1;
    snprintf(applied.pkg, sizeof(applied.pkg), "com.example.game");
    applied.downscale = 0.75f;
    applied.fps = 60;
    applied.saturation = 100;
    applied.game_cmd = 1;
    applied.governor_set = 1;
    snprintf(applied.gov_path, sizeof(applied.gov_path),
             "/sys/class/kgsl/kgsl-3d0/devfreq/governor");
    snprintf(applied.gov_orig, sizeof(applied.gov_orig), "msm-adreno-tz");
    CHECK(d5_applied_save(path, &applied));

    D5Applied loaded;
    memset(&loaded, 0, sizeof(loaded));
    CHECK(d5_applied_load(path, &loaded));
    CHECK(loaded.active == 1);
    CHECK(strcmp(loaded.pkg, "com.example.game") == 0);
    CHECK(fabsf(loaded.downscale - 0.75f) < 1e-4f);
    CHECK(loaded.fps == 60);
    CHECK(loaded.governor_set == 1);
    CHECK(strcmp(loaded.gov_orig, "msm-adreno-tz") == 0);
    CHECK(d5_applied_clear(path));
    CHECK(!d5_applied_load(path, &loaded));
}

static void test_model(void) {
    D5Config cfg;
    d5_config_defaults(&cfg);
    cfg.downscale = 0.50f;

    D5Decision low, high;
    D5Signals cool = {0.05f, 0.20f, 0.90f, 0.90f};
    D5Signals hot = {0.95f, 0.95f, 0.90f, 0.90f};
    d5_tune(&cool, &cfg, &low);
    d5_tune(&hot, &cfg, &high);

    CHECK(low.downscale >= cfg.downscale && low.downscale <= 1.0f);
    CHECK(high.downscale >= cfg.downscale && high.downscale <= 1.0f);
    CHECK(high.downscale <= low.downscale);
    CHECK(fabsf(low.downscale - d5_snap_scale(low.downscale)) < 1e-4f ||
          low.downscale == 1.0f);
    CHECK(low.fps == cfg.target_fps);
    CHECK(high.fps <= cfg.target_fps);
    CHECK(high.fps == 0 || high.fps >= 24);

    /* Fast SoC must not be tuned below a slow one in the same conditions. */
    D5Signals fast = {0.40f, 0.60f, 1.00f, 0.80f};
    D5Signals slow = {0.40f, 0.60f, 0.00f, 0.80f};
    D5Decision d_fast, d_slow;
    d5_tune(&fast, &cfg, &d_fast);
    d5_tune(&slow, &cfg, &d_slow);
    CHECK(d_fast.downscale >= d_slow.downscale);

    /* AI off means exactly the user floor. */
    cfg.ai = 0;
    cfg.downscale = 0.65f;
    d5_tune(&cool, &cfg, &low);
    CHECK(fabsf(low.downscale - 0.65f) < 1e-4f);

    /* Sweep: every output must stay inside the documented range. */
    for (int t = 0; t <= 10; t++) {
        for (int b = 0; b <= 10; b++) {
            D5Signals s = {t / 10.0f, b / 10.0f, 0.5f, 0.6f};
            D5Config c;
            d5_config_defaults(&c);
            D5Decision d;
            d5_tune(&s, &c, &d);
            CHECK(d.downscale >= D5_SCALE_MIN && d.downscale <= 1.0f);
            CHECK(d.fps == 0 || (d.fps >= 24 && d.fps <= c.target_fps));
        }
    }
}

static void test_commands(void) {
    char buf[512];
    CHECK(d5_build_game_set(buf, sizeof(buf), 0.75f, 60, "com.example.game"));
    CHECK(strcmp(buf, "cmd game set --downscale 0.75 --fps 60 com.example.game") == 0);
    CHECK(d5_build_game_set(buf, sizeof(buf), 1.0f, 0, "com.example.game"));
    CHECK(strcmp(buf, "cmd game set --downscale disable --fps 0 com.example.game") == 0);
    CHECK(!d5_build_game_set(buf, sizeof(buf), 0.75f, 60, "com.example; reboot"));
}

int d5_selftest(void) {
    printf("dlss5d %s selftest\n", D5_VERSION);
    test_json();
    test_scale();
    test_packages();
    test_config();
    test_applied();
    test_model();
    test_commands();
    printf("%d checks, %d failure(s)\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
