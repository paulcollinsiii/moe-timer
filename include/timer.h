#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#ifndef NATIVE
#include "esp_attr.h"
#endif

#define NTP_SYNC_INTERVAL_SEC 600 /* 10-minute recheck window */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TIMER_IDLE = 0,
    TIMER_RUNNING,
    TIMER_PAUSED,
    TIMER_EXPIRED,
    TIMER_BREAK, /* appended (=4): snapshots store state as uint8 */
} timer_state_t;

typedef struct {
    timer_state_t state;
    int64_t expiry_wall_time;   /* Unix ts; 0 if unset */
    int32_t remaining_at_pause; /* seconds saved on PAUSE/BREAK */
    int32_t allocation_sec;
    char last_date[11]; /* "YYYY-MM-DD\0" */
    int64_t next_ntp_sync;
    uint8_t partial_refresh_count;
    /* Eye-rest accrual: completed RUNNING seconds since last break/reset,
       plus the wall time the current run segment started (0 unless RUNNING). */
    int32_t run_accum_sec;
    int64_t run_started_wall;
    int64_t break_expiry_wall; /* wall time the current break ends; 0 unless BREAK */
} rtc_state_t;

extern rtc_state_t g_rtc_state;

/* Persisted-to-NVS snapshot of the timer for crash/reset recovery: a panic
   or external reset wipes RTC memory, which would otherwise refund the
   day's entire allocation. Bump the version on any layout change — the
   XOR checksum (carried over from the MicroPython predecessor) then
   invalidates stale-layout blobs even if NVS hands them back intact. */
#define TIMER_SNAPSHOT_VERSION 1

typedef struct {
    uint8_t version;
    uint8_t state;    /* timer_state_t */
    uint8_t checksum; /* XOR of all bytes with this field zeroed */
    int32_t remaining_at_pause;
    int32_t allocation_sec;
    int64_t expiry_wall_time;
    char date[11]; /* day the snapshot belongs to; stale days never restore */
} timer_snapshot_t;

timer_state_t timer_get_state(void);
void timer_start(time_t now, int32_t allocation_sec);
void timer_pause(time_t now);
void timer_resume(time_t now);
/* Shift a RUNNING timer's expiry after an NTP clock step (immediate-start
   flow); no-op unless RUNNING with a set expiry. */
void timer_shift_expiry(int64_t delta_sec);
int32_t timer_tick(time_t now); /* returns remaining seconds; negative = expired */
void timer_reset(void);
bool timer_is_new_day(time_t now);
void timer_record_date(time_t now);
bool timer_needs_ntp_sync(time_t now);
void timer_record_ntp_sync(time_t now);

/* Eye-rest break: accrued RUNNING seconds trigger an enforced break. */
int32_t timer_run_accum(time_t now);                      /* accum incl. current run segment */
bool timer_break_due(time_t now, int32_t interval_sec);   /* RUNNING && accum >= interval */
void timer_start_break(time_t now, int32_t duration_sec); /* RUNNING->BREAK; freezes remaining */
int32_t timer_break_remaining(time_t now);                /* BREAK: seconds left, else 0 */

/* Crash recovery: capture g_rtc_state into a snapshot / restore it when the
   snapshot validates (version, checksum, plausibility) AND its date is
   still today (returns false otherwise, state untouched). */
void timer_make_snapshot(timer_snapshot_t *out);
bool timer_restore_snapshot(const timer_snapshot_t *snap, time_t now);
uint8_t timer_snapshot_checksum(const timer_snapshot_t *snap);

#ifdef __cplusplus
}
#endif
