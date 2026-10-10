/* Ember - warm analog pads + vocoder for the R36Max (ArkOS4Clone) and other
 * RK3326 handhelds. main.c: screen, controls, chords, presets, settings. */
#include "dsp.h"
#include "io.h"
#include "font.h"
#include "rec.h"
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define W 720
#define H 720
#ifndef EMBER_VERSION
#define EMBER_VERSION "dev"      /* the Makefile passes the contents of VERSION */
#endif
#define VERSION EMBER_VERSION

/* ------------------------------------------------------------- palette */
typedef struct { Uint8 r, g, b; } Col;
static const Col C_BG = {0x15, 0x10, 0x0d}, C_PANEL = {0x24, 0x1b, 0x15}, C_LINE = {0x3a, 0x2c, 0x22},
                 C_AMBER = {0xf2, 0xa5, 0x41}, C_ORANGE = {0xe0, 0x70, 0x3a}, C_CREAM = {0xf1, 0xe4, 0xcc},
                 C_DIM = {0x9c, 0x8a, 0x76}, C_FAINT = {0x5d, 0x4d, 0x40}, C_OK = {0x9f, 0xbf, 0x6a},
                 C_ERR = {0xd9, 0x58, 0x3b}, C_TRACK = {0x2e, 0x23, 0x1c};

static SDL_Renderer *ren;
static SDL_Texture *tex_small, *tex_med, *tex_big;

static SDL_Texture *font_texture(const EmberFont *f) {
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, f->w * 95, f->h, 32, SDL_PIXELFORMAT_RGBA32);
    Uint8 *px = s->pixels;
    for (int g = 0; g < 95; g++)
        for (int y = 0; y < f->h; y++)
            for (int x = 0; x < f->w; x++) {
                Uint8 *p = px + y * s->pitch + (g * f->w + x) * 4;
                p[0] = p[1] = p[2] = 255;
                p[3] = f->data[(g * f->h + y) * f->w + x];
            }
    SDL_Texture *t = SDL_CreateTextureFromSurface(ren, s);
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    SDL_FreeSurface(s);
    return t;
}

static void fill(int x, int y, int w, int h, Col c) {
    SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, 255);
    SDL_Rect r = {x, y, w, h};
    SDL_RenderFillRect(ren, &r);
}

static int text_w(const EmberFont *f, const char *s, int spacing) {
    int n = (int)strlen(s);
    return n * f->w + (n > 0 ? (n - 1) * spacing : 0);
}

/* align: 0 left, 1 center, 2 right */
static void text(const EmberFont *f, int x, int y, const char *s, Col c, int align, int spacing) {
    SDL_Texture *t = f == &font_small ? tex_small : f == &font_med ? tex_med : tex_big;
    int w = text_w(f, s, spacing);
    if (align == 1) x -= w / 2; else if (align == 2) x -= w;
    SDL_SetTextureColorMod(t, c.r, c.g, c.b);
    for (; *s; s++) {
        int ch = (unsigned char)*s;
        if (ch < 32 || ch > 126) ch = '?';
        SDL_Rect src = {(ch - 32) * f->w, 0, f->w, f->h}, dst = {x, y, f->w, f->h};
        SDL_RenderCopy(ren, t, &src, &dst);
        x += f->w + spacing;
    }
}

/* ------------------------------------------------------- music theory */
static const char *NOTE_NAMES[12] = {"C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"};
static const char *SCALE_NAMES[5] = {"Major", "Minor", "Dorian", "Mixolydian", "Lydian"};
static const int SCALES[5][7] = {
    {0, 2, 4, 5, 7, 9, 11}, {0, 2, 3, 5, 7, 8, 10}, {0, 2, 3, 5, 7, 9, 10},
    {0, 2, 4, 5, 7, 9, 10}, {0, 2, 4, 6, 7, 9, 11},
};
static const char *CHORD_TYPES[5] = {"Triad", "Seventh", "Sus2", "Add9", "Open"};

/* UI-side settings (not engine params) */
static struct { float preset, key, scale, chord, octave, bass, hold, rec, saveloop, slot, savepreset; } U = {0, 0, 0, 1, 0, 1, 0, 0, 0, 0, 0};
static int shot_mode;

static int degree_note(int d) {
    int sc = (int)U.scale;
    int base = 48 + (int)U.key + (int)U.octave * 12;
    return base + 12 * (d / 7) + SCALES[sc][d % 7];
}

static int build_chord(int d, int *out) {
    int n = 0, t = (int)U.chord;
    if (U.bass > 0.5f) out[n++] = degree_note(d) - 12;
    switch (t) {
    case 0: out[n++] = degree_note(d); out[n++] = degree_note(d + 2); out[n++] = degree_note(d + 4); break;
    case 1: out[n++] = degree_note(d); out[n++] = degree_note(d + 2); out[n++] = degree_note(d + 4); out[n++] = degree_note(d + 6); break;
    case 2: out[n++] = degree_note(d); out[n++] = degree_note(d + 1); out[n++] = degree_note(d + 4); break;
    case 3: out[n++] = degree_note(d); out[n++] = degree_note(d + 2); out[n++] = degree_note(d + 4); out[n++] = degree_note(d + 8); break;
    default: out[n++] = degree_note(d); out[n++] = degree_note(d + 4); out[n++] = degree_note(d + 7); out[n++] = degree_note(d + 9); break;
    }
    return n;
}

/* name whatever notes are sounding (works for the buttons and the MIDI keys) */
static void chord_name(char *out, size_t on) {
    int notes[128], n = 0, pcs = 0;
    for (int i = 0; i < 128; i++)
        if (M.notes[i >> 6] & (1ull << (i & 63))) notes[n++] = i;
    if (!n) { snprintf(out, on, "-"); return; }
    for (int i = 0; i < n; i++) pcs |= 1 << (notes[i] % 12);
    static const struct { int mask; const char *suffix; } T[] = {
        {0x091, ""}, {0x089, "m"}, {0x049, "dim"}, {0x111, "aug"}, {0x085, "sus2"}, {0x0a1, "sus4"},
        {0x891, "maj7"}, {0x491, "7"}, {0x489, "m7"}, {0x449, "m7b5"}, {0x095, "add9"}, {0x08d, "madd9"},
        {0x895, "maj9"}, {0x48d, "m9"}, {0x081, "5"}, {0x001, ""},
    };
    for (unsigned t = 0; t < sizeof T / sizeof T[0]; t++)
        for (int root = 0; root < 12; root++) {
            int rot = ((pcs >> root) | (pcs << (12 - root))) & 0xfff;
            if (rot == T[t].mask) {
                int bass = notes[0] % 12;
                if (bass != root) snprintf(out, on, "%s%s/%s", NOTE_NAMES[root], T[t].suffix, NOTE_NAMES[bass]);
                else snprintf(out, on, "%s%s", NOTE_NAMES[root], T[t].suffix);
                return;
            }
        }
    /* unknown cluster: list the notes */
    out[0] = 0;
    for (int i = 0; i < n && i < 5; i++) {
        strncat(out, NOTE_NAMES[notes[i] % 12], on - strlen(out) - 1);
        if (i < n - 1 && i < 4) strncat(out, " ", on - strlen(out) - 1);
    }
}

/* ------------------------------------------------------------ params */
typedef enum { F_PCT, F_SEC, F_HZ, F_KEY, F_SCALE, F_CHORD, F_SRC, F_ONOFF, F_OCT, F_PRESET, F_GAIN, F_REC, F_ACTION, F_SLOT, F_SAVE } Fmt;
typedef struct { const char *id, *name; float *v; float min, max, step; Fmt fmt; } Param;

/* Everything a preset stores. Key, scale, octave and volume stay as you set them. */
typedef struct { const char *id; float *v; } Field;
static Field FIELDS[] = {
    {"detune", &P.detune}, {"sub", &P.sub}, {"drift", &P.drift}, {"cutoff", &P.cutoff},
    {"reso", &P.reso}, {"envamt", &P.envamt}, {"attack", &P.attack}, {"release", &P.release},
    {"tape", &P.tape}, {"chorus", &P.chorus}, {"rvsize", &P.rvsize}, {"rvmix", &P.rvmix},
    {"vocmix", &P.vocmix}, {"vocsrc", &P.vocsrc}, {"sib", &P.sib}, {"vowel", &P.vowel},
    {"chord", &U.chord}, {"bass", &U.bass},
};
#define NFIELDS (int)(sizeof FIELDS / sizeof FIELDS[0])

#define NFACTORY 16
#define NUSER 8
static const char *FACTORY_NAMES[NFACTORY] = {
    "Ember Pad", "Velvet Strings", "Tape Choir", "Robot Hymn", "Slow Bloom", "Warm Drone",
    "Juno Dawn", "Brass Haze", "Felt Keys", "Midnight Choir", "Whisper Pad", "Mic Choir",
    "Cassette Sunset", "Deep Sub Drone", "Shimmer Wash", "Soft Keys",
};
/* detune sub  drift cutoff reso  env   attack release tape chorus size  mix   voc  src sib   vowel chord bass */
static const float FACTORY[NFACTORY][18] = {
    {0.50f, 0.35f, 0.45f, 0.50f, 0.20f, 0.35f, 1.20f, 3.0f, 0.45f, 0.65f, 0.70f, 0.35f, 0.00f, 1, 0.30f, 0.00f, 1, 1},
    {0.70f, 0.15f, 0.50f, 0.58f, 0.10f, 0.25f, 1.80f, 4.0f, 0.30f, 0.80f, 0.75f, 0.40f, 0.00f, 1, 0.30f, 0.00f, 1, 1},
    {0.45f, 0.25f, 0.50f, 0.62f, 0.15f, 0.20f, 1.50f, 4.0f, 0.50f, 0.60f, 0.80f, 0.45f, 0.85f, 1, 0.25f, 0.00f, 1, 1},
    {0.35f, 0.30f, 0.30f, 0.70f, 0.10f, 0.10f, 0.02f, 0.8f, 0.35f, 0.50f, 0.60f, 0.30f, 1.00f, 0, 0.45f, 0.00f, 1, 1},
    {0.55f, 0.40f, 0.60f, 0.30f, 0.35f, 0.70f, 4.00f, 6.0f, 0.55f, 0.55f, 0.90f, 0.50f, 0.00f, 1, 0.30f, 0.00f, 1, 1},
    {0.30f, 0.60f, 0.70f, 0.40f, 0.30f, 0.20f, 2.50f, 8.0f, 0.70f, 0.40f, 0.85f, 0.45f, 0.00f, 1, 0.30f, 0.00f, 1, 1},
    /* Juno Dawn: bright, lush chorus, add9 shimmer */
    {0.30f, 0.20f, 0.25f, 0.62f, 0.15f, 0.40f, 0.40f, 2.0f, 0.30f, 1.00f, 0.55f, 0.30f, 0.00f, 1, 0.30f, 0.00f, 3, 1},
    /* Brass Haze: filter blooms open on every chord */
    {0.40f, 0.25f, 0.35f, 0.38f, 0.20f, 0.85f, 0.25f, 1.5f, 0.60f, 0.45f, 0.60f, 0.30f, 0.00f, 1, 0.30f, 0.00f, 0, 1},
    /* Felt Keys: soft, dark, quick attack */
    {0.15f, 0.50f, 0.20f, 0.35f, 0.05f, 0.60f, 0.01f, 1.8f, 0.50f, 0.50f, 0.50f, 0.30f, 0.00f, 1, 0.30f, 0.00f, 1, 0},
    /* Midnight Choir: vowel voice on "O", huge room */
    {0.50f, 0.30f, 0.40f, 0.55f, 0.10f, 0.20f, 2.00f, 5.0f, 0.45f, 0.70f, 0.95f, 0.55f, 0.90f, 1, 0.15f, 0.75f, 4, 1},
    /* Whisper Pad: breathy "E" with lots of sibilance */
    {0.60f, 0.10f, 0.40f, 0.75f, 0.05f, 0.10f, 1.50f, 4.0f, 0.30f, 0.80f, 0.80f, 0.50f, 0.70f, 1, 0.70f, 0.25f, 2, 0},
    /* Mic Choir: sing into the USB mic over open chords */
    {0.60f, 0.20f, 0.40f, 0.65f, 0.15f, 0.15f, 0.30f, 2.5f, 0.40f, 0.75f, 0.80f, 0.45f, 0.90f, 0, 0.35f, 0.00f, 4, 1},
    /* Cassette Sunset: worn tape, lots of drift */
    {0.55f, 0.30f, 0.90f, 0.45f, 0.25f, 0.30f, 1.00f, 3.5f, 0.90f, 0.60f, 0.60f, 0.35f, 0.00f, 1, 0.30f, 0.00f, 1, 1},
    /* Deep Sub Drone: low, resonant, endless */
    {0.20f, 0.90f, 0.50f, 0.25f, 0.45f, 0.30f, 3.00f, 9.0f, 0.65f, 0.30f, 0.90f, 0.40f, 0.00f, 1, 0.30f, 0.00f, 4, 1},
    /* Shimmer Wash: bright and mostly reverb */
    {0.65f, 0.10f, 0.40f, 0.75f, 0.10f, 0.20f, 2.50f, 7.0f, 0.20f, 0.90f, 1.00f, 0.60f, 0.00f, 1, 0.30f, 0.00f, 3, 0},
    /* Soft Keys: plucky filter, echoing room */
    {0.35f, 0.20f, 0.30f, 0.30f, 0.35f, 0.90f, 0.005f, 1.2f, 0.40f, 0.60f, 0.75f, 0.50f, 0.00f, 1, 0.30f, 0.00f, 0, 0},
};

typedef struct { int used; char name[32]; float v[NFIELDS]; } UserPreset;
static UserPreset USERS[NUSER];
static int preset_modified;
static const char *USER_FILE = "user_presets.cfg";
static void show_toast(const char *m);

static const char *preset_name(int i) {
    if (i < NFACTORY) return FACTORY_NAMES[i];
    i -= NFACTORY;
    return USERS[i].used ? USERS[i].name : "(empty)";
}

static void load_preset(int i) {
    if (i < NFACTORY) for (int f = 0; f < NFIELDS; f++) *FIELDS[f].v = FACTORY[i][f];
    else if (USERS[i - NFACTORY].used) for (int f = 0; f < NFIELDS; f++) *FIELDS[f].v = USERS[i - NFACTORY].v[f];
    preset_modified = 0;
}

static void save_user_file(void) {
    FILE *f = fopen(USER_FILE, "w");
    if (!f) { show_toast("Could not write user_presets.cfg"); return; }
    fprintf(f, "# Ember user presets. You can rename a slot by editing its name= line.\n");
    for (int u = 0; u < NUSER; u++) {
        if (!USERS[u].used) continue;
        fprintf(f, "\n[%d]\nname=%s\n", u + 1, USERS[u].name);
        for (int k = 0; k < NFIELDS; k++) fprintf(f, "%s=%g\n", FIELDS[k].id, USERS[u].v[k]);
    }
    fclose(f);
}

static void load_user_file(void) {
    FILE *f = fopen(USER_FILE, "r");
    if (!f) return;
    char line[96]; int u = -1;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0] == '[') { u = atoi(line + 1) - 1; if (u < 0 || u >= NUSER) u = -1; else USERS[u].used = 1; continue; }
        char *eq = strchr(line, '=');
        if (u < 0 || !eq || line[0] == '#') continue;
        *eq = 0;
        if (!strcmp(line, "name")) { snprintf(USERS[u].name, sizeof USERS[u].name, "%s", eq + 1); continue; }
        for (int k = 0; k < NFIELDS; k++) if (!strcmp(FIELDS[k].id, line)) USERS[u].v[k] = (float)atof(eq + 1);
    }
    for (int i = 0; i < NUSER; i++) if (USERS[i].used && !USERS[i].name[0]) snprintf(USERS[i].name, sizeof USERS[i].name, "User %d", i + 1);
    fclose(f);
}

static Uint32 overwrite_until; static int overwrite_slot = -1;
static void save_to_slot(int slot) {
    char m[96];
    if (USERS[slot].used && !(overwrite_slot == slot && SDL_GetTicks() < overwrite_until)) {
        overwrite_slot = slot; overwrite_until = SDL_GetTicks() + 3000;
        snprintf(m, sizeof m, "Press RIGHT again to overwrite %s", USERS[slot].name);
        show_toast(m);
        return;
    }
    overwrite_slot = -1;
    if (!USERS[slot].used) snprintf(USERS[slot].name, sizeof USERS[slot].name, "User %d", slot + 1);
    USERS[slot].used = 1;
    for (int k = 0; k < NFIELDS; k++) USERS[slot].v[k] = *FIELDS[k].v;
    save_user_file();
    U.preset = (float)(NFACTORY + slot);
    preset_modified = 0;
    snprintf(m, sizeof m, "Saved to %s", USERS[slot].name);
    show_toast(m);
}

static int is_preset_field(const float *v) {
    for (int k = 0; k < NFIELDS; k++) if (FIELDS[k].v == v) return 1;
    return 0;
}

static Param PAGE_PLAY[] = {
    {"preset", "Preset", &U.preset, 0, NFACTORY + NUSER - 1, 1, F_PRESET},
    {"slot", "Save to", &U.slot, 0, NUSER - 1, 1, F_SLOT},
    {NULL, "Save preset", &U.savepreset, 0, 0, 0, F_SAVE},
    {"key", "Key", &U.key, 0, 11, 1, F_KEY},
    {"scale", "Scale", &U.scale, 0, 4, 1, F_SCALE},
    {"chord", "Chord type", &U.chord, 0, 4, 1, F_CHORD},
    {"octave", "Octave", &U.octave, -2, 2, 1, F_OCT},
    {"bass", "Bass note", &U.bass, 0, 1, 1, F_ONOFF},
    {"volume", "Volume", &P.volume, 0, 1, 0.02f, F_PCT},
};
static Param PAGE_TONE[] = {
    {"detune", "Detune", &P.detune, 0, 1, 0.02f, F_PCT},
    {"sub", "Sub osc", &P.sub, 0, 1, 0.02f, F_PCT},
    {"drift", "Analog drift", &P.drift, 0, 1, 0.02f, F_PCT},
    {"cutoff", "Cutoff", &P.cutoff, 0, 1, 0.01f, F_HZ},
    {"reso", "Resonance", &P.reso, 0, 1, 0.02f, F_PCT},
    {"envamt", "Filter env", &P.envamt, 0, 1, 0.02f, F_PCT},
    {"attack", "Attack", &P.attack, 0.005f, 8, 0, F_SEC},
    {"release", "Release", &P.release, 0.05f, 12, 0, F_SEC},
    {"tape", "Tape warmth", &P.tape, 0, 1, 0.02f, F_PCT},
};
static Param PAGE_VOICE[] = {
    {"vocmix", "Vocoder", &P.vocmix, 0, 1, 0.02f, F_PCT},
    {"vocsrc", "Source", &P.vocsrc, 0, 1, 1, F_SRC},
    {"micgain", "Mic gain", &P.micgain, 0, 4, 0.05f, F_GAIN},
    {"vowel", "Vowel", &P.vowel, 0, 1, 0.02f, F_PCT},
    {"sib", "Sibilance", &P.sib, 0, 1, 0.02f, F_PCT},
};
static Param PAGE_SPACE[] = {
    {"chorus", "Chorus", &P.chorus, 0, 1, 0.02f, F_PCT},
    {"rvsize", "Reverb size", &P.rvsize, 0, 1, 0.02f, F_PCT},
    {"rvmix", "Reverb mix", &P.rvmix, 0, 1, 0.02f, F_PCT},
};
static Param PAGE_LOOP[] = {
    {"looplevel", "Loop level", &P.looplevel, 0, 1, 0.02f, F_PCT},
    {"loopfb", "Overdub keep", &P.loopfb, 0.5f, 1, 0.01f, F_PCT},
    {NULL, "Record to SD", &U.rec, 0, 1, 1, F_REC},
    {NULL, "Save loop to SD", &U.saveloop, 0, 0, 0, F_ACTION},
};
typedef struct { const char *name; Param *p; int n; } Page;
static Page PAGES[] = {
    {"PLAY", PAGE_PLAY, sizeof PAGE_PLAY / sizeof(Param)},
    {"TONE", PAGE_TONE, sizeof PAGE_TONE / sizeof(Param)},
    {"VOICE", PAGE_VOICE, sizeof PAGE_VOICE / sizeof(Param)},
    {"SPACE", PAGE_SPACE, sizeof PAGE_SPACE / sizeof(Param)},
    {"LOOP", PAGE_LOOP, sizeof PAGE_LOOP / sizeof(Param)},
    {"INFO", NULL, 0},
};
#define NPAGES 6
static int page, sel[NPAGES];

static int recording_now(void) { return shot_mode ? 1 : rec_active(); }
static float rec_secs(void) { return shot_mode ? 83.f : rec_seconds(); }
static void fmt_time(float sec, char *out, size_t on) { int t = (int)sec; snprintf(out, on, "%d:%02d", t / 60, t % 60); }

static void fmt_value(const Param *p, char *out, size_t on) {
    float v = *p->v;
    switch (p->fmt) {
    case F_PCT: snprintf(out, on, "%d%%", (int)lroundf(v * 100)); break;
    case F_GAIN: snprintf(out, on, "x%.2f", v); break;
    case F_SEC: if (v < 1.f) snprintf(out, on, "%d ms", (int)lroundf(v * 1000)); else snprintf(out, on, "%.1f s", v); break;
    case F_HZ: { float hz = 40.f * exp2f(v * 8.2f);
        if (hz < 1000) snprintf(out, on, "%d Hz", (int)hz); else snprintf(out, on, "%.1f kHz", hz / 1000); break; }
    case F_KEY: snprintf(out, on, "%s", NOTE_NAMES[(int)v]); break;
    case F_SCALE: snprintf(out, on, "%s", SCALE_NAMES[(int)v]); break;
    case F_CHORD: snprintf(out, on, "%s", CHORD_TYPES[(int)v]); break;
    case F_SRC: snprintf(out, on, "%s", v < 0.5f ? "USB mic" : "Vowel voice"); break;
    case F_ONOFF: snprintf(out, on, "%s", v > 0.5f ? "On" : "Off"); break;
    case F_OCT: snprintf(out, on, "%+d", (int)v); break;
    case F_PRESET: snprintf(out, on, "%s%s", preset_name((int)v), preset_modified ? " *" : ""); break;
    case F_SLOT: { int u = (int)v;
        if (USERS[u].used && strcmp(USERS[u].name, "User") && strncmp(USERS[u].name, "User ", 5))
            snprintf(out, on, "Slot %d: %s", u + 1, USERS[u].name);
        else snprintf(out, on, "User %d%s", u + 1, USERS[u].used ? " (overwrite)" : " (empty)");
        break; }
    case F_SAVE: snprintf(out, on, "press RIGHT"); break;
    case F_REC:
        if (recording_now()) { char t[16]; fmt_time(rec_secs(), t, sizeof t); snprintf(out, on, "On  %s", t); }
        else snprintf(out, on, "Off");
        break;
    case F_ACTION: snprintf(out, on, "%s", M.loop_len > 0 && M.loop_state >= LOOP_PLAY ? "press RIGHT" : "no loop yet"); break;
    }
}

static int is_enum(Fmt f) { return f == F_KEY || f == F_SCALE || f == F_CHORD || f == F_SRC || f == F_ONOFF || f == F_OCT || f == F_PRESET || f == F_REC || f == F_ACTION || f == F_SLOT || f == F_SAVE; }

static Uint32 dirty_at;
static int repeating;
static void adjust(Param *p, int dir) {
    if (p->fmt == F_ACTION || p->fmt == F_REC || p->fmt == F_SAVE) {   /* one-shot actions: no key repeat */
        if (repeating) return;
        if (p->fmt == F_SAVE) { if (dir > 0) save_to_slot((int)U.slot); return; }
        if (p->fmt == F_REC) { if (dir > 0) rec_start(); else rec_stop(); }
        else if (dir > 0) { float *l, *r; int n = loop_snapshot(&l, &r); rec_save_loop(l, r, n); }
        return;
    }
    float v = *p->v;
    if (p->fmt == F_PRESET) {                     /* step through factory presets and filled user slots */
        int i = (int)v, total = NFACTORY + NUSER;
        do { i = (i + dir + total) % total; } while (i >= NFACTORY && !USERS[i - NFACTORY].used);
        U.preset = (float)i;
        load_preset(i);
        dirty_at = SDL_GetTicks();
        return;
    }
    if (p->fmt == F_SEC) v = dir > 0 ? v * 1.12f + 0.004f : (v - 0.004f) / 1.12f;
    else if (is_enum(p->fmt)) {
        v += dir;
        if (p->fmt == F_KEY || p->fmt == F_PRESET) {           /* wrap around */
            if (v > p->max) v = p->min;
            if (v < p->min) v = p->max;
        }
    } else v += dir * p->step;
    if (v < p->min) v = p->min;
    if (v > p->max) v = p->max;
    *p->v = v;
    if (is_preset_field(p->v)) preset_modified = 1;
    dirty_at = SDL_GetTicks();
}

/* ------------------------------------------------------ settings file */
static const char *CFG = "ember.cfg";

static void save_settings(void) {
    FILE *f = fopen(CFG, "w");
    if (!f) return;
    fprintf(f, "# Ember %s settings (saved automatically)\n", VERSION);
    for (int pg = 0; pg < NPAGES; pg++)
        for (int i = 0; i < PAGES[pg].n; i++)
            if (PAGES[pg].p[i].id) fprintf(f, "%s=%g\n", PAGES[pg].p[i].id, *PAGES[pg].p[i].v);
    fclose(f);
}

static void load_settings(void) {
    FILE *f = fopen(CFG, "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (!eq || line[0] == '#') continue;
        *eq = 0;
        for (int pg = 0; pg < NPAGES; pg++)
            for (int i = 0; i < PAGES[pg].n; i++)
                if (PAGES[pg].p[i].id && !strcmp(PAGES[pg].p[i].id, line)) {
                    float v = (float)atof(eq + 1);
                    Param *p = &PAGES[pg].p[i];
                    *p->v = v < p->min ? p->min : v > p->max ? p->max : v;
                }
    }
    fclose(f);
}

/* ------------------------------------------------------------ controls */
enum { B_A, B_B, B_X, B_Y, B_L1, B_R1, B_L2, B_R2, B_UP, B_DOWN, B_LEFT, B_RIGHT, B_SELECT, B_START, B_COUNT };
static const char *BTN_IDS[B_COUNT] = {"a", "b", "x", "y", "l1", "r1", "l2", "r2", "up", "down", "left", "right", "select", "start"};
/* raw joystick numbers; ArkOS RG351MP-style layout. Edit controls.cfg if yours differ. */
static int rawmap[B_COUNT] = {1, 0, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13};
static int axis_map[4] = {0, 1, 2, 3};
static int use_gc = 1;
static int held[B_COUNT];
static SDL_GameController *gc;
static SDL_Joystick *joy;
static char ctrl_desc[96] = "none";
static int last_raw_button = -1, last_axis = -1;

static void load_controls(void) {
    FILE *f = fopen("controls.cfg", "r");
    if (!f) {                                     /* write a template the first time */
        f = fopen("controls.cfg", "w");
        if (!f) return;
        fprintf(f, "# Ember controls. use_gamecontroller=1 uses SDL's controller database when it\n"
                   "# knows your device; set it to 0 to use the raw numbers below instead.\n"
                   "# The INFO page shows the raw number of the last button you pressed.\n"
                   "use_gamecontroller=1\n");
        for (int i = 0; i < B_COUNT; i++) fprintf(f, "%s=%d\n", BTN_IDS[i], rawmap[i]);
        fprintf(f, "axis_lx=0\naxis_ly=1\naxis_rx=2\naxis_ry=3\n");
        fclose(f);
        return;
    }
    char line[96];
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (!eq || line[0] == '#') continue;
        *eq = 0; int v = atoi(eq + 1);
        if (!strcmp(line, "use_gamecontroller")) use_gc = v;
        for (int i = 0; i < B_COUNT; i++) if (!strcmp(line, BTN_IDS[i])) rawmap[i] = v;
        if (!strcmp(line, "axis_lx")) axis_map[0] = v;
        if (!strcmp(line, "axis_ly")) axis_map[1] = v;
        if (!strcmp(line, "axis_rx")) axis_map[2] = v;
        if (!strcmp(line, "axis_ry")) axis_map[3] = v;
    }
    fclose(f);
}

static void open_controller(int index) {
    if (gc || joy) return;
    if (use_gc && SDL_IsGameController(index)) {
        gc = SDL_GameControllerOpen(index);
        if (gc) {
            joy = SDL_GameControllerGetJoystick(gc);
            snprintf(ctrl_desc, sizeof ctrl_desc, "%s (mapped)", SDL_GameControllerName(gc));
            return;
        }
    }
    joy = SDL_JoystickOpen(index);
    if (joy) snprintf(ctrl_desc, sizeof ctrl_desc, "%s (raw: controls.cfg)", SDL_JoystickName(joy));
}

/* chord playing */
static int chord_notes[8], chord_n, chord_btn = -1;

static void notes_off(void) {
    for (int i = 0; i < chord_n; i++) { Event e = {EV_NOTE_OFF, (uint8_t)chord_notes[i], 0, 0}; evq_push(&q_ui, e); }
    chord_n = 0;
}

static void play_degree(int d, int btn) {
    notes_off();
    chord_n = build_chord(d, chord_notes);
    for (int i = 0; i < chord_n; i++) { Event e = {EV_NOTE_ON, (uint8_t)chord_notes[i], 100, 0}; evq_push(&q_ui, e); }
    chord_btn = btn;
}

static Uint32 repeat_at; static int repeat_btn = -1;
static char toast[128]; static Uint32 toast_until;
static void show_toast(const char *m) { snprintf(toast, sizeof toast, "%s", m); toast_until = SDL_GetTicks() + 3000; }
static Uint32 select_down; static int select_used;

static void loop_button(void) { Event e = {EV_LOOP, 0, 0, 0}; evq_push(&q_ui, e); }
static void loop_undo(void) {
    if (M.loop_state == LOOP_REC) show_toast("Take cancelled");
    else if (M.loop_can_undo) show_toast(M.loop_undone ? "Redo overdub" : "Undo overdub");
    else { show_toast("Nothing to undo"); return; }
    Event e = {EV_LOOP_UNDO, 0, 0, 0}; evq_push(&q_ui, e);
}
static int running = 1;

static void press(int b) {
    if (held[b]) return;
    held[b] = 1;
    Page *pg = &PAGES[page];
    switch (b) {
    case B_A: case B_B: case B_X: case B_Y: {
        static const int base[4] = {0, 3, 4, 5}, shifted[4] = {1, 2, 6, 7};
        play_degree(held[B_L1] ? shifted[b] : base[b], b);
        break;
    }
    case B_R1:
        U.hold = U.hold > 0.5f ? 0.f : 1.f;
        if (U.hold < 0.5f && (chord_btn < 0 || !held[chord_btn])) notes_off();
        break;
    case B_L2: page = (page + NPAGES - 1) % NPAGES; break;
    case B_R2: page = (page + 1) % NPAGES; break;
    case B_UP:   if (pg->n) sel[page] = (sel[page] + pg->n - 1) % pg->n; repeat_btn = b; repeat_at = SDL_GetTicks() + 350; break;
    case B_DOWN: if (pg->n) sel[page] = (sel[page] + 1) % pg->n; repeat_btn = b; repeat_at = SDL_GetTicks() + 350; break;
    case B_LEFT: case B_RIGHT:
        if (pg->n) adjust(&pg->p[sel[page]], b == B_RIGHT ? 1 : -1);
        repeat_btn = b; repeat_at = SDL_GetTicks() + 350;
        break;
    case B_START: if (held[B_SELECT]) { running = 0; select_used = 1; } else loop_button(); break;
    case B_SELECT:
        if (held[B_START]) running = 0;
        select_down = SDL_GetTicks(); select_used = held[B_START];
        break;
    }
}

static void release(int b) {
    if (!held[b]) return;
    held[b] = 0;
    if (b == repeat_btn) repeat_btn = -1;
    if (b == B_SELECT && !select_used) loop_undo();
    if (b == chord_btn) { chord_btn = -1; if (U.hold < 0.5f) notes_off(); }
}

static void select_hold(void) {                  /* hold SELECT: clear the loop */
    if (held[B_SELECT] && !select_used && SDL_GetTicks() - select_down > 700) {
        select_used = 1;
        if (M.loop_state != LOOP_EMPTY) { Event e = {EV_LOOP_CLEAR, 0, 0, 0}; evq_push(&q_ui, e); show_toast("Loop cleared"); }
    }
}

static void key_repeat(void) {
    if (repeat_btn < 0 || SDL_TICKS_PASSED(SDL_GetTicks(), repeat_at) == 0) return;
    int b = repeat_btn;
    held[b] = 0; repeating = 1; press(b); repeating = 0;
    repeat_at = SDL_GetTicks() + 55;
}

static float dz(float v) { float d = 0.15f; if (fabsf(v) < d) return 0.f; return (v - (v > 0 ? d : -d)) / (1.f - d); }
static float axes[4];
static int stick_bend;

static void apply_sticks(void) {
    P.vowel_mod = dz(axes[0]) * 0.5f;
    P.mod_cutoff = -dz(axes[1]);
    float bx = dz(axes[2]);
    if (bx != 0.f) { P.bend = bx * 2.f; stick_bend = 1; }
    else if (stick_bend) { P.bend = 0.f; stick_bend = 0; }
    float sy = -dz(axes[3]);
    P.swell = sy > 0 ? sy : 0.f;
}

static void handle_event(SDL_Event *e) {
    static const struct { SDL_Keycode k; int b; } KEYS[] = {
        {SDLK_z, B_A}, {SDLK_x, B_B}, {SDLK_a, B_X}, {SDLK_s, B_Y}, {SDLK_q, B_L1}, {SDLK_w, B_R1},
        {SDLK_1, B_L2}, {SDLK_2, B_R2}, {SDLK_UP, B_UP}, {SDLK_DOWN, B_DOWN}, {SDLK_LEFT, B_LEFT},
        {SDLK_RIGHT, B_RIGHT}, {SDLK_RSHIFT, B_SELECT}, {SDLK_RETURN, B_START},
    };
    static const int GC_MAP[B_COUNT] = {
        SDL_CONTROLLER_BUTTON_A, SDL_CONTROLLER_BUTTON_B, SDL_CONTROLLER_BUTTON_X, SDL_CONTROLLER_BUTTON_Y,
        SDL_CONTROLLER_BUTTON_LEFTSHOULDER, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, -1, -1,
        SDL_CONTROLLER_BUTTON_DPAD_UP, SDL_CONTROLLER_BUTTON_DPAD_DOWN, SDL_CONTROLLER_BUTTON_DPAD_LEFT,
        SDL_CONTROLLER_BUTTON_DPAD_RIGHT, SDL_CONTROLLER_BUTTON_BACK, SDL_CONTROLLER_BUTTON_START,
    };
    switch (e->type) {
    case SDL_QUIT: running = 0; break;
    case SDL_KEYDOWN: case SDL_KEYUP:
        if (e->key.repeat) break;
        if (e->key.keysym.sym == SDLK_ESCAPE) { running = 0; break; }
        for (unsigned i = 0; i < sizeof KEYS / sizeof KEYS[0]; i++)
            if (KEYS[i].k == e->key.keysym.sym) { if (e->type == SDL_KEYDOWN) press(KEYS[i].b); else release(KEYS[i].b); }
        break;
    case SDL_JOYDEVICEADDED: open_controller(e->jdevice.which); break;
    case SDL_JOYDEVICEREMOVED:
        if (joy && e->jdevice.which == SDL_JoystickInstanceID(joy)) {
            if (gc) SDL_GameControllerClose(gc); else SDL_JoystickClose(joy);
            gc = NULL; joy = NULL; snprintf(ctrl_desc, sizeof ctrl_desc, "none");
        }
        break;
    case SDL_JOYBUTTONDOWN: case SDL_JOYBUTTONUP:
        if (e->type == SDL_JOYBUTTONDOWN) last_raw_button = e->jbutton.button;
        /* With a controller mapping, everything else arrives as controller
         * events. SELECT and START always come from the raw numbers too,
         * because some handheld mappings leave them out. press/release ignore
         * repeats, so getting a button from both paths is harmless. */
        for (int b = 0; b < B_COUNT; b++) {
            if (gc && b != B_SELECT && b != B_START) continue;
            if (rawmap[b] == e->jbutton.button) { if (e->type == SDL_JOYBUTTONDOWN) press(b); else release(b); }
        }
        break;
    case SDL_JOYHATMOTION:
        if (gc) break;
        { int v = e->jhat.value;
          if (v & SDL_HAT_UP) press(B_UP); else release(B_UP);
          if (v & SDL_HAT_DOWN) press(B_DOWN); else release(B_DOWN);
          if (v & SDL_HAT_LEFT) press(B_LEFT); else release(B_LEFT);
          if (v & SDL_HAT_RIGHT) press(B_RIGHT); else release(B_RIGHT); }
        break;
    case SDL_JOYAXISMOTION:
        if (abs(e->jaxis.value) > 12000) last_axis = e->jaxis.axis;
        if (gc) break;
        for (int a = 0; a < 4; a++) if (axis_map[a] == e->jaxis.axis) axes[a] = e->jaxis.value / 32767.f;
        apply_sticks();
        break;
    case SDL_CONTROLLERBUTTONDOWN: case SDL_CONTROLLERBUTTONUP:
        for (int b = 0; b < B_COUNT; b++)
            if (GC_MAP[b] == e->cbutton.button) { if (e->type == SDL_CONTROLLERBUTTONDOWN) press(b); else release(b); }
        break;
    case SDL_CONTROLLERAXISMOTION: {
        int a = e->caxis.axis; float v = e->caxis.value / 32767.f;
        if (a == SDL_CONTROLLER_AXIS_TRIGGERLEFT) { if (v > 0.5f) press(B_L2); else if (v < 0.3f) release(B_L2); }
        else if (a == SDL_CONTROLLER_AXIS_TRIGGERRIGHT) { if (v > 0.5f) press(B_R2); else if (v < 0.3f) release(B_R2); }
        else if (a <= SDL_CONTROLLER_AXIS_RIGHTY) { axes[a] = v; apply_sticks(); }
        break;
    }
    }
}

/* ---------------------------------------------------------------- draw */
static void status_dot(int x, int y, const char *label, Col c) {
    fill(x, y + 7, 8, 8, c);
    text(&font_small, x + 14, y, label, C_DIM, 0, 0);
}

static void draw(void) {
    char buf[160];
    SDL_SetRenderDrawColor(ren, C_BG.r, C_BG.g, C_BG.b, 255);
    SDL_RenderClear(ren);

    /* header */
    text(&font_med, 28, 20, "EMBER", C_AMBER, 0, 4);
    text(&font_small, 28 + text_w(&font_med, "EMBER", 4) + 12, 26, "v" VERSION, C_FAINT, 0, 0);
    if (recording_now()) {                                      /* SD recorder running */
        char t[16]; fmt_time(rec_secs(), t, sizeof t);
        snprintf(buf, sizeof buf, "REC %s", t);
        int rx = 28 + text_w(&font_med, "EMBER", 4) + 12 + text_w(&font_small, "v" VERSION, 0) + 16;
        fill(rx, 31, 10, 10, (SDL_GetTicks() / 500) % 2 || shot_mode ? C_ERR : C_BG);
        text(&font_small, rx + 16, 24, buf, C_ERR, 0, 0);
    }
    int sx = 692 - 3 * 92 - 80;
    status_dot(sx, 24, "OUT", IO.out_ok ? C_OK : C_ERR);
    status_dot(sx + 92, 24, "MIC", IO.mic_name[0] ? C_OK : C_FAINT);
    status_dot(sx + 184, 24, "MIDI", IO.midi_name[0] ? C_OK : C_FAINT);
    snprintf(buf, sizeof buf, "%d%%", (int)lroundf(M.cpu * 100));
    text(&font_small, 692, 24, buf, M.cpu > 0.7f ? C_ERR : C_DIM, 2, 0);
    fill(28, 66, 664, 1, C_LINE);

    /* chord */
    chord_name(buf, sizeof buf);
    text(&font_big, 360, 80, buf, buf[0] == '-' ? C_FAINT : C_CREAM, 1, 2);
    snprintf(buf, sizeof buf, "%s %s  |  %s  |  %s%s", NOTE_NAMES[(int)U.key], SCALE_NAMES[(int)U.scale],
             CHORD_TYPES[(int)U.chord], preset_name((int)U.preset), preset_modified ? " *" : "");
    text(&font_small, 28, 160, buf, C_DIM, 0, 0);
    int pill_x = 692;
    if (U.hold > 0.5f) {
        int w = text_w(&font_small, "HOLD", 2) + 20;
        fill(692 - w, 157, w, 28, C_AMBER);
        text(&font_small, 692 - w / 2, 160, "HOLD", C_BG, 1, 2);
        pill_x = 692 - w - 10;
    }
    int ls = M.loop_state;
    if (ls != LOOP_EMPTY) {                                     /* looper state pill */
        char t[16], lbl[32];
        fmt_time((ls == LOOP_REC ? M.loop_pos : M.loop_len) / 48000.f, t, sizeof t);
        snprintf(lbl, sizeof lbl, "%s %s", ls == LOOP_REC ? "REC" : ls == LOOP_DUB ? "DUB" : "LOOP", t);
        int w = text_w(&font_small, lbl, 2) + 20;
        Col bg = ls == LOOP_REC ? C_ERR : ls == LOOP_DUB ? C_ORANGE : C_LINE;
        fill(pill_x - w, 157, w, 28, bg);
        text(&font_small, pill_x - w / 2, 160, lbl, ls == LOOP_PLAY ? C_AMBER : C_BG, 1, 2);
    }

    /* vocoder bands */
    const int bx = 35, by = 198, bh = 124, bw = 35, gap = 6;
    float vm = P.vocmix;
    for (int b = 0; b < NBANDS; b++) {
        int x = bx + b * (bw + gap);
        fill(x, by, bw, bh, C_TRACK);
        float lv = M.band[b]; if (lv > 1.f) lv = 1.f;
        int h = (int)(lv * bh);
        if (h > 0) {
            Col c = vm > 0.01f ? C_AMBER : C_FAINT;
            fill(x, by + bh - h, bw, h, c);
            if (lv > 0.85f) fill(x, by + bh - h, bw, 4, C_ORANGE);
        }
    }
    if (SDL_GetTicks() < toast_until) {
        text(&font_small, 360, 330, toast, C_AMBER, 1, 0);
    } else if (!IO.out_ok) {
        snprintf(buf, sizeof buf, "Audio: %s", IO.out_name);
        text(&font_small, 35, 330, buf, C_ERR, 0, 0);
    } else {
        int mic = P.vocsrc < 0.5f;
        if (mic) snprintf(buf, sizeof buf, "MIC  %s", IO.mic_name[0] ? IO.mic_name : "not connected");
        else snprintf(buf, sizeof buf, "VOWEL VOICE  %c", "AEIOU"[(int)lroundf(fminf(fmaxf(P.vowel + P.vowel_mod, 0.f), 1.f) * 4)]);
        text(&font_small, 35, 330, buf, mic && !IO.mic_name[0] ? C_ERR : C_DIM, 0, 0);
        snprintf(buf, sizeof buf, "VOCODER %d%%", (int)lroundf(vm * 100));
        text(&font_small, 685, 330, buf, vm > 0.01f ? C_AMBER : C_FAINT, 2, 0);
    }
    float op = M.out_peak; if (op > 1.f) op = 1.f;
    fill(35, 355, 650, 2, C_TRACK);
    fill(35, 355, (int)(650 * op), 2, op > 0.95f ? C_ERR : C_ORANGE);
    fill(35, 360, 650, 5, C_TRACK);                            /* loop position */
    if (ls == LOOP_REC) fill(35, 360, (int)(650.f * M.loop_pos / (48000.f * LOOP_MAX_SECONDS)), 5, C_ERR);
    else if (ls != LOOP_EMPTY && M.loop_len > 0)
        fill(35, 360, (int)(650.f * M.loop_pos / M.loop_len), 5, ls == LOOP_DUB ? C_ORANGE : C_AMBER);

    /* tabs */
    for (int i = 0; i < NPAGES; i++) {
        int cx = 35 + 650 / NPAGES / 2 + i * 650 / NPAGES;
        text(&font_small, cx, 374, PAGES[i].name, i == page ? C_AMBER : C_FAINT, 1, 3);
        if (i == page) fill(cx - 36, 400, 72, 3, C_AMBER);
    }
    fill(28, 403, 664, 1, C_LINE);

    /* page body */
    Page *pg = &PAGES[page];
    int y0 = 412, rh = 29;
    if (pg->n) {
        for (int i = 0; i < pg->n; i++) {
            Param *p = &pg->p[i];
            int y = y0 + i * rh, s = i == sel[page];
            if (s) fill(24, y, 672, rh - 2, C_PANEL);
            text(&font_small, 38, y + 3, p->name, s ? C_CREAM : C_DIM, 0, 0);
            fmt_value(p, buf, sizeof buf);
            if (is_enum(p->fmt)) {
                if (s) { char t[180]; snprintf(t, sizeof t, "< %s >", buf); text(&font_small, 682, y + 3, t, C_AMBER, 2, 0); }
                else text(&font_small, 682, y + 3, buf, C_CREAM, 2, 0);
            } else {
                float t = (*p->v - p->min) / (p->max - p->min);
                if (p->fmt == F_SEC) t = logf(*p->v / p->min) / logf(p->max / p->min);
                fill(250, y + 12, 300, 4, C_TRACK);
                fill(250, y + 12, (int)(300 * t), 4, s ? C_AMBER : C_ORANGE);
                text(&font_small, 682, y + 3, buf, s ? C_CREAM : C_DIM, 2, 0);
            }
        }
    } else {                                                    /* INFO page */
        const char *lines[10]; char l[10][128]; int n = 0;
        snprintf(l[n], 128, "Output   %s", IO.out_name); n++;
        snprintf(l[n], 128, "Latency  %d frames x3 @ %d Hz  (%.0f ms)  xruns %d", IO.period, IO.rate ? IO.rate : 48000,
                 IO.period * 3 * 1000.0 / 48000.0, IO.xruns); n++;
        snprintf(l[n], 128, "Mic      %s", IO.mic_name[0] ? IO.mic_name : "none (plug a USB mic)"); n++;
        snprintf(l[n], 128, "MIDI     %s", IO.midi_name[0] ? IO.midi_name : "none (plug a USB keyboard)"); n++;
        if (IO.midi_last_cc >= 0 && IO.midi_name[0]) snprintf(l[n], 128, "Last CC  #%d = %d", IO.midi_last_cc, IO.midi_last_val);
        else snprintf(l[n], 128, "Last CC  -");
        n++;
        snprintf(l[n], 128, "Pad      %s", ctrl_desc); n++;
        snprintf(l[n], 128, "Raw btn  %d   axis %d", last_raw_button, last_axis); n++;
        snprintf(l[n], 128, "Voices   %d / %d   CPU %d%%", M.voices_active, NVOICES, (int)lroundf(M.cpu * 100)); n++;
        for (int i = 0; i < n; i++) { lines[i] = l[i]; text(&font_small, 38, y0 + 3 + i * rh, lines[i], i == 6 ? C_AMBER : C_DIM, 0, 0); }
    }

    /* footer */
    fill(28, 680, 664, 1, C_LINE);
    text(&font_small, 360, 690, "ABXY chords  R1 hold  START loop  SELECT undo  L2/R2 page", C_FAINT, 1, 0);
    SDL_RenderPresent(ren);
}

/* ---------------------------------------------------------------- main */
int main(int argc, char **argv) {
    const char *shot = NULL; int shot_page = 0;
    if (argc >= 3 && !strcmp(argv[1], "--screenshot")) {
        shot = argv[2];
        if (argc >= 4) shot_page = atoi(argv[3]);
        setenv("SDL_VIDEODRIVER", "dummy", 1);
    }

    engine_init();
    load_user_file();
    load_preset(0);
    load_settings();
    if (U.preset >= NFACTORY && !USERS[(int)U.preset - NFACTORY].used) U.preset = 0;
    load_controls();
    IO.midi_last_cc = -1;

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER) < 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GameControllerAddMappingsFromFile("gamecontrollerdb.txt");   /* optional, next to the binary */
    SDL_DisplayMode dm;
    int fullscreen = !shot && SDL_GetCurrentDisplayMode(0, &dm) == 0 && !getenv("EMBER_WINDOWED");
    SDL_Window *win = SDL_CreateWindow("Ember", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, W, H,
                                       fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
    if (!win) { fprintf(stderr, "window: %s\n", SDL_GetError()); return 1; }
    ren = SDL_CreateRenderer(win, -1, shot ? SDL_RENDERER_SOFTWARE : SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    if (!ren) { fprintf(stderr, "renderer: %s\n", SDL_GetError()); return 1; }
    SDL_RenderSetLogicalSize(ren, W, H);
    SDL_ShowCursor(SDL_DISABLE);
    tex_small = font_texture(&font_small);
    tex_med = font_texture(&font_med);
    tex_big = font_texture(&font_big);

    if (shot) {                       /* fake a lively state and save one frame */
        page = shot_page; shot_mode = 1;
        M.loop_state = LOOP_DUB; M.loop_len = 48000 * 16; M.loop_pos = 48000 * 6;
        U.hold = 1; P.vocmix = 0.85f; P.vocsrc = 0;
        snprintf((char *)IO.out_name, sizeof IO.out_name, "plughw:0,0");
        snprintf((char *)IO.mic_name, sizeof IO.mic_name, "USB Composite Device");
        snprintf((char *)IO.midi_name, sizeof IO.midi_name, "Impact LX61+");
        IO.out_ok = 1; IO.period = 256; IO.rate = 48000; M.cpu = 0.23f; M.out_peak = 0.62f;
        M.voices_active = 5;
        int notes[] = {41, 53, 57, 60, 64};
        for (int i = 0; i < 5; i++) M.notes[notes[i] >> 6] |= 1ull << (notes[i] & 63);
        for (int b = 0; b < NBANDS; b++) M.band[b] = 0.25f + 0.6f * expf(-(b - 4) * (b - 4) / 6.f) + 0.35f * expf(-(b - 11) * (b - 11) / 3.f);
        draw();
        SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, W, H, 32, SDL_PIXELFORMAT_ARGB8888);
        SDL_RenderReadPixels(ren, NULL, SDL_PIXELFORMAT_ARGB8888, s->pixels, s->pitch);
        SDL_SaveBMP(s, shot);
        SDL_Quit();
        return 0;
    }

    for (int i = 0; i < SDL_NumJoysticks(); i++) open_controller(i);
    if (io_start() < 0) { fprintf(stderr, "could not start audio thread\n"); return 1; }
    rec_init();
    unsigned msg_seen; rec_message(&msg_seen);

    Uint32 last_draw = 0;
    while (running) {
        SDL_Event e;
        if (SDL_WaitEventTimeout(&e, 8)) {
            handle_event(&e);
            while (SDL_PollEvent(&e)) handle_event(&e);
        }
        key_repeat();
        select_hold();
        { unsigned seq; const char *m = rec_message(&seq);
          if (seq != msg_seen) { msg_seen = seq; show_toast(m); } }
        if (dirty_at && SDL_GetTicks() - dirty_at > 2000) { save_settings(); dirty_at = 0; }
        if (SDL_GetTicks() - last_draw >= 33) { draw(); last_draw = SDL_GetTicks(); }
    }

    Event off = {EV_ALL_OFF, 0, 0, 0};
    evq_push(&q_ui, off);
    SDL_Delay(50);
    save_settings();
    io_stop();
    rec_shutdown();
    SDL_Quit();
    return 0;
}
