#pragma once
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "chores.h" /* CHORE_MAX, CHORE_NAME_BUF for the checklist block below */
#include "schedule.h"
#include "timer.h"

/* Landscape panel geometry (shared by display.c and the screen builders). */
#define DISP_HOR 296
#define DISP_VER 128

typedef struct {
    /* Today's EFFECTIVE remaining, adjustment included. IDLE reports the
       whole effective allocation (ProductOverview: an idle day shows what
       it has, not 0), which is what lets it exceed allocation_sec below.
       What that draws depends on whether the chore gate is holding part
       of the day, so there is no single answer any more:
         UNGATED, the bar's denominator is allocation_sec, the day's
         default, so an idle day carrying -30 against a 60 min default
         draws the bar HALF full (fill edge x=147), and one carrying a
         grant pins it full with time to spare.
         GATED, display_bar_split saturates the free tranche at the room
         the locked block leaves, so that same -30 day draws the tranche
         FULL (fill_end_px 280) — 30 effective minutes against a 20 min
         tranche has nowhere shorter to go. The deduction is then visible
         on the status row and the counter, not on the bar.
       Both ends are clamped by display_bar_fill_px. */
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
    bool swap_available; /* Button C label (extras exist, state allows swap) */
    /* The raw timer_reload_allowed() gate: the selected timer is
       reloadable and not RUNNING. NOT "B says Reload" — that is narrower,
       because B resumes a PAUSED slot rather than reloading it. Only an
       EXPIRED slot draws the reload label; display_button_b_label() owns
       the narrowing. */
    bool reload_available;
    /* Button B's start/resume leg: false while a Screen Break refuses to
       start this slot (not break_eligible). Same "label shows iff a press
       would work" convention as the two above. */
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

    /* ---- the chore checklist (design §2.4, §4.1) -----------------------
       One snapshot of the gate per paint, because three screens read it —
       the chore list, the Screen bar's locked block and the break
       screen's prompt — and three re-derivations from the store and the
       RTC would be three chances to disagree about the same day. */

    /* The configured list, COPIED rather than pointed at, which is the one
       departure from timer_name / swap_next_name / fw_version above and is
       deliberate. Those borrow storage that outlives the state on its own:
       a timer def in timer_defs.c's file-statics (timer.c holds only a
       pointer to that array), the app descriptor in flash. A chore name
       has no such home — chore_store_load_names() reads NVS into a buffer
       the CALLER owns — so a pointer here could only point at a
       file-static inside app_state.c, and then every snapshot ever taken
       would alias that one buffer. Two states built either side of a list
       edit would silently agree about the list. 63 bytes buys a field that
       means what it says, on a struct that lives on one stack frame for
       the length of a paint.
       Rows at and above chore_count are "", and EVERY row is
       NUL-terminated (chore_store_load_names guarantees that even for a
       stored row that filled all CHORE_NAME_MAX bytes), so a renderer may
       hand any row to str* without first checking the count. It does NOT
       bound rendered width — see CHORE_NAME_MAX in chores.h, which
       measured exactly that trap; the geometric cap is the screen
       builder's. */
    char chore_names[CHORE_MAX][CHORE_NAME_BUF];
    /* How many of those rows are configured, 0..CHORE_MAX, straight from
       the store. THE off switch, and the state every device in the field
       is in today: 0 makes the whole block inert (design row C1) — no
       gate, nothing withheld, nothing to paint — so read this before any
       other chore field. */
    uint8_t chore_count;
    /* Today's acks, bit i = chore i, MASKED to chore_count here. That is
       the one thing this differs from timer_chore_acked() in, and the
       asymmetry is the point: storage keeps the byte raw so a restored
       list can make an old bit meaningful again, while a render view has
       no such duty and gains nothing from a bit it must never draw. The
       masking happens once at this seam instead of asking every renderer
       to remember chores_is_acked(), so a renderer may walk these bits
       directly. */
    uint8_t chore_acked;
    /* Configured chores still un-acked, 0..chore_count. NOT a synonym for
       "the gate is shut": with chore_count == 0 this is also 0 and nothing
       was ever done. The "n of 3" header is (chore_count -
       chore_outstanding) and means nothing until chore_count > 0. */
    uint8_t chore_outstanding;
    /* Seconds of today's SCREEN allocation held back until every chore is
       acked — chores_withheld_sec() over the day's allocation and its
       chore_free tranche.
       Always slot 0's day, whichever timer is selected. allocation_sec
       above follows the selection and this does not, so the two disagree
       while an extra timer is drawn; that is correct, because the gate is
       a statement about the day and not about the timer on screen. The
       locked block is this over the day's allocation, so only Screen's
       paint has any use for it.
       0 collapses three situations — no chores configured, the day already
       released, and the gate off for this day type (chore_free >=
       allocation, which includes an allocation of 0) — so it cannot be
       read as "is the gate armed?"; chore_count and chore_released answer
       that. chores.h states the same three at chores_withheld_sec().
       uint32_t, matching that function's return, so nothing narrows on the
       way to the panel. The int32_t narrowing timer.h flags at its "M2
       CALL SITE" note belongs to the RELEASE path — timer_release_gated()
       — and not to this field. */
    uint32_t chore_withheld_sec;
    /* The day's LATCHED release: the withheld remainder has already been
       granted, so the locked block is gone for the rest of the day
       whatever the acks do afterwards (C8). Deliberately not the same as
       chore_outstanding == 0, which is the tick BEFORE the latch and still
       withholds the whole remainder — that figure is what the release
       owes. */
    bool chore_released;
    /* Which screen to paint (C16), read live from the RTC on every paint.
       A UI value and only that: no timer runs, expires or sleeps
       differently because of it. Not clamped anywhere on the way here (see
       app_mode_t in timer.h), so a painter must treat anything that is not
       APP_MODE_CHORES as Timers rather than switch on it exhaustively. */
    app_mode_t app_mode;
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

/* Button B label: the action a press will take in the given state.
   display_screens.c maps these to LV_SYMBOL_PLAY/PAUSE and the "Reload"
   text (layout code stays LVGL-free). */
typedef enum {
    DISPLAY_BTN_LABEL_NONE = 0,
    DISPLAY_BTN_LABEL_PLAY,
    DISPLAY_BTN_LABEL_PAUSE,
    DISPLAY_BTN_LABEL_RELOAD,
} display_btn_label_t;

/* Pure layout math (display_layout.c) — host-tested */
/* The whole Button B decision, gates included, so that nothing is left
   for the (goldens-only) screen builder to re-decide:
     RUNNING            -> PAUSE  (pausing is never gated)
     IDLE / PAUSED      -> PLAY   iff start_available
     EXPIRED            -> RELOAD iff reload_available
     anything else      -> NONE
   start_available carries a Screen Break's per-slot refusal; note that it
   gates only the start/resume legs, never the pause or the reload. */
display_btn_label_t display_button_b_label(timer_state_t state, bool start_available, bool reload_available);
/* Battery icon bucket 0=empty..4=full; display_screens.c maps to LV_SYMBOL_BATTERY_*. */
int display_battery_icon_level(int pct);
uint16_t display_bar_fill_px(int32_t remaining_sec, uint32_t allocation_sec);
/* The Screen bar split in two by the chore gate (design §4.1), in the
   bar's own 280 px:
     locked_px    the outlined, unfilled block held at the LEFT — the
                  withheld part of the day. 0 = no block at all.
     fill_end_px  the RIGHT edge of the free tranche's fill, measured
                  from the bar's left edge like any other bar value, so
                  the screen builder sets it as the value and lays the
                  block over the part that is not free.
   The free tranche is therefore (fill_end_px - locked_px) px wide and is
   drawn at the SAME pixels-per-second as the block and as an ungated
   bar — the scale does not change at the divider. Both fields come from
   one conversion in display_layout.c; see it for the idle-day saturation,
   the one pixel truncation can cost, and why a sub-pixel withholding
   draws nothing. */
typedef struct {
    uint16_t locked_px;
    uint16_t fill_end_px;
} display_bar_split_t;
display_bar_split_t display_bar_split(int32_t remaining_sec, uint32_t allocation_sec, uint32_t withheld_sec);
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

/* ---- the chore checklist screen (design §2.4) --------------------------- */

/* Which of the three full-screen layouts a paint selects. display.c's
   build_for_state() is a plain switch over this and decides nothing of its
   own, so the precedence below has exactly one home. */
typedef enum {
    DISPLAY_SCREEN_MAIN = 0, /* the ordinary timer screen */
    DISPLAY_SCREEN_BREAK,    /* the inverted Screen Break screen */
    DISPLAY_SCREEN_CHORES,   /* the chore checklist */
} display_screen_t;

/* The precedence rule, and the two reasons it is shaped this way.

   CHORES OUTRANKS BREAK. Design §2.6 puts an "A -> Chores" cell on the
   break screen precisely because "a break is the natural moment to do
   chores", and §4.2 lets chore mode in throughout a break because a BREAK
   is not RUNNING. If the break screen outranked chore mode, that press
   would repaint the break screen and the prompt would lead nowhere. So the
   break screen is what APP_MODE_TIMERS paints during a break — which is
   exactly the screen M2-T6 decorates — and A moves off it to here.

   RUNNING FALLS BACK TO THE TIMER SCREEN rather than painting a checklist.
   §4.2: "you cannot tick off dishes away while the TV clock ticks". The
   same rule arms Button A (button_a_toggle_allowed(), button_actions.h),
   and the two MUST agree: if this painted a checklist in a state where A
   is refused, the panel would show a screen with a "Timers" label on a
   button that would not act — a device stuck on the chore screen for as
   long as the timer runs. Agreeing is what lets the screen builder draw
   that label unconditionally instead of re-deriving the gate a third time.

   THIS FUNCTION IS THE RESTATEMENT button_actions.h forbids, struck
   knowingly: the painter is handed a display_state_t snapshot and cannot
   reach live state or NVS, which is what calling the predicate would mean.
   The deal is one-directional — CHORES here implies A is allowed, never
   the converse — and it is enforced, not merely documented, by
   test_the_chore_screen_is_never_painted_where_button_a_would_be_refused
   in test_button_actions (which compiles display_layout.c for it). A
   REFUSAL REASON ADDED TO A AND NOT TO THIS FUNCTION FAILS THAT CASE;
   M2-T10's device lock is the next one due, so decide there whether the
   painter inherits it rather than leaving the two to drift.
   The stored mode is deliberately NOT reverted here: this is a paint
   decision, and the day's mode byte is the wake flow's (C16, C17).

   chore_count == 0 is row C1's off switch: with no list there is nothing
   to paint, so an unreadable or emptied list degrades to the timer screen
   rather than to a blank checklist. `mode` is not clamped anywhere on the
   way here (app_mode_t in timer.h), hence the == test rather than a
   switch. */
display_screen_t display_screen_for(timer_state_t timer_state, app_mode_t mode, uint8_t chore_count);

/* Whether row idx draws a tick. chores_is_acked()'s rule, restated here
   ONLY because chores.c is not linked into either display test binary
   (both are single-TU builds over display_layout.c).

   THE RE-MASK IS REDUNDANT IN PRODUCTION AND MUST STAY ANYWAY, and the
   reason is not the one that stood here. "Bits at or above the configured
   count are stored RAW" is true of timer_chore_acked(), the RTC byte —
   but this function is never handed that byte. Its argument is
   display_state_t.chore_acked, which app_state.c has already masked
   through chores_is_acked() at the assembly seam (see the field's own
   comment above), so on device a stale bit cannot arrive here in the
   first place.

   What can is a TEST setting the field directly: test_display_render's
   render cases build a display_state_t by hand and bypass that seam
   entirely, and one of them (`st.chore_acked = 0x05` against a two-row
   list) exists precisely to hold this line — it is the stale third tick
   from a longer list, and without the mask it paints. So the masking is
   load bearing for the goldens and belt-and-braces for the field, which
   is the opposite of how it read. Dropping it would leave every
   production path green. */
bool display_chore_row_ticked(uint8_t acked, uint8_t count, uint8_t idx);

/* The header's right-hand count, "n of 3". n is derived from the SAME mask
   the ticks are drawn from rather than from chore_outstanding, so the
   header can never disagree with the rows underneath it. count is the
   configured list length, so a two-chore list reads "1 of 2". */
void display_format_chore_count(char *buf, size_t len, uint8_t acked, uint8_t count);

/* How much of the locked block's label there is room to say. The block is
   `withheld / allocation` of a 280 px bar, so its width is a config
   decision, not a layout one, and on an ordinary day — 60 min with
   chore_free 40 — it is 93 px, which the full sentence does not fit in.
   Clipping it is not an option: "0/3 Chores - 20 min" cut to the width
   reads "0/3 Chores - 2", a complete and plausible statement of a number
   the day does not have. So the label degrades in steps instead, and the
   painter picks the widest step that fits whole (display_screens.c).

   Ordered WIDEST FIRST: the ladder's order is this enum's order, and
   DISPLAY_LOCKED_FORM_COUNT is the rung past the last one, where the
   answer is to draw no label at all. Every rung states only numbers the
   day actually has, so any of them is honest; they differ in how much
   context they can afford. */
typedef enum {
    /* "0/3 Chores - 40 min", or "0/3 Chores to unlock 60 min" when
       chore_free == 0 and the block IS the bar, where the text is all
       there is and has to say what the chores are for. §4.1's form. */
    DISPLAY_LOCKED_FORM_FULL = 0,
    /* "0/3 - 40 min": both figures, the noun dropped. Roughly 46 px
       narrower, and the step that carries the common 93 px block. */
    DISPLAY_LOCKED_FORM_PAIR,
    /* "40 min": the figure the block's own width is a picture of, so the
       last rung that adds anything a reader cannot already see. Below
       this the outline alone is the statement — a bare "0/3" with no noun
       and no unit inside a 60 px sliver reads as damage, not as a word. */
    DISPLAY_LOCKED_FORM_MINUTES,
    DISPLAY_LOCKED_FORM_COUNT
} display_locked_form_t;

/* The label inside the Screen bar's locked block (§4.1), at the given
   rung. Same acks, same masking and same cap on `count` as the checklist
   header above; the minutes are `withheld_sec`, the part of the day the
   chores are holding, NOT what is left to spend. `allocation_sec` picks
   between the two FULL wordings and nothing else, so pass the day the
   withholding was computed against. A rung at or past
   DISPLAY_LOCKED_FORM_COUNT yields "". */
void display_format_locked_block(char *buf, size_t len, display_locked_form_t form, uint8_t acked, uint8_t count,
                                 uint32_t withheld_sec, uint32_t allocation_sec);

/* Whether the screen says "Screen time unlocked" (design §2.4: the mode
   does not bounce you out on the last ack, so the screen has to say that
   something happened).

   An OR of two terms, and both are needed. All-acked covers the last ack
   itself, whose paint can run before the release latch is written. And
   `released` covers what comes after: acks TOGGLE, so un-ticking a chore
   afterwards puts a row back outstanding while the day's screen time stays
   granted (C8, a latch and not a recomputation) — dropping the line there
   would claim the gate had re-shut. With no chores configured neither term
   applies: nothing was ever locked. */
bool display_chore_unlocked(uint8_t acked, uint8_t count, bool released);

/* Invert byte columns [b0..b1] (clamped) of every row in a row-major 1bpp
   framebuffer — builds the inverse pass of the ghost-cleaning double partial. */
void display_fb_invert_byte_cols(uint8_t *fb, int rows, int row_bytes, int b0, int b1);
/* Same, but only rows where fb differs from prev within [b0..b1] — limits
   the cleaning flash to the characters that changed. Returns dirty rows. */
int display_fb_invert_dirty_rows(uint8_t *fb, const uint8_t *prev, int rows, int row_bytes, int b0, int b1);

#ifdef __cplusplus
}
#endif
