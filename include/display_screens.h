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
void display_screens_build_timesup(void);
void display_screens_build_sync_failed(void);
void display_screens_build_charge_me(void);
void display_screens_build_bedtime(void);

#ifdef __cplusplus
}
#endif
