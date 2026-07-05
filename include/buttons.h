#pragma once
#include <stdbool.h>

typedef enum {
    BTN_A = 0, /* GPIO 15 — Start/Pause */
    BTN_B,     /* GPIO 14 — Reset (parent-testing builds only) */
    BTN_C,     /* GPIO 12 — unbound in v1; not a wake source */
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

#ifdef __cplusplus
}
#endif
