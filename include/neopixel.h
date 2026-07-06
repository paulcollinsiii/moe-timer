#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Must be called on every boot/wake before any other peripheral code.
   Ensures GPIO 21 (power gate) is HIGH (off).
   TODO(stream-neopixel): call gpio_hold_en(21) + gpio_deep_sleep_hold_en()
   after setting HIGH so the pin doesn't float during deep sleep. */
void neopixel_init(void);
void neopixel_alert_start(void); /* GPIO 21 LOW, slow red pulse (expiry) */
/* Slow pulse in an arbitrary colour; blocks until the stop flag like
   neopixel_alert_start. Run from a dedicated task; tear down via
   neopixel_request_stop + done-flag handshake (see below). */
void neopixel_pulse_start(uint8_t r, uint8_t g, uint8_t b);
void neopixel_stop(void); /* stops RMT, GPIO 21 HIGH; safe if already off */
/* Flag-only stop request — safe from any task. neopixel_stop() flushes the
   RMT channel and MUST NOT run concurrently with neopixel_alert_start's
   loop (two tasks on one RMT channel can deadlock rmt_tx_wait_all_done);
   the alert task performs its own final flush after seeing the flag. */
void neopixel_request_stop(void);

/* Set one pixel (0-3), leaving the others unchanged (turns the power gate
   ON). Call neopixel_stop() to turn everything off — never enter deep
   sleep with the gate LOW. */
void neopixel_set_pixel(int idx, uint8_t r, uint8_t g, uint8_t b);

#ifdef __cplusplus
}
#endif
