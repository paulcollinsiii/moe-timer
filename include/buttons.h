#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    BTN_A = 0, /* GPIO 15 — Start/Pause */
    BTN_B,     /* GPIO 14 — Reset selected timer (reloadable or parent-testing) */
    BTN_C,     /* GPIO 12 — Swap timer type (v1.3) */
    BTN_D,     /* GPIO 11 — Force NTP sync */
    BTN_NONE,
} button_id_t;

#ifdef __cplusplus
extern "C" {
#endif

void buttons_init(void);
void buttons_configure_wakeup(void);
button_id_t buttons_get_wakeup_button(void);
bool buttons_is_pressed(button_id_t btn);
/* Consume presses latched by the awake-window GPIO ISR (bit n = button n).
   Latched between buttons_init() and buttons_configure_wakeup(); level
   reads above stay the tool for "is it held right now". */
uint8_t buttons_take_pressed(void);

#ifdef __cplusplus
}
#endif
