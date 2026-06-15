/* 8-ball rules regression: CPU must not target the 8 early; hitting your own
 * group when balls remain must NOT foul.
 *   gcc -O2 -I../game rulestest.c ../game/cue_rules.c ../game/cue_table.c \
 *       ../game/cue_physics.c -lm -o /tmp/rulestest && /tmp/rulestest
 */
#include "cue_rules.h"
#include "cue_table.h"
#include "cue_physics.h"
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("[FAIL] %s\n", msg); fails++; } \
                           else printf("[PASS] %s\n", msg); } while (0)

int main(void) {
    CueTable t; cue_table_init(&t, CUE_GAME_POOL);
    CueBall b[CUE_MAX_BALLS]; int n = cue_table_rack(&t, b);
    CueWorld w; cue_table_build_world(&t, &w);

    CueRules r; cue_rules_init(&r, &t, 1);
    /* simulate: groups assigned, player 0 = solids (1-7), player 1 = stripes */
    r.open = 0; r.break_shot = 0; r.turn = 0; r.group[0] = 1; r.group[1] = 2;

    /* leave solids 3,4,5,6,7 on table (1,2 potted); stripes all on; 8 on */
    for (int i = 0; i < n; i++) {
        int id = b[i].id;
        if (id == 1 || id == 2) b[i].on = 0;   /* two solids gone */
    }

    /* (1) the 8 is NOT a legal target while solids remain */
    CHECK(!cue_rules_ball_legal(&r, b, n, 8), "8 illegal while group has balls left");
    /* a remaining solid IS legal; a stripe is not */
    CHECK( cue_rules_ball_legal(&r, b, n, 3), "own group ball legal");
    CHECK(!cue_rules_ball_legal(&r, b, n, 11), "opponent group ball illegal");

    /* (2) hitting your own group first (no pot) must NOT foul */
    {
        CueRules rr = r; int potted[1]; int np = 0;
        cue_rules_resolve(&rr, b, n, &w, /*first_hit=*/3, /*scratch=*/0,
                          /*cushion=*/1, potted, np);
        CHECK(rr.msg[0] == 0 || strstr(rr.msg, "FOUL") == NULL,
              "hit own group, no pot -> no foul (turn passes)");
    }

    /* (3a) THE REPORTED BUG: one solid left, hit AND pot it this shot. The
     * group becomes empty only because of this shot, so it must NOT foul. */
    for (int i = 0; i < n; i++)            /* leave exactly one solid (id 7) */
        if (b[i].id >= 1 && b[i].id <= 6) b[i].on = 0;
    {
        CueRules rr = r;                   /* group 1, turn 0 */
        CueBall bb[CUE_MAX_BALLS]; memcpy(bb, b, sizeof bb);
        int pi = -1; for (int i = 0; i < n; i++) if (bb[i].id == 7) pi = i;
        bb[pi].on = 0;                     /* potted the last solid this shot */
        int potted[1] = { 7 }; int np = 1;
        cue_rules_resolve(&rr, bb, n, &w, /*first_hit=*/7, 0, 1, potted, np);
        CHECK(strstr(rr.msg, "FOUL") == NULL,
              "pot your LAST group ball -> legal, no must-hit-8 foul");
    }

    /* (3) clear all solids -> now the 8 is legal and required */
    for (int i = 0; i < n; i++) if (b[i].id >= 1 && b[i].id <= 7) b[i].on = 0;
    CHECK( cue_rules_ball_legal(&r, b, n, 8), "8 legal once group cleared");
    {
        CueRules rr = r; int potted[1]; int np = 0;
        cue_rules_resolve(&rr, b, n, &w, /*first_hit=*/11, 0, 1, potted, np);
        CHECK(strstr(rr.msg, "FOUL") != NULL,
              "group cleared, hit opponent/non-8 -> foul (must hit 8)");
    }

    /* ---- US 9-ball ---- */
    {
        CueTable t9; cue_table_init(&t9, CUE_GAME_US9);
        CueBall b9[CUE_MAX_BALLS]; int n9 = cue_table_rack(&t9, b9);
        CueWorld w9; cue_table_build_world(&t9, &w9);
        CueRules r9; cue_rules_init(&r9, &t9, 1);

        CHECK( cue_rules_ball_legal(&r9, b9, n9, 1), "9ball: lowest (1) is legal");
        CHECK(!cue_rules_ball_legal(&r9, b9, n9, 5), "9ball: a higher ball is illegal to hit first");

        /* hit the lowest, no pot, with a rail -> no foul, turn passes */
        { CueRules rr = r9; int pt[1]; int np = 0;
          cue_rules_resolve(&rr, b9, n9, &w9, 1, 0, 1, pt, np);
          CHECK(strstr(rr.msg,"FOUL")==NULL, "9ball: hit lowest, rail, no pot -> legal"); }
        /* hit a non-lowest first -> foul */
        { CueRules rr = r9; int pt[1]; int np = 0;
          cue_rules_resolve(&rr, b9, n9, &w9, 5, 0, 1, pt, np);
          CHECK(strstr(rr.msg,"FOUL")!=NULL, "9ball: hit wrong ball first -> foul"); }
        /* pot the 9 legally (1 still lowest, hit it first, 9 falls via combo) -> win */
        { CueRules rr = r9; CueBall bb[CUE_MAX_BALLS]; memcpy(bb,b9,sizeof bb);
          int pi=-1; for(int i=0;i<n9;i++) if(bb[i].id==9) pi=i; bb[pi].on=0;
          int pt[1]={9}; int np=1;
          cue_rules_resolve(&rr, bb, n9, &w9, 1, 0, 1, pt, np);
          CHECK(rr.frame_over && rr.winner==0, "9ball: pot the 9 legally -> win"); }
    }

    /* ---- snooker (10-red): reds tracked from the table; a foul-potted red must
     * still reduce the count, and the last red + colour must reach the clearance
     * (not stay stuck ON RED). ---- */
    {
        CueTable ts; cue_table_init(&ts, CUE_GAME_SNK10);
        CueBall bs[CUE_MAX_BALLS]; int ns = cue_table_rack(&ts, bs);
        CueWorld ws; cue_table_build_world(&ts, &ws);
        CueRules rs; cue_rules_init(&rs, &ts, 0);
        int cols[6] = { CUE_ID_YELLOW, CUE_ID_GREEN, CUE_ID_BROWN,
                        CUE_ID_BLUE, CUE_ID_PINK, CUE_ID_BLACK };
        #define POT(BID) do { for (int i=0;i<ns;i++) if (bs[i].id==(BID)) bs[i].on=0; \
            int _p[1]={(BID)}; cue_rules_resolve(&rs,bs,ns,&ws,(BID),0,1,_p,1); } while (0)
        POT(1);                 /* legal red -> ON COLOUR */
        POT(2);                 /* red potted while ON COLOUR = foul; red 2 is gone */
        int cnt = 0; for (int i=0;i<ns;i++) if (bs[i].on && bs[i].id>=1 && bs[i].id<=10) cnt++;
        CHECK(rs.reds_left == cnt && rs.reds_left == 8,
              "snooker: foul-potted red still reduces reds_left (table count)");
        for (int rr = 3; rr <= 10; rr++) { POT(rr); POT(cols[(rr-3)%6]); }
        CHECK(rs.target == 2,
              "snooker: last red + colour -> clearance (not stuck ON RED)");
        #undef POT
    }

    printf(fails ? "\n%d FAIL\n" : "\nALL PASS\n", fails);
    return fails ? 1 : 0;
}
