#include "audio.h"

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "audio";

#define AMP_ENABLE_GPIO GPIO_NUM_16
#define BEEP_GPIO GPIO_NUM_17 /* TODO: verify against MagTag schematic */
#define BEEP_FREQ_HZ 1000
#define LEDC_CHANNEL LEDC_CHANNEL_0
#define LEDC_TIMER LEDC_TIMER_0
#define LEDC_MODE LEDC_LOW_SPEED_MODE
#define LEDC_RESOLUTION LEDC_TIMER_10_BIT

static volatile bool s_stop_requested = false;

void audio_init(void) {
    gpio_config_t amp_cfg = {
        .pin_bit_mask = (1ULL << AMP_ENABLE_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t ret = gpio_config(&amp_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config(amp) failed: %s", esp_err_to_name(ret));
    }
    gpio_set_level(AMP_ENABLE_GPIO, 0);

    ledc_timer_config_t timer_cfg = {
        .speed_mode = LEDC_MODE,
        .timer_num = LEDC_TIMER,
        .duty_resolution = LEDC_RESOLUTION,
        .freq_hz = BEEP_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ret = ledc_timer_config(&timer_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ledc_timer_config failed: %s", esp_err_to_name(ret));
        return;
    }

    ledc_channel_config_t ch_cfg = {
        .gpio_num = BEEP_GPIO,
        .speed_mode = LEDC_MODE,
        .channel = LEDC_CHANNEL,
        .timer_sel = LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    ret = ledc_channel_config(&ch_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ledc_channel_config failed: %s", esp_err_to_name(ret));
    }
}

static void beep_on(void) {
    gpio_set_level(AMP_ENABLE_GPIO, 1);
    ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, 512);
    ledc_update_duty(LEDC_MODE, LEDC_CHANNEL);
}

static void beep_off(void) {
    ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, 0);
    ledc_update_duty(LEDC_MODE, LEDC_CHANNEL);
    gpio_set_level(AMP_ENABLE_GPIO, 0);
}

void audio_beep_sequence(void) {
    s_stop_requested = false;

    for (int cycle = 0; cycle < 5 && !s_stop_requested; cycle++) {
        for (int beep = 0; beep < 3 && !s_stop_requested; beep++) {
            beep_on();
            vTaskDelay(pdMS_TO_TICKS(200));
            beep_off();
            if (!s_stop_requested) {
                vTaskDelay(pdMS_TO_TICKS(100));
            }
        }
        if (!s_stop_requested) {
            /* Remaining inter-cycle gap: 3 s cycle total minus 3*(200+100) ms = 2100 ms */
            vTaskDelay(pdMS_TO_TICKS(2100));
        }
    }

    beep_off();
}

void audio_stop(void) {
    s_stop_requested = true;
    beep_off();
}
