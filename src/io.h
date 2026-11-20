/* Ember device I/O: ALSA output, USB mic capture, ALSA sequencer MIDI. */
#pragma once

typedef struct {
    char out_name[96];   /* output device in use, or why it's not running */
    int  out_ok;         /* 1 = audio running, 0 = retrying (busy / missing) */
    int  period, rate;   /* period size in frames, sample rate */
    char mic_name[96];   /* USB mic name, empty when none */
    char mic_why[96];    /* why the mic isn't running (a device that won't open), empty when fine or none */
    char mic_dev[64];    /* the capture device open, e.g. plughw:1,0 */
    int  mic_level;      /* 0..100: the mic's peak, falling back over ~1 s */
    long mic_silent_ms;  /* how long it has read only silence (a muted capture) */
    char midi_name[96];  /* connected MIDI device names, empty when none */
    int  midi_last_cc, midi_last_val;
    int  xruns;
} IoStatus;

extern volatile IoStatus IO;
extern volatile int io_quit;

int  io_start(void);     /* starts output, mic and MIDI threads */
void io_stop(void);
