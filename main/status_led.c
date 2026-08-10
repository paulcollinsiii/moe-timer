#include "status_led.h"

#include "neopixel.h"
#include "timer.h"

status_led_rgb_t status_led_for_state(timer_state_t state) {
    switch (state) {
        case TIMER_RUNNING:
            return (status_led_rgb_t){0, 20, 0};
        case TIMER_PAUSED:
            return (status_led_rgb_t){25, 15, 0};
        case TIMER_EXPIRED:
            return (status_led_rgb_t){25, 0, 0};
        case TIMER_BREAK:
            return (status_led_rgb_t){0, 10, 25}; /* blue-cyan */
        default:
            return (status_led_rgb_t){10, 10, 10};
    }
}

void status_led_show_timer_state(void) {
    status_led_rgb_t rgb = status_led_for_state(timer_get_state());
    neopixel_status_pixel(NP_STATE_PIXEL, rgb.r, rgb.g, rgb.b);
}
