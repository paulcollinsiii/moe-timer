#pragma once
#include <stdint.h>

#include "timer.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Status pixels: one for timer state here, a different one for WiFi
   (pixel 3, owned by net_window.c) so both can be read at once. */
#define NP_STATE_PIXEL 0

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} status_led_rgb_t;

/* Traffic-light state feedback while the slow e-ink refresh runs:
   RUNNING = green, PAUSED = amber, EXPIRED = red, BREAK = cyan,
   IDLE = white. Split out pure so the triples are host-testable — they
   are read off the device by eye, so they are part of the contract. */
status_led_rgb_t status_led_for_state(timer_state_t state);

/* Drives NP_STATE_PIXEL from the live timer state. Status class — quiet
   hours are handled inside neopixel.c. */
void status_led_show_timer_state(void);

#ifdef __cplusplus
}
#endif
