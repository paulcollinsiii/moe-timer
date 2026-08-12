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

#define MANIFEST_UPDATE "[{\"schema\":1,\"default\":{\"version\":\"" TARGET "\",\"url\":\"" IMAGE_URL "\"}}]"
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

static int64_t mock_mono_ms(void) {
    return m_mono;
}

static bool mock_dl_begin(const char *url, ota_error_facts_t *facts) {
    note("dl_begin");
    snprintf(m_image_url_seen, sizeof(m_image_url_seen), "%s", url);
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

static bool mock_dl_finish(ota_error_facts_t *facts) {
    note("finish");
    if (!m_finish_ok) {
        *facts = m_finish_facts;
        return false;
    }
    return true;
}

static void mock_dl_abort(void) {
    note("abort");
}

static bool mock_session_begin(void) {
    note("net_on");
    return m_session_ok;
}

static void mock_session_end(void) {
    note("net_off");
}

static char m_paint_from[32], m_paint_to[32];

static void mock_paint_update(const char *from, const char *to) {
    note("paint");
    snprintf(m_paint_from, sizeof(m_paint_from), "%s", from != NULL ? from : "(null)");
    snprintf(m_paint_to, sizeof(m_paint_to), "%s", to != NULL ? to : "(null)");
}

static void mock_repaint(void) {
    note("repaint");
}

static void mock_extend_awake(int seconds) {
    note_int("extend", seconds);
}

static void mock_restart(void) {
    note("restart");
}

static uint32_t mock_free_heap(void) {
    return m_heap;
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
    .restart = mock_restart,
    .free_heap = mock_free_heap,
    .mono_ms = mock_mono_ms,
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

/* Arming is what clears last wake's decision. Without it a buffered
   update would survive into a wake that never checked and get applied
   from stale facts. */
static void test_arming_clears_the_previous_wake_s_pending_update(void) {
    check_at_rollover();
    TEST_ASSERT_TRUE(ota_flow_pending());
    ota_flow_arm(OTA_TRIGGER_NONE, 90, false);
    TEST_ASSERT_FALSE(ota_flow_pending());
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
     paint      before the radio comes up — display_ota() blocks on a full
                refresh and has no net_window_active() guard, so painting
                inside an open window is the brownout in net_window.c:65-79
     extend:300 after the paint, before the window: an extension applied
                once the download "looks slow" races the thing it protects
                against, and a paint charged to the download budget is 3 s
                the download does not get
     finish     before restart: the commit is what switches the boot
                partition, and it must be the last thing that can fail */
static void test_a_successful_update_paints_extends_downloads_commits_reboots(void) {
    check_at_rollover();
    ota_flow_apply(90, false);
    TEST_ASSERT_EQUAL_STRING("get paint extend:300 net_on dl_begin step step finish net_off restart", g_log);
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

static void test_a_successful_update_clears_the_retry_state(void) {
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_ota_target(TARGET));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_ota_fails(2));
    check_at_rollover();
    ota_flow_apply(90, false);
    TEST_ASSERT_EQUAL_UINT16(0, stored_fails());
    TEST_ASSERT_EQUAL_STRING("", stored_target());
    TEST_ASSERT_EQUAL_STRING("", stored_result());
}

static void test_the_download_duration_is_recorded(void) {
    m_step_advance_ms = 4000;
    check_at_rollover();
    ota_flow_apply(90, false);
    TEST_ASSERT_EQUAL_UINT32(8000, ota_flow_last_dl_ms());
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
    TEST_ASSERT_EQUAL_STRING("get paint extend:300 net_on dl_begin step step abort net_off extend:180 repaint", g_log);
    TEST_ASSERT_EQUAL_STRING("timeout", stored_result());
    TEST_ASSERT_EQUAL_STRING(TARGET, stored_target());
    TEST_ASSERT_EQUAL_UINT16(1, stored_fails());
}

static void test_a_transport_failure_repaints_and_does_not_reboot(void) {
    m_step_fails = true;
    m_step_facts.transport_failed = true;
    check_at_rollover();
    ota_flow_apply(90, false);
    TEST_ASSERT_EQUAL_STRING("get paint extend:300 net_on dl_begin step abort net_off extend:180 repaint", g_log);
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
    TEST_ASSERT_EQUAL_STRING("get paint extend:300 net_on dl_begin net_off extend:180 repaint", g_log);
    TEST_ASSERT_EQUAL_STRING("tls_cert", stored_result());
}

static void test_a_failed_commit_is_recorded_and_does_not_reboot(void) {
    m_finish_ok = false;
    m_finish_facts.image_rejected = true;
    check_at_rollover();
    ota_flow_apply(90, false);
    TEST_ASSERT_EQUAL_STRING("get paint extend:300 net_on dl_begin step step finish net_off extend:180 repaint", g_log);
    TEST_ASSERT_EQUAL_STRING("bad_image", stored_result());
}

/* A window that never opened has nothing to tear down — net_window.c does
   not call wifi_session_end() after a failed begin either. */
static void test_a_failed_association_repaints_without_tearing_down(void) {
    m_session_ok = false;
    check_at_rollover();
    ota_flow_apply(90, false);
    TEST_ASSERT_EQUAL_STRING("get paint extend:300 net_on extend:180 repaint", g_log);
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

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_rollover_always_checks);
    RUN_TEST(test_plain_tick_wake_never_checks);
    RUN_TEST(test_button_d_checks_only_when_ota_on_sync_is_set);
    RUN_TEST(test_arming_clears_the_previous_wake_s_pending_update);

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

    RUN_TEST(test_no_pending_update_opens_no_window);
    RUN_TEST(test_a_successful_update_paints_extends_downloads_commits_reboots);
    RUN_TEST(test_the_update_screen_names_both_versions);
    RUN_TEST(test_one_attempt_per_wake);
    RUN_TEST(test_a_successful_update_clears_the_retry_state);
    RUN_TEST(test_the_download_duration_is_recorded);

    RUN_TEST(test_low_heap_at_the_download_skips_without_painting);
    RUN_TEST(test_a_charge_lock_engaging_between_the_windows_cancels_the_download);

    RUN_TEST(test_the_deadline_aborts_cleanly_and_never_commits);
    RUN_TEST(test_a_transport_failure_repaints_and_does_not_reboot);
    RUN_TEST(test_a_rejected_certificate_is_reported_as_tls_cert);
    RUN_TEST(test_a_failed_commit_is_recorded_and_does_not_reboot);
    RUN_TEST(test_a_failed_association_repaints_without_tearing_down);

    RUN_TEST(test_three_failed_wakes_then_the_device_gives_up);
    RUN_TEST(test_a_new_target_version_rearms_a_given_up_device);
    RUN_TEST(test_a_failure_survives_into_the_next_window);
    return UNITY_END();
}
