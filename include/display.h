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
    /* Today's EFFECTIVE remaining, adjustment included. IDLE reports the
       whole effective allocation (ProductOverview: an idle day shows what
       it has, not 0), which is what lets it exceed allocation_sec below.
       That is a FULL bar only on an unadjusted day: the bar's denominator
       is allocation_sec, the day's default, so an idle day carrying -30
       against a 60 min default draws the bar HALF full, and one carrying
       a grant pins it full with time to spare. Both ends are clamped by
       display_bar_fill_px. */
    int32_t remaining_sec;
    /* The day's DEFAULT — the scheduled figure for the day type, or an
       extra timer's configured duration. NOT today's effective limit:
       adjust_sec carries what was applied on top, and the status line
       renders the two separately so a 60-minute weekday with -30 applied
       still says what a weekday is worth. Also the progress bar's
       denominator, which is why a grant simply pins the bar full
       (display_bar_fill_px clamps both ends) rather than rescaling the
       day under the reader.
       Read LIVE from config/schedule on every paint, so it can move under
       a timer that already started — which is precisely why adjust_sec
       below cannot be a subtraction against it. */
    uint32_t allocation_sec;
    /* Signed seconds of adjustment applied to this slot TODAY: HA's
       "Screen adjust (min) today", a cmd-topic grant, or both, summed.
       0 = the day is running on its default, and the status line then
       renders exactly as it did before this field existed.
       Comes from the timer's own tracked total (timer_slot_adjust_today),
       NOT from (effective limit - allocation_sec): those two are read at
       different times, so that subtraction reported a phantom adjustment
       whenever the day's default moved mid-run.
       Clamped for display so allocation_sec + adjust_sec is never
       negative — a -120 min deduction against a 60 min day arrives here
       as -60. The timer keeps the unclamped record. */
    int32_t adjust_sec;
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
    bool reload_available; /* Button B label (selected timer reloadable) */
    /* Button A label: false while a Screen Break refuses to start this
       slot (not break_eligible). Same "label shows iff a press would
       work" convention as the two above. */
    bool start_available;
    bool charge_warn; /* battery <= 15%: Charge Me!!! badge on the bar */
    /* Running firmware version, folded into the battery row's label
       ("[batt] 87%   v1.5.0") rather than given a label of its own: it
       adds no rows, so display.c's clean-band table is untouched. NULL or
       "" renders the row exactly as it did before the field existed.
       Rendered bare — the "v" is added at draw time, so nothing that
       compares versions ever sees it — and truncated to a display budget,
       since a manifest may publish up to 31 characters. Injected as a
       string: display_screens.c never calls esp_app_get_description(),
       which is what keeps the goldens deterministic. */
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
/* Firmware version as rendered on the main screen's battery row and on
   both lines of the update screen. NULL/"" writes an empty string; the
   "v" prefix is added by the caller at draw time, so what round-trips
   through ota_policy stays unprefixed.

   The budget counts BYTES, not codepoints: a multi-byte UTF-8 sequence
   straddling the cut would be truncated mid-character. Version strings
   are ASCII in practice and the length is enforced upstream
   (OTA_VERSION_MAX), so this is documented rather than handled.

   12 is measured, not guessed. A manifest may publish up to 31
   characters, and the digit curve is the realistic worst case for a
   version: at 12 digits the battery row ends at x=164 against the 28 pt
   remaining time starting at x=180, and "Installing v" renders 243 px on
   a 296 px panel. Note a character budget CANNOT bound rendered width on
   its own — a 12 pt digit advances ~8 px but 'W' ~14, so an all-'W'
   version overstrikes at 8 characters — which is why the labels also
   carry a geometric cap in display_screens.c. */
#define DISPLAY_VERSION_MAX 12
void display_format_version(char *buf, size_t len, const char *version);
/* Mode line for an extra timer: "Meditation - 10 min", or with the day's
   completed-run counter ("Meditation (x2) - 10 min") when reloadable.
   Screen (slot 0) uses display_format_day_line below.

   allocation_sec is the timer's CONFIGURED duration and adjust_sec the
   signed seconds a grant has moved it by, rendered as a suffix:
   "Piano - 15 min (+10 min today)". adjust_sec 0 renders nothing extra.
   Extras carry it for the same reason Screen does — a cmd-topic grant can
   bank on any slot, and a line that reports the configured duration while
   the clock beside it counts something else is the same misreport. */
void display_format_mode_line(char *buf, size_t len, const char *name, uint16_t completions, bool reloadable,
                              uint32_t allocation_sec, int32_t adjust_sec);
/* Status line for Screen (slot 0): "Weekday - 60 min", plus today's
   adjustment when there is one — "Weekday - 60 min (-30 min today)".

   allocation_sec is the DAY'S DEFAULT, never the adjusted limit: the
   point of the split is that the family can still read what a weekday is
   worth on a day 30 minutes were taken off it. day_type is the already
   resolved label (display_screens.c owns that mapping; this file stays
   free of anything but string math). */
void display_format_day_line(char *buf, size_t len, const char *day_type, uint32_t allocation_sec, int32_t adjust_sec);
/* Invert byte columns [b0..b1] (clamped) of every row in a row-major 1bpp
   framebuffer — builds the inverse pass of the ghost-cleaning double partial. */
void display_fb_invert_byte_cols(uint8_t *fb, int rows, int row_bytes, int b0, int b1);
/* Same, but only rows where fb differs from prev within [b0..b1] — limits
   the cleaning flash to the characters that changed. Returns dirty rows. */
int display_fb_invert_dirty_rows(uint8_t *fb, const uint8_t *prev, int rows, int row_bytes, int b0, int b1);

#ifdef __cplusplus
}
#endif
