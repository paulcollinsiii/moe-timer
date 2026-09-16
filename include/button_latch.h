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
/* Take only the buttons in `mask`, leaving other latched presses for a
   later consumer — an awake poll interested in one button must not eat
   presses a later checkpoint (e.g. the tick-wake drain) will act on. */
uint8_t button_latch_take_masked(uint8_t mask);
/* Pick the single button to act on from a taken mask, restricted to
   allowed_mask; priority B > C > D > A. Returns -1 when none allowed.

   The order is "the time-sensitive action wins", not "the lowest index
   wins", and A being LAST is the load-bearing part — button_latch.c says
   why, and both of wake_flow.c's latch drains admit A to their allowed
   masks only because of it. This line said "A > C > B > D" until
   2026-09-16, which was ACCURATE when it was written — the table was
   {0, 2, 1, 3} back when A was start/pause — and went stale at 04b4752
   (M0-T4), which rewrote the table to {1, 2, 3, 0} without it. Nothing
   caught that for six days because the two latch drains in wake_flow.c
   carry their own copy of the order and both state it correctly, so the
   only reader who could have been misled is a future one. Callers reason
   about this order — do not change it without reading both of them. */
int button_latch_pick(uint8_t mask, uint8_t allowed_mask);

#ifdef __cplusplus
}
#endif
