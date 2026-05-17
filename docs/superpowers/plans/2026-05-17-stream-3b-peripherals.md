# Stream 3b — Peripherals Implementation Plan (Buttons, Audio, NeoPixel)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement `buttons.c`, `audio.c`, and `neopixel.c`. These are hardware drivers with no host-testable logic. The compile gate (`pio run`) is the validation; no hardware flashing in this stream.

**Architecture:** Three independent drivers sharing one feature branch and worktree. Each driver uses only ESP-IDF built-in APIs: GPIO driver for buttons and NeoPixel power gate, LEDC PWM driver for audio, RMT driver for NeoPixel data. The NeoPixel power gate (GPIO 21) is driven HIGH in `neopixel_init()` on every boot/wake — this is a hard power correctness invariant.

**Tech Stack:** ESP-IDF GPIO, LEDC PWM, RMT, esp_sleep, esp_timer

**Prerequisite:** Stream 1 (`feature/test-harness`) merged to `integration`. (No native tests for this stream — hardware APIs only.)

---

## Files

| Action | Path |
|---|---|
| Modify | `src/buttons.c` |
| Modify | `src/audio.c` |
| Modify | `src/neopixel.c` |

All headers (`include/buttons.h`, `include/audio.h`, `include/neopixel.h`) are created in Stream 0 — do not modify their public API.

---

## Task 1: Create branch

- [ ] **Step 1: Branch from integration**

```bash
git fetch origin
git checkout integration
git checkout -b feature/peripherals
```

---

## Task 2: Implement buttons.c

**Files:** Replace `src/buttons.c`

Button-to-GPIO mapping (from ProductOverview Feature 6):
- BTN_A → GPIO 15 (Start/Pause)
- BTN_B → GPIO 12 (Reset)
- BTN_C → GPIO 14 (Cycle contrast)
- BTN_D → GPIO 11 (Force NTP sync)

All buttons: active-LOW, internal pull-up.

- [ ] **Step 1: Implement buttons.c**

```c
#include "buttons.h"
#include "driver/gpio.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "buttons";

/* GPIO assignments — must match schematic */
static const gpio_num_t BTN_GPIOS[4] = {
    GPIO_NUM_15,  /* BTN_A */
    GPIO_NUM_12,  /* BTN_B */
    GPIO_NUM_14,  /* BTN_C */
    GPIO_NUM_11,  /* BTN_D */
};

#define DEBOUNCE_US 10000  /* 10 ms */

void buttons_init(void)
{
    for (int i = 0; i < 4; i++) {
        gpio_config_t cfg = {
            .pin_bit_mask = (1ULL << BTN_GPIOS[i]),
            .mode         = GPIO_MODE_INPUT,
            .pull_up_en   = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_DISABLE,
        };
        esp_err_t ret = gpio_config(&cfg);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "gpio_config failed for GPIO %d: %s",
                     BTN_GPIOS[i], esp_err_to_name(ret));
        }
    }
}

void buttons_configure_wakeup(void)
{
    for (int i = 0; i < 4; i++) {
        /* Wake on LOW (button press = active-LOW) */
        esp_err_t ret = esp_sleep_enable_gpio_wakeup();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "enable_gpio_wakeup failed: %s", esp_err_to_name(ret));
            return;
        }
        gpio_wakeup_enable(BTN_GPIOS[i], GPIO_INTR_LOW_LEVEL);
    }
}

button_id_t buttons_get_wakeup_button(void)
{
    /* Wakeup cause must be GPIO for this to be meaningful */
    if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_GPIO) {
        return BTN_NONE;
    }

    /* Software debounce: wait 10 ms then sample */
    esp_rom_delay_us(DEBOUNCE_US);

    for (int i = 0; i < 4; i++) {
        if (gpio_get_level(BTN_GPIOS[i]) == 0) {  /* LOW = pressed */
            ESP_LOGI(TAG, "Wakeup button: %d (GPIO %d)", i, BTN_GPIOS[i]);
            return (button_id_t)i;
        }
    }

    ESP_LOGW(TAG, "GPIO wakeup but no button active after debounce");
    return BTN_NONE;
}

bool buttons_is_pressed(button_id_t btn)
{
    if (btn == BTN_NONE || (int)btn >= 4) return false;
    return gpio_get_level(BTN_GPIOS[(int)btn]) == 0;
}
```

- [ ] **Step 2: Commit**

```bash
git add src/buttons.c
git commit -m "feat(buttons): implement GPIO wakeup, debounce, and button identification"
```

---

## Task 3: Implement audio.c

**Files:** Replace `src/audio.c`

The speaker amplifier is controlled by GPIO 16 (HIGH = amplifier on, LOW = off). Tone generation uses ESP-IDF's LEDC PWM driver.

Beep spec: 3 short beeps (200 ms on, 100 ms off), repeated every 3 s, for 5 cycles (15 s total). Any call to `audio_stop()` ends immediately.

- [ ] **Step 1: Implement audio.c**

```c
#include "audio.h"
#include "driver/ledc.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "audio";

#define AMP_ENABLE_GPIO  GPIO_NUM_16
#define BEEP_GPIO        GPIO_NUM_17    /* Speaker signal — verify against schematic */
#define BEEP_FREQ_HZ     1000
#define LEDC_CHANNEL     LEDC_CHANNEL_0
#define LEDC_TIMER       LEDC_TIMER_0
#define LEDC_MODE        LEDC_LOW_SPEED_MODE
#define LEDC_RESOLUTION  LEDC_TIMER_10_BIT

static volatile bool s_stop_requested = false;

void audio_init(void)
{
    /* Configure amplifier enable pin */
    gpio_config_t amp_cfg = {
        .pin_bit_mask = (1ULL << AMP_ENABLE_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&amp_cfg);
    gpio_set_level(AMP_ENABLE_GPIO, 0);  /* amp off by default */

    /* Configure LEDC timer */
    ledc_timer_config_t timer_cfg = {
        .speed_mode      = LEDC_MODE,
        .timer_num       = LEDC_TIMER,
        .duty_resolution = LEDC_RESOLUTION,
        .freq_hz         = BEEP_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    esp_err_t ret = ledc_timer_config(&timer_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ledc_timer_config failed: %s", esp_err_to_name(ret));
        return;
    }

    /* Configure LEDC channel */
    ledc_channel_config_t ch_cfg = {
        .gpio_num   = BEEP_GPIO,
        .speed_mode = LEDC_MODE,
        .channel    = LEDC_CHANNEL,
        .timer_sel  = LEDC_TIMER,
        .duty       = 0,            /* silent initially */
        .hpoint     = 0,
    };
    ret = ledc_channel_config(&ch_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ledc_channel_config failed: %s", esp_err_to_name(ret));
    }
}

static void beep_on(void)
{
    gpio_set_level(AMP_ENABLE_GPIO, 1);               /* amp on */
    ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, 512);      /* 50% duty */
    ledc_update_duty(LEDC_MODE, LEDC_CHANNEL);
}

static void beep_off(void)
{
    ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, 0);        /* silent */
    ledc_update_duty(LEDC_MODE, LEDC_CHANNEL);
    gpio_set_level(AMP_ENABLE_GPIO, 0);               /* amp off */
}

void audio_beep_sequence(void)
{
    s_stop_requested = false;

    for (int cycle = 0; cycle < 5 && !s_stop_requested; cycle++) {
        for (int beep = 0; beep < 3 && !s_stop_requested; beep++) {
            beep_on();
            vTaskDelay(pdMS_TO_TICKS(200));  /* 200 ms on */
            beep_off();
            if (!s_stop_requested) {
                vTaskDelay(pdMS_TO_TICKS(100));  /* 100 ms gap */
            }
        }
        if (!s_stop_requested && cycle < 4) {
            /* Wait ~2.7 s to make each cycle ~3 s total
               (3 beeps × 300 ms = 900 ms already elapsed) */
            vTaskDelay(pdMS_TO_TICKS(2700));
        }
    }

    beep_off();  /* ensure amp is off after sequence */
}

void audio_stop(void)
{
    s_stop_requested = true;
    beep_off();
}
```

**Note on BEEP_GPIO**: The MagTag speaker signal GPIO must be verified against the schematic. GPIO 17 is a placeholder — replace with the actual PWM-capable output connected to the amplifier's input. The amplifier shutdown is GPIO 16 (HIGH = on).

- [ ] **Step 2: Commit**

```bash
git add src/audio.c
git commit -m "feat(audio): implement LEDC PWM beep sequence with stop support"
```

---

## Task 4: Implement neopixel.c

**Files:** Replace `src/neopixel.c`

NeoPixel data: GPIO 1. Power gate: GPIO 21 (LOW = power on, HIGH = power off). The invariant is that GPIO 21 is HIGH on every boot/wake path before any other code runs.

WS2812B protocol: Each LED needs a 24-bit value (GRB order). The RMT driver handles the timing. LovyanGFX is NOT used here — use the ESP-IDF `led_strip` component which wraps RMT for NeoPixels.

- [ ] **Step 1: Add led_strip to CMakeLists.txt REQUIRES**

In `src/CMakeLists.txt`, add `led_strip` to the `REQUIRES` list:

```cmake
    REQUIRES
        nvs_flash
        esp_wifi
        esp_netif
        esp_timer
        driver
        log
        led_strip
```

- [ ] **Step 2: Implement neopixel.c**

```c
#include "neopixel.h"
#include "driver/gpio.h"
#include "led_strip.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "neopixel";

#define NEOPIXEL_DATA_GPIO   GPIO_NUM_1
#define NEOPIXEL_POWER_GPIO  GPIO_NUM_21
#define NEOPIXEL_COUNT       4

static led_strip_handle_t s_strip = NULL;
static volatile bool      s_stop_requested = false;

void neopixel_init(void)
{
    /* Ensure power gate is OFF (HIGH) immediately — power correctness invariant */
    gpio_config_t pwr_cfg = {
        .pin_bit_mask = (1ULL << NEOPIXEL_POWER_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&pwr_cfg);
    gpio_set_level(NEOPIXEL_POWER_GPIO, 1);  /* HIGH = power gate OFF */

    /* Configure RMT-backed LED strip */
    led_strip_config_t strip_cfg = {
        .strip_gpio_num = NEOPIXEL_DATA_GPIO,
        .max_leds       = NEOPIXEL_COUNT,
        .led_model      = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags = { .invert_out = false },
    };
    led_strip_rmt_config_t rmt_cfg = {
        .clk_src        = RMT_CLK_SRC_DEFAULT,
        .resolution_hz  = 10000000,  /* 10 MHz */
        .mem_block_symbols = 64,
        .flags = { .with_dma = false },
    };
    esp_err_t ret = led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "led_strip_new_rmt_device failed: %s", esp_err_to_name(ret));
        s_strip = NULL;
        return;
    }
    /* Clear all LEDs */
    led_strip_clear(s_strip);
}

void neopixel_alert_start(void)
{
    if (!s_strip) return;
    s_stop_requested = false;

    /* Power gate ON (LOW) */
    gpio_set_level(NEOPIXEL_POWER_GPIO, 0);

    /* Slow red pulse: fade in/out repeatedly until stopped */
    while (!s_stop_requested) {
        /* Fade in */
        for (int brightness = 0; brightness <= 32 && !s_stop_requested; brightness++) {
            for (int i = 0; i < NEOPIXEL_COUNT; i++) {
                led_strip_set_pixel(s_strip, i, brightness * 8, 0, 0);  /* red */
            }
            led_strip_refresh(s_strip);
            vTaskDelay(pdMS_TO_TICKS(30));
        }
        /* Fade out */
        for (int brightness = 32; brightness >= 0 && !s_stop_requested; brightness--) {
            for (int i = 0; i < NEOPIXEL_COUNT; i++) {
                led_strip_set_pixel(s_strip, i, brightness * 8, 0, 0);
            }
            led_strip_refresh(s_strip);
            vTaskDelay(pdMS_TO_TICKS(30));
        }
    }

    neopixel_stop();
}

void neopixel_stop(void)
{
    s_stop_requested = true;
    if (s_strip) {
        led_strip_clear(s_strip);
        led_strip_refresh(s_strip);
    }
    /* Power gate OFF (HIGH) — safe even if already off */
    gpio_set_level(NEOPIXEL_POWER_GPIO, 1);
}
```

**Note**: `neopixel_alert_start()` blocks the calling task in a loop (not a background task). In Stream 4, `main.c` will call this only when EXPIRED and the device should NOT sleep. The loop exits on `s_stop_requested` (set by `neopixel_stop()`), which will be called from the button handler after any button press.

- [ ] **Step 3: Commit**

```bash
git add src/neopixel.c src/CMakeLists.txt
git commit -m "feat(neopixel): implement RMT NeoPixel driver with power gate invariant"
```

---

## Task 5: Compile gate — verify pio run succeeds

- [ ] **Step 1: Run the build**

```bash
pio run
```

Expected: `[SUCCESS]`.

**Troubleshooting:**

- `led_strip.h` not found: the `led_strip` component is bundled with ESP-IDF v5+. If using an older ESP-IDF, add `idf_component_manager` and run `idf.py add-dependency "espressif/led_strip"`. Check ESP-IDF version with `idf.py --version`.
- `led_strip_rmt_config_t` missing fields: API changed between ESP-IDF versions. Check the ESP-IDF `led_strip` component header for the correct struct fields.
- `LEDC_LOW_SPEED_MODE` vs `LEDC_HIGH_SPEED_MODE`: ESP32-S2 only supports low-speed mode. Use `LEDC_LOW_SPEED_MODE`.
- `vTaskDelay` in `audio_beep_sequence` and `neopixel_alert_start`: these functions block the calling task. Ensure the calling context (in Stream 4) is `app_main` or a task with sufficient stack (default `app_main` stack is 8 KB).
- BEEP_GPIO: if compilation fails because the GPIO is not PWM-capable on ESP32-S2, check the ESP32-S2 datasheet for LEDC-capable pins. All GPIOs on ESP32-S2 support LEDC.

- [ ] **Step 2: Commit any build fixes**

```bash
git add -u
git commit -m "fix(peripherals): resolve ESP-IDF API compatibility issues"
```

---

## Task 6: Code review

- [ ] **Step 1: Run adversarial code review**

Use `superpowers:requesting-code-review` skill. Focus areas:
- `neopixel_init()`: is GPIO 21 configured and set HIGH BEFORE any other init? (Yes — first statement) ✓
- `neopixel_stop()`: called when `s_stop_requested` is already true or `s_strip` is NULL — safe? (Yes — null-checked; gpio_set_level is always called) ✓
- `audio_beep_sequence()`: `s_stop_requested` is a `volatile bool` — is memory ordering sufficient for single-core ESP32-S2? (Yes — single core, no barriers needed)
- `buttons_get_wakeup_button()`: after `esp_rom_delay_us(10000)`, all pressed buttons should be stable. But what if two buttons are pressed? (Returns the first LOW found in order A→D) — is this the desired behaviour?
- `gpio_wakeup_enable` called inside a loop in `buttons_configure_wakeup`: should `esp_sleep_enable_gpio_wakeup()` be called only once (not per button)? Check ESP-IDF docs — `esp_sleep_enable_gpio_wakeup()` enables the GPIO wakeup source globally; `gpio_wakeup_enable()` configures individual pins. Calling `esp_sleep_enable_gpio_wakeup()` 4 times in a loop is redundant but harmless. Fix to call it once.
- `BEEP_GPIO` is a placeholder — review must confirm the actual speaker signal GPIO before merge.
- Power invariant: after `neopixel_alert_start()` returns (normally or via stop), GPIO 21 must be HIGH. Verify the code path.

Fix any issues before merging.

---

## Task 7: Merge to integration

- [ ] **Step 1: Push and merge**

```bash
git push -u origin feature/peripherals
git checkout integration
git merge --no-ff feature/peripherals -m "feat(peripherals): implement buttons, audio LEDC, and NeoPixel RMT drivers"
git push origin integration
```
