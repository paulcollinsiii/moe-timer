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

void display_format_remaining(char *buf, size_t len, int32_t remaining_sec) {
    if (remaining_sec < 0)
        remaining_sec = 0;
    snprintf(buf, len, "%02ld:%02ld:%02ld", (long)(remaining_sec / 3600), (long)((remaining_sec / 60) % 60),
             (long)(remaining_sec % 60));
}
