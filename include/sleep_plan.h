#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "panic_soak.h" /* MAGTAG_PANIC_SOAK_FAST_LOCKS - the lock-sleep knob */
#include "timer.h"      /* timer_state_t */

#ifdef __cplusplus
extern "C" {
#endif

/* Internal leads — correctness margins, deliberately NOT menuconfig. */
#define SLEEP_PLAN_MIN_SEC 5         /* never nap shorter than this */
#define SLEEP_PLAN_SYNC_LEAD_SEC 20  /* wake early so NTP finishes before :00 */
#define SLEEP_PLAN_EVENT_LEAD_SEC 70 /* wake with ~70 s to expiry/break end */
/* Awake-side watch threshold: a wake with event_remaining <= this enters the
   stay-awake watch instead of sleeping again (= LEAD + margin, so a
   planner-landed wake at ~70 s always qualifies). */
#define SLEEP_PLAN_WATCH_SEC (SLEEP_PLAN_EVENT_LEAD_SEC + 5)

/* How late a break end may be OBSERVED and still chime. Derived, not
   chosen: the watch window already means "close enough to the event that
   the firmware owns it", so the rule reads as "chime only if we were
   inside the watch window for it". Every path that should chime observes
   the transition inside the awake watch (~1 s) or on the next tick wake
   (60 s max); the paths that deliberately stop doing timer work — charge
   lock (600 s naps), bed-time lock (7200 s), power cycle — land far
   outside it and go silent, matching what the snapshot restore already
   does with an elapsed break. */
#define BREAK_CHIME_GRACE_SEC SLEEP_PLAN_WATCH_SEC

typedef struct {
    timer_state_t state;
    int sec_into_minute;         /* time(NULL) % 60, 0..59 */
    int32_t event_remaining_sec; /* RUNNING: to expiry; BREAK: to break end; else 0 */
    bool sync_due_by_next_wake;  /* RUNNING only; false otherwise */
    /* Optional SECONDARY event: a Screen Break running on slot 0 behind
       another selected timer. 0 = none. sleep_plan_from_timer() fills
       this only when the break end will actually chime (nothing RUNNING)
       — a suppressed end is silent and needs no dedicated wake, it just
       drops the chip at whatever the next tick wake is. Ignored when the
       break IS the primary event (state == TIMER_BREAK, i.e. Screen
       selected). */
    int32_t break_remaining_sec;
} sleep_plan_in_t;

/* Seconds to deep-sleep before the next wake. Pure — host-tested. */
int32_t sleep_plan_seconds(const sleep_plan_in_t *in);

/* How far ahead the NTP recheck window is probed at sleep entry: the
   sync is "due" if the window lapses before the wake AFTER next, so the
   early-wake lead has a wake to be applied to. One tick wake is at most
   60 s (RUNNING never sleeps past the countdown grid) plus the grid slack
   the planner may add, so 90 s clears it. */
#define SLEEP_PLAN_SYNC_LOOKAHEAD_SEC 90

/* Everything the plan assembly reads from the timer module at sleep
   entry, taken raw. main.c reads all of it unconditionally — each of
   these is a side-effect-free getter — so that the branching over which
   readings matter lives in sleep_plan_from_timer() below rather than in
   the one file with no host test.
   Do NOT pre-digest these at the call site: the moment main.c decides
   which reading is relevant, the decision is back where it cannot be
   tested. */
typedef struct {
    timer_state_t state;         /* timer_get_state() — the SELECTED slot */
    time_t now;                  /* the single clock read the whole plan folds against */
    int64_t expiry_wall;         /* timer_expiry_wall(); meaningful only when RUNNING */
    bool ntp_recheck_due;        /* timer_needs_ntp_sync(now + SLEEP_PLAN_SYNC_LOOKAHEAD_SEC) */
    bool break_active;           /* timer_break_active() — slot 0 is on a break */
    int32_t break_remaining_sec; /* timer_break_remaining(now) */
    bool extra_running;          /* timer_any_extra_running() — suppresses the chime */
} sleep_plan_timer_in_t;

/* Fold the timer readings into the planner's input. Pure — host-tested.

   Two readings name the same wall event from different sides and must not
   be confused: when Screen is SELECTED during its break the end is the
   PRIMARY event (state == TIMER_BREAK, so it lands in
   event_remaining_sec); when a break runs BEHIND another selected timer
   it is the secondary, and only earns a dedicated wake if it will chime.
   `break_active` — not a non-zero remaining — is what gates the
   secondary: timer_break_remaining() happens to return 0 with no break
   running, but inheriting correctness from another module's internal
   guard is how the rule quietly dies when that module changes. */
sleep_plan_in_t sleep_plan_from_timer(const sleep_plan_timer_in_t *in);

/* ---- sleep mode: which policy a wake ends under ------------------------ */

/* The two locks deliberately stop doing timer work, so they bypass the
   planner for a fixed long interval and leave the buttons dark. Fixed and
   not menuconfig, like the leads above: both are protection margins, not
   preferences — 600 s is "re-read the battery often enough to notice a
   charger" and 7200 s is "wake only for NTP and the rollover check".

   THE BEDTIME ONE — AND ONLY THAT ONE — COLLAPSES TO 90 s WHEN
   MAGTAG_PANIC_SOAK_FAST_LOCKS IS 1, a debugging knob that ships at 0;
   include/panic_soak.h carries the whole argument. The one-line version:
   a bedtime-locked re-wake is the only path that opens a network window
   on EVERY wake (lock_gate_check_bedtime calls net_apply_try_window
   unconditionally, where the daytime path asks wake_policy first), so it
   is the population the nightly panic cluster comes from - and at 7200 s
   it costs two hours to observe one of them. Shortening the sleep is the
   whole harness; nothing else about the locked path changes.

   CHARGE_LOCK_SLEEP_SEC IS DELIBERATELY LEFT ALONE, and an earlier draft
   of this header shortened it too. That was wrong twice over. It cannot
   contribute to the hypothesis: lock_gate_check_charge() calls
   net_apply_try_window() only inside its `if (!s_charge_locked)` engage
   branch, and s_charge_locked is RTC_DATA_ATTR, so a charge-locked
   RE-wake opens no window at all — there is nothing here to soak. And it
   is actively harmful: 600 -> 90 puts a device at or below 10 % battery
   through a full boot (display_init, lv_init, the lot) every 90 s
   instead of every 600 s, ~6.7x the wake rate of the one mechanism whose
   entire job is to stop draining a nearly-dead battery. It is also the
   fail-closed fallback for an unrecognised mode in sleep_plan_outcome(),
   so shortening it silently shortened that safety net as well.

   The guard is written the long way, with the production value repeated
   in the #else, rather than as a ternary or a scaling factor. These are
   protection margins a reader has to be able to find by grepping for
   them, and every assertion in test_sleep_plan names them literally - so
   a flag left flipped fails the host suite here rather than shipping a
   device that wakes every 90 s all night. */
#define CHARGE_LOCK_SLEEP_SEC 600
#if MAGTAG_PANIC_SOAK_FAST_LOCKS
#define BEDTIME_SLEEP_SEC 90
#else
#define BEDTIME_SLEEP_SEC 7200
#endif

/* The config-error lock (design 5.3), and the odd one out in this group
   in two ways, both deliberate.

   IT ARMS BUTTON D, on the sleeps this mode is the one selected for. The
   other two go dark, and can: a charge lock ends when the pack charges, a
   bed-time lock when the clock says morning, and neither needs a human.
   This one ends only when somebody edits config, so a device whose
   network fix path is also down — the WiFi password was what got broken,
   say — would have no exit at all and would need a serial cable. That is
   the failure this whole gate is scored against, so D stays live and the
   fix is one press away.

   WITH ONE EXCEPTION, and it is not a small one, so it is named here
   rather than left to be re-derived from the precedence rule below. A
   device that is config-locked AND charge- or bed-time-locked does not
   select this mode at all: the precedence below hands back
   WAKE_SLEEP_CHARGE_LOCK or WAKE_SLEEP_BEDTIME, whose outcome is
   enable_buttons = false. So on those wakes D is dead like everything
   else, and on a charge re-wake there is no network window either — the
   charge gate opens one only as it engages (lock_gate.c) — which leaves
   that device with neither exit until the battery recovers or the
   interval expires. That is the intended trade (a pack below the lock
   band cannot pay for a refresh, and nobody edits config at bed time),
   but "D stays live" above is true of this MODE, not of every
   config-locked device.

   IT SITS BETWEEN THE OTHER TWO INTERVALS, and the reasoning is the
   network window rather than the panel. Like the bed-time lock, and
   UNLIKE the charge lock (which opens no window on a re-wake at all — see
   panic_soak.h), this one runs a full window on EVERY wake, so it is the
   expensive kind: at 600 s a device left broken over a holiday would
   spend ~1000 full boots and windows on it. 7200 s is the other error —
   the fault is the kind that gets fixed within minutes of somebody
   noticing, and two hours is a long time to hold a device the parent has
   already corrected. D covers the attended case, so this interval only
   has to serve the unattended one.

   NOT SHORTENED BY MAGTAG_PANIC_SOAK_FAST_LOCKS. That knob exists to
   reproduce the bedtime-locked re-wake's panic cluster, and it names one
   population; widening it to a second, rarer one would change what a soak
   run is evidence about. */
#define CONFIG_ERR_SLEEP_SEC 1800

typedef enum {
    WAKE_SLEEP_NORMAL = 0,
    WAKE_SLEEP_CHARGE_LOCK,
    WAKE_SLEEP_BEDTIME,
    WAKE_SLEEP_CONFIG_ERR,
} wake_sleep_mode_t;

/* Precedence between the locks. Both can be engaged at once, by a
   specific path: bed time engages and sets its flag in RTC memory, the
   device sleeps its 2 h chunk, and the next wake runs
   lock_gate_check_charge() — which comes before any bedtime code — with
   the battery at or under BATT_LOCK_PCT. The charge flag goes up while
   the bed-time flag is still set, and nothing on that boot clears it.
   (The 10-15% hysteresis band
   only HOLDS an engaged lock; engaging needs <= BATT_LOCK_PCT.) Charge
   lock wins: a battery that cannot afford a refresh cannot afford the 2 h
   cadence either.

   The config-error lock is LAST, below both, and the reason is the same
   sentence read twice. Below the charge band a press cannot be afforded,
   so arming D there would spend the battery this device has left on a
   refresh it cannot complete; and at bed time the panel is already saying
   "not in service" and nobody is editing config. It outranks NORMAL and
   nothing else. It also holds across both of those — its flag is RTC
   memory like theirs — so the morning a bed-time lock lets go, the config
   lock is still standing and picks the panel back up (lock_gate.c).
   Pure — host-tested. */
wake_sleep_mode_t wake_sleep_mode_select(bool charge_locked, bool bedtime_locked, bool config_locked);

/* Everything the deep-sleep call needs, so main.c can act on the mode
   without a branch of its own. `reason` is a log PREFIX — empty on the
   normal path, "<why>, " on the locked ones — so a single format string
   reproduces all three messages. */
typedef struct {
    uint32_t seconds;
    bool enable_buttons;
    const char *reason;
} sleep_outcome_t;

/* Resolve a mode into that outcome. NORMAL defers to sleep_plan_seconds();
   the locks ignore `in` entirely (main.c gathers the readings
   unconditionally — they are all side-effect-free getters). Pure —
   host-tested. */
sleep_outcome_t sleep_plan_outcome(wake_sleep_mode_t mode, const sleep_plan_in_t *in);

#ifdef __cplusplus
}
#endif
