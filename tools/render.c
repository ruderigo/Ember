/* Offline test: renders a chord progression through the engine to a WAV file
 * and prints levels and speed. Build: make render. Usage: ./render out.wav */
#include "../src/dsp.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>

static void note(int on, int n) { Event e = { on ? EV_NOTE_ON : EV_NOTE_OFF, (uint8_t)n, 100, 0 }; evq_push(&q_ui, e); }

static void wav_header(FILE *f, int frames) {
    int bytes = frames * 4;
    fwrite("RIFF", 1, 4, f); int v = 36 + bytes; fwrite(&v, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f); v = 16; fwrite(&v, 4, 1, f);
    short s = 1; fwrite(&s, 2, 1, f); s = 2; fwrite(&s, 2, 1, f);
    v = 48000; fwrite(&v, 4, 1, f); v = 48000 * 4; fwrite(&v, 4, 1, f);
    s = 4; fwrite(&s, 2, 1, f); s = 16; fwrite(&s, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&bytes, 4, 1, f);
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "ember_demo.wav";
    engine_init();
    /* "warm analog" default preset */
    P.detune = 0.5f; P.sub = 0.35f; P.drift = 0.45f; P.cutoff = 0.5f; P.reso = 0.2f;
    P.envamt = 0.35f; P.attack = 1.2f; P.release = 3.f; P.tape = 0.45f;
    P.chorus = 0.65f; P.rvsize = 0.7f; P.rvmix = 0.35f; P.volume = 0.7f;
    P.vocsrc = 1.f; P.sib = 0.3f;

    /* C major key: Cmaj7 - Am7 - Fmaj7 - G(add9), 4 s each; the second pass
     * through the progression fades the vowel vocoder in and morphs vowels */
    static const int chords[4][4] = { {48,55,64,71}, {45,52,60,67}, {41,48,57,64}, {43,50,59,69} };
    const int block = 256, total = 48000 * 36;
    FILE *f = fopen(path, "wb"); if (!f) return 1;
    wav_header(f, total);
    float L[256], R[256]; short buf[512];
    double t0 = (double)clock() / CLOCKS_PER_SEC;
    double sumsq[9] = {0}; float pk[9] = {0}; int cnt[9] = {0};
    int cur = -1, nans = 0;
    for (int pos = 0; pos < total; pos += block) {
        float sec = pos / 48000.f;
        int slot = (int)(sec / 4.f);
        if (slot != cur && slot < 8) {
            if (cur >= 0) for (int k = 0; k < 4; k++) note(0, chords[cur % 4][k]);
            cur = slot;
            for (int k = 0; k < 4; k++) note(1, chords[cur % 4][k]);
        } else if (slot >= 8 && cur >= 0 && cur < 8) {
            for (int k = 0; k < 4; k++) note(0, chords[cur % 4][k]);
            cur = 99;
        }
        P.vocmix = sec < 16.f ? 0.f : fminf(1.f, (sec - 16.f) / 3.f) * 0.8f;
        P.vowel = sec < 16.f ? 0.f : fmodf((sec - 16.f) / 8.f, 1.f);
        P.mod_cutoff = 0.25f * sinf(sec * 0.4f);
        engine_render(L, R, block);
        int sec_slot = (int)(sec / 4.f); if (sec_slot > 8) sec_slot = 8;
        for (int i = 0; i < block; i++) {
            if (isnan(L[i]) || isnan(R[i])) nans++;
            sumsq[sec_slot] += L[i] * L[i] + R[i] * R[i]; cnt[sec_slot] += 2;
            float a = fabsf(L[i]); if (a > pk[sec_slot]) pk[sec_slot] = a;
            buf[2 * i] = (short)(L[i] * 32000.f); buf[2 * i + 1] = (short)(R[i] * 32000.f);
        }
        fwrite(buf, 2, 2 * block, f);
    }
    double t1 = (double)clock() / CLOCKS_PER_SEC;
    fclose(f);
    for (int i = 0; i < 9; i++)
        printf("segment %d (%2d-%2ds): rms %5.1f dBFS  peak %.2f\n", i, i * 4, i * 4 + 4,
               20 * log10(sqrt(sumsq[i] / (cnt[i] ? cnt[i] : 1)) + 1e-9), pk[i]);
    printf("NaNs: %d\nrendered 36 s in %.2f s  (%.1fx realtime)\n", nans, t1 - t0, 36.0 / (t1 - t0));
    int fail = nans > 0;
    for (int i = 0; i < 8; i++) if (pk[i] < 0.05f || pk[i] > 0.99f) { printf("FAIL: segment %d level out of range\n", i); fail = 1; }
    printf("%s\n", fail ? "render: FAIL" : "render: ok");
    return fail;
}
