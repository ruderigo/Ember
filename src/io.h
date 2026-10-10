/* Ember device I/O: ALSA output, USB mic capture, ALSA sequencer MIDI. */
#pragma once

typedef struct {
    char out_name[96];   /* output device in use, or why it's not running */
    int  out_ok;         /* 1 = audio running, 0 = retrying (busy / missing) */
    int  period, rate;   /* period size in frames, sample rate */
    char mic_name[96];   /* USB mic name, empty when none */
    char midi_name[96];  /* connected MIDI device names, empty when none */
    int  midi_last_cc, midi_last_val;
    int  xruns;
} IoStatus;

extern volatile IoStatus IO;
extern volatile int io_quit;

int  io_start(void);     /* starts output, mic and MIDI threads */
void io_stop(void);
