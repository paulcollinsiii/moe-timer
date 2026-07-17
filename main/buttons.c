#include "buttons.h"

#include "button_latch.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"
#include "timer.h"

static const char *TAG = "buttons";

/* Adafruit MagTag pinout: A=D15, B=D14, C=D12, D=D11. Verified in hardware
   bring-up 2026-07: with B/C swapped, physical Button C fired the BTN_B
   (reset) action. */
static const gpio_num_t BTN_GPIOS[4] = {
    GPIO_NUM_15, /* BTN_A */
    GPIO_NUM_14, /* BTN_B */
    GPIO_NUM_12, /* BTN_C */
    GPIO_NUM_11, /* BTN_D */
};

#define DEBOUNCE_US 10000 /* 10 ms */

/* ---- awake press latch (ISR-fed; see button_latch.h) ---------------- */

static portMUX_TYPE s_latch_mux = portMUX_INITIALIZER_UNLOCKED;

static void IRAM_ATTR button_isr(void *arg) {
    portENTER_CRITICAL_ISR(&s_latch_mux);
    button_latch_record((int)(intptr_t)arg, esp_timer_get_time());
    portEXIT_CRITICAL_ISR(&s_latch_mux);
}

/* Latch presses while awake. No false latch for the wake button: it is
   already low at boot, so no falling edge fires. */
static void buttons_watch_begin(void) {
    esp_err_t ret = gpio_install_isr_service(0);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) { /* INVALID_STATE = already installed */
        ESP_LOGE(TAG, "isr service install failed: %s", esp_err_to_name(ret));
        return;
    }
    portENTER_CRITICAL(&s_latch_mux);
    button_latch_reset();
    portEXIT_CRITICAL(&s_latch_mux);
    for (int i = 0; i < 4; i++) {
        gpio_set_intr_type(BTN_GPIOS[i], GPIO_INTR_NEGEDGE);
        gpio_isr_handler_add(BTN_GPIOS[i], button_isr, (void *)(intptr_t)i);
    }
}

/* Detach before buttons_configure_wakeup() moves the pads to the RTC mux. */
static void buttons_watch_end(void) {
    for (int i = 0; i < 4; i++) {
        gpio_isr_handler_remove(BTN_GPIOS[i]);
    }
}

uint8_t buttons_take_pressed(void) {
    portENTER_CRITICAL(&s_latch_mux);
    uint8_t mask = button_latch_take();
    portEXIT_CRITICAL(&s_latch_mux);
    return mask;
}

uint8_t buttons_take_pressed_mask(uint8_t mask) {
    portENTER_CRITICAL(&s_latch_mux);
    uint8_t taken = button_latch_take_masked(mask);
    portEXIT_CRITICAL(&s_latch_mux);
    return taken;
}

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
    buttons_watch_begin();
}

/* Wake policy (UX: prevent button mashing from burning battery/refreshes):
   C wakes only when a swap would actually succeed — extra timers exist and
   the active timer is not RUNNING/BREAK (the mask is rebuilt at every
   sleep entry, so it tracks the state machine); B likewise wakes only
   when a reset would succeed — a reloadable selected timer or the
   parent-testing reset, and never while RUNNING. Non-wake buttons are
   left out of the EXT1 mask AND unconfigured in the RTC domain (an open
   button on an isolated pad draws nothing; a pull-up would leak ~70 uA
   while held). */
static bool is_wake_source(int i) {
    switch ((button_id_t)i) {
        case BTN_C:
            return timer_swap_allowed();
        case BTN_B:
#if CONFIG_MAGTAG_PARENT_TESTING
            return timer_reload_allowed(true);
#else
            return timer_reload_allowed(false);
#endif
        default:
            return true;
    }
}

void buttons_configure_wakeup(void) {
    buttons_watch_end();
    uint64_t mask = 0;
    for (int i = 0; i < 4; i++) {
        if (!is_wake_source(i))
            continue;
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

uint8_t buttons_scan_held(void) {
    uint8_t mask = 0;
    for (int b = 0; b < 4; b++) {
        if (buttons_is_pressed((button_id_t)b))
            mask |= (uint8_t)(1u << b);
    }
    return mask;
}
