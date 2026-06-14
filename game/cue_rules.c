/*
 * ThumbyCue — rules & scoring. See cue_rules.h. Faithful-but-simplified port
 * of 2dpool/js/game.js: UK-style 8-ball (numeric groups, ball-in-hand fouls,
 * black last) and snooker (red→colour alternation, values, colour respotting,
 * foul = max(4,…), clearance sequence, frame end on the black).
 */
#include "cue_rules.h"
#include "cue_types.h"
#include <string.h>
#include <stdio.h>

/* ---- ball classification --------------------------------------------- */
static int pool_group(int id) {            /* 1 low, 2 high, 0 = the 8 */
    if (id >= 1 && id <= 7) return 1;
    if (id >= 9 && id <= 15) return 2;
    return 0;                              /* id == 8 */
}
static int is_red(int id)    { return id >= 1 && id <= 15; }
static int is_colour(int id) { return id >= CUE_ID_YELLOW && id <= CUE_ID_BLACK; }
static int snk_value(int id) {
    if (is_red(id)) return 1;
    switch (id) {
        case CUE_ID_YELLOW: return 2; case CUE_ID_GREEN: return 3;
        case CUE_ID_BROWN:  return 4; case CUE_ID_BLUE:  return 5;
        case CUE_ID_PINK:   return 6; case CUE_ID_BLACK: return 7;
    }
    return 0;
}
static int colour_id_for_value(int v) {
    switch (v) {
        case 2: return CUE_ID_YELLOW; case 3: return CUE_ID_GREEN;
        case 4: return CUE_ID_BROWN;  case 5: return CUE_ID_BLUE;
        case 6: return CUE_ID_PINK;   case 7: return CUE_ID_BLACK;
    }
    return -1;
}

void cue_rules_init(CueRules *r, const CueTable *t, int cpu) {
    memset(r, 0, sizeof(*r));
    r->kind = t->is_snooker;
    r->mode = t->kind;
    r->cpu = cpu;
    r->turn = 0; r->winner = -1; r->open = 1; r->break_shot = 1;
    r->shots_remaining = 1; r->two_shot = 0; r->free_shot = 0;
    if (r->kind) {
        r->target = 0; r->reds_left = t->reds ? t->reds : 15;
        /* colour spots by value 2..7 */
        r->spot[2] = v3(t->baulk_x, t->R, +t->d_radius);   /* yellow */
        r->spot[3] = v3(t->baulk_x, t->R, -t->d_radius);   /* green  */
        r->spot[4] = v3(t->baulk_x, t->R, 0.0f);           /* brown  */
        r->spot[5] = v3(t->blue_x,  t->R, 0.0f);           /* blue   */
        r->spot[6] = v3(t->pink_x,  t->R, 0.0f);           /* pink   */
        r->spot[7] = v3(t->black_x, t->R, 0.0f);           /* black  */
    }
}

static int group_cleared(const CueBall *b, int n, int grp) {
    for (int i = 1; i < n; i++)
        if (b[i].on && pool_group(b[i].id) == grp) return 0;
    return 1;
}

static CueBall *find_ball(CueBall *b, int n, int id) {
    for (int i = 0; i < n; i++) if (b[i].id == id) return &b[i];
    return NULL;
}
/* re-spot the 8 (illegally potted on the break). */
static void respot_eight(CueBall *b, int n) {
    CueBall *q = find_ball(b, n, 8);
    if (q) { q->on = 1; q->vel = v3(0,0,0); q->w = v3(0,0,0); }
}
static void respot_colour(CueRules *r, CueBall *b, int n, int id) {
    CueBall *q = find_ball(b, n, id);
    if (!q) return;
    int v = snk_value(id);
    q->on = 1; q->vel = v3(0,0,0); q->w = v3(0,0,0);
    q->pos = r->spot[v];           /* (occupancy not checked — good enough) */
    q->orient = m3_identity();
}

/* ---- 8-ball ---------------------------------------------------------- */
static void resolve_pool(CueRules *r, CueBall *b, int n, int first_hit,
                         int scratch, int cushion, const int *potted, int np) {
    int grp = r->group[r->turn];
    int low = 0, high = 0, eight = 0;
    for (int k = 0; k < np; k++) {
        int g = pool_group(potted[k]);
        if (potted[k] == 8) eight = 1; else if (g == 1) low++; else if (g == 2) high++;
    }
    int my_potted = (grp == 1) ? low : high;   /* own group balls potted THIS shot */
    int legal_pot = r->open ? (low || high) : my_potted;
    /* "on the 8" only if the group was cleared BEFORE this shot — i.e. it's
     * empty now AND you didn't just pot a group ball this shot. Otherwise the
     * shot that pots your last group ball would wrongly read as must-hit-8. */
    int on_eight = !r->open && group_cleared(b, n, grp) && my_potted == 0;

    int foul = 0; const char *why = "";
    if (scratch)            { foul = 1; why = "SCRATCH"; }
    else if (first_hit < 0) { foul = 1; why = "NO BALL"; }
    else {
        int fg = pool_group(first_hit);
        if (!r->open) {
            if (on_eight) { if (first_hit != 8) { foul = 1; why = "MUST HIT 8"; } }
            else if (fg != grp) { foul = 1; why = "WRONG BALL"; }   /* incl. hitting the 8 early */
        } else if (first_hit == 8)        { foul = 1; why = "HIT 8 FIRST"; }
    }
    (void)cushion;

    /* the 8 */
    if (eight) {
        if (r->break_shot) {                       /* re-spot, no result */
            respot_eight(b, n);
        } else {
            /* legal win only if the group was clear BEFORE potting the 8 */
            int win = !foul && !scratch && on_eight;
            r->frame_over = 1; r->winner = win ? r->turn : (1 - r->turn);
            snprintf(r->msg, sizeof r->msg, win ? "FRAME WON!" : "FOUL ON 8");
            return;
        }
    }

    if (r->open && !foul && !r->break_shot && (low || high)) {  /* assign */
        int g = (low && !high) ? 1 : (high && !low) ? 2 : pool_group(first_hit);
        if (g == 1 || g == 2) { r->group[r->turn] = g; r->group[1-r->turn] = (g==1)?2:1; r->open = 0; }
    }

    if (foul) {
        if (r->mode == CUE_GAME_US8) {
            /* US 8-ball: any foul → opponent gets ball-in-hand anywhere. */
            r->turn = 1 - r->turn; r->ball_in_hand = 1;
            r->two_shot = 0; r->shots_remaining = 1; r->free_shot = 0;
            snprintf(r->msg, sizeof r->msg, "FOUL: %s", why);
        } else {
            /* UK two-shot rule: opponent gets two visits; the cue ball stays put
             * unless it was potted (scratch → ball in hand behind the line). */
            r->turn = 1 - r->turn;
            r->two_shot = 1; r->shots_remaining = 2; r->free_shot = 1;
            r->ball_in_hand = scratch ? 1 : 0;
            snprintf(r->msg, sizeof r->msg, "FOUL: %s", why);   /* HUD shows 2 SHOTS */
        }
    } else if (legal_pot) {
        /* potting your own ball cancels any two-shot advantage carried in */
        r->two_shot = 0; r->shots_remaining = 1; r->free_shot = 0;
        r->msg[0] = 0;                              /* same player continues */
    } else if (r->shots_remaining > 1) {
        /* missed but still holding a shot from the carry — play on, same player */
        r->shots_remaining--; r->free_shot = 0;
        snprintf(r->msg, sizeof r->msg, "2ND SHOT");
    } else {
        r->turn = 1 - r->turn;
        r->two_shot = 0; r->shots_remaining = 1; r->free_shot = 0;
        r->msg[0] = 0;
    }
    r->break_shot = 0;
}

/* ---- snooker --------------------------------------------------------- */
static int snk_on(const CueRules *r, int id) {
    if (r->target == 0) return is_red(id);
    if (r->target == 1) return is_colour(id);
    return is_colour(id) && snk_value(id) == r->seq;   /* clearance */
}

static void resolve_snooker(CueRules *r, CueBall *b, int n, int first_hit,
                            int scratch, const int *potted, int np) {
    int target_before = r->target;
    int legal_pots = 0, illegal_pot = 0, maxpot = 0, reds_potted = 0;
    for (int k = 0; k < np; k++) {
        if (snk_on(r, potted[k])) legal_pots += snk_value(potted[k]);
        else illegal_pot = 1;
        if (snk_value(potted[k]) > maxpot) maxpot = snk_value(potted[k]);
        if (is_red(potted[k])) reds_potted++;
    }
    int foul = 0;
    if (scratch || first_hit < 0 || !snk_on(r, first_hit) || illegal_pot) foul = 1;

    /* respot every potted colour unless it was legally cleared in sequence */
    for (int k = 0; k < np; k++)
        if (is_colour(potted[k]) && (foul || target_before != 2))
            respot_colour(r, b, n, potted[k]);

    if (foul) {
        int fv = 4;
        int tv = (target_before == 2) ? r->seq : 1;
        if (tv > fv) fv = tv;
        if (first_hit >= 0 && snk_value(first_hit) > fv) fv = snk_value(first_hit);
        if (maxpot > fv) fv = maxpot;
        r->score[1 - r->turn] += fv;
        r->brk = 0;
        if (scratch) r->ball_in_hand = 1;
        r->turn = 1 - r->turn;
        r->target = (r->reds_left > 0) ? 0 : 2;
        if (r->target == 2 && r->seq < 2) r->seq = 2;
        snprintf(r->msg, sizeof r->msg, "FOUL +%d", fv);
        return;
    }

    /* legal */
    r->reds_left -= reds_potted;
    r->score[r->turn] += legal_pots;
    r->brk += legal_pots;

    if (target_before == 0) {                 /* was on a red */
        if (reds_potted > 0) r->target = 1;   /* now a colour */
    } else if (target_before == 1) {          /* was on a colour */
        if (r->reds_left > 0) r->target = 0;
        else { r->target = 2; r->seq = 2; }   /* clearance from yellow */
    } else {                                  /* clearance */
        if (legal_pots > 0) {
            r->seq++;
            if (r->seq > 7) {
                r->frame_over = 1;
                r->winner = (r->score[0] >= r->score[1]) ? 0 : 1;
                snprintf(r->msg, sizeof r->msg, "FRAME OVER");
                return;
            }
        }
    }

    if (legal_pots > 0) { snprintf(r->msg, sizeof r->msg, "BREAK %d", r->brk); }
    else {
        r->brk = 0; r->turn = 1 - r->turn;
        r->target = (r->reds_left > 0) ? 0 : 2;
        if (r->target == 2 && r->seq < 2) r->seq = 2;
        r->msg[0] = 0;
    }
}

void cue_rules_resolve(CueRules *r, CueBall *b, int n, const CueWorld *w,
                       int first_hit, int scratch, int cushion,
                       const int *potted, int np) {
    (void)w;
    r->ball_in_hand = 0;
    if (r->kind) resolve_snooker(r, b, n, first_hit, scratch, potted, np);
    else         resolve_pool(r, b, n, first_hit, scratch, cushion, potted, np);
}

int cue_rules_ball_legal(const CueRules *r, const CueBall *b, int n, int id) {
    if (id == CUE_ID_CUE) return 0;
    if (r->kind) return snk_on(r, id);
    if (r->open) return id != 8;                 /* open table: anything but the 8 */
    /* the 8 is legal ONLY once your own group is fully cleared */
    if (id == 8) return group_cleared(b, n, r->group[r->turn]);
    return pool_group(id) == r->group[r->turn];
}

void cue_rules_status(const CueRules *r, char *buf, int cap) {
    if (r->kind) {
        const char *on = r->target == 0 ? "RED" : r->target == 1 ? "COLOUR" :
            (r->seq == 2 ? "YELLOW" : r->seq == 3 ? "GREEN" : r->seq == 4 ? "BROWN" :
             r->seq == 5 ? "BLUE" : r->seq == 6 ? "PINK" : "BLACK");
        snprintf(buf, cap, "ON %s", on);
    } else {
        int g = r->group[r->turn];
        const char *grp = r->open ? "OPEN" : g == 1 ? "SOLIDS" : "STRIPES";
        if (r->shots_remaining > 1) snprintf(buf, cap, "%s  2 SHOTS", grp);
        else                        snprintf(buf, cap, "%s", grp);
    }
}
