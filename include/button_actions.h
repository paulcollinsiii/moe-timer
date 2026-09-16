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
   day-schedule allocation MINUS whatever the chore gate is withholding
   today.

   THE SUBTRACTION IS THE GATE. chores.h names two calling shapes that
   each total one allocation per day, and this is half of the second one:
   carry `allocation - withheld` as a live cap, and let the release edge
   contribute the difference once. Design 4.1's worked example is that cap
   on the panel — a 60 min day with `chore_free: 20` and ten minutes used
   reads 00:10:00, not 00:50:00 — and timer_release_gated()'s IDLE refusal
   rests on the other half ("the allocation is read live at the next start
   and the gate's own `released` latch already makes it full").

   Without it the release is not a release: the day would already be
   whole, and finishing the chores would add a SECOND allocation on top of
   it. chores_withheld_sec() saturates, so the result can never go
   negative or wrap, and it is 0 on every device with no chore list —
   which is the whole fleet, and why this number is unchanged there.

   SCREEN ONLY, like the gate itself: an extra's duration comes from its
   def and no chore setting may touch it. */
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

/* ---- Buttons B, C and D in chore mode: the ack (design 2.4) ------------- */

/* Which chore each button ticks. Design 2.4's checklist has THREE FIXED
   ROWS — "Nothing slides, nothing refills, the row order never changes —
   which is what makes B/C/D unambiguous" — so this is a constant mapping
   and not a lookup: row i is ack button i, always, whatever is on the
   list and whatever has been ticked.

   Macros over a `button_id_t -> index` function so this header does not
   have to include buttons.h. That header is the GPIO-facing driver's, and
   pulling it in would put it into every host suite that includes this one
   with no use for it; the call sites that map a button to a row are the
   wake flow's button arms, which know which button they are. */
#define BUTTON_CHORE_IDX_B 0u
#define BUTTON_CHORE_IDX_C 1u
#define BUTTON_CHORE_IDX_D 2u

typedef enum {
    BTN_ACK_NONE = 0, /* refused: see button_chore_ack_allowed() */
    BTN_ACK_TOGGLED,  /* the bit moved; the day was not released by this press */
    /* the bit moved AND this press released the day's withheld remainder.
       NOT a promise that seconds were granted: on a day whose gate is off
       (chore_free >= allocation) the edge still fires and the amount is 0
       — chores_release_due() documents that distinction. */
    BTN_ACK_RELEASED,
} btn_ack_action_t;

/* Whether a press of chore `idx`'s ack button would do anything, and —
   exactly like button_a_toggle_allowed() above — the reason this is a
   PUBLIC predicate rather than an `if` inside the apply: the answer is
   also what arms Button C as an EXT1 wake source. buttons.c reads it at
   sleep entry for BUTTON_CHORE_IDX_C and hands it to
   buttons_policy_wake_mask() in buttons_policy_in_t.chore_ack_allowed.

   WHY C AND ONLY C. B and D are unconditional wake sources, so rows 1 and
   3 can always be ticked from sleep. C is gated on timer_swap_allowed(),
   which is false forever on a device with no extra timers configured — so
   without this the middle checkbox would be dead from sleep while the two
   either side of it worked, which is the "primary control dead to the
   press" failure buttons_policy.c is otherwise built to avoid.

   Two refusal reasons, ANDed:

     the device is not in chore mode. B, C and D are the timer controls
       then, and rebinding them would be a silent mis-action rather than a
       refused one.

     `idx` is not a configured chore. With two chores there is no row 3,
       the panel draws no label under D (design 2.4), and the press must
       do nothing. Reads the count out of the names blob, the only place
       it exists, and inherits button_a_toggle_allowed()'s cost with it:
       chore_store_load_names() reports n = 0 on every failure, so an
       unreadable blob refuses the ack exactly like an empty list. It
       self-heals for the same reason — a refusal stores nothing.

   The mode is checked FIRST because it is free (one RTC byte) and the
   count is not: on every device that is not looking at the checklist —
   the whole fleet, most of the time — this answers without touching
   flash at all. */
bool button_chore_ack_allowed(uint8_t idx);

/* Toggle chore `idx`'s ack and, when that completes the list, release the
   day's withheld seconds. BTN_ACK_NONE when button_chore_ack_allowed()
   says no, and in that case NOTHING is written — not the mask, not flash.

   `now` is the wall clock the press happened at. It is what the day stamp
   on the flash record and the day type behind the allocation are both
   derived from, so the record is matched against the same day every other
   date comparison in the firmware computes from the same instant.

   WHAT MOVES, and the order is a contract rather than an implementation
   detail (chores.h, CALLER CONTRACT):

     1. the RTC mask (the working copy), through timer_chore_set_acked();
     2. on the release edge only, the amount owed is read from
        chores_withheld_sec() WHILE `released` IS STILL FALSE — that
        function returns 0 once the flag is latched, so latching first
        grants nothing and the gate silently never opens;
     3. the grant, through timer_release_gated() and never timer_adjust():
        a gate release is not a screen adjustment (design 5.2);
     4. the latch, timer_chore_set_released(true), immediately after the
        grant and with no wake boundary between them;
     5. one flash write carrying both fields, because chore_store's record
        is the AUTHORITY and the RTC copy only the working one (design
        5.1).

   IF THE FLASH WRITE FAILS the RTC copy still stands and the press is
   still reported as applied: the ack is honoured for this day and
   survives deep sleep, and only an esp_restart would lose it. Rolling the
   mask back instead would be worse — the seconds are already granted and
   `released` is latched by design (row C8), so a rollback would show a
   cleared checkbox against a released day. */
btn_ack_action_t button_chore_ack_apply(uint8_t idx, time_t now);
