#include "timer.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "date_fmt.h"
#include "hal_time.h"

/* ---- RTC state ---- */
#ifndef NATIVE
rtc_state_t RTC_DATA_ATTR g_rtc_state;
#else
rtc_state_t g_rtc_state;
#endif

/* The whole of the guard: two fields compared, and on any disagreement
   the struct is zeroed rather than repaired. Repair is not on the table —
   there is nothing to repair TO. A mismatch means these bytes were
   written by a different build (or by nothing at all), so every field
   behind them is suspect, and the firmware already has a tested,
   validated recovery for "the RTC state is gone": the NVS snapshot, with
   its own version, checksum and range checks. Zeroing routes the fault
   there instead of inventing a second recovery path. */
bool timer_rtc_state_guard(void) {
    if (g_rtc_state.magic == RTC_STATE_MAGIC && g_rtc_state.version == RTC_STATE_VERSION)
        return false;
    memset(&g_rtc_state, 0, sizeof(g_rtc_state));
    g_rtc_state.magic = RTC_STATE_MAGIC;
    g_rtc_state.version = RTC_STATE_VERSION;
    return true;
}

/* Slot definitions live in rodata, not RTC memory — re-injected every boot
   (timer_defs.c on firmware, the test table on host). */
static const timer_def_t *s_defs;
static int s_defs_count;

/* Break-end latch (see timer.h). Deliberately NOT RTC-persistent: it is
   drained within the wake that set it, and a transition that reached
   deep sleep undrained is by definition too late to chime about. */
static bool s_break_ended_latched;
static int64_t s_break_ended_wall;

void timer_set_defs(const timer_def_t *defs, int count) {
    s_defs = defs;
    s_defs_count = (defs == NULL) ? 0 : count;
    if (s_defs_count > TIMER_SLOT_COUNT)
        s_defs_count = TIMER_SLOT_COUNT;
}

static bool slot_enabled(int slot) {
    if (slot == 0)
        return true; /* Screen always exists */
    if (slot < 0 || slot >= s_defs_count)
        return false;
    return s_defs[slot].name != NULL && s_defs[slot].name[0] != '\0' && s_defs[slot].duration_sec > 0;
}

static timer_slot_state_t *active(void) {
    return &g_rtc_state.slots[g_rtc_state.active_slot];
}

/* The Screen slot. A break lives here whichever slot is selected, so
   every break helper goes through this, never active(). */
static timer_slot_state_t *screen_slot(void) {
    return &g_rtc_state.slots[0];
}

int timer_active_slot(void) {
    return g_rtc_state.active_slot;
}

static void fold_run_segment_signed(time_t now, bool eligible);

/* BUG-7: a run must not outlive its own definition. Moving only the
   selection left an orphan RUNNING behind it (I2/I3), and its live
   segment then kept accruing until some later fold swept the whole gap
   into the balance. So every RUNNING extra whose definition is gone is
   retired first: reset like timer_reload (completions stay, the old
   timer's grants go), as timer_reconcile_def resets a mid-window disable.

   Only RUNNING (decided 2026-09-25). A PAUSED orphan breaks no invariant,
   and on a restore "no definition" may be a lie: timer_defs_install falls
   back to the menuconfig table on any read failure or blob-version
   change, and an HA-defined slot menuconfig leaves empty then reads as
   disabled for that one boot. Resetting it would destroy a paused timer
   that comes back intact on the next boot. So a PAUSED orphan keeps its
   state and only the selection moves off it; IDLE and EXPIRED hold no
   run. A RUNNING orphan is still reset on such a boot — that needs a
   running extra, lost RTC memory and a fallback table at once, and
   resetting is the simple way to restore I3.

   The segment folds at `now` and NON-eligible (decided 2026-08-07, see
   include/timer.h) — never at the slot's own sign, which went with the
   definition. It folds when the orphan armed it, or when the slot that
   armed it is not RUNNING (only a corrupt snapshot names one): the orphan
   was then the only run feeding it, and resetting the orphan without
   the fold would leave the balance moving with nothing RUNNING (I8). A
   segment another RUNNING slot armed is that slot's, and stays armed. */
void timer_ensure_active_slot_enabled(time_t now) {
    for (int i = 1; i < TIMER_SLOT_COUNT; i++) {
        timer_slot_state_t *sl = &g_rtc_state.slots[i];
        if (slot_enabled(i) || sl->state != TIMER_RUNNING)
            continue;
        uint8_t owner = g_rtc_state.run_segment_slot;
        if (owner == (uint8_t)i || g_rtc_state.slots[owner].state != TIMER_RUNNING)
            fold_run_segment_signed(now, false);
        uint16_t completions = sl->completions;
        memset(sl, 0, sizeof(*sl));
        sl->state = TIMER_IDLE;
        sl->completions = completions;
    }
    if (!slot_enabled(g_rtc_state.active_slot))
        g_rtc_state.active_slot = 0;
}

int timer_slot_by_name(const char *name) {
    if (name == NULL || name[0] == '\0' || strcmp(name, "Screen") == 0)
        return 0;
    for (int i = 1; i < TIMER_SLOT_COUNT; i++) {
        if (slot_enabled(i) && strcmp(s_defs[i].name, name) == 0)
            return i;
    }
    return -1; /* no such enabled timer */
}

const timer_def_t *timer_slot_def(int slot) {
    if (slot <= 0 || !slot_enabled(slot))
        return NULL; /* Screen (0), disabled, or out of range */
    return &s_defs[slot];
}

/* Same table, without the enablement filter: a slot that has a name but no
   duration is a definition that exists and is not yet runnable, and the
   callers that mirror the stored table need to see it. See timer.h. */
const timer_def_t *timer_slot_def_raw(int slot) {
    if (slot <= 0 || slot >= s_defs_count)
        return NULL; /* Screen (0) or out of range; s_defs_count is 0 when unset */
    return &s_defs[slot];
}

const timer_def_t *timer_active_def(void) {
    int slot = g_rtc_state.active_slot;
    if (slot == 0 || slot >= s_defs_count)
        return NULL;
    return &s_defs[slot];
}

int timer_extra_count(void) {
    int n = 0;
    for (int i = 1; i < TIMER_SLOT_COUNT; i++) {
        if (slot_enabled(i))
            n++;
    }
    return n;
}

/* Slot 0 is permanently non-eligible (screen time IS the exposure), and a
   slot with no definition cannot claim to be a break activity. Every sign
   and every start decision goes through here. */
bool timer_slot_break_eligible(int slot) {
    if (slot <= 0 || !slot_enabled(slot))
        return false;
    return s_defs[slot].break_eligible;
}

int timer_eligible_extra_count(void) {
    int n = 0;
    for (int i = 1; i < TIMER_SLOT_COUNT; i++) {
        if (timer_slot_break_eligible(i))
            n++;
    }
    return n;
}

bool timer_start_allowed(void) {
    if (screen_slot()->state != TIMER_BREAK)
        return true; /* no break: every slot is startable */
    return timer_slot_break_eligible(g_rtc_state.active_slot);
}

bool timer_swap_allowed(void) {
    /* A background Screen Break does NOT refuse: the break enforces the
       screen timer, not the whole device — running Piano during it is the
       point. Only a RUNNING slot must be paused first. */
    if (active()->state == TIMER_RUNNING)
        return false;
    return timer_extra_count() > 0;
}

int timer_next_slot(void) {
    for (int i = 1; i < TIMER_SLOT_COUNT; i++) {
        int cand = (g_rtc_state.active_slot + i) % TIMER_SLOT_COUNT;
        if (slot_enabled(cand))
            return cand;
    }
    return -1; /* no other enabled slot */
}

bool timer_select_next(void) {
    if (!timer_swap_allowed())
        return false;
    int cand = timer_next_slot();
    if (cand < 0)
        return false;
    g_rtc_state.active_slot = (uint8_t)cand;
    return true;
}

bool timer_select_screen(void) {
    if (g_rtc_state.active_slot == 0)
        return true; /* already there */
    if (active()->state == TIMER_RUNNING)
        return false; /* never steal the selection mid-run */
    g_rtc_state.active_slot = 0;
    return true;
}

int timer_break_interrupted_slot(void) {
    return g_rtc_state.break_interrupted_slot;
}

bool timer_select_interrupted(void) {
    if (active()->state == TIMER_RUNNING)
        return false; /* same rule as timer_select_screen */
    int slot = g_rtc_state.break_interrupted_slot;
    /* The slot may have been disabled by an HA edit while the break ran —
       never strand the selection somewhere the buttons cannot leave. */
    if (slot < 0 || slot >= TIMER_SLOT_COUNT || !slot_enabled(slot))
        slot = 0;
    g_rtc_state.active_slot = (uint8_t)slot;
    return true;
}

bool timer_any_extra_running(void) {
    for (int i = 1; i < TIMER_SLOT_COUNT; i++) {
        if (g_rtc_state.slots[i].state == TIMER_RUNNING)
            return true;
    }
    return false;
}

bool timer_reload_allowed(void) {
    if (active()->state == TIMER_RUNNING)
        return false; /* can't reset a running timer — pause first */
    const timer_def_t *def = timer_active_def();
    return def != NULL && def->reloadable; /* Screen has no def: never */
}

bool timer_reload(void) {
    timer_slot_state_t *sl = active();
    if (sl->state == TIMER_RUNNING)
        return false; /* pause first */
    uint16_t completions = sl->completions;
    /* On slot 0 these two are the exposure balance, which only a break
       start and the day rollover may reset (I9) — a parent reloading the
       screen timer is neither. Zero (and therefore a no-op) on extras. */
    int32_t accum = sl->run_accum_sec;
    int64_t segment = sl->run_started_wall;
    memset(sl, 0, sizeof(*sl));
    sl->state = TIMER_IDLE;
    sl->completions = completions; /* reload never counts as a run */
    sl->run_accum_sec = accum;
    sl->run_started_wall = segment;
    return true;
}

timer_state_t timer_get_state(void) {
    return active()->state;
}

int64_t timer_expiry_wall(void) {
    return active()->expiry_wall_time;
}

int32_t timer_slot_remaining(int slot, time_t now, int32_t idle_fallback) {
    if (slot < 0 || slot >= TIMER_SLOT_COUNT)
        return 0;
    const timer_slot_state_t *sl = &g_rtc_state.slots[slot];
    int32_t r;
    switch (sl->state) {
        case TIMER_RUNNING:
            r = (int32_t)(sl->expiry_wall_time - (int64_t)now);
            break;
        case TIMER_PAUSED:
        case TIMER_BREAK:
            r = sl->remaining_at_pause;
            break;
        case TIMER_IDLE:
            r = idle_fallback;
            break;
        default: /* EXPIRED */
            r = 0;
            break;
    }
    return (r > 0) ? r : 0;
}

int32_t timer_screen_used_sec(time_t now) {
    const timer_slot_state_t *s0 = &g_rtc_state.slots[0];
    int32_t remaining = timer_slot_remaining(0, now, s0->allocation_sec);
    int32_t used = s0->allocation_sec - remaining;
    return (used > 0) ? used : 0;
}

uint16_t timer_completions(void) {
    return active()->completions;
}

timer_state_t timer_slot_state(int slot) {
    if (slot < 0 || slot >= TIMER_SLOT_COUNT)
        return TIMER_IDLE;
    return g_rtc_state.slots[slot].state;
}

int32_t timer_slot_allocation(int slot) {
    if (slot < 0 || slot >= TIMER_SLOT_COUNT)
        return 0;
    return g_rtc_state.slots[slot].allocation_sec;
}

uint16_t timer_slot_completions(int slot) {
    if (slot < 0 || slot >= TIMER_SLOT_COUNT)
        return 0;
    return g_rtc_state.slots[slot].completions;
}

int32_t timer_screen_bonus_applied(void) {
    return g_rtc_state.slots[0].bonus_applied;
}

int32_t timer_slot_banked_bonus(int slot) {
    if (slot < 0 || slot >= TIMER_SLOT_COUNT)
        return 0;
    return g_rtc_state.slots[slot].bonus_sec;
}

int32_t timer_slot_adjust_today(int slot) {
    if (slot < 0 || slot >= TIMER_SLOT_COUNT)
        return 0;
    return g_rtc_state.slots[slot].adjust_today_sec;
}

const char *timer_current_date(void) {
    return g_rtc_state.last_date; /* "" until timer_record_date / restore */
}

/* ---- chore checklist state (see timer.h) -------------------------------- */

uint8_t timer_chore_acked(void) {
    return g_rtc_state.chore_acked;
}

/* Stored exactly as handed over, bits above the configured count and all.
   See timer.h: chore_store_load_ack() makes the same choice for the same
   value, and chores.c is what bounds every bit that is read. */
void timer_chore_set_acked(uint8_t mask) {
    g_rtc_state.chore_acked = mask;
}

bool timer_chore_released(void) {
    return g_rtc_state.chore_released;
}

void timer_chore_set_released(bool released) {
    g_rtc_state.chore_released = released;
}

app_mode_t timer_mode(void) {
    return (app_mode_t)g_rtc_state.mode;
}

/* Stored as the caller's byte, unclamped, like the ack mask above: see
   app_mode_t in timer.h for why the RTC field is not the place to bound
   it. test_timer pins the round trip. */
void timer_set_mode(app_mode_t mode) {
    g_rtc_state.mode = (uint8_t)mode;
}

void timer_reset(void) {
    memset(&g_rtc_state, 0, sizeof(g_rtc_state));
    /* all slots IDLE (=0), active_slot 0 (Screen), counters cleared.
       Row C13 rides on this one memset and deliberately adds nothing:
       chore_acked and chore_released go to 0/false, and `mode` goes to
       APP_MODE_TIMERS because that enumerator IS 0 (see app_mode_t). An
       explicit re-clear here would be a second site to keep in step with
       a struct that already grew fields once. */
    s_break_ended_latched = false; /* never chime yesterday's break */
    s_break_ended_wall = 0;
}

/* ---- screen-exposure balance (slot 0) ----------------------------------

   The balance is one signed quantity fed by whichever slot is RUNNING:
   non-eligible time adds 1:1, eligible time subtracts 1:1, floored at
   zero. The DIRECTION is never stored — it is derived here, at every
   fold, from the running slot's break_eligible (I10). That is what makes
   an HA edit of the flag impossible to desynchronise from the balance,
   and it is why every transition into or out of RUNNING must fold. */

/* Fold slot 0's live run segment at `eligible`'s sign and disarm it. */
static void fold_run_segment_signed(time_t now, bool eligible) {
    timer_slot_state_t *s0 = screen_slot();
    if (s0->run_started_wall == 0)
        return; /* nothing running: the balance is frozen (I8) */
    int64_t seg = (int64_t)now - s0->run_started_wall;
    if (seg > 0) {
        int64_t v = (int64_t)s0->run_accum_sec + (eligible ? -seg : seg);
        if (v < 0)
            v = 0; /* no banked credit below empty (I8) */
        s0->run_accum_sec = (int32_t)v;
    }
    s0->run_started_wall = 0;
}

/* Fold at the sign of the slot that ARMED the segment. Never the active
   slot: the segment lives on slot 0 and the selection can move
   underneath it (a snapshot restore moves it off a slot whose definition
   is gone), which would turn a drain into an accrual. The sign is a
   property of the run. */
static void fold_run_segment(time_t now) {
    fold_run_segment_signed(now, timer_slot_break_eligible(g_rtc_state.run_segment_slot));
}

/* Arm a fresh segment on slot 0 for a run of `slot` starting now. */
static void arm_run_segment(int slot, time_t now) {
    screen_slot()->run_started_wall = (int64_t)now;
    g_rtc_state.run_segment_slot = (uint8_t)slot;
}

void timer_start(time_t now, int32_t allocation_sec) {
    timer_slot_state_t *sl = active();
    allocation_sec += sl->bonus_sec; /* fold in any HA adjustment banked while IDLE */
    if (allocation_sec < 0)
        allocation_sec = 0; /* banked deduction beyond the allocation: start empty */
    sl->bonus_sec = 0;
    sl->state = TIMER_RUNNING;
    sl->allocation_sec = allocation_sec;
    sl->expiry_wall_time = (int64_t)now + allocation_sec;
    /* G1: a start must NOT reset the balance. It once did, which under a
       shared balance means folding laundry for 29 minutes and pressing B
       on Piano puts the eye-rest clock back to zero. Only a break start
       and the day rollover reset it (I9); here we merely fold whatever
       was running and re-arm. */
    fold_run_segment(now);
    arm_run_segment(g_rtc_state.active_slot, now);
}

static void mark_expired(timer_slot_state_t *sl);
static void expire_slot(int slot, time_t now);

/* The adjustment state machine, shared by the two bookkeeping policies
   below. `record` is their ONLY difference and it gates exactly one line
   (the adjust_today_sec site after the switch). Split rather than
   duplicated deliberately: a second copy of these arms would drift, and
   the EXPIRED and BREAK contracts here are subtle enough that the drift
   would be silent. */
static bool adjust_core(int slot, int32_t sec, bool record) {
    if (slot < 0 || slot >= TIMER_SLOT_COUNT || sec == 0)
        return false;
    timer_slot_state_t *sl = &g_rtc_state.slots[slot];
    /* A BREAK is slot 0 PARKED, and I7 says it comes back holding the
       state it ENTERED with. So when that state is EXPIRED the arms below
       have to run the EXPIRED contract and write through break_prev_state
       — the frozen BREAK one is the wrong answer and loses the seconds
       outright. Taking the PAUSED/BREAK arm there puts the grant in
       remaining_at_pause; timer_break_tick() then overwrites sl->state
       back to EXPIRED, and timer_slot_remaining() reports 0 for EXPIRED.
       The seconds land in a field nobody reads, and `released` is latched
       by then, so the grant can never be offered a second time.

       Reachable straight off design §2.6 rather than by contrivance: the
       free tranche runs out, Screen EXPIRES, the kid runs an extra timer,
       earns an eye-rest break, and does the chores DURING it.

       Only EXPIRED is redirected, not the whole arm. A break over an IDLE
       screen must keep taking the BREAK arm: timer_release_gated()'s IDLE
       refusal is a precondition on the WRAPPER (it tests slots[0].state,
       which reads BREAK here), so routing the break case to the shared
       IDLE arm would bank the release into bonus_sec — precisely the
       second copy of the remainder that refusal exists to prevent. */
    const bool parked_expired = (sl->state == TIMER_BREAK && g_rtc_state.break_prev_state == (uint8_t)TIMER_EXPIRED);
    switch (parked_expired ? (int)TIMER_EXPIRED : (int)sl->state) {
        case TIMER_RUNNING:
            /* A deduction past zero expires on the next tick — the normal
               expiry path, alert included. */
            sl->expiry_wall_time += sec;
            sl->allocation_sec += sec;
            if (sl->allocation_sec < 0)
                sl->allocation_sec = 0;
            break;
        case TIMER_PAUSED:
        case TIMER_BREAK:
            sl->remaining_at_pause += sec;
            sl->allocation_sec += sec;
            if (sl->allocation_sec < 0)
                sl->allocation_sec = 0;
            if (sl->remaining_at_pause <= 0) {
                sl->remaining_at_pause = 0;
                /* Emptied PAUSED expires (same contract as
                   timer_reconcile_def). A BREAK stays intact and keeps its
                   frozen zero: it leaves the break in the state it entered
                   with (I7), so an emptied one comes back PAUSED holding
                   nothing — pressing B then expires it by the normal path.
                   PAUSED has no live segment, so no fold is owed. */
                if (sl->state == TIMER_PAUSED)
                    mark_expired(sl);
            }
            break;
        case TIMER_EXPIRED:
            if (sec < 0)
                return false; /* nothing left to reclaim */
            /* Chores-done grant after time ran out: hold it PAUSED so the
               kid presses B to start — never auto-run, and the expiry
               alert (already heard) must not re-fire.

               Written through break_prev_state while the slot is parked
               in a break: the break itself must still run to its end
               (rule 7 does not let a chore ack cut it short), and
               break_prev_state is the state it will restore when it does.
               remaining_at_pause is set either way, so the grant is
               visible as screen time DURING the break too — mark_expired
               left it at 0, so this is the only writer. */
            if (parked_expired) {
                g_rtc_state.break_prev_state = (uint8_t)TIMER_PAUSED;
            } else {
                sl->state = TIMER_PAUSED;
            }
            sl->remaining_at_pause = sec;
            sl->allocation_sec += sec;
            break;
        default: /* IDLE: bank it; timer_start folds it into the allocation */
            sl->bonus_sec += sec;
            break;
    }
    /* One site, after the switch, so every branch that returns true is
       counted and the two that return false (zero/bad slot, a deduction
       against an EXPIRED slot) are not. Deliberately the REQUESTED delta,
       not the clamped one: this is the record of what the parent did, and
       the panel does its own clamping against the day's default.

       `record` gates THIS LINE ONLY. A chore-gate release runs every arm
       above and writes nothing here: that field means "what a parent
       asked for" and drives the panel's "(-30 min today)", so a gate
       writing into it would manufacture an adjustment nobody made. */
    if (record)
        sl->adjust_today_sec += sec;
    return true;
}

bool timer_adjust(int slot, int32_t sec) {
    return adjust_core(slot, sec, true);
}

bool timer_release_gated(int32_t sec) {
    /* Slot 0 always: the gate withholds screen time, and a break lives on
       slot 0 whichever slot is selected. record = false is the whole
       difference — see the header for why the release must not simply be
       timer_adjust(0, +withheld).

       IDLE is refused before the state machine ever runs: a gate release
       owes an IDLE timer nothing, because C5's allocation is read live at
       the next start and the gate's own `released` latch already makes it
       full. Banking it there — which is what the shared IDLE arm would do
       — hands timer_start a second copy of the same remainder, an
       RTC-persisted 90 min against a 60 min day that no adjust_today_sec
       explains and that display.h says cannot exist.

       A PRECONDITION ON THIS WRAPPER, deliberately, rather than a fourth
       arm inside adjust_core: timer_adjust must still bank while IDLE
       (that is the parent-adjustment contract), and the justification
       above belongs to the gate alone — it holds whatever that shared arm
       later becomes.

       It returns TRUE all the same. Nothing moved in the timer, but the
       PANEL moved: the locked block goes and the bar fills, with no state
       change for a diff to catch — the same case, and the same answer, as
       timer_adjust's IDLE bank. sec == 0 stays with adjust_core's own
       refusal even here: a day whose gate was off never drew a locked
       block, so it has no repaint to ask for. */
    if (sec != 0 && g_rtc_state.slots[0].state == TIMER_IDLE)
        return true;
    return adjust_core(0, sec, false);
}

bool timer_bonus_reconcile(int slot, int32_t target_sec) {
    if (slot < 0 || slot >= TIMER_SLOT_COUNT)
        return false;
    timer_slot_state_t *sl = &g_rtc_state.slots[slot];
    int32_t delta = target_sec - sl->bonus_applied;
    if (delta == 0)
        return false; /* target met — idempotent across wakes and replays */
    bool moved = timer_adjust(slot, delta);
    sl->bonus_applied = target_sec;
    return moved;
}

/* Mark a slot's run as reaching 00:00 (shared by tick and reconcile).
   G2: expiry is an exit from RUNNING, so it folds the live segment like
   every other one (I10). Without this the final segment is silently
   dropped — and, since the balance is keyed on run_started_wall rather
   than on state, the same gap would instead leave it advancing forever
   behind an expired timer. */
static void mark_expired(timer_slot_state_t *sl) {
    sl->state = TIMER_EXPIRED;
    /* An expired timer holds nothing — timer_slot_remaining already
       reports 0 for it, and leaving a stale banked value behind is what
       a break entered from EXPIRED would otherwise hand back. */
    sl->remaining_at_pause = 0;
    if (sl->completions != UINT16_MAX)
        sl->completions++; /* the run reached 00:00; saturate, never wrap */
}

static void expire_slot(int slot, time_t now) {
    timer_slot_state_t *sl = &g_rtc_state.slots[slot];
    if (sl->state == TIMER_RUNNING)
        fold_run_segment(now);
    mark_expired(sl);
}

timer_reconcile_t timer_reconcile_def(int slot, const timer_def_t *old_def, const timer_def_t *new_def, time_t now,
                                      bool *was_running) {
    if (was_running != NULL)
        *was_running = false;
    if (slot <= 0 || slot >= TIMER_SLOT_COUNT || old_def == NULL)
        return TIMER_RECONCILE_NONE; /* Screen follows schedule.c — exempt */
    timer_slot_state_t *sl = &g_rtc_state.slots[slot];
    if (sl->state != TIMER_RUNNING && sl->state != TIMER_PAUSED)
        return TIMER_RECONCILE_NONE; /* IDLE/EXPIRED pick up the new def at next start */
    bool running = (sl->state == TIMER_RUNNING);
    if (was_running != NULL)
        *was_running = running;

    bool disabled =
        (new_def == NULL || new_def->name == NULL || new_def->name[0] == '\0' || new_def->duration_sec <= 0);
    if (disabled || strcmp(old_def->name, new_def->name) != 0) {
        /* A different (or deleted) timer lives here now — the old run is
           meaningless. Reset like timer_reload: completions stay (today's
           history), grants/bonus belong to the old timer and go. The run
           is over, so its live segment folds at the OLD sign first — the
           balance lives on slot 0 and would otherwise keep advancing
           behind a timer that no longer exists. */
        if (running)
            fold_run_segment_signed(now, old_def->break_eligible);
        uint16_t completions = sl->completions;
        memset(sl, 0, sizeof(*sl));
        sl->state = TIMER_IDLE;
        sl->completions = completions;
        return TIMER_RECONCILE_RESET;
    }

    /* A break_eligible flip is a DIRECTION change, and therefore a fold
       point (I10): the segment so far must land at the old sign before
       the new def takes effect, or the whole in-flight run is
       retroactively re-signed. net_apply reinstalls the defs table before
       reconciling, so old_def is the only place the old sign survives. */
    if (running && old_def->break_eligible != new_def->break_eligible) {
        fold_run_segment_signed(now, old_def->break_eligible);
        arm_run_segment(slot, now);
    }

    int32_t delta = new_def->duration_sec - old_def->duration_sec;
    if (delta == 0)
        return TIMER_RECONCILE_NONE; /* flag-only edit: the defs table carries it */

    /* Delta-shift: allocation and expiry/remaining move together, so time
       already elapsed and HA grants are both preserved (and the snapshot
       invariant remaining <= allocation keeps holding). run_started_wall is
       the current run SEGMENT (reset on every resume) — never derive the
       new expiry from it. */
    sl->allocation_sec += delta;
    if (sl->allocation_sec < 0)
        sl->allocation_sec = 0;
    if (running) {
        sl->expiry_wall_time += delta;
        if (sl->expiry_wall_time <= (int64_t)now) {
            expire_slot(slot, now);
            return TIMER_RECONCILE_EXPIRED;
        }
    } else {
        sl->remaining_at_pause += delta;
        if (sl->remaining_at_pause <= 0) {
            sl->remaining_at_pause = 0;
            mark_expired(sl); /* PAUSED: no live segment to fold */
            return TIMER_RECONCILE_EXPIRED;
        }
    }
    return TIMER_RECONCILE_UPDATED;
}

int32_t timer_tick(time_t now) {
    /* A break runs on slot 0 whichever slot is selected, so end it here
       too — no path (a tick while Piano is active, a wake that skipped the
       edge handler) may strand one. Silent by design: callers that need
       the edge call timer_break_tick() first. */
    timer_break_tick(now);

    timer_slot_state_t *sl = active();
    if (sl->state == TIMER_BREAK) {
        return sl->remaining_at_pause; /* screen-time stays frozen */
    }
    if (sl->state == TIMER_PAUSED) {
        return sl->remaining_at_pause;
    }
    if (sl->state != TIMER_RUNNING) {
        return 0; /* IDLE or EXPIRED */
    }
    int64_t remaining = sl->expiry_wall_time - (int64_t)now;
    if (remaining <= 0) {
        expire_slot(g_rtc_state.active_slot, now);
        return (int32_t)remaining;
    }
    /* expiry_wall_time is NOT modified here */
    return (int32_t)remaining;
}

void timer_pause(time_t now) {
    timer_slot_state_t *sl = active();
    if (sl->state != TIMER_RUNNING)
        return; /* no-op; caller checks state */
    int64_t remaining = sl->expiry_wall_time - (int64_t)now;
    sl->remaining_at_pause = (remaining > 0) ? (int32_t)remaining : 0;
    sl->expiry_wall_time = 0;
    fold_run_segment(now);
    sl->state = TIMER_PAUSED;
}

void timer_resume(time_t now) {
    timer_slot_state_t *sl = active();
    if (sl->state != TIMER_PAUSED)
        return; /* no-op; caller checks state */
    sl->expiry_wall_time = (int64_t)now + sl->remaining_at_pause;
    arm_run_segment(g_rtc_state.active_slot, now); /* the balance lives on slot 0 */
    sl->state = TIMER_RUNNING;
}

int32_t timer_run_accum(time_t now) {
    /* Keyed on slot 0's run_started_wall, NOT on any slot's state: slot 0
       is routinely IDLE or PAUSED while Laundry or Violin drives the
       balance. The clamp is the read-side half of the floor — the live
       segment is not folded yet, so a long eligible run must report 0
       rather than a negative number to logging, HA and the break check. */
    const timer_slot_state_t *s0 = screen_slot();
    int64_t accum = s0->run_accum_sec;
    if (s0->run_started_wall != 0) {
        int64_t seg = (int64_t)now - s0->run_started_wall;
        if (seg > 0)
            accum += timer_slot_break_eligible(g_rtc_state.run_segment_slot) ? -seg : seg;
    }
    return (accum > 0) ? (int32_t)accum : 0;
}

bool timer_break_due(time_t now, int32_t interval_sec) {
    if (interval_sec <= 0)
        return false;
    if (screen_slot()->state == TIMER_BREAK)
        return false; /* one is already running */
    /* No RUNNING requirement and no requirement on slot 0's state: the
       balance may have crossed while Screen sat IDLE and a non-eligible
       extra did the work, an earned break is not un-earned by pausing,
       and eye rest must keep working once the day's allocation is spent
       (EXPIRED is the normal end-of-day state, and folding laundry with
       the TV on is exactly when a break still matters). Slot 0 comes back
       from the break in the state it went in with, so there is nothing
       here to protect. */
    return timer_run_accum(now) >= interval_sec;
}

void timer_start_break(time_t now, int32_t duration_sec) {
    /* Slot 0 explicitly: the break lives there whichever slot is
       selected. Unlike the predecessor it no longer requires a RUNNING
       Screen — the break may have been earned entirely by a non-eligible
       extra while Screen was never started. */
    timer_slot_state_t *s0 = screen_slot();
    if (s0->state == TIMER_BREAK)
        return; /* idempotent */

    /* Recorded BEFORE the selection snaps: nothing else remembers what
       the break interrupted (rule 8). */
    g_rtc_state.break_interrupted_slot = g_rtc_state.active_slot;

    /* Nothing may keep running behind a break (I6) — a break is only
       honest if the exposure stops. Only the active slot can be RUNNING,
       and pausing it folds its segment at its own sign.

       When slot 0 IS that slot, this is also what converts its live
       expiry into remaining_at_pause. Doing it here rather than in the
       break is G3: recomputing unconditionally would read a PAUSED slot
       0's expiry_wall_time of 0 as a large negative remaining, clamp it
       to zero, and destroy the kid's banked screen time. */
    if (active()->state == TIMER_RUNNING)
        timer_pause(now);

    /* Captured AFTER the pause: a break must never resume the screen
       timer by itself, so a RUNNING slot 0 is remembered as PAUSED. */
    g_rtc_state.break_prev_state = (uint8_t)s0->state;

    s0->expiry_wall_time = 0;
    s0->run_accum_sec = 0; /* fresh window after the break (I9) */
    s0->run_started_wall = 0;
    s0->break_expiry_wall = (int64_t)now + duration_sec;
    s0->state = TIMER_BREAK;
    g_rtc_state.active_slot = 0; /* selection snaps to the break screen */
}

bool timer_break_active(void) {
    return screen_slot()->state == TIMER_BREAK;
}

int32_t timer_break_remaining(time_t now) {
    timer_slot_state_t *sl = screen_slot();
    if (sl->state != TIMER_BREAK)
        return 0;
    int64_t remaining = sl->break_expiry_wall - (int64_t)now;
    return (remaining > 0) ? (int32_t)remaining : 0;
}

void timer_break_tick(time_t now) {
    timer_slot_state_t *sl = screen_slot();
    if (sl->state != TIMER_BREAK || (int64_t)now < sl->break_expiry_wall)
        return;
    /* Latch the WALL end, not the lateness: the drain computes how late
       IT is, which is what the grace window actually judges. */
    s_break_ended_wall = sl->break_expiry_wall;
    s_break_ended_latched = true;
    /* I7: back to whatever slot 0 was doing before the break — PAUSED
       with its banked time, IDLE if Screen never started today, EXPIRED
       if the day's allocation was already spent. */
    sl->state = (timer_state_t)g_rtc_state.break_prev_state;
    sl->break_expiry_wall = 0;
}

bool timer_break_take_ended(time_t now, int32_t *overdue_sec) {
    if (overdue_sec != NULL)
        *overdue_sec = 0;
    if (!s_break_ended_latched)
        return false;
    if (overdue_sec != NULL)
        *overdue_sec = (int32_t)((int64_t)now - s_break_ended_wall);
    s_break_ended_latched = false;
    s_break_ended_wall = 0;
    return true;
}

void timer_shift_expiry(int64_t delta_sec) {
    /* An NTP sync may step time(NULL); every stored WALL time must step by
       the same amount so stored durations are preserved. PAUSED stores a
       duration — no shift.

       Two wall times can be pending at once (a background break on slot 0
       behind a RUNNING extra), so both are handled. No double-shift when
       slot 0 is the active slot: a slot is never RUNNING and BREAK at the
       same time, so at most one branch matches it. */
    timer_slot_state_t *sl = active();
    if (sl->state == TIMER_RUNNING && sl->expiry_wall_time != 0) {
        sl->expiry_wall_time += delta_sec;
    }
    timer_slot_state_t *s0 = screen_slot();
    /* The exposure balance's live segment always starts on SLOT 0, even
       when an extra timer is what is running, so it is shifted here and
       not with the active slot's expiry. Unconditional on state: slot 0
       is routinely IDLE or PAUSED while it holds a live segment. */
    if (s0->run_started_wall != 0) {
        s0->run_started_wall += delta_sec;
    }
    if (s0->state == TIMER_BREAK && s0->break_expiry_wall != 0) {
        s0->break_expiry_wall += delta_sec;
    }
    /* A latched (transitioned but not yet drained) break end is a stored
       wall time too. main.c can land a clock step in exactly that gap —
       finish_action_and_render ticks, net_apply_finish applies the step,
       then the drain runs — and an unshifted latch would read a forward
       step as lateness and silence a chime that is not actually late. */
    if (s_break_ended_latched) {
        s_break_ended_wall += delta_sec;
    }
}

bool timer_is_new_day(time_t now) {
    if (g_rtc_state.last_date[0] == '\0')
        return true;
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    char today[11];
    date_fmt_iso(today, sizeof(today), &tm_now);
    return (strcmp(today, g_rtc_state.last_date) != 0);
}

void timer_record_date(time_t now) {
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    date_fmt_iso(g_rtc_state.last_date, sizeof(g_rtc_state.last_date), &tm_now);
}

bool timer_needs_ntp_sync(time_t now) {
    if (g_rtc_state.next_ntp_sync == 0)
        return true;
    return (int64_t)now >= g_rtc_state.next_ntp_sync;
}

void timer_record_ntp_sync(time_t now) {
    g_rtc_state.next_ntp_sync = (int64_t)now + NTP_SYNC_INTERVAL_SEC;
}

time_t timer_last_ntp_sync(void) {
    /* Derived, not stored twice: next_ntp_sync is written only by
       timer_record_ntp_sync, so subtracting the interval recovers the
       recorded time exactly. 0 = never synced since RTC loss or day
       rollover (timer_reset) — callers treat 0 as "unknown". */
    if (g_rtc_state.next_ntp_sync == 0)
        return 0;
    return (time_t)(g_rtc_state.next_ntp_sync - NTP_SYNC_INTERVAL_SEC);
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
    memset(out, 0, sizeof(*out)); /* also zeroes padding for the checksum */
    out->version = TIMER_SNAPSHOT_VERSION;
    out->active_slot = g_rtc_state.active_slot;
    out->break_interrupted_slot = g_rtc_state.break_interrupted_slot;
    out->break_prev_state = g_rtc_state.break_prev_state;
    out->run_segment_slot = g_rtc_state.run_segment_slot;
    for (int i = 0; i < TIMER_SLOT_COUNT; i++) {
        const timer_slot_state_t *sl = &g_rtc_state.slots[i];
        timer_snapshot_slot_t *os = &out->slots[i];
        os->state = (uint8_t)sl->state;
        os->remaining_at_pause = sl->remaining_at_pause;
        os->allocation_sec = sl->allocation_sec;
        os->expiry_wall_time = sl->expiry_wall_time;
        os->run_accum_sec = sl->run_accum_sec;
        os->run_started_wall = sl->run_started_wall;
        os->break_expiry_wall = sl->break_expiry_wall;
        os->completions = sl->completions;
        os->bonus_sec = sl->bonus_sec;
        os->bonus_applied = sl->bonus_applied;
        os->adjust_today_sec = sl->adjust_today_sec;
    }
    memcpy(out->date, g_rtc_state.last_date, sizeof(out->date));
    out->checksum = timer_snapshot_checksum(out);
}

static bool snapshot_valid(const timer_snapshot_t *snap, time_t now) {
    if (snap->version != TIMER_SNAPSHOT_VERSION)
        return false;
    /* All-zeros XORs to 0 — indistinguishable from blank storage */
    if (snap->slots[0].state == 0 && snap->slots[0].expiry_wall_time == 0 && snap->date[0] == '\0')
        return false;
    if (timer_snapshot_checksum(snap) != snap->checksum)
        return false;
    if (snap->active_slot >= TIMER_SLOT_COUNT)
        return false;
    if (snap->break_interrupted_slot >= TIMER_SLOT_COUNT)
        return false;
    if (snap->run_segment_slot >= TIMER_SLOT_COUNT)
        return false;
    /* Leaving a break INTO a break is nonsense, and a state past the
       enum would be restored straight into the state machine. */
    if (snap->break_prev_state >= TIMER_BREAK)
        return false;
    for (int i = 0; i < TIMER_SLOT_COUNT; i++) {
        const timer_snapshot_slot_t *sl = &snap->slots[i];
        if (sl->state > TIMER_BREAK)
            return false;
        /* BREAK belongs to slot 0 alone (see timer.h): corruption that
           slips past the checksum must not resurrect one elsewhere. */
        if (i > 0 && (sl->state == TIMER_BREAK || sl->break_expiry_wall != 0))
            return false;
        if (sl->allocation_sec < 0 || sl->allocation_sec > SNAPSHOT_MAX_HORIZON_SEC)
            return false;
        if (sl->remaining_at_pause < 0 || sl->remaining_at_pause > sl->allocation_sec)
            return false;
        /* Signed since the adjust feature: a banked deduction is negative */
        if (sl->bonus_sec < -SNAPSHOT_MAX_HORIZON_SEC || sl->bonus_sec > SNAPSHOT_MAX_HORIZON_SEC)
            return false;
        /* Same bound, same reason. A running total of repeatable grants
           has no natural ceiling of its own, so it borrows the horizon:
           anything past a week of adjustment in one day is corruption,
           not a parent. */
        if (sl->adjust_today_sec < -SNAPSHOT_MAX_HORIZON_SEC || sl->adjust_today_sec > SNAPSHOT_MAX_HORIZON_SEC)
            return false;
        if (sl->state == TIMER_RUNNING) {
            int64_t delta = sl->expiry_wall_time - (int64_t)now;
            if (delta > SNAPSHOT_MAX_HORIZON_SEC || delta < -SNAPSHOT_MAX_HORIZON_SEC)
                return false;
        }
        if (sl->state == TIMER_BREAK) {
            int64_t delta = sl->break_expiry_wall - (int64_t)now;
            if (delta > SNAPSHOT_MAX_HORIZON_SEC || delta < -SNAPSHOT_MAX_HORIZON_SEC)
                return false;
        }
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
    date_fmt_iso(today, sizeof(today), &tm_now);
    if (strcmp(today, snap->date) != 0)
        return false;

    g_rtc_state.active_slot = snap->active_slot;
    g_rtc_state.break_interrupted_slot = snap->break_interrupted_slot;
    g_rtc_state.break_prev_state = snap->break_prev_state;
    g_rtc_state.run_segment_slot = snap->run_segment_slot;
    int64_t expired_at = 0; /* 0 = no powered-off expiry to fold */
    for (int i = 0; i < TIMER_SLOT_COUNT; i++) {
        const timer_snapshot_slot_t *ss = &snap->slots[i];
        timer_slot_state_t *sl = &g_rtc_state.slots[i];
        sl->state = (timer_state_t)ss->state;
        sl->remaining_at_pause = ss->remaining_at_pause;
        sl->allocation_sec = ss->allocation_sec;
        sl->expiry_wall_time = ss->expiry_wall_time;
        sl->run_accum_sec = ss->run_accum_sec;
        sl->run_started_wall = ss->run_started_wall;
        sl->break_expiry_wall = ss->break_expiry_wall;
        sl->completions = ss->completions;
        sl->bonus_sec = ss->bonus_sec;
        sl->bonus_applied = ss->bonus_applied;
        sl->adjust_today_sec = ss->adjust_today_sec;
        /* Expiry passed while powered off (snapshot saved before the EXPIRED
           transition landed): restore directly as EXPIRED so the next tick
           does not re-transition and re-fire the already-heard alert. The
           run still reached 00:00 — count it. Through mark_expired, so
           this site cannot drift from the live one: it also clears the
           banked remaining, without which the slot restores EXPIRED still
           carrying whatever it held at its last pause. */
        if (sl->state == TIMER_RUNNING && sl->expiry_wall_time <= (int64_t)now) {
            /* The run ended at its expiry, not now: fold the balance's
               live segment there (I10) so the powered-off gap after it
               adds nothing. Deferred past the loop because the fold reads
               run_segment_slot, and the segment is slot 0's — folding
               mid-loop would race the restore of slot 0's own fields. */
            expired_at = sl->expiry_wall_time;
            mark_expired(sl);
        }
        /* Break finished while powered off: restore per I7 — the state
           slot 0 held when the break started. */
        if (sl->state == TIMER_BREAK && sl->break_expiry_wall <= (int64_t)now) {
            sl->state = (timer_state_t)g_rtc_state.break_prev_state;
            sl->break_expiry_wall = 0;
        }
    }
    /* Signed by run_segment_slot like every other fold, not by the slot
       that expired: the two agree in any uncorrupted snapshot (the
       expiring slot is the one that armed the segment), and going through
       the same helper keeps the "sign comes from the arming slot" rule
       true without exception. */
    if (expired_at != 0)
        fold_run_segment((time_t)expired_at);
    memcpy(g_rtc_state.last_date, snap->date, sizeof(g_rtc_state.last_date));
    /* The firmware may have been reflashed with this slot removed from
       menuconfig — never strand the device on a slot the buttons can no
       longer reach, and never let its run survive the definition (BUG-7).

       The orphan's segment folds at `now`, the restore time. The snapshot
       carries no save timestamp, and a timestamp would not help: a
       RUNNING slot's snapshot bytes do not change from wake to wake, so
       timer_persist_save skips the write and the blob's age is that of the
       last state change, not of the last wake. Nothing later than `now`
       can be swept in, which is what the defect was.

       The time up to `now` CAN include a power-off. There are two restore
       sites: the boot restore (main.c), which needs a clock that survived,
       and the rollover restore (wake_flow_handle_day_rollover), which runs
       after a genuine power-off once NTP has corrected the clock. On that
       path the dark time folds as exposure — but bounded by the run's own
       remaining time: the restore loop above has already made an orphan
       that expired in the dark EXPIRED and folded it at its expiry, and
       this call leaves it alone. That is the same span a still-defined
       timer's wall-clock countdown uses up across the same power-off.
       Accepted (2026-09-25): it errs toward more eye rest, as the
       2026-08-07 sign decision does. */
    timer_ensure_active_slot_enabled(now);
    return true;
}
