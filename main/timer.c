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
    g_rtc_state.run_accum_sec = 0;
    g_rtc_state.run_started_wall = (int64_t)now;
}

int32_t timer_tick(time_t now) {
    if (g_rtc_state.state == TIMER_BREAK) {
        if ((int64_t)now >= g_rtc_state.break_expiry_wall) {
            g_rtc_state.state = TIMER_PAUSED; /* break over — wait for manual resume */
            g_rtc_state.break_expiry_wall = 0;
        }
        return g_rtc_state.remaining_at_pause; /* screen-time stays frozen */
    }
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

/* Fold the current run segment into the accrual counter. */
static void fold_run_segment(time_t now) {
    if (g_rtc_state.run_started_wall != 0) {
        int64_t seg = (int64_t)now - g_rtc_state.run_started_wall;
        if (seg > 0)
            g_rtc_state.run_accum_sec += (int32_t)seg;
        g_rtc_state.run_started_wall = 0;
    }
}

void timer_pause(time_t now) {
    if (g_rtc_state.state != TIMER_RUNNING)
        return; /* no-op; caller checks state */
    int64_t remaining = g_rtc_state.expiry_wall_time - (int64_t)now;
    g_rtc_state.remaining_at_pause = (remaining > 0) ? (int32_t)remaining : 0;
    g_rtc_state.expiry_wall_time = 0;
    fold_run_segment(now);
    g_rtc_state.state = TIMER_PAUSED;
}

void timer_resume(time_t now) {
    if (g_rtc_state.state != TIMER_PAUSED)
        return; /* no-op; caller checks state */
    g_rtc_state.expiry_wall_time = (int64_t)now + g_rtc_state.remaining_at_pause;
    g_rtc_state.run_started_wall = (int64_t)now;
    g_rtc_state.state = TIMER_RUNNING;
}

int32_t timer_run_accum(time_t now) {
    int32_t accum = g_rtc_state.run_accum_sec;
    if (g_rtc_state.state == TIMER_RUNNING && g_rtc_state.run_started_wall != 0) {
        int64_t seg = (int64_t)now - g_rtc_state.run_started_wall;
        if (seg > 0)
            accum += (int32_t)seg;
    }
    return accum;
}

bool timer_break_due(time_t now, int32_t interval_sec) {
    if (g_rtc_state.state != TIMER_RUNNING || interval_sec <= 0)
        return false;
    return timer_run_accum(now) >= interval_sec;
}

void timer_start_break(time_t now, int32_t duration_sec) {
    if (g_rtc_state.state != TIMER_RUNNING)
        return;
    int64_t remaining = g_rtc_state.expiry_wall_time - (int64_t)now;
    g_rtc_state.remaining_at_pause = (remaining > 0) ? (int32_t)remaining : 0;
    g_rtc_state.expiry_wall_time = 0;
    g_rtc_state.run_accum_sec = 0; /* fresh 30-min window after the break */
    g_rtc_state.run_started_wall = 0;
    g_rtc_state.break_expiry_wall = (int64_t)now + duration_sec;
    g_rtc_state.state = TIMER_BREAK;
}

int32_t timer_break_remaining(time_t now) {
    if (g_rtc_state.state != TIMER_BREAK)
        return 0;
    int64_t remaining = g_rtc_state.break_expiry_wall - (int64_t)now;
    return (remaining > 0) ? (int32_t)remaining : 0;
}

void timer_shift_expiry(int64_t delta_sec) {
    /* An NTP sync may step time(NULL); every stored WALL time must step by
       the same amount so stored durations are preserved. PAUSED stores a
       duration — no shift. */
    if (g_rtc_state.state == TIMER_RUNNING && g_rtc_state.expiry_wall_time != 0) {
        g_rtc_state.expiry_wall_time += delta_sec;
        if (g_rtc_state.run_started_wall != 0)
            g_rtc_state.run_started_wall += delta_sec;
    } else if (g_rtc_state.state == TIMER_BREAK && g_rtc_state.break_expiry_wall != 0) {
        g_rtc_state.break_expiry_wall += delta_sec;
    }
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

/* ---- crash-recovery snapshot ---- */

/* Widest plausible expiry horizon (also bounds allocation): corrupt data
   that slips past the checksum still cannot restore a nonsense timer. */
#define SNAPSHOT_MAX_HORIZON_SEC (7 * 86400)

uint8_t timer_snapshot_checksum(const timer_snapshot_t *snap) {
    timer_snapshot_t tmp = *snap;
    tmp.checksum = 0;
    const uint8_t *p = (const uint8_t *)&tmp;
    uint8_t x = 0;
    for (size_t i = 0; i < sizeof(tmp); i++)
        x ^= p[i];
    return x;
}

void timer_make_snapshot(timer_snapshot_t *out) {
    memset(out, 0, sizeof(*out));
    out->version = TIMER_SNAPSHOT_VERSION;
    out->state = (uint8_t)g_rtc_state.state;
    out->remaining_at_pause = g_rtc_state.remaining_at_pause;
    out->allocation_sec = g_rtc_state.allocation_sec;
    out->expiry_wall_time = g_rtc_state.expiry_wall_time;
    out->run_accum_sec = g_rtc_state.run_accum_sec;
    out->run_started_wall = g_rtc_state.run_started_wall;
    out->break_expiry_wall = g_rtc_state.break_expiry_wall;
    memcpy(out->date, g_rtc_state.last_date, sizeof(out->date));
    out->checksum = timer_snapshot_checksum(out);
}

static bool snapshot_valid(const timer_snapshot_t *snap, time_t now) {
    if (snap->version != TIMER_SNAPSHOT_VERSION)
        return false;
    /* All-zeros XORs to 0 — indistinguishable from blank storage */
    if (snap->state == 0 && snap->expiry_wall_time == 0 && snap->date[0] == '\0')
        return false;
    if (timer_snapshot_checksum(snap) != snap->checksum)
        return false;
    if (snap->state > TIMER_BREAK)
        return false;
    if (snap->allocation_sec < 0 || snap->allocation_sec > SNAPSHOT_MAX_HORIZON_SEC)
        return false;
    if (snap->remaining_at_pause < 0 || snap->remaining_at_pause > snap->allocation_sec)
        return false;
    if (snap->state == TIMER_RUNNING) {
        int64_t delta = snap->expiry_wall_time - (int64_t)now;
        if (delta > SNAPSHOT_MAX_HORIZON_SEC || delta < -SNAPSHOT_MAX_HORIZON_SEC)
            return false;
    }
    if (snap->state == TIMER_BREAK) {
        int64_t delta = snap->break_expiry_wall - (int64_t)now;
        if (delta > SNAPSHOT_MAX_HORIZON_SEC || delta < -SNAPSHOT_MAX_HORIZON_SEC)
            return false;
    }
    return true;
}

bool timer_restore_snapshot(const timer_snapshot_t *snap, time_t now) {
    if (!snapshot_valid(snap, now))
        return false;
    /* Stale day: never restore yesterday's timer (rollover will reset) */
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    char today[11];
    fill_date(today, tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday);
    if (strcmp(today, snap->date) != 0)
        return false;

    g_rtc_state.state = (timer_state_t)snap->state;
    g_rtc_state.remaining_at_pause = snap->remaining_at_pause;
    g_rtc_state.allocation_sec = snap->allocation_sec;
    g_rtc_state.expiry_wall_time = snap->expiry_wall_time;
    g_rtc_state.run_accum_sec = snap->run_accum_sec;
    g_rtc_state.run_started_wall = snap->run_started_wall;
    g_rtc_state.break_expiry_wall = snap->break_expiry_wall;
    memcpy(g_rtc_state.last_date, snap->date, sizeof(g_rtc_state.last_date));
    /* Expiry passed while powered off (snapshot saved before the EXPIRED
       transition landed): restore directly as EXPIRED so the next tick
       does not re-transition and re-fire the already-heard alert. */
    if (g_rtc_state.state == TIMER_RUNNING && g_rtc_state.expiry_wall_time <= (int64_t)now) {
        g_rtc_state.state = TIMER_EXPIRED;
    }
    /* Break finished while powered off: restore as PAUSED (manual resume) */
    if (g_rtc_state.state == TIMER_BREAK && g_rtc_state.break_expiry_wall <= (int64_t)now) {
        g_rtc_state.state = TIMER_PAUSED;
        g_rtc_state.break_expiry_wall = 0;
    }
    return true;
}
