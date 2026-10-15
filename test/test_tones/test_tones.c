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

/* ---- volume ---- */

/* Render a whole tone at a given volume; returns total samples. */
static size_t render_all_vol(int id, int volume_pct, uint8_t *buf, size_t cap) {
    tone_player_t p;
    tones_player_init(&p, id, RATE);
    tones_player_set_volume(&p, volume_pct);
    size_t total = 0;
    size_t got;
    while ((got = tones_render(&p, buf + total, cap - total)) > 0) {
        total += got;
        TEST_ASSERT_TRUE_MESSAGE(total <= cap, "tone longer than test cap");
    }
    return total;
}

static void peak_deviation(const uint8_t *buf, size_t n, int *up, int *down) {
    *up = 0;
    *down = 0;
    for (size_t i = 0; i < n; i++) {
        int d = (int)buf[i] - 128;
        if (d > *up)
            *up = d;
        if (-d > *down)
            *down = -d;
    }
}

void test_volume_zero_is_silent(void) {
    size_t got = render_all_vol(TONE_CLASSIC, 0, s_buf, sizeof(s_buf));
    TEST_ASSERT_TRUE(got > 0); /* still renders (timing unchanged), just silent */
    for (size_t i = 0; i < got; i++) {
        TEST_ASSERT_EQUAL_UINT8(128, s_buf[i]);
    }
}

void test_volume_100_matches_default_amplitude(void) {
    size_t got = render_all_vol(TONE_CLASSIC, 100, s_buf, sizeof(s_buf));
    int up, down;
    peak_deviation(s_buf, got, &up, &down);
    /* 100% = the legacy TONE_AMPLITUDE peak (same output as no set call) */
    TEST_ASSERT_INT_WITHIN(2, TONE_AMPLITUDE, up);
    TEST_ASSERT_INT_WITHIN(2, TONE_AMPLITUDE, down);
}

void test_volume_50_halves_peak(void) {
    size_t got = render_all_vol(TONE_CLASSIC, 50, s_buf, sizeof(s_buf));
    int up, down;
    peak_deviation(s_buf, got, &up, &down);
    TEST_ASSERT_INT_WITHIN(2, TONE_AMPLITUDE / 2, up);
    TEST_ASSERT_INT_WITHIN(2, TONE_AMPLITUDE / 2, down);
}

void test_volume_200_clips_at_full_scale(void) {
    size_t got = render_all_vol(TONE_CLASSIC, TONES_VOLUME_MAX, s_buf, sizeof(s_buf));
    int up, down;
    peak_deviation(s_buf, got, &up, &down);
    /* Boost drives into the rails, never past them. */
    TEST_ASSERT_EQUAL_INT(127, up);
    TEST_ASSERT_EQUAL_INT(127, down);
    /* And actually clips: a sustained plateau at the rails, not one point
       per period. 1 kHz for 200 ms = 200 periods; a clean full-scale sine
       touches each rail ~once per period, clipping holds it there. */
    size_t at_top = 0;
    for (size_t i = 0; i < got; i++) {
        if (s_buf[i] == 255)
            at_top++;
    }
    TEST_ASSERT_TRUE_MESSAGE(at_top > 1000, "no clipping plateau at 200%");
}

void test_volume_out_of_range_clamped(void) {
    /* > max behaves as max; negative behaves as mute */
    size_t got = render_all_vol(TONE_DING, 999, s_buf, sizeof(s_buf));
    int up, down;
    peak_deviation(s_buf, got, &up, &down);
    TEST_ASSERT_EQUAL_INT(127, up);
    got = render_all_vol(TONE_DING, -5, s_buf, sizeof(s_buf));
    for (size_t i = 0; i < got; i++) {
        TEST_ASSERT_EQUAL_UINT8(128, s_buf[i]);
    }
}

void test_render_after_finish_stays_finished(void) {
    tone_player_t p;
    tones_player_init(&p, TONE_DING, RATE);
    while (tones_render(&p, s_buf, 1024) > 0) {
    }
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)tones_render(&p, s_buf, 1024));
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)tones_render(&p, s_buf, 1024));
}

/* ---- chore reward jingles (their own list; not tone_id_t) ---- */

static uint32_t reward_total_ms(int id) {
    const tone_def_t *def = reward_tones_get(id);
    uint32_t ms = 0;
    for (int i = 0; i < def->n_notes; i++)
        ms += def->notes[i].dur_ms;
    return ms;
}

void test_reward_option_list_is_off_first_and_unique(void) {
    TEST_ASSERT_EQUAL_INT(0, REWARD_OFF);
    TEST_ASSERT_EQUAL_INT(4, REWARD_COUNT);
    TEST_ASSERT_EQUAL_INT(REWARD_COUNT, reward_tones_option_count());
    TEST_ASSERT_EQUAL_STRING("Off", reward_tones_option_name(REWARD_OFF));
    TEST_ASSERT_EQUAL_STRING("Zelda secret", reward_tones_option_name(REWARD_ZELDA));
    TEST_ASSERT_EQUAL_STRING("Mario coin", reward_tones_option_name(REWARD_COIN));
    TEST_ASSERT_EQUAL_STRING("Mario 1-up", reward_tones_option_name(REWARD_1UP));
    TEST_ASSERT_NULL(reward_tones_option_name(-1));
    TEST_ASSERT_NULL(reward_tones_option_name(REWARD_COUNT));
    for (int i = 0; i < REWARD_COUNT; i++)
        for (int j = i + 1; j < REWARD_COUNT; j++)
            TEST_ASSERT_TRUE(strcmp(reward_tones_names[i], reward_tones_names[j]) != 0);
}

void test_reward_does_not_widen_the_alert_tone_list(void) {
    /* Decision 2: the jingles are not offered to expiry/break/bed. */
    TEST_ASSERT_EQUAL_INT(6, TONE_COUNT);
    TEST_ASSERT_EQUAL_INT(TONE_COUNT, tones_option_count());
    for (int i = 0; i < TONE_COUNT; i++) {
        for (int j = 0; j < REWARD_COUNT; j++)
            TEST_ASSERT_TRUE(strcmp(tones_option_name(i), reward_tones_option_name(j)) != 0);
    }
}

void test_reward_off_and_out_of_range_have_no_def(void) {
    TEST_ASSERT_NULL(reward_tones_get(REWARD_OFF));
    TEST_ASSERT_NULL(reward_tones_get(-1));
    TEST_ASSERT_NULL(reward_tones_get(REWARD_COUNT));
    for (int id = REWARD_ZELDA; id < REWARD_COUNT; id++) {
        const tone_def_t *def = reward_tones_get(id);
        TEST_ASSERT_NOT_NULL(def);
        TEST_ASSERT_NOT_NULL(def->notes);
        TEST_ASSERT_TRUE(def->n_notes > 0);
    }
}

void test_reward_resolve_plays_only_a_real_jingle_at_audible_volume(void) {
    for (int id = REWARD_ZELDA; id < REWARD_COUNT; id++) {
        TEST_ASSERT_EQUAL_PTR(reward_tones_get(id), reward_tones_resolve((uint16_t)id, 100));
        TEST_ASSERT_EQUAL_PTR(reward_tones_get(id), reward_tones_resolve((uint16_t)id, 1));
        TEST_ASSERT_EQUAL_PTR(reward_tones_get(id), reward_tones_resolve((uint16_t)id, TONES_VOLUME_MAX));
        TEST_ASSERT_NULL(reward_tones_resolve((uint16_t)id, 0)); /* mute */
    }
    TEST_ASSERT_NULL(reward_tones_resolve(REWARD_OFF, 100));
    TEST_ASSERT_NULL(reward_tones_resolve(REWARD_COUNT, 100)); /* future/older firmware */
    TEST_ASSERT_NULL(reward_tones_resolve(999, 100));
    TEST_ASSERT_NULL(reward_tones_resolve(0xFFFF, 100)); /* NVS sentinel */
}

void test_reward_every_jingle_fits_the_blocking_budget(void) {
    for (int id = REWARD_ZELDA; id < REWARD_COUNT; id++)
        TEST_ASSERT_TRUE_MESSAGE(reward_total_ms(id) <= REWARD_TONE_MAX_MS, reward_tones_option_name(id));
    /* The drain tail is fixed by audio.c's DAC ring: 4 desc x 1024 B at 16 kHz. */
    TEST_ASSERT_EQUAL_INT(256, REWARD_TONE_DRAIN_MS);
}

void test_reward_notes_are_under_8khz(void) {
    for (int id = REWARD_ZELDA; id < REWARD_COUNT; id++) {
        const tone_def_t *def = reward_tones_get(id);
        for (int i = 0; i < def->n_notes; i++) {
            TEST_ASSERT_TRUE_MESSAGE(def->notes[i].freq_hz < 8000, reward_tones_option_name(id));
            TEST_ASSERT_TRUE_MESSAGE(def->notes[i].dur_ms > 0, reward_tones_option_name(id));
        }
    }
}

void test_reward_note_counts_and_durations(void) {
    TEST_ASSERT_EQUAL_UINT8(8, reward_tones_get(REWARD_ZELDA)->n_notes);
    TEST_ASSERT_EQUAL_UINT8(2, reward_tones_get(REWARD_COIN)->n_notes);
    TEST_ASSERT_EQUAL_UINT8(6, reward_tones_get(REWARD_1UP)->n_notes);
    TEST_ASSERT_TRUE(reward_total_ms(REWARD_ZELDA) >= 900 && reward_total_ms(REWARD_ZELDA) <= 1100);
    TEST_ASSERT_TRUE(reward_total_ms(REWARD_COIN) >= 500 && reward_total_ms(REWARD_COIN) <= 700);
    TEST_ASSERT_TRUE(reward_total_ms(REWARD_1UP) >= 700 && reward_total_ms(REWARD_1UP) <= 900);
}

void test_reward_last_note_rings_on_zelda_and_coin(void) {
    for (int id = REWARD_ZELDA; id <= REWARD_COIN; id++) {
        const tone_def_t *def = reward_tones_get(id);
        const uint16_t last = def->notes[def->n_notes - 1].dur_ms;
        for (int i = 0; i + 1 < def->n_notes; i++)
            TEST_ASSERT_TRUE_MESSAGE(last > def->notes[i].dur_ms, reward_tones_option_name(id));
    }
}

void test_reward_renders_exactly_its_note_table(void) {
    for (int id = REWARD_ZELDA; id < REWARD_COUNT; id++) {
        tone_player_t p;
        tones_player_init_def(&p, reward_tones_get(id), RATE);
        size_t total = 0, got;
        while ((got = tones_render(&p, s_buf + total, 61)) > 0)
            total += got;
        uint32_t expect = 0;
        const tone_def_t *def = reward_tones_get(id);
        for (int i = 0; i < def->n_notes; i++)
            expect += (uint32_t)def->notes[i].dur_ms * RATE / 1000u;
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(expect, (uint32_t)total, reward_tones_option_name(id));
        int up, down;
        peak_deviation(s_buf, total, &up, &down);
        TEST_ASSERT_TRUE_MESSAGE(up > 10, "reward jingle rendered silence");
    }
}

void test_reward_off_renders_nothing(void) {
    tone_player_t p;
    tones_player_init_def(&p, reward_tones_get(REWARD_OFF), RATE);
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)tones_render(&p, s_buf, 1024));
    tones_player_init_def(&p, NULL, RATE);
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)tones_render(&p, s_buf, 1024));
}

void test_player_init_def_matches_player_init_for_alert_ids(void) {
    static uint8_t other[RATE * 2];
    tone_player_t a, b;
    tones_player_init(&a, TONE_DING, RATE);
    tones_player_init_def(&b, tones_get(TONE_DING), RATE);
    size_t na = tones_render(&a, s_buf, sizeof(s_buf));
    size_t nb = tones_render(&b, other, sizeof(other));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)na, (uint32_t)nb);
    TEST_ASSERT_EQUAL_MEMORY(s_buf, other, na);
}

void test_reward_volume_zero_is_silent(void) {
    tone_player_t p;
    tones_player_init_def(&p, reward_tones_get(REWARD_COIN), RATE);
    tones_player_set_volume(&p, 0);
    size_t got = tones_render(&p, s_buf, sizeof(s_buf));
    TEST_ASSERT_TRUE(got > 0);
    for (size_t i = 0; i < got; i++)
        TEST_ASSERT_EQUAL_UINT8(128, s_buf[i]);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_reward_option_list_is_off_first_and_unique);
    RUN_TEST(test_reward_does_not_widen_the_alert_tone_list);
    RUN_TEST(test_reward_off_and_out_of_range_have_no_def);
    RUN_TEST(test_reward_resolve_plays_only_a_real_jingle_at_audible_volume);
    RUN_TEST(test_reward_every_jingle_fits_the_blocking_budget);
    RUN_TEST(test_reward_notes_are_under_8khz);
    RUN_TEST(test_reward_note_counts_and_durations);
    RUN_TEST(test_reward_last_note_rings_on_zelda_and_coin);
    RUN_TEST(test_reward_renders_exactly_its_note_table);
    RUN_TEST(test_reward_off_renders_nothing);
    RUN_TEST(test_player_init_def_matches_player_init_for_alert_ids);
    RUN_TEST(test_reward_volume_zero_is_silent);
    RUN_TEST(test_all_tones_have_unique_names);
    RUN_TEST(test_synth_tones_have_notes_custom_does_not);
    RUN_TEST(test_clamp_id);
    RUN_TEST(test_render_sample_counts_match_note_tables);
    RUN_TEST(test_render_custom_is_empty);
    RUN_TEST(test_rests_emit_midpoint);
    RUN_TEST(test_envelope_bounds_and_click_free_edges);
    RUN_TEST(test_note_frequency_via_zero_crossings);
    RUN_TEST(test_render_after_finish_stays_finished);
    RUN_TEST(test_volume_zero_is_silent);
    RUN_TEST(test_volume_100_matches_default_amplitude);
    RUN_TEST(test_volume_50_halves_peak);
    RUN_TEST(test_volume_200_clips_at_full_scale);
    RUN_TEST(test_volume_out_of_range_clamped);
    return UNITY_END();
}
