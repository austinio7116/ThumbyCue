/*
 * ThumbyCue — scene renderer. Built directly on r3d_raster (flat-shaded
 * depth-tested triangles) + a per-pixel sphere-impostor ball pass adapted
 * from the Elite planet renderer. Dual-core: core0 calls cue_render_build()
 * to project the table, balls and aim cue into screen-space lists; then both
 * cores call cue_render_raster() clamped to their screen half.
 */
#ifndef CUE_RENDER_H
#define CUE_RENDER_H

#include "cue_physics.h"
#include "cue_table.h"
#include <stdint.h>

/* Camera: world position + orthonormal basis (rows right/up/forward) + fov. */
typedef struct { Vec3 pos; Mat3 basis; float fov_deg; } CueView;

/* Build the static table triangle mesh from the table + its collision world
 * (so render and physics share one geometry source). Call once per table. */
void cue_render_build_table(const CueTable *t, const CueWorld *w);

/* Per-frame (core0): project everything for the given view. balls[0..n).
 * aim_active draws the cue stick + aiming line from the cue ball along
 * aim_dir (unit world X–Z); ghost shows the contact ghost-ball ring.
 * power 0..1 pulls the cue back. */
void cue_render_build(const CueView *v, const CueBall *balls, int n,
                      int aim_active, int aim_ball, Vec3 aim_dir,
                      float power, int show_ghost);

/* Rasterise rows [y0,y1) into fb (logical 128-space rows). Safe to call
 * concurrently on disjoint bands from both cores. */
void cue_render_raster(uint16_t *fb, int y0, int y1);

/* Project a world point with the current view. Returns 0 if behind near. */
int cue_render_project(Vec3 world, float *sx, float *sy, uint16_t *d);

#endif
