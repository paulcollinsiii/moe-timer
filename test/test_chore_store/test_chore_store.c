#include <string.h>
#include <unity.h>

/* Single-TU: the store under test on top of the *real* chores.c, with only
   the flash itself mocked. chores_reconcile() is not stubbed on purpose —
   the C10 rule (a list edit clears the acks but keeps the release) is the
   single most important thing this file asserts, and asserting it against
   a restatement of the rule would prove nothing about the firmware. What
   is mocked is the one thing a host cannot have: the flash. mock_hal_nvs
   counts reads AND writes per key, which is what turns "a toggle writes
   unconditionally, and does NOT read the record back first" into an
   assertion about flash traffic rather than about a resulting value.

   Nothing here reads a clock. Every date is a literal, because the store's
   whole date rule takes `today` as an argument. */
// clang-format off
#include "../../main/chores.c"
#include "../../main/chore_store.c"
#include "mock_hal_nvs.c"
// clang-format on

#define TODAY "2026-09-11"
#define YESTERDAY "2026-09-10"
#define LAST_YEAR "2025-09-11"

/* Distinct, so a transposed field cannot pass unnoticed. */
#define HASH_A ((uint16_t)0xBEEF)
#define HASH_B ((uint16_t)0x1234)

void setUp(void) {
    mock_nvs_reset();
}

void tearDown(void) {}

/* ---- fixture ------------------------------------------------------------ */

static char g_names[CHORE_MAX][CHORE_NAME_BUF];
static uint8_t g_n;

static void clobber_out_params(void) {
    /* Poison the outputs before every load, so "the output is zeroed on
       every failure path" is a real assertion and not an artefact of a
       fresh static. */
    memset(g_names, 'X', sizeof(g_names));
    g_n = 0xFF;
}

static void assert_no_chores_configured(void) {
    TEST_ASSERT_EQUAL_UINT8(0, g_n);
    for (int i = 0; i < CHORE_MAX; i++) {
        TEST_ASSERT_EQUAL_CHAR('\0', g_names[i][0]);
    }
}

/* A well-formed names blob, built field by field rather than via the
   writer: the rejection tests below each spoil exactly one thing in it,
   and that is only meaningful if everything else is known-good. */
static nvs_chore_names_blob_t good_names_blob(void) {
    nvs_chore_names_blob_t b;
    memset(&b, 0, sizeof(b));
    b.version = CHORE_NAMES_BLOB_VERSION;
    b.n = 3;
    strcpy(b.names[0], "Dishes");
    strcpy(b.names[1], "Homework");
    strcpy(b.names[2], "Trash");
    return b;
}

static nvs_chore_ack_blob_t good_ack_blob(void) {
    nvs_chore_ack_blob_t r;
    memset(&r, 0, sizeof(r));
    r.version = CHORE_ACK_BLOB_VERSION;
    memcpy(r.date, TODAY, CHORE_DATE_LEN);
    r.list_hash = HASH_A;
    r.acked = 0x05;
    r.released = 1;
    return r;
}

static void put_raw(const char *key, const void *data, size_t len) {
    TEST_ASSERT_EQUAL(ESP_OK, hal_nvs_write_blob(key, data, len));
}

/* ---- the names blob: round trip ---------------------------------------- */

static void test_a_saved_name_list_reads_back_row_for_row(void) {
    const char src[CHORE_MAX][CHORE_NAME_BUF] = {"Dishes", "Homework", "Trash"};
    TEST_ASSERT_EQUAL(ESP_OK, chore_store_save_names(src, 3));
    clobber_out_params();
    TEST_ASSERT_EQUAL(ESP_OK, chore_store_load_names(g_names, &g_n));
    TEST_ASSERT_EQUAL_UINT8(3, g_n);
    TEST_ASSERT_EQUAL_STRING("Dishes", g_names[0]);
    TEST_ASSERT_EQUAL_STRING("Homework", g_names[1]);
    TEST_ASSERT_EQUAL_STRING("Trash", g_names[2]);
}

/* C1: the default every device in the field is in today. An empty list is
   a CONFIGURED state, not a missing one, so it round-trips as ESP_OK. */
static void test_an_empty_list_round_trips_as_a_configured_empty_list(void) {
    TEST_ASSERT_EQUAL(ESP_OK, chore_store_save_names(NULL, 0));
    clobber_out_params();
    TEST_ASSERT_EQUAL(ESP_OK, chore_store_load_names(g_names, &g_n));
    assert_no_chores_configured();
}

static void test_rows_above_the_count_are_stored_empty(void) {
    const char src[CHORE_MAX][CHORE_NAME_BUF] = {"Dishes", "leftover", "junk"};
    TEST_ASSERT_EQUAL(ESP_OK, chore_store_save_names(src, 1));
    nvs_chore_names_blob_t stored;
    size_t len = sizeof(stored);
    TEST_ASSERT_EQUAL(ESP_OK, hal_nvs_read_blob(NVS_KEY_CHORES, &stored, &len));
    TEST_ASSERT_EQUAL_UINT8(1, stored.n);
    TEST_ASSERT_EQUAL_STRING("Dishes", stored.names[0]);
    TEST_ASSERT_EQUAL_CHAR('\0', stored.names[1][0]);
    TEST_ASSERT_EQUAL_CHAR('\0', stored.names[2][0]);
}

/* The reserve bytes are what the "add a field without bumping the version"
   rule in chore_store.h rests on. They are only trustworthy if the writer
   memsets, so assert the bytes, not the intention. */
static void test_the_names_writer_zeroes_the_version_header_reserve(void) {
    const char src[CHORE_MAX][CHORE_NAME_BUF] = {"Dishes", "", ""};
    TEST_ASSERT_EQUAL(ESP_OK, chore_store_save_names(src, 1));
    nvs_chore_names_blob_t stored;
    size_t len = sizeof(stored);
    TEST_ASSERT_EQUAL(ESP_OK, hal_nvs_read_blob(NVS_KEY_CHORES, &stored, &len));
    TEST_ASSERT_EQUAL_UINT(sizeof(stored), len);
    TEST_ASSERT_EQUAL_UINT8(CHORE_NAMES_BLOB_VERSION, stored.version);
    TEST_ASSERT_EQUAL_UINT8(0, stored.rsvd[0]);
    TEST_ASSERT_EQUAL_UINT8(0, stored.rsvd[1]);
    /* And the tail of a short name, so a longer previous name cannot show
       through a partially-overwritten row. */
    for (size_t i = strlen("Dishes"); i < CHORE_NAME_BUF; i++) {
        TEST_ASSERT_EQUAL_CHAR('\0', stored.names[0][i]);
    }
}

static void test_the_name_list_uses_the_one_nvs_key(void) {
    const char src[CHORE_MAX][CHORE_NAME_BUF] = {"Dishes", "", ""};
    TEST_ASSERT_EQUAL(ESP_OK, chore_store_save_names(src, 1));
    TEST_ASSERT_EQUAL_INT(1, mock_nvs_write_count(NVS_KEY_CHORES));
    TEST_ASSERT_EQUAL_INT(0, mock_nvs_write_count(NVS_KEY_CHORE_ACK));
}

/* ---- the names blob: rejection ----------------------------------------- */

/* First boot, and every device in the field today. */
static void test_an_absent_name_list_reads_as_no_chores_configured(void) {
    clobber_out_params();
    TEST_ASSERT_EQUAL(ESP_ERR_NVS_NOT_FOUND, chore_store_load_names(g_names, &g_n));
    assert_no_chores_configured();
}

static void test_a_name_blob_from_another_version_is_rejected(void) {
    nvs_chore_names_blob_t b = good_names_blob();
    b.version = CHORE_NAMES_BLOB_VERSION + 1;
    put_raw(NVS_KEY_CHORES, &b, sizeof(b));
    clobber_out_params();
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_VERSION, chore_store_load_names(g_names, &g_n));
    assert_no_chores_configured();
}

/* Short: the bytes that ARE there look perfectly valid, which is exactly
   why a length check and not a version check has to catch this. */
static void test_a_short_name_blob_is_rejected_rather_than_partly_consumed(void) {
    nvs_chore_names_blob_t b = good_names_blob();
    put_raw(NVS_KEY_CHORES, &b, sizeof(b) - 1);
    clobber_out_params();
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_VERSION, chore_store_load_names(g_names, &g_n));
    assert_no_chores_configured();
}

static void test_an_oversized_name_blob_is_rejected(void) {
    uint8_t raw[sizeof(nvs_chore_names_blob_t) + 1];
    nvs_chore_names_blob_t b = good_names_blob();
    memset(raw, 0, sizeof(raw));
    memcpy(raw, &b, sizeof(b));
    put_raw(NVS_KEY_CHORES, raw, sizeof(raw));
    clobber_out_params();
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_VERSION, chore_store_load_names(g_names, &g_n));
    assert_no_chores_configured();
}

/* A count the hardware cannot have. Every ack bit in chores.c is bounded
   by this number, so a corrupt one must not escape the loader. */
static void test_a_name_blob_claiming_more_chores_than_exist_is_rejected(void) {
    nvs_chore_names_blob_t b = good_names_blob();
    b.n = CHORE_MAX + 1;
    put_raw(NVS_KEY_CHORES, &b, sizeof(b));
    clobber_out_params();
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_VERSION, chore_store_load_names(g_names, &g_n));
    assert_no_chores_configured();
}

static void test_saving_more_names_than_exist_is_refused_and_writes_nothing(void) {
    const char src[CHORE_MAX][CHORE_NAME_BUF] = {"Dishes", "Homework", "Trash"};
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, chore_store_save_names(src, CHORE_MAX + 1));
    TEST_ASSERT_EQUAL_INT(0, mock_nvs_write_count(NVS_KEY_CHORES));
}

/* ---- the names blob: name rows ----------------------------------------- */

/* CHORE_NAME_MAX is the usable byte count, so all 20 must survive. */
static void test_a_full_width_name_survives_the_round_trip(void) {
    char src[CHORE_MAX][CHORE_NAME_BUF];
    memset(src, 0, sizeof(src));
    memset(src[0], 'W', CHORE_NAME_MAX);
    TEST_ASSERT_EQUAL(ESP_OK, chore_store_save_names(src, 1));
    clobber_out_params();
    TEST_ASSERT_EQUAL(ESP_OK, chore_store_load_names(g_names, &g_n));
    TEST_ASSERT_EQUAL_UINT8(1, g_n);
    TEST_ASSERT_EQUAL_UINT(CHORE_NAME_MAX, strlen(g_names[0]));
    TEST_ASSERT_EQUAL_STRING(src[0], g_names[0]);
}

/* A source row with no terminator at all: the writer must stop at
   CHORE_NAME_MAX, exactly as chores_list_hash() does, and must not read
   past the row (ASAN is watching). */
static void test_an_unterminated_source_row_is_stored_truncated(void) {
    char src[CHORE_MAX][CHORE_NAME_BUF];
    memset(src, 'W', sizeof(src));
    TEST_ASSERT_EQUAL(ESP_OK, chore_store_save_names(src, 1));
    clobber_out_params();
    TEST_ASSERT_EQUAL(ESP_OK, chore_store_load_names(g_names, &g_n));
    TEST_ASSERT_EQUAL_UINT(CHORE_NAME_MAX, strlen(g_names[0]));
}

/* The case that actually reaches the field: a row read back out of flash
   with no NUL in it. chores_list_hash() tolerates that by design, but any
   str* in a caller would run off the end, so the loader terminates. */
static void test_an_unterminated_stored_row_comes_back_terminated(void) {
    nvs_chore_names_blob_t b = good_names_blob();
    b.n = 1;
    memset(b.names[0], 'W', CHORE_NAME_BUF); /* all 21 bytes, no NUL */
    put_raw(NVS_KEY_CHORES, &b, sizeof(b));
    clobber_out_params();
    TEST_ASSERT_EQUAL(ESP_OK, chore_store_load_names(g_names, &g_n));
    TEST_ASSERT_EQUAL_UINT8(1, g_n);
    TEST_ASSERT_EQUAL_UINT(CHORE_NAME_MAX, strlen(g_names[0]));
}

/* A configured-but-empty name is representable and must not collapse the
   count: "3 chores, the middle one unnamed" is not "1 chore". */
static void test_an_empty_name_row_does_not_shrink_the_count(void) {
    const char src[CHORE_MAX][CHORE_NAME_BUF] = {"Dishes", "", "Trash"};
    TEST_ASSERT_EQUAL(ESP_OK, chore_store_save_names(src, 3));
    clobber_out_params();
    TEST_ASSERT_EQUAL(ESP_OK, chore_store_load_names(g_names, &g_n));
    TEST_ASSERT_EQUAL_UINT8(3, g_n);
    TEST_ASSERT_EQUAL_STRING("", g_names[1]);
    TEST_ASSERT_EQUAL_STRING("Trash", g_names[2]);
}

/* ---- the ack record: round trip ---------------------------------------- */

static void test_todays_acks_round_trip_unchanged(void) {
    const chore_ack_t saved = {.acked = 0x05, .released = false};
    TEST_ASSERT_EQUAL(ESP_OK, chore_store_save_ack(TODAY, HASH_A, saved));
    chore_ack_t got = {.acked = 0xFF, .released = true};
    TEST_ASSERT_EQUAL(ESP_OK, chore_store_load_ack(TODAY, HASH_A, &got));
    TEST_ASSERT_EQUAL_UINT8(0x05, got.acked);
    TEST_ASSERT_FALSE(got.released);
}

/* What this actually demonstrates: `released` is CARRIED IN THE PERSISTED
   RECORD, not derived and not held in RAM — which is the property C14
   needs, since an OTA reboot zeroes every RTC variable. It is NOT evidence
   of the C14 path itself: nothing here reboots, and the reboot hazard
   lives in the CALLER (a post-restart `today` of "" — see WHERE `today`
   MUST COME FROM in chore_store.h), not in this round trip. */
static void test_a_released_day_is_carried_in_the_persisted_record(void) {
    const chore_ack_t saved = {.acked = 0x07, .released = true};
    TEST_ASSERT_EQUAL(ESP_OK, chore_store_save_ack(TODAY, HASH_A, saved));
    chore_ack_t got = {.acked = 0, .released = false};
    TEST_ASSERT_EQUAL(ESP_OK, chore_store_load_ack(TODAY, HASH_A, &got));
    TEST_ASSERT_EQUAL_UINT8(0x07, got.acked);
    TEST_ASSERT_TRUE(got.released);
}

/* released without any acks is reachable: the list can be edited after the
   release, and C10 keeps the flag while clearing the mask. It has to
   survive a round trip in that shape too. */
static void test_a_released_day_with_no_acks_round_trips(void) {
    const chore_ack_t saved = {.acked = 0x00, .released = true};
    TEST_ASSERT_EQUAL(ESP_OK, chore_store_save_ack(TODAY, HASH_A, saved));
    chore_ack_t got = {.acked = 0xFF, .released = false};
    TEST_ASSERT_EQUAL(ESP_OK, chore_store_load_ack(TODAY, HASH_A, &got));
    TEST_ASSERT_EQUAL_UINT8(0x00, got.acked);
    TEST_ASSERT_TRUE(got.released);
}

/* The design decision, asserted as flash traffic: toggles are a handful of
   writes a day, so this record takes NONE of timer_persist's
   write-only-when-changed care. Two identical saves are two writes, and
   neither reads the record back to compare. */
static void test_every_save_writes_and_none_reads_the_record_back(void) {
    const chore_ack_t ack = {.acked = 0x01, .released = false};
    TEST_ASSERT_EQUAL(ESP_OK, chore_store_save_ack(TODAY, HASH_A, ack));
    TEST_ASSERT_EQUAL(ESP_OK, chore_store_save_ack(TODAY, HASH_A, ack));
    TEST_ASSERT_EQUAL_INT(2, mock_nvs_write_count(NVS_KEY_CHORE_ACK));
    TEST_ASSERT_EQUAL_INT(0, mock_nvs_read_count(NVS_KEY_CHORE_ACK));
}

/* The ack record carries no reserve and no padding, so all 16 of its bytes
   are named fields and the writer must set every one. The only byte no
   argument carries is the date's terminator, which comes from the memset —
   assert it directly, since the loader's strcmp depends on it. The width
   is asserted here too: it is the on-flash size, and a silent change to it
   invalidates every record in the field. */
static void test_the_ack_writer_fills_every_byte_of_the_record(void) {
    const chore_ack_t ack = {.acked = 0x03, .released = false};
    TEST_ASSERT_EQUAL(ESP_OK, chore_store_save_ack(TODAY, HASH_B, ack));
    nvs_chore_ack_blob_t stored;
    size_t len = sizeof(stored);
    TEST_ASSERT_EQUAL(ESP_OK, hal_nvs_read_blob(NVS_KEY_CHORE_ACK, &stored, &len));
    TEST_ASSERT_EQUAL_UINT(16, sizeof(stored));
    TEST_ASSERT_EQUAL_UINT(sizeof(stored), len);
    TEST_ASSERT_EQUAL_UINT8(CHORE_ACK_BLOB_VERSION, stored.version);
    TEST_ASSERT_EQUAL_STRING(TODAY, stored.date);
    TEST_ASSERT_EQUAL_CHAR('\0', stored.date[CHORE_DATE_LEN]);
    TEST_ASSERT_EQUAL_UINT16(HASH_B, stored.list_hash);
    TEST_ASSERT_EQUAL_UINT8(0x03, stored.acked);
    TEST_ASSERT_EQUAL_UINT8(0, stored.released);
}

static void test_the_ack_record_uses_the_one_nvs_key(void) {
    const chore_ack_t ack = {.acked = 0x01, .released = false};
    TEST_ASSERT_EQUAL(ESP_OK, chore_store_save_ack(TODAY, HASH_A, ack));
    TEST_ASSERT_EQUAL_INT(1, mock_nvs_write_count(NVS_KEY_CHORE_ACK));
    TEST_ASSERT_EQUAL_INT(0, mock_nvs_write_count(NVS_KEY_CHORES));
}

/* ---- the ack record: the date rule (C13) -------------------------------- */

static void test_an_absent_ack_record_reads_as_nothing_acked(void) {
    chore_ack_t got = {.acked = 0xFF, .released = true};
    TEST_ASSERT_EQUAL(ESP_ERR_NVS_NOT_FOUND, chore_store_load_ack(TODAY, HASH_A, &got));
    TEST_ASSERT_EQUAL_UINT8(0, got.acked);
    TEST_ASSERT_FALSE(got.released);
}

/* The asymmetry that matters: a DATE mismatch clears BOTH flags. A day
   that rolled over starts locked, whatever yesterday achieved. */
static void test_yesterdays_record_clears_the_acks_and_the_release(void) {
    const chore_ack_t saved = {.acked = 0x07, .released = true};
    TEST_ASSERT_EQUAL(ESP_OK, chore_store_save_ack(YESTERDAY, HASH_A, saved));
    chore_ack_t got = {.acked = 0xFF, .released = true};
    TEST_ASSERT_EQUAL(ESP_OK, chore_store_load_ack(TODAY, HASH_A, &got));
    TEST_ASSERT_EQUAL_UINT8(0, got.acked);
    TEST_ASSERT_FALSE(got.released);
}

/* Not just yesterday: any date that is not today. A record from a year ago
   is the same answer, and a wrong-way comparison (">" instead of "!=")
   would pass the yesterday case and fail this one. */
static void test_a_record_from_last_year_clears_the_acks_and_the_release(void) {
    const chore_ack_t saved = {.acked = 0x07, .released = true};
    TEST_ASSERT_EQUAL(ESP_OK, chore_store_save_ack(LAST_YEAR, HASH_A, saved));
    chore_ack_t got = {.acked = 0xFF, .released = true};
    TEST_ASSERT_EQUAL(ESP_OK, chore_store_load_ack(TODAY, HASH_A, &got));
    TEST_ASSERT_EQUAL_UINT8(0, got.acked);
    TEST_ASSERT_FALSE(got.released);
}

/* A future date is a mismatch too, not a "close enough". The clock can
   move backwards here: a device that woke before its first NTP sync has a
   1970 date, and one that synced badly can have a date past today. */
static void test_a_record_stamped_in_the_future_clears_both(void) {
    const chore_ack_t saved = {.acked = 0x07, .released = true};
    TEST_ASSERT_EQUAL(ESP_OK, chore_store_save_ack("2027-01-01", HASH_A, saved));
    chore_ack_t got = {.acked = 0xFF, .released = true};
    TEST_ASSERT_EQUAL(ESP_OK, chore_store_load_ack(TODAY, HASH_A, &got));
    TEST_ASSERT_EQUAL_UINT8(0, got.acked);
    TEST_ASSERT_FALSE(got.released);
}

/* The date comes straight back from flash, so it need not be terminated.
   It must not match, and reading it must not run off the field (ASAN).

   This pins the OUTCOME, not any particular guard: the loader carries no
   length check on the stored date, because `today` is terminated and
   strcmp therefore stops at or before index CHORE_DATE_LEN, inside the
   field. A version that reinstated a guard would pass this too. */
static void test_an_unterminated_stored_date_never_matches_today(void) {
    nvs_chore_ack_blob_t r = good_ack_blob();
    memset(r.date, '9', sizeof(r.date)); /* all 11 bytes, no NUL */
    put_raw(NVS_KEY_CHORE_ACK, &r, sizeof(r));
    chore_ack_t got = {.acked = 0xFF, .released = true};
    TEST_ASSERT_EQUAL(ESP_OK, chore_store_load_ack(TODAY, HASH_A, &got));
    TEST_ASSERT_EQUAL_UINT8(0, got.acked);
    TEST_ASSERT_FALSE(got.released);
}

/* A date whose first ten bytes are today's but which carries junk where
   the terminator belongs is NOT today: truncating the comparison at ten
   characters (strncmp, or a memcmp of CHORE_DATE_LEN) would accept a
   record this loader has no reason to trust. What this pins is that the
   eleventh byte is part of the comparison — it is strcmp against a
   terminated `today` that gives that for free, not a length guard.

   This fixture is also the over-read proof, which is why the loader needs
   no length check on the stored date. Every byte of the record from the
   date onward is non-zero here ('Z', then 0xBEEF, 0x05, 0x01), so a
   comparison that did not stop inside the field would run off the end of
   a 16-byte stack struct and ASAN would say so. It stops at index 10,
   against today's terminator. */
static void test_todays_date_with_a_corrupt_terminator_is_not_today(void) {
    nvs_chore_ack_blob_t r = good_ack_blob();
    r.date[CHORE_DATE_LEN] = 'Z';
    put_raw(NVS_KEY_CHORE_ACK, &r, sizeof(r));
    chore_ack_t got = {.acked = 0xFF, .released = true};
    TEST_ASSERT_EQUAL(ESP_OK, chore_store_load_ack(TODAY, HASH_A, &got));
    TEST_ASSERT_EQUAL_UINT8(0, got.acked);
    TEST_ASSERT_FALSE(got.released);
}

/* ---- the ack record: the hash rule (C10) -------------------------------- */

/* The other half of the asymmetry: a HASH mismatch clears the acks and
   KEEPS the release. A list edit must never re-lock a day that already
   released — that would be a retroactive claw-back nobody asked for. */
static void test_a_list_edit_clears_the_acks_but_keeps_the_release(void) {
    const chore_ack_t saved = {.acked = 0x07, .released = true};
    TEST_ASSERT_EQUAL(ESP_OK, chore_store_save_ack(TODAY, HASH_A, saved));
    chore_ack_t got = {.acked = 0xFF, .released = false};
    TEST_ASSERT_EQUAL(ESP_OK, chore_store_load_ack(TODAY, HASH_B, &got));
    TEST_ASSERT_EQUAL_UINT8(0, got.acked);
    TEST_ASSERT_TRUE(got.released);
}

static void test_a_list_edit_before_the_release_leaves_the_day_locked(void) {
    const chore_ack_t saved = {.acked = 0x03, .released = false};
    TEST_ASSERT_EQUAL(ESP_OK, chore_store_save_ack(TODAY, HASH_A, saved));
    chore_ack_t got = {.acked = 0xFF, .released = true};
    TEST_ASSERT_EQUAL(ESP_OK, chore_store_load_ack(TODAY, HASH_B, &got));
    TEST_ASSERT_EQUAL_UINT8(0, got.acked);
    TEST_ASSERT_FALSE(got.released);
}

/* Both rules at once, and the date wins. A record from another day whose
   hash still matches is not a partially-valid record; it is a different
   day, and the release flag goes with it. */
static void test_the_date_rule_wins_over_the_hash_rule(void) {
    const chore_ack_t saved = {.acked = 0x07, .released = true};
    TEST_ASSERT_EQUAL(ESP_OK, chore_store_save_ack(YESTERDAY, HASH_A, saved));
    chore_ack_t got = {.acked = 0xFF, .released = true};
    TEST_ASSERT_EQUAL(ESP_OK, chore_store_load_ack(TODAY, HASH_A, &got));
    TEST_ASSERT_EQUAL_UINT8(0, got.acked);
    TEST_ASSERT_FALSE(got.released);
}

/* End to end through the real hash function rather than through two magic
   numbers: rename a chore in the stored list and the acks must drop while
   the release stands. This is the path the firmware actually takes. */
static void test_renaming_a_chore_in_the_real_list_drops_the_acks(void) {
    const char before[CHORE_MAX][CHORE_NAME_BUF] = {"Dishes", "Homework", "Trash"};
    const char after[CHORE_MAX][CHORE_NAME_BUF] = {"Dishes", "Reading", "Trash"};
    const uint16_t h_before = chores_list_hash(before, 3);
    const uint16_t h_after = chores_list_hash(after, 3);
    TEST_ASSERT_NOT_EQUAL(h_before, h_after);

    const chore_ack_t saved = {.acked = 0x07, .released = true};
    TEST_ASSERT_EQUAL(ESP_OK, chore_store_save_ack(TODAY, h_before, saved));
    chore_ack_t got = {.acked = 0xFF, .released = false};
    TEST_ASSERT_EQUAL(ESP_OK, chore_store_load_ack(TODAY, h_after, &got));
    TEST_ASSERT_EQUAL_UINT8(0, got.acked);
    TEST_ASSERT_TRUE(got.released);
}

/* The mirror of the test above: an UNCHANGED list must not disturb
   anything. Without this, a loader that cleared the mask unconditionally
   would pass every C10 test in this file. */
static void test_an_unchanged_real_list_keeps_the_acks(void) {
    const char names[CHORE_MAX][CHORE_NAME_BUF] = {"Dishes", "Homework", "Trash"};
    const uint16_t h = chores_list_hash(names, 3);
    const chore_ack_t saved = {.acked = 0x05, .released = false};
    TEST_ASSERT_EQUAL(ESP_OK, chore_store_save_ack(TODAY, h, saved));
    chore_ack_t got = {.acked = 0, .released = true};
    TEST_ASSERT_EQUAL(ESP_OK, chore_store_load_ack(TODAY, h, &got));
    TEST_ASSERT_EQUAL_UINT8(0x05, got.acked);
    TEST_ASSERT_FALSE(got.released);
}

/* ---- the ack record: rejection ------------------------------------------ */

static void test_an_ack_record_from_another_version_is_rejected(void) {
    nvs_chore_ack_blob_t r = good_ack_blob();
    r.version = CHORE_ACK_BLOB_VERSION + 1;
    put_raw(NVS_KEY_CHORE_ACK, &r, sizeof(r));
    chore_ack_t got = {.acked = 0xFF, .released = true};
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_VERSION, chore_store_load_ack(TODAY, HASH_A, &got));
    TEST_ASSERT_EQUAL_UINT8(0, got.acked);
    TEST_ASSERT_FALSE(got.released);
}

static void test_a_short_ack_record_is_rejected_rather_than_partly_consumed(void) {
    nvs_chore_ack_blob_t r = good_ack_blob();
    put_raw(NVS_KEY_CHORE_ACK, &r, sizeof(r) - 1);
    chore_ack_t got = {.acked = 0xFF, .released = true};
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_VERSION, chore_store_load_ack(TODAY, HASH_A, &got));
    TEST_ASSERT_EQUAL_UINT8(0, got.acked);
    TEST_ASSERT_FALSE(got.released);
}

static void test_an_oversized_ack_record_is_rejected(void) {
    uint8_t raw[sizeof(nvs_chore_ack_blob_t) + 1];
    nvs_chore_ack_blob_t r = good_ack_blob();
    memset(raw, 0, sizeof(raw));
    memcpy(raw, &r, sizeof(r));
    put_raw(NVS_KEY_CHORE_ACK, raw, sizeof(raw));
    chore_ack_t got = {.acked = 0xFF, .released = true};
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_VERSION, chore_store_load_ack(TODAY, HASH_A, &got));
    TEST_ASSERT_EQUAL_UINT8(0, got.acked);
    TEST_ASSERT_FALSE(got.released);
}

/* A date argument of the wrong length is a caller bug, and stamping it
   would produce a record that either never matches again or matches the
   wrong day. Refuse it, and write nothing. */
static void test_saving_with_a_short_date_is_refused_and_writes_nothing(void) {
    const chore_ack_t ack = {.acked = 0x01, .released = false};
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, chore_store_save_ack("2026-9-11", HASH_A, ack));
    TEST_ASSERT_EQUAL_INT(0, mock_nvs_write_count(NVS_KEY_CHORE_ACK));
}

static void test_saving_with_an_overlong_date_is_refused_and_writes_nothing(void) {
    const chore_ack_t ack = {.acked = 0x01, .released = false};
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, chore_store_save_ack("2026-09-11T12:00:00", HASH_A, ack));
    TEST_ASSERT_EQUAL_INT(0, mock_nvs_write_count(NVS_KEY_CHORE_ACK));
}

/* And the same check on the way in, so a malformed `today` cannot silently
   compare unequal against a perfectly good record and be read as a day
   rollover. It clears the OUTPUT and returns an error — which is a signal
   to defer, not a safe default: against C14 a lost day of acks is the
   harm, not the safe direction. What makes the cleared output survivable
   is that the RECORD is untouched, asserted next. */
static void test_loading_with_a_malformed_today_is_refused_and_clears(void) {
    const chore_ack_t saved = {.acked = 0x07, .released = true};
    TEST_ASSERT_EQUAL(ESP_OK, chore_store_save_ack(TODAY, HASH_A, saved));
    chore_ack_t got = {.acked = 0xFF, .released = true};
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, chore_store_load_ack("", HASH_A, &got));
    TEST_ASSERT_EQUAL_UINT8(0, got.acked);
    TEST_ASSERT_FALSE(got.released);
}

/* A refused load is refused BEFORE the read and writes nothing, which is
   the whole reason the header can tell a caller to defer on
   ESP_ERR_INVALID_SIZE: the record is still there to load again. The
   cleared output above is only dangerous if a caller saves on top of it —
   this asserts the half the module is responsible for. */
static void test_a_refused_load_leaves_the_stored_record_intact(void) {
    const chore_ack_t saved = {.acked = 0x07, .released = true};
    TEST_ASSERT_EQUAL(ESP_OK, chore_store_save_ack(TODAY, HASH_A, saved));
    const int writes_before = mock_nvs_write_count(NVS_KEY_CHORE_ACK);

    chore_ack_t got = {.acked = 0xFF, .released = true};
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, chore_store_load_ack("", HASH_A, &got));
    TEST_ASSERT_EQUAL_INT(writes_before, mock_nvs_write_count(NVS_KEY_CHORE_ACK));

    /* And the record is unchanged, so the deferred retry gets the truth. */
    got.acked = 0;
    got.released = false;
    TEST_ASSERT_EQUAL(ESP_OK, chore_store_load_ack(TODAY, HASH_A, &got));
    TEST_ASSERT_EQUAL_UINT8(0x07, got.acked);
    TEST_ASSERT_TRUE(got.released);
}

/* ---- flash failures and erased flash ------------------------------------ */

/* A write that does not land must be reported, not swallowed: the caller
   is the only layer that can decide whether to retry or to carry on with
   a mask that is now RAM-only. */
static void test_a_failed_flash_write_is_reported_by_the_names_writer(void) {
    const char src[CHORE_MAX][CHORE_NAME_BUF] = {"Dishes", "Homework", "Trash"};
    mock_nvs_fail_writes(-1);
    TEST_ASSERT_EQUAL(ESP_FAIL, chore_store_save_names(src, 3));
    TEST_ASSERT_EQUAL_INT(1, mock_nvs_write_count(NVS_KEY_CHORES));
}

static void test_a_failed_flash_write_is_reported_by_the_ack_writer(void) {
    const chore_ack_t ack = {.acked = 0x01, .released = false};
    mock_nvs_fail_writes(-1);
    TEST_ASSERT_EQUAL(ESP_FAIL, chore_store_save_ack(TODAY, HASH_A, ack));
    TEST_ASSERT_EQUAL_INT(1, mock_nvs_write_count(NVS_KEY_CHORE_ACK));
}

/* Erased flash reads as all-ones, and a partially-erased record is the
   shape a power cut during a write can leave. Version 0xFF is not this
   firmware's, so it must be rejected outright rather than read as a day
   whose mask happens to be every bit set. */
static void test_an_erased_ack_record_is_rejected(void) {
    uint8_t raw[sizeof(nvs_chore_ack_blob_t)];
    memset(raw, 0xFF, sizeof(raw));
    put_raw(NVS_KEY_CHORE_ACK, raw, sizeof(raw));
    chore_ack_t got = {.acked = 0xFF, .released = true};
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_VERSION, chore_store_load_ack(TODAY, HASH_A, &got));
    TEST_ASSERT_EQUAL_UINT8(0, got.acked);
    TEST_ASSERT_FALSE(got.released);
}

/* The other degenerate shape, and the one a zeroed page gives: version 0
   is not CHORE_ACK_BLOB_VERSION either, so it is rejected on the version
   byte and never reaches the date comparison against an all-NUL date. */
static void test_an_all_zero_ack_record_is_rejected(void) {
    uint8_t raw[sizeof(nvs_chore_ack_blob_t)];
    memset(raw, 0, sizeof(raw));
    put_raw(NVS_KEY_CHORE_ACK, raw, sizeof(raw));
    chore_ack_t got = {.acked = 0xFF, .released = true};
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_VERSION, chore_store_load_ack(TODAY, HASH_A, &got));
    TEST_ASSERT_EQUAL_UINT8(0, got.acked);
    TEST_ASSERT_FALSE(got.released);
}

/* The same for the names blob, where the stakes are the count: an erased
   blob claims n = 0xFF, far past CHORE_MAX, and every ack bit in chores.c
   is bounded by that number. It must land on the inert C1 default. */
static void test_an_erased_name_blob_is_rejected(void) {
    uint8_t raw[sizeof(nvs_chore_names_blob_t)];
    memset(raw, 0xFF, sizeof(raw));
    put_raw(NVS_KEY_CHORES, raw, sizeof(raw));
    clobber_out_params();
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_VERSION, chore_store_load_names(g_names, &g_n));
    assert_no_chores_configured();
}

/* ---- what the mask does NOT promise ------------------------------------- */

/* The mask comes back EXACTLY as stored, with no masking to the configured
   count — a record written when more chores were configured returns its
   high bits. That is right at this layer: masking here would destroy acks
   that a restored list would make meaningful again, and chores.c bounds
   every bit it reads. The consequence for consumers is the assertion at
   the bottom: read the mask through chores_is_acked(), never bit by bit,
   or a renderer will draw ticks for chores that are not on the list. */
static void test_ack_bits_above_the_configured_count_come_back_unmasked(void) {
    const uint8_t n = 3;
    const chore_ack_t saved = {.acked = 0xFF, .released = false};
    TEST_ASSERT_EQUAL(ESP_OK, chore_store_save_ack(TODAY, HASH_A, saved));
    chore_ack_t got = {.acked = 0, .released = true};
    TEST_ASSERT_EQUAL(ESP_OK, chore_store_load_ack(TODAY, HASH_A, &got));
    TEST_ASSERT_EQUAL_UINT8(0xFF, got.acked);
    for (uint8_t i = 0; i < n; i++) {
        TEST_ASSERT_TRUE(chores_is_acked(got.acked, i, n));
    }
    TEST_ASSERT_FALSE(chores_is_acked(got.acked, n, n));
    TEST_ASSERT_FALSE(chores_is_acked(got.acked, (uint8_t)(n + 1), n));
}

/* The two records have independent lifetimes, so the ack record can outlive
   the list it was acked against — a factory-reset config, or a names blob
   this firmware rejects. The empty list still hashes to a defined value,
   so this is an ordinary C10 hash mismatch: the acks drop and the release
   stands. Nothing here has to special-case "no list". */
static void test_an_ack_record_outliving_the_name_list_keeps_the_release(void) {
    const chore_ack_t saved = {.acked = 0x07, .released = true};
    TEST_ASSERT_EQUAL(ESP_OK, chore_store_save_ack(TODAY, HASH_A, saved));

    /* No names key at all — the C1 default every unconfigured device is in. */
    clobber_out_params();
    TEST_ASSERT_EQUAL(ESP_ERR_NVS_NOT_FOUND, chore_store_load_names(g_names, &g_n));
    assert_no_chores_configured();
    const uint16_t empty_hash = chores_list_hash(g_names, g_n);
    TEST_ASSERT_NOT_EQUAL(HASH_A, empty_hash);

    chore_ack_t got = {.acked = 0xFF, .released = false};
    TEST_ASSERT_EQUAL(ESP_OK, chore_store_load_ack(TODAY, empty_hash, &got));
    TEST_ASSERT_EQUAL_UINT8(0, got.acked);
    TEST_ASSERT_TRUE(got.released);
}

/* ---- the two records are independent ------------------------------------ */

/* Different keys, different lifetimes: the names are document-owned and
   the acks are device state. Rewriting one must not disturb the other. */
static void test_saving_the_name_list_leaves_todays_acks_alone(void) {
    const chore_ack_t saved = {.acked = 0x05, .released = true};
    TEST_ASSERT_EQUAL(ESP_OK, chore_store_save_ack(TODAY, HASH_A, saved));
    const char src[CHORE_MAX][CHORE_NAME_BUF] = {"Dishes", "Homework", "Trash"};
    TEST_ASSERT_EQUAL(ESP_OK, chore_store_save_names(src, 3));
    chore_ack_t got = {.acked = 0, .released = false};
    TEST_ASSERT_EQUAL(ESP_OK, chore_store_load_ack(TODAY, HASH_A, &got));
    TEST_ASSERT_EQUAL_UINT8(0x05, got.acked);
    TEST_ASSERT_TRUE(got.released);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_a_saved_name_list_reads_back_row_for_row);
    RUN_TEST(test_an_empty_list_round_trips_as_a_configured_empty_list);
    RUN_TEST(test_rows_above_the_count_are_stored_empty);
    RUN_TEST(test_the_names_writer_zeroes_the_version_header_reserve);
    RUN_TEST(test_the_name_list_uses_the_one_nvs_key);

    RUN_TEST(test_an_absent_name_list_reads_as_no_chores_configured);
    RUN_TEST(test_a_name_blob_from_another_version_is_rejected);
    RUN_TEST(test_a_short_name_blob_is_rejected_rather_than_partly_consumed);
    RUN_TEST(test_an_oversized_name_blob_is_rejected);
    RUN_TEST(test_a_name_blob_claiming_more_chores_than_exist_is_rejected);
    RUN_TEST(test_saving_more_names_than_exist_is_refused_and_writes_nothing);

    RUN_TEST(test_a_full_width_name_survives_the_round_trip);
    RUN_TEST(test_an_unterminated_source_row_is_stored_truncated);
    RUN_TEST(test_an_unterminated_stored_row_comes_back_terminated);
    RUN_TEST(test_an_empty_name_row_does_not_shrink_the_count);

    RUN_TEST(test_todays_acks_round_trip_unchanged);
    RUN_TEST(test_a_released_day_is_carried_in_the_persisted_record);
    RUN_TEST(test_a_released_day_with_no_acks_round_trips);
    RUN_TEST(test_every_save_writes_and_none_reads_the_record_back);
    RUN_TEST(test_the_ack_writer_fills_every_byte_of_the_record);
    RUN_TEST(test_the_ack_record_uses_the_one_nvs_key);

    RUN_TEST(test_an_absent_ack_record_reads_as_nothing_acked);
    RUN_TEST(test_yesterdays_record_clears_the_acks_and_the_release);
    RUN_TEST(test_a_record_from_last_year_clears_the_acks_and_the_release);
    RUN_TEST(test_a_record_stamped_in_the_future_clears_both);
    RUN_TEST(test_an_unterminated_stored_date_never_matches_today);
    RUN_TEST(test_todays_date_with_a_corrupt_terminator_is_not_today);

    RUN_TEST(test_a_list_edit_clears_the_acks_but_keeps_the_release);
    RUN_TEST(test_a_list_edit_before_the_release_leaves_the_day_locked);
    RUN_TEST(test_the_date_rule_wins_over_the_hash_rule);
    RUN_TEST(test_renaming_a_chore_in_the_real_list_drops_the_acks);
    RUN_TEST(test_an_unchanged_real_list_keeps_the_acks);

    RUN_TEST(test_an_ack_record_from_another_version_is_rejected);
    RUN_TEST(test_a_short_ack_record_is_rejected_rather_than_partly_consumed);
    RUN_TEST(test_an_oversized_ack_record_is_rejected);
    RUN_TEST(test_saving_with_a_short_date_is_refused_and_writes_nothing);
    RUN_TEST(test_saving_with_an_overlong_date_is_refused_and_writes_nothing);
    RUN_TEST(test_loading_with_a_malformed_today_is_refused_and_clears);
    RUN_TEST(test_a_refused_load_leaves_the_stored_record_intact);

    RUN_TEST(test_a_failed_flash_write_is_reported_by_the_names_writer);
    RUN_TEST(test_a_failed_flash_write_is_reported_by_the_ack_writer);
    RUN_TEST(test_an_erased_ack_record_is_rejected);
    RUN_TEST(test_an_all_zero_ack_record_is_rejected);
    RUN_TEST(test_an_erased_name_blob_is_rejected);

    RUN_TEST(test_ack_bits_above_the_configured_count_come_back_unmasked);
    RUN_TEST(test_an_ack_record_outliving_the_name_list_keeps_the_release);

    RUN_TEST(test_saving_the_name_list_leaves_todays_acks_alone);
    return UNITY_END();
}
