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
   slot. timer_tick() also ends an elapsed break, so no path can strand
   one. The break-end EDGE (chime, snap back to Screen) is latched, not
   returned: a tick never consumes it, and callers drain it with
   timer_break_take_ended() after whichever tick ended the break (see
   timer_break_tick).

   To add capacity: bump TIMER_EXTRA_SLOTS, add the matching Kconfig block
   and X-macro line in main/timer_defs.c. */
#define TIMER_EXTRA_SLOTS 4
#define TIMER_SLOT_COUNT (1 + TIMER_EXTRA_SLOTS)

/* Definition of one extra timer (from the NVS timer table, or menuconfig as
   the fallback; host tests inject their own table via timer_set_defs). */
typedef struct {
    const char *name; /* NULL or "" = slot disabled */
    int32_t duration_sec;
    bool reloadable; /* Button B may reload this timer the same day */
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
    /* Signed running total of every adjustment that LANDED on this slot
       today — the panel's "(-30 min today)". Tracked rather than derived,
       and neither of the two fields above can stand in for it:
       bonus_sec is ZEROED by timer_start's fold, and bonus_applied is the
       idempotent reconcile's baseline (a cmd-topic grant folded into it
       would corrupt the next delta the HA number computes). Deriving it
       instead as "today's limit minus the day's default" was the original
       bug: the default is read live from config while the limit is frozen
       at start, so an ordinary edit of the weekday minutes, a holiday
       landing mid-run, or a day-type flip all manufactured an adjustment
       nobody made — and rewrote a real one.
       Records what was ASKED for, not what survived clamping: the display
       seam clamps for presentation, this stays the truthful record.
       Resets with the day (timer_reset), and with the slot itself
       (timer_reload / a reconcile RESET both memset it away). */
    int32_t adjust_today_sec;
} timer_slot_state_t;

/* WHICH SCREEN THE DEVICE PAINTS (design row C16). A UI concept and only
 * that: the mode selects the paint, never the wake logic — no timer runs,
 * expires, accrues or sleeps differently because of it.
 *
 * WHY IT IS DECLARED HERE, in the timer header, when it is a UI value:
 * because this is where it is STORED — rtc_state_t below holds the byte
 * and timer_mode()/timer_set_mode() are the only accessors — so the enum
 * has to be visible wherever that field's meaning is. Every consumer
 * already includes timer.h to reach timer_mode(). It deliberately does
 * NOT live in chores.h: that header is the pure chore model, and making
 * timer.h include it would couple the timer to the chore feature for the
 * sake of two enumerators.
 *
 * APP_MODE_TIMERS MUST BE 0, and that is load-bearing rather than
 * cosmetic. timer_reset() clears the day by memset'ing the whole of
 * g_rtc_state, which is exactly what makes row C13 ("a day rollover takes
 * the mode back to Timers") cost nothing and adds no second clearing site
 * to keep in step. Reorder these enumerators and every midnight silently
 * lands the device in chore mode instead. The memset is in timer.c, a
 * long way from this enum, so the rule is restated at that site too —
 * two comments deliberately, because a reader standing at the memset has
 * no reason to come and open this header. test_timer pins both halves:
 * the enumerator's value, and what the memset leaves behind.
 *
 * NOT clamped anywhere. timer_set_mode() stores the byte it is given, so
 * a value outside these enumerators round-trips out of timer_mode()
 * unchanged, exactly as the ack mask does — pinned by test_timer for the
 * same reason the mask's rawness is: the byte crosses an RTC layout that
 * a different firmware build reads back, and a silent clamp would be a
 * second place for the mode's meaning to live. Painting code therefore
 * must treat any value that is not APP_MODE_CHORES as Timers. */
typedef enum {
    APP_MODE_TIMERS = 0, /* the ordinary timer screen */
    APP_MODE_CHORES,     /* the chore checklist */
} app_mode_t;

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
/* v3: + chore_acked, chore_released, mode (the chore checklist)
   v2: timer_slot_state_t gained adjust_today_sec */
#define RTC_STATE_VERSION 3

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
       underneath it (timer_ensure_active_slot_enabled on a restore, which
       folds an orphaned run before it moves) and would otherwise invert a
       drain into an accrual. */
    uint8_t run_segment_slot;
    char last_date[11]; /* "YYYY-MM-DD\0" */
    /* ---- the chore checklist (design §5.1) ------------------------------
       All three are DAY-SCOPED and all three are cleared by the one
       memset in timer_reset(), which IS row C13 — acks gone, release
       gone, mode back to APP_MODE_TIMERS. Nothing else clears them.

       WHY RTC: they are read on the paint path of every wake, and a
       deep-sleep wake keeps .rtc.data for free, which is all row C16 asks
       of the mode.

       WHY NVS AS WELL, for the two ack fields and only them: RTC memory
       is not enough. RTC_DATA_ATTR survives deep sleep ONLY — an
       esp_restart() reloads this segment from the image as zeros — and
       row C14 says a firmware update must not cost the kid their chores.
       So chore_store's "chore_ack" record is the AUTHORITY and these two
       are the working copy; timer_persist_restore_chore_acks() is what
       refills them. `mode` has no NVS row by design, so a restart
       legitimately comes back painting Timers. */
    uint8_t chore_acked; /* bit i = chore i acked today; CHORE_MAX is 3, so 3 bits are used */
    bool chore_released; /* latched: today's withheld remainder has been granted */
    /* app_mode_t, stored as a fixed byte rather than as the enum: an
       enum's width is implementation-defined and this struct's layout is
       read back by a different build of the firmware. */
    uint8_t mode;
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
/* v7: + adjust_today_sec
   v6: + break_interrupted_slot, break_prev_state, run_segment_slot */
#define TIMER_SNAPSHOT_VERSION 7

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
    int32_t adjust_today_sec;
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
   (defs are not in RTC memory); defs[0] (Screen) is ignored. */
void timer_set_defs(const timer_def_t *defs, int count);
/* Firmware glue (main/timer_defs.c): installs the NVS timer table, or the
   menuconfig-built table when no readable one is stored (BUG-8). */
void timer_defs_install(void);
int timer_active_slot(void);
/* Definition of the active slot; NULL for slot 0 (Screen uses schedule.c). */
const timer_def_t *timer_active_def(void);
/* Definition of any slot; NULL for slot 0, disabled, or out of range. */
const timer_def_t *timer_slot_def(int slot);
/* The installed definition for a slot WHETHER OR NOT it is enabled; NULL
   only for slot 0 and out of range. timer_slot_def() hides a slot that has
   a name but no duration yet, which is a real intermediate state (HA's
   CFG_TNAME writes the name; CFG_TMIN follows in a later window). Callers
   that mirror the stored table rather than the running timers -- the cfg
   state JSON and the discovery fingerprint -- must see that slot, or they
   publish a blank name and a fingerprint that flips as the blob becomes
   readable. Everything that decides whether a timer can RUN wants
   timer_slot_def(). */
const timer_def_t *timer_slot_def_raw(int slot);
/* The compile-time (menuconfig) definition for a slot, regardless of what
   is installed or stored; NULL out of range. This is the DEFAULT source for
   a field a retained config document does not mention -- see apply_timers()
   in config_apply.c. It is deliberately not the installed table: the
   installed table is whatever this boot happens to be running, which for a
   device that has a blob is the operator's own edits. */
const timer_def_t *timer_defs_compiled(int slot);
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
/* True when Button B may START or RESUME the active slot: refused only
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
   RUNNING, and otherwise only when the slot's def is reloadable. Screen
   (slot 0) carries no def, so it never qualifies. Also gates B as an EXT1
   wake source (same rationale as timer_swap_allowed). */
bool timer_reload_allowed(void);
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
   PAUSED holding it (press B to use it); a deduction is a no-op. Works
   on any slot — no now needed (RUNNING adjusts the stored wall expiry;
   the rest store durations).

   A BREAK ENTERED FROM EXPIRED takes the EXPIRED rules, not the BREAK
   ones, and that is not a special case so much as the only reading of
   rule 7 that is not a silent loss: a break is slot 0 PARKED, it hands
   the slot back in the state it entered with, and "adjust in place" on a
   state that is about to be overwritten means the seconds are written
   where nothing reads them (timer_slot_remaining reports 0 for EXPIRED).
   So the grant is written through break_prev_state — the break still
   runs to its own end, and comes back PAUSED holding the grant — and a
   deduction is the same no-op it is on a bare EXPIRED slot. A break over
   IDLE is unaffected and keeps the BREAK rules; see adjust_core. */
/* Returns whether anything actually moved: false for a zero/bad-slot call
   and for a deduction against an already-EXPIRED slot, true otherwise.
   The orchestrator needs that answer to decide on a repaint — an
   adjustment banked while IDLE changes the panel without changing the
   state, so a state diff cannot see it. */
bool timer_adjust(int slot, int32_t sec);
/* Idempotent signed "adjustment seconds today" for a slot (HA number):
   applies only the delta beyond what's already been applied today, in
   either direction, so re-delivering the same retained target every wake
   is a no-op. bonus_applied resets at timer_reset (day rollover). */
bool timer_bonus_reconcile(int slot, int32_t target_sec); /* true when a delta landed */

/* Release the chore gate's withheld seconds onto the Screen timer (slot 0).
   Runs timer_adjust's STATE MACHINE without its BOOKKEEPING: the seconds
   land exactly as a grant would (PAUSED/BREAK extend the held remainder,
   EXPIRED comes back PAUSED holding them for Button B), but
   adjust_today_sec is NOT touched.

   That is the whole point of the separate entry. adjust_today_sec is the
   truthful record of what a PARENT asked for and is what the panel's
   "(-30 min today)" renders; a gate release is not a screen adjustment,
   and a second writer into that line would manufacture an adjustment
   nobody made -- verbatim the bug that field was introduced to fix. It is
   not "hidden" from the user either way: the locked block vanishing and
   the bar going full width is loud, immediate feedback.

   Slot 0 only -- the gate withholds screen time, and TIMER_BREAK lives on
   slot 0 regardless of the selection. Returns whether the caller owes a
   REPAINT, on the same terms as timer_adjust: false for sec == 0 (which a
   day with the gate off legitimately produces, see chores_release_due)
   and for a deduction against an already-EXPIRED slot; true otherwise --
   including the IDLE case below, where nothing moved at all.

   IDLE is REFUSED HERE, not by the caller, and reported as true. There
   the day's allocation is read live at the next start and the gate's own
   `released` latch already makes it full, so the release owes the timer
   nothing; the seconds are dropped rather than banked, because banking
   them would hand timer_start a SECOND copy of a remainder the live
   allocation already carries -- a 90 min day against a 60 min default,
   RTC-persisted so it survives deep sleep, and a remaining_sec above
   allocation_sec + adjust_sec, which display.h's adjust_sec == 0 contract
   says cannot happen (the panel would show 90 against 60 with no
   parenthetical, and the bar, which divides by the default, would draw
   150%).

   The true is not a lie about the timer. It is the same answer, for the
   same reason, that timer_adjust gives for its IDLE bank: the PANEL
   changes -- the locked block vanishes, the bar goes full width -- while
   no timer state does, so a state diff cannot see it. A false would tell
   a repaint-driven caller (net_apply's moved_slots) to skip the one paint
   the release exists to trigger. sec == 0 is still left to adjust_core's
   own refusal, IDLE or not: with the gate off there was never a locked
   block to vanish, so there is no paint to ask for.

   The refusal is a precondition on THIS wrapper rather than a fourth arm
   in the shared state machine: timer_adjust's IDLE contract is still to
   bank (that is the parent-adjustment policy), and "a gate release owes
   an IDLE timer nothing, because C5's allocation is read live at the next
   start" is the gate's reasoning alone -- it stays true whatever that
   shared arm later does.

   M2 CALL SITE: chores_withheld_sec() returns uint32_t and this takes
   int32_t, so write the narrowing out -- timer_release_gated((int32_t)w).
   Neither build enables -Wconversion and cppcheck does not flag it, so an
   implicit conversion here is silent; it is safe only because schedule
   minutes are uint16_t (so alloc_sec <= 3,932,100 s, far under
   INT32_MAX), a bound enforced two modules away. The explicit cast is
   what keeps that reliance visible at the seam that depends on it. */
bool timer_release_gated(int32_t sec);

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
   is disabled (snapshot restore, or a config edit mid-window) — and first
   retire any extra whose definition is gone while it is RUNNING (BUG-7: a
   run must not outlive its definition, or it breaks I2/I3). A retired
   slot is reset like timer_reload (completions stay), as timer_reconcile_def
   resets a mid-window disable, so after that reconcile this finds the
   slot IDLE and folds nothing twice.

   A PAUSED orphan is NOT reset; only the selection moves off it. It
   breaks no invariant, and on a restore its "missing" definition may be
   a one-boot fallback to the menuconfig table (timer_defs_install on a
   read failure or blob-version change), so resetting it would destroy a
   paused timer the next boot would have found intact (decided 2026-09-25).

   A RUNNING orphan's live segment folds at `now` and NON-eligible —
   counted as screen exposure. Decided 2026-08-07: the slot's own
   break_eligible went with its definition and cannot be recovered, so the
   fold errs toward more eye rest. Folding here, at the retire, is what
   stops the time AFTER `now` being swept in at some later fold. The time
   before it is swept in, bounded: the rollover restore
   (wake_flow_handle_day_rollover) runs after a genuine power-off, so the
   dark time counts — but a run that expired in the dark was already
   folded at its expiry by the restore, so the fold never exceeds the
   run's own remaining time (accepted 2026-09-25; see
   timer_restore_snapshot). */
void timer_ensure_active_slot_enabled(time_t now);
int64_t timer_expiry_wall(void);  /* active slot's expiry wall time (0 if unset) */
uint16_t timer_completions(void); /* active slot's completed runs today */
/* Read-only per-slot views (stats/summary builders): out-of-range slots
   read as IDLE / 0. */
timer_state_t timer_slot_state(int slot);
int32_t timer_slot_allocation(int slot);
uint16_t timer_slot_completions(int slot);
int32_t timer_screen_bonus_applied(void); /* slot 0 HA bonus reconciled today */
/* Adjustment banked while the slot is IDLE, still waiting for timer_start
   to fold it into the allocation. Anyone rendering an IDLE allocation has
   to add this (and clamp at 0, as timer_start does), or an adjustment
   applied before the day's first start is invisible on every surface
   until someone presses B. */
int32_t timer_slot_banked_bonus(int slot);
/* Signed seconds of adjustment that landed on this slot TODAY, across
   every source (the HA number's reconciled deltas and cmd-topic grants),
   and across every state transition the day makes — timer_start's fold of
   the bank does not disturb it. 0 means the slot is running on its
   configured/scheduled default, which is what lets the status line drop
   the parenthetical entirely on the common day.
   This is what was ASKED for. A deduction deeper than the day is recorded
   in full even though the timer clamped at empty, so the value can be
   more negative than the day's default; callers that render it beside the
   default must clamp for presentation (app_state_display does). */
int32_t timer_slot_adjust_today(int slot);
const char *timer_current_date(void); /* "YYYY-MM-DD"; "" until first record */

/* ---- chore checklist state (design §5.1) --------------------------------
   Accessor pairs so no other module reaches into g_rtc_state, for the same
   reason timer_current_date() and timer_slot_adjust_today() exist. They
   are storage and nothing else: every RULE about these values lives in
   chores.c (what a mask means) or in chore_store.c (when a record may be
   believed), and none of it is restated here. */

/* Today's ack bits, RAW. Bits at or above the configured chore count are
   returned exactly as stored and are NOT masked here — the same choice
   chore_store_load_ack() documents for the same value, and for the same
   reason: masking would destroy acks that a restored list would make
   meaningful again. So read every bit through chores_is_acked(mask, i, n)
   and never test one directly, or a stale bit left by a longer list
   renders as a tick. The setter stores what it is given, unmasked, for
   the same reason. */
uint8_t timer_chore_acked(void);
void timer_chore_set_acked(uint8_t mask);

/* The day's LATCHED release flag (chore_ack_t.released): the withheld
   remainder has been granted. Latched, so nothing a later ack toggle does
   can re-lock the day (C8). */
bool timer_chore_released(void);
void timer_chore_set_released(bool released);

/* The painted mode (C16). Survives deep sleep; reverts to APP_MODE_TIMERS
   on the day rollover for free (the timer_reset() memset, which is why
   APP_MODE_TIMERS is 0); does NOT survive an esp_restart, having no NVS
   row. The setter stores the byte it is given and does not clamp it —
   see app_mode_t. The other two edges C16 names — a break end and an
   emptied chore list — belong to the wake flow: call
   timer_set_mode(APP_MODE_TIMERS) there. Nothing in timer.c moves the
   mode on its own. */
app_mode_t timer_mode(void);
void timer_set_mode(app_mode_t mode);
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
/* The whole of timer_restore_snapshot()'s refusal, asked without touching
   g_rtc_state: true exactly when that call would restore. For a caller
   that must clear the live day first, and only if the restore will land
   (timer_persist_try_restore on an unset-clock placeholder day, BUG-14). */
bool timer_snapshot_restorable(const timer_snapshot_t *snap, time_t now);
uint8_t timer_snapshot_checksum(const timer_snapshot_t *snap);

#ifdef __cplusplus
}
#endif
