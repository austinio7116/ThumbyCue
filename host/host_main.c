/*
 * ThumbyCue — host (Linux/SDL2) shell. Mirrors the device loop: reads a
 * CraftRawButtons, ticks the game, renders the 128×128 frame single-core,
 * draws the overlay, and presents at an integer scale.
 *
 * Keys: W/A/S/D = d-pad, K = A, J = B, U = LB, I = RB, Enter = MENU,
 *       1 = pool, 2 = snooker, Esc = quit.
 * Env:  CUE_SHOT=/path.ppm  settle N frames headless, dump a PPM, exit.
 */
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cue_types.h"
#include "cue_game.h"
#include "craft_buttons.h"

#define SCALE 5
static uint16_t g_fb[CUE_FB_W * CUE_FB_H];

static void render_frame(void) {
    cue_game_render_begin();
    cue_game_render(g_fb, 0, CUE_FB_H);
    cue_game_draw_overlay(g_fb);
}

static void dump_ppm(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", CUE_FB_W, CUE_FB_H);
    for (int i = 0; i < CUE_FB_W * CUE_FB_H; i++) {
        uint16_t c = g_fb[i];
        unsigned char rgb[3] = {
            (unsigned char)(((c >> 11) & 0x1F) * 255 / 31),
            (unsigned char)(((c >> 5) & 0x3F) * 255 / 63),
            (unsigned char)((c & 0x1F) * 255 / 31) };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

int main(int argc, char **argv) {
    int snooker = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "snooker")) snooker = 1;
        else if (!strcmp(argv[i], "pool")) snooker = 0;
    }

    cue_game_init(0x1234u);
    cue_game_set_kind(snooker);

    const char *shot = getenv("CUE_SHOT");
    if (shot) {
        CraftRawButtons b; memset(&b, 0, sizeof b);
        const char *cam = getenv("CUE_CAM");      /* ex,ey,ez,tx,ty,tz,fov */
        if (cam) {
            float p[7] = {0,0,0,0,0,0,52};
            sscanf(cam, "%f,%f,%f,%f,%f,%f,%f",
                   &p[0],&p[1],&p[2],&p[3],&p[4],&p[5],&p[6]);
            cue_game_debug_cam(p[0],p[1],p[2],p[3],p[4],p[5],p[6]);
        }
        if (getenv("CUE_OVERHEAD")) {           /* tap LB once to toggle */
            b.lb = 1; cue_game_tick(&b, 1.0f / 60.0f); b.lb = 0;
        }
        for (int i = 0; i < 30; i++) { cue_game_tick(&b, 1.0f / 60.0f); }
        render_frame();
        dump_ppm(shot);
        printf("wrote %s\n", shot);
        return 0;
    }

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window *win = SDL_CreateWindow("ThumbyCue",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        CUE_FB_W * SCALE, CUE_FB_H * SCALE, 0);
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
    SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB565,
        SDL_TEXTUREACCESS_STREAMING, CUE_FB_W, CUE_FB_H);

    Uint32 last = SDL_GetTicks();
    int running = 1;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) running = 0;
            if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE)
                running = 0;
            if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_1)
                cue_game_set_kind(0);
            if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_2)
                cue_game_set_kind(1);
        }
        const Uint8 *k = SDL_GetKeyboardState(NULL);
        CraftRawButtons b;
        b.up = k[SDL_SCANCODE_W]; b.down = k[SDL_SCANCODE_S];
        b.left = k[SDL_SCANCODE_A]; b.right = k[SDL_SCANCODE_D];
        b.a = k[SDL_SCANCODE_K]; b.b = k[SDL_SCANCODE_J];
        b.lb = k[SDL_SCANCODE_U]; b.rb = k[SDL_SCANCODE_I];
        b.menu = k[SDL_SCANCODE_RETURN];

        Uint32 now = SDL_GetTicks();
        float dt = (now - last) * 0.001f;
        if (dt > 0.05f) dt = 0.05f;
        last = now;

        cue_game_tick(&b, dt);
        Uint32 t0 = SDL_GetTicks();
        render_frame();
        cue_game_set_frame_ms((float)(SDL_GetTicks() - t0));

        SDL_UpdateTexture(tex, NULL, g_fb, CUE_FB_W * (int)sizeof(uint16_t));
        SDL_RenderClear(ren);
        SDL_RenderCopy(ren, tex, NULL, NULL);
        SDL_RenderPresent(ren);
    }
    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
