#pragma once
#include <stdbool.h>
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

/* ---- Button A: the mode toggle (design 4.2, row C3) --------------------- */

typedef enum {
    BTN_A_NONE = 0, /* refused: see button_a_toggle_allowed() */
    BTN_A_CHORES,   /* now painting the chore checklist */
    BTN_A_TIMERS,   /* now painting the timer screen */
} btn_a_action_t;

/* Whether a press of A would do anything, and the reason this is a
   PUBLIC predicate rather than an `if` inside button_a_apply(): the
   answer is also what arms A as an EXT1 wake source. buttons.c reads it
   at sleep entry and hands it to buttons_policy_wake_mask() in
   buttons_policy_in_t.mode_toggle_allowed, so a press that could only be
   refused does not burn a wake and a panel refresh. Whatever else reads
   it — the chore screen's Button-A label, when M2-T4/T5 add one — must
   read THIS, not a restatement of it: two copies of the rule drift, and
   both drift directions are bad (a button armed at sleep and refused on
   arrival, or a label offering something the map then refuses).

   Two refusal reasons, and they are ORs of each other:

     the active slot is RUNNING. Design 4.2: you cannot tick off "dishes
       away" while the TV clock ticks. This is the RUNNING half of
       timer_swap_allowed() WITHOUT that function's second condition that
       extras must exist — a device with no extra timers configured still
       reaches its chore list. Because only the active slot can ever be
       RUNNING, "no timer is running" and "the active slot is not RUNNING"
       are the same test.
       BREAK is not RUNNING, so chore mode is reachable throughout a
       screen break, which is exactly where 2.6 wants it; a BACKGROUND
       break with an extra timer running refuses, correctly, because the
       kid is at the violin.
       BLANKET — it refuses the way out of chore mode as well as the way
       in. That is what licenses dropping A from the wake mask while a
       timer runs: "a press that could only be refused" is only true of
       both directions. Button B still pauses, and the day rollover
       reverts the mode, so this is not a trap.

     no chores are configured. Nothing to switch to. Reads the count out
       of the names blob, which is the only place it exists — the RTC
       block holds the acks, the release and the mode but no count
       (timer.h).
       THE COST, inherited from design 4.2 and stated rather than hidden:
       chore_store_load_names() reports n = 0 on EVERY failure, so an
       unreadable blob refuses the toggle exactly like an empty list. It
       self-heals — a refusal stores nothing, so the next good read gives
       the button back — which is the one thing that keeps this cheaper
       than the emptied-list guard's stored revert.

   NOT clock-dependent, unlike every Button B entry point above: a toggle
   reads no time and stores none, so there is no `now` to be handed. */
bool button_a_toggle_allowed(void);

/* Apply the toggle to the stored mode and report which screen the device
   should now paint. BTN_A_NONE when button_a_toggle_allowed() says no,
   and in that case NOTHING is stored — a refusal must not move the byte
   the next press reads. The mode is a render selector: this touches no
   slot, no expiry and no schedule. */
btn_a_action_t button_a_apply(void);

#ifdef __cplusplus
}
#endif
