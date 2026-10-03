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

/* ---- BOOT (GPIO0) — deliberately NOT a fifth button_id_t ----------------

   A BTN_E would silently gain every A-D action binding at the many sites
   that loop `i < BTN_NONE` or guard `btn >= 4` (button_actions.c,
   button_latch.c, buttons_policy.c, wake_flow.c and their tests). BOOT
   has no action of its own — only the hold gesture in setup_trigger.h —
   so it stays out of that enum and gets its own queries instead. */

/* Plain level sample ("is BOOT down right now"), for the BOOT hold
   tracker (setup_trigger.h) to be fed from while awake. Unlike
   buttons_is_pressed() this is never gated by MAGTAG_BOOT_WAKES: the pad
   is always configured as a digital input in buttons_init(), so the hold
   gesture keeps working purely as an "already awake" gesture even on a
   board where BOOT is disabled as a wake SOURCE (see that Kconfig's help
   text). */
bool buttons_is_boot_pressed(void);

/* Whether GPIO0 was itself a cause of THIS wake. `wakeup_button` is
   buttons_get_wakeup_button()'s own result for the same wake — pass it
   in rather than letting this re-derive it, so BOOT's rank — last,
   behind whichever A-D button already resolved (button_latch_boot_wins())
   — is read once and the fallback debounce+scan
   buttons_get_wakeup_button() may already have paid for is never run a
   second time. Requires the EXT1 cause AND GPIO0's bit in the EXT1
   status; unlike that fallback scan, there is no level-read fallback of
   its own here — one candidate pin has nothing to disambiguate against,
   so an empty or absent status settles as "not BOOT" rather than
   guessing. Always false when MAGTAG_BOOT_WAKES=n, since GPIO0 is then
   never armed and cannot have caused a wake. */
bool buttons_woke_by_boot(button_id_t wakeup_button);

#ifdef __cplusplus
}
#endif
