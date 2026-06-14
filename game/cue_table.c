/*
 * ThumbyCue — table geometry & racks. See cue_table.h.
 *
 * The pocket-jaw model is the heart of the game. Each cushion is a straight
 * nose between two knuckle points, with a *facing* cut at each end running
 * back into the pocket throat. US pool uses straight mitred facings with
 * sharp points (corner cut 142°, side ~104°); snooker/UK uses short facings
 * with large rounded knuckles. Both the physics collision geometry (segments
 * + knuckle circles + capture points) and the 3D render mesh are generated
 * from this one description, so they can never disagree.
 */
#include "cue_table.h"
#include "cue_types.h"
#include <string.h>
#include <math.h>

#define DEG (3.14159265f / 180.0f)

void cue_table_init(CueTable *t, CueGameKind kind) {
    memset(t, 0, sizeof(*t));
    t->kind = kind;
    if (kind == CUE_GAME_POOL) {
        /* 7 ft US pool: 1.98 × 0.99 m, 2.25" balls. */
        t->half_len = 1.98f * 0.5f;
        t->half_wid = 0.99f * 0.5f;
        t->R = 0.028575f;
        t->mass = 0.170f;
        t->cushion_h = 1.27f * t->R;
        t->rail_w = 0.075f;
        t->pocket_round = 0;             /* straight mitred facings */
        t->mouth_corner = 0.1143f;       /* 4.5" */
        t->mouth_side = 0.127f;          /* 5"   */
        t->facing_len = 0.055f;
        t->cut_corner_deg = 142.0f;
        t->cut_side_deg = 104.0f;
        t->jaw_r = 0.006f;               /* sharp rubber point */
        t->cap_corner = t->mouth_corner * 0.5f;
        t->cap_side = t->mouth_side * 0.5f;
        t->cloth = RGB565C(22, 120, 70);
        t->rail = RGB565C(96, 54, 26);
        t->rail_top = RGB565C(128, 78, 38);
        t->spot = RGB565C(180, 180, 180);
        t->nballs = 16;
    } else {
        /* 12 ft snooker: 3.569 × 1.778 m, 52.5 mm balls. */
        t->half_len = 3.569f * 0.5f;
        t->half_wid = 1.778f * 0.5f;
        t->R = 0.0262500f;
        t->mass = 0.142f;
        t->cushion_h = 1.27f * t->R;
        t->rail_w = 0.085f;
        t->pocket_round = 1;             /* rounded knuckles */
        t->mouth_corner = 0.0889f;       /* 3.5" */
        t->mouth_side = 0.099f;          /* ~3.9" */
        t->facing_len = 0.030f;
        t->cut_corner_deg = 150.0f;      /* short facing, rounding dominates */
        t->cut_side_deg = 120.0f;
        t->jaw_r = 0.0135f;              /* big rounded knuckle */
        t->cap_corner = t->mouth_corner * 0.5f;
        t->cap_side = t->mouth_side * 0.5f;
        t->baulk_x = -t->half_len + 0.737f;
        t->d_radius = 0.292f;
        t->blue_x = 0.0f;
        t->pink_x = t->half_len * 0.5f;
        t->black_x = t->half_len - 0.324f;
        t->cloth = RGB565C(20, 100, 78);
        t->rail = RGB565C(74, 44, 22);
        t->rail_top = RGB565C(104, 62, 30);
        t->spot = RGB565C(200, 200, 200);
        t->nballs = 22;
    }
}

/* inward unit normal of segment a→b (points toward table centre). */
static Vec3 inward_n(float ax, float az, float bx, float bz) {
    float dx = bx - ax, dz = bz - az;
    Vec3 n = v3_norm(v3(dz, 0, -dx));
    float mx = (ax + bx) * 0.5f, mz = (az + bz) * 0.5f;
    if (n.x * (-mx) + n.z * (-mz) < 0) n = v3_scale(n, -1.0f);
    return n;
}
static void add_seg(CueWorld *w, Vec3 a, Vec3 b, uint8_t kind) {
    if (w->nseg >= CUE_MAX_SEG) return;
    CueSeg *s = &w->seg[w->nseg++];
    s->a = v3(a.x, w->R, a.z);
    s->b = v3(b.x, w->R, b.z);
    s->n = inward_n(a.x, a.z, b.x, b.z);
    s->kind = kind;
}
static void add_jaw(CueWorld *w, Vec3 k) {
    if (w->njaw >= CUE_MAX_SEG) return;
    w->jaw[w->njaw++] = v3(k.x, w->R, k.z);
}
static void add_pocket(CueWorld *w, float x, float z, float cap) {
    if (w->npocket >= CUE_MAX_POCKET) return;
    int i = w->npocket++;
    w->pocket[i] = v3(x, 0, z);
    w->pocket_r[i] = cap;
}

/* One facing from knuckle K: rotate the into-cushion direction t by the cut
 * angle toward the back (−nin) and run facing_len into the throat. */
static void add_facing(CueWorld *w, Vec3 K, Vec3 t, Vec3 nin,
                       float cut_deg, float len) {
    float A = cut_deg * DEG;
    Vec3 fdir = v3_norm(v3_add(v3_scale(t, cosf(A)), v3_scale(nin, -sinf(A))));
    Vec3 F = v3_add(K, v3_scale(fdir, len));
    add_seg(w, K, F, 1);
}

/* A cushion: straight nose K1→K2, a facing at each end, knuckle circles. */
static void add_cushion(CueWorld *w, Vec3 K1, Vec3 K2,
                        float cutA1, float cutA2, float flen) {
    add_seg(w, K1, K2, 0);
    Vec3 nin = inward_n(K1.x, K1.z, K2.x, K2.z);
    Vec3 t1 = v3_norm(v3_sub(K2, K1));
    add_facing(w, K1, t1, nin, cutA1, flen);
    add_facing(w, K2, v3_scale(t1, -1.0f), nin, cutA2, flen);
    add_jaw(w, K1);
    add_jaw(w, K2);
}

void cue_table_build_world(const CueTable *t, CueWorld *w) {
    cue_world_defaults(w, t->R, t->mass);
    w->cush_tilt = asinf((t->cushion_h - t->R) / t->R);
    w->jaw_r = t->jaw_r;

    const float hl = t->half_len, hw = t->half_wid;
    const float ac = t->mouth_corner * 0.70710678f;  /* corner setback */
    const float as = t->mouth_side * 0.5f;            /* side setback   */
    const float cc = t->cut_corner_deg, cs = t->cut_side_deg, fl = t->facing_len;

    /* Knuckle points (nose level). */
    Vec3 TLt = v3(-hl + ac, 0, hw), TRt = v3(hl - ac, 0, hw);
    Vec3 BLt = v3(-hl + ac, 0, -hw), BRt = v3(hl - ac, 0, -hw);
    Vec3 sTL = v3(-as, 0, hw), sTR = v3(as, 0, hw);
    Vec3 sBL = v3(-as, 0, -hw), sBR = v3(as, 0, -hw);
    Vec3 TRr = v3(hl, 0, hw - ac), BRr = v3(hl, 0, -hw + ac);
    Vec3 TLr = v3(-hl, 0, hw - ac), BLr = v3(-hl, 0, -hw + ac);

    /* Six cushions (corner end uses the corner cut, side end the side cut). */
    add_cushion(w, TLt, sTL, cc, cs, fl);   /* top-left   */
    add_cushion(w, sTR, TRt, cs, cc, fl);   /* top-right  */
    add_cushion(w, BLt, sBL, cc, cs, fl);   /* bottom-left*/
    add_cushion(w, sBR, BRt, cs, cc, fl);   /* bottom-right*/
    add_cushion(w, TRr, BRr, cc, cc, fl);   /* right      */
    add_cushion(w, TLr, BLr, cc, cc, fl);   /* left       */

    /* Drop-capture points, set into each throat by ~0.8 ball radii. */
    float ri = t->R * 0.8f, s = 0.70710678f;
    add_pocket(w, hl - ac * 0.5f + s * ri, hw - ac * 0.5f + s * ri, t->cap_corner);
    add_pocket(w, -hl + ac * 0.5f - s * ri, hw - ac * 0.5f + s * ri, t->cap_corner);
    add_pocket(w, hl - ac * 0.5f + s * ri, -hw + ac * 0.5f - s * ri, t->cap_corner);
    add_pocket(w, -hl + ac * 0.5f - s * ri, -hw + ac * 0.5f - s * ri, t->cap_corner);
    add_pocket(w, 0.0f, hw + ri, t->cap_side);
    add_pocket(w, 0.0f, -hw - ri, t->cap_side);
}

Vec3 cue_table_cue_home(const CueTable *t) {
    if (t->kind == CUE_GAME_POOL)
        return v3(-t->half_len * 0.5f, t->R, 0.0f);
    return v3(t->baulk_x, t->R, -t->d_radius * 0.4f);
}

static void set_ball(CueBall *b, int id, float x, float z, float R) {
    b->pos = v3(x, R, z);
    b->vel = v3(0, 0, 0);
    b->w = v3(0, 0, 0);
    b->orient = m3_identity();
    b->on = 1;
    b->id = (uint8_t)id;
    b->pocket = 0;
}

static int rack_pool(const CueTable *t, CueBall *b) {
    const float R = t->R;
    float footx = t->half_len * 0.5f;
    float dx = R * 1.7320508f;
    static const int rows[5][5] = {
        { 1 }, { 9, 2 }, { 10, 8, 3 }, { 11, 4, 12, 5 }, { 6, 13, 7, 14 },
    };
    static const int rown[5] = { 1, 2, 3, 4, 5 };
    int n = 1;
    for (int row = 0; row < 5; row++) {
        float x = footx + row * dx;
        for (int k = 0; k < rown[row]; k++) {
            float z = (-(row) * R) + k * 2.0f * R;
            int id = (row == 4 && k == 4) ? 15 : rows[row][k];
            set_ball(&b[n++], id, x, z, R);
        }
    }
    set_ball(&b[0], CUE_ID_CUE, -t->half_len * 0.5f, 0.0f, R);
    return n;
}

static int rack_snooker(const CueTable *t, CueBall *b) {
    const float R = t->R;
    int n = 0;
    set_ball(&b[n++], CUE_ID_CUE, t->baulk_x, -t->d_radius * 0.4f, R);
    set_ball(&b[n++], CUE_ID_YELLOW, t->baulk_x, +t->d_radius, R);
    set_ball(&b[n++], CUE_ID_GREEN,  t->baulk_x, -t->d_radius, R);
    set_ball(&b[n++], CUE_ID_BROWN,  t->baulk_x, 0.0f, R);
    set_ball(&b[n++], CUE_ID_BLUE,   t->blue_x, 0.0f, R);
    set_ball(&b[n++], CUE_ID_PINK,   t->pink_x, 0.0f, R);
    set_ball(&b[n++], CUE_ID_BLACK,  t->black_x, 0.0f, R);
    float apexx = t->pink_x + 2.0f * R + 0.002f;
    float dx = R * 1.7320508f;
    int red_id = 1;
    for (int row = 0; row < 5; row++) {
        float x = apexx + row * dx;
        for (int k = 0; k <= row; k++) {
            float z = (-(row) * R) + k * 2.0f * R;
            set_ball(&b[n++], red_id++, x, z, R);
        }
    }
    return n;
}

int cue_table_rack(const CueTable *t, CueBall *balls) {
    memset(balls, 0, sizeof(CueBall) * CUE_MAX_BALLS);
    return (t->kind == CUE_GAME_POOL) ? rack_pool(t, balls)
                                      : rack_snooker(t, balls);
}
