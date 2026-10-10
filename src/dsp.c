/* Ember sound engine.
 *
 * Signal flow:
 *   8 voices (2 detuned polyBLEP saws + triangle sub, per-voice analog drift,
 *   4-pole ladder filter with saturation, amp + filter envelopes)
 *     -> tape stage (soft saturation + roll-off)          = carrier
 *   modulator (USB mic with drift-corrected buffer, or built-in vowel voice)
 *     -> 16-band channel vocoder with carrier flattening + sibilance
 *   blend(pads, vocoded) -> stereo chorus -> 8-line FDN reverb -> soft clip
 */
#include "dsp.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>

Params P;
EventQueue q_ui, q_midi;
volatile Meters M;
_Atomic int mic_connected;

#define TWO_PI 6.28318530718f
#define CTRL   16            /* control-rate block (samples) */

/* ------------------------------------------------------------------ utils */

static inline float clampf(float x, float a, float b) { return x < a ? a : x > b ? b : x; }

static inline float fast_tanh(float x) {
    if (x > 3.f) return 1.f;
    if (x < -3.f) return -1.f;
    float x2 = x * x;
    return x * (27.f + x2) / (27.f + 9.f * x2);
}

static inline float rnd(uint32_t *s) {           /* uniform -1..1 */
    uint32_t x = *s; x ^= x << 13; x ^= x >> 17; x ^= x << 5; *s = x;
    return (float)(int32_t)x * (1.0f / 2147483648.0f);
}

static inline float tc(float seconds) {          /* one-pole coef for a time constant */
    if (seconds < 1e-4f) return 1.f;
    return 1.f - expf(-1.f / (seconds * SR));
}

static inline float mtof(float m) { return 440.f * exp2f((m - 69.f) / 12.f); }

static inline float poly_blep(float t, float dt) {
    if (t < dt) { t /= dt; return t + t - t * t - 1.f; }
    if (t > 1.f - dt) { t = (t - 1.f) / dt; return t * t + t + t + 1.f; }
    return 0.f;
}

int evq_push(EventQueue *q, Event e) {
    unsigned w = atomic_load_explicit(&q->w, memory_order_relaxed);
    unsigned r = atomic_load_explicit(&q->r, memory_order_acquire);
    if (w - r >= EVQ_SIZE) return 0;
    q->buf[w % EVQ_SIZE] = e;
    atomic_store_explicit(&q->w, w + 1, memory_order_release);
    return 1;
}

static int evq_pop(EventQueue *q, Event *e) {
    unsigned r = atomic_load_explicit(&q->r, memory_order_relaxed);
    unsigned w = atomic_load_explicit(&q->w, memory_order_acquire);
    if (r == w) return 0;
    *e = q->buf[r % EVQ_SIZE];
    atomic_store_explicit(&q->r, r + 1, memory_order_release);
    return 1;
}

/* biquad, transposed direct form II */
typedef struct { float b0, b1, b2, a1, a2, z1, z2; } Biquad;

static void bq_bandpass(Biquad *f, float freq, float q) {
    float w0 = TWO_PI * freq / SR, cw = cosf(w0), alpha = sinf(w0) / (2.f * q);
    float a0 = 1.f + alpha;
    f->b0 = alpha / a0; f->b1 = 0.f; f->b2 = -alpha / a0;
    f->a1 = -2.f * cw / a0; f->a2 = (1.f - alpha) / a0;
}

static inline float bq_run(Biquad *f, float x) {
    float y = f->b0 * x + f->z1;
    f->z1 = f->b1 * x - f->a1 * y + f->z2;
    f->z2 = f->b2 * x - f->a2 * y;
    return y;
}

/* ----------------------------------------------------------------- voices */

enum { ENV_IDLE, ENV_ATTACK, ENV_DECAY, ENV_RELEASE };

typedef struct {
    int note, gate, sustained, stage;
    float vel, p1, p2, p3;
    float drift, drift_target; int drift_count;
    float aenv, fenv;
    float s1, s2, s3, s4;    /* ladder states */
    float g, k;              /* ladder coefs (control rate) */
    float dt1, dt2, dt3;     /* phase increments (control rate) */
    unsigned age;
    uint32_t rng;
} Voice;

static void loop_event(int type);
static Voice V[NVOICES];
static unsigned age_counter;
static int sustain_pedal;

static void voice_on(int note, int vel) {
    Voice *v = NULL;
    for (int i = 0; i < NVOICES; i++)                     /* same note: retrigger */
        if (V[i].stage != ENV_IDLE && V[i].note == note) { v = &V[i]; break; }
    if (!v) for (int i = 0; i < NVOICES; i++)              /* free voice */
        if (V[i].stage == ENV_IDLE) { v = &V[i]; break; }
    if (!v) {                                              /* steal quietest released */
        float best = 9.f;
        for (int i = 0; i < NVOICES; i++)
            if (V[i].stage == ENV_RELEASE && V[i].aenv < best) { best = V[i].aenv; v = &V[i]; }
    }
    if (!v) {                                              /* steal oldest */
        unsigned oldest = ~0u;
        for (int i = 0; i < NVOICES; i++)
            if (V[i].age < oldest) { oldest = V[i].age; v = &V[i]; }
    }
    if (v->stage == ENV_IDLE) {                            /* fresh start: random phases */
        v->p1 = (rnd(&v->rng) + 1.f) * 0.5f;
        v->p2 = (rnd(&v->rng) + 1.f) * 0.5f;
        v->p3 = 0.f;
        v->s1 = v->s2 = v->s3 = v->s4 = 0.f;
        v->fenv = 0.f;
    }
    v->note = note; v->vel = vel / 127.f;
    v->gate = 1; v->sustained = 0; v->stage = ENV_ATTACK;
    v->age = ++age_counter;
}

static void voice_off(int note) {
    for (int i = 0; i < NVOICES; i++) {
        Voice *v = &V[i];
        if (v->note == note && v->gate) {
            v->gate = 0;
            if (sustain_pedal) v->sustained = 1;
            else v->stage = ENV_RELEASE;
        }
    }
}

static void handle_event(Event e) {
    switch (e.type) {
    case EV_NOTE_ON:  if (e.vel) voice_on(e.note, e.vel); else voice_off(e.note); break;
    case EV_NOTE_OFF: voice_off(e.note); break;
    case EV_ALL_OFF:
        for (int i = 0; i < NVOICES; i++) {
            V[i].gate = 0; V[i].sustained = 0;
            if (V[i].stage != ENV_IDLE) V[i].stage = ENV_RELEASE;
        }
        break;
    case EV_LOOP: case EV_LOOP_UNDO: case EV_LOOP_CLEAR:
        loop_event(e.type);
        break;
    case EV_SUSTAIN:
        sustain_pedal = e.vel >= 64;
        if (!sustain_pedal)
            for (int i = 0; i < NVOICES; i++)
                if (V[i].sustained) { V[i].sustained = 0; if (!V[i].gate) V[i].stage = ENV_RELEASE; }
        break;
    }
}

/* ------------------------------------------------------------- mic buffer */

#define MIC_RING   16384
#define MIC_TARGET 768        /* samples kept in the buffer (16 ms) */
static float mic_ring[MIC_RING];
static _Atomic unsigned mic_w, mic_r;
static unsigned mic_rpos;
static float mic_frac, mic_ratio = 1.f, mic_fill_avg = MIC_TARGET;
static int mic_primed;

void mic_push(const float *x, int n) {
    unsigned w = atomic_load_explicit(&mic_w, memory_order_relaxed);
    unsigned r = atomic_load_explicit(&mic_r, memory_order_acquire);
    for (int i = 0; i < n; i++) {
        if (w - r >= MIC_RING - 4) break;    /* consumer stalled: drop */
        mic_ring[w & (MIC_RING - 1)] = x[i];
        w++;
    }
    atomic_store_explicit(&mic_w, w, memory_order_release);
}

void mic_set_connected(int on) { atomic_store(&mic_connected, on); }

/* Pull one block, resampling slightly faster or slower so the buffer stays
 * near MIC_TARGET. This absorbs the clock difference between the USB mic
 * and the handheld's codec, which would otherwise glitch every few minutes. */
static void mic_pull(float *out, int n) {
    unsigned w = atomic_load_explicit(&mic_w, memory_order_acquire);
    if (!atomic_load(&mic_connected)) {
        memset(out, 0, sizeof(float) * n);
        mic_primed = 0; mic_rpos = w; atomic_store(&mic_r, w);
        return;
    }
    int fill = (int)(w - mic_rpos);
    if (fill > MIC_RING / 2) { mic_rpos = w - MIC_TARGET; fill = MIC_TARGET; } /* way behind: jump */
    if (!mic_primed) {
        if (fill >= MIC_TARGET) { mic_primed = 1; mic_fill_avg = fill; mic_frac = 0.f; }
        else { memset(out, 0, sizeof(float) * n); return; }
    }
    mic_fill_avg += (fill - mic_fill_avg) * 0.02f;
    mic_ratio = 1.f + clampf((mic_fill_avg - MIC_TARGET) / MIC_TARGET * 0.01f, -0.005f, 0.005f);
    for (int i = 0; i < n; i++) {
        if ((int)(w - mic_rpos) < 2) {               /* underrun: re-prime */
            mic_primed = 0;
            for (; i < n; i++) out[i] = 0.f;
            break;
        }
        float a = mic_ring[mic_rpos & (MIC_RING - 1)];
        float b = mic_ring[(mic_rpos + 1) & (MIC_RING - 1)];
        out[i] = a + (b - a) * mic_frac;
        mic_frac += mic_ratio;
        while (mic_frac >= 1.f) { mic_frac -= 1.f; mic_rpos++; }
    }
    atomic_store_explicit(&mic_r, mic_rpos, memory_order_release);
}

/* ---------------------------------------------------------------- vocoder */

static Biquad an_bp[NBANDS], syn_bp[NBANDS];
static float env_mod[NBANDS], env_car[NBANDS], band_peak[NBANDS];
static float voc_atk, voc_rel, mic_hp_x, mic_hp_y, mic_env, gate_gain;
static uint32_t noise_rng = 0x1234567u;
static float sib_hp_prev;

/* vowel voice: glottal saw through three formants */
static Biquad formant[3];
static float vox_phase, vox_amp, vox_freq = 110.f, vox_vib, last_vowel = -1.f;
static const float VOWELS[5][3] = {          /* A  E  I  O  U  (Hz) */
    {730, 1090, 2440}, {530, 1840, 2480}, {270, 2290, 3010}, {570, 840, 2410}, {300, 870, 2240},
};

static void set_vowel(float v) {
    v = clampf(v, 0.f, 1.f) * 4.f;
    int i = (int)v; if (i > 3) i = 3;
    float t = v - i;
    static const float bw[3] = {90.f, 110.f, 150.f};
    for (int k = 0; k < 3; k++) {
        float f = VOWELS[i][k] + (VOWELS[i + 1][k] - VOWELS[i][k]) * t;
        bq_bandpass(&formant[k], f, f / bw[k]);
    }
}

/* ------------------------------------------------------------ chorus/verb */

#define CH_LEN 4096
static float ch_buf[CH_LEN]; static int ch_w; static float ch_ph1, ch_ph2;

#define FDN_N 8
#define FDN_MAX 6400
static float fdn_buf[FDN_N][FDN_MAX];
static int fdn_pos[FDN_N], fdn_len[FDN_N];
static float fdn_g[FDN_N], fdn_lp[FDN_N];
static float last_size = -1.f;
static const float FDN_MS[FDN_N] = {29.7f, 37.1f, 41.1f, 43.7f, 53.3f, 59.9f, 67.1f, 73.3f};
#define PRE_LEN 1200
static float pre_buf[PRE_LEN]; static int pre_w;
#define AP1 225
#define AP2 556
static float ap1[AP1], ap2[AP2]; static int ap1w, ap2w;

static void fdn_setup(float size) {
    float scale = 0.6f + 0.9f * size;
    float t60 = 1.5f + size * size * 10.f;
    for (int i = 0; i < FDN_N; i++) {
        int len = (int)(FDN_MS[i] * scale * SR / 1000.f);
        if (len >= FDN_MAX) len = FDN_MAX - 1;
        fdn_len[i] = len;
        if (fdn_pos[i] >= len) fdn_pos[i] = 0;
        fdn_g[i] = powf(10.f, -3.f * len / (t60 * SR));
    }
}

static inline float allpass(float *buf, int len, int *w, float x, float g) {
    float d = buf[*w];
    float y = -g * x + d;
    buf[*w] = x + g * y;
    if (++*w >= len) *w = 0;
    return y;
}

/* --------------------------------------------------------------- looper
 * Records the finished sound (after reverb, before volume), so the loop
 * sounds exactly like what you played and the WAV export matches it.
 * Overdub keeps the previous take for undo without any big copy in the audio
 * thread: the old samples are saved as the overdub passes over them, and
 * undo swaps them back a chunk per block (tap again to redo). */
#define LOOP_MAX (48000 * LOOP_MAX_SECONDS)
#define LOOP_MIN 12000          /* 0.25 s: shorter first takes are discarded */
#define LOOP_XF  480            /* 10 ms crossfade at the loop seam          */
static float *loopL, *loopR, *undoL, *undoR;
static int loop_state, loop_len, loop_pos, loop_xf;
static int dub_start, dub_covered;          /* region saved in the undo buffer   */
static int undo_left, undo_idx, loop_undone;/* chunked undo/redo in progress     */
static int loop_fade;                       /* samples left in a clear fade-out  */

static void loop_clear(void) {
    loop_state = LOOP_EMPTY; loop_len = loop_pos = 0;
    dub_covered = 0; undo_left = 0; loop_undone = 0; loop_xf = 0; loop_fade = 0;
}

static void loop_finish_take(void) {
    if (loop_pos < LOOP_MIN) { loop_clear(); return; }
    loop_len = loop_pos; loop_pos = 0;
    loop_xf = LOOP_XF;            /* blend what follows the end into the start */
    loop_state = LOOP_PLAY;
    dub_covered = 0; loop_undone = 0;
}

static void loop_event(int type) {
    if (!loopL) return;
    switch (type) {
    case EV_LOOP:
        switch (loop_state) {
        case LOOP_EMPTY: loop_state = LOOP_REC; loop_pos = 0; break;
        case LOOP_REC:   loop_finish_take(); break;
        case LOOP_PLAY:  loop_state = LOOP_DUB; dub_start = loop_pos; dub_covered = 0; loop_undone = 0; undo_left = 0; break;
        case LOOP_DUB:   loop_state = LOOP_PLAY; break;
        }
        break;
    case EV_LOOP_UNDO:
        if (loop_state == LOOP_REC) { loop_clear(); break; }
        if (loop_state == LOOP_DUB) loop_state = LOOP_PLAY;
        if (dub_covered > 0 && undo_left == 0) {
            undo_left = dub_covered; undo_idx = dub_start;
            loop_undone = !loop_undone;
        }
        break;
    case EV_LOOP_CLEAR:
        if (loop_state >= LOOP_PLAY) { loop_state = LOOP_PLAY; loop_fade = LOOP_XF; }  /* fade, then clear */
        else loop_clear();
        break;
    }
}

static void loop_undo_step(void) {               /* ~1 s of audio per call */
    int n = undo_left < 48000 ? undo_left : 48000;
    for (int k = 0; k < n; k++) {
        float t = loopL[undo_idx]; loopL[undo_idx] = undoL[undo_idx]; undoL[undo_idx] = t;
        t = loopR[undo_idx]; loopR[undo_idx] = undoR[undo_idx]; undoR[undo_idx] = t;
        if (++undo_idx >= loop_len) undo_idx = 0;
    }
    undo_left -= n;
}

int loop_snapshot(float **L, float **R) {
    if (loop_state != LOOP_PLAY && loop_state != LOOP_DUB) return 0;
    *L = loopL; *R = loopR;
    return loop_len;
}

/* --------------------------------------------------------------- engine */

static float tape_lp;

void engine_init(void) {
    memset(V, 0, sizeof V);
    for (int i = 0; i < NVOICES; i++) V[i].rng = 0x9E3779B9u * (i + 1);
    for (int b = 0; b < NBANDS; b++) {
        float f = 120.f * powf(8000.f / 120.f, b / (float)(NBANDS - 1));
        bq_bandpass(&an_bp[b], f, 4.5f);
        bq_bandpass(&syn_bp[b], f, 4.5f);
    }
    voc_atk = tc(0.002f); voc_rel = tc(0.035f);
    set_vowel(0.f);
    fdn_setup(0.5f);
    loopL = calloc(LOOP_MAX, sizeof(float)); loopR = calloc(LOOP_MAX, sizeof(float));
    undoL = calloc(LOOP_MAX, sizeof(float)); undoR = calloc(LOOP_MAX, sizeof(float));
    if (!loopL || !loopR || !undoL || !undoR) loopL = NULL;   /* looper disabled */
    loop_clear();
    P.looplevel = 0.9f; P.loopfb = 0.95f;
    /* sensible defaults; the UI loads a preset over these */
    P.detune = 0.45f; P.sub = 0.35f; P.drift = 0.4f; P.cutoff = 0.55f; P.reso = 0.25f;
    P.envamt = 0.3f; P.attack = 0.8f; P.release = 2.5f; P.tape = 0.4f;
    P.chorus = 0.6f; P.rvsize = 0.65f; P.rvmix = 0.35f;
    P.vocmix = 0.f; P.vocsrc = 1.f; P.micgain = 1.f; P.vowel = 0.f; P.sib = 0.3f; P.volume = 0.7f;
}

void engine_render(float *outL, float *outR, int n) {
    Event e;
    while (evq_pop(&q_ui, &e)) handle_event(e);
    while (evq_pop(&q_midi, &e)) handle_event(e);

    float modbuf[CTRL];
    float a_coef = tc(clampf(P.attack, 0.003f, 12.f) * 0.45f);
    float r_coef = tc(clampf(P.release, 0.02f, 15.f) * 0.3f);
    float d_coef = tc(1.2f);
    float fa_coef = tc(clampf(P.attack, 0.003f, 12.f) * 0.35f);
    float fd_coef = tc(1.8f);
    float vocmix = clampf(P.vocmix, 0.f, 1.f);
    int use_mic = P.vocsrc < 0.5f;
    float vowel = clampf(P.vowel + P.vowel_mod, 0.f, 1.f);
    if (fabsf(vowel - last_vowel) > 0.002f) { set_vowel(vowel); last_vowel = vowel; }
    if (fabsf(P.rvsize - last_size) > 0.002f) { fdn_setup(P.rvsize); last_size = P.rvsize; }
    float tape = clampf(P.tape, 0.f, 1.f);
    float tape_drive = 1.f + tape * 3.f, tape_norm = 1.f / sqrtf(tape_drive);
    float tape_c = 1.f - expf(-TWO_PI * (16000.f - tape * 9500.f) / SR);
    float wet = clampf(P.rvmix + P.swell * 0.6f, 0.f, 1.f);
    float chorus = clampf(P.chorus, 0.f, 1.f);
    float micgain = P.micgain;
    float sib = P.sib;
    float hp_c = expf(-TWO_PI * 90.f / SR);
    float peak = 0.f, mic_peak = 0.f;
    float looplevel = clampf(P.looplevel, 0.f, 1.f), loopfb = clampf(P.loopfb, 0.f, 1.f);
    if (undo_left > 0) loop_undo_step();
    float bandmax[NBANDS] = {0};

    /* lowest held note sets the vowel voice pitch (folded to 95..190 Hz) */
    int lowest = 999, gated = 0;
    for (int i = 0; i < NVOICES; i++)
        if (V[i].stage != ENV_IDLE && (V[i].gate || V[i].sustained)) { gated = 1; if (V[i].note < lowest) lowest = V[i].note; }
    if (gated) {
        float f = mtof((float)lowest);
        while (f > 190.f) f *= 0.5f;
        while (f < 95.f) f *= 2.f;
        vox_freq = f;
    }

    for (int base = 0; base < n; base += CTRL) {
        int len = n - base < CTRL ? n - base : CTRL;

        /* ---- control rate: voice coefficients ---- */
        float vib = 0.f;
        vox_vib += TWO_PI * 5.3f * CTRL / SR; if (vox_vib > TWO_PI) vox_vib -= TWO_PI;
        vib = sinf(vox_vib) * P.modwheel * 0.25f;            /* semitones */
        float env_max = 0.f;
        for (int i = 0; i < NVOICES; i++) {
            Voice *v = &V[i];
            if (v->stage == ENV_IDLE) continue;
            if (--v->drift_count <= 0) {
                v->drift_target = rnd(&v->rng);
                v->drift_count = 300 + (int)((rnd(&v->rng) + 1.f) * 400.f);
            }
            v->drift += (v->drift_target - v->drift) * 0.002f;
            float det = P.detune * 0.18f;                        /* semitones */
            float note = v->note + P.bend + vib + v->drift * P.drift * 0.15f;
            float f = mtof(note);
            v->dt1 = mtof(note - det) / SR;
            v->dt2 = mtof(note + det) / SR;
            v->dt3 = f * 0.5f / SR;
            float oct = P.cutoff * 8.2f + (v->note - 60) / 12.f * 0.4f
                      + v->fenv * P.envamt * 5.f + P.mod_cutoff * 3.f + v->vel * 0.5f;
            float fc = clampf(40.f * exp2f(oct), 20.f, 14000.f);
            v->g = 1.f - expf(-TWO_PI * fc / SR);
            v->k = clampf(P.reso, 0.f, 1.f) * 3.7f;
            if (v->aenv > env_max) env_max = v->aenv;
        }
        float vox_target = gated ? 1.f : env_max;
        vox_amp += (vox_target - vox_amp) * 0.05f;

        /* ---- modulator for this block ---- */
        if (use_mic) {
            mic_pull(modbuf, len);
            for (int s = 0; s < len; s++) {                      /* 90 Hz high-pass + gate */
                float x = modbuf[s] * micgain;
                float y = hp_c * (mic_hp_y + x - mic_hp_x);
                mic_hp_x = x; mic_hp_y = y;
                float a = fabsf(y);
                mic_env += (a - mic_env) * (a > mic_env ? 0.01f : 0.0005f);
                float gt = mic_env > 0.004f ? 1.f : 0.f;
                gate_gain += (gt - gate_gain) * 0.002f;
                modbuf[s] = y * gate_gain;
            }
        } else {
            float dt = vox_freq / SR;
            for (int s = 0; s < len; s++) {
                vox_phase += dt; if (vox_phase >= 1.f) vox_phase -= 1.f;
                float src = (2.f * vox_phase - 1.f - poly_blep(vox_phase, dt)) + rnd(&noise_rng) * 0.04f;
                float y = bq_run(&formant[0], src) + 0.6f * bq_run(&formant[1], src) + 0.3f * bq_run(&formant[2], src);
                modbuf[s] = y * vox_amp * 0.9f;
            }
        }

        /* ---- audio rate ---- */
        for (int s = 0; s < len; s++) {
            float mix = 0.f;
            for (int i = 0; i < NVOICES; i++) {
                Voice *v = &V[i];
                if (v->stage == ENV_IDLE) continue;
                /* envelopes */
                switch (v->stage) {
                case ENV_ATTACK:
                    v->aenv += (1.25f - v->aenv) * a_coef;
                    if (v->aenv >= 1.f) { v->aenv = 1.f; v->stage = ENV_DECAY; }
                    v->fenv += (1.f - v->fenv) * fa_coef;
                    break;
                case ENV_DECAY:
                    v->aenv += (0.85f - v->aenv) * d_coef;
                    v->fenv += (0.35f - v->fenv) * fd_coef;
                    break;
                case ENV_RELEASE:
                    v->aenv -= v->aenv * r_coef;
                    v->fenv -= v->fenv * r_coef;
                    if (v->aenv < 0.0001f) { v->stage = ENV_IDLE; v->aenv = 0.f; continue; }
                    break;
                }
                /* oscillators */
                v->p1 += v->dt1; if (v->p1 >= 1.f) v->p1 -= 1.f;
                v->p2 += v->dt2; if (v->p2 >= 1.f) v->p2 -= 1.f;
                v->p3 += v->dt3; if (v->p3 >= 1.f) v->p3 -= 1.f;
                float s1 = 2.f * v->p1 - 1.f - poly_blep(v->p1, v->dt1);
                float s2 = 2.f * v->p2 - 1.f - poly_blep(v->p2, v->dt2);
                float tri = 4.f * fabsf(v->p3 - 0.5f) - 1.f;
                float x = (s1 + s2) * 0.5f + tri * P.sub;
                /* ladder filter with saturating feedback */
                float in = x * 0.6f * (1.f + 0.5f * v->k) - v->k * fast_tanh(v->s4);
                v->s1 += v->g * (fast_tanh(in) - v->s1);
                v->s2 += v->g * (v->s1 - v->s2);
                v->s3 += v->g * (v->s2 - v->s3);
                v->s4 += v->g * (v->s3 - v->s4);
                mix += v->s4 * v->aenv * (0.45f + 0.55f * v->vel);
            }
            mix *= 0.32f;

            /* tape: soft saturation and roll-off */
            float t = fast_tanh(mix * tape_drive) * tape_norm;
            tape_lp += (t - tape_lp) * tape_c;
            float carrier = tape_lp;

            /* vocoder */
            float m = modbuf[s];
            float am = fabsf(m); if (am > mic_peak) mic_peak = am;
            float voc = 0.f;
            for (int b = 0; b < NBANDS; b++) {
                float a = fabsf(bq_run(&an_bp[b], m));
                env_mod[b] += (a - env_mod[b]) * (a > env_mod[b] ? voc_atk : voc_rel);
                if (env_mod[b] > bandmax[b]) bandmax[b] = env_mod[b];
            }
            if (vocmix > 0.001f) {
                for (int b = 0; b < NBANDS; b++) {
                    float c = bq_run(&syn_bp[b], carrier);
                    float ac = fabsf(c);
                    env_car[b] += (ac - env_car[b]) * (ac > env_car[b] ? voc_atk : voc_rel);
                    float gain = env_mod[b] / (env_car[b] + 0.002f);
                    if (gain > 25.f) gain = 25.f;
                    voc += c * gain;
                }
                /* sibilance: high-passed noise following the top bands */
                float nz = rnd(&noise_rng);
                float hp = nz - sib_hp_prev; sib_hp_prev = nz;
                voc += hp * (env_mod[NBANDS - 1] + env_mod[NBANDS - 2]) * sib * 1.5f;
                voc *= 0.75f;
            }
            float x = carrier * (1.f - vocmix) + voc * vocmix;

            /* stereo chorus (two slow triangle LFOs, Juno-style) */
            ch_buf[ch_w] = x;
            ch_ph1 += 0.55f / SR; if (ch_ph1 >= 1.f) ch_ph1 -= 1.f;
            ch_ph2 += 0.13f / SR; if (ch_ph2 >= 1.f) ch_ph2 -= 1.f;
            float tri1 = fabsf(ch_ph1 * 2.f - 1.f), tri2 = fabsf(ch_ph2 * 2.f - 1.f);
            float dL = (0.0065f + 0.0022f * tri1 + 0.0008f * tri2) * SR;
            float dR = (0.0065f + 0.0022f * (1.f - tri1) + 0.0008f * (1.f - tri2)) * SR;
            float rpL = ch_w - dL; if (rpL < 0) rpL += CH_LEN;
            float rpR = ch_w - dR; if (rpR < 0) rpR += CH_LEN;
            int iL = (int)rpL, iR = (int)rpR;
            float fL = rpL - iL, fR = rpR - iR;
            float wL = ch_buf[iL] + (ch_buf[(iL + 1) & (CH_LEN - 1)] - ch_buf[iL]) * fL;
            float wR = ch_buf[iR] + (ch_buf[(iR + 1) & (CH_LEN - 1)] - ch_buf[iR]) * fR;
            ch_w = (ch_w + 1) & (CH_LEN - 1);
            float norm = 1.f / (1.f + chorus * 0.5f);
            float L = (x + wL * chorus) * norm;
            float R = (x + wR * chorus) * norm;

            /* reverb: predelay -> 2 allpass diffusers -> 8-line FDN */
            float rin = (L + R) * 0.5f;
            float pd = pre_buf[pre_w]; pre_buf[pre_w] = rin; if (++pre_w >= PRE_LEN) pre_w = 0;
            pd = allpass(ap1, AP1, &ap1w, pd, 0.6f);
            pd = allpass(ap2, AP2, &ap2w, pd, 0.6f);
            float o[FDN_N];
            for (int i = 0; i < FDN_N; i++) {
                float d = fdn_buf[i][fdn_pos[i]];
                fdn_lp[i] += (d - fdn_lp[i]) * 0.42f;            /* damping: warm, dark tail */
                o[i] = fdn_lp[i];
            }
            /* 8x8 Hadamard mix */
            for (int h = 1; h < FDN_N; h <<= 1)
                for (int i = 0; i < FDN_N; i += h << 1)
                    for (int j = i; j < i + h; j++) {
                        float a = o[j], b = o[j + h];
                        o[j] = a + b; o[j + h] = a - b;
                    }
            for (int i = 0; i < FDN_N; i++) {
                fdn_buf[i][fdn_pos[i]] = o[i] * 0.35355339f * fdn_g[i] + pd;
                if (++fdn_pos[i] >= fdn_len[i]) fdn_pos[i] = 0;
            }
            float rvL = (fdn_lp[0] + fdn_lp[2] + fdn_lp[4] + fdn_lp[6]) * 0.4f;
            float rvR = (fdn_lp[1] + fdn_lp[3] + fdn_lp[5] + fdn_lp[7]) * 0.4f;
            L = L * (1.f - wet * 0.55f) + rvL * wet;
            R = R * (1.f - wet * 0.55f) + rvR * wet;

            /* looper */
            if (loop_state == LOOP_REC) {
                loopL[loop_pos] = L; loopR[loop_pos] = R;
                if (++loop_pos >= LOOP_MAX) loop_finish_take();
            } else if (loop_state >= LOOP_PLAY) {
                if (loop_xf > 0) {                      /* seam crossfade after a first take */
                    float w = 1.f - (float)loop_xf / LOOP_XF;
                    loopL[loop_pos] = loopL[loop_pos] * w + L * (1.f - w);
                    loopR[loop_pos] = loopR[loop_pos] * w + R * (1.f - w);
                    loop_xf--;
                }
                float lL = loopL[loop_pos], lR = loopR[loop_pos];
                if (loop_state == LOOP_DUB) {
                    if (dub_covered < loop_len) { undoL[loop_pos] = lL; undoR[loop_pos] = lR; dub_covered++; }
                    loopL[loop_pos] = lL * loopfb + L;
                    loopR[loop_pos] = lR * loopfb + R;
                }
                if (++loop_pos >= loop_len) loop_pos = 0;
                float g = looplevel;
                if (loop_fade > 0) { g *= (float)loop_fade / LOOP_XF; if (--loop_fade == 0) loop_clear(); }
                L += lL * g; R += lR * g;
            }

            /* master: volume, gentle soft clip */
            float vol = P.volume * 1.4f;
            L = fast_tanh(L * vol); R = fast_tanh(R * vol);
            float ap = fabsf(L) > fabsf(R) ? fabsf(L) : fabsf(R);
            if (ap > peak) peak = ap;
            outL[base + s] = L; outR[base + s] = R;
        }
    }

    /* publish meters */
    uint64_t nb[2] = {0, 0};
    int active = 0;
    for (int i = 0; i < NVOICES; i++) {
        if (V[i].stage != ENV_IDLE) active++;
        if (V[i].stage != ENV_IDLE && (V[i].gate || V[i].sustained)) nb[V[i].note >> 6] |= 1ull << (V[i].note & 63);
    }
    M.notes[0] = nb[0]; M.notes[1] = nb[1];
    M.voices_active = active;
    M.loop_state = loop_state; M.loop_pos = loop_pos; M.loop_len = loop_len;
    M.loop_can_undo = dub_covered > 0; M.loop_undone = loop_undone;
    for (int b = 0; b < NBANDS; b++) {
        float v = bandmax[b] * 6.f;
        band_peak[b] = v > band_peak[b] ? v : band_peak[b] * 0.85f;
        M.band[b] = band_peak[b];
    }
    M.out_peak = peak > M.out_peak ? peak : M.out_peak * 0.9f;
    M.mic_level = mic_peak > M.mic_level ? mic_peak : M.mic_level * 0.9f;
}
