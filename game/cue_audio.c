/*
 * ThumbyCue — procedural SFX. A small voice pool of decaying sine + noise
 * bursts mixed at 22050 Hz. No assets, tiny RAM.
 */
#include "cue_audio.h"
#include <math.h>
#include <string.h>

#define RATE 22050.0f
#define NVOICE 8

typedef struct {
    int   on;
    float phase, dphase;   /* sine */
    float amp, decay;      /* exponential envelope */
    float noise;           /* 0..1 mix of noise vs tone */
    float sweep;           /* per-sample dphase multiply (pitch slide) */
    uint32_t rng;
} Voice;

static Voice s_v[NVOICE];
static float s_gain = 0.7f;
static uint32_t s_rng = 0x1234567u;

void cue_audio_init(void) { memset(s_v, 0, sizeof s_v); }
void cue_audio_set_volume(int vol) {
    if (vol < 0) vol = 0; if (vol > 20) vol = 20;
    s_gain = (float)vol / 20.0f * 0.9f;
}
void cue_audio_tick(float dt) { (void)dt; }

static Voice *alloc_voice(void) {
    Voice *q = NULL; float qa = 1e9f;
    for (int i = 0; i < NVOICE; i++) {
        if (!s_v[i].on) return &s_v[i];
        if (s_v[i].amp < qa) { qa = s_v[i].amp; q = &s_v[i]; }
    }
    return q;   /* steal the quietest */
}

void cue_audio_sfx(int which, float in) {
    if (in < 0) in = 0; if (in > 1) in = 1;
    Voice *v = alloc_voice();
    v->on = 1; v->phase = 0; v->sweep = 1.0f; v->rng = (s_rng += 0x9E3779B9u) | 1u;
    switch (which) {
    case CUE_SFX_STRIKE:   /* soft low thump + click */
        v->dphase = 180.0f * 2*3.14159265f / RATE;
        v->amp = 0.5f + 0.4f*in; v->decay = 0.9988f; v->noise = 0.45f; break;
    case CUE_SFX_CLACK:    /* bright ball-on-ball click */
        v->dphase = (1400.0f + 800.0f*in) * 2*3.14159265f / RATE;
        v->amp = 0.35f + 0.5f*in; v->decay = 0.9975f; v->noise = 0.35f; break;
    case CUE_SFX_CUSHION:  /* dull rubber thud */
        v->dphase = 240.0f * 2*3.14159265f / RATE;
        v->amp = 0.3f + 0.3f*in; v->decay = 0.9980f; v->noise = 0.55f; break;
    case CUE_SFX_POT:      /* descending drop into the pocket */
        v->dphase = 520.0f * 2*3.14159265f / RATE;
        v->amp = 0.5f; v->decay = 0.9990f; v->noise = 0.15f; v->sweep = 0.99992f; break;
    default:               /* UI blip */
        v->dphase = 880.0f * 2*3.14159265f / RATE;
        v->amp = 0.25f; v->decay = 0.9965f; v->noise = 0.0f; break;
    }
}

void cue_audio_render(int16_t *out, int n) {
    for (int s = 0; s < n; s++) {
        float acc = 0.0f;
        for (int i = 0; i < NVOICE; i++) {
            Voice *v = &s_v[i];
            if (!v->on) continue;
            v->rng ^= v->rng << 13; v->rng ^= v->rng >> 17; v->rng ^= v->rng << 5;
            float nz = ((int32_t)(v->rng & 0xFFFF) - 32768) * (1.0f/32768.0f);
            float tone = sinf(v->phase);
            acc += v->amp * (tone * (1.0f - v->noise) + nz * v->noise);
            v->phase += v->dphase; if (v->phase > 6.2831853f) v->phase -= 6.2831853f;
            v->dphase *= v->sweep;
            v->amp *= v->decay;
            if (v->amp < 0.002f) v->on = 0;
        }
        acc *= s_gain;
        if (acc > 1.0f) acc = 1.0f; if (acc < -1.0f) acc = -1.0f;
        out[s] = (int16_t)(acc * 30000.0f);
    }
}
