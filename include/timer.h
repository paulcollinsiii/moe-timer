#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#ifndef NATIVE
#include "esp_attr.h"
#endif

/* RUNNING clock-recheck window: menuconfig on firmware builds; host tests
   have no sdkconfig and use the fixed fallback (pattern: nvs_defaults.h). */
#ifndef NATIVE
#include "sdkconfig.h"
#endif
#ifdef CONFIG_MAGTAG_RUNNING_SYNC_INTERVAL_MIN
#define NTP_SYNC_INTERVAL_SEC (CONFIG_MAGTAG_RUNNING_SYNC_INTERVAL_MIN * 60)
#else
#define NTP_SYNC_INTERVAL_SEC 600
#endif

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

/* Multi-timer slots (v1.3): slot 0 is the daily Screen timer (allocation
   from schedule.c, eye-rest breaks); slots 1..TIMER_EXTRA_SLOTS are plain
   user-configured countdowns (Piano, Meditation, ...). Only the ACTIVE
   slot can ever be RUNNING or BREAK — swapping requires a pause first —
   so the single-timer API below always operates on the active slot.
   To add capacity: bump TIMER_EXTRA_SLOTS, add the matching Kconfig block
   and X-macro line in main/timer_defs.c. */
#define TIMER_EXTRA_SLOTS 4
#define TIMER_SLOT_COUNT (1 + TIMER_EXTRA_SLOTS)

/* Compile-time definition of one extra timer (from menuconfig; host tests
   inject their own table via timer_set_defs). */
typedef struct {
    const char *name; /* NULL or "" = slot disabled */
    int32_t duration_sec;
    bool reloadable; /* Button B reloads without ParentTesting */
} timer_def_t;

typedef struct {
    timer_state_t state;
    int64_t expiry_wall_time;   /* Unix ts; 0 if unset */
    int32_t remaining_at_pause; /* seconds saved on PAUSE/BREAK */
    int32_t allocation_sec;
    /* Eye-rest accrual (slot 0 only): completed RUNNING seconds since last
       break/reset, plus the wall time the current run segment started. */
    int32_t run_accum_sec;
    int64_t run_started_wall;
    int64_t break_expiry_wall; /* wall time the current break ends; 0 unless BREAK */
    uint16_t completions;      /* runs that reached expiry today */
} timer_slot_state_t;

typedef struct {
    timer_slot_state_t slots[TIMER_SLOT_COUNT];
    uint8_t active_slot; /* 0 = Screen */
    char last_date[11];  /* "YYYY-MM-DD\0" */
    int64_t next_ntp_sync;
    uint8_t partial_refresh_count;
} rtc_state_t;

extern rtc_state_t g_rtc_state;

/* Persisted-to-NVS snapshot of the timer for crash/reset recovery: a panic
   or external reset wipes RTC memory, which would otherwise refund the
   day's entire allocation. Bump the version on any layout change — the
   XOR checksum (carried over from the MicroPython predecessor) then
   invalidates stale-layout blobs even if NVS hands them back intact. */
#define TIMER_SNAPSHOT_VERSION 3 /* v3: multi-timer slots + completion counters */

typedef struct {
    uint8_t state; /* timer_state_t */
    int32_t remaining_at_pause;
    int32_t allocation_sec;
    int64_t expiry_wall_time;
    int32_t run_accum_sec;
    int64_t run_started_wall;
    int64_t break_expiry_wall;
    uint16_t completions;
} timer_snapshot_slot_t;

typedef struct {
    uint8_t version;
    uint8_t active_slot;
    uint8_t checksum; /* XOR of all bytes with this field zeroed */
    timer_snapshot_slot_t slots[TIMER_SLOT_COUNT];
    char date[11]; /* day the snapshot belongs to; stale days never restore */
} timer_snapshot_t;

/* Slot management. timer_set_defs must run before any other call each boot
   (defs live in flash/rodata, not RTC memory); defs[0] (Screen) is ignored. */
void timer_set_defs(const timer_def_t *defs, int count);
int timer_active_slot(void);
/* Definition of the active slot; NULL for slot 0 (Screen uses schedule.c). */
const timer_def_t *timer_active_def(void);
int timer_extra_count(void);     /* enabled extra slots */
bool timer_any_reloadable(void); /* any enabled extra slot reloadable */
/* Cycle to the next enabled slot (0 -> 1 -> ... -> 0). Refused (false)
   while the active slot is RUNNING or BREAK, or when no extras exist. */
bool timer_select_next(void);
/* Return the active slot to IDLE at full duration, keeping its completion
   counter. Refused (false) while RUNNING. */
bool timer_reload(void);

timer_state_t timer_get_state(void);
void timer_start(time_t now, int32_t allocation_sec);
void timer_pause(time_t now);
void timer_resume(time_t now);
/* Shift a RUNNING timer's expiry after an NTP clock step (immediate-start
   flow); no-op unless RUNNING with a set expiry. */
void timer_shift_expiry(int64_t delta_sec);
int32_t timer_tick(time_t now); /* returns remaining seconds; negative = expired */
void timer_reset(void);         /* day rollover: all slots to IDLE, selection to Screen */
bool timer_is_new_day(time_t now);
void timer_record_date(time_t now);
bool timer_needs_ntp_sync(time_t now);
void timer_record_ntp_sync(time_t now);
int64_t timer_expiry_wall(void);  /* active slot's expiry wall time (0 if unset) */
uint16_t timer_completions(void); /* active slot's completed runs today */

/* Eye-rest break: accrued RUNNING seconds trigger an enforced break.
   Screen-only — timer_break_due is always false on extra slots. */
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
