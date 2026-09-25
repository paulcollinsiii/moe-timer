#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unity.h>

/* Single-TU compilation: pull in mocks then source under test */
// clang-format off
#include "mock_hal_time.c"
#include "mock_hal_nvs.c"
#include "../../main/config_validate.c" /* the chore_free predicate the broken-pair mask judges with */
#include "../../main/schedule.c"
// clang-format on

/* ------------------------------------------------------------------ */
/* setUp / tearDown                                                     */
/* ------------------------------------------------------------------ */

void setUp(void) {
    mock_nvs_reset();
    schedule_cache_invalidate(); /* statics persist across tests in one TU */
    /* Use UTC so timestamps map to predictable dates regardless of host TZ */
    setenv("TZ", "UTC0", 1);
    tzset();
    /* Default: weekday allocation 60 min, weekend 120, holiday 120 */
    hal_nvs_write_u16("weekday_min", 60);
    hal_nvs_write_u16("weekend_min", 120);
    hal_nvs_write_u16("holiday_min", 120);
    /* Default holiday blob: just New Year's Day 2026 */
    const char *holidays = "2026-01-01\n";
    hal_nvs_write_blob("holidays", holidays, strlen(holidays));
    /* Default time: Monday 2026-01-05 */
    mock_time_set(1767571200);
}

void tearDown(void) {}

/* ------------------------------------------------------------------ */
/* schedule_is_holiday — pure string search, no HAL                    */
/* ------------------------------------------------------------------ */

void test_is_holiday_match(void) {
    const char *blob = "2026-01-01\n2026-07-04\n2026-12-25\n";
    TEST_ASSERT_TRUE(schedule_is_holiday("2026-01-01", blob, strlen(blob)));
    TEST_ASSERT_TRUE(schedule_is_holiday("2026-07-04", blob, strlen(blob)));
    TEST_ASSERT_TRUE(schedule_is_holiday("2026-12-25", blob, strlen(blob)));
}

void test_is_holiday_no_match(void) {
    const char *blob = "2026-01-01\n2026-07-04\n";
    TEST_ASSERT_FALSE(schedule_is_holiday("2026-06-15", blob, strlen(blob)));
    TEST_ASSERT_FALSE(schedule_is_holiday("2025-01-01", blob, strlen(blob)));
}

void test_is_holiday_partial_date_not_matched(void) {
    /* "2026-01-0" must NOT match "2026-01-01" */
    const char *blob = "2026-01-01\n";
    TEST_ASSERT_FALSE(schedule_is_holiday("2026-01-0", blob, strlen(blob)));
}

void test_is_holiday_windows_line_endings(void) {
    /* \r\n blobs should still match full dates */
    const char *blob = "2026-01-01\r\n2026-07-04\r\n";
    TEST_ASSERT_TRUE(schedule_is_holiday("2026-01-01", blob, strlen(blob)));
}

void test_is_holiday_trailing_newline(void) {
    const char *blob = "2026-12-25\n";
    TEST_ASSERT_TRUE(schedule_is_holiday("2026-12-25", blob, strlen(blob)));
}

/* ------------------------------------------------------------------ */
/* schedule_get_day_type                                               */
/* ------------------------------------------------------------------ */

void test_weekday_monday(void) {
    mock_time_set(1767571200); /* 2026-01-05 Mon */
    TEST_ASSERT_EQUAL(DAY_WEEKDAY, schedule_get_day_type(hal_time_now()));
}

void test_weekday_friday(void) {
    mock_time_set(1767830400); /* 2026-01-09 Fri */
    TEST_ASSERT_EQUAL(DAY_WEEKDAY, schedule_get_day_type(hal_time_now()));
}

void test_weekend_saturday(void) {
    mock_time_set(1767398400); /* 2026-01-03 Sat */
    TEST_ASSERT_EQUAL(DAY_WEEKEND, schedule_get_day_type(hal_time_now()));
}

void test_weekend_sunday(void) {
    mock_time_set(1767484800); /* 2026-01-04 Sun */
    TEST_ASSERT_EQUAL(DAY_WEEKEND, schedule_get_day_type(hal_time_now()));
}

void test_holiday_new_years_day(void) {
    mock_time_set(1767225600); /* 2026-01-01 Thu — in holiday blob */
    TEST_ASSERT_EQUAL(DAY_HOLIDAY, schedule_get_day_type(hal_time_now()));
}

void test_holiday_takes_priority_over_weekend(void) {
    /* Put a Saturday in the holiday blob; should return HOLIDAY not WEEKEND */
    const char *blob = "2026-01-03\n"; /* Sat */
    hal_nvs_write_blob("holidays", blob, strlen(blob));
    mock_time_set(1767398400); /* 2026-01-03 Sat */
    TEST_ASSERT_EQUAL(DAY_HOLIDAY, schedule_get_day_type(hal_time_now()));
}

void test_non_holiday_weekday_not_holiday(void) {
    mock_time_set(1767571200); /* 2026-01-05 Mon — not in blob */
    TEST_ASSERT_NOT_EQUAL(DAY_HOLIDAY, schedule_get_day_type(hal_time_now()));
}

/* ------------------------------------------------------------------ */
/* schedule_get_allocation_sec                                          */
/* ------------------------------------------------------------------ */

void test_allocation_weekday(void) {
    hal_nvs_write_u16("weekday_min", 45);
    TEST_ASSERT_EQUAL_UINT32(2700, schedule_get_allocation_sec(DAY_WEEKDAY));
}

void test_allocation_weekend(void) {
    hal_nvs_write_u16("weekend_min", 90);
    TEST_ASSERT_EQUAL_UINT32(5400, schedule_get_allocation_sec(DAY_WEEKEND));
}

void test_allocation_holiday(void) {
    hal_nvs_write_u16("holiday_min", 150);
    TEST_ASSERT_EQUAL_UINT32(9000, schedule_get_allocation_sec(DAY_HOLIDAY));
}

void test_allocation_missing_key_falls_back_to_default(void) {
    /* Don't write weekday_min — should fall back to NVS_DEFAULT_WEEKDAY_MIN */
    mock_nvs_reset();
    uint32_t alloc = schedule_get_allocation_sec(DAY_WEEKDAY);
    TEST_ASSERT_EQUAL_UINT32(NVS_DEFAULT_WEEKDAY_MIN * 60, alloc);
}

void test_holiday_falls_back_to_compile_time_defaults(void) {
    /* Clear NVS entirely — no holidays blob written. 2026-12-25 (winter
       break) is in NVS_DEFAULT_HOLIDAYS so must classify as DAY_HOLIDAY. */
    mock_nvs_reset();
    mock_time_set(1798156800); /* 2026-12-25 Fri */
    TEST_ASSERT_EQUAL(DAY_HOLIDAY, schedule_get_day_type(hal_time_now()));
}

/* ------------------------------------------------------------------ */
/* Summer break (Dublin 2026-27: school 2026-08-20 .. 2027-05-28)      */
/* ------------------------------------------------------------------ */

void test_is_summer_boundaries(void) {
    TEST_ASSERT_FALSE(schedule_is_summer("2026-01-05")); /* previous school year */
    TEST_ASSERT_FALSE(schedule_is_summer("2026-05-28")); /* 25-26 last day */
    TEST_ASSERT_TRUE(schedule_is_summer("2026-05-29"));  /* summer begins */
    TEST_ASSERT_TRUE(schedule_is_summer("2026-07-15"));  /* mid summer */
    TEST_ASSERT_TRUE(schedule_is_summer("2026-08-19"));  /* day before school */
    TEST_ASSERT_FALSE(schedule_is_summer("2026-08-20")); /* first day of school */
    TEST_ASSERT_FALSE(schedule_is_summer("2027-05-28")); /* last day of school */
    TEST_ASSERT_TRUE(schedule_is_summer("2027-05-29"));  /* summer resumes */
}

void test_summer_weekday_is_summer(void) {
    mock_time_set(1784073600); /* 2026-07-15 Wed */
    TEST_ASSERT_EQUAL(DAY_SUMMER, schedule_get_day_type(hal_time_now()));
    mock_time_set(1811808000); /* 2027-06-01 Tue */
    TEST_ASSERT_EQUAL(DAY_SUMMER, schedule_get_day_type(hal_time_now()));
}

void test_weekend_beats_summer(void) {
    mock_time_set(1784332800); /* 2026-07-18 Sat, mid summer */
    TEST_ASSERT_EQUAL(DAY_WEEKEND, schedule_get_day_type(hal_time_now()));
}

void test_holiday_beats_summer(void) {
    const char *holidays = "2026-07-15\n";
    hal_nvs_write_blob("holidays", holidays, strlen(holidays));
    mock_time_set(1784073600); /* 2026-07-15 Wed, in the blob */
    TEST_ASSERT_EQUAL(DAY_HOLIDAY, schedule_get_day_type(hal_time_now()));
}

void test_school_year_weekday_not_summer(void) {
    mock_time_set(1788825600); /* 2026-09-08 Tue */
    TEST_ASSERT_EQUAL(DAY_WEEKDAY, schedule_get_day_type(hal_time_now()));
    mock_time_set(1811462400); /* 2027-05-28 Fri — last day is a school day */
    TEST_ASSERT_EQUAL(DAY_WEEKDAY, schedule_get_day_type(hal_time_now()));
}

void test_allocation_summer(void) {
    hal_nvs_write_u16("summer_min", 90);
    TEST_ASSERT_EQUAL_UINT32(90u * 60u, schedule_get_allocation_sec(DAY_SUMMER));
}

void test_allocation_summer_missing_key_falls_back(void) {
    /* setUp seeds no summer_min: compile-time default 120 min */
    TEST_ASSERT_EQUAL_UINT32(120u * 60u, schedule_get_allocation_sec(DAY_SUMMER));
}

/* ------------------------------------------------------------------ */
/* Malformed holiday blobs (NVS content is external input)              */
/* ------------------------------------------------------------------ */

void test_is_holiday_garbage_blob_no_match_no_crash(void) {
    /* Non-text bytes, a stray CR mid-line, and a date with a junk tail */
    char garbage[40];
    memset(garbage, 0xEE, sizeof(garbage));
    memcpy(garbage + 3, "garbage\rnot-a-date\n2026-01-01junk\n", 34);
    TEST_ASSERT_FALSE(schedule_is_holiday("2026-01-01", garbage, sizeof(garbage)));
}

void test_is_holiday_unterminated_blob_respects_length(void) {
    /* Exactly one date, no newline, no NUL inside the given length —
       the parser must never read past blob + blob_len. */
    char blob[10];
    memcpy(blob, "2026-01-01", 10);
    TEST_ASSERT_TRUE(schedule_is_holiday("2026-01-01", blob, sizeof(blob)));
    TEST_ASSERT_FALSE(schedule_is_holiday("2026-01-02", blob, sizeof(blob)));
}

void test_is_holiday_short_and_empty_lines_skipped(void) {
    const char blob[] = "2026\n\n01-01\n2026-07-04\n";
    TEST_ASSERT_TRUE(schedule_is_holiday("2026-07-04", blob, sizeof(blob) - 1));
    TEST_ASSERT_FALSE(schedule_is_holiday("2026-01-01", blob, sizeof(blob) - 1));
}

void test_is_holiday_empty_blob(void) {
    TEST_ASSERT_FALSE(schedule_is_holiday("2026-01-01", "", 0));
}

/* ------------------------------------------------------------------ */
/* Wake-scoped caching: repeated calls in one wake read NVS once        */
/* ------------------------------------------------------------------ */

void test_day_type_reads_holiday_blob_once(void) {
    for (int i = 0; i < 5; i++) {
        schedule_get_day_type(hal_time_now());
    }
    TEST_ASSERT_EQUAL_INT(1, mock_nvs_read_count("holidays"));
}

void test_day_type_reads_school_dates_once(void) {
    /* Mid-summer weekday so schedule_is_summer is consulted every call */
    mock_time_set(1784073600); /* 2026-07-15 Wed */
    for (int i = 0; i < 5; i++) {
        TEST_ASSERT_EQUAL(DAY_SUMMER, schedule_get_day_type(hal_time_now()));
    }
    TEST_ASSERT_EQUAL_INT(1, mock_nvs_read_count("summer_start"));
    TEST_ASSERT_EQUAL_INT(1, mock_nvs_read_count("school_start"));
    TEST_ASSERT_EQUAL_INT(1, mock_nvs_read_count("school_end"));
}

void test_allocation_reads_key_once_per_day_type(void) {
    for (int i = 0; i < 5; i++) {
        TEST_ASSERT_EQUAL_UINT32(60u * 60u, schedule_get_allocation_sec(DAY_WEEKDAY));
        TEST_ASSERT_EQUAL_UINT32(120u * 60u, schedule_get_allocation_sec(DAY_WEEKEND));
    }
    TEST_ASSERT_EQUAL_INT(1, mock_nvs_read_count("weekday_min"));
    TEST_ASSERT_EQUAL_INT(1, mock_nvs_read_count("weekend_min"));
}

void test_invalidate_forces_reread(void) {
    TEST_ASSERT_EQUAL_UINT32(60u * 60u, schedule_get_allocation_sec(DAY_WEEKDAY));
    hal_nvs_write_u16("weekday_min", 45);
    /* Cached: an NVS edit alone must not change the value mid-wake... */
    TEST_ASSERT_EQUAL_UINT32(60u * 60u, schedule_get_allocation_sec(DAY_WEEKDAY));
    /* ...until the orchestrator invalidates (post-network-window). */
    schedule_cache_invalidate();
    TEST_ASSERT_EQUAL_UINT32(45u * 60u, schedule_get_allocation_sec(DAY_WEEKDAY));
    TEST_ASSERT_EQUAL_INT(2, mock_nvs_read_count("weekday_min"));
}

void test_invalidate_rereads_holiday_blob(void) {
    mock_time_set(1767571200); /* 2026-01-05 Mon, not a holiday */
    TEST_ASSERT_EQUAL(DAY_WEEKDAY, schedule_get_day_type(hal_time_now()));
    const char *blob = "2026-01-05\n";
    hal_nvs_write_blob("holidays", blob, strlen(blob));
    TEST_ASSERT_EQUAL(DAY_WEEKDAY, schedule_get_day_type(hal_time_now()));
    schedule_cache_invalidate();
    TEST_ASSERT_EQUAL(DAY_HOLIDAY, schedule_get_day_type(hal_time_now()));
}

/* ------------------------------------------------------------------ */
/* schedule_get_chore_free_sec — the chore gate's free slice            */
/* ------------------------------------------------------------------ */

/* Headroom on every day type, so the belt clamp cannot mask a mis-wired
   key: with 1440 min allocated everywhere, a wrong answer below is a
   wrong KEY and not a clamp that happened to land on the right number. */
static void seed_roomy_allocations(void) {
    hal_nvs_write_u16("weekday_min", 1440);
    hal_nvs_write_u16("weekend_min", 1440);
    hal_nvs_write_u16("holiday_min", 1440);
    hal_nvs_write_u16("summer_min", 1440);
}

/* Four DISTINCT values, one per day type. The allocation lookup this
   function mirrors resolves the day type through a four-arm switch, and a
   copy-pasted arm that reads the weekday knob for the weekend is the
   failure mode; distinct values turn that into a wrong number instead of
   a coincidence. Key names are spelled literally here, as the allocation
   tests above spell theirs, so the NVS wire names are pinned too — a
   typo'd NVS_KEY_CHORE_FREE_* macro would read a key nothing wrote. */
static void seed_distinct_chore_free(void) {
    hal_nvs_write_u16("chore_free_wd", 10);
    hal_nvs_write_u16("chore_free_we", 20);
    hal_nvs_write_u16("chore_free_hol", 30);
    hal_nvs_write_u16("chore_free_sum", 40);
}

void test_chore_free_weekday_reads_its_own_key(void) {
    seed_roomy_allocations();
    seed_distinct_chore_free();
    TEST_ASSERT_EQUAL_UINT32(10u * 60u, schedule_get_chore_free_sec(DAY_WEEKDAY));
}

void test_chore_free_weekend_reads_its_own_key(void) {
    seed_roomy_allocations();
    seed_distinct_chore_free();
    TEST_ASSERT_EQUAL_UINT32(20u * 60u, schedule_get_chore_free_sec(DAY_WEEKEND));
}

void test_chore_free_holiday_reads_its_own_key(void) {
    seed_roomy_allocations();
    seed_distinct_chore_free();
    TEST_ASSERT_EQUAL_UINT32(30u * 60u, schedule_get_chore_free_sec(DAY_HOLIDAY));
}

void test_chore_free_summer_reads_its_own_key(void) {
    seed_roomy_allocations();
    seed_distinct_chore_free();
    TEST_ASSERT_EQUAL_UINT32(40u * 60u, schedule_get_chore_free_sec(DAY_SUMMER));
}

void test_chore_free_converts_minutes_to_seconds(void) {
    /* The keys are MINUTES like weekday_min; everything downstream
       (chores_withheld_sec) is seconds. 30 -> 1800, not 30. */
    seed_roomy_allocations();
    hal_nvs_write_u16("chore_free_wd", 30);
    TEST_ASSERT_EQUAL_UINT32(1800u, schedule_get_chore_free_sec(DAY_WEEKDAY));
}

void test_chore_free_absent_key_is_zero(void) {
    /* The state of every device in the field: the chore_free_* keys are
       deliberately not seeded (nvs_defaults.h), so they read as their
       compile-time default of 0 — fully gated, and inert while no chore
       is configured (design row C1). */
    mock_nvs_reset();
    TEST_ASSERT_EQUAL_UINT32(0u, schedule_get_chore_free_sec(DAY_WEEKDAY));
    TEST_ASSERT_EQUAL_UINT32(0u, schedule_get_chore_free_sec(DAY_WEEKEND));
    TEST_ASSERT_EQUAL_UINT32(0u, schedule_get_chore_free_sec(DAY_HOLIDAY));
    TEST_ASSERT_EQUAL_UINT32(0u, schedule_get_chore_free_sec(DAY_SUMMER));
}

void test_chore_free_clamped_to_allocation(void) {
    /* chore_free > allocation is a config error caught elsewhere; the
       clamp here is the belt that keeps the arithmetic downstream from
       underflowing into a ~136-year withholding. */
    hal_nvs_write_u16("weekday_min", 60);
    hal_nvs_write_u16("chore_free_wd", 90);
    uint32_t alloc = schedule_get_allocation_sec(DAY_WEEKDAY);
    uint32_t free_sec = schedule_get_chore_free_sec(DAY_WEEKDAY);
    TEST_ASSERT_EQUAL_UINT32(60u * 60u, free_sec);
    TEST_ASSERT_TRUE_MESSAGE(free_sec <= alloc, "clamp must never return more than the allocation");
    TEST_ASSERT_EQUAL_UINT32(0u, alloc - free_sec);
}

void test_chore_free_equal_to_allocation_is_the_off_switch(void) {
    /* One character from the clamp case above, and a different meaning:
       chore_free == allocation withholds nothing, which is the
       per-day-type off switch (design 3.3) and needs no extra key. */
    hal_nvs_write_u16("weekend_min", 120);
    hal_nvs_write_u16("chore_free_we", 120);
    TEST_ASSERT_EQUAL_UINT32(120u * 60u, schedule_get_chore_free_sec(DAY_WEEKEND));
}

void test_chore_free_clamped_when_allocation_is_zero(void) {
    /* A zero allocation is the worst case for the belt: every positive
       chore_free is larger than it. */
    hal_nvs_write_u16("weekday_min", 0);
    hal_nvs_write_u16("chore_free_wd", 30);
    TEST_ASSERT_EQUAL_UINT32(0u, schedule_get_chore_free_sec(DAY_WEEKDAY));
}

void test_chore_free_range_top_is_a_whole_day(void) {
    /* 1440 min is the top of the configured range; 1440 * 60 = 86400
       overflows a uint16_t, so the widening must happen before the
       multiply. */
    hal_nvs_write_u16("weekday_min", 1440);
    hal_nvs_write_u16("chore_free_wd", 1440);
    TEST_ASSERT_EQUAL_UINT32(86400u, schedule_get_chore_free_sec(DAY_WEEKDAY));
}

void test_chore_free_range_bottom_is_zero(void) {
    seed_roomy_allocations();
    hal_nvs_write_u16("chore_free_wd", 0);
    TEST_ASSERT_EQUAL_UINT32(0u, schedule_get_chore_free_sec(DAY_WEEKDAY));
}

void test_chore_free_reads_key_once_per_wake(void) {
    seed_roomy_allocations();
    seed_distinct_chore_free();
    for (int i = 0; i < 5; i++) {
        TEST_ASSERT_EQUAL_UINT32(10u * 60u, schedule_get_chore_free_sec(DAY_WEEKDAY));
    }
    TEST_ASSERT_EQUAL_INT(1, mock_nvs_read_count("chore_free_wd"));
}

/* ---- the raw pair, for the config-error gate (design 5.3) --------------

   The seconds accessor above CLAMPS, which is exactly right for every
   consumer that does arithmetic with it and exactly wrong for the one
   caller whose whole job is to notice the fault. A gate built on
   schedule_get_chore_free_sec() would find every pair valid, for ever,
   and the blocking screen would never paint — so this accessor exists to
   hand back what is STORED, in the unit config is stored in. */
void test_the_raw_pair_is_minutes_and_is_not_clamped(void) {
    hal_nvs_write_u16("weekday_min", 60);
    hal_nvs_write_u16("chore_free_wd", 90);

    uint16_t free_min = 0;
    uint16_t alloc_min = 0;
    schedule_get_chore_free_pair_min(DAY_WEEKDAY, &free_min, &alloc_min);

    /* MINUTES, not seconds: 90 and 60, never 5400 and 3600. The predicate
       that judges these (config_is_valid_chore_free_min) takes uint16_t,
       so a seconds value would truncate silently and invert the answer —
       config_validate.h works the arithmetic. */
    TEST_ASSERT_EQUAL_UINT16(90, free_min);
    TEST_ASSERT_EQUAL_UINT16(60, alloc_min);
    /* And the clamped view of the same pair, side by side, so the two
       cannot quietly become the same function. */
    TEST_ASSERT_EQUAL_UINT32(60u * 60u, schedule_get_chore_free_sec(DAY_WEEKDAY));
}

void test_the_raw_pair_uses_each_day_types_own_keys(void) {
    hal_nvs_write_u16("weekday_min", 11);
    hal_nvs_write_u16("weekend_min", 22);
    hal_nvs_write_u16("holiday_min", 33);
    hal_nvs_write_u16("summer_min", 44);
    hal_nvs_write_u16("chore_free_wd", 1);
    hal_nvs_write_u16("chore_free_we", 2);
    hal_nvs_write_u16("chore_free_hol", 3);
    hal_nvs_write_u16("chore_free_sum", 4);

    const day_type_t days[] = {DAY_WEEKDAY, DAY_WEEKEND, DAY_HOLIDAY, DAY_SUMMER};
    const uint16_t want_free[] = {1, 2, 3, 4};
    const uint16_t want_alloc[] = {11, 22, 33, 44};
    for (size_t i = 0; i < sizeof days / sizeof days[0]; i++) {
        uint16_t free_min = 0;
        uint16_t alloc_min = 0;
        schedule_get_chore_free_pair_min(days[i], &free_min, &alloc_min);
        TEST_ASSERT_EQUAL_UINT16(want_free[i], free_min);
        TEST_ASSERT_EQUAL_UINT16(want_alloc[i], alloc_min);
    }
}

/* Absent is the NORMAL state for the chore_free_* keys — they are
   deliberately outside the seeded-defaults registry — so the gate must
   see the compile-time default rather than garbage, and 0 against any
   allocation is valid. A gate that read uninitialised stack here would
   lock devices at random. */
void test_the_raw_pair_falls_back_to_the_same_defaults(void) {
    uint16_t free_min = 0xAAAA;
    uint16_t alloc_min = 0xAAAA;
    schedule_get_chore_free_pair_min(DAY_WEEKDAY, &free_min, &alloc_min);
    TEST_ASSERT_EQUAL_UINT16(NVS_DEFAULT_CHORE_FREE_WD, free_min);
    TEST_ASSERT_EQUAL_UINT16(NVS_DEFAULT_WEEKDAY_MIN, alloc_min);
}

/* One cache, so the fix an HA edit lands mid-wake is visible to the gate
   the moment the orchestrator invalidates — and so the gate costs no
   extra flash reads on the wakes where nothing is wrong. */
void test_the_raw_pair_shares_the_wake_cache_with_the_seconds_accessors(void) {
    seed_roomy_allocations();
    hal_nvs_write_u16("chore_free_wd", 10);

    uint16_t free_min = 0;
    uint16_t alloc_min = 0;
    for (int i = 0; i < 5; i++) {
        schedule_get_chore_free_pair_min(DAY_WEEKDAY, &free_min, &alloc_min);
    }
    TEST_ASSERT_EQUAL_UINT16(10, free_min);
    TEST_ASSERT_EQUAL_INT(1, mock_nvs_read_count("chore_free_wd"));

    hal_nvs_write_u16("chore_free_wd", 20);
    schedule_get_chore_free_pair_min(DAY_WEEKDAY, &free_min, &alloc_min);
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(10, free_min, "an NVS edit alone moved a wake-scoped read");

    schedule_cache_invalidate();
    schedule_get_chore_free_pair_min(DAY_WEEKDAY, &free_min, &alloc_min);
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(20, free_min, "the gate cannot see a fix that landed in the network window");
}

void test_chore_free_invalidate_forces_reread(void) {
    seed_roomy_allocations();
    hal_nvs_write_u16("chore_free_wd", 10);
    TEST_ASSERT_EQUAL_UINT32(10u * 60u, schedule_get_chore_free_sec(DAY_WEEKDAY));
    hal_nvs_write_u16("chore_free_wd", 20);
    /* Cached for the wake: an NVS edit alone must not move it... */
    TEST_ASSERT_EQUAL_UINT32(10u * 60u, schedule_get_chore_free_sec(DAY_WEEKDAY));
    /* ...until the orchestrator invalidates after the network window. A
       new cache field that schedule_cache_invalidate() forgets to clear
       is exactly what this catches. */
    schedule_cache_invalidate();
    TEST_ASSERT_EQUAL_UINT32(20u * 60u, schedule_get_chore_free_sec(DAY_WEEKDAY));
    TEST_ASSERT_EQUAL_INT(2, mock_nvs_read_count("chore_free_wd"));
}

/* ---- the broken-pair mask, for the stat payload's config warning -------
   (M2-D6). Every day type, judged on the RAW pair with the predicate the
   config-error lock uses, so a bit is set exactly when that day would
   lock. */

static const day_type_t ALL_DAYS[] = {DAY_WEEKDAY, DAY_WEEKEND, DAY_HOLIDAY, DAY_SUMMER};
static const char *const ALL_FREE_KEYS[] = {"chore_free_wd", "chore_free_we", "chore_free_hol", "chore_free_sum"};

/* The state of every device in the field: nothing configured, nothing
   broken. A warning here would be on every device. */
void test_broken_mask_is_clear_on_an_unconfigured_device(void) {
    mock_nvs_reset();
    TEST_ASSERT_EQUAL_UINT8(0, schedule_chore_free_broken_mask());
}

void test_broken_mask_is_clear_when_every_pair_is_valid(void) {
    seed_roomy_allocations();
    seed_distinct_chore_free();
    TEST_ASSERT_EQUAL_UINT8(0, schedule_chore_free_broken_mask());
}

/* Each day type on its own, so a check that skips one — or reads one day
   type's pair under another's bit — sets the wrong bit or none. */
void test_broken_mask_names_each_day_type_on_its_own(void) {
    for (size_t i = 0; i < sizeof(ALL_DAYS) / sizeof(ALL_DAYS[0]); i++) {
        mock_nvs_reset();
        schedule_cache_invalidate();
        seed_roomy_allocations(); /* 1440 everywhere */
        hal_nvs_write_u16("weekday_min", 100);
        hal_nvs_write_u16("weekend_min", 100);
        hal_nvs_write_u16("holiday_min", 100);
        hal_nvs_write_u16("summer_min", 100);
        hal_nvs_write_u16(ALL_FREE_KEYS[i], 101);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE((uint8_t)(1u << ALL_DAYS[i]), schedule_chore_free_broken_mask(),
                                        ALL_FREE_KEYS[i]);
    }
}

void test_broken_mask_names_every_broken_day_type_at_once(void) {
    hal_nvs_write_u16("weekday_min", 60);
    hal_nvs_write_u16("weekend_min", 60);
    hal_nvs_write_u16("holiday_min", 60);
    hal_nvs_write_u16("summer_min", 60);
    hal_nvs_write_u16("chore_free_wd", 61);
    hal_nvs_write_u16("chore_free_sum", 900);
    TEST_ASSERT_EQUAL_UINT8((1u << DAY_WEEKDAY) | (1u << DAY_SUMMER), schedule_chore_free_broken_mask());

    hal_nvs_write_u16("chore_free_we", 61);
    hal_nvs_write_u16("chore_free_hol", 61);
    schedule_cache_invalidate();
    TEST_ASSERT_EQUAL_UINT8(0x0F, schedule_chore_free_broken_mask());
}

/* THE reason the mask exists as its own function rather than a loop over
   the seconds accessor: that accessor clamps, and after the clamp this
   pair is 60/60 — valid. A check built on it would find every device
   healthy for ever. */
void test_broken_mask_reads_the_raw_pair_not_the_clamped_one(void) {
    hal_nvs_write_u16("weekday_min", 60);
    hal_nvs_write_u16("chore_free_wd", 90);
    TEST_ASSERT_EQUAL_UINT32(schedule_get_allocation_sec(DAY_WEEKDAY), schedule_get_chore_free_sec(DAY_WEEKDAY));
    TEST_ASSERT_EQUAL_UINT8(1u << DAY_WEEKDAY, schedule_chore_free_broken_mask());
}

/* One character from broken, and valid: chore_free == allocation is the
   per-day-type off switch. And a zero allocation under any positive free
   slice is the worst case the clamp hides. */
void test_broken_mask_equal_is_valid_and_zero_allocation_is_not(void) {
    hal_nvs_write_u16("weekend_min", 120);
    hal_nvs_write_u16("chore_free_we", 120);
    TEST_ASSERT_EQUAL_UINT8(0, schedule_chore_free_broken_mask());
    hal_nvs_write_u16("holiday_min", 0);
    hal_nvs_write_u16("chore_free_hol", 1);
    schedule_cache_invalidate();
    TEST_ASSERT_EQUAL_UINT8(1u << DAY_HOLIDAY, schedule_chore_free_broken_mask());
}

/* The mask agrees with the gate's own reader, pair for pair — the claim
   the header makes, that a bit is set exactly when that day would lock. */
void test_broken_mask_agrees_with_the_raw_pair_reader(void) {
    hal_nvs_write_u16("weekday_min", 10);
    hal_nvs_write_u16("weekend_min", 20);
    hal_nvs_write_u16("holiday_min", 30);
    hal_nvs_write_u16("summer_min", 40);
    hal_nvs_write_u16("chore_free_wd", 10);  /* equal: valid */
    hal_nvs_write_u16("chore_free_we", 21);  /* broken */
    hal_nvs_write_u16("chore_free_hol", 0);  /* valid */
    hal_nvs_write_u16("chore_free_sum", 41); /* broken */
    const uint8_t mask = schedule_chore_free_broken_mask();
    for (size_t i = 0; i < sizeof(ALL_DAYS) / sizeof(ALL_DAYS[0]); i++) {
        uint16_t free_min = 0;
        uint16_t alloc_min = 0;
        schedule_get_chore_free_pair_min(ALL_DAYS[i], &free_min, &alloc_min);
        TEST_ASSERT_EQUAL_MESSAGE(!config_is_valid_chore_free_min(free_min, alloc_min), (mask >> ALL_DAYS[i]) & 1u,
                                  ALL_FREE_KEYS[i]);
    }
    TEST_ASSERT_EQUAL_UINT8((1u << DAY_WEEKEND) | (1u << DAY_SUMMER), mask);
}

/* Same wake cache as the gate: a fix that lands in the network window is
   seen the moment the orchestrator invalidates, and not before. */
void test_broken_mask_clears_when_the_fix_is_seen(void) {
    hal_nvs_write_u16("summer_min", 60);
    hal_nvs_write_u16("chore_free_sum", 61);
    TEST_ASSERT_EQUAL_UINT8(1u << DAY_SUMMER, schedule_chore_free_broken_mask());
    hal_nvs_write_u16("chore_free_sum", 60);
    TEST_ASSERT_EQUAL_UINT8(1u << DAY_SUMMER, schedule_chore_free_broken_mask()); /* wake-scoped */
    schedule_cache_invalidate();
    TEST_ASSERT_EQUAL_UINT8(0, schedule_chore_free_broken_mask());
}

void test_day_type_names_are_the_ones_ha_already_shows(void) {
    TEST_ASSERT_EQUAL_STRING("Weekday", schedule_day_type_name(DAY_WEEKDAY));
    TEST_ASSERT_EQUAL_STRING("Weekend", schedule_day_type_name(DAY_WEEKEND));
    TEST_ASSERT_EQUAL_STRING("Holiday", schedule_day_type_name(DAY_HOLIDAY));
    TEST_ASSERT_EQUAL_STRING("Summer", schedule_day_type_name(DAY_SUMMER));
    TEST_ASSERT_EQUAL_STRING("Weekday", schedule_day_type_name((day_type_t)99)); /* out of range */
}

/* ------------------------------------------------------------------ */
/* Runner                                                               */
/* ------------------------------------------------------------------ */

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_is_holiday_match);
    RUN_TEST(test_is_holiday_no_match);
    RUN_TEST(test_is_holiday_partial_date_not_matched);
    RUN_TEST(test_is_holiday_windows_line_endings);
    RUN_TEST(test_is_holiday_trailing_newline);
    RUN_TEST(test_weekday_monday);
    RUN_TEST(test_weekday_friday);
    RUN_TEST(test_weekend_saturday);
    RUN_TEST(test_weekend_sunday);
    RUN_TEST(test_holiday_new_years_day);
    RUN_TEST(test_holiday_takes_priority_over_weekend);
    RUN_TEST(test_non_holiday_weekday_not_holiday);
    RUN_TEST(test_holiday_falls_back_to_compile_time_defaults);
    RUN_TEST(test_allocation_weekday);
    RUN_TEST(test_allocation_weekend);
    RUN_TEST(test_allocation_holiday);
    RUN_TEST(test_allocation_missing_key_falls_back_to_default);
    RUN_TEST(test_is_summer_boundaries);
    RUN_TEST(test_summer_weekday_is_summer);
    RUN_TEST(test_weekend_beats_summer);
    RUN_TEST(test_holiday_beats_summer);
    RUN_TEST(test_school_year_weekday_not_summer);
    RUN_TEST(test_allocation_summer);
    RUN_TEST(test_allocation_summer_missing_key_falls_back);
    RUN_TEST(test_is_holiday_garbage_blob_no_match_no_crash);
    RUN_TEST(test_is_holiday_unterminated_blob_respects_length);
    RUN_TEST(test_is_holiday_short_and_empty_lines_skipped);
    RUN_TEST(test_is_holiday_empty_blob);
    RUN_TEST(test_day_type_reads_holiday_blob_once);
    RUN_TEST(test_day_type_reads_school_dates_once);
    RUN_TEST(test_allocation_reads_key_once_per_day_type);
    RUN_TEST(test_invalidate_forces_reread);
    RUN_TEST(test_invalidate_rereads_holiday_blob);
    RUN_TEST(test_chore_free_weekday_reads_its_own_key);
    RUN_TEST(test_chore_free_weekend_reads_its_own_key);
    RUN_TEST(test_chore_free_holiday_reads_its_own_key);
    RUN_TEST(test_chore_free_summer_reads_its_own_key);
    RUN_TEST(test_chore_free_converts_minutes_to_seconds);
    RUN_TEST(test_chore_free_absent_key_is_zero);
    RUN_TEST(test_chore_free_clamped_to_allocation);
    RUN_TEST(test_chore_free_equal_to_allocation_is_the_off_switch);
    RUN_TEST(test_chore_free_clamped_when_allocation_is_zero);
    RUN_TEST(test_chore_free_range_top_is_a_whole_day);
    RUN_TEST(test_chore_free_range_bottom_is_zero);
    RUN_TEST(test_chore_free_reads_key_once_per_wake);
    RUN_TEST(test_chore_free_invalidate_forces_reread);
    RUN_TEST(test_the_raw_pair_is_minutes_and_is_not_clamped);
    RUN_TEST(test_the_raw_pair_uses_each_day_types_own_keys);
    RUN_TEST(test_the_raw_pair_falls_back_to_the_same_defaults);
    RUN_TEST(test_the_raw_pair_shares_the_wake_cache_with_the_seconds_accessors);
    RUN_TEST(test_broken_mask_is_clear_on_an_unconfigured_device);
    RUN_TEST(test_broken_mask_is_clear_when_every_pair_is_valid);
    RUN_TEST(test_broken_mask_names_each_day_type_on_its_own);
    RUN_TEST(test_broken_mask_names_every_broken_day_type_at_once);
    RUN_TEST(test_broken_mask_reads_the_raw_pair_not_the_clamped_one);
    RUN_TEST(test_broken_mask_equal_is_valid_and_zero_allocation_is_not);
    RUN_TEST(test_broken_mask_agrees_with_the_raw_pair_reader);
    RUN_TEST(test_broken_mask_clears_when_the_fix_is_seen);
    RUN_TEST(test_day_type_names_are_the_ones_ha_already_shows);
    return UNITY_END();
}
