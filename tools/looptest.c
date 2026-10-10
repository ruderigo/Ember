/* Offline looper test: record, play, overdub, undo, redo, clear.
 * Build: make looptest. Writes looptest.wav and prints checks. */
#include "../src/dsp.h"
#include <stdio.h>
#include <math.h>
#include <string.h>

static void ev(int type, int note) { Event e = { (uint8_t)type, (uint8_t)note, 100, 0 }; evq_push(&q_ui, e); }
static float L[256], R[256];
static FILE *wav; static int frames, fails;
static void check(int ok, const char *what) { if (!ok) { printf("FAIL: %s\n", what); fails++; } }
static double run(double seconds) {              /* returns RMS of the rendered span */
    double sum = 0; int n = (int)(seconds * 48000 / 256);
    for (int b = 0; b < n; b++) {
        engine_render(L, R, 256);
        short s[512];
        for (int i = 0; i < 256; i++) {
            if (isnan(L[i])) { printf("NaN!\n"); return -1; }
            sum += L[i] * L[i];
            s[2 * i] = (short)(L[i] * 32000); s[2 * i + 1] = (short)(R[i] * 32000);
        }
        fwrite(s, 2, 512, wav); frames += 256;
    }
    return sqrt(sum / (n * 256.0));
}

int main(void) {
    engine_init();
    P.attack = 0.3f; P.release = 0.5f;
    wav = fopen("looptest.wav", "wb"); fseek(wav, 44, SEEK_SET);
    const int c1[3] = {48, 52, 55}, c2[3] = {57, 60, 64};

    ev(EV_LOOP, 0);                                  /* record */
    for (int i = 0; i < 3; i++) ev(EV_NOTE_ON, c1[i]);
    run(3.0);
    for (int i = 0; i < 3; i++) ev(EV_NOTE_OFF, c1[i]);
    run(1.0);
    ev(EV_LOOP, 0);                                  /* close take: 4 s loop */
    run(0.01);
    printf("after take: state %d len %.2f s\n", M.loop_state, M.loop_len / 48000.0);
    run(6.0);                                        /* let the live release die */
    double play = run(4.0);
    printf("loop only, one pass: rms %.4f (should be clearly > 0)\n", play);

    ev(EV_LOOP, 0);                                  /* overdub a second chord */
    for (int i = 0; i < 3; i++) ev(EV_NOTE_ON, c2[i]);
    run(2.0);
    for (int i = 0; i < 3; i++) ev(EV_NOTE_OFF, c2[i]);
    run(2.0);
    ev(EV_LOOP, 0);                                  /* back to play */
    run(4.0);
    double dubbed = run(4.0);
    printf("after overdub: rms %.4f  can_undo %d\n", dubbed, M.loop_can_undo);

    ev(EV_LOOP_UNDO, 0); run(1.0); run(3.0);
    double undone = run(4.0);
    printf("after undo: rms %.4f (should match one-pass %.4f)  undone %d\n", undone, play, M.loop_undone);
    ev(EV_LOOP_UNDO, 0); run(1.0); run(3.0);
    double redone = run(4.0);
    printf("after redo: rms %.4f (should match overdub %.4f)\n", redone, dubbed);
    ev(EV_LOOP_CLEAR, 0);
    double cleared = run(2.0);
    printf("after clear: state %d rms %.5f\n", M.loop_state, cleared);
    check(play > 0.02, "loop plays back");
    check(dubbed > play * 1.1, "overdub adds a layer");
    check(fabs(undone - play) < play * 0.03, "undo restores the first take");
    check(fabs(redone - dubbed) < dubbed * 0.03, "redo restores the overdub");
    check(M.loop_state == LOOP_EMPTY && cleared < 0.01, "clear empties the loop");

    /* header */
    int bytes = frames * 4, v; short sh;
    fseek(wav, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, wav); v = 36 + bytes; fwrite(&v, 4, 1, wav); fwrite("WAVEfmt ", 1, 8, wav);
    v = 16; fwrite(&v, 4, 1, wav); sh = 1; fwrite(&sh, 2, 1, wav); sh = 2; fwrite(&sh, 2, 1, wav);
    v = 48000; fwrite(&v, 4, 1, wav); v = 192000; fwrite(&v, 4, 1, wav); sh = 4; fwrite(&sh, 2, 1, wav);
    sh = 16; fwrite(&sh, 2, 1, wav); fwrite("data", 1, 4, wav); fwrite(&bytes, 4, 1, wav);
    fclose(wav);
    printf("%s\n", fails ? "looptest: FAIL" : "looptest: ok");
    return fails != 0;
}
