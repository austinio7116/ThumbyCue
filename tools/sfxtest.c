/* Render each synthesized SFX to a WAV for A/B comparison with the originals.
 *   gcc -O2 -I../game sfxtest.c -lm -o /tmp/sfxtest && /tmp/sfxtest /tmp
 */
#include "cue_audio.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "cue_audio.c"

static void write_wav(const char *path, const int16_t *s, int n) {
    FILE *f = fopen(path, "wb");
    int sr = 22050, br = sr*2; int16_t ba = 2, bps = 16, ch = 1;
    int data = n*2, riff = 36 + data;
    fwrite("RIFF",1,4,f); fwrite(&riff,4,1,f); fwrite("WAVE",1,4,f);
    fwrite("fmt ",1,4,f); int fl=16; fwrite(&fl,4,1,f); int16_t fmt=1;
    fwrite(&fmt,2,1,f); fwrite(&ch,2,1,f); fwrite(&sr,4,1,f); fwrite(&br,4,1,f);
    fwrite(&ba,2,1,f); fwrite(&bps,2,1,f);
    fwrite("data",1,4,f); fwrite(&data,4,1,f); fwrite(s,2,n,f); fclose(f);
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : "/tmp";
    cue_audio_init(); cue_audio_set_volume(18);
    struct { int which; const char *name; } sf[] = {
        {CUE_SFX_STRIKE,"strike"},{CUE_SFX_CLACK,"clack"},
        {CUE_SFX_CUSHION,"cushion"},{CUE_SFX_POT,"pot_soft"},
        {CUE_SFX_POT,"pot_hard"},{CUE_SFX_UI,"ui"} };
    char path[256];
    for (int i = 0; i < 6; i++) {
        cue_audio_init();
        cue_audio_sfx(sf[i].which, (i==4)?0.9f:0.7f);
        int n = 22050;                /* 1 s */
        int16_t *buf = calloc(n, 2);
        cue_audio_render(buf, n);
        snprintf(path, sizeof path, "%s/syn_%s.wav", dir, sf[i].name);
        write_wav(path, buf, n);
        free(buf);
        printf("wrote %s\n", path);
    }
    return 0;
}
