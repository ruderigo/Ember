/* Ember device I/O.
 *  - output thread: renders the engine straight into ALSA (no sound server)
 *  - mic thread:    finds a USB capture device by driver, survives hot-plug
 *  - MIDI thread:   ALSA sequencer client that auto-connects every hardware
 *                   MIDI port, including ones plugged in later
 * Environment overrides: EMBER_OUT (default plughw:0,0), EMBER_MIC, EMBER_PERIOD.
 */
#define _GNU_SOURCE
#include "io.h"
#include "dsp.h"
#include "rec.h"
#include <alsa/asoundlib.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <poll.h>

volatile IoStatus IO;
volatile int io_quit;
static pthread_t th_out, th_mic, th_midi;

static void msleep(int ms) { struct timespec t = { ms / 1000, (ms % 1000) * 1000000L }; nanosleep(&t, NULL); }
static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static void set_str(volatile char *dst, size_t n, const char *src) { snprintf((char *)dst, n, "%s", src); }

static void try_realtime(int prio) {
    struct sched_param sp = { .sched_priority = prio };
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);   /* fine if refused */
}

/* --------------------------------------------------------------- output */

static void *out_thread(void *arg) {
    (void)arg;
    const char *dev = getenv("EMBER_OUT") ? getenv("EMBER_OUT") : "plughw:0,0";
    int want = getenv("EMBER_PERIOD") ? atoi(getenv("EMBER_PERIOD")) : 256;
    if (want < 64 || want > 2048) want = 256;
    try_realtime(70);
    static float L[2048], R[2048];
    static short buf[4096];

    while (!io_quit) {
        snd_pcm_t *pcm;
        int err = snd_pcm_open(&pcm, dev, SND_PCM_STREAM_PLAYBACK, 0);
        if (err < 0) {
            char m[96];
            snprintf(m, sizeof m, "%s %s", dev, err == -EBUSY ? "busy - retrying" : snd_strerror(err));
            set_str(IO.out_name, sizeof IO.out_name, m);
            IO.out_ok = 0;
            msleep(500);
            continue;
        }
        err = snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
                                 2, 48000, 1, (unsigned)(want * 3 * 1000000.0 / 48000));
        if (err < 0) {
            char m[96]; snprintf(m, sizeof m, "%s setup failed: %s", dev, snd_strerror(err));
            set_str(IO.out_name, sizeof IO.out_name, m);
            snd_pcm_close(pcm); msleep(1000); continue;
        }
        snd_pcm_uframes_t bufsz, per;
        snd_pcm_get_params(pcm, &bufsz, &per);
        if (per > 2048) per = 2048;
        IO.period = (int)per; IO.rate = 48000;
        set_str(IO.out_name, sizeof IO.out_name, dev);
        IO.out_ok = 1;

        double load = 0.0;
        while (!io_quit) {
            double t0 = now_s();
            engine_render(L, R, (int)per);
            double dt = now_s() - t0;
            load += (dt / (per / 48000.0) - load) * 0.05;
            M.cpu = (float)load;
            rec_push(L, R, (int)per);
            for (snd_pcm_uframes_t i = 0; i < per; i++) {
                buf[2 * i]     = (short)(L[i] * 32700.f);
                buf[2 * i + 1] = (short)(R[i] * 32700.f);
            }
            snd_pcm_sframes_t w = snd_pcm_writei(pcm, buf, per);
            if (w == -EPIPE || w == -ESTRPIPE || w == -EINTR) {
                IO.xruns++;
                snd_pcm_recover(pcm, (int)w, 1);
            } else if (w < 0) {
                break;  /* device gone: reopen */
            }
        }
        IO.out_ok = 0;
        snd_pcm_drop(pcm);
        snd_pcm_close(pcm);
    }
    return NULL;
}

/* ------------------------------------------------------------------ mic */

/* First USB-Audio card that has a capture device. The MIDI keyboard is also
 * a USB-Audio card but has no capture PCM, so it is skipped automatically. */
static int find_usb_mic(char *pcmname, size_t pn, char *label, size_t ln) {
    int card = -1;
    while (snd_card_next(&card) == 0 && card >= 0) {
        char hw[16]; snprintf(hw, sizeof hw, "hw:%d", card);
        snd_ctl_t *ctl;
        if (snd_ctl_open(&ctl, hw, 0) < 0) continue;
        snd_ctl_card_info_t *info; snd_ctl_card_info_alloca(&info);
        if (snd_ctl_card_info(ctl, info) == 0 && strcmp(snd_ctl_card_info_get_driver(info), "USB-Audio") == 0) {
            int dev = -1;
            while (snd_ctl_pcm_next_device(ctl, &dev) == 0 && dev >= 0) {
                snd_pcm_info_t *pi; snd_pcm_info_alloca(&pi);
                snd_pcm_info_set_device(pi, dev);
                snd_pcm_info_set_subdevice(pi, 0);
                snd_pcm_info_set_stream(pi, SND_PCM_STREAM_CAPTURE);
                if (snd_ctl_pcm_info(ctl, pi) >= 0) {
                    snprintf(pcmname, pn, "plughw:%d,%d", card, dev);
                    snprintf(label, ln, "%s", snd_ctl_card_info_get_name(info));
                    snd_ctl_close(ctl);
                    return 1;
                }
            }
        }
        snd_ctl_close(ctl);
    }
    return 0;
}

static void *mic_thread(void *arg) {
    (void)arg;
    try_realtime(60);
    static short in[512];
    static float f[512];
    while (!io_quit) {
        char dev[64], label[96];
        if (getenv("EMBER_MIC")) { snprintf(dev, sizeof dev, "%s", getenv("EMBER_MIC")); snprintf(label, sizeof label, "%s", dev); }
        else if (!find_usb_mic(dev, sizeof dev, label, sizeof label)) { IO.mic_name[0] = 0; msleep(1500); continue; }

        snd_pcm_t *pcm;
        if (snd_pcm_open(&pcm, dev, SND_PCM_STREAM_CAPTURE, 0) < 0) { msleep(1500); continue; }
        if (snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
                               1, 48000, 1, 16000) < 0) {
            snd_pcm_close(pcm); msleep(1500); continue;
        }
        set_str(IO.mic_name, sizeof IO.mic_name, label);
        int connected = 0;
        while (!io_quit) {
            snd_pcm_sframes_t r = snd_pcm_readi(pcm, in, 256);
            if (r == -EPIPE || r == -ESTRPIPE || r == -EINTR) { snd_pcm_recover(pcm, (int)r, 1); continue; }
            if (r < 0) break;                               /* unplugged */
            for (int i = 0; i < r; i++) f[i] = in[i] * (1.f / 32768.f);
            mic_push(f, (int)r);
            if (!connected) { mic_set_connected(1); connected = 1; }
        }
        mic_set_connected(0);
        IO.mic_name[0] = 0;
        snd_pcm_close(pcm);
        msleep(800);
    }
    return NULL;
}

/* ----------------------------------------------------------------- MIDI */

static snd_seq_t *seq;
static int my_port;

static int wanted_port(snd_seq_client_info_t *ci, snd_seq_port_info_t *pi) {
    int client = snd_seq_port_info_get_client(pi);
    if (client == SND_SEQ_CLIENT_SYSTEM || client == snd_seq_client_id(seq)) return 0;
    if (strstr(snd_seq_client_info_get_name(ci), "Midi Through")) return 0;
    unsigned caps = snd_seq_port_info_get_capability(pi);
    if ((caps & (SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ)) != (SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ)) return 0;
    if (caps & SND_SEQ_PORT_CAP_NO_EXPORT) return 0;
    return 1;
}

/* connect every hardware source and rebuild the status name list */
static void scan_midi(int connect) {
    char names[96] = "";
    snd_seq_client_info_t *ci; snd_seq_port_info_t *pi;
    snd_seq_client_info_alloca(&ci); snd_seq_port_info_alloca(&pi);
    snd_seq_client_info_set_client(ci, -1);
    while (snd_seq_query_next_client(seq, ci) >= 0) {
        int client = snd_seq_client_info_get_client(ci);
        int any = 0;
        snd_seq_port_info_set_client(pi, client);
        snd_seq_port_info_set_port(pi, -1);
        while (snd_seq_query_next_port(seq, pi) >= 0) {
            if (!wanted_port(ci, pi)) continue;
            if (connect) snd_seq_connect_from(seq, my_port, client, snd_seq_port_info_get_port(pi));
            any = 1;
        }
        if (any) {
            if (names[0]) strncat(names, ", ", sizeof names - strlen(names) - 1);
            strncat(names, snd_seq_client_info_get_name(ci), sizeof names - strlen(names) - 1);
        }
    }
    set_str(IO.midi_name, sizeof IO.midi_name, names);
}

static void send(int type, int note, int vel) {
    Event e = { (uint8_t)type, (uint8_t)note, (uint8_t)vel, 0 };
    evq_push(&q_midi, e);
}

static void handle_cc(int cc, int v) {
    float x = v / 127.f;
    IO.midi_last_cc = cc; IO.midi_last_val = v;
    switch (cc) {
    case 1:  P.modwheel = x; break;
    case 2: case 11: P.vocmix = x; break;     /* breath / expression -> vocoder blend */
    case 7:  P.volume = x; break;
    case 64: send(EV_SUSTAIN, 0, v); break;
    case 71: P.reso = x; break;
    case 72: P.release = 0.05f + x * x * 12.f; break;
    case 73: P.attack = 0.005f + x * x * 8.f; break;
    case 74: P.cutoff = x; break;
    case 91: P.rvmix = x; break;
    case 93: P.chorus = x; break;
    case 120: case 123: send(EV_ALL_OFF, 0, 0); break;
    }
}

static void *midi_thread(void *arg) {
    (void)arg;
    while (!io_quit && snd_seq_open(&seq, "default", SND_SEQ_OPEN_INPUT, SND_SEQ_NONBLOCK) < 0) msleep(2000);
    if (io_quit) return NULL;
    snd_seq_set_client_name(seq, "Ember");
    my_port = snd_seq_create_simple_port(seq, "Ember In",
        SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
        SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
    snd_seq_connect_from(seq, my_port, SND_SEQ_CLIENT_SYSTEM, SND_SEQ_PORT_SYSTEM_ANNOUNCE);
    scan_midi(1);

    int npfd = snd_seq_poll_descriptors_count(seq, POLLIN);
    struct pollfd pfd[4];
    if (npfd > 4) npfd = 4;
    snd_seq_poll_descriptors(seq, pfd, npfd, POLLIN);

    while (!io_quit) {
        if (poll(pfd, npfd, 250) <= 0) continue;
        snd_seq_event_t *ev;
        while (snd_seq_event_input(seq, &ev) >= 0 && ev) {
            switch (ev->type) {
            case SND_SEQ_EVENT_NOTEON:  send(EV_NOTE_ON, ev->data.note.note, ev->data.note.velocity); break;
            case SND_SEQ_EVENT_NOTEOFF: send(EV_NOTE_OFF, ev->data.note.note, 0); break;
            case SND_SEQ_EVENT_CONTROLLER: handle_cc((int)ev->data.control.param, ev->data.control.value); break;
            case SND_SEQ_EVENT_PITCHBEND: P.bend = ev->data.control.value / 8192.f * 2.f; break;
            case SND_SEQ_EVENT_PORT_START: scan_midi(1); break;   /* new keyboard plugged in */
            case SND_SEQ_EVENT_PORT_EXIT:
            case SND_SEQ_EVENT_CLIENT_EXIT: scan_midi(0); break;
            }
        }
    }
    snd_seq_close(seq);
    return NULL;
}

/* ---------------------------------------------------------------- start */

int io_start(void) {
    snd_lib_error_set_handler(NULL);
    set_str(IO.out_name, sizeof IO.out_name, "starting");
    if (pthread_create(&th_out, NULL, out_thread, NULL)) return -1;
    pthread_create(&th_mic, NULL, mic_thread, NULL);
    pthread_create(&th_midi, NULL, midi_thread, NULL);
    return 0;
}

void io_stop(void) {
    io_quit = 1;
    pthread_join(th_out, NULL);
    pthread_join(th_mic, NULL);
    pthread_join(th_midi, NULL);
}
