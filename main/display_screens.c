/* LVGL screen builders (host-renderable; see display_screens.h). */
#include "display_screens.h"

#include <stdint.h>
#include <stdio.h>
#include <time.h>

#include "lvgl.h"
#include "qr_render.h"
#include "setup_trigger.h" /* SETUP_TRIGGER_BOOT_HOLD_MS: the timeout screen's retry hint */
#include "sprite.h"
#include "sprite_data.h"

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

/* Defined with the sprite screens below; the break screen (above them in
   this file) and the time's-up / charge screens all paint through it. */
static void build_sprite(lv_obj_t *scr, const sprite_t *spr, int32_t x, int32_t y, int32_t scale, lv_color_t ink);

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
/* The config-error screen's pair line, centred on a 296 px panel like the
   OTA version lines and for the same 8 px-a-side margin. A geometric
   backstop, not a width the worst line fits inside: the numbers are read
   raw out of NVS, so no character budget bounds them. */
#define CONFIG_ERR_LINE_MAX_W 280
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

    if (st->status_hint != NULL && st->status_hint[0] != '\0') {
        /* Same slot, same font: it is the Last-sync line's replacement, so
           it inherits that line's clean-band rows and nothing new has to be
           added to display.c's refresh tables. */
        lv_obj_t *hint = make_label(scr, st->status_hint, &lv_font_montserrat_12, LV_ALIGN_TOP_RIGHT, -4, 3);
        lv_obj_set_style_text_letter_space(hint, 1, 0);
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
   chip already had to abandon on this panel. 12 pt BLACK-ON-WHITE HAS NOW
   BEEN READ ON THE REAL GLASS (M2-HW1, 2026-09-22) and is legible, so the
   size is settled and the block the four goldens carry —
   main_chore_gated, main_chore_gated_running, main_chore_fully_gated and
   main_chore_gated_narrow — is final rather than provisional: a future
   diff inside it is a regression to explain, not a placeholder still
   waiting on a panel. Note what that measurement does NOT cover: it says nothing
   about 12 pt INVERTED, which the break chip's own finding still rules
   out and which is why the config-error screen below stays non-inverted.

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
   THE MAIN SCREEN'S row — the break screen builds its own, at 16 pt and
   with different cells.

   Cell 0 (A) NOW CARRIES "Chores", and until M2-HW-FIX it carried
   nothing — which is the bug a board found. A is the Timers/Chores mode
   toggle and has been bound since M2-T3, the break screen labels it
   (§2.6) and the chore screen labels its way back ("Timers"), but the
   main screen — the one a Timer-mode device is actually sitting on —
   offered no hint at all. So a configured list was reachable only if you
   already knew A did something, or had read the cell on a break screen
   and remembered it: discoverable during a break, invisible the rest of
   the day, which is most of it. Three spellings of one affordance and the
   third was simply missed.

   THE GATE IS button_a_toggle_allowed()'s RULE (button_actions.h) — the
   predicate the BUTTON MAP consults, so the label never offers a press
   the map would refuse. Not, note, the same thing as "what arms A as a
   wake source": buttons_policy.c's wake_source() short-circuits on
   config_locked and returns D-only however this predicate answers, so
   under the config-error lock A is unarmed while this rule may still say
   yes. That is the break painter's distinction 130 lines below, and it
   applies here identically; the lock's own screen is not this one, which
   is why the difference costs nothing rather than being a second gate.

   Spelled here as the two conditions the painter can see: not RUNNING,
   and a non-empty list. The break screen gets to write only
   `chore_count > 0` because TIMER_BREAK rules out the predicate's other
   refusal. This painter has no such shortcut, and the reason is NOT that
   it runs in every state — it does not. display_screen_for()
   (display_layout.c) sends every TIMER_BREAK to the break screen or the
   checklist, so TIMER_BREAK never reaches this function in the app at
   all. What it does reach is RUNNING, which the break screen cannot, and
   that alone is why both conditions must be tested here. The sweep that
   holds this line nevertheless drives TIMER_BREAK through it — see the
   note in that test for why an unreachable row is deliberate there.

   SO THIS IS THE THIRD DISPLAY-SIDE SPELLING of that predicate, and the
   cross-check is STILL held by nothing — the same hazard the break
   painter's own note names, now one copy worse. test_button_actions has
   the predicate but cannot compile this file (no LVGL); test_display_render
   compiles this file but cannot reach the predicate (no NVS, no timer).
   What holds this copy is a render-suite sweep of the boundary in both
   directions (test_the_main_screen_offers_chores_exactly_when_button_a_would_act),
   which is the same shape the break screen's sweep has and catches the
   same two mutants, `> 1` and `>= 0`, plus the RUNNING arm this screen
   adds. A future refusal reason added to button_a_toggle_allowed() and
   not to this line makes "Chores" a dead label here as it would there —
   but NOT to the same degree, and the asymmetry runs the wrong way for
   this copy. The break screen is transient: it is painted for the length
   of a break and the next state change repaints over it, so a stale label
   there is wrong for minutes. This screen is the RESIDENT one — an e-ink
   panel holds it through every deep sleep until something else needs
   drawing — so a stale label here is what the device is showing, all day,
   to someone pressing a button that does nothing. Of the three spellings
   this is the one whose drift costs most.

   LEFT-ANCHORED, NOT CENTRED ON BTN_MID_OFS(0), for the reason the chore
   screen's "Timers" records at its own call: button A's centre is only
   17 px in (BTN_X0), so a ~41 px label centred on it starts at x=-4 and
   loses its first glyph off the panel. Measured there, not guessed; this
   is the same geometry and takes the same answer.

   B shows the action a press will take; C only when its press would work;
   D = sync, always.

   Every decision about B lives in display_button_b_label() — this is a
   plain switch over its answer, so no gate is re-tested here. */
static void build_button_row(lv_obj_t *scr, const display_state_t *st) {
    /* Built first so the row's children are left to right in build order,
       which is what the render suite's adjacency sweeps read. */
    if (st->timer_state != TIMER_RUNNING && st->chore_count > 0) {
        make_label(scr, "Chores", &lv_font_montserrat_12, LV_ALIGN_BOTTOM_LEFT, 4, -2);
    }

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

/* ---- Screen Break layout (inverted), design §2.6 -----------------------

   Two columns. At the left the break art (SPRITE_BREAK, a 46x52 slot; the
   template is a meditating figure, see docs/developer_setup.md) drawn at
   2x, WHITE on the black screen, at (4, 4): a 92x104 box, x 4..95,
   y 4..107. At the right, a 192 px column x 100..291 holding the title,
   countdown, bar and chore line. The art is static between partial
   refreshes: nothing here repaints it.

   The figures below are MEASURED ink rows (inclusive) on the 128 px panel —
   first/last row actually painted, not LVGL box extents, which for a
   48 pt label overstate the glyphs by 9 rows at each end:

     element               ink rows     box y
     "SCREEN BREAK" 18pt     4..16       0     (centred on the column)
     countdown 48pt         26..59      17     (m:ss / mm:ss / h:mm:ss)
     break bar              68..83      68     (192 x 16)
     chore line 16pt        89..104     88     (only with a chore list)
     bottom row 16pt       109..124     BOTTOM -2 (unchanged)

   The chore line's rows 89..104 are the '/' of "Chores 0/3", which
   ascends and descends two rows past the 12-row letters (91..102); the tick
   form paints only 91..102. Gutters between bands: 9 / 8 / 5 / 4 rows, and
   4 is the gutter the screen has always run at, so this is no tighter than
   it has been. test_the_break_chore_line_clears_the_countdown_and_the_row
   measures the bands off the framebuffer and fails if any two touch,
   because on e-ink overlapping white-on-black text is unreadable and a
   regenerated golden would record the overlap as the new correct answer.
   The art's box ends at row 107 and the bottom row starts at 109: one blank
   row (a test pins that no ink crosses it).

   With no list the same stack stands and the chore band is simply empty;
   the frozen screen time moves to cell A of the bottom row as before.

   12 pt renders illegibly white-on-black on e-ink (thin strokes eaten by
   the inversion), so EVERY string on this screen is 16 pt or larger — which
   is also why the swap hint's name is truncated to a fixed budget.

   CLEAN_BANDS (display.c) needs no change and its geometry is the main
   screen's, which jointly covers every byte of every row. Which band holds
   which element here (test_the_break_elements_sit_inside_the_clean_bands_
   that_cover_them reads the real table):
     band 0 rows  0..23   the title (static, so never dirty)
     band 1 rows 24..55   the top of the countdown
     band 2 rows 56..87   the bottom of the countdown, the draining bar
     band 3 rows 88..127  the chore line, the bottom row / footer
   The countdown straddles bands 1 and 2, which is fine: no band shares a
   framebuffer byte with another. */
#define BREAK_ART_X 4
#define BREAK_ART_Y 4
#define BREAK_COL_X 100
#define BREAK_COL_W 192
#define BREAK_TITLE_Y 0
#define BREAK_COUNTDOWN_Y 17
#define BREAK_BAR_Y 68
#define BREAK_BAR_H 16
#define BREAK_CHORE_LINE_Y 88
/* The widest chore prompt ("Chores 0/3", measured, CHORE_MAX is 3) and the
   width left for the right-hand "Screen h:mm" beside it. */
#define BREAK_PROMPT_MAX_W 86
#define BREAK_SCREEN_MAX_W (BREAK_COL_W - BREAK_PROMPT_MAX_W)

void display_screens_build_break(const display_state_t *st, const sprite_t *art) {
    lv_obj_t *scr = fresh_screen(true);
    char buf[64];
    char rem_buf[16];

    /* §2.6's matrix as two independent questions. The old rule tested
       `swap_next_name == NULL` and used the answer for BOTH "is there a
       swap hint" and "is there a button row"; those stopped being the
       same question when cell A gained a label of its own, so the row's
       existence is now the OR of its cells and each cell asks only about
       itself.

       has_chores is the display-side spelling of button_a_toggle_allowed()
       (button_actions.h): that predicate refuses on no-chores-configured
       or an active slot that is RUNNING, and the second can never hold
       here — display_screen_for() only routes to this screen on
       TIMER_BREAK, and only the active slot is ever RUNNING. So on this
       screen the two conditions collapse to one, and the label can never
       offer a press the map would refuse.

       ONE OF THREE display-side spellings of that predicate — display.h
       describes the chore screen's, and M2-HW-FIX added the main screen's
       in build_button_row() above — and a refusal reason added to A and
       not to this line makes "Chores" below a button that does nothing —
       silent, where the chore screen's version is at least loud.

       WHAT HOLDS IT, precisely, because the two halves are not in one
       suite. The boundary of this literal is swept by
       test_the_break_screen_offers_chores_exactly_when_the_list_is_non_empty
       (test_display_render), which renders both sides of it and kills
       `> 1` and `>= 0` alike; the main screen's spelling has a sweep of
       the same shape beside it. The CROSS-CHECK against
       button_a_toggle_allowed() is held by nothing: test_button_actions
       has the predicate but not this file (no LVGL), and the render suite
       has this file but not the predicate. So if a device lock adds a
       refusal reason, the chore screen's spelling is covered by that
       suite's sweep and NEITHER PAINTER'S IS — teaching them the lock
       would be a manual obligation of that task, not something a test
       will catch, and there are now two lines to teach.

       M2-T10 CAME AND WENT AND THIS LINE IS UNCHANGED, which is a
       finding and not an omission, so it is recorded here rather than
       left for the next reader to re-derive. The config-error lock adds
       NO refusal reason to button_a_toggle_allowed(), so the predicate
       and this literal still agree and this label still cannot offer a
       press the map would refuse. WHAT THE LOCK NARROWS IS THE WAKE MASK
       (buttons_policy.c), not the set of screens that can reach the
       panel, and the difference is the whole of what follows.

       NO GUARANTEE IS CLAIMED HERE ABOUT WHAT IS ON THE GLASS. An
       earlier draft of this comment said the config-error screen was the
       only screen that could reach the panel while the lock held. That
       was false in two directions at once, and both are worth naming
       because the argument is the tempting one. It is false ACROSS
       LOCKS: the other two gates paint as they engage and can go up over
       a held config lock, which lock_gate.c's own config gate is written
       to correct. And it was false WITHIN the locked path: the network
       window every locked wake runs can repaint through
       on_active_expired_alert(), which is how the chore checklist could
       end up on the panel above a D-only wake mask — M2-T10's
       HIGH-severity finding. The reachable-screens claim was an
       enumeration of the painters its author could see, stated as a
       property of the system.

       WHAT IS TRUE NOW, by mechanism rather than by enumeration: every
       locked path in lock_gate.c repaints its own screen immediately
       before enter_deep_sleep(), unconditionally, so whatever painted
       during the wake, the lock's screen is what the panel is left
       holding. That is a statement about the sleep, not about the wake:
       DURING a locked wake other screens do reach the panel, and this
       label can be among them.

       WHICH MEANS THE UNDERLYING HAZARD IS NOT RETIRED, only this
       task's version of it. A future gate that adds a REFUSAL REASON to
       button_a_toggle_allowed() without adding one here still makes
       "Chores" a dead label, and nothing tests the cross-check (see the
       paragraph above). A blocking lock does not put that obligation
       back; a refusing one always does. */
    const bool has_chores = st->chore_count > 0;
    const bool has_swap = st->swap_next_name != NULL;

    /* The art is static: white ink on the black screen, drawn at 2x. */
    build_sprite(scr, art ? art : &SPRITE_BREAK, BREAK_ART_X, BREAK_ART_Y, 2, lv_color_white());

    /* Right column: title, countdown and bar are centred on it. */
    lv_obj_t *title =
        make_label(scr, "SCREEN BREAK", &lv_font_montserrat_18, LV_ALIGN_TOP_LEFT, BREAK_COL_X, BREAK_TITLE_Y);
    lv_obj_set_width(title, BREAK_COL_W);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);

    display_format_countdown(buf, sizeof(buf), st->break_remaining_sec);
    lv_obj_t *count = make_label(scr, buf, &lv_font_montserrat_48, LV_ALIGN_TOP_LEFT, BREAK_COL_X, BREAK_COUNTDOWN_Y);
    lv_obj_set_width(count, BREAK_COL_W);
    /* CLIP, not the default WRAP: a value too wide for the column must not
       drop a second line into the bar. Breaks cap at CFG_BOUND_BREAK_DUR_HI
       ("2:00:00" fits), so this is a backstop. */
    lv_label_set_long_mode(count, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(count, LV_TEXT_ALIGN_CENTER, 0);

    /* Break-progress bar: white indicator draining on the black screen */
    lv_obj_t *bar = lv_bar_create(scr);
    lv_obj_set_size(bar, BREAK_COL_W, BREAK_BAR_H);
    lv_obj_align(bar, LV_ALIGN_TOP_LEFT, BREAK_COL_X, BREAK_BAR_Y);
    /* The bar is BREAK_COL_W (192) px wide but display_bar_fill_px() works
       in the main screen's 0..280 range. The fraction is right (fill/280
       of the width), so the range is kept rather than rescaled. */
    lv_bar_set_range(bar, 0, 280);
    lv_bar_set_value(bar, display_bar_fill_px(st->break_remaining_sec, st->break_duration_sec), LV_ANIM_OFF);
    style_bar(bar, true);

    if (has_chores) {
        /* The chore line, left and right on one band of the right column:
           the prompt at the column's left edge, the frozen screen time at
           its right edge (x 292, the panel's usual 4 px margin). The
           frozen time is on this line rather than in the row below
           because it cannot BE in that row any more — "Screen 1:30" is
           90 px and centred over button B it spans x 46..136, which runs
           into "Chores" at one end and the swap hint at the other. The
           design's mock draws it over B; the measurement says only three
           items fit that row, so the fourth moved up.

           THE PROMPT IS ON THE LEFT so that "Chores" holds one x for the
           life of the screen: it is the item whose text changes, and a
           right-anchored label of changing width walks its own left edge
           across the panel every time an ack lands. The same reasoning
           puts the tick after the word rather than before it.

           WIDTH, measured on the built objects (LVGL box extents, which
           is what the render test reads) over EVERY string either side
           can produce, because the digits of this font do not share one
           advance — '0' is 3 px wider than '1' — so neither side's worst
           case can be reasoned about from the character count. The
           column is 192 px (the figure takes the rest of the panel):
             "Chores 0 of 3"   105 px  + "Screen 20:00" 106 px = 211 > 192
             "Chores 0/3"       86 px  + "Screen 20:00" 106 px = 192, no gap
           so the plan's "Chores 1 of 3" does not fit at the widest values
           and the prompt is "Chores 1/3" (display_format_chore_prompt).
           Even that leaves a gap only while the hour is one digit: the
           boxes are at least 9 px apart under 10 h, and from 10 h they
           abut (0..3 px).

           THE FROZEN TIME IS NOT BOUNDED BY THE ALLOCATION (24 h,
           CFG_BOUND_ALLOC_HI): remaining_sec is the allocation PLUS
           grants (a cmd grant is up to GRANT_MAX_MINUTES and they stack,
           one per network window), and the snapshot accepts up to
           SNAPSHOT_MAX_HORIZON_SEC (7 days), so "Screen 168:00" is
           reachable. Uncapped it is 119 px wide and would overstrike the
           prompt from 40 h (2 px at 2400 min, 8 px at 100 h). So the
           right-hand label is capped at BREAK_SCREEN_MAX_W — the column
           less the widest prompt, i.e. exactly the widest time that does
           not touch it — and clips (cap_width()) instead of growing left.
           Up to "Screen 20:00" the cap changes nothing, so every
           ordinary value renders exactly as before; beyond it the label
           loses its trailing digits, which is the lesser evil next to
           overprinting the prompt. test_the_break_chore_line_items_never_
           meet sweeps to the 7-day horizon.

           HEIGHT: the letters and digits are 12 ink rows (91..102) and
           '/' paints 89..104, between the bar (ends 83) and the button
           row (starts 109): gutters of 5 and 4 rows. The column's three
           bands and the row used to be packed into the full-width screen
           with no room for '/', which is why the prompt used to read
           "2 of 3"; the column has the rows now. */
        display_format_chore_prompt(buf, sizeof(buf), st->chore_acked, st->chore_count);
        make_label(scr, buf, &lv_font_montserrat_16, LV_ALIGN_TOP_LEFT, BREAK_COL_X, BREAK_CHORE_LINE_Y);

        display_format_hm(rem_buf, sizeof(rem_buf), st->remaining_sec);
        snprintf(buf, sizeof(buf), "Screen %s", rem_buf);
        cap_width(make_label(scr, buf, &lv_font_montserrat_16, LV_ALIGN_TOP_RIGHT, -4, BREAK_CHORE_LINE_Y),
                  BREAK_SCREEN_MAX_W);
    }

    if (!has_chores && !has_swap) {
        /* Matrix row 4: no list to offer and nothing to swap to, so no
           cell has anything in it and there is no row. The original
           centred footer is the whole bottom, exactly as before — this is
           the ONE cell that keeps it, and it is the only place the full
           h:mm:ss remaining is spelled out. */
        display_format_remaining(rem_buf, sizeof(rem_buf), st->remaining_sec);
        snprintf(buf, sizeof(buf), "Timer paused - %s left", rem_buf);
        make_label(scr, buf, &lv_font_montserrat_16, LV_ALIGN_BOTTOM_MID, 0, -4);
        return;
    }

    /* Cell A. With a list, the mode toggle finally gets its label; without
       one, the cell carries the frozen screen time as it always has.
       The art's box ends at row 107, one row above this row (109..124).
       Either way this is NOT one cell's worth of width, and the two
       overrun differently:
         "Chores"      renders x 5..59  — 5 px past the A/B midline (54)
         "Screen 1:30" renders x 5..91  — all of A and most of B, ending
                                          on B's button centre
       Both are safe for the same single reason: cell B carries no label on
       this screen, because the break is still enforced for the Screen
       timer and display_button_b_label() yields nothing for TIMER_BREAK.
       Anything that ever puts a label over B has to move this one first.
       test_the_break_chore_row_fits_its_cells measures it. */
    if (has_chores) {
        make_label(scr, "Chores", &lv_font_montserrat_16, LV_ALIGN_BOTTOM_LEFT, 4, -2);
    } else {
        display_format_hm(rem_buf, sizeof(rem_buf), st->remaining_sec);
        snprintf(buf, sizeof(buf), "Screen %s", rem_buf);
        make_label(scr, buf, &lv_font_montserrat_16, LV_ALIGN_BOTTOM_LEFT, 4, -2);
    }

    /* Cell C: the swap affordance, only when there is somewhere to swap
       to. This row is built here rather than by build_button_row because
       it is 16 pt and cell A is not the main screen's cell A. */
    if (has_swap) {
        char hint[DISPLAY_SWAP_HINT_MAX + 1];
        display_format_swap_hint(hint, sizeof(hint), st->swap_next_name);
        snprintf(buf, sizeof(buf), "%s %s", LV_SYMBOL_RIGHT, hint);
        make_label(scr, buf, &lv_font_montserrat_16, LV_ALIGN_BOTTOM_MID, BTN_MID_OFS(2), -2);
    }

    /* Cell D: sync, the same refresh symbol as the main layout. Present
       whenever the row is, which is what "D = sync, always" means on a
       screen that can have no row at all. */
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

/* ---- Time's up -----------------------------------------------------------

   SPRITE_TIMESUP (a 178x124 slot, tools/sprites/timesup.png; the template is
   a shocked alarm clock) sits at the left, FLUSH WITH THE BOTTOM EDGE
   (y = 128 - 124 = 4): the template is cropped open at the bottom, so its
   ink may run off the panel's bottom row (the bleed rule: ink on that row
   only inside the art's own columns; the template has 12 such pixels).
   "TIME'S" (28 pt) over "UP!" (48 pt) are centred in the space to its
   right. The sprite is painted by the same sprite_draw_cb() /
   build_sprite() the charge lock uses (defined below, with the charge art;
   build_sprite() is forward-declared at the top of the file), at scale 1
   in black.

   Not inverted (thin 1-3 px lines, as the charge lock). Full refresh only
   (display_timesup()), so no CLEAN_BANDS entry.
   test_the_timesup_screen_fits_the_panel_but_for_the_art_bottom_bleed
   pins the one edge exception; the bitmap's ink count is the PNG's.

   TIMESUP_COL_X is a literal because SPRITE_TIMESUP.w is an extern field,
   not a constant expression; the paint test asserts that
   TIMESUP_ART_X + SPRITE_TIMESUP.w <= TIMESUP_COL_X. */
#define TIMESUP_ART_X 2
#define TIMESUP_COL_X 180 /* right column: 180..295 */
#define TIMESUP_COL_W 116
#define TIMESUP_TIMES_Y 26
#define TIMESUP_UP_Y 60

void display_screens_build_timesup(const sprite_t *art) {
    lv_obj_t *scr = fresh_screen(false);
    build_sprite(scr, art ? art : &SPRITE_TIMESUP, TIMESUP_ART_X, DISP_VER - SPRITE_TIMESUP.h, 1, lv_color_black());

    lv_obj_t *times =
        make_label(scr, "TIME'S", &lv_font_montserrat_28, LV_ALIGN_TOP_LEFT, TIMESUP_COL_X, TIMESUP_TIMES_Y);
    lv_obj_set_width(times, TIMESUP_COL_W);
    lv_obj_set_style_text_align(times, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *up = make_label(scr, "UP!", &lv_font_montserrat_48, LV_ALIGN_TOP_LEFT, TIMESUP_COL_X, TIMESUP_UP_Y);
    lv_obj_set_width(up, TIMESUP_COL_W);
    lv_obj_set_style_text_align(up, LV_TEXT_ALIGN_CENTER, 0);
}

void display_screens_build_sync_failed(void) {
    lv_obj_t *scr = fresh_screen(false);
    make_label(scr, "No sync - check WiFi", &lv_font_montserrat_28, LV_ALIGN_CENTER, 0, 0);
}

/* No-clock lock (BUG-14): a power-on whose NTP attempt failed does not
   know what day it is, so it hands out no screen time until NTP works.
   Its own screen rather than sync_failed's one line, because this one is
   read by the child and has to say what to DO (owner decision, "clear and
   easy UX is important"): what is wrong, the likely cause, what it costs,
   and that D retries at once rather than on the 30-minute cadence.

   Shaped on the config-error screen, and NOT INVERTED for its reason: it
   carries a 12 pt line, which white-on-black shreds on this panel. Full
   refresh only, so no CLEAN_BANDS entry (as charge_me / bedtime /
   config_error). test_the_no_clock_screen_fits_the_panel keeps every line
   off the edges, since LVGL clips rather than wraps. */
void display_screens_build_no_clock(void) {
    lv_obj_t *scr = fresh_screen(false);
    make_label(scr, "No Clock", &lv_font_montserrat_28, LV_ALIGN_TOP_MID, 0, 2);
    make_label(scr, "Time not synced - check WiFi", &lv_font_montserrat_16, LV_ALIGN_TOP_MID, 0, 44);
    make_label(scr, "No screen time until it syncs", &lv_font_montserrat_12, LV_ALIGN_TOP_MID, 0, 70);
    make_label(scr, "Press D to retry", &lv_font_montserrat_16, LV_ALIGN_BOTTOM_MID, 0, -4);
}

/* ---- Battery lock (<= 10%): "Charge me!" --------------------------------

   The charge art (SPRITE_CHARGE, a 105x122 slot, tools/sprites/charge.png;
   the template is a sad battery holding its plug) sits at the left,
   vertically centred; "Charge me!" and a nearly-empty battery outline (one
   bar) fill the right column. The panel says only this until the pack
   charges back above the warn band.

   Not inverted: thin 1-3 px lines, which white-on-black shreds on this
   panel (the reason no_clock and config_error are not inverted either).
   Full refresh only, so no CLEAN_BANDS entry, as before. No new font: 48 pt
   cannot fit two lines plus the battery in 128 px, so the words use the
   28 pt already enabled.

   Margins: the bitmap is cropped tight to the ink (ink in its first and
   last row and column), so the 3 px top/bottom and 8 px left gaps come only
   from the placement; they keep the lines off the panel edge
   (test_the_charge_me_screen_fits_the_panel). */
#define CHARGE_ART_X 8
#define CHARGE_COL_X 124 /* right column: 124..287, centred on x=206 */
#define CHARGE_COL_W 164
#define CHARGE_TEXT_Y 14
#define BATT_W 56 /* body, nub excluded */
#define BATT_H 24
#define BATT_BORDER 3
#define BATT_NUB_W 5
#define BATT_NUB_H 10
#define BATT_Y 90
#define BATT_X (CHARGE_COL_X + (CHARGE_COL_W - (BATT_W + BATT_NUB_W)) / 2)

/* LV_EVENT_DRAW_MAIN handler for the sprites (the four lock-screen art
   slots: charge, time's up, break and bed time). The same shape as qr_draw_cb(): one lv_draw_rect()
   per horizontal run of ink, read straight from the RLE in flash by
   sprite.c, so no scratch bitmap is allocated and no new draw path reaches
   the I1 side. The sprite comes from the event's user data.

   Two things are per-object rather than per-sprite, so one callback serves
   black-on-white and white-on-black and 1x and 2x without a second decoder:
   the ink colour is the object's text colour (set by build_sprite(), so it
   follows the screen and is never hard-coded here) and the integer scale is
   the object's user data. Each run of len px at (x, y) becomes a
   len*scale x scale rect at (x*scale, y*scale). */
static void sprite_draw_cb(lv_event_t *e) {
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_obj_t *obj = lv_event_get_current_target(e);
    const sprite_t *spr = lv_event_get_user_data(e);
    const int32_t k = (int32_t)(intptr_t)lv_obj_get_user_data(obj);

    lv_area_t coords;
    lv_obj_get_coords(obj, &coords);

    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = lv_obj_get_style_text_color(obj, LV_PART_MAIN);
    dsc.border_width = 0;

    sprite_iter_t it;
    uint16_t x, y, len;
    sprite_iter_init(&it, spr);
    while (sprite_iter_next(&it, &x, &y, &len)) {
        lv_area_t a = {.x1 = coords.x1 + x * k,
                       .y1 = coords.y1 + y * k,
                       .x2 = coords.x1 + (x + len) * k - 1,
                       .y2 = coords.y1 + (y + 1) * k - 1};
        lv_draw_rect(layer, &dsc, &a);
    }
}

/* A plain lv_obj the size of the sprite times `scale` (an integer >= 1),
   painted in `ink` by sprite_draw_cb(); the background is transparent, so
   only the ink is drawn on whatever the screen is (white or inverted). */
static void build_sprite(lv_obj_t *scr, const sprite_t *spr, int32_t x, int32_t y, int32_t scale, lv_color_t ink) {
    if (scale < 1)
        scale = 1; /* callers pass literals; a zero would make a 0x0 box */
    lv_obj_t *o = lv_obj_create(scr);
    lv_obj_set_size(o, spr->w * scale, spr->h * scale);
    lv_obj_align(o, LV_ALIGN_TOP_LEFT, x, y);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_color(o, ink, 0);
    lv_obj_set_user_data(o, (void *)(intptr_t)scale);
    lv_obj_add_event_cb(o, sprite_draw_cb, LV_EVENT_DRAW_MAIN, (void *)spr);
}

/* A solid black rectangle, for the battery's nub and its one bar. */
static void build_block(lv_obj_t *scr, int32_t x, int32_t y, int32_t w, int32_t h) {
    lv_obj_t *o = lv_obj_create(scr);
    lv_obj_set_size(o, w, h);
    lv_obj_align(o, LV_ALIGN_TOP_LEFT, x, y);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_bg_color(o, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
}

/* The empty-ish battery: a hollow body, a nub on its right, and a single
   bar hugging the left inside edge (the charge that is left). */
static void build_empty_battery(lv_obj_t *scr, int32_t x, int32_t y) {
    lv_obj_t *body = lv_obj_create(scr);
    lv_obj_set_size(body, BATT_W, BATT_H);
    lv_obj_align(body, LV_ALIGN_TOP_LEFT, x, y);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(body, 0, 0);
    lv_obj_set_style_radius(body, 0, 0);
    lv_obj_set_style_border_width(body, BATT_BORDER, 0);
    lv_obj_set_style_border_color(body, lv_color_black(), 0);
    lv_obj_set_style_bg_color(body, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(body, LV_OPA_COVER, 0);

    build_block(scr, x + BATT_W, y + (BATT_H - BATT_NUB_H) / 2, BATT_NUB_W, BATT_NUB_H);
    build_block(scr, x + BATT_BORDER + 2, y + BATT_BORDER + 2, 8, BATT_H - 2 * (BATT_BORDER + 2));
}

void display_screens_build_charge_me(const sprite_t *art) {
    lv_obj_t *scr = fresh_screen(false);
    build_sprite(scr, art ? art : &SPRITE_CHARGE, CHARGE_ART_X, (DISP_VER - SPRITE_CHARGE.h) / 2, 1, lv_color_black());

    lv_obj_t *words =
        make_label(scr, "Charge\nme!", &lv_font_montserrat_28, LV_ALIGN_TOP_LEFT, CHARGE_COL_X, CHARGE_TEXT_Y);
    lv_obj_set_width(words, CHARGE_COL_W);
    lv_obj_set_style_text_align(words, LV_TEXT_ALIGN_CENTER, 0);

    build_empty_battery(scr, BATT_X, BATT_Y);
}

/* Bed Time lock: inverted like the break screen (night-appropriate and
   unmistakably "not in service"); stays until day rollover.

   The bed-time art (SPRITE_BEDTIME, a 29x23 slot, tools/sprites/bedtime.png;
   the template is a bed with zzz) is drawn at BEDTIME_ART_SCALE (4), WHITE
   on the black screen, at (4, 4): a 116x92 box, x 4..119, y 4..95. The
   words sit in the space to its right, a 172 px column x 120..291, "Bed Time"
   over "Goodnight!", both 28 pt (48 pt "Bed Time" beside the art overruns the
   panel). The brush-teeth line runs along the bottom, below the box. This
   screen only ever arrives through a full refresh (display_bedtime), so it
   has no CLEAN_BANDS entry. */
#define BEDTIME_ART_X 4
#define BEDTIME_ART_Y 4
#define BEDTIME_COL_X 120
#define BEDTIME_COL_W 172
#define BEDTIME_ART_SCALE 4
#define BEDTIME_TITLE_Y 10    /* "Bed Time" label top: ink rows 14..34 */
#define BEDTIME_GREETING_Y 54 /* "Goodnight!" label top: ink rows 58..83 (measured off the golden) */
#define BEDTIME_LINE_Y -2     /* bottom line: offset from the bottom edge, ink rows 108..125 */

void display_screens_build_bedtime(const sprite_t *art) {
    lv_obj_t *scr = fresh_screen(true);
    build_sprite(scr, art ? art : &SPRITE_BEDTIME, BEDTIME_ART_X, BEDTIME_ART_Y, BEDTIME_ART_SCALE, lv_color_white());

    /* Two labels, not one two-line label: the gap between the words is
       wider than the font's line height. */
    lv_obj_t *bed =
        make_label(scr, "Bed Time", &lv_font_montserrat_28, LV_ALIGN_TOP_LEFT, BEDTIME_COL_X, BEDTIME_TITLE_Y);
    lv_obj_set_width(bed, BEDTIME_COL_W);
    lv_obj_set_style_text_align(bed, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t *night =
        make_label(scr, "Goodnight!", &lv_font_montserrat_28, LV_ALIGN_TOP_LEFT, BEDTIME_COL_X, BEDTIME_GREETING_Y);
    lv_obj_set_width(night, BEDTIME_COL_W);
    lv_obj_set_style_text_align(night, LV_TEXT_ALIGN_CENTER, 0);

    /* Single centered line, 18 pt: 16 pt strokes shredded into fragments
       by the inverted 1 bpp render (no bold Montserrat exists in LVGL),
       and 20 pt overruns the 296 px panel. */
    make_label(scr, "Brush teeth | Get water bottles", &lv_font_montserrat_18, LV_ALIGN_BOTTOM_MID, 0, BEDTIME_LINE_Y);
}

/* Config-error lock (design 5.3, row C11): today's `chore_free_*` is
   larger than the allocation it is paired with, which cannot mean
   anything, so the device stops and says so.

   NOT INVERTED, unlike the bed-time lock it is otherwise shaped on, and
   the reason is the small type rather than the mood: this is the only
   lock screen carrying a 12 pt line, and 12 pt white-on-black renders
   illegibly on this panel — the inversion eats the thin strokes (the
   break screen's own comment records the same measurement). A screen
   nobody can read is a screen that cannot route the fault to the person
   who can clear it, which is the entire point of layer 3.

   THE NUMBERS COME FROM NVS, NOT FROM A VALIDATED DOCUMENT. The pairs
   this screen is asked to render are precisely the ones no setter would
   have accepted — an older firmware's value, an NVS oddity, a bug in a
   setter — so the width case is uint16_t's ceiling and not
   CFG_BOUND_CHORE_FREE_HI's 1440. Hence the cap on the pair line, and
   test_the_config_error_screen_fits_the_panel_at_its_widest, which
   renders 65535/65534 on every day type and fails if ink reaches either
   edge. LVGL clips rather than wraps, so an overrun would otherwise be
   frozen into a regenerated golden as the new correct answer.

   No CLEAN_BANDS entry is needed, for the same reason charge_me and
   bedtime need none: this screen only ever arrives through a full
   refresh. */
void display_screens_build_config_error(day_type_t day_type, uint16_t chore_free_min, uint16_t alloc_min) {
    lv_obj_t *scr = fresh_screen(false);
    char buf[64];

    make_label(scr, "Config Error", &lv_font_montserrat_28, LV_ALIGN_TOP_MID, 0, 2);

    /* NAMES THE PAIR, which is what makes the fix possible without a
       laptop and is the one thing distinguishing this from the
       config_ack entry nobody reads. Day type first because it is what
       tells a parent which of the four settings to open. */
    snprintf(buf, sizeof(buf), "%s: free %u > %u min", day_type_str(day_type), (unsigned)chore_free_min,
             (unsigned)alloc_min);
    cap_width(make_label(scr, buf, &lv_font_montserrat_16, LV_ALIGN_TOP_MID, 0, 44), CONFIG_ERR_LINE_MAX_W);

    /* The rule, in the words of the setting rather than the code: a
       parent who has never read design 3.3 still needs to know which way
       to move which number. */
    make_label(scr, "chore_free exceeds the allocation", &lv_font_montserrat_12, LV_ALIGN_TOP_MID, 0, 70);

    make_label(scr, "Fix in Home Assistant, press D", &lv_font_montserrat_16, LV_ALIGN_BOTTOM_MID, 0, -4);
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

/* ---- WiFi + MQTT provisioning plan: the setup screens --------------------

   QR arithmetic (also in main/qr_render.c, which owns the version/capacity
   half of it): QR_RENDER_MAX_MODULES (29, version 3, the version every
   real payload encodes at) at 3 px/module is an 87x87 px code; a 4-module
   quiet zone (the spec's own minimum) adds 12 px a side, for a 111x111 px
   block. 3 px per module is the most the 128 px panel allows: 4 px would
   need 148 px even with the minimum quiet zone. The block is centred with
   8 px to spare top and bottom, and sits at a 4 px left margin like every
   other screen's left-aligned content. */
#define QR_SCALE_PX 3
#define QR_QUIET_MODULES 4
#define QR_BLOCK_PX ((QR_RENDER_MAX_MODULES + 2 * QR_QUIET_MODULES) * QR_SCALE_PX)
#define QR_LEFT_MARGIN 4
#define QR_TOP_MARGIN ((DISP_VER - QR_BLOCK_PX) / 2)
#define SETUP_TEXT_X (QR_LEFT_MARGIN + QR_BLOCK_PX + 4)
#define SETUP_TEXT_MAX_W (DISP_HOR - SETUP_TEXT_X - 4)

/* The text column's rows (y of each label's top edge). The instruction and
   the address to open come first, being what the owner acts on; the AP name
   and password below them are only for a phone that will not scan. */
#define SETUP_SCAN_Y 2
#define SETUP_HOST_Y 18
#define SETUP_AP_Y 44
#define SETUP_PW_LABEL_Y 62
#define SETUP_PW_Y 78
#define SETUP_HINT_Y 108

/* LV_EVENT_DRAW_MAIN handler for the QR block created below. Draws one
   lv_draw_rect() per horizontal run of dark modules rather than one per
   module (cheaper, and the module count can reach 841 at version 3) —
   the same draw call style_bar()'s indicator already issues, so this adds
   no new draw path on the I1 side. The module count comes from
   qr_render_last_size() rather than a value captured when the lv_obj was
   built: both it and qr_render_module() below then answer from the SAME
   encode, so a second qr_render_encode() landing between build time and
   this draw (not reachable today — this project renders one screen
   synchronously, start to finish, on a single task) cannot leave this
   walking a stale size against a fresh matrix. */
static void qr_draw_cb(lv_event_t *e) {
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_obj_t *obj = lv_event_get_current_target(e);
    int size = qr_render_last_size();

    lv_area_t coords;
    lv_obj_get_coords(obj, &coords);
    int32_t ox = coords.x1 + QR_QUIET_MODULES * QR_SCALE_PX;
    int32_t oy = coords.y1 + QR_QUIET_MODULES * QR_SCALE_PX;

    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = lv_color_black();
    dsc.border_width = 0;

    for (int y = 0; y < size; y++) {
        int x = 0;
        while (x < size) {
            if (!qr_render_module(x, y)) {
                x++;
                continue;
            }
            int run_start = x;
            while (x < size && qr_render_module(x, y))
                x++;
            lv_area_t a = {.x1 = ox + run_start * QR_SCALE_PX,
                           .y1 = oy + y * QR_SCALE_PX,
                           .x2 = ox + x * QR_SCALE_PX - 1,
                           .y2 = oy + y * QR_SCALE_PX + QR_SCALE_PX - 1};
            lv_draw_rect(layer, &dsc, &a);
        }
    }
}

/* A plain lv_obj rather than LV_USE_QRCODE: that widget draws through an
   indexed-image canvas that blends through ARGB8888 before reaching this
   panel's I1 format, and sdkconfig.defaults turns ARGB8888 support off
   (the plan's "QR rendering trap"). `qr_payload` is encoded here, once;
   qr_draw_cb() above reads qr_render_module() straight out of
   qr_render.c's own result for the paint that follows. */
static void build_setup_qr(lv_obj_t *scr, const char *qr_payload) {
    int size = 0;
    if (!qr_render_encode(qr_payload, &size))
        return; /* falls back to the text column alone — no blank box drawn */

    lv_obj_t *qr = lv_obj_create(scr);
    lv_obj_set_size(qr, QR_BLOCK_PX, QR_BLOCK_PX);
    lv_obj_align(qr, LV_ALIGN_TOP_LEFT, QR_LEFT_MARGIN, QR_TOP_MARGIN);
    lv_obj_clear_flag(qr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(qr, 0, 0);
    lv_obj_set_style_radius(qr, 0, 0);
    lv_obj_set_style_border_width(qr, 0, 0);
    lv_obj_set_style_bg_color(qr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(qr, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(qr, qr_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
}

void display_screens_build_setup(const char *ap_ssid, const char *ap_password, const char *qr_payload,
                                 const char *page_host) {
    lv_obj_t *scr = fresh_screen(false);
    char buf[48];

    build_setup_qr(scr, qr_payload);

    make_label(scr, "Scan to join, then open", &lv_font_montserrat_12, LV_ALIGN_TOP_LEFT, SETUP_TEXT_X, SETUP_SCAN_Y);

    /* Larger than the lines around it: this is the one thing a phone that
       does not open the page by itself needs typed into its browser. The
       caller supplies it, so it gets the same cap_width() backstop as every
       other caller-supplied line in this tree. */
    cap_width(make_label(scr, page_host, &lv_font_montserrat_18, LV_ALIGN_TOP_LEFT, SETUP_TEXT_X, SETUP_HOST_Y),
              SETUP_TEXT_MAX_W);

    snprintf(buf, sizeof(buf), "AP: %s", ap_ssid);
    cap_width(make_label(scr, buf, &lv_font_montserrat_12, LV_ALIGN_TOP_LEFT, SETUP_TEXT_X, SETUP_AP_Y),
              SETUP_TEXT_MAX_W);

    /* The AP name and password stay as text for a phone that will not scan
       the code: join by hand, then open the address above. */
    make_label(scr, "Password:", &lv_font_montserrat_12, LV_ALIGN_TOP_LEFT, SETUP_TEXT_X, SETUP_PW_LABEL_Y);

    /* The largest compiled size that fits the column for a typical draw —
       measured, not assumed: 28 pt already clips a sample password (201 px
       natural against a 178 px column), so 18 pt is the ceiling, at 129 px
       for that same string (the column is 173 px since the QR block grew
       to 111 px, still ample). AP_PASS_ALPHABET (setup_session.c) still
       includes 'W'/'w'/'M'/'m', so a cap_width() backstop stays on this
       label the same as every other variable-content line in this tree —
       NOT because ten of the alphabet's widest glyph in a row is the only
       input that could ever clip (any draw over the column budget does,
       the same as on every other capped label here), but because 2,000,000
       sampled passwords measured zero clips, which is the sort of margin a
       geometric cap exists to hold rather than a font size to chase (see
       the OTA screen's own all-'W' case for the same trade-off). */
    cap_width(make_label(scr, ap_password, &lv_font_montserrat_18, LV_ALIGN_TOP_LEFT, SETUP_TEXT_X, SETUP_PW_Y),
              SETUP_TEXT_MAX_W);

    make_label(scr, "It may open by itself", &lv_font_montserrat_12, LV_ALIGN_TOP_LEFT, SETUP_TEXT_X, SETUP_HINT_Y);
}

/* Shown while the BOOT hold is armed; releasing now enters setup
   (setup_trigger.h's SETUP_TRIGGER_BOOT_HOLD_ARMED). Full refresh, same as
   every screen below — a hold that reaches this point is already the
   least frequent paint on the panel, so the cadence has no say over it. */
void display_screens_build_setup_release(void) {
    lv_obj_t *scr = fresh_screen(false);
    make_label(scr, "Release to", &lv_font_montserrat_28, LV_ALIGN_TOP_MID, 0, 24);
    make_label(scr, "enter setup", &lv_font_montserrat_28, LV_ALIGN_TOP_MID, 0, 64);
}

/* ---- the end-of-session screens: what will actually retry setup -------- */

/* The one spot this screen reads CONFIG_MAGTAG_BOOT_WAKES — a tiny shim
   rather than an #if inside format_setup_retry_line() below, so a host
   build (no sdkconfig.h here; the symbol is simply undefined, which an
   #if reads as 0) always exercises the off branch while that function's
   on branch stays reachable from the very same host build, by calling it
   directly with boot_wakes=true. */
static bool boot_wakes_enabled(void) {
#if CONFIG_MAGTAG_BOOT_WAKES
    return true;
#else
    return false;
#endif
}

/* SETUP_TRIGGER_BOOT_HOLD_MS as seconds, without truncating a non-whole
   value down to the second below it — truncating a 2500 ms threshold
   used to render "2 s", which a 2.0 s hold then satisfies and does
   nothing with, since the real threshold is 2.5 s. Whole seconds render
   as a bare integer; anything else gets exactly one decimal digit
   (floored, not rounded — simple, and the Kconfig default is round
   anyway, so the rounding direction is untested ground this module has
   no reason to walk into). */
static void format_hold_seconds(uint32_t hold_ms, char *out, size_t out_cap) {
    unsigned whole = hold_ms / 1000u;
    unsigned tenths = (hold_ms % 1000u) / 100u;
    if (tenths == 0)
        snprintf(out, out_cap, "%u", whole);
    else
        snprintf(out, out_cap, "%u.%u", whole, tenths);
}

/* The timeout/failed screens' second line — what will actually retry
   setup on THIS build, in THIS state, never a bare "hold BOOT" promise
   that does nothing on a build where BOOT cannot wake the device from deep
   sleep (CONFIG_MAGTAG_BOOT_WAKES off):
     no SSID      -> any press that wakes the device re-enters setup on
                     its own (setup_trigger_decide's no-SSID rule), so a
                     button press is the whole gesture.
     SSID present -> a button press alone only wakes the device. Where
                     BOOT itself can wake it (CONFIG_MAGTAG_BOOT_WAKES),
                     holding BOOT is the whole gesture. Where it cannot,
                     the owner holds BOOT FIRST and then presses a button:
                     the press wakes the device with BOOT already down, and
                     wake_flow.c takes that as the start of the hold. The
                     opposite order (press, then hold) would find nothing
                     listening, which is why the line is worded this way.
   has_wifi_ssid and boot_wakes are parameters rather than a global/#if
   read here so a host test can drive every combination directly. */
static void format_setup_retry_line(bool has_wifi_ssid, bool boot_wakes, char *out, size_t out_cap) {
    char secs[16];
    format_hold_seconds(SETUP_TRIGGER_BOOT_HOLD_MS, secs, sizeof(secs));
    if (!has_wifi_ssid)
        snprintf(out, out_cap, "Press any button to retry");
    else if (boot_wakes)
        snprintf(out, out_cap, "Hold BOOT %s s to retry", secs);
    else
        snprintf(out, out_cap, "Hold BOOT, press a button, hold %s s", secs);
}

/* The retry line is too long at 18 pt to survive a single-line clip — the
   "hold BOOT, press a button, hold" phrasing measures well past the panel
   width — so this wraps it across up to two lines inside a fixed-width,
   centred box anchored to the bottom edge, rather than cap_width()'s
   single-line LONG_CLIP (fine for the shorter caller-supplied lines
   elsewhere on this screen, wrong here: a clipped retry instruction can
   silently drop the number or the word "retry" itself). The shortest of
   the three phrasings still fits on one line; WRAP leaves a one-line
   label exactly as centred as it already was. */
static void make_retry_line(lv_obj_t *scr, const char *text) {
    lv_obj_t *lbl = make_label(scr, text, &lv_font_montserrat_18, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_width(lbl, OTA_LINE_MAX_W);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(lbl, LV_ALIGN_BOTTOM_MID, 0, -8); /* re-align: the width change above moves the auto-fit box */
}

/* The end of a session, whichever way it ended:
     WIFI_SAVED  : WiFi provisioned and verified. The first real network
                   window (NTP sync, HA discovery) is the next wake, not
                   this one, so "connecting" is accurate, not aspirational.
     MQTT_SAVED  : the device already had an SSID and only the MQTT form
                   was submitted this session — WiFi was never touched, so
                   this says so instead of claiming a WiFi join that did
                   not happen.
     TIMED_OUT   : the budget expired with nothing provisioned (or, on a
                   device that already had an SSID, nothing NEW this
                   session).
     FAILED      : a start failure, a verified join whose credential store
                   failed, or a hard error mid-session — see
                   setup_session_run()'s own doc comment for why all three
                   share this screen: by the time any of them is possible
                   the panel already showed an AP name, password and QR
                   for a session that no longer exists, so it must not be
                   left showing them. */
void display_screens_build_setup_end(display_setup_end_t kind, bool has_wifi_ssid) {
    lv_obj_t *scr = fresh_screen(false);
    char line[48];

    switch (kind) {
        case DISPLAY_SETUP_END_WIFI_SAVED:
            make_label(scr, "Setup complete", &lv_font_montserrat_28, LV_ALIGN_TOP_MID, 0, 30);
            make_label(scr, "Connecting to WiFi...", &lv_font_montserrat_18, LV_ALIGN_TOP_MID, 0, 72);
            break;
        case DISPLAY_SETUP_END_MQTT_SAVED:
            make_label(scr, "Setup complete", &lv_font_montserrat_28, LV_ALIGN_TOP_MID, 0, 30);
            make_label(scr, "MQTT broker saved", &lv_font_montserrat_18, LV_ALIGN_TOP_MID, 0, 72);
            break;
        case DISPLAY_SETUP_END_TIMED_OUT:
            make_label(scr, "Setup timed out", &lv_font_montserrat_28, LV_ALIGN_TOP_MID, 0, 20);
            format_setup_retry_line(has_wifi_ssid, boot_wakes_enabled(), line, sizeof(line));
            make_retry_line(scr, line);
            break;
        case DISPLAY_SETUP_END_FAILED:
        default:
            make_label(scr, "Setup failed", &lv_font_montserrat_28, LV_ALIGN_TOP_MID, 0, 20);
            format_setup_retry_line(has_wifi_ssid, boot_wakes_enabled(), line, sizeof(line));
            make_retry_line(scr, line);
            break;
    }
}
