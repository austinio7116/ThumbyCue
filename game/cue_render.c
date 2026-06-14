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
#define MAX_TABLE_TRI 1500
#define MAX_STRI      2400     /* near-clipping can split a tri into two */
static CueTri   s_tab[MAX_TABLE_TRI];
static int      s_ntab;
static int      s_bed_ntab;   /* first s_bed_ntab tris are the flat cloth bed */
static uint16_t s_cloth, s_bg_top, s_bg_bot;
static uint16_t s_cloth_shadow;  /* dark cloth tint for ball shadow-side bounce */
static float    s_ballR = 0.0286f;
static int      s_is_snooker;   /* ids 1..15 mean reds, not solids/stripes */
static int      s_lip_mode = 1;  /* 0=none 1=tight 2=wide 3=deep (CUE_LIP env) */

/* ---- per-frame projected lists ---------------------------------------- */
typedef struct { float x0,y0,x1,y1,x2,y2; uint16_t d0,d1,d2; uint16_t color; } STri;
static STri s_stri[MAX_STRI]; static int s_nstri;
static int s_bed_nstri;   /* s_stri[0..s_bed_nstri) are the flat cloth bed */

typedef struct { float cx, cy, rad, viewz; Mat3 orient; uint8_t id; } Sprite;
static Sprite s_spr[CUE_MAX_BALLS]; static int s_nspr;
/* ground-plane shadow decal: centre + two screen-space axis vectors (the
 * projection of world +X and +Z offsets), so it foreshortens with the cloth */
static struct { float cx, cy, ux, uy, vx, vy; } s_shadow[CUE_MAX_BALLS];
static int s_nshadow;

#define MAX_DOTS 48
static struct { float x, y; uint16_t d; } s_dot[MAX_DOTS]; static int s_ndot;
static struct { float x, y; uint16_t d; } s_odot[MAX_DOTS]; static int s_nodot;
static struct { float x0,y0,x1,y1; uint16_t color; int on; } s_cue;
static struct { float cx, cy, rad; uint16_t d; int on; } s_ghost;

/* View globals used by the per-pixel pass. */
static CueView s_view;
static float   s_focal;
static Vec3    s_light = { 0.10f, 0.975f, 0.20f };  /* nearly overhead (snooker lamps) */

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
/* Blend a→b by t in [0,1]. */
static inline uint16_t mix565(uint16_t a, uint16_t b, float t) {
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    int ar = (a>>11)&0x1F, ag = (a>>5)&0x3F, ab = a&0x1F;
    int br = (b>>11)&0x1F, bg = (b>>5)&0x3F, bb = b&0x1F;
    int rr = ar + (int)((br-ar)*t), gg = ag + (int)((bg-ag)*t), bl = ab + (int)((bb-ab)*t);
    return (uint16_t)((rr<<11)|(gg<<5)|bl);
}

/* Ball lighting style (0=smooth/current, 1=hard spec, 2=toon, 3=gloss). */
static int s_light_mode = 1;
void cue_render_set_light_mode(int m) { s_light_mode = m; }

int cue_render_project(Vec3 world, float *sx, float *sy, uint16_t *d) {
    Vec3 rel = v3_sub(world, s_view.pos);
    Vec3 vv = m3_mul_v3_t(&s_view.basis, rel);
    if (vv.z <= CUE_NEAR) return 0;
    float inv = 1.0f / vv.z;
    *sx = 64.0f + s_focal * vv.x * inv;
    *sy = 64.0f - s_focal * vv.y * inv;
    if (d) {
        float dd = CUE_DEPTH_K * inv;
        *d = (dd >= 65535.0f) ? 65535u : (dd < 1.0f ? 1u : (uint16_t)dd);
    }
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
/* Ribbon quad a→b→c→d with a CHOSEN diagonal. The cushion strip is non-planar
 * (back verts use per-node normals), so the diagonal must follow the geometry,
 * not the vertex labels — otherwise a jaw renders mirror-broken on one side.
 * alt=0 splits a-c; alt=1 splits b-d. */
static void ribbon(Vec3 a, Vec3 b, Vec3 c, Vec3 d, uint16_t col) {
    /* Split along the SHORTER diagonal — a pure distance test, so mirrored
     * geometry triangulates identically (the jaw was broken on one side
     * because a fixed-label diagonal is not mirror-invariant). */
    if (v3_len2(v3_sub(b, d)) < v3_len2(v3_sub(a, c))) {
        tri(a, b, d, col); tri(b, c, d, col);
    } else {
        tri(a, b, c, col); tri(a, c, d, col);
    }
}

/* Baize lip (the drop): rolls the cloth down into each pocket throat. Emitted
 * AFTER the pocket voids so depth-test layers it OVER the void (no rim cutting
 * across it) while the raised cushions still occlude its sides. */
static void emit_pocket_lips(const CueTable *t, const CueWorld *w) {
    if (!s_lip_mode) return;
    int nb = w->njaw;
    for (int i = 0; i < nb; i++) {
        if (!(i & 1)) continue;                 /* only pocket-mouth edges */
        Vec3 a = v3(w->jaw[i].x, 0, w->jaw[i].z);
        Vec3 b = v3(w->jaw[(i + 1) % nb].x, 0, w->jaw[(i + 1) % nb].z);
        Vec3 m = v3((a.x + b.x) * 0.5f, 0, (a.z + b.z) * 0.5f);
        float ml = sqrtf(m.x * m.x + m.z * m.z);
        Vec3 din = (ml > 1e-5f) ? v3(-m.x / ml, 0, -m.z / ml) : v3(0, 0, 0);
        float chord = sqrtf((b.x-a.x)*(b.x-a.x) + (b.z-a.z)*(b.z-a.z));
        float blg = chord * 0.32f;
        Vec3 c = v3(m.x + din.x * blg * 2.0f, 0, m.z + din.z * blg * 2.0f);
        const int N = 6;
        Vec3 arc[N + 1]; arc[0] = a;
        for (int k = 1; k <= N; k++) {
            float tt = (float)k / N, o = 1.0f - tt;
            arc[k] = v3(o*o*a.x + 2*o*tt*c.x + tt*tt*b.x, 0,
                        o*o*a.z + 2*o*tt*c.z + tt*tt*b.z);
        }
        int pidx = 0; float bestp = 1e9f;
        for (int q = 0; q < w->npocket; q++) {
            float dx = w->pocket[q].x - m.x, dz = w->pocket[q].z - m.z;
            float dd = dx*dx + dz*dz;
            if (dd < bestp) { bestp = dd; pidx = q; }
        }
        Vec3 pc = w->pocket[pidx];
        float pr = (pidx < 4) ? t->pr_corner : t->pr_side;
        int M; float lw, ld;
        switch (s_lip_mode) {
            case 2:  M = 5; lw = 0.95f*pr; ld = 0.55f*pr; break;
            case 3:  M = 6; lw = 0.80f*pr; ld = 0.80f*pr; break;
            default: M = 4; lw = 0.60f*pr; ld = 0.45f*pr; break;
        }
        Vec3 ring0[N + 1]; for (int k = 0; k <= N; k++) ring0[k] = arc[k];
        for (int s = 1; s <= M; s++) {
            float phi = (float)s / M * 1.5707963f;
            float off = lw * sinf(phi), yy = -ld * (1.0f - cosf(phi));
            uint16_t col = shade565(t->cloth, 1.0f - 0.92f*(1.0f - cosf(phi)));
            Vec3 ring1[N + 1];
            for (int k = 0; k <= N; k++) {
                float dx = pc.x - arc[k].x, dz = pc.z - arc[k].z;
                float l = sqrtf(dx*dx + dz*dz) + 1e-6f;
                ring1[k] = v3(arc[k].x + dx/l*off, yy, arc[k].z + dz/l*off);
            }
            for (int k = 0; k < N; k++)
                quad(ring0[k], ring0[k+1], ring1[k+1], ring1[k], col);
            for (int k = 0; k <= N; k++) ring0[k] = ring1[k];
        }
    }
}

/* A rail-top wood band [xa,xb]×[za,zb] with a real circular hole cut out at
 * (hx,hz) radius hr — so the side-pocket void has NO wood inside it to peek
 * through at the seams (the thin rail-edge line / shoulder wedges). */
static void wood_band(float xa, float xb, float za, float zb,
                      float hx, float hz, float hr, float y, uint16_t col) {
    float lx = hx - hr, rx = hx + hr;
    if (lx > xa) quad(v3(xa,y,za), v3(lx,y,za), v3(lx,y,zb), v3(xa,y,zb), col);
    if (rx < xb) quad(v3(rx,y,za), v3(xb,y,za), v3(xb,y,zb), v3(rx,y,zb), col);
    float x0 = lx > xa ? lx : xa, x1 = rx < xb ? rx : xb;
    const int NS = 16; float r2 = hr * hr;
    for (int i = 0; i < NS; i++) {
        float sx0 = x0 + (x1-x0)*i/NS, sx1 = x0 + (x1-x0)*(i+1)/NS;
        float dx = 0.5f*(sx0+sx1) - hx;
        float h = (dx*dx < r2) ? sqrtf(r2 - dx*dx) : 0.0f;
        float zlo = hz - h, zhi = hz + h;
        if (zlo > za) quad(v3(sx0,y,za), v3(sx1,y,za), v3(sx1,y,zlo), v3(sx0,y,zlo), col);
        if (zhi < zb) quad(v3(sx0,y,zhi), v3(sx1,y,zhi), v3(sx1,y,zb), v3(sx0,y,zb), col);
    }
}

void cue_render_build_table(const CueTable *t, const CueWorld *w) {
    { extern char *getenv(const char*); const char *e = getenv("CUE_LIP"); if (e) s_lip_mode = e[0]-'0'; }
    s_ntab = 0;
    s_cloth = t->cloth;
    s_ballR = t->R;
    s_is_snooker = t->is_snooker;
    s_cloth_shadow = shade565(t->cloth, 0.42f);   /* cloth bounce tint */
    s_bg_top = RGB565C(24, 26, 36);
    s_bg_bot = RGB565C(6, 7, 12);
    const float hl = t->half_len, hw = t->half_wid;
    const float rw = t->rail_w;
    const float cw = rw * 0.42f;        /* cushion depth (nose → cushion back) */
    const float nose_h = t->cushion_h;  /* contact height */
    const float rail_h = t->cushion_h * 1.75f;
    uint16_t wood = t->rail, woodt = t->rail_top;

    /* Cloth bed — fanned from the centre over the knuckle boundary (w->jaw is
     * stored in boundary order), so the felt edge follows the cushion noses
     * and the pocket MOUTHS are real gaps (the angled jaws stay visible). */
    int nb = w->njaw;
    for (int i = 0; i < nb; i++) {
        Vec3 a = v3(w->jaw[i].x, 0, w->jaw[i].z);
        Vec3 b = v3(w->jaw[(i + 1) % nb].x, 0, w->jaw[(i + 1) % nb].z);
        /* Edges within a chain (i even) are the straight nose; edges ACROSS a
         * pocket (i odd) are the mouth — cut it as a CURVED arc bulging toward
         * the table so the pocket drop is rounded, not a straight chord. */
        if (i & 1) {
            Vec3 m = v3((a.x + b.x) * 0.5f, 0, (a.z + b.z) * 0.5f);
            float ml = sqrtf(m.x * m.x + m.z * m.z);
            Vec3 din = (ml > 1e-5f) ? v3(-m.x / ml, 0, -m.z / ml) : v3(0, 0, 0);
            float chord = sqrtf((b.x-a.x)*(b.x-a.x) + (b.z-a.z)*(b.z-a.z));
            float blg = chord * 0.32f;                 /* curved mouth arc (same for all) */
            Vec3 c = v3(m.x + din.x * blg * 2.0f, 0, m.z + din.z * blg * 2.0f);
            const int N = 6;
            Vec3 arc[N + 1]; arc[0] = a;
            for (int k = 1; k <= N; k++) {
                float tt = (float)k / N, o = 1.0f - tt;
                Vec3 p = v3(o*o*a.x + 2*o*tt*c.x + tt*tt*b.x, 0,
                            o*o*a.z + 2*o*tt*c.z + tt*tt*b.z);
                tri(v3(0, 0, 0), arc[k-1], p, t->cloth);
                arc[k] = p;
            }
            /* the baize lip (drop) is emitted AFTER the pocket voids — see
             * emit_pocket_lips() below — so the void can't draw its rim across it */
        } else {
            tri(v3(0, 0, 0), a, b, t->cloth);
        }
    }
    s_bed_ntab = s_ntab;   /* everything after here is raised (cushions/frame/voids) */

    /* Cushions from the chain segments: steep cloth playing face up to the
     * nose, then a cloth top sloping back to the cushion back. The facings
     * (which splay outward) shape the jaws automatically. */
    /* Cushion cross-section (K66-ish): from the bed it leans FORWARD up to the
     * protruding nose (the contact line at ~nose_h), a small vertical flat just
     * above the nose, then the cloth top slopes back to the rail. The base is
     * set back from the nose by `ub` so the nose overhangs (the "cut in below"). */
    uint16_t fdark = shade565(t->cloth, 0.55f);   /* undercut face (in shadow) */
    uint16_t face  = shade565(t->cloth, 0.72f);   /* the nose flat */
    uint16_t ctop  = shade565(t->cloth, 0.92f);   /* cloth top to the rail */
    const float flat_h = nose_h * 1.30f;          /* top of the small flat */
    const float ub = 0.45f * t->R;                /* undercut / overhang */
    for (int s = 0; s < w->nseg; s++) {
        const CueSeg *sg = &w->seg[s];
        /* Per-NODE back normal: average with the neighbouring segment when they
         * share an endpoint, so adjacent cushion tops share their back vertices
         * — a continuous strip with no V-gaps (the "holes in the top"). */
        Vec3 pa = sg->a, pb = sg->b, na = sg->n, nb = sg->n;
        int sharedA = 0, sharedB = 0;
        if (s > 0) {
            const CueSeg *pr = &w->seg[s-1];
            if (v3_len2(v3_sub(pr->b, sg->a)) < 1e-8f) { na = v3_norm(v3_add(sg->n, pr->n)); sharedA = 1; }
        }
        if (s < w->nseg - 1) {
            const CueSeg *nx = &w->seg[s+1];
            if (v3_len2(v3_sub(sg->b, nx->a)) < 1e-8f) { nb = v3_norm(v3_add(sg->n, nx->n)); sharedB = 1; }
        }
        float uba = sharedA ? ub : 0.0f, ubb = sharedB ? ub : 0.0f;
        float cwa = sharedA ? cw : 0.0f, cwb = sharedB ? cw : 0.0f;
        Vec3 ba = v3(pa.x - na.x*uba, 0, pa.z - na.z*uba);
        Vec3 bb = v3(pb.x - nb.x*ubb, 0, pb.z - nb.z*ubb);
        Vec3 an = v3(pa.x, nose_h, pa.z), bn = v3(pb.x, nose_h, pb.z);
        Vec3 af = v3(pa.x, flat_h, pa.z), bf = v3(pb.x, flat_h, pb.z);
        Vec3 ar = v3(pa.x - na.x*cwa, rail_h, pa.z - na.z*cwa);
        Vec3 br = v3(pb.x - nb.x*cwb, rail_h, pb.z - nb.z*cwb);
        ribbon(ba, bb, bn, an, fdark);      /* undercut face (leans to nose) */
        quad(an, bn, bf, af, face);            /* small flat (planar) */
        ribbon(af, bf, br, ar, ctop);       /* cloth top → rail */
    }

    /* Wood rail frame: full rectangular ring (the pocket caps punch holes
     * through it, flush with the rail). */
    const float fw = rw + 0.030f;
    const float ox = hl + fw, oz = hw + fw;
    const float ibx = hl + cw, ibz = hw + cw;
    /* Long rails carry the side pockets — cut a real hole for each so no wood
     * shows inside the void (matches the void radius; the void stays small). */
    const float scz = hw + t->off_side, shr = t->pr_side;
    wood_band(-ox, ox, ibz, oz, 0.0f,  scz, shr, rail_h, woodt);
    wood_band(-ox, ox, -oz, -ibz, 0.0f, -scz, shr, rail_h, woodt);
    quad(v3(-ox,rail_h,-ibz), v3(-ibx,rail_h,-ibz), v3(-ibx,rail_h,ibz), v3(-ox,rail_h,ibz), woodt);
    quad(v3(ibx,rail_h,-ibz), v3(ox,rail_h,-ibz), v3(ox,rail_h,ibz), v3(ibx,rail_h,ibz), woodt);
    quad(v3(-ox,rail_h,oz), v3(ox,rail_h,oz), v3(ox,0,oz), v3(-ox,0,oz), wood);
    quad(v3(ox,rail_h,-oz), v3(-ox,rail_h,-oz), v3(-ox,0,-oz), v3(ox,0,-oz), wood);
    quad(v3(ox,rail_h,oz), v3(ox,rail_h,-oz), v3(ox,0,-oz), v3(ox,0,oz), wood);
    quad(v3(-ox,rail_h,-oz), v3(-ox,rail_h,oz), v3(-ox,0,oz), v3(-ox,0,-oz), wood);

    /* Pockets = circular VOIDS you look down into. The bed is already cut at
     * the mouth, so a downward cone gives the recess. The OUTWARD half of each
     * pocket (the half sitting over the wood frame) gets a flush rail-level cap
     * + a frame-thickness wall to punch the hole through the wood; the inward
     * (mouth) half is left open so nothing floats above the playing surface. */
    uint16_t pk_wall = RGB565C(9, 11, 11);
    /* Snooker: a deeper, dark-olive "net bag" pouch the potted ball drops into.
     * Pool: a shallow near-black void. */
    uint16_t pk_floor = s_is_snooker ? RGB565C(34, 30, 20) : RGB565C(3, 4, 4);
    uint16_t pk_net   = s_is_snooker ? RGB565C(22, 20, 13) : RGB565C(6, 7, 7);
    const float cap_y = rail_h + 0.002f;
    const float floor_y = s_is_snooker ? -0.105f : -0.055f;
    for (int p = 0; p < w->npocket; p++) {
        float cx = w->pocket[p].x, cz = w->pocket[p].z;
        float r = (p < 4) ? t->pr_corner : t->pr_side;
        Vec3 cap_c = v3(cx, cap_y, cz), floor_c = v3(cx, floor_y, cz);
        const int N = 20;
        /* Align the cone segments to the pocket's outward diagonal so its
         * coverage is symmetric about that axis — otherwise it bites the two
         * jaws unequally (one looked smooth, the other kept a spike). */
        float base = atan2f(cz, cx);
        for (int k = 0; k < N; k++) {
            float a0 = base + k * (6.2831853f / N), a1 = base + (k + 1) * (6.2831853f / N);
            float c0 = cosf(a0), s0 = sinf(a0), c1 = cosf(a1), s1 = sinf(a1);
            float mx = cx + r * cosf(0.5f*(a0+a1)), mz = cz + r * sinf(0.5f*(a0+a1));
            /* Cap exactly where the wood frame actually is (the rail ring,
             * outside the cushion-back rectangle) — flush, no floating cap over
             * the playing side and no uncapped frame sliver. */
            int over_frame = (fabsf(mx) > ibx || fabsf(mz) > ibz);
            Vec3 bed0 = v3(cx + r*c0, -0.002f, cz + r*s0);
            Vec3 bed1 = v3(cx + r*c1, -0.002f, cz + r*s1);
            if (s_is_snooker) {                              /* two-tone net pouch */
                float midy = -0.05f, midr = r * 0.62f;
                Vec3 m0 = v3(cx+midr*c0, midy, cz+midr*s0);
                Vec3 m1 = v3(cx+midr*c1, midy, cz+midr*s1);
                quad(bed0, bed1, m1, m0, pk_floor);          /* upper throat */
                tri(floor_c, m0, m1, pk_net);                /* taper into the bag */
            } else {
                tri(floor_c, bed0, bed1, pk_floor);          /* shallow dark void */
            }
            if (over_frame) {
                Vec3 top0 = v3(cx + r*c0, cap_y, cz + r*s0);
                Vec3 top1 = v3(cx + r*c1, cap_y, cz + r*s1);
                tri(cap_c, top0, top1, pk_wall);             /* punch frame (flush) */
                quad(top0, top1, bed1, bed0, pk_wall);       /* frame-thickness wall */
            }
        }
    }

    emit_pocket_lips(t, w);   /* drop lip last → layers over the voids cleanly */
}

/* ---- per-frame build --------------------------------------------------- */
static void project_view(Vec3 v, float *sx, float *sy, uint16_t *d) {
    if (v.z < CUE_NEAR) v.z = CUE_NEAR;
    float inv = 1.0f / v.z;
    *sx = 64.0f + s_focal * v.x * inv;
    *sy = 64.0f - s_focal * v.y * inv;
    float dd = CUE_DEPTH_K * inv;
    *d = (dd >= 65535.0f) ? 65535u : (dd < 1.0f ? 1u : (uint16_t)dd);
}
/* Push one screen triangle, ordering verts for positive area (double-sided
 * so a table face is never culled by winding). */
static void push_stri(float ax, float ay, uint16_t da, float bx, float by,
                      uint16_t db, float cx, float cy, uint16_t dc, uint16_t col) {
    if (s_nstri >= MAX_STRI) return;
    float area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
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
/* Transform to view space, clip against the near plane (Sutherland-Hodgman,
 * one plane → 1 or 2 tris), shade and emit. This is what keeps the bed/rails
 * visible when the camera sits on the table (near corners behind it). */
static void add_stri(Vec3 a, Vec3 b, Vec3 c, Vec3 nrm, uint16_t base) {
    Vec3 va = m3_mul_v3_t(&s_view.basis, v3_sub(a, s_view.pos));
    Vec3 vb = m3_mul_v3_t(&s_view.basis, v3_sub(b, s_view.pos));
    Vec3 vc = m3_mul_v3_t(&s_view.basis, v3_sub(c, s_view.pos));
    float ndl = v3_dot(nrm, s_light); if (ndl < 0) ndl = -ndl;
    uint16_t col = shade565(base, 0.32f + 0.68f * ndl);

    Vec3 poly[3] = { va, vb, vc };
    Vec3 out[4]; int no = 0;
    for (int i = 0; i < 3; i++) {
        Vec3 p = poly[i], q = poly[(i + 1) % 3];
        int pin = p.z > CUE_NEAR, qin = q.z > CUE_NEAR;
        if (pin) out[no++] = p;
        if (pin != qin) {
            float t = (CUE_NEAR - p.z) / (q.z - p.z);
            out[no++] = v3_lerp(p, q, t);
        }
    }
    if (no < 3) return;
    float ox[4], oy[4]; uint16_t od[4];
    for (int i = 0; i < no; i++) project_view(out[i], &ox[i], &oy[i], &od[i]);
    push_stri(ox[0], oy[0], od[0], ox[1], oy[1], od[1], ox[2], oy[2], od[2], col);
    if (no == 4)
        push_stri(ox[0], oy[0], od[0], ox[2], oy[2], od[2], ox[3], oy[3], od[3], col);
}

void cue_render_build(const CueView *v, const CueBall *balls, int n,
                      int aim_active, int aim_ball, Vec3 aim_dir,
                      float power, int show_ghost) {
    s_view = *v;
    s_focal = 64.0f / tanf(v->fov_deg * (3.14159265f / 180.0f) * 0.5f);

    s_nstri = 0;
    for (int i = 0; i < s_ntab; i++) {
        if (i == s_bed_ntab) s_bed_nstri = s_nstri;   /* bed→raised boundary */
        add_stri(s_tab[i].v[0], s_tab[i].v[1], s_tab[i].v[2],
                 s_tab[i].nrm, s_tab[i].color);
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
        /* shadow: a soft disc lying flat ON the cloth, directly under the ball
         * (lamps are overhead → no side cast). Built as a ground-plane decal so
         * it foreshortens with the table and spreads toward the camera, staying
         * visible at the low aim-cam angle. */
        float gcx, gcy, axx, axy, azx, azy; uint16_t shd;
        const float sr = s_ballR * 1.55f;      /* shadow radius in world metres */
        if (b->drop <= 0.0f &&                  /* no shadow once it's dropping in */
            cue_render_project(v3(b->pos.x,      0.0f, b->pos.z),      &gcx, &gcy, &shd) &&
            cue_render_project(v3(b->pos.x + sr, 0.0f, b->pos.z),      &axx, &axy, NULL) &&
            cue_render_project(v3(b->pos.x,      0.0f, b->pos.z + sr), &azx, &azy, NULL)) {
            if (s_nshadow < CUE_MAX_BALLS) {
                s_shadow[s_nshadow].cx = gcx; s_shadow[s_nshadow].cy = gcy;
                s_shadow[s_nshadow].ux = axx - gcx; s_shadow[s_nshadow].uy = axy - gcy;
                s_shadow[s_nshadow].vx = azx - gcx; s_shadow[s_nshadow].vy = azy - gcy;
                s_nshadow++;
            }
        }
    }

    /* Aim line, ghost ball, object-ball line, cue stick. */
    s_ndot = 0; s_nodot = 0; s_cue.on = 0; s_ghost.on = 0;
    if (aim_active && aim_ball >= 0 && aim_ball < n && balls[aim_ball].on) {
        Vec3 cuepos = balls[aim_ball].pos;
        Vec3 dir = v3_norm(v3(aim_dir.x, 0, aim_dir.z));
        const float twoR = 2.0f * s_ballR;
        /* nearest ball the cue ball actually contacts along the ray: solve for
         * the travel distance s where centre separation = 2R. */
        float bests = 1e9f; int hit = -1;
        for (int i = 0; i < n; i++) {
            if (i == aim_ball || !balls[i].on) continue;
            Vec3 d = v3_sub(balls[i].pos, cuepos); d.y = 0;
            float along = v3_dot(d, dir);
            if (along <= 0) continue;
            float perp2 = (d.x * d.x + d.z * d.z) - along * along;
            if (perp2 < twoR * twoR) {
                float s = along - sqrtf(twoR * twoR - perp2);
                if (s > 0 && s < bests) { bests = s; hit = i; }
            }
        }
        float linelen = (hit >= 0) ? bests : 1.4f;
        float step = s_ballR * 2.2f;
        int ndots = (int)(linelen / step);
        if (ndots > MAX_DOTS) ndots = MAX_DOTS;
        for (int k = 1; k <= ndots; k++) {
            Vec3 wp = v3(cuepos.x + dir.x * (k * step), s_ballR,
                         cuepos.z + dir.z * (k * step));
            float dx, dy; uint16_t dd;
            if (cue_render_project(wp, &dx, &dy, &dd) && s_ndot < MAX_DOTS) {
                s_dot[s_ndot].x = dx; s_dot[s_ndot].y = dy; s_dot[s_ndot].d = dd;
                s_ndot++;
            }
        }
        if (hit >= 0) {
            /* ghost-ball: cue centre at contact = cuepos + dir*bests. */
            Vec3 ghost = v3(cuepos.x + dir.x * bests, s_ballR, cuepos.z + dir.z * bests);
            /* object ball departs along the line of centres ghost→object. */
            Vec3 odir = v3_norm(v3(balls[hit].pos.x - ghost.x, 0,
                                   balls[hit].pos.z - ghost.z));
            for (int k = 1; k <= 10; k++) {
                Vec3 wp = v3(balls[hit].pos.x + odir.x * (k * step), s_ballR,
                             balls[hit].pos.z + odir.z * (k * step));
                float dx, dy; uint16_t dd;
                if (cue_render_project(wp, &dx, &dy, &dd) && s_nodot < MAX_DOTS) {
                    s_odot[s_nodot].x = dx; s_odot[s_nodot].y = dy;
                    s_odot[s_nodot].d = dd; s_nodot++;
                }
            }
            if (show_ghost) {
                float gx, gy, gvz;
                if (project_z(ghost, &gx, &gy, &gvz)) {
                    s_ghost.cx = gx; s_ghost.cy = gy;
                    s_ghost.rad = s_focal * s_ballR / gvz;
                    uint16_t dd; float t1, t2;
                    cue_render_project(ghost, &t1, &t2, &dd);
                    s_ghost.d = dd; s_ghost.on = 1;
                }
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
    if (s_is_snooker) return RGB565C(190, 30, 30);          /* reds 1..15 */
    /* pool: 1-7 solids, 8 black, 9-15 stripes (hue of id-8). */
    static const uint16_t hue[8] = {
        0, RGB565C(235,200,40), RGB565C(30,80,200), RGB565C(200,40,40),
        RGB565C(120,40,160), RGB565C(230,120,30), RGB565C(20,130,50),
        RGB565C(120,30,40) };
    if (id >= 1 && id <= 7) return hue[id];
    if (id == 8) return RGB565C(20, 20, 22);
    if (id >= 9 && id <= 15) return RGB565C(235, 235, 225); /* white body */
    return RGB565C(200, 40, 40);
}
/* Sample the ball's surface colour for a ball-local unit normal. */
static uint16_t ball_sample(uint8_t id, Vec3 nb, uint16_t base) {
    /* Cue ball: a "measles" spotted ball — six small red dots, one centred on
     * each axis pole (±x, ±y, ±z), so spin reads clearly however it rolls. */
    if (id == CUE_ID_CUE) {
        float ax = fabsf(nb.x), ay = fabsf(nb.y), az = fabsf(nb.z);
        float m = ax > ay ? (ax > az ? ax : az) : (ay > az ? ay : az);
        if (m > 0.965f) return RGB565C(150, 70, 60);  /* small, muted pole dots */
        return base;
    }
    if (s_is_snooker) return base;              /* snooker balls are unmarked */
    if (id >= 9 && id <= 15) {                  /* stripe band + number patch */
        if (fabsf(nb.y) < 0.45f) {
            static const uint16_t hue[8] = {
                0, RGB565C(235,200,40), RGB565C(30,80,200), RGB565C(200,40,40),
                RGB565C(120,40,160), RGB565C(230,120,30), RGB565C(20,130,50),
                RGB565C(120,30,40) };
            return hue[id - 8];
        }
        if (nb.x > 0.90f) return RGB565C(245, 245, 245);   /* number patch */
        return base;
    }
    if ((id >= 1 && id <= 8)) {                 /* solids + 8: white patch */
        if (nb.x > 0.90f) return RGB565C(245, 245, 245);
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
    /* Overhead fixture = 4 lamps in a 2×2 cluster → 4 sharp reflection dots.
     * Each reflects where the surface normal ≈ that lamp's half-vector. */
    const float lx = 0.42f, lz = 0.28f;   /* wide enough to read as 4 dots */
    Vec3 Hl[4];
    Hl[0] = v3_norm(v3_add(v3_norm(v3(s_light.x+lx, s_light.y, s_light.z+lz)), vcam));
    Hl[1] = v3_norm(v3_add(v3_norm(v3(s_light.x-lx, s_light.y, s_light.z+lz)), vcam));
    Hl[2] = v3_norm(v3_add(v3_norm(v3(s_light.x+lx, s_light.y, s_light.z-lz)), vcam));
    Hl[3] = v3_norm(v3_add(v3_norm(v3(s_light.x-lx, s_light.y, s_light.z-lz)), vcam));
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
            float s = v3_dot(Nw, H); if (s < 0) s = 0;  /* specular base */
            float down = -Nw.y; if (down < 0) down = 0; /* underside (faces cloth) */
            Vec3 Nb = m3_mul_v3_t(&sp->orient, Nw);     /* world→ball-local */
            uint16_t bc = ball_sample(sp->id, Nb, base);
            uint16_t col;
            switch (s_light_mode) {
            case 0:  /* SMOOTH (original soft look) */
                col = shade565(bc, (0.30f + 0.70f*diff) * (0.78f + 0.22f*nz));
                { float ss = s; ss*=ss; ss*=ss; ss*=ss; int hi=(int)(ss*26.0f);
                  if (hi>0) col = add565(col, hi, hi*2, hi); }
                break;
            case 2:  /* TOON: banded diffuse + cloth shadow + crisp dot */
                col = shade565(bc, diff>0.62f?1.0f : diff>0.30f?0.74f : 0.52f);
                col = mix565(col, s_cloth_shadow, (1.0f-diff)*0.40f + down*0.22f);
                if (s > 0.82f) col = RGB565C(250,250,250);
                break;
            case 3:  /* GLOSS: smooth body, strong cloth tint, sharp hotspot */
                col = shade565(bc, 0.30f + 0.70f*diff);
                col = mix565(col, s_cloth_shadow, (1.0f-diff)*0.50f + down*0.40f);
                if (s > 0.60f) { float h=(s-0.60f)*2.5f; h*=h*h; int hi=(int)(h*30.0f);
                  if (hi>0) col = add565(col, hi, hi, hi); }
                break;
            case 4:  /* 4-DOT medium */
            case 5:  /* 4-DOT large/soft */
            default: /* 1 = 4-DOT sharp: polished ball reflecting the 4 overhead
                      * lamps as crisp bright dots; saturated body, cloth-tinted
                      * lower half. */
            {
                float thr = (s_light_mode==5) ? 0.93f : (s_light_mode==4) ? 0.955f : 0.975f;
                float gain = (s_light_mode==5) ? 0.85f : 1.0f;
                col = shade565(bc, 0.46f + 0.54f*diff);
                col = mix565(col, s_cloth_shadow, (1.0f-diff)*0.40f + down*0.42f);
                float refl = 0.0f;
                for (int li = 0; li < 4; li++) {
                    float si = v3_dot(Nw, Hl[li]);
                    if (si > thr) { float h = (si - thr) / (1.0f - thr); refl += h*h; }
                }
                if (refl > 1.0f) refl = 1.0f;
                /* lamp reflections are neutral white, NOT a brighter shade of
                 * the ball — blend toward pure white so every ball shows the
                 * same white dots (no pink/coloured tinge). */
                if (refl > 0.0f) col = mix565(col, RGB565C(255,255,255), refl * gain);
                break;
            }
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

    /* table triangles — BED first (flat cloth), so shadows can paint over it
     * without the slate occluding them; the RAISED geometry (cushions, rails,
     * pocket voids) is drawn after the shadows and depth-tests over them. */
    for (int i = 0; i < s_bed_nstri; i++) {
        const STri *t = &s_stri[i];
        r3d_tri(t->x0, t->y0, t->d0, t->x1, t->y1, t->d1,
                t->x2, t->y2, t->d2, t->color, y0, y1);
    }

    /* soft ground-plane shadow decals lying flat on the cloth. Each is an
     * ellipse C + s*U + t*V (|(s,t)|<=1) where U,V are the screen projections
     * of world +X/+Z offsets, so it foreshortens with the table and spreads
     * toward the camera (stays visible at the low aim-cam). We invert the 2×2
     * [U V] per pixel to recover (s,t) and fade darkness from centre to edge. */
    for (int i = 0; i < s_nshadow; i++) {
        float cx = s_shadow[i].cx, cy = s_shadow[i].cy;
        float ux = s_shadow[i].ux, uy = s_shadow[i].uy;
        float vx = s_shadow[i].vx, vy = s_shadow[i].vy;
        float det = ux * vy - uy * vx;
        if (det > -1e-4f && det < 1e-4f) continue;
        float inv = 1.0f / det;
        int bx = (int)(fabsf(ux) + fabsf(vx)) + 1;   /* screen bounding box */
        int by = (int)(fabsf(uy) + fabsf(vy)) + 1;
        int x0 = (int)cx - bx, x1b = (int)cx + bx;
        int yy0 = (int)cy - by, yy1 = (int)cy + by;
        for (int py = yy0; py <= yy1; py++) {
            if (py < y0 || py >= y1 || py < 0 || py >= CUE_FB_H) continue;
            uint16_t *frow = fb + py * R3D_FB_W;
            float ry = py - cy;
            for (int px = x0; px <= x1b; px++) {
                if (px < 0 || px >= CUE_FB_W) continue;
                float rx = px - cx;
                float s = ( rx * vy - ry * vx) * inv;
                float t = (-rx * uy + ry * ux) * inv;
                float r2 = s * s + t * t;
                if (r2 > 1.0f) continue;
                /* No depth test: shadows are drawn AFTER the cloth bed but
                 * BEFORE the raised geometry, so the slate never occludes them
                 * while cushions/rails (drawn next, depth-tested) paint over. */
                float k = 0.5f + 0.5f * r2 * r2;
                frow[px] = shade565(frow[px], k);
            }
        }
    }

    /* raised table geometry (cushions, rail frame, pocket voids) — depth-tested
     * over the shadows so a cushion/rail correctly hides a shadow behind it. */
    for (int i = s_bed_nstri; i < s_nstri; i++) {
        const STri *t = &s_stri[i];
        r3d_tri(t->x0, t->y0, t->d0, t->x1, t->y1, t->d1,
                t->x2, t->y2, t->d2, t->color, y0, y1);
    }

    /* aim dots (cue path, pale yellow) + object-ball path (cyan) */
    for (int i = 0; i < s_ndot; i++)
        r3d_point((int)s_dot[i].x, (int)s_dot[i].y, 65000, RGB565C(240,240,160),
                  1, y0, y1);
    for (int i = 0; i < s_nodot; i++)
        r3d_point((int)s_odot[i].x, (int)s_odot[i].y, 65000, RGB565C(120,230,235),
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
