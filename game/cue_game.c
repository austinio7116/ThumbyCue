/*
 * ThumbyCue — game shell: screens (title/menus/options/customize), the in-game
 * turn loop, camera and controls. Rules/scoring live in cue_rules.* and are
 * driven from the shot-resolve step here.
 */
#include "cue_game.h"
#include "cue_types.h"
#include "cue_physics.h"
#include "cue_table.h"
#include "cue_render.h"
#include "cue_rules.h"
#include "cue_audio.h"
#include "craft_font.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define MAX_STRIKE_SPEED 6.0f
#define DEG2RAD (3.14159265f / 180.0f)

/* Top-level screens. */
enum { SC_TITLE = 0, SC_MAIN, SC_PLAY, SC_OPTIONS, SC_CUSTOM, SC_GAME, SC_PAUSE, SC_OVER };
/* In-game sub-states. */
enum { GS_AIM = 0, GS_BACKSWING, GS_SHOOTING, GS_PLACE };

/* ---- options / settings ---------------------------------------------- */
static const uint16_t k_cloth[5] = {
    RGB565C(22,120,70), RGB565C(20,100,78), RGB565C(30,70,150),
    RGB565C(120,30,40), RGB565C(60,60,68),
};
static const char *k_cloth_name[5] = { "GREEN","TEAL","BLUE","CLARET","SLATE" };

static int s_kind;            /* CueGameKind: 0 UK8, 1 US8, 2 US9, 3 SNK10, 4 SNK15 */
static const char *k_mode_name[CUE_GAME_COUNT] = {
    "UK 8-BALL", "US 8-BALL", "US 9-BALL", "SNOOKER 10", "SNOOKER 15" };
static int s_cpu;             /* opponent: 0 = 2 player, 1 = CPU */
static int s_cloth_idx;
static int s_vol = 14;        /* 0..20 */

/* ---- world / table --------------------------------------------------- */
static CueTable  s_table;
static CueWorld  s_world;
static CueBall   s_balls[CUE_MAX_BALLS];
static int       s_n;
static CueRules  s_rules;

/* ---- control / camera ------------------------------------------------ */
static int   s_screen = SC_TITLE;
static int   s_state;             /* in-game sub-state */
static float s_aim, s_view_az, s_aim_hold;
static int   s_aim_dir;
static float s_cam_pitch  = 0.45f; /* 0 = low/level … 1 = high/steep */
static float s_cam_dist_z = 0.50f; /* 0 = far/wide … 1 = close/zoomed-in */
static int   s_overhead;
static Vec3  s_orbit_c;            /* frozen camera-orbit centre (freeview) */
static int   s_freeview;          /* shot cam: 0 = follow cue ball, 1 = free-roam */
static float s_power, s_tip_side, s_tip_vert;
static CraftRawButtons s_prev;
static float s_frame_ms;
static float s_menu_spin;         /* orbiting backdrop angle */
static int   s_cursor;            /* menu cursor */
static float s_msg_t;             /* status banner timer */

/* shot tracking (for rules) */
static int   s_first_hit;         /* id of first object ball the cue contacted */
static uint8_t s_was_on[CUE_MAX_BALLS];
static int   s_cushion_seen;      /* any cushion contact this shot */
static int   s_cpu_think;         /* CPU pre-shot delay frames */

static int jp(int cur, int prev) { return cur && !prev; }

/* ---- table / game setup ---------------------------------------------- */
static void rack(void) {
    cue_table_init(&s_table, (CueGameKind)s_kind);
    if (!s_table.is_snooker) s_table.cloth = k_cloth[s_cloth_idx];  /* pool felt choice */
    cue_table_build_world(&s_table, &s_world);
    s_n = cue_table_rack(&s_table, s_balls);
    cue_render_build_table(&s_table, &s_world);
}
static void new_frame(void) {
    rack();
    cue_rules_init(&s_rules, &s_table, s_cpu);
    s_state = GS_AIM;
    s_aim = 0; s_view_az = 0; s_power = 0; s_tip_side = s_tip_vert = 0;
    s_overhead = 0;
}
static Vec3 cue_pos(void) {
    return s_balls[0].on ? s_balls[0].pos : cue_table_cue_home(&s_table);
}

void cue_game_set_kind(int snooker) { s_kind = snooker ? CUE_GAME_SNK15 : CUE_GAME_UK8; new_frame(); s_screen = SC_GAME; }
void cue_game_set_mode(int mode) { if (mode < 0) mode = 0; if (mode >= CUE_GAME_COUNT) mode = CUE_GAME_COUNT-1; s_kind = mode; new_frame(); s_screen = SC_GAME; }
void cue_game_init(uint32_t seed) {
    cue_audio_init();
    (void)seed; s_kind = CUE_GAME_UK8; s_cpu = 1; s_screen = SC_TITLE;
    rack();   /* something to show behind the title */
}
void cue_game_set_frame_ms(float ms) { s_frame_ms = ms; }

/* ---- shot strike + resolve ------------------------------------------- */
static void begin_shot(void) {
    Vec3 dir = v3(cosf(s_aim), 0, sinf(s_aim));
    if (!s_balls[0].on) { s_balls[0].pos = cue_table_cue_home(&s_table); s_balls[0].on = 1; }
    cue_phys_strike(&s_world, &s_balls[0], dir, s_power * MAX_STRIKE_SPEED,
                    s_tip_side, s_tip_vert);
    s_world._acc = 0.0f;
    s_world.first_hit = -1;            /* physics records the cue's real first contact */
    s_orbit_c = cue_pos();
    s_freeview = 0;                    /* follow the cue ball by default */
    s_first_hit = -1; s_cushion_seen = 0;
    for (int i = 0; i < s_n; i++) s_was_on[i] = s_balls[i].on;
    cue_audio_sfx(CUE_SFX_STRIKE, s_power);
    s_state = GS_SHOOTING;
}

static void clamp_tip(void) {
    float r = sqrtf(s_tip_side*s_tip_side + s_tip_vert*s_tip_vert);
    if (r > 0.5f) { s_tip_side *= 0.5f/r; s_tip_vert *= 0.5f/r; }
}

/* ---- simple CPU: pick a legal target, aim at its ghost toward a pocket - */
static void cpu_plan(void) {
    Vec3 cue = cue_pos();
    float best = -1e9f; float best_aim = s_aim; float best_pow = 0.5f;
    for (int i = 1; i < s_n; i++) {
        if (!s_balls[i].on) continue;
        if (!cue_rules_ball_legal(&s_rules, s_balls, s_n, s_balls[i].id)) continue;
        Vec3 ob = s_balls[i].pos;
        for (int p = 0; p < s_world.npocket; p++) {
            Vec3 pk = s_world.pocket[p];
            Vec3 op = v3_norm(v3(pk.x-ob.x, 0, pk.z-ob.z));     /* ob -> pocket */
            Vec3 ghost = v3(ob.x - op.x*2*s_table.R, 0, ob.z - op.z*2*s_table.R);
            Vec3 ca = v3_norm(v3(ghost.x-cue.x, 0, ghost.z-cue.z));
            float aimdot = v3_dot(ca, op);                       /* cut angle quality */
            float dist = sqrtf((pk.x-ob.x)*(pk.x-ob.x)+(pk.z-ob.z)*(pk.z-ob.z));
            float score = aimdot*3.0f - dist;
            if (aimdot > 0.2f && score > best) {
                best = score; best_aim = atan2f(ca.z, ca.x);
                best_pow = 0.45f + dist*0.20f; if (best_pow>0.9f) best_pow=0.9f;
            }
        }
    }
    s_aim = best_aim; s_view_az = best_aim;
    s_power = best_pow; s_tip_side = s_tip_vert = 0;
}

/* ---- in-game tick ---------------------------------------------------- */
static void ingame_tick(const CraftRawButtons *b, float dt) {
    int jr_a = !b->a && s_prev.a;
    int adir = (b->left && !b->right) ? +1 : (b->right && !b->left) ? -1 : 0;
    if (jp(b->lb, s_prev.lb)) s_overhead ^= 1;

    int cpu_turn = (s_cpu && s_rules.turn == 1);

    if (s_state == GS_PLACE) {
        if (cpu_turn) {            /* CPU places its own ball (home spot) and shoots */
            s_state = GS_AIM;
            return;
        }
        /* ball-in-hand: move the cue ball relative to the CAMERA view (UP =
         * away into the screen, RIGHT = screen-right), not world axes — else the
         * d-pad feels mixed up whenever the camera has rotated. */
        float az = s_view_az;
        float fx = cosf(az), fz = sinf(az);     /* camera forward (into screen) */
        float rx = sinf(az), rz = -cosf(az);    /* camera right */
        float mf = (b->up ? 1.0f : 0.0f) - (b->down ? 1.0f : 0.0f);
        float mr = (b->right ? 1.0f : 0.0f) - (b->left ? 1.0f : 0.0f);
        float sp = 0.4f * dt;
        s_balls[0].pos.x += (fx*mf + rx*mr) * sp;
        s_balls[0].pos.z += (fz*mf + rz*mr) * sp;
        float lim = s_table.half_wid - s_table.R;
        if (s_balls[0].pos.z >  lim) s_balls[0].pos.z =  lim;
        if (s_balls[0].pos.z < -lim) s_balls[0].pos.z = -lim;
        float lx = s_table.half_len - s_table.R;
        if (s_balls[0].pos.x >  lx) s_balls[0].pos.x =  lx;
        if (s_balls[0].pos.x < -lx) s_balls[0].pos.x = -lx;
        if (jp(b->a, s_prev.a)) s_state = GS_AIM;
        s_view_az = s_aim;
        return;
    }

    if ((s_state == GS_AIM || s_state == GS_BACKSWING) && !cpu_turn) {
        if (b->b) {
            if (b->left)  s_tip_side -= 1.5f*dt;
            if (b->right) s_tip_side += 1.5f*dt;
            if (b->up)    s_tip_vert += 1.5f*dt;
            if (b->down)  s_tip_vert -= 1.5f*dt;
            clamp_tip(); s_aim_hold = 0; s_aim_dir = 0;
        } else {
            if (adir && adir == s_aim_dir) s_aim_hold += dt;
            else s_aim_hold = adir ? dt : 0;
            s_aim_dir = adir;
            float rate = b->rb ? 0.022f      /* RB = ultra-fine aim (~1.3°/s) */
                       : (0.16f + 1.14f * (s_aim_hold>0.7f?1.0f:(s_aim_hold/0.7f)*(s_aim_hold/0.7f)));
            s_aim += adir * rate * dt;
            if (s_state == GS_BACKSWING) {
                if (b->down) s_power += 0.85f*dt;
                if (b->up)   s_power -= 0.85f*dt;
                if (s_power<0) s_power=0; if (s_power>1) s_power=1;
            } else if (b->rb) {               /* hold RB + UP/DOWN = zoom (at current pitch) */
                if (b->up)   s_cam_dist_z += 0.6f*dt;
                if (b->down) s_cam_dist_z -= 0.6f*dt;
                if (s_cam_dist_z<0.0f) s_cam_dist_z=0.0f;
                if (s_cam_dist_z>1.0f) s_cam_dist_z=1.0f;
            } else {                          /* UP/DOWN = camera pitch */
                if (b->up)   s_cam_pitch += 0.5f*dt;
                if (b->down) s_cam_pitch -= 0.5f*dt;
                if (s_cam_pitch<0.0f) s_cam_pitch=0.0f;
                if (s_cam_pitch>1.0f) s_cam_pitch=1.0f;
            }
        }
        s_view_az = s_aim;
    }

    if (cpu_turn && s_state == GS_AIM) {
        if (s_cpu_think == 0) cpu_plan();
        s_cpu_think++;
        if (s_cpu_think > 40) { s_cpu_think = 0; begin_shot(); }
        return;
    }

    if (s_state == GS_AIM) {
        if (b->a) { s_state = GS_BACKSWING; s_power = 0; }
    } else if (s_state == GS_BACKSWING) {
        if (jr_a) {
            if (s_power > 0.04f) begin_shot();
            else s_state = GS_AIM;
        }
    } else if (s_state == GS_SHOOTING) {
        /* A taps toggle FREEVIEW: stop following the cue ball and roam freely
         * (orbit/pitch/zoom) from the ball's current spot for the rest of the shot. */
        if (jp(b->a, s_prev.a)) { s_freeview ^= 1; if (s_freeview) s_orbit_c = cue_pos(); }
        if (!b->b) {       /* orbit / pitch / zoom (works in follow + freeview) */
            if (b->left)  s_view_az += 1.1f*dt;
            if (b->right) s_view_az -= 1.1f*dt;
            if (b->rb) {                       /* RB + UP/DOWN = zoom */
                if (b->up)   s_cam_dist_z += 0.6f*dt;
                if (b->down) s_cam_dist_z -= 0.6f*dt;
            } else {                           /* UP/DOWN = pitch */
                if (b->up)   s_cam_pitch += 0.5f*dt;
                if (b->down) s_cam_pitch -= 0.5f*dt;
            }
            if (s_cam_dist_z<0.0f) s_cam_dist_z=0.0f; if (s_cam_dist_z>1.0f) s_cam_dist_z=1.0f;
            if (s_cam_pitch<0.0f)  s_cam_pitch=0.0f;  if (s_cam_pitch>1.0f)  s_cam_pitch=1.0f;
        }
        /* loudness of impacts tracks the fastest ball this step → a hard clack
         * is loud, a gentle kiss soft, a slow ball trickling in pots softly. */
        float vmax = 0.0f;
        for (int i = 0; i < s_n; i++) {
            if (!s_balls[i].on) continue;
            float v2 = s_balls[i].vel.x*s_balls[i].vel.x + s_balls[i].vel.z*s_balls[i].vel.z;
            if (v2 > vmax) vmax = v2;
        }
        vmax = sqrtf(vmax);
        float hit_i = vmax / (MAX_STRIKE_SPEED * 0.55f);   /* normalise to 0..1 */
        if (hit_i > 1.0f) hit_i = 1.0f;
        uint32_t ev = 0;
        int moving = cue_phys_step(&s_world, s_balls, s_n, dt, &ev);
        if (ev & CUE_EV_BALL_HIT) cue_audio_sfx(CUE_SFX_CLACK, 0.25f + 0.75f*hit_i);
        if (ev & CUE_EV_CUSHION)  { cue_audio_sfx(CUE_SFX_CUSHION, 0.2f + 0.7f*hit_i); s_cushion_seen = 1; }
        if (ev & CUE_EV_POCKET)   cue_audio_sfx(CUE_SFX_POT, 0.2f + 0.7f*hit_i);
        /* the cue ball's true first object-ball contact, recorded by the physics */
        s_first_hit = s_world.first_hit;
        if (!moving) {
            /* gather what happened, hand to the rules engine */
            int potted[CUE_MAX_BALLS], np = 0, cue_scratch = !s_balls[0].on;
            for (int i = 1; i < s_n; i++)
                if (s_was_on[i] && !s_balls[i].on) potted[np++] = s_balls[i].id;
            cue_rules_resolve(&s_rules, s_balls, s_n, &s_world,
                              s_first_hit, cue_scratch, s_cushion_seen, potted, np);
            s_power = 0; s_tip_side = s_tip_vert = 0; s_aim = s_view_az;
            s_msg_t = 2.0f;
            if (s_rules.frame_over) { s_screen = SC_OVER; s_cursor = 0; }
            else if (s_rules.ball_in_hand) {
                s_balls[0].on = 1; s_balls[0].pos = cue_table_cue_home(&s_table);
                s_balls[0].vel = v3(0,0,0); s_balls[0].w = v3(0,0,0);
                s_state = GS_PLACE;
            } else s_state = GS_AIM;
        }
    }
}

/* ---- menu widget ----------------------------------------------------- */
static int menu_move(const CraftRawButtons *b, int n) {
    if (jp(b->down, s_prev.down)) { s_cursor = (s_cursor+1) % n; cue_audio_sfx(CUE_SFX_UI,0.3f); }
    if (jp(b->up,   s_prev.up))   { s_cursor = (s_cursor+n-1) % n; cue_audio_sfx(CUE_SFX_UI,0.3f); }
    if (jp(b->a, s_prev.a)) return s_cursor;
    return -1;
}

void cue_game_tick(const CraftRawButtons *b, float dt) {
    s_menu_spin += dt * 0.18f;
    if (s_msg_t > 0) s_msg_t -= dt;

    switch (s_screen) {
    case SC_TITLE:
        if (jp(b->a, s_prev.a)) { s_screen = SC_MAIN; s_cursor = 0; }
        break;
    case SC_MAIN: {
        int sel = menu_move(b, 3);
        if (sel == 0) { s_screen = SC_PLAY; s_cursor = 0; }
        else if (sel == 1) { s_screen = SC_OPTIONS; s_cursor = 0; }
        else if (sel == 2) { s_screen = SC_CUSTOM; s_cursor = 0; }
        break; }
    case SC_PLAY: {
        /* items: GAME, OPPONENT, START, (back via B) */
        menu_move(b, 3);
        if (s_cursor == 0 && jp(b->right,s_prev.right)) s_kind = (s_kind + 1) % CUE_GAME_COUNT;
        if (s_cursor == 0 && jp(b->left, s_prev.left))  s_kind = (s_kind + CUE_GAME_COUNT - 1) % CUE_GAME_COUNT;
        if (s_cursor == 1 && (jp(b->left,s_prev.left)||jp(b->right,s_prev.right))) s_cpu ^= 1;
        if (s_cursor == 2 && jp(b->a, s_prev.a)) { new_frame(); s_screen = SC_GAME; }
        if (jp(b->b, s_prev.b)) { s_screen = SC_MAIN; s_cursor = 0; }
        break; }
    case SC_OPTIONS: {
        menu_move(b, 2);
        int lr = (jp(b->right,s_prev.right)?1:0) - (jp(b->left,s_prev.left)?1:0);
        if (s_cursor == 0 && lr) { s_vol += lr; if(s_vol<0)s_vol=0; if(s_vol>20)s_vol=20; cue_audio_set_volume(s_vol); }
        if (s_cursor == 1 && lr) { s_cloth_idx = (s_cloth_idx + 5 + lr) % 5; }
        if (jp(b->b, s_prev.b)) { s_screen = SC_MAIN; s_cursor = 0; }
        break; }
    case SC_CUSTOM: {
        menu_move(b, 1);
        int lr = (jp(b->right,s_prev.right)?1:0) - (jp(b->left,s_prev.left)?1:0);
        if (s_cursor == 0 && lr) s_cloth_idx = (s_cloth_idx + 5 + lr) % 5;
        if (jp(b->b, s_prev.b)) { s_screen = SC_MAIN; s_cursor = 0; }
        break; }
    case SC_GAME:
        if (jp(b->menu, s_prev.menu)) { s_screen = SC_PAUSE; s_cursor = 0; }
        else ingame_tick(b, dt);
        break;
    case SC_PAUSE: {
        int sel = menu_move(b, 3);
        if (jp(b->menu, s_prev.menu) || jp(b->b,s_prev.b)) s_screen = SC_GAME;
        else if (sel == 0) s_screen = SC_GAME;             /* resume */
        else if (sel == 1) { new_frame(); s_screen = SC_GAME; } /* new frame */
        else if (sel == 2) { s_screen = SC_MAIN; s_cursor = 0; } /* quit to menu */
        break; }
    case SC_OVER:
        if (jp(b->a, s_prev.a)) { new_frame(); s_screen = SC_GAME; }
        if (jp(b->b, s_prev.b)) { s_screen = SC_MAIN; s_cursor = 0; }
        break;
    }
    cue_audio_tick(dt);
    s_prev = *b;
}

/* ---- debug camera (screenshots) -------------------------------------- */
static int  s_dbg = 0;
static Vec3 s_dbg_eye, s_dbg_tgt;
static float s_dbg_fov = 52.0f;
void cue_game_debug_cam(float ex,float ey,float ez,float tx,float ty,float tz,float fov){
    s_dbg=1; s_dbg_eye=v3(ex,ey,ez); s_dbg_tgt=v3(tx,ty,tz); s_dbg_fov=fov;
}
void cue_game_debug_spread(void) {
    s_kind = CUE_GAME_SNK15; rack(); memset(s_balls,0,sizeof s_balls);
    float R=s_table.R;
    const int ids[8]={CUE_ID_CUE,1,CUE_ID_YELLOW,CUE_ID_GREEN,CUE_ID_BROWN,CUE_ID_BLUE,CUE_ID_PINK,CUE_ID_BLACK};
    int n=0; for(int i=0;i<8;i++){int row=i/4,col=i%4; CueBall*bb=&s_balls[n++];
        bb->pos=v3((col-1.5f)*7*R,R,(row-0.5f)*7*R); bb->orient=m3_identity(); bb->on=1; bb->id=ids[i];}
    s_n=n; s_screen=SC_GAME; s_state=GS_AIM;
}

/* ---- camera ---------------------------------------------------------- */
static void build_view(CueView *v) {
    if (s_dbg) {
        v->fov_deg = s_dbg_fov;
        Vec3 fwd=v3_norm(v3_sub(s_dbg_tgt,s_dbg_eye));
        Vec3 right=v3_norm(v3_cross(v3(0,1,0),fwd)); Vec3 up=v3_cross(fwd,right);
        v->pos=s_dbg_eye; v->basis.r[0]=right; v->basis.r[1]=up; v->basis.r[2]=fwd;
        return;
    }
    v->fov_deg = 52.0f;
    int menu = (s_screen != SC_GAME && s_screen != SC_PAUSE);
    if (menu) {                            /* slow orbit backdrop */
        float ext = (s_table.half_len>s_table.half_wid)?s_table.half_len:s_table.half_wid;
        float d = ext*1.7f, hgt = ext*0.9f;
        Vec3 cam = v3(cosf(s_menu_spin)*d, hgt, sinf(s_menu_spin)*d);
        Vec3 fwd=v3_norm(v3_sub(v3(0,0,0),cam));
        Vec3 right=v3_norm(v3_cross(v3(0,1,0),fwd)); Vec3 up=v3_cross(fwd,right);
        v->pos=cam; v->basis.r[0]=right; v->basis.r[1]=up; v->basis.r[2]=fwd;
        return;
    }
    /* During a shot the camera FOLLOWS the cue ball; tapping A enters freeview,
     * where it orbits the frozen point and the player roams with the controls. */
    Vec3 P = (s_state == GS_SHOOTING && s_freeview) ? s_orbit_c : cue_pos();
    Vec3 dir = v3(cosf(s_view_az),0,sinf(s_view_az));
    if (s_overhead) {
        float focal=64.0f/tanf(v->fov_deg*DEG2RAD*0.5f);
        float ext=(s_table.half_len>s_table.half_wid)?s_table.half_len:s_table.half_wid;
        float H=focal*(ext+0.12f)/58.0f; if(H<ext*1.6f) H=ext*1.6f;
        v->pos=v3(0,H,0); v->basis.r[0]=v3(1,0,0); v->basis.r[1]=v3(0,0,1); v->basis.r[2]=v3(0,-1,0);
    } else {
        /* pitch and zoom are independent: UP/DOWN tilts (elevation), RB+UP/DOWN
         * dollies in/out at the current pitch. */
        float dist = 0.82f - 0.52f*s_cam_dist_z;   /* 0.82 m (far) … 0.30 m (close) */
        float elev = 0.12f + 0.52f*s_cam_pitch;    /* 0.12 m (low) … 0.64 m (high)  */
        Vec3 cam=v3(P.x-dir.x*dist, s_table.R+elev, P.z-dir.z*dist);
        Vec3 target=v3(P.x+dir.x*0.20f, s_table.R, P.z+dir.z*0.20f);
        Vec3 fwd=v3_norm(v3_sub(target,cam));
        Vec3 right=v3_norm(v3_cross(v3(0,1,0),fwd)); Vec3 up=v3_cross(fwd,right);
        v->pos=cam; v->basis.r[0]=right; v->basis.r[1]=up; v->basis.r[2]=fwd;
    }
}

void cue_game_render_begin(void) {
    CueView v; build_view(&v);
    Vec3 dir = v3(cosf(s_aim),0,sinf(s_aim));
    int aiming = !s_dbg && s_screen==SC_GAME && (s_state==GS_AIM || s_state==GS_BACKSWING || s_state==GS_PLACE);
    float pw = (s_state==GS_BACKSWING) ? s_power : 0.0f;
    cue_render_build(&v, s_balls, s_n, aiming, 0, dir, pw, aiming);
}
void cue_game_render(uint16_t *fb, int y0, int y1) { cue_render_raster(fb, y0, y1); }

/* ---- HUD / menus ----------------------------------------------------- */
static void center(uint16_t *fb, const char *s, int y, uint16_t c) {
    int w = craft_font_width(s); craft_font_draw(fb, s, 64 - w/2, y, c);
}
static void menu_list(uint16_t *fb, const char *const *items, int n, int cursor, int y0) {
    for (int i = 0; i < n; i++) {
        int y = y0 + i*10;
        uint16_t c = (i==cursor) ? RGB565C(255,240,120) : RGB565C(190,190,200);
        int w = craft_font_width(items[i]);
        if (i==cursor) craft_font_draw(fb, ">", 64 - w/2 - 8, y, c);
        craft_font_draw(fb, items[i], 64 - w/2, y, c);
    }
}
static void dim(uint16_t *fb, int amt) {            /* darken whole fb for overlays */
    for (int i = 0; i < CUE_FB_W*CUE_FB_H; i++) {
        uint16_t p = fb[i];
        int r=((p>>11)&31)*amt/16, g=((p>>5)&63)*amt/16, b=(p&31)*amt/16;
        fb[i] = (uint16_t)((r<<11)|(g<<5)|b);
    }
}

static void draw_spin_indicator(uint16_t *fb, int cx, int cy, int r) {
    for (int dy=-r;dy<=r;dy++) for(int dx=-r;dx<=r;dx++){
        if (dx*dx+dy*dy>r*r) continue; int x=cx+dx,y=cy+dy;
        if ((unsigned)x>=CUE_FB_W||(unsigned)y>=CUE_FB_H) continue;
        fb[y*CUE_FB_W+x]=RGB565C(230,230,220);
    }
    int tx=cx+(int)(s_tip_side/0.5f*r), ty=cy-(int)(s_tip_vert/0.5f*r);
    for (int dy=-1;dy<=1;dy++) for(int dx=-1;dx<=1;dx++){
        int x=tx+dx,y=ty+dy; if((unsigned)x>=CUE_FB_W||(unsigned)y>=CUE_FB_H) continue;
        fb[y*CUE_FB_W+x]=RGB565C(210,40,40);
    }
}

void cue_game_draw_overlay(uint16_t *fb) {
    char buf[40];
    switch (s_screen) {
    case SC_TITLE:
        dim(fb, 7);
        craft_font_draw_title(fb, "THUMBYCUE", 14, 40, 3,
                              RGB565C(255,250,210), RGB565C(210,150,40), RGB565C(20,20,20));
        center(fb, "SNOOKER & POOL", 70, RGB565C(200,220,255));
        if (((int)(s_menu_spin*2))&1) center(fb, "PRESS A", 96, RGB565C(240,240,160));
        break;
    case SC_MAIN: {
        dim(fb, 8);
        center(fb, "THUMBYCUE", 16, RGB565C(255,240,200));
        const char *it[3] = { "PLAY", "OPTIONS", "TABLE" };
        menu_list(fb, it, 3, s_cursor, 50);
        break; }
    case SC_PLAY: {
        dim(fb, 8);
        center(fb, "PLAY", 14, RGB565C(255,240,200));
        snprintf(buf,sizeof buf,"GAME  < %s >", k_mode_name[s_kind]);
        const char *it[3]; it[0]=buf;
        char obuf[24]; snprintf(obuf,sizeof obuf,"VS     < %s >", s_cpu?"CPU":"PLAYER 2");
        it[1]=obuf; it[2]="START";
        menu_list(fb, it, 3, s_cursor, 44);
        center(fb, "B BACK", 116, RGB565C(150,150,160));
        break; }
    case SC_OPTIONS: {
        dim(fb, 8);
        center(fb, "OPTIONS", 14, RGB565C(255,240,200));
        char vbuf[24]; snprintf(vbuf,sizeof vbuf,"VOLUME < %d >", s_vol);
        char cbuf[24]; snprintf(cbuf,sizeof cbuf,"FELT   < %s >", k_cloth_name[s_cloth_idx]);
        const char *it[2]={vbuf,cbuf};
        menu_list(fb, it, 2, s_cursor, 50);
        center(fb, "B BACK", 116, RGB565C(150,150,160));
        break; }
    case SC_CUSTOM: {
        dim(fb, 8);
        center(fb, "TABLE", 14, RGB565C(255,240,200));
        char cbuf[24]; snprintf(cbuf,sizeof cbuf,"FELT < %s >", k_cloth_name[s_cloth_idx]);
        const char *it[1]={cbuf};
        menu_list(fb, it, 1, s_cursor, 56);
        center(fb, "B BACK", 116, RGB565C(150,150,160));
        break; }
    case SC_PAUSE: {
        dim(fb, 7);
        center(fb, "PAUSED", 18, RGB565C(255,240,200));
        const char *it[3]={"RESUME","NEW FRAME","QUIT"};
        menu_list(fb, it, 3, s_cursor, 52);
        break; }
    case SC_OVER: {
        dim(fb, 6);
        center(fb, "FRAME OVER", 34, RGB565C(255,240,200));
        snprintf(buf,sizeof buf,"%s WINS", s_rules.winner==0?"PLAYER 1":(s_cpu?"CPU":"PLAYER 2"));
        center(fb, buf, 54, RGB565C(255,230,120));
        if (s_table.is_snooker) {
            snprintf(buf,sizeof buf,"%d - %d", s_rules.score[0], s_rules.score[1]);
            center(fb, buf, 68, RGB565C(200,220,255));
        }
        center(fb, "A REMATCH   B MENU", 100, RGB565C(180,180,190));
        break; }
    case SC_GAME: {
        /* in-game HUD */
        cue_rules_status(&s_rules, buf, sizeof buf);
        craft_font_draw(fb, buf, 3, 3, RGB565C(230,230,210));
        if (s_table.is_snooker) {
            char sb[24]; snprintf(sb,sizeof sb,"%d-%d", s_rules.score[0], s_rules.score[1]);
            craft_font_draw(fb, sb, 3, 11, RGB565C(200,220,255));
        }
        /* turn indicator */
        const char *who = s_rules.turn==0?"P1":(s_cpu?"CPU":"P2");
        craft_font_draw(fb, who, 3, 119, (s_rules.turn==0)?RGB565C(120,230,255):RGB565C(255,180,120));
        if (s_state == GS_BACKSWING) {
            int h=(int)(s_power*60.0f);
            for (int y=0;y<62;y++){ int yy=122-y;
                uint16_t c=(y<h)?((y>44)?RGB565C(230,40,30):(y>26)?RGB565C(230,170,30):RGB565C(60,210,70)):RGB565C(40,40,48);
                fb[yy*CUE_FB_W+3]=c; fb[yy*CUE_FB_W+4]=c; fb[yy*CUE_FB_W+5]=c; }
        }
        draw_spin_indicator(fb, 116, 112, 9);
        if (s_state == GS_PLACE) center(fb, "PLACE: DPAD  A SET", 119, RGB565C(240,240,160));
        else if (s_state == GS_SHOOTING) center(fb, s_freeview ? "FREEVIEW" : "A FREEVIEW", 119, RGB565C(150,200,150));
        else if (s_msg_t > 0 && s_rules.msg[0]) center(fb, s_rules.msg, 30, RGB565C(255,230,140));
        int fps=(s_frame_ms>0.1f)?(int)(1000.0f/s_frame_ms+0.5f):0; if(fps>999)fps=999;
        snprintf(buf,sizeof buf,"%dF", fps); craft_font_draw(fb, buf, 110, 3, RGB565C(140,140,150));
        break; }
    }
}
