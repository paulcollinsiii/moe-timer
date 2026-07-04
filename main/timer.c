#include "timer.h"

#include <string.h>
#include <time.h>

#include "hal_time.h"

/* ---- RTC state ---- */
#ifndef NATIVE
rtc_state_t RTC_DATA_ATTR g_rtc_state;
#else
rtc_state_t g_rtc_state;
#endif

timer_state_t timer_get_state(void) {
    return g_rtc_state.state;
}

void timer_reset(void) {
    memset(&g_rtc_state, 0, sizeof(g_rtc_state));
    g_rtc_state.state = TIMER_IDLE;
}

void timer_start(time_t now, int32_t allocation_sec) {
    g_rtc_state.state = TIMER_RUNNING;
    g_rtc_state.allocation_sec = allocation_sec;
    g_rtc_state.expiry_wall_time = (int64_t)now + allocation_sec;
}

int32_t timer_tick(time_t now) {
    if (g_rtc_state.state == TIMER_PAUSED) {
        return g_rtc_state.remaining_at_pause;
    }
    if (g_rtc_state.state != TIMER_RUNNING) {
        return 0; /* IDLE or EXPIRED */
    }
    int64_t remaining = g_rtc_state.expiry_wall_time - (int64_t)now;
    if (remaining <= 0) {
        g_rtc_state.state = TIMER_EXPIRED;
        return (int32_t)remaining;
    }
    /* expiry_wall_time is NOT modified here */
    return (int32_t)remaining;
}

void timer_pause(time_t now) {
    if (g_rtc_state.state != TIMER_RUNNING)
        return; /* no-op; caller checks state */
    int64_t remaining = g_rtc_state.expiry_wall_time - (int64_t)now;
    g_rtc_state.remaining_at_pause = (remaining > 0) ? (int32_t)remaining : 0;
    g_rtc_state.expiry_wall_time = 0;
    g_rtc_state.state = TIMER_PAUSED;
}

void timer_resume(time_t now) {
    if (g_rtc_state.state != TIMER_PAUSED)
        return; /* no-op; caller checks state */
    g_rtc_state.expiry_wall_time = (int64_t)now + g_rtc_state.remaining_at_pause;
    g_rtc_state.state = TIMER_RUNNING;
}

static void fill_date(char *buf, int year, int mon, int day) {
    buf[0] = '0' + (year / 1000) % 10;
    buf[1] = '0' + (year / 100) % 10;
    buf[2] = '0' + (year / 10) % 10;
    buf[3] = '0' + year % 10;
    buf[4] = '-';
    buf[5] = '0' + mon / 10;
    buf[6] = '0' + mon % 10;
    buf[7] = '-';
    buf[8] = '0' + day / 10;
    buf[9] = '0' + day % 10;
    buf[10] = '\0';
}

bool timer_is_new_day(time_t now) {
    if (g_rtc_state.last_date[0] == '\0')
        return true;
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    char today[11];
    fill_date(today, tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday);
    return (strcmp(today, g_rtc_state.last_date) != 0);
}

void timer_record_date(time_t now) {
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    fill_date(g_rtc_state.last_date, tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday);
}

bool timer_needs_ntp_sync(time_t now) {
    if (g_rtc_state.next_ntp_sync == 0)
        return true;
    return (int64_t)now >= g_rtc_state.next_ntp_sync;
}

void timer_record_ntp_sync(time_t now) {
    g_rtc_state.next_ntp_sync = (int64_t)now + NTP_SYNC_INTERVAL_SEC;
}
