/* Pure button wake-source policy — no ESP dependencies; host-tested.

   Layout: A = Timers/Chores mode toggle, B = Start/Pause/Resume/Reload,
   C = Next timer, D = Refresh.

   UX rule (button mashing must not burn battery or refreshes): a button
   whose action could only be refused is not worth a wake.

   - A wakes only when the mode toggle would actually be honoured — the
     active slot is not RUNNING and a chore list is configured. Both
     conditions arrive already folded into one gate by
     button_a_toggle_allowed(), which is also what the dispatch's A arm
     acts on, so the mask and the press can never disagree. On the fleet
     as it ships today — no chore list configured anywhere — that gate is
     false and A is armed on nothing, which is the point: the feature
     costs no wakes until it is used.
   - C wakes when a swap would actually succeed — extra timers exist and
     the active timer is not RUNNING (a Screen Break does NOT refuse, so C
     stays a wake source right through one) — OR when it would tick a
     chore. In chore mode B, C and D are the three ack buttons (design
     2.4), and the second reason is what keeps the MIDDLE CHECKBOX alive:
     B and D are unconditional below, so without it a device with no extra
     timers configured — swap_allowed false forever — would have a working
     ✓1 and ✓3 and a dead ✓2. That is the "primary control dead to the
     press" failure this module's policy exists to avoid, and it is worth
     more than the wake an over-armed C can cost.
     The two reasons are an OR and neither implies the other: outside
     chore mode only the swap arms C, and on the chore screen only the ack
     does.
   - B and D are unconditional.

   A and C are gated SEPARATELY even though both refuse while RUNNING.
   Folding them is the obvious saving and it is wrong: swap_allowed also
   requires extra timers to exist, and design 4.2 is explicit that the
   mode gate does not inherit that condition. A shared gate would take the
   chore screen away from every device with no extra timers configured.

   B is the one deliberate overshoot of the rule above, and it is worth
   naming because it looks like an oversight. button_b_apply() returns
   BTN_B_NONE in four classes of state, not one or two:

     1. EXPIRED with Screen (slot 0) selected — Screen has no def, so
        timer_reload_allowed() can never permit a reload there.
     2. EXPIRED on an extra whose `reloadable` switch is off. This one is
        not narrow: it can stand for the rest of the day, because a
        depleted non-reloadable timer just shows its empty bar until the
        rollover (display_layout.c) and `reloadable` is a per-extra HA
        switch (ha_config.c) a parent may simply leave off.
     3. A Screen Break in progress with Screen (slot 0) selected —
        refusing to start screen time IS how the break is enforced.
     4. A Screen Break in progress with a non-break-eligible extra
        selected and IDLE or PAUSED (a disabled slot counts as
        non-eligible, timer.c). `break_eligible` is a per-extra HA switch
        too. TIMER_BREAK lives on slot 0 only, so IDLE/PAUSED is the whole
        set of states this class can be in.

   Classes 3 and 4 are the old Button A's break gate, inherited byte for
   byte. Class 1 and 2's EXPIRED leg is new, and it refuses strictly LESS
   than old A did: old button_a_apply() fell through to NONE on every
   TIMER_EXPIRED, whereas B now reloads a reloadable expired slot.

   So B is armed knowing it can do nothing, possibly for a whole day. The
   justification is an asymmetry of failure, not rarity. Arming B when it
   would do nothing costs one wake and one refresh. Failing to arm B when
   it WOULD have done something makes the device's primary control dead to
   the press — the user holds the button and nothing happens at all, with
   no feedback and no way to tell a flat battery from a policy decision. C
   can be gated safely because a refused swap has a visible alternative; B
   is how you start, pause and resume, so the safe direction is to arm it
   and let the action map refuse in the open.

   Computing an exact "B would do something" predicate is also not a small
   gate: it means evaluating the whole action map's decision — state plus
   timer_start_allowed() plus timer_reload_allowed() — without its side
   effects. That module does not exist and is not in this milestone.

   Non-wake buttons are left out of the EXT1 mask AND unconfigured in the
   RTC domain by the driver: an open button on an isolated pad draws
   nothing, whereas a pull-up would leak ~70 uA while held. */
#include "buttons_policy.h"

static bool wake_source(button_id_t btn, const buttons_policy_in_t *in) {
    switch (btn) {
        case BTN_A:
            return in->mode_toggle_allowed;
        case BTN_C:
            return in->swap_allowed || in->chore_ack_allowed;
        default:
            return true;
    }
}

uint8_t buttons_policy_wake_mask(const buttons_policy_in_t *in) {
    /* A locked sleep (charge / bed time) arms nothing at all: a press
       could only burn a refresh the battery cannot afford. */
    if (!in->enable)
        return 0;
    uint8_t mask = 0;
    for (int i = 0; i < BTN_NONE; i++) {
        if (wake_source((button_id_t)i, in))
            mask |= (uint8_t)(1u << i);
    }
    return mask;
}
