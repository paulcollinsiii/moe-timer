#include <stdio.h>
#include <string.h>
#include <unity.h>

/* Single-TU compilation, same shape as test_mqtt_form.c and test_ota_flow.c.
   config_validate.c and mqtt_form.c come first because setup_session.c
   composes with mqtt_form.h's types and calls mqtt_form_error_str() /
   mqtt_form_html_escape() for real — the vocabulary under test is the
   real one, not a restatement. cJSON.h only (cJSON.c is its own TU via
   EXTRA_SRCS, same as test_mqtt_form). */
#include "../../main/config_validate.c"
#include "../../main/mqtt_form.c"
#include "cJSON.h"

/* device_id() is declared in device_id.h and called directly by
   setup_session.c (not threaded through setup_session_cfg_t) — the same
   pattern test_ota_flow.c uses for the same function. Mutable so a test
   can see what a non-standard id does to the AP SSID. */
static const char *s_dev_id = "magtag-a1b2c3";
const char *device_id(void) {
    return s_dev_id;
}

#include "../../main/setup_session.c"

/* ---- the call log, same shape as test_ota_flow.c's ---------------------- */

static char g_log[512];

static void note(const char *tag) {
    size_t used = strlen(g_log);
    if (used > 0 && used + 1 < sizeof(g_log)) {
        g_log[used++] = ' ';
        g_log[used] = '\0';
    }
    snprintf(g_log + used, sizeof(g_log) - used, "%s", tag);
}

/* ---- fakes for every ops field ------------------------------------------ */

static int m_extend_calls;
static int m_extend_arg;
static bool m_extend_ok;

static bool fake_extend_awake(int seconds) {
    m_extend_calls++;
    m_extend_arg = seconds;
    note("extend");
    return m_extend_ok;
}

static int m_rand_idx;

static uint8_t fake_rand_byte(void) {
    /* Always in-range for the 57-char alphabet (limit 228): deterministic,
       never rejected, so session-level tests need no knowledge of the
       password generator's internals. The dedicated password tests below
       script the sequence explicitly instead of using this fake. */
    uint8_t b = (uint8_t)((m_rand_idx++ * 7 + 11) % 200);
    return b;
}

static int m_start_calls;
static bool m_start_ok;
static char m_start_ssid[SETUP_SESSION_AP_SSID_MAX];
static char m_start_pass[SETUP_SESSION_AP_PASS_BUF];

static bool fake_start(const char *ap_ssid, const char *ap_password) {
    m_start_calls++;
    snprintf(m_start_ssid, sizeof(m_start_ssid), "%s", ap_ssid);
    snprintf(m_start_pass, sizeof(m_start_pass), "%s", ap_password);
    note("start");
    return m_start_ok;
}

static int m_stop_calls;

static void fake_stop(void) {
    m_stop_calls++;
    note("stop");
}

#define SCRIPT_MAX 8
static setup_session_event_t m_script_ev[SCRIPT_MAX];
static setup_session_poll_out_t m_script_out[SCRIPT_MAX];
static int m_script_len;
static int m_script_pos;
static int m_poll_calls;

static setup_session_event_t fake_poll(uint32_t timeout_ms, setup_session_poll_out_t *out) {
    (void)timeout_ms;
    m_poll_calls++;
    if (m_script_pos < m_script_len) {
        setup_session_event_t ev = m_script_ev[m_script_pos];
        *out = m_script_out[m_script_pos];
        m_script_pos++;
        note("poll:ev");
        return ev;
    }
    note("poll:none");
    return SETUP_SESSION_EVENT_NONE;
}

static int m_render_setup_calls;
static int m_render_complete_calls;
static int m_render_timeout_calls;
static setup_session_screen_info_t m_render_setup_info;

static void fake_render_setup(const setup_session_screen_info_t *info) {
    m_render_setup_calls++;
    m_render_setup_info = *info;
    note("render_setup");
}

static void fake_render_complete(void) {
    m_render_complete_calls++;
    note("render_complete");
}

static void fake_render_timeout(void) {
    m_render_timeout_calls++;
    note("render_timeout");
}

static int m_set_wifi_calls;
static bool m_set_wifi_ok;
static char m_set_wifi_ssid[40];
static char m_set_wifi_pass[80];

static bool fake_set_wifi(const char *ssid, const char *password) {
    m_set_wifi_calls++;
    snprintf(m_set_wifi_ssid, sizeof(m_set_wifi_ssid), "%s", ssid);
    snprintf(m_set_wifi_pass, sizeof(m_set_wifi_pass), "%s", password);
    note("set_wifi");
    return m_set_wifi_ok;
}

static int m_clear_driver_calls;

static void fake_clear_driver(void) {
    m_clear_driver_calls++;
    note("clear_driver");
}

static int m_set_mqtt_calls;
static bool m_set_mqtt_ok;
static char m_set_mqtt_uri[MQTT_FORM_URI_MAX];
static char m_set_mqtt_user[MQTT_FORM_USER_MAX];
static char m_set_mqtt_pass[MQTT_FORM_PASS_MAX];
static bool m_set_mqtt_keep_pass;

static bool fake_set_mqtt(const char *uri, const char *user, const char *pass, bool keep_pass) {
    m_set_mqtt_calls++;
    snprintf(m_set_mqtt_uri, sizeof(m_set_mqtt_uri), "%s", uri);
    snprintf(m_set_mqtt_user, sizeof(m_set_mqtt_user), "%s", user);
    snprintf(m_set_mqtt_pass, sizeof(m_set_mqtt_pass), "%s", pass);
    m_set_mqtt_keep_pass = keep_pass;
    note("set_mqtt");
    return m_set_mqtt_ok;
}

static setup_session_ops_t make_ops(void) {
    setup_session_ops_t ops;
    memset(&ops, 0, sizeof(ops));
    ops.extend_awake = fake_extend_awake;
    ops.rand_byte = fake_rand_byte;
    ops.start = fake_start;
    ops.stop = fake_stop;
    ops.poll = fake_poll;
    ops.render_setup_screen = fake_render_setup;
    ops.render_complete_screen = fake_render_complete;
    ops.render_timeout_screen = fake_render_timeout;
    ops.set_wifi_creds = fake_set_wifi;
    ops.clear_wifi_driver_store = fake_clear_driver;
    ops.set_mqtt_creds = fake_set_mqtt;
    return ops;
}

static void script_wifi_success(const char *ssid, const char *pass) {
    setup_session_poll_out_t out;
    memset(&out, 0, sizeof(out));
    snprintf(out.wifi_ssid, sizeof(out.wifi_ssid), "%s", ssid);
    snprintf(out.wifi_password, sizeof(out.wifi_password), "%s", pass);
    m_script_ev[m_script_len] = SETUP_SESSION_EVENT_WIFI_SUCCESS;
    m_script_out[m_script_len] = out;
    m_script_len++;
}

static void script_wifi_fail(void) {
    memset(&m_script_out[m_script_len], 0, sizeof(m_script_out[0]));
    m_script_ev[m_script_len] = SETUP_SESSION_EVENT_WIFI_FAIL;
    m_script_len++;
}

static void script_mqtt_submit(const char *uri, const char *user, const char *pass, bool keep_pass) {
    setup_session_poll_out_t out;
    memset(&out, 0, sizeof(out));
    snprintf(out.mqtt_uri, sizeof(out.mqtt_uri), "%s", uri);
    snprintf(out.mqtt_user, sizeof(out.mqtt_user), "%s", user);
    snprintf(out.mqtt_pass, sizeof(out.mqtt_pass), "%s", pass);
    out.mqtt_keep_pass = keep_pass;
    m_script_ev[m_script_len] = SETUP_SESSION_EVENT_MQTT_SUBMIT;
    m_script_out[m_script_len] = out;
    m_script_len++;
}

static void script_hard_error(void) {
    memset(&m_script_out[m_script_len], 0, sizeof(m_script_out[0]));
    m_script_ev[m_script_len] = SETUP_SESSION_EVENT_HARD_ERROR;
    m_script_len++;
}

static void assert_teardown_ran(void) {
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, m_stop_calls,
                                  "stop() must run exactly once on every path out of setup_session_run");
}

void setUp(void) {
    g_log[0] = '\0';
    s_dev_id = "magtag-a1b2c3";
    m_extend_calls = 0;
    m_extend_arg = 0;
    m_extend_ok = true;
    m_rand_idx = 0;
    m_start_calls = 0;
    m_start_ok = true;
    m_start_ssid[0] = '\0';
    m_start_pass[0] = '\0';
    m_stop_calls = 0;
    m_script_len = 0;
    m_script_pos = 0;
    m_poll_calls = 0;
    m_render_setup_calls = 0;
    m_render_complete_calls = 0;
    m_render_timeout_calls = 0;
    memset(&m_render_setup_info, 0, sizeof(m_render_setup_info));
    m_set_wifi_calls = 0;
    m_set_wifi_ok = true;
    m_set_wifi_ssid[0] = '\0';
    m_set_wifi_pass[0] = '\0';
    m_clear_driver_calls = 0;
    m_set_mqtt_calls = 0;
    m_set_mqtt_ok = true;
    m_set_mqtt_uri[0] = '\0';
    m_set_mqtt_user[0] = '\0';
    m_set_mqtt_pass[0] = '\0';
    m_set_mqtt_keep_pass = false;
}

void tearDown(void) {}

/* ===== pure helper: AP password =========================================== */

void test_ap_password_default_fake_is_full_length_and_in_alphabet(void) {
    char out[SETUP_SESSION_AP_PASS_BUF];
    setup_session_make_ap_password(fake_rand_byte, out);
    TEST_ASSERT_EQUAL_INT(SETUP_SESSION_AP_PASS_LEN, (int)strlen(out));
    static const char alphabet[] = "23456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
    for (int i = 0; i < SETUP_SESSION_AP_PASS_LEN; i++) {
        TEST_ASSERT_NOT_NULL_MESSAGE(strchr(alphabet, out[i]), "password character outside the unambiguous alphabet");
    }
}

/* 8-12 chars, WPA2 requires >= 8. */
void test_ap_password_length_is_wpa2_valid(void) {
    TEST_ASSERT_GREATER_OR_EQUAL(8, SETUP_SESSION_AP_PASS_LEN);
    TEST_ASSERT_LESS_OR_EQUAL(12, SETUP_SESSION_AP_PASS_LEN);
}

void test_ap_password_excludes_ambiguous_characters(void) {
    /* fake_rand_byte never emits a rejected byte (always < 200 < 228), so
       this confirms the mapped output directly against the exclusion
       list rather than merely restating "is it in the alphabet". */
    char out[SETUP_SESSION_AP_PASS_BUF];
    setup_session_make_ap_password(fake_rand_byte, out);
    for (int i = 0; i < SETUP_SESSION_AP_PASS_LEN; i++) {
        TEST_ASSERT_NOT_EQUAL('0', out[i]);
        TEST_ASSERT_NOT_EQUAL('O', out[i]);
        TEST_ASSERT_NOT_EQUAL('1', out[i]);
        TEST_ASSERT_NOT_EQUAL('l', out[i]);
        TEST_ASSERT_NOT_EQUAL('I', out[i]);
    }
}

static int m_script_rand_pos;
static int m_script_rand_calls;
static const uint8_t *m_script_rand_bytes;
static int m_script_rand_len;

static uint8_t scripted_rand_byte(void) {
    m_script_rand_calls++;
    uint8_t b = m_script_rand_bytes[m_script_rand_pos];
    if (m_script_rand_pos + 1 < m_script_rand_len)
        m_script_rand_pos++;
    return b;
}

void test_ap_password_rejects_out_of_range_bytes_without_modulo_bias(void) {
    /* limit = floor(256/57)*57 == 228, so every byte >= 228 must be
       SKIPPED rather than folded in by `% 57`. 230 is out of range, and
       230 % 57 == 2 -- so a buggy implementation that mapped instead of
       rejecting would produce alphabet[2] here. The byte that follows, 5,
       is in range and maps to alphabet[5], a value nothing else in this
       two-byte sequence could produce by coincidence. */
    static const uint8_t seq[] = {230, 5};
    m_script_rand_bytes = seq;
    m_script_rand_len = (int)(sizeof(seq) / sizeof(seq[0]));
    m_script_rand_pos = 0;
    m_script_rand_calls = 0;

    char out[SETUP_SESSION_AP_PASS_BUF];
    setup_session_make_ap_password(scripted_rand_byte, out);

    static const char alphabet[] = "23456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
    TEST_ASSERT_EQUAL_INT(alphabet[5], out[0]);
    /* The first character cost 2 calls (230 rejected, then 5 accepted);
       every character after it costs exactly 1 (the sequence's own last
       entry, 5, repeating and always accepted). 2 + 9 == 11 for a
       10-character password -- one more than a naive per-character mapping
       could ever consume. */
    TEST_ASSERT_EQUAL_INT(SETUP_SESSION_AP_PASS_LEN + 1, m_script_rand_calls);
}

/* ===== pure helper: AP SSID ================================================ */

void test_ap_ssid_strips_the_devices_own_magtag_prefix(void) {
    s_dev_id = "magtag-a1b2c3";
    char out[SETUP_SESSION_AP_SSID_MAX];
    setup_session_make_ap_ssid(out);
    TEST_ASSERT_EQUAL_STRING("MagTag-a1b2c3", out);
}

void test_ap_ssid_without_the_prefix_falls_back_to_the_whole_id(void) {
    s_dev_id = "weirdid123";
    char out[SETUP_SESSION_AP_SSID_MAX];
    setup_session_make_ap_ssid(out);
    TEST_ASSERT_EQUAL_STRING("MagTag-weirdid123", out);
}

void test_ap_ssid_fits_32_bytes_even_for_a_long_id(void) {
    s_dev_id = "magtag-0123456789abcdef0123456789abcdef";
    char out[SETUP_SESSION_AP_SSID_MAX];
    setup_session_make_ap_ssid(out);
    TEST_ASSERT_LESS_OR_EQUAL(SETUP_SESSION_AP_SSID_MAX - 1, (int)strlen(out));
}

/* ===== pure helper: QR payload ============================================= */

void test_qr_payload_matches_the_espressif_sec2_softap_format(void) {
    char out[SETUP_SESSION_QR_MAX];
    bool ok = setup_session_make_qr_payload("MagTag-a1b2c3", "Xk3mQ9Lp7R", out, sizeof(out));
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_STRING(
        "{\"ver\":\"v1\",\"name\":\"MagTag-a1b2c3\",\"username\":\"magtag\",\"pop\":\"Xk3mQ9Lp7R\",\"transport\":"
        "\"softap\"}",
        out);
}

void test_qr_payload_too_small_buffer_fails_and_empties(void) {
    char out[8] = "garbage";
    bool ok = setup_session_make_qr_payload("MagTag-a1b2c3", "Xk3mQ9Lp7R", out, sizeof(out));
    TEST_ASSERT_FALSE(ok);
    TEST_ASSERT_EQUAL_STRING("", out);
}

/* ===== pure helper: MQTT status line ======================================= */

void test_format_mqtt_status_ok_says_saved(void) {
    char out[64];
    mqtt_form_status_t st = {MQTT_FORM_ERR_NONE, MQTT_FORM_FIELD_NONE};
    setup_session_format_mqtt_status(true, st, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("Saved.", out);
}

void test_format_mqtt_status_names_the_field_when_there_is_one(void) {
    char out[64];
    mqtt_form_status_t st = {MQTT_FORM_ERR_BAD_SCHEME, MQTT_FORM_FIELD_URI};
    setup_session_format_mqtt_status(false, st, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("uri: must start with mqtt:// or mqtts://", out);
}

void test_format_mqtt_status_omits_the_field_label_when_there_is_none(void) {
    char out[64];
    mqtt_form_status_t st = {MQTT_FORM_ERR_BODY_TOO_LONG, MQTT_FORM_FIELD_NONE};
    setup_session_format_mqtt_status(false, st, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("request body is too long", out);
}

/* ===== pure helper: the /mqtt page ========================================= */

#define PAGE_CAP 2048
static char m_page_buf[PAGE_CAP];
static size_t m_page_len;
static int m_sink_calls;
static int m_sink_fail_after; /* 0 = never fail */

static bool page_sink(const char *chunk, size_t len, void *ctx) {
    (void)ctx;
    m_sink_calls++;
    if (m_sink_fail_after > 0 && m_sink_calls > m_sink_fail_after)
        return false;
    TEST_ASSERT_LESS_OR_EQUAL(PAGE_CAP, m_page_len + len + 1);
    memcpy(m_page_buf + m_page_len, chunk, len);
    m_page_len += len;
    m_page_buf[m_page_len] = '\0';
    return true;
}

static void reset_page_capture(void) {
    m_page_buf[0] = '\0';
    m_page_len = 0;
    m_sink_calls = 0;
    m_sink_fail_after = 0;
}

void test_mqtt_page_prefills_uri_and_user_escaped(void) {
    reset_page_capture();
    setup_session_mqtt_page_in_t in = {.uri_escaped = "mqtt://broker&co", .user_escaped = "bob", .status_msg = NULL};
    bool ok = setup_session_render_mqtt_page(&in, page_sink, NULL);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_NOT_NULL(strstr(m_page_buf, "mqtt://broker&co"));
    TEST_ASSERT_NOT_NULL(strstr(m_page_buf, "bob"));
    TEST_ASSERT_NULL(strstr(m_page_buf, "<script"));
}

void test_mqtt_page_never_prefills_a_password(void) {
    reset_page_capture();
    setup_session_mqtt_page_in_t in = {.uri_escaped = "", .user_escaped = "", .status_msg = NULL};
    setup_session_render_mqtt_page(&in, page_sink, NULL);
    /* No password was ever given to this function at all -- the type
       signature has no field for one -- so the strongest check available
       is structural: the password input carries no value attribute. */
    TEST_ASSERT_NULL(strstr(m_page_buf, "name=pass value="));
}

void test_mqtt_page_shows_the_status_banner_when_present(void) {
    reset_page_capture();
    setup_session_mqtt_page_in_t in = {.uri_escaped = "", .user_escaped = "", .status_msg = "uri: bad scheme"};
    setup_session_render_mqtt_page(&in, page_sink, NULL);
    TEST_ASSERT_NOT_NULL(strstr(m_page_buf, "uri: bad scheme"));
}

void test_mqtt_page_omits_the_banner_on_a_plain_load(void) {
    reset_page_capture();
    setup_session_mqtt_page_in_t in = {.uri_escaped = "", .user_escaped = "", .status_msg = NULL};
    setup_session_render_mqtt_page(&in, page_sink, NULL);
    TEST_ASSERT_NULL(strstr(m_page_buf, "<b>"));
}

void test_mqtt_page_stops_the_moment_the_sink_refuses(void) {
    reset_page_capture();
    m_sink_fail_after = 1;
    setup_session_mqtt_page_in_t in = {.uri_escaped = "x", .user_escaped = "y", .status_msg = "z"};
    bool ok = setup_session_render_mqtt_page(&in, page_sink, NULL);
    TEST_ASSERT_FALSE(ok);
    /* fail_after=1: the first chunk is accepted, the second is refused.
       Exactly two sink calls happen in total -- nothing after the
       refusal was attempted, which is the property under test. */
    TEST_ASSERT_EQUAL_INT(2, m_sink_calls);
}

/* ===== the session state machine =========================================== */

void test_success_with_wifi_only(void) {
    script_wifi_success("HomeNet", "homepass1");
    setup_session_ops_t ops = make_ops();
    setup_session_cfg_t cfg = {.has_wifi_ssid = false, .budget_sec = 600};

    setup_session_result_t r = setup_session_run(&ops, &cfg);

    TEST_ASSERT_EQUAL(SETUP_SESSION_OUTCOME_WIFI_OK, r.outcome);
    TEST_ASSERT_EQUAL(SETUP_SESSION_SLEEP_NET_WINDOW, r.sleep);
    TEST_ASSERT_EQUAL_INT(1, m_set_wifi_calls);
    TEST_ASSERT_EQUAL_STRING("HomeNet", m_set_wifi_ssid);
    TEST_ASSERT_EQUAL_STRING("homepass1", m_set_wifi_pass);
    TEST_ASSERT_EQUAL_INT(1, m_clear_driver_calls);
    TEST_ASSERT_EQUAL_INT(0, m_set_mqtt_calls);
    TEST_ASSERT_EQUAL_INT(1, m_render_setup_calls);
    TEST_ASSERT_EQUAL_INT(1, m_render_complete_calls);
    TEST_ASSERT_EQUAL_INT(0, m_render_timeout_calls);
    /* start() and render_setup_screen() must agree on the AP credentials. */
    TEST_ASSERT_EQUAL_STRING(m_start_ssid, m_render_setup_info.ap_ssid);
    TEST_ASSERT_EQUAL_STRING(m_start_pass, m_render_setup_info.ap_password);
    TEST_ASSERT_TRUE(strncmp(m_render_setup_info.ap_ssid, "MagTag-", 7) == 0);
    assert_teardown_ran();
}

void test_wifi_plus_mqtt_in_the_same_session(void) {
    script_mqtt_submit("mqtt://broker", "bob", "secret", false);
    script_wifi_success("HomeNet", "homepass1");
    setup_session_ops_t ops = make_ops();
    setup_session_cfg_t cfg = {.has_wifi_ssid = false, .budget_sec = 600};

    setup_session_result_t r = setup_session_run(&ops, &cfg);

    TEST_ASSERT_EQUAL(SETUP_SESSION_OUTCOME_WIFI_OK, r.outcome);
    TEST_ASSERT_EQUAL(SETUP_SESSION_SLEEP_NET_WINDOW, r.sleep);
    TEST_ASSERT_EQUAL_INT(1, m_set_wifi_calls);
    TEST_ASSERT_EQUAL_INT(1, m_set_mqtt_calls);
    TEST_ASSERT_EQUAL_STRING("mqtt://broker", m_set_mqtt_uri);
    TEST_ASSERT_FALSE(m_set_mqtt_keep_pass);
    assert_teardown_ran();
}

void test_mqtt_only_with_an_ssid_already_present(void) {
    script_mqtt_submit("mqtt://broker", "bob", "", true);
    setup_session_ops_t ops = make_ops();
    setup_session_cfg_t cfg = {.has_wifi_ssid = true, .budget_sec = 600};

    setup_session_result_t r = setup_session_run(&ops, &cfg);

    TEST_ASSERT_EQUAL(SETUP_SESSION_OUTCOME_MQTT_ONLY, r.outcome);
    TEST_ASSERT_EQUAL(SETUP_SESSION_SLEEP_NET_WINDOW, r.sleep);
    TEST_ASSERT_EQUAL_INT(0, m_set_wifi_calls);
    TEST_ASSERT_EQUAL_INT(0, m_clear_driver_calls);
    TEST_ASSERT_EQUAL_INT(1, m_set_mqtt_calls);
    TEST_ASSERT_TRUE(m_set_mqtt_keep_pass);
    TEST_ASSERT_EQUAL_INT(1, m_render_complete_calls);
    assert_teardown_ran();
}

void test_mqtt_submitted_on_a_no_ssid_device_then_times_out(void) {
    script_mqtt_submit("mqtt://broker", "bob", "secret", false);
    setup_session_ops_t ops = make_ops();
    setup_session_cfg_t cfg = {.has_wifi_ssid = false, .budget_sec = 1}; /* 4 poll quanta at 250 ms */

    setup_session_result_t r = setup_session_run(&ops, &cfg);

    TEST_ASSERT_EQUAL(SETUP_SESSION_OUTCOME_TIMEOUT, r.outcome);
    TEST_ASSERT_EQUAL(SETUP_SESSION_SLEEP_BUTTON_ONLY, r.sleep);
    TEST_ASSERT_EQUAL_INT(1, m_set_mqtt_calls); /* stored even though the session keeps waiting */
    TEST_ASSERT_EQUAL_INT(0, m_set_wifi_calls);
    TEST_ASSERT_EQUAL_INT(0, m_render_complete_calls);
    TEST_ASSERT_EQUAL_INT(1, m_render_timeout_calls);
    TEST_ASSERT_EQUAL_INT((cfg.budget_sec * 1000) / (int)SETUP_SESSION_POLL_QUANTUM_MS, m_poll_calls);
    assert_teardown_ran();
}

void test_a_wrong_wifi_password_is_not_stored_and_the_session_continues(void) {
    script_wifi_fail();
    script_wifi_success("HomeNet", "homepass1");
    setup_session_ops_t ops = make_ops();
    setup_session_cfg_t cfg = {.has_wifi_ssid = false, .budget_sec = 600};

    setup_session_result_t r = setup_session_run(&ops, &cfg);

    TEST_ASSERT_EQUAL(SETUP_SESSION_OUTCOME_WIFI_OK, r.outcome);
    TEST_ASSERT_EQUAL_INT(1, m_set_wifi_calls); /* only for the eventual success */
    assert_teardown_ran();
}

void test_timeout_with_no_ssid_sleeps_buttons_only(void) {
    setup_session_ops_t ops = make_ops();
    setup_session_cfg_t cfg = {.has_wifi_ssid = false, .budget_sec = 1};

    setup_session_result_t r = setup_session_run(&ops, &cfg);

    TEST_ASSERT_EQUAL(SETUP_SESSION_OUTCOME_TIMEOUT, r.outcome);
    TEST_ASSERT_EQUAL(SETUP_SESSION_SLEEP_BUTTON_ONLY, r.sleep);
    TEST_ASSERT_EQUAL_INT(1, m_render_timeout_calls);
    assert_teardown_ran();
}

void test_timeout_with_an_ssid_present_sleeps_the_normal_schedule(void) {
    setup_session_ops_t ops = make_ops();
    setup_session_cfg_t cfg = {.has_wifi_ssid = true, .budget_sec = 1};

    setup_session_result_t r = setup_session_run(&ops, &cfg);

    TEST_ASSERT_EQUAL(SETUP_SESSION_OUTCOME_TIMEOUT, r.outcome);
    TEST_ASSERT_EQUAL(SETUP_SESSION_SLEEP_NORMAL, r.sleep);
    assert_teardown_ran();
}

void test_start_failure_is_a_hard_error_with_no_setup_screen(void) {
    m_start_ok = false;
    setup_session_ops_t ops = make_ops();
    setup_session_cfg_t cfg = {.has_wifi_ssid = false, .budget_sec = 600};

    setup_session_result_t r = setup_session_run(&ops, &cfg);

    TEST_ASSERT_EQUAL(SETUP_SESSION_OUTCOME_ERROR, r.outcome);
    TEST_ASSERT_EQUAL(SETUP_SESSION_SLEEP_BUTTON_ONLY, r.sleep);
    TEST_ASSERT_EQUAL_INT(0, m_render_setup_calls);
    TEST_ASSERT_EQUAL_INT(0, m_render_complete_calls);
    TEST_ASSERT_EQUAL_INT(0, m_render_timeout_calls);
    TEST_ASSERT_EQUAL_INT(0, m_poll_calls);
    assert_teardown_ran();
}

void test_start_failure_with_an_ssid_present_sleeps_the_normal_schedule(void) {
    m_start_ok = false;
    setup_session_ops_t ops = make_ops();
    setup_session_cfg_t cfg = {.has_wifi_ssid = true, .budget_sec = 600};

    setup_session_result_t r = setup_session_run(&ops, &cfg);

    TEST_ASSERT_EQUAL(SETUP_SESSION_OUTCOME_ERROR, r.outcome);
    TEST_ASSERT_EQUAL(SETUP_SESSION_SLEEP_NORMAL, r.sleep);
    assert_teardown_ran();
}

/* The earliest possible exit: nothing after step 1 ever ran, and teardown
   still has to be the thing that runs. This is the path that makes "always"
   true rather than "true for every path this suite happened to try". */
void test_extend_awake_failure_is_a_hard_error_and_still_tears_down(void) {
    m_extend_ok = false;
    setup_session_ops_t ops = make_ops();
    setup_session_cfg_t cfg = {.has_wifi_ssid = false, .budget_sec = 600};

    setup_session_result_t r = setup_session_run(&ops, &cfg);

    TEST_ASSERT_EQUAL(SETUP_SESSION_OUTCOME_ERROR, r.outcome);
    TEST_ASSERT_EQUAL_INT(1, m_extend_calls);
    TEST_ASSERT_EQUAL_INT(600, m_extend_arg);
    TEST_ASSERT_EQUAL_INT(0, m_start_calls);
    TEST_ASSERT_EQUAL_INT(0, m_poll_calls);
    assert_teardown_ran();
}

void test_a_hard_error_mid_session_ends_the_session(void) {
    script_hard_error();
    setup_session_ops_t ops = make_ops();
    setup_session_cfg_t cfg = {.has_wifi_ssid = false, .budget_sec = 600};

    setup_session_result_t r = setup_session_run(&ops, &cfg);

    TEST_ASSERT_EQUAL(SETUP_SESSION_OUTCOME_ERROR, r.outcome);
    TEST_ASSERT_EQUAL_INT(0, m_render_complete_calls);
    TEST_ASSERT_EQUAL_INT(0, m_render_timeout_calls);
    assert_teardown_ran();
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_ap_password_default_fake_is_full_length_and_in_alphabet);
    RUN_TEST(test_ap_password_length_is_wpa2_valid);
    RUN_TEST(test_ap_password_excludes_ambiguous_characters);
    RUN_TEST(test_ap_password_rejects_out_of_range_bytes_without_modulo_bias);

    RUN_TEST(test_ap_ssid_strips_the_devices_own_magtag_prefix);
    RUN_TEST(test_ap_ssid_without_the_prefix_falls_back_to_the_whole_id);
    RUN_TEST(test_ap_ssid_fits_32_bytes_even_for_a_long_id);

    RUN_TEST(test_qr_payload_matches_the_espressif_sec2_softap_format);
    RUN_TEST(test_qr_payload_too_small_buffer_fails_and_empties);

    RUN_TEST(test_format_mqtt_status_ok_says_saved);
    RUN_TEST(test_format_mqtt_status_names_the_field_when_there_is_one);
    RUN_TEST(test_format_mqtt_status_omits_the_field_label_when_there_is_none);

    RUN_TEST(test_mqtt_page_prefills_uri_and_user_escaped);
    RUN_TEST(test_mqtt_page_never_prefills_a_password);
    RUN_TEST(test_mqtt_page_shows_the_status_banner_when_present);
    RUN_TEST(test_mqtt_page_omits_the_banner_on_a_plain_load);
    RUN_TEST(test_mqtt_page_stops_the_moment_the_sink_refuses);

    RUN_TEST(test_success_with_wifi_only);
    RUN_TEST(test_wifi_plus_mqtt_in_the_same_session);
    RUN_TEST(test_mqtt_only_with_an_ssid_already_present);
    RUN_TEST(test_mqtt_submitted_on_a_no_ssid_device_then_times_out);
    RUN_TEST(test_a_wrong_wifi_password_is_not_stored_and_the_session_continues);
    RUN_TEST(test_timeout_with_no_ssid_sleeps_buttons_only);
    RUN_TEST(test_timeout_with_an_ssid_present_sleeps_the_normal_schedule);
    RUN_TEST(test_start_failure_is_a_hard_error_with_no_setup_screen);
    RUN_TEST(test_start_failure_with_an_ssid_present_sleeps_the_normal_schedule);
    RUN_TEST(test_extend_awake_failure_is_a_hard_error_and_still_tears_down);
    RUN_TEST(test_a_hard_error_mid_session_ends_the_session);
    return UNITY_END();
}
