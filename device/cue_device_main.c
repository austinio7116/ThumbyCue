/*
 * ThumbyCue — device entry point (standalone, RP2350 / Thumby Color).
 *
 * Thin shell over the shared cue_game loop, mirroring ThumbyElite: core0
 * builds the frame, then both cores rasterise it — core0 rows [0,64),
 * core1 rows [64,128). Single framebuffer + async DMA present, same
 * go/done volatile handshake.
 */
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/rand.h"
#include "hardware/clocks.h"

#include "craft_lcd_gc9107.h"
#include "craft_buttons.h"
#include "craft_rumble.h"
#include "craft_audio_pwm.h"
#include "cue_types.h"
#include "cue_game.h"
#include "cue_audio.h"

static uint16_t g_fb[CUE_FB_W * CUE_FB_H];

static volatile bool s_core1_go = false;
static volatile bool s_core1_done = false;

static void core1_entry(void) {
    multicore_lockout_victim_init();
    while (true) {
        while (!s_core1_go) tight_loop_contents();
        s_core1_go = false;
        cue_game_render(g_fb, CUE_FB_H / 2, CUE_FB_H);
        s_core1_done = true;
    }
}

int main(void) {
    set_sys_clock_khz(280000, true);
    craft_lcd_init();
    for (int i = 0; i < CUE_FB_W * CUE_FB_H; i++) g_fb[i] = 0;
    craft_lcd_present(g_fb);
    craft_buttons_init();
    craft_rumble_init();
    craft_audio_pwm_init();

    cue_game_init(get_rand_32());

    multicore_launch_core1(core1_entry);

    uint32_t last_ms = to_ms_since_boot(get_absolute_time());
    while (true) {
        CraftRawButtons btn;
        craft_buttons_read(&btn);
        uint32_t now_ms = to_ms_since_boot(get_absolute_time());
        float dt = (now_ms - last_ms) * 0.001f;
        if (dt > 0.1f) dt = 0.1f;
        last_ms = now_ms;

        cue_game_tick(&btn, dt);
        craft_rumble_tick(dt);

        /* keep the PWM audio ring fed */
        int room = craft_audio_pwm_room();
        while (room > 0) {
            int16_t abuf[128];
            int nn = room < 128 ? room : 128;
            cue_audio_render(abuf, nn);
            craft_audio_pwm_push(abuf, nn);
            room -= nn;
        }

        uint64_t t0 = to_us_since_boot(get_absolute_time());
        cue_game_render_begin();                     /* core0: build draw list */
        uint64_t t1 = to_us_since_boot(get_absolute_time());

        craft_lcd_wait_idle();                        /* prev DMA must finish */
        uint64_t t2 = to_us_since_boot(get_absolute_time());
        s_core1_done = false;
        s_core1_go = true;                            /* core1: lower half */
        cue_game_render(g_fb, 0, CUE_FB_H / 2);       /* core0: upper half */
        while (!s_core1_done) tight_loop_contents();
        uint64_t t3 = to_us_since_boot(get_absolute_time());

        cue_game_draw_overlay(g_fb);
        cue_game_set_frame_ms((float)((t1 - t0) + (t3 - t2)) * 0.001f);

        craft_lcd_present(g_fb);
    }
    return 0;
}
