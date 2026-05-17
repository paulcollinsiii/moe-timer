#pragma once

/* Must be called on every boot/wake before any other peripheral code.
   Ensures GPIO 21 (power gate) is HIGH (off). */
void neopixel_init(void);
void neopixel_alert_start(void); /* GPIO 21 LOW, slow red pulse */
void neopixel_stop(void);        /* stops RMT, GPIO 21 HIGH; safe if already off */
