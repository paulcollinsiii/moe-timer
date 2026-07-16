/* Alert-tone tables + streaming renderer (pure C, host-tested; see
   tones.h). No ESP includes — the DAC pump in audio.c consumes this. */
#include "tones.h"

#include <math.h>
#include <string.h>

/* Peak deviation from the 8-bit DAC midpoint. Kept below full scale so
   the little speaker stays gentle and the envelope math cannot clip. */
#define TONE_AMPLITUDE 100

/* ---- note tables ------------------------------------------------------ */

static const tone_note_t NOTES_CLASSIC[] = {
    {1000, 200}, {0, 100}, {1000, 200}, {0, 100}, {1000, 200},
};

/* Two quick strikes on E6, the second ringing out. */
static const tone_note_t NOTES_DING[] = {
    {1319, 130},
    {0, 90},
    {1319, 260},
};

/* Doorbell: E5 down to C5. */
static const tone_note_t NOTES_CHIME[] = {
    {659, 250},
    {523, 450},
};

/* Ascending C5-E5-G5 roll. */
static const tone_note_t NOTES_MARIMBA[] = {
    {523, 150},
    {659, 150},
    {784, 350},
};

/* Tarrega, Gran Vals opening phrase (public domain; the "Nokia tune"):
   E5 D#5 F#4 G#4 C#5 B4 D4 E4 B4 A4 C#4 E4 A4. */
static const tone_note_t NOTES_GRANVALS[] = {
    {659, 150}, {622, 150}, {370, 300}, {415, 300}, {554, 150}, {494, 150}, {294, 300},
    {330, 300}, {494, 150}, {440, 150}, {277, 300}, {330, 300}, {440, 600},
};

#define N(tbl) (sizeof(tbl) / sizeof((tbl)[0]))

const char *const tones_names[TONE_COUNT] = {
    [TONE_CLASSIC] = "Classic beep",     [TONE_DING] = "Ding-ding",     [TONE_CHIME] = "Gentle chime",
    [TONE_MARIMBA] = "Marimba arpeggio", [TONE_GRANVALS] = "Gran Vals", [TONE_CUSTOM] = "Custom WAV",
};

static const tone_def_t TONES[TONE_COUNT] = {
    [TONE_CLASSIC] = {NOTES_CLASSIC, N(NOTES_CLASSIC), 4, 4},
    [TONE_DING] = {NOTES_DING, N(NOTES_DING), 4, 200},
    [TONE_CHIME] = {NOTES_CHIME, N(NOTES_CHIME), 8, 350},
    [TONE_MARIMBA] = {NOTES_MARIMBA, N(NOTES_MARIMBA), 4, 120},
    [TONE_GRANVALS] = {NOTES_GRANVALS, N(NOTES_GRANVALS), 6, 80},
    [TONE_CUSTOM] = {NULL, 0, 0, 0},
};

const tone_def_t *tones_get(int id) {
    if (id < 0 || id >= TONE_COUNT)
        return NULL;
    return &TONES[id];
}

const char *tones_option_name(int idx) {
    if (idx < 0 || idx >= TONE_COUNT)
        return NULL;
    return tones_names[idx];
}

int tones_option_count(void) {
    return TONE_COUNT;
}

int tones_clamp_id(int idx, int fallback) {
    return (idx >= 0 && idx < TONE_COUNT) ? idx : fallback;
}

/* ---- renderer ---------------------------------------------------------- */

/* One sine period = 256 LUT entries = (1 << 24) phase units. Built on
   first use — a 256-entry table is not worth carrying in the binary. */
static int8_t s_sine[256];
static int s_sine_ready;

static void sine_lut_init(void) {
    if (s_sine_ready)
        return;
    for (int i = 0; i < 256; i++) {
        s_sine[i] = (int8_t)lrintf(127.0f * sinf(6.283185307f * (float)i / 256.0f));
    }
    s_sine_ready = 1;
}

static void load_note(tone_player_t *p) {
    const tone_note_t *note = &p->def->notes[p->note_idx];
    p->sample_pos = 0;
    p->note_samples = (uint32_t)note->dur_ms * p->sample_rate / 1000u;
    p->phase = 0;
    p->phase_inc = (uint32_t)(((uint64_t)note->freq_hz << 24) / p->sample_rate);
}

void tones_player_init(tone_player_t *p, int id, uint32_t sample_rate) {
    sine_lut_init();
    memset(p, 0, sizeof(*p));
    const tone_def_t *def = tones_get(id);
    p->sample_rate = sample_rate;
    p->def = (def && def->notes) ? def : NULL; /* TONE_CUSTOM/invalid: empty */
    if (p->def)
        load_note(p);
}

/* Linear attack, linear decay, both per note; min() of the two ramps and
   full scale, in Q15. A rest (freq 0) bypasses the envelope entirely. */
static uint32_t envelope_q15(const tone_player_t *p) {
    uint32_t env = 32768;
    uint32_t att = (uint32_t)p->def->attack_ms * p->sample_rate / 1000u;
    uint32_t dec = (uint32_t)p->def->decay_ms * p->sample_rate / 1000u;
    if (att > 0 && p->sample_pos < att) {
        uint32_t a = (p->sample_pos << 15) / att;
        if (a < env)
            env = a;
    }
    if (dec > 0) {
        uint32_t left = p->note_samples - p->sample_pos;
        if (left < dec) {
            uint32_t d = (left << 15) / dec;
            if (d < env)
                env = d;
        }
    }
    return env;
}

size_t tones_render(tone_player_t *p, uint8_t *out, size_t n) {
    if (!p->def)
        return 0;
    size_t written = 0;
    while (written < n) {
        if (p->sample_pos >= p->note_samples) {
            if (p->note_idx + 1 >= p->def->n_notes) {
                p->def = NULL; /* finished; stay finished */
                break;
            }
            p->note_idx++;
            load_note(p);
            continue; /* re-check: a note can be zero samples long */
        }
        const tone_note_t *note = &p->def->notes[p->note_idx];
        if (note->freq_hz == 0) {
            out[written] = 128;
        } else {
            int32_t s = s_sine[p->phase >> 16];
            int32_t scale = (int32_t)((envelope_q15(p) * TONE_AMPLITUDE) >> 15);
            out[written] = (uint8_t)(128 + (s * scale) / 127);
            p->phase = (p->phase + p->phase_inc) & 0xFFFFFFu;
        }
        p->sample_pos++;
        written++;
    }
    return written;
}
