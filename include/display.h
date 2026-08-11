#pragma once
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "schedule.h"
#include "timer.h"

/* Landscape panel geometry (shared by display.c and the screen builders). */
#define DISP_HOR 296
#define DISP_VER 128

typedef struct {
    int32_t remaining_sec;
    uint32_t allocation_sec;
    timer_state_t timer_state;
    day_type_t day_type;
    time_t wall_time;
    time_t last_sync_time;
    uint8_t battery_pct; /* 0-100 */
    /* Eye-rest break. A break runs on slot 0 whichever timer is selected,
       so these are valid whenever one is running — not only when
       timer_state == TIMER_BREAK (which means "Screen is selected AND on
       a break", i.e. the break screen is what gets drawn). */
    int32_t break_remaining_sec;
    uint32_t break_duration_sec;
    /* Break running BEHIND another selected timer: the main layout's
       header carries an inverted "BREAK m:ss" chip where Last sync
       normally sits. False when the break screen itself will be drawn. */
    bool break_banner;
    /* Break screen only: the timer Button C would select, for the swap
       hint. NULL = the break has nothing to offer — no extra timers at
       all, or none that are break_eligible, since the hint must promise a
       timer a press would actually start. The break screen then falls
       back to its centred "Timer paused" footer, i.e. it behaves like the
       pre-non-blocking locking break, which is correct. */
    const char *swap_next_name;
    /* Extra timers (v1.3): NULL/"" name = Screen (day-type mode line) */
    const char *timer_name;
    uint16_t completions;
    bool reloadable;
    bool swap_available;   /* Button C label (extras exist, state allows swap) */
    bool reload_available; /* Button B label without ParentTesting */
    /* Button A label: false while a Screen Break refuses to start this
       slot (not break_eligible). Same "label shows iff a press would
       work" convention as the two above. */
    bool start_available;
    bool charge_warn; /* battery <= 15%: Charge Me!!! badge on the bar */
    /* Running firmware version, folded into the battery row's label
       ("[batt] 87%   v1.5.0") — the {58,87} clean band already covers it,
       so no new geometry and no risk of straddling a framebuffer byte.
       NULL or "" renders the row exactly as it did before the field
       existed. Injected as a string: display_screens.c never calls
       esp_app_get_description(), which is what keeps the golden
       deterministic. */
    const char *fw_version;
} display_state_t;

#ifdef __cplusplus
extern "C" {
#endif

void display_init(void);
void display_update(const display_state_t *state);       /* partial-refresh policy */
void display_full_refresh(const display_state_t *state); /* forced full refresh */
void display_timesup(void);                              /* TIME'S UP layout, full refresh */
void display_sync_failed(void);                          /* "No sync - check WiFi" layout */
void display_charge_me(void);                            /* battery lock layout, full refresh */
void display_bedtime(void);                              /* bed-time lock layout, full refresh */
/* Firmware update in progress, full refresh. Version strings are bare
   ("1.5.0"); the screen adds the "v". */
void display_ota(const char *from_version, const char *to_version);

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
/* Coarse duration, "1:30" (h:mm, truncated). Used for the frozen screen
   time on the break screen, where the value cannot change for the whole
   break and the row has three 16 pt items to fit. Clamps at zero. */
void display_format_hm(char *buf, size_t len, int32_t sec);
/* Header chip while a break runs behind another timer: "BREAK 12:34".
   Always M:SS, never H:MM:SS — break durations are bounded (minutes) and
   the chip has no width to spare. Clamps at zero. */
void display_format_break_chip(char *buf, size_t len, int32_t break_remaining_sec);
/* Button C hint on the break screen: the next timer's name, truncated to
   the bottom row's width budget. NULL/"" writes an empty string. The
   swap symbol is prepended by display_screens.c (this file stays LVGL-
   free). */
#define DISPLAY_SWAP_HINT_MAX 8
void display_format_swap_hint(char *buf, size_t len, const char *name);
/* Mode line for an extra timer: "Meditation - 10 min", or with the day's
   completed-run counter ("Meditation (x2) - 10 min") when reloadable.
   Screen (slot 0) keeps the day-type line rendered by display.c. */
void display_format_mode_line(char *buf, size_t len, const char *name, uint16_t completions, bool reloadable,
                              uint32_t allocation_sec);
/* Invert byte columns [b0..b1] (clamped) of every row in a row-major 1bpp
   framebuffer — builds the inverse pass of the ghost-cleaning double partial. */
void display_fb_invert_byte_cols(uint8_t *fb, int rows, int row_bytes, int b0, int b1);
/* Same, but only rows where fb differs from prev within [b0..b1] — limits
   the cleaning flash to the characters that changed. Returns dirty rows. */
int display_fb_invert_dirty_rows(uint8_t *fb, const uint8_t *prev, int rows, int row_bytes, int b0, int b1);

#ifdef __cplusplus
}
#endif
