/*
 * ThumbyCue — procedural SFX by FILTERED-NOISE bursts.
 *
 * The 2D game's recordings are almost entirely NOISE (FFT tonal ratio ~0.01–
 * 0.03; see tools/analyze_sfx.py) with broad spectral colouring — a hard
 * collision is a brief broadband CLICK, not a ringing tone. So each effect is
 * a short white-noise burst (the impact transient) shaped by one or two
 * resonant band-pass filters tuned to the recording's spectral peaks, plus a
 * very fast dry "snap" for the attack. NOT a sum of sine modes (that rang like
 * bells). No samples stored.
 *
 * Per voice: noise × decaying excitation → 1–2 RBJ band-pass biquads. Inner
 * loop is a handful of multiply-adds — cheap enough for the device.
 */
#include "cue_audio.h"
#include <math.h>
#include <string.h>

#define RATE   22050.0f
#define NVOICE 8
#define NBP    2
#define TWO_PI 6.2831853072f

/* ---- sound models ---------------------------------------------------- */
typedef struct {
    int   nbp;
    float f[NBP];        /* band-pass centre (Hz) */
    float q[NBP];        /* resonance — keep modest so it stays noisy */
    float g[NBP];        /* relative gain */
    float decay;         /* burst length, time constant (s) */
    float click;         /* dry broadband snap level (very fast attack) */
    float out_gain;      /* overall level trim */
    float lp;            /* one-pole LP on the noise (0..1): spectral tilt /
                          * brightness. low = dark (pot), high = bright (strike) */
} SfxModel;

/* STRIKE — cue tip: bright, short, noisy click (centroid ~5.7 kHz, ~32 ms). */
static const SfxModel M_STRIKE  = { 2, {4200, 7200}, {1.6f, 2.0f}, {1.0f, 0.7f},
    0.028f, 0.6f, 1.5f, 0.85f };
/* CLACK — ball on ball: broad band ~2.3–2.9 kHz, short tock (~55 ms). */
static const SfxModel M_CLACK   = { 2, {2150, 2700}, {5.5f, 5.5f}, {1.0f, 0.7f},
    0.045f, 0.18f, 1.8f, 0.45f };
static const SfxModel M_CLACK2  = { 2, {2500, 1850}, {5.0f, 4.0f}, {1.0f, 0.7f},
    0.050f, 0.20f, 1.8f, 0.55f };
/* CUSHION — duller, lower, rubbery thud. */
static const SfxModel M_CUSHION = { 2, {750, 1300}, {3.0f, 2.5f}, {1.0f, 0.5f},
    0.045f, 0.12f, 1.7f, 0.22f };
/* POT (soft) — low thunk ~250–520 Hz, longer (~150 ms). Dark. */
static const SfxModel M_POT_SOFT= { 2, {260, 500}, {3.5f, 3.0f}, {1.0f, 0.6f},
    0.150f, 0.04f, 2.1f, 0.05f };
/* POT (hard) — faster, brighter drop with a longer mid body (~220 ms). */
static const SfxModel M_POT_HARD= { 2, {620, 1050}, {4.0f, 3.0f}, {1.0f, 0.55f},
    0.200f, 0.10f, 1.9f, 0.16f };
/* UI — a clean short blip (this one IS tonal, on purpose). */
static const SfxModel M_UI      = { 1, {1000, 0}, {30.0f, 0}, {1.0f, 0},
    0.060f, 0.0f, 1.2f, 1.0f };

/* ---- voice ----------------------------------------------------------- */
typedef struct {
    /* RBJ band-pass biquads, transposed direct form II */
    float b0[NBP], a1[NBP], a2[NBP], z1[NBP], z2[NBP], g[NBP];
    int   nbp;
    float ex, ex_decay;        /* noise excitation envelope (master length) */
    float ck, ck_decay;        /* dry snap envelope (fast attack transient) */
    float lp, lpy;             /* noise brightness one-pole LP */
    float out_gain;
    uint32_t rng;
    int on;
} Voice;

static Voice s_v[NVOICE];
static float s_gain = 0.7f;
static uint32_t s_rng = 0x1234567u;
static int s_clack_alt = 0;

void cue_audio_init(void) { memset(s_v, 0, sizeof s_v); }
void cue_audio_set_volume(int vol) {
    if (vol < 0) vol = 0; if (vol > 20) vol = 20;
    s_gain = (float)vol / 20.0f * 0.9f;
}
void cue_audio_tick(float dt) { (void)dt; }

static Voice *alloc_voice(void) {
    Voice *q = &s_v[0]; float qa = 1e9f;
    for (int i = 0; i < NVOICE; i++) {
        if (!s_v[i].on) return &s_v[i];
        if (s_v[i].ex < qa) { qa = s_v[i].ex; q = &s_v[i]; }
    }
    return q;
}

static float decay_of(float tau) {
    if (tau < 1e-4f) return 0.0f;
    return expf(-1.0f / (tau * RATE));
}

static void trigger(const SfxModel *m, float level) {
    Voice *v = alloc_voice();
    memset(v, 0, sizeof *v);
    v->on = 1;
    v->rng = (s_rng += 0x9E3779B9u) | 1u;
    int nb = m->nbp; if (nb > NBP) nb = NBP;
    v->nbp = nb;
    for (int i = 0; i < nb; i++) {
        float w0 = TWO_PI * m->f[i] / RATE;
        float alpha = sinf(w0) / (2.0f * m->q[i]);
        float a0 = 1.0f + alpha;
        /* band-pass (constant 0 dB peak): b0=alpha, b1=0, b2=-alpha */
        v->b0[i] = alpha / a0;
        v->a1[i] = (-2.0f * cosf(w0)) / a0;
        v->a2[i] = (1.0f - alpha) / a0;
        v->g[i]  = m->g[i];
    }
    v->ex = level;
    v->ex_decay = decay_of(m->decay);
    v->ck = m->click * level;
    v->ck_decay = decay_of(0.004f);     /* ~4 ms attack snap */
    v->lp = m->lp; v->lpy = 0.0f;
    v->out_gain = m->out_gain;
}

void cue_audio_sfx(int which, float in) {
    if (in < 0) in = 0; if (in > 1) in = 1;
    float level = 0.35f + 0.65f * in;
    switch (which) {
    case CUE_SFX_STRIKE:  trigger(&M_STRIKE,  level); break;
    case CUE_SFX_CLACK:
        trigger((s_clack_alt ^= 1) ? &M_CLACK : &M_CLACK2, level); break;
    case CUE_SFX_CUSHION: trigger(&M_CUSHION, level); break;
    case CUE_SFX_POT:     trigger(in > 0.5f ? &M_POT_HARD : &M_POT_SOFT, level); break;
    default:              trigger(&M_UI, 0.8f); break;
    }
}

void cue_audio_render(int16_t *out, int n) {
    for (int s = 0; s < n; s++) {
        float acc = 0.0f;
        for (int i = 0; i < NVOICE; i++) {
            Voice *v = &s_v[i];
            if (!v->on) continue;
            v->rng ^= v->rng << 13; v->rng ^= v->rng >> 17; v->rng ^= v->rng << 5;
            float white = ((int32_t)(v->rng & 0xFFFF) - 32768) * (1.0f/32768.0f);
            v->lpy += v->lp * (white - v->lpy);      /* spectral tilt */
            white = v->lpy;
            float x = white * v->ex;                 /* excitation burst */
            float sm = v->ck * white;                /* dry attack snap */
            for (int k = 0; k < v->nbp; k++) {       /* TDF2 band-pass */
                float y = v->b0[k] * x + v->z1[k];
                v->z1[k] = -v->a1[k] * y + v->z2[k];
                v->z2[k] = -v->b0[k] * x - v->a2[k] * y;
                sm += v->g[k] * y;
            }
            acc += sm * v->out_gain;
            v->ex *= v->ex_decay;
            v->ck *= v->ck_decay;
            if (v->ex < 0.0009f) v->on = 0;
        }
        acc *= s_gain;
        if (acc > 1.0f) acc = 1.0f; if (acc < -1.0f) acc = -1.0f;
        out[s] = (int16_t)(acc * 30000.0f);
    }
}
