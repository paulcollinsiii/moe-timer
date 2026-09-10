#include "button_actions.h"

#include "schedule.h"
#include "timer.h"

int32_t button_b_start_allocation(time_t now) {
    const timer_def_t *def = timer_active_def();
    return (def != NULL) ? def->duration_sec : (int32_t)schedule_get_allocation_sec(schedule_get_day_type(now));
}

btn_b_action_t button_b_apply(time_t now) {
    /* EXPIRED first, and deliberately AHEAD of the break gate below: on an
       expired slot B has no start/pause/resume job, so that is where Reload
       lives, and a reload is not a start. timer_reload() returns the slot to
       IDLE at full duration rather than running anything, and the direct
       dispatch this leg replaces was gated only by timer_reload_allowed() —
       putting it behind timer_start_allowed() would silently stop a
       non-break-eligible expired extra from being reloaded during a break.
       timer_reload_allowed() carries the rest: never while RUNNING, and only
       a reloadable def, so Screen (no def) can never be reset here. */
    if (timer_get_state() == TIMER_EXPIRED) {
        return (timer_reload_allowed() && timer_reload()) ? BTN_B_RELOADED : BTN_B_NONE;
    }
    /* A Screen Break refuses to START anything that is not a genuine break
       activity (rule 7) — including Screen itself, which is how the break
       has always been enforced. Pausing is never gated: stopping is always
       safe, and a RUNNING slot during a break is break-eligible anyway. */
    if (timer_get_state() != TIMER_RUNNING && !timer_start_allowed()) {
        return BTN_B_NONE;
    }
    switch (timer_get_state()) {
        case TIMER_RUNNING:
            timer_pause(now);
            return BTN_B_PAUSED;
        case TIMER_IDLE:
            timer_start(now, button_b_start_allocation(now));
            return BTN_B_STARTED;
        case TIMER_PAUSED:
            timer_resume(now);
            return BTN_B_RESUMED;
        default:
            return BTN_B_NONE; /* TIMER_BREAK: EXPIRED already returned above */
    }
}
