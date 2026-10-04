#pragma once
#include "setup_session.h" /* setup_session_screen_info_t: the shape render_setup_screen is called with */

/* WiFi + MQTT provisioning plan: thin adapters wiring the setup session's
   setup_session_ops_t render fields to the setup screens in display.c.
   Each function here matches one ops-table field's signature exactly, so
   a caller can assign it straight into the table literal:
     .render_setup_screen = setup_screens_render_setup_screen
     .render_end_screen   = setup_screens_render_end_screen

   Device-only — each calls into display.c, which needs LVGL, the ssd1680
   driver and FreeRTOS, so this module is not itself host-tested, the same
   as main/setup_session_idf.c. The screens it reaches (display_screens.c)
   are: display goldens cover the layout, this file only has to forward
   the call correctly — including mapping setup_session_end_kind_t onto
   display.h's own display_setup_end_t, which exists only so display.c
   stays free of setup_session.h (that header's own comment on the ops
   table's render fields says why). */

#ifdef __cplusplus
extern "C" {
#endif

void setup_screens_render_setup_screen(const setup_session_screen_info_t *info);
void setup_screens_render_end_screen(setup_session_end_kind_t kind, bool has_wifi_ssid);

/* The "release to enter setup" screen, shown while the BOOT hold is
   armed. NOT part of setup_session_ops_t — it paints before any session
   exists, while setup_trigger.c's hold tracker is still running — so the
   wake flow calls this directly rather than through the ops table. */
void setup_screens_render_release_hint(void);

#ifdef __cplusplus
}
#endif
