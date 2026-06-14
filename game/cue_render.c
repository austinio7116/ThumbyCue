/*
 * ThumbyCue — scene renderer. See cue_render.h.
 */
#include "cue_render.h"
#include "cue_types.h"
#include "r3d_raster.h"
#include <math.h>
#include <string.h>

#define CUE_NEAR    0.05f
#define CUE_DEPTH_K (65535.0f * CUE_NEAR)

/* ---- static table mesh (world space) ---------------------------------- */
typedef struct { Vec3 v[3]; Vec3 nrm; uint16_t color; } CueTri;
#define MAX_TABLE_TRI 220
static CueTri   s_tab[MAX_TABLE_TRI];
static int      s_ntab;
static uint16_t s_cloth, s_bg_top, s_bg_bot;
static float    s_ballR = 0.0286f;

/* Pocket dark discs (drawn on the bed). */
static Vec3  s_pock[CUE_MAX_POCKET]; static float s_pock_r[CUE_MAX_POCKET];
static int   s_npock;

/* ---- per-frame projected lists ---------------------------------------- */
typedef struct { float x0,y0,x1,y1,x2,y2; uint16_t d0,d1,d2; uint16_t color; } STri;
static STri s_stri[MAX_TABLE_TRI]; static int s_nstri;

typedef struct { float cx, cy, rad, viewz; Mat3 orient; uint8_t id; } Sprite;
static Sprite s_spr[CUE_MAX_BALLS]; static int s_nspr;
static struct { float cx, cy, rad; uint16_t d; } s_shadow[CUE_MAX_BALLS];
static int s_nshadow;

#define MAX_DOTS 48
static struct { float x, y; uint16_t d; } s_dot[MAX_DOTS]; static int s_ndot;
static struct { float x0,y0,x1,y1; uint16_t color; int on; } s_cue;
static struct { float cx, cy, rad; uint16_t d; int on; } s_ghost;

/* View globals used by the per-pixel pass. */
static CueView s_view;
static float   s_focal;
static Vec3    s_light = { 0.35f, 0.92f, 0.18f };   /* toward the key light */

/* ---- helpers ----------------------------------------------------------- */
static inline uint16_t shade565(uint16_t c, float s) {
    if (s < 0) s = 0;
    if (s > 1.999f) s = 1.999f;
    int r = (int)(((c >> 11) & 0x1F) * s); if (r > 31) r = 31;
    int g = (int)(((c >> 5) & 0x3F) * s);  if (g > 63) g = 63;
    int b = (int)((c & 0x1F) * s);         if (b > 31) b = 31;
    return (uint16_t)((r << 11) | (g << 5) | b);
}
static inline uint16_t add565(uint16_t c, int ar, int ag, int ab) {
    int r = ((c >> 11) & 0x1F) + ar; if (r > 31) r = 31;
    int g = ((c >> 5) & 0x3F) + ag;  if (g > 63) g = 63;
    int b = (c & 0x1F) + ab;         if (b > 31) b = 31;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

int cue_render_project(Vec3 world, float *sx, float *sy, uint16_t *d) {
    Vec3 rel = v3_sub(world, s_view.pos);
    Vec3 vv = m3_mul_v3_t(&s_view.basis, rel);
    if (vv.z <= CUE_NEAR) return 0;
    float inv = 1.0f / vv.z;
    *sx = 64.0f + s_focal * vv.x * inv;
    *sy = 64.0f - s_focal * vv.y * inv;
    float dd = CUE_DEPTH_K * inv;
    *d = (dd >= 65535.0f) ? 65535u : (dd < 1.0f ? 1u : (uint16_t)dd);
    return 1;
}
/* project + return view-space z (for sphere radius / per-pixel depth). */
static int project_z(Vec3 world, float *sx, float *sy, float *vz) {
    Vec3 rel = v3_sub(world, s_view.pos);
    Vec3 vv = m3_mul_v3_t(&s_view.basis, rel);
    if (vv.z <= CUE_NEAR) return 0;
    float inv = 1.0f / vv.z;
    *sx = 64.0f + s_focal * vv.x * inv;
    *sy = 64.0f - s_focal * vv.y * inv;
    *vz = vv.z;
    return 1;
}

/* ---- table mesh build -------------------------------------------------- */
static void tri(Vec3 a, Vec3 b, Vec3 c, uint16_t col) {
    if (s_ntab >= MAX_TABLE_TRI) return;
    CueTri *t = &s_tab[s_ntab++];
    t->v[0] = a; t->v[1] = b; t->v[2] = c;
    t->nrm = v3_norm(v3_cross(v3_sub(b, a), v3_sub(c, a)));
    t->color = col;
}
static void quad(Vec3 a, Vec3 b, Vec3 c, Vec3 d, uint16_t col) {
    tri(a, b, c, col); tri(a, c, d, col);
}

void cue_render_build_table(const CueTable *t, const CueWorld *w) {
    s_ntab = 0;
    s_cloth = t->cloth;
    s_ballR = t->R;
    s_bg_top = RGB565C(26, 28, 40);
    s_bg_bot = RGB565C(8, 9, 16);
    const float hl = t->half_len, hw = t->half_wid;
    const float rw = t->rail_w, ch = t->cushion_h;

    /* Cloth bed (slightly inset so the cushion noses sit over its edge). */
    quad(v3(-hl, 0, -hw), v3(hl, 0, -hw), v3(hl, 0, hw), v3(-hl, 0, hw), t->cloth);

    /* Wood frame, four strips at cushion-top height (corners covered by the
     * full-width top/bottom strips). */
    uint16_t woodt = t->rail_top;
    quad(v3(-hl-rw, ch, hw), v3(hl+rw, ch, hw), v3(hl+rw, ch, hw+rw), v3(-hl-rw, ch, hw+rw), woodt);
    quad(v3(-hl-rw, ch, -hw-rw), v3(hl+rw, ch, -hw-rw), v3(hl+rw, ch, -hw), v3(-hl-rw, ch, -hw), woodt);
    quad(v3(-hl-rw, ch, -hw), v3(-hl, ch, -hw), v3(-hl, ch, hw), v3(-hl-rw, ch, hw), woodt);
    quad(v3(hl, ch, -hw), v3(hl+rw, ch, -hw), v3(hl+rw, ch, hw), v3(hl, ch, hw), woodt);

    /* Cushion rubber: a vertical inner face per nose segment (the part the
     * ball strikes), plus a small sloped top so it reads as 3D. */
    uint16_t rub = shade565(t->cloth, 0.78f);
    uint16_t rubtop = shade565(t->cloth, 1.05f);
    float ich = ch * 0.82f;             /* rubber slightly below the rail top */
    for (int s = 0; s < w->nseg; s++) {
        const CueSeg *sg = &w->seg[s];
        Vec3 a = v3(sg->a.x, 0, sg->a.z), b = v3(sg->b.x, 0, sg->b.z);
        Vec3 n = sg->n;                  /* inward */
        Vec3 a0 = a, b0 = b;
        Vec3 a1 = v3(a.x, ich, a.z), b1 = v3(b.x, ich, b.z);
        /* inner vertical face (faces inward = +n) */
        quad(a0, b0, b1, a1, rub);
        /* sloped cushion top from nose back to the rail */
        Vec3 ar = v3(a.x - n.x * rw, ch, a.z - n.z * rw);
        Vec3 br = v3(b.x - n.x * rw, ch, b.z - n.z * rw);
        quad(a1, b1, br, ar, rubtop);
    }

    /* Pocket discs. */
    s_npock = w->npocket;
    for (int p = 0; p < w->npocket; p++) {
        s_pock[p] = v3(w->pocket[p].x, ch + 0.001f, w->pocket[p].z);
        s_pock_r[p] = w->pocket_r[p] * 1.15f;
    }
}

/* ---- per-frame build --------------------------------------------------- */
static void add_stri(Vec3 a, Vec3 b, Vec3 c, Vec3 nrm, uint16_t base) {
    float ax, ay, bx, by, cx, cy; uint16_t da, db, dc;
    if (!cue_render_project(a, &ax, &ay, &da)) return;
    if (!cue_render_project(b, &bx, &by, &db)) return;
    if (!cue_render_project(c, &cx, &cy, &dc)) return;
    /* double-sided: order for positive screen area so r3d_tri won't cull. */
    float area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
    float ndl = v3_dot(nrm, s_light); if (ndl < 0) ndl = -ndl;
    float sh = 0.32f + 0.68f * ndl;
    uint16_t col = shade565(base, sh);
    if (s_nstri >= MAX_TABLE_TRI) return;
    STri *st = &s_stri[s_nstri++];
    st->color = col;
    if (area >= 0) {
        st->x0 = ax; st->y0 = ay; st->d0 = da;
        st->x1 = bx; st->y1 = by; st->d1 = db;
        st->x2 = cx; st->y2 = cy; st->d2 = dc;
    } else {
        st->x0 = ax; st->y0 = ay; st->d0 = da;
        st->x1 = cx; st->y1 = cy; st->d1 = dc;
        st->x2 = bx; st->y2 = by; st->d2 = db;
    }
}

void cue_render_build(const CueView *v, const CueBall *balls, int n,
                      int aim_active, int aim_ball, Vec3 aim_dir,
                      float power, int show_ghost) {
    s_view = *v;
    s_focal = 64.0f / tanf(v->fov_deg * (3.14159265f / 180.0f) * 0.5f);

    s_nstri = 0;
    for (int i = 0; i < s_ntab; i++)
        add_stri(s_tab[i].v[0], s_tab[i].v[1], s_tab[i].v[2],
                 s_tab[i].nrm, s_tab[i].color);

    /* Pocket discs as flat tri-fans on the rail surface. */
    for (int p = 0; p < s_npock; p++) {
        Vec3 c = s_pock[p]; float r = s_pock_r[p];
        for (int k = 0; k < 8; k++) {
            float a0 = k * 0.7853982f, a1 = (k + 1) * 0.7853982f;
            Vec3 p0 = v3(c.x + r * cosf(a0), c.y, c.z + r * sinf(a0));
            Vec3 p1 = v3(c.x + r * cosf(a1), c.y, c.z + r * sinf(a1));
            add_stri(c, p0, p1, v3(0, 1, 0), RGB565C(6, 8, 6));
        }
    }

    /* Balls → impostor sprites (+ shadow discs). */
    s_nspr = 0; s_nshadow = 0;
    for (int i = 0; i < n; i++) {
        const CueBall *b = &balls[i];
        if (!b->on) continue;
        float sx, sy, vz;
        if (!project_z(b->pos, &sx, &sy, &vz)) continue;
        float rad = s_focal * s_ballR / vz;
        if (rad < 0.7f) rad = 0.7f;
        if (s_nspr < CUE_MAX_BALLS) {
            Sprite *sp = &s_spr[s_nspr++];
            sp->cx = sx; sp->cy = sy; sp->rad = rad; sp->viewz = vz;
            sp->orient = b->orient; sp->id = b->id;
        }
        /* shadow: projected at the ball's contact point, flattened. */
        float shx, shy; uint16_t shd;
        if (cue_render_project(v3(b->pos.x + 0.4f * s_ballR, 0.0001f,
                                  b->pos.z), &shx, &shy, &shd)) {
            if (s_nshadow < CUE_MAX_BALLS) {
                s_shadow[s_nshadow].cx = shx; s_shadow[s_nshadow].cy = shy;
                s_shadow[s_nshadow].rad = rad * 0.95f;
                s_shadow[s_nshadow].d = shd; s_nshadow++;
            }
        }
    }

    /* Aim line, ghost ball, cue stick. */
    s_ndot = 0; s_cue.on = 0; s_ghost.on = 0;
    if (aim_active && aim_ball >= 0 && aim_ball < n && balls[aim_ball].on) {
        Vec3 cuepos = balls[aim_ball].pos;
        Vec3 dir = v3_norm(v3(aim_dir.x, 0, aim_dir.z));
        /* find nearest object ball along the ray (for the ghost). */
        float best = 1e9f; int hit = -1;
        for (int i = 0; i < n; i++) {
            if (i == aim_ball || !balls[i].on) continue;
            Vec3 d = v3_sub(balls[i].pos, cuepos); d.y = 0;
            float along = v3_dot(d, dir);
            if (along <= 0) continue;
            Vec3 perp = v3_sub(d, v3_scale(dir, along));
            float pd = sqrtf(perp.x * perp.x + perp.z * perp.z);
            if (pd < 2.0f * s_ballR && along < best) { best = along; hit = i; }
        }
        float linelen = (hit >= 0) ? best : 1.2f;
        int ndots = (int)(linelen / (s_ballR * 2.2f));
        if (ndots > MAX_DOTS) ndots = MAX_DOTS;
        for (int k = 1; k <= ndots; k++) {
            float dpos = k * s_ballR * 2.2f;
            Vec3 wp = v3(cuepos.x + dir.x * dpos, s_ballR, cuepos.z + dir.z * dpos);
            float dx, dy; uint16_t dd;
            if (cue_render_project(wp, &dx, &dy, &dd) && s_ndot < MAX_DOTS) {
                s_dot[s_ndot].x = dx; s_dot[s_ndot].y = dy; s_dot[s_ndot].d = dd;
                s_ndot++;
            }
        }
        if (show_ghost && hit >= 0) {
            Vec3 g = v3_sub(balls[hit].pos, v3_scale(dir, 2.0f * s_ballR));
            float gx, gy, gvz;
            if (project_z(v3(g.x, s_ballR, g.z), &gx, &gy, &gvz)) {
                s_ghost.cx = gx; s_ghost.cy = gy;
                s_ghost.rad = s_focal * s_ballR / gvz;
                uint16_t dd; float t1, t2;
                cue_render_project(v3(g.x, s_ballR, g.z), &t1, &t2, &dd);
                s_ghost.d = dd; s_ghost.on = 1;
            }
        }
        /* cue stick: behind the ball, pulled back by power. */
        float gap = 0.015f + power * 0.18f;
        Vec3 tip = v3(cuepos.x - dir.x * gap, s_ballR, cuepos.z - dir.z * gap);
        Vec3 butt = v3(tip.x - dir.x * 0.55f, s_ballR, tip.z - dir.z * 0.55f);
        float tx, ty, bx2, by2; uint16_t td, bd;
        if (cue_render_project(tip, &tx, &ty, &td) &&
            cue_render_project(butt, &bx2, &by2, &bd)) {
            s_cue.x0 = tx; s_cue.y0 = ty; s_cue.x1 = bx2; s_cue.y1 = by2;
            s_cue.color = RGB565C(210, 180, 110); s_cue.on = 1;
        }
    }
}

/* ---- ball texture ------------------------------------------------------ */
static uint16_t ball_base(uint8_t id) {
    switch (id) {
        case CUE_ID_CUE:    return RGB565C(245, 245, 235);
        case CUE_ID_YELLOW: return RGB565C(235, 200, 40);
        case CUE_ID_GREEN:  return RGB565C(20, 130, 50);
        case CUE_ID_BROWN:  return RGB565C(120, 70, 35);
        case CUE_ID_BLUE:   return RGB565C(30, 80, 200);
        case CUE_ID_PINK:   return RGB565C(235, 120, 150);
        case CUE_ID_BLACK:  return RGB565C(20, 20, 22);
    }
    /* pool: 1-7 solids, 8 black, 9-15 stripes (hue of id-8). */
    static const uint16_t hue[8] = {
        0, RGB565C(235,200,40), RGB565C(30,80,200), RGB565C(200,40,40),
        RGB565C(120,40,160), RGB565C(230,120,30), RGB565C(20,130,50),
        RGB565C(120,30,40) };
    if (id >= 1 && id <= 7) return hue[id];
    if (id == 8) return RGB565C(20, 20, 22);
    if (id >= 9 && id <= 15) return RGB565C(235, 235, 225); /* white body */
    return RGB565C(200, 40, 40);                            /* snooker red */
}
/* Sample the ball's surface colour for a ball-local unit normal. */
static uint16_t ball_sample(uint8_t id, Vec3 nb, uint16_t base) {
    /* Cue ball: a red spot so the spin is visible. */
    if (id == CUE_ID_CUE) {
        if (nb.x > 0.86f) return RGB565C(210, 40, 40);
        return base;
    }
    if (id >= 9 && id <= 15) {                  /* stripe band + number patch */
        if (fabsf(nb.y) < 0.45f) {
            static const uint16_t hue[8] = {
                0, RGB565C(235,200,40), RGB565C(30,80,200), RGB565C(200,40,40),
                RGB565C(120,40,160), RGB565C(230,120,30), RGB565C(20,130,50),
                RGB565C(120,30,40) };
            return hue[id - 8];
        }
        if (nb.x > 0.80f) return RGB565C(245, 245, 245);   /* number patch */
        return base;
    }
    if ((id >= 1 && id <= 8)) {                 /* solids + 8: white patch */
        if (nb.x > 0.80f) return RGB565C(245, 245, 245);
        return base;
    }
    return base;                                /* snooker plain colours */
}

/* ---- raster ------------------------------------------------------------ */
static void draw_ball(uint16_t *fb, uint16_t *depth, const Sprite *sp,
                      int y0, int y1) {
    int rad = (int)(sp->rad + 0.999f);
    float inv_rad = 1.0f / sp->rad;
    int icx = (int)(sp->cx + 0.5f), icy = (int)(sp->cy + 0.5f);
    uint16_t base = ball_base(sp->id);
    /* camera-to-surface dir (specular) and light, world space. */
    Vec3 vcam = v3_scale(s_view.basis.r[2], -1.0f);   /* toward camera */
    Vec3 H = v3_norm(v3_add(s_light, vcam));
    float R = s_ballR;
    for (int py = icy - rad; py <= icy + rad; py++) {
        if (py < y0 || py >= y1 || py < 0 || py >= CUE_FB_H) continue;
        float v = (py - sp->cy) * inv_rad;
        uint16_t *frow = fb + py * R3D_FB_W;
        uint16_t *drow = depth + py * R3D_FB_W;
        for (int px = icx - rad; px <= icx + rad; px++) {
            if (px < 0 || px >= CUE_FB_W) continue;
            float u = (px - sp->cx) * inv_rad;
            float rr = u * u + v * v;
            if (rr > 1.0f) continue;
            float nz = sqrtf(1.0f - rr);
            /* per-pixel depth: nearer than centre by R*nz. */
            float zpix = sp->viewz - R * nz;
            if (zpix < CUE_NEAR) zpix = CUE_NEAR;
            uint16_t d = (uint16_t)(CUE_DEPTH_K / zpix);
            if (d <= drow[px]) continue;
            /* view-space normal (screen y down → view up = -v). */
            Vec3 Nv = v3(u, -v, -nz);
            Vec3 Nw = m3_mul_v3(&s_view.basis, Nv);     /* view→world */
            float diff = v3_dot(Nw, s_light); if (diff < 0) diff = 0;
            float lum = 0.30f + 0.70f * diff;
            lum *= 0.78f + 0.22f * nz;                  /* gentle rim falloff */
            Vec3 Nb = m3_mul_v3_t(&sp->orient, Nw);     /* world→ball-local */
            uint16_t col = shade565(ball_sample(sp->id, Nb, base), lum);
            float s = v3_dot(Nw, H);
            if (s > 0.0f) {
                s *= s; s *= s; s *= s;                 /* ^8 highlight */
                int hi = (int)(s * 26.0f);
                if (hi > 0) col = add565(col, hi, hi * 2, hi);
            }
            frow[px] = col;
            drow[px] = d;
        }
    }
}

void cue_render_raster(uint16_t *fb, int y0, int y1) {
    if (y0 < 0) y0 = 0;
    if (y1 > CUE_FB_H) y1 = CUE_FB_H;
    if (y0 >= y1) return;
    r3d_raster_set_fb(fb);
    uint16_t *depth = r3d_depth_buffer();

    /* background vertical gradient + depth clear */
    for (int y = y0; y < y1; y++) {
        float t = (float)y / (float)CUE_FB_H;
        int r = (int)(((s_bg_top >> 11) & 31) * (1 - t) + ((s_bg_bot >> 11) & 31) * t);
        int g = (int)(((s_bg_top >> 5) & 63) * (1 - t) + ((s_bg_bot >> 5) & 63) * t);
        int b = (int)(((s_bg_top) & 31) * (1 - t) + ((s_bg_bot) & 31) * t);
        uint16_t c = (uint16_t)((r << 11) | (g << 5) | b);
        uint16_t *row = fb + y * R3D_FB_W;
        for (int x = 0; x < R3D_FB_W; x++) row[x] = c;
    }
    r3d_depth_clear(y0, y1);

    /* table triangles */
    for (int i = 0; i < s_nstri; i++) {
        const STri *t = &s_stri[i];
        r3d_tri(t->x0, t->y0, t->d0, t->x1, t->y1, t->d1,
                t->x2, t->y2, t->d2, t->color, y0, y1);
    }

    /* soft shadows (dark, depth-tested at cloth) */
    for (int i = 0; i < s_nshadow; i++) {
        int rad = (int)s_shadow[i].rad;
        int cx = (int)s_shadow[i].cx, cy = (int)s_shadow[i].cy;
        uint16_t d = s_shadow[i].d;
        for (int py = cy - rad / 2; py <= cy + rad / 2; py++) {
            if (py < y0 || py >= y1 || py < 0 || py >= CUE_FB_H) continue;
            float vv = (py - cy) / (0.55f * rad + 0.01f);
            uint16_t *frow = fb + py * R3D_FB_W;
            uint16_t *drow = depth + py * R3D_FB_W;
            for (int px = cx - rad; px <= cx + rad; px++) {
                if (px < 0 || px >= CUE_FB_W) continue;
                float uu = (px - cx) / (float)(rad + 0.01f);
                if (uu * uu + vv * vv > 1.0f) continue;
                if (d < drow[px]) continue;        /* behind table */
                frow[px] = shade565(frow[px], 0.55f);
            }
        }
    }

    /* aim dots (on the cloth) */
    for (int i = 0; i < s_ndot; i++)
        r3d_point((int)s_dot[i].x, (int)s_dot[i].y, 65000, RGB565C(240,240,160),
                  1, y0, y1);

    /* ghost-ball ring */
    if (s_ghost.on) {
        int seg = 18;
        for (int k = 0; k < seg; k++) {
            float a0 = k * (6.2831853f / seg), a1 = (k + 1) * (6.2831853f / seg);
            r3d_line(s_ghost.cx + s_ghost.rad * cosf(a0),
                     s_ghost.cy + s_ghost.rad * sinf(a0), 65000,
                     s_ghost.cx + s_ghost.rad * cosf(a1),
                     s_ghost.cy + s_ghost.rad * sinf(a1), 65000,
                     RGB565C(230, 230, 230), y0, y1);
        }
    }

    /* balls */
    for (int i = 0; i < s_nspr; i++) draw_ball(fb, depth, &s_spr[i], y0, y1);

    /* cue stick (over the balls for clarity) */
    if (s_cue.on) {
        r3d_line(s_cue.x0, s_cue.y0, 65000, s_cue.x1, s_cue.y1, 65000,
                 s_cue.color, y0, y1);
        /* a second offset line gives it thickness */
        r3d_line(s_cue.x0, s_cue.y0 + 1, 65000, s_cue.x1, s_cue.y1 + 1, 65000,
                 shade565(s_cue.color, 0.7f), y0, y1);
    }
}
