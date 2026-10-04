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
                     "{\"ver\":\"v1\",\"name\":\"%s\",\"username\":\"%s\",\"pop\":\"%s\",\"transport\":\"%s\"}",
                     ap_ssid, SETUP_SESSION_QR_USERNAME, ap_password, SETUP_SESSION_QR_TRANSPORT);
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

static setup_session_outcome_t run_loop(const setup_session_ops_t *ops, const setup_session_cfg_t *cfg,
                                        bool *mqtt_stored) {
    uint32_t ticks = ((uint32_t)cfg->budget_sec * 1000u) / SETUP_SESSION_POLL_QUANTUM_MS;
    if (ticks == 0)
        ticks = 1; /* a budget shorter than one quantum still gets one poll */

    for (; ticks > 0; ticks--) {
        setup_session_poll_out_t out;
        memset(&out, 0, sizeof(out));
        setup_session_event_t ev = ops->poll(SETUP_SESSION_POLL_QUANTUM_MS, &out);

        switch (ev) {
            case SETUP_SESSION_EVENT_NONE:
            case SETUP_SESSION_EVENT_WIFI_FAIL:
                /* A bad password is the manager's to report to the phone
                   app (plan, "Components"); nothing to store here, and the
                   session keeps waiting for a real attempt. */
                continue;

            case SETUP_SESSION_EVENT_WIFI_SUCCESS:
                ops->set_wifi_creds(out.wifi_ssid, out.wifi_password);
                ops->clear_wifi_driver_store();
                return SETUP_SESSION_OUTCOME_WIFI_OK;

            case SETUP_SESSION_EVENT_MQTT_SUBMIT:
                ops->set_mqtt_creds(out.mqtt_uri, out.mqtt_user, out.mqtt_pass, out.mqtt_keep_pass);
                *mqtt_stored = true;
                /* cfg->has_wifi_ssid is still the value this call started
                   with: the only event that could make it stale is
                   WIFI_SUCCESS above, and that one always returns before
                   this case can run again. */
                if (cfg->has_wifi_ssid)
                    return SETUP_SESSION_OUTCOME_MQTT_ONLY;
                continue; /* no SSID yet: keep waiting for WiFi */

            case SETUP_SESSION_EVENT_HARD_ERROR:
                return SETUP_SESSION_OUTCOME_ERROR;
        }
    }
    return SETUP_SESSION_OUTCOME_TIMEOUT;
}

setup_session_result_t setup_session_run(const setup_session_ops_t *ops, const setup_session_cfg_t *cfg) {
    setup_session_outcome_t outcome;
    bool mqtt_stored = false;

    /* Step 1: the budget, before anything else runs — a session nothing
       can end must not start (same reasoning as ota_flow_apply's refusal
       when extend_awake has nothing to arm: the SoftAP + httpd is the
       single most expensive thing this device ever runs, plan's Risks). */
    if (!ops->extend_awake(cfg->budget_sec)) {
        outcome = SETUP_SESSION_OUTCOME_ERROR;
        goto teardown;
    }

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
            outcome = SETUP_SESSION_OUTCOME_ERROR;
            goto teardown;
        }

        /* Step 4: the setup screen, now that there is something to show. */
        ops->render_setup_screen(&info);
    }

    /* Step 5: loop until an outcome. */
    outcome = run_loop(ops, cfg, &mqtt_stored);

teardown:
    /* Step 6: unconditional, on every path above — this is what makes
       "teardown always runs" a property of the control flow rather than
       of how carefully each branch remembered to call it. Safe even when
       nothing was ever started (the device layer's contract). */
    ops->stop();

    /* Step 7: the outcome's screen (plan names only "complete" and "timed
       out" — an ERROR gets none of its own; task 6's own repaint covers
       it, the same as every other early-exit path in this tree). */
    switch (outcome) {
        case SETUP_SESSION_OUTCOME_WIFI_OK:
        case SETUP_SESSION_OUTCOME_MQTT_ONLY:
            ops->render_complete_screen();
            break;
        case SETUP_SESSION_OUTCOME_TIMEOUT:
            ops->render_timeout_screen();
            break;
        case SETUP_SESSION_OUTCOME_ERROR:
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
    (void)mqtt_stored; /* not read by the outcome/sleep decision; kept for a future caller and for symmetry with the
                          tests */
    return result;
}
