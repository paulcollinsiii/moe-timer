#include "buttons.h"

#include "driver/gpio.h"
#include "esp_log.h"
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
    esp_err_t ret = esp_sleep_enable_gpio_wakeup();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "enable_gpio_wakeup failed: %s", esp_err_to_name(ret));
        return;
    }
    for (int i = 0; i < 4; i++) {
        gpio_wakeup_enable(BTN_GPIOS[i], GPIO_INTR_LOW_LEVEL);
    }
}

button_id_t buttons_get_wakeup_button(void) {
    if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_GPIO) {
        return BTN_NONE;
    }

    esp_rom_delay_us(DEBOUNCE_US);

    for (int i = 0; i < 4; i++) {
        if (gpio_get_level(BTN_GPIOS[i]) == 0) {
            ESP_LOGI(TAG, "Wakeup button: %d (GPIO %d)", i, BTN_GPIOS[i]);
            return (button_id_t)i;
        }
    }

    ESP_LOGW(TAG, "GPIO wakeup but no button active after debounce");
    return BTN_NONE;
}

bool buttons_is_pressed(button_id_t btn) {
    if (btn == BTN_NONE || (int)btn >= 4)
        return false;
    return gpio_get_level(BTN_GPIOS[(int)btn]) == 0;
}
