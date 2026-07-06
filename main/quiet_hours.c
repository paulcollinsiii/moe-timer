/* Pure quiet-hours window logic — no ESP dependencies; host-tested. */
#include "quiet_hours.h"

bool quiet_hours_active(int now_min, int start_min, int end_min) {
    if (start_min == end_min)
        return false; /* zero-length window = disabled */
    if (start_min < end_min)
        return now_min >= start_min && now_min < end_min;
    /* window wraps midnight (e.g. 22:30 -> 08:00) */
    return now_min >= start_min || now_min < end_min;
}

int quiet_hhmm_to_minutes(int hhmm) {
    return (hhmm / 100) * 60 + (hhmm % 100);
}
