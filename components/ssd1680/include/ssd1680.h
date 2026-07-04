#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define SSD1680_WIDTH 128                                      /* panel sources used (portrait width) */
#define SSD1680_HEIGHT 296                                     /* panel gates (portrait height) */
#define SSD1680_FB_SIZE ((SSD1680_WIDTH / 8) * SSD1680_HEIGHT) /* 4736 bytes */

/* Minimum seconds between panel refreshes (runaway protection). */
#define SSD1680_MIN_REFRESH_INTERVAL_SEC 1

/* MagTag FPC-7519rev.b panels map source 0 to RAM byte 1 (colstart = 8 px).
   Symptom of a wrong value: image shifted 8 px along the short axis.
   Reference: Adafruit_EPD ssd1680_fpc7519_init_code (_xram_offset = 1). */
#define SSD1680_XRAM_OFFSET 1

typedef enum {
    SSD1680_REFRESH_FULL,    /* full inversion flash — best quality */
    SSD1680_REFRESH_PARTIAL, /* differential vs previous frame — no flash */
} ssd1680_refresh_mode_t;

typedef struct {
    int pin_sclk; /* MagTag: 36 */
    int pin_mosi; /* MagTag: 35 */
    int pin_cs;   /* MagTag: 8  */
    int pin_dc;   /* MagTag: 7  */
    int pin_rst;  /* MagTag: 6  */
    int pin_busy; /* MagTag: 5  */
} ssd1680_pins_t;

#ifdef __cplusplus
extern "C" {
#endif

/* SPI bus + panel init + hardware reset. Call once per wake. */
esp_err_t ssd1680_init(const ssd1680_pins_t *pins);

/* fb: SSD1680_FB_SIZE bytes, portrait, row-major, MSB-first,
   bit set = BLACK pixel. Writes to controller RAM (no refresh). */
esp_err_t ssd1680_write_framebuffer(const uint8_t *fb);

/* Trigger a panel update. Serialized on BUSY; rejects calls faster than
   SSD1680_MIN_REFRESH_INTERVAL_SEC with ESP_ERR_INVALID_STATE. */
esp_err_t ssd1680_refresh(ssd1680_refresh_mode_t mode);

/* Panel deep-sleep mode 1 (RAM retained). Call after every refresh. */
esp_err_t ssd1680_sleep(void);

/* Pure guard logic (ssd1680_guard.c) — exposed for host tests. */
bool ssd1680_refresh_allowed(int64_t now_sec, int64_t last_refresh_sec, int32_t min_interval_sec);

#ifdef __cplusplus
}
#endif
