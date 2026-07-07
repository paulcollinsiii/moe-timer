#pragma once
#include <stdint.h>

/* ISR-fed press latch: GPIO negative-edge interrupts record presses that
   land while the firmware is awake (e-ink flush, NTP sync, between poll
   samples); the awake checkpoints consume them instead of sampling pin
   levels. Pure logic — no ESP dependencies; the firmware wrapper in
   buttons.c provides the ISR and the critical sections (record runs in
   ISR context, take in task context). Plain RAM: unconsumed presses
   evaporate at deep sleep and can never fire on a later wake. */

/* Ignore edges this close to the last ACCEPTED edge of the same button:
   contact chatter on press plus falling-edge bounce on release. */
#define BUTTON_LATCH_DEBOUNCE_US 50000

#ifdef __cplusplus
extern "C" {
#endif

void button_latch_reset(void);
/* Record a falling edge on button `btn` (0..3) at time t_us; out-of-range
   buttons and edges inside the debounce window are ignored. */
void button_latch_record(int btn, int64_t t_us);
/* Return the latched press bitmask (bit n = button n) and clear it. */
uint8_t button_latch_take(void);

#ifdef __cplusplus
}
#endif
