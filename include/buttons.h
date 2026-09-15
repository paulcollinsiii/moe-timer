#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    BTN_A = 0, /* GPIO 15 — Timers/Chores mode toggle; a CONDITIONAL wake source */
    BTN_B,     /* GPIO 14 — Start/Pause/Resume, or Reload once expired */
    BTN_C,     /* GPIO 12 — Swap timer type (v1.3) */
    BTN_D,     /* GPIO 11 — Force NTP sync */
    BTN_NONE,
} button_id_t;

#ifdef __cplusplus
extern "C" {
#endif

void buttons_init(void);
/* Arm the EXT1 button wake sources for the sleep being entered. `enable`
   is the sleep outcome's arm decision: false on a locked sleep (charge,
   bed time), which arms nothing and leaves the RTC domain untouched.
   Which buttons qualify is decided by buttons_policy.c. */
void buttons_configure_wakeup_if(bool enable);
button_id_t buttons_get_wakeup_button(void);
bool buttons_is_pressed(button_id_t btn);
/* Level scan of all four buttons at once (bit n = button n held now). */
uint8_t buttons_scan_held(void);
/* Consume presses latched by the awake-window GPIO ISR (bit n = button n).
   Latched between buttons_init() and buttons_configure_wakeup_if(); level
   reads above stay the tool for "is it held right now". */
uint8_t buttons_take_pressed(void);
/* Consume only the latched presses in `mask`, leaving the rest latched
   for a later checkpoint (e.g. the tick-wake drain). */
uint8_t buttons_take_pressed_mask(uint8_t mask);

#ifdef __cplusplus
}
#endif
