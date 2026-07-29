#include "button_actions.h"

#include "schedule.h"
#include "timer.h"

int32_t button_a_start_allocation(time_t now) {
    const timer_def_t *def = timer_active_def();
    return (def != NULL) ? def->duration_sec : (int32_t)schedule_get_allocation_sec(schedule_get_day_type(now));
}

btn_a_action_t button_a_apply(time_t now) {
    /* A Screen Break refuses to START anything that is not a genuine break
       activity (rule 7) — including Screen itself, which is how the break
       has always been enforced. Pausing is never gated: stopping is always
       safe, and a RUNNING slot during a break is break-eligible anyway. */
    if (timer_get_state() != TIMER_RUNNING && !timer_start_allowed()) {
        return BTN_A_NONE;
    }
    switch (timer_get_state()) {
        case TIMER_RUNNING:
            timer_pause(now);
            return BTN_A_PAUSED;
        case TIMER_IDLE:
            timer_start(now, button_a_start_allocation(now));
            return BTN_A_STARTED;
        case TIMER_PAUSED:
            timer_resume(now);
            return BTN_A_RESUMED;
        default:
            return BTN_A_NONE;
    }
}
