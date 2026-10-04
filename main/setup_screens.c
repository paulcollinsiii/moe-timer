/* Thin adapters wiring the setup session's setup_session_ops_t render
   fields to the setup screens in display.c. See include/setup_screens.h. Reachable but
   not yet called from anywhere — the wake flow composes the real ops
   table and calls setup_screens_render_release_hint() for the BOOT-hold
   screen. */
#include "setup_screens.h"

#include "display.h"

void setup_screens_render_setup_screen(const setup_session_screen_info_t *info) {
    display_setup(info->ap_ssid, info->ap_password, info->qr_payload, SETUP_SESSION_QR_USERNAME, info->form_url);
}

/* The only place setup_session_end_kind_t and display_setup_end_t meet —
   both enumerate the same four cases in the same order, by construction,
   so this is a 1:1 relabelling rather than a decision of its own. */
static display_setup_end_t to_display_kind(setup_session_end_kind_t kind) {
    switch (kind) {
        case SETUP_SESSION_END_WIFI_SAVED:
            return DISPLAY_SETUP_END_WIFI_SAVED;
        case SETUP_SESSION_END_MQTT_SAVED:
            return DISPLAY_SETUP_END_MQTT_SAVED;
        case SETUP_SESSION_END_TIMED_OUT:
            return DISPLAY_SETUP_END_TIMED_OUT;
        case SETUP_SESSION_END_FAILED:
        default:
            return DISPLAY_SETUP_END_FAILED;
    }
}

void setup_screens_render_end_screen(setup_session_end_kind_t kind, bool has_wifi_ssid) {
    display_setup_end(to_display_kind(kind), has_wifi_ssid);
}

void setup_screens_render_release_hint(void) {
    display_setup_release();
}
