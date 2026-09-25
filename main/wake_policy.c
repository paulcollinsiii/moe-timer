/* Pure wake-orchestration decisions — no ESP dependencies; host-tested. */
#include "wake_policy.h"

#include "sleep_plan.h" /* BREAK_CHIME_GRACE_SEC */

bool wake_policy_break_chime(bool extra_running, int32_t overdue_sec) {
    if (extra_running)
        return false; /* that timer's own alert is the one that matters */
    return overdue_sec <= BREAK_CHIME_GRACE_SEC;
}

wake_render_t wake_policy_render(timer_state_t before, timer_state_t after, bool button_wake, bool break_ended,
                                 bool selection_changed) {
    /* A swap onto an already-expired slot is a selection change, not a
       transition — the alarm was heard when that timer actually ran. */
    if (after == TIMER_EXPIRED && before != TIMER_EXPIRED && !selection_changed)
        return WAKE_RENDER_EXPIRY_ALERT; /* TIME'S UP outranks a repaint */
    if (break_ended) {
        /* A break can end behind another selected timer, changing the
           panel without changing that timer's state: the inverted BREAK
           chip vanishes, and when the end chimed the selection has also
           snapped back to Screen — a different layout entirely. Either
           way a partial diff would ghost. */
        return WAKE_RENDER_FULL;
    }
    if (button_wake) {
        /* Buttons ride the partial cadence for snappy feedback, except
           across the break screen: that layout is a full-screen inversion
           of the main one, and a partial diff across it would ghost the
           whole panel.

           THE EXCEPTION BELOW IS NO LONGER THE WHOLE LIST, and this
           function cannot hold the rest. It sees only two timer states,
           so the break boundary is the one full-screen layout change it
           can detect; the chore checklist is a third layout selected by
           the mode and the chore count (display_screen_for), and neither
           is an input here. Three promotions therefore sit ABOVE this
           answer, in render_action_result() (wake_flow.c), and a caller
           reading PARTIAL off this function alone is reading half the
           decision:

             the screen-kind test   display_screen_for(before) against
                                    display_screen_for(after) — the same
                                    rule as this one, generalised to every
                                    pair of layouts.
             the mode toggle        wake-sticky, because a toggle changes
                                    the layout while every input this
                                    function can see stays put.
             Button D               the user-facing full refresh, minus
                                    the chore-ack suppression.

           Left here rather than moved up: the boundary below is the only
           one derivable from this signature, and widening the signature
           to take a mode and a count would move a paint decision into a
           module that deliberately has no display dependency. */
        if ((before == TIMER_BREAK) != (after == TIMER_BREAK))
            return WAKE_RENDER_FULL;
        return WAKE_RENDER_PARTIAL;
    }
    if (after != before)
        return WAKE_RENDER_FULL;
    return WAKE_RENDER_PARTIAL;
}

int32_t wake_policy_snap_minute(int32_t sec, int32_t watch_threshold_sec) {
    if (sec <= watch_threshold_sec)
        return sec;
    int32_t m = sec % 60;
    if (m <= 2)
        return sec - m;
    if (m >= 58)
        return sec + 60 - m;
    return sec;
}

bool wake_policy_sync_due(timer_state_t state, bool running_recheck_due, time_t now, time_t last_sync,
                          int32_t idle_interval_sec) {
    switch (state) {
        case TIMER_RUNNING:
            return running_recheck_due;
        case TIMER_BREAK:
            return false;
        default: /* IDLE / PAUSED / EXPIRED: long-lived clock-only states */
            return last_sync == 0 || (int64_t)now - (int64_t)last_sync >= idle_interval_sec;
    }
}

int32_t wake_policy_grid_wait_sec(timer_state_t state, int32_t event_remaining_sec, int sec_into_minute,
                                  int32_t max_wait_sec) {
    int32_t to;
    if (state == TIMER_RUNNING || state == TIMER_BREAK) {
        if (event_remaining_sec <= 0)
            return 0; /* event passed: the watch/alert path owns it now */
        to = event_remaining_sec % 60;
    } else {
        to = 60 - sec_into_minute;
        if (to == 60)
            to = 0; /* already on the wall boundary */
    }
    return (to > 0 && to <= max_wait_sec) ? to : 0;
}

static const int32_t COUNTDOWN_STEPS[WAKE_COUNTDOWN_STEPS] = {60, 45, 30, 15};

int32_t wake_policy_countdown_step(int idx) {
    if (idx < 0 || idx >= WAKE_COUNTDOWN_STEPS)
        return 0;
    return COUNTDOWN_STEPS[idx];
}

int wake_policy_first_countdown_step(int32_t remaining_sec) {
    int idx = 0;
    while (idx < WAKE_COUNTDOWN_STEPS && COUNTDOWN_STEPS[idx] > remaining_sec) {
        idx++; /* woke late (e.g. slow sync): skip already-passed marks */
    }
    return idx;
}
