/* The panic breadcrumb's rules.
 *
 * A device that panics unattended several times a day tells nobody why.
 * This module's job is to answer two questions over MQTT — how often, and
 * doing what — and the whole way it can fail is by answering them WRONG,
 * because a wrong answer here sends someone to debug the OTA path when
 * the fault is in the renderer, or the reverse.
 *
 * So the cases below are written against specific ways of getting it
 * wrong, not against the happy path:
 *
 *   - the cold-boot guard. RTC_NOINIT_ATTR memory is UNINITIALISED at
 *     power-on: whatever the RTC RAM held is what the first read sees. A
 *     guard that let that through would publish noise as a real reading
 *     on the one boot where nobody would question it.
 *   - the two slots. A single phase byte written by both the main task
 *     and the network task would report a panic during MQTT as RENDER,
 *     which is precisely the confusion this feature exists to remove.
 *   - nesting. RENDER sits inside AWAKE; a paint that did not put AWAKE
 *     back would mislabel every later panic in that wake.
 *   - "no breadcrumb" vs "no phase". Empty and "NONE" are different
 *     findings and must not collapse into each other.
 *
 * Only the pure half of panic_diag.c compiles here — the RTC/NVS/
 * FreeRTOS half is behind the NATIVE fence — and that is the half every
 * rule above lives in.
 */
#include <string.h>
#include <unity.h>

/* Single-TU, like every suite here. */
#include "../../main/panic_diag.c"

void setUp(void) {}
void tearDown(void) {}

static const panic_sample_t SAMPLE = {.uptime_ms = 12345, .heap_free = 90000, .stack_free = 2048};

/* ---- the phase table ---- */

void test_every_phase_has_a_distinct_name(void) {
    /* A duplicated string here would make two different phases
       indistinguishable in Home Assistant, which is the one output this
       module has. */
    for (int a = 0; a < PANIC_PHASE__COUNT; a++) {
        for (int b = a + 1; b < PANIC_PHASE__COUNT; b++) {
            TEST_ASSERT_NOT_EQUAL_MESSAGE(
                0, strcmp(panic_diag_phase_str((panic_phase_t)a), panic_diag_phase_str((panic_phase_t)b)),
                "two phases share a name");
        }
    }
}

void test_the_phases_the_brief_asked_for_are_all_separable(void) {
    /* Early boot / render / network window / MQTT / OTA check / OTA
       download / sleep entry — named individually rather than by count,
       so dropping one fails here rather than passing a total. */
    TEST_ASSERT_EQUAL_STRING("BOOT", panic_diag_phase_str(PANIC_PHASE_BOOT));
    TEST_ASSERT_EQUAL_STRING("AWAKE", panic_diag_phase_str(PANIC_PHASE_AWAKE));
    TEST_ASSERT_EQUAL_STRING("RENDER", panic_diag_phase_str(PANIC_PHASE_RENDER));
    TEST_ASSERT_EQUAL_STRING("NET", panic_diag_phase_str(PANIC_PHASE_NET));
    TEST_ASSERT_EQUAL_STRING("OTA_CHECK", panic_diag_phase_str(PANIC_PHASE_OTA_CHECK));
    TEST_ASSERT_EQUAL_STRING("MQTT", panic_diag_phase_str(PANIC_PHASE_MQTT));
    TEST_ASSERT_EQUAL_STRING("OTA_DL", panic_diag_phase_str(PANIC_PHASE_OTA_DL));
    TEST_ASSERT_EQUAL_STRING("SLEEP", panic_diag_phase_str(PANIC_PHASE_SLEEP));
}

void test_each_boot_subdivision_names_one_init_call(void) {
    /* The whole reason these exist: a breadcrumb reading "BOOT" spans
       NVS init, the OTA rollback detector, the ADC, the LVGL framebuffer
       and a charge gate that can run a full refresh AND a network window
       — which localises nothing. Named individually, and against the
       exact strings, because the strings ARE the diagnostic: an operator
       reads them in Home Assistant and goes to that call. */
    TEST_ASSERT_EQUAL_STRING("BOOT_NVS", panic_diag_phase_str(PANIC_PHASE_BOOT_NVS));
    TEST_ASSERT_EQUAL_STRING("BOOT_OTA", panic_diag_phase_str(PANIC_PHASE_BOOT_OTA));
    TEST_ASSERT_EQUAL_STRING("BOOT_DISP", panic_diag_phase_str(PANIC_PHASE_BOOT_DISP));
    TEST_ASSERT_EQUAL_STRING("BOOT_BATT", panic_diag_phase_str(PANIC_PHASE_BOOT_BATT));
    TEST_ASSERT_EQUAL_STRING("BOOT_LOCK", panic_diag_phase_str(PANIC_PHASE_BOOT_LOCK));
    /* And the phase they subdivide is still its own reading: the wiring
       calls nobody gave a sub-phase to (neopixel_init, the TZ read, the
       timer restore, buttons_init, the heap log) still report BOOT. */
    TEST_ASSERT_EQUAL_STRING("BOOT", panic_diag_phase_str(PANIC_PHASE_BOOT));
}

void test_the_reachable_boot_lock_pairing_fills_the_published_field_exactly(void) {
    /* This is the pairing that spends the entire width budget, and it is
       reachable rather than theoretical: lock_gate_check_charge() runs
       from inside BOOT_LOCK and calls net_apply_try_window(), which is
       what puts OTA_CHECK in the net slot. 9 + 1 + 9 + 1 = 20 =
       DIAG_PHASE_MAX, so the field is FULL — a tenth character on a
       main-slot label ("BOOT_DISPLAY" was the first draft) truncates
       here, and a truncated label still reads like a phase name in Home
       Assistant. Asserted as an exact length rather than "fits", so
       shortening DIAG_PHASE_MAX or lengthening a name both land here
       with the arithmetic on show. */
    char buf[DIAG_PHASE_MAX];
    const int n = panic_diag_phase_label(buf, sizeof(buf), PANIC_PHASE_BOOT_LOCK, PANIC_PHASE_OTA_CHECK);
    TEST_ASSERT_EQUAL_STRING("BOOT_LOCK+OTA_CHECK", buf);
    TEST_ASSERT_EQUAL_INT(19, n);
    TEST_ASSERT_EQUAL_INT((int)sizeof(buf) - 1, n); /* exactly full, not merely fitting */
}

void test_a_phase_this_image_does_not_know_is_not_reported_as_one_it_does(void) {
    /* The values are stored — in RTC memory and in an NVS blob — so a
       record written by another firmware can arrive here. "?" says the
       record is unreadable; silently answering NONE would claim a panic
       happened outside every phase, which is a finding, not a shrug. */
    TEST_ASSERT_EQUAL_STRING("?", panic_diag_phase_str((panic_phase_t)PANIC_PHASE__COUNT));
    TEST_ASSERT_EQUAL_STRING("?", panic_diag_phase_str((panic_phase_t)200));
}

void test_the_slot_map_puts_every_network_phase_on_the_network_side(void) {
    /* This is what stops a panic in the TLS path being attributed to the
       main task's last paint. */
    TEST_ASSERT_TRUE(panic_diag_phase_is_net(PANIC_PHASE_NET));
    TEST_ASSERT_TRUE(panic_diag_phase_is_net(PANIC_PHASE_OTA_CHECK));
    TEST_ASSERT_TRUE(panic_diag_phase_is_net(PANIC_PHASE_MQTT));
    TEST_ASSERT_TRUE(panic_diag_phase_is_net(PANIC_PHASE_OTA_DL));
    TEST_ASSERT_FALSE(panic_diag_phase_is_net(PANIC_PHASE_BOOT));
    TEST_ASSERT_FALSE(panic_diag_phase_is_net(PANIC_PHASE_AWAKE));
    TEST_ASSERT_FALSE(panic_diag_phase_is_net(PANIC_PHASE_RENDER));
    TEST_ASSERT_FALSE(panic_diag_phase_is_net(PANIC_PHASE_SLEEP));
    TEST_ASSERT_FALSE(panic_diag_phase_is_net(PANIC_PHASE_NONE));
    TEST_ASSERT_FALSE(panic_diag_phase_is_net((panic_phase_t)200));
    /* The BOOT subdivisions, every one of them. They are marked from the
       main task inside app_main, and BOOT_LOCK in particular runs a whole
       network window from inside itself (lock_gate_check_charge calls
       net_apply_try_window) — so a boot phase that answered true here
       would overwrite the very NET/OTA_CHECK/MQTT reading that window is
       there to produce, and the published label would lose the half that
       matters. */
    TEST_ASSERT_FALSE(panic_diag_phase_is_net(PANIC_PHASE_BOOT_NVS));
    TEST_ASSERT_FALSE(panic_diag_phase_is_net(PANIC_PHASE_BOOT_OTA));
    TEST_ASSERT_FALSE(panic_diag_phase_is_net(PANIC_PHASE_BOOT_DISP));
    TEST_ASSERT_FALSE(panic_diag_phase_is_net(PANIC_PHASE_BOOT_BATT));
    TEST_ASSERT_FALSE(panic_diag_phase_is_net(PANIC_PHASE_BOOT_LOCK));
}

/* ---- the label ---- */

void test_both_sides_busy_reports_both(void) {
    char buf[DIAG_PHASE_MAX];
    panic_diag_phase_label(buf, sizeof(buf), PANIC_PHASE_RENDER, PANIC_PHASE_OTA_CHECK);
    TEST_ASSERT_EQUAL_STRING("RENDER+OTA_CHECK", buf);
}

void test_one_side_idle_reports_only_the_other(void) {
    char buf[DIAG_PHASE_MAX];
    panic_diag_phase_label(buf, sizeof(buf), PANIC_PHASE_AWAKE, PANIC_PHASE_NONE);
    TEST_ASSERT_EQUAL_STRING("AWAKE", buf);
    panic_diag_phase_label(buf, sizeof(buf), PANIC_PHASE_NONE, PANIC_PHASE_MQTT);
    TEST_ASSERT_EQUAL_STRING("MQTT", buf);
}

void test_neither_side_in_a_phase_is_a_reading_not_a_blank(void) {
    /* "NONE" is a positive statement: a panic was recorded, and it landed
       outside every marked phase — which points at the unmarked code
       (interrupt context, the wifi driver's own tasks) rather than at any
       of ours. The empty string means something else entirely and is
       asserted separately below. */
    char buf[DIAG_PHASE_MAX];
    panic_diag_phase_label(buf, sizeof(buf), PANIC_PHASE_NONE, PANIC_PHASE_NONE);
    TEST_ASSERT_EQUAL_STRING("NONE", buf);
}

void test_the_worst_case_label_fits_the_published_field(void) {
    /* The field is a fixed width in the stat payload; a label that did
       not fit would be truncated into a different phase's name. The
       static assert in panic_diag.c pins the width, this pins the
       string. */
    char buf[DIAG_PHASE_MAX];
    int n = panic_diag_phase_label(buf, sizeof(buf), PANIC_PHASE_RENDER, PANIC_PHASE_OTA_CHECK);
    TEST_ASSERT_LESS_THAN_INT((int)sizeof(buf), n);
    TEST_ASSERT_EQUAL_INT((int)strlen(buf), n);
}

void test_the_label_answers_a_zero_length_buffer_rather_than_writing_to_it(void) {
    char buf[1] = {'x'};
    TEST_ASSERT_EQUAL_INT(0, panic_diag_phase_label(buf, 0, PANIC_PHASE_AWAKE, PANIC_PHASE_NONE));
    TEST_ASSERT_EQUAL_CHAR('x', buf[0]); /* untouched */
    TEST_ASSERT_EQUAL_INT(0, panic_diag_phase_label(NULL, 16, PANIC_PHASE_AWAKE, PANIC_PHASE_NONE));
}

/* ---- the cold-boot guard ---- */

void test_a_freshly_reset_record_is_valid(void) {
    panic_diag_rec_t r;
    memset(&r, 0xAB, sizeof(r));
    panic_diag_rec_reset(&r);
    TEST_ASSERT_TRUE(panic_diag_rec_valid(&r));
    TEST_ASSERT_EQUAL_UINT8(PANIC_PHASE_NONE, r.main_phase);
    TEST_ASSERT_EQUAL_UINT8(PANIC_PHASE_NONE, r.net_phase);
}

void test_uninitialised_rtc_memory_does_not_pass_the_guard(void) {
    /* THE case this guard exists for. RTC_NOINIT_ATTR is not zeroed at
       power-on, so on a cold boot the record holds whatever was in the
       RAM. Two independent fillers, because a guard that only rejected
       0x00 would happily accept 0xFF. */
    panic_diag_rec_t zeros;
    memset(&zeros, 0x00, sizeof(zeros));
    TEST_ASSERT_FALSE(panic_diag_rec_valid(&zeros));

    panic_diag_rec_t ones;
    memset(&ones, 0xFF, sizeof(ones));
    TEST_ASSERT_FALSE(panic_diag_rec_valid(&ones));
}

void test_the_magic_alone_is_not_enough(void) {
    /* Half a guard: RTC RAM that happens to contain the magic word — or
       a record from a firmware that used the same magic with a different
       layout — must still be rejected by the checksum. */
    panic_diag_rec_t r;
    memset(&r, 0x5A, sizeof(r));
    r.magic = PANIC_DIAG_MAGIC;
    TEST_ASSERT_FALSE(panic_diag_rec_valid(&r));
}

void test_the_checksum_alone_is_not_enough(void) {
    panic_diag_rec_t r;
    panic_diag_rec_reset(&r);
    r.magic = PANIC_DIAG_MAGIC + 1u;
    r.sum = rec_sum(&r); /* internally consistent, wrong identity */
    TEST_ASSERT_FALSE(panic_diag_rec_valid(&r));
}

void test_a_single_flipped_bit_anywhere_in_the_payload_is_caught(void) {
    /* Every byte before `sum` must be covered. A checksum that skipped a
       field would let that field be published as garbage. */
    panic_diag_rec_t base;
    panic_diag_rec_reset(&base);
    panic_diag_rec_mark(&base, PANIC_PHASE_MQTT, PANIC_PHASE_MQTT, &SAMPLE);
    TEST_ASSERT_TRUE(panic_diag_rec_valid(&base));

    for (size_t i = 0; i < offsetof(panic_diag_rec_t, sum); i++) {
        panic_diag_rec_t r = base;
        ((uint8_t *)&r)[i] ^= 0x01u;
        TEST_ASSERT_FALSE_MESSAGE(panic_diag_rec_valid(&r), "a corrupted byte passed the guard");
    }
}

void test_the_guard_answers_null_rather_than_dereferencing_it(void) {
    TEST_ASSERT_FALSE(panic_diag_rec_valid(NULL));
    panic_diag_rec_reset(NULL); /* must not crash */
    panic_diag_rec_mark(NULL, PANIC_PHASE_BOOT, PANIC_PHASE_BOOT, &SAMPLE);
    panic_diag_fill_stat(NULL, 3, NULL);
}

/* ---- the record state machine ---- */

void test_a_mark_lands_in_the_slot_the_phase_owns(void) {
    panic_diag_rec_t r;
    panic_diag_rec_reset(&r);

    panic_diag_rec_mark(&r, PANIC_PHASE_AWAKE, PANIC_PHASE_AWAKE, &SAMPLE);
    TEST_ASSERT_EQUAL_UINT8(PANIC_PHASE_AWAKE, r.main_phase);
    TEST_ASSERT_EQUAL_UINT8(PANIC_PHASE_NONE, r.net_phase);

    panic_diag_rec_mark(&r, PANIC_PHASE_OTA_CHECK, PANIC_PHASE_OTA_CHECK, &SAMPLE);
    /* The main slot MUST survive the network mark. If it did not, the
       breadcrumb would be a single phase byte with extra steps and a
       panic during the update check would erase what the main task was
       doing. */
    TEST_ASSERT_EQUAL_UINT8(PANIC_PHASE_AWAKE, r.main_phase);
    TEST_ASSERT_EQUAL_UINT8(PANIC_PHASE_OTA_CHECK, r.net_phase);
    TEST_ASSERT_TRUE(panic_diag_rec_valid(&r));
}

void test_a_nested_render_puts_the_outer_phase_back(void) {
    /* display.c's render() is entered from inside AWAKE. Leaving the
       record reading RENDER afterwards would mislabel every later panic
       in that wake as a paint. */
    panic_diag_rec_t r;
    panic_diag_rec_reset(&r);
    panic_diag_rec_mark(&r, PANIC_PHASE_AWAKE, PANIC_PHASE_AWAKE, &SAMPLE);
    const panic_phase_t prev = (panic_phase_t)r.main_phase;
    panic_diag_rec_mark(&r, PANIC_PHASE_RENDER, PANIC_PHASE_RENDER, &SAMPLE);
    TEST_ASSERT_EQUAL_UINT8(PANIC_PHASE_RENDER, r.main_phase);
    panic_diag_rec_mark(&r, PANIC_PHASE_RENDER, prev, &SAMPLE);
    TEST_ASSERT_EQUAL_UINT8(PANIC_PHASE_AWAKE, r.main_phase);
}

void test_the_two_stack_figures_stay_attached_to_their_own_tasks(void) {
    /* stack_main and stack_net are high-water marks of different stacks.
       A mark that wrote both would report the network task's floor as
       the main task's. */
    panic_diag_rec_t r;
    panic_diag_rec_reset(&r);
    const panic_sample_t main_s = {.uptime_ms = 100, .heap_free = 80000, .stack_free = 4000};
    const panic_sample_t net_s = {.uptime_ms = 200, .heap_free = 50000, .stack_free = 900};

    panic_diag_rec_mark(&r, PANIC_PHASE_AWAKE, PANIC_PHASE_AWAKE, &main_s);
    panic_diag_rec_mark(&r, PANIC_PHASE_MQTT, PANIC_PHASE_MQTT, &net_s);
    TEST_ASSERT_EQUAL_UINT16(4000, r.stack_main);
    TEST_ASSERT_EQUAL_UINT16(900, r.stack_net);
    /* Uptime and heap are global, so the LAST mark from either side wins
       — that is what makes "uptime at panic" mean how far into the wake
       it died. */
    TEST_ASSERT_EQUAL_UINT32(200, r.uptime_ms);
    TEST_ASSERT_EQUAL_UINT32(50000, r.heap_free);
}

void test_a_mark_on_a_record_that_failed_its_guard_rearms_it(void) {
    /* Total function: the pure layer cannot assume panic_diag_init() ran.
       Re-arming is the only answer that leaves the record self-consistent
       — writing into unvalidated bytes would produce a record that passes
       the guard while carrying garbage in the fields nobody touched. */
    panic_diag_rec_t r;
    memset(&r, 0x77, sizeof(r));
    panic_diag_rec_mark(&r, PANIC_PHASE_BOOT, PANIC_PHASE_BOOT, &SAMPLE);
    TEST_ASSERT_TRUE(panic_diag_rec_valid(&r));
    TEST_ASSERT_EQUAL_UINT8(PANIC_PHASE_BOOT, r.main_phase);
    TEST_ASSERT_EQUAL_UINT8(PANIC_PHASE_NONE, r.net_phase);
    TEST_ASSERT_EQUAL_UINT16(0, r.stack_net); /* not carried over from the garbage */
}

void test_a_mark_without_a_sample_still_moves_the_phase(void) {
    panic_diag_rec_t r;
    panic_diag_rec_reset(&r);
    panic_diag_rec_mark(&r, PANIC_PHASE_SLEEP, PANIC_PHASE_SLEEP, NULL);
    TEST_ASSERT_EQUAL_UINT8(PANIC_PHASE_SLEEP, r.main_phase);
    TEST_ASSERT_TRUE(panic_diag_rec_valid(&r));
}

/* ---- the published view ---- */

void test_no_record_publishes_an_empty_phase_not_none(void) {
    /* The distinction an operator reads at a glance: blank = there is no
       breadcrumb on file (never panicked, or the record failed its
       guard); "NONE" = a panic happened outside every marked phase. */
    diag_stat_t d;
    memset(&d, 0xEE, sizeof(d));
    panic_diag_fill_stat(&d, 0, NULL);
    TEST_ASSERT_EQUAL_UINT32(0, d.panics);
    TEST_ASSERT_EQUAL_STRING("", d.panic_phase);
    TEST_ASSERT_EQUAL_UINT32(0, d.panic_uptime_s);
    TEST_ASSERT_EQUAL_UINT32(0, d.panic_heap);
}

void test_a_corrupt_stored_record_is_not_published_as_evidence(void) {
    /* The blob comes off flash. A short/garbled read must reach HA as
       "no breadcrumb", never as a phase name. */
    panic_diag_rec_t r;
    panic_diag_rec_reset(&r);
    panic_diag_rec_mark(&r, PANIC_PHASE_OTA_DL, PANIC_PHASE_OTA_DL, &SAMPLE);
    r.uptime_ms ^= 0x4000u; /* payload changed, checksum stale */

    diag_stat_t d;
    panic_diag_fill_stat(&d, 9, &r);
    TEST_ASSERT_EQUAL_UINT32(9, d.panics); /* the counter is independent and still reported */
    TEST_ASSERT_EQUAL_STRING("", d.panic_phase);
}

void test_a_good_record_publishes_the_phase_uptime_heap_and_stacks(void) {
    panic_diag_rec_t r;
    panic_diag_rec_reset(&r);
    const panic_sample_t main_s = {.uptime_ms = 1000, .heap_free = 70000, .stack_free = 3500};
    const panic_sample_t net_s = {.uptime_ms = 41500, .heap_free = 21000, .stack_free = 1200};
    panic_diag_rec_mark(&r, PANIC_PHASE_RENDER, PANIC_PHASE_RENDER, &main_s);
    panic_diag_rec_mark(&r, PANIC_PHASE_OTA_CHECK, PANIC_PHASE_OTA_CHECK, &net_s);

    diag_stat_t d;
    panic_diag_fill_stat(&d, 42, &r);
    TEST_ASSERT_EQUAL_UINT32(42, d.panics);
    TEST_ASSERT_EQUAL_STRING("RENDER+OTA_CHECK", d.panic_phase);
    TEST_ASSERT_EQUAL_UINT32(41, d.panic_uptime_s); /* ms -> s, truncating */
    TEST_ASSERT_EQUAL_UINT32(21000, d.panic_heap);
    TEST_ASSERT_EQUAL_UINT16(3500, d.panic_stack_main);
    TEST_ASSERT_EQUAL_UINT16(1200, d.panic_stack_net);
}

void test_the_ota_hypothesis_is_answerable_from_the_published_phase_alone(void) {
    /* The reason this feature was built. A panic inside the update
       check/download and a panic nowhere near the radio must produce two
       obviously different strings — no shared prefix, no need to read a
       second sensor to tell them apart. */
    panic_diag_rec_t in_ota, out_of_ota;
    panic_diag_rec_reset(&in_ota);
    panic_diag_rec_reset(&out_of_ota);
    panic_diag_rec_mark(&in_ota, PANIC_PHASE_OTA_DL, PANIC_PHASE_OTA_DL, &SAMPLE);
    panic_diag_rec_mark(&out_of_ota, PANIC_PHASE_RENDER, PANIC_PHASE_RENDER, &SAMPLE);

    diag_stat_t a, b;
    panic_diag_fill_stat(&a, 1, &in_ota);
    panic_diag_fill_stat(&b, 1, &out_of_ota);
    TEST_ASSERT_EQUAL_STRING("OTA_DL", a.panic_phase);
    TEST_ASSERT_EQUAL_STRING("RENDER", b.panic_phase);
    TEST_ASSERT_NOT_EQUAL(0, strcmp(a.panic_phase, b.panic_phase));
}

void test_every_phase_pair_fits_the_published_field(void) {
    /* The _Static_assert in panic_diag.c compares two hardcoded literals
       and therefore does NOT catch a longer phase name being added — see
       the comment above it. This walks the real table instead, so adding
       PANIC_PHASE_OTA_ROLLBACK ("OTA_ROLLBACK_CHECK") fails HERE rather
       than shipping a label snprintf-truncated to something that still
       reads like a valid phase in HA. */
    for (int m = 0; m < PANIC_PHASE__COUNT; m++) {
        for (int n = 0; n < PANIC_PHASE__COUNT; n++) {
            char buf[DIAG_PHASE_MAX];
            const int want = panic_diag_phase_label(buf, sizeof(buf), (uint8_t)m, (uint8_t)n);
            /* snprintf answers what it WOULD have written. Equal to the
               buffer size is already truncation. */
            TEST_ASSERT_TRUE_MESSAGE(want < (int)sizeof(buf), "a phase pair no longer fits DIAG_PHASE_MAX - grow it");
            TEST_ASSERT_EQUAL_INT(want, (int)strlen(buf));
        }
    }
}

void test_the_stored_phase_numbers_are_pinned_because_they_outlive_the_image(void) {
    /* These values are written to RTC memory AND to an NVS blob, so a
       field device that panicked under an older image hands them to a
       newer one. Renumbering the enum decodes those records as the wrong
       phase — silently, and only on the devices that actually panicked.
       Append; do not reorder. The name<->symbol tests elsewhere in this
       file all survive a renumbering, which is why this one exists. */
    TEST_ASSERT_EQUAL_INT(0, PANIC_PHASE_NONE);
    TEST_ASSERT_EQUAL_INT(1, PANIC_PHASE_BOOT);
    TEST_ASSERT_EQUAL_INT(2, PANIC_PHASE_AWAKE);
    TEST_ASSERT_EQUAL_INT(3, PANIC_PHASE_RENDER);
    TEST_ASSERT_EQUAL_INT(4, PANIC_PHASE_SLEEP);
    TEST_ASSERT_EQUAL_INT(5, PANIC_PHASE_NET);
    TEST_ASSERT_EQUAL_INT(6, PANIC_PHASE_OTA_CHECK);
    TEST_ASSERT_EQUAL_INT(7, PANIC_PHASE_MQTT);
    TEST_ASSERT_EQUAL_INT(8, PANIC_PHASE_OTA_DL);
    /* The BOOT subdivision, appended in 2026-08 rather than slotted in
       next to PANIC_PHASE_BOOT where it would read in wake order. A
       device that panicked under the image before it has records on
       flash carrying 1..8; inserting BOOT_NVS at 2 would decode every
       one of those as the phase after the one it really was. */
    TEST_ASSERT_EQUAL_INT(9, PANIC_PHASE_BOOT_NVS);
    TEST_ASSERT_EQUAL_INT(10, PANIC_PHASE_BOOT_OTA);
    TEST_ASSERT_EQUAL_INT(11, PANIC_PHASE_BOOT_DISP);
    TEST_ASSERT_EQUAL_INT(12, PANIC_PHASE_BOOT_BATT);
    TEST_ASSERT_EQUAL_INT(13, PANIC_PHASE_BOOT_LOCK);
    TEST_ASSERT_EQUAL_INT(14, PANIC_PHASE__COUNT);
}

void test_leaving_a_phase_nobody_disturbed_restores_the_outer_one(void) {
    /* The ordinary render()-inside-AWAKE case, now through the exit
       path rather than a raw mark. */
    panic_diag_rec_t r;
    panic_diag_rec_reset(&r);
    panic_diag_rec_mark(&r, PANIC_PHASE_AWAKE, PANIC_PHASE_AWAKE, &SAMPLE);
    panic_diag_rec_mark(&r, PANIC_PHASE_RENDER, PANIC_PHASE_RENDER, &SAMPLE);

    TEST_ASSERT_TRUE(panic_diag_rec_exit(&r, PANIC_PHASE_RENDER, PANIC_PHASE_AWAKE, &SAMPLE));
    TEST_ASSERT_EQUAL_UINT8(PANIC_PHASE_AWAKE, r.main_phase);
    TEST_ASSERT_TRUE(panic_diag_rec_valid(&r));
}

void test_a_late_exit_does_not_put_a_stale_phase_back_over_the_sleep_funnel(void) {
    /* The awake failsafe fires on the esp_timer task and marks SLEEP
       while a render() started on ota_dl is still inside its 2-4 s e-ink
       refresh. When that refresh finally returns, its exit must NOT
       restore AWAKE: a panic in the sleep funnel would then publish
       "AWAKE+OTA_DL" and point the reader at the wake handler instead of
       at the funnel that was actually running. This is the failure the
       breadcrumb exists to avoid, on the one path where the device is
       already known to be wedged. */
    panic_diag_rec_t r;
    panic_diag_rec_reset(&r);
    panic_diag_rec_mark(&r, PANIC_PHASE_AWAKE, PANIC_PHASE_AWAKE, &SAMPLE);
    panic_diag_rec_mark(&r, PANIC_PHASE_OTA_DL, PANIC_PHASE_OTA_DL, &SAMPLE);
    panic_diag_rec_mark(&r, PANIC_PHASE_RENDER, PANIC_PHASE_RENDER, &SAMPLE); /* ota_dl paints */

    panic_diag_rec_mark(&r, PANIC_PHASE_SLEEP, PANIC_PHASE_SLEEP, &SAMPLE); /* failsafe, other task */

    TEST_ASSERT_FALSE(panic_diag_rec_exit(&r, PANIC_PHASE_RENDER, PANIC_PHASE_AWAKE, &SAMPLE));
    TEST_ASSERT_EQUAL_UINT8(PANIC_PHASE_SLEEP, r.main_phase);
    TEST_ASSERT_EQUAL_UINT8(PANIC_PHASE_OTA_DL, r.net_phase);

    diag_stat_t st;
    panic_diag_fill_stat(&st, 1, &r);
    TEST_ASSERT_EQUAL_STRING("SLEEP+OTA_DL", st.panic_phase);
}

void test_a_refused_exit_leaves_the_record_completely_alone(void) {
    /* Not just the phase: the task that owns the slot now is the one
       whose uptime and heap should be published, and a late exit has
       nothing to add. */
    panic_diag_rec_t r;
    panic_diag_rec_reset(&r);
    panic_diag_rec_mark(&r, PANIC_PHASE_RENDER, PANIC_PHASE_RENDER, &SAMPLE);
    const panic_sample_t owner = {.uptime_ms = 5000, .heap_free = 61000, .stack_free = 3000};
    panic_diag_rec_mark(&r, PANIC_PHASE_SLEEP, PANIC_PHASE_SLEEP, &owner);

    const panic_sample_t late = {.uptime_ms = 9999, .heap_free = 11111, .stack_free = 77};
    TEST_ASSERT_FALSE(panic_diag_rec_exit(&r, PANIC_PHASE_RENDER, PANIC_PHASE_AWAKE, &late));
    TEST_ASSERT_EQUAL_UINT32(5000, r.uptime_ms);
    TEST_ASSERT_EQUAL_UINT32(61000, r.heap_free);
    TEST_ASSERT_EQUAL_UINT16(3000, r.stack_main);
    TEST_ASSERT_TRUE(panic_diag_rec_valid(&r));
}

void test_an_exit_on_a_record_that_failed_its_guard_rearms_without_restoring(void) {
    panic_diag_rec_t r;
    memset(&r, 0xFF, sizeof(r));
    TEST_ASSERT_FALSE(panic_diag_rec_exit(&r, PANIC_PHASE_RENDER, PANIC_PHASE_AWAKE, &SAMPLE));
    TEST_ASSERT_TRUE(panic_diag_rec_valid(&r));
    /* Re-armed, not restored: the phase the corrupt record claimed to
       hold is not evidence, so AWAKE must not appear. */
    TEST_ASSERT_EQUAL_UINT8(PANIC_PHASE_NONE, r.main_phase);
}

void test_the_exit_path_answers_null_rather_than_dereferencing_it(void) {
    TEST_ASSERT_FALSE(panic_diag_rec_exit(NULL, PANIC_PHASE_RENDER, PANIC_PHASE_AWAKE, &SAMPLE));
}

void test_a_mark_from_a_task_that_does_not_own_the_slot_leaves_its_stack_alone(void) {
    /* render() also runs from ota_dl, whose stack is 16 KB against the
       main task's 7 KB. Writing ota_dl's high-water mark into stack_main
       publishes a figure the main task cannot physically produce, under
       a label that says it did. The phase still moves — only the stack
       figure is withheld. */
    panic_diag_rec_t r;
    panic_diag_rec_reset(&r);
    const panic_sample_t mine = {.uptime_ms = 100, .heap_free = 80000, .stack_free = 2600};
    const panic_sample_t theirs = {.uptime_ms = 200, .heap_free = 79000, .stack_free = 14200, .stack_foreign = true};

    panic_diag_rec_mark(&r, PANIC_PHASE_AWAKE, PANIC_PHASE_AWAKE, &mine);
    panic_diag_rec_mark(&r, PANIC_PHASE_RENDER, PANIC_PHASE_RENDER, &theirs);

    TEST_ASSERT_EQUAL_UINT8(PANIC_PHASE_RENDER, r.main_phase);
    TEST_ASSERT_EQUAL_UINT16(2600, r.stack_main);
    /* Uptime and heap are global readings, not per-task ones, so they
       are still taken from whoever marked last. */
    TEST_ASSERT_EQUAL_UINT32(200, r.uptime_ms);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_every_phase_has_a_distinct_name);
    RUN_TEST(test_the_phases_the_brief_asked_for_are_all_separable);
    RUN_TEST(test_each_boot_subdivision_names_one_init_call);
    RUN_TEST(test_the_reachable_boot_lock_pairing_fills_the_published_field_exactly);
    RUN_TEST(test_a_phase_this_image_does_not_know_is_not_reported_as_one_it_does);
    RUN_TEST(test_the_slot_map_puts_every_network_phase_on_the_network_side);

    RUN_TEST(test_both_sides_busy_reports_both);
    RUN_TEST(test_one_side_idle_reports_only_the_other);
    RUN_TEST(test_neither_side_in_a_phase_is_a_reading_not_a_blank);
    RUN_TEST(test_the_worst_case_label_fits_the_published_field);
    RUN_TEST(test_every_phase_pair_fits_the_published_field);
    RUN_TEST(test_the_stored_phase_numbers_are_pinned_because_they_outlive_the_image);
    RUN_TEST(test_the_label_answers_a_zero_length_buffer_rather_than_writing_to_it);

    RUN_TEST(test_a_freshly_reset_record_is_valid);
    RUN_TEST(test_uninitialised_rtc_memory_does_not_pass_the_guard);
    RUN_TEST(test_the_magic_alone_is_not_enough);
    RUN_TEST(test_the_checksum_alone_is_not_enough);
    RUN_TEST(test_a_single_flipped_bit_anywhere_in_the_payload_is_caught);
    RUN_TEST(test_the_guard_answers_null_rather_than_dereferencing_it);

    RUN_TEST(test_a_mark_lands_in_the_slot_the_phase_owns);
    RUN_TEST(test_a_nested_render_puts_the_outer_phase_back);
    RUN_TEST(test_the_two_stack_figures_stay_attached_to_their_own_tasks);
    RUN_TEST(test_a_mark_on_a_record_that_failed_its_guard_rearms_it);
    RUN_TEST(test_a_mark_without_a_sample_still_moves_the_phase);
    RUN_TEST(test_a_mark_from_a_task_that_does_not_own_the_slot_leaves_its_stack_alone);

    RUN_TEST(test_leaving_a_phase_nobody_disturbed_restores_the_outer_one);
    RUN_TEST(test_a_late_exit_does_not_put_a_stale_phase_back_over_the_sleep_funnel);
    RUN_TEST(test_a_refused_exit_leaves_the_record_completely_alone);
    RUN_TEST(test_an_exit_on_a_record_that_failed_its_guard_rearms_without_restoring);
    RUN_TEST(test_the_exit_path_answers_null_rather_than_dereferencing_it);

    RUN_TEST(test_no_record_publishes_an_empty_phase_not_none);
    RUN_TEST(test_a_corrupt_stored_record_is_not_published_as_evidence);
    RUN_TEST(test_a_good_record_publishes_the_phase_uptime_heap_and_stacks);
    RUN_TEST(test_the_ota_hypothesis_is_answerable_from_the_published_phase_alone);
    return UNITY_END();
}
