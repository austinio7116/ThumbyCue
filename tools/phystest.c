/* Standalone physics sanity tests:
 *   gcc -O2 -ffast-math -I../game phystest.c ../game/cue_physics.c \
 *       ../game/cue_table.c -lm -o /tmp/phystest && /tmp/phystest
 */
#include "cue_physics.h"
#include "cue_table.h"
#include <stdio.h>
#include <math.h>
#include <string.h>

static int fails = 0;
static void check(const char *name, int ok, const char *detail) {
    printf("[%s] %s  %s\n", ok ? "PASS" : "FAIL", name, detail ? detail : "");
    if (!ok) fails++;
}

/* Open infinite table: same cloth/ball constants but no rails/pockets, so a
 * shot rolls freely and we can measure pure ball–cloth behaviour. */
static void open_world(CueWorld *w) {
    CueTable t; cue_table_init(&t, CUE_GAME_POOL);
    cue_table_build_world(&t, w);
    w->nseg = 0; w->njaw = 0; w->npocket = 0;
}
static void fresh_ball(CueBall *b, float x, float z, float R) {
    memset(b, 0, sizeof(*b));
    b->on = 1; b->pos = v3(x, R, z); b->orient = m3_identity();
}
static float speed_of(const CueBall *b) {
    return sqrtf(b->vel.x * b->vel.x + b->vel.z * b->vel.z);
}

/* Roll one cue ball to rest on the open table; return travel distance. */
static float free_travel(float speed, float tip_side, float tip_vert) {
    CueWorld w; open_world(&w);
    CueBall b; fresh_ball(&b, 0, 0, w.R);
    cue_phys_strike(&w, &b, v3(1, 0, 0), speed, tip_side, tip_vert);
    Vec3 s = b.pos;
    int n = 0;
    while (cue_phys_step(&w, &b, 1, 1.0f / 250.0f, NULL) && n++ < 8000) {}
    Vec3 d = v3_sub(b.pos, s);
    return sqrtf(d.x * d.x + d.z * d.z);
}

int main(void) {
    char buf[128];

    /* 1. Energy strictly non-increasing on a free roll. */
    {
        CueWorld w; open_world(&w);
        CueBall b; fresh_ball(&b, 0, 0, w.R);
        cue_phys_strike(&w, &b, v3(1, 0, 0), 2.0f, 0, 0);
        float prev = 1e30f; int mono = 1; float worst = 0;
        for (int i = 0; i < 1200; i++) {
            cue_phys_step(&w, &b, 1, 1.0f / 250.0f, NULL);
            float v2 = b.vel.x * b.vel.x + b.vel.z * b.vel.z;
            float wl = v3_len(b.w);
            float KE = 0.5f * w.mass * v2
                     + 0.5f * 0.4f * w.mass * w.R * w.R * wl * wl;
            if (KE > prev) { mono = 0; if (KE - prev > worst) worst = KE - prev; }
            prev = KE;
        }
        snprintf(buf, sizeof buf, "max increase=%.2e J", worst);
        check("energy non-increasing (free roll)", mono, buf);
    }

    /* 2. Faster shot travels farther (free table). */
    {
        float d1 = free_travel(1.0f, 0, 0);
        float d2 = free_travel(2.0f, 0, 0);
        float d3 = free_travel(3.0f, 0, 0);
        snprintf(buf, sizeof buf, "1m/s=%.2f 2m/s=%.2f 3m/s=%.2f m", d1, d2, d3);
        check("faster travels farther", d3 > d2 && d2 > d1, buf);
    }

    /* 3. Follow rolls farther forward than stun than draw (same speed). */
    {
        float follow = free_travel(2.0f, 0, +0.45f);
        float stun   = free_travel(2.0f, 0,  0.0f);
        float draw   = free_travel(2.0f, 0, -0.45f);
        snprintf(buf, sizeof buf, "draw=%.2f stun=%.2f follow=%.2f m",
                 draw, stun, follow);
        check("follow > stun > draw travel", follow > stun && stun > draw, buf);
    }

    /* 4. Center-ball shot settles into natural roll: w.z = -vel.x/R. */
    {
        CueWorld w; open_world(&w);
        CueBall b; fresh_ball(&b, 0, 0, w.R);
        cue_phys_strike(&w, &b, v3(1, 0, 0), 2.0f, 0, 0);
        for (int i = 0; i < 250; i++) cue_phys_step(&w, &b, 1, 1.0f / 250.0f, NULL);
        float want = -b.vel.x / w.R;
        snprintf(buf, sizeof buf, "w.z=%.2f want=%.2f rad/s", b.w.z, want);
        check("settles into natural roll", fabsf(b.w.z - want) < 1.0f, buf);
    }

    /* 5. Stop distance is bounded above by the pure-rolling model
     *    d = 7v²/(10·μr·g) (the slide phase removes extra energy), and is a
     *    sensible fraction of it — a friction-tuning sanity check. */
    {
        float v = 1.5f;
        float d = free_travel(v, 0, 0);
        float dmodel = 7.0f * v * v / (10.0f * 0.010f * 9.806f);
        snprintf(buf, sizeof buf, "sim=%.2f rolling-model=%.2f m", d, dmodel);
        check("stop distance below pure-roll model", d > 0.25f * dmodel && d < dmodel, buf);
    }

    /* 6. Full-ball straight stun: cue nearly stops, object goes forward at
     *    nearly the cue's incoming speed. Open table so nothing interferes. */
    {
        CueWorld w; open_world(&w);
        CueBall b[2];
        fresh_ball(&b[0], -0.05f, 0, w.R);      /* close, still sliding */
        fresh_ball(&b[1],  0.0f,  0, w.R); b[1].id = 1;
        cue_phys_strike(&w, &b[0], v3(1, 0, 0), 2.0f, 0, 0);
        float vin = speed_of(&b[0]);
        for (int i = 0; i < 200; i++) cue_phys_step(&w, b, 2, 1.0f / 1000.0f, NULL);
        snprintf(buf, sizeof buf, "vin=%.2f cueVx=%.2f objVx=%.2f",
                 vin, b[0].vel.x, b[1].vel.x);
        check("stun: object launched forward", b[1].vel.x > 1.5f, buf);
        check("stun: cue checks up", b[0].vel.x < 0.6f, buf);
    }

    /* 7. Half-ball cut: the two balls separate by ~90° just after contact
     *    (the 90-degree rule, cue ball sliding/stun at impact). */
    {
        CueWorld w; open_world(&w);
        CueBall b[2];
        fresh_ball(&b[0], -0.05f, 0, w.R);
        fresh_ball(&b[1], 0, w.R, w.R); b[1].id = 1;   /* 30° contact line */
        cue_phys_strike(&w, &b[0], v3(1, 0, 0), 3.0f, 0, 0);
        float ang = 0; int got = 0;
        for (int i = 0; i < 200; i++) {
            cue_phys_step(&w, b, 2, 1.0f / 2000.0f, NULL);
            if (speed_of(&b[1]) > 0.2f && speed_of(&b[0]) > 0.2f) {
                float a0 = atan2f(b[0].vel.z, b[0].vel.x);
                float a1 = atan2f(b[1].vel.z, b[1].vel.x);
                ang = fabsf(a0 - a1) * 57.2958f;
                if (ang > 180) ang = 360 - ang;
                got = 1; break;
            }
        }
        snprintf(buf, sizeof buf, "separation=%.1f deg", ang);
        check("cut shot ~90deg separation", got && ang > 70 && ang < 110, buf);
    }

    /* 8. Cushion rebound: ball straight into a rail (not a pocket) reverses
     *    and stays on its line. Use the −X end rail, off-centre in Z. */
    {
        CueTable t; cue_table_init(&t, CUE_GAME_POOL);
        CueWorld w; cue_table_build_world(&t, &w);
        CueBall b; fresh_ball(&b, 0.0f, 0.0f, w.R);
        cue_phys_strike(&w, &b, v3(-1, 0, 0), 2.0f, 0, 0);   /* toward −X end */
        int bounced = 0; float zdrift = 0;
        for (int i = 0; i < 1500; i++) {
            cue_phys_step(&w, &b, 1, 1.0f / 1000.0f, NULL);
            if (b.vel.x > 0.05f) { bounced = 1; zdrift = b.pos.z; break; }
        }
        snprintf(buf, sizeof buf, "vx=%.2f zdrift=%.3f", b.vel.x, zdrift);
        check("cushion reverses vx", bounced, buf);
        check("straight rebound on-line", fabsf(zdrift) < 0.03f, buf);
    }

    /* 9. Side spin throws the rebound angle off the cushion (english works).
     *    Pocketless table so the deflected ball isn't captured; measure the
     *    outgoing vel.z right after the bounce vs a no-english control. */
    {
        CueTable t; cue_table_init(&t, CUE_GAME_POOL);
        float vz_eng = 0, vz_ctrl = 0;
        for (int e = 0; e < 2; e++) {
            CueWorld w; cue_table_build_world(&t, &w);
            w.npocket = 0; w.njaw = 0;             /* no capture, just rails */
            CueBall b; fresh_ball(&b, 0.0f, 0.0f, w.R);
            float side = e ? 0.45f : 0.0f;
            cue_phys_strike(&w, &b, v3(-1, 0, 0), 2.0f, side, 0);
            for (int i = 0; i < 1500; i++) {
                cue_phys_step(&w, &b, 1, 1.0f / 1000.0f, NULL);
                if (b.vel.x > 0.05f) { if (e) vz_eng = b.vel.z; else vz_ctrl = b.vel.z; break; }
            }
        }
        snprintf(buf, sizeof buf, "vz english=%.3f control=%.3f m/s", vz_eng, vz_ctrl);
        check("english deflects rebound", fabsf(vz_eng) > 0.05f
                                          && fabsf(vz_eng) > fabsf(vz_ctrl) + 0.04f, buf);
    }

    /* 10. Corner pot. */
    {
        CueTable t; cue_table_init(&t, CUE_GAME_POOL);
        CueWorld w; cue_table_build_world(&t, &w);
        CueBall b; fresh_ball(&b, t.half_len * 0.4f, t.half_wid * 0.4f, w.R);
        Vec3 dir = v3_norm(v3(t.half_len - b.pos.x, 0, t.half_wid - b.pos.z));
        cue_phys_strike(&w, &b, dir, 1.6f, 0, 0);
        int n = 0;
        while (cue_phys_step(&w, &b, 1, 1.0f / 250.0f, NULL) && n++ < 4000) {}
        snprintf(buf, sizeof buf, "on=%d pocket=%d", b.on, b.pocket);
        check("corner pot", b.on == 0, buf);
    }

    printf("\n%s (%d failures)\n", fails ? "TESTS FAILED" : "ALL PASS", fails);
    return fails ? 1 : 0;
}
