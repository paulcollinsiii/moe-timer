#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include <unity.h>

/* Single-TU: the REAL policy layer and the REAL NVS accessors (over the
   mock store) underneath the flow. Only the device effects are injected.
   That split is deliberate — what this suite exercises is the ORDER the
   effects happen in, and an order is only worth asserting when the
   decisions driving it are the ones the firmware will actually make. A
   mocked ota_policy would let a wrong sequence pass by agreeing with
   itself.

   The ordering assertions are string compares against a call log rather
   than a pile of counters, because the properties being pinned ARE
   sequences: paint before the radio, extend before the window, abort
   instead of commit. A counter can say each happened; only the log says
   they happened in the order that keeps the panel from browning out the
   rail and the boot partition from pointing at half an image. */
// clang-format off
#include "cJSON.h"
#include "mock_hal_nvs.c"
#include "../../main/config_validate.c"
#include "../../main/nvs_config.c"
#include "../../main/ota_policy.c"
// clang-format on

/* The flow's own header comes before the mocks below, which have to name
   its types; ota_flow.c itself is included after them. */
#include "ota_flow.h"

/* The device id is MAC-derived (layer 3); the flow only forwards it to
   the manifest resolver. */
static const char *s_dev_id = "magtag-a1b2c3";
const char *device_id(void) {
    return s_dev_id;
}

/* ---- the call log ------------------------------------------------------- */

static char g_log[512];

static void note(const char *tag) {
    size_t used = strlen(g_log);
    if (used > 0 && used + 1 < sizeof(g_log)) {
        g_log[used++] = ' ';
        g_log[used] = '\0';
    }
    snprintf(g_log + used, sizeof(g_log) - used, "%s", tag);
}

/* The failsafe extension logs its VALUE, not just its occurrence: the
   sequence has two extensions with opposite meanings (open the download
   budget, restore the ordinary one) and "extend extend" would read as
   correct no matter which order they came in. */
static void note_int(const char *tag, int value) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%s:%d", tag, value);
    note(buf);
}

/* ---- injected effects --------------------------------------------------- */

#define RUNNING "1.5.0"
#define TARGET "1.6.0"
#define MANIFEST_URL "https://ota.test/magtag.json"
#define IMAGE_URL "https://ota.test/magtag-1.6.0.bin"

/* A THIRD version, for the cases where an attempt happens between a
   commit and its revert: the retry budget's key moves to this one while
   the record of what was committed must not. */
#define NEXT "1.7.0"
#define NEXT_IMAGE_URL "https://ota.test/magtag-1.7.0.bin"

#define MANIFEST_UPDATE "[{\"schema\":1,\"default\":{\"version\":\"" TARGET "\",\"url\":\"" IMAGE_URL "\"}}]"
#define MANIFEST_NEXT "[{\"schema\":1,\"default\":{\"version\":\"" NEXT "\",\"url\":\"" NEXT_IMAGE_URL "\"}}]"
#define MANIFEST_SAME "[{\"schema\":1,\"default\":{\"version\":\"" RUNNING "\",\"url\":\"" IMAGE_URL "\"}}]"
#define MANIFEST_PINNED "[{\"schema\":1,\"default\":{\"version\":null}}]"

static const char *m_body; /* NULL = the GET fails with m_body_facts */
static ota_error_facts_t m_body_facts;
static char m_manifest_url_seen[OTA_URL_MAX];
static char m_image_url_seen[OTA_URL_MAX];

static int mock_manifest_get(const char *url, char *buf, size_t len, ota_error_facts_t *facts) {
    note("get");
    snprintf(m_manifest_url_seen, sizeof(m_manifest_url_seen), "%s", url);
    if (m_body == NULL) {
        *facts = m_body_facts;
        return -1;
    }
    size_t n = strlen(m_body);
    if (n > len)
        n = len;
    memcpy(buf, m_body, n);
    return (int)n;
}

static bool m_begin_ok;
static ota_error_facts_t m_begin_facts;
static int m_steps_to_done; /* MORE this many times, then DONE */
static bool m_step_fails;
static ota_error_facts_t m_step_facts;
static int m_step_advance_ms; /* how much monotonic time one chunk costs */
static bool m_finish_ok;
static ota_error_facts_t m_finish_facts;
static bool m_session_ok;
static uint32_t m_heap;
static int64_t m_mono;

/* When the awake failsafe would fire, on the same clock as m_mono. The
   extension is not merely logged, it is MODELLED, because "the failsafe
   outlasts the download's own deadline" is a relationship between two
   numbers and a pure logger cannot see one — which is exactly how a
   deadline that could never be reached shipped. */
static int64_t m_failsafe_at;

static int64_t mock_mono_ms(void) {
    return m_mono;
}

/* The awake failsafe firing inside dl_begin, modelled honestly: main.c's
   awake_failsafe_cb calls esp_deep_sleep_start(), which DOES NOT RETURN.
   A mock that returned false instead would be modelling an observed
   failure, which is the one case that was never in question.

   dl_begin is where this belongs. It sits outside the loop ota_flow
   bounds, and two verified paths inside it run without any bound of
   their own: esp_https_ota's connect loop has no hop counter
   (esp_https_ota.c:166-218), and its read_header retries EAGAIN forever
   against a peer that sends headers and then stalls
   (esp_https_ota.c:591-617). */
static jmp_buf m_wake_killed;
static bool m_kill_in_dl_begin;

static bool mock_dl_begin(const char *url, ota_error_facts_t *facts) {
    note("dl_begin");
    snprintf(m_image_url_seen, sizeof(m_image_url_seen), "%s", url);
    if (m_kill_in_dl_begin)
        longjmp(m_wake_killed, 1);
    if (!m_begin_ok) {
        *facts = m_begin_facts;
        return false;
    }
    return true;
}

static ota_step_t mock_dl_step(ota_error_facts_t *facts) {
    note("step");
    /* Always advances. The loop's only bound is the deadline, so a step
       that cost no time would spin forever here — on the device
       esp_https_ota_perform() blocks on the socket, which is why the real
       loop needs no iteration cap. */
    m_mono += m_step_advance_ms;
    if (m_step_fails) {
        *facts = m_step_facts;
        return OTA_STEP_FAIL;
    }
    if (--m_steps_to_done <= 0)
        return OTA_STEP_DONE;
    return OTA_STEP_MORE;
}

/* Captured AT the commit, not read afterwards, for the same reason the
   abort captures its pair below: the commit tail re-arms the failsafe on
   the way in and fail_attempt re-arms it again on the way out, so a
   reading taken after ota_flow_apply returns says nothing about the
   headroom the two SHA-256 passes inside dl_finish actually ran with. */
static int64_t m_finish_mono;
static int64_t m_finish_failsafe_at;

static bool mock_dl_finish(ota_error_facts_t *facts) {
    note("finish");
    m_finish_mono = m_mono;
    m_finish_failsafe_at = m_failsafe_at;
    if (!m_finish_ok) {
        *facts = m_finish_facts;
        return false;
    }
    return true;
}

/* Captured AT the abort, not read afterwards: fail_attempt() re-arms the
   failsafe for the ordinary awake budget a few lines later, which would
   erase the very relationship under test. */
static int64_t m_abort_mono;
static int64_t m_abort_failsafe_at;

static void mock_dl_abort(void) {
    note("abort");
    m_abort_mono = m_mono;
    m_abort_failsafe_at = m_failsafe_at;
}

/* Association is not free, and the mock has to say so. The awake failsafe
   is re-armed BEFORE the session opens and the loop's deadline is
   measured against the same clock, so a mock whose session_begin cost
   nothing cannot express the one relationship that matters between them:
   on the device wifi_session_begin() blocks for up to
   WIFI_CONNECT_TIMEOUT_MS (15 s) and every millisecond of it is already
   burning against the failsafe. 5 s is a healthy association. */
#define ASSOCIATION_MS 5000

static bool mock_session_begin(void) {
    note("net_on");
    m_mono += ASSOCIATION_MS;
    return m_session_ok;
}

static void mock_session_end(void) {
    note("net_off");
}

static char m_paint_from[32], m_paint_to[32];

/* The instant the download's budget is anchored at. ota_flow_apply takes
   budget_start immediately after this paint returns and nothing advances
   the clock in between, so capturing it here reconstructs the deadline
   exactly — which is what lets a test assert against the WORST case
   overshoot rather than whatever the chunk alignment happened to
   produce. */
static int64_t m_budget_start;

static void mock_paint_update(const char *from, const char *to) {
    note("paint");
    m_budget_start = m_mono;
    snprintf(m_paint_from, sizeof(m_paint_from), "%s", from != NULL ? from : "(null)");
    snprintf(m_paint_to, sizeof(m_paint_to), "%s", to != NULL ? to : "(null)");
}

static void mock_repaint(void) {
    note("repaint");
}

/* main.c's extend_awake_failsafe() is an esp_timer_stop followed by a
   start_once(seconds): the failsafe is re-armed ABSOLUTELY, from the
   moment of the call. The mock models exactly that, so a test can ask
   whether the download's own deadline still fits inside it.

   m_extend_ok models the OTHER outcome main.c can produce: the failsafe
   timer was never created (esp_timer_create or start_once failed at
   boot, which arm_awake_failsafe only logs), so the extender null-guards
   and does nothing at all. Modelled as "returns false AND moves no
   clock", because that is the point — a device in that state has no
   bound on the wake whatsoever. */
static bool m_extend_ok;

static bool mock_extend_awake(int seconds) {
    note_int("extend", seconds);
    if (!m_extend_ok)
        return false;
    m_failsafe_at = m_mono + (int64_t)seconds * 1000;
    return true;
}

/* The pre-reboot snapshot flush (timer_persist_save on the device). Its
   ORDER is the whole property — after the radio is down, before the
   restart that wipes RTC memory — so it goes in the call log like every
   other effect, and the counter is only here to keep "exactly once"
   honest. */
static int m_persists;
static bool m_persisted_before_restart;

static void mock_persist_state(void) {
    note("persist");
    m_persists++;
}

static void mock_restart(void) {
    note("restart");
    m_persisted_before_restart = (m_persists > 0);
}

static uint32_t mock_free_heap(void) {
    return m_heap;
}

/* The rollback confirmation (esp_ota_mark_app_valid_cancel_rollback on
   the device, via ota.c's ota_mark_valid_if_pending). Counted rather than
   logged: the cases below are about whether it happened at all, and it
   never shares a sequence with the download effects — it runs on the
   NEXT boot, the one that came up on what the download wrote. */
static int m_marks_valid;

static void mock_mark_valid(void) {
    note("mark_valid");
    m_marks_valid++;
}

// clang-format off
#include "../../main/ota_flow.c"
// clang-format on

static const ota_flow_ops_t OPS = {
    .manifest_get = mock_manifest_get,
    .dl_begin = mock_dl_begin,
    .dl_step = mock_dl_step,
    .dl_finish = mock_dl_finish,
    .dl_abort = mock_dl_abort,
    .session_begin = mock_session_begin,
    .session_end = mock_session_end,
    .paint_update = mock_paint_update,
    .repaint = mock_repaint,
    .extend_awake = mock_extend_awake,
    .persist_state = mock_persist_state,
    .restart = mock_restart,
    .free_heap = mock_free_heap,
    .mono_ms = mock_mono_ms,
    .mark_valid = mock_mark_valid,
};

/* Mirrors the Kconfig defaults, but nothing keeps them in step and
   nothing needs to: these are the values the assertions below are
   written against, and the firmware passes its own. */
static const ota_flow_cfg_t CFG = {
    .max_sec = 300,
    .awake_sec = 180,
    .min_batt_pct = 30,
    .max_fails = 3,
    .min_free_heap = 40000,
    .running_version = RUNNING,
};

void setUp(void) {
    mock_nvs_reset();
    g_log[0] = '\0';
    m_manifest_url_seen[0] = '\0';
    m_image_url_seen[0] = '\0';
    m_paint_from[0] = '\0';
    m_paint_to[0] = '\0';
    m_body = MANIFEST_UPDATE;
    memset(&m_body_facts, 0, sizeof(m_body_facts));
    m_begin_ok = true;
    memset(&m_begin_facts, 0, sizeof(m_begin_facts));
    m_steps_to_done = 2;
    m_step_fails = false;
    memset(&m_step_facts, 0, sizeof(m_step_facts));
    m_step_advance_ms = 10;
    m_finish_ok = true;
    memset(&m_finish_facts, 0, sizeof(m_finish_facts));
    m_session_ok = true;
    m_heap = 120000;
    m_mono = 1000;
    m_failsafe_at = 0;
    m_abort_mono = 0;
    m_abort_failsafe_at = 0;
    m_budget_start = 0;
    m_finish_mono = 0;
    m_finish_failsafe_at = 0;
    m_extend_ok = true;
    m_persists = 0;
    m_persisted_before_restart = false;
    m_kill_in_dl_begin = false;
    m_marks_valid = 0;
    s_dev_id = "magtag-a1b2c3";
    ota_flow_init(&OPS, &CFG);
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_ota_url(MANIFEST_URL));
}

void tearDown(void) {}

/* ---- helpers ------------------------------------------------------------ */

/* A healthy rollover wake: battery fine, no charge lock, clock settled. */
static void check_at_rollover(void) {
    ota_flow_arm(OTA_TRIGGER_ROLLOVER, 90, false);
    ota_flow_check(true);
}

static const char *stored_result(void) {
    static char buf[CFG_BOUND_OTA_RESULT_MAX];
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_ota_result(buf, sizeof(buf)));
    return buf;
}

static const char *stored_target(void) {
    static char buf[CFG_BOUND_OTA_TARGET_MAX];
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_ota_target(buf, sizeof(buf)));
    return buf;
}

static uint16_t stored_fails(void) {
    uint16_t n = 0;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_ota_fails(&n));
    return n;
}

static uint32_t stored_dl_ms(void) {
    uint32_t n = 0;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_ota_dl_ms(&n));
    return n;
}

/* The certification token: the version that was committed, "" when there
   is nothing outstanding. Deliberately a different key from ota_target —
   see the misattribution cases below. */
static const char *stored_pend_ver(void) {
    static char buf[CFG_BOUND_OTA_TARGET_MAX];
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_ota_pend_ver(buf, sizeof(buf)));
    return buf;
}

/* A boot of the firmware whose app descriptor says `ver`. Everything
   about a rollback is a disagreement between the version that was
   committed and the version that came up, so the tests below need to
   choose the latter. */
static void boot_running(const char *ver) {
    ota_flow_cfg_t cfg = CFG;
    cfg.running_version = ver;
    ota_flow_init(&OPS, &cfg);
}

/* Run an apply that the awake failsafe kills part-way. The setjmp frame
   stands in for "the device deep-slept and this call never returned" —
   nothing after the kill point in ota_flow_apply runs, which is exactly
   the situation the pre-charge exists to survive. */
static void apply_and_let_the_failsafe_kill_it(void) {
    if (setjmp(m_wake_killed) == 0)
        ota_flow_apply(90, false);
}

/* ---- triggers ----------------------------------------------------------- */

static void test_rollover_always_checks(void) {
    check_at_rollover();
    TEST_ASSERT_EQUAL_STRING("get", g_log);
    TEST_ASSERT_TRUE(ota_flow_pending());
}

static void test_plain_tick_wake_never_checks(void) {
    ota_flow_arm(OTA_TRIGGER_NONE, 90, false);
    ota_flow_check(true);
    TEST_ASSERT_EQUAL_STRING("", g_log);
    TEST_ASSERT_FALSE(ota_flow_pending());
    /* Not merely "no download": no radio time either. The common wake is
       the one this feature must stay out of. */
    TEST_ASSERT_EQUAL_STRING("", stored_result());
}

static void test_button_d_checks_only_when_ota_on_sync_is_set(void) {
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_ota_on_sync(0));
    ota_flow_arm(OTA_TRIGGER_SYNC, 90, false);
    ota_flow_check(true);
    TEST_ASSERT_EQUAL_STRING("", g_log);

    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_ota_on_sync(1));
    ota_flow_arm(OTA_TRIGGER_SYNC, 90, false);
    ota_flow_check(true);
    TEST_ASSERT_EQUAL_STRING("get", g_log);
    TEST_ASSERT_TRUE(ota_flow_pending());
}

/* Arming for a check is what clears the previous decision — the check
   about to run is entitled to replace it. Without the clear, a manifest
   that has since withdrawn the offer would leave yesterday's answer
   standing. */
static void test_arming_a_check_clears_the_previous_buffered_update(void) {
    check_at_rollover();
    TEST_ASSERT_TRUE(ota_flow_pending());

    m_body = MANIFEST_SAME; /* the offer is withdrawn */
    check_at_rollover();
    TEST_ASSERT_FALSE(ota_flow_pending());
}

/* The other half, and the one that costs a real update if it regresses.
   TWO network sessions in one wake is the routine path, not the exotic
   one — wifi_session.c:95 names "day rollover + mandatory start sync" as
   the ordinary example, and net_apply_open() has two call sites
   (wake_flow.c:349, :1068). The rollover window buffers 1.6.0, the
   operator presses Button A, and the second window arms with a trigger
   that will not check. An arm that clears unconditionally would drop the
   update on the floor and the device would sit a day behind for no
   reason anybody could see. Nothing is lost by keeping it: s_armed is
   already false, so no second check can run either way. */
static void test_a_second_window_in_the_same_wake_keeps_the_buffered_update(void) {
    check_at_rollover();
    TEST_ASSERT_TRUE(ota_flow_pending());

    ota_flow_arm(OTA_TRIGGER_NONE, 90, false);
    TEST_ASSERT_TRUE(ota_flow_pending());

    /* And it is still the whole buffer, not just the flag. */
    ota_flow_apply(90, false);
    TEST_ASSERT_EQUAL_STRING(TARGET, m_paint_to);
    TEST_ASSERT_EQUAL_STRING(IMAGE_URL, m_image_url_seen);
}

/* Whatever happens below the arm, exactly one check runs per wake: the
   check spends a TLS handshake, and a second window in the same wake
   must not spend another. */
static void test_only_one_check_runs_per_wake(void) {
    ota_flow_arm(OTA_TRIGGER_ROLLOVER, 90, false);
    ota_flow_check(true);
    ota_flow_check(true);
    TEST_ASSERT_EQUAL_STRING("get", g_log);
}

/* ---- the check gate ----------------------------------------------------- */

static void test_empty_endpoint_disables_the_check_entirely(void) {
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_ota_url(""));
    check_at_rollover();
    TEST_ASSERT_EQUAL_STRING("", g_log);
    /* no_url is not persistable: an unconfigured device is not a failed
       one, and writing it would overwrite a real earlier result. */
    TEST_ASSERT_EQUAL_STRING("", stored_result());
}

/* The one that protects the evidence. A download failure happens after
   MQTT has closed, so it can only be published by the NEXT window — and
   the next window may well be one where the clock has not settled yet.
   If a routine skip overwrote ota_result, the failure would never be
   seen at all. */
static void test_no_ntp_skips_the_check_and_leaves_an_earlier_failure_alone(void) {
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_ota_result("timeout"));
    ota_flow_arm(OTA_TRIGGER_ROLLOVER, 90, false);
    ota_flow_check(false);
    TEST_ASSERT_EQUAL_STRING("", g_log);
    TEST_ASSERT_EQUAL_STRING("timeout", stored_result());
}

static void test_charge_lock_skips_the_check(void) {
    ota_flow_arm(OTA_TRIGGER_ROLLOVER, 90, true);
    ota_flow_check(true);
    TEST_ASSERT_EQUAL_STRING("", g_log);
    TEST_ASSERT_EQUAL_STRING("", stored_result());
}

/* low_batt IS recorded, unlike the skips above: "I could have updated but
   the cell was too low" is a fact the operator wants to see in Home
   Assistant, and it is not overwriting anything more informative. */
static void test_low_battery_skips_the_check_and_says_so(void) {
    ota_flow_arm(OTA_TRIGGER_ROLLOVER, 12, false);
    ota_flow_check(true);
    TEST_ASSERT_EQUAL_STRING("", g_log);
    TEST_ASSERT_EQUAL_STRING("low_batt", stored_result());
}

/* A flaky ADC must not become a permanent OTA block. */
static void test_unreadable_battery_does_not_gate(void) {
    ota_flow_arm(OTA_TRIGGER_ROLLOVER, -1, false);
    ota_flow_check(true);
    TEST_ASSERT_EQUAL_STRING("get", g_log);
    TEST_ASSERT_TRUE(ota_flow_pending());
}

/* ---- the check itself --------------------------------------------------- */

static void test_the_check_fetches_the_configured_endpoint(void) {
    check_at_rollover();
    TEST_ASSERT_EQUAL_STRING(MANIFEST_URL, m_manifest_url_seen);
}

/* The result is in NVS before the call returns, which is the half of the
   "check before MQTT" contract that lives in this module: the stat
   payload is built downstream of it, so a failure reaches Home Assistant
   in the same window that produced it. The other half — that net_window
   calls this between the rendezvous and mqtt_ha_window — is a call-site
   property, asserted where that wiring lands. */
static void test_the_check_records_its_result_before_returning(void) {
    m_body = NULL;
    m_body_facts.http_status = 404;
    ota_flow_arm(OTA_TRIGGER_ROLLOVER, 90, false);
    ota_flow_check(true);
    TEST_ASSERT_EQUAL_STRING("http_404", stored_result());
}

/* A manifest that never arrived names no version, so there is nothing for
   a retry budget to count against. Counting it anyway would let a
   week of DNS trouble burn the budget for a build the device has never
   even been offered. */
static void test_a_failed_manifest_fetch_costs_no_retry_budget(void) {
    m_body = NULL;
    m_body_facts.transport_failed = true;
    check_at_rollover();
    TEST_ASSERT_EQUAL_STRING("net", stored_result());
    TEST_ASSERT_EQUAL_UINT16(0, stored_fails());
    TEST_ASSERT_EQUAL_STRING("", stored_target());
    TEST_ASSERT_FALSE(ota_flow_pending());
}

static void test_up_to_date_buffers_nothing_and_records_nothing(void) {
    m_body = MANIFEST_SAME;
    check_at_rollover();
    TEST_ASSERT_FALSE(ota_flow_pending());
    TEST_ASSERT_EQUAL_STRING("", stored_result());
}

static void test_a_pinned_device_buffers_nothing(void) {
    m_body = MANIFEST_PINNED;
    check_at_rollover();
    TEST_ASSERT_FALSE(ota_flow_pending());
    TEST_ASSERT_EQUAL_STRING("", stored_result());
}

static void test_a_malformed_manifest_is_recorded(void) {
    m_body = "{\"schema\":1}";
    check_at_rollover();
    TEST_ASSERT_FALSE(ota_flow_pending());
    TEST_ASSERT_EQUAL_STRING("bad_manifest", stored_result());
}

/* A body that overruns the reader, built so the copy fills every byte of
   s_manifest and the LAST one is not a NUL. That is the whole contract
   between ota_flow.c and ota_policy.c: the byte count is authoritative
   and the buffer is never terminated on the caller's behalf. A version
   of this module that reached for strlen() instead would read straight
   off the end of the static — invisible in every other case here,
   because every other manifest happens to leave a zero behind it, and a
   global-buffer-overflow under ASan in this one.

   The truncation itself must be reported, not silently half-parsed: the
   cut lands mid-string, cJSON refuses it, and bad_manifest is a loud,
   published failure rather than a targeting table read halfway. */
static char m_big[4096];

static void fill_the_manifest_buffer(void) {
    int n = snprintf(m_big, sizeof(m_big), "[{\"schema\":1,\"default\":{\"version\":\"%s\",\"url\":\"%s\"},\"pad\":\"",
                     TARGET, IMAGE_URL);
    TEST_ASSERT_TRUE(n > 0 && (size_t)n < sizeof(m_big));
    memset(m_big + n, 'x', 3000);
    m_big[n + 3000] = '\0';
    m_body = m_big;
}

static void test_a_manifest_that_exactly_fills_the_buffer_is_rejected(void) {
    fill_the_manifest_buffer();
    check_at_rollover();
    TEST_ASSERT_EQUAL_STRING("bad_manifest", stored_result());
    TEST_ASSERT_FALSE(ota_flow_pending());
}

/* s_manifest is a static and nothing clears it between fetches, so a
   short manifest is always read out of a buffer still holding the tail
   of a longer one. That is safe only because the length is honoured, and
   "only because" is the kind of thing that should be pinned rather than
   left to luck: the second decision here must come from the second body
   alone, and a strlen-shaped reader would run past the end of a static
   the previous fetch filled to the last byte. */
static void test_a_shorter_manifest_after_a_full_one_is_read_at_its_own_length(void) {
    fill_the_manifest_buffer();
    check_at_rollover();
    TEST_ASSERT_EQUAL_STRING("bad_manifest", stored_result());

    m_body = MANIFEST_UPDATE; /* ~90 bytes into a buffer holding 2048 */
    check_at_rollover();
    TEST_ASSERT_TRUE(ota_flow_pending());
    ota_flow_apply(90, false);
    TEST_ASSERT_EQUAL_STRING(TARGET, m_paint_to);
    TEST_ASSERT_EQUAL_STRING(IMAGE_URL, m_image_url_seen);
}

/* ---- the apply sequence ------------------------------------------------- */

/* The common path: nothing pending costs one comparison. No window, no
   paint, no radio — a device with no update available must be
   indistinguishable from one with the feature switched off. */
static void test_no_pending_update_opens_no_window(void) {
    m_body = MANIFEST_SAME;
    check_at_rollover();
    g_log[0] = '\0';
    ota_flow_apply(90, false);
    TEST_ASSERT_EQUAL_STRING("", g_log);
}

/* The whole sequence in one assertion. Every element of it is load
   bearing:
     extend:180 FIRST, before anything is charged or painted: it doubles as
                the check that there IS a failsafe to arm, and the whole
                attempt below is bounded by nothing else
     paint      before the radio comes up — display_ota() blocks on a full
                refresh and has no net_window_active() guard, so painting
                inside an open window is the brownout in net_window.c:65-79
     extend:310 after the paint, before the window: an extension applied
                once the download "looks slow" races the thing it protects
                against, and a paint charged to the download budget is 3 s
                the download does not get
     extend:180 after the last chunk, before the commit: the commit tail
                runs two full-image SHA-256 passes that the 310 s arming
                never budgeted for (ota_timing.h's assertion covers the
                ABORT tail only)
     finish     before restart: the commit is what switches the boot
                partition, and it must be the last thing that can fail
     persist    between the radio coming down and the reboot: RTC memory
                does not survive esp_restart, so this is the only thing
                that saves the day's timer state */
static void test_a_successful_update_paints_extends_downloads_commits_reboots(void) {
    check_at_rollover();
    ota_flow_apply(90, false);
    TEST_ASSERT_EQUAL_STRING(
        "get extend:180 paint extend:310 net_on dl_begin step step extend:180 finish net_off persist restart", g_log);
    TEST_ASSERT_EQUAL_STRING(IMAGE_URL, m_image_url_seen);
}

static void test_the_update_screen_names_both_versions(void) {
    check_at_rollover();
    ota_flow_apply(90, false);
    TEST_ASSERT_EQUAL_STRING(RUNNING, m_paint_from);
    TEST_ASSERT_EQUAL_STRING(TARGET, m_paint_to);
}

static void test_one_attempt_per_wake(void) {
    check_at_rollover();
    ota_flow_apply(90, false);
    g_log[0] = '\0';
    ota_flow_apply(90, false);
    TEST_ASSERT_EQUAL_STRING("", g_log);
}

/* What a commit clears, and what it deliberately does not.
 *
 * ota_result must go: task 14 reads it back in the mqtt_ha_window that
 * runs before the next sleep, and the string in it describes the image
 * being replaced.
 *
 * The counter and its target must STAY. With rollback enabled the reboot
 * below is not the end of the attempt — if the new image never certifies
 * itself, the bootloader boots the old slot back. Clearing here would
 * hand that returning image a fresh budget, and the same bad build would
 * be downloaded again every day forever. This asserts the asymmetry
 * directly, so reinstating either clear fails here.
 */
static void test_a_successful_update_clears_the_result_but_not_the_retry_counter(void) {
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_ota_result("net"));
    check_at_rollover();
    ota_flow_apply(90, false);
    TEST_ASSERT_EQUAL_STRING("", stored_result());
    TEST_ASSERT_EQUAL_STRING(TARGET, stored_target());
    TEST_ASSERT_EQUAL_UINT16(1, stored_fails());
}

static void test_the_download_duration_is_recorded(void) {
    m_step_advance_ms = 4000;
    check_at_rollover();
    ota_flow_apply(90, false);
    TEST_ASSERT_EQUAL_UINT32(8000, ota_flow_last_dl_ms());
}

/* THE DURATION HAS TO REACH FLASH, or it is a sensor that reads zero
   forever.
 *
 * ota_flow_last_dl_ms() above is a plain static, and the download that
 * sets it runs in the SECOND window — after net_window.c has closed MQTT
 * — and then either restarts the chip or deep-sleeps. Both discard RAM.
 * So the value the stat payload would find by calling that function at
 * publish time is 0 on the success path (RAM gone with the reboot), 0 on
 * the failure path (RAM gone with the sleep), and 0 on the next wake
 * (nothing ran). The NVS copy is the only one anything can publish.
 */
static void test_the_download_duration_reaches_nvs(void) {
    m_step_advance_ms = 4000;
    check_at_rollover();
    ota_flow_apply(90, false);
    TEST_ASSERT_EQUAL_UINT32(8000, stored_dl_ms());
}

/* And on the timeout, which is the sample that matters most: the field
   exists so a link creeping toward the deadline is visible BEFORE it
   becomes chronic, and the download that hit the deadline is the last
   data point in that trend. */
static void test_a_timed_out_download_records_its_duration_in_nvs(void) {
    m_steps_to_done = 99;
    m_step_advance_ms = 200000; /* 200 s a chunk against a 300 s budget */
    check_at_rollover();
    ota_flow_apply(90, false);
    TEST_ASSERT_EQUAL_STRING("timeout", stored_result());
    TEST_ASSERT_EQUAL_UINT32(400000, stored_dl_ms());
}

/* An ordinary transport failure too — every path through fail_attempt. */
static void test_a_failed_download_records_its_duration_in_nvs(void) {
    m_step_advance_ms = 7000;
    m_step_fails = true;
    m_step_facts.transport_failed = true;
    check_at_rollover();
    ota_flow_apply(90, false);
    TEST_ASSERT_EQUAL_UINT32(7000, stored_dl_ms());
}

/* But an attempt that never opened a transfer must not overwrite the
   last real measurement with a zero: there was no download to time, and
   publishing 0 would read as an instant one. */
static void test_an_attempt_that_never_started_leaves_the_last_duration_alone(void) {
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_ota_dl_ms(12345));
    m_session_ok = false;
    check_at_rollover();
    ota_flow_apply(90, false);
    TEST_ASSERT_EQUAL_UINT32(12345, stored_dl_ms());
}

/* ---- what the stat payload reads, and WHEN ------------------------------ */

/* The publish-time property, pinned directly.
 *
 * mqtt_ha.c calls this from inside the MQTT window, two statements after
 * net_window.c has run ota_flow_check(). If the four fields were instead
 * carried in stats_snapshot_t they would have been frozen on the main
 * task before the check ran, and the payload would publish the PREVIOUS
 * wake's result forever. The sequence below is that exact race: an old
 * result is in NVS, this wake's check overwrites it, and the read has to
 * see the new one. A cached or early-filled implementation returns
 * "timeout" here.
 */
static void test_the_stat_read_sees_what_this_wake_just_wrote(void) {
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_ota_result("timeout"));
    m_body = "not a manifest at all";
    check_at_rollover();

    ota_stat_t stat;
    ota_flow_stat(&stat);
    TEST_ASSERT_EQUAL_STRING("bad_manifest", stat.result);
}

static void test_the_stat_read_reports_every_ota_field(void) {
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_ota_result("gave_up"));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_ota_target("9.9.9"));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_ota_fails(3));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_ota_dl_ms(41250));

    ota_stat_t stat;
    ota_flow_stat(&stat);
    TEST_ASSERT_EQUAL_STRING("gave_up", stat.result);
    TEST_ASSERT_EQUAL_STRING("9.9.9", stat.target);
    TEST_ASSERT_EQUAL_UINT16(3, stat.fails);
    TEST_ASSERT_EQUAL_UINT32(41250, stat.dl_ms);
}

/* A device that has never attempted an update publishes ""/0 rather than
   whatever was on the caller's stack — hal_nvs_read_str writes NOTHING
   into a buffer on a miss, which is the same trap read_counted_target
   exists for. */
static void test_the_stat_read_of_a_virgin_device_is_empty(void) {
    ota_stat_t stat;
    memset(&stat, 0xAB, sizeof(stat));
    ota_flow_stat(&stat);
    TEST_ASSERT_EQUAL_STRING("", stat.result);
    TEST_ASSERT_EQUAL_STRING("", stat.target);
    TEST_ASSERT_EQUAL_UINT16(0, stat.fails);
    TEST_ASSERT_EQUAL_UINT32(0, stat.dl_ms);
}

/* ---- the second gate ---------------------------------------------------- */

/* The two windows are minutes and a full-panel repaint apart, so the
   check gate's answer is not evidence by the time the download starts —
   and the heap headroom for a TLS session is only knowable here. */
static void test_low_heap_at_the_download_skips_without_painting(void) {
    check_at_rollover();
    m_heap = 1000;
    g_log[0] = '\0';
    ota_flow_apply(90, false);
    TEST_ASSERT_EQUAL_STRING("", g_log);
    TEST_ASSERT_EQUAL_STRING("low_heap", stored_result());
}

/* A cell that crossed into the charge lock between the two windows is
   exactly the one that must not be asked for a sustained radio burst
   followed by a flash write.

   ota_result is deliberately NOT written here, and that is the same rule
   the check gate follows: "locked" says no attempt was made, and a
   non-attempt must never overwrite the record of something that actually
   went wrong — which, for a download failure, is the only copy there is
   until the next window publishes it. The update is simply re-found at
   the next rollover. */
static void test_a_charge_lock_engaging_between_the_windows_cancels_the_download(void) {
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_ota_result("tls_cert"));
    check_at_rollover();
    g_log[0] = '\0';
    ota_flow_apply(90, true); /* lock engaged since the check */
    TEST_ASSERT_EQUAL_STRING("", g_log);
    TEST_ASSERT_EQUAL_STRING("tls_cert", stored_result());
}

/* ---- the failsafe gate -------------------------------------------------- */

/* The hole in "the unbounded join is bounded by the failsafe".
 *
 * ota_task_run_apply blocks on xSemaphoreTake(done, portMAX_DELAY) and
 * names the awake failsafe as its only bound. But main.c's
 * arm_awake_failsafe merely LOGS an esp_timer_create/start_once failure,
 * and extend_awake_failsafe then null-guards on the NULL handle and
 * silently does nothing. On such a boot every extension in this file is a
 * no-op, dl_begin is unbounded in time by construction, and a wedged
 * server holds the radio on until the battery is flat — the exact
 * outcome the failsafe exists to make impossible.
 *
 * So the extension is asked for before anything else and its answer is
 * believed. Nothing is painted, no window opens, and — the half that is
 * easy to get wrong — no retry budget is spent, because a refusal to
 * start is not a failed attempt. */
static void test_no_awake_failsafe_to_arm_declines_the_download(void) {
    check_at_rollover();
    m_extend_ok = false;
    g_log[0] = '\0';
    ota_flow_apply(90, false);

    /* The refusal is visible as the asked-for extension and nothing
       after it: no paint, no radio, no download. */
    TEST_ASSERT_EQUAL_STRING("extend:180", g_log);
    TEST_ASSERT_EQUAL_STRING("", m_paint_to);
}

/* Which is why the gate sits AHEAD of charge_the_attempt and not behind
   it. A device that cannot arm its failsafe would otherwise spend a
   retry every wake without ever opening a socket, and would "give up" on
   a perfectly good image after three of them. */
static void test_a_declined_download_costs_no_retry_budget(void) {
    check_at_rollover();
    m_extend_ok = false;
    ota_flow_apply(90, false);
    TEST_ASSERT_EQUAL_UINT16(0, stored_fails());
    TEST_ASSERT_EQUAL_STRING("", stored_target());
}

/* ---- the commit tail ---------------------------------------------------- */

/* The commit path was never budgeted for, and it is the more expensive
 * of the two.
 *
 * The failsafe is armed for max_sec + OTA_ABORT_TAIL_MS, and the
 * _Static_assert in ota_timing.h justifies that tail entirely in ABORT
 * terms: one worst-case dl_step, plus dl_abort, plus session_end. The
 * commit does none of those. It runs esp_https_ota_finish ->
 * esp_ota_end -> ota_verify_partition, a full ~1.5 MB SHA-256, then
 * esp_ota_set_boot_partition -> image_validate, a SECOND full ~1.5 MB
 * SHA-256, plus an ota_data erase and write, three NVS writes and a
 * session_end.
 *
 * Losing that race is the worst outcome in the whole feature, and the
 * only one in the system that yields GARBAGE rather than zeros: the
 * failsafe fires after set_boot_partition succeeded but before restart,
 * main.c deep-sleeps, and the next boot is a deep-sleep wake OF THE NEW
 * IMAGE — the one reset for which the bootloader skips loading the RTC
 * segments — so the new image reads the old image's .rtc.data at its own
 * offsets.
 *
 * The download here is deliberately slow enough that the 310 s arming
 * has almost nothing left by the time the last chunk lands, which is
 * precisely the case a fast-download test would never notice. */
static void test_the_commit_tail_is_given_a_budget_of_its_own(void) {
    m_steps_to_done = 2;
    m_step_advance_ms = 140000; /* 280 s of download inside a 300 s budget */
    check_at_rollover();
    ota_flow_apply(90, false);

    TEST_ASSERT_TRUE_MESSAGE(m_finish_failsafe_at > 0, "the download never reached dl_finish");
    /* EQUAL, not merely "enough": the re-arm is ABSOLUTE from the moment
       of the call, so the commit tail must run with the full ordinary
       awake budget rather than with whatever the download left behind
       (25 s here) or with the download's own 300 s. */
    TEST_ASSERT_EQUAL_INT64_MESSAGE((int64_t)CFG.awake_sec * 1000, m_finish_failsafe_at - m_finish_mono,
                                    "the commit ran on the download's leftover failsafe");
}

/* And the flip side, stated so nobody "fixes" the shortening: on a FAST
   download the same line takes the remaining budget DOWN — 300 s left
   becomes 180 s — because it re-arms absolutely rather than extending.
   That is deliberate. The commit tail is seconds against 180, and 180 s
   is the bound every ordinary wake in this firmware already runs under. */
static void test_the_commit_rearm_is_absolute_and_may_shorten_the_budget(void) {
    m_steps_to_done = 2;
    m_step_advance_ms = 10; /* a download that finishes almost instantly */
    check_at_rollover();
    ota_flow_apply(90, false);

    TEST_ASSERT_EQUAL_INT64((int64_t)CFG.awake_sec * 1000, m_finish_failsafe_at - m_finish_mono);
    /* Which is genuinely LESS than what the download arming had left. */
    TEST_ASSERT_TRUE(m_finish_failsafe_at < m_budget_start + (int64_t)(CFG.max_sec * 1000 + OTA_ABORT_TAIL_MS));
}

/* The abort path must be untouched by all of the above: fail_attempt
   re-arms for awake_sec itself, so a dl_finish that answers false lands
   on exactly the budget it always had, and ota_timing.h's assertion
   keeps saying what it always said. */
static void test_a_failed_commit_still_lands_on_the_ordinary_awake_budget(void) {
    m_finish_ok = false;
    m_finish_facts.image_rejected = true;
    check_at_rollover();
    ota_flow_apply(90, false);
    TEST_ASSERT_EQUAL_STRING("bad_image", stored_result());
    TEST_ASSERT_EQUAL_INT64((int64_t)CFG.awake_sec * 1000, m_failsafe_at - m_mono);
}

/* ---- the pre-reboot snapshot -------------------------------------------- */

/* MAJOR-2: the OTA reboot is the one sleep-less exit in the firmware.
 *
 * timer_persist_save() is called from exactly four places — enter_deep_sleep,
 * the charge lock, a break start and the expiry alert. NOT on a day
 * rollover, NOT on a button action, NOT on an HA grant; those all rely on
 * enter_deep_sleep to flush eventually. This reboot never reaches it,
 * because maybe_apply_update() sits ahead of the sleep in both wake
 * tails, and RTC memory does not survive esp_restart on the S2 — only a
 * deep-sleep wake preserves the RTC segments.
 *
 * The sharp case is the primary trigger: on a rollover wake the stale
 * snapshot still carries YESTERDAY'S date, timer_restore_snapshot refuses
 * it, g_rtc_state stays zeroed, timer_is_new_day() answers true on an
 * empty last_date, and the new firmware runs the rollover a SECOND time —
 * publishing yesterday's screen-time summary from all-zero slots and
 * overwriting Home Assistant's record with zeros. */
static void test_the_state_is_persisted_before_the_reboot(void) {
    check_at_rollover();
    ota_flow_apply(90, false);
    TEST_ASSERT_EQUAL_INT(1, m_persists);
    TEST_ASSERT_TRUE_MESSAGE(m_persisted_before_restart, "the device rebooted before the snapshot was written");
}

/* After the radio is down, not before: the flash write should not be
   competing with a TX burst for the rail, and session_end is the last
   thing that can block. The full-sequence test pins the whole order;
   this one names the pair so a reordering says which pair broke. */
static void test_the_snapshot_is_written_after_the_radio_comes_down(void) {
    check_at_rollover();
    ota_flow_apply(90, false);
    const char *tail = "net_off persist restart";
    TEST_ASSERT_EQUAL_STRING(tail, g_log + strlen(g_log) - strlen(tail));
}

/* A failed attempt does NOT persist, and that is not an oversight: it
   returns to wake_flow, which sleeps through enter_deep_sleep() as
   usual, and that is where the save belongs. Persisting here as well
   would write the same blob twice per wake for nothing — flash wear on a
   path that already has an owner. */
static void test_a_failed_attempt_leaves_the_snapshot_to_the_ordinary_sleep(void) {
    m_step_fails = true;
    m_step_facts.transport_failed = true;
    check_at_rollover();
    ota_flow_apply(90, false);
    TEST_ASSERT_EQUAL_INT(0, m_persists);
}

/* ---- outcomes the flow never saw ---------------------------------------- */

/* MINOR-2: ota_task_run_apply can fail to spawn — a 16 KB stack is the
   largest single allocation this firmware makes — and ota_flow_apply
   then never runs at all. Before this, s_pending had been true (so an
   update was found AND announced by the check) and then nothing was ever
   reported, on that wake or any wake after it.

   Reported as low_heap rather than as a code of its own: what failed IS
   a heap condition, it presents to the operator exactly as the download
   gate's own low_heap does, and it already has a row in the plan's
   failure table. */
static void test_a_spawn_failure_is_reported_to_home_assistant(void) {
    ota_flow_note_spawn_failed();
    TEST_ASSERT_EQUAL_STRING("low_heap", stored_result());
}

/* And it is a REPORT, not an attempt. ota_task.c's reasoning for leaving
   the budget alone is right and stays: giving up on a good image because
   the heap was tight one evening is the wrong failure. */
static void test_a_spawn_failure_costs_no_retry_budget(void) {
    check_at_rollover();
    ota_flow_note_spawn_failed();
    TEST_ASSERT_EQUAL_UINT16(0, stored_fails());
    TEST_ASSERT_EQUAL_STRING("", stored_target());
}

/* ---- failure paths ------------------------------------------------------ */

/* The guarantee the four-call download API exists to make observable: a
   download stopped by our own deadline discards the partial image
   (abort) and never commits it (no finish), so the boot partition is
   never switched to half an image. */
static void test_the_deadline_aborts_cleanly_and_never_commits(void) {
    m_steps_to_done = 99;
    m_step_advance_ms = 200000; /* 200 s a chunk against a 300 s budget */
    check_at_rollover();
    ota_flow_apply(90, false);
    TEST_ASSERT_EQUAL_STRING(
        "get extend:180 paint extend:310 net_on dl_begin step step abort net_off extend:180 repaint", g_log);
    TEST_ASSERT_EQUAL_STRING("timeout", stored_result());
    TEST_ASSERT_EQUAL_STRING(TARGET, stored_target());
    TEST_ASSERT_EQUAL_UINT16(1, stored_fails());
}

/* The relationship the abort depends on, and the reason the two numbers
   at the top of ota_flow_apply are deliberately different.

   extend_awake re-arms the failsafe ABSOLUTELY from the moment of the
   call, so the failsafe and the loop's deadline are measured from the
   same instant against the same clock. If the deadline is anchored any
   LATER than the extension — after session_begin, say, which blocks for
   the whole association — the failsafe fires first and the device deep
   sleeps in the middle of dl_step: no abort, no ota_result, no fail
   count, and tomorrow it attempts the identical doomed download again.
   Forever. The retry budget never learns anything, which is precisely
   the sad loop it exists to end.

   So: the abort must happen while there is still failsafe left, with
   room for the tail behind it (dl_abort, then session_end, which itself
   waits up to a second for the stack). A realistic chunk here rather
   than the coarse one the deadline test uses, because the deadline is
   checked AFTER a chunk and the overshoot has to fit in that room too. */
static void test_the_deadline_fires_while_the_failsafe_still_has_room(void) {
    m_steps_to_done = 9999;
    m_step_advance_ms = 1000; /* a chunk a second against a 300 s budget */
    check_at_rollover();
    ota_flow_apply(90, false);

    TEST_ASSERT_EQUAL_STRING("timeout", stored_result());
    TEST_ASSERT_TRUE_MESSAGE(m_abort_failsafe_at > 0, "the download never reached dl_abort");
    TEST_ASSERT_TRUE_MESSAGE(m_abort_mono < m_abort_failsafe_at,
                             "the awake failsafe fired before the download's own deadline");
    /* And the tail fits: session_end can block for ~1 s behind the
       abort, so "just barely first" is not good enough. */
    TEST_ASSERT_TRUE(m_abort_failsafe_at - m_abort_mono >= 1000);
}

/* F1: the arithmetic, not the anecdote.
 *
 * The test above uses whatever chunk size it happens to use and asserts
 * the failsafe had "room". That is not the property. The property is
 * that the failsafe outlasts the deadline plus the WORST case overshoot
 * — and the worst case is a number, OTA_DL_STEP_WORST_MS, derived in
 * ota_timing.h from the socket timeout and the read-buffer arithmetic.
 *
 * The defect this pins: ota.c set a 10 s timeout_ms while ota_flow armed
 * a 5 s tail, and because `.buffer_size` was left unset the OTA buffer
 * (1024) was twice the per-iteration transport read cap (512), so one
 * dl_step could block for TWO full timeout periods. Twenty seconds of
 * possible overshoot behind a five second tail: the failsafe could beat
 * the deadline outright, and then nothing is aborted, nothing recorded.
 *
 * Both constants now live in one header with a _Static_assert tying
 * them together, so the compiler catches the incompatible pair. This
 * catches the other half — that ota_flow actually ARMS the tail it was
 * given. */
static void test_the_failsafe_outlasts_the_worst_case_deadline_overshoot(void) {
    m_steps_to_done = 9999;
    /* A chunk costing exactly the longest a dl_step can block. */
    m_step_advance_ms = OTA_DL_STEP_WORST_MS;
    check_at_rollover();
    ota_flow_apply(90, false);

    TEST_ASSERT_EQUAL_STRING("timeout", stored_result());
    TEST_ASSERT_TRUE_MESSAGE(m_abort_failsafe_at > 0, "the download never reached dl_abort");

    /* Both clocks are anchored at budget_start, which is the instant
       after the paint. */
    int64_t deadline = m_budget_start + (int64_t)CFG.max_sec * 1000;
    /* The latest the loop can POSSIBLY notice the deadline: it is
       checked after a chunk, so a chunk that began one millisecond
       before the deadline still runs to completion first. */
    int64_t latest_abort = deadline + OTA_DL_STEP_WORST_MS;

    TEST_ASSERT_TRUE_MESSAGE(m_abort_mono <= latest_abort, "a chunk overshot the deadline by more than one dl_step");
    /* And behind even that latest abort there is still enough failsafe
       left to discard the partial image and bring the radio down. If
       there is not, the device deep-sleeps mid-teardown and the whole
       clean-abort guarantee is decoration. */
    TEST_ASSERT_TRUE_MESSAGE(latest_abort + OTA_DL_ABORT_MS + OTA_SESSION_END_MS < m_abort_failsafe_at,
                             "the awake failsafe can fire before the abort tail has been paid for");
}

/* Only the success path recorded a duration before, which is the wrong
   way round: the number exists so a link trending toward the cap is
   visible in Home Assistant BEFORE it becomes chronic, and by definition
   that link is the one that is failing. */
static void test_a_failed_download_still_records_its_duration(void) {
    m_step_advance_ms = 7000;
    m_step_fails = true;
    m_step_facts.transport_failed = true;
    check_at_rollover();
    ota_flow_apply(90, false);
    TEST_ASSERT_EQUAL_STRING("net", stored_result());
    TEST_ASSERT_EQUAL_UINT32(7000, ota_flow_last_dl_ms());
}

/* Two ways for ota.c to misbehave identically: fail while filling no
   facts at all. The reason must still be reportable — an outcome with no
   ota_result is invisible to Home Assistant and, worse, leaves an older
   unrelated failure standing as though it were current. */
static void test_a_begin_that_fails_without_facts_is_still_reported(void) {
    m_begin_ok = false; /* m_begin_facts is all zero from setUp */
    check_at_rollover();
    ota_flow_apply(90, false);
    TEST_ASSERT_EQUAL_STRING("net", stored_result());
}

static void test_a_step_that_fails_without_facts_is_still_reported(void) {
    m_step_fails = true; /* m_step_facts is all zero from setUp */
    check_at_rollover();
    ota_flow_apply(90, false);
    TEST_ASSERT_EQUAL_STRING("net", stored_result());
}

static void test_a_transport_failure_repaints_and_does_not_reboot(void) {
    m_step_fails = true;
    m_step_facts.transport_failed = true;
    check_at_rollover();
    ota_flow_apply(90, false);
    TEST_ASSERT_EQUAL_STRING("get extend:180 paint extend:310 net_on dl_begin step abort net_off extend:180 repaint",
                             g_log);
    TEST_ASSERT_EQUAL_STRING("net", stored_result());
}

/* A rejected chain and a flaky TLS session have completely different
   fixes — one needs a serial cable, the other fixes itself — so the
   distinction has to survive all the way to the stat payload.

   Note the absent abort: a dl_begin that answers false has already
   cleaned up after itself, and aborting a transfer that was never opened
   would be a double free on the device. */
static void test_a_rejected_certificate_is_reported_as_tls_cert(void) {
    m_begin_ok = false;
    m_begin_facts.tls_failed = true;
    m_begin_facts.tls_cert_flags = 0x08;
    check_at_rollover();
    ota_flow_apply(90, false);
    TEST_ASSERT_EQUAL_STRING("get extend:180 paint extend:310 net_on dl_begin net_off extend:180 repaint", g_log);
    TEST_ASSERT_EQUAL_STRING("tls_cert", stored_result());
}

static void test_a_failed_commit_is_recorded_and_does_not_reboot(void) {
    m_finish_ok = false;
    m_finish_facts.image_rejected = true;
    check_at_rollover();
    ota_flow_apply(90, false);
    TEST_ASSERT_EQUAL_STRING(
        "get extend:180 paint extend:310 net_on dl_begin step step extend:180 finish net_off extend:180 repaint",
        g_log);
    TEST_ASSERT_EQUAL_STRING("bad_image", stored_result());
}

/* A window that never opened has nothing to tear down — net_window.c does
   not call wifi_session_end() after a failed begin either. */
static void test_a_failed_association_repaints_without_tearing_down(void) {
    m_session_ok = false;
    check_at_rollover();
    ota_flow_apply(90, false);
    TEST_ASSERT_EQUAL_STRING("get extend:180 paint extend:310 net_on extend:180 repaint", g_log);
    TEST_ASSERT_EQUAL_STRING("net", stored_result());
}

/* ---- the retry budget, across wakes ------------------------------------- */

/* The sad loop, and its end. Without the budget a build that cannot be
   downloaded costs a window, a full-panel repaint and a wasted radio
   burst EVERY day, forever — strictly worse than having no OTA at all,
   because it looks from the outside like nothing is happening. */
static void test_three_failed_wakes_then_the_device_gives_up(void) {
    m_step_fails = true;
    m_step_facts.transport_failed = true;
    for (int wake = 1; wake <= 3; wake++) {
        check_at_rollover();
        TEST_ASSERT_TRUE(ota_flow_pending());
        ota_flow_apply(90, false);
        TEST_ASSERT_EQUAL_UINT16(wake, stored_fails());
    }
    /* Fourth wake: the check still runs, but the decision is now "no". */
    g_log[0] = '\0';
    check_at_rollover();
    TEST_ASSERT_FALSE(ota_flow_pending());
    TEST_ASSERT_EQUAL_STRING("gave_up", stored_result());
    ota_flow_apply(90, false);
    TEST_ASSERT_EQUAL_STRING("get", g_log); /* no paint, no window */
}

/* The escape hatch, and the reason the counter is keyed on the target
   rather than being a bare tally: publishing a different version re-arms
   the device with no reset needed from anywhere else. */
static void test_a_new_target_version_rearms_a_given_up_device(void) {
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_ota_target(TARGET));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_ota_fails(3));
    check_at_rollover();
    TEST_ASSERT_FALSE(ota_flow_pending());

    m_body = "[{\"schema\":1,\"default\":{\"version\":\"1.6.1\",\"url\":\"" IMAGE_URL "\"}}]";
    check_at_rollover();
    TEST_ASSERT_TRUE(ota_flow_pending());
}

/* The read-before-write inside fail_attempt, which is invisible unless
   the two versions differ. A device that already has two failures
   against 1.6.0 and is now attempting 1.6.1 must come out of a failed
   attempt with ONE failure against 1.6.1 — the count is keyed on the
   target precisely so a new version re-arms it. Store the new target
   before reading the old one and the comparison matches itself: the
   device inherits the old count, reaches max_fails after a single
   attempt, and gives up on a build it has tried exactly once. */
static void test_a_failure_against_a_new_target_starts_its_count_at_one(void) {
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_ota_target(TARGET)); /* 1.6.0 */
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_ota_fails(2));
    m_body = "[{\"schema\":1,\"default\":{\"version\":\"1.6.1\",\"url\":\"" IMAGE_URL "\"}}]";
    m_step_fails = true;
    m_step_facts.transport_failed = true;

    check_at_rollover();
    TEST_ASSERT_TRUE(ota_flow_pending());
    ota_flow_apply(90, false);

    TEST_ASSERT_EQUAL_STRING("1.6.1", stored_target());
    TEST_ASSERT_EQUAL_UINT16(1, stored_fails());
}

/* ---- the budget is charged BEFORE the attempt, not after it ------------- */

/* F2, and the test that would have caught the whole defect class.
 *
 * The budget used to be charged at the end of fail_attempt — that is,
 * only for a failure this module actually OBSERVED. An attempt killed
 * mid-flight is observed by nobody: the awake failsafe fires, main.c
 * deep-sleeps immediately, dl_abort never runs and record() never
 * happens. So the counter stayed put, the budget never engaged, and the
 * identical doomed attempt burned MAX_AWAKE_SEC of radio-on time at
 * every rollover forever, on a battery device.
 *
 * That is not hypothetical. dl_begin sits OUTSIDE the loop ota_flow
 * bounds and has two verified unbounded paths of its own, and neither a
 * brownout nor a battery pull is bounded by anything at all.
 *
 * The fix is to write the attempt down before making it. */
static void test_an_attempt_killed_before_any_record_still_costs_the_budget(void) {
    m_kill_in_dl_begin = true;
    check_at_rollover();
    apply_and_let_the_failsafe_kill_it();

    /* Nothing was recorded — by construction, the wake died before any
       code that could record anything. */
    TEST_ASSERT_EQUAL_STRING("", stored_result());
    /* But the attempt is on the books, so the next boot knows it
       happened. */
    TEST_ASSERT_EQUAL_STRING(TARGET, stored_target());
    TEST_ASSERT_EQUAL_UINT16(1, stored_fails());
}

/* The consequence that matters: a device dying inside dl_begin every
   single wake still converges. Without the pre-charge this loop runs
   until the battery is flat. */
static void test_a_wake_killed_mid_attempt_still_converges_on_gave_up(void) {
    m_kill_in_dl_begin = true;
    for (int wake = 1; wake <= 3; wake++) {
        check_at_rollover();
        TEST_ASSERT_TRUE(ota_flow_pending());
        apply_and_let_the_failsafe_kill_it();
        TEST_ASSERT_EQUAL_UINT16(wake, stored_fails());
    }
    /* Fourth rollover: the check runs, and the answer is now no. */
    check_at_rollover();
    TEST_ASSERT_FALSE(ota_flow_pending());
    TEST_ASSERT_EQUAL_STRING("gave_up", stored_result());
}

/* The other side of "this is a MOVE, not an addition". Charging up front
   AND on the observed failure would double-count, halving every budget
   silently: a max_fails of 3 would give up after two attempts. */
static void test_an_observed_failure_is_charged_exactly_once(void) {
    m_step_fails = true;
    m_step_facts.transport_failed = true;
    check_at_rollover();
    ota_flow_apply(90, false);
    TEST_ASSERT_EQUAL_STRING("net", stored_result());
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(1, stored_fails(),
                                     "the attempt was charged twice: once up front and once on failure");
}

/* A gate that declined is not an attempt and must not be charged. Both
   second-gate refusals return before the charge, and the update is
   simply re-found at the next rollover with the budget untouched. */
static void test_a_gate_that_declines_costs_no_budget(void) {
    check_at_rollover();
    m_heap = 1000; /* low_heap at the download gate */
    ota_flow_apply(90, false);
    TEST_ASSERT_EQUAL_STRING("low_heap", stored_result());
    TEST_ASSERT_EQUAL_UINT16(0, stored_fails());
    TEST_ASSERT_EQUAL_STRING("", stored_target());

    check_at_rollover();
    ota_flow_apply(90, true); /* charge lock at the download gate */
    TEST_ASSERT_EQUAL_UINT16(0, stored_fails());
    TEST_ASSERT_EQUAL_STRING("", stored_target());
}

/* The success case, and the reason the pre-charge costs nothing even
   though it is never cleared. The counter is elevated the moment the
   attempt starts and is still standing when the device reboots — but the
   image it reboots into IS the counted target, and ota_policy answers
   up_to_date before it ever consults the budget, which the next case
   pins directly. The charge is only ever read again by a device that
   came back on the OLD image, which is precisely the rollback this
   counter is there to bound. */
static void test_a_successful_update_leaves_its_charge_standing(void) {
    check_at_rollover();
    ota_flow_apply(90, false);
    TEST_ASSERT_EQUAL_STRING("restart", g_log + strlen(g_log) - strlen("restart"));
    TEST_ASSERT_EQUAL_UINT16(1, stored_fails());
    TEST_ASSERT_EQUAL_STRING(TARGET, stored_target());
}

/* The claim the pre-charge's safety rests on, asserted against the real
   ota_policy rather than assumed: once the device is RUNNING the version
   the counter was charged against, the version compare answers
   up_to_date and the budget is never consulted. So an elevated counter
   that somehow survived a commit cannot strand the device.
   Driven here by running a check whose manifest offers exactly the
   running version, with the counter already at max_fails. */
static void test_an_elevated_counter_against_the_running_version_is_never_consulted(void) {
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_ota_target(RUNNING));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_ota_fails(3)); /* at the cap */
    m_body = MANIFEST_SAME;
    check_at_rollover();
    TEST_ASSERT_FALSE(ota_flow_pending());
    /* up_to_date, NOT gave_up: the budget never got a look in. */
    TEST_ASSERT_EQUAL_STRING("", stored_result());
}

/* THE ROLLBACK LOOP, and the reason the commit path clears ota_result
   and nothing else.
 *
 * CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE turns a successful commit into a
 * conditional one: the new image has to reach ota_flow_confirm_image()
 * or the bootloader marks it ABORTED and boots the old slot back. From
 * this module's side that is invisible — the old image simply wakes up
 * again, still on RUNNING, and finds the same manifest offering the same
 * TARGET. Nothing in the check gate can tell that apart from a first
 * look at a new build.
 *
 * So the only thing standing between a bad build and a device that
 * downloads ~1.5 MB every single day until the battery is flat is the
 * retry counter surviving the reboot. Each turn of the loop is charged
 * by charge_the_attempt() before the download; the fourth wake finds the
 * budget spent and stops, permanently and visibly.
 *
 * This is the case that fails if either clear comes back to the commit
 * path: with the counter wiped at every commit the loop below never
 * advances past one, and the last two assertions never come true.
 *
 * The escape hatch is unchanged and is not a bug: publishing a different
 * version string re-arms the device with no reset needed from anywhere
 * else (test_a_new_target_version_rearms_a_given_up_device).
 */
static void test_a_rollback_loop_is_bounded_by_the_retry_budget(void) {
    for (int boot = 1; boot <= 3; boot++) {
        ota_flow_init(&OPS, &CFG); /* the old image, back from a rollback */
        check_at_rollover();
        TEST_ASSERT_TRUE(ota_flow_pending());
        ota_flow_apply(90, false);
        /* It really did commit and reboot on every turn — this is a loop
           of SUCCESSFUL updates, undone underneath us. */
        TEST_ASSERT_EQUAL_STRING("restart", g_log + strlen(g_log) - strlen("restart"));
        TEST_ASSERT_EQUAL_UINT16(boot, stored_fails());
        TEST_ASSERT_EQUAL_STRING(TARGET, stored_target());
    }

    /* Fourth boot: the budget is spent and the loop stops for good. */
    ota_flow_init(&OPS, &CFG);
    g_log[0] = '\0';
    check_at_rollover();
    TEST_ASSERT_FALSE(ota_flow_pending());
    /* rolled_back, NOT gave_up: this boot came back from the third
       revert, and a revert reported at init keeps ota_result for the rest
       of the boot (see record()). The budget verdict is a level and
       returns on the next wake —
       test_the_verdict_a_revert_displaces_returns_on_the_next_wake pins
       that. What this case is about is the BOUND, and the bound is
       ota_fails, which the report does not touch. */
    TEST_ASSERT_EQUAL_STRING("rolled_back", stored_result());
    TEST_ASSERT_EQUAL_UINT16(3, stored_fails());
    ota_flow_apply(90, false);
    TEST_ASSERT_EQUAL_STRING("get", g_log); /* no paint, no radio, no download */
}

/* Bullet nine: the failure has to still be there in the next window, and
   arming a new wake must not wipe it. It is the only channel the failure
   has — MQTT was already closed when the download died. */
static void test_a_failure_survives_into_the_next_window(void) {
    m_step_fails = true;
    m_step_facts.transport_failed = true;
    check_at_rollover();
    ota_flow_apply(90, false);
    TEST_ASSERT_EQUAL_STRING("net", stored_result());

    ota_flow_arm(OTA_TRIGGER_NONE, 90, false);
    TEST_ASSERT_EQUAL_STRING("net", stored_result());
    TEST_ASSERT_EQUAL_STRING(TARGET, stored_target());
    TEST_ASSERT_EQUAL_UINT16(1, stored_fails());
}

/* ---- rollback: certifying the image this boot came up on ----------------

   These are about the NEXT boot, not this one: enter_deep_sleep() calls
   ota_flow_confirm_image() on the wake that came up on a freshly written
   image, and whether that call reaches the device is the whole question
   — with CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y a wake that ends
   without it reverts the update.

   Note what is NOT tested here: whether the image is actually pending
   verify. That test is inside ota.c's ota_mark_valid_if_pending (a bare
   esp_ota_get_state_partition read, no branch of ours), and the mock
   below stands in for the whole of it. What is ours is which wakes get
   to call it at all. */

static void test_an_ordinary_sleep_certifies_the_running_image(void) {
    ota_flow_confirm_image();
    TEST_ASSERT_EQUAL_INT(1, m_marks_valid);
}

/* The pre-init boot, which is not a hypothetical: main.c arms the awake
   failsafe as app_main's SECOND call and only reaches ota_flow_init()
   some seventy lines later, so every boot spends a real window in which
   the failsafe can fire into a funnel whose ops table is still all
   zeroes. enter_deep_sleep() calls confirm_image() unconditionally, and
   without the guard that is a call through a NULL function pointer on a
   path taken by a device that is already in trouble.

   Nothing is lost by declining: a boot that wedged before init is
   exactly a boot that should not be certifying anything.

   Modelled by zeroing s_ops rather than by skipping setUp, because s_ops
   is a file-static and zero is genuinely the state it is in before the
   first ota_flow_init() of the process. */
static void test_confirm_before_init_marks_nothing_and_does_not_crash(void) {
    memset(&s_ops, 0, sizeof(s_ops));
    ota_flow_confirm_image();
    TEST_ASSERT_EQUAL_INT(0, m_marks_valid);
    TEST_ASSERT_EQUAL_STRING("", g_log);
}

/* The substantive decision. The awake failsafe fires when a wake is
   already wedged; certifying there would cancel the rollback on the one
   piece of evidence anyone has that the new image is bad. */
static void test_the_awake_failsafe_path_does_not_certify_the_image(void) {
    ota_flow_note_failsafe_sleep();
    ota_flow_confirm_image();
    TEST_ASSERT_EQUAL_INT(0, m_marks_valid);
}

/* Every other way into enter_deep_sleep() — the lock gates, the early-out
   sleeps, the ordinary tails — is an ordinary sleep and must certify. A
   check-based implementation ("was I on the main task?") could get this
   wrong; an announced fact cannot, so the property worth pinning is that
   nothing but the announcement suppresses the call. Downloading in the
   same wake does not suppress it either. */
static void test_a_wake_that_ran_a_download_still_certifies_at_sleep(void) {
    m_step_fails = true;
    m_step_facts.transport_failed = true;
    check_at_rollover();
    ota_flow_apply(90, false);
    ota_flow_confirm_image();
    TEST_ASSERT_EQUAL_INT(1, m_marks_valid);
}

/* The flag is per-boot, and init is what clears it. Without this, a
   failsafe on one wake would poison the confirmation on every wake after
   it — which on a device that deep-sleeps is not reachable, but on the
   host (and after any future esp_restart path) it is exactly the kind of
   sticky static that outlives its wake. */
static void test_the_failsafe_flag_does_not_outlive_its_boot(void) {
    ota_flow_note_failsafe_sleep();
    ota_flow_confirm_image();
    TEST_ASSERT_EQUAL_INT(0, m_marks_valid);

    ota_flow_init(&OPS, &CFG); /* the next boot */
    ota_flow_confirm_image();
    TEST_ASSERT_EQUAL_INT(1, m_marks_valid);
}

/* ---- rollback: making the revert visible to the operator ----------------

   The gap these close: the commit path clears ota_result (task 13
   requires it), so a device whose new image fails to certify comes back
   reporting an EMPTY result. `fw` and the status screen both read the app
   descriptor and agree on the old version, and nothing anywhere says the
   device tried. The only evidence was indirect and took three ~1.5 MB
   downloads to become a gave_up.

   The token these turn on is ota_pend_ver, written by the commit at the
   one instant that is unambiguous and consumed by whichever future
   arrives: certification, or a boot that came up on something else. It
   holds the committed VERSION rather than a flag, so that the detector
   never has to ask ota_target — the retry budget's key, re-pointed before
   every attempt — what this device committed. */

/* The commit arms the token, and arms it with the version it committed.
   Nothing else in the flow does — see the failure cases below. */
static void test_a_commit_arms_the_certification_token(void) {
    check_at_rollover();
    ota_flow_apply(90, false);
    TEST_ASSERT_EQUAL_STRING(TARGET, stored_pend_ver());
}

/* THE HEADLINE CASE. The update committed, the new image never certified
   itself, and the bootloader booted the old slot back — so this boot is
   running RUNNING while the token says TARGET was committed. That
   disagreement is what a revert looks like from inside the firmware, and
   it has to reach ota_result before the network window opens. */
static void test_a_revert_is_reported_on_the_boot_that_comes_back(void) {
    check_at_rollover();
    ota_flow_apply(90, false);
    TEST_ASSERT_EQUAL_STRING("", stored_result()); /* the commit cleared it */

    boot_running(RUNNING); /* the old image, back again */
    TEST_ASSERT_EQUAL_STRING("rolled_back", stored_result());
}

/* AND IT MUST NOT LATCH. This is the whole reason the detector is a
   consumed token rather than a read of ESP_OTA_IMG_ABORTED on the other
   slot: that marker stays set until the slot is rewritten, so "the other
   slot is aborted" is just as true on the tenth boot after a revert as
   on the first. A detector built on it reports a month-old rollback over
   every later result, forever.
 *
 * The second half is the one with teeth: something else writes a genuine
 * result, and the boot after that must leave it alone. */
static void test_a_revert_is_reported_once_and_never_again(void) {
    check_at_rollover();
    ota_flow_apply(90, false);
    boot_running(RUNNING);
    TEST_ASSERT_EQUAL_STRING("rolled_back", stored_result());
    TEST_ASSERT_EQUAL_STRING("", stored_pend_ver()); /* consumed */

    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_ota_result("tls_cert"));
    boot_running(RUNNING);
    TEST_ASSERT_EQUAL_STRING("tls_cert", stored_result());
    boot_running(RUNNING);
    TEST_ASSERT_EQUAL_STRING("tls_cert", stored_result());
}

/* The boot right after a SUCCESSFUL update also finds the token set —
   the difference is that the committed version is the one running. That
   is not a revert, it is an image awaiting certification, and reporting
   a rollback here would fire on every healthy update this device ever
   performs. */
static void test_the_boot_that_came_up_on_the_new_image_reports_nothing(void) {
    check_at_rollover();
    ota_flow_apply(90, false);

    boot_running(TARGET); /* the new image, running */
    TEST_ASSERT_EQUAL_STRING("", stored_result());
    TEST_ASSERT_EQUAL_STRING(TARGET, stored_pend_ver()); /* left for the certification */
}

/* ...and the certification retires it. After that the device is on an
   ordinary footing again: a later boot on a different version (a
   downgrade, a reflash) is not a rollback and must not be reported as
   one. */
static void test_certifying_the_image_retires_the_token(void) {
    check_at_rollover();
    ota_flow_apply(90, false);
    boot_running(TARGET);
    ota_flow_confirm_image();
    TEST_ASSERT_EQUAL_STRING("", stored_pend_ver());

    boot_running(RUNNING);
    TEST_ASSERT_EQUAL_STRING("", stored_result());
}

/* The failsafe path declines to certify on purpose — that wake was
   already wedged and the pending image is exactly what is suspected. It
   must equally decline to retire the token, or the revert that follows
   would be invisible again. */
static void test_a_failsafe_sleep_leaves_the_revert_detectable(void) {
    check_at_rollover();
    ota_flow_apply(90, false);

    boot_running(TARGET);
    ota_flow_note_failsafe_sleep();
    ota_flow_confirm_image();
    TEST_ASSERT_EQUAL_STRING(TARGET, stored_pend_ver());

    boot_running(RUNNING);
    TEST_ASSERT_EQUAL_STRING("rolled_back", stored_result());
}

/* A download that FAILED never moved the boot partition, so the next
   boot is running the old image for an entirely ordinary reason. Arming
   the token before the commit (rather than after it) would turn every
   such failure into a false rollback report written over the genuine
   reason — which is the one thing worse than no report at all. */
static void test_a_failed_download_never_reports_a_rollback(void) {
    m_finish_ok = false;
    m_finish_facts.transport_failed = true;
    check_at_rollover();
    ota_flow_apply(90, false);
    TEST_ASSERT_EQUAL_STRING("net", stored_result());
    TEST_ASSERT_EQUAL_STRING("", stored_pend_ver());

    boot_running(RUNNING);
    TEST_ASSERT_EQUAL_STRING("net", stored_result());
}

/* A device that has never run an OTA has no token and no story. */
static void test_a_virgin_boot_reports_no_rollback(void) {
    boot_running(RUNNING);
    TEST_ASSERT_EQUAL_STRING("", stored_result());
}

/* EVERY revert in the loop, not just the first. The rollback loop task
   13 bounded runs the commit path once per turn, so the token is
   re-armed each time; a detector that only ever fired once per device
   would report the first revert and then go quiet for the two turns that
   follow, which is the same invisibility in a smaller box. */
static void test_every_revert_in_a_rollback_loop_is_reported(void) {
    for (int turn = 1; turn <= 2; turn++) {
        check_at_rollover();
        ota_flow_apply(90, false);
        TEST_ASSERT_EQUAL_STRING("", stored_result());

        boot_running(RUNNING);
        TEST_ASSERT_EQUAL_STRING("rolled_back", stored_result());
        TEST_ASSERT_EQUAL_UINT16(turn, stored_fails());
    }
}

/* ---- the revert has to SURVIVE the wake that detected it ---------------

   The detector runs from ota_flow_init(), which is the earliest thing in
   the wake, and ota_result is one slot. Everything the wake does
   afterwards — the check's gate, its budget verdict, the second window's
   download failure — writes that same slot BEFORE the payload is built
   (net_window.c runs ota_flow_check() and then mqtt_ha_window()). The
   token is already spent by then, so an overwrite is not a delay, it is
   the permanent loss of a one-shot event.

   So a revert reported on this boot keeps ota_result for the rest of the
   boot. The cases below pin both halves of that trade: what the rollback
   holds on to, and what the displaced verdict does instead (it comes
   back on the next wake, because it is a level and the condition has not
   gone anywhere). */

/* The exact collision that made the report losable: a revert detected at
   init, then an ordinary gate decline in the same wake, ahead of the
   publish. */
static void test_a_revert_outranks_the_same_wakes_check_result(void) {
    check_at_rollover();
    ota_flow_apply(90, false); /* commits TARGET */

    boot_running(RUNNING); /* the revert */
    TEST_ASSERT_EQUAL_STRING("rolled_back", stored_result());

    ota_flow_arm(OTA_TRIGGER_ROLLOVER, 12, false); /* flat battery */
    ota_flow_check(true);
    TEST_ASSERT_EQUAL_STRING("rolled_back", stored_result());
}

/* The second window's failure is the other writer, and it lands after
   the payload has been built rather than before it — but the token is
   just as spent, so it is held off too. The retry budget is NOT: the
   attempt still counts, which is what keeps a rollback loop bounded
   while the operator is reading a rollback report. */
static void test_a_download_failure_in_the_same_boot_cannot_bury_the_revert(void) {
    check_at_rollover();
    ota_flow_apply(90, false); /* commits TARGET, charging fails=1 */

    boot_running(RUNNING);
    TEST_ASSERT_EQUAL_STRING("rolled_back", stored_result());

    m_finish_ok = false;
    m_finish_facts.transport_failed = true;
    check_at_rollover();
    ota_flow_apply(90, false);
    TEST_ASSERT_EQUAL_STRING("rolled_back", stored_result());
    TEST_ASSERT_EQUAL_UINT16(2, stored_fails()); /* the attempt still counted */
}

/* What the sacrifice actually costs, stated as a test: the displaced
   verdict is a LEVEL, so the next wake records it again and it reaches
   HA one day late. gave_up is the sharpest case — it is the verdict a
   device in a rollback loop reaches, i.e. the one most likely to collide
   with a revert report. */
static void test_the_verdict_a_revert_displaces_returns_on_the_next_wake(void) {
    /* A device whose budget is spent, coming back from a revert. */
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_ota_target(TARGET));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_ota_fails(3)); /* at the cap */
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_ota_pend_ver(TARGET));

    boot_running(RUNNING);
    TEST_ASSERT_EQUAL_STRING("rolled_back", stored_result());
    check_at_rollover();
    TEST_ASSERT_FALSE(ota_flow_pending());
    TEST_ASSERT_EQUAL_STRING("rolled_back", stored_result()); /* not gave_up */

    boot_running(RUNNING); /* the next day: the token is spent */
    check_at_rollover();
    TEST_ASSERT_EQUAL_STRING("gave_up", stored_result());
}

/* ---- what was committed is not what the budget is counting ------------

   ota_target is the RETRY BUDGET's key and charge_the_attempt() writes it
   before every download by design. Reading it as "the version that was
   committed" is only correct while no attempt happens between a commit
   and its revert — and an image bad enough to be reverted usually wedges
   before it certifies, so that attempt is ordinary rather than exotic.
   Both directions of the error are below. */

/* MISATTRIBUTION. The uncertified boot attempts a third version, which
   re-points ota_target. Reading ota_target here would call the boot that
   comes up on the committed image a revert — a rollback reported against
   a version that was never downloaded, over the top of the genuine
   failure the attempt recorded. */
static void test_an_attempt_after_the_commit_does_not_forge_a_revert(void) {
    check_at_rollover();
    ota_flow_apply(90, false); /* commits TARGET */

    boot_running(TARGET); /* the new image, not yet certified */
    TEST_ASSERT_EQUAL_STRING("", stored_result());

    m_body = MANIFEST_NEXT; /* and a newer build is offered straight away */
    m_finish_ok = false;
    m_finish_facts.transport_failed = true;
    check_at_rollover();
    ota_flow_apply(90, false);
    TEST_ASSERT_EQUAL_STRING(NEXT, stored_target());     /* the budget moved */
    TEST_ASSERT_EQUAL_STRING(TARGET, stored_pend_ver()); /* the record did not */
    TEST_ASSERT_EQUAL_STRING("net", stored_result());

    /* That wake wedged without certifying, and the next boot comes up on
       the committed image again. Nothing was reverted. */
    boot_running(TARGET);
    TEST_ASSERT_EQUAL_STRING("net", stored_result());
    TEST_ASSERT_EQUAL_STRING(TARGET, stored_pend_ver()); /* still owed a verdict */
}

/* THE INVERSE, and the worse of the two. ota_policy allows downgrades on
   purpose — "a downgrade is how a rollback is published" — so the
   operator's designed recovery is to re-point the manifest at the
   known-good build. That attempt sets ota_target to the version the
   device is about to revert TO. Comparing against it would match, the
   genuine revert would be swallowed, and the token would stay armed. */
static void test_a_revert_to_the_version_the_manifest_now_names_is_reported(void) {
    check_at_rollover();
    ota_flow_apply(90, false); /* commits TARGET */

    boot_running(TARGET);   /* the new image, not yet certified */
    m_body = MANIFEST_SAME; /* the operator re-points at the good build */
    m_finish_ok = false;
    m_finish_facts.transport_failed = true;
    check_at_rollover();
    ota_flow_apply(90, false);
    TEST_ASSERT_EQUAL_STRING(RUNNING, stored_target()); /* the recovery target */

    /* Then the image wedges and the bootloader reverts to it anyway. */
    boot_running(RUNNING);
    TEST_ASSERT_EQUAL_STRING("rolled_back", stored_result());
    TEST_ASSERT_EQUAL_STRING("", stored_pend_ver());
}

/* ---- the token's own edges --------------------------------------------- */

/* A report must never be written against a token that is still armed:
   that is the latch, one boot at a time. So the consume comes first AND
   its answer is believed — a store that refuses the write leaves this
   boot exactly as it found it, and the next boot detects the same revert
   and reports it then. Deferred, not lost, and never doubled. */
static void test_a_token_that_cannot_be_consumed_is_left_for_the_next_boot(void) {
    check_at_rollover();
    ota_flow_apply(90, false); /* commits TARGET */

    mock_nvs_fail_writes(1); /* the consume is this boot's first write */
    boot_running(RUNNING);
    TEST_ASSERT_EQUAL_STRING("", stored_result());       /* nothing claimed */
    TEST_ASSERT_EQUAL_STRING(TARGET, stored_pend_ver()); /* still armed */

    boot_running(RUNNING); /* the store recovers */
    TEST_ASSERT_EQUAL_STRING("rolled_back", stored_result());
    TEST_ASSERT_EQUAL_STRING("", stored_pend_ver());
}

/* A committed version the detector cannot read WHOLE is the one case
   where it knows something happened and cannot name it. Reporting would
   blame whatever version the operator last saw; leaving it armed would
   re-ask an unanswerable question on every boot forever. So it is
   consumed silently — the declared gap, now pinned rather than assumed.
   Written through the store's own primitive because the bounded setter
   exists precisely to refuse this value. */
static void test_an_unreadable_committed_version_is_consumed_unreported(void) {
    char big[CFG_BOUND_OTA_TARGET_MAX + 8];
    memset(big, 'v', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, nvs_config_set_ota_pend_ver(big));
    TEST_ASSERT_EQUAL(ESP_OK, hal_nvs_write_str(NVS_KEY_OTA_PEND_VER, big));

    boot_running(RUNNING);
    TEST_ASSERT_EQUAL_STRING("", stored_result());   /* nothing to attribute */
    TEST_ASSERT_EQUAL_STRING("", stored_pend_ver()); /* and it does not ask again */
}

/* The other edge of the same key, and the reason it is one key: with no
   commit outstanding there is no state to interpret and nothing to
   write. A version change with an empty token is a reflash or a manual
   downgrade, not a revert — and an ordinary boot costs no flash. */
static void test_a_boot_with_nothing_committed_reports_nothing_and_writes_nothing(void) {
    boot_running("9.9.9");
    TEST_ASSERT_EQUAL_STRING("", stored_result());
    TEST_ASSERT_EQUAL_INT(0, mock_nvs_write_count(NVS_KEY_OTA_PEND_VER));
}

/* The reason string is stored and published like any other, so it has to
   fit the same widths — and it is persistable, because the boot that
   writes it is a boot on which nothing else runs at all. */
static void test_the_rollback_reason_is_a_persistable_code_that_fits(void) {
    TEST_ASSERT_TRUE(ota_policy_reason_is_persistable(OTA_REASON_ROLLED_BACK));
    TEST_ASSERT_EQUAL_STRING("rolled_back", ota_policy_reason_str(OTA_REASON_ROLLED_BACK));
    TEST_ASSERT_LESS_THAN_UINT(OTA_REASON_TEXT_MAX, strlen(ota_policy_reason_str(OTA_REASON_ROLLED_BACK)) + 1);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_rollover_always_checks);
    RUN_TEST(test_plain_tick_wake_never_checks);
    RUN_TEST(test_button_d_checks_only_when_ota_on_sync_is_set);
    RUN_TEST(test_arming_a_check_clears_the_previous_buffered_update);
    RUN_TEST(test_a_second_window_in_the_same_wake_keeps_the_buffered_update);
    RUN_TEST(test_only_one_check_runs_per_wake);

    RUN_TEST(test_empty_endpoint_disables_the_check_entirely);
    RUN_TEST(test_no_ntp_skips_the_check_and_leaves_an_earlier_failure_alone);
    RUN_TEST(test_charge_lock_skips_the_check);
    RUN_TEST(test_low_battery_skips_the_check_and_says_so);
    RUN_TEST(test_unreadable_battery_does_not_gate);

    RUN_TEST(test_the_check_fetches_the_configured_endpoint);
    RUN_TEST(test_the_check_records_its_result_before_returning);
    RUN_TEST(test_a_failed_manifest_fetch_costs_no_retry_budget);
    RUN_TEST(test_up_to_date_buffers_nothing_and_records_nothing);
    RUN_TEST(test_a_pinned_device_buffers_nothing);
    RUN_TEST(test_a_malformed_manifest_is_recorded);
    RUN_TEST(test_a_manifest_that_exactly_fills_the_buffer_is_rejected);
    RUN_TEST(test_a_shorter_manifest_after_a_full_one_is_read_at_its_own_length);

    RUN_TEST(test_no_pending_update_opens_no_window);
    RUN_TEST(test_a_successful_update_paints_extends_downloads_commits_reboots);
    RUN_TEST(test_the_update_screen_names_both_versions);
    RUN_TEST(test_one_attempt_per_wake);
    RUN_TEST(test_a_successful_update_clears_the_result_but_not_the_retry_counter);
    RUN_TEST(test_the_download_duration_is_recorded);

    RUN_TEST(test_low_heap_at_the_download_skips_without_painting);
    RUN_TEST(test_a_charge_lock_engaging_between_the_windows_cancels_the_download);

    RUN_TEST(test_no_awake_failsafe_to_arm_declines_the_download);
    RUN_TEST(test_a_declined_download_costs_no_retry_budget);

    RUN_TEST(test_the_commit_tail_is_given_a_budget_of_its_own);
    RUN_TEST(test_the_commit_rearm_is_absolute_and_may_shorten_the_budget);
    RUN_TEST(test_a_failed_commit_still_lands_on_the_ordinary_awake_budget);

    RUN_TEST(test_the_state_is_persisted_before_the_reboot);
    RUN_TEST(test_the_snapshot_is_written_after_the_radio_comes_down);
    RUN_TEST(test_a_failed_attempt_leaves_the_snapshot_to_the_ordinary_sleep);

    RUN_TEST(test_a_spawn_failure_is_reported_to_home_assistant);
    RUN_TEST(test_a_spawn_failure_costs_no_retry_budget);

    RUN_TEST(test_the_deadline_aborts_cleanly_and_never_commits);
    RUN_TEST(test_the_deadline_fires_while_the_failsafe_still_has_room);
    RUN_TEST(test_the_failsafe_outlasts_the_worst_case_deadline_overshoot);
    RUN_TEST(test_a_failed_download_still_records_its_duration);
    RUN_TEST(test_a_begin_that_fails_without_facts_is_still_reported);
    RUN_TEST(test_a_step_that_fails_without_facts_is_still_reported);
    RUN_TEST(test_a_transport_failure_repaints_and_does_not_reboot);
    RUN_TEST(test_a_rejected_certificate_is_reported_as_tls_cert);
    RUN_TEST(test_a_failed_commit_is_recorded_and_does_not_reboot);
    RUN_TEST(test_a_failed_association_repaints_without_tearing_down);

    RUN_TEST(test_three_failed_wakes_then_the_device_gives_up);
    RUN_TEST(test_a_new_target_version_rearms_a_given_up_device);
    RUN_TEST(test_a_failure_against_a_new_target_starts_its_count_at_one);
    RUN_TEST(test_a_failure_survives_into_the_next_window);

    RUN_TEST(test_an_attempt_killed_before_any_record_still_costs_the_budget);
    RUN_TEST(test_a_wake_killed_mid_attempt_still_converges_on_gave_up);
    RUN_TEST(test_an_observed_failure_is_charged_exactly_once);
    RUN_TEST(test_a_gate_that_declines_costs_no_budget);
    RUN_TEST(test_a_successful_update_leaves_its_charge_standing);
    RUN_TEST(test_an_elevated_counter_against_the_running_version_is_never_consulted);
    RUN_TEST(test_a_rollback_loop_is_bounded_by_the_retry_budget);

    RUN_TEST(test_an_ordinary_sleep_certifies_the_running_image);
    RUN_TEST(test_confirm_before_init_marks_nothing_and_does_not_crash);
    RUN_TEST(test_the_awake_failsafe_path_does_not_certify_the_image);
    RUN_TEST(test_a_wake_that_ran_a_download_still_certifies_at_sleep);
    RUN_TEST(test_the_failsafe_flag_does_not_outlive_its_boot);

    RUN_TEST(test_the_download_duration_reaches_nvs);
    RUN_TEST(test_a_timed_out_download_records_its_duration_in_nvs);
    RUN_TEST(test_a_failed_download_records_its_duration_in_nvs);
    RUN_TEST(test_an_attempt_that_never_started_leaves_the_last_duration_alone);
    RUN_TEST(test_the_stat_read_sees_what_this_wake_just_wrote);
    RUN_TEST(test_the_stat_read_reports_every_ota_field);
    RUN_TEST(test_the_stat_read_of_a_virgin_device_is_empty);

    RUN_TEST(test_a_commit_arms_the_certification_token);
    RUN_TEST(test_a_revert_is_reported_on_the_boot_that_comes_back);
    RUN_TEST(test_a_revert_is_reported_once_and_never_again);
    RUN_TEST(test_the_boot_that_came_up_on_the_new_image_reports_nothing);
    RUN_TEST(test_certifying_the_image_retires_the_token);
    RUN_TEST(test_a_failsafe_sleep_leaves_the_revert_detectable);
    RUN_TEST(test_a_failed_download_never_reports_a_rollback);
    RUN_TEST(test_a_virgin_boot_reports_no_rollback);
    RUN_TEST(test_every_revert_in_a_rollback_loop_is_reported);
    RUN_TEST(test_the_rollback_reason_is_a_persistable_code_that_fits);

    RUN_TEST(test_a_revert_outranks_the_same_wakes_check_result);
    RUN_TEST(test_a_download_failure_in_the_same_boot_cannot_bury_the_revert);
    RUN_TEST(test_the_verdict_a_revert_displaces_returns_on_the_next_wake);
    RUN_TEST(test_an_attempt_after_the_commit_does_not_forge_a_revert);
    RUN_TEST(test_a_revert_to_the_version_the_manifest_now_names_is_reported);
    RUN_TEST(test_a_token_that_cannot_be_consumed_is_left_for_the_next_boot);
    RUN_TEST(test_an_unreadable_committed_version_is_consumed_unreported);
    RUN_TEST(test_a_boot_with_nothing_committed_reports_nothing_and_writes_nothing);
    return UNITY_END();
}
