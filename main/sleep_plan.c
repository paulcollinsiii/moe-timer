/* Pure sleep-duration planner — no ESP dependencies; host-tested.

   Consolidates every "how long should the device sleep" rule:
   - clock-only states (IDLE/PAUSED/EXPIRED) align wakes to WALL-CLOCK
     minute boundaries so the header time flips with real clocks
   - RUNNING/BREAK align to the COUNTDOWN's minute grid — wake when the
     remaining value hits a round minute, so the display truly reads
     1:11:00 (a fresh start/resume shows one precise value, then the next
     wake lands on the grid)
   - RUNNING wakes SLEEP_PLAN_SYNC_LEAD_SEC early when an NTP sync will be
     due, so the sync finishes before the grid render
   - RUNNING/BREAK wakes land ~SLEEP_PLAN_EVENT_LEAD_SEC before their event
     (expiry / break end) so the awake-side watch loop takes over
   - a Screen Break running BEHIND another selected timer is a secondary
     event with the same lead, so its chime lands on time too

   sleep_plan_from_timer() below owns the other half: folding the raw
   timer readings into that input, i.e. which reading each state actually
   uses and when a background break earns its own wake. It lives here
   rather than in main.c because it is all decision and no device. */
#include "sleep_plan.h"

int32_t sleep_plan_seconds(const sleep_plan_in_t *in) {
    int32_t sleep_sec;

    if (in->state == TIMER_RUNNING || in->state == TIMER_BREAK) {
        /* Countdown grid: seconds until event_remaining is a round minute */
        sleep_sec = in->event_remaining_sec % 60;
        if (sleep_sec == 0)
            sleep_sec = 60;
        if (in->state == TIMER_RUNNING && in->sync_due_by_next_wake)
            sleep_sec -= SLEEP_PLAN_SYNC_LEAD_SEC;
        if (sleep_sec < SLEEP_PLAN_MIN_SEC)
            sleep_sec += 60; /* grid point too close — take the previous grid minute */

        /* Event handoff: land ~LEAD before expiry/break end for the watch */
        if (in->event_remaining_sec > 0) {
            int32_t to_event_wake = in->event_remaining_sec - SLEEP_PLAN_EVENT_LEAD_SEC;
            if (to_event_wake < sleep_sec)
                sleep_sec = to_event_wake;
        }
    } else {
        /* Wall grid for the clock-only states */
        sleep_sec = 60 - in->sec_into_minute;
        if (sleep_sec < SLEEP_PLAN_MIN_SEC)
            sleep_sec += 60;
    }

    /* Secondary event: a break running on slot 0 behind the selected
       timer, whose end will chime. Same lead as the primary — the awake
       watch takes over from there. Only ever pulls the wake IN. */
    if (in->break_remaining_sec > 0) {
        int32_t to_break_wake = in->break_remaining_sec - SLEEP_PLAN_EVENT_LEAD_SEC;
        if (to_break_wake < sleep_sec)
            sleep_sec = to_break_wake;
    }

    if (sleep_sec < SLEEP_PLAN_MIN_SEC)
        sleep_sec = SLEEP_PLAN_MIN_SEC;
    return sleep_sec;
}

sleep_plan_in_t sleep_plan_from_timer(const sleep_plan_timer_in_t *in) {
    sleep_plan_in_t out = {
        .state = in->state,
        .sec_into_minute = (int)(in->now % 60),
        .event_remaining_sec = 0,
        .sync_due_by_next_wake = false,
        .break_remaining_sec = 0,
    };
    if (in->state == TIMER_RUNNING) {
        out.event_remaining_sec = (int32_t)(in->expiry_wall - (int64_t)in->now);
        /* Left signed on purpose: a wake that arrives after the expiry
           reports a NEGATIVE remaining, and the planner's own clamp is
           what handles it. Folding to 0 here would move the grid. */
        out.sync_due_by_next_wake = in->ntp_recheck_due;
    } else if (in->state == TIMER_BREAK) {
        /* Screen selected during its own break: the break end IS the
           primary event, so it goes in event_remaining_sec and the
           secondary rule below deliberately skips this state. */
        out.event_remaining_sec = in->break_remaining_sec;
    }
    /* Secondary event: a break running behind another selected timer.
       Only when its end will actually CHIME does it need a dedicated
       wake — a suppressed end (an extra timer RUNNING) is silent, so it
       can land at whatever the next tick wake is and just drop the chip
       there. Suppression can only change via a button press, which is a
       wake and therefore a re-plan. */
    if (in->break_active && in->state != TIMER_BREAK && !in->extra_running) {
        out.break_remaining_sec = in->break_remaining_sec;
    }
    return out;
}

wake_sleep_mode_t wake_sleep_mode_select(bool charge_locked, bool bedtime_locked, bool config_locked) {
    if (charge_locked)
        return WAKE_SLEEP_CHARGE_LOCK;
    if (bedtime_locked)
        return WAKE_SLEEP_BEDTIME;
    if (config_locked)
        return WAKE_SLEEP_CONFIG_ERR;
    return WAKE_SLEEP_NORMAL;
}

sleep_outcome_t sleep_plan_outcome(wake_sleep_mode_t mode, const sleep_plan_in_t *in) {
    /* No default case on purpose: adding an enumerator without handling it
       is a hard -Wswitch error in the FIRMWARE build, which compiles
       -Werror. The host suites run -Wall -Wextra without -Werror, so there
       it is only a warning — which is why the fallback below fails closed
       instead of trusting the switch to be exhaustive. */
    switch (mode) {
        case WAKE_SLEEP_NORMAL:
            /* sleep_plan_seconds() clamps to SLEEP_PLAN_MIN_SEC, so the
               unsigned narrowing can never see a negative. */
            return (sleep_outcome_t){.seconds = (uint32_t)sleep_plan_seconds(in), .enable_buttons = true, .reason = ""};
        case WAKE_SLEEP_CHARGE_LOCK:
            return (sleep_outcome_t){
                .seconds = CHARGE_LOCK_SLEEP_SEC, .enable_buttons = false, .reason = "charge lock, "};
        case WAKE_SLEEP_BEDTIME:
            return (sleep_outcome_t){.seconds = BEDTIME_SLEEP_SEC, .enable_buttons = false, .reason = "bed time, "};
        case WAKE_SLEEP_CONFIG_ERR:
            /* THE ONE LOCK THAT ARMS ANYTHING, and `true` here is the
               whole of it: buttons_policy.c narrows the mask to D alone
               off lock_gate_wake_d_only(), but it never gets asked
               unless this flag says the driver may arm at all. Flipping
               this to false to "match the other two" leaves a device that
               can only be recovered with a serial cable. The reason names
               both locks that sleep this sleep (BUG-14 folds no-clock in):
               "config error" on a device whose config is fine misleads. */
            return (sleep_outcome_t){
                .seconds = CONFIG_ERR_SLEEP_SEC, .enable_buttons = true, .reason = "config/no-clock lock, "};
    }
    /* Unreachable while wake_sleep_mode_select() is the only producer, but
       a cast value would land here. Fail CLOSED: this whole mechanism
       exists to keep the buttons dark on a battery that cannot afford a
       refresh, so an unrecognised mode takes the lock interval with no
       wake sources rather than a planner nap with buttons armed. */
    return (sleep_outcome_t){.seconds = CHARGE_LOCK_SLEEP_SEC, .enable_buttons = false, .reason = "unknown mode, "};
}
