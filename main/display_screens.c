/* LVGL screen builders (host-renderable; see display_screens.h). */
#include "display_screens.h"

#include <stdio.h>
#include <time.h>

#include "lvgl.h"

#define DISP_HOR 296

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

static lv_obj_t *fresh_screen(void) {
    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(scr, lv_color_black(), 0);
    return scr;
}

/* Inverted variant for the Screen Break layout — unmistakable at a glance */
static lv_obj_t *fresh_screen_inverted(void) {
    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(scr, lv_color_white(), 0);
    return scr;
}

/* newlib's C locale renders strftime %p empty — format 12h time manually. */
static void format_time_12h(char *buf, size_t len, const struct tm *tm) {
    int h12 = tm->tm_hour % 12;
    if (h12 == 0)
        h12 = 12;
    snprintf(buf, len, "%d:%02d %s", h12, tm->tm_min, tm->tm_hour < 12 ? "AM" : "PM");
}

static void style_bar(lv_obj_t *bar) {
    lv_obj_set_style_radius(bar, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(bar, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(bar, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 0, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(bar, lv_color_black(), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
}

void display_screens_build_main(const display_state_t *st) {
    lv_obj_t *scr = fresh_screen();
    char buf[64];
    char time_buf[16];
    struct tm tm;

    /* Row 0-18: date + time (left), last sync (right) */
    localtime_r(&st->wall_time, &tm);
    format_time_12h(time_buf, sizeof(time_buf), &tm);
    char date_buf[24];
    strftime(date_buf, sizeof(date_buf), "%a %b %d", &tm);
    snprintf(buf, sizeof(buf), "%s  %s", date_buf, time_buf);
    lv_obj_t *hdr = lv_label_create(scr);
    lv_label_set_text(hdr, buf);
    lv_obj_set_style_text_font(hdr, &lv_font_montserrat_12, 0);
    /* At 12 pt the ':' hugs the preceding digit — open it up slightly */
    lv_obj_set_style_text_letter_space(hdr, 1, 0);
    lv_obj_align(hdr, LV_ALIGN_TOP_LEFT, 4, 3);

    if (st->last_sync_time > 0) {
        struct tm ts;
        localtime_r(&st->last_sync_time, &ts);
        format_time_12h(time_buf, sizeof(time_buf), &ts);
        snprintf(buf, sizeof(buf), "Last sync: %s", time_buf);
    } else {
        snprintf(buf, sizeof(buf), "Last sync: --:--");
    }
    lv_obj_t *sync = lv_label_create(scr);
    lv_label_set_text(sync, buf);
    lv_obj_set_style_text_font(sync, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_letter_space(sync, 1, 0);
    lv_obj_align(sync, LV_ALIGN_TOP_RIGHT, -4, 3);

    /* Row 26-50: progress bar, 284x24 with 2 px border */
    lv_obj_t *bar = lv_bar_create(scr);
    lv_obj_set_size(bar, 284, 24);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 26);
    lv_bar_set_range(bar, 0, 280);
    lv_bar_set_value(bar, display_bar_fill_px(st->remaining_sec, st->allocation_sec), LV_ANIM_OFF);
    style_bar(bar);

    /* Low battery (<= 15%): badge riding the bar — white background so it
       reads over both the filled (black) and empty parts of the bar */
    if (st->charge_warn) {
        lv_obj_t *warn = lv_label_create(scr);
        lv_label_set_text(warn, "Charge Me!!!");
        lv_obj_set_style_text_font(warn, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(warn, lv_color_black(), 0);
        lv_obj_set_style_bg_color(warn, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(warn, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_hor(warn, 6, 0);
        lv_obj_align(warn, LV_ALIGN_TOP_MID, 0, 29);
    }

    /* Row 58-86: battery (left, 12 pt) + remaining time (right, 28 pt) */
    static const char *BATT_SYMS[] = {LV_SYMBOL_BATTERY_EMPTY, LV_SYMBOL_BATTERY_1, LV_SYMBOL_BATTERY_2,
                                      LV_SYMBOL_BATTERY_3, LV_SYMBOL_BATTERY_FULL};
    snprintf(buf, sizeof(buf), "%s %u%%", BATT_SYMS[display_battery_icon_level(st->battery_pct)],
             (unsigned)st->battery_pct);
    lv_obj_t *batt = lv_label_create(scr);
    lv_label_set_text(batt, buf);
    lv_obj_set_style_text_font(batt, &lv_font_montserrat_12, 0);
    lv_obj_align(batt, LV_ALIGN_TOP_LEFT, 4, 66);

    display_format_remaining(buf, sizeof(buf), st->remaining_sec);
    lv_obj_t *rem = lv_label_create(scr);
    lv_label_set_text(rem, buf);
    lv_obj_set_style_text_font(rem, &lv_font_montserrat_28, 0);
    lv_obj_align(rem, LV_ALIGN_TOP_RIGHT, -4, 58);

    /* Status row (moved up to make room for button labels): mode line
       (left: day-type + allocation, or the extra timer's name/counter),
       state (right) */
    if (st->timer_name != NULL && st->timer_name[0] != '\0') {
        display_format_mode_line(buf, sizeof(buf), st->timer_name, st->completions, st->reloadable, st->allocation_sec);
    } else {
        snprintf(buf, sizeof(buf), "%s - %u min", day_type_str(st->day_type), (unsigned)(st->allocation_sec / 60));
    }
    lv_obj_t *day = lv_label_create(scr);
    lv_label_set_text(day, buf);
    lv_obj_set_style_text_font(day, &lv_font_montserrat_12, 0);
    lv_obj_align(day, LV_ALIGN_BOTTOM_LEFT, 4, -18);

    lv_obj_t *state = lv_label_create(scr);
    lv_label_set_text(state, state_str(st->timer_state));
    lv_obj_set_style_text_font(state, &lv_font_montserrat_12, 0);
    lv_obj_align(state, LV_ALIGN_BOTTOM_RIGHT, -4, -18);

    /* Bottom edge: labels centred over the physical buttons. Calibrated
       on hardware (2026-07): button D's centre lands at screen x=239 and
       the pitch is 74 px, so the row runs 17/91/165/239 — the display
       active area is offset ~20 px relative to the button row, it is NOT
       centred over it. Tune BTN_X0 (slides row) / BTN_PITCH (stretches)
       if a future panel batch differs.
       A shows the action a press will take; B/C only when their press
       would work; D = sync. */
#define BTN_X0 17    /* screen x of button A's centre */
#define BTN_PITCH 74 /* px between adjacent button centres */
#define BTN_MID_OFS(i) (BTN_X0 + (i)*BTN_PITCH - DISP_HOR / 2)
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
        lv_obj_t *lbl_a = lv_label_create(scr);
        lv_label_set_text(lbl_a, a_sym);
        lv_obj_set_style_text_font(lbl_a, &lv_font_montserrat_12, 0);
        lv_obj_align(lbl_a, LV_ALIGN_BOTTOM_MID, BTN_MID_OFS(0), -2);
    }
    /* reload_available already folds in ParentTesting and the not-RUNNING
       rule (timer_reload_allowed) — label shows iff a press would work. */
    if (st->reload_available) {
        lv_obj_t *lbl_b = lv_label_create(scr);
        lv_label_set_text(lbl_b, "Reset");
        lv_obj_set_style_text_font(lbl_b, &lv_font_montserrat_12, 0);
        lv_obj_align(lbl_b, LV_ALIGN_BOTTOM_MID, BTN_MID_OFS(1), -2);
    }
    if (st->swap_available) {
        lv_obj_t *lbl_c = lv_label_create(scr);
        lv_label_set_text(lbl_c, LV_SYMBOL_RIGHT); /* swap timer type */
        lv_obj_set_style_text_font(lbl_c, &lv_font_montserrat_12, 0);
        lv_obj_align(lbl_c, LV_ALIGN_BOTTOM_MID, BTN_MID_OFS(2), -2);
    }
    lv_obj_t *lbl_d = lv_label_create(scr);
    lv_label_set_text(lbl_d, LV_SYMBOL_REFRESH);
    lv_obj_set_style_text_font(lbl_d, &lv_font_montserrat_12, 0);
    lv_obj_align(lbl_d, LV_ALIGN_BOTTOM_MID, BTN_MID_OFS(3), -2);
}

/* Screen Break layout (inverted): title, draining break bar, break
   countdown, and the frozen screen-time remaining as a footer. */
void display_screens_build_break(const display_state_t *st) {
    lv_obj_t *scr = fresh_screen_inverted();
    char buf[64];

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "SCREEN BREAK");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 4);

    /* Break-progress bar: white indicator draining on the black screen */
    lv_obj_t *bar = lv_bar_create(scr);
    lv_obj_set_size(bar, 284, 16);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 40);
    lv_bar_set_range(bar, 0, 280);
    lv_bar_set_value(bar, display_bar_fill_px(st->break_remaining_sec, st->break_duration_sec), LV_ANIM_OFF);
    lv_obj_set_style_radius(bar, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(bar, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(bar, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 0, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(bar, lv_color_white(), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);

    display_format_remaining(buf, sizeof(buf), st->break_remaining_sec);
    lv_obj_t *cnt = lv_label_create(scr);
    lv_label_set_text(cnt, buf);
    lv_obj_set_style_text_font(cnt, &lv_font_montserrat_48, 0);
    lv_obj_align(cnt, LV_ALIGN_TOP_MID, 0, 62);

    char rem_buf[16];
    display_format_remaining(rem_buf, sizeof(rem_buf), st->remaining_sec);
    snprintf(buf, sizeof(buf), "Timer paused - %s left", rem_buf);
    lv_obj_t *foot = lv_label_create(scr);
    lv_label_set_text(foot, buf);
    /* 12 pt renders illegibly white-on-black on e-ink (thin strokes eaten
       by the inversion) — 16 pt keeps the footer readable. */
    lv_obj_set_style_text_font(foot, &lv_font_montserrat_16, 0);
    lv_obj_align(foot, LV_ALIGN_BOTTOM_MID, 0, -4);
}

void display_screens_build_timesup(void) {
    lv_obj_t *scr = fresh_screen();

    lv_obj_t *msg = lv_label_create(scr);
    lv_label_set_text(msg, "TIME'S UP");
    lv_obj_set_style_text_font(msg, &lv_font_montserrat_48, 0);
    lv_obj_align(msg, LV_ALIGN_CENTER, 0, -10);

    /* Empty bar underneath, per spec */
    lv_obj_t *bar = lv_bar_create(scr);
    lv_obj_set_size(bar, 284, 16);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_bar_set_range(bar, 0, 280);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    style_bar(bar);
}

void display_screens_build_sync_failed(void) {
    lv_obj_t *scr = fresh_screen();
    lv_obj_t *msg = lv_label_create(scr);
    lv_label_set_text(msg, "No sync - check WiFi");
    lv_obj_set_style_text_font(msg, &lv_font_montserrat_28, 0);
    lv_obj_align(msg, LV_ALIGN_CENTER, 0, 0);
}

/* Battery lock (<= 10%): the panel says only this until the pack charges
   back above the warn band. */
void display_screens_build_charge_me(void) {
    lv_obj_t *scr = fresh_screen();
    lv_obj_t *msg = lv_label_create(scr);
    lv_label_set_text(msg, "Charge Me!");
    lv_obj_set_style_text_font(msg, &lv_font_montserrat_48, 0);
    lv_obj_align(msg, LV_ALIGN_CENTER, 0, 0);
}
