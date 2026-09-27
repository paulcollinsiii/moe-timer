/* Pure layout math — no LVGL/ESP dependencies; host-tested via ctest. */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "display.h"

#define BAR_FILL_MAX_PX 280u

/* Seconds -> bar pixels. THE conversion this bar has, and the only one:
   the draining fill and the chore gate's locked block both come through
   here, so "pixels-per-second stays uniform across the whole bar" (§4.1)
   is structural rather than a comment — there is no second expression
   for it to drift away from. A free tranche given its own denominator
   would still look like a bar and would silently mean something else.

   Saturates at the full width so neither end can overrun the bar object,
   and 0 for a zero allocation (a day with no screen time has no scale to
   draw against). */
static uint16_t bar_px(uint32_t sec, uint32_t allocation_sec) {
    if (allocation_sec == 0 || sec == 0)
        return 0;
    if (sec >= allocation_sec)
        return BAR_FILL_MAX_PX;
    return (uint16_t)(sec * BAR_FILL_MAX_PX / allocation_sec);
}

uint16_t display_bar_fill_px(int32_t remaining_sec, uint32_t allocation_sec) {
    return (remaining_sec <= 0) ? 0 : bar_px((uint32_t)remaining_sec, allocation_sec);
}

/* The bar split in two by the chore gate (§4.1): a locked block held at
   the LEFT and the free tranche draining to its right. Both measured in
   the same 280 px at the same rate, so the boundary between them is a
   divider and not a change of scale.

   `withheld_sec` is chores_withheld_sec() for the DAY. It is already 0
   for every ungated case — no chores configured, the gate off for this
   day type, the day released — so a caller has no second condition to
   remember, and with 0 the result is exactly the bar that existed before
   the gate did: locked_px 0 and fill_end_px == display_bar_fill_px().
   `allocation_sec` must be the SAME day the withholding was computed
   against; the screen builder suppresses the block when an extra timer's
   duration is on this axis instead.

   The fill SATURATES at the divider rather than drawing over the block,
   and that is what makes a gated IDLE day honest. app_state.c reports the
   day's whole effective allocation as `remaining` while it is idle
   (ProductOverview: an idle day shows what it has), so the unsplit fill
   would be the full 280 px and would claim the locked part is available
   to spend now. Saturated, the panel reads "40 min locked behind the
   chores, 20 min free and full", which is the point of carrying the split
   on the bar instead of shrinking the number.

   The two sum to the day's DEFAULT — `allocation_sec`, this function's
   own denominator — and NOT to whatever the big counter says. They
   coincide only while adjust_sec is 0. Bank -30 on the same 60/20 day
   and the counter reads 30 min while the bar still shows 40 locked and a
   full 20 min tranche: 30 effective minutes against a 20 min tranche
   saturate, so the deduction lands on the counter and the status row and
   is not visible here at all. That is deliberate — the block is a
   statement about how the DAY is split, not about what is left — but it
   is the reason the bar alone does not reconstruct the counter.

   And it is NOT the whole story, so do not read the saturation as a
   licence to leave the mechanism alone. On that same gated 60/20 day
   with -30 banked there are THREE readings of one day and no two agree:
   the bar says 60 (40 locked + a full 20 min tranche), the counter says
   00:30:00, and pressing B starts 0 s — button_b_start_allocation()
   returns alloc - withheld = 1200 s, then timer_start() folds in the
   banked bonus_sec of -1800 and clamps at zero. The third is the one
   nobody can see coming, and none of the three is fixable here: they are
   the S1/S2 root cause (the gate is a capped allocation FROZEN at
   timer_start, with the remainder added back on release), tracked in
   m2-notes.md under "S1 + S2 ARE ONE PROBLEM". This split makes the BAR
   honest about the day; it does not make the mechanism coherent.

   Rounding: both ends truncate, so a completely full free tranche can
   land one pixel short of the bar's end (186 + 93 = 279). A pixel of
   white at the far end is cheaper than either half lying about its own
   length, and the test pins the gap at no more than one.

   And a withholding worth less than a pixel draws no block at all — five
   minutes of a 24 h day. Deliberate: a minimum width would buy that
   sliver by breaking the one invariant this split exists to keep. */
display_bar_split_t display_bar_split(int32_t remaining_sec, uint32_t allocation_sec, uint32_t withheld_sec) {
    display_bar_split_t split = {0, 0};
    split.locked_px = bar_px(withheld_sec, allocation_sec);
    uint16_t room = (uint16_t)(BAR_FILL_MAX_PX - split.locked_px);
    uint16_t free_px = display_bar_fill_px(remaining_sec, allocation_sec);
    if (free_px > room)
        free_px = room;
    split.fill_end_px = (uint16_t)(split.locked_px + free_px);
    return split;
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

/* The break screen's prompt (§2.6). "Chores 2 of 3" while there is
   anything to do, "Chores" and a tick once there is not.

   THE SAME acked/count PAIR AS THE HEADER ABOVE, and the same derived
   numerator, because button A sends the reader from this line to that
   header: two screens carrying a fraction that looks alike must not mean
   opposite things. So this counts what is DONE. chore_outstanding would
   have been the shorter route to a figure and it is the wrong figure —
   every value it produced would still be in range, which is why the
   choice is pinned by exact strings in both display suites rather than
   left to a pixel compare.

   "n of m" RATHER THAN "(n/m)", and that is a measurement and not a
   preference. '(' ')' and '/' all ascend two rows above this font's cap
   height and descend two or three below its baseline: "Chores (2/3)"
   paints 17 ink rows where "Chores 2 of 3" paints 12, and the band the
   break screen opens for this line is 12 (see display_screens.c, which
   has the arithmetic). Letters and digits alone stay inside it. The
   wording it lands on is also the checklist header's own, which is what
   the two screens had to agree on in the first place.

   THE TICK IS LAST. Leading, it would shove "Chores" rightwards the
   moment the list was finished, and a word that moves on e-ink reads as
   churn; trailing, the word stays on the left margin in both states.
   There is no bracket-and-cross pair for the two states because the
   symbol font has no box glyph, and the device's vocabulary is already
   the checklist's: a tick means done and nothing means not done.

   THE TICK IS THE LIST'S STATE AND NOT THE GATE'S. display_chore_unlocked()
   disagrees with "every row ticked" after C8's toggle-back-off, where the
   day stays released and a row is outstanding again; this line is about
   the list, so it follows the rows. The count > 0 guard is what stops
   0 == 0 ticking an empty list — a state this screen never renders, the
   line being drawn only when chore_count > 0, so nothing downstream would
   catch it.

   Both halves clamp to CHORE_MAX for the same reason the checklist header
   does: that screen has CHORE_MAX rows on it and a larger figure here
   could not be reconciled with what the reader finds there. The numerator
   is already bounded by display_chore_row_ticked's own cap, so the clamp
   below is the denominator's. */
void display_format_chore_prompt(char *buf, size_t len, uint8_t acked, uint8_t count) {
    unsigned cfg = (count > CHORE_MAX) ? CHORE_MAX : count;
    unsigned done = chore_acked_count(acked, count);
    if (count > 0 && done == cfg) {
        snprintf(buf, len, "Chores " DISPLAY_CHORE_TICK);
    } else {
        snprintf(buf, len, "Chores %u of %u", done, cfg);
    }
}

/* The label the Screen bar's locked block holds (§4.1). Same acked/count
   pair as the checklist header and the same masking, but "0/3" rather
   than "0 of 3": this one shares a 24 px bar with a figure in minutes and
   a draining fill, where the checklist header has a row of its own.

   FULL has two wordings, because a fully gated day (chore_free == 0) has
   no draining region beside the text to explain it — the block is the
   whole bar, so the text is the only thing on it and has to say what the
   chores are for. Partially gated, the bar to the right already says it.

   The narrower rungs have one wording each. A fully gated day can only
   reach them if 272 px will not hold "…to unlock…", which no list and no
   allocation can manage today; they are written to be true there anyway
   rather than to be unreachable, because "0/3 - 60 min" on a block that
   is the whole bar still states two numbers the day has.

   " - " and not the design's "·": the built-in Montserrat faces carry
   ASCII, and display_format_mode_line already spells the same separator
   this way on the row below. A middle dot would render as a missing
   glyph on the one screen the family reads every day.

   Minutes truncate, and cannot mislead: both allocation and chore_free
   are configured in whole minutes, so withheld_sec is a whole number of
   them. That is what makes every rung safe to shorten — the figure is
   exact at any width, so dropping words around it drops context and
   never precision. */
void display_format_locked_block(char *buf, size_t len, display_locked_form_t form, uint8_t acked, uint8_t count,
                                 uint32_t withheld_sec, uint32_t allocation_sec) {
    unsigned cfg = (count > CHORE_MAX) ? CHORE_MAX : count;
    unsigned done = (unsigned)chore_acked_count(acked, count);
    unsigned min = (unsigned)(withheld_sec / 60);
    switch (form) {
        case DISPLAY_LOCKED_FORM_FULL:
            if (allocation_sec > 0 && withheld_sec >= allocation_sec) {
                snprintf(buf, len, "%u/%u Chores to unlock %u min", done, cfg, min);
            } else {
                snprintf(buf, len, "%u/%u Chores - %u min", done, cfg, min);
            }
            break;
        case DISPLAY_LOCKED_FORM_PAIR:
            snprintf(buf, len, "%u/%u - %u min", done, cfg, min);
            break;
        case DISPLAY_LOCKED_FORM_MINUTES:
            snprintf(buf, len, "%u min", min);
            break;
        default:
            if (len > 0)
                buf[0] = '\0';
            break;
    }
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

/* ---- refresh cadence and ghost cleaning (design §2.5) ------------------- */

display_refresh_plan_t display_refresh_plan(uint8_t *partial_count) {
    display_refresh_plan_t plan = {.full = false, .ghost_clean = false};

    /* `>=` and not `==` so a counter that came back from RTC memory above
       the threshold still lands here instead of wrapping. */
    (*partial_count)++;
    if (*partial_count >= DISPLAY_FULL_REFRESH_EVERY_N) {
        *partial_count = 0;
        plan.full = true;
        return plan; /* a full refresh drives every pixel; no double pass */
    }

    /* EVERY PARTIAL CLEANS, the checklist included (M2-T15).

       M2-T9 exempted a CHORES -> CHORES partial from the cleaning pass, on
       design §2.5's argument that an ack is a tiny diff and the pass is most
       of the latency. The board said otherwise: with the exemption, ticks
       ticked and unticked across a session left their residue on the glass
       and the "X of 3 done" header ghosted; with it undone (the M2-T14
       A/B, 2026-09-23) both painted clean.

       WHY THE PASS FIXES IT, since the argument that it could not was
       made and was wrong. The pass does NOT drive a changing pixel any
       harder: a pixel that changes is driven once, old -> new, with or
       without it. What it adds is the UNCHANGED pixels. display_fb_invert_-
       dirty_rows() inverts the WHOLE of a dirty (column, band) segment —
       one landscape column across one band's rows, whenever any pixel of
       it changed — so the pixels above and below a changed stroke, whose
       colour is the same in both frames, are driven away and back. Those
       are the pixels that accumulate residue over a run of partials, and
       nothing else on the partial path ever touches them.

       THE LATENCY IT COSTS moved channel rather than disappeared: the
       NeoPixels are the fast acknowledgement, and wake_flow's quiet window
       (CONFIG_MAGTAG_CHORE_PAINT_QUIET_MS) lets a gesture settle before the
       panel paints it once, so the pass is paid once per gesture rather than
       once per press.

       NO PREVIOUS-SCREEN INPUT ANY MORE. The exemption was a transition
       test and needed the screen on the glass (M2-T9's s_prev_screen
       record); with no exemption there is no transition to test, and the
       record and its forget_painted_screen() calls went with it. The
       every-Nth full refresh above is unchanged. */
    plan.ghost_clean = true;
    return plan;
}
