/* Pure layout math — no LVGL/ESP dependencies; host-tested via ctest. */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "display.h"

#define BAR_FILL_MAX_PX 280u

uint16_t display_bar_fill_px(int32_t remaining_sec, uint32_t allocation_sec) {
    if (allocation_sec == 0 || remaining_sec <= 0)
        return 0;
    if ((uint32_t)remaining_sec >= allocation_sec)
        return BAR_FILL_MAX_PX;
    return (uint16_t)((uint32_t)remaining_sec * BAR_FILL_MAX_PX / allocation_sec);
}

int display_battery_icon_level(int pct) {
    if (pct <= 10)
        return 0;
    if (pct <= 35)
        return 1;
    if (pct <= 60)
        return 2;
    if (pct <= 85)
        return 3;
    return 4;
}

/* One label out, every gate folded in — see the header for the rule. The
   alternative (state here, gates in the screen builder) put a three-input
   decision in the one module with no unit test, only goldens. */
display_btn_label_t display_button_b_label(timer_state_t state, bool start_available, bool reload_available) {
    switch (state) {
        case TIMER_RUNNING:
            /* A RUNNING slot during a break is break_eligible by
               construction (I6), so start_available is not consulted —
               pausing is never refused. */
            return DISPLAY_BTN_LABEL_PAUSE;
        case TIMER_IDLE:
        case TIMER_PAUSED:
            /* start_available carries a Screen Break's per-slot refusal:
               a non-eligible slot draws no glyph, because a press would
               do nothing. reload_available may well be true here (the
               slot is reloadable and not RUNNING), but B resumes a paused
               slot — it does not reload it. */
            return start_available ? DISPLAY_BTN_LABEL_PLAY : DISPLAY_BTN_LABEL_NONE;
        case TIMER_EXPIRED:
            /* Where B has no other job: reload if the slot allows it.
               Screen (slot 0) has no def and is never reloadable, so a
               kid still cannot reset their own screen timer. */
            return reload_available ? DISPLAY_BTN_LABEL_RELOAD : DISPLAY_BTN_LABEL_NONE;
        default:
            return DISPLAY_BTN_LABEL_NONE; /* BREAK: no early resume */
    }
}

void display_fb_invert_byte_cols(uint8_t *fb, int rows, int row_bytes, int b0, int b1) {
    if (b0 < 0)
        b0 = 0;
    if (b1 >= row_bytes)
        b1 = row_bytes - 1;
    for (int r = 0; r < rows; r++) {
        uint8_t *row = fb + (size_t)r * row_bytes;
        for (int b = b0; b <= b1; b++)
            row[b] = (uint8_t)~row[b];
    }
}

int display_fb_invert_dirty_rows(uint8_t *fb, const uint8_t *prev, int rows, int row_bytes, int b0, int b1) {
    if (b0 < 0)
        b0 = 0;
    if (b1 >= row_bytes)
        b1 = row_bytes - 1;
    int dirty = 0;
    for (int r = 0; r < rows; r++) {
        uint8_t *row = fb + (size_t)r * row_bytes;
        const uint8_t *prow = prev + (size_t)r * row_bytes;
        int changed = 0;
        for (int b = b0; b <= b1 && !changed; b++)
            changed = (row[b] != prow[b]);
        if (!changed)
            continue;
        display_fb_invert_byte_cols(row, 1, row_bytes, b0, b1);
        dirty++;
    }
    return dirty;
}

/* " (-30 min today)", appended to a status line that already carries the
   day's DEFAULT allocation. A suffix rather than a field inside each
   format string because the two lines below differ only in their head,
   and because it has to vanish COMPLETELY when nothing was adjusted: an
   unadjusted day must render byte-identically to the line that existed
   before the adjustment was ever split out (the render goldens pin that).

   The sign is explicit. The control it reports is signed — "15 min today"
   would not say which way the day moved — so a grant reads "+15" and a
   deduction "-30".

   Sub-minute adjustments round away rather than printing "(+0 min
   today)". Both sources work in whole minutes (HA's number and the cmd
   topic each multiply by 60), so this is a floor on nonsense, not a
   rounding policy.

   The division is what makes negating the magnitude safe: adjust_sec / 60
   is evaluated in int32_t and so cannot exceed 35791394 in magnitude, and
   -x is only undefined at INT32_MIN. (`long` here is for the %ld, not for
   the negation.)

   len == 0 is a real call shape — snprintf accepts it and writes nothing,
   which is what BOTH formatters did before this suffix existed. strlen()
   on a buffer nothing has written to would read off the end of whatever
   the caller owns, so the zero case returns before it. Past that, the
   snprintf above has always NUL-terminated within len, so `used` is at
   most len - 1 and the snprintf below gets a length of at least 1: no
   further guard is possible, and one that looked like protection would
   only be dead code claiming otherwise. */
static void append_adjust(char *buf, size_t len, int32_t adjust_sec) {
    long min = (long)(adjust_sec / 60);
    if (min == 0 || len == 0)
        return;
    size_t used = strlen(buf);
    snprintf(buf + used, len - used, " (%c%ld min today)", (min < 0) ? '-' : '+', (min < 0) ? -min : min);
}

void display_format_mode_line(char *buf, size_t len, const char *name, uint16_t completions, bool reloadable,
                              uint32_t allocation_sec, int32_t adjust_sec) {
    /* Completions only surface on reloadable timers — a depleted
       non-reloadable timer just shows its empty bar until rollover. */
    if (reloadable && completions > 0) {
        snprintf(buf, len, "%s (x%u) - %u min", name, (unsigned)completions, (unsigned)(allocation_sec / 60));
    } else {
        snprintf(buf, len, "%s - %u min", name, (unsigned)(allocation_sec / 60));
    }
    append_adjust(buf, len, adjust_sec);
}

void display_format_day_line(char *buf, size_t len, const char *day_type, uint32_t allocation_sec, int32_t adjust_sec) {
    snprintf(buf, len, "%s - %u min", day_type, (unsigned)(allocation_sec / 60));
    append_adjust(buf, len, adjust_sec);
}

/* ---- the chore checklist screen (design §2.4) --------------------------- */

/* The order of the three tests IS the precedence — see the header for why
   chores outrank the break screen and why RUNNING falls back rather than
   painting a checklist nobody could leave. */
display_screen_t display_screen_for(timer_state_t timer_state, app_mode_t mode, uint8_t chore_count) {
    if (mode == APP_MODE_CHORES && chore_count > 0 && timer_state != TIMER_RUNNING)
        return DISPLAY_SCREEN_CHORES;
    if (timer_state == TIMER_BREAK)
        return DISPLAY_SCREEN_BREAK;
    return DISPLAY_SCREEN_MAIN;
}

bool display_chore_row_ticked(uint8_t acked, uint8_t count, uint8_t idx) {
    /* CHORE_MAX bounds the shift as well as the count: `count` arrives
       from a store that clamps it, but a wider value must still never
       reach a shift wider than the mask. */
    if (idx >= count || idx >= CHORE_MAX)
        return false;
    return ((acked >> idx) & 1u) != 0u;
}

/* Configured rows carrying a tick. Bounded by display_chore_row_ticked, so
   there is one masking rule on this screen and not two. */
static uint8_t chore_acked_count(uint8_t acked, uint8_t count) {
    uint8_t n = 0;
    for (uint8_t i = 0; i < CHORE_MAX; i++) {
        if (display_chore_row_ticked(acked, count, i))
            n++;
    }
    return n;
}

void display_format_chore_count(char *buf, size_t len, uint8_t acked, uint8_t count) {
    unsigned cfg = (count > CHORE_MAX) ? CHORE_MAX : count;
    snprintf(buf, len, "%u of %u", (unsigned)chore_acked_count(acked, count), cfg);
}

bool display_chore_unlocked(uint8_t acked, uint8_t count, bool released) {
    if (count == 0)
        return false; /* C1: nothing was ever locked */
    if (released)
        return true;
    return chore_acked_count(acked, count) == ((count > CHORE_MAX) ? CHORE_MAX : count);
}

void display_format_hm(char *buf, size_t len, int32_t sec) {
    if (sec < 0)
        sec = 0;
    snprintf(buf, len, "%ld:%02ld", (long)(sec / 3600), (long)((sec / 60) % 60));
}

void display_format_break_chip(char *buf, size_t len, int32_t break_remaining_sec) {
    if (break_remaining_sec < 0)
        break_remaining_sec = 0;
    /* Minutes never roll into hours: the configured break duration is a
       small number of minutes, and "BREAK 01:02:34" would not fit beside
       the header date. */
    snprintf(buf, len, "BREAK %ld:%02ld", (long)(break_remaining_sec / 60), (long)(break_remaining_sec % 60));
}

/* Copy at most `max` bytes, never more than the buffer holds, always
   NUL-terminating; NULL in yields "" out so callers can test buf[0]
   rather than the pointer. Shared by the two width-budgeted strings
   below — they differ only in which constant seeds the budget. Bytes,
   not codepoints (see the header note on DISPLAY_VERSION_MAX). */
static void copy_bounded(char *buf, size_t len, const char *src, size_t max) {
    if (len == 0)
        return;
    if (src == NULL) {
        buf[0] = '\0';
        return;
    }
    if (max > len - 1)
        max = len - 1;
    size_t i = 0;
    for (; i < max && src[i] != '\0'; i++)
        buf[i] = src[i];
    buf[i] = '\0';
}

void display_format_swap_hint(char *buf, size_t len, const char *name) {
    copy_bounded(buf, len, name, DISPLAY_SWAP_HINT_MAX);
}

void display_format_version(char *buf, size_t len, const char *version) {
    copy_bounded(buf, len, version, DISPLAY_VERSION_MAX);
}

void display_format_remaining(char *buf, size_t len, int32_t remaining_sec) {
    if (remaining_sec < 0)
        remaining_sec = 0;
    snprintf(buf, len, "%02ld:%02ld:%02ld", (long)(remaining_sec / 3600), (long)((remaining_sec / 60) % 60),
             (long)(remaining_sec % 60));
}
