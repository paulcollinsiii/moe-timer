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

/* How late a break end may be OBSERVED and still chime. Derived, not
   chosen: the watch window already means "close enough to the event that
   the firmware owns it", so the rule reads as "chime only if we were
   inside the watch window for it". Every path that should chime observes
   the transition inside the awake watch (~1 s) or on the next tick wake
   (60 s max); the paths that deliberately stop doing timer work — charge
   lock (600 s naps), bed-time lock (7200 s), power cycle — land far
   outside it and go silent, matching what the snapshot restore already
   does with an elapsed break. */
#define BREAK_CHIME_GRACE_SEC SLEEP_PLAN_WATCH_SEC

typedef struct {
    timer_state_t state;
    int sec_into_minute;         /* time(NULL) % 60, 0..59 */
    int32_t event_remaining_sec; /* RUNNING: to expiry; BREAK: to break end; else 0 */
    bool sync_due_by_next_wake;  /* RUNNING only; false otherwise */
    /* Optional SECONDARY event: a Screen Break running on slot 0 behind
       another selected timer. 0 = none. main.c fills this only when the
       break end will actually chime (nothing RUNNING) — a suppressed end
       is silent and needs no dedicated wake, it just drops the chip at
       whatever the next tick wake is. Ignored when the break IS the
       primary event (state == TIMER_BREAK, i.e. Screen selected). */
    int32_t break_remaining_sec;
} sleep_plan_in_t;

/* Seconds to deep-sleep before the next wake. Pure — host-tested. */
int32_t sleep_plan_seconds(const sleep_plan_in_t *in);

#ifdef __cplusplus
}
#endif
