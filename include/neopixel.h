#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Must be called on every boot/wake before any other peripheral code.
   Ensures GPIO 21 (power gate) is HIGH (off). */
void neopixel_init(void);
/* Everything off, RMT flushed, GPIO 21 HIGH; idempotent. Never enter deep
   sleep with the gate LOW. */
void neopixel_stop(void);

/* Library configuration, injected from main so the module stays clock- and
   Kconfig-agnostic. */
void neopixel_set_quiet_cb(bool (*is_quiet)(void));
void neopixel_set_status_brightness(uint8_t pct); /* 0-100 scale, status class only */

/* STATUS class — silently no-ops while the quiet callback returns true;
   colours are scaled by the status brightness. */
void neopixel_status_pixel(int idx, uint8_t r, uint8_t g, uint8_t b);
/* 4-bit binary display: pixel 0 (over button A) = bit3 ... pixel 3 = bit0. */
void neopixel_status_binary4(uint8_t value, uint8_t r, uint8_t g, uint8_t b);

/* HIGHPRI class — ignores quiet hours (accompanies audible alarms). */
void neopixel_highpri_pixel(int idx, uint8_t r, uint8_t g, uint8_t b);
/* Slow pulse on all pixels via an internally-owned task. Exactly one task
   may drive the RMT channel (two deadlock rmt_tx_wait_all_done), so the
   task lives here: begin spawns it, end sets the stop flag, joins with a
   2 s cap, and forces the LEDs off. */
void neopixel_alert_pulse_begin(uint8_t r, uint8_t g, uint8_t b);
void neopixel_alert_pulse_end(void);

#ifdef __cplusplus
}
#endif
