/*
 * ThumbyCue — game loop, camera and controls. See cue_game.h.
 *
 * v1 turn loop: aim → backswing-timing strike → simulate → settle. Full
 * 8-ball / snooker rules land in a later pass (rules reference: ~/2dpool).
 */
#include "cue_game.h"
#include "cue_types.h"
#include "cue_physics.h"
#include "cue_table.h"
#include "cue_render.h"
#include "craft_font.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

enum { GS_AIM = 0, GS_BACKSWING, GS_SHOOTING };

#define MAX_STRIKE_SPEED 6.0f
#define DEG2RAD (3.14159265f / 180.0f)

static CueTable  s_table;
static CueWorld  s_world;
static CueBall   s_balls[CUE_MAX_BALLS];
static int       s_n;
static int       s_state;
static int       s_kind;          /* 0 pool 1 snooker */

static float s_aim;               /* aim azimuth (rad) */
static float s_view_az;           /* camera orbit azimuth (rad) */
static float s_aim_hold;          /* how long the aim d-pad has been held (s) */
static int   s_aim_dir;           /* last aim direction (+1/-1/0) */
static float s_cam_elev = 0.34f;  /* aim-cam height (m) */
static float s_cam_dist = 0.62f;  /* aim-cam back distance (m) */
static int   s_overhead;
static float s_power;             /* 0..1 */
static float s_tip_side, s_tip_vert;
static int   s_potted;
static CraftRawButtons s_prev;
static float s_frame_ms;
static uint32_t s_settle_flash;   /* event bits from last shot */

static void rerack(void) {
    cue_table_init(&s_table, s_kind ? CUE_GAME_SNOOKER : CUE_GAME_POOL);
    cue_table_build_world(&s_table, &s_world);
    s_n = cue_table_rack(&s_table, s_balls);
    cue_render_build_table(&s_table, &s_world);
    s_state = GS_AIM;
    s_aim = 0.0f;
    s_power = 0; s_tip_side = s_tip_vert = 0;
    s_potted = 0;
}

void cue_game_set_kind(int snooker) { s_kind = snooker ? 1 : 0; rerack(); }

void cue_game_init(uint32_t seed) {
    (void)seed;
    s_kind = 0;
    rerack();
}

void cue_game_set_frame_ms(float ms) { s_frame_ms = ms; }

/* Lay out individual balls spread on the cloth so the ball shading can be
 * judged like the reference photo (white, a red, and the colours). */
void cue_game_debug_spread(void) {
    s_kind = 1;
    cue_table_init(&s_table, CUE_GAME_SNOOKER);
    cue_table_build_world(&s_table, &s_world);
    cue_render_build_table(&s_table, &s_world);
    memset(s_balls, 0, sizeof(s_balls));
    float R = s_table.R;
    const int ids[8] = { CUE_ID_CUE, 1, CUE_ID_YELLOW, CUE_ID_GREEN,
                         CUE_ID_BROWN, CUE_ID_BLUE, CUE_ID_PINK, CUE_ID_BLACK };
    int n = 0;
    for (int i = 0; i < 8; i++) {
        int row = i / 4, col = i % 4;
        float x = (col - 1.5f) * 7.0f * R;
        float z = (row - 0.5f) * 7.0f * R;
        CueBall *b = &s_balls[n++];
        b->pos = v3(x, R, z); b->orient = m3_identity();
        b->on = 1; b->id = (uint8_t)ids[i];
    }
    s_n = n;
    s_state = GS_AIM;
}

static Vec3 cue_pos(void) {
    return s_balls[0].on ? s_balls[0].pos : cue_table_cue_home(&s_table);
}

/* clamp the tip offset to the miscue circle (~0.5 R from centre). */
static void clamp_tip(void) {
    float r = sqrtf(s_tip_side * s_tip_side + s_tip_vert * s_tip_vert);
    const float lim = 0.5f;
    if (r > lim) { s_tip_side *= lim / r; s_tip_vert *= lim / r; }
}

static float s_menu_ms;

void cue_game_tick(const CraftRawButtons *b, float dt) {
    int jp_lb = b->lb && !s_prev.lb;

    /* MENU: tap = re-rack the same game; hold (>0.5s) = switch pool/snooker. */
    if (b->menu) {
        s_menu_ms += dt * 1000.0f;
    } else if (s_prev.menu) {            /* just released */
        if (s_menu_ms >= 500.0f) s_kind ^= 1;
        rerack();
        s_menu_ms = 0.0f;
        s_prev = *b;
        return;
    }
    if (jp_lb) s_overhead ^= 1;
    int jr_a = !b->a && s_prev.a;                 /* A released this frame */
    /* RB held = fine aim (precision); otherwise normal speed. Kept slow —
     * a cue stroke is a small angular adjustment. */
    /* Aim direction this frame (LEFT swings the aim left = +az). */
    int adir = (b->left && !b->right) ? +1 : (b->right && !b->left) ? -1 : 0;

    if (s_state == GS_AIM || s_state == GS_BACKSWING) {
        if (b->b) {
            /* spin mode: d-pad moves the tip contact point on the cue ball */
            if (b->left)  s_tip_side -= 1.5f * dt;
            if (b->right) s_tip_side += 1.5f * dt;
            if (b->up)    s_tip_vert += 1.5f * dt;
            if (b->down)  s_tip_vert -= 1.5f * dt;
            clamp_tip();
            s_aim_hold = 0; s_aim_dir = 0;
        } else {
            /* Aim with an acceleration ramp: starts slow for a fine nudge,
             * speeds up the longer you hold for big swings. RB = constant fine. */
            if (adir != 0 && adir == s_aim_dir) s_aim_hold += dt;
            else s_aim_hold = (adir ? dt : 0.0f);
            s_aim_dir = adir;
            float rate;
            if (b->rb) {
                rate = 0.09f;                       /* fine: constant slow */
            } else {
                float tr = s_aim_hold / 0.7f; if (tr > 1.0f) tr = 1.0f;
                rate = 0.16f + (1.30f - 0.16f) * tr * tr;   /* ease-in accel */
            }
            s_aim += adir * rate * dt;
            if (s_state == GS_BACKSWING) {
                /* draw the cue back (power) / push it in */
                if (b->down) s_power += 0.85f * dt;
                if (b->up)   s_power -= 0.85f * dt;
                if (s_power < 0) s_power = 0;
                if (s_power > 1) s_power = 1;
            } else {
                /* camera elevation */
                if (b->up)   s_cam_elev += 0.4f * dt;
                if (b->down) s_cam_elev -= 0.4f * dt;
                if (s_cam_elev < 0.10f) s_cam_elev = 0.10f;
                if (s_cam_elev > 0.9f)  s_cam_elev = 0.9f;
            }
        }
    }

    /* while aiming, the camera sits behind the aim line */
    if (s_state == GS_AIM || s_state == GS_BACKSWING) s_view_az = s_aim;

    if (s_state == GS_AIM) {
        /* hold A to begin the backswing */
        if (b->a) { s_state = GS_BACKSWING; s_power = 0.0f; }
    } else if (s_state == GS_BACKSWING) {
        /* release A to strike (power = how far the cue was drawn back) */
        if (jr_a) {
            if (s_power > 0.04f) {
                Vec3 dir = v3(cosf(s_aim), 0, sinf(s_aim));
                if (!s_balls[0].on) {
                    s_balls[0].pos = cue_table_cue_home(&s_table);
                    s_balls[0].on = 1;
                }
                cue_phys_strike(&s_world, &s_balls[0], dir,
                                s_power * MAX_STRIKE_SPEED, s_tip_side, s_tip_vert);
                s_world._acc = 0.0f;
                s_state = GS_SHOOTING;
            } else {
                s_state = GS_AIM;     /* released with no draw: cancel */
            }
        }
    } else { /* GS_SHOOTING */
        /* free-orbit the follow camera so you can watch the balls; the camera
         * tracks the (moving) cue ball, d-pad spins/tilts around it. */
        if (!b->b) {
            if (b->left)  s_view_az += 1.1f * dt;
            if (b->right) s_view_az -= 1.1f * dt;
            if (b->up)    s_cam_elev += 0.5f * dt;
            if (b->down)  s_cam_elev -= 0.5f * dt;
            if (s_cam_elev < 0.10f) s_cam_elev = 0.10f;
            if (s_cam_elev > 0.9f)  s_cam_elev = 0.9f;
        }
        uint32_t ev = 0;
        int moving = cue_phys_step(&s_world, s_balls, s_n, dt, &ev);
        s_settle_flash |= ev;
        if (!moving) {
            /* scratch: respot the cue ball in hand. */
            if (!s_balls[0].on) {
                s_balls[0].on = 1;
                s_balls[0].pos = cue_table_cue_home(&s_table);
                s_balls[0].vel = v3(0, 0, 0);
                s_balls[0].w = v3(0, 0, 0);
            }
            /* count pots (object balls off the table). */
            int on = 0;
            for (int i = 1; i < s_n; i++) if (s_balls[i].on) on++;
            s_potted = (s_n - 1) - on;
            s_power = 0; s_tip_side = s_tip_vert = 0;
            /* keep the camera where the player left it during the shot — aim
             * from that angle, no snap-back jump. */
            s_aim = s_view_az;
            s_state = GS_AIM;
        }
    }
    s_prev = *b;
}

/* ---- debug camera (inspection screenshots) ----------------------------- */
static int  s_dbg = 0;
static Vec3 s_dbg_eye, s_dbg_tgt;
static float s_dbg_fov = 52.0f;
void cue_game_debug_cam(float ex, float ey, float ez,
                        float tx, float ty, float tz, float fov) {
    s_dbg = 1; s_dbg_eye = v3(ex, ey, ez); s_dbg_tgt = v3(tx, ty, tz);
    s_dbg_fov = fov;
}

/* ---- camera ------------------------------------------------------------ */
static void build_view(CueView *v) {
    if (s_dbg) {
        v->fov_deg = s_dbg_fov;
        Vec3 fwd = v3_norm(v3_sub(s_dbg_tgt, s_dbg_eye));
        Vec3 right = v3_norm(v3_cross(v3(0, 1, 0), fwd));
        Vec3 up = v3_cross(fwd, right);
        v->pos = s_dbg_eye;
        v->basis.r[0] = right; v->basis.r[1] = up; v->basis.r[2] = fwd;
        return;
    }
    v->fov_deg = 52.0f;
    Vec3 P = cue_pos();
    Vec3 dir = v3(cosf(s_view_az), 0, sinf(s_view_az));   /* camera orbit */
    if (s_overhead) {
        float focal = 64.0f / tanf(v->fov_deg * DEG2RAD * 0.5f);
        float ext = (s_table.half_len > s_table.half_wid)
                        ? s_table.half_len : s_table.half_wid;
        float H = focal * (ext + 0.12f) / 58.0f * s_cam_dist / 0.62f;
        if (H < ext * 1.6f) H = ext * 1.6f;
        v->pos = v3(0, H, 0);
        v->basis.r[0] = v3(1, 0, 0);   /* right = +X */
        v->basis.r[1] = v3(0, 0, 1);   /* up = +Z (screen) */
        v->basis.r[2] = v3(0, -1, 0);  /* forward = down */
    } else {
        Vec3 cam = v3(P.x - dir.x * s_cam_dist, s_table.R + s_cam_elev,
                      P.z - dir.z * s_cam_dist);
        Vec3 target = v3(P.x + dir.x * 0.20f, s_table.R, P.z + dir.z * 0.20f);
        Vec3 fwd = v3_norm(v3_sub(target, cam));
        Vec3 right = v3_norm(v3_cross(v3(0, 1, 0), fwd));
        Vec3 up = v3_cross(fwd, right);
        v->pos = cam;
        v->basis.r[0] = right;
        v->basis.r[1] = up;
        v->basis.r[2] = fwd;
    }
}

void cue_game_render_begin(void) {
    CueView v; build_view(&v);
    Vec3 dir = v3(cosf(s_aim), 0, sinf(s_aim));
    int aiming = !s_dbg && (s_state == GS_AIM || s_state == GS_BACKSWING);
    float pw = (s_state == GS_BACKSWING) ? s_power : 0.0f;
    cue_render_build(&v, s_balls, s_n, aiming, 0, dir, pw, aiming);
}

void cue_game_render(uint16_t *fb, int y0, int y1) {
    cue_render_raster(fb, y0, y1);
}

/* ---- HUD --------------------------------------------------------------- */
static void draw_spin_indicator(uint16_t *fb, int cx, int cy, int r) {
    for (int dy = -r; dy <= r; dy++)
        for (int dx = -r; dx <= r; dx++) {
            if (dx * dx + dy * dy > r * r) continue;
            int x = cx + dx, y = cy + dy;
            if ((unsigned)x >= CUE_FB_W || (unsigned)y >= CUE_FB_H) continue;
            fb[y * CUE_FB_W + x] = RGB565C(230, 230, 220);
        }
    int tx = cx + (int)(s_tip_side / 0.5f * r);
    int ty = cy - (int)(s_tip_vert / 0.5f * r);
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) {
            int x = tx + dx, y = ty + dy;
            if ((unsigned)x >= CUE_FB_W || (unsigned)y >= CUE_FB_H) continue;
            fb[y * CUE_FB_W + x] = RGB565C(210, 40, 40);
        }
}

void cue_game_draw_overlay(uint16_t *fb) {
    char buf[32];
    /* title / pots */
    craft_font_draw(fb, s_kind ? "SNOOKER" : "8-BALL", 3, 3, RGB565C(230,230,210));
    snprintf(buf, sizeof buf, "POTS %d", s_potted);
    craft_font_draw(fb, buf, 3, 11, RGB565C(200,220,255));

    /* power bar (left side) when drawing back */
    if (s_state == GS_BACKSWING) {
        int h = (int)(s_power * 60.0f);
        for (int y = 0; y < 62; y++) {
            int yy = 122 - y;
            uint16_t c = (y < h) ? ((y > 44) ? RGB565C(230,40,30)
                                  : (y > 26) ? RGB565C(230,170,30)
                                             : RGB565C(60,210,70))
                                 : RGB565C(40,40,48);
            fb[yy * CUE_FB_W + 3] = c; fb[yy * CUE_FB_W + 4] = c;
            fb[yy * CUE_FB_W + 5] = c;
        }
    }

    /* spin indicator (bottom-right) */
    draw_spin_indicator(fb, 116, 112, 9);

    /* state hint + frame time */
    const char *hint = (s_state == GS_SHOOTING) ? ""
                     : (s_state == GS_BACKSWING) ? "DN DRAW  RLS A FIRE"
                                                 : "HOLD A  RB FINE";
    craft_font_draw(fb, hint, 30, 119, RGB565C(180,180,180));
    int fps = (s_frame_ms > 0.1f) ? (int)(1000.0f / s_frame_ms + 0.5f) : 0;
    if (fps > 999) fps = 999;
    snprintf(buf, sizeof buf, "%dFPS", fps);
    craft_font_draw(fb, buf, 98, 3, RGB565C(150,150,160));
}
