#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mqtt_form.h"  /* mqtt_form_status_t, mqtt_form_result_t, MQTT_FORM_*_MAX — reused, not restated */
#include "nvs_config.h" /* NVS_CONFIG_WIFI_SSID_BUF/_PASS_BUF — the widths NVS actually stores */

#ifndef NATIVE
#include "sdkconfig.h"
#endif

/* WiFi + MQTT provisioning plan (docs/planning/20261003.wifi-provisioning.plan.md):
   the setup session itself. Drives the whole SoftAP-provisioning +
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

/* The longest a single `poll` call is allowed to block. The budget and
   the success linger below are both measured against `ops->now_ms()`
   (wall time, real on device, an injected fake under test), so this is
   no longer a charge-per-call unit — it is only how finely the deadline
   gets checked: a `poll` that blocks for this long and returns NONE
   costs the loop one more look at the clock, not a fixed slice of the
   budget. Small enough that a budget shorter than one quantum still
   gets at least one real poll. */
#define SETUP_SESSION_POLL_QUANTUM_MS 250u

/* How long WIFI_SUCCESS keeps the session (and the SoftAP + manager +
   httpd underneath it) alive before returning WIFI_OK, instead of
   tearing down within about a second of the credentials verifying.
   The stock provisioning apps poll the manager's
   own "are you connected now?" status roughly every 5 s after sending
   credentials (confirmed from the Android app's ESPDevice.
   pollForWifiConnectionStatus, a 5 s sleep per poll) and report
   "Provisioning Failed" the moment one such poll fails — which it will,
   immediately, if the transport is already gone. 15 s covers three of
   those polls even if the first one lands an instant before the
   credentials verify, which is the worst timing this module can control
   for. Charged against the same budget as everything else, so a session
   that succeeds with only a few seconds of budget left lingers for
   whatever is left, not the full 15 s. */
#define SETUP_SESSION_SUCCESS_LINGER_MS 15000u

/* extend_awake(cfg->budget_sec + SETUP_SESSION_TAIL_SEC) — everything
   that still has to run after the loop returns, with no further extend
   call behind it (this is the session's last one), has to fit inside
   this tail or the awake failsafe can fire mid-teardown: main.c's
   awake_failsafe_cb does not render a screen, so the
   panel would be left showing the SoftAP name, password and QR for a
   session that is no longer running.

   Budgeted against the worst case each term admits to, not the typical
   case, because a failsafe tail exists for the run that doesn't go
   typically:
     - the SRP6a 3072-bit modexp (esp_srp_gen_salt_verifier) — this
       module logs it at runtime but has no bench figure for an S2, so
       10 s is a deliberately generous placeholder;
     - two full e-paper refreshes (the setup screen, then the outcome
       screen) at the panel driver's own worst-case wait rather than its
       documented typical one (ssd1680.c's BUSY_TIMEOUT_MS is 10 s;
       the same file's comment puts a normal full refresh at "~3-4 s"),
       20 s total;
     - teardown — the manager's own cleanup_delay plus esp_wifi and
       httpd coming down — bounded generously at 10 s.
   40 s accounted for, plus 20 s of margin for whatever this list
   missed, the same spirit as OTA_ABORT_TAIL_MS's own margin over its
   strict minimum (ota_timing.h). */
#define SETUP_SESSION_TAIL_SEC 60

/* ---- outcomes and the sleep the caller owes (D4) ------------------------- */

typedef enum {
    SETUP_SESSION_OUTCOME_WIFI_OK = 0, /* WiFi provisioned, verified STA join; MQTT too if submitted this session */
    SETUP_SESSION_OUTCOME_MQTT_ONLY,   /* MQTT submitted on a device that already had an SSID; session ends */
    SETUP_SESSION_OUTCOME_TIMEOUT,     /* budget expired */
    SETUP_SESSION_OUTCOME_ERROR,       /* a start failure, a failed credential store, or a hard error mid-session */
} setup_session_outcome_t;

/* What the caller (the wake flow) must sleep into. THE SESSION NEVER
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
     {"ver":"v1","name":"<ap_ssid>","username":"<user>","pop":"<ap_password>",
      "password":"<ap_password>","transport":"softap","security":2}

   ver/name/username/pop/transport confirmed against espressif/network_
   provisioning 1.3.1's own example (examples/wifi_prov/main/app_main.c:
   264-277, the sec2 branch) and its README's logged QR text (README.md:145).
   "pop" doubles as the SRP password — the example's own comment says so:
   "this pop field represents the password that will be used to generate
   salt and verifier ... present here in order to generate the QR code
   containing password."

   "password" and "security" are additional keys both stock phone apps
   read from this same JSON object to join the SoftAP itself, rather than
   making the owner find it in the OS WiFi picker: Android's
   ESPProvisionManager.processQrCode decodes name/pop/transport/security/
   username/password and, for softap, builds a WiFiAccessPoint from
   name+password and joins it programmatically; iOS's parseQrCode decodes
   the same keys and passes `softAPPassword: decodeResponse.password ??
   ""`. The example this format was first confirmed against omits both
   because it runs dev-mode with service_key = NULL (an open AP), which
   is not this device's case — its AP is WPA2, so without "password" the
   scanning app has no way to join it and the QR-scan flow never
   completes. ap_ssid/ap_password are never escaped:
   both come from this module's own generators, whose alphabets contain
   no `"` or `\`. "security":2 is a JSON number, matching this session's
   fixed security level, not a string.

   Writes the JSON into `out` (capacity out_cap) and returns true, or
   writes "" and returns false if it would not fit (out_cap >= 1
   required to see even that). */
#define SETUP_SESSION_QR_USERNAME "magtag"
#define SETUP_SESSION_QR_TRANSPORT "softap"
/* The skeleton below with every variable field at its maximum: a 31-byte
   ap_ssid (SETUP_SESSION_AP_SSID_MAX-1) and the fixed-length ap_password
   used twice (pop and password) comes to exactly 150 bytes; this adds a
   NUL and slack for a future field without needing to revisit callers. */
#define SETUP_SESSION_QR_MAX 160

bool setup_session_make_qr_payload(const char *ap_ssid, const char *ap_password, char *out, size_t out_cap);

/* ---- the setup screen's info, the render call's input --------------------- */

typedef struct {
    char ap_ssid[SETUP_SESSION_AP_SSID_MAX];
    char ap_password[SETUP_SESSION_AP_PASS_BUF];
    char qr_payload[SETUP_SESSION_QR_MAX];
    const char *form_url; /* fixed literal; the setup screen renders it verbatim */
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
    /* The handler already stored this, synchronously, through
       setup_session_apply_mqtt() below, before it posted — this event
       only says "stored", carrying no credential of its own. */
    SETUP_SESSION_EVENT_MQTT_STORED,
    /* A manager/httpd failure the device layer observed mid-session: an
       unsolicited NETWORK_PROV_END or NETWORK_PROV_DEINIT, one neither
       requested by this session's own stop() nor explained by any of
       the events above. */
    SETUP_SESSION_EVENT_HARD_ERROR,
} setup_session_event_t;

/* wifi_ssid/wifi_password mirror wifi_sta_config_t's own widths plus a
   NUL (NVS_CONFIG_WIFI_SSID_BUF/_PASS_BUF — the NETWORK_PROV_WIFI_CRED_RECV
   event's payload type) — valid only when poll returns WIFI_SUCCESS. The
   caller (setup_session.c) zeroes this before every poll call, so a
   device layer that fills only what its event needs still hands back a
   clean struct for every other field. */
typedef struct {
    char wifi_ssid[NVS_CONFIG_WIFI_SSID_BUF];
    char wifi_password[NVS_CONFIG_WIFI_PASS_BUF];
} setup_session_poll_out_t;

/* ---- the ops table -------------------------------------------------------

   Every device effect the session needs, injected once per call — the
   same seam ota_flow_ops_t and wake_flow's render tails use. */
typedef struct {
    /* C5: push the awake failsafe out to the session's own budget plus
       SETUP_SESSION_TAIL_SEC. Same signature as main.c's real
       extend_awake_failsafe(int seconds); the wake flow threads that op
       straight through. Absolute-from-now, like every other caller of this op
       (ota_flow_ops_t's extend_awake says why). False means there is no
       failsafe to arm — same reasoning as ota_flow_apply's refusal: a
       session nothing can end must not start, because the SoftAP + httpd
       is the single most expensive thing this device ever runs (plan,
       Risks). */
    bool (*extend_awake)(int seconds);

    /* Wall-clock milliseconds, real on device (esp_timer_get_time()/1000)
       and fake under test (the test's clock advances per poll call, not
       per quantum charged). Every deadline in this module — the overall
       budget and the success linger — is computed by comparing two
       readings of this with wrap-safe (subtract, then compare the
       signed difference) arithmetic, not by assuming any one poll call
       cost a fixed amount of it: a `poll` that blocks longer than its
       own timeout (scheduler starvation, a slow httpd handler on the
       same core) can no longer make the loop run past its deadline, and
       an early return can no longer make it stop short of it. */
    uint32_t (*now_ms)(void);

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
       queue receive fed by the manager's event handler and the MQTT
       entry points (the form POST and the protocomm endpoint), both of
       which store the credential themselves before posting. */
    setup_session_event_t (*poll)(uint32_t timeout_ms, setup_session_poll_out_t *out);

    /* The setup screens' renders. */
    void (*render_setup_screen)(const setup_session_screen_info_t *info);
    void (*render_complete_screen)(void);
    void (*render_timeout_screen)(void);

    /* D3: the app's own NVS keys are the source of truth.
       nvs_config_set_wifi_ssid/_pass. A failure here must not be
       swallowed — see run_loop's WIFI_SUCCESS case in setup_session.c:
       the driver's store is the only verified copy left if this fails,
       so it must not be cleared either. */
    bool (*set_wifi_creds)(const char *ssid, const char *password);

    /* D3: clear the esp_wifi driver's OWN persisted copy once the app's
       keys hold the verified credentials, so the two stores cannot
       disagree — network_prov_mgr_reset_wifi_provisioning() (confirmed by
       reading network_provisioning 1.3.1's manager.c: that call is a thin
       wrapper over esp_wifi_restore(), and WIFI_CRED_RECV is what wrote
       the driver's copy in the first place, via
       esp_wifi_set_storage(WIFI_STORAGE_FLASH) + esp_wifi_set_config).
       Only called after set_wifi_creds has already succeeded. */
    void (*clear_wifi_driver_store)(void);

    /* nvs_config_set_mqtt_uri/_user, and _pass unless keep_pass. Called
       from setup_session_apply_mqtt(), below — not from run_loop, now
       that both MQTT entry points store synchronously before posting
       MQTT_STORED. */
    bool (*set_mqtt_creds)(const char *uri, const char *user, const char *pass, bool keep_pass);
} setup_session_ops_t;

/* ---- the one place an MQTT form result becomes a stored credential ------

   Both MQTT entry points (the /mqtt form POST handler and the
   "mqtt-config" protocomm endpoint handler) call this, synchronously, in
   the httpd task, BEFORE replying — so the reply reflects whether the
   store actually happened ("Saved." only on true) rather than merely
   "was queued". keep_pass and the empty-URI-clears-
   the-password behaviour are entirely mqtt_form_result_t's own contract
   (mqtt_form.h's doc comment on that struct); this function adds no
   decision beyond forwarding it to the op. Pure and host-tested: callers
   on the real device build a one-field ops table around their own
   set_mqtt_creds rather than threading setup_session_run's full table
   through the httpd layer. */
bool setup_session_apply_mqtt(const setup_session_ops_t *ops, const mqtt_form_result_t *result);

/* ---- the session itself --------------------------------------------------

   Runs the whole episode to completion and returns. Never deep-sleeps or
   restarts (see setup_session_sleep_t above) — the wake flow does
   that with the result this returns.

   Flow (plan, "The setup session"):
     1. extend_awake(cfg->budget_sec + SETUP_SESSION_TAIL_SEC); refuse
        (ERROR) if it cannot.
     2. generate the AP password and SSID (pure) and the QR payload (pure).
     3. start(ap_ssid, ap_password); refuse (ERROR) if it cannot.
     4. render_setup_screen(&info).
     5. poll in a loop until WIFI_SUCCESS (then linger up to
        SETUP_SESSION_SUCCESS_LINGER_MS before returning WIFI_OK), a
        budget-exhausting run of NONEs (TIMEOUT), an MQTT_STORED that
        ends the session (cfg->has_wifi_ssid already true), or
        HARD_ERROR. A WIFI_SUCCESS whose set_wifi_creds fails is ERROR,
        not WIFI_OK, and does not clear the driver's store.
     6. stop() — unconditionally, on every path, including the two early
        refusals above.
     7. render the outcome's screen (complete/timed out; a start/hard-
        error ERROR gets no screen of its own, the same as every other
        early-exit in this tree, but the WIFI_SUCCESS-store-failure ERROR
        above renders the timeout screen, because by that point the
        panel is already showing the now-torn-down AP's name, password
        and QR and the wake flow has no screen of its own for this case)
        and return the outcome + the sleep it implies. */
setup_session_result_t setup_session_run(const setup_session_ops_t *ops, const setup_session_cfg_t *cfg);

#ifdef __cplusplus
}
#endif
