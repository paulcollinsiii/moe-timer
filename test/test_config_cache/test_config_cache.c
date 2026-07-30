#include <stdlib.h>
#include <time.h>
#include <unity.h>

/* Single-TU: the cache under test on top of the *real* NVS config layer,
   the real HHMM validators, and the in-memory NVS mock. Stubbing
   nvs_config_get_*() instead would let a cache bug hide behind a stub
   that never touched a key — mock_hal_nvs counts reads per key, so
   "loads once per wake" becomes an assertion about the actual store. */
#include "../../main/bedtime.c"
#include "../../main/config_cache.c"
#include "../../main/nvs_config.c"
#include "../../main/quiet_hours.c"
#include "mock_hal_nvs.c"

/* ---- link-time stubs ---------------------------------------------------- */

/* schedule.c owns its own wake-scoped cache; config_cache drops it in the
   same call so the orchestrator has one invalidation point. */
static int stub_schedule_invalidations;

void schedule_cache_invalidate(void) {
    stub_schedule_invalidations++;
}

/* ---- fixture ------------------------------------------------------------ */

/* Epoch seconds for a wall-clock time; under the UTC0 fixture below that
   is also the local time. Local minutes-of-day is what both caches
   compare against, so the zone is pinned per test — an inherited TZ would
   silently shift every window boundary. */
static time_t epoch_at(int hh, int mm) {
    return (time_t)(hh * 3600 + mm * 60);
}

void setUp(void) {
    setenv("TZ", "UTC0", 1);
    tzset();
    mock_nvs_reset();
    stub_schedule_invalidations = 0;
    /* Zero the wake-scoped statics directly rather than calling
       config_cache_invalidate(): the invalidator is itself under test and
       must not be the thing that makes the other cases start clean. */
    s_quiet_cfg_loaded = false;
    s_quiet_start_cfg = 0;
    s_quiet_end_cfg = 0;
    s_bedtime_cfg_loaded = false;
    s_bedtime_cfg = 0;
}

void tearDown(void) {}

/* ---- quiet hours: miss / hit -------------------------------------------- */

void test_quiet_miss_reads_each_key_exactly_once(void) {
    (void)config_cache_quiet_active(epoch_at(12, 0));
    TEST_ASSERT_EQUAL_INT(1, mock_nvs_read_count(NVS_KEY_QUIET_START));
    TEST_ASSERT_EQUAL_INT(1, mock_nvs_read_count(NVS_KEY_QUIET_END));
}

void test_quiet_hit_reads_nothing_more(void) {
    /* The neopixel driver calls this from the LED task on every pixel
       update — a per-call NVS read would put flash access on the hot
       path of a status blink. */
    (void)config_cache_quiet_active(epoch_at(12, 0));
    (void)config_cache_quiet_active(epoch_at(13, 0));
    (void)config_cache_quiet_active(epoch_at(23, 0));
    TEST_ASSERT_EQUAL_INT(1, mock_nvs_read_count(NVS_KEY_QUIET_START));
    TEST_ASSERT_EQUAL_INT(1, mock_nvs_read_count(NVS_KEY_QUIET_END));
}

void test_quiet_cache_holds_the_stale_window_until_invalidated(void) {
    /* Proves it is a cache and not an accidental re-read: the stored
       window changes underneath and the answer must not move. */
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_quiet_start(2230));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_quiet_end(800));
    TEST_ASSERT_TRUE(config_cache_quiet_active(epoch_at(23, 0)));

    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_quiet_start(100));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_quiet_end(200));
    TEST_ASSERT_TRUE(config_cache_quiet_active(epoch_at(23, 0)));

    config_cache_invalidate();
    TEST_ASSERT_FALSE(config_cache_quiet_active(epoch_at(23, 0)));
}

void test_quiet_invalidate_forces_exactly_one_fresh_read(void) {
    (void)config_cache_quiet_active(epoch_at(12, 0));
    config_cache_invalidate();
    (void)config_cache_quiet_active(epoch_at(12, 0));
    (void)config_cache_quiet_active(epoch_at(12, 0));
    TEST_ASSERT_EQUAL_INT(2, mock_nvs_read_count(NVS_KEY_QUIET_START));
    TEST_ASSERT_EQUAL_INT(2, mock_nvs_read_count(NVS_KEY_QUIET_END));
}

void test_quiet_invalidate_before_any_load_does_not_double_read(void) {
    /* A network window can finish before anything asked for the window;
       the invalidation must not leave the cache in a state that reloads
       twice on the first real call. */
    config_cache_invalidate();
    (void)config_cache_quiet_active(epoch_at(12, 0));
    TEST_ASSERT_EQUAL_INT(1, mock_nvs_read_count(NVS_KEY_QUIET_START));
    TEST_ASSERT_EQUAL_INT(1, mock_nvs_read_count(NVS_KEY_QUIET_END));
}

/* ---- quiet hours: the window itself ------------------------------------- */

void test_quiet_converts_hhmm_before_comparing(void) {
    /* The stored form is HHMM, the comparison unit is minutes-of-day.
       Skipping the fold makes 2230 read as minute 2230 (> 24 h), which
       parks the window permanently in the future — LEDs never go dark. */
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_quiet_start(2230));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_quiet_end(800));
    TEST_ASSERT_TRUE(config_cache_quiet_active(epoch_at(22, 45)));
    TEST_ASSERT_TRUE(config_cache_quiet_active(epoch_at(3, 0)));
    TEST_ASSERT_FALSE(config_cache_quiet_active(epoch_at(12, 0)));
}

void test_quiet_wrapping_window_boundaries_are_start_inclusive_end_exclusive(void) {
    /* Only the wrapping branch, which is the shipped configuration; the
       non-wrapping branch and the fold itself belong to test_quiet_hours
       and are not duplicated here. */
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_quiet_start(2230));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_quiet_end(800));
    TEST_ASSERT_TRUE(config_cache_quiet_active(epoch_at(22, 30)));
    TEST_ASSERT_FALSE(config_cache_quiet_active(epoch_at(8, 0)));
    TEST_ASSERT_TRUE(config_cache_quiet_active(epoch_at(7, 59)));
}

void test_quiet_zero_length_window_is_disabled(void) {
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_quiet_start(0));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_quiet_end(0));
    TEST_ASSERT_FALSE(config_cache_quiet_active(epoch_at(3, 0)));
    TEST_ASSERT_FALSE(config_cache_quiet_active(epoch_at(23, 0)));
}

void test_quiet_does_not_validate_the_stored_window(void) {
    /* Pins the asymmetry against bed time (see config_cache.h): the quiet
       path folds whatever is stored and never falls back. 9999 folds to
       minute 6039, so the start edge moves out of a day's reach and 23:00
       — quiet under the 2230 default — stops being quiet. Adding a
       quiet_hhmm_valid() fallback here to "make the two paths
       consistent" would light 23:00 back up and fail this. */
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_quiet_start(9999));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_quiet_end(800));
    TEST_ASSERT_FALSE(config_cache_quiet_active(epoch_at(23, 0)));
    TEST_ASSERT_FALSE(config_cache_quiet_active(epoch_at(12, 0)));
    /* The surviving end edge still bounds a window, so the corruption is
       arbitrary rather than uniformly "never quiet" — 03:00 stays dark. */
    TEST_ASSERT_TRUE(config_cache_quiet_active(epoch_at(3, 0)));
}

void test_quiet_corrupt_window_is_cosmetic_not_a_lockout(void) {
    /* The justification for not validating: the worst a corrupt window
       can do is invert the pixels. 8888 folds below 9999, so the wrap
       branch swallows the whole day and every hour reads as quiet — dark
       status LEDs, nothing else. Contrast bed time, where the equivalent
       corruption would lock the screen; that is why only one of them
       carries a fallback. */
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_quiet_start(9999));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_quiet_end(8888));
    TEST_ASSERT_TRUE(config_cache_quiet_active(epoch_at(3, 0)));
    TEST_ASSERT_TRUE(config_cache_quiet_active(epoch_at(12, 0)));
    TEST_ASSERT_TRUE(config_cache_quiet_active(epoch_at(23, 0)));
}

void test_quiet_falls_back_to_the_compile_time_window_on_blank_nvs(void) {
    /* Unprovisioned device: the defaults must still darken the pixels
       overnight rather than leaving the window at 00:00-00:00. */
    const int start = quiet_hhmm_to_minutes(NVS_DEFAULT_QUIET_START);
    const int end = quiet_hhmm_to_minutes(NVS_DEFAULT_QUIET_END);
    TEST_ASSERT_TRUE_MESSAGE(start != end, "default quiet window is degenerate; test is vacuous");
    TEST_ASSERT_TRUE(config_cache_quiet_active(epoch_at(NVS_DEFAULT_QUIET_START / 100, NVS_DEFAULT_QUIET_START % 100)));
}

void test_quiet_uses_local_time_not_utc(void) {
    /* The device runs on the NVS timezone; comparing a UTC reading would
       put the window hours off (CLAUDE.md: bed time / quiet hours are
       local-wall-clock policies). */
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_quiet_start(2230));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_quiet_end(800));
    setenv("TZ", "EST5", 1); /* UTC-5: 03:00Z is 22:00 local, outside */
    tzset();
    TEST_ASSERT_FALSE(config_cache_quiet_active(epoch_at(3, 0)));
    TEST_ASSERT_TRUE(config_cache_quiet_active(epoch_at(4, 0))); /* 23:00 local */
}

/* ---- bed time: miss / hit ----------------------------------------------- */

void test_bedtime_miss_reads_the_key_exactly_once(void) {
    (void)config_cache_bedtime_minutes();
    TEST_ASSERT_EQUAL_INT(1, mock_nvs_read_count(NVS_KEY_BEDTIME));
}

void test_bedtime_hit_reads_nothing_more(void) {
    /* Three call sites per wake (check_bedtime, the rollover recheck and
       the break-crossing test); one read between them. */
    (void)config_cache_bedtime_minutes();
    (void)config_cache_bedtime_minutes();
    (void)config_cache_bedtime_minutes();
    TEST_ASSERT_EQUAL_INT(1, mock_nvs_read_count(NVS_KEY_BEDTIME));
}

void test_bedtime_invalidate_forces_exactly_one_fresh_read(void) {
    (void)config_cache_bedtime_minutes();
    config_cache_invalidate();
    (void)config_cache_bedtime_minutes();
    (void)config_cache_bedtime_minutes();
    TEST_ASSERT_EQUAL_INT(2, mock_nvs_read_count(NVS_KEY_BEDTIME));
}

void test_bedtime_edit_applies_within_the_same_wake_after_invalidate(void) {
    /* The reason the invalidator exists: HA edits bedtime during the
       network window and the lock must release in this wake, not in two
       hours (plan row 14). */
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_bedtime(1900));
    TEST_ASSERT_EQUAL_INT(19 * 60, config_cache_bedtime_minutes());
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_bedtime(2100));
    TEST_ASSERT_EQUAL_INT(19 * 60, config_cache_bedtime_minutes());
    config_cache_invalidate();
    TEST_ASSERT_EQUAL_INT(21 * 60, config_cache_bedtime_minutes());
}

/* ---- bed time: the fallback asymmetry ----------------------------------- */

void test_bedtime_valid_value_folds_to_minutes(void) {
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_bedtime(2130));
    TEST_ASSERT_EQUAL_INT(21 * 60 + 30, config_cache_bedtime_minutes());
}

void test_bedtime_zero_stays_disabled_and_does_not_fall_back(void) {
    /* THE ASYMMETRY. 0 is a legitimate stored value meaning "no bedtime
       on this device" — not every device belongs to a kid (bedtime.h).
       bedtime_minutes(0) returns -1 exactly as an invalid value does, so
       a fallback keyed on "-1" alone would silently lock a disabled
       device onto the Bed Time screen every evening. Anyone who
       "simplifies" the `!= 0` guard away fails here. */
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_bedtime(0));
    TEST_ASSERT_EQUAL_INT(-1, config_cache_bedtime_minutes());
}

void test_bedtime_zero_does_not_re_read_looking_for_a_better_value(void) {
    /* Disabled is a cached answer like any other; retrying the key would
       reintroduce the per-call flash read the cache exists to remove. */
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_bedtime(0));
    TEST_ASSERT_EQUAL_INT(-1, config_cache_bedtime_minutes());
    TEST_ASSERT_EQUAL_INT(-1, config_cache_bedtime_minutes());
    TEST_ASSERT_EQUAL_INT(1, mock_nvs_read_count(NVS_KEY_BEDTIME));
}

void test_bedtime_below_the_evening_floor_falls_back_to_the_default(void) {
    /* 12:00 is a well-formed HHMM but below bedtime.c's 1800 floor: a bad
       edit must not day-lock the device, and disabling instead would lose
       the lock entirely. The default is the only safe third answer. */
    const int fallback = bedtime_minutes(NVS_DEFAULT_BEDTIME);
    TEST_ASSERT_TRUE_MESSAGE(fallback >= 0, "default bedtime is itself invalid; test is vacuous");
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_bedtime(1200));
    TEST_ASSERT_EQUAL_INT(fallback, config_cache_bedtime_minutes());
}

void test_bedtime_malformed_hhmm_falls_back_to_the_default(void) {
    /* 2260 is in range (<= 2359) and past the 1800 floor, so it clears
       both cheap checks and is rejected only on minute 60 — the case
       quiet_hours.h calls out as the reason a plain range test is not
       enough. A value like 2560 would not exercise this: the range check
       rejects it first and the minute test is never reached. */
    const int fallback = bedtime_minutes(NVS_DEFAULT_BEDTIME);
    TEST_ASSERT_TRUE_MESSAGE(fallback >= 0, "default bedtime is itself invalid; test is vacuous");
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_bedtime(2260));
    TEST_ASSERT_EQUAL_INT(fallback, config_cache_bedtime_minutes());
}

void test_bedtime_blank_nvs_yields_the_default(void) {
    const int fallback = bedtime_minutes(NVS_DEFAULT_BEDTIME);
    TEST_ASSERT_TRUE_MESSAGE(fallback >= 0, "default bedtime is itself invalid; test is vacuous");
    TEST_ASSERT_EQUAL_INT(fallback, config_cache_bedtime_minutes());
}

void test_bedtime_fallback_is_computed_not_persisted(void) {
    /* Writing the default back would destroy the operator's edit and make
       the bad value unrecoverable from the HA side. */
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_bedtime(1200));
    (void)config_cache_bedtime_minutes();
    uint16_t stored = 0xFFFF;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_bedtime(&stored));
    TEST_ASSERT_EQUAL_UINT16(1200, stored);
    /* And the cache still holds the raw edit, not the fallback. Writing
       the folded default back into s_bedtime_cfg would make the fallback
       sticky: the next invalidation would reload 1200, but any reader in
       between would see a bedtime the operator never set, and the "0
       stays disabled" tie-break would start reading the wrong value. */
    TEST_ASSERT_EQUAL_UINT16(1200, s_bedtime_cfg);
}

void test_bedtime_fallback_survives_invalidation(void) {
    /* Re-reading the same bad value must reach the same answer — the
       fallback lives in the read path, not in a one-shot repair. */
    const int fallback = bedtime_minutes(NVS_DEFAULT_BEDTIME);
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_bedtime(1200));
    TEST_ASSERT_EQUAL_INT(fallback, config_cache_bedtime_minutes());
    config_cache_invalidate();
    TEST_ASSERT_EQUAL_INT(fallback, config_cache_bedtime_minutes());
}

/* ---- invalidation ------------------------------------------------------- */

void test_invalidate_drops_the_schedule_cache_too(void) {
    /* One call after a network window drops every wake-scoped config
       cache; a caller that had to remember two would eventually
       remember one. */
    config_cache_invalidate();
    TEST_ASSERT_EQUAL_INT(1, stub_schedule_invalidations);
}

void test_invalidate_drops_both_caches_in_one_call(void) {
    (void)config_cache_quiet_active(epoch_at(12, 0));
    (void)config_cache_bedtime_minutes();
    config_cache_invalidate();
    (void)config_cache_quiet_active(epoch_at(12, 0));
    (void)config_cache_bedtime_minutes();
    TEST_ASSERT_EQUAL_INT(2, mock_nvs_read_count(NVS_KEY_QUIET_START));
    TEST_ASSERT_EQUAL_INT(2, mock_nvs_read_count(NVS_KEY_QUIET_END));
    TEST_ASSERT_EQUAL_INT(2, mock_nvs_read_count(NVS_KEY_BEDTIME));
}

void test_invalidate_does_not_read_nvs_itself(void) {
    /* Invalidation runs at the end of the network window, where the
       radio is still up and a synchronous flash read is pure latency;
       the reload is deferred to whoever asks next. */
    config_cache_invalidate();
    TEST_ASSERT_EQUAL_INT(0, mock_nvs_read_count(NVS_KEY_QUIET_START));
    TEST_ASSERT_EQUAL_INT(0, mock_nvs_read_count(NVS_KEY_QUIET_END));
    TEST_ASSERT_EQUAL_INT(0, mock_nvs_read_count(NVS_KEY_BEDTIME));
}

void test_the_two_caches_are_independent(void) {
    /* Loading one must not load the other: the quiet callback fires from
       the LED task, and pulling the bedtime key in with it would move a
       flash read onto that path. */
    (void)config_cache_quiet_active(epoch_at(12, 0));
    TEST_ASSERT_EQUAL_INT(0, mock_nvs_read_count(NVS_KEY_BEDTIME));
    (void)config_cache_bedtime_minutes();
    TEST_ASSERT_EQUAL_INT(1, mock_nvs_read_count(NVS_KEY_QUIET_START));
    TEST_ASSERT_EQUAL_INT(1, mock_nvs_read_count(NVS_KEY_BEDTIME));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_quiet_miss_reads_each_key_exactly_once);
    RUN_TEST(test_quiet_hit_reads_nothing_more);
    RUN_TEST(test_quiet_cache_holds_the_stale_window_until_invalidated);
    RUN_TEST(test_quiet_invalidate_forces_exactly_one_fresh_read);
    RUN_TEST(test_quiet_invalidate_before_any_load_does_not_double_read);
    RUN_TEST(test_quiet_converts_hhmm_before_comparing);
    RUN_TEST(test_quiet_wrapping_window_boundaries_are_start_inclusive_end_exclusive);
    RUN_TEST(test_quiet_zero_length_window_is_disabled);
    RUN_TEST(test_quiet_does_not_validate_the_stored_window);
    RUN_TEST(test_quiet_corrupt_window_is_cosmetic_not_a_lockout);
    RUN_TEST(test_quiet_falls_back_to_the_compile_time_window_on_blank_nvs);
    RUN_TEST(test_quiet_uses_local_time_not_utc);
    RUN_TEST(test_bedtime_miss_reads_the_key_exactly_once);
    RUN_TEST(test_bedtime_hit_reads_nothing_more);
    RUN_TEST(test_bedtime_invalidate_forces_exactly_one_fresh_read);
    RUN_TEST(test_bedtime_edit_applies_within_the_same_wake_after_invalidate);
    RUN_TEST(test_bedtime_valid_value_folds_to_minutes);
    RUN_TEST(test_bedtime_zero_stays_disabled_and_does_not_fall_back);
    RUN_TEST(test_bedtime_zero_does_not_re_read_looking_for_a_better_value);
    RUN_TEST(test_bedtime_below_the_evening_floor_falls_back_to_the_default);
    RUN_TEST(test_bedtime_malformed_hhmm_falls_back_to_the_default);
    RUN_TEST(test_bedtime_blank_nvs_yields_the_default);
    RUN_TEST(test_bedtime_fallback_is_computed_not_persisted);
    RUN_TEST(test_bedtime_fallback_survives_invalidation);
    RUN_TEST(test_invalidate_drops_the_schedule_cache_too);
    RUN_TEST(test_invalidate_drops_both_caches_in_one_call);
    RUN_TEST(test_invalidate_does_not_read_nvs_itself);
    RUN_TEST(test_the_two_caches_are_independent);
    return UNITY_END();
}
