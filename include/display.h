#pragma once
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "schedule.h"
#include "timer.h"

typedef struct {
    int32_t remaining_sec;
    uint32_t allocation_sec;
    timer_state_t timer_state;
    day_type_t day_type;
    time_t wall_time;
    time_t last_sync_time;
    uint8_t battery_pct; /* 0-100 */
} display_state_t;

#ifdef __cplusplus
extern "C" {
#endif

void display_init(void);
void display_update(const display_state_t *state);       /* partial-refresh policy */
void display_full_refresh(const display_state_t *state); /* forced full refresh */
void display_timesup(void);                              /* TIME'S UP layout, full refresh */
void display_sync_failed(void);                          /* "No sync - check WiFi" layout */

/* Button A label: the action a press will take in the given state.
   display.c maps these to LV_SYMBOL_PLAY/PAUSE (layout code stays LVGL-free). */
typedef enum {
    DISPLAY_BTN_LABEL_NONE = 0,
    DISPLAY_BTN_LABEL_PLAY,
    DISPLAY_BTN_LABEL_PAUSE,
} display_btn_label_t;

/* Pure layout math (display_layout.c) — host-tested */
display_btn_label_t display_button_a_label(timer_state_t state);
/* Battery icon bucket 0=empty..4=full; display.c maps to LV_SYMBOL_BATTERY_*. */
int display_battery_icon_level(int pct);
uint16_t display_bar_fill_px(int32_t remaining_sec, uint32_t allocation_sec);
void display_format_remaining(char *buf, size_t len, int32_t remaining_sec);
/* Invert byte columns [b0..b1] (clamped) of every row in a row-major 1bpp
   framebuffer — builds the inverse pass of the ghost-cleaning double partial. */
void display_fb_invert_byte_cols(uint8_t *fb, int rows, int row_bytes, int b0, int b1);
/* Same, but only rows where fb differs from prev within [b0..b1] — limits
   the cleaning flash to the characters that changed. Returns dirty rows. */
int display_fb_invert_dirty_rows(uint8_t *fb, const uint8_t *prev, int rows, int row_bytes, int b0, int b1);

#ifdef __cplusplus
}
#endif
