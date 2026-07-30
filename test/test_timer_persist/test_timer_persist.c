#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unity.h>

/* Single-TU: the persistence policy under test on top of the *real* timer
   state machine and the *real* NVS config layer, with only the flash
   itself mocked. Stubbing timer_make_snapshot() or the nvs_config
   snapshot accessors would let the write-only-on-change diff pass against
   a snapshot shape the device never produces — and the diff is a memcmp
   over a struct with padding, so the shape is exactly what it depends on.
   mock_hal_nvs counts reads AND writes per key, which turns "one write
   per change, none otherwise" into an assertion about flash traffic
   rather than about the resulting value. */
// clang-format off
#include "../../main/timer.c"
#include "../../main/nvs_config.c"
#include "../../main/timer_persist.c"
#include "mock_hal_nvs.c"
// clang-format on

/* 2026-01-05 00:00:00 UTC (Monday) and noon the same local day. TZ is
   pinned to UTC0 per test: the snapshot's staleness test is a local-date
   string comparison, so an inherited zone would move "today". */
#define DAY0 ((time_t)1767571200)
#define NOON (DAY0 + 12 * 3600)
#define YESTERDAY_NOON (NOON - 86400)
#define TOMORROW_NOON (NOON + 86400)

/* Same shape as test_timer's table: slot 2 stays disabled so the
   defs-dependence test has a hole to fall into. */
static const timer_def_t TEST_DEFS[TIMER_SLOT_COUNT] = {
    {"Screen", 0, false, false},   /* slot 0: never break-eligible */
    {"Piano", 900, true, true},    /* break-eligible */
    {"", 0, false, false},         /* disabled */
    {"Laundry", 600, true, false}, /* feeds the balance */
    {"Violin", 900, false, true},  /* break-eligible */
};

#define SLOT_PIANO 1
#define SLOT_DISABLED 2
#define SLOT_LAUNDRY 3
#define SLOT_VIOLIN 4

void setUp(void) {
    setenv("TZ", "UTC0", 1);
    tzset();
    mock_nvs_reset();
    timer_set_defs(TEST_DEFS, TIMER_SLOT_COUNT);
    timer_reset();
}

void tearDown(void) {}

/* ---- fixture ------------------------------------------------------------ */

static int snap_writes(void) {
    return mock_nvs_write_count(NVS_KEY_TIMER_SNAP);
}

static int snap_reads(void) {
    return mock_nvs_read_count(NVS_KEY_TIMER_SNAP);
}

/* A legal mid-day state with a distinct value in every field the snapshot
   carries. Written straight into g_rtc_state rather than driven through
   the state machine on purpose: the round-trip test below is looking for
   a dropped or transposed field, and two fields that happen to hold the
   same value cannot catch a transposition. Every value here clears
   snapshot_valid() (allocation and bonus inside the 7-day horizon,
   remaining <= allocation, BREAK confined to slot 0, no expiry already in
   the past) so a refused restore always means a real defect, never an
   implausible fixture. */
static void arm_rich_state(time_t now) {
    timer_reset();
    timer_record_date(now);
    g_rtc_state.active_slot = SLOT_PIANO;
    g_rtc_state.break_interrupted_slot = SLOT_LAUNDRY;
    g_rtc_state.break_prev_state = TIMER_PAUSED;
    g_rtc_state.run_segment_slot = SLOT_PIANO;
    g_rtc_state.next_ntp_sync = (int64_t)now + 600;

    timer_slot_state_t *s0 = &g_rtc_state.slots[0];
    s0->state = TIMER_PAUSED;
    s0->remaining_at_pause = 1234;
    s0->allocation_sec = 3600;
    s0->expiry_wall_time = 0;
    s0->run_accum_sec = 777;
    s0->run_started_wall = (int64_t)now - 300;
    s0->break_expiry_wall = 0;
    s0->completions = 2;
    s0->bonus_sec = 111;
    s0->bonus_applied = 222;

    timer_slot_state_t *s1 = &g_rtc_state.slots[SLOT_PIANO];
    s1->state = TIMER_RUNNING;
    s1->remaining_at_pause = 0;
    s1->allocation_sec = 900;
    s1->expiry_wall_time = (int64_t)now + 600;
    s1->completions = 1;
    s1->bonus_sec = -60;
    s1->bonus_applied = -60;

    timer_slot_state_t *s3 = &g_rtc_state.slots[SLOT_LAUNDRY];
    s3->state = TIMER_EXPIRED;
    s3->remaining_at_pause = 0;
    s3->allocation_sec = 600;
    s3->completions = 3;
    s3->bonus_sec = 30;
    s3->bonus_applied = 30;

    timer_slot_state_t *s4 = &g_rtc_state.slots[SLOT_VIOLIN];
    s4->state = TIMER_IDLE;
    s4->remaining_at_pause = 450;
    s4->allocation_sec = 900;
    s4->completions = 0;
    s4->bonus_sec = 15;
    s4->bonus_applied = 0;
}

/* Simulate the RTC memory loss a panic, an EN reset or a power cycle
   causes: every RTC_DATA_ATTR byte comes back zeroed, which is precisely
   the "" last_date the restore keys off. */
static void wipe_rtc(void) {
    memset(&g_rtc_state, 0, sizeof(g_rtc_state));
}

/* ---- save: write only on change ----------------------------------------- */

void test_first_save_on_a_blank_store_writes_once(void) {
    arm_rich_state(NOON);
    timer_persist_save();
    TEST_ASSERT_EQUAL_INT(1, snap_writes());
    timer_snapshot_t stored;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_load_timer_snapshot(&stored));
    TEST_ASSERT_EQUAL_UINT8(TIMER_SNAPSHOT_VERSION, stored.version);
}

void test_saving_an_unchanged_state_writes_nothing(void) {
    /* The whole point of the cycle. enter_deep_sleep() saves on every
       wake and the fields are stable across routine RUNNING ticks, so a
       device that wakes on the tick cadence all day would otherwise burn
       a flash page per wake for a byte-identical blob. */
    arm_rich_state(NOON);
    timer_persist_save();
    timer_persist_save();
    timer_persist_save();
    TEST_ASSERT_EQUAL_INT(1, snap_writes());
}

void test_each_save_reads_the_stored_blob_exactly_once(void) {
    /* The diff is against flash, so a read is unavoidable — but one, not
       one per slot and not one per field. */
    arm_rich_state(NOON);
    timer_persist_save();
    TEST_ASSERT_EQUAL_INT(1, snap_reads());
    timer_persist_save();
    TEST_ASSERT_EQUAL_INT(2, snap_reads());
}

/* Every field timer_snapshot_t carries, mutated by the smallest step that
   is still a change: +1 for the numerics, one enum step for the states,
   one character for the date. A comparison that skipped a field, compared
   fewer bytes than the struct holds, or compared only slot 0 would let at
   least one of these rows through as "unchanged" and lose the state to
   the next reset. The date row is deliberately the last meaningful byte
   before the trailing NUL, so a memcmp with a short length fails here. */
#define SNAPSHOT_FIELD_CASES(X)                                                         \
    X(slot0_state, g_rtc_state.slots[0].state = TIMER_EXPIRED)                          \
    X(slot0_remaining_at_pause, g_rtc_state.slots[0].remaining_at_pause += 1)           \
    X(slot0_allocation_sec, g_rtc_state.slots[0].allocation_sec += 1)                   \
    X(slot0_expiry_wall_time, g_rtc_state.slots[0].expiry_wall_time += 1)               \
    X(slot0_run_accum_sec, g_rtc_state.slots[0].run_accum_sec += 1)                     \
    X(slot0_run_started_wall, g_rtc_state.slots[0].run_started_wall += 1)               \
    X(slot0_break_expiry_wall, g_rtc_state.slots[0].break_expiry_wall += 1)             \
    X(slot0_completions, g_rtc_state.slots[0].completions += 1)                         \
    X(slot0_bonus_sec, g_rtc_state.slots[0].bonus_sec += 1)                             \
    X(slot0_bonus_applied, g_rtc_state.slots[0].bonus_applied += 1)                     \
    X(slot1_state, g_rtc_state.slots[SLOT_PIANO].state = TIMER_PAUSED)                  \
    X(slot1_expiry_wall_time, g_rtc_state.slots[SLOT_PIANO].expiry_wall_time += 1)      \
    X(slot1_bonus_applied, g_rtc_state.slots[SLOT_PIANO].bonus_applied += 1)            \
    X(slot3_allocation_sec, g_rtc_state.slots[SLOT_LAUNDRY].allocation_sec += 1)        \
    X(slot3_completions, g_rtc_state.slots[SLOT_LAUNDRY].completions += 1)              \
    X(slot4_remaining_at_pause, g_rtc_state.slots[SLOT_VIOLIN].remaining_at_pause += 1) \
    X(slot4_bonus_sec, g_rtc_state.slots[SLOT_VIOLIN].bonus_sec += 1)                   \
    X(active_slot, g_rtc_state.active_slot = SLOT_LAUNDRY)                              \
    X(break_interrupted_slot, g_rtc_state.break_interrupted_slot = SLOT_VIOLIN)         \
    X(break_prev_state, g_rtc_state.break_prev_state = TIMER_RUNNING)                   \
    X(run_segment_slot, g_rtc_state.run_segment_slot = SLOT_VIOLIN)                     \
    X(date_last_digit, g_rtc_state.last_date[9] = '6')

#define DEFINE_FIELD_MUTATOR(name, expr) \
    static void mutate_##name(void) {    \
        expr;                            \
    }
SNAPSHOT_FIELD_CASES(DEFINE_FIELD_MUTATOR)

#define FIELD_MUTATOR_ROW(name, expr) {#name, mutate_##name},
static const struct {
    const char *name;
    void (*fn)(void);
} FIELD_MUTATORS[] = {SNAPSHOT_FIELD_CASES(FIELD_MUTATOR_ROW)};

void test_a_one_step_change_in_any_snapshot_field_forces_a_write(void) {
    for (size_t i = 0; i < sizeof(FIELD_MUTATORS) / sizeof(FIELD_MUTATORS[0]); i++) {
        mock_nvs_reset();
        arm_rich_state(NOON);
        timer_persist_save();
        TEST_ASSERT_EQUAL_INT(1, snap_writes());
        /* Control: the same state twice is still one write, so a row
           that "passes" by writing unconditionally cannot hide here. */
        timer_persist_save();
        TEST_ASSERT_EQUAL_INT_MESSAGE(1, snap_writes(), FIELD_MUTATORS[i].name);
        FIELD_MUTATORS[i].fn();
        timer_persist_save();
        TEST_ASSERT_EQUAL_INT_MESSAGE(2, snap_writes(), FIELD_MUTATORS[i].name);
        /* And the change actually reached flash, not just the counter. */
        timer_snapshot_t live, stored;
        timer_make_snapshot(&live);
        TEST_ASSERT_EQUAL(ESP_OK, nvs_config_load_timer_snapshot(&stored));
        TEST_ASSERT_EQUAL_MEMORY_MESSAGE(&live, &stored, sizeof(live), FIELD_MUTATORS[i].name);
    }
}

/* The field table above cannot actually pin the comparison's WIDTH, and
   the checksum is why: timer_snapshot_t carries an 8-bit XOR of the whole
   struct at offset 2, so it sits inside every prefix a truncated memcmp
   would still compare. Any one-field edit moves that byte too, so a
   comparison that stopped short of the field that really changed is
   caught anyway — by the digest, not by the width. Only two edits with
   equal XOR deltas cancel in the checksum, and that is the one way to
   make the width itself observable. The two below are placed past where
   a plausible truncation stops (sizeof of the slots array; offsetof of
   the trailing date). An 8-bit XOR is far too weak to be the thing
   standing between a state change and flash. */

void test_a_checksum_neutral_change_in_the_last_slot_forces_a_write(void) {
    /* bonus_applied 0 -> 0x0303 moves two bytes by the same delta. It is
       the last field of the last slot, so a memcmp sized with
       sizeof(snap.slots) — the member instead of the struct — stops
       immediately before it. */
    arm_rich_state(NOON);
    timer_persist_save();
    TEST_ASSERT_EQUAL_INT(1, snap_writes());

    timer_snapshot_t before, after;
    timer_make_snapshot(&before);
    g_rtc_state.slots[SLOT_VIOLIN].bonus_applied = 0x0303;
    timer_make_snapshot(&after);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(before.checksum, after.checksum, "fixture must be checksum-neutral");
    TEST_ASSERT_TRUE_MESSAGE(memcmp(&before, &after, sizeof(before)) != 0, "fixture must still differ");

    timer_persist_save();
    TEST_ASSERT_EQUAL_INT(2, snap_writes());
}

void test_a_checksum_neutral_change_of_the_date_forces_a_write(void) {
    /* January 5 -> October 5: the two digits swap, so the XOR is
       unchanged. date[] is the final member, past the end of both a
       sizeof(slots) and an offsetof(date) truncation — and a blob left
       stored under the wrong day is refused by the restore, which is the
       entire reason the date is carried. */
    arm_rich_state(NOON);
    timer_persist_save();
    TEST_ASSERT_EQUAL_INT(1, snap_writes());

    timer_snapshot_t before, after;
    timer_make_snapshot(&before);
    memcpy(g_rtc_state.last_date, "2026-10-05", sizeof(g_rtc_state.last_date));
    timer_make_snapshot(&after);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(before.checksum, after.checksum, "fixture must be checksum-neutral");
    TEST_ASSERT_TRUE_MESSAGE(memcmp(&before, &after, sizeof(before)) != 0, "fixture must still differ");

    timer_persist_save();
    TEST_ASSERT_EQUAL_INT(2, snap_writes());
}

void test_a_change_that_is_reverted_within_a_wake_still_settles_on_one_blob(void) {
    /* Pause then resume before sleeping: two writes went out, but the
       stored blob must match the live state, not the intermediate one. */
    arm_rich_state(NOON);
    timer_persist_save();
    g_rtc_state.slots[0].remaining_at_pause += 1;
    timer_persist_save();
    g_rtc_state.slots[0].remaining_at_pause -= 1;
    timer_persist_save();
    TEST_ASSERT_EQUAL_INT(3, snap_writes());
    timer_snapshot_t live, stored;
    timer_make_snapshot(&live);
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_load_timer_snapshot(&stored));
    TEST_ASSERT_EQUAL_MEMORY(&live, &stored, sizeof(live));
}

void test_snapshot_bytes_are_deterministic_for_identical_state(void) {
    /* The precondition the memcmp rests on. timer_snapshot_t has interior
       padding (a uint8 state ahead of int32/int64 fields) and trailing
       padding after date[11]; timer_make_snapshot() memsets before
       filling, so those bytes are zero on both sides of the diff. If that
       memset ever went away, two snapshots of one unchanged state would
       differ in the padding and the guard would write on every single
       wake — silently, with the value still correct. Poisoning the two
       buffers with different bytes first is what makes that visible. */
    arm_rich_state(NOON);
    timer_snapshot_t a, b;
    memset(&a, 0xAA, sizeof(a));
    memset(&b, 0x55, sizeof(b));
    timer_make_snapshot(&a);
    timer_make_snapshot(&b);
    TEST_ASSERT_EQUAL_MEMORY(&a, &b, sizeof(a));
    TEST_ASSERT_TRUE_MESSAGE(sizeof(timer_snapshot_t) > 6 + sizeof(timer_snapshot_slot_t) * TIMER_SLOT_COUNT + 11,
                             "struct has no padding; this test is vacuous");
}

void test_a_failed_write_is_retried_on_the_next_save(void) {
    /* The guard compares against flash, not against a "last saved" copy
       in RAM. If it cached what it believed it had written, a write that
       failed (full or worn-out page) would never be retried and the
       device would sleep with stale state persisted. */
    arm_rich_state(NOON);
    mock_nvs_fail_writes(1);
    timer_persist_save();
    TEST_ASSERT_EQUAL_INT(1, snap_writes());
    timer_snapshot_t stored;
    TEST_ASSERT_EQUAL(ESP_ERR_NVS_NOT_FOUND, nvs_config_load_timer_snapshot(&stored));

    timer_persist_save(); /* same state, still nothing stored */
    TEST_ASSERT_EQUAL_INT(2, snap_writes());
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_load_timer_snapshot(&stored));
}

void test_a_foreign_edit_of_the_stored_blob_is_corrected(void) {
    /* Same "compare to flash" property from the other direction: the
       stored blob drifts while the live state does not, and the next
       save must put it back. */
    arm_rich_state(NOON);
    timer_persist_save();
    timer_snapshot_t tampered;
    timer_make_snapshot(&tampered);
    tampered.slots[0].completions = 99;
    tampered.checksum = timer_snapshot_checksum(&tampered);
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_save_timer_snapshot(&tampered));

    timer_persist_save();
    timer_snapshot_t stored;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_load_timer_snapshot(&stored));
    TEST_ASSERT_EQUAL_UINT16(2, stored.slots[0].completions);
}

/* The two below reach the bytes past the date's last digit, and they are
   the only rows that can. Every other write-side case compares a snapshot
   the *device* built, and timer_make_snapshot() memsets first, so its NUL
   and its trailing padding are always zero — nothing on the RAM side of
   the diff can ever vary there. The other operand does not come from RAM:
   `stored` comes from flash, and nvs_config_load_timer_snapshot() gates on
   size and version only, never on the checksum (nvs_config.c). A blob with
   a plausible size and version but a corrupt tail therefore loads
   successfully and reaches the memcmp, which makes those trailing bytes a
   reachable difference rather than a theoretical one.

   Skipping the correction is not cosmetic. The tail stays wrong forever,
   because every later save re-reads the same blob and reaches the same
   verdict; and the next restore refuses it — timer_snapshot_checksum()
   XORs the whole struct, padding included, so the stale checksum fails
   snapshot_valid(), and a date[] left without its NUL fails the strcmp
   even when the checksum was recomputed. A refused restore is exactly the
   day-refund this module exists to prevent. */

void test_a_corrupt_nul_after_the_date_is_corrected(void) {
    arm_rich_state(NOON);
    timer_persist_save();
    timer_snapshot_t forged;
    timer_make_snapshot(&forged);
    forged.date[sizeof(forged.date) - 1] = 'X'; /* the terminator, not a digit */
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_save_timer_snapshot(&forged));

    const int before = snap_writes();
    timer_persist_save();
    TEST_ASSERT_EQUAL_INT(before + 1, snap_writes());
    timer_snapshot_t stored;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_load_timer_snapshot(&stored));
    TEST_ASSERT_EQUAL_INT8('\0', stored.date[sizeof(stored.date) - 1]);
}

void test_a_corrupt_byte_in_the_trailing_padding_is_corrected(void) {
    /* Same argument one step further out. Padding is not a field, so it
       is tempting to exclude it from the diff — but the checksum covers
       it, so a device that never corrects it can never restore again. */
    arm_rich_state(NOON);
    timer_persist_save();
    timer_snapshot_t forged;
    timer_make_snapshot(&forged);
    TEST_ASSERT_TRUE_MESSAGE(sizeof(forged) > offsetof(timer_snapshot_t, date) + sizeof(forged.date),
                             "struct has no trailing padding; this test is vacuous");
    ((uint8_t *)&forged)[sizeof(forged) - 1] = 0xA5;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_save_timer_snapshot(&forged));

    const int before = snap_writes();
    timer_persist_save();
    TEST_ASSERT_EQUAL_INT(before + 1, snap_writes());
    timer_snapshot_t stored;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_load_timer_snapshot(&stored));
    TEST_ASSERT_EQUAL_UINT8(0, ((const uint8_t *)&stored)[sizeof(stored) - 1]);
}

void test_a_short_stored_blob_is_overwritten(void) {
    /* A truncated blob (interrupted write, or an older/smaller layout)
       must still end with the live state stored. What this pins is the
       outcome — one write, correct content — and nothing more.

       It does NOT pin the `== ESP_OK` half of the guard, despite being
       the obvious place to look for it. Dropping that half still passes
       here, because the stub's second byte (0) already differs from the
       live active_slot (Piano), so the memcmp reports "changed" on its
       own and the load result never gets consulted. The gate's real job
       is to stop the diff running over the part of the caller's struct
       the short read never filled, and that is not black-box observable:
       the buffer is a local inside timer_persist_save(), and a
       comparison against indeterminate bytes is undefined behaviour, not
       a deterministic wrong answer a test could assert on. Reviewed by
       reading the guard, not by this test. */
    const uint8_t stub[4] = {TIMER_SNAPSHOT_VERSION, 0, 0, 0};
    TEST_ASSERT_EQUAL(ESP_OK, hal_nvs_write_blob(NVS_KEY_TIMER_SNAP, stub, sizeof(stub)));
    arm_rich_state(NOON);
    const int before = snap_writes();
    timer_persist_save();
    TEST_ASSERT_EQUAL_INT(before + 1, snap_writes());
    timer_snapshot_t stored;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_load_timer_snapshot(&stored));
    TEST_ASSERT_EQUAL_UINT16(2, stored.slots[0].completions);
}

void test_a_stale_layout_blob_is_overwritten(void) {
    /* Post-OTA: the blob is the right size but the previous version's
       layout, so the load returns ESP_ERR_INVALID_VERSION. What this
       pins is that such a blob ends up replaced by a current-version one.

       Like the short-blob case above, it does not isolate the load-result
       gate: the version byte the load rejected on is itself part of the
       compared struct, so the memcmp reaches the same verdict unaided.
       The two cases coincide here by construction and cannot be teased
       apart from outside. */
    arm_rich_state(NOON);
    timer_snapshot_t old;
    timer_make_snapshot(&old);
    old.version = TIMER_SNAPSHOT_VERSION - 1;
    old.checksum = timer_snapshot_checksum(&old);
    TEST_ASSERT_EQUAL(ESP_OK, hal_nvs_write_blob(NVS_KEY_TIMER_SNAP, &old, sizeof(old)));
    const int before = snap_writes();

    timer_persist_save();
    TEST_ASSERT_EQUAL_INT(before + 1, snap_writes());
    timer_snapshot_t stored;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_load_timer_snapshot(&stored));
    TEST_ASSERT_EQUAL_UINT8(TIMER_SNAPSHOT_VERSION, stored.version);
}

void test_save_uses_the_one_snapshot_key(void) {
    /* Pins the key: the restore path and every previously flashed
       firmware read "timer_snap", so renaming it silently orphans the
       stored state (docs/planning notes the orphaned-slot case). */
    arm_rich_state(NOON);
    timer_persist_save();
    TEST_ASSERT_EQUAL_INT(1, mock_nvs_write_count("timer_snap"));
}

/* ---- restore: beats a reset, loses to intact RTC ------------------------ */

void test_restore_is_refused_while_rtc_state_is_intact(void) {
    /* Normal deep-sleep wake: RTC memory survived, so the live state is
       newer than anything in flash by definition. */
    arm_rich_state(NOON);
    timer_persist_save();
    g_rtc_state.slots[0].remaining_at_pause = 42; /* live state moves on */
    const int reads = snap_reads();

    TEST_ASSERT_FALSE(timer_persist_try_restore(NOON));
    TEST_ASSERT_EQUAL_INT32(42, g_rtc_state.slots[0].remaining_at_pause);
    /* The RTC test comes first: reaching flash to discover the restore is
       unnecessary would put a blob read on every ordinary wake. */
    TEST_ASSERT_EQUAL_INT(reads, snap_reads());
}

void test_restore_is_refused_when_rtc_is_intact_but_stale(void) {
    /* The refusal condition is "last_date is non-empty", NOT "the stored
       date differs from today". Here RTC survived across midnight holding
       yesterday, and NVS holds a valid snapshot dated TODAY — the one
       arrangement where the two rules disagree. RTC wins: the caller
       (handle_day_rollover) then reaches timer_reset(), which is correct,
       because a genuine date change is exactly when the day SHOULD be
       refunded. Rewriting the guard as `if (!timer_is_new_day(now))` or
       `if (timer_current_date() == today)` fails here. */
    arm_rich_state(NOON);
    timer_persist_save(); /* snapshot dated today */
    arm_rich_state(YESTERDAY_NOON);
    g_rtc_state.slots[0].completions = 7; /* yesterday's leftovers */

    TEST_ASSERT_FALSE(timer_persist_try_restore(NOON));
    TEST_ASSERT_EQUAL_UINT16(7, g_rtc_state.slots[0].completions);
    TEST_ASSERT_TRUE_MESSAGE(timer_is_new_day(NOON), "fixture must look like a rollover");
}

void test_restore_is_accepted_after_an_rtc_wipe(void) {
    arm_rich_state(NOON);
    timer_persist_save();
    wipe_rtc();

    TEST_ASSERT_TRUE(timer_persist_try_restore(NOON));
    TEST_ASSERT_EQUAL_INT(TIMER_PAUSED, timer_slot_state(0));
    TEST_ASSERT_EQUAL_INT32(1234, g_rtc_state.slots[0].remaining_at_pause);
}

void test_restore_is_refused_when_nothing_is_stored(void) {
    /* First boot of a factory device: nothing to restore, and the state
       must be left alone for the caller's reset path. */
    wipe_rtc();
    TEST_ASSERT_FALSE(timer_persist_try_restore(NOON));
    TEST_ASSERT_EQUAL_STRING("", timer_current_date());
    TEST_ASSERT_EQUAL_INT(1, snap_reads()); /* it did look */
}

void test_restore_is_refused_for_yesterdays_snapshot(void) {
    /* The device was off overnight. Restoring would carry yesterday's
       spent allocation into today, which is the mirror-image failure of
       refunding it. */
    arm_rich_state(YESTERDAY_NOON);
    timer_persist_save();
    wipe_rtc();

    TEST_ASSERT_FALSE(timer_persist_try_restore(NOON));
    TEST_ASSERT_EQUAL_STRING("", timer_current_date());
    TEST_ASSERT_EQUAL_INT(TIMER_IDLE, timer_slot_state(0));
}

void test_restore_is_refused_for_a_snapshot_from_the_future(void) {
    /* Clock not yet corrected by NTP after a power cycle: the stored date
       is ahead of the (wrong) wall clock. Same date-mismatch rule, and
       the reason app_main calls the restore a second time after the
       rollover's sync. */
    arm_rich_state(TOMORROW_NOON);
    timer_persist_save();
    wipe_rtc();

    TEST_ASSERT_FALSE(timer_persist_try_restore(NOON));
    TEST_ASSERT_EQUAL_STRING("", timer_current_date());
}

void test_restore_is_refused_for_a_corrupt_snapshot(void) {
    /* Checksum failure must leave the state untouched, not half-applied:
       the caller treats false as "reset the day", which is a safe answer;
       a partially restored slot table is not. */
    arm_rich_state(NOON);
    timer_snapshot_t bad;
    timer_make_snapshot(&bad);
    bad.slots[0].completions = 55; /* checksum left stale on purpose */
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_save_timer_snapshot(&bad));
    wipe_rtc();

    TEST_ASSERT_FALSE(timer_persist_try_restore(NOON));
    TEST_ASSERT_EQUAL_UINT16(0, g_rtc_state.slots[0].completions);
    TEST_ASSERT_EQUAL_STRING("", timer_current_date());
}

void test_restore_never_writes_flash(void) {
    /* It runs on the boot path, before the display comes up; a write here
       would add flash latency to every cold boot and, worse, would make
       the boot path capable of destroying the very blob it is reading. */
    arm_rich_state(NOON);
    timer_persist_save();
    const int writes = snap_writes();
    wipe_rtc();
    TEST_ASSERT_TRUE(timer_persist_try_restore(NOON));
    TEST_ASSERT_EQUAL_INT(writes, snap_writes());
}

void test_restore_reads_the_blob_at_most_once(void) {
    arm_rich_state(NOON);
    timer_persist_save();
    const int reads = snap_reads();
    wipe_rtc();
    TEST_ASSERT_TRUE(timer_persist_try_restore(NOON));
    TEST_ASSERT_EQUAL_INT(reads + 1, snap_reads());
}

/* ---- the round trip ----------------------------------------------------- */

void test_round_trip_preserves_every_persisted_field(void) {
    /* Field by field rather than a struct memcmp: a memcmp would report
       "differs" without naming the field, and next_ntp_sync is
       deliberately absent from the snapshot (pinned separately below). */
    arm_rich_state(NOON);
    const rtc_state_t before = g_rtc_state;
    timer_persist_save();
    wipe_rtc();
    TEST_ASSERT_TRUE(timer_persist_try_restore(NOON));

    for (int i = 0; i < TIMER_SLOT_COUNT; i++) {
        char msg[32];
        snprintf(msg, sizeof(msg), "slot %d", i);
        const timer_slot_state_t *b = &before.slots[i];
        const timer_slot_state_t *a = &g_rtc_state.slots[i];
        TEST_ASSERT_EQUAL_INT_MESSAGE(b->state, a->state, msg);
        TEST_ASSERT_EQUAL_INT32_MESSAGE(b->remaining_at_pause, a->remaining_at_pause, msg);
        TEST_ASSERT_EQUAL_INT32_MESSAGE(b->allocation_sec, a->allocation_sec, msg);
        TEST_ASSERT_EQUAL_INT64_MESSAGE(b->expiry_wall_time, a->expiry_wall_time, msg);
        TEST_ASSERT_EQUAL_INT32_MESSAGE(b->run_accum_sec, a->run_accum_sec, msg);
        TEST_ASSERT_EQUAL_INT64_MESSAGE(b->run_started_wall, a->run_started_wall, msg);
        TEST_ASSERT_EQUAL_INT64_MESSAGE(b->break_expiry_wall, a->break_expiry_wall, msg);
        TEST_ASSERT_EQUAL_UINT16_MESSAGE(b->completions, a->completions, msg);
        TEST_ASSERT_EQUAL_INT32_MESSAGE(b->bonus_sec, a->bonus_sec, msg);
        TEST_ASSERT_EQUAL_INT32_MESSAGE(b->bonus_applied, a->bonus_applied, msg);
    }
    TEST_ASSERT_EQUAL_UINT8(before.active_slot, g_rtc_state.active_slot);
    TEST_ASSERT_EQUAL_UINT8(before.break_interrupted_slot, g_rtc_state.break_interrupted_slot);
    TEST_ASSERT_EQUAL_UINT8(before.break_prev_state, g_rtc_state.break_prev_state);
    TEST_ASSERT_EQUAL_UINT8(before.run_segment_slot, g_rtc_state.run_segment_slot);
    TEST_ASSERT_EQUAL_STRING(before.last_date, g_rtc_state.last_date);
}

void test_round_trip_does_not_carry_the_ntp_schedule(void) {
    /* timer_snapshot_t has no next_ntp_sync field, so a restored device
       reads as "sync due now". That is the conservative answer: the
       schedule was computed against the clock that just died. */
    arm_rich_state(NOON);
    TEST_ASSERT_FALSE(timer_needs_ntp_sync(NOON)); /* fixture armed it */
    timer_persist_save();
    wipe_rtc();
    TEST_ASSERT_TRUE(timer_persist_try_restore(NOON));
    TEST_ASSERT_EQUAL_INT64(0, g_rtc_state.next_ntp_sync);
    TEST_ASSERT_TRUE(timer_needs_ntp_sync(NOON));
}

void test_a_restored_state_saves_without_a_further_write(void) {
    /* End-to-end idempotence: boot restores, the wake changes nothing,
       enter_deep_sleep saves. A restore that dropped or widened a field
       would show up as a write here even though nothing happened. */
    arm_rich_state(NOON);
    timer_persist_save();
    wipe_rtc();
    TEST_ASSERT_TRUE(timer_persist_try_restore(NOON));
    const int writes = snap_writes();
    timer_persist_save();
    TEST_ASSERT_EQUAL_INT(writes, snap_writes());
}

/* ---- plan row 13: rollover vs. a same-day snapshot ---------------------- */

void test_day_rollover_with_a_same_day_snapshot_does_not_refund_the_allocation(void) {
    /* Plan row 13. Power cycle at midday: RTC memory is gone, so
       timer_is_new_day() reads "" as a new day and the wake handler's
       rollover fires — timer_reset() would hand back the whole day's
       screen allocation. This restore is the only thing standing in
       front of it: handle_day_rollover reaches timer_reset() exactly
       when this returns false. */
    arm_rich_state(NOON); /* 3600 s allocated, 1234 s left => 2366 s spent */
    const int32_t spent = timer_screen_used_sec(NOON);
    TEST_ASSERT_EQUAL_INT32(2366, spent);
    timer_persist_save();
    wipe_rtc();

    TEST_ASSERT_TRUE_MESSAGE(timer_is_new_day(NOON), "rollover must fire, or the test is vacuous");
    TEST_ASSERT_TRUE(timer_persist_try_restore(NOON));
    TEST_ASSERT_EQUAL_INT32_MESSAGE(spent, timer_screen_used_sec(NOON), "allocation was refunded");
    TEST_ASSERT_EQUAL_INT32(3600, g_rtc_state.slots[0].allocation_sec);
    TEST_ASSERT_EQUAL_UINT16(2, timer_slot_completions(0));
    /* And the day is now recorded, so the caller's own rollover check
       does not fire a second time and reset what was just restored. */
    TEST_ASSERT_FALSE(timer_is_new_day(NOON));
}

void test_day_rollover_without_a_snapshot_falls_through_to_the_reset(void) {
    /* The counterfactual for row 13: same wipe, empty NVS. The restore
       declines, the caller resets, and the day starts clean — which is
       the behaviour a genuine new day needs. */
    wipe_rtc();
    TEST_ASSERT_TRUE(timer_is_new_day(NOON));
    TEST_ASSERT_FALSE(timer_persist_try_restore(NOON));
    timer_reset();
    timer_record_date(NOON);
    TEST_ASSERT_EQUAL_INT32(0, timer_screen_used_sec(NOON));
    TEST_ASSERT_EQUAL_UINT16(0, timer_slot_completions(0));
}

/* ---- the timer_defs_install ordering invariant -------------------------- */

void test_restore_depends_on_the_defs_table_being_installed_first(void) {
    /* CLAUDE.md invariant: timer_defs_install() must run each boot before
       any timer_* call. This restore is the first timer_* call in
       app_main after that install, and the dependency is real, not
       stylistic: timer_restore_snapshot() ends in
       timer_ensure_active_slot_enabled(), which reads the defs table to
       decide whether the restored selection still exists. Run it with no
       table and every extra slot reads as disabled, so the selection is
       dragged to Screen — a quietly wrong device rather than a crash,
       which is why the ordering needs a test and not a comment. */
    arm_rich_state(NOON); /* selection on Piano */
    timer_persist_save();

    wipe_rtc();
    timer_set_defs(NULL, 0); /* as if timer_defs_install() had not run */
    TEST_ASSERT_TRUE(timer_persist_try_restore(NOON));
    TEST_ASSERT_EQUAL_INT(0, timer_active_slot());

    wipe_rtc();
    timer_set_defs(TEST_DEFS, TIMER_SLOT_COUNT);
    TEST_ASSERT_TRUE(timer_persist_try_restore(NOON));
    TEST_ASSERT_EQUAL_INT(SLOT_PIANO, timer_active_slot());
}

void test_restore_of_a_slot_the_firmware_no_longer_defines_lands_on_screen(void) {
    /* Reflash with a slot removed from menuconfig: the slot's state is
       still restored, only the selection moves, so nothing is refunded. */
    arm_rich_state(NOON);
    g_rtc_state.active_slot = SLOT_DISABLED;
    timer_persist_save();
    wipe_rtc();

    TEST_ASSERT_TRUE(timer_persist_try_restore(NOON));
    TEST_ASSERT_EQUAL_INT(0, timer_active_slot());
    TEST_ASSERT_EQUAL_INT(TIMER_RUNNING, timer_slot_state(SLOT_PIANO));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_first_save_on_a_blank_store_writes_once);
    RUN_TEST(test_saving_an_unchanged_state_writes_nothing);
    RUN_TEST(test_each_save_reads_the_stored_blob_exactly_once);
    RUN_TEST(test_a_one_step_change_in_any_snapshot_field_forces_a_write);
    RUN_TEST(test_a_checksum_neutral_change_in_the_last_slot_forces_a_write);
    RUN_TEST(test_a_checksum_neutral_change_of_the_date_forces_a_write);
    RUN_TEST(test_a_change_that_is_reverted_within_a_wake_still_settles_on_one_blob);
    RUN_TEST(test_snapshot_bytes_are_deterministic_for_identical_state);
    RUN_TEST(test_a_failed_write_is_retried_on_the_next_save);
    RUN_TEST(test_a_foreign_edit_of_the_stored_blob_is_corrected);
    RUN_TEST(test_a_corrupt_nul_after_the_date_is_corrected);
    RUN_TEST(test_a_corrupt_byte_in_the_trailing_padding_is_corrected);
    RUN_TEST(test_a_short_stored_blob_is_overwritten);
    RUN_TEST(test_a_stale_layout_blob_is_overwritten);
    RUN_TEST(test_save_uses_the_one_snapshot_key);
    RUN_TEST(test_restore_is_refused_while_rtc_state_is_intact);
    RUN_TEST(test_restore_is_refused_when_rtc_is_intact_but_stale);
    RUN_TEST(test_restore_is_accepted_after_an_rtc_wipe);
    RUN_TEST(test_restore_is_refused_when_nothing_is_stored);
    RUN_TEST(test_restore_is_refused_for_yesterdays_snapshot);
    RUN_TEST(test_restore_is_refused_for_a_snapshot_from_the_future);
    RUN_TEST(test_restore_is_refused_for_a_corrupt_snapshot);
    RUN_TEST(test_restore_never_writes_flash);
    RUN_TEST(test_restore_reads_the_blob_at_most_once);
    RUN_TEST(test_round_trip_preserves_every_persisted_field);
    RUN_TEST(test_round_trip_does_not_carry_the_ntp_schedule);
    RUN_TEST(test_a_restored_state_saves_without_a_further_write);
    RUN_TEST(test_day_rollover_with_a_same_day_snapshot_does_not_refund_the_allocation);
    RUN_TEST(test_day_rollover_without_a_snapshot_falls_through_to_the_reset);
    RUN_TEST(test_restore_depends_on_the_defs_table_being_installed_first);
    RUN_TEST(test_restore_of_a_slot_the_firmware_no_longer_defines_lands_on_screen);
    return UNITY_END();
}
