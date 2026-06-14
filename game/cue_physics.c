/*
 * ThumbyCue — billiard physics implementation. See cue_physics.h for the
 * model overview. All float; the M33 FPU makes this cheap and there are
 * only ≤22 balls so pairwise work is trivial.
 */
#include "cue_physics.h"
#include <math.h>
#include <string.h>

/* Fixed substep. 2 kHz keeps a fast break (≈6 m/s) to ~3 mm of travel per
 * step — well under a ball radius, so overlap-based collision resolution
 * never tunnels. */
#define CUE_H        (1.0f / 2000.0f)
#define CUE_MAX_SUB  400      /* cap iterations per call (anti death-spiral) */

/* Below these the corresponding motion is treated as stopped. */
#define V_STOP   0.005f       /* m/s linear */
#define U_ROLL   0.01f        /* m/s contact-point slip => rolling */
#define W_STOP   0.05f        /* rad/s spin */

void cue_world_defaults(CueWorld *w, float R, float mass) {
    memset(w, 0, sizeof(*w));
    w->R = R;
    w->mass = mass;
    w->g = 9.806f;
    w->mu_s = 0.20f;          /* ball–cloth sliding (Marlow-ish) */
    w->mu_r = 0.010f;         /* rolling resistance */
    /* Vertical-spin decay: alpha = 5 mu_sp g / (2R). mu_sp ~ 0.022 gives a
     * couple of seconds of carry, which matches real side-spin persistence. */
    w->spin_decel = 5.0f * 0.022f * w->g / (2.0f * R);
    w->e_bb = 0.96f;
    w->mu_bb = 0.06f;         /* ball–ball throw friction */
    w->e_cush = 0.92f;     /* lively cushions */
    w->mu_cush = 0.18f;
    /* Cushion nose ≈ 0.635 × ball diameter = 1.27 R above the cloth, so the
     * contact point sits ~0.27 R above centre; the contact normal tilts up by
     * asin(0.27). This tilt is what couples top/back spin into the rebound. */
    w->cush_tilt = asinf(0.27f);
    w->_acc = 0.0f;
}

void cue_phys_strike(const CueWorld *w, CueBall *b, Vec3 dir, float speed,
                     float tip_side, float tip_vert) {
    dir.y = 0.0f;
    dir = v3_norm(dir);
    b->vel = v3_scale(dir, speed);
    b->vel.y = 0.0f;

    /* Tip contact point on the cue ball relative to centre, in the cue frame:
     * forward = dir, up = +Y, right = up × forward. The tip strikes at
     * r = right*tip_side*R + up*tip_vert*R (offsets are fractions of R).
     * A horizontal impulse J = m*vel applied at r produces angular velocity
     * w = (r × J) / I, I = 2/5 m R^2. This is the textbook relation between
     * tip offset and the resulting top/back/side spin. */
    Vec3 fwd = dir;
    Vec3 up  = v3(0, 1, 0);
    Vec3 right = v3_norm(v3_cross(up, fwd));   /* points to the shooter's right of the aim */
    Vec3 r = v3_add(v3_scale(right, tip_side * w->R),
                    v3_scale(up,    tip_vert * w->R));
    Vec3 J = v3_scale(b->vel, w->mass);        /* impulse along the cue */
    float I = 0.4f * w->mass * w->R * w->R;
    b->w = v3_scale(v3_cross(r, J), 1.0f / I);
}

/* ---- per-ball cloth-contact evolution for one substep ------------------ */
static void ball_cloth(const CueWorld *w, CueBall *b, float h) {
    const float R = w->R, g = w->g;
    Vec3 rc = v3(0, -R, 0);                    /* centre -> contact point */
    /* Contact-point velocity (slip of the ball on the cloth). */
    Vec3 u = v3_add(b->vel, v3_cross(b->w, rc));
    u.y = 0.0f;
    float uh = sqrtf(u.x * u.x + u.z * u.z);

    /* The contact-point slip |u| decays under kinetic friction at (7/2)·μ_s·g
     * (the combined linear + angular effect for a uniform sphere). One substep
     * can therefore kill up to du_full of slip. */
    float du_full = 3.5f * w->mu_s * g * h;
    float I = 0.4f * w->mass * R * R;

    if (uh > du_full) {
        /* SLIDING: full kinetic friction opposing the slip. */
        Vec3 uhat = v3_scale(u, 1.0f / uh);
        Vec3 a = v3_scale(uhat, -w->mu_s * g);
        b->vel = v3_add(b->vel, v3_scale(a, h));
        Vec3 F = v3_scale(a, w->mass);                    /* tau = rc × F */
        b->w = v3_add(b->w, v3_scale(v3_cross(rc, F), h / I));
    } else {
        /* Reaching rolling THIS step: apply exactly enough friction to zero
         * the remaining slip (energy-exact, no snap bump), then roll. */
        if (uh > 1e-6f) {
            Vec3 uhat = v3_scale(u, 1.0f / uh);
            float f = uh / du_full;                       /* < 1: scaled */
            Vec3 a = v3_scale(uhat, -w->mu_s * g * f);
            b->vel = v3_add(b->vel, v3_scale(a, h));
            Vec3 F = v3_scale(a, w->mass);
            b->w = v3_add(b->w, v3_scale(v3_cross(rc, F), h / I));
        }
        /* ROLLING: light resistance; w tracks the (now decreasing) velocity so
         * the slip stays zero. u = 0 ⇒ w.x = vel.z/R, w.z = −vel.x/R. */
        float sp = sqrtf(b->vel.x * b->vel.x + b->vel.z * b->vel.z);
        if (sp > V_STOP) {
            Vec3 vhat = v3_scale(b->vel, 1.0f / sp);
            b->vel = v3_add(b->vel, v3_scale(vhat, -w->mu_r * g * h));
            if (v3_dot(b->vel, vhat) < 0.0f) b->vel = v3(0, 0, 0);
        } else {
            b->vel = v3(0, 0, 0);
        }
        b->w.x = b->vel.z / R;
        b->w.z = -b->vel.x / R;
    }

    /* Vertical spin (english) decays independently of motion. */
    if (b->w.y > W_STOP)       b->w.y -= w->spin_decel * h;
    else if (b->w.y < -W_STOP) b->w.y += w->spin_decel * h;
    else                       b->w.y = 0.0f;

    b->vel.y = 0.0f;
}

/* Integrate the render orientation from the angular velocity. */
static void ball_spin_orient(CueBall *b, float h) {
    float wl = v3_len(b->w);
    if (wl > 1e-5f) {
        Vec3 axis = v3_scale(b->w, 1.0f / wl);
        m3_rotate_world(&b->orient, axis, wl * h);
    }
}

/* ---- ball–ball impulse (restitution + Coulomb throw) ------------------- */
static int collide_ball_ball(const CueWorld *w, CueBall *bi, CueBall *bj) {
    Vec3 d = v3_sub(bj->pos, bi->pos);
    d.y = 0.0f;
    float dist = sqrtf(d.x * d.x + d.z * d.z);
    float mind = 2.0f * w->R;
    if (dist >= mind || dist < 1e-6f) return 0;

    Vec3 n = v3_scale(d, 1.0f / dist);         /* i -> j */
    /* Separate the overlap so they never stick. */
    float overlap = mind - dist;
    Vec3 push = v3_scale(n, overlap * 0.5f);
    bi->pos = v3_sub(bi->pos, push);
    bj->pos = v3_add(bj->pos, push);

    Vec3 dv = v3_sub(bj->vel, bi->vel);
    float vn = v3_dot(dv, n);
    if (vn >= 0.0f) return 0;                  /* separating already */

    float m = w->mass;
    /* Normal impulse (equal masses, reduced mass m/2). */
    float Jn = -(1.0f + w->e_bb) * vn / (2.0f / m);
    Vec3 Jn_v = v3_scale(n, Jn);
    bi->vel = v3_sub(bi->vel, v3_scale(Jn_v, 1.0f / m));
    bj->vel = v3_add(bj->vel, v3_scale(Jn_v, 1.0f / m));

    /* Tangential friction → throw / spin transfer. Relative surface velocity
     * at the contact point (midway): contact offset is +R*n on i, −R*n on j. */
    Vec3 ri = v3_scale(n,  w->R);
    Vec3 rj = v3_scale(n, -w->R);
    Vec3 si = v3_add(bi->vel, v3_cross(bi->w, ri));
    Vec3 sj = v3_add(bj->vel, v3_cross(bj->w, rj));
    Vec3 s = v3_sub(sj, si);
    Vec3 st = v3_sub(s, v3_scale(n, v3_dot(s, n)));   /* tangential slip */
    float stl = v3_len(st);
    if (stl > 1e-5f) {
        Vec3 that = v3_scale(st, 1.0f / stl);
        /* Tangential effective inverse-mass for two equal spheres at contact:
         * each ball 1/m + R^2/I = 7/(2m); two balls ⇒ 7/m. */
        float Jt_stop = stl / (7.0f / m);
        float Jt_max = w->mu_bb * fabsf(Jn);
        float Jt = (Jt_stop < Jt_max) ? Jt_stop : Jt_max;
        Vec3 Jt_v = v3_scale(that, Jt);
        float I = 0.4f * m * w->R * w->R;
        /* j gets +Jt_v, i gets −Jt_v (friction opposes j's slip relative to i). */
        bj->vel = v3_add(bj->vel, v3_scale(Jt_v, 1.0f / m));
        bi->vel = v3_sub(bi->vel, v3_scale(Jt_v, 1.0f / m));
        bj->w = v3_add(bj->w, v3_scale(v3_cross(rj, Jt_v), 1.0f / I));
        bi->w = v3_sub(bi->w, v3_scale(v3_cross(ri, Jt_v), 1.0f / I));
    }
    bi->vel.y = bj->vel.y = 0.0f;
    return 1;
}

/* ---- ball vs an immovable surface with contact normal N (unit, into ball)
 * raised by an optional tilt. Used for cushions (tilted) and jaw circles
 * (horizontal). Returns 1 if a collision was resolved. ------------------- */
static int collide_surface(const CueWorld *w, CueBall *b, Vec3 N,
                           float e, float mu) {
    /* Contact point on the ball is opposite N: r = −R N. The normal impulse
     * is therefore central (no torque); english/throw come from friction. */
    Vec3 r = v3_scale(N, -w->R);
    Vec3 vc = v3_add(b->vel, v3_cross(b->w, r));
    float vn = v3_dot(vc, N);
    if (vn >= 0.0f) return 0;                  /* moving away from the surface */

    float m = w->mass, I = 0.4f * m * w->R * w->R;
    float Jn = -(1.0f + e) * vn * m;           /* central: inverse mass = 1/m */
    Vec3 Jn_v = v3_scale(N, Jn);
    b->vel = v3_add(b->vel, v3_scale(Jn_v, 1.0f / m));

    /* Friction (and thus speed loss / english) only on a genuine impact — a
     * ball merely rolling ALONG the rail has a near-zero approach speed and
     * must not be braked every substep (that was the "sticking"). */
    if (-vn < 0.025f) { b->vel.y = 0.0f; return 1; }

    /* Tangential friction (rail/jaw): opposes the tangential surface slip,
     * which includes side spin — this is english-off-the-cushion. */
    vc = v3_add(b->vel, v3_cross(b->w, r));
    Vec3 vt = v3_sub(vc, v3_scale(N, v3_dot(vc, N)));
    float vtl = v3_len(vt);
    if (vtl > 1e-5f) {
        Vec3 that = v3_scale(vt, -1.0f / vtl);
        float Jt_stop = vtl / (7.0f / (2.0f * m));   /* 1/m + R^2/I = 7/(2m) */
        float Jt_max = mu * fabsf(Jn);
        float Jt = (Jt_stop < Jt_max) ? Jt_stop : Jt_max;
        Vec3 Jt_v = v3_scale(that, Jt);
        b->vel = v3_add(b->vel, v3_scale(Jt_v, 1.0f / m));
        b->w = v3_add(b->w, v3_scale(v3_cross(r, Jt_v), 1.0f / I));
    }
    b->vel.y = 0.0f;
    return 1;
}

/* Closest point on segment [a,b] to point p (X–Z plane). */
static Vec3 seg_closest(Vec3 a, Vec3 b, Vec3 p) {
    Vec3 ab = v3_sub(b, a); ab.y = 0;
    Vec3 ap = v3_sub(p, a); ap.y = 0;
    float L2 = ab.x * ab.x + ab.z * ab.z;
    float t = (L2 > 1e-9f) ? (ap.x * ab.x + ap.z * ab.z) / L2 : 0.0f;
    if (t < 0) t = 0; else if (t > 1) t = 1;
    return v3(a.x + ab.x * t, p.y, a.z + ab.z * t);
}

static int collide_cushions(const CueWorld *w, CueBall *b, uint32_t *ev) {
    int hit = 0;
    /* Tilt the rail normal up by cush_tilt so top/back spin couples into the
     * rebound; then re-normalise. */
    float ct = cosf(w->cush_tilt), st = sinf(w->cush_tilt);
    for (int s = 0; s < w->nseg; s++) {
        const CueSeg *seg = &w->seg[s];
        Vec3 cp = seg_closest(seg->a, seg->b, b->pos);
        Vec3 d = v3_sub(b->pos, cp); d.y = 0.0f;
        float dist = sqrtf(d.x * d.x + d.z * d.z);
        if (dist < w->R) {
            /* push out along the inward normal */
            b->pos = v3_add(b->pos, v3_scale(seg->n, (w->R - dist)));
            Vec3 N = v3_norm(v3(seg->n.x * ct, st, seg->n.z * ct));
            if (collide_surface(w, b, N, w->e_cush, w->mu_cush)) {
                hit = 1;
                if (ev) *ev |= CUE_EV_CUSHION;
            }
        }
    }
    /* Jaw tip circles (immovable) — rattle in the pocket mouths. */
    for (int j = 0; j < w->njaw; j++) {
        Vec3 d = v3_sub(b->pos, w->jaw[j]); d.y = 0.0f;
        float dist = sqrtf(d.x * d.x + d.z * d.z);
        float mind = w->R + w->jaw_r;
        if (dist < mind && dist > 1e-6f) {
            Vec3 N = v3_scale(d, 1.0f / dist);
            b->pos = v3_add(b->pos, v3_scale(N, (mind - dist)));
            if (collide_surface(w, b, N, w->e_cush, w->mu_cush)) {
                hit = 1;
                if (ev) *ev |= CUE_EV_JAW;
            }
        }
    }
    return hit;
}

static int check_pockets(const CueWorld *w, CueBall *b) {
    for (int p = 0; p < w->npocket; p++) {
        Vec3 d = v3_sub(b->pos, w->pocket[p]); d.y = 0.0f;
        float dist = sqrtf(d.x * d.x + d.z * d.z);
        if (dist < w->pocket_r[p]) {
            b->on = 0;
            b->pocket = (uint8_t)p;
            b->vel = v3(0, 0, 0);
            b->w = v3(0, 0, 0);
            return 1;
        }
    }
    return 0;
}

static void substep(CueWorld *w, CueBall *balls, int n, float h, uint32_t *ev) {
    /* 1. cloth friction + integrate position/orientation */
    for (int i = 0; i < n; i++) {
        CueBall *b = &balls[i];
        if (!b->on) continue;
        ball_cloth(w, b, h);
        b->pos = v3_add(b->pos, v3_scale(b->vel, h));
        b->pos.y = w->R;
        ball_spin_orient(b, h);
    }
    /* 2. ball–ball */
    for (int i = 0; i < n; i++) {
        if (!balls[i].on) continue;
        for (int j = i + 1; j < n; j++) {
            if (!balls[j].on) continue;
            if (collide_ball_ball(w, &balls[i], &balls[j]))
                if (ev) *ev |= CUE_EV_BALL_HIT;
        }
    }
    /* 3. cushions + jaws, then 4. pockets */
    for (int i = 0; i < n; i++) {
        CueBall *b = &balls[i];
        if (!b->on) continue;
        collide_cushions(w, b, ev);
        if (check_pockets(w, b) && ev) *ev |= CUE_EV_POCKET;
    }
}

int cue_phys_moving(const CueWorld *w, const CueBall *balls, int n) {
    for (int i = 0; i < n; i++) {
        if (!balls[i].on) continue;
        const CueBall *b = &balls[i];
        float v2 = b->vel.x * b->vel.x + b->vel.z * b->vel.z;
        if (v2 > V_STOP * V_STOP) return 1;
        /* Spinning in place (english on a stationary ball) still counts. */
        if (fabsf(b->w.y) > W_STOP) return 1;
    }
    return 0;
}

int cue_phys_step(CueWorld *w, CueBall *balls, int n, float dt, uint32_t *events) {
    if (events) *events = 0;
    w->_acc += dt;
    int iters = 0;
    while (w->_acc >= CUE_H && iters < CUE_MAX_SUB) {
        substep(w, balls, n, CUE_H, events);
        w->_acc -= CUE_H;
        iters++;
    }
    if (iters >= CUE_MAX_SUB) w->_acc = 0.0f;   /* shed backlog */

    /* Hard stop once everything has settled so we don't creep forever. */
    if (!cue_phys_moving(w, balls, n)) {
        for (int i = 0; i < n; i++) {
            if (!balls[i].on) continue;
            balls[i].vel = v3(0, 0, 0);
            balls[i].w.y = 0.0f;
            /* leave w.x/w.z = rolling residual; harmless, zero at next strike */
        }
        return 0;
    }
    return 1;
}
