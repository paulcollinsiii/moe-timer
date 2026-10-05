#pragma once
#include "display.h"

/* LVGL screen builders — pure widget construction on the active screen,
   no ESP/panel dependencies, so the layouts render on the host too
   (test_display_render golden tests). display.c owns flushing the built
   screen to the ssd1680 panel and the partial/full refresh policy. */

#ifdef __cplusplus
extern "C" {
#endif

void display_screens_build_main(const display_state_t *st);
void display_screens_build_break(const display_state_t *st);
/* The chore checklist (design §2.4). Reads chore_names / chore_acked /
   chore_count / chore_released and nothing else about the timer, because
   display_screen_for() has already decided this screen is the right one —
   in particular that no timer is RUNNING, which is what makes the Button A
   "Timers" label honest without re-deriving button_a_toggle_allowed()
   here. Never call it directly: go through display.c's build_for_state(). */
void display_screens_build_chores(const display_state_t *st);
void display_screens_build_timesup(void);
void display_screens_build_sync_failed(void);
void display_screens_build_no_clock(void);
void display_screens_build_charge_me(void);
void display_screens_build_bedtime(void);
/* The config-error lock screen (design 5.3). Both numbers are MINUTES, as
   stored, and are rendered as given — this screen is the report of a pair
   that is already wrong, so nothing here clamps, reorders or "corrects"
   them. Any uint16_t value is renderable; the pair line is capped
   geometrically because the values come from NVS rather than from a
   validated document. */
void display_screens_build_config_error(day_type_t day_type, uint16_t chore_free_min, uint16_t alloc_min);
/* Firmware update in progress. Version strings are bare ("1.5.0"); the
   screen prefixes the "v" at draw time, so callers pass — and ota_policy
   keeps comparing — the unprefixed string. NULL or "" renders that line
   as "Current: v?" / "Installing v?"; anything longer than the screen's
   display budget is truncated. */
void display_screens_build_ota(const char *from_version, const char *to_version);

/* ---- WiFi + MQTT provisioning plan: the setup screens ------------------- */

/* The QR at the left (display_screens.c owns the pixel-scale/quiet-zone
   arithmetic that fits it on this panel; qr_render.h owns the version
   this module asks qrcodegen for), and on the right: the instruction
   "scan to join, then open" and the address to open (`page_host`), then
   the AP name and password as text for a phone that will not scan.
   `qr_payload` is handed straight to qr_render_encode(); if it does not
   fit QR_RENDER_MAX_VERSION at ECC LOW, no QR is drawn and the text lines
   render unchanged — they are the complete manual-entry path, not merely
   a note that one exists. ap_ssid longer than the column budget (up to
   SETUP_SESSION_AP_SSID_MAX-1, the input buffer's own width) is clipped
   rather than overflowing into the QR block or off the panel's right
   edge. */
void display_screens_build_setup(const char *ap_ssid, const char *ap_password, const char *qr_payload,
                                 const char *page_host);
/* Shown while the BOOT hold is armed (SETUP_TRIGGER_BOOT_HOLD_IDLE's
   threshold reached) — releasing now enters setup. */
void display_screens_build_setup_release(void);
/* The end of a session, whichever way it ended — WiFi/MQTT saved, timed
   out, or failed. The retry hint on the latter two is read from
   SETUP_TRIGGER_BOOT_HOLD_MS and CONFIG_MAGTAG_BOOT_WAKES (setup_trigger.h
   and this file's own boot_wakes_enabled()) rather than restated, so a
   Kconfig change cannot leave this screen quoting a stale promise. */
void display_screens_build_setup_end(display_setup_end_t kind, bool has_wifi_ssid);

#ifdef __cplusplus
}
#endif
