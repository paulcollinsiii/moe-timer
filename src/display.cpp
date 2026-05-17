#include "display.h"
#include <stdint.h>
#include <string.h>
#include <stdio.h>

void display_init(void) {}
void display_update(const display_state_t *state) { (void)state; }
void display_full_refresh(const display_state_t *state) { (void)state; }
void display_timesup(void) {}

uint16_t display_bar_fill_px(int32_t remaining_sec, uint32_t allocation_sec)
{
    if (allocation_sec == 0 || remaining_sec <= 0) return 0;
    if ((uint32_t)remaining_sec >= allocation_sec) return 280;
    return (uint16_t)((uint32_t)remaining_sec * 280 / allocation_sec);
}

void display_format_remaining(char *buf, size_t len, int32_t remaining_sec)
{
    if (remaining_sec <= 0) {
        snprintf(buf, len, "0 min");
        return;
    }
    if (remaining_sec < 300) {
        snprintf(buf, len, "%ld min %ld sec",
                 (long)(remaining_sec / 60), (long)(remaining_sec % 60));
    } else {
        snprintf(buf, len, "%ld min", (long)(remaining_sec / 60));
    }
}
