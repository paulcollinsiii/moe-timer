/* SSD1680 e-paper driver for the MagTag 2.9" 296x128 panel.
 *
 * Init/refresh sequences follow GxEPD2_290_BS (OTP waveform path) with the
 * MagTag x-RAM offset from Adafruit_EPD's ssd1680_fpc7519_init_code.
 * If hardware bring-up shows weak/ghosting full refreshes on FPC-7519rev.b
 * panels, the fallback is Adafruit's custom-LUT init (VCOM 0x24, gate 0x17,
 * source 0x41/0xae/0x32, 153-byte LUT, update value 0xC7):
 * https://github.com/adafruit/Adafruit_EPD/blob/master/src/drivers/Adafruit_SSD1680.cpp
 */
#include "ssd1680.h"

#include <string.h>
#include <time.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "ssd1680";

/* SSD1680 command set (datasheet section 7) */
#define CMD_DRIVER_OUTPUT 0x01
#define CMD_DEEP_SLEEP 0x10
#define CMD_DATA_ENTRY 0x11
#define CMD_SW_RESET 0x12
#define CMD_TEMP_SENSOR 0x18
#define CMD_MASTER_ACTIVATE 0x20
#define CMD_DISP_UPDATE_CTRL1 0x21
#define CMD_DISP_UPDATE_CTRL2 0x22
#define CMD_WRITE_RAM_BW 0x24
#define CMD_WRITE_RAM_RED 0x26 /* holds "previous" frame for partial diff */
#define CMD_BORDER_WAVEFORM 0x3C
#define CMD_RAM_X_RANGE 0x44
#define CMD_RAM_Y_RANGE 0x45
#define CMD_RAM_X_COUNTER 0x4E
#define CMD_RAM_Y_COUNTER 0x4F

#define BUSY_TIMEOUT_MS 10000 /* full refresh ~3-4 s; init can be slower */

#define FB_ROW_BYTES (SSD1680_WIDTH / 8)

static spi_device_handle_t s_spi;
static ssd1680_pins_t s_pins;
static bool s_initialized;
static uint8_t s_fb_cache[SSD1680_FB_SIZE]; /* last frame, for 0x26 bookkeeping */

/* Survives deep sleep so the refresh-rate guard holds across wakes. */
static RTC_DATA_ATTR int64_t s_last_refresh_sec;
/* True once previous-frame RAM (0x26) has been written this power cycle.
   Zeroed on power-on reset — panel RAM is garbage then, and a partial
   diff against it corrupts the screen (seen in hardware bring-up). */
static RTC_DATA_ATTR bool s_prev_frame_valid;

/* BUSY-release signal: the NEGEDGE ISR gives, busy_wait blocks on it —
   the wait ends when the panel finishes instead of at the next poll
   tick. NULL when the ISR could not be set up; busy_wait then falls
   back to the 10 ms poll. */
static SemaphoreHandle_t s_busy_sem;

static void IRAM_ATTR busy_isr(void *arg) {
    (void)arg;
    BaseType_t hp = pdFALSE;
    xSemaphoreGiveFromISR(s_busy_sem, &hp);
    if (hp)
        portYIELD_FROM_ISR();
}

static esp_err_t busy_wait(void) {
    if (!gpio_get_level(s_pins.pin_busy)) /* BUSY is active-high */
        return ESP_OK;
    if (s_busy_sem != NULL) {
        /* Drain a stale give, re-check the level (the edge may have landed
           between the check above and here — the give then waits in the
           semaphore, harmlessly drained next time), then block. */
        xSemaphoreTake(s_busy_sem, 0);
        if (!gpio_get_level(s_pins.pin_busy))
            return ESP_OK;
        if (xSemaphoreTake(s_busy_sem, pdMS_TO_TICKS(BUSY_TIMEOUT_MS)) == pdTRUE)
            return ESP_OK;
        if (!gpio_get_level(s_pins.pin_busy))
            return ESP_OK; /* released, edge missed (e.g. ISR detached) */
        ESP_LOGE(TAG, "BUSY stuck high for %d ms - panel dead or disconnected", BUSY_TIMEOUT_MS);
        return ESP_ERR_TIMEOUT;
    }
    /* Poll fallback (no ISR service) */
    int waited = 0;
    while (gpio_get_level(s_pins.pin_busy)) {
        if (waited >= BUSY_TIMEOUT_MS) {
            ESP_LOGE(TAG, "BUSY stuck high for %d ms - panel dead or disconnected", waited);
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
        waited += 10;
    }
    return ESP_OK;
}

static esp_err_t write_cmd(uint8_t cmd) {
    gpio_set_level(s_pins.pin_dc, 0);
    spi_transaction_t t = {.length = 8, .tx_buffer = &cmd};
    return spi_device_polling_transmit(s_spi, &t);
}

static esp_err_t write_data(const uint8_t *data, size_t len) {
    gpio_set_level(s_pins.pin_dc, 1);
    spi_transaction_t t = {.length = len * 8, .tx_buffer = data};
    return spi_device_polling_transmit(s_spi, &t);
}

static esp_err_t cmd_with_data(uint8_t cmd, const uint8_t *data, size_t len) {
    esp_err_t ret = write_cmd(cmd);
    if (ret != ESP_OK)
        return ret;
    return write_data(data, len);
}

static void hw_reset(void) {
    gpio_set_level(s_pins.pin_rst, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(s_pins.pin_rst, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
}

esp_err_t ssd1680_init(const ssd1680_pins_t *pins) {
    s_pins = *pins;

    gpio_config_t out_cfg = {
        .pin_bit_mask = (1ULL << pins->pin_dc) | (1ULL << pins->pin_rst),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&out_cfg), TAG, "gpio out");
    gpio_config_t in_cfg = {
        .pin_bit_mask = (1ULL << pins->pin_busy),
        .mode = GPIO_MODE_INPUT,
        .intr_type = GPIO_INTR_NEGEDGE, /* BUSY release edge feeds busy_wait */
    };
    ESP_RETURN_ON_ERROR(gpio_config(&in_cfg), TAG, "gpio busy");

    /* BUSY-release ISR (best-effort: on any failure busy_wait polls).
       The ISR service is shared — buttons_init usually installed it
       already, so INVALID_STATE is the expected "fine" answer. */
    if (s_busy_sem == NULL) {
        s_busy_sem = xSemaphoreCreateBinary();
        if (s_busy_sem != NULL) {
            esp_err_t ret = gpio_install_isr_service(0);
            if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
                ESP_LOGW(TAG, "isr service unavailable (%s): BUSY wait falls back to polling", esp_err_to_name(ret));
                vSemaphoreDelete(s_busy_sem);
                s_busy_sem = NULL;
            } else if (gpio_isr_handler_add(pins->pin_busy, busy_isr, NULL) != ESP_OK) {
                ESP_LOGW(TAG, "BUSY isr add failed: falling back to polling");
                vSemaphoreDelete(s_busy_sem);
                s_busy_sem = NULL;
            }
        }
    }

    if (!s_spi) {
        spi_bus_config_t bus = {
            .sclk_io_num = pins->pin_sclk,
            .mosi_io_num = pins->pin_mosi,
            .miso_io_num = -1,
            .quadwp_io_num = -1,
            .quadhd_io_num = -1,
            .max_transfer_sz = SSD1680_FB_SIZE,
        };
        ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO), TAG, "spi bus");
        spi_device_interface_config_t dev = {
            .clock_speed_hz = 4 * 1000 * 1000, /* conservative for e-paper */
            .mode = 0,
            .spics_io_num = pins->pin_cs,
            .queue_size = 2,
        };
        ESP_RETURN_ON_ERROR(spi_bus_add_device(SPI2_HOST, &dev, &s_spi), TAG, "spi dev");
    }

    /* Mandatory after panel deep sleep (RAM is retained; registers are not). */
    hw_reset();
    ESP_RETURN_ON_ERROR(busy_wait(), TAG, "reset busy");
    ESP_RETURN_ON_ERROR(write_cmd(CMD_SW_RESET), TAG, "swreset");
    ESP_RETURN_ON_ERROR(busy_wait(), TAG, "swreset busy");

    /* 296 gates: 295 = 0x0127 */
    ESP_RETURN_ON_ERROR(cmd_with_data(CMD_DRIVER_OUTPUT, (const uint8_t[]){0x27, 0x01, 0x00}, 3), TAG, "output");
    /* Data entry: x increment, y increment */
    ESP_RETURN_ON_ERROR(cmd_with_data(CMD_DATA_ENTRY, (const uint8_t[]){0x03}, 1), TAG, "entry");
    /* RAM x window: offset .. offset+15 (16 bytes = 128 px) */
    ESP_RETURN_ON_ERROR(
        cmd_with_data(CMD_RAM_X_RANGE, (const uint8_t[]){SSD1680_XRAM_OFFSET, FB_ROW_BYTES - 1 + SSD1680_XRAM_OFFSET},
                      2),
        TAG, "ram x");
    /* RAM y window: 0 .. 295 */
    ESP_RETURN_ON_ERROR(cmd_with_data(CMD_RAM_Y_RANGE, (const uint8_t[]){0x00, 0x00, 0x27, 0x01}, 4), TAG, "ram y");
    ESP_RETURN_ON_ERROR(cmd_with_data(CMD_BORDER_WAVEFORM, (const uint8_t[]){0x05}, 1), TAG, "border");
    ESP_RETURN_ON_ERROR(cmd_with_data(CMD_DISP_UPDATE_CTRL1, (const uint8_t[]){0x00, 0x80}, 2), TAG, "ctrl1");
    /* Internal temperature sensor drives the OTP waveform */
    ESP_RETURN_ON_ERROR(cmd_with_data(CMD_TEMP_SENSOR, (const uint8_t[]){0x80}, 1), TAG, "temp");

    s_initialized = true;
    return ESP_OK;
}

static esp_err_t set_ram_counters(void) {
    ESP_RETURN_ON_ERROR(cmd_with_data(CMD_RAM_X_COUNTER, (const uint8_t[]){SSD1680_XRAM_OFFSET}, 1), TAG, "x ctr");
    return cmd_with_data(CMD_RAM_Y_COUNTER, (const uint8_t[]){0x00, 0x00}, 2);
}

static esp_err_t write_ram(uint8_t ram_cmd, const uint8_t *fb) {
    ESP_RETURN_ON_ERROR(set_ram_counters(), TAG, "counters");
    ESP_RETURN_ON_ERROR(write_cmd(ram_cmd), TAG, "ram cmd");
    /* Panel RAM: 1 = white, 0 = black. Our fb: 1 = black - invert. */
    uint8_t row[FB_ROW_BYTES];
    for (int y = 0; y < SSD1680_HEIGHT; y++) {
        const uint8_t *src = fb + (size_t)y * sizeof(row);
        for (size_t i = 0; i < sizeof(row); i++)
            row[i] = (uint8_t)~src[i];
        gpio_set_level(s_pins.pin_dc, 1);
        spi_transaction_t t = {.length = sizeof(row) * 8, .tx_buffer = row};
        ESP_RETURN_ON_ERROR(spi_device_polling_transmit(s_spi, &t), TAG, "ram tx");
    }
    return ESP_OK;
}

esp_err_t ssd1680_write_framebuffer(const uint8_t *fb) {
    if (!s_initialized)
        return ESP_ERR_INVALID_STATE;
    memcpy(s_fb_cache, fb, SSD1680_FB_SIZE);
    return write_ram(CMD_WRITE_RAM_BW, fb);
}

static esp_err_t refresh_impl(ssd1680_refresh_mode_t mode, bool rearm_guard) {
    if (!s_initialized)
        return ESP_ERR_INVALID_STATE;
    int64_t now = (int64_t)time(NULL);
    if (!ssd1680_refresh_allowed(now, s_last_refresh_sec, SSD1680_MIN_REFRESH_INTERVAL_SEC)) {
        ESP_LOGW(TAG, "refresh rejected: %lld s since last (min %d)", (long long)(now - s_last_refresh_sec),
                 SSD1680_MIN_REFRESH_INTERVAL_SEC);
        return ESP_ERR_INVALID_STATE;
    }

    ssd1680_refresh_mode_t resolved =
        (ssd1680_refresh_mode_t)ssd1680_resolve_refresh_mode((int)mode, s_prev_frame_valid);
    if (resolved != mode) {
        ESP_LOGI(TAG, "partial promoted to full: no valid previous frame this power cycle");
        mode = resolved;
    }

    if (mode == SSD1680_REFRESH_PARTIAL) {
        ESP_RETURN_ON_ERROR(cmd_with_data(CMD_BORDER_WAVEFORM, (const uint8_t[]){0x80}, 1), TAG, "border");
        ESP_RETURN_ON_ERROR(cmd_with_data(CMD_DISP_UPDATE_CTRL2, (const uint8_t[]){0xFF}, 1), TAG, "ctrl2");
    } else {
        ESP_RETURN_ON_ERROR(cmd_with_data(CMD_BORDER_WAVEFORM, (const uint8_t[]){0x05}, 1), TAG, "border");
        ESP_RETURN_ON_ERROR(cmd_with_data(CMD_DISP_UPDATE_CTRL2, (const uint8_t[]){0xF7}, 1), TAG, "ctrl2");
    }
    ESP_RETURN_ON_ERROR(write_cmd(CMD_MASTER_ACTIVATE), TAG, "activate");
    esp_err_t ret = busy_wait();
    if (ret != ESP_OK)
        return ret; /* timeout logged; caller proceeds to deep sleep */

    /* Bookkeeping for the next partial diff: previous-frame RAM = this frame */
    ESP_RETURN_ON_ERROR(write_ram(CMD_WRITE_RAM_RED, s_fb_cache), TAG, "prev frame");
    s_prev_frame_valid = true;

    if (rearm_guard)
        s_last_refresh_sec = now;
    return ESP_OK;
}

esp_err_t ssd1680_refresh(ssd1680_refresh_mode_t mode) {
    return refresh_impl(mode, true);
}

esp_err_t ssd1680_refresh_intermediate(ssd1680_refresh_mode_t mode) {
    return refresh_impl(mode, false);
}

bool ssd1680_partial_diff_ready(void) {
    return s_initialized && s_prev_frame_valid;
}

int32_t ssd1680_refresh_wait(void) {
    return ssd1680_refresh_wait_sec((int64_t)time(NULL), s_last_refresh_sec, SSD1680_MIN_REFRESH_INTERVAL_SEC);
}

esp_err_t ssd1680_sleep(void) {
    if (!s_initialized)
        return ESP_ERR_INVALID_STATE;
    /* Mode 1: RAM retained - required for partial diffs across MCU deep sleep */
    return cmd_with_data(CMD_DEEP_SLEEP, (const uint8_t[]){0x01}, 1);
}
