#include "neopixel.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "neopixel";

#define NEOPIXEL_DATA_GPIO GPIO_NUM_1
#define NEOPIXEL_POWER_GPIO GPIO_NUM_21
/* NEOPIXEL_COUNT lives in neopixel.h: it bounds the public API's idx. */
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
static uint8_t s_pixels[NEOPIXEL_COUNT * 3]; /* GRB byte order: [G, R, B] per LED */

/* Library configuration, injected from main (this module stays clock- and
   Kconfig-agnostic): quiet-hours predicate + status brightness scale. */
static bool (*s_quiet_cb)(void);
static uint8_t s_status_brightness = 100; /* percent */

/* INVARIANT: exactly one task may drive the RMT channel — concurrent
   flush_pixels from two tasks deadlocks rmt_tx_wait_all_done(portMAX_DELAY)
   (found the hard way in bring-up). The LED task below is that one task; it
   also owns the power gate. Everything else posts messages. */

typedef enum {
    NP_MSG_PIXEL = 0,  /* one pixel, status class (quiet hours + brightness) */
    NP_MSG_PIXEL_HI,   /* one pixel, alert class */
    NP_MSG_BINARY4,    /* 4-bit binary on all pixels, status class (idx = value) */
    NP_MSG_PULSE,      /* begin the slow all-pixel pulse, alert class */
    NP_MSG_PULSE_STOP, /* end the pulse: all pixels off, gate HIGH */
    NP_MSG_CLEAR,      /* all pixels off, gate HIGH */
    NP_MSG_STOP,       /* CLEAR + ack on s_stop_ack (sleep entry) */
} np_msg_type_t;

typedef struct {
    uint8_t type; /* np_msg_type_t */
    uint8_t idx;  /* pixel index, or the BINARY4 value */
    uint8_t r, g, b;
} np_msg_t;

static QueueHandle_t s_queue;
static SemaphoreHandle_t s_stop_ack;

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

static void led_task(void *arg);

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

    /* Single LED owner: queue + task. Depth 8 absorbs a full alert
       sequence of posts without ever blocking a caller. */
    s_queue = xQueueCreate(8, sizeof(np_msg_t));
    s_stop_ack = xSemaphoreCreateBinary();
    /* 4096: the task runs RMT driver calls plus ESP_LOG formatting — 2.5 KB
       was within canary distance of overflowing. */
    if (s_queue == NULL || s_stop_ack == NULL || xTaskCreate(led_task, "np_led", 4096, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "LED task/queue create failed - LEDs disabled");
        if (s_queue != NULL) {
            vQueueDelete(s_queue);
            s_queue = NULL; /* posts become no-ops; gate stays HIGH */
        }
    }
}

/* ---- LED task (sole RMT + gate owner) ---- */

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

static void all_off_gate_high(void) {
    memset(s_pixels, 0, sizeof(s_pixels));
    flush_pixels();
    gpio_set_level(NEOPIXEL_POWER_GPIO, 1); /* power gate OFF */
}

/* Pulse mode, task-local: while active the pulse owns every pixel; static
   pixel posts still update the frame but the next pulse step overwrites
   them (a PULSE_STOP darkens everything — callers re-light after). */
typedef struct {
    bool active;
    int step; /* 0..64 triangle: 0..32 up, 33..64 down */
    uint8_t r, g, b;
} pulse_state_t;

static void led_task_apply(const np_msg_t *m, pulse_state_t *pulse) {
    switch ((np_msg_type_t)m->type) {
        case NP_MSG_PIXEL:
            if (status_muted())
                return;
            s_pixels[m->idx * 3 + 0] = scale_status(m->g);
            s_pixels[m->idx * 3 + 1] = scale_status(m->r);
            s_pixels[m->idx * 3 + 2] = scale_status(m->b);
            break;
        case NP_MSG_PIXEL_HI:
            s_pixels[m->idx * 3 + 0] = m->g; /* GRB byte order */
            s_pixels[m->idx * 3 + 1] = m->r;
            s_pixels[m->idx * 3 + 2] = m->b;
            break;
        case NP_MSG_BINARY4:
            if (status_muted())
                return;
            for (int i = 0; i < NEOPIXEL_COUNT; i++) {
                /* pixel 0 (over button A) = bit3 ... pixel 3 = bit0 */
                bool lit = (m->idx >> (3 - i)) & 1;
                s_pixels[i * 3 + 0] = lit ? scale_status(m->g) : 0;
                s_pixels[i * 3 + 1] = lit ? scale_status(m->r) : 0;
                s_pixels[i * 3 + 2] = lit ? scale_status(m->b) : 0;
            }
            break;
        case NP_MSG_PULSE:
            pulse->active = true;
            pulse->step = 0;
            pulse->r = m->r;
            pulse->g = m->g;
            pulse->b = m->b;
            gpio_set_level(NEOPIXEL_POWER_GPIO, 0); /* gate ON */
            return;                                 /* steps flush; nothing to paint yet */
        case NP_MSG_PULSE_STOP:
            if (pulse->active) {
                pulse->active = false;
                all_off_gate_high();
            }
            return;
        case NP_MSG_CLEAR:
        case NP_MSG_STOP:
            pulse->active = false;
            all_off_gate_high();
            if (m->type == NP_MSG_STOP)
                xSemaphoreGive(s_stop_ack); /* gate confirmed HIGH */
            return;
        default:
            return;
    }
    if (!pulse->active) {                       /* pulse frames overwrite static paints anyway */
        gpio_set_level(NEOPIXEL_POWER_GPIO, 0); /* gate ON */
        flush_pixels();
    }
}

static void led_task(void *arg) {
    (void)arg;
    pulse_state_t pulse = {0};
    for (;;) {
        np_msg_t msg;
        /* Idle: block forever. Pulsing: 30 ms frame cadence between posts. */
        TickType_t wait = pulse.active ? pdMS_TO_TICKS(30) : portMAX_DELAY;
        if (xQueueReceive(s_queue, &msg, wait) == pdTRUE) {
            led_task_apply(&msg, &pulse);
        } else if (pulse.active) {
            int level = (pulse.step <= 32) ? pulse.step : 64 - pulse.step;
            set_all_scaled(pulse.r, pulse.g, pulse.b, level);
            flush_pixels();
            pulse.step = (pulse.step + 1) % 65;
        }
    }
}

static void post(np_msg_t msg, TickType_t timeout) {
    if (s_queue == NULL)
        return; /* task never came up: LEDs stay dark, gate stays HIGH */
    if (xQueueSend(s_queue, &msg, timeout) != pdTRUE) {
        ESP_LOGW(TAG, "LED queue full, dropping msg type %d", (int)msg.type);
    }
}

/* ---- public posting API ---- */

void neopixel_status_pixel(int idx, uint8_t r, uint8_t g, uint8_t b) {
    if (idx < 0 || idx >= NEOPIXEL_COUNT)
        return;
    post((np_msg_t){.type = NP_MSG_PIXEL, .idx = (uint8_t)idx, .r = r, .g = g, .b = b}, 0);
}

void neopixel_highpri_pixel(int idx, uint8_t r, uint8_t g, uint8_t b) {
    if (idx < 0 || idx >= NEOPIXEL_COUNT)
        return;
    post((np_msg_t){.type = NP_MSG_PIXEL_HI, .idx = (uint8_t)idx, .r = r, .g = g, .b = b}, 0);
}

void neopixel_status_binary4(uint8_t value, uint8_t r, uint8_t g, uint8_t b) {
    post((np_msg_t){.type = NP_MSG_BINARY4, .idx = value, .r = r, .g = g, .b = b}, 0);
}

void neopixel_alert_pulse_begin(uint8_t r, uint8_t g, uint8_t b) {
    post((np_msg_t){.type = NP_MSG_PULSE, .r = r, .g = g, .b = b}, pdMS_TO_TICKS(50));
}

void neopixel_alert_pulse_end(void) {
    post((np_msg_t){.type = NP_MSG_PULSE_STOP}, pdMS_TO_TICKS(50));
}

void neopixel_stop(void) {
    post((np_msg_t){.type = NP_MSG_CLEAR}, pdMS_TO_TICKS(50));
}

void neopixel_stop_sync(uint32_t timeout_ms) {
    if (s_queue != NULL && s_stop_ack != NULL) {
        while (xSemaphoreTake(s_stop_ack, 0) == pdTRUE) {
        } /* drain stale acks */
        post((np_msg_t){.type = NP_MSG_STOP}, pdMS_TO_TICKS(50));
        if (xSemaphoreTake(s_stop_ack, pdMS_TO_TICKS(timeout_ms)) == pdTRUE) {
            return; /* LED task confirmed: pixels dark, gate HIGH */
        }
        ESP_LOGW(TAG, "LED task did not ack STOP in %lu ms", (unsigned long)timeout_ms);
    }
    /* Wedged or never started: force the gate off WITHOUT an RMT transmit
       (racing the LED task on the channel deadlocks; power-off darkens the
       pixels regardless). Plain GPIO write — safe from any task. */
    gpio_set_level(NEOPIXEL_POWER_GPIO, 1);
}
