#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mqtt_form.h" /* mqtt_form_status_t, MQTT_FORM_*_MAX — reused, not restated */

#ifndef NATIVE
#include "sdkconfig.h"
#endif

/* WiFi + MQTT provisioning plan (docs/planning/20261003.wifi-provisioning.plan.md),
   task 4: the setup session itself. Drives the whole SoftAP-provisioning +
   MQTT-form episode through one call over an injected ops table, the same
   seam ota_flow.c and wake_flow.c use for their device effects — no
   ESP-IDF calls live in this file or in setup_session.c; main/setup_session_idf.c
   supplies the real ops and is not host-tested.

   One call for the whole session (start..teardown), not an init-then-poll
   module like ota_flow.c: a setup session has no second phase to resume
   across — it runs once, to completion, inside a single wake that does
   nothing else (plan, "The setup session" item 4). So there is no internal
   state to leak between host tests either. */

#ifdef __cplusplus
extern "C" {
#endif

/* ---- the Kconfig-fallback pattern (setup_trigger.h's MAGTAG_BOOT_HOLD_MS) --- */

#ifdef CONFIG_MAGTAG_SETUP_MAX_SEC
#define SETUP_SESSION_MAX_SEC_DEFAULT CONFIG_MAGTAG_SETUP_MAX_SEC
#else
#define SETUP_SESSION_MAX_SEC_DEFAULT 600
#endif

/* ---- budget bookkeeping granularity -------------------------------------- */

/* How much budget one `poll` call is charged, whether it returns early on
   an event or runs the full quantum to NONE. Coarse on purpose — see
   setup_session.c's run loop for why this needs no clock op at all. Public
   so a caller (or a test) can compute how many NONE polls make a budget
   expire. */
#define SETUP_SESSION_POLL_QUANTUM_MS 250u

/* ---- outcomes and the sleep the caller owes (D4) ------------------------- */

typedef enum {
    SETUP_SESSION_OUTCOME_WIFI_OK = 0, /* WiFi provisioned, verified STA join; MQTT too if submitted this session */
    SETUP_SESSION_OUTCOME_MQTT_ONLY,   /* MQTT submitted on a device that already had an SSID; session ends */
    SETUP_SESSION_OUTCOME_TIMEOUT,     /* budget expired */
    SETUP_SESSION_OUTCOME_ERROR,       /* a start failure, or a hard error reported mid-session */
} setup_session_outcome_t;

/* What the caller (task 6's wake_flow) must sleep into. THE SESSION NEVER
   DEEP-SLEEPS OR RESTARTS ITSELF (C6) — it only reports which of these the
   caller owes:
     - NET_WINDOW : a 1 s timer wake into a normal network window (D4) —
       the first real join, NTP sync and HA discovery happen there, not here.
     - BUTTON_ONLY: no SSID even after this session — sleep buttons-only, so
       an unprovisioned device in a drawer does not cycle the radio.
     - NORMAL     : an SSID was already present before this session (a
       WiFi-only device re-entering setup to change MQTT, that then timed
       out or errored) — fall back to the ordinary schedule, unchanged. */
typedef enum {
    SETUP_SESSION_SLEEP_NET_WINDOW = 0,
    SETUP_SESSION_SLEEP_BUTTON_ONLY,
    SETUP_SESSION_SLEEP_NORMAL,
} setup_session_sleep_t;

typedef struct {
    setup_session_outcome_t outcome;
    setup_session_sleep_t sleep;
} setup_session_result_t;

/* ---- what the session is told, once, at the top -------------------------- */

typedef struct {
    bool has_wifi_ssid; /* nvs_config_get_wifi_ssid() returned non-empty, read BEFORE this call */
    int budget_sec;     /* MAGTAG_SETUP_MAX_SEC (SETUP_SESSION_MAX_SEC_DEFAULT), already bounded by Kconfig's range */
} setup_session_cfg_t;

/* ---- the AP password: pure, from injected random bytes ------------------ */

/* 8-12 chars, WPA2-valid (minimum 8); fixed at 10 so every session's panel
   layout and every host-test expectation is the same length. Alphabet
   excludes 0/O/1/l/I (unambiguous on an e-paper panel, per the plan) — 57
   characters, so a byte is mapped by REJECTION SAMPLING rather than `% 57`:
   a plain modulo would favour the alphabet's first 256%57=28 characters
   over the rest. See setup_session.c for the exact threshold. */
#define SETUP_SESSION_AP_PASS_LEN 10
#define SETUP_SESSION_AP_PASS_BUF (SETUP_SESSION_AP_PASS_LEN + 1)

typedef uint8_t (*setup_session_rand_byte_fn)(void);

/* Fills out[0..SETUP_SESSION_AP_PASS_LEN) from the alphabet and NUL-
   terminates. Calls `rand_byte` at least SETUP_SESSION_AP_PASS_LEN times
   and more if a byte is rejected — an adversarial or buggy source that
   never returns an in-range byte spins forever, the same tradeoff every
   rejection-sampling loop carries; esp_random() is uniform over all 256
   values so the real device rejects roughly 11% of bytes and always
   terminates. */
void setup_session_make_ap_password(setup_session_rand_byte_fn rand_byte, char out[SETUP_SESSION_AP_PASS_BUF]);

/* ---- the AP SSID: pure, from device_id() --------------------------------- */

/* WiFi SSIDs cap at 32 bytes including the NUL this buffer reserves.
   "MagTag-<suffix>": device_id() (include/device_id.h) already returns
   "magtag-a1b2c3"-shaped ids, so this strips THAT prefix before adding its
   own rather than emitting "MagTag-magtag-a1b2c3". Calls device_id()
   directly — not threaded through setup_session_cfg_t — the same way
   ota_flow.c calls it directly and test_ota_flow.c stubs it; this file's
   host test does the same. */
#define SETUP_SESSION_AP_SSID_MAX 32

void setup_session_make_ap_ssid(char out[SETUP_SESSION_AP_SSID_MAX]);

/* ---- the provisioning QR payload: pure, from the two strings above ------- */

/* Espressif's ESP SoftAP Prov app JSON, security 2 over SoftAP transport:
     {"ver":"v1","name":"<ap_ssid>","username":"<user>","pop":"<ap_password>","transport":"softap"}
   Confirmed against espressif/network_provisioning 1.3.1's own example
   (examples/wifi_prov/main/app_main.c:264-277, the sec2 branch: ver/name/
   username/pop/transport in that order) and its README's logged QR text
   (README.md:145). "pop" doubles as the SRP password — the example's own
   comment says so: "this pop field represents the password that will be
   used to generate salt and verifier... present here in order to generate
   the QR code containing password." There is no key for the SoftAP's own
   WPA2 join password in this format: the phone app expects the user to
   join that network through the OS WiFi picker, so task 5's setup screen
   must show ap_password as plain text beside the QR, not fold it into this
   string.

   Username is fixed (every session uses the same one; only the SRP
   password — the AP password — varies per session, which is how the
   server side tells sessions apart via SRP6a in the first place).

   Writes the JSON into `out` (capacity out_cap) and returns true, or
   writes "" and returns false if it would not fit (out_cap >= 1
   required to see even that). ap_ssid/ap_password are never escaped: both
   come from this module's own generators, whose alphabets contain no `"`
   or `\`. */
#define SETUP_SESSION_QR_USERNAME "magtag"
#define SETUP_SESSION_QR_TRANSPORT "softap"
/* {"ver":"v1","name":"","username":"magtag","pop":"","transport":"softap"}
   plus the longest ap_ssid (31 chars, SETUP_SESSION_AP_SSID_MAX-1) and the
   fixed AP password length, plus slack. */
#define SETUP_SESSION_QR_MAX 128

bool setup_session_make_qr_payload(const char *ap_ssid, const char *ap_password, char *out, size_t out_cap);

/* ---- the setup screen's info (task 5's render input) --------------------- */

typedef struct {
    char ap_ssid[SETUP_SESSION_AP_SSID_MAX];
    char ap_password[SETUP_SESSION_AP_PASS_BUF];
    char qr_payload[SETUP_SESSION_QR_MAX];
    const char *form_url; /* fixed literal; task 5 renders it verbatim */
} setup_session_screen_info_t;

/* ---- the /mqtt form page: pure, chunked (no stack buffer) ---------------- */

/* Sink for one piece of the page. `chunk` is NOT NUL-terminated-by-
   contract beyond what `len` says (every chunk this module hands it is in
   fact a C string, but the sink must honour `len`, not strlen it again).
   Returns false to abort the render early — the real sink
   (httpd_resp_send_chunk) can fail if the client has gone away, and this
   module must stop walking its own literals the moment that happens
   rather than build the rest of a page nobody will receive. */
typedef bool (*setup_session_chunk_sink_fn)(const char *chunk, size_t len, void *ctx);

typedef struct {
    const char
        *uri_escaped; /* mqtt_form_html_escape() output (or "" ), sized MQTT_FORM_HTML_ESCAPED_MAX(MQTT_FORM_URI_MAX) */
    const char *user_escaped; /* same, MQTT_FORM_HTML_ESCAPED_MAX(MQTT_FORM_USER_MAX) */
    /* NULL = no status banner (a plain GET). Non-NULL is shown verbatim,
       with NO further escaping: the only strings this project ever passes
       here are the fixed literal "Saved." and setup_session_format_mqtt_status's
       output, both built from mqtt_form_error_str()'s fixed literals —
       never a submitted value. A caller that ever changes that must escape
       first. */
    const char *status_msg;
} setup_session_mqtt_page_in_t;

/* Never prefills the password field (plan's security note: "credentials
   are never echoed back"), carries no JS and no external resources (plan's
   "keep the page tiny"). Returns false the moment `sink` does (see the
   sink's own doc comment); true once the whole page has been handed over. */
bool setup_session_render_mqtt_page(const setup_session_mqtt_page_in_t *in, setup_session_chunk_sink_fn sink,
                                    void *ctx);

/* A short, human status line from a form submit's result — "Saved." on
   success, or "<field>: <reason>" / "<reason>" (field NONE) built from
   mqtt_form_error_str()'s own fixed vocabulary on failure. Pure and safe
   to show unescaped in the page above or as the protocomm endpoint's
   plain-text response body: it never contains a submitted value. Writes
   at most out_cap-1 bytes plus a NUL; a status this short never truncates
   against any sane out_cap, but a 0 out_cap or NULL out is a no-op. */
void setup_session_format_mqtt_status(bool ok, mqtt_form_status_t status, char *out, size_t out_cap);

/* ---- the session's own events, fed by the device layer's poll ----------- */

typedef enum {
    SETUP_SESSION_EVENT_NONE = 0, /* the quantum elapsed; nothing happened */
    SETUP_SESSION_EVENT_WIFI_SUCCESS,
    SETUP_SESSION_EVENT_WIFI_FAIL, /* a bad password etc.; the manager has already told the phone app */
    SETUP_SESSION_EVENT_MQTT_SUBMIT,
    SETUP_SESSION_EVENT_HARD_ERROR, /* the manager, the httpd or the AP died mid-session */
} setup_session_event_t;

/* wifi_ssid/wifi_password mirror wifi_sta_config_t's own widths (the
   NETWORK_PROV_WIFI_CRED_RECV event's payload type) — valid only when
   poll returns WIFI_SUCCESS. The mqtt_* fields mirror mqtt_form_result_t
   and are valid only on MQTT_SUBMIT. The caller (setup_session.c) zeroes
   this before every poll call, so a device layer that fills only what its
   event needs still hands back a clean struct for every other field. */
typedef struct {
    char wifi_ssid[33];
    char wifi_password[64];
    char mqtt_uri[MQTT_FORM_URI_MAX];
    char mqtt_user[MQTT_FORM_USER_MAX];
    char mqtt_pass[MQTT_FORM_PASS_MAX];
    bool mqtt_keep_pass;
} setup_session_poll_out_t;

/* ---- the ops table -------------------------------------------------------

   Every device effect the session needs, injected once per call — the
   same seam ota_flow_ops_t and wake_flow's render tails use. */
typedef struct {
    /* C5: push the awake failsafe out to the session's own budget. Same
       signature as main.c's real extend_awake_failsafe(int seconds); task 6
       threads that op straight through. Absolute-from-now, like every
       other caller of this op (ota_flow_ops_t's extend_awake says why).
       False means there is no failsafe to arm — same reasoning as
       ota_flow_apply's refusal: a session nothing can end must not start,
       because the SoftAP + httpd is the single most expensive thing this
       device ever runs (plan, Risks). */
    bool (*extend_awake)(int seconds);

    /* One byte of randomness for setup_session_make_ap_password. */
    uint8_t (*rand_byte)(void);

    /* Device-side: derive the SRP6a salt+verifier for security 2 from
       ap_password (esp_srp_gen_salt_verifier — log how long this takes,
       it is a 3072-bit modexp on an S2), then bring up the SoftAP, the
       httpd instance, the provisioning manager (security 2, service_name =
       ap_ssid, service_key = ap_password), the /mqtt GET+POST handlers and
       the "mqtt-config" protocomm endpoint. False on any failure — nothing
       was reachable, so ERROR with no screen shown. */
    bool (*start)(const char *ap_ssid, const char *ap_password);

    /* Tear down everything `start` brought up: stop the provisioning
       manager, free the SRP salt/verifier, stop the httpd instance, drop
       the SoftAP. ALWAYS SAFE TO CALL, including when `start` was never
       reached (extend_awake refused) or never returned true (it failed
       partway) — the device layer's job is to make this a clean no-op in
       those cases, because setup_session_run() calls it unconditionally on
       every path out, by construction (see its single teardown label). */
    void (*stop)(void);

    /* Block for up to timeout_ms for one event, or return NONE once it
       elapses. `out` arrives zeroed; fill only the fields the returned
       event documents as valid. Modelled on device as a bounded FreeRTOS
       queue receive fed by the manager's event handler and the two MQTT
       entry points (the form POST and the protocomm endpoint). */
    setup_session_event_t (*poll)(uint32_t timeout_ms, setup_session_poll_out_t *out);

    /* Task 5's renders. */
    void (*render_setup_screen)(const setup_session_screen_info_t *info);
    void (*render_complete_screen)(void);
    void (*render_timeout_screen)(void);

    /* D3: the app's own NVS keys are the source of truth. nvs_config_set_wifi_ssid/_pass. */
    bool (*set_wifi_creds)(const char *ssid, const char *password);

    /* D3: clear the esp_wifi driver's OWN persisted copy once the app's
       keys hold the verified credentials, so the two stores cannot
       disagree — network_prov_mgr_reset_wifi_provisioning() (confirmed by
       reading network_provisioning 1.3.1's manager.c: that call is a thin
       wrapper over esp_wifi_restore(), and WIFI_CRED_RECV is what wrote
       the driver's copy in the first place, via
       esp_wifi_set_storage(WIFI_STORAGE_FLASH) + esp_wifi_set_config). */
    void (*clear_wifi_driver_store)(void);

    /* nvs_config_set_mqtt_uri/_user, and _pass unless keep_pass. */
    bool (*set_mqtt_creds)(const char *uri, const char *user, const char *pass, bool keep_pass);
} setup_session_ops_t;

/* ---- the session itself --------------------------------------------------

   Runs the whole episode to completion and returns. Never deep-sleeps or
   restarts (see setup_session_sleep_t above) — task 6's wake_flow does
   that with the result this returns.

   Flow (plan, "The setup session"):
     1. extend_awake(cfg->budget_sec); refuse (ERROR) if it cannot.
     2. generate the AP password and SSID (pure) and the QR payload (pure).
     3. start(ap_ssid, ap_password); refuse (ERROR) if it cannot.
     4. render_setup_screen(&info).
     5. poll in a loop until WIFI_SUCCESS, a budget-exhausting run of NONEs
        (TIMEOUT), an MQTT_SUBMIT that ends the session (cfg->has_wifi_ssid
        already true), or HARD_ERROR.
     6. stop() — unconditionally, on every path, including the two early
        refusals above.
     7. render the outcome's screen (complete/timed out; ERROR gets no
        screen of its own — the plan names only two, and task 6's own
        repaint covers an error the same way every other early-exit in
        this tree already does) and return the outcome + the sleep it
        implies. */
setup_session_result_t setup_session_run(const setup_session_ops_t *ops, const setup_session_cfg_t *cfg);

#ifdef __cplusplus
}
#endif
