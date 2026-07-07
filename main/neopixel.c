#include "neopixel.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "neopixel";

#define NEOPIXEL_DATA_GPIO GPIO_NUM_1
#define NEOPIXEL_POWER_GPIO GPIO_NUM_21
#define NEOPIXEL_COUNT 4
#define RMT_RESOLUTION_HZ 10000000

/* ---------- Embedded WS2812B RMT encoder ---------- */

typedef struct {
    rmt_encoder_t base;
    rmt_encoder_t *bytes_encoder;
    rmt_encoder_t *copy_encoder;
    int state;
    rmt_symbol_word_t reset_code;
} ws2812_encoder_t;

RMT_ENCODER_FUNC_ATTR
static size_t ws2812_encode(rmt_encoder_t *encoder, rmt_channel_handle_t channel, const void *data, size_t data_size,
                            rmt_encode_state_t *ret_state) {
    ws2812_encoder_t *enc = __containerof(encoder, ws2812_encoder_t, base);
    rmt_encode_state_t session = RMT_ENCODING_RESET;
    rmt_encode_state_t state = RMT_ENCODING_RESET;
    size_t encoded = 0;
    switch (enc->state) {
        case 0:
            encoded += enc->bytes_encoder->encode(enc->bytes_encoder, channel, data, data_size, &session);
            if (session & RMT_ENCODING_COMPLETE)
                enc->state = 1;
            if (session & RMT_ENCODING_MEM_FULL) {
                state |= RMT_ENCODING_MEM_FULL;
                goto out;
            }
            /* fall-through */
        case 1:
            encoded += enc->copy_encoder->encode(enc->copy_encoder, channel, &enc->reset_code, sizeof(enc->reset_code),
                                                 &session);
            if (session & RMT_ENCODING_COMPLETE) {
                enc->state = RMT_ENCODING_RESET;
                state |= RMT_ENCODING_COMPLETE;
            }
            if (session & RMT_ENCODING_MEM_FULL)
                state |= RMT_ENCODING_MEM_FULL;
    }
out:
    *ret_state = state;
    return encoded;
}

static esp_err_t ws2812_del(rmt_encoder_t *encoder) {
    ws2812_encoder_t *enc = __containerof(encoder, ws2812_encoder_t, base);
    rmt_del_encoder(enc->bytes_encoder);
    rmt_del_encoder(enc->copy_encoder);
    free(enc);
    return ESP_OK;
}

RMT_ENCODER_FUNC_ATTR
static esp_err_t ws2812_reset(rmt_encoder_t *encoder) {
    ws2812_encoder_t *enc = __containerof(encoder, ws2812_encoder_t, base);
    rmt_encoder_reset(enc->bytes_encoder);
    rmt_encoder_reset(enc->copy_encoder);
    enc->state = RMT_ENCODING_RESET;
    return ESP_OK;
}

static esp_err_t ws2812_encoder_create(uint32_t resolution_hz, rmt_encoder_handle_t *ret_encoder) {
    ws2812_encoder_t *enc = rmt_alloc_encoder_mem(sizeof(ws2812_encoder_t));
    if (!enc) {
        ESP_LOGE(TAG, "no mem for encoder");
        return ESP_ERR_NO_MEM;
    }
    enc->bytes_encoder = NULL;
    enc->copy_encoder = NULL;
    enc->base.encode = ws2812_encode;
    enc->base.del = ws2812_del;
    enc->base.reset = ws2812_reset;

    rmt_bytes_encoder_config_t bytes_cfg = {
        .bit0 = {.level0 = 1,
                 .duration0 = (uint16_t)(0.3 * resolution_hz / 1000000),
                 .level1 = 0,
                 .duration1 = (uint16_t)(0.9 * resolution_hz / 1000000)},
        .bit1 = {.level0 = 1,
                 .duration0 = (uint16_t)(0.9 * resolution_hz / 1000000),
                 .level1 = 0,
                 .duration1 = (uint16_t)(0.3 * resolution_hz / 1000000)},
        .flags.msb_first = 1,
    };
    esp_err_t ret = rmt_new_bytes_encoder(&bytes_cfg, &enc->bytes_encoder);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "bytes encoder failed: %s", esp_err_to_name(ret));
        free(enc);
        return ret;
    }

    rmt_copy_encoder_config_t copy_cfg = {};
    ret = rmt_new_copy_encoder(&copy_cfg, &enc->copy_encoder);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "copy encoder failed: %s", esp_err_to_name(ret));
        rmt_del_encoder(enc->bytes_encoder);
        free(enc);
        return ret;
    }

    uint32_t reset_ticks = resolution_hz / 1000000 * 100 / 2; /* 100 µs LOW for broad WS2812B compatibility */
    enc->reset_code.level0 = 0;
    enc->reset_code.duration0 = reset_ticks;
    enc->reset_code.level1 = 0;
    enc->reset_code.duration1 = reset_ticks;
    *ret_encoder = &enc->base;
    return ESP_OK;
}

/* ---------- NeoPixel driver ---------- */

static rmt_channel_handle_t s_rmt_chan = NULL;
static rmt_encoder_handle_t s_encoder = NULL;
static volatile bool s_stop_requested = false;
static uint8_t s_pixels[NEOPIXEL_COUNT * 3]; /* GRB byte order: [G, R, B] per LED */

/* Library configuration, injected from main (this module stays clock- and
   Kconfig-agnostic): quiet-hours predicate + status brightness scale. */
static bool (*s_quiet_cb)(void);
static uint8_t s_status_brightness = 100; /* percent */

/* Internal pulse task state. INVARIANT: exactly one task may drive the RMT
   channel — concurrent flush_pixels from two tasks deadlocks
   rmt_tx_wait_all_done(portMAX_DELAY) (found the hard way in bring-up).
   The pulse task is owned here; callers only begin/end. */
static volatile bool s_pulse_done = true;
static uint8_t s_pulse_r, s_pulse_g, s_pulse_b;

void neopixel_set_quiet_cb(bool (*is_quiet)(void)) {
    s_quiet_cb = is_quiet;
}

void neopixel_set_status_brightness(uint8_t pct) {
    s_status_brightness = (pct > 100) ? 100 : pct;
}

static bool status_muted(void) {
    return s_quiet_cb && s_quiet_cb();
}

static uint8_t scale_status(uint8_t ch) {
    return (uint8_t)((int)ch * s_status_brightness / 100);
}

void neopixel_init(void) {
    /* Release the deep-sleep hold placed by enter_deep_sleep() so the pin
       can be reconfigured; power gate OFF (HIGH) first — hard invariant on
       every boot/wake */
    gpio_hold_dis(NEOPIXEL_POWER_GPIO);
    gpio_config_t pwr_cfg = {
        .pin_bit_mask = (1ULL << NEOPIXEL_POWER_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&pwr_cfg);
    gpio_set_level(NEOPIXEL_POWER_GPIO, 1);

    rmt_tx_channel_config_t tx_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .gpio_num = NEOPIXEL_DATA_GPIO,
        .mem_block_symbols = 64,
        .resolution_hz = RMT_RESOLUTION_HZ,
        .trans_queue_depth = 4,
    };
    esp_err_t ret = rmt_new_tx_channel(&tx_cfg, &s_rmt_chan);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "rmt_new_tx_channel failed: %s", esp_err_to_name(ret));
        return;
    }
    ret = ws2812_encoder_create(RMT_RESOLUTION_HZ, &s_encoder);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "encoder create failed: %s", esp_err_to_name(ret));
        rmt_del_channel(s_rmt_chan);
        s_rmt_chan = NULL;
        return;
    }
    rmt_enable(s_rmt_chan);
    memset(s_pixels, 0, sizeof(s_pixels));
}

static void flush_pixels(void) {
    if (!s_rmt_chan || !s_encoder)
        return;
    rmt_transmit_config_t tx_cfg = {.loop_count = 0};
    rmt_transmit(s_rmt_chan, s_encoder, s_pixels, sizeof(s_pixels), &tx_cfg);
    rmt_tx_wait_all_done(s_rmt_chan, portMAX_DELAY);
}

/* Scale a colour by step/32 onto all pixels (GRB byte order) */
static void set_all_scaled(uint8_t r, uint8_t g, uint8_t b, int step) {
    for (int i = 0; i < NEOPIXEL_COUNT; i++) {
        s_pixels[i * 3 + 0] = (uint8_t)((int)g * step / 32);
        s_pixels[i * 3 + 1] = (uint8_t)((int)r * step / 32);
        s_pixels[i * 3 + 2] = (uint8_t)((int)b * step / 32);
    }
}

static void pulse_task(void *arg) {
    (void)arg;
    gpio_set_level(NEOPIXEL_POWER_GPIO, 0); /* power gate ON */
    while (!s_stop_requested) {
        for (int step = 0; step < 32 && !s_stop_requested; step++) {
            set_all_scaled(s_pulse_r, s_pulse_g, s_pulse_b, step);
            flush_pixels();
            vTaskDelay(pdMS_TO_TICKS(30));
        }
        for (int step = 32; step >= 0 && !s_stop_requested; step--) {
            set_all_scaled(s_pulse_r, s_pulse_g, s_pulse_b, step);
            flush_pixels();
            vTaskDelay(pdMS_TO_TICKS(30));
        }
    }
    neopixel_stop(); /* the task's OWN final flush — single-threaded */
    s_pulse_done = true;
    vTaskDelete(NULL);
}

void neopixel_alert_pulse_begin(uint8_t r, uint8_t g, uint8_t b) {
    if (!s_rmt_chan || !s_encoder || !s_pulse_done)
        return; /* already pulsing */
    s_pulse_r = r;
    s_pulse_g = g;
    s_pulse_b = b;
    s_stop_requested = false;
    s_pulse_done = false;
    if (xTaskCreate(pulse_task, "np_pulse", 2048, NULL, 5, NULL) != pdPASS) {
        s_pulse_done = true;
    }
}

void neopixel_alert_pulse_end(void) {
    s_stop_requested = true;
    for (int i = 0; i < 40 && !s_pulse_done; i++) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (!s_pulse_done) {
        ESP_LOGW(TAG, "pulse task did not finish; forcing LEDs off");
    }
    neopixel_stop(); /* idempotent gate-off; safe once the task is done */
}

void neopixel_highpri_pixel(int idx, uint8_t r, uint8_t g, uint8_t b) {
    if (!s_rmt_chan || !s_encoder || idx < 0 || idx >= NEOPIXEL_COUNT)
        return;
    s_pixels[idx * 3 + 0] = g; /* GRB byte order */
    s_pixels[idx * 3 + 1] = r;
    s_pixels[idx * 3 + 2] = b;
    gpio_set_level(NEOPIXEL_POWER_GPIO, 0); /* power gate ON */
    flush_pixels();
}

void neopixel_status_pixel(int idx, uint8_t r, uint8_t g, uint8_t b) {
    if (status_muted())
        return;
    neopixel_highpri_pixel(idx, scale_status(r), scale_status(g), scale_status(b));
}

void neopixel_status_binary4(uint8_t value, uint8_t r, uint8_t g, uint8_t b) {
    if (status_muted() || !s_rmt_chan || !s_encoder)
        return;
    for (int i = 0; i < NEOPIXEL_COUNT; i++) {
        /* pixel 0 (over button A) = bit3 ... pixel 3 = bit0 */
        bool lit = (value >> (3 - i)) & 1;
        s_pixels[i * 3 + 0] = lit ? scale_status(g) : 0;
        s_pixels[i * 3 + 1] = lit ? scale_status(r) : 0;
        s_pixels[i * 3 + 2] = lit ? scale_status(b) : 0;
    }
    gpio_set_level(NEOPIXEL_POWER_GPIO, 0); /* power gate ON */
    flush_pixels();
}

void neopixel_stop(void) {
    s_stop_requested = true;
    memset(s_pixels, 0, sizeof(s_pixels));
    flush_pixels();
    gpio_set_level(NEOPIXEL_POWER_GPIO, 1); /* power gate OFF */
}
