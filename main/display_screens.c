/* LVGL screen builders (host-renderable; see display_screens.h). */
#include "display_screens.h"

#include <stdio.h>
#include <time.h>

#include "lvgl.h"

/* Bottom edge: labels centred over the physical buttons. Calibrated on
   hardware (2026-07): button D's centre lands at screen x=239 and the
   pitch is 74 px, so the row runs 17/91/165/239 — the display active area
   is offset ~20 px relative to the button row, it is NOT centred over it.
   Tune BTN_X0 (slides row) / BTN_PITCH (stretches) if a future panel
   batch differs. */
#define BTN_X0 17    /* screen x of button A's centre */
#define BTN_PITCH 74 /* px between adjacent button centres */
#define BTN_MID_OFS(i) (BTN_X0 + (i)*BTN_PITCH - DISP_HOR / 2)

static const char *day_type_str(day_type_t dt) {
    switch (dt) {
        case DAY_WEEKEND:
            return "Weekend";
        case DAY_HOLIDAY:
            return "Holiday";
        case DAY_SUMMER:
            return "Summer";
        default:
            return "Weekday";
    }
}

static const char *state_str(timer_state_t st) {
    switch (st) {
        case TIMER_RUNNING:
            return "RUNNING";
        case TIMER_PAUSED:
            return "PAUSED";
        case TIMER_EXPIRED:
            return "TIME'S UP";
        case TIMER_BREAK:
            return "BREAK";
        default:
            return "IDLE";
    }
}

/* Cleared screen; inverted = white-on-black (the Screen Break layout —
   unmistakable at a glance). */
static lv_obj_t *fresh_screen(bool inverted) {
    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, inverted ? lv_color_black() : lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(scr, inverted ? lv_color_white() : lv_color_black(), 0);
    return scr;
}

/* The create/text/font/align quartet — every label on every screen. */
static lv_obj_t *make_label(lv_obj_t *parent, const char *text, const lv_font_t *font, lv_align_t align, int32_t x,
                            int32_t y) {
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_align(lbl, align, x, y);
    return lbl;
}

/* Cap geometry — the geometric half of the version-width defence, the
   other half being DISPLAY_VERSION_MAX (see display.h for why a character
   budget alone cannot bound rendered width). The battery label is
   left-aligned at x=4 and must stay clear of the 28 pt remaining time,
   whose left edge bottoms out at x=180: 4 + 168 = 172, 8 px clear. The
   OTA screen's two version lines are centred on a 296 px panel: 280
   leaves 8 px each side, matching the margin the 28 pt title already
   renders with. */
#define BATT_ROW_MAX_W 168
#define OTA_LINE_MAX_W 280
/* The status row's left half, against the state word right-aligned at
   x=292. "TIME'S UP" is the widest state word at 62 px, so it starts at
   x=230; allowing 6 px of gap leaves x=224, and from the left margin at
   x=4 that is 220. Measured against the real font in
   test_mode_row_fits_beside_the_state_word, not guessed.

   This is a geometric BACKSTOP, not a width the worst line fits inside.
   Uncapped, "Weekday - 1440 min (-240 min today)" ends at exactly x=230:
   it abuts "TIME'S UP" with no gap rather than overstriking it, and the
   cap does not make it fit — it clips the last 6 px, which is the closing
   paren. The cap's job is that the two strings never touch, whatever the
   row is asked to carry.

   And it can be asked to carry more than that line. The suffix's
   magnitude is a RUNNING TOTAL of everything applied today, so the
   ±240 min of HA's own number (BONUS_MAX_MIN) and of a single cmd grant
   (GRANT_MAX_MINUTES) bound one adjustment, not the day: cmd grants
   repeat, one per network window, and stack. A character budget cannot
   bound this row either — the content is proportional text and the timer
   name comes from an HA text field. Hence a pixel cap. */
#define MODE_ROW_MAX_W 220

/* Hard geometric backstop for a label carrying caller-supplied text.
   LV_SIZE_CONTENT keeps auto-sizing below the cap (so ordinary strings
   render byte-identically and the goldens do not move); at the cap the
   label stops growing. LONG_CLIP rather than LONG_DOT because DOT wraps
   a content-sized label onto extra lines (measured: h=15 -> 45), which
   would push it out of its clean band and into the row below. */
static void cap_width(lv_obj_t *lbl, int32_t max_w) {
    lv_obj_set_style_max_width(lbl, max_w, 0);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_CLIP);
}

/* newlib's C locale renders strftime %p empty — format 12h time manually. */
static void format_time_12h(char *buf, size_t len, const struct tm *tm) {
    int h12 = tm->tm_hour % 12;
    if (h12 == 0)
        h12 = 12;
    snprintf(buf, len, "%d:%02d %s", h12, tm->tm_min, tm->tm_hour < 12 ? "AM" : "PM");
}

/* Square bar, 2 px border; inverted = white indicator on the black break
   screen. */
static void style_bar(lv_obj_t *bar, bool inverted) {
    lv_color_t bg = inverted ? lv_color_black() : lv_color_white();
    lv_color_t fill = inverted ? lv_color_white() : lv_color_black();
    lv_obj_set_style_radius(bar, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, bg, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(bar, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(bar, fill, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 0, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(bar, fill, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
}

/* Row 0-18: date + time (left), last sync (right) */
static void build_main_header(lv_obj_t *scr, const display_state_t *st) {
    char buf[64];
    char time_buf[16];
    struct tm tm;

    localtime_r(&st->wall_time, &tm);
    format_time_12h(time_buf, sizeof(time_buf), &tm);
    char date_buf[24];
    strftime(date_buf, sizeof(date_buf), "%a %b %d", &tm);
    snprintf(buf, sizeof(buf), "%s  %s", date_buf, time_buf);
    lv_obj_t *hdr = make_label(scr, buf, &lv_font_montserrat_12, LV_ALIGN_TOP_LEFT, 4, 3);
    /* At 12 pt the ':' hugs the preceding digit — open it up slightly */
    lv_obj_set_style_text_letter_space(hdr, 1, 0);

    if (st->break_banner) {
        /* A Screen Break is running behind this timer. The chip takes the
           Last-sync slot (the least load-bearing thing in the header) and
           echoes the break screen's inversion, so "the break is still on"
           reads at a glance. 16 pt: 12 pt white-on-black is illegible on
           this panel. Measured extent is rows 3..20, cols 182..291 —
           inside the header CLEAN_BAND's framebuffer bytes 0..2, which
           run to row 23 (see display.c). A chip reaching row 24 would
           share byte 3 with the progress-bar band and the ghost-cleaning
           double partial would invert it twice and cancel; the render
           test asserts rows 24..25 stay blank. */
        display_format_break_chip(buf, sizeof(buf), st->break_remaining_sec);
        lv_obj_t *chip = make_label(scr, buf, &lv_font_montserrat_16, LV_ALIGN_TOP_RIGHT, -4, 3);
        lv_obj_set_style_text_color(chip, lv_color_white(), 0);
        lv_obj_set_style_bg_color(chip, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_hor(chip, 4, 0);
        return;
    }

    if (st->last_sync_time > 0) {
        struct tm ts;
        localtime_r(&st->last_sync_time, &ts);
        format_time_12h(time_buf, sizeof(time_buf), &ts);
        snprintf(buf, sizeof(buf), "Last sync: %s", time_buf);
    } else {
        snprintf(buf, sizeof(buf), "Last sync: --:--");
    }
    lv_obj_t *sync = make_label(scr, buf, &lv_font_montserrat_12, LV_ALIGN_TOP_RIGHT, -4, 3);
    lv_obj_set_style_text_letter_space(sync, 1, 0);
}

/* The main progress bar's box. LV_ALIGN_TOP_MID on a 296 px panel puts
   the 284 px bar at x=6, and lv_bar maps its 0..280 range across that
   whole 284 px — the indicator starts at the bar's OUTER edge and the
   2 px border is painted over its ends (measured, and pinned by
   test_the_free_tranche_drains_at_the_same_rate_as_an_ungated_bar).

   So 280 is a scale, not a pixel count, and the locked block below is
   placed in the same scale from the same origin: it is a bar-value wide
   plus the two borders it carries. That is what keeps it aligned with a
   fill it does not compute. The 280 here and BAR_FILL_MAX_PX in
   display_layout.c are the same number and must stay so.

   The break and TIME'S UP bars are 284x16 boxes of their own and do not
   use these: nothing is ever laid over them. */
#define MAIN_BAR_W 284
#define MAIN_BAR_H 24
#define MAIN_BAR_Y 26
#define MAIN_BAR_BORDER 2
#define MAIN_BAR_X ((DISP_HOR - MAIN_BAR_W) / 2)
#define MAIN_BAR_INNER_W (MAIN_BAR_W - 2 * MAIN_BAR_BORDER)
/* The locked label's inset inside the block, each side. A margin, and
   NOT the threshold below which the label disappears: 2 * PAD is 8 px,
   and a 9 px block clears it while being nowhere near wide enough to
   hold a word. What actually decides whether a label is drawn is
   build_locked_block's fit ladder below, and the threshold is the
   MEASURED width of the narrowest rung, so it moves with the figure:
   "40 min" is 43 px and needs a 51 px block, a one-digit "9 min" 34 px
   and 42, "240 min" 50 px and 58. Below its own threshold each figure
   draws nothing at all.

   An empty outline is a true statement, and a lossy one: the withheld
   figure is on no other screen. The status row carries the day's DEFAULT
   ("Weekday - 60 min") and the checklist carries acks, so all that
   survives a suppressed label is the block's own share of the bar. That
   is still the better trade — a fragment of a glyph reads as damage on
   an outline rather than as a word, and a clipped figure is worse again,
   because it reads as a number. */
#define LOCKED_LABEL_PAD 4

/* The chore gate on the bar (§4.1): an outlined, UNFILLED block held at
   the left, with the free tranche draining to its right.

   Built as an empty lv_bar so it takes style_bar()'s exact frame. The
   block shares the bar's left, top and bottom border and its own right
   border IS the divider, which is why the box is locked_px + two borders
   wide: at locked_px == 280 that makes it 284, the bar itself, so a fully
   gated day needs no special case in the GEOMETRY here — the two right
   borders land on the same pixels rather than leaving a sliver. Its TEXT
   is special-cased, and elsewhere: display_format_locked_block switches
   wording on withheld >= allocation, because a block that is the whole
   bar has no draining region beside it to say what the chores are for.

   Nothing re-anchors the fill. The bar's value is the split's
   fill_end_px, measured from the bar's own left edge as any bar value is,
   and this block is simply laid over the part of it that is not free.
   LVGL lays the bar's 280 units across the object's whole 284 px while
   the block is placed in units 1:1, so below the fully gated end the
   divider sits 4 - locked_px/70 px into the free fill: 4 px for any block
   up to 69 px, 1 px by 210, and none at 280. The free tranche pays that
   at its LEFT edge, where nothing is read, and keeps its RIGHT edge —
   the one that moves.

   So §4.1's "pixels-per-second stays uniform across the whole bar" is
   exact at the seconds-to-UNITS conversion, which bar_px() in
   display_layout.c is the single expression of, and holds to within those
   4 px once LVGL has laid the units on glass: the free tranche's STATIC
   width can be up to 4 px short of the same seconds on an ungated bar,
   while its DRAIN RATE is the ungated one, both edges coming from the one
   value-to-pixel map.

   Unfilled, and black-on-white: a label over a filled bar would be
   half-inverted (§4.1), and white-on-black at 12 pt is what the break
   chip already had to abandon on this panel. 12 pt is PROVISIONAL —
   M2-HW1 is a look at the real glass.

   THE LABEL MUST NOT BE CLIPPED. The block's width is a config decision
   (withheld / allocation of 280 px), not a layout one, and §4.1's
   sentence needs a 127 px block for a two-digit figure and 134 for a
   three-digit one; a 60 min day with chore_free 40
   gives 93 px, where clipping renders "0/3 Chores - 2" for 20 locked
   minutes — a complete, plausible, wrong number, which is worse than no
   number. So the label degrades in steps instead (display_locked_form_t,
   widest first) and this measures each one against the room there is,
   keeping the first that fits whole and drawing nothing if none does.
   Measured and not estimated: the widths are font metrics, and a
   character budget cannot bound them (the same trap as CHORE_NAME_MAX_W
   above).

   NO cap_width() here, deliberately, and it is not an oversight to
   "fix": a width cap is a CLIPPING mechanism, which is the one thing
   this block must never do to a figure. The ladder already guarantees
   the chosen string fits, so a cap could only ever be reached if the
   ladder were broken — and then it would clip the number rather than
   catch the fault, which is the bug this code exists to prevent. Mutation
   confirmed it: with the ladder in place, deleting the cap changed no
   pixel in any of the 44 render tests (mutant S3, m2t5-mutate.py), so it
   was unreachable code that could only do harm. The margin before the
   divider comes from the fit itself — the label sits at LOCKED_LABEL_PAD
   and is no wider than locked_px - 2*PAD — and LVGL's clip to the parent
   remains as the structural backstop, bounded by the block rather than
   by a style. */
static void build_locked_block(lv_obj_t *scr, const display_state_t *st, uint16_t locked_px) {
    lv_obj_t *blk = lv_bar_create(scr);
    lv_obj_set_size(blk, locked_px + 2 * MAIN_BAR_BORDER, MAIN_BAR_H);
    lv_obj_align(blk, LV_ALIGN_TOP_LEFT, MAIN_BAR_X, MAIN_BAR_Y);
    lv_bar_set_range(blk, 0, MAIN_BAR_INNER_W);
    lv_bar_set_value(blk, 0, LV_ANIM_OFF); /* outlined and empty: the block is not a second countdown */
    style_bar(blk, false);

    const int32_t max_w = (int32_t)locked_px - 2 * LOCKED_LABEL_PAD;
    if (max_w <= 0)
        return;

    /* A CHILD of the block, so the label centres on the block's own
       height whatever 12 pt measures, and LVGL's clip to the parent backs
       up the width cap. One label re-texted down the ladder rather than
       one per rung: lv_obj_update_layout() re-measures it in place, and
       the loser rungs never exist as objects. */
    char buf[48];
    lv_obj_t *lbl = make_label(blk, "", &lv_font_montserrat_12, LV_ALIGN_LEFT_MID, LOCKED_LABEL_PAD, 0);
    for (int form = 0; form < DISPLAY_LOCKED_FORM_COUNT; form++) {
        display_format_locked_block(buf, sizeof(buf), (display_locked_form_t)form, st->chore_acked, st->chore_count,
                                    st->chore_withheld_sec, st->allocation_sec);
        lv_label_set_text(lbl, buf);
        lv_obj_update_layout(lbl);
        /* The natural width, because no cap is ever put on this label — a
           max-width style would make lv_obj_get_width() answer with the
           cap and this comparison would be true of every string. */
        if (lv_obj_get_width(lbl) <= max_w) {
            return;
        }
    }
    lv_obj_delete(lbl); /* not even the narrowest rung fits: the outline alone */
}

/* Row 26-50: progress bar (+ the chore gate's locked block, + low-battery
   badge riding both) */
static void build_main_bar(lv_obj_t *scr, const display_state_t *st) {
    /* The gate is a statement about the DAY — always slot 0's allocation,
       whichever timer is selected (see chore_withheld_sec in display.h).
       This bar's denominator follows the SELECTION, so with an extra
       timer drawn the two are different scales and a block measured in
       one and painted against the other would be a fraction of the wrong
       thing. Screen is the slot with no name, decided here exactly as
       build_main_status decides which status line to write.
       chore_count is re-tested because it is THE off switch (row C1) and
       nothing else on this screen reads it; the released day needs no
       test of its own, because chores_withheld_sec() has already
       returned 0 for it. */
    const bool screen_selected = (st->timer_name == NULL || st->timer_name[0] == '\0');
    const uint32_t withheld = (screen_selected && st->chore_count > 0) ? st->chore_withheld_sec : 0;
    const display_bar_split_t split = display_bar_split(st->remaining_sec, st->allocation_sec, withheld);

    lv_obj_t *bar = lv_bar_create(scr);
    lv_obj_set_size(bar, MAIN_BAR_W, MAIN_BAR_H);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, MAIN_BAR_Y);
    lv_bar_set_range(bar, 0, MAIN_BAR_INNER_W);
    lv_bar_set_value(bar, split.fill_end_px, LV_ANIM_OFF);
    style_bar(bar, false);

    /* With nothing withheld fill_end_px IS display_bar_fill_px() and no
       block is built, so every ungated screen renders byte-for-byte as it
       did before the gate existed — which is what the untouched goldens
       assert. */
    if (split.locked_px > 0)
        build_locked_block(scr, st, split.locked_px);

    /* Low battery (<= 15%): badge riding the bar — white background so it
       reads over both the filled (black) and empty parts of the bar.
       Built last, so it rides over the locked block too rather than
       disappearing under it. */
    if (st->charge_warn) {
        lv_obj_t *warn = make_label(scr, "Charge Me!!!", &lv_font_montserrat_16, LV_ALIGN_TOP_MID, 0, 29);
        lv_obj_set_style_text_color(warn, lv_color_black(), 0);
        lv_obj_set_style_bg_color(warn, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(warn, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_hor(warn, 6, 0);
    }
}

/* Row 58-86 + status row: battery, remaining time, mode line, state */
static void build_main_status(lv_obj_t *scr, const display_state_t *st) {
    char buf[64];
    static const char *BATT_SYMS[] = {LV_SYMBOL_BATTERY_EMPTY, LV_SYMBOL_BATTERY_1, LV_SYMBOL_BATTERY_2,
                                      LV_SYMBOL_BATTERY_3, LV_SYMBOL_BATTERY_FULL};
    /* The firmware version rides this label rather than getting one of
       its own: it adds no rows, so the clean-band table in display.c is
       unchanged (that table already tiles every byte of every row — see
       the comment there). Three spaces are the gap; the 'v' is
       presentation only and deliberately not part of the stored string,
       because ota_policy compares the BARE version against the manifest
       and a prefix must never leak upstream of the render.

       Two independent limits keep this off the 28 pt remaining time,
       which starts at x=180 in the worst case: the DISPLAY_VERSION_MAX
       budget (12 digits -> ends x=164, 16 px clear) and cap_width()'s
       geometric backstop at 168 px, which holds for any glyph mix.
       Buffer: 3 (symbol, a 3-byte UTF-8 private-use codepoint) + 1 + 3
       ("100") + 1 ('%') + 4 (gap + 'v') + 12 (budget) = 24 bytes + NUL,
       inside buf. test_version_fits_the_battery_row pins all of it. */
    char ver[DISPLAY_VERSION_MAX + 1];
    display_format_version(ver, sizeof(ver), st->fw_version);
    snprintf(buf, sizeof(buf), "%s %u%%%s%s", BATT_SYMS[display_battery_icon_level(st->battery_pct)],
             (unsigned)st->battery_pct, (ver[0] != '\0') ? "   v" : "", ver);
    lv_obj_t *batt = make_label(scr, buf, &lv_font_montserrat_12, LV_ALIGN_TOP_LEFT, 4, 66);
    cap_width(batt, BATT_ROW_MAX_W);

    display_format_remaining(buf, sizeof(buf), st->remaining_sec);
    make_label(scr, buf, &lv_font_montserrat_28, LV_ALIGN_TOP_RIGHT, -4, 58);

    /* Status row (moved up to make room for button labels): mode line
       (left: day-type + allocation, or the extra timer's name/counter),
       state (right) */
    if (st->timer_name != NULL && st->timer_name[0] != '\0') {
        display_format_mode_line(buf, sizeof(buf), st->timer_name, st->completions, st->reloadable, st->allocation_sec,
                                 st->adjust_sec);
    } else {
        display_format_day_line(buf, sizeof(buf), day_type_str(st->day_type), st->allocation_sec, st->adjust_sec);
    }
    lv_obj_t *mode = make_label(scr, buf, &lv_font_montserrat_12, LV_ALIGN_BOTTOM_LEFT, 4, -18);
    cap_width(mode, MODE_ROW_MAX_W);

    make_label(scr, state_str(st->timer_state), &lv_font_montserrat_12, LV_ALIGN_BOTTOM_RIGHT, -4, -18);
}

/* Button-label row along the bottom edge (geometry: see BTN_X0/BTN_PITCH).
   Cell 0 (A) is blank. A is the Timers/Chores mode toggle and has been
   bound since M2-T3, so "unbound" is no longer the reason; what it has
   not got yet is a LABEL. When one is added it must be gated on
   button_a_toggle_allowed() (button_actions.h) — the same predicate that
   arms A as a wake source — so the label never offers a press the map
   would refuse. B shows the action a press will take; C only when its
   press would work; D = sync, always.

   Every decision about B lives in display_button_b_label() — this is a
   plain switch over its answer, so no gate is re-tested here. */
static void build_button_row(lv_obj_t *scr, const display_state_t *st) {
    const char *b_text = NULL;
    switch (display_button_b_label(st->timer_state, st->start_available, st->reload_available)) {
        case DISPLAY_BTN_LABEL_PLAY:
            b_text = LV_SYMBOL_PLAY;
            break;
        case DISPLAY_BTN_LABEL_PAUSE:
            b_text = LV_SYMBOL_PAUSE;
            break;
        case DISPLAY_BTN_LABEL_RELOAD:
            b_text = "Reload";
            break;
        case DISPLAY_BTN_LABEL_NONE:
            break; /* draw nothing */
    }
    /* Every enumerator listed and no default: -Wswitch (an error under
       IDF's -Wall -Werror) then catches a new label that nobody wired up
       here, rather than letting it render as a blank cell. */
    if (b_text) {
        make_label(scr, b_text, &lv_font_montserrat_12, LV_ALIGN_BOTTOM_MID, BTN_MID_OFS(1), -2);
    }
    if (st->swap_available) {
        make_label(scr, LV_SYMBOL_RIGHT /* swap timer type */, &lv_font_montserrat_12, LV_ALIGN_BOTTOM_MID,
                   BTN_MID_OFS(2), -2);
    }
    make_label(scr, LV_SYMBOL_REFRESH, &lv_font_montserrat_12, LV_ALIGN_BOTTOM_MID, BTN_MID_OFS(3), -2);
}

void display_screens_build_main(const display_state_t *st) {
    lv_obj_t *scr = fresh_screen(false);
    build_main_header(scr, st);
    build_main_bar(scr, st);
    build_main_status(scr, st);
    build_button_row(scr, st);
}

/* Screen Break layout (inverted): title, draining break bar, break
   countdown, and the frozen screen-time remaining as a footer. */
void display_screens_build_break(const display_state_t *st) {
    lv_obj_t *scr = fresh_screen(true);
    char buf[64];

    make_label(scr, "SCREEN BREAK", &lv_font_montserrat_28, LV_ALIGN_TOP_MID, 0, 4);

    /* Break-progress bar: white indicator draining on the black screen */
    lv_obj_t *bar = lv_bar_create(scr);
    lv_obj_set_size(bar, 284, 16);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 40);
    lv_bar_set_range(bar, 0, 280);
    lv_bar_set_value(bar, display_bar_fill_px(st->break_remaining_sec, st->break_duration_sec), LV_ANIM_OFF);
    style_bar(bar, true);

    display_format_remaining(buf, sizeof(buf), st->break_remaining_sec);
    make_label(scr, buf, &lv_font_montserrat_48, LV_ALIGN_TOP_MID, 0, 62);

    char rem_buf[16];

    /* 12 pt renders illegibly white-on-black on e-ink (thin strokes eaten
       by the inversion), so the whole bottom row is 16 pt — which is also
       why the swap hint's name is truncated to a fixed budget. */
    if (st->swap_next_name == NULL) {
        /* No extra timers configured: nothing to swap to, so keep the
           original centred footer and no button row. */
        display_format_remaining(rem_buf, sizeof(rem_buf), st->remaining_sec);
        snprintf(buf, sizeof(buf), "Timer paused - %s left", rem_buf);
        make_label(scr, buf, &lv_font_montserrat_16, LV_ALIGN_BOTTOM_MID, 0, -4);
        return;
    }

    /* Left: the frozen screen time (h:mm — it cannot change during the
       break, and the row has three items to fit). At 16 pt this spans
       x 5..91: all of cell A (x<54) AND most of cell B (54..128), ending
       on B's button centre. Anything added to cell A of this row has to
       account for that — the width is NOT one cell's worth. */
    display_format_hm(rem_buf, sizeof(rem_buf), st->remaining_sec);
    snprintf(buf, sizeof(buf), "Screen %s", rem_buf);
    make_label(scr, buf, &lv_font_montserrat_16, LV_ALIGN_BOTTOM_LEFT, 4, -2);

    /* Over button C: the swap affordance. This row is built here rather
       than by build_button_row (16 pt, and the frozen screen time runs
       across cells A and B in place of their labels), but it already
       agrees with the new layout: A carries no label yet (see
       build_button_row), and B is blank because the break is still
       enforced for the Screen timer — TIMER_BREAK yields no label from
       display_button_b_label() either. */
    char hint[DISPLAY_SWAP_HINT_MAX + 1];
    display_format_swap_hint(hint, sizeof(hint), st->swap_next_name);
    snprintf(buf, sizeof(buf), "%s %s", LV_SYMBOL_RIGHT, hint);
    make_label(scr, buf, &lv_font_montserrat_16, LV_ALIGN_BOTTOM_MID, BTN_MID_OFS(2), -2);

    /* Over button D: the same refresh symbol as the main layout. */
    make_label(scr, LV_SYMBOL_REFRESH, &lv_font_montserrat_16, LV_ALIGN_BOTTOM_MID, BTN_MID_OFS(3), -2);
}

/* ---- the chore checklist (design §2.4) ----------------------------------

   Geometry, top to bottom on the 128 px panel:

     y=2    "CHORES" (16 pt)                      "n of 3" (12 pt, right)
     y=24   "Screen time unlocked" (16 pt, centred) — only once unlocked
     y=46   row 0   tick column at x=6, name at x=CHORE_NAME_X
     y=68   row 1
     y=90   row 2
     bottom "Timers"   "OK 1"   "OK 2"   "OK 3"   (12 pt, over the buttons)

   THREE FIXED ROWS is the whole point (§2.4): nothing slides, nothing
   refills, row order never changes, which is what makes B/C/D unambiguous
   and lets the NeoPixels mirror the rows one-for-one (§2.5). So the row y
   values are constants and a shorter list leaves the spare rows blank —
   never a list that closes up. */
#define CHORE_TICK_X 6
#define CHORE_NAME_X 32
/* Geometric cap for a name, which is the ONLY thing that can bound its
   rendered width: chores.h measured exactly this trap (a byte budget
   cannot, because glyph advances differ — 20 'W' at 16 pt render 360 px
   against a 296 px panel). From the name column to a 4 px right margin:
   296 - 32 - 4. cap_width() clips rather than wraps, so an over-long name
   cannot push itself down into the next row's band. */
#define CHORE_NAME_MAX_W 260
static const int32_t CHORE_ROW_Y[CHORE_MAX] = {46, 68, 90};

void display_screens_build_chores(const display_state_t *st) {
    lv_obj_t *scr = fresh_screen(false);
    char buf[64];

    /* Not inverted, unlike the break screen. The checklist is a place the
       kid reads names off and presses buttons at, not a "the device is
       withholding something" takeover; 12 pt white-on-black is illegible
       on this panel (see the break chip), and the tick column needs the
       small font to stay in its cell. */
    make_label(scr, "CHORES", &lv_font_montserrat_16, LV_ALIGN_TOP_LEFT, 4, 2);

    display_format_chore_count(buf, sizeof(buf), st->chore_acked, st->chore_count);
    lv_obj_t *count = make_label(scr, buf, &lv_font_montserrat_12, LV_ALIGN_TOP_RIGHT, -4, 5);
    lv_obj_set_style_text_letter_space(count, 1, 0); /* as the main header */

    /* §2.4: the mode does not bounce you out on the last ack, so the
       screen has to say that something happened. Absent otherwise — the
       row is blank in the design's own sketch. */
    if (display_chore_unlocked(st->chore_acked, st->chore_count, st->chore_released)) {
        make_label(scr, "Screen time unlocked", &lv_font_montserrat_16, LV_ALIGN_TOP_MID, 0, 24);
    }

    for (uint8_t i = 0; i < CHORE_MAX; i++) {
        if (i >= st->chore_count)
            break; /* a spare row stays blank; the rows below do not move up */
        if (display_chore_row_ticked(st->chore_acked, st->chore_count, i)) {
            make_label(scr, LV_SYMBOL_OK, &lv_font_montserrat_16, LV_ALIGN_TOP_LEFT, CHORE_TICK_X, CHORE_ROW_Y[i]);
        }
        /* chore_names rows are always NUL-terminated (display.h), so this
           needs no length check of its own — only the width cap. */
        lv_obj_t *name = make_label(scr, st->chore_names[i], &lv_font_montserrat_16, LV_ALIGN_TOP_LEFT, CHORE_NAME_X,
                                    CHORE_ROW_Y[i]);
        cap_width(name, CHORE_NAME_MAX_W);
    }

    /* The button row. Unlike build_button_row's, every cell here is
       unconditional for a configured row: display_screen_for() has already
       established that chores are configured and no timer is RUNNING, which
       is exactly button_a_toggle_allowed()'s rule, so A really will act.
       A's label is LEFT-ALIGNED, not centred on its button like the other
       three. Button A's centre is only 17 px in (BTN_X0), so a 41 px
       "Timers" centred there starts at x=-4 and loses its first glyph off
       the panel — measured, by test_the_chore_button_row_fits_its_cells
       before this line read BOTTOM_LEFT. The break screen's bottom row
       already anchors its cell-A item at x=4 for the same reason, and
       §2.4's own sketch draws "Timers" flush left. */
    make_label(scr, "Timers", &lv_font_montserrat_12, LV_ALIGN_BOTTOM_LEFT, 4, -2);
    for (uint8_t i = 0; i < CHORE_MAX; i++) {
        if (i >= st->chore_count)
            break; /* an unconfigured ack button is not a chore button (C1) */
        snprintf(buf, sizeof(buf), "%s %u", LV_SYMBOL_OK, (unsigned)(i + 1));
        make_label(scr, buf, &lv_font_montserrat_12, LV_ALIGN_BOTTOM_MID, BTN_MID_OFS(i + 1), -2);
    }
}

void display_screens_build_timesup(void) {
    lv_obj_t *scr = fresh_screen(false);

    make_label(scr, "TIME'S UP", &lv_font_montserrat_48, LV_ALIGN_CENTER, 0, -10);

    /* Empty bar underneath, per spec */
    lv_obj_t *bar = lv_bar_create(scr);
    lv_obj_set_size(bar, 284, 16);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_bar_set_range(bar, 0, 280);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    style_bar(bar, false);
}

void display_screens_build_sync_failed(void) {
    lv_obj_t *scr = fresh_screen(false);
    make_label(scr, "No sync - check WiFi", &lv_font_montserrat_28, LV_ALIGN_CENTER, 0, 0);
}

/* Battery lock (<= 10%): the panel says only this until the pack charges
   back above the warn band. */
void display_screens_build_charge_me(void) {
    lv_obj_t *scr = fresh_screen(false);
    make_label(scr, "Charge Me!", &lv_font_montserrat_48, LV_ALIGN_CENTER, 0, 0);
}

/* Bed Time lock: inverted like the break screen (night-appropriate and
   unmistakably "not in service"); stays until day rollover. */
void display_screens_build_bedtime(void) {
    lv_obj_t *scr = fresh_screen(true);
    make_label(scr, "Bed Time", &lv_font_montserrat_48, LV_ALIGN_TOP_MID, 0, 2);
    /* Single centered line, 18 pt: 16 pt strokes shredded into fragments
       by the inverted 1 bpp render (no bold Montserrat exists in LVGL),
       and 20 pt overruns the 296 px panel. */
    make_label(scr, "Brush teeth | Get water bottles", &lv_font_montserrat_18, LV_ALIGN_CENTER, 0, 4);
    make_label(scr, "Goodnight!", &lv_font_montserrat_28, LV_ALIGN_BOTTOM_MID, 0, -2);
}

/* Firmware update, full refresh only (display_ota): static, no progress
   bar. Both versions render — the one being left and the one being
   installed — so a device that gets stuck says which transition it was
   attempting without needing the logs or HA.

   No CLEAN_BANDS entry in display.c is needed: the bands are consulted
   only on the PARTIAL path, and this screen (like charge_me / bedtime /
   timesup) only ever renders as a full refresh, which drives the whole
   panel and clears ghosting by itself. */
void display_screens_build_ota(const char *from_version, const char *to_version) {
    lv_obj_t *scr = fresh_screen(false);
    char buf[64];
    char ver[DISPLAY_VERSION_MAX + 1];

    /* Title case, not the plan's "UPDATING FIRMWARE": all caps at 28 pt
       measures 327 px against a 296 px panel, so LVGL would clip the tail
       off "FIRMWARE" (a content-sized label does not wrap). Title case is
       279 px — 8 px of margin each side — and matches the other big
       headlines in this tree ("Charge Me!", "Bed Time"). The render test
       pins the width. */
    make_label(scr, "Updating Firmware", &lv_font_montserrat_28, LV_ALIGN_TOP_MID, 0, 6);

    display_format_version(ver, sizeof(ver), from_version);
    snprintf(buf, sizeof(buf), "Current: v%s", (ver[0] != '\0') ? ver : "?");
    cap_width(make_label(scr, buf, &lv_font_montserrat_12, LV_ALIGN_TOP_MID, 0, 46), OTA_LINE_MAX_W);

    /* "Installing", not "Upgrading to": the OTA policy deliberately
       treats a *different* published version as an update, downgrades
       included, so the directional verb would be actively wrong on a
       rollback. The "Current:" line above already supplies the
       direction. */
    display_format_version(ver, sizeof(ver), to_version);
    snprintf(buf, sizeof(buf), "Installing v%s", (ver[0] != '\0') ? ver : "?");
    /* 18 pt is the binding line: untruncated it clips off the panel at 21
       characters (measured 303 px against 296). */
    cap_width(make_label(scr, buf, &lv_font_montserrat_18, LV_ALIGN_TOP_MID, 0, 66), OTA_LINE_MAX_W);

    /* Honest rather than strictly necessary — the write goes to the
       inactive slot and the boot partition only flips after the image
       verifies — but a yanked cable still wastes the download. */
    make_label(scr, "Do not remove power", &lv_font_montserrat_12, LV_ALIGN_TOP_MID, 0, 98);
}
