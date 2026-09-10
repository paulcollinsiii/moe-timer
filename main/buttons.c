#include "buttons.h"

#include "button_latch.h"
#include "buttons_policy.h"
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

/* Detach before buttons_configure_wakeup_if() moves the pads to the RTC mux. */
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

/* Which buttons earn a wake is policy and lives in buttons_policy.c; what
   stays here is the RTC/EXT1 plumbing. That plumbing is not host-tested —
   nothing compiles this TU without ESP-IDF — so four seams below rest on
   review alone: that the pad loop indexes BTN_GPIOS with the same bit the
   policy set, that timer_swap_allowed() is wired into the right policy
   field, that the early return stays AHEAD of buttons_watch_end(), and
   that buttons_get_wakeup_button()'s fallback level scan only ever blames
   a pad the policy could have armed. Closing them needs a host suite for
   this file (stubbed gpio/rtc_io/esp_sleep/FreeRTOS), which is a bigger
   move than the policy carve. */
void buttons_configure_wakeup_if(bool enable) {
    /* A locked sleep arms nothing: leave the RTC domain exactly as the
       last sleep left it, on a battery that cannot spare the work. */
    if (!enable)
        return;
    buttons_policy_in_t pol = {
        .enable = enable,
        .swap_allowed = timer_swap_allowed(),
    };
    uint8_t wake = buttons_policy_wake_mask(&pol);
    buttons_watch_end();
    uint64_t mask = 0;
    for (int i = 0; i < BTN_NONE; i++) {
        if (!(wake & (1u << i)))
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

    /* Fallback: latch was empty — debounce then scan levels, but only
       across pads that COULD have been armed. The maximal mask (every
       gate open) is the set of buttons the policy will arm under some
       condition; a button outside it — A, which never wakes — cannot have
       caused this EXT1 wake whatever the user happens to be holding.
       Without the filter, a wake genuinely caused by B, C or D while A is
       also held returns BTN_A, because A is index 0 and wins the scan;
       the real press is then discarded and A's dispatch runs instead.
       Derived from the policy rather than hardcoding A here so that
       buttons_policy.c stays the single source of truth, and so a button
       that later becomes conditionally armed keeps being scanned — a
       conditional button is still in the maximal mask. Nothing is
       retained across the sleep: the armed mask was computed before it
       and RAM is gone by now, so recomputing the bound is the only option
       anyway. NB: designated initializer — a gate field added to
       buttons_policy_in_t defaults to false here and would narrow this
       below maximal; any new gate must be set true.
       The primary path above needs no such filter: an unarmed pad can
       never appear in the EXT1 status latch. */
    const buttons_policy_in_t maximal = {
        .enable = true,
        .swap_allowed = true,
    };
    const uint8_t armable = buttons_policy_wake_mask(&maximal);
    esp_rom_delay_us(DEBOUNCE_US);
    for (int i = 0; i < 4; i++) {
        if (!(armable & (1u << i)))
            continue;
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
