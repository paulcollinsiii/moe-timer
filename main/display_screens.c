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
           this panel. Stays inside rows 3..23 = the header CLEAN_BAND's
           framebuffer bytes 0..2 (see display.c) — pinned by a render
           test, because a taller chip would share byte 3 with the
           progress-bar band and the two inversions would cancel. */
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

/* Row 26-50: progress bar (+ low-battery badge riding it) */
static void build_main_bar(lv_obj_t *scr, const display_state_t *st) {
    lv_obj_t *bar = lv_bar_create(scr);
    lv_obj_set_size(bar, 284, 24);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 26);
    lv_bar_set_range(bar, 0, 280);
    lv_bar_set_value(bar, display_bar_fill_px(st->remaining_sec, st->allocation_sec), LV_ANIM_OFF);
    style_bar(bar, false);

    /* Low battery (<= 15%): badge riding the bar — white background so it
       reads over both the filled (black) and empty parts of the bar */
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
    snprintf(buf, sizeof(buf), "%s %u%%", BATT_SYMS[display_battery_icon_level(st->battery_pct)],
             (unsigned)st->battery_pct);
    make_label(scr, buf, &lv_font_montserrat_12, LV_ALIGN_TOP_LEFT, 4, 66);

    display_format_remaining(buf, sizeof(buf), st->remaining_sec);
    make_label(scr, buf, &lv_font_montserrat_28, LV_ALIGN_TOP_RIGHT, -4, 58);

    /* Status row (moved up to make room for button labels): mode line
       (left: day-type + allocation, or the extra timer's name/counter),
       state (right) */
    if (st->timer_name != NULL && st->timer_name[0] != '\0') {
        display_format_mode_line(buf, sizeof(buf), st->timer_name, st->completions, st->reloadable, st->allocation_sec);
    } else {
        snprintf(buf, sizeof(buf), "%s - %u min", day_type_str(st->day_type), (unsigned)(st->allocation_sec / 60));
    }
    make_label(scr, buf, &lv_font_montserrat_12, LV_ALIGN_BOTTOM_LEFT, 4, -18);

    make_label(scr, state_str(st->timer_state), &lv_font_montserrat_12, LV_ALIGN_BOTTOM_RIGHT, -4, -18);
}

/* Button-label row along the bottom edge (geometry: see BTN_X0/BTN_PITCH).
   A shows the action a press will take; B/C only when their press would
   work; D = sync. */
static void build_button_row(lv_obj_t *scr, const display_state_t *st) {
    const char *a_sym = NULL;
    switch (display_button_a_label(st->timer_state)) {
        case DISPLAY_BTN_LABEL_PLAY:
            a_sym = LV_SYMBOL_PLAY;
            break;
        case DISPLAY_BTN_LABEL_PAUSE:
            a_sym = LV_SYMBOL_PAUSE;
            break;
        default:
            break;
    }
    if (a_sym) {
        make_label(scr, a_sym, &lv_font_montserrat_12, LV_ALIGN_BOTTOM_MID, BTN_MID_OFS(0), -2);
    }
    /* reload_available already folds in ParentTesting and the not-RUNNING
       rule (timer_reload_allowed) — label shows iff a press would work. */
    if (st->reload_available) {
        make_label(scr, "Reset", &lv_font_montserrat_12, LV_ALIGN_BOTTOM_MID, BTN_MID_OFS(1), -2);
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
       break, and the row has three items to fit). */
    display_format_hm(rem_buf, sizeof(rem_buf), st->remaining_sec);
    snprintf(buf, sizeof(buf), "Screen %s", rem_buf);
    make_label(scr, buf, &lv_font_montserrat_16, LV_ALIGN_BOTTOM_LEFT, 4, -2);

    /* Over button C: the swap affordance. Button A stays deliberately
       unlabelled — the break is still enforced for the Screen timer. */
    char hint[DISPLAY_SWAP_HINT_MAX + 1];
    display_format_swap_hint(hint, sizeof(hint), st->swap_next_name);
    snprintf(buf, sizeof(buf), "%s %s", LV_SYMBOL_RIGHT, hint);
    make_label(scr, buf, &lv_font_montserrat_16, LV_ALIGN_BOTTOM_MID, BTN_MID_OFS(2), -2);

    /* Over button D: the same refresh symbol as the main layout. */
    make_label(scr, LV_SYMBOL_REFRESH, &lv_font_montserrat_16, LV_ALIGN_BOTTOM_MID, BTN_MID_OFS(3), -2);
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
