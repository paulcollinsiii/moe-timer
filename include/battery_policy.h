#pragma once
#include <stdbool.h>

/* Low-battery policy (pure, host-tested). Two tiers below normal:
   - WARN (<= 15%): the main layout's progress bar carries a
     "Charge Me!!!" badge; everything else keeps working.
   - LOCK (<= 10%): full stop — the panel shows only "Charge Me!" and the
     device sleeps long intervals with buttons/NTP/timer actions disabled
     (an e-ink refresh during brownout can leave persistent artifacts).
   Hysteresis: once locked, stay locked until the reading clears the WARN
   band (> 15%) so readings bouncing around 10% cannot flap the panel. */

#define BATT_WARN_PCT 15
#define BATT_LOCK_PCT 10

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BATT_OK = 0,
    BATT_WARN,
    BATT_LOCK,
} batt_policy_t;

batt_policy_t battery_policy_evaluate(int pct, bool currently_locked);

#ifdef __cplusplus
}
#endif
