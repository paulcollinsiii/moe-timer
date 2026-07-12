#include "audio.h"

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "audio";

#define AMP_ENABLE_GPIO GPIO_NUM_16
#define BEEP_GPIO GPIO_NUM_17 /* TODO: verify against MagTag schematic */
#define BEEP_FREQ_HZ 1000
#define LEDC_CHANNEL LEDC_CHANNEL_0
#define LEDC_TIMER LEDC_TIMER_0
#define LEDC_MODE LEDC_LOW_SPEED_MODE
#define LEDC_RESOLUTION LEDC_TIMER_10_BIT

static volatile bool s_stop_requested = false;
static bool s_initialized;

/* Lazy: first beep_on() this wake initializes. Most wakes never make a
   sound — configuring the amp pin + LEDC timer/channel for them was pure
   awake-time overhead. Until then the deep-sleep hold keeps the amp pin
   low (amp off), which is exactly the state init would set. */
void audio_init(void) {
    if (s_initialized)
        return;
    s_initialized = true;
    /* Release the deep-sleep hold placed by enter_deep_sleep() */
    gpio_hold_dis(AMP_ENABLE_GPIO);
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
    audio_init(); /* lazy — no-op once initialized */
    gpio_set_level(AMP_ENABLE_GPIO, 1);
    ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, 512);
    ledc_update_duty(LEDC_MODE, LEDC_CHANNEL);
}

static void beep_off(void) {
    if (!s_initialized)
        return; /* nothing to silence; amp pin still held low */
    ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, 0);
    ledc_update_duty(LEDC_MODE, LEDC_CHANNEL);
    gpio_set_level(AMP_ENABLE_GPIO, 0);
}

void audio_beep_sequence(void) {
    s_stop_requested = false;

    for (int cycle = 0; cycle < CONFIG_MAGTAG_EXPIRY_ALARM_CYCLES && !s_stop_requested; cycle++) {
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

void audio_break_alarm(void) {
    s_stop_requested = false;

    /* Distinct from the expiry alarm: 2 beeps per cycle (~2.2 s each) */
    for (int cycle = 0; cycle < CONFIG_MAGTAG_BREAK_ALARM_CYCLES && !s_stop_requested; cycle++) {
        for (int beep = 0; beep < 2 && !s_stop_requested; beep++) {
            beep_on();
            vTaskDelay(pdMS_TO_TICKS(300));
            beep_off();
            if (!s_stop_requested) {
                vTaskDelay(pdMS_TO_TICKS(150));
            }
        }
        if (!s_stop_requested) {
            vTaskDelay(pdMS_TO_TICKS(1100));
        }
    }

    beep_off();
}

void audio_break_over_chime(void) {
    /* Single short double-beep (~0.6 s); fire-and-forget, no dismissal */
    beep_on();
    vTaskDelay(pdMS_TO_TICKS(150));
    beep_off();
    vTaskDelay(pdMS_TO_TICKS(100));
    beep_on();
    vTaskDelay(pdMS_TO_TICKS(300));
    beep_off();
}

void audio_stop(void) {
    s_stop_requested = true;
    beep_off();
}
