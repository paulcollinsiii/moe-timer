#include "buttons.h"

#include "esp_sleep.h"

void buttons_init(void) {}
void buttons_configure_wakeup(void) {}
button_id_t buttons_get_wakeup_button(void) {
    return BTN_NONE;
}
bool buttons_is_pressed(button_id_t btn) {
    (void)btn;
    return false;
}
