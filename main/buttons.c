#include "buttons.h"

#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_sleep.h"

static const char *TAG = "buttons";

static const gpio_num_t BTN_GPIOS[4] = {
    GPIO_NUM_15, /* BTN_A */
    GPIO_NUM_12, /* BTN_B */
    GPIO_NUM_14, /* BTN_C */
    GPIO_NUM_11, /* BTN_D */
};

#define DEBOUNCE_US 10000 /* 10 ms */

void buttons_init(void) {
    for (int i = 0; i < 4; i++) {
        /* ext1_wakeup_prepare() enables pad HOLD on all EXT1 pins when the
           RTC peripheral domain powers down, and hold persists through the
           deep-sleep reset — without releasing it, gpio_config() below is
           latched out and digital button reads break after the first wake. */
        rtc_gpio_hold_dis(BTN_GPIOS[i]);
        /* Pins may still be latched to the RTC domain from the previous
           deep sleep; release them so digital reads work. */
        rtc_gpio_deinit(BTN_GPIOS[i]);
        gpio_config_t cfg = {
            .pin_bit_mask = (1ULL << BTN_GPIOS[i]),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        esp_err_t ret = gpio_config(&cfg);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "gpio_config failed for GPIO %d: %s", BTN_GPIOS[i], esp_err_to_name(ret));
        }
    }
}

void buttons_configure_wakeup(void) {
    uint64_t mask = 0;
    for (int i = 0; i < 4; i++) {
        gpio_num_t pin = BTN_GPIOS[i];
        rtc_gpio_init(pin);
        rtc_gpio_set_direction(pin, RTC_GPIO_MODE_INPUT_ONLY);
        rtc_gpio_pullup_en(pin); /* buttons are active-low; hold high in sleep */
        rtc_gpio_pulldown_dis(pin);
        mask |= 1ULL << pin;
    }
    /* GPIO wakeup (esp_sleep_enable_gpio_wakeup) is light-sleep-only on
       ESP32-S2 — deep sleep requires EXT1 on RTC-capable pins
       (11/12/14/15 all are). */
    esp_err_t ret = esp_sleep_enable_ext1_wakeup_io(mask, ESP_EXT1_WAKEUP_ANY_LOW);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ext1 wakeup config failed: %s", esp_err_to_name(ret));
    }
}

button_id_t buttons_get_wakeup_button(void) {
    if (!(esp_sleep_get_wakeup_causes() & BIT(ESP_SLEEP_WAKEUP_EXT1))) {
        return BTN_NONE;
    }

    /* EXT1 status latches which pin(s) triggered the wake */
    uint64_t status = esp_sleep_get_ext1_wakeup_status();
    for (int i = 0; i < 4; i++) {
        if (status & (1ULL << BTN_GPIOS[i])) {
            ESP_LOGI(TAG, "Wakeup button: %d (GPIO %d)", i, BTN_GPIOS[i]);
            return (button_id_t)i;
        }
    }

    /* Fallback: latch was empty — debounce then scan levels */
    esp_rom_delay_us(DEBOUNCE_US);
    for (int i = 0; i < 4; i++) {
        if (gpio_get_level(BTN_GPIOS[i]) == 0) {
            return (button_id_t)i;
        }
    }
    ESP_LOGW(TAG, "EXT1 wakeup but no button identified");
    return BTN_NONE;
}

bool buttons_is_pressed(button_id_t btn) {
    if (btn == BTN_NONE || (int)btn >= 4)
        return false;
    return gpio_get_level(BTN_GPIOS[(int)btn]) == 0;
}
