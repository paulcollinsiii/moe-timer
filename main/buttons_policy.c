/* Pure button wake-source policy — no ESP dependencies; host-tested.

   UX rule (button mashing must not burn battery or refreshes): a button
   whose action could only be refused is not worth a wake. C wakes only
   when a swap would actually succeed — extra timers exist and the active
   timer is not RUNNING (a Screen Break does NOT refuse, so C stays a wake
   source right through one). B likewise wakes only when a reset would
   succeed — a reloadable selected timer or the parent-testing reset, and
   never while RUNNING. A and D are unconditional.

   Non-wake buttons are left out of the EXT1 mask AND unconfigured in the
   RTC domain by the driver: an open button on an isolated pad draws
   nothing, whereas a pull-up would leak ~70 uA while held. */
#include "buttons_policy.h"

static bool wake_source(button_id_t btn, const buttons_policy_in_t *in) {
    switch (btn) {
        case BTN_C:
            return in->swap_allowed;
        case BTN_B:
            return in->reload_allowed;
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
