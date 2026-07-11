#pragma once
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BTN_A_NONE = 0, /* BREAK/EXPIRED: wake-press-only contexts, no transition */
    BTN_A_PAUSED,
    BTN_A_STARTED,
    BTN_A_RESUMED,
} btn_a_action_t;

/* The one Button-A state map, shared by the EXT1 wake handler and the
   awake join-poll: RUNNING pauses, IDLE starts with the right allocation
   (selected extra timer's duration, else today's schedule), PAUSED
   resumes. Applies the transition to the active slot and reports what it
   did. */
btn_a_action_t button_a_apply(time_t now);

/* Allocation a fresh start gets: selected def's duration, else the
   day-schedule allocation. */
int32_t button_a_start_allocation(time_t now);

#ifdef __cplusplus
}
#endif
