/* Rules-driven AI validation: plays a full game calling cue_rules_resolve after
 * each shot (like cue_game.c), so red→colour alternation / group / lowest-ball
 * rules actually advance. Counts FOULS (illegal first contact / scratch) and
 * reports timing.
 *   gcc -O2 -Igame tools/aitest.c game/cue_ai.c game/cue_physics.c \
 *       game/cue_table.c game/cue_rules.c -lm -o /tmp/aitest && /tmp/aitest
 */
#include "cue_ai.h"
#include "cue_table.h"
#include "cue_physics.h"
#include "cue_rules.h"
#include <stdio.h>
#include <math.h>
#include <string.h>
#include <time.h>

extern void cue_phys_strike(const CueWorld*, CueBall*, Vec3, float, float, float);
extern int  cue_rules_ball_legal(const CueRules*, const CueBall*, int, int);

static long settle(CueWorld *w, CueBall *b, int n, int *fh, int *cush) {
    *fh = -1; *cush = 0; w->first_hit = -1; w->first_hit_idx = -1; w->_acc = 0;
    long steps = 0;
    for (int it = 0; it < 300; it++) {
        uint32_t ev = 0; cue_phys_step(w, b, n, 0.05f, &ev); steps++;
        if (ev & CUE_EV_CUSHION) *cush = 1;
        if (!cue_phys_moving(w, b, n)) break;
    }
    *fh = w->first_hit;
    return steps;
}

static void play(CueGameKind kind, const char *label, int persona, uint32_t seed) {
    CueTable t; cue_table_init(&t, kind);
    CueWorld w; cue_table_build_world(&t, &w);
    CueBall b[CUE_MAX_BALLS]; int n = cue_table_rack(&t, b);
    CueRules r; cue_rules_init(&r, &t, 1);
    const CuePersona *p = &CUE_PERSONAS[persona];
    uint32_t rng = seed;
    b[0].pos = cue_table_cue_home(&t); b[0].on = 1;

    int shots=0, pots=0, fouls=0, scratches=0, safes=0, illegal_first=0;
    long total_steps = 0; double plan_ms_acc = 0; clock_t t0 = clock();

    for (int turn = 0; turn < 80 && !r.frame_over; turn++) {
        int rem = 0; for (int i=1;i<n;i++) if (b[i].on) rem++;
        if (rem == 0) break;
        if (!b[0].on) { b[0].pos = cue_table_cue_home(&t); b[0].on=1; b[0].vel=v3(0,0,0); b[0].w=v3(0,0,0); }

        clock_t pt0 = clock();
        CueAIShot s = cue_ai_plan(&w, &t, &r, b, n, p, &rng);
        plan_ms_acc += 1000.0*(clock()-pt0)/CLOCKS_PER_SEC;
        if (!s.valid) break;
        shots++; if (s.safe) safes++;

        /* snapshot legality of the planned first target is implicit; we judge the
         * ACTUAL first contact after the shot. */
        int was_on[CUE_MAX_BALLS]; for (int i=0;i<n;i++) was_on[i]=b[i].on;
        CueBall b0[CUE_MAX_BALLS]; memcpy(b0, b, sizeof b0);   /* pre-shot snapshot */

        cue_phys_strike(&w, &b[0], v3(cosf(s.aim),0,sinf(s.aim)), s.power01*8.5f, s.tip_side, s.tip_vert);
        int fh, cush; total_steps += settle(&w, b, n, &fh, &cush);

        int bef=0; for (int i=1;i<n;i++) if (was_on[i]) bef++;
        int aft=0; for (int i=1;i<n;i++) if (b[i].on) aft++;
        pots += bef-aft;
        int scratch = !b[0].on; if (scratch) scratches++;

        /* foul: illegal/no first contact (judged against the PRE-shot table) */
        int fh_legal = (fh >= 0) && cue_rules_ball_legal(&r, b0, n, fh);
        if (!fh_legal) { illegal_first++; }

        int potted[CUE_MAX_BALLS], np=0;
        for (int i=1;i<n;i++) if (was_on[i] && !b[i].on) potted[np++]=b[i].id;
        int target_before = r.target;
        cue_rules_resolve(&r, b, n, &w, fh, scratch, cush, potted, np);
        if (r.ball_in_hand) fouls++;   /* rules flagged a foul (ball-in-hand awarded) */

        if (turn < 16)
            printf("  %s #%2d %s pow=%.2f fh=%d(%s) potted=%d %s%s\n",
                label, shots, s.safe?"SAFE":"POT ", s.power01, fh,
                fh_legal?"legal":"ILLEGAL", bef-aft,
                scratch?"SCRATCH ":"", r.ball_in_hand?"FOUL":"");
        (void)target_before;
    }
    printf("%s [%s]: %d shots, %d pots, %d ILLEGAL-first, %d scratch, %d foul, %d safe | PLAN %.1fms/shot\n\n",
        label, p->name, shots, pots, illegal_first, scratches, fouls, safes, shots? plan_ms_acc/shots:0);
    (void)total_steps; (void)t0;
}

int main(void) {
    printf("=== The Machine, rules-driven ===\n");
    play(CUE_GAME_UK8,  "UK8 ", 7, 0x1001);
    play(CUE_GAME_US8,  "US8 ", 7, 0x1002);
    play(CUE_GAME_US9,  "US9 ", 7, 0x1003);
    play(CUE_GAME_SNK6, "SNK6", 7, 0x1004);
    play(CUE_GAME_SNK15,"SNK ", 7, 0x1005);
    return 0;
}
