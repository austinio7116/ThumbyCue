/*
 * ThumbyCue — table geometry & racks. See cue_table.h.
 */
#include "cue_table.h"
#include "cue_types.h"
#include <string.h>
#include <math.h>

void cue_table_init(CueTable *t, CueGameKind kind) {
    memset(t, 0, sizeof(*t));
    t->kind = kind;
    if (kind == CUE_GAME_POOL) {
        /* 7 ft table: playing area 1.98 × 0.99 m, 2.25" balls (R=28.575mm). */
        t->half_len = 1.98f * 0.5f;
        t->half_wid = 0.99f * 0.5f;
        t->R = 0.028575f;
        t->mass = 0.170f;            /* 6 oz */
        t->cushion_h = 1.27f * t->R;
        t->rail_w = 0.075f;
        t->corner_gap = 2.7f * t->R; /* mouth ≈ 3.8R between jaw tips */
        t->side_gap = 2.0f * t->R;
        t->cap_corner = 1.30f * t->R;
        t->cap_side = 1.35f * t->R;
        t->pocket_out = 0.5f * t->R;
        t->jaw_r = 0.22f * t->R;
        t->cloth = RGB565C(20, 110, 60);
        t->rail = RGB565C(90, 50, 24);
        t->rail_top = RGB565C(120, 70, 34);
        t->spot = RGB565C(180, 180, 180);
        t->nballs = 16;
    } else {
        /* 12 ft snooker: playing area 3.569 × 1.778 m, 52.5mm balls. */
        t->half_len = 3.569f * 0.5f;
        t->half_wid = 1.778f * 0.5f;
        t->R = 0.0262500f;
        t->mass = 0.142f;
        t->cushion_h = 1.27f * t->R;
        t->rail_w = 0.085f;
        t->corner_gap = 2.3f * t->R;  /* tighter templates */
        t->side_gap = 1.8f * t->R;
        t->cap_corner = 1.18f * t->R;
        t->cap_side = 1.22f * t->R;
        t->pocket_out = 0.45f * t->R;
        t->jaw_r = 0.25f * t->R;
        /* Spots (proportional to the real full-size measurements). */
        t->baulk_x = -t->half_len + 0.737f;        /* baulk line 737 mm in */
        t->d_radius = 0.292f;                       /* D radius 11.5" */
        t->blue_x = 0.0f;                           /* centre spot */
        t->pink_x = t->half_len * 0.5f;             /* midway centre↔top */
        t->black_x = t->half_len - 0.324f;          /* 324 mm from top cushion */
        t->cloth = RGB565C(18, 95, 70);
        t->rail = RGB565C(70, 40, 20);
        t->rail_top = RGB565C(100, 58, 28);
        t->spot = RGB565C(190, 190, 190);
        t->nballs = 22;
    }
}

static void add_seg(CueWorld *w, float ax, float az, float bx, float bz,
                    Vec3 n) {
    if (w->nseg >= CUE_MAX_SEG) return;
    CueSeg *s = &w->seg[w->nseg++];
    s->a = v3(ax, w->R, az);
    s->b = v3(bx, w->R, bz);
    s->n = n;
}
static void add_jaw(CueWorld *w, float x, float z) {
    if (w->njaw >= CUE_MAX_SEG) return;
    w->jaw[w->njaw++] = v3(x, w->R, z);
}
static void add_pocket(CueWorld *w, float x, float z, float cap) {
    if (w->npocket >= CUE_MAX_POCKET) return;
    int i = w->npocket++;
    w->pocket[i] = v3(x, 0, z);
    w->pocket_r[i] = cap;
}

void cue_table_build_world(const CueTable *t, CueWorld *w) {
    cue_world_defaults(w, t->R, t->mass);
    w->cush_tilt = asinf((t->cushion_h - t->R) / t->R);
    w->jaw_r = t->jaw_r;

    const float hl = t->half_len, hw = t->half_wid;
    const float cg = t->corner_gap, sg = t->side_gap;

    /* Long rail +Z (inward −Z): corner → side pocket → corner. */
    add_seg(w, -hl + cg, +hw, -sg, +hw, v3(0, 0, -1));
    add_seg(w, +sg, +hw, +hl - cg, +hw, v3(0, 0, -1));
    /* Long rail −Z (inward +Z). */
    add_seg(w, -hl + cg, -hw, -sg, -hw, v3(0, 0, +1));
    add_seg(w, +sg, -hw, +hl - cg, -hw, v3(0, 0, +1));
    /* Short rail +X (inward −X). */
    add_seg(w, +hl, -hw + cg, +hl, +hw - cg, v3(-1, 0, 0));
    /* Short rail −X (inward +X). */
    add_seg(w, -hl, -hw + cg, -hl, +hw - cg, v3(+1, 0, 0));

    /* Jaw-tip circles at every nose end (rattle). */
    add_jaw(w, -hl + cg, +hw); add_jaw(w, -sg, +hw);
    add_jaw(w, +sg, +hw);      add_jaw(w, +hl - cg, +hw);
    add_jaw(w, -hl + cg, -hw); add_jaw(w, -sg, -hw);
    add_jaw(w, +sg, -hw);      add_jaw(w, +hl - cg, -hw);
    add_jaw(w, +hl, +hw - cg); add_jaw(w, +hl, -hw + cg);
    add_jaw(w, -hl, +hw - cg); add_jaw(w, -hl, -hw + cg);

    /* Pocket capture centres (corners pushed out diagonally, sides outward). */
    const float po = t->pocket_out, s = 0.70710678f;
    add_pocket(w, +hl + po * s, +hw + po * s, t->cap_corner);
    add_pocket(w, -hl - po * s, +hw + po * s, t->cap_corner);
    add_pocket(w, +hl + po * s, -hw - po * s, t->cap_corner);
    add_pocket(w, -hl - po * s, -hw - po * s, t->cap_corner);
    add_pocket(w, 0.0f, +hw + po, t->cap_side);
    add_pocket(w, 0.0f, -hw - po, t->cap_side);
}

Vec3 cue_table_cue_home(const CueTable *t) {
    if (t->kind == CUE_GAME_POOL)
        return v3(-t->half_len * 0.5f, t->R, 0.0f);   /* head spot */
    return v3(t->baulk_x, t->R, -t->d_radius * 0.4f);  /* in the D */
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

/* 8-ball rack: apex on the foot spot, triangle opening toward the head. The
 * 8 sits in the centre of the third row; corners are a stripe and a solid
 * (a legal-looking arrangement). */
static int rack_pool(const CueTable *t, CueBall *b) {
    const float R = t->R;
    float footx = t->half_len * 0.5f;        /* foot spot */
    float dx = R * 1.7320508f;               /* row pitch = 2R·cos30 */
    /* id grid by row, top→bottom within each row. -1 → fill later. */
    static const int rows[5][5] = {
        { 1 },
        { 9, 2 },
        { 10, 8, 3 },
        { 11, 4, 12, 5 },
        { 6, 13, 7, 14 },   /* last row needs 5; 15 appended below */
    };
    static const int rown[5] = { 1, 2, 3, 4, 5 };
    int n = 1;                                /* index 0 = cue ball */
    for (int row = 0; row < 5; row++) {
        float x = footx + row * dx;
        for (int k = 0; k < rown[row]; k++) {
            float z = (-(row) * R) + k * 2.0f * R;
            int id = (row == 4 && k == 4) ? 15 : rows[row][k];
            set_ball(&b[n++], id, x, z, R);
        }
    }
    set_ball(&b[0], CUE_ID_CUE, -t->half_len * 0.5f, 0.0f, R);
    return n;                                 /* 16 */
}

/* Snooker: 15 reds in a pyramid behind the pink, six colours on their spots,
 * cue ball in the D. */
static int rack_snooker(const CueTable *t, CueBall *b) {
    const float R = t->R;
    int n = 0;
    set_ball(&b[n++], CUE_ID_CUE, t->baulk_x, -t->d_radius * 0.4f, R);
    /* Colours. */
    set_ball(&b[n++], CUE_ID_YELLOW, t->baulk_x, +t->d_radius, R);
    set_ball(&b[n++], CUE_ID_GREEN,  t->baulk_x, -t->d_radius, R);
    set_ball(&b[n++], CUE_ID_BROWN,  t->baulk_x, 0.0f, R);
    set_ball(&b[n++], CUE_ID_BLUE,   t->blue_x, 0.0f, R);
    set_ball(&b[n++], CUE_ID_PINK,   t->pink_x, 0.0f, R);
    set_ball(&b[n++], CUE_ID_BLACK,  t->black_x, 0.0f, R);
    /* 15 reds: pyramid apex just behind the pink, base toward the top rail. */
    float apexx = t->pink_x + 2.0f * R + 0.002f;
    float dx = R * 1.7320508f;
    int red_id = 1;                            /* reds are ids 1..15 */
    for (int row = 0; row < 5; row++) {
        float x = apexx + row * dx;
        for (int k = 0; k <= row; k++) {
            float z = (-(row) * R) + k * 2.0f * R;
            set_ball(&b[n++], red_id++, x, z, R);
        }
    }
    return n;                                  /* 22 */
}

int cue_table_rack(const CueTable *t, CueBall *balls) {
    memset(balls, 0, sizeof(CueBall) * CUE_MAX_BALLS);
    return (t->kind == CUE_GAME_POOL) ? rack_pool(t, balls)
                                      : rack_snooker(t, balls);
}
