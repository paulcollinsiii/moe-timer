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
    /* lock_gate_wake_d_only() — the config-error lock (design 5.3) or,
       since BUG-14, the no-clock lock that shares its sleep — and
       the ONLY field here that NARROWS rather than widens. True arms D
       and drops everything else, whatever the three gates above say.

       It has to live here and not in those gates. B is UNCONDITIONAL
       everywhere else in this module and there is no gate to hang its
       refusal on; without this field a config-locked sleep would arm B,
       and a press would buy a wake and a full refresh on a device that
       can do nothing with either.
       AND THE EARLY RETURN IN wake_source() IS THE WHOLE RULE, not half
       of it. The driver used to short-circuit A and C to false as well,
       so the narrowing was spelt in two places and only this one was
       testable (buttons.c is in no host suite). It now reports the flag
       and nothing else: A and C arrive with their honest answers and are
       dropped here, which is where a "which buttons may wake this device"
       decision belongs and where a suite can see it happen.
       D survives because it is the exit: the lock ends only when someone
       edits config, and D forces the network window that carries the fix
       rather than waiting out CONFIG_ERR_SLEEP_SEC. */
    bool config_locked;
} buttons_policy_in_t;

/* Which buttons may wake the device from the sleep being entered. Bit n =
   button n, matching buttons_scan_held(). Zero means arm NOTHING: a
   locked sleep must leave the RTC domain untouched rather than write an
   empty wake mask. Because B and D are unconditional wake sources, a zero
   result can ONLY mean `enable` was false — the driver's early return
   therefore tests "arm nothing", never "no button happened to qualify".
   The guarantee rests on D ALONE now that the config-error lock can drop
   B: A, B and C can each be absent from some mask this function returns,
   so none of them is part of it, and gating D would break it. Swept in
   test_buttons_policy (test_enabled_is_never_an_empty_mask) across the
   full cross product of all four gates rather than argued.
   Pure — host-tested. */
uint8_t buttons_policy_wake_mask(const buttons_policy_in_t *in);

#ifdef __cplusplus
}
#endif
