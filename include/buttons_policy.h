#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "buttons.h" /* button_id_t */

/* Pure button wake-source policy, carved out of buttons.c so the rules
   deciding which buttons may wake the device are host-tested; the driver
   keeps the RTC/EXT1 calls and the pad ownership. Same split as
   ssd1680_guard.c: hardware there, decisions here. */

#ifdef __cplusplus
extern "C" {
#endif

/* Everything the decision reads. `swap_allowed` is the timer module's own
   gate taken raw at sleep entry — the mask is rebuilt on every sleep, so it
   tracks the state machine rather than caching it. */
typedef struct {
    bool enable;       /* the sleep outcome's arm decision: false on a locked sleep */
    bool swap_allowed; /* timer_swap_allowed() — gates BTN_C */
    /* button_a_toggle_allowed() — gates BTN_A. Taken raw at sleep entry
       like swap_allowed above, and composite for the same reason: the
       whole predicate (not RUNNING, and a configured chore list) lives in
       button_actions.c so the wake mask and the press itself can never
       disagree about what A would do. */
    bool mode_toggle_allowed;
    /* button_chore_ack_allowed(BUTTON_CHORE_IDX_C) — C's SECOND reason to
       wake, ORed with swap_allowed above. Taken raw at sleep entry like
       the other two, and composite for the same reason: "in chore mode
       and row 2 is configured" lives in button_actions.c so the wake mask
       and the press cannot disagree.
       C ONLY. In chore mode B, C and D are the three ack buttons, and B
       and D are unconditional wake sources already; C is the one whose
       gate could leave the middle checkbox dead from sleep while the two
       either side of it worked. */
    bool chore_ack_allowed;
} buttons_policy_in_t;

/* Which buttons may wake the device from the sleep being entered. Bit n =
   button n, matching buttons_scan_held(). Zero means arm NOTHING: a
   locked sleep must leave the RTC domain untouched rather than write an
   empty wake mask. Because B and D are unconditional wake sources, a zero
   result can ONLY mean `enable` was false — the driver's early return
   therefore tests "arm nothing", never "no button happened to qualify".
   The guarantee rests on B and D ALONE: A and C are both conditional and
   both can be absent from a perfectly ordinary mask, so neither is part
   of it, and gating B or D would break it. Swept in test_buttons_policy
   (test_enabled_is_never_an_empty_mask) across the full cross product of
   the two gates rather than argued.
   Pure — host-tested. */
uint8_t buttons_policy_wake_mask(const buttons_policy_in_t *in);

#ifdef __cplusplus
}
#endif
