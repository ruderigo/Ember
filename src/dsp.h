/* Ember - warm analog pad synth + vocoder for RK3326 handhelds.
 * dsp.h: the sound engine interface. The engine is pure C with no I/O,
 * so it can be driven by ALSA on the device or rendered offline for tests. */
#pragma once
#include <stdint.h>
#include <stdatomic.h>

#define SR       48000.0f
#define NVOICES  8
#define NBANDS   16

/* All engine parameters are plain floats written by the UI/MIDI threads and
 * read by the audio thread. Torn reads of a float are not possible on arm64,
 * and a one-block-late update is inaudible, so no locking is needed. */
typedef struct {
    /* tone */
    float detune;   /* 0..1  oscillator spread (0..18 cents)          */
    float sub;      /* 0..1  sub oscillator (triangle, -1 oct) level  */
    float drift;    /* 0..1  analog pitch drift per voice             */
    float cutoff;   /* 0..1  ladder cutoff (40 Hz .. 12 kHz, exp)      */
    float reso;     /* 0..1  ladder resonance                         */
    float envamt;   /* 0..1  filter envelope depth                    */
    float attack;   /* seconds                                        */
    float release;  /* seconds                                        */
    float tape;     /* 0..1  saturation + high-end roll-off           */
    /* space */
    float chorus;   /* 0..1  stereo chorus mix                        */
    float rvsize;   /* 0..1  reverb size / decay                      */
    float rvmix;    /* 0..1  reverb wet                               */
    /* voice */
    float vocmix;   /* 0..1  0 = plain pads, 1 = fully vocoded        */
    float vocsrc;   /* 0 = USB mic, 1 = built-in vowel voice          */
    float micgain;  /* 0..4                                           */
    float vowel;    /* 0..1  A E I O U morph                          */
    float sib;      /* 0..1  sibilance (noise in the top bands)       */
    float volume;   /* 0..1                                           */
    /* looper */
    float looplevel;  /* 0..1 loop playback level                    */
    float loopfb;     /* 0.5..1 how much of the loop survives each overdub pass */
    /* live modulation (sticks, MIDI CCs) */
    float mod_cutoff; /* -1..1 octaves*3 offset                       */
    float bend;       /* semitones                                    */
    float swell;      /* 0..1 extra reverb                            */
    float modwheel;   /* 0..1 vibrato depth                           */
    float vowel_mod;  /* -1..1 added to vowel                         */
} Params;

extern Params P;

enum { EV_NOTE_ON = 1, EV_NOTE_OFF, EV_ALL_OFF, EV_SUSTAIN,
       EV_LOOP,        /* empty->record, record->play, play->overdub, overdub->play */
       EV_LOOP_UNDO,   /* undo / redo the last overdub; cancels a first take */
       EV_LOOP_CLEAR };
enum { LOOP_EMPTY, LOOP_REC, LOOP_PLAY, LOOP_DUB };
#define LOOP_MAX_SECONDS 64
typedef struct { uint8_t type, note, vel, pad; } Event;

/* single-producer / single-consumer queue */
#define EVQ_SIZE 256
typedef struct { Event buf[EVQ_SIZE]; _Atomic unsigned w, r; } EventQueue;
int evq_push(EventQueue *q, Event e);
extern EventQueue q_ui, q_midi;

/* meters published by the audio thread for the UI */
typedef struct {
    float band[NBANDS];     /* modulator band envelopes, 0..~1     */
    float out_peak;         /* output peak, 0..1                    */
    float mic_level;        /* modulator input level, 0..1          */
    float cpu;              /* render time / real time, 0..1        */
    uint64_t notes[2];      /* MIDI notes currently held (bitmap)   */
    int voices_active;
    int loop_state, loop_pos, loop_len;   /* looper position in samples */
    int loop_can_undo, loop_undone;
} Meters;
extern volatile Meters M;

void engine_init(void);
void engine_render(float *L, float *R, int n);

/* copy of the current loop for saving (returns length in samples, 0 if none) */
int loop_snapshot(float **L, float **R);

/* USB mic path: capture thread pushes, audio thread pulls with drift correction */
void mic_push(const float *x, int n);
void mic_set_connected(int on);
extern _Atomic int mic_connected;
