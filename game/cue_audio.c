/*
 * ThumbyCue — procedural SFX by MODAL SYNTHESIS.
 *
 * Each impact sound is reproduced as a small sum of exponentially-decaying
 * sinusoids (the object's resonant modes) plus a short filtered-noise onset
 * transient — which is physically what a hard collision sounds like. The
 * modal frequencies / decay times below were measured (FFT + envelope fit)
 * from the 2D game's recordings: cueshot, ballhit/ballhit2, pot/hardpot/
 * softpot (see tools/analyze_sfx.py output). No samples are stored.
 *
 * Each mode is a 2-pole resonator  y[n] = c1·y[n-1] − c2·y[n-2]  with
 * c1 = 2r·cos w, c2 = r², excited by an impulse — so the inner loop is just
 * multiply–adds (no per-sample sinf). Mixed mono at 22050 Hz.
 */
#include "cue_audio.h"
#include <math.h>
#include <string.h>

#define RATE   22050.0f
#define NVOICE 8
#define NMODE  4
#define TWO_PI 6.2831853072f

/* ---- sound models (measured) ---------------------------------------- */
typedef struct {
    int   nmodes;
    float freq[NMODE];     /* Hz */
    float amp [NMODE];     /* relative */
    float mode_tau;        /* modal decay time constant (s) */
    float noise_amp;       /* onset noise burst level */
    float noise_tau;       /* noise decay (s) */
    float noise_lp;        /* one-pole LP coeff 0..1 (higher = brighter) */
} SfxModel;

/* STRIKE — cue tip on ball: bright, noise-dominated, fast (~32 ms). */
static const SfxModel M_STRIKE = {
    3, {3400, 5700, 7950, 0}, {0.45f, 0.40f, 0.32f, 0}, 0.030f,
    1.00f, 0.030f, 0.65f };
/* CLACK — ball on ball: tight modal cluster ~2.3–3.5 kHz, ~95 ms. */
static const SfxModel M_CLACK = {
    4, {2293, 2778, 2969, 3122}, {1.00f, 0.95f, 0.97f, 0.74f}, 0.090f,
    0.35f, 0.008f, 0.55f };
/* CLACK variant (ballhit2) — slightly higher/longer, for variety. */
static const SfxModel M_CLACK2 = {
    4, {2759, 2920, 3219, 1801}, {1.00f, 0.81f, 0.85f, 0.68f}, 0.105f,
    0.32f, 0.008f, 0.55f };
/* CUSHION — no recording existed; modelled as a duller, lower, rubbery
 * thud (damped, darker noise) — a soft cousin of the clack. */
static const SfxModel M_CUSHION = {
    3, {900, 1300, 600, 0}, {1.00f, 0.55f, 0.50f, 0}, 0.050f,
    0.50f, 0.012f, 0.25f };
/* POT (soft) — ball settling: low thunk 231/514 Hz + a little ring (~180 ms). */
static const SfxModel M_POT_SOFT = {
    4, {231, 514, 864, 1136}, {1.00f, 0.85f, 0.35f, 0.20f}, 0.180f,
    0.30f, 0.020f, 0.30f };
/* POT (hard) — faster, brighter drop with a longer mid ring (~350 ms). */
static const SfxModel M_POT_HARD = {
    4, {899, 1104, 630, 1930}, {1.00f, 0.50f, 0.45f, 0.30f}, 0.350f,
    0.45f, 0.016f, 0.45f };
/* UI — clean two-tone blip. */
static const SfxModel M_UI = {
    2, {880, 1320, 0, 0}, {1.00f, 0.45f, 0, 0}, 0.055f,
    0.0f, 0.0f, 0.0f };

/* ---- voice ----------------------------------------------------------- */
typedef struct {
    int   on;
    int   nmodes;
    float c1[NMODE], c2[NMODE], y1[NMODE], y2[NMODE];
    float namp, ndecay, nlp, nz;   /* noise burst */
    float life, life_decay;        /* envelope estimate for voice retire */
    uint32_t rng;
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
        if (s_v[i].life < qa) { qa = s_v[i].life; q = &s_v[i]; }
    }
    return q;   /* steal the quietest */
}

/* per-sample decay multiplier for a time constant tau (seconds) */
static float decay_of(float tau) {
    if (tau < 1e-4f) return 0.0f;
    return expf(-1.0f / (tau * RATE));
}

static void trigger(const SfxModel *m, float level) {
    Voice *v = alloc_voice();
    memset(v, 0, sizeof *v);
    v->on = 1;
    v->rng = (s_rng += 0x9E3779B9u) | 1u;
    float rmode = decay_of(m->mode_tau);
    int nm = m->nmodes; if (nm > NMODE) nm = NMODE;
    v->nmodes = nm;
    float slowest = rmode;
    for (int i = 0; i < nm; i++) {
        float w = TWO_PI * m->freq[i] / RATE;
        v->c1[i] = 2.0f * rmode * cosf(w);
        v->c2[i] = rmode * rmode;
        /* impulse excitation → y[n] ≈ A·r^n·sin(wn) */
        v->y1[i] = m->amp[i] * level * sinf(w);
        v->y2[i] = 0.0f;
    }
    v->namp   = m->noise_amp * level;
    v->ndecay = decay_of(m->noise_tau);
    v->nlp    = m->noise_lp;
    if (v->ndecay > slowest) slowest = v->ndecay;
    v->life = 1.0f;
    v->life_decay = slowest;
}

void cue_audio_sfx(int which, float in) {
    if (in < 0) in = 0; if (in > 1) in = 1;
    float level = 0.35f + 0.65f * in;
    switch (which) {
    case CUE_SFX_STRIKE:  trigger(&M_STRIKE,  level); break;
    case CUE_SFX_CLACK:
        trigger((s_clack_alt ^= 1) ? &M_CLACK : &M_CLACK2, level); break;
    case CUE_SFX_CUSHION: trigger(&M_CUSHION, level); break;
    case CUE_SFX_POT:     /* blend soft↔hard model by how hard the drop was */
        trigger(in > 0.5f ? &M_POT_HARD : &M_POT_SOFT, level); break;
    default:              trigger(&M_UI, 0.8f); break;
    }
}

void cue_audio_render(int16_t *out, int n) {
    for (int s = 0; s < n; s++) {
        float acc = 0.0f;
        for (int i = 0; i < NVOICE; i++) {
            Voice *v = &s_v[i];
            if (!v->on) continue;
            float sm = 0.0f;
            for (int k = 0; k < v->nmodes; k++) {
                float y = v->c1[k] * v->y1[k] - v->c2[k] * v->y2[k];
                v->y2[k] = v->y1[k]; v->y1[k] = y;
                sm += y;
            }
            if (v->namp > 0.0005f) {
                v->rng ^= v->rng << 13; v->rng ^= v->rng >> 17; v->rng ^= v->rng << 5;
                float white = ((int32_t)(v->rng & 0xFFFF) - 32768) * (1.0f/32768.0f);
                v->nz += v->nlp * (white - v->nz);   /* one-pole LP for colour */
                sm += v->namp * v->nz;
                v->namp *= v->ndecay;
            }
            acc += sm;
            v->life *= v->life_decay;
            if (v->life < 0.0008f) v->on = 0;
        }
        acc *= s_gain;
        if (acc > 1.0f) acc = 1.0f; if (acc < -1.0f) acc = -1.0f;
        out[s] = (int16_t)(acc * 30000.0f);
    }
}
