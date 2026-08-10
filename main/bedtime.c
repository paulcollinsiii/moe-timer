/* Bed Time gate policy — pure decisions over the quiet-hours HHMM
   helpers; no ESP dependencies (see bedtime.h). */
#include "bedtime.h"

#include "quiet_hours.h"

#define BEDTIME_HHMM_FLOOR 1800

bool bedtime_hhmm_valid(int hhmm) {
    if (hhmm == 0)
        return true; /* disabled */
    return quiet_hhmm_valid(hhmm) && hhmm >= BEDTIME_HHMM_FLOOR;
}

int bedtime_minutes(int hhmm) {
    if (hhmm == 0 || !bedtime_hhmm_valid(hhmm))
        return -1;
    return quiet_hhmm_to_minutes(hhmm);
}

bool bedtime_active(int now_min, int bed_min) {
    return bed_min >= 0 && now_min >= bed_min;
}

bool bedtime_break_would_cross(int now_min, int dur_min, int bed_min) {
    return bed_min >= 0 && now_min < bed_min && now_min + dur_min >= bed_min;
}

bool bedtime_should_alert(timer_state_t state, bool break_active) {
    /* break_active is read from slot 0, not from `state`: a Screen Break
       can run behind any selected timer, so an IDLE/PAUSED active slot
       still means "we interrupted a break". */
    return state == TIMER_RUNNING || state == TIMER_BREAK || break_active;
}
