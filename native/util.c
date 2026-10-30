#include "dlss5.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fnmatch.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>

static int g_verbose = 0;
static int g_dry_run = 0;

void d5_set_verbose(int verbose) { g_verbose = verbose; }
void d5_set_dry_run(int dry_run) { g_dry_run = dry_run; }
int d5_dry_run(void) { return g_dry_run; }

void d5_log(const char *fmt, ...) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm tm;
    localtime_r(&tv.tv_sec, &tm);
    fprintf(stderr, "%04d-%02d-%02d %02d:%02d:%02d.%03d dlss5d: ",
            tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min,
            tm.tm_sec, (int)(tv.tv_usec / 1000));
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

float d5_clampf(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

int d5_valid_package(const char *pkg) {
    if (!pkg || !*pkg) return 0;
    size_t len = strlen(pkg);
    if (len >= D5_PKG_LEN) return 0;
    int dots = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)pkg[i];
        if (c == '.') {
            if (i == 0 || i + 1 == len || pkg[i - 1] == '.') return 0;
            dots++;
            continue;
        }
        if (!isalnum(c) && c != '_') return 0;
    }
    return dots >= 1;
}

int d5_read_file(const char *path, char *buf, size_t len) {
    if (!path || !buf || len == 0) return 0;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    size_t n = fread(buf, 1, len - 1, f);
    fclose(f);
    buf[n] = '\0';
    /* Trim a trailing newline, sysfs always has one. */
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = '\0';
    return 1;
}

int d5_read_number(const char *path, double *out) {
    char buf[128];
    if (!d5_read_file(path, buf, sizeof(buf))) return 0;
    char *end = NULL;
    double v = strtod(buf, &end);
    if (end == buf) return 0;
    if (out) *out = v;
    return 1;
}

int d5_write_file(const char *path, const char *data) {
    char tmp[D5_PATH_LEN + 32];
    if (strlen(path) + 5 >= sizeof(tmp)) return 0;
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return 0;
    size_t len = strlen(data);
    int ok = fwrite(data, 1, len, f) == len;
    ok = fflush(f) == 0 && ok;
    fclose(f);
    if (!ok) {
        unlink(tmp);
        return 0;
    }
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return 0;
    }
    chmod(path, 0600);
    return 1;
}

long long d5_now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

/* One daemon per device.  The boot watchdog and the control app both start
 * the daemon, so a non-blocking flock is taken before anything is applied.
 * The lock is released by the kernel when the process dies, so a crash never
 * leaves a stale lock behind.
 *
 * Returns a file descriptor, -1 when another process holds the lock, or -2
 * when the lock file itself cannot be created. */
int d5_lock_take(const char *path) {
    int fd = open(path, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (fd < 0) return -2;
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        int err = errno;
        close(fd);
        return (err == EWOULDBLOCK || err == EAGAIN) ? -1 : -2;
    }
    char pid[32];
    int n = snprintf(pid, sizeof(pid), "%d\n", (int)getpid());
    if (lseek(fd, 0, SEEK_SET) >= 0) {
        ssize_t written = write(fd, pid, (size_t)n);
        (void)written;
    }
    return fd;
}

void d5_lock_release(int fd) {
    if (fd < 0) return;
    flock(fd, LOCK_UN);
    close(fd);
}

int d5_cmd(const char *cmd, char *out, size_t out_len) {
    if (out && out_len) out[0] = '\0';
    if (g_dry_run) {
        printf("%s\n", cmd);
        fflush(stdout);
        return 0;
    }
    if (g_verbose) d5_log("exec: %s", cmd);
    FILE *p = popen(cmd, "r");
    if (!p) {
        d5_log("cannot run: %s", cmd);
        return -1;
    }
    size_t used = 0;
    char chunk[512];
    while (fgets(chunk, sizeof(chunk), p)) {
        if (out && used + 1 < out_len) {
            size_t n = strlen(chunk);
            if (n > out_len - 1 - used) n = out_len - 1 - used;
            memcpy(out + used, chunk, n);
            used += n;
            out[used] = '\0';
        }
    }
    int status = pclose(p);
    if (status == -1) return -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;
}

int d5_build_game_set(char *buf, size_t len, float downscale, int fps,
                      const char *pkg) {
    if (!buf || len == 0 || !d5_valid_package(pkg)) return 0;
    if (downscale < 0.975f) {
        snprintf(buf, len, "cmd game set --downscale %.2f --fps %d %s",
                 (double)downscale, fps, pkg);
    } else {
        snprintf(buf, len, "cmd game set --downscale disable --fps %d %s", fps, pkg);
    }
    return 1;
}

typedef struct {
    char (*paths)[D5_PATH_LEN];
    int max;
    int count;
} GlobState;

static int match_component(const char *name, const char *component, size_t len) {
    char pattern[256];
    if (len == 0 || len >= sizeof(pattern)) return 0;
    memcpy(pattern, component, len);
    pattern[len] = '\0';
    return fnmatch(pattern, name, 0) == 0;
}

/* bionic has no glob(3), so the few patterns the daemon needs are expanded
 * here: one wildcard component per directory level, fnmatch per entry. */
static void glob_walk(const char *pattern, GlobState *state) {
    if (state->count >= state->max) return;
    const char *star = strchr(pattern, '*');
    if (!star) {
        if (access(pattern, F_OK) == 0) {
            snprintf(state->paths[state->count], D5_PATH_LEN, "%s", pattern);
            state->count++;
        }
        return;
    }
    const char *slash = star;
    while (slash > pattern && *slash != '/') slash--;
    char dir[D5_PATH_LEN];
    size_t dir_len = (size_t)(slash - pattern);
    if (dir_len == 0) {
        snprintf(dir, sizeof(dir), "/");
    } else {
        if (dir_len >= sizeof(dir)) return;
        memcpy(dir, pattern, dir_len);
        dir[dir_len] = '\0';
    }
    const char *component = slash + 1;
    const char *rest = strchr(component, '/');
    size_t component_len = rest ? (size_t)(rest - component) : strlen(component);

    DIR *handle = opendir(dir);
    if (!handle) return;
    struct dirent *entry;
    while ((entry = readdir(handle)) != NULL && state->count < state->max) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        if (!match_component(entry->d_name, component, component_len)) continue;
        char next[D5_PATH_LEN];
        size_t used = 0;
        if (strcmp(dir, "/") != 0) {
            size_t dir_len_used = strlen(dir);
            if (dir_len_used >= sizeof(next)) continue;
            memcpy(next, dir, dir_len_used);
            used = dir_len_used;
        }
        if (used + 1 + strlen(entry->d_name) >= sizeof(next)) continue;
        next[used++] = '/';
        size_t name_len = strlen(entry->d_name);
        memcpy(next + used, entry->d_name, name_len);
        used += name_len;
        if (rest) {
            size_t rest_len = strlen(rest);
            if (used + rest_len >= sizeof(next)) continue;
            memcpy(next + used, rest, rest_len);
            used += rest_len;
        }
        next[used] = '\0';
        glob_walk(next, state);
    }
    closedir(handle);
}

int d5_glob_paths(const char *pattern, char paths[][D5_PATH_LEN], int max) {
    GlobState state = {paths, max, 0};
    glob_walk(pattern, &state);
    return state.count;
}

float d5_snap_scale(float scale) {
    if (scale >= 0.975f) return 1.0f;
    if (scale < D5_SCALE_MIN) scale = D5_SCALE_MIN;
    if (scale > D5_SCALE_MAX) scale = D5_SCALE_MAX;
    float steps = roundf((scale - D5_SCALE_MIN) / D5_SCALE_STEP);
    float snapped = D5_SCALE_MIN + steps * D5_SCALE_STEP;
    if (snapped < D5_SCALE_MIN) snapped = D5_SCALE_MIN;
    if (snapped > D5_SCALE_MAX) snapped = D5_SCALE_MAX;
    return snapped;
}
