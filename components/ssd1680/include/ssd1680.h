#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define SSD1680_WIDTH 128                                      /* panel sources used (portrait width) */
#define SSD1680_HEIGHT 296                                     /* panel gates (portrait height) */
#define SSD1680_FB_SIZE ((SSD1680_WIDTH / 8) * SSD1680_HEIGHT) /* 4736 bytes */

/* Minimum seconds between panel refreshes (runaway protection). */
#define SSD1680_MIN_REFRESH_INTERVAL_SEC 1

/* Hardware bring-up (2026-07): with offset 1 (Adafruit_EPD
   ssd1680_fpc7519_init_code value) the panel shows an 8 px noise stripe at
   source 0 — this FPC-7519rev.b panel maps source 0 to RAM byte 0.
   Symptom of a wrong value: image shifted 8 px along the short axis, with a
   noise stripe of never-written RAM on the edge it shifted away from. */
#define SSD1680_XRAM_OFFSET 0

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

/* Intermediate pass of a multi-pass render (the ghost-cleaning inverse):
   checked against the guard like any refresh but does NOT re-arm it —
   the passes are one logical render, and the final pass would otherwise
   need a fixed inter-pass delay (was 1.1 s on every partial wake). The
   guard's runaway protection still applies to the render as a whole. */
esp_err_t ssd1680_refresh_intermediate(ssd1680_refresh_mode_t mode);

/* Panel deep-sleep mode 1 (RAM retained). Call after every refresh. */
esp_err_t ssd1680_sleep(void);

/* True once previous-frame RAM is valid this power cycle — i.e. a partial
   refresh would actually diff instead of being promoted to full. */
bool ssd1680_partial_diff_ready(void);

/* Pure guard logic (ssd1680_guard.c) — exposed for host tests. */
bool ssd1680_refresh_allowed(int64_t now_sec, int64_t last_refresh_sec, int32_t min_interval_sec);
int32_t ssd1680_refresh_wait_sec(int64_t now_sec, int64_t last_refresh_sec, int32_t min_interval_sec);
int ssd1680_resolve_refresh_mode(int requested_mode, bool prev_frame_valid);

/* Driver-level view of the guard: seconds until ssd1680_refresh would be
   accepted (0 = now). Callers wait this out instead of losing a frame. */
int32_t ssd1680_refresh_wait(void);

#ifdef __cplusplus
}
#endif
