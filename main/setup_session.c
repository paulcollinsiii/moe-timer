/* The setup session — lifted out as its own module so the ordering that
   matters (extend the failsafe before anything else runs; tear down on
   every path out, whichever one it is) carries a test instead of a
   convention. See setup_session.h for the full flow and the ops table's
   contract; what lives here is the flow itself plus the pure helpers the
   plan asks to be tested in isolation. */
#include "setup_session.h"

#include <stdio.h>
#include <string.h>

#include "device_id.h"

/* Non-elidable wipe for a buffer that held a password: a plain memset on
   a local about to go out of scope can be (and in practice sometimes is)
   optimized away once the compiler sees the write is never read back.
   The device layer (setup_session_idf.c) uses mbedtls_platform_zeroize
   for the same purpose; this file stays free of ESP-IDF/mbedtls so it
   can be host-tested, hence the hand-rolled volatile loop instead. */
static void secure_zero(void *buf, size_t len) {
    volatile unsigned char *p = (volatile unsigned char *)buf;
    while (len--)
        *p++ = 0;
}

/* Wrap-safe "has `now` reached or passed `deadline`": comparing the
   SIGNED difference, not `now >= deadline` directly, is what stays
   correct across a uint32_t wrap of the underlying clock, as long as the
   true gap between the two readings is under ~24.8 days (2^31 ms) — true
   for any budget this module's Kconfig range allows (1800 s max) plus
   the linger on top of it. */
static bool deadline_passed(uint32_t now, uint32_t deadline) {
    return (int32_t)(now - deadline) >= 0;
}

/* The earlier of two absolute deadlines, same wrap-safe comparison. */
static uint32_t earlier_deadline(uint32_t a, uint32_t b) {
    return deadline_passed(a, b) ? b : a;
}

/* ---- AP password: rejection sampling over the unambiguous alphabet ------ */

/* No 0/O/1/l/I. 57 characters, so a plain `% 57` would favour the
   alphabet's first 256 % 57 == 28 characters over the rest — avoided by
   rejecting any byte at or past the largest multiple of 57 that fits in a
   byte (floor(256/57)*57 == 228) and drawing again. */
static const char AP_PASS_ALPHABET[] = "23456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

void setup_session_make_ap_password(setup_session_rand_byte_fn rand_byte, char out[SETUP_SESSION_AP_PASS_BUF]) {
    const size_t n = sizeof(AP_PASS_ALPHABET) - 1;
    const unsigned limit = (unsigned)(256u / n) * (unsigned)n;
    for (int i = 0; i < SETUP_SESSION_AP_PASS_LEN; i++) {
        unsigned b;
        do {
            b = rand_byte();
        } while (b >= limit);
        out[i] = AP_PASS_ALPHABET[b % n];
    }
    out[SETUP_SESSION_AP_PASS_LEN] = '\0';
}

/* ---- AP SSID: "MagTag-<suffix>" from device_id() ------------------------ */

void setup_session_make_ap_ssid(char out[SETUP_SESSION_AP_SSID_MAX]) {
    static const char PREFIX[] = "magtag-";
    const size_t prefix_len = sizeof(PREFIX) - 1;
    const char *id = device_id();
    const char *suffix = (strncmp(id, PREFIX, prefix_len) == 0) ? id + prefix_len : id;
    snprintf(out, SETUP_SESSION_AP_SSID_MAX, "MagTag-%s", suffix);
}

/* ---- QR payload ----------------------------------------------------------- */

bool setup_session_make_qr_payload(const char *ap_ssid, const char *ap_password, char *out, size_t out_cap) {
    int n = snprintf(out, out_cap,
                     "{\"ver\":\"v1\",\"name\":\"%s\",\"username\":\"%s\",\"pop\":\"%s\",\"password\":\"%s\","
                     "\"transport\":\"%s\",\"security\":2}",
                     ap_ssid, SETUP_SESSION_QR_USERNAME, ap_password, ap_password, SETUP_SESSION_QR_TRANSPORT);
    if (n < 0 || (size_t)n >= out_cap) {
        if (out_cap > 0)
            out[0] = '\0';
        return false;
    }
    return true;
}

/* ---- MQTT status line ------------------------------------------------------ */

static const char *field_label(mqtt_form_field_t f) {
    switch (f) {
        case MQTT_FORM_FIELD_URI:
            return "uri";
        case MQTT_FORM_FIELD_USER:
            return "user";
        case MQTT_FORM_FIELD_PASS:
            return "pass";
        case MQTT_FORM_FIELD_NONE:
        default:
            return "";
    }
}

void setup_session_format_mqtt_status(bool ok, mqtt_form_status_t status, char *out, size_t out_cap) {
    if (out == NULL || out_cap == 0)
        return;
    if (ok) {
        snprintf(out, out_cap, "Saved.");
        return;
    }
    const char *label = field_label(status.field);
    if (label[0] != '\0') {
        snprintf(out, out_cap, "%s: %s", label, mqtt_form_error_str(status.err));
    } else {
        snprintf(out, out_cap, "%s", mqtt_form_error_str(status.err));
    }
}

/* ---- the one place an MQTT form result becomes a stored credential ------ */

bool setup_session_apply_mqtt(const setup_session_ops_t *ops, const mqtt_form_result_t *result) {
    return ops->set_mqtt_creds(result->uri, result->user, result->pass, result->keep_pass);
}

/* ---- the /mqtt form page: chunked, no stack buffer ------------------------ */

typedef struct {
    setup_session_chunk_sink_fn sink;
    void *ctx;
    bool ok;
} page_emit_t;

static void emit(page_emit_t *st, const char *s) {
    if (!st->ok || s == NULL)
        return;
    size_t len = strlen(s);
    if (len == 0)
        return;
    if (!st->sink(s, len, st->ctx))
        st->ok = false;
}

bool setup_session_render_mqtt_page(const setup_session_mqtt_page_in_t *in, setup_session_chunk_sink_fn sink,
                                    void *ctx) {
    page_emit_t st = {.sink = sink, .ctx = ctx, .ok = true};

    emit(&st,
         "<!doctype html><meta name=viewport content=\"width=device-width,initial-scale=1\">"
         "<title>MagTag MQTT setup</title>"
         "<body style=\"font-family:sans-serif;max-width:360px;margin:2em auto\">"
         "<h1>MQTT broker</h1>");
    if (in->status_msg != NULL) {
        emit(&st, "<p><b>");
        emit(&st, in->status_msg);
        emit(&st, "</b></p>");
    }
    emit(&st,
         "<form method=post>"
         "<p>Broker URI<br><input name=uri style=\"width:100%\" value=\"");
    emit(&st, in->uri_escaped);
    emit(&st,
         "\" placeholder=\"mqtt://host:1883\"></p>"
         "<p>Username<br><input name=user style=\"width:100%\" value=\"");
    emit(&st, in->user_escaped);
    emit(&st,
         "\"></p>"
         "<p>Password (leave blank to keep the current one)<br>"
         "<input name=pass type=password style=\"width:100%\"></p>"
         "<p><button type=submit>Save</button></p></form>");
    return st.ok;
}

/* ---- the session itself ---------------------------------------------------- */

/* Runs after a WIFI_SUCCESS whose credentials are already verified and
   stored: keeps polling so the phone app's own "are you connected now?"
   query still gets an answer, until SETUP_SESSION_SUCCESS_LINGER_MS
   elapses, the session's own overall deadline arrives first (whichever
   is sooner), or HARD_ERROR cuts it short. The outcome is already
   WIFI_OK by the time this runs; nothing in here can change it back —
   the credentials are safely in NVS regardless of how the SoftAP/manager
   spend the time that is left. */
static void linger_after_wifi_success(const setup_session_ops_t *ops, uint32_t overall_deadline) {
    uint32_t stop_at = earlier_deadline(ops->now_ms() + SETUP_SESSION_SUCCESS_LINGER_MS, overall_deadline);

    for (;;) {
        uint32_t now = ops->now_ms();
        if (deadline_passed(now, stop_at))
            return;
        uint32_t remaining = stop_at - now;
        uint32_t wait = (remaining < SETUP_SESSION_POLL_QUANTUM_MS) ? remaining : SETUP_SESSION_POLL_QUANTUM_MS;
        if (wait == 0)
            wait = 1;

        setup_session_poll_out_t out;
        memset(&out, 0, sizeof(out));
        setup_session_event_t ev = ops->poll(wait, &out);
        if (ev == SETUP_SESSION_EVENT_HARD_ERROR)
            return;
        /* NONE, a stray WIFI_FAIL/WIFI_SUCCESS (the manager fires
           CRED_SUCCESS once; defensive only) and MQTT_STORED all just
           keep lingering — an MQTT submit during the linger is still
           accepted and stored, it just does not end the session early. */
    }
}

static setup_session_outcome_t run_loop(const setup_session_ops_t *ops, const setup_session_cfg_t *cfg,
                                        uint32_t deadline, bool *wifi_store_failed) {
    for (;;) {
        uint32_t now = ops->now_ms();
        if (deadline_passed(now, deadline))
            return SETUP_SESSION_OUTCOME_TIMEOUT;
        uint32_t remaining = deadline - now;
        uint32_t wait = (remaining < SETUP_SESSION_POLL_QUANTUM_MS) ? remaining : SETUP_SESSION_POLL_QUANTUM_MS;
        if (wait == 0)
            wait = 1; /* a 0 ms poll would not block at all */

        setup_session_poll_out_t out;
        memset(&out, 0, sizeof(out));
        setup_session_event_t ev = ops->poll(wait, &out);

        switch (ev) {
            case SETUP_SESSION_EVENT_NONE:
            case SETUP_SESSION_EVENT_WIFI_FAIL:
                /* A bad password is the manager's to report to the phone
                   app (plan, "Components"); nothing to store here, and the
                   session keeps waiting for a real attempt (the device
                   layer resets the manager's own state machine so a retry
                   is possible — this layer never needed to know that
                   happened). */
                continue;

            case SETUP_SESSION_EVENT_WIFI_SUCCESS: {
                bool stored = ops->set_wifi_creds(out.wifi_ssid, out.wifi_password);
                secure_zero(out.wifi_password, sizeof(out.wifi_password));
                if (!stored) {
                    /* The driver's own copy is the only verified copy
                       left — must not be cleared. ERROR, not
                       WIFI_OK: the join really did happen, but this
                       session cannot tell the rest of the device about
                       it, so it must not claim success. */
                    *wifi_store_failed = true;
                    return SETUP_SESSION_OUTCOME_ERROR;
                }
                ops->clear_wifi_driver_store();
                linger_after_wifi_success(ops, deadline);
                return SETUP_SESSION_OUTCOME_WIFI_OK;
            }

            case SETUP_SESSION_EVENT_MQTT_STORED:
                /* Already stored, synchronously, by the handler that
                   posted this (setup_session_apply_mqtt) — nothing left
                   to do here but decide whether it also ends the
                   session. cfg->has_wifi_ssid is still the value this
                   call started with: the only event that could make it
                   stale is WIFI_SUCCESS above, and that one always
                   returns before this case can run again. */
                if (cfg->has_wifi_ssid)
                    return SETUP_SESSION_OUTCOME_MQTT_ONLY;
                continue; /* no SSID yet: keep waiting for WiFi */

            case SETUP_SESSION_EVENT_HARD_ERROR:
                return SETUP_SESSION_OUTCOME_ERROR;
        }
    }
}

setup_session_result_t setup_session_run(const setup_session_ops_t *ops, const setup_session_cfg_t *cfg) {
    setup_session_outcome_t outcome;
    bool wifi_store_failed = false;
    uint32_t deadline;

    /* Step 1: the budget, before anything else runs — a session nothing
       can end must not start (same reasoning as ota_flow_apply's refusal
       when extend_awake has nothing to arm: the SoftAP + httpd is the
       single most expensive thing this device ever runs, plan's Risks).
       SETUP_SESSION_TAIL_SEC covers everything that still has to run
       after the loop below returns — there is no second extend_awake
       call after this one. */
    if (!ops->extend_awake(cfg->budget_sec + SETUP_SESSION_TAIL_SEC)) {
        outcome = SETUP_SESSION_OUTCOME_ERROR;
        goto teardown;
    }
    deadline = ops->now_ms() + (uint32_t)cfg->budget_sec * 1000u;

    /* Step 2: the pure generators. Cannot fail — snprintf truncates safely
       in the pathological case of an oversized device_id(), and the QR
       buffer is sized generously enough that this never happens on this
       device (SETUP_SESSION_QR_MAX's own comment). */
    {
        setup_session_screen_info_t info;
        memset(&info, 0, sizeof(info));
        setup_session_make_ap_ssid(info.ap_ssid);
        setup_session_make_ap_password(ops->rand_byte, info.ap_password);
        (void)setup_session_make_qr_payload(info.ap_ssid, info.ap_password, info.qr_payload, sizeof(info.qr_payload));
        info.form_url = "http://192.168.4.1/mqtt";

        /* Step 3: bring the SoftAP, httpd, manager and endpoints up. */
        if (!ops->start(info.ap_ssid, info.ap_password)) {
            secure_zero(&info, sizeof(info));
            outcome = SETUP_SESSION_OUTCOME_ERROR;
            goto teardown;
        }

        /* Step 4: the setup screen, now that there is something to show. */
        ops->render_setup_screen(&info);
        secure_zero(&info, sizeof(info)); /* ap_password (and its copy inside qr_payload) done being needed */
    }

    /* Step 5: loop until an outcome. */
    outcome = run_loop(ops, cfg, deadline, &wifi_store_failed);

teardown:
    /* Step 6: unconditional, on every path above — this is what makes
       "teardown always runs" a property of the control flow rather than
       of how carefully each branch remembered to call it. Safe even when
       nothing was ever started (the device layer's contract). */
    ops->stop();

    /* Step 7: the outcome's screen (plan names only "complete" and "timed
       out" for a clean exit; a start/hard-error ERROR gets none of its
       own, the same as every other early-exit path in this tree — except
       the ERROR that follows a verified-but-unstored WIFI_SUCCESS, where
       the panel is already showing the now-torn-down AP's name,
       password and QR and there is no dedicated error screen to show
       instead, so this reuses the timeout one). */
    switch (outcome) {
        case SETUP_SESSION_OUTCOME_WIFI_OK:
        case SETUP_SESSION_OUTCOME_MQTT_ONLY:
            ops->render_complete_screen();
            break;
        case SETUP_SESSION_OUTCOME_TIMEOUT:
            ops->render_timeout_screen();
            break;
        case SETUP_SESSION_OUTCOME_ERROR:
            if (wifi_store_failed)
                ops->render_timeout_screen();
            break;
    }

    setup_session_result_t result;
    result.outcome = outcome;
    switch (outcome) {
        case SETUP_SESSION_OUTCOME_WIFI_OK:
        case SETUP_SESSION_OUTCOME_MQTT_ONLY:
            result.sleep = SETUP_SESSION_SLEEP_NET_WINDOW;
            break;
        case SETUP_SESSION_OUTCOME_TIMEOUT:
        case SETUP_SESSION_OUTCOME_ERROR:
        default:
            result.sleep = cfg->has_wifi_ssid ? SETUP_SESSION_SLEEP_NORMAL : SETUP_SESSION_SLEEP_BUTTON_ONLY;
            break;
    }
    return result;
}
