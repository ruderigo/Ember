/* Recorder test: session recording + loop export. Build: make rectest */
#include "../src/rec.h"
#include <math.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
int main(void) {
    if (system("rm -rf recordings")) {}
    float L[256], R[256], ph = 0;
    rec_init();
    rec_start();
    for (int b = 0; b < 48000 * 2 / 256; b++) {          /* 2 s of 220 Hz */
        for (int i = 0; i < 256; i++) { L[i] = R[i] = 0.5f * sinf(ph); ph += 6.2831853f * 220 / 48000; }
        rec_push(L, R, 256);
        usleep(5333);
    }
    rec_stop();
    static float lp[48000], rp[48000];
    for (int i = 0; i < 48000; i++) lp[i] = rp[i] = 0.3f * sinf(i * 0.05f);
    rec_save_loop(lp, rp, 48000);
    usleep(300000);
    rec_shutdown();
    unsigned seq; const char *m = rec_message(&seq);
    printf("last message: %s\n", m);
    /* both files must exist with the right length */
    int ok = 1;
    FILE *p = popen("for f in recordings/ember-*.wav recordings/loop-*.wav; do stat -c %s \"$f\"; done", "r");
    long sizes[2] = {0, 0}; for (int i = 0; i < 2 && p; i++) if (fscanf(p, "%ld", &sizes[i]) != 1) ok = 0;
    if (p) pclose(p);
    if (sizes[0] < 44 + 48000 * 4 * 19 / 10) ok = 0;      /* ~2 s session (allow scheduling slack) */
    if (sizes[1] != 44 + 48000 * 4) ok = 0;               /* exactly 1 s loop */
    if (strncmp(m, "Saved loop", 10)) ok = 0;
    printf("rectest: %s (session %ld bytes, loop %ld bytes)\n", ok ? "ok" : "FAIL", sizes[0], sizes[1]);
    return !ok;
}
