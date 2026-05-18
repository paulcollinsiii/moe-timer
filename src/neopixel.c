#include "neopixel.h"

#include "driver/gpio.h"

#define NEOPIXEL_POWER_GPIO 21

void neopixel_init(void) {
    gpio_set_direction(NEOPIXEL_POWER_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(NEOPIXEL_POWER_GPIO, 1); /* HIGH = power gate OFF */
}

void neopixel_alert_start(void) {}
void neopixel_stop(void) {
    gpio_set_level(NEOPIXEL_POWER_GPIO, 1); /* safe even if already off */
}
