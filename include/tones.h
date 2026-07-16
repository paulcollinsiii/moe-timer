/* Alert-tone tables + streaming sample renderer (pure C, host-testable).
 *
 * A tone is a table of {freq_hz, dur_ms} notes (freq 0 = rest) rendered
 * as a sine with a per-note attack/decay envelope into unsigned 8-bit
 * samples (128 = DAC midpoint). The same renderer feeds the DAC pump on
 * device and the assertions in test_tones on the host.
 */
#ifndef TONES_H
#define TONES_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    TONE_CLASSIC = 0, /* the original 1 kHz triple beep */
    TONE_DING,        /* two quick strikes on one note */
    TONE_CHIME,       /* two descending notes, doorbell-like */
    TONE_MARIMBA,     /* soft ascending C-E-G roll */
    TONE_GRANVALS,    /* Tarrega, Gran Vals opening phrase (public domain) */
    TONE_CUSTOM,      /* 16-bit mono WAV from the assets partition */
    TONE_COUNT,
} tone_id_t;

typedef struct {
    uint16_t freq_hz; /* 0 = rest */
    uint16_t dur_ms;
} tone_note_t;

typedef struct {
    const tone_note_t *notes;
    uint8_t n_notes;
    uint16_t attack_ms; /* linear ramp-in per note */
    uint16_t decay_ms;  /* linear ramp-out per note (>= note length - attack
                           yields a pluck that decays the whole note) */
} tone_def_t;

/* HA select option strings, indexed by tone_id_t (compile-time constant
   so the ha_config field registry can reference it in initializers). */
extern const char *const tones_names[TONE_COUNT];

/* NULL for out-of-range ids; TONE_CUSTOM has notes == NULL (played from
   the WAV path, not the synth). */
const tone_def_t *tones_get(int id);

/* HA select plumbing: option strings by index (NULL out of range). */
const char *tones_option_name(int idx);
int tones_option_count(void);

/* Stored config index -> playable id; anything out of range (or a
   negative sentinel) collapses to fallback. */
int tones_clamp_id(int idx, int fallback);

/* Volume ceiling (percent). 100 = the tuned reference amplitude; above
   that the renderer applies digital gain and clips at the DAC rails,
   trading sine purity for real loudness on the small speaker. */
#define TONES_VOLUME_MAX 200

typedef struct {
    const tone_def_t *def;
    uint32_t sample_rate;
    uint8_t note_idx;
    uint32_t sample_pos;   /* within the current note */
    uint32_t note_samples; /* length of the current note */
    uint32_t phase;        /* one period == 1 << 24 */
    uint32_t phase_inc;
    uint16_t peak; /* peak deviation target; may exceed the rails (clipped) */
} tone_player_t;

void tones_player_init(tone_player_t *p, int id, uint32_t sample_rate);

/* Volume in percent, clamped to [0, TONES_VOLUME_MAX]; init defaults to
   100. 0 renders silence (timing preserved). */
void tones_player_set_volume(tone_player_t *p, int volume_pct);

/* Fill out with up to n samples; returns the count written (0 = tone
   finished). Rests and silence emit 128. */
size_t tones_render(tone_player_t *p, uint8_t *out, size_t n);

#endif /* TONES_H */
