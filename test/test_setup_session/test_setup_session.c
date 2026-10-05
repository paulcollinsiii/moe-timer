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
#include "qr_render.h"

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

/* Sized for the heaviest single test: the full-length success linger
   (SETUP_SESSION_SUCCESS_LINGER_MS / SETUP_SESSION_POLL_QUANTUM_MS == 60
   "poll:none" tokens, ~600 bytes) plus every other landmark call a
   session makes. Reset in setUp(), so this only ever has to hold ONE
   test's worth of log, not the whole suite's. */
static char g_log[2048];

static void note(const char *tag) {
    size_t used = strlen(g_log);
    if (used > 0 && used + 1 < sizeof(g_log)) {
        g_log[used++] = ' ';
        g_log[used] = '\0';
    }
    snprintf(g_log + used, sizeof(g_log) - used, "%s", tag);
}

/* Asserts `first` happened before `second` in the call log, by
   position rather than an exact full-log string match — a full-string
   match would also have to hardcode exactly how many "poll:none"/
   "poll:ev" tokens land between the landmarks in each scenario, which
   varies with the fake clock and would make these assertions fragile
   against a harmless change to how many times the loop happens to poll.
   Comparing substring POSITIONS still proves the real property (one
   call ran before another) without that coupling. */
static void assert_before(const char *log, const char *first, const char *second) {
    const char *p1 = strstr(log, first);
    const char *p2 = strstr(log, second);
    TEST_ASSERT_NOT_NULL_MESSAGE(p1, first);
    TEST_ASSERT_NOT_NULL_MESSAGE(p2, second);
    TEST_ASSERT_TRUE_MESSAGE(p1 < p2, first);
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

/* Wall clock, fake. Every fake_poll() call below advances this by the
   `timeout_ms` it was given, BEFORE reporting an event — "the tests'
   fake clock advances per poll", matching how the real device's
   xQueueReceive(..., pdMS_TO_TICKS(timeout_ms)) genuinely spends that
   much wall time whether or not something was already waiting. */
static uint32_t m_fake_now_ms;

static uint32_t fake_now_ms(void) {
    return m_fake_now_ms;
}

static int m_rand_idx;

static uint8_t fake_rand_byte(void) {
    /* Always in-range for the 49-char alphabet (limit 245): deterministic,
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
    m_poll_calls++;
    m_fake_now_ms += timeout_ms;
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
static int m_render_end_calls;
static setup_session_end_kind_t m_render_end_kind;
static bool m_render_end_has_ssid;
static setup_session_screen_info_t m_render_setup_info;

static void fake_render_setup(const setup_session_screen_info_t *info) {
    m_render_setup_calls++;
    m_render_setup_info = *info;
    note("render_setup");
}

static void fake_render_end(setup_session_end_kind_t kind, bool has_wifi_ssid) {
    m_render_end_calls++;
    m_render_end_kind = kind;
    m_render_end_has_ssid = has_wifi_ssid;
    note("render_end");
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

static int m_join_calls;
static bool m_join_ok;
static char m_join_ssid[40];
static char m_join_pass[80];

static bool fake_join_wifi(const char *ssid, const char *password) {
    m_join_calls++;
    snprintf(m_join_ssid, sizeof(m_join_ssid), "%s", ssid);
    snprintf(m_join_pass, sizeof(m_join_pass), "%s", password);
    note("join");
    return m_join_ok;
}

static setup_session_ops_t make_ops(void) {
    setup_session_ops_t ops;
    memset(&ops, 0, sizeof(ops));
    ops.join_wifi = fake_join_wifi;
    ops.extend_awake = fake_extend_awake;
    ops.now_ms = fake_now_ms;
    ops.rand_byte = fake_rand_byte;
    ops.start = fake_start;
    ops.stop = fake_stop;
    ops.poll = fake_poll;
    ops.render_setup_screen = fake_render_setup;
    ops.render_end_screen = fake_render_end;
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

/* No payload: by the time this event reaches setup_session_run, the
   httpd handler (setup_session_apply_mqtt, exercised directly below)
   has already stored the credentials. This event only says "stored". */
static void script_mqtt_stored(void) {
    memset(&m_script_out[m_script_len], 0, sizeof(m_script_out[0]));
    m_script_ev[m_script_len] = SETUP_SESSION_EVENT_MQTT_STORED;
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
    m_fake_now_ms = 0;
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
    m_render_end_calls = 0;
    m_render_end_kind = SETUP_SESSION_END_WIFI_SAVED;
    m_render_end_has_ssid = false;
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
    m_join_calls = 0;
    m_join_ok = true;
    m_join_ssid[0] = '\0';
    m_join_pass[0] = '\0';
}

void tearDown(void) {
    /* qr_render_encode() heap-allocates (main/qr_render.c); only one test
       in this file calls it, but releasing unconditionally here — a
       documented no-op otherwise — means a future test that encodes and
       forgets to clean up fails with its own assertion, not with an
       end-of-process ASan leak report pointing at an unrelated test. */
    qr_render_release();
}

/* ===== pure helper: AP password =========================================== */

void test_ap_password_default_fake_is_full_length_and_in_alphabet(void) {
    char out[SETUP_SESSION_AP_PASS_BUF];
    setup_session_make_ap_password(fake_rand_byte, out);
    TEST_ASSERT_EQUAL_INT(SETUP_SESSION_AP_PASS_LEN, (int)strlen(out));
    static const char alphabet[] = "3467ACDEFGHJKLMNPQRTUVWXYabcdefhijkmnopqrstuvwxyz";
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
    /* fake_rand_byte never emits a rejected byte (always < 200 < 245), so
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
        /* Confusable at 18 pt (the setup screen's password font), cut for
           the same reason as the five above. */
        TEST_ASSERT_NOT_EQUAL('S', out[i]);
        TEST_ASSERT_NOT_EQUAL('5', out[i]);
        TEST_ASSERT_NOT_EQUAL('Z', out[i]);
        TEST_ASSERT_NOT_EQUAL('2', out[i]);
        TEST_ASSERT_NOT_EQUAL('B', out[i]);
        TEST_ASSERT_NOT_EQUAL('8', out[i]);
        TEST_ASSERT_NOT_EQUAL('g', out[i]);
        TEST_ASSERT_NOT_EQUAL('9', out[i]);
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
    /* limit = floor(256/49)*49 == 245, so every byte >= 245 must be
       SKIPPED rather than folded in by `% 49`. 246 is out of range, and
       246 % 49 == 1 -- so a buggy implementation that mapped instead of
       rejecting would produce alphabet[1] here. The byte that follows, 5,
       is in range and maps to alphabet[5], a value nothing else in this
       two-byte sequence could produce by coincidence. */
    static const uint8_t seq[] = {246, 5};
    m_script_rand_bytes = seq;
    m_script_rand_len = (int)(sizeof(seq) / sizeof(seq[0]));
    m_script_rand_pos = 0;
    m_script_rand_calls = 0;

    char out[SETUP_SESSION_AP_PASS_BUF];
    setup_session_make_ap_password(scripted_rand_byte, out);

    static const char alphabet[] = "3467ACDEFGHJKLMNPQRTUVWXYabcdefhijkmnopqrstuvwxyz";
    TEST_ASSERT_EQUAL_INT(alphabet[5], out[0]);
    /* The first character cost 2 calls (246 rejected, then 5 accepted);
       every character after it costs exactly 1 (the sequence's own last
       entry, 5, repeating and always accepted). 2 + 9 == 11 for a
       10-character password -- one more than a naive per-character mapping
       could ever consume. */
    TEST_ASSERT_EQUAL_INT(SETUP_SESSION_AP_PASS_LEN + 1, m_script_rand_calls);
}

void test_ap_password_rejection_boundary_245_vs_244(void) {
    /* limit == 245 exactly: 245 must be REJECTED (>= limit) and cost a
       second draw; 244 is the largest byte that must be ACCEPTED. Once
       244 is drawn, scripted_rand_byte keeps returning it (it is the
       sequence's last entry), so every remaining character costs
       exactly 1 call — the same "+1 total calls" signature as the 246
       case above, but anchored at the boundary itself rather than deep
       in the rejected range. */
    static const uint8_t seq[] = {245, 244};
    m_script_rand_bytes = seq;
    m_script_rand_len = (int)(sizeof(seq) / sizeof(seq[0]));
    m_script_rand_pos = 0;
    m_script_rand_calls = 0;

    char out[SETUP_SESSION_AP_PASS_BUF];
    setup_session_make_ap_password(scripted_rand_byte, out);

    static const char alphabet[] = "3467ACDEFGHJKLMNPQRTUVWXYabcdefhijkmnopqrstuvwxyz";
    TEST_ASSERT_EQUAL_INT(alphabet[244 % 49], out[0]);
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

void test_qr_payload_is_the_standard_wifi_join_string(void) {
    char out[SETUP_SESSION_QR_MAX];
    bool ok = setup_session_make_qr_payload("MagTag-a1b2c3", "Xk3mQ9Lp7R", out, sizeof(out));
    TEST_ASSERT_TRUE(ok);
    /* The exact bytes a phone camera's WIFI: parser reads. */
    TEST_ASSERT_EQUAL_STRING("WIFI:T:WPA;S:MagTag-a1b2c3;P:Xk3mQ9Lp7R;;", out);
}

void test_qr_payload_escapes_every_special_character_with_a_backslash(void) {
    char out[SETUP_SESSION_QR_MAX];
    TEST_ASSERT_TRUE(setup_session_make_qr_payload("a\\b;c,d:e\"f", "p\\;,:\"q", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("WIFI:T:WPA;S:a\\\\b\\;c\\,d\\:e\\\"f;P:p\\\\\\;\\,\\:\\\"q;;", out);
}

void test_qr_payload_leaves_other_characters_alone(void) {
    char out[SETUP_SESSION_QR_MAX];
    TEST_ASSERT_TRUE(setup_session_make_qr_payload("My Net-1_x.y", "p@ss/w0rd!#", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("WIFI:T:WPA;S:My Net-1_x.y;P:p@ss/w0rd!#;;", out);
}

void test_qr_payload_too_small_buffer_fails_and_empties(void) {
    char out[8] = "garbage";
    bool ok = setup_session_make_qr_payload("MagTag-a1b2c3", "Xk3mQ9Lp7R", out, sizeof(out));
    TEST_ASSERT_FALSE(ok);
    TEST_ASSERT_EQUAL_STRING("", out);
}

/* The buffer boundary to the byte: the real payload is 41 characters, so
   42 bytes (with the NUL) is the smallest buffer that takes it and 41
   is one short. */
void test_qr_payload_capacity_boundary_is_exact(void) {
    char out[64];
    TEST_ASSERT_TRUE(setup_session_make_qr_payload("MagTag-a1b2c3", "Xk3mQ9Lp7R", out, 42));
    TEST_ASSERT_EQUAL_INT(41, (int)strlen(out));
    memset(out, 'x', sizeof(out));
    TEST_ASSERT_FALSE(setup_session_make_qr_payload("MagTag-a1b2c3", "Xk3mQ9Lp7R", out, 41));
    TEST_ASSERT_EQUAL_STRING("", out);
}

/* An escape that would be cut in half must fail rather than leave a
   dangling backslash that a scanner would read as escaping the next
   field's separator. */
void test_qr_payload_never_truncates_inside_an_escape(void) {
    char out[64];
    /* "WIFI:T:WPA;S:" is 13, then `\;` is 2 more: 15, then ";P:" 18, "x" 19, ";;" 21, NUL 22 */
    TEST_ASSERT_TRUE(setup_session_make_qr_payload(";", "x", out, 22));
    TEST_ASSERT_EQUAL_STRING("WIFI:T:WPA;S:\\;;P:x;;", out);
    TEST_ASSERT_FALSE(setup_session_make_qr_payload(";", "x", out, 21));
    TEST_ASSERT_EQUAL_STRING("", out);
}

void test_qr_payload_rejects_null_and_zero_capacity(void) {
    char out[8];
    TEST_ASSERT_FALSE(setup_session_make_qr_payload(NULL, "x", out, sizeof(out)));
    TEST_ASSERT_FALSE(setup_session_make_qr_payload("x", NULL, out, sizeof(out)));
    TEST_ASSERT_FALSE(setup_session_make_qr_payload("x", "y", out, 0));
    TEST_ASSERT_FALSE(setup_session_make_qr_payload("x", "y", NULL, 8));
}

/* The real payload: device_id() (device_id.c) always returns "magtag-"
   plus exactly 6 hex nibbles, so setup_session_make_ap_ssid() always
   produces a 13-byte SSID and the payload this device ever sends is
   always exactly 41 bytes. This builds it through the real pure helpers
   (not a hand copy) and asserts it fits SETUP_SESSION_QR_MAX and encodes
   at the version the panel block is sized for, so a change to any of
   make_ap_ssid/make_ap_password/make_qr_payload that grew it past
   qr_render.h's ceiling is caught here, not by a blank panel. */
void test_the_real_payload_encodes_at_the_version_the_panel_is_sized_for(void) {
    s_dev_id = "magtag-a1b2c3"; /* any 6 hex nibbles; device_id() never varies the length */
    char ssid[SETUP_SESSION_AP_SSID_MAX];
    setup_session_make_ap_ssid(ssid);
    TEST_ASSERT_EQUAL_INT(13, (int)strlen(ssid));

    char pw[SETUP_SESSION_AP_PASS_BUF];
    setup_session_make_ap_password(fake_rand_byte, pw);

    char payload[SETUP_SESSION_QR_MAX];
    TEST_ASSERT_TRUE(setup_session_make_qr_payload(ssid, pw, payload, sizeof(payload)));
    TEST_ASSERT_EQUAL_INT(41, (int)strlen(payload));

    int size = -1;
    TEST_ASSERT_TRUE(qr_render_encode(payload, &size));
    TEST_ASSERT_EQUAL_INT(QR_RENDER_MAX_MODULES, size);
    TEST_ASSERT_EQUAL_INT(29, size); /* version 3 */
}

/* The widest SSID the AP ever has (31 bytes) with a 10-character password
   still builds into the buffer, even though qr_render rejects it and the
   screen falls back to text. */
void test_a_31_byte_ssid_fits_the_buffer_even_if_it_will_not_encode(void) {
    char ssid[SETUP_SESSION_AP_SSID_MAX];
    memset(ssid, 'S', 31);
    ssid[31] = '\0';
    char payload[SETUP_SESSION_QR_MAX];
    TEST_ASSERT_TRUE(setup_session_make_qr_payload(ssid, "ABCDEFGHJK", payload, sizeof(payload)));
    int size = -1;
    TEST_ASSERT_FALSE(qr_render_encode(payload, &size));
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

/* ===== pure helper: setup_session_apply_mqtt =============================== */
/* Coverage for the parse-result -> stored-credential step, pulled out of
   setup_session_idf.c's two handlers into one pure function and tested
   directly here, rather than only through setup_session_run (which no
   longer calls set_mqtt_creds at all; storage happens before the session
   ever hears about it). */

void test_apply_mqtt_forwards_the_result_to_set_mqtt_creds(void) {
    mqtt_form_result_t result;
    memset(&result, 0, sizeof(result));
    snprintf(result.uri, sizeof(result.uri), "mqtt://broker");
    snprintf(result.user, sizeof(result.user), "bob");
    snprintf(result.pass, sizeof(result.pass), "secret");
    result.keep_pass = false;
    setup_session_ops_t ops = make_ops();

    bool ok = setup_session_apply_mqtt(&ops, &result);

    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_INT(1, m_set_mqtt_calls);
    TEST_ASSERT_EQUAL_STRING("mqtt://broker", m_set_mqtt_uri);
    TEST_ASSERT_EQUAL_STRING("bob", m_set_mqtt_user);
    TEST_ASSERT_EQUAL_STRING("secret", m_set_mqtt_pass);
    TEST_ASSERT_FALSE(m_set_mqtt_keep_pass);
}

void test_apply_mqtt_propagates_keep_pass(void) {
    mqtt_form_result_t result;
    memset(&result, 0, sizeof(result));
    snprintf(result.uri, sizeof(result.uri), "mqtt://broker");
    result.keep_pass = true;
    setup_session_ops_t ops = make_ops();

    TEST_ASSERT_TRUE(setup_session_apply_mqtt(&ops, &result));
    TEST_ASSERT_TRUE(m_set_mqtt_keep_pass);
}

void test_apply_mqtt_propagates_a_store_failure(void) {
    mqtt_form_result_t result;
    memset(&result, 0, sizeof(result));
    setup_session_ops_t ops = make_ops();
    m_set_mqtt_ok = false;

    TEST_ASSERT_FALSE(setup_session_apply_mqtt(&ops, &result));
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

static setup_session_page_in_t plain_page(void) {
    setup_session_page_in_t in;
    memset(&in, 0, sizeof(in));
    in.ssid_escaped = "";
    in.uri_escaped = "";
    in.user_escaped = "";
    in.status_msg = NULL;
    in.join_state = SETUP_JOIN_IDLE;
    in.join_reason = SETUP_JOIN_REASON_NONE;
    return in;
}

void test_page_prefills_uri_and_user_escaped(void) {
    reset_page_capture();
    setup_session_page_in_t in = plain_page();
    in.uri_escaped = "mqtt://broker&amp;co";
    in.user_escaped = "bob";
    bool ok = setup_session_render_page(&in, page_sink, NULL);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_NOT_NULL(strstr(m_page_buf, "value=\"mqtt://broker&amp;co\""));
    TEST_ASSERT_NOT_NULL(strstr(m_page_buf, "value=\"bob\""));
}

void test_page_has_the_wifi_and_mqtt_fields_posting_to_the_root(void) {
    reset_page_capture();
    setup_session_page_in_t in = plain_page();
    setup_session_render_page(&in, page_sink, NULL);
    TEST_ASSERT_NOT_NULL(strstr(m_page_buf, "<form method=post action=/>"));
    TEST_ASSERT_NOT_NULL(strstr(m_page_buf, "name=ssid"));
    TEST_ASSERT_NOT_NULL(strstr(m_page_buf, "name=wpass type=password"));
    TEST_ASSERT_NOT_NULL(strstr(m_page_buf, "name=uri"));
    TEST_ASSERT_NOT_NULL(strstr(m_page_buf, "name=user"));
    TEST_ASSERT_NOT_NULL(strstr(m_page_buf, "name=pass type=password"));
}

void test_page_never_prefills_either_password(void) {
    reset_page_capture();
    setup_session_page_in_t in = plain_page();
    in.ssid_escaped = "HomeNet";
    in.uri_escaped = "mqtt://b";
    in.user_escaped = "bob";
    setup_session_render_page(&in, page_sink, NULL);
    /* Neither password was ever given to this function -- the type has no
       field for one -- so the strongest check available is structural: the
       two password inputs carry no value attribute. */
    TEST_ASSERT_NULL(strstr(m_page_buf, "name=pass type=password value"));
    TEST_ASSERT_NULL(strstr(m_page_buf, "name=wpass type=password value"));
    TEST_ASSERT_NULL(strstr(m_page_buf, "name=pass value="));
    TEST_ASSERT_NULL(strstr(m_page_buf, "name=wpass value="));
}

/* A prefilled SSID with a password box left blank would read as "join this
   network, no password" -- an open-network join of the owner's own WPA2
   network. So the current name is a placeholder, and the real value is
   empty. */
void test_page_shows_the_current_ssid_as_a_placeholder_never_a_value(void) {
    reset_page_capture();
    setup_session_page_in_t in = plain_page();
    in.ssid_escaped = "Home&amp;Net";
    setup_session_render_page(&in, page_sink, NULL);
    TEST_ASSERT_NOT_NULL(strstr(m_page_buf, "placeholder=\"Home&amp;Net\""));
    TEST_ASSERT_NULL(strstr(m_page_buf, "name=ssid value"));
    TEST_ASSERT_NULL(strstr(m_page_buf, "name=ssid maxlength=32 value"));
}

void test_page_says_a_network_is_required_when_none_is_stored(void) {
    reset_page_capture();
    setup_session_page_in_t in = plain_page();
    setup_session_render_page(&in, page_sink, NULL);
    TEST_ASSERT_NOT_NULL(strstr(m_page_buf, "required"));

    reset_page_capture();
    in.ssid_escaped = "HomeNet";
    setup_session_render_page(&in, page_sink, NULL);
    TEST_ASSERT_NULL(strstr(m_page_buf, "required"));
}

void test_page_shows_the_status_banner_when_present(void) {
    reset_page_capture();
    setup_session_page_in_t in = plain_page();
    in.status_msg = "uri: bad scheme";
    setup_session_render_page(&in, page_sink, NULL);
    TEST_ASSERT_NOT_NULL(strstr(m_page_buf, "uri: bad scheme"));
}

void test_page_omits_the_banner_and_script_on_a_plain_load(void) {
    reset_page_capture();
    setup_session_page_in_t in = plain_page();
    setup_session_render_page(&in, page_sink, NULL);
    TEST_ASSERT_NULL(strstr(m_page_buf, "<b>"));
    TEST_ASSERT_NULL(strstr(m_page_buf, "<script"));
}

void test_page_while_connecting_says_so_and_polls_status(void) {
    reset_page_capture();
    setup_session_page_in_t in = plain_page();
    in.join_state = SETUP_JOIN_CONNECTING;
    setup_session_render_page(&in, page_sink, NULL);
    /* The no-JS path: the banner itself tells the owner to reload. */
    TEST_ASSERT_NOT_NULL(strstr(m_page_buf, "Connecting"));
    TEST_ASSERT_NOT_NULL(strstr(m_page_buf, "reload"));
    /* The JS path: poll /status, reload once it is no longer connecting. */
    TEST_ASSERT_NOT_NULL(strstr(m_page_buf, "<script>"));
    TEST_ASSERT_NOT_NULL(strstr(m_page_buf, "/status"));
    TEST_ASSERT_NOT_NULL(strstr(m_page_buf, "connecting"));
    TEST_ASSERT_NOT_NULL(strstr(m_page_buf, "location.reload"));
}

void test_page_after_a_failed_join_names_the_reason_and_does_not_poll(void) {
    const setup_join_reason_t reasons[] = {SETUP_JOIN_REASON_WRONG_PASSWORD, SETUP_JOIN_REASON_NOT_FOUND,
                                           SETUP_JOIN_REASON_UNKNOWN};
    const char *texts[] = {"wrong password", "network not found", "unknown"};
    for (size_t i = 0; i < 3; i++) {
        reset_page_capture();
        setup_session_page_in_t in = plain_page();
        in.join_state = SETUP_JOIN_FAILED;
        in.join_reason = reasons[i];
        setup_session_render_page(&in, page_sink, NULL);
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(m_page_buf, "Could not join"), texts[i]);
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(m_page_buf, texts[i]), texts[i]);
        TEST_ASSERT_NULL(strstr(m_page_buf, "<script"));
    }
}

void test_page_after_a_saved_join_says_saved(void) {
    reset_page_capture();
    setup_session_page_in_t in = plain_page();
    in.join_state = SETUP_JOIN_SAVED;
    setup_session_render_page(&in, page_sink, NULL);
    TEST_ASSERT_NOT_NULL(strstr(m_page_buf, "WiFi saved"));
    TEST_ASSERT_NULL(strstr(m_page_buf, "<script"));
}

/* A submit's own error (a bad field) is shown over the join state's text,
   but the poll still runs when a join is underway. */
void test_page_status_msg_wins_the_banner_but_a_running_join_still_polls(void) {
    reset_page_capture();
    setup_session_page_in_t in = plain_page();
    in.join_state = SETUP_JOIN_CONNECTING;
    in.status_msg = "uri: bad scheme";
    setup_session_render_page(&in, page_sink, NULL);
    TEST_ASSERT_NOT_NULL(strstr(m_page_buf, "uri: bad scheme"));
    TEST_ASSERT_NULL(strstr(m_page_buf, "Connecting"));
    TEST_ASSERT_NOT_NULL(strstr(m_page_buf, "<script>"));
}

/* Flash is tight: the page is a fixed cost every build pays. */
void test_page_stays_small(void) {
    reset_page_capture();
    setup_session_page_in_t in = plain_page();
    in.join_state = SETUP_JOIN_CONNECTING;
    setup_session_render_page(&in, page_sink, NULL);
    TEST_ASSERT_LESS_THAN_size_t(1400, m_page_len);
}

void test_page_stops_the_moment_the_sink_refuses(void) {
    reset_page_capture();
    m_sink_fail_after = 1;
    setup_session_page_in_t in = plain_page();
    in.uri_escaped = "x";
    in.user_escaped = "y";
    in.status_msg = "z";
    bool ok = setup_session_render_page(&in, page_sink, NULL);
    TEST_ASSERT_FALSE(ok);
    /* fail_after=1: the first chunk is accepted, the second is refused.
       Exactly two sink calls happen in total -- nothing after the
       refusal was attempted, which is the property under test. */
    TEST_ASSERT_EQUAL_INT(2, m_sink_calls);
}

/* ===== pure helper: the join state's wire forms ============================ */

void test_status_json_for_every_state(void) {
    char out[96];
    TEST_ASSERT_TRUE(setup_session_format_status_json(SETUP_JOIN_IDLE, SETUP_JOIN_REASON_NONE, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("{\"state\":\"idle\",\"reason\":\"\"}", out);
    TEST_ASSERT_TRUE(setup_session_format_status_json(SETUP_JOIN_CONNECTING, SETUP_JOIN_REASON_NONE, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("{\"state\":\"connecting\",\"reason\":\"\"}", out);
    TEST_ASSERT_TRUE(setup_session_format_status_json(SETUP_JOIN_SAVED, SETUP_JOIN_REASON_NONE, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("{\"state\":\"saved\",\"reason\":\"\"}", out);
}

void test_status_json_failed_carries_the_reason(void) {
    char out[96];
    TEST_ASSERT_TRUE(
        setup_session_format_status_json(SETUP_JOIN_FAILED, SETUP_JOIN_REASON_WRONG_PASSWORD, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("{\"state\":\"failed\",\"reason\":\"wrong password\"}", out);
    TEST_ASSERT_TRUE(
        setup_session_format_status_json(SETUP_JOIN_FAILED, SETUP_JOIN_REASON_NOT_FOUND, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("{\"state\":\"failed\",\"reason\":\"network not found\"}", out);
    TEST_ASSERT_TRUE(setup_session_format_status_json(SETUP_JOIN_FAILED, SETUP_JOIN_REASON_UNKNOWN, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("{\"state\":\"failed\",\"reason\":\"unknown\"}", out);
    TEST_ASSERT_TRUE(
        setup_session_format_status_json(SETUP_JOIN_FAILED, SETUP_JOIN_REASON_SAVE_FAILED, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("{\"state\":\"failed\",\"reason\":\"could not save\"}", out);
}

/* A stale reason left from an earlier failure must not leak into a
   state that has none. */
void test_status_json_drops_a_reason_outside_the_failed_state(void) {
    char out[96];
    TEST_ASSERT_TRUE(
        setup_session_format_status_json(SETUP_JOIN_CONNECTING, SETUP_JOIN_REASON_WRONG_PASSWORD, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("{\"state\":\"connecting\",\"reason\":\"\"}", out);
}

void test_status_json_a_too_small_buffer_fails_and_empties(void) {
    char out[16] = "garbage";
    TEST_ASSERT_FALSE(
        setup_session_format_status_json(SETUP_JOIN_FAILED, SETUP_JOIN_REASON_NOT_FOUND, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("", out);
    TEST_ASSERT_FALSE(setup_session_format_status_json(SETUP_JOIN_IDLE, SETUP_JOIN_REASON_NONE, NULL, 8));
    TEST_ASSERT_FALSE(setup_session_format_status_json(SETUP_JOIN_IDLE, SETUP_JOIN_REASON_NONE, out, 0));
}

void test_status_json_an_out_of_range_state_reads_as_idle(void) {
    char out[96];
    TEST_ASSERT_TRUE(
        setup_session_format_status_json((setup_join_state_t)99, SETUP_JOIN_REASON_NONE, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("{\"state\":\"idle\",\"reason\":\"\"}", out);
}

/* ===== pure helper: one setup form submit ================================= */

static mqtt_form_setup_t parsed(const char *body) {
    mqtt_form_setup_t f;
    mqtt_form_status_t st = mqtt_form_parse_setup(body, strlen(body), &f);
    TEST_ASSERT_EQUAL(MQTT_FORM_ERR_NONE, st.err);
    return f;
}

void test_apply_setup_wifi_only_starts_the_join_and_stores_nothing(void) {
    setup_session_ops_t ops = make_ops();
    mqtt_form_setup_t f = parsed("ssid=HomeNet&wpass=hunter22x");
    TEST_ASSERT_EQUAL(SETUP_APPLY_JOINING, setup_session_apply_setup(&ops, &f));
    TEST_ASSERT_EQUAL_INT(1, m_join_calls);
    TEST_ASSERT_EQUAL_STRING("HomeNet", m_join_ssid);
    TEST_ASSERT_EQUAL_STRING("hunter22x", m_join_pass);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, m_set_wifi_calls, "WiFi is stored only after the join verifies");
    TEST_ASSERT_EQUAL_INT(0, m_set_mqtt_calls);
}

void test_apply_setup_wifi_and_mqtt_joins_first_then_stores_the_broker(void) {
    setup_session_ops_t ops = make_ops();
    mqtt_form_setup_t f = parsed("ssid=HomeNet&wpass=hunter22x&uri=mqtt://b.local&user=bob&pass=secret");
    TEST_ASSERT_EQUAL(SETUP_APPLY_JOINING, setup_session_apply_setup(&ops, &f));
    TEST_ASSERT_EQUAL_INT(1, m_join_calls);
    TEST_ASSERT_EQUAL_INT(1, m_set_mqtt_calls);
    TEST_ASSERT_EQUAL_STRING("mqtt://b.local", m_set_mqtt_uri);
    TEST_ASSERT_EQUAL_STRING("secret", m_set_mqtt_pass);
    assert_before(g_log, "join", "set_mqtt");
}

void test_apply_setup_a_refused_join_stores_nothing_at_all(void) {
    setup_session_ops_t ops = make_ops();
    m_join_ok = false;
    mqtt_form_setup_t f = parsed("ssid=HomeNet&wpass=hunter22x&uri=mqtt://b.local");
    TEST_ASSERT_EQUAL(SETUP_APPLY_JOIN_REFUSED, setup_session_apply_setup(&ops, &f));
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, m_set_mqtt_calls, "a join that never started must not half-save the broker");
}

void test_apply_setup_mqtt_only_stores_the_broker_without_a_join(void) {
    setup_session_ops_t ops = make_ops();
    mqtt_form_setup_t f = parsed("ssid=&wpass=&uri=mqtt://b.local&user=&pass=");
    TEST_ASSERT_EQUAL(SETUP_APPLY_MQTT_SAVED, setup_session_apply_setup(&ops, &f));
    TEST_ASSERT_EQUAL_INT(0, m_join_calls);
    TEST_ASSERT_EQUAL_INT(1, m_set_mqtt_calls);
    TEST_ASSERT_TRUE(m_set_mqtt_keep_pass);
}

void test_apply_setup_an_mqtt_store_failure_is_reported(void) {
    setup_session_ops_t ops = make_ops();
    m_set_mqtt_ok = false;
    mqtt_form_setup_t f = parsed("uri=mqtt://b.local");
    TEST_ASSERT_EQUAL(SETUP_APPLY_MQTT_FAILED, setup_session_apply_setup(&ops, &f));

    /* With a join already underway the failure is still reported, and the join is not undone. */
    f = parsed("ssid=Net&wpass=&uri=mqtt://b.local");
    m_join_calls = 0;
    TEST_ASSERT_EQUAL(SETUP_APPLY_MQTT_FAILED, setup_session_apply_setup(&ops, &f));
    TEST_ASSERT_EQUAL_INT(1, m_join_calls);
}

void test_apply_setup_an_empty_submission_is_nothing_to_save(void) {
    setup_session_ops_t ops = make_ops();
    mqtt_form_setup_t f = parsed("ssid=&wpass=&uri=&user=&pass=");
    TEST_ASSERT_EQUAL(SETUP_APPLY_NOTHING, setup_session_apply_setup(&ops, &f));
    TEST_ASSERT_EQUAL_INT(0, m_join_calls);
    TEST_ASSERT_EQUAL_INT(0, m_set_mqtt_calls);
}

void test_apply_setup_messages_are_distinct_and_only_saved_says_saved(void) {
    setup_session_apply_t all[] = {SETUP_APPLY_NOTHING, SETUP_APPLY_JOIN_REFUSED, SETUP_APPLY_MQTT_FAILED,
                                   SETUP_APPLY_JOINING, SETUP_APPLY_MQTT_SAVED};
    for (size_t i = 0; i < 5; i++) {
        TEST_ASSERT_TRUE(strlen(setup_session_apply_msg(all[i])) > 0);
        for (size_t j = i + 1; j < 5; j++)
            TEST_ASSERT_NOT_EQUAL(0, strcmp(setup_session_apply_msg(all[i]), setup_session_apply_msg(all[j])));
    }
    TEST_ASSERT_EQUAL_STRING("Saved.", setup_session_apply_msg(SETUP_APPLY_MQTT_SAVED));
    TEST_ASSERT_NULL(strstr(setup_session_apply_msg(SETUP_APPLY_MQTT_FAILED), "Saved."));
}

void test_format_status_names_the_wifi_fields(void) {
    char out[64];
    mqtt_form_status_t st = {MQTT_FORM_ERR_TOO_LONG, MQTT_FORM_FIELD_WIFI_SSID};
    setup_session_format_mqtt_status(false, st, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("network name: value is too long", out);
    st = (mqtt_form_status_t){MQTT_FORM_ERR_TOO_SHORT, MQTT_FORM_FIELD_WIFI_PASS};
    setup_session_format_mqtt_status(false, st, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("wifi password: value is too short", out);
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
    TEST_ASSERT_EQUAL_INT(1, m_render_end_calls);
    TEST_ASSERT_EQUAL(SETUP_SESSION_END_WIFI_SAVED, m_render_end_kind);
    /* start() and render_setup_screen() must agree on the AP credentials. */
    TEST_ASSERT_EQUAL_STRING(m_start_ssid, m_render_setup_info.ap_ssid);
    TEST_ASSERT_EQUAL_STRING(m_start_pass, m_render_setup_info.ap_password);
    TEST_ASSERT_TRUE(strncmp(m_render_setup_info.ap_ssid, "MagTag-", 7) == 0);
    /* The QR carries exactly the credentials the SoftAP was started with, and
       the screen is told the address to open. */
    char expect_qr[SETUP_SESSION_QR_MAX];
    snprintf(expect_qr, sizeof(expect_qr), "WIFI:T:WPA;S:%s;P:%s;;", m_start_ssid, m_start_pass);
    TEST_ASSERT_EQUAL_STRING(expect_qr, m_render_setup_info.qr_payload);
    TEST_ASSERT_EQUAL_STRING("192.168.4.1", m_render_setup_info.page_host);
    assert_teardown_ran();

    /* Ordering, not just "did it happen". render_setup runs before start
       — deliberately, so the setup screen is already painted before the
       SoftAP's radio comes up (setup_session_run's own flow comment says
       why). */
    assert_before(g_log, "extend", "render_setup");
    assert_before(g_log, "render_setup", "start");
    assert_before(g_log, "set_wifi", "clear_driver");
    assert_before(g_log, "clear_driver", "stop");
    assert_before(g_log, "stop", "render_end");
}

/* WIFI_SUCCESS does not return immediately -- it lingers so a
   polling phone app's own "connected now?" query still gets an answer
   (see setup_session.h's SETUP_SESSION_SUCCESS_LINGER_MS comment). */
void test_wifi_success_lingers_before_returning_ok(void) {
    script_wifi_success("HomeNet", "homepass1");
    setup_session_ops_t ops = make_ops();
    setup_session_cfg_t cfg = {.has_wifi_ssid = false, .budget_sec = 600};

    setup_session_result_t r = setup_session_run(&ops, &cfg);

    TEST_ASSERT_EQUAL(SETUP_SESSION_OUTCOME_WIFI_OK, r.outcome);
    /* One poll resolves WIFI_SUCCESS; the rest is the linger, which
       polls in SETUP_SESSION_POLL_QUANTUM_MS steps until
       SETUP_SESSION_SUCCESS_LINGER_MS has elapsed. A 600 s budget does
       not cap it short, so the count is exact. */
    int expected_linger_polls = (int)(SETUP_SESSION_SUCCESS_LINGER_MS / SETUP_SESSION_POLL_QUANTUM_MS);
    TEST_ASSERT_EQUAL_INT(1 + expected_linger_polls, m_poll_calls);
    /* stop() -- and hence teardown -- must not have run until the
       linger finished, which assert_teardown_ran (checked after the
       call returns) already pins at exactly once; what this adds is
       that it was still exactly once across all those extra polls. */
    assert_teardown_ran();
}

/* A session with little budget left when WIFI_SUCCESS arrives lingers
   only until the overall deadline, not the full linger period -- the
   linger ends early when the budget runs out. */
void test_wifi_success_linger_is_capped_by_the_overall_budget(void) {
    script_wifi_success("HomeNet", "homepass1");
    setup_session_ops_t ops = make_ops();
    setup_session_cfg_t cfg = {.has_wifi_ssid = false, .budget_sec = 1}; /* far shorter than the 15 s linger */

    setup_session_result_t r = setup_session_run(&ops, &cfg);

    TEST_ASSERT_EQUAL(SETUP_SESSION_OUTCOME_WIFI_OK, r.outcome); /* NOT timeout -- WiFi already succeeded */
    TEST_ASSERT_EQUAL(SETUP_SESSION_SLEEP_NET_WINDOW, r.sleep);
    /* deadline is 1000 ms; the first poll (WIFI_SUCCESS) consumes 250 of
       it, leaving exactly 3 more 250 ms steps for the linger to reach
       the overall deadline instead of the full 15 s. */
    TEST_ASSERT_EQUAL_INT(4, m_poll_calls);
    assert_teardown_ran();
}

/* During the linger, an MQTT submit is still accepted and stored. */
void test_mqtt_stored_during_the_linger_does_not_end_it_early(void) {
    script_wifi_success("HomeNet", "homepass1");
    script_mqtt_stored();
    setup_session_ops_t ops = make_ops();
    setup_session_cfg_t cfg = {.has_wifi_ssid = false, .budget_sec = 600};

    setup_session_result_t r = setup_session_run(&ops, &cfg);

    TEST_ASSERT_EQUAL(SETUP_SESSION_OUTCOME_WIFI_OK, r.outcome);
    /* Same total as the plain linger test: MQTT_STORED just occupies
       one of the linger's poll slots instead of a NONE; it is not
       treated specially. */
    int expected_linger_polls = (int)(SETUP_SESSION_SUCCESS_LINGER_MS / SETUP_SESSION_POLL_QUANTUM_MS);
    TEST_ASSERT_EQUAL_INT(1 + expected_linger_polls, m_poll_calls);
    assert_teardown_ran();
}

/* A HARD_ERROR during the linger ends it early, but the credentials are
   already safely stored, so the outcome must stay WIFI_OK, not degrade
   to ERROR. */
void test_hard_error_during_the_linger_ends_it_early_but_outcome_stays_ok(void) {
    script_wifi_success("HomeNet", "homepass1");
    script_hard_error();
    setup_session_ops_t ops = make_ops();
    setup_session_cfg_t cfg = {.has_wifi_ssid = false, .budget_sec = 600};

    setup_session_result_t r = setup_session_run(&ops, &cfg);

    TEST_ASSERT_EQUAL(SETUP_SESSION_OUTCOME_WIFI_OK, r.outcome);
    TEST_ASSERT_EQUAL_INT(2, m_poll_calls); /* WIFI_SUCCESS, then the HARD_ERROR that cuts the linger short */
    TEST_ASSERT_EQUAL_INT(1, m_render_end_calls);
    TEST_ASSERT_EQUAL(SETUP_SESSION_END_WIFI_SAVED, m_render_end_kind);
    assert_teardown_ran();
}

/* A WIFI_SUCCESS whose credential store fails must not claim
   success, and must not clear the only verified copy that is left
   (the driver's own store). */
void test_wifi_store_failure_is_an_error_with_the_failed_screen(void) {
    script_wifi_success("HomeNet", "homepass1");
    m_set_wifi_ok = false;
    setup_session_ops_t ops = make_ops();
    setup_session_cfg_t cfg = {.has_wifi_ssid = false, .budget_sec = 600};

    setup_session_result_t r = setup_session_run(&ops, &cfg);

    TEST_ASSERT_EQUAL(SETUP_SESSION_OUTCOME_ERROR, r.outcome);
    TEST_ASSERT_EQUAL(SETUP_SESSION_SLEEP_BUTTON_ONLY, r.sleep);
    TEST_ASSERT_EQUAL_INT(1, m_set_wifi_calls);
    TEST_ASSERT_EQUAL_INT(0, m_clear_driver_calls);
    /* By the time this is decided the panel is already showing the
       (about to be torn down) AP's name, password and QR, so this ERROR
       — like every ERROR that happens after the setup screen painted —
       renders FAILED. */
    TEST_ASSERT_EQUAL_INT(1, m_render_end_calls);
    TEST_ASSERT_EQUAL(SETUP_SESSION_END_FAILED, m_render_end_kind);
    TEST_ASSERT_FALSE(m_render_end_has_ssid);
    TEST_ASSERT_EQUAL_INT(1, m_poll_calls); /* no linger on this path */
    assert_teardown_ran();

    assert_before(g_log, "set_wifi", "stop");
    assert_before(g_log, "stop", "render_end");
}

void test_wifi_store_failure_with_an_ssid_present_sleeps_the_normal_schedule(void) {
    script_wifi_success("HomeNet", "homepass1");
    m_set_wifi_ok = false;
    setup_session_ops_t ops = make_ops();
    setup_session_cfg_t cfg = {.has_wifi_ssid = true, .budget_sec = 600};

    setup_session_result_t r = setup_session_run(&ops, &cfg);

    TEST_ASSERT_EQUAL(SETUP_SESSION_OUTCOME_ERROR, r.outcome);
    TEST_ASSERT_EQUAL(SETUP_SESSION_SLEEP_NORMAL, r.sleep);
    TEST_ASSERT_EQUAL_INT(1, m_render_end_calls);
    TEST_ASSERT_EQUAL(SETUP_SESSION_END_FAILED, m_render_end_kind);
    TEST_ASSERT_TRUE(m_render_end_has_ssid);
    assert_teardown_ran();
}

/* MQTT storage itself happens outside this pure layer now (the httpd
   handler calls setup_session_apply_mqtt before this event is even
   posted — see the tests above), so an MQTT_STORED event arriving
   before WiFi is provisioned is purely a "does this end the session"
   question: it must not, because the device still has no SSID. */
void test_mqtt_stored_before_wifi_still_waits_for_wifi(void) {
    script_mqtt_stored();
    script_wifi_success("HomeNet", "homepass1");
    setup_session_ops_t ops = make_ops();
    setup_session_cfg_t cfg = {.has_wifi_ssid = false, .budget_sec = 600};

    setup_session_result_t r = setup_session_run(&ops, &cfg);

    TEST_ASSERT_EQUAL(SETUP_SESSION_OUTCOME_WIFI_OK, r.outcome);
    TEST_ASSERT_EQUAL(SETUP_SESSION_SLEEP_NET_WINDOW, r.sleep);
    TEST_ASSERT_EQUAL_INT(1, m_set_wifi_calls);
    /* Not this layer's job any more -- set_mqtt_creds is only
       ever reached through setup_session_apply_mqtt, which the httpd
       handler calls before this layer hears about it at all. */
    TEST_ASSERT_EQUAL_INT(0, m_set_mqtt_calls);
    assert_teardown_ran();
}

void test_mqtt_only_with_an_ssid_already_present(void) {
    script_mqtt_stored();
    setup_session_ops_t ops = make_ops();
    setup_session_cfg_t cfg = {.has_wifi_ssid = true, .budget_sec = 600};

    setup_session_result_t r = setup_session_run(&ops, &cfg);

    TEST_ASSERT_EQUAL(SETUP_SESSION_OUTCOME_MQTT_ONLY, r.outcome);
    TEST_ASSERT_EQUAL(SETUP_SESSION_SLEEP_NET_WINDOW, r.sleep);
    TEST_ASSERT_EQUAL_INT(0, m_set_wifi_calls);
    TEST_ASSERT_EQUAL_INT(0, m_clear_driver_calls);
    TEST_ASSERT_EQUAL_INT(0, m_set_mqtt_calls); /* already stored before this event was posted */
    TEST_ASSERT_EQUAL_INT(1, m_render_end_calls);
    TEST_ASSERT_EQUAL(SETUP_SESSION_END_MQTT_SAVED, m_render_end_kind);
    assert_teardown_ran();
}

void test_mqtt_stored_on_a_no_ssid_device_then_times_out(void) {
    script_mqtt_stored();
    setup_session_ops_t ops = make_ops();
    setup_session_cfg_t cfg = {.has_wifi_ssid = false, .budget_sec = 1}; /* 4 poll quanta at 250 ms */

    setup_session_result_t r = setup_session_run(&ops, &cfg);

    TEST_ASSERT_EQUAL(SETUP_SESSION_OUTCOME_TIMEOUT, r.outcome);
    TEST_ASSERT_EQUAL(SETUP_SESSION_SLEEP_BUTTON_ONLY, r.sleep);
    TEST_ASSERT_EQUAL_INT(0, m_set_wifi_calls);
    TEST_ASSERT_EQUAL_INT(1, m_render_end_calls);
    TEST_ASSERT_EQUAL(SETUP_SESSION_END_TIMED_OUT, m_render_end_kind);
    TEST_ASSERT_FALSE(m_render_end_has_ssid);
    TEST_ASSERT_EQUAL_INT((cfg.budget_sec * 1000) / (int)SETUP_SESSION_POLL_QUANTUM_MS, m_poll_calls);
    assert_teardown_ran();
}

/* The device layer resets the manager's state machine on CRED_FAIL so a
   retry can succeed (not host-tested, that call is in
   setup_session_idf.c) — what THIS layer must guarantee is that it
   never stops the session just because one attempt failed. */
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
    TEST_ASSERT_EQUAL_INT(1, m_render_end_calls);
    TEST_ASSERT_EQUAL(SETUP_SESSION_END_TIMED_OUT, m_render_end_kind);
    TEST_ASSERT_FALSE(m_render_end_has_ssid);
    assert_teardown_ran();
}

void test_timeout_with_an_ssid_present_sleeps_the_normal_schedule(void) {
    setup_session_ops_t ops = make_ops();
    setup_session_cfg_t cfg = {.has_wifi_ssid = true, .budget_sec = 1};

    setup_session_result_t r = setup_session_run(&ops, &cfg);

    TEST_ASSERT_EQUAL(SETUP_SESSION_OUTCOME_TIMEOUT, r.outcome);
    TEST_ASSERT_EQUAL(SETUP_SESSION_SLEEP_NORMAL, r.sleep);
    TEST_ASSERT_EQUAL(SETUP_SESSION_END_TIMED_OUT, m_render_end_kind);
    TEST_ASSERT_TRUE(m_render_end_has_ssid);
    assert_teardown_ran();
}

/* render_setup_screen runs BEFORE start() (setup_session_run's own flow
   comment says why), so a start() failure happens with the setup screen
   already on the glass — unlike the extend_awake refusal below, this one
   still renders FAILED, not nothing. */
void test_start_failure_is_a_hard_error_after_the_setup_screen_already_painted(void) {
    m_start_ok = false;
    setup_session_ops_t ops = make_ops();
    setup_session_cfg_t cfg = {.has_wifi_ssid = false, .budget_sec = 600};

    setup_session_result_t r = setup_session_run(&ops, &cfg);

    TEST_ASSERT_EQUAL(SETUP_SESSION_OUTCOME_ERROR, r.outcome);
    TEST_ASSERT_EQUAL(SETUP_SESSION_SLEEP_BUTTON_ONLY, r.sleep);
    TEST_ASSERT_EQUAL_INT(1, m_render_setup_calls);
    TEST_ASSERT_EQUAL_INT(1, m_render_end_calls);
    TEST_ASSERT_EQUAL(SETUP_SESSION_END_FAILED, m_render_end_kind);
    TEST_ASSERT_FALSE(m_render_end_has_ssid);
    TEST_ASSERT_EQUAL_INT(0, m_poll_calls); /* the loop never starts */
    assert_teardown_ran();

    assert_before(g_log, "render_setup", "start");
}

void test_start_failure_with_an_ssid_present_sleeps_the_normal_schedule(void) {
    m_start_ok = false;
    setup_session_ops_t ops = make_ops();
    setup_session_cfg_t cfg = {.has_wifi_ssid = true, .budget_sec = 600};

    setup_session_result_t r = setup_session_run(&ops, &cfg);

    TEST_ASSERT_EQUAL(SETUP_SESSION_OUTCOME_ERROR, r.outcome);
    TEST_ASSERT_EQUAL(SETUP_SESSION_SLEEP_NORMAL, r.sleep);
    TEST_ASSERT_EQUAL(SETUP_SESSION_END_FAILED, m_render_end_kind);
    TEST_ASSERT_TRUE(m_render_end_has_ssid);
    assert_teardown_ran();
}

/* The earliest possible exit: nothing after step 1 ever ran, and teardown
   still has to be the thing that runs. This is the path that makes "always"
   true rather than "true for every path this suite happened to try" — and
   the ONLY path where nothing ever painted, so it is also the only ERROR
   that renders no screen at all. */
void test_extend_awake_failure_is_a_hard_error_and_still_tears_down(void) {
    m_extend_ok = false;
    setup_session_ops_t ops = make_ops();
    setup_session_cfg_t cfg = {.has_wifi_ssid = false, .budget_sec = 600};

    setup_session_result_t r = setup_session_run(&ops, &cfg);

    TEST_ASSERT_EQUAL(SETUP_SESSION_OUTCOME_ERROR, r.outcome);
    TEST_ASSERT_EQUAL_INT(1, m_extend_calls);
    /* The tail is added on top of the caller's own budget -- this
       is the one call that arms the failsafe for the whole session, so
       it has to cover everything that runs after the loop too. */
    TEST_ASSERT_EQUAL_INT(600 + SETUP_SESSION_TAIL_SEC, m_extend_arg);
    TEST_ASSERT_EQUAL_INT(0, m_render_setup_calls);
    TEST_ASSERT_EQUAL_INT(0, m_render_end_calls);
    TEST_ASSERT_EQUAL_INT(0, m_start_calls);
    TEST_ASSERT_EQUAL_INT(0, m_poll_calls);
    assert_teardown_ran();
}

/* The defect this directly guards: a hard error mid-session used to leave
   the panel showing a dead QR and password with no corrective screen.
   screen_painted is true by this point (render_setup_screen already ran),
   so this ERROR renders FAILED like every other one that happens after
   the setup screen painted. */
void test_a_hard_error_mid_session_ends_the_session_with_the_failed_screen(void) {
    script_hard_error();
    setup_session_ops_t ops = make_ops();
    setup_session_cfg_t cfg = {.has_wifi_ssid = false, .budget_sec = 600};

    setup_session_result_t r = setup_session_run(&ops, &cfg);

    TEST_ASSERT_EQUAL(SETUP_SESSION_OUTCOME_ERROR, r.outcome);
    TEST_ASSERT_EQUAL_INT(1, m_render_end_calls);
    TEST_ASSERT_EQUAL(SETUP_SESSION_END_FAILED, m_render_end_kind);
    assert_teardown_ran();
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_ap_password_default_fake_is_full_length_and_in_alphabet);
    RUN_TEST(test_ap_password_length_is_wpa2_valid);
    RUN_TEST(test_ap_password_excludes_ambiguous_characters);
    RUN_TEST(test_ap_password_rejects_out_of_range_bytes_without_modulo_bias);
    RUN_TEST(test_ap_password_rejection_boundary_245_vs_244);

    RUN_TEST(test_ap_ssid_strips_the_devices_own_magtag_prefix);
    RUN_TEST(test_ap_ssid_without_the_prefix_falls_back_to_the_whole_id);
    RUN_TEST(test_ap_ssid_fits_32_bytes_even_for_a_long_id);

    RUN_TEST(test_qr_payload_is_the_standard_wifi_join_string);
    RUN_TEST(test_qr_payload_escapes_every_special_character_with_a_backslash);
    RUN_TEST(test_qr_payload_leaves_other_characters_alone);
    RUN_TEST(test_qr_payload_too_small_buffer_fails_and_empties);
    RUN_TEST(test_qr_payload_capacity_boundary_is_exact);
    RUN_TEST(test_qr_payload_never_truncates_inside_an_escape);
    RUN_TEST(test_qr_payload_rejects_null_and_zero_capacity);
    RUN_TEST(test_the_real_payload_encodes_at_the_version_the_panel_is_sized_for);
    RUN_TEST(test_a_31_byte_ssid_fits_the_buffer_even_if_it_will_not_encode);

    RUN_TEST(test_format_mqtt_status_ok_says_saved);
    RUN_TEST(test_format_mqtt_status_names_the_field_when_there_is_one);
    RUN_TEST(test_format_mqtt_status_omits_the_field_label_when_there_is_none);
    RUN_TEST(test_format_status_names_the_wifi_fields);

    RUN_TEST(test_apply_mqtt_forwards_the_result_to_set_mqtt_creds);
    RUN_TEST(test_apply_mqtt_propagates_keep_pass);
    RUN_TEST(test_apply_mqtt_propagates_a_store_failure);

    RUN_TEST(test_apply_setup_wifi_only_starts_the_join_and_stores_nothing);
    RUN_TEST(test_apply_setup_wifi_and_mqtt_joins_first_then_stores_the_broker);
    RUN_TEST(test_apply_setup_a_refused_join_stores_nothing_at_all);
    RUN_TEST(test_apply_setup_mqtt_only_stores_the_broker_without_a_join);
    RUN_TEST(test_apply_setup_an_mqtt_store_failure_is_reported);
    RUN_TEST(test_apply_setup_an_empty_submission_is_nothing_to_save);
    RUN_TEST(test_apply_setup_messages_are_distinct_and_only_saved_says_saved);

    RUN_TEST(test_status_json_for_every_state);
    RUN_TEST(test_status_json_failed_carries_the_reason);
    RUN_TEST(test_status_json_drops_a_reason_outside_the_failed_state);
    RUN_TEST(test_status_json_a_too_small_buffer_fails_and_empties);
    RUN_TEST(test_status_json_an_out_of_range_state_reads_as_idle);

    RUN_TEST(test_page_prefills_uri_and_user_escaped);
    RUN_TEST(test_page_has_the_wifi_and_mqtt_fields_posting_to_the_root);
    RUN_TEST(test_page_never_prefills_either_password);
    RUN_TEST(test_page_shows_the_current_ssid_as_a_placeholder_never_a_value);
    RUN_TEST(test_page_says_a_network_is_required_when_none_is_stored);
    RUN_TEST(test_page_shows_the_status_banner_when_present);
    RUN_TEST(test_page_omits_the_banner_and_script_on_a_plain_load);
    RUN_TEST(test_page_while_connecting_says_so_and_polls_status);
    RUN_TEST(test_page_after_a_failed_join_names_the_reason_and_does_not_poll);
    RUN_TEST(test_page_after_a_saved_join_says_saved);
    RUN_TEST(test_page_status_msg_wins_the_banner_but_a_running_join_still_polls);
    RUN_TEST(test_page_stays_small);
    RUN_TEST(test_page_stops_the_moment_the_sink_refuses);

    RUN_TEST(test_success_with_wifi_only);
    RUN_TEST(test_wifi_success_lingers_before_returning_ok);
    RUN_TEST(test_wifi_success_linger_is_capped_by_the_overall_budget);
    RUN_TEST(test_mqtt_stored_during_the_linger_does_not_end_it_early);
    RUN_TEST(test_hard_error_during_the_linger_ends_it_early_but_outcome_stays_ok);
    RUN_TEST(test_wifi_store_failure_is_an_error_with_the_failed_screen);
    RUN_TEST(test_wifi_store_failure_with_an_ssid_present_sleeps_the_normal_schedule);
    RUN_TEST(test_mqtt_stored_before_wifi_still_waits_for_wifi);
    RUN_TEST(test_mqtt_only_with_an_ssid_already_present);
    RUN_TEST(test_mqtt_stored_on_a_no_ssid_device_then_times_out);
    RUN_TEST(test_a_wrong_wifi_password_is_not_stored_and_the_session_continues);
    RUN_TEST(test_timeout_with_no_ssid_sleeps_buttons_only);
    RUN_TEST(test_timeout_with_an_ssid_present_sleeps_the_normal_schedule);
    RUN_TEST(test_start_failure_is_a_hard_error_after_the_setup_screen_already_painted);
    RUN_TEST(test_start_failure_with_an_ssid_present_sleeps_the_normal_schedule);
    RUN_TEST(test_extend_awake_failure_is_a_hard_error_and_still_tears_down);
    RUN_TEST(test_a_hard_error_mid_session_ends_the_session_with_the_failed_screen);
    return UNITY_END();
}
