/* Recorder test: session recording + loop export. Build: make rectest */
#include "../src/rec.h"
#include <math.h>
#include <stdio.h>
#include <unistd.h>
int main(void) {
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
    unsigned seq; printf("last message: %s\n", rec_message(&seq));
    return 0;
}
