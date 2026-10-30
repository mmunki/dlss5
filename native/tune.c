/*
 * The adaptive controller.  A small fully connected network is fitted
 * offline by tools/train_model.py on a synthetic quality/latency objective;
 * the weights end up in build/model.h.  At runtime the network is blended
 * with an explicit physical prior so the result is always monotonic in the
 * signals (hotter or busier -> lower render scale, quicker SoC -> higher).
 */
#include "dlss5.h"

#include <math.h>

#include "model.h"

static float sigmoidf_(float x) { return 1.0f / (1.0f + expf(-x)); }

static int snap_fps(int fps, int cap) {
    static const int ladder[] = {24, 30, 45, 48, 60, 90, 120, 144, 165, 240};
    if (cap <= 0) return 0;
    int best = 24;
    for (size_t i = 0; i < sizeof(ladder) / sizeof(ladder[0]); i++) {
        if (ladder[i] <= cap && ladder[i] > best) best = ladder[i];
        if (ladder[i] >= fps) {
            return ladder[i] <= cap ? ladder[i] : best;
        }
    }
    return best;
}

void d5_tune(const D5Signals *sig, const D5Config *cfg, D5Decision *out) {
    if (!sig || !cfg || !out) return;

    float x[5];
    x[0] = d5_clampf(sig->thermal, 0.0f, 1.0f);
    x[1] = d5_clampf(sig->gpu_busy, 0.0f, 1.0f);
    x[2] = d5_clampf(sig->perf_class, 0.0f, 1.0f);
    x[3] = d5_clampf(1.0f - cfg->downscale, 0.0f, 1.0f);
    x[4] = d5_clampf(sig->battery, 0.0f, 1.0f);

    float hidden[D5_HIDDEN];
    for (int j = 0; j < D5_HIDDEN; j++) {
        float sum = D5_B1[j];
        for (int i = 0; i < D5_INPUTS; i++) sum += D5_W1[j][i] * x[i];
        hidden[j] = tanhf(sum);
    }
    float o[2];
    for (int k = 0; k < 2; k++) {
        float sum = D5_B2[k];
        for (int j = 0; j < D5_HIDDEN; j++) sum += D5_W2[k][j] * hidden[j];
        o[k] = sigmoidf_(sum);
    }

    out->scale_hint = o[0];
    out->fps_hint = o[1];

    /* Network output mapped onto the render-scale ladder. */
    float mlp_scale = D5_SCALE_MIN + o[0] * (1.0f - D5_SCALE_MIN);
    /* Never let the model override physics or the SoC class. */
    float prior = 0.55f + 0.30f * sig->perf_class + 0.20f * sig->battery
                  - 0.45f * sig->thermal - 0.30f * sig->gpu_busy;
    prior = d5_clampf(prior, 0.35f, 1.0f);

    float quality = 0.6f * mlp_scale + 0.4f * prior;

    float floor_scale = cfg->downscale;
    float scale = cfg->ai ? d5_clampf(quality, floor_scale, 1.0f) : floor_scale;
    if (scale >= 0.965f) {
        scale = 1.0f;
    } else {
        scale = d5_snap_scale(scale);
        if (scale < floor_scale) scale = d5_snap_scale(floor_scale);
    }
    out->downscale = scale;

    int fps = cfg->target_fps;
    if (fps > 0) {
        int stressed = sig->thermal > 0.45f || sig->gpu_busy > 0.85f;
        if (cfg->ai && stressed) {
            float fraction = 0.5f + 0.5f * o[1];
            fps = snap_fps((int)lroundf((float)fps * fraction), fps);
        }
    }
    out->fps = fps;
}
