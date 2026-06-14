/*
 * ThumbyCue — table dimensions, collision geometry and ball racks for both
 * 8-ball pool (7 ft) and snooker (12 ft). One dimension table feeds both the
 * physics world (cushion segments / jaws / pockets) and the renderer, so the
 * felt you see and the felt the balls bounce off are guaranteed identical.
 *
 * Coordinates: world metres, X = table length, Z = width, Y up. The playing
 * area runs x∈[−half_len,half_len], z∈[−half_wid,half_wid] to the cushion
 * NOSE. Baulk end is −X, top (foot/black) end is +X.
 */
#ifndef CUE_TABLE_H
#define CUE_TABLE_H

#include "cue_physics.h"

typedef enum { CUE_GAME_POOL = 0, CUE_GAME_SNOOKER = 1 } CueGameKind;

/* Ball id conventions (shared by physics, render, rules).
 * Pool:    0 = cue, 1..7 solids, 8 = black, 9..15 stripes.
 * Snooker: 0 = cue, 1..15 reds, then the six colours below. */
enum {
    CUE_ID_CUE = 0,
    CUE_ID_YELLOW = 20, CUE_ID_GREEN, CUE_ID_BROWN,
    CUE_ID_BLUE, CUE_ID_PINK, CUE_ID_BLACK,
};

typedef struct {
    CueGameKind kind;
    float half_len, half_wid;   /* to cushion nose (m) */
    float R, mass;
    float cushion_h;            /* nose height above cloth (m) */
    float rail_w;               /* rail/frame width, render only (m) */

    /* Pocket-jaw model (the crux of play). Two families:
     *   pocket_round = 0 → US pool: straight mitred facings, sharp points,
     *                      corner cut 142°, side cut ~104°.
     *   pocket_round = 1 → snooker/UK: rounded knuckles (arc), no points. */
    int   pocket_round;
    float mouth_corner;         /* opening between knuckle tips, corner (m) */
    float mouth_side;           /* opening, side/middle pocket (m) */
    float facing_len;           /* throat depth of a facing (m) */
    float cut_corner_deg;       /* facing splay vs rail, corner */
    float cut_side_deg;         /* facing splay vs rail, side */
    float jaw_r;                /* knuckle radius (sharp point / rounded jaw) */
    float cap_corner, cap_side; /* drop-capture radii in the throat */

    /* Snooker layout (ignored for pool). */
    float baulk_x, d_radius, blue_x, pink_x, black_x;
    uint16_t cloth, rail, rail_top, spot;
    int nballs;
} CueTable;

void cue_table_init(CueTable *t, CueGameKind kind);

/* Fill a physics world with this table's constants + collision geometry. */
void cue_table_build_world(const CueTable *t, CueWorld *w);

/* Lay out the opening rack / spots. Returns the number of balls placed.
 * balls[0] is always the cue ball. orient set to identity. */
int cue_table_rack(const CueTable *t, CueBall *balls);

/* Cue-ball home (head spot / brown-end D) for ball-in-hand placement. */
Vec3 cue_table_cue_home(const CueTable *t);

#endif
