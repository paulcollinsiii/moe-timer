#include <stdio.h>
#include <string.h>
#include <unity.h>

/* BUG-8 regression: what boot leaves behind for the network window.

   Single-TU, and deliberately the REAL timer_defs_install() rather than a
   transcription of it — docs/planning/implemented/bug8.repro.c (commit
   ef3af99) had to copy the function because main/timer_defs.c would not
   compile for the host; it does now, and this is the only suite that
   compiles it. The other half of the defect is the REAL config_apply() over
   the mock NVS: the bug is not in either function alone, it is in the boot
   writing a table the window then mistakes for an operator's. */

/* The compile-time table this TU's timer_defs.c is built against. Defined
   ahead of the include because that file's #ifndef fallbacks exist for
   exactly this case: the host has no sdkconfig.h.

   BREAK_ELIGIBLE is 1 on both configured slots ON PURPOSE, and it matches
   the firmware this repo actually ships: the gitignored, hand-maintained
   `sdkconfig` sets CONFIG_MAGTAG_TIMER1_BREAK_ELIGIBLE=y and
   CONFIG_MAGTAG_TIMER2_BREAK_ELIGIBLE=y. (`sdkconfig.defaults` contains no
   MAGTAG_TIMER symbols at all, and the `default n` in Kconfig.projbuild is
   the menu default, not the shipping value — an earlier version of this
   comment claimed `n` on every slot and was simply wrong. Nothing on this
   code path is inert on the real device.)

   Compile-time 1 is also what makes both halves observable. It makes the
   BUG-8 masquerade visible — persist the boot table and apply_timers() sees
   `existed`, so a value nobody chose is stored and published as an
   operator's — and it is the value the menuconfig rung of apply_timers()'
   optional-key ladder has to hand back when the document is silent.
   RELOADABLE differs between the two slots (1 and 0) deliberately: a test
   that saw `reload` come out 1 for both could not tell the compile-time
   table from a hardcoded true. */
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
/* This suite tests timer_defs, not chores. These two are here because
   config_apply.c below reconciles the RTC chore acks after a chore-list
   edit and so now links against chores.c + chore_store.c — the dependency
   cost of keeping that reconcile in config_apply.c rather than in
   net_apply_finish(), recorded and justified at apply_chores(). Every
   single-TU suite that includes config_apply.c inherits this pair. */
#include "../../main/chores.c"
#include "../../main/chore_store.c"
#include "../../main/quiet_hours.c"
#include "../../main/bedtime.c"
#include "../../main/config_validate.c"
#include "../../main/tones.c"
#include "../../main/config_apply.c"
#include "../../main/ha_config.c"
#include "../../main/timer_defs.c"
/* BUG-13's end-to-end case: the real set/<key> router in front of the
   real ha_config_set and the real slot table. */
#include "../../main/mqtt_rx.c"
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

/* Only the required keys. Both optional flags absent, so both fall to the
   bottom rung of the ladder for a slot with no stored definition. */
static const char *DOC_BARE =
    "{\"ver\":\"20260710\",\"timers\":["
    "{\"name\":\"Piano\",\"min\":15},"
    "{\"name\":\"Meditation\",\"min\":10}]}";

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
   retained document lands, and it does not mention `break`. THE case.

   The optional-key ladder is `document > stored blob > menuconfig`, so with
   no stored blob the compile-time value answers: an operator who set
   BREAK_ELIGIBLE=y in menuconfig keeps break-eligibility across an NVS
   erase. That is not the same thing as the BUG-8 masquerade, which was boot
   WRITING the compile-time table so it would be mistaken for stored state.
   Here the value is consulted in place, and it lands in flash only because
   an authoritative document was applied over it.

   This is the assertion ad62dff inverted: it removed the boot write and the
   menuconfig rung together, so both slots came out 0 and a deliberate
   menuconfig choice was silently dropped on the first document after an
   erase. */
void test_lost_blob_takes_break_from_kconfig_when_the_document_is_silent(void) {
    timer_defs_install(); /* boot: no blob to read */
    char ack[CONFIG_ACK_MIN];
    config_apply(DOC_NO_BREAK, ack, sizeof(ack));
    nvs_timer_defs_blob_t b;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_timer_defs(&b));
    TEST_ASSERT_EQUAL_UINT8(1, b.defs[0].break_eligible);
    TEST_ASSERT_EQUAL_UINT8(1, b.defs[1].break_eligible);
    /* Everything the document does carry still lands. */
    TEST_ASSERT_EQUAL_STRING("Piano", b.defs[0].name);
    TEST_ASSERT_EQUAL_INT32(15, b.defs[0].min);
    TEST_ASSERT_EQUAL_UINT8(1, b.defs[0].reload);
    TEST_ASSERT_EQUAL_STRING("Meditation", b.defs[1].name);
    TEST_ASSERT_EQUAL_INT32(10, b.defs[1].min);
}

/* `reload` takes the identical rung. The compile-time table differs between
   the two slots (1 and 0), so this cannot pass by accident: a hardcoded
   default of either polarity fails one of the two assertions. */
void test_lost_blob_takes_reload_from_kconfig_when_the_document_is_silent(void) {
    timer_defs_install();
    char ack[CONFIG_ACK_MIN];
    config_apply(DOC_BARE, ack, sizeof(ack));
    nvs_timer_defs_blob_t b;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_timer_defs(&b));
    TEST_ASSERT_EQUAL_UINT8(1, b.defs[0].reload); /* TIMER1_RELOADABLE=1 */
    TEST_ASSERT_EQUAL_UINT8(0, b.defs[1].reload); /* TIMER2_RELOADABLE=0 */
    TEST_ASSERT_EQUAL_UINT8(1, b.defs[0].break_eligible);
    TEST_ASSERT_EQUAL_UINT8(1, b.defs[1].break_eligible);
}

/* The rung is menuconfig's, not the INSTALLED table's. Slot 4 is unnamed in
   the compile-time table, so a document defining it for the first time gets
   0 for both flags — the bottom of the ladder really is the compile-time
   value for THAT SLOT INDEX, not a blanket "true". */
void test_a_slot_menuconfig_does_not_configure_still_defaults_to_off(void) {
    timer_defs_install();
    char ack[CONFIG_ACK_MIN];
    config_apply(
        "{\"ver\":\"20260710\",\"timers\":[{},{},{},"
        "{\"name\":\"Yoga\",\"min\":20}]}",
        ack, sizeof(ack));
    nvs_timer_defs_blob_t b;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_timer_defs(&b));
    TEST_ASSERT_EQUAL_STRING("Yoga", b.defs[3].name);
    TEST_ASSERT_EQUAL_UINT8(0, b.defs[3].break_eligible);
    TEST_ASSERT_EQUAL_UINT8(0, b.defs[3].reload);
}

/* Tier 2 still outranks tier 3: a stored 0 that an operator chose from the
   per-timer switch is not overwritten by menuconfig's 1. Without this, the
   ladder would collapse the other way and the switch would be useless. */
void test_a_stored_off_outranks_the_compile_time_on(void) {
    timer_defs_install();
    nvs_timer_defs_blob_t b;
    memset(&b, 0, sizeof(b));
    b.version = TIMER_DEFS_BLOB_VERSION;
    snprintf(b.defs[0].name, sizeof(b.defs[0].name), "Piano");
    b.defs[0].min = 15;
    b.defs[0].break_eligible = 0; /* operator turned the switch OFF */
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_timer_defs(&b));
    char ack[CONFIG_ACK_MIN];
    config_apply(DOC_NO_BREAK, ack, sizeof(ack));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_timer_defs(&b));
    TEST_ASSERT_EQUAL_UINT8(0, b.defs[0].break_eligible);
}

/* BUG-8's other door: ha_config_set's read-modify-write cases. They used to
   NAK unconditionally, which kept the "a blob exists means somebody
   authoritative wrote it" invariant by refusing to be an authority at all —
   and left an operator who never publishes a `timers` document NAKing
   forever, with no diagnostic (nothing publishes this ack; see mqtt_ha.c).

   The control IS an authority. So the edit now lands, and the invariant
   moves from the blob's EXISTENCE to a per-slot `defined` bit: the table
   this writes says, byte by byte, which slot a human chose and which slots
   are menuconfig defaults carried along so the write does not disable them.

   Ordering note for why this path is reachable rather than theoretical:
   mqtt_ha's apply_incoming() runs config_apply BEFORE apply_sets, so a
   retained document in the same window would have closed the hole; it is
   open precisely on a device that has no `timers` document at all. */
void test_a_single_field_edit_creates_the_table_and_stamps_its_slot(void) {
    timer_defs_install(); /* boot: no blob, running on the Kconfig table */
    char ack[CONFIG_ACK_MIN];
    TEST_ASSERT_EQUAL(HA_CFG_OK, ha_config_set("timer1_min", "42", ack, sizeof(ack)));
    nvs_timer_defs_blob_t b;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_timer_defs(&b));
    TEST_ASSERT_EQUAL_INT32(42, b.defs[0].min);
    TEST_ASSERT_EQUAL_UINT8(1, b.defs[0].defined);
    /* The bystanders are carried, not adopted. Carried, because writing
       zeros over them would disable timers menuconfig enabled — the wipe
       BUG-8's first form actually caused. Not adopted, because `defined`
       stays 0 and their values are menuconfig's own, verbatim. */
    TEST_ASSERT_EQUAL_STRING("Meditation", b.defs[1].name);
    TEST_ASSERT_EQUAL_INT32(10, b.defs[1].min);
    TEST_ASSERT_EQUAL_UINT8(0, b.defs[1].reload); /* TIMER2_RELOADABLE=0 */
    TEST_ASSERT_EQUAL_UINT8(0, b.defs[1].defined);
    TEST_ASSERT_EQUAL_UINT8(0, b.defs[2].defined);
    TEST_ASSERT_EQUAL_UINT8(0, b.defs[3].defined);
    /* And the document still wins over the control when it arrives. */
    config_apply(DOC_NO_BREAK, ack, sizeof(ack));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_timer_defs(&b));
    TEST_ASSERT_EQUAL_INT32(15, b.defs[0].min);
}

/* The stamp is what carries an operator's choice on a slot that has no
   name yet — the case the pre-`defined` name test could not represent, and
   the reason a bit was needed rather than a widened name test.

   Slot 4 is unnamed in the compile-time table, so flipping its break
   switch leaves `name` empty. Under the old predicate the next document to
   name that slot saw "no previous definition" and handed the flag back to
   menuconfig, silently discarding the switch flip. */
void test_a_switch_flip_on_an_unnamed_slot_survives_the_next_document(void) {
    timer_defs_install();
    char ack[CONFIG_ACK_MIN];
    TEST_ASSERT_EQUAL(HA_CFG_OK, ha_config_set("timer4_break", "ON", ack, sizeof(ack)));
    nvs_timer_defs_blob_t b;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_timer_defs(&b));
    TEST_ASSERT_EQUAL_STRING("", b.defs[3].name);
    TEST_ASSERT_EQUAL_UINT8(1, b.defs[3].defined);

    config_apply(
        "{\"ver\":\"20260712\",\"timers\":[{},{},{},"
        "{\"name\":\"Yoga\",\"min\":20}]}",
        ack, sizeof(ack));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_timer_defs(&b));
    TEST_ASSERT_EQUAL_STRING("Yoga", b.defs[3].name);
    TEST_ASSERT_EQUAL_UINT8(1, b.defs[3].break_eligible); /* the switch, not menuconfig's 0 */
}

/* The menuconfig rung survives a control-created table, for a slot the
   control did not stamp.

   Read the assertion honestly: for slot 2 the stored value and the
   menuconfig value are the SAME value, because the table the control wrote
   was seeded from timer_defs_compiled(). That identity is exactly why
   `existed` may stay OR'd with the name test without reopening BUG-8 — a
   carried bystander cannot answer differently from the rung it came from.
   Slot 4, unnamed in menuconfig, is where the two are distinguishable, and
   it goes to the rung. The stamped slot is the control: it holds the
   operator's OFF against a menuconfig that says ON. */
void test_the_rung_survives_a_table_a_control_created(void) {
    timer_defs_install();
    char ack[CONFIG_ACK_MIN];
    /* menuconfig says TIMER1_BREAK_ELIGIBLE=1; the operator says no. */
    TEST_ASSERT_EQUAL(HA_CFG_OK, ha_config_set("timer1_break", "OFF", ack, sizeof(ack)));
    nvs_timer_defs_blob_t b;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_timer_defs(&b));
    TEST_ASSERT_EQUAL_UINT8(1, b.defs[0].defined);
    TEST_ASSERT_EQUAL_UINT8(0, b.defs[1].defined);

    /* A document defining all four slots and stating neither optional key. */
    config_apply(
        "{\"ver\":\"20260713\",\"timers\":["
        "{\"name\":\"Piano\",\"min\":15},"
        "{\"name\":\"Meditation\",\"min\":10},"
        "{\"name\":\"Cello\",\"min\":30},"
        "{\"name\":\"Yoga\",\"min\":20}]}",
        ack, sizeof(ack));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_timer_defs(&b));
    TEST_ASSERT_EQUAL_UINT8(0, b.defs[0].break_eligible); /* stamped: the operator's OFF */
    TEST_ASSERT_EQUAL_UINT8(1, b.defs[0].reload);         /* stamped: carried, = TIMER1_RELOADABLE */
    TEST_ASSERT_EQUAL_UINT8(1, b.defs[1].break_eligible); /* unstamped: TIMER2_BREAK_ELIGIBLE=1 */
    TEST_ASSERT_EQUAL_UINT8(0, b.defs[1].reload);         /* unstamped: TIMER2_RELOADABLE=0 */
    /* Slots 3 and 4 are unnamed in menuconfig, so the rung is 0/0 and the
       carried-bystander identity above cannot be masking anything. */
    TEST_ASSERT_EQUAL_UINT8(0, b.defs[2].break_eligible);
    TEST_ASSERT_EQUAL_UINT8(0, b.defs[3].break_eligible);
}

/* A v2 blob written before `defined` existed reads as all-zero in those
   bytes, and must behave EXACTLY as it does today: the name test carries
   it and nothing about the ladder moves. This is the whole safety argument
   for landing the field without a version bump, so it is asserted rather
   than reasoned about — including on the byte itself, since a writer that
   forgot to memset would show up here first. */
void test_a_blob_with_no_defined_bytes_behaves_exactly_as_before(void) {
    timer_defs_install();
    operator_sets_both_break_switches(); /* memset: every `defined` is 0 */
    nvs_timer_defs_blob_t b;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_timer_defs(&b));
    for (int i = 0; i < TIMER_EXTRA_SLOTS; i++) {
        TEST_ASSERT_EQUAL_UINT8(0, b.defs[i].defined);
        TEST_ASSERT_EQUAL_UINT8(0, b.defs[i].rsvd);
    }
    char ack[CONFIG_ACK_MIN];
    config_apply(DOC_NO_BREAK, ack, sizeof(ack));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_timer_defs(&b));
    /* Tier 2 answered for both slots on the strength of the name alone. */
    TEST_ASSERT_EQUAL_UINT8(1, b.defs[0].break_eligible);
    TEST_ASSERT_EQUAL_UINT8(1, b.defs[1].break_eligible);
    TEST_ASSERT_EQUAL_UINT8(1, b.defs[0].reload);
    TEST_ASSERT_EQUAL_UINT8(1, b.defs[1].reload);
}

/* BUG-5. `cfg_ver` and the timer-defs blob are independent NVS keys, so the
   `ver`-match early return could strand a device forever: the table is
   gone, the only document that would rebuild it is skipped unread, and
   every per-timer control NAKs because there is nothing readable to
   modify. Nothing in HA says so and nothing on the device heals it. The
   skip is now conditional on the table being readable. */
void test_a_ver_match_still_rebuilds_an_unreadable_table(void) {
    timer_defs_install();
    char ack[CONFIG_ACK_MIN];
    TEST_ASSERT_EQUAL(CONFIG_APPLIED, config_apply(DOC_WITH_BREAK, ack, sizeof(ack)));

    /* The table stops parsing; cfg_ver is untouched, as a separate key is. */
    nvs_timer_defs_blob_t b;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_timer_defs(&b));
    b.version = (uint8_t)(TIMER_DEFS_BLOB_VERSION - 1);
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_timer_defs(&b));
    TEST_ASSERT_NOT_EQUAL(ESP_OK, nvs_config_get_timer_defs(&b));

    /* The same retained document, redelivered on the next window. */
    TEST_ASSERT_EQUAL(CONFIG_SKIPPED, config_apply(DOC_WITH_BREAK, ack, sizeof(ack)));
    TEST_ASSERT_NOT_NULL(strstr(ack, "\"skipped\":true"));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_timer_defs(&b));
    TEST_ASSERT_EQUAL_STRING("Piano", b.defs[0].name);
    TEST_ASSERT_EQUAL_STRING("Meditation", b.defs[1].name);
    TEST_ASSERT_EQUAL_UINT8(1, b.defs[0].break_eligible);
    /* Rebuilt from the document, so the slots it defines are stamped. */
    TEST_ASSERT_EQUAL_UINT8(1, b.defs[0].defined);
}

/* ...and the skip is still a skip when the table is fine: a ver match must
   not rewrite the blob every window (a flash write per wake on a battery
   device), nor undo an HA control edit made since the document landed. */
void test_a_ver_match_with_a_readable_table_writes_nothing(void) {
    timer_defs_install();
    char ack[CONFIG_ACK_MIN];
    TEST_ASSERT_EQUAL(CONFIG_APPLIED, config_apply(DOC_WITH_BREAK, ack, sizeof(ack)));
    TEST_ASSERT_EQUAL(HA_CFG_OK, ha_config_set("timer1_min", "42", ack, sizeof(ack)));
    int writes_before = mock_nvs_write_count(NVS_KEY_TIMER_DEFS);
    TEST_ASSERT_EQUAL(CONFIG_SKIPPED, config_apply(DOC_WITH_BREAK, ack, sizeof(ack)));
    TEST_ASSERT_EQUAL_INT(writes_before, mock_nvs_write_count(NVS_KEY_TIMER_DEFS));
    nvs_timer_defs_blob_t b;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_timer_defs(&b));
    TEST_ASSERT_EQUAL_INT32(42, b.defs[0].min);
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

/* Both BUG-8 warnings sit on paths that run EVERY network window: main.c
   installs at wake, net_apply's reconcile_defs() re-installs after each
   window, and both of ha_config's read-only callers run per window. An
   unconditional ESP_LOGW there is several log lines per window on a device
   that legitimately has no blob, which is noise on exactly the devices
   whose logs someone would be reading. Both are latched, and both re-arm on
   a successful read so a LATER loss still says so. ESP_LOGW is a no-op on
   the host, so the latch itself is what is pinned. */
void test_the_missing_blob_warnings_are_latched(void) {
    s_no_blob_warned = false;
    s_defs_fallback_warned = false;
    nvs_timer_defs_blob_t b;

    timer_defs_install(); /* no blob */
    TEST_ASSERT_TRUE(s_no_blob_warned);
    TEST_ASSERT_EQUAL(ESP_ERR_NVS_NOT_FOUND, load_defs(&b, DEFS_SEED_INSTALLED));
    TEST_ASSERT_TRUE(s_defs_fallback_warned);

    /* A readable table re-arms both. */
    operator_sets_both_break_switches();
    timer_defs_install();
    TEST_ASSERT_FALSE(s_no_blob_warned);
    TEST_ASSERT_EQUAL(ESP_OK, load_defs(&b, DEFS_SEED_INSTALLED));
    TEST_ASSERT_FALSE(s_defs_fallback_warned);
}

/* The other half of the split BUG-5 asked for: load_defs() and
   timer_defs_install() must be able to say WHICH failure they hit, because
   "never written" is routine and "written but unparseable" is data loss
   that also disables every per-timer control. ESP_LOGW is a no-op on the
   host, so what is pinned is that the two states are distinguishable at
   all — the write path's behaviour turns on exactly this distinction. */
void test_a_never_written_table_is_distinguishable_from_an_unreadable_one(void) {
    nvs_timer_defs_blob_t b;
    TEST_ASSERT_EQUAL(ESP_ERR_NVS_NOT_FOUND, load_defs(&b, DEFS_SEED_COMPILED));

    memset(&b, 0, sizeof(b));
    b.version = (uint8_t)(TIMER_DEFS_BLOB_VERSION - 1);
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_timer_defs(&b));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_VERSION, load_defs(&b, DEFS_SEED_COMPILED));

    /* And the write path acts on the difference: bytes that are there but
       unreadable are still not overwritten by a single-field edit. */
    char ack[CONFIG_ACK_MIN];
    TEST_ASSERT_EQUAL(HA_CFG_REJECTED, ha_config_set("timer1_min", "42", ack, sizeof(ack)));
    TEST_ASSERT_NOT_NULL(strstr(ack, "\"err\":\"nodefs\""));
}

/* BUG-13, as far as the host reaches: HA blanks "Timer 1 name", its
   command template turns the blank into the two characters "", the broker
   delivers them on set/timer1_name, and the slot is disabled once the
   window re-installs the table (net_apply.c's reconcile_defs() calls
   timer_defs_install() for exactly this). A zero-length message on the
   same kind of topic, the form HA used to send, still never gets past the
   router. What is not reached: HA's own template rendering, and the
   broker. Nor a slot that is mid-run: this one is idle. The blob path
   installs exactly the name="" def those cases use, and they are pinned
   elsewhere — test_timer.c test_reconcile_disable_resets_like_rename (a
   RUNNING slot disabled by name="" folds and resets), test_net_apply.c
   test_active_slot_disabled_by_edit_reverts_selection (the active PAUSED
   slot disabled mid-window: selection moves off it) and
   test_background_paused_slot_fixed_silently (a background PAUSED slot
   redefined mid-window: reset, no sound — shown with a rename, which
   takes the same reconcile reset). */
void test_a_blank_timer_name_from_ha_disables_the_slot(void) {
    timer_defs_install(); /* boot: Piano and Meditation from menuconfig */
    TEST_ASSERT_NOT_NULL(timer_slot_def(SLOT_PIANO));
    TEST_ASSERT_NOT_NULL(timer_slot_def(SLOT_MEDITATION));

    static const char PREFIX[] = "magtag/magtag-a1b2c3/set/";
    mqtt_set_kv_t sets[4];
    memset(sets, 0, sizeof(sets));
    mqtt_rx_t rx = {.sets = sets, .sets_cap = 4, .set_prefix_len = (int)strlen(PREFIX)};
    const char *t1 = "magtag/magtag-a1b2c3/set/timer1_name";
    const char *t2 = "magtag/magtag-a1b2c3/set/timer2_name";
    /* The old wire form: dropped, so slot 2 must survive untouched. */
    TEST_ASSERT_EQUAL(MQTT_RX_IGNORED, mqtt_rx_on_data(&rx, t2, (int)strlen(t2), "", 0, 0, 0));
    /* The new one. */
    TEST_ASSERT_EQUAL(MQTT_RX_OK, mqtt_rx_on_data(&rx, t1, (int)strlen(t1), "\"\"", 2, 2, 0));
    TEST_ASSERT_EQUAL_INT(1, rx.set_count);

    /* mqtt_ha.c apply_sets(): every buffered set goes to ha_config_set. */
    char ack[CONFIG_ACK_MIN];
    TEST_ASSERT_EQUAL(HA_CFG_OK, ha_config_set(sets[0].key, sets[0].value, ack, sizeof(ack)));

    nvs_timer_defs_blob_t b;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_timer_defs(&b));
    TEST_ASSERT_EQUAL_STRING("", b.defs[SLOT_PIANO - 1].name); /* never the literal "" */
    TEST_ASSERT_EQUAL_UINT8(1, b.defs[SLOT_PIANO - 1].defined);
    TEST_ASSERT_EQUAL_STRING("Meditation", b.defs[SLOT_MEDITATION - 1].name);

    timer_defs_install(); /* the window's reconcile, or the next boot */
    TEST_ASSERT_NULL(timer_slot_def(SLOT_PIANO));
    TEST_ASSERT_NOT_NULL(timer_slot_def(SLOT_MEDITATION));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_install_without_blob_does_not_write_nvs);
    RUN_TEST(test_install_without_blob_still_runs_on_kconfig_table);
    RUN_TEST(test_install_prefers_stored_blob);
    RUN_TEST(test_intact_blob_keeps_operator_break);
    RUN_TEST(test_lost_blob_takes_break_from_kconfig_when_the_document_is_silent);
    RUN_TEST(test_lost_blob_takes_reload_from_kconfig_when_the_document_is_silent);
    RUN_TEST(test_a_slot_menuconfig_does_not_configure_still_defaults_to_off);
    RUN_TEST(test_a_stored_off_outranks_the_compile_time_on);
    RUN_TEST(test_a_single_field_edit_creates_the_table_and_stamps_its_slot);
    RUN_TEST(test_a_switch_flip_on_an_unnamed_slot_survives_the_next_document);
    RUN_TEST(test_the_rung_survives_a_table_a_control_created);
    RUN_TEST(test_a_blob_with_no_defined_bytes_behaves_exactly_as_before);
    RUN_TEST(test_a_ver_match_still_rebuilds_an_unreadable_table);
    RUN_TEST(test_a_ver_match_with_a_readable_table_writes_nothing);
    RUN_TEST(test_lost_blob_document_carrying_break_wins);
    RUN_TEST(test_next_boot_runs_on_the_applied_document);
    RUN_TEST(test_stale_version_blob_is_not_overwritten_at_boot);
    RUN_TEST(test_the_missing_blob_warnings_are_latched);
    RUN_TEST(test_a_never_written_table_is_distinguishable_from_an_unreadable_one);
    RUN_TEST(test_a_blank_timer_name_from_ha_disables_the_slot);
    return UNITY_END();
}
