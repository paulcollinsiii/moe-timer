#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#ifndef NATIVE
#include "esp_attr.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TIMER_IDLE = 0,
    TIMER_RUNNING,
    TIMER_PAUSED,
    TIMER_EXPIRED,
} timer_state_t;

typedef struct {
    timer_state_t state;
    int64_t expiry_wall_time;   /* Unix ts; 0 if unset */
    int32_t remaining_at_pause; /* seconds saved on PAUSE */
    int32_t allocation_sec;
    char last_date[11]; /* "YYYY-MM-DD\0" */
    int64_t next_ntp_sync;
    uint8_t partial_refresh_count;
} rtc_state_t;

extern rtc_state_t g_rtc_state;

timer_state_t timer_get_state(void);
void timer_start(time_t now, int32_t allocation_sec);
void timer_pause(time_t now);
void timer_resume(time_t now);
int32_t timer_tick(time_t now); /* returns remaining seconds; negative = expired */
void timer_reset(void);
bool timer_is_new_day(time_t now);
void timer_record_date(time_t now);
bool timer_needs_ntp_sync(time_t now);
void timer_record_ntp_sync(time_t now);

#ifdef __cplusplus
}
#endif
