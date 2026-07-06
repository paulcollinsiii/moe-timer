#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "timer.h" /* timer_state_t */

#ifdef __cplusplus
extern "C" {
#endif

/* Internal leads — correctness margins, deliberately NOT menuconfig. */
#define SLEEP_PLAN_MIN_SEC 5         /* never nap shorter than this */
#define SLEEP_PLAN_SYNC_LEAD_SEC 20  /* wake early so NTP finishes before :00 */
#define SLEEP_PLAN_EVENT_LEAD_SEC 70 /* wake with ~70 s to expiry/break end */
/* Awake-side watch threshold: a wake with event_remaining <= this enters the
   stay-awake watch instead of sleeping again (= LEAD + margin, so a
   planner-landed wake at ~70 s always qualifies). */
#define SLEEP_PLAN_WATCH_SEC (SLEEP_PLAN_EVENT_LEAD_SEC + 5)

typedef struct {
    timer_state_t state;
    int sec_into_minute;         /* time(NULL) % 60, 0..59 */
    int32_t event_remaining_sec; /* RUNNING: to expiry; BREAK: to break end; else 0 */
    bool sync_due_by_next_wake;  /* RUNNING only; false otherwise */
} sleep_plan_in_t;

/* Seconds to deep-sleep before the next wake. Pure — host-tested. */
int32_t sleep_plan_seconds(const sleep_plan_in_t *in);

#ifdef __cplusplus
}
#endif
