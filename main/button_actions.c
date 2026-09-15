#include "button_actions.h"

#include "chore_store.h"
#include "schedule.h"
#include "timer.h"

bool button_a_toggle_allowed(void) {
    /* THE RUNNING HALF OF timer_swap_allowed(), and only that half —
       design 4.2 spells out that the extras condition does not come with
       it. Written as the comparison rather than as a call so the two
       cannot be "simplified" into one: a device with no extra timers
       configured has swap_allowed false forever and must still reach its
       chore list.

       Only the active slot can ever be RUNNING (timer.c), so this is
       simultaneously "no timer is running" and "the active slot is not
       RUNNING". TIMER_BREAK is not TIMER_RUNNING, which is the whole of
       why chore mode is reachable throughout a screen break; a background
       break with an extra running arrives here as TIMER_RUNNING on the
       ACTIVE slot and is refused. */
    if (timer_get_state() == TIMER_RUNNING) {
        return false;
    }
    /* FIRST because it is free (one RTC byte) and this is not: the
       sleep-entry caller reaches NVS only when the cheap half says
       "maybe". See buttons.c for why that ordering is worth writing down
       there rather than only here.

       The names are loaded and thrown away because the count is what is
       being asked for and chore_store has no count-only entry point —
       adding one would put a second reader of the blob layout beside the
       one that already validates it. 64 bytes of stack (CHORE_MAX 3 x
       CHORE_NAME_BUF 21 = 63, plus the count) against a second place for
       the version rule to be got wrong — the arithmetic is spelled out so
       the number cannot drift silently when either constant moves. */
    char names[CHORE_MAX][CHORE_NAME_BUF];
    uint8_t n = 0;
    /* Return code deliberately discarded, exactly as app_state.c does:
       every failure path in chore_store_load_names() sets n = 0 first, so
       "unreadable" and "not configured" land on the same answer and there
       is nothing for this layer to do differently. Design 4.2 records the
       cost of that (an NVS fault refuses the toggle while it lasts) and
       accepts it; the refusal stores nothing, so it self-heals on the
       next good read. */
    (void)chore_store_load_names(names, &n);
    return n > 0;
}

btn_a_action_t button_a_apply(void) {
    if (!button_a_toggle_allowed()) {
        return BTN_A_NONE;
    }
    /* Toggle, not a set: A is the only control the user has over the
       mode, so it has to be able to leave chore mode as well as enter it.
       Written against APP_MODE_CHORES rather than APP_MODE_TIMERS because
       the stored byte is NOT clamped (timer.h) — anything that is not
       APP_MODE_CHORES is Timers, so a byte outside the enum toggles INTO
       chores and the next press brings it back, instead of the mode
       getting stuck on an unpaintable value. */
    const app_mode_t next = (timer_mode() == APP_MODE_CHORES) ? APP_MODE_TIMERS : APP_MODE_CHORES;
    timer_set_mode(next);
    return (next == APP_MODE_CHORES) ? BTN_A_CHORES : BTN_A_TIMERS;
}

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
