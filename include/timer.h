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
    int32_t bonus_sec;         /* HA grant banked while IDLE; folded in at timer_start */
    int32_t bonus_applied;     /* total HA "bonus today" reconciled (idempotent target tracking) */
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
#define TIMER_SNAPSHOT_VERSION 5 /* v5: + per-slot HA daily-bonus applied tracking */

typedef struct {
    uint8_t state; /* timer_state_t */
    int32_t remaining_at_pause;
    int32_t allocation_sec;
    int64_t expiry_wall_time;
    int32_t run_accum_sec;
    int64_t run_started_wall;
    int64_t break_expiry_wall;
    uint16_t completions;
    int32_t bonus_sec;
    int32_t bonus_applied;
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
/* Firmware glue (main/timer_defs.c): installs the menuconfig-built table. */
void timer_defs_install(void);
int timer_active_slot(void);
/* Definition of the active slot; NULL for slot 0 (Screen uses schedule.c). */
const timer_def_t *timer_active_def(void);
/* Definition of any slot; NULL for slot 0, disabled, or out of range. */
const timer_def_t *timer_slot_def(int slot);
/* Slot index for a timer name; 0 for "Screen"/NULL/"", -1 if no enabled
   extra slot matches (HA grant targeting). */
int timer_slot_by_name(const char *name);
int timer_extra_count(void); /* enabled extra slots */
/* True when a Button C swap would succeed: extras exist and the active
   slot is not RUNNING/BREAK. Also gates C as an EXT1 wake source — a
   press that can only be refused must not wake the device and burn a
   full refresh. */
bool timer_swap_allowed(void);
/* Cycle to the next enabled slot (0 -> 1 -> ... -> 0). Refused (false)
   while the active slot is RUNNING or BREAK, or when no extras exist. */
bool timer_select_next(void);
/* True when a Button B press would reset the active slot: never while
   RUNNING; otherwise when the slot is reloadable or parent_testing is
   compiled in. Also gates B as an EXT1 wake source (same rationale as
   timer_swap_allowed). */
bool timer_reload_allowed(bool parent_testing);
/* Return the active slot to IDLE at full duration, keeping its completion
   counter. Refused (false) while RUNNING. */
bool timer_reload(void);
/* Grant extra seconds to a slot (HA command). IDLE banks a bonus realized
   at the next start; RUNNING/PAUSED/BREAK extend in place; EXPIRED becomes
   PAUSED holding the grant (press A to use it). Works on any slot — no now
   needed (RUNNING extends the stored wall expiry; the rest store durations). */
void timer_grant(int slot, int32_t sec);
/* Idempotent "bonus seconds today" for a slot (HA number): grants only the
   delta beyond what's already been applied today, so re-delivering the same
   retained target every wake is a no-op. Lowering the target never reclaims
   granted time. bonus_applied resets at timer_reset (day rollover). */
void timer_bonus_reconcile(int slot, int32_t target_sec);

/* Outcome of reconciling a slot against an HA config edit that changed its
   definition mid-run (timer_reconcile_def). */
typedef enum {
    TIMER_RECONCILE_NONE = 0, /* nothing state-affecting changed */
    TIMER_RECONCILE_RESET,    /* renamed/disabled: slot reset to IDLE */
    TIMER_RECONCILE_UPDATED,  /* duration delta applied in place */
    TIMER_RECONCILE_EXPIRED,  /* shrink past elapsed: slot now EXPIRED */
} timer_reconcile_t;

/* Reconcile a RUNNING/PAUSED extra slot (1..N) whose definition changed
   during a network window: rename/disable resets to IDLE (like reload);
   a duration change delta-shifts allocation and expiry/remaining so time
   already elapsed and HA grants are preserved — expiring the run when the
   new duration is already used up. Slot 0 (Screen) and IDLE/EXPIRED slots
   are never touched. was_running (nullable) reports the pre-call RUNNING
   state so the caller can chirp/alert appropriately. */
timer_reconcile_t timer_reconcile_def(int slot, const timer_def_t *old_def, const timer_def_t *new_def, time_t now,
                                      bool *was_running);

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
/* Last recorded sync, derived from next_ntp_sync (single RTC source).
   0 = none since RTC loss or day rollover. */
time_t timer_last_ntp_sync(void);
/* Revert selection to Screen (slot 0) when the active slot's definition
   is disabled (snapshot restore, or a config edit mid-window). */
void timer_ensure_active_slot_enabled(void);
int64_t timer_expiry_wall(void);  /* active slot's expiry wall time (0 if unset) */
uint16_t timer_completions(void); /* active slot's completed runs today */
/* Read-only per-slot views (stats/summary builders): out-of-range slots
   read as IDLE / 0. */
timer_state_t timer_slot_state(int slot);
int32_t timer_slot_allocation(int slot);
uint16_t timer_slot_completions(int slot);
int32_t timer_screen_bonus_applied(void); /* slot 0 HA bonus reconciled today */
const char *timer_current_date(void);     /* "YYYY-MM-DD"; "" until first record */
/* Display-facing remaining seconds for any slot, without ticking (no state
   change): RUNNING = expiry-now, PAUSED/BREAK = frozen remaining, IDLE =
   idle_fallback (caller's allocation), EXPIRED = 0; clamped >= 0. */
int32_t timer_slot_remaining(int slot, time_t now, int32_t idle_fallback);
/* Screen-timer (slot 0) seconds consumed today: allocation - remaining. */
int32_t timer_screen_used_sec(time_t now);

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
