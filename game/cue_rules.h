/*
 * ThumbyCue — rules & scoring for 8-ball and snooker. Ported (simplified) from
 * the 2D game's game.js. Driven by the shot-resolve step in cue_game.
 */
#ifndef CUE_RULES_H
#define CUE_RULES_H

#include "cue_physics.h"
#include "cue_table.h"

typedef struct {
    int kind;            /* 0 = 8-ball, 1 = snooker */
    int cpu;             /* player 1 (index 1) is the CPU */
    int turn;            /* 0 or 1 — whose shot */
    int score[2];        /* snooker points */
    int frame_over, winner;
    int ball_in_hand;    /* set on resolve; consumed by cue_game */
    char msg[24];

    /* 8-ball */
    int group[2];        /* 0 = open, 1 = low(1-7), 2 = high(9-15) */
    int open;
    int break_shot;
    int shots_remaining; /* UK two-shot rule: shots left in this visit (1 or 2) */
    int two_shot;        /* opponent is on the two-shot carry from a foul */
    int free_shot;       /* first of the two shots — informational */

    /* snooker */
    int target;          /* 0 = red, 1 = a colour, 2 = clearance sequence */
    int seq;             /* clearance: value of the colour on (2..7) */
    int reds_left;
    int brk;             /* current break points */
    Vec3 spot[8];        /* colour spots indexed by value 2..7 */
} CueRules;

void cue_rules_init(CueRules *r, const CueTable *t, int cpu);

/* Resolve a completed shot. balls[]/n is the post-shot table state (potted
 * balls have on=0). first_hit = id of the first object ball the cue contacted
 * (-1 if none). potted[] = ids potted this shot. May respot snooker colours
 * (sets balls[].on=1 + position). */
void cue_rules_resolve(CueRules *r, CueBall *balls, int n, const CueWorld *w,
                       int first_hit, int cue_scratch, int cushion_seen,
                       const int *potted, int npotted);

/* Is `id` a legal ball to go for right now (used by the CPU planner)? */
int  cue_rules_ball_legal(const CueRules *r, int id);

/* Short status line for the HUD (group / ball-on). */
void cue_rules_status(const CueRules *r, char *buf, int cap);

#endif
