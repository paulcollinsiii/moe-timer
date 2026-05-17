#pragma once
#include <stdbool.h>

typedef enum {
    BTN_A = 0, /* GPIO 15 — Start/Pause */
    BTN_B,     /* GPIO 12 — Reset */
    BTN_C,     /* GPIO 14 — Cycle contrast */
    BTN_D,     /* GPIO 11 — Force NTP sync */
    BTN_NONE,
} button_id_t;

void buttons_init(void);
void buttons_configure_wakeup(void);
button_id_t buttons_get_wakeup_button(void);
bool buttons_is_pressed(button_id_t btn);
