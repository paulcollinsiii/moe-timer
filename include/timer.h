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
   user-configured countdowns (Piano, Meditation, ...).

   State-model invariant (v1.4, non-blocking breaks):
     - only the ACTIVE slot can ever be RUNNING — swapping requires a
       pause first, so the single-timer API below operates on the active
       slot;
     - TIMER_BREAK lives on slot 0 ONLY, and may be held there while ANY
       slot is active. A Screen Break enforces the SCREEN timer (no early
       resume, screen time frozen, absolute wall-clock end) without
       freezing the device: Button C stays live and the selected extra
       timer starts/pauses/expires normally behind the break.

   Every break helper therefore reads slot 0 explicitly, never the active
   slot. Callers that need the break-end EDGE (chime, snap back to Screen)
   must call timer_break_tick() BEFORE timer_tick(): timer_tick() also
   ends an elapsed break, but silently, so no path can strand one.

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
    /* "This activity is time away from a screen." One property, two
       consequences: the timer may be STARTED during a Screen Break, and
       its RUNNING time DRAINS the exposure balance instead of feeding it.
       Slot 0 (Screen) is permanently false — screen time is the original
       non-eligible activity, which is what collapses the eye-rest counter
       and the break rules into one signed balance (see I8). */
    bool break_eligible;
} timer_def_t;

typedef struct {
    timer_state_t state;
    int64_t expiry_wall_time;   /* Unix ts; 0 if unset */
    int32_t remaining_at_pause; /* seconds saved on PAUSE/BREAK */
    int32_t allocation_sec;
    /* Screen-exposure balance — SLOT 0 ONLY, whichever slot is running.
       run_accum_sec is the folded balance since the last break/reset;
       run_started_wall is the wall time the live run segment started (0 =
       nothing running). The SIGN is derived at fold time from the
       break_eligible of the slot that ARMED the segment
       (rtc_state_t.run_segment_slot) — never from the selection, which
       can move underneath a run. So the direction itself is not stored,
       only which slot owns it, and it cannot desynchronise from the defs
       table. The extras' own copies of these two fields are unused. */
    int32_t run_accum_sec;
    int64_t run_started_wall;
    int64_t break_expiry_wall; /* wall time the current break ends; 0 unless BREAK */
    uint16_t completions;      /* runs that reached expiry today */
    int32_t bonus_sec;         /* HA grant banked while IDLE; folded in at timer_start */
    int32_t bonus_applied;     /* total HA "bonus today" reconciled (idempotent target tracking) */
} timer_slot_state_t;

/* Self-validation for rtc_state_t, and the one path it exists for.
 *
 * RTC memory normally comes back one of two ways, and BOTH are already
 * safe. A deep-sleep wake preserves the .rtc.data segment intact. Every
 * other reset — panic, EN, esp_restart — RELOADS that segment from the
 * image, so the struct comes back zeroed, `last_date` is empty, and
 * timer_persist_try_restore() takes over from NVS. Neither needs a magic.
 *
 * The path that does is the OTA commit's worst case, and it is the only
 * one in the firmware that can hand this struct GARBAGE rather than
 * zeros. If the awake failsafe fires between esp_ota_set_boot_partition()
 * and esp_restart(), the device deep-sleeps and the NEXT boot is a
 * deep-sleep wake OF THE NEW IMAGE — and deep-sleep wake is precisely the
 * reset for which the bootloader SKIPS loading the RTC segments
 * (esp_image_format.c: load_rtc_memory is false only for
 * RESET_REASON_CORE_DEEP_SLEEP). The new image then reads the old
 * image's .rtc.data bytes at its own offsets: a plausible-looking
 * `last_date`, a running slot with a nonsense expiry, whatever the layout
 * drift produces.
 *
 * ota_flow.c closes that race by re-arming the failsafe before the
 * commit. This is the belt to that braces: cheap (8 bytes of the 3 KB
 * free in RTC slow), and the only validation this struct has at all —
 * the NVS snapshot has a version, a checksum AND range checks, and this
 * had nothing.
 *
 * BUMP THE VERSION on any layout change to rtc_state_t, for the same
 * reason TIMER_SNAPSHOT_VERSION is bumped: two builds that agree on the
 * magic but disagree on the offsets behind it are exactly the case the
 * magic alone cannot catch. */
#define RTC_STATE_MAGIC 0x4D414754u /* "MAGT", legible in a memory dump */
#define RTC_STATE_VERSION 1

typedef struct {
    /* First two fields, deliberately: a struct whose head is its own
       identity is the one that survives being read by the wrong build. */
    uint32_t magic;
    uint16_t version;
    timer_slot_state_t slots[TIMER_SLOT_COUNT];
    uint8_t active_slot; /* 0 = Screen */
    /* Slot that was selected when the current break started. The selection
       snaps to 0 at break entry, so nothing else records what the break
       interrupted; break end returns to it (rule 8). */
    uint8_t break_interrupted_slot;
    /* Slot 0's state at break entry (after the forced pause), restored at
       break end (I7). Stored rather than derived: every derivation from
       remaining_at_pause / allocation_sec refunds the day somewhere — an
       HA deduction landing mid-break reads as "never started", and an
       EXPIRED slot 0 reads as whatever was banked before it expired. */
    uint8_t break_prev_state;
    /* Slot that ARMED the live run segment on slot 0. The segment's sign
       is a property of the run, not of the selection — which can move
       underneath it (timer_ensure_active_slot_enabled on a restore) and
       would otherwise invert a drain into an accrual. */
    uint8_t run_segment_slot;
    char last_date[11]; /* "YYYY-MM-DD\0" */
    int64_t next_ntp_sync;
} rtc_state_t;

extern rtc_state_t g_rtc_state;

/* Validate g_rtc_state's magic and version, zeroing and re-stamping the
   struct when either is wrong. Answers whether it had to.

   Call once per boot, before anything reads the timer state and in
   particular before timer_persist_try_restore() — zeroing is what makes
   `last_date` empty, which is that function's entire cue to fall back to
   the NVS snapshot. So a wiped or foreign RTC image degrades to the
   already-tested "restore from NVS" path rather than to a garbage day.

   Answering TRUE is NOT an anomaly by itself: it is the normal answer on
   a cold boot and on every esp_restart, where the segment is reloaded
   from the image as zeros. It is only interesting on a deep-sleep wake,
   which is the case described above rtc_state_t. */
bool timer_rtc_state_guard(void);

/* Persisted-to-NVS snapshot of the timer for crash/reset recovery: a panic
   or external reset wipes RTC memory, which would otherwise refund the
   day's entire allocation. Bump the version on any layout change — the
   XOR checksum (carried over from the MicroPython predecessor) then
   invalidates stale-layout blobs even if NVS hands them back intact. */
#define TIMER_SNAPSHOT_VERSION 6 /* v6: + break_interrupted_slot, break_prev_state, run_segment_slot */

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
    uint8_t break_interrupted_slot;
    uint8_t break_prev_state;
    uint8_t run_segment_slot;
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
/* Enabled extra slots marked break_eligible — i.e. slots a Screen Break
   would actually let you start. 0 means the break offers nothing, which is
   what suppresses the break screen's swap hint. */
int timer_eligible_extra_count(void);
/* Is `slot` a genuine break activity? Slot 0 (Screen), disabled and
   out-of-range slots are always false. */
bool timer_slot_break_eligible(int slot);
/* True when Button A may START or RESUME the active slot: refused only
   while a Screen Break runs on slot 0 and the active slot is not
   break_eligible (rule 7). Slot 0 is never eligible, so this also carries
   the existing "the break screen has no play glyph" behaviour. Pausing is
   never gated — it is always safe to stop. */
bool timer_start_allowed(void);
/* True when a Button C swap would succeed: extras exist and the active
   slot is not RUNNING. A background Screen Break does NOT refuse — going
   and running Piano is exactly what the break time is for. Also gates C
   as an EXT1 wake source (rebuilt at every sleep entry) — a press that
   can only be refused must not wake the device and burn a full refresh. */
bool timer_swap_allowed(void);
/* Cycle to the next enabled slot (0 -> 1 -> ... -> 0). Refused (false)
   while the active slot is RUNNING, or when no extras exist. */
bool timer_select_next(void);
/* The slot Button C would land on (next enabled slot, wrapping through
   0 = Screen); -1 when no other slot is enabled. Answers "which one",
   not "may I" — pair with timer_swap_allowed() for the latter. */
int timer_next_slot(void);
/* Put the selection back on slot 0 (Screen). Refused (false) only while
   the active extra slot is RUNNING — stealing the selection mid-run
   would be hostile. Already on Screen: true, no change. */
bool timer_select_screen(void);
/* Put the selection back on the slot the break interrupted (rule 8).
   Refused (false) while the active slot is RUNNING — the same "never steal
   the selection mid-run" rule as timer_select_screen. Falls back to slot 0
   when the recorded slot is no longer enabled. */
bool timer_select_interrupted(void);
/* Slot recorded at the last break start (0 until one happens). */
int timer_break_interrupted_slot(void);
/* Any extra slot (1..N) RUNNING? Suppresses the break-over chime and the
   snap back to Screen, and tells the sleep planner the break end needs no
   dedicated wake. */
bool timer_any_extra_running(void);
/* True when a Button B press would reset the active slot: never while
   RUNNING; otherwise when the slot is reloadable or parent_testing is
   compiled in. Also gates B as an EXT1 wake source (same rationale as
   timer_swap_allowed). */
bool timer_reload_allowed(bool parent_testing);
/* Return the active slot to IDLE at full duration, keeping its completion
   counter. Refused (false) while RUNNING. */
bool timer_reload(void);
/* Adjust a slot by signed seconds (HA command): positive grants extra
   time, negative takes it back ("chores not done"). IDLE banks the
   adjustment, realized at the next start (clamped at an empty start);
   RUNNING/PAUSED/BREAK adjust in place — a deduction that empties a
   PAUSED timer expires it (same contract as timer_reconcile_def), an
   emptied RUNNING timer expires on its next tick, a BREAK keeps its
   frozen zero until the post-break resume. EXPIRED: a grant becomes
   PAUSED holding it (press A to use it); a deduction is a no-op. Works
   on any slot — no now needed (RUNNING adjusts the stored wall expiry;
   the rest store durations). */
void timer_adjust(int slot, int32_t sec);
/* Idempotent signed "adjustment seconds today" for a slot (HA number):
   applies only the delta beyond what's already been applied today, in
   either direction, so re-delivering the same retained target every wake
   is a no-op. bonus_applied resets at timer_reset (day rollover). */
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

/* Eye-rest break, driven by the SCREEN-EXPOSURE BALANCE. Everything below
   reads/writes SLOT 0 regardless of which slot is selected, but the
   balance is fed by whichever slot is RUNNING:

     I8  the balance moves iff a slot is RUNNING — up 1:1 when that slot is
         not break_eligible, down 1:1 when it is. Floored at zero (no
         banked credit), frozen while nothing runs.
     I9  slot 0's run_accum_sec is reset by exactly two things: break start
         and day rollover. Draining to zero is arithmetic, not a reset.
     I10 every transition into or out of RUNNING folds the live segment at
         the OLD direction before re-arming at the new one — which is what
         lets the segment carry only its owning slot rather than a
         direction bit that could contradict the defs table. */
int32_t timer_run_accum(time_t now); /* balance incl. the live segment; >= 0 */
/* True when the balance has reached the interval. Not gated on RUNNING
   or on slot 0's state: the balance can cross while Screen is IDLE,
   PAUSED or EXPIRED and a non-eligible extra drives it — eye rest is
   about exposure, so it must keep working after the day's screen
   allocation is spent. Refused only while a break is already running. */
bool timer_break_due(time_t now, int32_t interval_sec);
/* Start a break on slot 0: pauses whatever is RUNNING (I6), records the
   interrupted slot, resets the balance, snaps the selection to slot 0. */
void timer_start_break(time_t now, int32_t duration_sec);
bool timer_break_active(void);             /* slot 0 == TIMER_BREAK */
int32_t timer_break_remaining(time_t now); /* slot 0 BREAK: seconds left, else 0 */
/* End an elapsed break: slot 0 returns to the state it was in when the
   break started (I7), which may be IDLE — Screen need never have been
   started, the break having been earned entirely by a non-eligible
   extra. break_expiry_wall cleared.

   The transition is LATCHED, not returned. timer_tick() calls this
   internally so no path can strand a break — which means any tick could
   otherwise consume the edge and silently lose the chime (it did: the
   expiry alert's tick swallowed it). Latching removes the ordering
   obligation entirely: tick whenever you like, drain whenever you like. */
void timer_break_tick(time_t now);

/* Consume the latched break-end transition. Returns true exactly once
   per transition — the single owner of the chime + snap-back decision.
   *overdue_sec (nullable) is how late THIS call is relative to the
   break's WALL end, i.e. how late the user is being told, not how late
   the tick was; 0 when nothing was latched. Feed it to
   wake_policy_break_chime(). Cleared by timer_reset (day rollover); a
   snapshot restore of an already-elapsed break never latches, since that
   path is silent by design. */
bool timer_break_take_ended(time_t now, int32_t *overdue_sec);

/* Crash recovery: capture g_rtc_state into a snapshot / restore it when the
   snapshot validates (version, checksum, plausibility) AND its date is
   still today (returns false otherwise, state untouched). */
void timer_make_snapshot(timer_snapshot_t *out);
bool timer_restore_snapshot(const timer_snapshot_t *snap, time_t now);
uint8_t timer_snapshot_checksum(const timer_snapshot_t *snap);

#ifdef __cplusplus
}
#endif
