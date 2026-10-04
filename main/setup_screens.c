/* Thin adapters wiring the setup session's setup_session_ops_t render
   fields to the setup screens in display.c. See include/setup_screens.h. Reachable but
   not yet called from anywhere — the wake flow composes the real ops
   table and calls setup_screens_render_release_hint() for the BOOT-hold
   screen. */
#include "setup_screens.h"

#include "display.h"

void setup_screens_render_setup_screen(const setup_session_screen_info_t *info) {
    display_setup(info->ap_ssid, info->ap_password, info->qr_payload, info->form_url);
}

void setup_screens_render_complete_screen(void) {
    display_setup_complete();
}

void setup_screens_render_timeout_screen(void) {
    display_setup_timeout();
}

void setup_screens_render_release_hint(void) {
    display_setup_release();
}
