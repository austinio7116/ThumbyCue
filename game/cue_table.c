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
        /* 7 ft US pool: 1.98 × 0.99 m, 2.25" balls. Pocket geometry follows
         * the 2D game's ratios (pocket≈2.17R, knuckle gap≈2.67R, 45°/70°). */
        t->half_len = 1.98f * 0.5f;
        t->half_wid = 0.99f * 0.5f;
        t->R = 0.028575f;
        t->mass = 0.170f;
        t->cushion_h = 1.27f * t->R;
        t->rail_w = 0.075f;
        t->pocket_round = 0;
        t->pr_corner  = 2.167f * t->R;
        t->pr_side    = 1.95f  * t->R;
        t->gap_corner = 2.667f * t->R;
        t->gap_side   = 2.50f  * t->R;
        t->facing_len = 1.667f * t->R;
        t->ang_corner = 45.0f;
        t->ang_side   = 70.0f;
        t->off_corner = 0.42f * t->R;
        t->off_side   = 1.25f * t->R;
        t->jaw_r      = 0.004f;
        t->cloth = RGB565C(22, 120, 70);
        t->rail = RGB565C(96, 54, 26);
        t->rail_top = RGB565C(128, 78, 38);
        t->spot = RGB565C(180, 180, 180);
        t->nballs = 16;
    } else {
        /* 12 ft snooker: tighter, rounder pockets (steeper facings). */
        t->half_len = 3.569f * 0.5f;
        t->half_wid = 1.778f * 0.5f;
        t->R = 0.0262500f;
        t->mass = 0.142f;
        t->cushion_h = 1.27f * t->R;
        t->rail_w = 0.085f;
        t->pocket_round = 1;
        t->pr_corner  = 1.85f * t->R;
        t->pr_side    = 1.70f * t->R;
        t->gap_corner = 2.30f * t->R;
        t->gap_side   = 2.05f * t->R;
        t->facing_len = 1.25f * t->R;
        t->ang_corner = 60.0f;          /* steeper = rounder mouth */
        t->ang_side   = 80.0f;
        t->off_corner = 0.30f * t->R;
        t->off_side   = 1.00f * t->R;
        t->jaw_r      = 0.012f;
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

/* A cushion chain (faithful to the 2D model): facing-tip P1 → knuckle P2 →
 * knuckle P3 → facing-tip P4. Segments: P1-P2 facing, P2-P3 nose, P3-P4 facing.
 * Knuckle circles sit at P2,P3. P2,P3 are pushed into w->jaw in boundary order
 * so the renderer can fan the bed straight off them. */
static void add_chain(CueWorld *w, Vec3 P1, Vec3 P2, Vec3 P3, Vec3 P4) {
    add_seg(w, P1, P2, 1);
    add_seg(w, P2, P3, 0);
    add_seg(w, P3, P4, 1);
    add_jaw(w, P2);
    add_jaw(w, P3);
}

void cue_table_build_world(const CueTable *t, CueWorld *w) {
    cue_world_defaults(w, t->R, t->mass);
    w->cush_tilt = asinf((t->cushion_h - t->R) / t->R);
    w->jaw_r = t->jaw_r;

    const float hl = t->half_len, hw = t->half_wid;
    const float g = t->gap_corner, sg = t->gap_side, sl = t->facing_len;
    const float cc = cosf(t->ang_corner * DEG), sc = sinf(t->ang_corner * DEG);
    const float cs = cosf(t->ang_side * DEG),   ss = sinf(t->ang_side * DEG);

    /* Six cushion chains. Facings splay OUTWARD (|z|>hw on long rails, |x|>hl
     * on short rails). Top = −Z, bottom = +Z, left = −X, right = +X. */
    add_chain(w, v3(-hl+g - cc*sl, 0, -hw - sc*sl), v3(-hl+g, 0, -hw),
                 v3(-sg, 0, -hw),                   v3(-sg + cs*sl, 0, -hw - ss*sl));
    add_chain(w, v3(sg - cs*sl, 0, -hw - ss*sl),    v3(sg, 0, -hw),
                 v3(hl-g, 0, -hw),                  v3(hl-g + cc*sl, 0, -hw - sc*sl));
    add_chain(w, v3(hl + sc*sl, 0, -hw+g - cc*sl),  v3(hl, 0, -hw+g),
                 v3(hl, 0, hw-g),                   v3(hl + sc*sl, 0, hw-g + cc*sl));
    add_chain(w, v3(hl-g + cc*sl, 0, hw + sc*sl),   v3(hl-g, 0, hw),
                 v3(sg, 0, hw),                     v3(sg - cs*sl, 0, hw + ss*sl));
    add_chain(w, v3(-sg + cs*sl, 0, hw + ss*sl),    v3(-sg, 0, hw),
                 v3(-hl+g, 0, hw),                  v3(-hl+g - cc*sl, 0, hw + sc*sl));
    add_chain(w, v3(-hl - sc*sl, 0, hw-g + cc*sl),  v3(-hl, 0, hw-g),
                 v3(-hl, 0, -hw+g),                 v3(-hl - sc*sl, 0, -hw+g - cc*sl));

    /* Pocket circles: centre offset just beyond the boundary; drop-capture
     * when the ball centre is within (radius − 0.3R), matching the 2D game. */
    const float d = 0.70710678f, oc = t->off_corner, os = t->off_side;
    float capc = t->pr_corner - 0.3f * t->R, caps = t->pr_side - 0.3f * t->R;
    add_pocket(w, -hl - oc*d, -hw - oc*d, capc);
    add_pocket(w,  hl + oc*d, -hw - oc*d, capc);
    add_pocket(w,  hl + oc*d,  hw + oc*d, capc);
    add_pocket(w, -hl - oc*d,  hw + oc*d, capc);
    add_pocket(w, 0.0f, -hw - os, caps);
    add_pocket(w, 0.0f,  hw + os, caps);
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
