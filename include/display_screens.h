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
void display_screens_build_charge_me(void);
void display_screens_build_bedtime(void);
/* Firmware update in progress. Version strings are bare ("1.5.0"); the
   screen prefixes the "v" at draw time, so callers pass — and ota_policy
   keeps comparing — the unprefixed string. NULL or "" renders that line
   as "Current: v?" / "Installing v?"; anything longer than the screen's
   display budget is truncated. */
void display_screens_build_ota(const char *from_version, const char *to_version);

#ifdef __cplusplus
}
#endif
