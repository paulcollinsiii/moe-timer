/* Pure layout math — no LVGL/ESP dependencies; host-tested via ctest. */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define BAR_FILL_MAX_PX 280u

uint16_t display_bar_fill_px(int32_t remaining_sec, uint32_t allocation_sec) {
    if (allocation_sec == 0 || remaining_sec <= 0)
        return 0;
    if ((uint32_t)remaining_sec >= allocation_sec)
        return BAR_FILL_MAX_PX;
    return (uint16_t)((uint32_t)remaining_sec * BAR_FILL_MAX_PX / allocation_sec);
}

void display_format_remaining(char *buf, size_t len, int32_t remaining_sec) {
    if (remaining_sec <= 0) {
        snprintf(buf, len, "0 min");
        return;
    }
    if (remaining_sec < 300) {
        snprintf(buf, len, "%ld min %ld sec", (long)(remaining_sec / 60), (long)(remaining_sec % 60));
    } else {
        snprintf(buf, len, "%ld min", (long)(remaining_sec / 60));
    }
}
