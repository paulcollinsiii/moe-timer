#include <string.h>
#include <unity.h>

/* Single-TU compilation */
#include "../../main/tones.c"

void setUp(void) {}
void tearDown(void) {}

#define RATE 16000u

/* Render an entire tone in awkward chunk sizes; returns total samples. */
static size_t render_all(int id, uint8_t *buf, size_t cap) {
    tone_player_t p;
    tones_player_init(&p, id, RATE);
    size_t total = 0;
    for (;;) {
        size_t chunk = (total % 3) + 61; /* deliberately unaligned */
        if (chunk > cap - total)
            chunk = cap - total;
        size_t got = tones_render(&p, buf + total, chunk);
        if (got == 0)
            break;
        total += got;
        TEST_ASSERT_TRUE_MESSAGE(total <= cap, "tone longer than test cap");
    }
    return total;
}

static uint32_t tone_expected_samples(int id) {
    const tone_def_t *def = tones_get(id);
    uint32_t total = 0;
    for (int i = 0; i < def->n_notes; i++) {
        total += (uint32_t)def->notes[i].dur_ms * RATE / 1000u;
    }
    return total;
}

/* ---- table sanity ---- */

void test_all_tones_have_unique_names(void) {
    for (int i = 0; i < TONE_COUNT; i++) {
        TEST_ASSERT_NOT_NULL(tones_option_name(i));
        for (int j = i + 1; j < TONE_COUNT; j++) {
            TEST_ASSERT_TRUE(strcmp(tones_option_name(i), tones_option_name(j)) != 0);
        }
    }
    TEST_ASSERT_EQUAL_INT(TONE_COUNT, tones_option_count());
    TEST_ASSERT_NULL(tones_option_name(-1));
    TEST_ASSERT_NULL(tones_option_name(TONE_COUNT));
}

void test_synth_tones_have_notes_custom_does_not(void) {
    for (int i = 0; i < TONE_COUNT; i++) {
        const tone_def_t *def = tones_get(i);
        TEST_ASSERT_NOT_NULL(def);
        if (i == TONE_CUSTOM) {
            TEST_ASSERT_NULL(def->notes);
        } else {
            TEST_ASSERT_NOT_NULL(def->notes);
            TEST_ASSERT_TRUE(def->n_notes > 0);
        }
    }
    TEST_ASSERT_NULL(tones_get(-1));
    TEST_ASSERT_NULL(tones_get(TONE_COUNT));
}

void test_clamp_id(void) {
    TEST_ASSERT_EQUAL_INT(TONE_MARIMBA, tones_clamp_id(TONE_MARIMBA, TONE_CHIME));
    TEST_ASSERT_EQUAL_INT(TONE_CUSTOM, tones_clamp_id(TONE_CUSTOM, TONE_CHIME));
    TEST_ASSERT_EQUAL_INT(TONE_CHIME, tones_clamp_id(-1, TONE_CHIME));
    TEST_ASSERT_EQUAL_INT(TONE_CHIME, tones_clamp_id(TONE_COUNT, TONE_CHIME));
    TEST_ASSERT_EQUAL_INT(TONE_CHIME, tones_clamp_id(999, TONE_CHIME));
}

/* ---- renderer ---- */

static uint8_t s_buf[RATE * 8]; /* 8 s cap, plenty for every tone */

void test_render_sample_counts_match_note_tables(void) {
    for (int id = 0; id < TONE_CUSTOM; id++) {
        size_t got = render_all(id, s_buf, sizeof(s_buf));
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(tone_expected_samples(id), (uint32_t)got, tones_option_name(id));
    }
}

void test_render_custom_is_empty(void) {
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)render_all(TONE_CUSTOM, s_buf, sizeof(s_buf)));
}

void test_rests_emit_midpoint(void) {
    /* Classic = beep / rest / beep / rest / beep: every rest sample must
       be exactly the DAC midpoint. */
    const tone_def_t *def = tones_get(TONE_CLASSIC);
    size_t got = render_all(TONE_CLASSIC, s_buf, sizeof(s_buf));
    TEST_ASSERT_TRUE(got > 0);
    size_t pos = 0;
    for (int i = 0; i < def->n_notes; i++) {
        size_t len = (size_t)def->notes[i].dur_ms * RATE / 1000u;
        if (def->notes[i].freq_hz == 0) {
            for (size_t s = pos; s < pos + len; s++) {
                TEST_ASSERT_EQUAL_UINT8(128, s_buf[s]);
            }
        }
        pos += len;
    }
}

void test_envelope_bounds_and_click_free_edges(void) {
    for (int id = 0; id < TONE_CUSTOM; id++) {
        const tone_def_t *def = tones_get(id);
        size_t got = render_all(id, s_buf, sizeof(s_buf));
        /* Amplitude never leaves the sane band around the midpoint. */
        for (size_t s = 0; s < got; s++) {
            TEST_ASSERT_TRUE_MESSAGE(s_buf[s] >= 128 - TONE_AMPLITUDE && s_buf[s] <= 128 + TONE_AMPLITUDE,
                                     tones_option_name(id));
        }
        /* Attack/decay pin the note edges near the midpoint - no clicks. */
        size_t pos = 0;
        for (int i = 0; i < def->n_notes; i++) {
            size_t len = (size_t)def->notes[i].dur_ms * RATE / 1000u;
            if (def->notes[i].freq_hz != 0 && len > 0) {
                TEST_ASSERT_TRUE_MESSAGE(s_buf[pos] >= 120 && s_buf[pos] <= 136, "note start clicks");
                TEST_ASSERT_TRUE_MESSAGE(s_buf[pos + len - 1] >= 120 && s_buf[pos + len - 1] <= 136, "note end clicks");
            }
            pos += len;
        }
    }
}

void test_note_frequency_via_zero_crossings(void) {
    /* First note of Classic: 1 kHz for 200 ms. Count midpoint crossings
       in the sustained region (skip attack/decay edges). */
    const tone_def_t *def = tones_get(TONE_CLASSIC);
    uint32_t freq = def->notes[0].freq_hz;
    size_t note_len = (size_t)def->notes[0].dur_ms * RATE / 1000u;
    render_all(TONE_CLASSIC, s_buf, sizeof(s_buf));

    size_t lo = (size_t)def->attack_ms * RATE / 1000u + 8;
    size_t hi = note_len - (size_t)def->decay_ms * RATE / 1000u - 8;
    TEST_ASSERT_TRUE(hi > lo);
    int crossings = 0;
    for (size_t s = lo + 1; s < hi; s++) {
        int a = (int)s_buf[s - 1] - 128;
        int b = (int)s_buf[s] - 128;
        if ((a < 0 && b >= 0) || (a >= 0 && b < 0))
            crossings++;
    }
    double seconds = (double)(hi - lo) / RATE;
    double measured = crossings / (2.0 * seconds);
    TEST_ASSERT_TRUE_MESSAGE(measured > freq * 0.98 && measured < freq * 1.02, "frequency off by more than 2%");
}

void test_render_after_finish_stays_finished(void) {
    tone_player_t p;
    tones_player_init(&p, TONE_DING, RATE);
    while (tones_render(&p, s_buf, 1024) > 0) {
    }
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)tones_render(&p, s_buf, 1024));
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)tones_render(&p, s_buf, 1024));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_all_tones_have_unique_names);
    RUN_TEST(test_synth_tones_have_notes_custom_does_not);
    RUN_TEST(test_clamp_id);
    RUN_TEST(test_render_sample_counts_match_note_tables);
    RUN_TEST(test_render_custom_is_empty);
    RUN_TEST(test_rests_emit_midpoint);
    RUN_TEST(test_envelope_bounds_and_click_free_edges);
    RUN_TEST(test_note_frequency_via_zero_crossings);
    RUN_TEST(test_render_after_finish_stays_finished);
    return UNITY_END();
}
