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
    /* lock_gate_config_locked() — the config-error lock (design 5.3) — and,
       with clock_locked below, one of the two fields here that NARROW
       rather than widen. True arms D and drops everything else, whatever
       the three gates above say, and it also keeps BOOT dark (see
       buttons_policy_boot_wake_allowed).

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
    /* lock_gate_clock_locked() — the no-clock lock (BUG-14). Narrows the
       A-D mask to D exactly as config_locked does, because its press is the
       same retry. It is a separate field because the two locks differ on
       BOOT: a config-error device is normally fixed from HA, so BOOT alone
       does not wake it, but a no-clock device may have lost its WiFi (a new
       router, then a power cut) and BOOT is then the only way back into
       setup, since D retries credentials that no longer work. A
       config-locked device whose WiFi changed is in the same position, and
       it has no BOOT wake: D pressed with BOOT already held still enters
       setup (wake_flow.c), so the gesture is its way back. */
    bool clock_locked;
    /* gpio_get_level(GPIO_NUM_0) == 0 at sleep entry, sampled raw by the
       driver like the other fields here — NOT consumed by
       buttons_policy_wake_mask() above (BOOT is not a button_id_t and
       never sets a bit in that mask); it feeds
       buttons_policy_boot_wake_allowed() below only. EXT1 is
       level-triggered: arming GPIO0 while it already reads low would
       wake the device the instant it reached deep sleep and again on
       every re-wake for as long as the press lasted, with no button
       resolved to stop it (the continuation guard in wake_flow.c tracks
       only A-D). Refusing to arm BOOT while it is already down is what
       closes that loop. */
    bool boot_currently_down;
} buttons_policy_in_t;

/* Whether BOOT (GPIO0) may be armed as an EXT1 wake source for the sleep
   being entered. Kept separate from buttons_policy_wake_mask() above
   rather than a fifth bit in its return value, for the same reason BOOT
   is not a button_id_t (buttons.h): that mask's bits are read by index
   against BTN_GPIOS, and a fifth one would need a GPIO table entry and a
   BTN_NONE-sized widening everywhere that loops `i < BTN_NONE` over it.

   Armed when `enable` is true and `config_locked` is false. The
   config-error lock keeps the sleep to Button D alone: its fix is made in
   HA, and BOOT's recovery (setup) is not what it is waiting for. That does
   not close setup to it, because D with BOOT held already is the gesture
   (wake_flow.c), and a device that cannot reach HA needs it. The
   no-clock lock arms BOOT beside D, because a device whose stored WiFi
   stopped working and then lost power would otherwise have no way into
   setup without that gesture. The charge and Bed Time locks arm nothing, same as every
   other button; `enable` false covers both without a separate check.

   And never armed while `boot_currently_down` is true: see that field's
   comment for why (the level-triggered re-wake loop). Pure — host-tested. */
bool buttons_policy_boot_wake_allowed(const buttons_policy_in_t *in);

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
