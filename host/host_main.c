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
#include "cue_render.h"
#include "cue_audio.h"
#include "craft_buttons.h"

#define SCALE 5
static uint16_t g_fb[CUE_FB_W * CUE_FB_H];

static void audio_cb(void *ud, Uint8 *stream, int len) {
    (void)ud; cue_audio_render((int16_t *)stream, len / (int)sizeof(int16_t));
}

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
    if (getenv("CUE_MODE"))      cue_game_set_mode(atoi(getenv("CUE_MODE")));
    else if (!getenv("CUE_MENU")) cue_game_set_kind(snooker);   /* CUE_MENU: stay on title */
    if (getenv("CUE_BALLSET"))   cue_game_set_ballset(atoi(getenv("CUE_BALLSET")));
    if (getenv("CUE_CLOTH"))     cue_game_set_cloth(atoi(getenv("CUE_CLOTH")));
    if (getenv("CUE_FRAME"))     cue_game_set_frame(atoi(getenv("CUE_FRAME")));
    if (getenv("CUE_AIM"))       cue_game_set_aim(atoi(getenv("CUE_AIM")));

    /* Headless video capture: run a configured CPU-vs-CPU match, dump one PPM
     * per frame (30 fps) into $CUE_VIDEO. Config via CUE_MODE/CUE_P1/CUE_P2/
     * CUE_CLOTH/CUE_FRAME/CUE_BALLSET/CUE_BO, count via CUE_VFRAMES. */
    const char *vid = getenv("CUE_VIDEO");
    if (vid) {
        int g  = getenv("CUE_MODE")    ? atoi(getenv("CUE_MODE"))    : 0;
        int p1 = getenv("CUE_P1")      ? atoi(getenv("CUE_P1"))      : 0;
        int p2 = getenv("CUE_P2")      ? atoi(getenv("CUE_P2"))      : 7;
        int cl = getenv("CUE_CLOTH")   ? atoi(getenv("CUE_CLOTH"))   : 0;
        int fr = getenv("CUE_FRAME")   ? atoi(getenv("CUE_FRAME"))   : 0;
        int bs = getenv("CUE_BALLSET") ? atoi(getenv("CUE_BALLSET")) : 0;
        int bo = getenv("CUE_BO")      ? atoi(getenv("CUE_BO"))      : 1;
        int nf   = getenv("CUE_VFRAMES") ? atoi(getenv("CUE_VFRAMES")) : 600;
        cue_audio_init();
        cue_audio_set_volume(16);
        cue_game_start_demo(g, p1, p2, cl, fr, bs, bo);
        int skip = getenv("CUE_VSKIP") ? atoi(getenv("CUE_VSKIP")) : 5;
        CraftRawButtons b; memset(&b, 0, sizeof b);
        /* Play through the break + several shots BEFORE recording so the capture
         * opens mid-frame (table well developed), then start on a clean CPU
         * "thinking" view. A shot is counted each time play leaves the thinking
         * state (a strike). Then record video + frame-synced audio (735/frame). */
        int played = 0, wasT = cue_game_demo_thinking();
        for (int f = 0; f < 6000 && played < skip; f++) {
            cue_game_tick(&b, 1.0f/30.0f);
            int nowT = cue_game_demo_thinking();
            if (wasT && !nowT) played++;          /* a shot was just struck */
            wasT = nowT;
        }
        for (int f = 0; f < 1200 && !cue_game_demo_thinking(); f++) cue_game_tick(&b, 1.0f/30.0f);
        char apath[512]; snprintf(apath, sizeof apath, "%s/audio.raw", vid);
        FILE *aw = fopen(apath, "wb");
        for (int f = 0; f < nf; f++) {
            cue_game_tick(&b, 1.0f / 30.0f);
            if (aw) { int16_t ab[735]; cue_audio_render(ab, 735); fwrite(ab, 2, 735, aw); }
            render_frame();
            char path[512]; snprintf(path, sizeof path, "%s/f%05d.ppm", vid, f);
            dump_ppm(path);
        }
        if (aw) fclose(aw);
        printf("video: wrote %d frames to %s\n", nf, vid);
        return 0;
    }

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
        const char *lm = getenv("CUE_LIGHT");
        if (lm) cue_render_set_light_mode(atoi(lm));
        if (getenv("CUE_BALLTEST")) cue_game_debug_spread();
        if (getenv("CUE_NUMTEST"))  cue_game_debug_numbers();
        if (getenv("CUE_MAINMENU")) {            /* title -> main menu */
            b.a = 1; cue_game_tick(&b, 1.0f/60.0f); b.a = 0;
            for (int i = 0; i < 4; i++) cue_game_tick(&b, 1.0f/60.0f);
        }
        if (getenv("CUE_TABLEMENU")) {           /* title -> main -> PLAY -> TABLE editor */
            #define TAP(btn) do { b.btn=1; cue_game_tick(&b,1.0f/60.0f); b.btn=0; \
                                  for(int i=0;i<3;i++) cue_game_tick(&b,1.0f/60.0f); } while(0)
            TAP(a);                              /* title -> main (PLAY) */
            TAP(a);                              /* enter PLAY (GAME) */
            TAP(down); TAP(down); TAP(down); TAP(down);  /* GAME->MODE->CPU->BALLS->TABLE */
            TAP(a);                              /* open the TABLE editor */
            int frame = getenv("CUE_TBLFRAME") ? atoi(getenv("CUE_TBLFRAME")) : 0;
            if (frame) { TAP(down); for (int k=0;k<frame;k++) TAP(right); }  /* to FRAME, cycle */
            #undef TAP
        }
        if (getenv("CUE_PLAYMENU")) {            /* title -> main -> PLAY menu */
            for (int t = 0; t < 2; t++) {
                b.a = 1; cue_game_tick(&b, 1.0f/60.0f); b.a = 0;
                for (int i = 0; i < 4; i++) cue_game_tick(&b, 1.0f/60.0f);
            }
        }
        if (getenv("CUE_AIVSAI")) {              /* title->main->PLAY, set MODE=CPU vs CPU, START */
            #define TAP(btn) do { b.btn=1; cue_game_tick(&b,1.0f/60.0f); b.btn=0; \
                                  for(int i=0;i<3;i++) cue_game_tick(&b,1.0f/60.0f); } while(0)
            TAP(a);                              /* title -> main */
            TAP(a);                              /* main: PLAY -> SC_PLAY (cursor 0 = GAME) */
            TAP(down);                           /* GAME -> MODE */
            TAP(right);                          /* VS CPU -> CPU vs CPU */
            for (int k = 0; k < 5; k++) TAP(down);  /* MODE -> ... -> START */
            TAP(a);                              /* START */
            #undef TAP
        }
        if (getenv("CUE_OVERHEAD")) {           /* tap LB once to toggle */
            b.lb = 1; cue_game_tick(&b, 1.0f / 60.0f); b.lb = 0;
        }
        if (getenv("CUE_AUTO")) {               /* scripted shots — crash/flow test */
            int shots = atoi(getenv("CUE_AUTO"));
            for (int s = 0; s < shots; s++) {
                CraftRawButtons x; memset(&x, 0, sizeof x);   /* draw back */
                x.a = 1; x.down = 1;
                for (int i = 0; i < 45; i++) cue_game_tick(&x, 1.0f/60.0f);
                memset(&x, 0, sizeof x);                       /* release = strike */
                cue_game_tick(&x, 1.0f/60.0f);
                for (int i = 0; i < 700; i++) cue_game_tick(&x, 1.0f/60.0f); /* settle + CPU */
            }
            printf("autoplay %d shots ok\n", shots);
        }
        for (int i = 0; i < 30; i++) { cue_game_tick(&b, 1.0f / 60.0f); }
        if (getenv("CUE_FREELOOK") || getenv("CUE_TOPDOWN")) {  /* confirm (A) then free-look (LB) */
            b.a = 1; cue_game_tick(&b, 1.0f/60.0f); b.a = 0;
            for (int i = 0; i < 3; i++) cue_game_tick(&b, 1.0f/60.0f);
            b.lb = 1; cue_game_tick(&b, 1.0f/60.0f); b.lb = 0;
            for (int i = 0; i < 3; i++) cue_game_tick(&b, 1.0f/60.0f);
            if (getenv("CUE_TOPDOWN")) {            /* pitch the free-look camera fully overhead */
                b.up = 1; for (int i = 0; i < 150; i++) cue_game_tick(&b, 1.0f/60.0f); b.up = 0;
                cue_game_tick(&b, 1.0f/60.0f);
            }
        }
        if (getenv("CUE_NOHUD")) {                  /* clean scene, no HUD — for icons */
            cue_game_render_begin();
            cue_game_render(g_fb, 0, CUE_FB_H);
        } else {
            render_frame();
        }
        dump_ppm(shot);
        printf("wrote %s\n", shot);
        return 0;
    }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_AudioSpec want; memset(&want, 0, sizeof want);
    want.freq = 22050; want.format = AUDIO_S16SYS; want.channels = 1;
    want.samples = 512; want.callback = audio_cb;
    SDL_OpenAudio(&want, NULL); SDL_PauseAudio(0);
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
