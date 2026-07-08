/* Pure low-battery policy — no ESP dependencies; host-tested. */
#include "battery_policy.h"

batt_policy_t battery_policy_evaluate(int pct, bool currently_locked) {
    int lock_below = currently_locked ? BATT_WARN_PCT : BATT_LOCK_PCT; /* hysteresis */
    if (pct <= lock_below)
        return BATT_LOCK;
    if (pct <= BATT_WARN_PCT)
        return BATT_WARN;
    return BATT_OK;
}
