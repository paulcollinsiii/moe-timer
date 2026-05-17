#pragma once
#include <stdint.h>
#include <time.h>

#include "schedule.h"
#include "timer.h"

typedef struct {
    int32_t remaining_sec;
    uint32_t allocation_sec;
    timer_state_t timer_state;
    day_type_t day_type;
    time_t wall_time;
    time_t last_sync_time;
    uint8_t contrast_level; /* 0, 1, or 2 */
} display_state_t;

#ifdef __cplusplus
extern "C" {
#endif

void display_init(void);
void display_update(const display_state_t *state);       /* partial refresh */
void display_full_refresh(const display_state_t *state); /* full refresh */
void display_timesup(void);                              /* TIME'S UP full-screen layout */

/* Layout math helpers — pure C, testable on native */
uint16_t display_bar_fill_px(int32_t remaining_sec, uint32_t allocation_sec);
void display_format_remaining(char *buf, size_t len, int32_t remaining_sec);

#ifdef __cplusplus
}
#endif
