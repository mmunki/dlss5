#include "dlss5.h"

#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop;
static volatile sig_atomic_t g_reload;

static void on_stop(int sig) {
    (void)sig;
    g_stop = 1;
}

static void on_reload(int sig) {
    (void)sig;
    g_reload = 1;
}

static long long mtime_ms(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return (long long)st.st_mtime * 1000;
}

static void mkdir_p(const char *dir) {
    char tmp[D5_PATH_LEN];
    snprintf(tmp, sizeof(tmp), "%s", dir);
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        mkdir(tmp, 0700);
        *p = '/';
    }
    mkdir(tmp, 0700);
}

static void sleep_ms(long ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

static void gather_signals(D5Signals *sig) {
    sig->thermal = d5_read_thermal();
    sig->gpu_busy = d5_read_gpu_busy();
    sig->perf_class = d5_read_perf_class();
    sig->battery = d5_read_battery();
}

typedef struct {
    const char *config;
    const char *status;
    const char *applied;
    const char *once_pkg;
    int keep;
} Paths;

static int run_daemon(const Paths *paths) {
    mkdir_p(D5_DEFAULT_DIR);

    D5Config cfg;
    d5_config_defaults(&cfg);
    if (d5_config_load(paths->config, &cfg)) {
        d5_log("config loaded: enabled=%d ai=%d games=%d scale=%.2f fps=%d sat=%d",
               cfg.enabled, cfg.ai, cfg.game_count, (double)cfg.downscale,
               cfg.target_fps, cfg.saturation);
    } else {
        d5_log("config %s missing or invalid, using defaults", paths->config);
        d5_config_save(paths->config, &cfg);
    }

    D5Applied applied;
    memset(&applied, 0, sizeof(applied));
    if (d5_applied_load(paths->applied, &applied)) {
        d5_log("restoring stale state from %s before starting", paths->applied);
        d5_restore_profile(&applied);
        d5_applied_clear(paths->applied);
    }

    long long config_stamp = mtime_ms(paths->config);
    long long next_signals = 0;
    long long next_screen = 0;
    long long next_status = 0;
    int screen_on = 1;
    D5Decision decision;
    memset(&decision, 0, sizeof(decision));
    decision.downscale = cfg.downscale;
    decision.fps = cfg.target_fps;

    while (!g_stop) {
        long long now = d5_now_ms();
        long long stamp = mtime_ms(paths->config);
        if (g_reload || stamp != config_stamp) {
            g_reload = 0;
            config_stamp = stamp;
            D5Config fresh;
            d5_config_defaults(&fresh);
            if (d5_config_load(paths->config, &fresh)) {
                cfg = fresh;
                d5_log("config reloaded: enabled=%d ai=%d games=%d",
                       cfg.enabled, cfg.ai, cfg.game_count);
            }
        }

        if (!cfg.enabled) {
            if (applied.active) {
                d5_restore_profile(&applied);
                d5_applied_clear(paths->applied);
            }
            if (now >= next_status) {
                d5_write_status(paths->status, 0, NULL, NULL);
                next_status = now + 2000;
            }
            sleep_ms(1200);
            continue;
        }

        if (now >= next_screen) {
            screen_on = d5_screen_on();
            next_screen = now + 5000;
        }
        if (!screen_on) {
            if (applied.active) {
                d5_restore_profile(&applied);
                d5_applied_clear(paths->applied);
            }
            sleep_ms(2000);
            continue;
        }

        char top[D5_PKG_LEN];
        if (!d5_foreground_package(top, sizeof(top))) {
            sleep_ms(1200);
            continue;
        }

        int eligible = d5_config_has_game(&cfg, top) && d5_is_user_package(top);
        if (!eligible) {
            if (applied.active) {
                d5_restore_profile(&applied);
                d5_applied_clear(paths->applied);
                d5_log("left %s", top);
            }
            if (now >= next_status) {
                d5_write_status(paths->status, 1, NULL, NULL);
                next_status = now + 2000;
            }
            sleep_ms(1000);
            continue;
        }

        int entering = !applied.active || strcmp(applied.pkg, top) != 0;
        if (entering) {
            if (applied.active) {
                d5_restore_profile(&applied);
                d5_applied_clear(paths->applied);
            }
            D5Signals sig;
            gather_signals(&sig);
            d5_tune(&sig, &cfg, &decision);
            d5_apply_profile(&cfg, top, &decision, &applied);
            d5_applied_save(paths->applied, &applied);
            d5_log("boost %s: scale %.2f fps %d (thermal %.2f gpu %.2f perf %.2f)",
                   top, (double)decision.downscale, decision.fps, (double)sig.thermal,
                   (double)sig.gpu_busy, (double)sig.perf_class);
            next_signals = now + 4000;
            next_status = 0;
        } else if (now >= next_signals) {
            D5Signals sig;
            gather_signals(&sig);
            D5Decision next;
            d5_tune(&sig, &cfg, &next);
            if (fabsf(next.downscale - applied.downscale) >= (D5_SCALE_STEP / 2.0f) ||
                next.fps != applied.fps) {
                d5_log("retune %s: %.2f -> %.2f, fps %d -> %d", top,
                       (double)applied.downscale, (double)next.downscale, applied.fps,
                       next.fps);
                d5_apply_profile(&cfg, top, &next, &applied);
                d5_applied_save(paths->applied, &applied);
            }
            decision = next;
            next_signals = now + 4000;
            next_status = 0;
        }

        if (now >= next_status) {
            d5_write_status(paths->status, 1, &applied, &decision);
            next_status = now + 2000;
        }
        sleep_ms(applied.active ? 700 : 1000);
    }

    d5_log("stopping");
    if (applied.active) {
        d5_restore_profile(&applied);
        d5_applied_clear(paths->applied);
    }
    d5_write_status(paths->status, 0, NULL, NULL);
    return 0;
}

static int run_once(const Paths *paths) {
    D5Config cfg;
    d5_config_defaults(&cfg);
    d5_config_load(paths->config, &cfg);

    char top[D5_PKG_LEN] = "";
    if (paths->once_pkg) {
        snprintf(top, sizeof(top), "%s", paths->once_pkg);
    } else if (!d5_foreground_package(top, sizeof(top))) {
        printf("{\"ok\": false, \"reason\": \"no foreground package\"}\n");
        return 1;
    }
    if (!d5_valid_package(top)) {
        printf("{\"ok\": false, \"reason\": \"invalid package\"}\n");
        return 1;
    }

    D5Signals sig;
    gather_signals(&sig);
    D5Decision decision;
    d5_tune(&sig, &cfg, &decision);

    int selected = d5_config_has_game(&cfg, top);
    int user_app = d5_is_user_package(top);
    printf("{\"package\": \"%s\", \"selected\": %s, \"user_app\": %s, "
           "\"enabled\": %s, \"downscale\": %.2f, \"fps\": %d, "
           "\"thermal\": %.3f, \"gpu\": %.3f, \"perf\": %.3f, \"battery\": %.3f}\n",
           top, selected ? "true" : "false", user_app ? "true" : "false",
           cfg.enabled ? "true" : "false", (double)decision.downscale,
           decision.fps, (double)sig.thermal, (double)sig.gpu_busy,
           (double)sig.perf_class, (double)sig.battery);

    if (!cfg.enabled || !selected || !user_app) return 0;

    D5Applied applied;
    memset(&applied, 0, sizeof(applied));
    d5_apply_profile(&cfg, top, &decision, &applied);
    d5_applied_save(paths->applied, &applied);
    if (!paths->keep) {
        d5_restore_profile(&applied);
        d5_applied_clear(paths->applied);
    }
    return 0;
}

static void usage(void) {
    printf("dlss5d %s -- per-game render scaling, GPU boost and colour\n"
           "usage: dlss5d [--daemon|--once|--status|--selftest] [options]\n"
           "  --config PATH     config JSON (default %s)\n"
           "  --status-file P   status JSON (default %s)\n"
           "  --applied-file P  recovery state (default %s)\n"
           "  --package NAME    with --once: evaluate NAME instead of the foreground app\n"
           "  --keep            with --once: leave the profile applied\n"
           "  --dry-run         print the commands instead of running them\n"
           "  --verbose         log every command\n"
           "  --version         print the version\n",
           D5_VERSION, D5_DEFAULT_CONFIG, D5_DEFAULT_STATUS, D5_DEFAULT_APPLIED);
}

int main(int argc, char **argv) {
    Paths paths = {D5_DEFAULT_CONFIG, D5_DEFAULT_STATUS, D5_DEFAULT_APPLIED, NULL, 0};
    enum { MODE_DAEMON, MODE_ONCE, MODE_STATUS, MODE_SELFTEST, MODE_VERSION } mode =
        MODE_DAEMON;

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (strcmp(arg, "--daemon") == 0) mode = MODE_DAEMON;
        else if (strcmp(arg, "--once") == 0) mode = MODE_ONCE;
        else if (strcmp(arg, "--status") == 0) mode = MODE_STATUS;
        else if (strcmp(arg, "--selftest") == 0) mode = MODE_SELFTEST;
        else if (strcmp(arg, "--version") == 0) mode = MODE_VERSION;
        else if (strcmp(arg, "--verbose") == 0) d5_set_verbose(1);
        else if (strcmp(arg, "--dry-run") == 0) d5_set_dry_run(1);
        else if (strcmp(arg, "--keep") == 0) paths.keep = 1;
        else if (strcmp(arg, "--config") == 0 && i + 1 < argc) paths.config = argv[++i];
        else if (strcmp(arg, "--status-file") == 0 && i + 1 < argc) paths.status = argv[++i];
        else if (strcmp(arg, "--applied-file") == 0 && i + 1 < argc) paths.applied = argv[++i];
        else if (strcmp(arg, "--package") == 0 && i + 1 < argc) paths.once_pkg = argv[++i];
        else if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            usage();
            return 0;
        } else {
            fprintf(stderr, "unknown option: %s\n", arg);
            usage();
            return 2;
        }
    }

    if (mode == MODE_VERSION) {
        printf("dlss5d %s (model inputs=%d hidden=%d)\n", D5_VERSION, 5, 8);
        return 0;
    }
    if (mode == MODE_SELFTEST) return d5_selftest() == 0 ? 0 : 1;
    if (mode == MODE_STATUS) {
        char buf[2048];
        if (d5_read_file(paths.status, buf, sizeof(buf))) {
            printf("%s\n", buf);
            return 0;
        }
        printf("{\"running\": false}\n");
        return 1;
    }
    if (mode == MODE_ONCE) {
        d5_log("single evaluation");
        return run_once(&paths);
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_stop;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sa.sa_handler = on_reload;
    sigaction(SIGHUP, &sa, NULL);

    d5_log("starting, config %s", paths.config);
    int rc = run_daemon(&paths);
    d5_log("exit %d", rc);
    return rc;
}
