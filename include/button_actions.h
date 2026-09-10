#pragma once
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BTN_B_NONE = 0, /* BREAK, or an EXPIRED slot that cannot reload: no transition */
    BTN_B_PAUSED,
    BTN_B_STARTED,
    BTN_B_RESUMED,
    BTN_B_RELOADED, /* EXPIRED reloadable slot returned to IDLE at full duration */
} btn_b_action_t;

/* The one Button-B state map, shared by the EXT1 wake handler and the
   awake join-poll: RUNNING pauses, IDLE starts with the right allocation
   (selected extra timer's duration, else today's schedule), PAUSED
   resumes, and EXPIRED reloads when the slot's def is reloadable — the
   one state where B would otherwise have no job at all. Applies the
   transition to the active slot and reports what it did. */
btn_b_action_t button_b_apply(time_t now);

/* Allocation a fresh start gets: selected def's duration, else the
   day-schedule allocation. */
int32_t button_b_start_allocation(time_t now);

#ifdef __cplusplus
}
#endif
