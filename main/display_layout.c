/* Pure layout math — no LVGL/ESP dependencies; host-tested via ctest. */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

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

display_btn_label_t display_button_a_label(timer_state_t state) {
    switch (state) {
        case TIMER_RUNNING:
            return DISPLAY_BTN_LABEL_PAUSE;
        case TIMER_IDLE:
        case TIMER_PAUSED:
            return DISPLAY_BTN_LABEL_PLAY;
        default:
            return DISPLAY_BTN_LABEL_NONE; /* EXPIRED: a press does nothing */
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

void display_format_mode_line(char *buf, size_t len, const char *name, uint16_t completions, bool reloadable,
                              uint32_t allocation_sec) {
    /* Completions only surface on reloadable timers — a depleted
       non-reloadable timer just shows its empty bar until rollover. */
    if (reloadable && completions > 0) {
        snprintf(buf, len, "%s (x%u) - %u min", name, (unsigned)completions, (unsigned)(allocation_sec / 60));
    } else {
        snprintf(buf, len, "%s - %u min", name, (unsigned)(allocation_sec / 60));
    }
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

void display_format_swap_hint(char *buf, size_t len, const char *name) {
    if (len == 0)
        return;
    if (name == NULL) {
        buf[0] = '\0';
        return;
    }
    size_t max = DISPLAY_SWAP_HINT_MAX;
    if (max > len - 1)
        max = len - 1;
    size_t i = 0;
    for (; i < max && name[i] != '\0'; i++)
        buf[i] = name[i];
    buf[i] = '\0';
}

void display_format_remaining(char *buf, size_t len, int32_t remaining_sec) {
    if (remaining_sec < 0)
        remaining_sec = 0;
    snprintf(buf, len, "%02ld:%02ld:%02ld", (long)(remaining_sec / 3600), (long)((remaining_sec / 60) % 60),
             (long)(remaining_sec % 60));
}
