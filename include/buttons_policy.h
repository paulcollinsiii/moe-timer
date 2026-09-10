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

/* Everything the decision reads. The two `*_allowed` flags are the timer
   module's own gates taken raw at sleep entry — the mask is rebuilt on
   every sleep, so it tracks the state machine rather than caching it. */
typedef struct {
    bool enable;         /* the sleep outcome's arm decision: false on a locked sleep */
    bool swap_allowed;   /* timer_swap_allowed() — gates BTN_C */
    bool reload_allowed; /* timer_reload_allowed() — gates BTN_B */
} buttons_policy_in_t;

/* Which buttons may wake the device from the sleep being entered. Bit n =
   button n, matching buttons_scan_held(). Zero means arm NOTHING: a
   locked sleep must leave the RTC domain untouched rather than write an
   empty wake mask. Because A and D are unconditional wake sources, a zero
   result can ONLY mean `enable` was false — the driver's early return
   therefore tests "arm nothing", never "no button happened to qualify".
   Pure — host-tested. */
uint8_t buttons_policy_wake_mask(const buttons_policy_in_t *in);

#ifdef __cplusplus
}
#endif
