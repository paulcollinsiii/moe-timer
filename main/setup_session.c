/* The setup session — lifted out as its own module so the ordering that
   matters (extend the failsafe before anything else runs; render before
   the radio comes up; tear down on every path out, whichever one it is)
   carries a test instead of a convention. See setup_session.h for the
   full flow and the ops table's contract; what lives here is the flow
   itself plus the pure helpers worth testing in isolation. */
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

/* No 0/O/1/l/I, and no S/5, Z/2, B/8 or g/9 either — all eight are
   confusable pairs at 18 pt on this panel's font, the same standard the
   original five were cut for, not a new one. 49 characters, so a plain
   `% 49` would favour the alphabet's first 256 % 49 == 11 characters over
   the rest — avoided by rejecting any byte at or past the largest
   multiple of 49 that fits in a byte (floor(256/49)*49 == 245) and
   drawing again. 10 characters from a 49-character alphabet is
   log2(49^10) =~ 56.1 bits of entropy (was =~58.3 for the 57-character
   set) — the trade this module makes so a password is never misread
   typing it in by hand. */
static const char AP_PASS_ALPHABET[] = "3467ACDEFGHJKLMNPQRTUVWXYabcdefhijkmnopqrstuvwxyz";

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

/* Appends s to out at *o, backslash-escaping the characters the WIFI: format
   reserves. Whole escapes only: a character that needs two bytes and has
   room for one is a failure, never a dangling backslash. Always leaves one
   byte for the NUL. */
static bool put_escaped(char *out, size_t cap, size_t *o, const char *s, bool escape) {
    for (; *s != '\0'; s++) {
        bool special = escape && (*s == '\\' || *s == ';' || *s == ',' || *s == ':' || *s == '"');
        size_t need = special ? 2 : 1;
        if (*o + need + 1 > cap)
            return false;
        if (special)
            out[(*o)++] = '\\';
        out[(*o)++] = *s;
    }
    return true;
}

bool setup_session_make_qr_payload(const char *ap_ssid, const char *ap_password, char *out, size_t out_cap) {
    if (out == NULL || out_cap == 0)
        return false;
    out[0] = '\0';
    if (ap_ssid == NULL || ap_password == NULL)
        return false;

    size_t o = 0;
    bool ok = put_escaped(out, out_cap, &o, "WIFI:T:WPA;S:", false) && put_escaped(out, out_cap, &o, ap_ssid, true) &&
              put_escaped(out, out_cap, &o, ";P:", false) && put_escaped(out, out_cap, &o, ap_password, true) &&
              put_escaped(out, out_cap, &o, ";;", false);
    if (!ok) {
        out[0] = '\0';
        return false;
    }
    out[o] = '\0';
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
        case MQTT_FORM_FIELD_WIFI_SSID:
            return "network name";
        case MQTT_FORM_FIELD_WIFI_PASS:
            return "wifi password";
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

/* The join state's own banner, shown when no submit message overrides it.
   The CONNECTING text tells a JavaScript-off browser to reload by hand;
   with the script on, the page reloads itself. */
static const char *join_banner(setup_join_state_t state, setup_join_reason_t reason, const char **tail) {
    *tail = "";
    switch (state) {
        case SETUP_JOIN_CONNECTING:
            return "Connecting to WiFi... reload this page to see the result.";
        case SETUP_JOIN_FAILED:
            /* Each reason gets advice that fits it: telling the owner to
               check the password after "network not found", or to retry a
               save the device has already given up on, sends them the
               wrong way. */
            switch (reason) {
                case SETUP_JOIN_REASON_WRONG_PASSWORD:
                    *tail = ". Check the password and try again.";
                    return "Could not join: wrong password";
                case SETUP_JOIN_REASON_NOT_FOUND:
                    *tail = ". Check the network name and try again.";
                    return "Could not join: network not found";
                case SETUP_JOIN_REASON_SECURITY_MISMATCH:
                    *tail = ". Check the password (leave it blank only for an open network) and try again.";
                    return "Could not join: the network did not accept the security setting";
                case SETUP_JOIN_REASON_SAVE_FAILED:
                    *tail = ". The device will end setup.";
                    return "Joined, but could not save: flash write failed";
                case SETUP_JOIN_REASON_NONE:
                case SETUP_JOIN_REASON_UNKNOWN:
                default:
                    *tail = ". Check the name and password and try again.";
                    return "Could not join: unknown";
            }
        case SETUP_JOIN_SAVED:
            return "WiFi saved. The device will reconnect on its own; you can close this page.";
        case SETUP_JOIN_IDLE:
        default:
            return NULL;
    }
}

bool setup_session_render_page(const setup_session_page_in_t *in, setup_session_chunk_sink_fn sink, void *ctx) {
    page_emit_t st = {.sink = sink, .ctx = ctx, .ok = true};
    bool has_ssid = in->ssid_escaped != NULL && in->ssid_escaped[0] != '\0';

    emit(&st,
         "<!doctype html><meta charset=utf-8>"
         "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
         "<title>MagTag setup</title>"
         "<body style=\"font-family:sans-serif;max-width:360px;margin:2em auto\">"
         "<h1>MagTag setup</h1>");

    const char *tail = "";
    const char *banner = in->status_msg != NULL ? in->status_msg : join_banner(in->join_state, in->join_reason, &tail);
    if (banner != NULL) {
        emit(&st, "<p><b id=b>");
        emit(&st, banner);
        emit(&st, tail);
        emit(&st, "</b></p>");
    }

    emit(&st, "<form method=post action=/><h2>WiFi</h2><p>Network name ");
    emit(&st, has_ssid ? "(blank keeps the current one)" : "(required)");
    emit(&st, "<br><input name=ssid maxlength=32 style=\"width:100%\" placeholder=\"");
    emit(&st, in->ssid_escaped);
    emit(&st,
         "\"></p><p>Password (blank for an open network)<br>"
         "<input name=wpass type=password maxlength=64 style=\"width:100%\"></p>"
         "<h2>MQTT broker</h2><p>Broker URI (blank keeps the current one)<br>"
         "<input name=uri style=\"width:100%\" placeholder=\"mqtt://host:1883\" value=\"");
    emit(&st, in->uri_escaped);
    emit(&st, "\"></p><p>Username<br><input name=user style=\"width:100%\" value=\"");
    emit(&st, in->user_escaped);
    emit(&st,
         "\"></p><p>Password (blank keeps the current one)<br>"
         "<input name=pass type=password style=\"width:100%\"></p>"
         "<p><button type=submit>Save</button></p></form>");

    if (in->join_state == SETUP_JOIN_CONNECTING) {
        emit(&st,
             /* The phone can leave the SoftAP when the join moves it to the
                router's channel, and the AP itself goes away after a
                success, so the poll stops after 60 tries (2 minutes) or 5
                failed fetches in a row and says where to look instead. */
             "<script>var n=0,f=0;function g(){document.getElementById('b').textContent="
             "'This page stopped hearing from the device. Check the device screen, "
             "or reopen http://" SETUP_SESSION_AP_IP
             " to see the result.'}"
             "function p(){if(n++>60||f>4)return g();fetch('/status').then(function(r){return r.json()})"
             ".then(function(j){f=0;if(j.state=='connecting')setTimeout(p,2000);else location.reload()})"
             ".catch(function(){f++;setTimeout(p,2000)})}setTimeout(p,2000)</script>");
    }
    return st.ok;
}

/* ---- the join state's wire forms ------------------------------------------ */

static const char *join_state_str(setup_join_state_t state) {
    switch (state) {
        case SETUP_JOIN_CONNECTING:
            return "connecting";
        case SETUP_JOIN_FAILED:
            return "failed";
        case SETUP_JOIN_SAVED:
            return "saved";
        case SETUP_JOIN_IDLE:
        default:
            return "idle";
    }
}

static const char *join_reason_str(setup_join_reason_t reason) {
    switch (reason) {
        case SETUP_JOIN_REASON_WRONG_PASSWORD:
            return "wrong password";
        case SETUP_JOIN_REASON_NOT_FOUND:
            return "network not found";
        case SETUP_JOIN_REASON_SECURITY_MISMATCH:
            return "security mismatch";
        case SETUP_JOIN_REASON_UNKNOWN:
            return "unknown";
        case SETUP_JOIN_REASON_SAVE_FAILED:
            return "could not save";
        case SETUP_JOIN_REASON_NONE:
        default:
            return "";
    }
}

bool setup_session_format_status_json(setup_join_state_t state, setup_join_reason_t reason, char *out, size_t out_cap) {
    if (out == NULL || out_cap == 0)
        return false;
    const char *reason_text = (state == SETUP_JOIN_FAILED) ? join_reason_str(reason) : "";
    int n = snprintf(out, out_cap, "{\"state\":\"%s\",\"reason\":\"%s\"}", join_state_str(state), reason_text);
    if (n < 0 || (size_t)n >= out_cap) {
        out[0] = '\0';
        return false;
    }
    return true;
}

/* ---- one setup page submit ------------------------------------------------- */

setup_session_apply_t setup_session_apply_setup(const setup_session_ops_t *ops, const mqtt_form_setup_t *form) {
    if (!form->has_wifi && !form->has_mqtt)
        return SETUP_APPLY_NOTHING;

    bool join_refused = form->has_wifi && !ops->join_wifi(form->ssid, form->wifi_pass);

    if (form->has_mqtt && !setup_session_apply_mqtt(ops, &form->mqtt))
        return SETUP_APPLY_MQTT_FAILED;

    if (join_refused)
        return form->has_mqtt ? SETUP_APPLY_JOIN_REFUSED_MQTT_SAVED : SETUP_APPLY_JOIN_REFUSED;
    return form->has_wifi ? SETUP_APPLY_JOINING : SETUP_APPLY_MQTT_SAVED;
}

/* ---- the join's reasons and transitions ------------------------------------ */

setup_join_reason_t setup_join_reason_from_disconnect(int wifi_reason) {
    switch (wifi_reason) {
        case SETUP_WIFI_REASON_AUTH_FAIL:
        case SETUP_WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        case SETUP_WIFI_REASON_HANDSHAKE_TIMEOUT:
        case SETUP_WIFI_REASON_MIC_FAILURE:
            return SETUP_JOIN_REASON_WRONG_PASSWORD;
        case SETUP_WIFI_REASON_NO_AP_FOUND:
            return SETUP_JOIN_REASON_NOT_FOUND;
        case SETUP_WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
        case SETUP_WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
            return SETUP_JOIN_REASON_SECURITY_MISMATCH;
        default:
            return SETUP_JOIN_REASON_UNKNOWN;
    }
}

void setup_join_init(setup_join_t *j) {
    j->reason = SETUP_JOIN_REASON_NONE;
    j->state = SETUP_JOIN_IDLE;
}

void setup_join_on_connecting(setup_join_t *j) {
    j->reason = SETUP_JOIN_REASON_NONE;
    j->state = SETUP_JOIN_CONNECTING;
}

void setup_join_on_saved(setup_join_t *j) {
    j->state = SETUP_JOIN_SAVED;
}

void setup_join_on_save_failed(setup_join_t *j) {
    j->reason = SETUP_JOIN_REASON_SAVE_FAILED;
    j->state = SETUP_JOIN_FAILED;
}

void setup_join_on_failed(setup_join_t *j, int wifi_reason, void (*reset)(void *ctx), void *ctx) {
    if (reset != NULL)
        reset(ctx);
    j->reason = setup_join_reason_from_disconnect(wifi_reason);
    j->state = SETUP_JOIN_FAILED;
}

const char *setup_session_apply_msg(setup_session_apply_t result) {
    switch (result) {
        case SETUP_APPLY_NOTHING:
            return "Nothing to save: enter a WiFi network or a broker.";
        case SETUP_APPLY_JOIN_REFUSED:
            return "WiFi was not changed: a join is running, or WiFi is already saved. Reload to see which.";
        case SETUP_APPLY_JOIN_REFUSED_MQTT_SAVED:
            return "Broker saved. WiFi was not changed: a join is running, or WiFi is already saved.";
        case SETUP_APPLY_MQTT_FAILED:
            return "could not save: flash write failed";
        case SETUP_APPLY_JOINING:
            return "Connecting to WiFi...";
        case SETUP_APPLY_MQTT_SAVED:
        default:
            return "Saved.";
    }
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
                                        uint32_t deadline) {
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
                /* A bad password is the provisioning manager's own job to
                   report to the phone app; nothing to store here, and the
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
                       it, so it must not claim success. setup_session_run()
                       still renders the FAILED screen for this, because the
                       setup screen already painted. */
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
    bool screen_painted = false;
    uint32_t deadline;

    /* Step 1: the budget, before anything else runs — a session nothing
       can end must not start (same reasoning as ota_flow_apply's refusal
       when extend_awake has nothing to arm: the SoftAP + httpd is the
       single most expensive thing this device ever runs).
       SETUP_SESSION_TAIL_SEC covers everything that still has to run
       after the loop below returns — there is no second extend_awake
       call after this one. */
    if (!ops->extend_awake(cfg->budget_sec + SETUP_SESSION_TAIL_SEC)) {
        outcome = SETUP_SESSION_OUTCOME_ERROR;
        goto teardown;
    }
    deadline = ops->now_ms() + (uint32_t)cfg->budget_sec * 1000u;

    /* Step 2+3: the pure generators (cannot fail — snprintf truncates
       safely in the pathological case of an oversized device_id(), and
       the QR buffer is sized generously enough that this never happens
       on this device; SETUP_SESSION_QR_MAX's own comment), then the
       setup screen — BEFORE start() below, deliberately. Every value
       this screen needs is already in hand, and painting first means
       the full refresh this screen costs happens with the SoftAP still
       down, not beaconing at full TX power throughout it: net_window.c's
       own rendezvous comment (~line 120) documents a panel refresh
       coinciding with a WiFi TX burst browning out the rail on this
       board, and a brownout reset on a no-SSID device is a FAULT reset
       that repaints Setup failed and waits buttons-only for a press, so a
       session that browned out each time would cost a refresh and a
       press per attempt — the cost this ordering avoids. */
    {
        setup_session_screen_info_t info;
        memset(&info, 0, sizeof(info));
        setup_session_make_ap_ssid(info.ap_ssid);
        setup_session_make_ap_password(ops->rand_byte, info.ap_password);
        (void)setup_session_make_qr_payload(info.ap_ssid, info.ap_password, info.qr_payload, sizeof(info.qr_payload));
        info.page_host = SETUP_SESSION_AP_IP;

        ops->render_setup_screen(&info);
        screen_painted = true;

        /* Step 4: bring the SoftAP, httpd, manager and endpoints up. A
           failure here is ERROR, and — because the setup screen is
           already on the glass — still renders the FAILED screen below,
           not none. */
        bool started = ops->start(info.ap_ssid, info.ap_password);
        secure_zero(&info, sizeof(info)); /* ap_password (and its copy inside qr_payload) done being needed */
        if (!started) {
            outcome = SETUP_SESSION_OUTCOME_ERROR;
            goto teardown;
        }
    }

    /* Step 5: loop until an outcome. */
    outcome = run_loop(ops, cfg, deadline);

teardown:
    /* Step 6: unconditional, on every path above — this is what makes
       "teardown always runs" a property of the control flow rather than
       of how carefully each branch remembered to call it. Safe even when
       nothing was ever started (the device layer's contract). The radio
       is down by the time render_end_screen below runs, on every path:
       this call always precedes it. */
    ops->stop();

    /* Step 7: the end-of-session screen. Every outcome gets one except
       the extend_awake refusal above (screen_painted stays false there —
       nothing has painted yet, so there is nothing to correct). Every
       ERROR that follows the setup screen — a start failure, the
       WIFI_SUCCESS-store-failure in run_loop above, or a hard error
       mid-session — gets FAILED: by the time any of those is possible
       the panel is already showing the AP's name, password and QR for a
       session that is about to stop existing, and this is the only
       screen that says so. */
    if (screen_painted) {
        setup_session_end_kind_t kind;
        switch (outcome) {
            case SETUP_SESSION_OUTCOME_WIFI_OK:
                kind = SETUP_SESSION_END_WIFI_SAVED;
                break;
            case SETUP_SESSION_OUTCOME_MQTT_ONLY:
                kind = SETUP_SESSION_END_MQTT_SAVED;
                break;
            case SETUP_SESSION_OUTCOME_TIMEOUT:
                kind = SETUP_SESSION_END_TIMED_OUT;
                break;
            case SETUP_SESSION_OUTCOME_ERROR:
            default:
                kind = SETUP_SESSION_END_FAILED;
                break;
        }
        ops->render_end_screen(kind, cfg->has_wifi_ssid);
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
