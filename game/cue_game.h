/*
 * ThumbyCue — game loop, camera, controls and the platform interface the
 * host/device shells drive (mirrors ThumbyElite's split exactly).
 */
#ifndef CUE_GAME_H
#define CUE_GAME_H

#include <stdint.h>
#include "craft_buttons.h"

void cue_game_init(uint32_t seed);
void cue_game_set_kind(int snooker);          /* 0 = pool, 1 = snooker */
void cue_game_tick(const CraftRawButtons *btn, float dt);

/* core0 builds the frame; both cores raster their band; overlay is 2D HUD. */
void cue_game_render_begin(void);
void cue_game_render(uint16_t *fb, int y0, int y1);
void cue_game_draw_overlay(uint16_t *fb);
void cue_game_set_frame_ms(float ms);

#endif
