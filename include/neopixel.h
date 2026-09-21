#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Status pixels on the board. Public because it is the bound on every
   `idx` below (out-of-range indices are dropped, silently) and because
   callers that paint the WHOLE strip rather than one pixel — the chore
   checklist, status_led.h — have to size themselves by it. */
#define NEOPIXEL_COUNT 4

/* Must be called on every boot/wake before any other peripheral code.
   Ensures GPIO 21 (power gate) is HIGH (off), then starts the LED task:
   the single owner of the RMT channel AND the power gate. Every call
   below posts a message to that task — two tasks flushing the channel
   concurrently deadlock rmt_tx_wait_all_done (found the hard way in
   bring-up), so nothing else may ever transmit. */
void neopixel_init(void);

/* Post: everything off, gate HIGH. Fire-and-forget — for mid-wake clears
   (e.g. wiping the binary countdown before a state repaint). */
void neopixel_stop(void);

/* Blocking stop for sleep entry: posts a STOP and waits (up to timeout_ms)
   for the LED task to confirm the pixels are dark and the gate is HIGH —
   never sleep with it LOW, the deep-sleep hold would keep the LEDs powered
   all night. On timeout (wedged task) the gate GPIO is forced HIGH
   directly WITHOUT touching the RMT channel, so this is safe from any
   context including the awake-failsafe's esp_timer task. */
void neopixel_stop_sync(uint32_t timeout_ms);

/* Library configuration, injected from main so the module stays clock- and
   Kconfig-agnostic. */
void neopixel_set_quiet_cb(bool (*is_quiet)(void));
void neopixel_set_status_brightness(uint8_t pct); /* 0-100 scale; every non-pulse paint */

/* STATUS class — silently no-ops while the quiet callback returns true;
   colours are scaled by the status brightness. */
void neopixel_status_pixel(int idx, uint8_t r, uint8_t g, uint8_t b);
/* 4-bit binary display: pixel 0 (over button A) = bit3 ... pixel 3 = bit0. */
void neopixel_status_binary4(uint8_t value, uint8_t r, uint8_t g, uint8_t b);

/* HIGHPRI class — ignores quiet hours, and ONLY quiet hours: the status
   brightness still applies, because that is the user's dimmer rather than a
   schedule. For a paint whose absence would leave a deliberate press with no
   feedback at all (the chore checklist's strip, design §2.5) or which
   accompanies an audible alarm. */
void neopixel_highpri_pixel(int idx, uint8_t r, uint8_t g, uint8_t b);
/* Slow pulse on all pixels, run inside the LED task. begin/end are posts:
   end clears every pixel and drops the gate (callers re-light what they
   need afterwards). */
void neopixel_alert_pulse_begin(uint8_t r, uint8_t g, uint8_t b);
void neopixel_alert_pulse_end(void);

#ifdef __cplusplus
}
#endif
