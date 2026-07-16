/* Pure wake-orchestration decisions — no ESP dependencies; host-tested. */
#include "wake_policy.h"

wake_render_t wake_policy_render(timer_state_t before, timer_state_t after, bool button_wake) {
    if (after == TIMER_EXPIRED && before != TIMER_EXPIRED)
        return WAKE_RENDER_EXPIRY_ALERT;
    if (button_wake) {
        /* Buttons ride the partial cadence for snappy feedback, except
           across the break screen: that layout is a full-screen inversion
           of the main one, and a partial diff across it would ghost the
           whole panel. */
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
