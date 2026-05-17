#include "timer.h"

#include <string.h>
#include <time.h>

#include "hal_time.h"

#ifndef NATIVE
rtc_state_t RTC_DATA_ATTR g_rtc_state;
#else
rtc_state_t g_rtc_state;
#endif

timer_state_t timer_get_state(void) {
    return g_rtc_state.state;
}
void timer_start(time_t now, int32_t allocation_sec) {
    (void)now;
    (void)allocation_sec;
}
void timer_pause(time_t now) {
    (void)now;
}
void timer_resume(time_t now) {
    (void)now;
}
int32_t timer_tick(time_t now) {
    (void)now;
    return 0;
}
void timer_reset(void) {
    memset(&g_rtc_state, 0, sizeof(g_rtc_state));
}
bool timer_is_new_day(time_t now) {
    (void)now;
    return false;
}
void timer_record_date(time_t now) {
    (void)now;
}
bool timer_needs_ntp_sync(time_t now) {
    (void)now;
    return false;
}
void timer_record_ntp_sync(time_t now) {
    (void)now;
}
