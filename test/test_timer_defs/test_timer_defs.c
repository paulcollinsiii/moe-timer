#include <stdio.h>
#include <string.h>
#include <unity.h>

/* BUG-8 regression: what boot leaves behind for the network window.

   Single-TU, and deliberately the REAL timer_defs_install() rather than a
   transcription of it — docs/planning/bug8.repro.c (commit ef3af99) had to
   copy the function because main/timer_defs.c would not compile for the
   host; it does now, and this is the only suite that compiles it. The other
   half of the defect is the REAL config_apply() over the mock NVS: the bug
   is not in either function alone, it is in the boot writing a table the
   window then mistakes for an operator's. */

/* The compile-time table this TU's timer_defs.c is built against. Defined
   ahead of the include because that file's #ifndef fallbacks exist for
   exactly this case: the host has no sdkconfig.h.

   BREAK_ELIGIBLE is 1 on both configured slots ON PURPOSE, and it is the
   one value here that is not arbitrary. With the shipping Kconfig (`n` on
   every slot) the defect is invisible in the final flag — an invented 0 and
   a first-time-definition 0 are the same 0 — so the spec's own repro can
   only show the stray NVS write, not a wrong value. Compile-time `y` makes
   the masquerade observable: persist the boot table and apply_timers() sees
   `existed`, so BUG-6's "absent means unchanged" rule hands the
   COMPILE-TIME flag back as though an operator had chosen it. */
#define CONFIG_MAGTAG_TIMER1_NAME "Piano"
#define CONFIG_MAGTAG_TIMER1_MIN 15
#define CONFIG_MAGTAG_TIMER1_RELOADABLE 1
#define CONFIG_MAGTAG_TIMER1_BREAK_ELIGIBLE 1
#define CONFIG_MAGTAG_TIMER2_NAME "Meditation"
#define CONFIG_MAGTAG_TIMER2_MIN 10
#define CONFIG_MAGTAG_TIMER2_RELOADABLE 0
#define CONFIG_MAGTAG_TIMER2_BREAK_ELIGIBLE 1
/* Slots 3 and 4 keep the empty-name default: disabled, as they ship. */

// clang-format off
#include "cJSON.h"
#include "mock_hal_nvs.c"
#include "mock_hal_time.c"
#include "../../main/timer.c"
#include "../../main/nvs_config.c"
#include "../../main/quiet_hours.c"
#include "../../main/bedtime.c"
#include "../../main/config_validate.c"
#include "../../main/tones.c"
#include "../../main/config_apply.c"
#include "../../main/timer_defs.c"
// clang-format on

#define SLOT_PIANO 1
#define SLOT_MEDITATION 2

/* The retained HA config document as it exists on a broker set up before
   the docs gained `break`: documentation-shaped, no `break` key. This is
   the document the reporter's device was actually being handed. */
static const char *DOC_NO_BREAK =
    "{\"ver\":\"20260708\",\"timers\":["
    "{\"name\":\"Piano\",\"min\":15,\"reload\":true},"
    "{\"name\":\"Meditation\",\"min\":10,\"reload\":true}]}";

static const char *DOC_WITH_BREAK =
    "{\"ver\":\"20260709\",\"timers\":["
    "{\"name\":\"Piano\",\"min\":15,\"reload\":true,\"break\":true},"
    "{\"name\":\"Meditation\",\"min\":10,\"reload\":true,\"break\":true}]}";

void setUp(void) {
    mock_nvs_reset();
    mock_time_reset();
    timer_set_defs(NULL, 0);
}
void tearDown(void) {}

/* Write the blob an operator's HA switch edits would leave behind: the
   documented timers, both break switches ON. This — not the boot — is what
   legitimately creates the table. */
static void operator_sets_both_break_switches(void) {
    nvs_timer_defs_blob_t b;
    memset(&b, 0, sizeof(b));
    b.version = TIMER_DEFS_BLOB_VERSION;
    snprintf(b.defs[0].name, sizeof(b.defs[0].name), "Piano");
    b.defs[0].min = 15;
    b.defs[0].reload = 1;
    b.defs[0].break_eligible = 1;
    snprintf(b.defs[1].name, sizeof(b.defs[1].name), "Meditation");
    b.defs[1].min = 10;
    b.defs[1].reload = 1;
    b.defs[1].break_eligible = 1;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_timer_defs(&b));
}

/* ---- boot: the compile-time table is installed, never persisted ---- */

/* The defect in one assertion. A blob that exists means "something
   authoritative wrote it"; boot inventing one destroys that meaning, and
   every consequence below follows from it. */
void test_install_without_blob_does_not_write_nvs(void) {
    timer_defs_install();
    TEST_ASSERT_EQUAL_INT(0, mock_nvs_write_count(NVS_KEY_TIMER_DEFS));
    nvs_timer_defs_blob_t b;
    TEST_ASSERT_NOT_EQUAL(ESP_OK, nvs_config_get_timer_defs(&b));
}

/* ...and the boot still runs on the compile-time table, which is the half
   of the old behaviour that has to survive: the timers must work, and the
   HA-facing readers reach this table through timer_slot_def(). */
void test_install_without_blob_still_runs_on_kconfig_table(void) {
    timer_defs_install();
    const timer_def_t *d = timer_slot_def(SLOT_PIANO);
    TEST_ASSERT_NOT_NULL(d);
    TEST_ASSERT_EQUAL_STRING("Piano", d->name);
    TEST_ASSERT_EQUAL_INT32(15 * 60, d->duration_sec);
    TEST_ASSERT_TRUE(d->reloadable);
    TEST_ASSERT_TRUE(d->break_eligible);
    d = timer_slot_def(SLOT_MEDITATION);
    TEST_ASSERT_NOT_NULL(d);
    TEST_ASSERT_EQUAL_STRING("Meditation", d->name);
    TEST_ASSERT_TRUE(d->break_eligible);
    TEST_ASSERT_NULL(timer_slot_def(3)); /* empty Kconfig name = disabled */
}

/* A stored table still wins, and still costs no write. */
void test_install_prefers_stored_blob(void) {
    operator_sets_both_break_switches();
    nvs_timer_defs_blob_t b;
    nvs_config_get_timer_defs(&b);
    b.defs[0].min = 42; /* an edit the Kconfig table does not have */
    nvs_config_set_timer_defs(&b);
    int writes_before = mock_nvs_write_count(NVS_KEY_TIMER_DEFS);
    timer_defs_install();
    TEST_ASSERT_EQUAL_INT(writes_before, mock_nvs_write_count(NVS_KEY_TIMER_DEFS));
    TEST_ASSERT_EQUAL_INT32(42 * 60, timer_slot_def(SLOT_PIANO)->duration_sec);
}

/* ---- boot + network window: the three cases of the spec's repro ---- */

/* Case A — blob intact. BUG-6's guarantee, unchanged: a document with no
   `break` key must not clear a flag the operator set. */
void test_intact_blob_keeps_operator_break(void) {
    timer_defs_install();
    operator_sets_both_break_switches();
    char ack[CONFIG_ACK_MIN];
    config_apply(DOC_NO_BREAK, ack, sizeof(ack));
    nvs_timer_defs_blob_t b;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_timer_defs(&b));
    TEST_ASSERT_EQUAL_UINT8(1, b.defs[0].break_eligible);
    TEST_ASSERT_EQUAL_UINT8(1, b.defs[1].break_eligible);
}

/* Case B — blob LOST (NVS erase, panic, layout drift), then the first
   retained document lands. THE regression test.

   The document is defining these slots for the first time as far as
   persisted state is concerned, so `break` must come from the document —
   absent means false, per apply_timers()'s documented rule and the safe
   direction for this field. Before the fix, boot had already written the
   compile-time table, apply_timers() read `existed` as true, and the
   COMPILE-TIME flag was preserved as though an operator had chosen it. */
void test_lost_blob_takes_break_from_the_document_not_kconfig(void) {
    timer_defs_install(); /* boot: no blob to read */
    char ack[CONFIG_ACK_MIN];
    config_apply(DOC_NO_BREAK, ack, sizeof(ack));
    nvs_timer_defs_blob_t b;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_timer_defs(&b));
    TEST_ASSERT_EQUAL_UINT8(0, b.defs[0].break_eligible);
    TEST_ASSERT_EQUAL_UINT8(0, b.defs[1].break_eligible);
    /* Everything the document does carry still lands, so this is not the
       fix trading one lost field for another. */
    TEST_ASSERT_EQUAL_STRING("Piano", b.defs[0].name);
    TEST_ASSERT_EQUAL_INT32(15, b.defs[0].min);
    TEST_ASSERT_EQUAL_UINT8(1, b.defs[0].reload);
    TEST_ASSERT_EQUAL_STRING("Meditation", b.defs[1].name);
    TEST_ASSERT_EQUAL_INT32(10, b.defs[1].min);
}

/* Case C — same loss, but the document carries `break`. Held before the fix
   and must keep holding: it is the operator-side mitigation. */
void test_lost_blob_document_carrying_break_wins(void) {
    timer_defs_install();
    char ack[CONFIG_ACK_MIN];
    config_apply(DOC_WITH_BREAK, ack, sizeof(ack));
    nvs_timer_defs_blob_t b;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_timer_defs(&b));
    TEST_ASSERT_EQUAL_UINT8(1, b.defs[0].break_eligible);
    TEST_ASSERT_EQUAL_UINT8(1, b.defs[1].break_eligible);
}

/* The document's table is authoritative from the next boot on — the point
   of not persisting at boot is that the blob, once it exists, was written
   by something that had the right to. */
void test_next_boot_runs_on_the_applied_document(void) {
    timer_defs_install();
    char ack[CONFIG_ACK_MIN];
    config_apply(DOC_WITH_BREAK, ack, sizeof(ack));
    timer_set_defs(NULL, 0); /* reboot */
    timer_defs_install();
    const timer_def_t *d = timer_slot_def(SLOT_PIANO);
    TEST_ASSERT_NOT_NULL(d);
    TEST_ASSERT_EQUAL_STRING("Piano", d->name);
    TEST_ASSERT_TRUE(d->break_eligible);
}

/* A stale-layout blob (the reflash/version-bump loss the spec lists as
   candidate 2) is the same case as no blob: unreadable, so it must not be
   overwritten at boot either. */
void test_stale_version_blob_is_not_overwritten_at_boot(void) {
    nvs_timer_defs_blob_t b;
    memset(&b, 0, sizeof(b));
    b.version = (uint8_t)(TIMER_DEFS_BLOB_VERSION - 1);
    snprintf(b.defs[0].name, sizeof(b.defs[0].name), "Old");
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_timer_defs(&b));
    int writes_before = mock_nvs_write_count(NVS_KEY_TIMER_DEFS);
    timer_defs_install();
    TEST_ASSERT_EQUAL_INT(writes_before, mock_nvs_write_count(NVS_KEY_TIMER_DEFS));
    TEST_ASSERT_EQUAL_STRING("Piano", timer_slot_def(SLOT_PIANO)->name);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_install_without_blob_does_not_write_nvs);
    RUN_TEST(test_install_without_blob_still_runs_on_kconfig_table);
    RUN_TEST(test_install_prefers_stored_blob);
    RUN_TEST(test_intact_blob_keeps_operator_break);
    RUN_TEST(test_lost_blob_takes_break_from_the_document_not_kconfig);
    RUN_TEST(test_lost_blob_document_carrying_break_wins);
    RUN_TEST(test_next_boot_runs_on_the_applied_document);
    RUN_TEST(test_stale_version_blob_is_not_overwritten_at_boot);
    return UNITY_END();
}
