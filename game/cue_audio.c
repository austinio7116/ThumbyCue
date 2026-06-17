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
 *
 * EXCEPTION — the cue STRIKE, ball-on-ball CLACK, rail CUSHION and POCKET are
 * real embedded SAMPLES from 2dpool (cue_cueshot/clack/cushion/pot/softpot/
 * hardpot_pcm.h). Most are decoded recordings; the cushion is a faithful render
 * of 2dpool's playRailCollision 3-layer synth. The pocket follows playPocket():
 * pool uses pot.mp3, snooker splits soft/hard at ~4 m/s (softpot/hardpot.mp3).
 * Sample voices play them back with per-hit amplitude and a tiny pitch wobble.
 * Only UI stays procedural synth.
 */
#include "cue_audio.h"
#include "cue_clack_pcm.h"
#include "cue_cueshot_pcm.h"    /* real 2dpool cue-tip strike sample */
#include "cue_cushion_pcm.h"    /* 2dpool playRailCollision render */
#include "cue_pot_pcm.h"        /* 2dpool pool pocket (pot.mp3) */
#include "cue_softpot_pcm.h"    /* 2dpool snooker soft pot (softpot.mp3) */
#include "cue_hardpot_pcm.h"    /* 2dpool snooker hard pot (hardpot.mp3) */
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

/* STRIKE, CLACK and CUSHION are now real embedded samples from 2dpool
 * (cue_cueshot_pcm.h, cue_clack_pcm.h, cue_cushion_pcm.h) rather than synth
 * models — the synth couldn't match the recordings' body/character. */
/* POT is now real 2dpool samples (cue_pot/softpot/hardpot_pcm.h). */
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
    /* sample-playback voice (clack): if smp != NULL the synth path is skipped */
    const int16_t *smp; int smp_len; float smp_pos, smp_rate, smp_gain;
    int on;
} Voice;

static Voice s_v[NVOICE];
static float s_gain = 0.7f;
static uint32_t s_rng = 0x1234567u;
static int s_snooker = 0;       /* picks the snooker pot samples over the pool one */

void cue_audio_init(void) { memset(s_v, 0, sizeof s_v); }
void cue_audio_set_snooker(int on) { s_snooker = on ? 1 : 0; }
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

/* Sample-playback voice (the real clack). rate>1 = higher/faster. */
static void trigger_sample(const int16_t *data, int len, float gain, float rate) {
    Voice *v = alloc_voice();
    memset(v, 0, sizeof *v);
    v->on = 1;
    v->smp = data; v->smp_len = len; v->smp_pos = 0.0f;
    v->smp_rate = rate; v->smp_gain = gain;
    v->out_gain = 1.0f;
}

void cue_audio_sfx(int which, float in) {
    if (in < 0) in = 0; if (in > 1) in = 1;
    float level = 0.35f + 0.65f * in;
    switch (which) {
    case CUE_SFX_STRIKE: {
        /* real 2dpool cue-tip sample; gain tracks power like 2dpool's
         * playCueStrike (volume = 0.4 + power*0.6). */
        float gain = (0.4f + 0.6f * in) * 0.9f;
        trigger_sample(cue_cueshot_pcm, CUE_CUESHOT_PCM_LEN, gain, 1.0f);
        break; }
    case CUE_SFX_CLACK: {
        /* real recorded clack; tiny per-hit pitch wobble (±2%) for variety —
         * NOT a real speed-up (natural pitch sounded best). */
        s_rng = s_rng * 1664525u + 1013904223u;
        float rate = 0.98f + 0.04f * ((s_rng >> 16 & 0xFF) * (1.0f / 255.0f));
        trigger_sample(cue_clack_pcm, CUE_CLACK_LEN, level * 1.25f, rate);
        break; }
    case CUE_SFX_CUSHION: {
        /* 2dpool playRailCollision render; gain scales with impact intensity,
         * tiny per-hit pitch wobble (±3%) so repeated rail hits vary. */
        s_rng = s_rng * 1664525u + 1013904223u;
        float rate = 0.97f + 0.06f * ((s_rng >> 16 & 0xFF) * (1.0f / 255.0f));
        trigger_sample(cue_cushion_pcm, CUE_CUSHION_PCM_LEN, level * 0.70f, rate);  /* -30%: cushions were too loud */
        break; }
    case CUE_SFX_POT: {
        /* real 2dpool pocket samples, selected like playPocket(). The pot
         * intensity `in` = 0.2 + 0.7*hit_i (hit_i = vmax/4.675 m/s), so the
         * snooker 4 m/s hard/soft split lands at in ~= 0.8. */
        if (s_snooker) {
            if (in > 0.8f) {              /* hard pot: 0.5 + excess, like 2dpool */
                float gain = 0.5f + (in - 0.8f) / 0.2f * 0.3f;   /* 0.5 .. 0.8 */
                trigger_sample(cue_hardpot_pcm, CUE_HARDPOT_LEN, gain, 1.0f);
            } else {                       /* soft pot: naturally quieter */
                trigger_sample(cue_softpot_pcm, CUE_SOFTPOT_LEN, 0.6f, 1.0f);
            }
        } else {                           /* pool: single pot sample at ~0.8 */
            trigger_sample(cue_pot_pcm, CUE_POT_LEN, 0.55f + 0.35f * in, 1.0f);
        }
        break; }
    default:              trigger(&M_UI, 0.8f); break;
    }
}

void cue_audio_render(int16_t *out, int n) {
    for (int s = 0; s < n; s++) {
        float acc = 0.0f;
        for (int i = 0; i < NVOICE; i++) {
            Voice *v = &s_v[i];
            if (!v->on) continue;
            if (v->smp) {                            /* sample-playback voice */
                int idx = (int)v->smp_pos;
                if (idx >= v->smp_len - 1) { v->on = 0; continue; }
                float frac = v->smp_pos - (float)idx;
                float a = v->smp[idx]     * (1.0f / 32768.0f);
                float b = v->smp[idx + 1] * (1.0f / 32768.0f);
                acc += (a + (b - a) * frac) * v->smp_gain;
                v->smp_pos += v->smp_rate;
                continue;
            }
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
