/* Ember recorder.
 * Session recording: the audio thread drops 16-bit frames into a ring buffer
 * (never touching the SD card), and a writer thread drains it to disk.
 * Loop export: the loop is copied and written by a short-lived thread. */
#define _GNU_SOURCE
#include "rec.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define RATE 48000
#define RING (RATE * 4)                 /* 4 s of stereo frames */
static short ring[RING * 2];
static _Atomic unsigned rw, rr;
static _Atomic int recording, stop_req, quit;
static _Atomic long frames_written;
static FILE *file;
static pthread_t writer;
static char msg[128];
static _Atomic unsigned msg_seq;
static pthread_mutex_t msg_lock = PTHREAD_MUTEX_INITIALIZER;

static void set_msg(const char *m) {
    pthread_mutex_lock(&msg_lock);
    snprintf(msg, sizeof msg, "%s", m);
    pthread_mutex_unlock(&msg_lock);
    atomic_fetch_add(&msg_seq, 1);
}

const char *rec_message(unsigned *seq) {
    static char copy[128];
    pthread_mutex_lock(&msg_lock);
    memcpy(copy, msg, sizeof copy);
    pthread_mutex_unlock(&msg_lock);
    *seq = atomic_load(&msg_seq);
    return copy;
}

static void header(FILE *f, long frames) {
    unsigned bytes = (unsigned)(frames * 4), v;
    unsigned short s;
    fseek(f, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, f); v = 36 + bytes; fwrite(&v, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f); v = 16; fwrite(&v, 4, 1, f);
    s = 1; fwrite(&s, 2, 1, f); s = 2; fwrite(&s, 2, 1, f);
    v = RATE; fwrite(&v, 4, 1, f); v = RATE * 4; fwrite(&v, 4, 1, f);
    s = 4; fwrite(&s, 2, 1, f); s = 16; fwrite(&s, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&bytes, 4, 1, f);
}

static void make_name(char *out, size_t n, const char *prefix) {
    mkdir("recordings", 0755);
    time_t t = time(NULL);
    struct tm tm; localtime_r(&t, &tm);
    char stamp[32]; strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tm);
    snprintf(out, n, "recordings/%s-%s.wav", prefix, stamp);
}

static inline short to16(float x) {
    if (x > 1.f) x = 1.f;
    if (x < -1.f) x = -1.f;
    return (short)(x * 32700.f);
}

void rec_push(const float *L, const float *R, int n) {
    if (!atomic_load_explicit(&recording, memory_order_relaxed)) return;
    unsigned w = atomic_load_explicit(&rw, memory_order_relaxed);
    unsigned r = atomic_load_explicit(&rr, memory_order_acquire);
    for (int i = 0; i < n; i++) {
        if (w - r >= RING) break;                     /* SD card too slow: drop */
        unsigned k = (w % RING) * 2;
        ring[k] = to16(L[i]); ring[k + 1] = to16(R[i]);
        w++;
    }
    atomic_store_explicit(&rw, w, memory_order_release);
}

static void drain(void) {
    unsigned r = atomic_load_explicit(&rr, memory_order_relaxed);
    unsigned w = atomic_load_explicit(&rw, memory_order_acquire);
    while (r != w) {
        unsigned start = r % RING, n = w - r;
        if (start + n > RING) n = RING - start;        /* write up to the wrap */
        if (file) fwrite(&ring[start * 2], 4, n, file);
        frames_written += n;
        r += n;
    }
    atomic_store_explicit(&rr, r, memory_order_release);
}

static void *writer_thread(void *arg) {
    (void)arg;
    while (!atomic_load(&quit)) {
        usleep(60000);
        if (atomic_load(&recording) || file) drain();
        if (atomic_load(&stop_req) && file) {
            atomic_store(&recording, 0);
            drain();
            header(file, frames_written);
            fclose(file); file = NULL;
            atomic_store(&stop_req, 0);
        }
    }
    return NULL;
}

void rec_init(void) { pthread_create(&writer, NULL, writer_thread, NULL); }

int rec_start(void) {
    if (atomic_load(&recording) || file) return 1;
    char name[96]; make_name(name, sizeof name, "ember");
    FILE *f = fopen(name, "wb");
    if (!f) { set_msg("Could not create recordings folder"); return 0; }
    header(f, 0);
    frames_written = 0;
    atomic_store(&rr, atomic_load(&rw));
    file = f;
    atomic_store(&recording, 1);
    char m[128]; snprintf(m, sizeof m, "Recording %s", name + 11);
    set_msg(m);
    return 1;
}

void rec_stop(void) {
    if (!file) return;
    atomic_store(&stop_req, 1);
    char m[128]; snprintf(m, sizeof m, "Saved recording (%.0f s)", rec_seconds());
    set_msg(m);
}

int rec_active(void) { return atomic_load(&recording); }
float rec_seconds(void) { return (float)frames_written / RATE; }

void rec_shutdown(void) {
    if (file) {
        atomic_store(&stop_req, 1);
        for (int i = 0; i < 100 && atomic_load(&stop_req); i++) usleep(20000);
    }
    atomic_store(&quit, 1);
    pthread_join(writer, NULL);
}

/* loop export */
typedef struct { short *data; int len; char name[96]; } LoopJob;

static void *loop_writer(void *arg) {
    LoopJob *j = arg;
    FILE *f = fopen(j->name, "wb");
    if (f) {
        header(f, j->len);
        fwrite(j->data, 4, j->len, f);
        fclose(f);
        char m[128]; snprintf(m, sizeof m, "Saved %s", j->name + 11);
        set_msg(m);
    } else set_msg("Could not save loop");
    free(j->data); free(j);
    return NULL;
}

int rec_save_loop(const float *L, const float *R, int len) {
    if (len <= 0) { set_msg("No loop to save yet"); return 0; }
    LoopJob *j = malloc(sizeof *j);
    if (!j) return 0;
    j->data = malloc((size_t)len * 4);
    if (!j->data) { free(j); return 0; }
    for (int i = 0; i < len; i++) { j->data[2 * i] = to16(L[i]); j->data[2 * i + 1] = to16(R[i]); }
    j->len = len;
    make_name(j->name, sizeof j->name, "loop");
    pthread_t t;
    if (pthread_create(&t, NULL, loop_writer, j)) { free(j->data); free(j); return 0; }
    pthread_detach(t);
    set_msg("Saving loop...");
    return 1;
}
